#include <dsfilemanager/readonlyfilemanager.h>
#include "async_p.h"
#include "label_order_p.h"
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileSystemWatcher>
#include <QJsonDocument>
#include <QJsonArray>
#include <QTimer>
#include <sqlite3.h>
#include <dirent.h>
#include <fcntl.h>
#include <linux/openat2.h>
#include <linux/ioprio.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/vfs.h>
#include <sys/syscall.h>
#include <chrono>

namespace dsfm {
using namespace detail;
namespace {
using Clock = std::chrono::steady_clock;
thread_local std::function<bool(FilesystemIdentity)> filesystem_check;
class FilesystemCheckScope {
public:
    explicit FilesystemCheckScope(std::function<bool(FilesystemIdentity)> check) : previous_(std::exchange(filesystem_check, std::move(check))) {}
    ~FilesystemCheckScope() { filesystem_check = std::move(previous_); }
private:
    std::function<bool(FilesystemIdentity)> previous_;
};
struct Evidence;
void checkProjected(const Evidence&); // Defined after Evidence.
struct Statement {
    sqlite3_stmt* statement = nullptr;
    Statement(sqlite3* db, const char* sql, const QVariantList& args = {}) {
        require(sqlite3_prepare_v2(db, sql, -1, &statement, nullptr) == SQLITE_OK, ErrorCode::Corruption);
        for (qsizetype i = 0; i < args.size(); ++i) {
            const auto& v = args[i]; int rc;
            if (v.isNull()) rc = sqlite3_bind_null(statement, int(i + 1));
            else if (v.metaType().id() == QMetaType::QByteArray) {
                const auto bytes = v.toByteArray();
                rc = sqlite3_bind_blob(statement, int(i + 1), bytes.constData(), int(bytes.size()), SQLITE_TRANSIENT);
            } else if (v.metaType().id() == QMetaType::QString) {
                const auto bytes = v.toString().toUtf8();
                rc = sqlite3_bind_text(statement, int(i + 1), bytes.constData(), int(bytes.size()), SQLITE_TRANSIENT);
            } else rc = sqlite3_bind_int64(statement, int(i + 1), v.toLongLong());
            require(rc == SQLITE_OK, ErrorCode::Corruption);
        }
    }
    ~Statement() { sqlite3_finalize(statement); }
    Statement(const Statement&) = delete;
    bool next() {
        const auto rc = sqlite3_step(statement);
        if (rc == SQLITE_ROW) return true;
        if (rc == SQLITE_FULL) fail(ErrorCode::QuotaExceeded);
        require(rc == SQLITE_DONE, ErrorCode::Corruption); return false;
    }
    QByteArray bytes(int i) const {
        return QByteArray(static_cast<const char*>(sqlite3_column_blob(statement, i)), sqlite3_column_bytes(statement, i));
    }
    QString text(int i) const { return QString::fromUtf8(bytes(i)); }
    qint64 number(int i) const { return sqlite3_column_int64(statement, i); }
};
struct Database {
    sqlite3* db = nullptr;
    ~Database() { if (db) sqlite3_close(db); }
    void exec(const char* sql, const QVariantList& args = {}) { Statement q(db, sql, args); while (q.next()) {} }
    QByteArray meta(QString key) { Statement q(db, "SELECT value FROM meta WHERE key=?", {key}); return q.next() ? q.bytes(0) : QByteArray{}; }
    void meta(QString key, QByteArray value) { exec("INSERT INTO meta VALUES(?,?) ON CONFLICT(key) DO UPDATE SET value=excluded.value", {key, value}); }
};
struct DbTransaction {
    Database& db; bool done = false;
    explicit DbTransaction(Database& value) : db(value) { db.exec("BEGIN IMMEDIATE"); }
    ~DbTransaction() { if (!done) sqlite3_exec(db.db, "ROLLBACK", nullptr, nullptr, nullptr); }
    void commit() { db.exec("COMMIT"); done = true; }
};
struct Evidence {
    quint64 dev = 0, ino = 0, mount = 0;
    quint64 size = 0;
    qint64 mtime = 0, ctime = 0;
    mode_t mode = 0; nlink_t links = 0; qint64 birth = 0;
    bool sameObject(const Evidence& other) const { return dev == other.dev && ino == other.ino && (!birth || !other.birth || birth == other.birth); }
    bool sameVersion(const Evidence& other) const {
        return sameObject(other) && size == other.size && mtime == other.mtime && ctime == other.ctime && mode == other.mode && links == other.links;
    }
};
void checkProjected(const Evidence& item) {
    if (!filesystem_check) return;
    bool allowed = false;
    try { allowed = filesystem_check({item.dev, item.ino, bool(S_ISDIR(item.mode))}); } catch (...) {}
    require(allowed, ErrorCode::MissingEntry);
}
qint64 nanoseconds(qint64 seconds, quint32 fraction) {
    qint64 product = 0, result = 0;
    require(fraction < 1000000000U && !__builtin_mul_overflow(seconds, qint64(1000000000), &product)
        && !__builtin_add_overflow(product, qint64(fraction), &result), ErrorCode::ExternalModification);
    return result;
}
Evidence evidence(int fd) {
    struct stat st{}; ioCheck(::fstat(fd, &st) == 0);
    struct statx sx{};
    ioCheck(::statx(fd, "", AT_EMPTY_PATH | AT_SYMLINK_NOFOLLOW, STATX_MNT_ID | STATX_BTIME, &sx) == 0);
    require(sx.stx_mask & STATX_MNT_ID, ErrorCode::UnsupportedCapability);
    return {quint64(st.st_dev), quint64(st.st_ino), sx.stx_mnt_id, quint64(st.st_size),
        nanoseconds(st.st_mtim.tv_sec, quint32(st.st_mtim.tv_nsec)),
        nanoseconds(st.st_ctim.tv_sec, quint32(st.st_ctim.tv_nsec)), st.st_mode, st.st_nlink, (sx.stx_mask & STATX_BTIME) ? nanoseconds(sx.stx_btime.tv_sec, sx.stx_btime.tv_nsec) : 0};
}
Fd duplicate(int fd) { return Fd(::fcntl(fd, F_DUPFD_CLOEXEC, 0)); }
Fd pathAt(int fd, const QByteArray& name) { return Fd(::openat(fd, name.constData(), O_PATH | O_NOFOLLOW | O_CLOEXEC)); }
struct Excluded { QString path; Evidence identity; bool directory; Fd handle; };
struct Cursor {
    DirectoryId parent; QUuid scope; QByteArray key, filesystem_generation; quint64 generation = 0, policy = 0, exclusions = 0;
    Clock::time_point expires; std::shared_ptr<const std::atomic<quint64>> authority;
    bool stable_order=false, ascii_order=false;
};
struct DirectoryCache {
    FileInfo info; QByteArray raw; Evidence identity; bool alias = false;
    quint64 catalog_generation = 0, epoch = 0;
    Clock::time_point touched = Clock::now();
};
struct Observer {
    NamespaceId ns; std::optional<DirectoryId> directory;
    QPointer<QObject> context;
    std::shared_ptr<QObject> dispatch;
    std::function<void(DirectoryChange)> callback;
    std::shared_ptr<std::atomic_bool> active = std::make_shared<std::atomic_bool>(true);
    std::atomic_bool queued{false}; std::atomic<quint64> generation{0};
    AccessContext access;
    std::shared_ptr<DirectoryCache> pin;
    std::atomic_bool invalidated{false};
};
struct ScanDirectory {
    DirectoryId id; Fd fd; DIR* stream = nullptr; Evidence before; quint64 epoch = 0;
    bool enumerated = false;
    ~ScanDirectory() { if (stream) ::closedir(stream); }
};
struct ReadState;
struct Source {
    ReadOnlyNamespaceConfig config;
    Fd root, internal, owner_lock;
    Evidence root_identity;
    DirectoryId root_id;
    Database catalog;
    std::vector<Excluded> excluded;
    NamespaceStatus status = NamespaceStatus::Updating;
    std::optional<Error> readiness_error;
    quint64 generation = 1, epoch = 0, scan_epoch = 0, exclusion_generation = 1;
    bool scanning_active = false;
    std::shared_ptr<std::atomic<quint64>> access_epoch = std::make_shared<std::atomic<quint64>>(1);
    quint32 handles = 0;
    quint64 entry_count = 0, recovery_bytes = 0;
    quint32 scan_retries = 0;
    std::vector<std::weak_ptr<ReadState>> readers;
    std::atomic_bool stopped{false};
    std::mutex observer_mutex;
    NamespaceLimits subscription_limits;
    std::unique_ptr<ScanDirectory> scanning;
    QHash<QByteArray, Cursor> cursors;
    std::map<DirectoryId, std::shared_ptr<DirectoryCache>> directories;
    std::vector<std::shared_ptr<Observer>> observers;
    Clock::time_point next_reconcile = Clock::now();
};
QByteArray configuration(const ReadOnlyNamespaceConfig& config) {
    QJsonArray directories, objects;
    for (const auto& p : config.exclusions.directory_roots) directories.append(p);
    for (const auto& p : config.exclusions.objects) objects.append(p);
    return QJsonDocument(QJsonObject{{"root", config.common.storage_root}, {"symlinks", int(config.symlinks)},
        {"verification", int(config.verification)}, {"encoding", int(linux_name_encoding_version)},
        {"unicode", int(QChar::currentUnicodeVersion())}, {"directories", directories}, {"objects", objects},
        {"exclusionGeneration", QString::number(config.exclusions.generation)}, {"limits", limitsJson(config.common.limits)},
        {"scanDirectories", int(config.limits.max_scan_directories)}, {"scanRetries", int(config.limits.max_scan_retries)}, {"namespaces", int(config.limits.max_namespaces)}, {"watches", int(config.limits.max_watches)},
        {"symlinkDepth", int(config.limits.max_symlinks)}, {"cursors", int(config.limits.max_cursors)},
        {"cachedDirectories", int(config.limits.max_cached_directories)}, {"reconcileMs", int(config.limits.reconcile_interval_ms)},
        {"readerMs", int(config.limits.reader_lifetime_ms)}}).toJson(QJsonDocument::Compact);
}
void checkConfig(const ReadOnlyNamespaceConfig& config) {
    validateLimits(config.common.limits);
    require(!config.common.id.isNull() && config.common.storage_root.startsWith('/')
        && config.common.storage_root == QDir::cleanPath(config.common.storage_root)
        && config.common.storage_root.isValidUtf16() && !config.common.storage_root.contains(QChar::Null) && config.common.storage_root.size() <= 32768,
        ErrorCode::InvalidArgument);
    require(config.symlinks >= SymlinkPolicy::Never && config.symlinks <= SymlinkPolicy::Always
        && config.verification >= ReadVerification::Cached && config.verification <= ReadVerification::Snapshot,
        ErrorCode::InvalidArgument);
    const auto& l = config.limits;
    require(l.max_scan_retries <= 100 && l.max_scan_directories && l.max_scan_directories <= 65536 && l.max_watches <= 4096
        && l.max_namespaces && l.max_namespaces <= 128 && l.max_symlinks && l.max_symlinks <= 64
        && l.max_cursors && l.max_cursors <= 4096 && l.max_cached_directories && l.max_cached_directories <= 4096
        && l.reconcile_interval_ms >= 100 && l.reconcile_interval_ms <= 86400000
        && l.reader_lifetime_ms >= 100 && l.reader_lifetime_ms <= 86400000,
        ErrorCode::InvalidArgument);
    require(config.exclusions.generation && config.exclusions.generation < quint64(std::numeric_limits<qint64>::max()) && config.exclusions.directory_roots.size() + config.exclusions.objects.size() <= 1024,
        ErrorCode::InvalidArgument);
}
// Inspect mountinfo only for an already opened object's mount. A mounted subtree
// can hide ancestors; quarantine it rather than claim excluded-root protection.
void checkMount(quint64 mount, int fd) {
    struct statfs filesystem{}; ioCheck(::fstatfs(fd, &filesystem) == 0);
    const auto type = quint64(filesystem.f_type);
    require(type == 0xEF53 || type == 0x58465342 || type == 0x9123683E || type == 0x01021994 || type == 0x794c7630, ErrorCode::MissingEntry);
    QFile mounts("/proc/self/mountinfo"); require(mounts.open(QIODevice::ReadOnly), ErrorCode::StorageUnavailable);
    quint64 bytes = 0;
    for (;;) {
        const auto line = mounts.readLine(16385); if (line.isEmpty()) break; bytes += quint64(line.size());
        require(line.size() <= 16384 && bytes <= (4U << 20), ErrorCode::QuotaExceeded);
        const auto fields = line.split(' ');
        require(fields.size() >= 6, ErrorCode::StorageUnavailable);
        if (fields[0].toULongLong() == mount) {
            require(fields[3] == "/", ErrorCode::MissingEntry); return;
        }
    }
    fail(ErrorCode::StorageUnavailable);
}
void excludedObject(const Source& source, const Evidence& found) {
    for (const auto& item : source.excluded)
        require(!(item.identity.dev == found.dev && item.identity.ino == found.ino), ErrorCode::MissingEntry);
    require(S_ISDIR(found.mode) || S_ISREG(found.mode) || S_ISLNK(found.mode), ErrorCode::MissingEntry);
    if (S_ISREG(found.mode)) require(found.links == 1, ErrorCode::MissingEntry);
}
struct Opened { Fd fd; Evidence identity; bool alias = false; bool outside_assignment = false; };
// Walk from an anchored slash, retaining parent descriptors; never turn logical
// labels into source paths. Every ancestor is checked, including symlink targets.
Opened resolve(Source& source, QByteArray absolute, bool enforce_policy = true, bool enforce_exclusions = true, const Evidence* boundary = nullptr) {
    require(absolute.startsWith('/') && absolute.size() <= 32768 && !absolute.contains('\0'), ErrorCode::InvalidArgument);
    auto pending = absolute.split('/');
    std::vector<Fd> stack; stack.emplace_back(::open("/", O_PATH | O_DIRECTORY | O_CLOEXEC));
    bool alias = false; quint32 links = 0, steps = 0;
    auto parent_mount = evidence(stack.back().value).mount;
    if (enforce_exclusions) { const auto found = evidence(stack.back().value); excludedObject(source, found); checkProjected(found); }
    while (!pending.isEmpty()) {
        require(++steps <= 4096, ErrorCode::QuotaExceeded);
        auto component = pending.takeFirst();
        if (component.isEmpty() || component == ".") continue;
        if (component == "..") { if (stack.size() > 1) stack.pop_back(); parent_mount = evidence(stack.back().value).mount; continue; }
        require(component.size() <= 255 && stack.size() <= source.config.common.limits.max_depth + 128U, ErrorCode::QuotaExceeded);
        auto fd = pathAt(stack.back().value, component); auto found = evidence(fd.value);
        if (enforce_exclusions) { excludedObject(source, found); checkProjected(found); }
        if (found.mount != parent_mount) checkMount(found.mount, fd.value);
        if (S_ISLNK(found.mode)) {
            require(!enforce_policy || source.config.symlinks != SymlinkPolicy::Never, ErrorCode::MissingEntry);
            require(++links <= source.config.limits.max_symlinks, ErrorCode::MissingEntry);
            QByteArray target(32769, Qt::Uninitialized);
            const auto count = ::readlinkat(fd.value, "", target.data(), size_t(target.size())); ioCheck(count >= 0);
            require(count > 0 && count <= 32768, ErrorCode::MissingEntry); target.resize(count);
            if (target.startsWith('/')) { stack.resize(1); parent_mount = evidence(stack.back().value).mount; }
            pending = target.split('/') + pending; alias = true; continue;
        }
        if (!pending.isEmpty()) require(S_ISDIR(found.mode), ErrorCode::MissingEntry);
        stack.push_back(std::move(fd)); parent_mount = found.mount;
    }
    auto found = evidence(stack.back().value);
    bool outside = boundary != nullptr;
    if (boundary) for (const auto &fd : stack) if (evidence(fd.value).sameObject(*boundary)) outside=false;
    return {std::move(stack.back()), found, alias, outside};
}
void validatePolicy(Source& source) {
    FilesystemCheckScope scope({});
    require(!source.stopped, ErrorCode::ShuttingDown);
    for (const auto& item : source.excluded) {
        auto opened = resolve(source, QFile::encodeName(item.path), false, false);
        require(opened.identity.sameObject(item.identity), ErrorCode::StorageUnavailable);
    }
    auto root = resolve(source, QFile::encodeName(source.config.common.storage_root), false);
    require(root.identity.sameObject(source.root_identity), ErrorCode::StorageUnavailable);
}
std::vector<Excluded> prepareExclusions(Source& source, const ExclusionPolicy& policy, const QString& private_root) {
    require(policy.generation && policy.generation < quint64(std::numeric_limits<qint64>::max()) && policy.directory_roots.size() + policy.objects.size() <= 1024, ErrorCode::InvalidArgument);
    std::vector<Excluded> result;
    auto append = [&](const QString& path, bool directory) {
        require(path.startsWith('/') && path == QDir::cleanPath(path) && !path.contains(QChar::Null), ErrorCode::InvalidArgument);
        auto opened = resolve(source, QFile::encodeName(path), false, false);
        require(!directory || S_ISDIR(opened.identity.mode), ErrorCode::InvalidArgument);
        result.push_back({path, opened.identity, directory, std::move(opened.fd)});
    };
    append(private_root, true);
    for (const auto& p : policy.directory_roots) append(p, true);
    for (const auto& p : policy.objects) append(p, false);
    return result;
}
struct Entry { FileInfo info; QByteArray raw; Evidence identity; bool alias = false; quint64 epoch = 0; };
constexpr auto entry_columns = "id,parent,name,type,status,size,mtime,sha,generation,raw,dev,ino,mount,ctime,mode,links,alias,epoch,birth";
Entry entryFrom(Statement& q) {
    Entry entry; auto& info = entry.info;
    info.id = QUuid(q.text(0)); if (!q.text(1).isEmpty()) info.parent = QUuid(q.text(1));
    info.name = q.text(2); info.type = EntryType(q.number(3)); info.status = EntryStatus(q.number(4));
    info.size = quint64(q.number(5)); info.modified = QDateTime::fromMSecsSinceEpoch(q.number(6) / 1000000, QTimeZone::UTC);
    info.sha256 = q.bytes(7); info.generation = quint64(q.number(8)); entry.raw = q.bytes(9);
    entry.identity = {q.text(10).toULongLong(), q.text(11).toULongLong(), q.text(12).toULongLong(), info.size,
        q.number(6), q.number(13), mode_t(q.number(14)), nlink_t(q.number(15))};
    entry.alias = q.number(16); entry.epoch = quint64(q.number(17)); entry.identity.birth = q.number(18);
    if (info.type == EntryType::Directory) info.size = 0; // Physical directory sizes can disclose hidden entries.
    if (entry.identity.birth) info.created = QDateTime::fromMSecsSinceEpoch(entry.identity.birth / 1000000, QTimeZone::UTC);
    require(!info.id.isNull() && q.number(3) >= 0 && q.number(3) <= 1 && q.number(4) >= 0 && q.number(4) <= 3
        && q.number(5) >= 0 && q.number(8) >= 0 && q.number(15) > 0 && q.number(17) >= 0
        && (info.sha256.isEmpty() || info.sha256.size() == 32), ErrorCode::Corruption);
    info.hash_state = info.sha256.isEmpty() ? HashState::Missing : isReadable(info.status) ? HashState::Verified : HashState::Stale;
    return entry;
}
Entry entry(Source& source, QUuid id) {
    const auto cached = source.directories.find(id);
    if (cached != source.directories.end() && cached->second->catalog_generation == source.generation) {
        const auto& value = *cached->second;
        return {value.info, value.raw, value.identity, value.alias, value.epoch};
    }
    const auto sql = QByteArray("SELECT ") + entry_columns + " FROM entries WHERE id=?";
    Statement q(source.catalog.db, sql.constData(), {uuid(id)}); require(q.next(), ErrorCode::MissingEntry);
    auto value = entryFrom(q); value.info.namespace_id = source.config.common.id;
    if (value.info.type == EntryType::File) {
        Statement computing(source.catalog.db, "SELECT 1 FROM hash_jobs WHERE id=?", {uuid(id)});
        if (computing.next()) value.info.hash_state = HashState::Computing;
    }
    if (value.epoch != source.epoch && isListable(value.info.status)) {
        value.info.status = EntryStatus::Updating;
        if (!value.info.sha256.isEmpty()) value.info.hash_state = HashState::Stale;
    }
    return value;
}
QByteArray physicalPath(Source& source, QUuid id, bool allow_missing = false) {
    QList<QByteArray> components;
    for (quint32 depth = 0; depth <= source.config.common.limits.max_depth; ++depth) {
        auto item = entry(source, id);
        require(allow_missing || isListable(item.info.status), ErrorCode::MissingEntry);
        if (!item.info.parent) {
            require(item.info.id == source.root_id, ErrorCode::MissingEntry);
            return QFile::encodeName(source.config.common.storage_root) + (components.isEmpty() ? QByteArray{} : '/' + components.join('/'));
        }
        auto encoded = encodeLinuxName(item.raw); require(encoded && encoded.value() == item.info.name, ErrorCode::Corruption);
        components.prepend(item.raw); id = *item.info.parent;
    }
    fail(ErrorCode::Corruption);
}
DirectoryId listingParent(Source& source, const Entry& directory) {
    Statement target(source.catalog.db, "SELECT id FROM scan_targets WHERE dev=? AND ino=?", {QString::number(directory.identity.dev), QString::number(directory.identity.ino)});
    if (!target.next()) return directory.info.id;
    const auto id = QUuid(target.text(0));
    auto current = resolve(source, physicalPath(source, id));
    require(current.identity.sameObject(directory.identity), ErrorCode::MissingEntry);
    return id;
}
void authorize(const AccessContext& access, NamespaceId ns, QUuid id, AccessOperation operation, bool required = false) {
    require(access.current() && (!required || bool(access.check)), ErrorCode::MissingEntry);
    if (access.check) {
        bool permitted = false;
        try { auto result = access.check(ns, id, operation); permitted = result && result.value(); } catch (...) {}
        require(permitted, ErrorCode::MissingEntry);
    }
}
Result<EntryTarget> targetFor(const AccessContext& access, FilesystemIdentity identity) {
    if (!access.resolve_target || !access.current_generation || access.authorization_scope.isNull() || !access.current() || access.remaining_aliases > 64) return Error{ErrorCode::MissingEntry};
    try { return access.resolve_target(identity); } catch (...) { return Error{ErrorCode::MissingEntry}; }
}
bool externalFileAllowed(Source& source, const Entry& item, const AccessContext& access) {
    if (source.config.symlinks!=SymlinkPolicy::Always || item.info.type!=EntryType::File || !access.allow_external_files) return false;
    auto boundary=entry(source,access.assignment_directory.isNull()?source.root_id:access.assignment_directory);
    require(boundary.info.type==EntryType::Directory && !boundary.alias,ErrorCode::MissingEntry);
    auto opened=resolve(source,physicalPath(source,item.info.id),true,true,&boundary.identity);
    require(opened.identity.sameObject(item.identity),ErrorCode::MissingEntry);
    return opened.outside_assignment;
}
void authorizeAlias(Source& source, const Entry& item, const AccessContext& access, AccessOperation operation) {
    if (!item.alias) return;
    if (source.config.symlinks == SymlinkPolicy::Always && item.info.type == EntryType::File && access.allow_external_files) {
        auto target = targetFor(access, {item.identity.dev,item.identity.ino,false});
        if (!target && externalFileAllowed(source,item,access)) return;
    }
    require(bool(access.resolve_target), ErrorCode::MissingEntry);
    auto target = targetFor(access, {item.identity.dev, item.identity.ino, item.info.type == EntryType::Directory});
    require(bool(target) && !target.value().namespace_id.isNull() && !target.value().id.isNull()
        && (target.value().namespace_id != source.config.common.id || target.value().id != item.info.id), ErrorCode::MissingEntry);
    authorize(access, target.value().namespace_id, target.value().id, operation, true);
}
std::shared_ptr<IFileManager> backendFor(const AccessContext& access, NamespaceId ns) {
    try { return access.backend ? access.backend(ns) : nullptr; } catch (...) { return {}; }
}
AccessContext projectedAccess(Source& source, AccessContext access) {
    require(access.filesystem_generation.size() <= 32, ErrorCode::InvalidArgument);
    QVector<FilesystemIdentity> exclusions;
    for (const auto& item : source.excluded) exclusions.append({item.identity.dev, item.identity.ino, item.directory});
    auto previous = std::move(access.filesystem_check); auto epoch = source.access_epoch; const auto selected = source.exclusion_generation;
    access.filesystem_generation = QCryptographicHash::hash(access.filesystem_generation + source.config.common.id.toRfc4122() + QByteArray::number(selected), QCryptographicHash::Sha256);
    access.filesystem_check = [exclusions = std::move(exclusions), previous = std::move(previous), epoch, selected](FilesystemIdentity object) {
        if (epoch->load() != selected || (previous && !previous(object))) return false;
        for (const auto& excluded : exclusions) if (excluded.device == object.device && excluded.inode == object.inode) return false;
        return true;
    };
    return access;
}
void advance(Source& source) {
    require(source.generation < quint64(std::numeric_limits<qint64>::max()) - 1, ErrorCode::QuotaExceeded);
    ++source.generation; source.catalog.meta("generation", QByteArray::number(source.generation));
}
void changed(Source& source, QUuid id) {
    Statement current(source.catalog.db, "SELECT status FROM entries WHERE id=?", {uuid(id)});
    if (current.next() && current.number(0) == int(EntryStatus::Updating)) return;
    advance(source);
    source.catalog.exec("UPDATE entries SET status=3,generation=? WHERE id=?", {qlonglong(source.generation), uuid(id)});
    source.next_reconcile = Clock::now();
}
Opened validateEntry(Source& source, const Entry& item, const AccessContext& access, AccessOperation operation) {
    FilesystemCheckScope scope(access.filesystem_check);
    authorize(access, source.config.common.id, item.info.id, operation);
    require(isListable(item.info.status), ErrorCode::MissingEntry);
    authorizeAlias(source, item, access, operation);
    validatePolicy(source);
    auto opened = resolve(source, physicalPath(source, item.info.id));
    if (!opened.identity.sameVersion(item.identity) || opened.alias != item.alias) {
        changed(source, item.info.id); fail(ErrorCode::Busy);
    }
    authorizeAlias(source, item, access, operation);
    return opened;
}
Fd contentFd(const Opened& opened, bool directory) {
    require(directory ? S_ISDIR(opened.identity.mode) : S_ISREG(opened.identity.mode), ErrorCode::MissingEntry);
    // Reopen the O_PATH handle, never the original pathname. Type was already
    // inspected; O_NONBLOCK prevents a replacement race from blocking a worker.
    const auto path = QByteArray("/proc/self/fd/") + QByteArray::number(opened.fd.value);
    Fd fd(::open(path.constData(), O_RDONLY | O_NONBLOCK | O_CLOEXEC | (directory ? O_DIRECTORY : 0)));
    require(evidence(fd.value).sameVersion(opened.identity), ErrorCode::ExternalModification);
    return fd;
}
NamespaceInfo sourceInfo(Source& source) {
    NamespaceInfo result; result.config = source.config.common; result.root = source.root_id;
    result.generation = source.generation; result.status = source.status; result.readiness_error = source.readiness_error;
    Statement staging(source.catalog.db, "SELECT COALESCE(sum(size),0) FROM leases");
    if (staging.next()) result.charged_bytes = quint64(staging.number(0)) + source.recovery_bytes;
    result.capabilities = Capability::Read | Capability::PreparedRead | Capability::DirectorySubscriptions;
    if (source.config.verification == ReadVerification::Snapshot) result.capabilities |= Capability::SnapshotReads | Capability::ResumeLeases;
    return result;
}
FileInfo metadata(Source& source, QUuid id, const AccessContext& access);
void notify(Source& source) {
    std::vector<std::shared_ptr<Observer>> observers;
    {
        std::lock_guard lock(source.observer_mutex);
        std::erase_if(source.observers, [](const auto& old) { return !old->active->load(); });
        observers = source.observers;
    }
    for (const auto& sub : observers) {
        sub->generation = source.generation;
        if (sub->directory) {
            try {
                metadata(source, *sub->directory, sub->access);
                auto item = entry(source, *sub->directory);
                authorize(sub->access, sub->ns, *sub->directory, AccessOperation::List);
                authorizeAlias(source, item, sub->access, AccessOperation::List);
            } catch (...) { sub->invalidated = true; }
        }
        if (sub->queued.exchange(true)) continue;
        QMetaObject::invokeMethod(sub->dispatch.get(), [sub] {
            sub->queued = false;
            if (!sub->active->load() || !sub->context || !sub->access.current()) { sub->active->store(false); return; }
            try {
                if (sub->invalidated) {
                    sub->active->store(false);
                    sub->callback({sub->ns, sub->directory.value_or(DirectoryId{}), sub->generation.load(), DirectoryChangeKind::Invalidated});
                } else {
                    if (sub->directory) authorize(sub->access, sub->ns, *sub->directory, AccessOperation::List);
                    sub->callback({sub->ns, sub->directory.value_or(DirectoryId{}), sub->generation.load(), DirectoryChangeKind::RescanRequired});
                }
            } catch (...) { sub->active->store(false); }
        }, Qt::QueuedConnection);
    }
}
} // namespace
namespace {
void writeEntry(Source& source, DirectoryId id, std::optional<DirectoryId> parent, QByteArray raw, const Opened& opened, QByteArray hash = {}) {
    auto encoded = raw.isEmpty() ? Result<QString>{QStringLiteral("")} : encodeLinuxName(raw);
    require(bool(encoded), ErrorCode::InvalidName);
    const auto& e = opened.identity;
    source.catalog.exec("INSERT INTO entries(id,parent,name,type,status,size,mtime,sha,generation,raw,dev,ino,mount,ctime,mode,links,alias,epoch,seen,birth) VALUES(?,?,?,?,0,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?) ON CONFLICT(id) DO UPDATE SET parent=excluded.parent,name=excluded.name,type=excluded.type,status=0,size=excluded.size,mtime=excluded.mtime,sha=excluded.sha,generation=excluded.generation,raw=excluded.raw,dev=excluded.dev,ino=excluded.ino,mount=excluded.mount,ctime=excluded.ctime,mode=excluded.mode,links=excluded.links,alias=excluded.alias,epoch=excluded.epoch,seen=excluded.seen,birth=excluded.birth",
        {uuid(id), parent ? QVariant(uuid(*parent)) : QVariant{}, encoded.value(), int(S_ISDIR(e.mode) ? EntryType::Directory : EntryType::File),
         qlonglong(e.size), e.mtime, hash.isEmpty() ? QVariant{} : QVariant(hash), qlonglong(source.generation), raw,
         QString::number(e.dev), QString::number(e.ino), QString::number(e.mount), e.ctime, int(e.mode), qlonglong(e.links), int(opened.alias),
         qlonglong(source.epoch), qlonglong(source.scan_epoch), e.birth});
}
void beginScan(Source& source, bool reset_retries = true, bool revalidate = false) {
    source.scanning.reset();
    const auto old_epoch = source.epoch, old_scan_epoch = source.scan_epoch, old_generation = source.generation, old_count = source.entry_count;
    try {
        DbTransaction transaction(source.catalog);
        source.catalog.exec("DELETE FROM scan_queue");
        require(source.epoch < quint64(std::numeric_limits<qint64>::max()) - 1 && source.scan_epoch < quint64(std::numeric_limits<qint64>::max()) - 1, ErrorCode::QuotaExceeded);
        ++source.scan_epoch;
        if (revalidate) { ++source.epoch; advance(source); }
        source.catalog.meta("epoch", QByteArray::number(source.epoch));
        source.catalog.meta("scanEpoch", QByteArray::number(source.scan_epoch));
        source.catalog.exec("DELETE FROM entries WHERE id IN(SELECT e.id FROM entries e WHERE status=1 AND seen<? AND id!=? AND NOT EXISTS(SELECT 1 FROM entries c WHERE c.parent=e.id) LIMIT ?)",
            {qlonglong(source.scan_epoch > 1 ? source.scan_epoch - 1 : 0), uuid(source.root_id), source.config.common.limits.max_batch});
        source.entry_count -= quint64(sqlite3_changes(source.catalog.db));
        source.catalog.exec("DELETE FROM scan_targets WHERE epoch<? OR NOT EXISTS(SELECT 1 FROM entries WHERE entries.id=scan_targets.id)", {qlonglong(source.scan_epoch > 1 ? source.scan_epoch - 1 : 0)});
        source.catalog.exec("INSERT INTO scan_queue VALUES(?)", {uuid(source.root_id)});
        transaction.commit();
    } catch (const Failure& failure) {
        source.epoch = old_epoch; source.scan_epoch = old_scan_epoch; source.generation = old_generation; source.entry_count = old_count;
        source.status = NamespaceStatus::Updating; source.readiness_error = failure.error;
        throw;
    }
    if (reset_retries) source.scan_retries = 0;
    source.scanning_active = true;
    source.status = NamespaceStatus::Updating;
    source.readiness_error.reset();
    if (revalidate) source.cursors.clear();
    source.next_reconcile = Clock::now() + std::chrono::milliseconds(source.config.limits.reconcile_interval_ms);
}
class BackgroundIoPriority {
public:
    BackgroundIoPriority() {
        previous_ = int(::syscall(SYS_ioprio_get, IOPRIO_WHO_PROCESS, 0));
        if (previous_ >= 0) changed_ = ::syscall(SYS_ioprio_set, IOPRIO_WHO_PROCESS, 0, IOPRIO_PRIO_VALUE(IOPRIO_CLASS_IDLE, 0)) == 0;
    }
    ~BackgroundIoPriority() { if (changed_) ::syscall(SYS_ioprio_set, IOPRIO_WHO_PROCESS, 0, previous_); }
private:
    int previous_ = -1;
    bool changed_ = false;
};
void scanTurn(Source& source) {
    BackgroundIoPriority priority;
    validatePolicy(source);
    if (!source.scanning) {
        Statement next(source.catalog.db, "SELECT id FROM scan_queue ORDER BY rowid LIMIT 1");
        if (!next.next()) {
            source.catalog.exec("UPDATE entries SET status=1 WHERE id IN(SELECT e.id FROM entries e JOIN entries p ON p.id=e.parent WHERE p.status=1 AND e.status!=1 LIMIT ?)", {source.config.common.limits.max_batch});
            if (sqlite3_changes(source.catalog.db)) { advance(source); notify(source); return; }
            source.status = NamespaceStatus::Ready; source.readiness_error.reset(); source.scan_retries = 0;
            source.scanning_active = false;
            source.next_reconcile = Clock::now() + std::chrono::milliseconds(source.config.limits.reconcile_interval_ms);
            return;
        }
        auto current = std::make_unique<ScanDirectory>(); current->id = QUuid(next.text(0)); current->epoch = source.scan_epoch;
        auto opened = resolve(source, physicalPath(source, current->id));
        require(S_ISDIR(opened.identity.mode), ErrorCode::MissingEntry);
        Statement target(source.catalog.db, "SELECT id,epoch FROM scan_targets WHERE dev=? AND ino=?", {QString::number(opened.identity.dev), QString::number(opened.identity.ino)});
        if (target.next() && target.text(0) != uuid(current->id)) {
            const auto canonical = QUuid(target.text(0));
            bool preserve = quint64(target.number(1)) == source.scan_epoch;
            if (!preserve) {
                try { preserve = resolve(source, physicalPath(source, canonical)).identity.sameObject(opened.identity); }
                catch (const Failure&) {}
            }
            if (preserve) {
                source.catalog.exec("DELETE FROM scan_queue WHERE id=?", {uuid(current->id)});
                if (quint64(target.number(1)) != source.scan_epoch) source.catalog.exec("INSERT OR IGNORE INTO scan_queue VALUES(?)", {uuid(canonical)});
                return;
            }
        }
        source.catalog.exec("INSERT INTO scan_targets VALUES(?,?,?,?) ON CONFLICT(dev,ino) DO UPDATE SET id=excluded.id,epoch=excluded.epoch",
            {QString::number(opened.identity.dev), QString::number(opened.identity.ino), uuid(current->id), qlonglong(source.scan_epoch)});
        current->before = opened.identity; current->fd = contentFd(opened, true);
        current->stream = ::fdopendir(::fcntl(current->fd.value, F_DUPFD_CLOEXEC, 0)); ioCheck(current->stream != nullptr);
        source.scanning = std::move(current);
    }
    auto& current = *source.scanning;
    DbTransaction transaction(source.catalog);
    bool visible_change = false;
    auto mark_change = [&] { if (!visible_change) { advance(source); visible_change = true; } };
    quint32 count = 0;
    while (count++ < source.config.common.limits.max_batch) {
        errno = 0; const auto* de = current.enumerated ? nullptr : ::readdir(current.stream);
        if (!de) {
            ioCheck(errno == 0);
            require(evidence(current.fd.value).sameVersion(current.before), ErrorCode::ExternalModification);
            current.enumerated = true;
            Statement missing(source.catalog.db, "SELECT id FROM entries WHERE parent=? AND status IN(0,2,3) AND seen<? LIMIT 1", {uuid(current.id), qlonglong(source.scan_epoch)});
            if (missing.next()) {
                mark_change();
                source.catalog.exec("UPDATE entries SET status=1,generation=? WHERE id IN(SELECT id FROM entries WHERE parent=? AND status IN(0,2,3) AND seen<? LIMIT ?)",
                    {qlonglong(source.generation), uuid(current.id), qlonglong(source.scan_epoch), source.config.common.limits.max_batch});
                transaction.commit(); notify(source); return;
            }
            auto opened = resolve(source, physicalPath(source, current.id));
            require(opened.identity.sameVersion(current.before), ErrorCode::ExternalModification);
            auto parent = entry(source, current.id);
            if (!parent.identity.sameVersion(opened.identity) || parent.info.status != EntryStatus::Available || parent.epoch != source.epoch) {
                mark_change(); writeEntry(source, current.id, parent.info.parent, parent.raw, opened);
            }
            source.catalog.exec("DELETE FROM scan_queue WHERE id=?", {uuid(current.id)});
            transaction.commit(); source.scanning.reset(); if (visible_change) notify(source); return;
        }
        const QByteArray raw(de->d_name); if (raw == "." || raw == "..") continue;
        auto encoded = encodeLinuxName(raw); require(bool(encoded), ErrorCode::InvalidName);
        std::optional<Opened> opened;
        try {
            opened.emplace(resolve(source, physicalPath(source, current.id) + '/' + raw));
            require(S_ISREG(opened->identity.mode) || S_ISDIR(opened->identity.mode), ErrorCode::MissingEntry);
        } catch (const Failure& failure) {
            if (failure.error.code != ErrorCode::MissingEntry && failure.error.code != ErrorCode::PermissionDenied) throw;
            // A known inaccessible object is hidden immediately; unrelated unseen
            // entries remain untouched until this enumeration finishes.
            Statement old(source.catalog.db, "SELECT status FROM entries WHERE parent=? AND raw=?", {uuid(current.id), raw});
            if (old.next() && old.number(0) != int(EntryStatus::Inconsistent)) {
                mark_change(); source.catalog.exec("UPDATE entries SET status=2,seen=?,generation=? WHERE parent=? AND raw=?",
                    {qlonglong(source.scan_epoch), qlonglong(source.generation), uuid(current.id), raw});
            }
            continue;
        }
        QUuid id; QByteArray hash; bool unchanged = false;
        Statement previous(source.catalog.db, "SELECT id FROM entries WHERE parent=? AND raw=?", {uuid(current.id), raw});
        if (previous.next()) {
            const auto old = entry(source, QUuid(previous.text(0)));
            // Same pathname plus unchanged identity and ctime is strong enough
            // for unchanged entries. An uncertain replacement receives a new ID.
            if (old.identity.sameObject(opened->identity) && old.identity.mode == opened->identity.mode
                && ((old.identity.birth && opened->identity.birth) || old.identity.sameVersion(opened->identity))) {
                id = old.info.id;
                if (old.identity.sameVersion(opened->identity)) {
                    hash = old.info.sha256;
                    unchanged = old.info.status == EntryStatus::Available && old.epoch == source.epoch && old.alias == opened->alias;
                }
            } else {
                mark_change(); source.catalog.exec("UPDATE entries SET parent=NULL,status=1 WHERE id=?", {uuid(old.info.id)});
            }
        }
        if (id.isNull() && opened->identity.birth && !opened->alias) {
            // Birth time disambiguates inode reuse where the filesystem supplies
            // it. Require a unique candidate and prove the old name is absent;
            // never merge existing copies or resolve ambiguity by query order.
            Statement candidate(source.catalog.db, "SELECT id FROM entries WHERE dev=? AND ino=? AND birth=? AND alias=0 LIMIT 2",
                {QString::number(opened->identity.dev), QString::number(opened->identity.ino), opened->identity.birth});
            if (candidate.next()) {
                const auto previous_id = QUuid(candidate.text(0));
                if (!candidate.next()) {
                    try { resolve(source, physicalPath(source, previous_id, true)); }
                    catch (const Failure& failure) {
                        if (failure.error.code == ErrorCode::MissingEntry && failure.error.native_code == ENOENT) id = previous_id;
                    }
                }
            }
        }
        if (id.isNull()) {
            // Without unambiguous evidence, assign a fresh identity.
            require(source.entry_count < source.config.common.limits.max_entries, ErrorCode::QuotaExceeded);
            ++source.entry_count; id = QUuid::createUuid();
        }
        if (unchanged) source.catalog.exec("UPDATE entries SET seen=? WHERE id=?", {qlonglong(source.scan_epoch), uuid(id)});
        else { mark_change(); writeEntry(source, id, current.id, raw, *opened, hash); }
        if (S_ISDIR(opened->identity.mode) && !opened->alias) {
            Statement queued(source.catalog.db, "SELECT count(*) FROM scan_queue"); queued.next();
            require(quint64(queued.number(0)) < source.config.limits.max_scan_directories, ErrorCode::QueueFull);
            source.catalog.exec("INSERT OR IGNORE INTO scan_queue VALUES(?)", {uuid(id)});
        }
    }
    transaction.commit(); if (visible_change) notify(source);
}
QByteArray snapshotName(QByteArray token);
bool leasePinned(const Source& source, QByteArrayView token);
void cleanupLeases(Source& source, bool startup = false) {
    Statement leases(source.catalog.db, "SELECT token,stage_dev,stage_ino FROM leases WHERE expires<=? OR exclusions!=? OR (? AND length(sha)!=32) LIMIT ?",
        {QDateTime::currentMSecsSinceEpoch(), qlonglong(source.exclusion_generation), int(startup), source.config.common.limits.max_batch});
    while (leases.next()) {
        const auto token = leases.bytes(0); if (leasePinned(source, token)) continue;
        const auto name = snapshotName(token);
        struct stat st{};
        if (::fstatat(source.internal.value, name.constData(), &st, AT_SYMLINK_NOFOLLOW) == 0) {
            require(S_ISREG(st.st_mode) && st.st_uid == ::geteuid() && st.st_nlink == 1
                && quint64(st.st_dev) == leases.text(1).toULongLong() && quint64(st.st_ino) == leases.text(2).toULongLong(), ErrorCode::IntegrityError);
            ioCheck(::unlinkat(source.internal.value, name.constData(), 0) == 0);
            ioCheck(::fsync(source.internal.value) == 0);
        } else ioCheck(errno == ENOENT);
        source.catalog.exec("DELETE FROM leases WHERE token=?", {token});
    }
}
void expireReaders(Source& source);
std::shared_ptr<Observer> observer(NamespaceId ns, std::optional<DirectoryId> directory, DirectoryChangeCallback callback, AccessContext access) {
    require(callback.context && callback.callback && callback.context->thread() == QThread::currentThread(), ErrorCode::InvalidArgument);
    auto sub = std::make_shared<Observer>(); sub->ns = ns; sub->directory = directory;
    sub->context = callback.context; sub->callback = std::move(callback.callback); sub->access = std::move(access);
    sub->dispatch = std::shared_ptr<QObject>(new QObject, [](QObject* object) { object->deleteLater(); });
    std::weak_ptr<Observer> weak = sub;
    QObject::connect(callback.context, &QObject::destroyed, sub->dispatch.get(), [weak] {
        if (auto item = weak.lock()) item->active->store(false);
    }, Qt::DirectConnection);
    return sub;
}
} // namespace
struct ReadOnlyFileManager::Impl : std::enable_shared_from_this<ReadOnlyFileManager::Impl> {
    QString private_root;
    std::shared_ptr<Scheduler> scheduler = Scheduler::create();
    QUuid lane = QUuid::createUuid();
    QUuid background_group = QUuid::createUuid();
    std::map<NamespaceId, std::shared_ptr<Source>> sources;
    std::shared_ptr<QObject> control;
    QFileSystemWatcher* watcher = nullptr;
    QTimer* timer = nullptr;
    QHash<QString, quint32> watched_roots; // Qt event-thread ownership.
    std::atomic_bool stopped{false};
    std::atomic<quint32> operation_limit{128};
    std::atomic<quint64> buffer_limit{8U << 20};
    std::atomic_bool tick_queued{false};
    NamespaceId last_background;
    std::mutex registration_mutex;
    std::map<NamespaceId, std::weak_ptr<Source>> known_sources;
    std::map<quint64, std::weak_ptr<Observer>> registrations;
    quint64 next_registration = 1;
    quint64 registerObserver(const std::shared_ptr<Source>& source, const std::shared_ptr<Observer>& sub) {
        std::lock_guard registry_lock(registration_mutex);
        require(!stopped && !source->stopped, ErrorCode::ShuttingDown);
        std::erase_if(registrations, [](const auto& item) { auto sub = item.second.lock(); return !sub || !sub->active->load(); });
        require(registrations.size() < 4096 && next_registration < std::numeric_limits<quint64>::max(), ErrorCode::QuotaExceeded);
        std::lock_guard source_lock(source->observer_mutex);
        std::erase_if(source->observers, [](const auto& old) { return !old->active->load(); });
        require(source->observers.size() < source->subscription_limits.max_notifications, ErrorCode::QuotaExceeded);
        const auto token = next_registration++; registrations.emplace(token, sub); source->observers.push_back(sub); return token;
    }
    ~Impl() { scheduler->stop(); }
    std::shared_ptr<Source> source(NamespaceId ns) {
        require(!stopped, ErrorCode::ShuttingDown);
        auto it = sources.find(ns); require(it != sources.end(), ErrorCode::MissingEntry); return it->second;
    }
    std::function<bool()> deliveryCheck(NamespaceId ns, const AccessContext& access) {
        std::shared_ptr<std::atomic<quint64>> epoch;
        {
            std::lock_guard lock(registration_mutex); auto found = known_sources.find(ns);
            if (found != known_sources.end()) if (auto source = found->second.lock()) epoch = source->access_epoch;
        }
        const auto selected = epoch ? epoch->load() : 0;
        return [epoch, selected, access] { return epoch && selected && epoch->load() == selected && access.current(); };
    }
    template<typename T, typename Fn> QCoro::Task<Result<T>> run(Fn fn, quint64 bytes = 0) {
        auto self = shared_from_this();
        std::function<Result<T>()> work = [self, fn = std::move(fn)]() mutable -> Result<T> {
            require(!self->stopped, ErrorCode::ShuttingDown); return fn(*self);
        };
        return execute<T>(scheduler, lane, bytes, operation_limit.load(), buffer_limit.load(), std::move(work));
    }
    void watch(QString path, bool add, quint32 budget = 1) {
        auto self = shared_from_this();
        QMetaObject::invokeMethod(control.get(), [self, path = std::move(path), add, budget] {
            if (self->stopped) return;
            if (add) {
                if (!budget) return;
                auto found = self->watched_roots.find(path);
                if (found != self->watched_roots.end()) { ++found.value(); return; }
                if (self->watched_roots.size() < 4096 && self->watcher->addPath(path)) self->watched_roots.insert(path, 1);
            } else {
                auto found = self->watched_roots.find(path);
                if (found != self->watched_roots.end() && --found.value() == 0) {
                    self->watcher->removePath(path); self->watched_roots.erase(found);
                }
            }
        }, Qt::QueuedConnection);
    }
    void tick(bool = false) {
        if (stopped || tick_queued.exchange(true)) return;
        auto self = shared_from_this();
        if (!scheduler->enqueue({lane, 0, [self] {
            bool more = false;
            // One bounded directory batch per worker turn, round-robin across
            // namespaces. Interactive work can run between any two batches.
            auto candidate = self->sources.upper_bound(self->last_background);
            for (size_t n = 0; n < self->sources.size(); ++n) {
                if (candidate == self->sources.end()) candidate = self->sources.begin();
                auto source = candidate->second; self->last_background = candidate->first; ++candidate;
                if (source->stopped) continue;
                try {
                    expireReaders(*source);
                    const bool due = Clock::now() >= source->next_reconcile;
                    if ((!source->scanning_active || source->readiness_error) && !due) continue;
                    if (source->readiness_error && source->scan_retries >= source->config.limits.max_scan_retries) continue;
                    cleanupLeases(*source);
                    if (source->readiness_error) { ++source->scan_retries; beginScan(*source, false); }
                    else if (!source->scanning_active) beginScan(*source);
                    scanTurn(*source);
                } catch (const Failure& failure) {
                    Statement entries(source->catalog.db, "SELECT count(*) FROM entries"); entries.next(); source->entry_count = quint64(entries.number(0)); source->generation = source->catalog.meta("generation").toULongLong();
                    source->readiness_error = failure.error;
                    source->status = failure.error.code == ErrorCode::Corruption || failure.error.code == ErrorCode::StorageUnavailable
                        ? NamespaceStatus::Failed : NamespaceStatus::Updating;
                    source->next_reconcile = Clock::now() + std::chrono::milliseconds(source->config.limits.reconcile_interval_ms);
                    source->scanning.reset(); notify(*source);
                    LFLOG_DEBUG << "Read-only scan paused; code=" << int(failure.error.code);
                } catch (...) { source->readiness_error = Error{ErrorCode::IoError}; source->scanning.reset(); }
                break;
            }
            for (const auto& [id, source] : self->sources)
                more |= !source->stopped && ((source->scanning_active && !source->readiness_error)
                    || (Clock::now() >= source->next_reconcile && (!source->readiness_error || source->scan_retries < source->config.limits.max_scan_retries)));
            self->tick_queued = false;
            if (more && !self->stopped) QMetaObject::invokeMethod(self->control.get(), [self] { self->tick(); }, Qt::QueuedConnection);
        }, background_group}, 2, 8U << 20)) tick_queued = false;
    }
};
namespace {
void checkNamespaceInventory(int root, quint32 maximum, bool creating) {
    auto* stream = ::fdopendir(::fcntl(root, F_DUPFD_CLOEXEC, 0)); ioCheck(stream != nullptr);
    std::unique_ptr<DIR, int(*)(DIR*)> guard(stream, ::closedir);
    quint32 namespaces = 0, entries = 0;
    for (;;) {
        errno = 0; const auto* item = ::readdir(stream); if (!item) { ioCheck(errno == 0); break; }
        const QByteArray raw(item->d_name); if (raw == "." || raw == "..") continue;
        require(++entries <= maximum * 2 + 16, ErrorCode::QuotaExceeded);
        const QUuid id(QString::fromLatin1(raw));
        if (!id.isNull() && uuid(id).toLatin1() == raw) ++namespaces;
    }
    require(creating ? namespaces < maximum : namespaces <= maximum, ErrorCode::QuotaExceeded);
}
quint64 recoveryCharge(int private_root, const QString& name, quint64 limit) {
    struct stat present{};
    if (::fstatat(private_root, QFile::encodeName(name).constData(), &present, AT_SYMLINK_NOFOLLOW) < 0) { ioCheck(errno == ENOENT); return 0; }
    auto directory = openBeneath(private_root, name, O_RDONLY | O_DIRECTORY); identity(directory.value, true);
    auto* stream = ::fdopendir(::fcntl(directory.value, F_DUPFD_CLOEXEC, 0)); ioCheck(stream != nullptr);
    std::unique_ptr<DIR, int(*)(DIR*)> guard(stream, ::closedir);
    quint64 total = 0; quint32 entries = 0;
    for (;;) {
        errno = 0; const auto* item = ::readdir(stream); if (!item) { ioCheck(errno == 0); return total; }
        const QByteArray raw(item->d_name); if (raw == "." || raw == "..") continue;
        require(++entries <= 8192, ErrorCode::QuotaExceeded);
        struct stat st{}; ioCheck(::fstatat(directory.value, raw.constData(), &st, AT_SYMLINK_NOFOLLOW) == 0);
        require(S_ISREG(st.st_mode) && st.st_uid == ::geteuid() && st.st_nlink == 1 && st.st_size >= 0, ErrorCode::UnsupportedCapability);
        require(quint64(st.st_size) <= limit - total, ErrorCode::QuotaExceeded); total += quint64(st.st_size);
    }
}
std::shared_ptr<Source> openSourceState(ReadOnlyFileManager::Impl& impl, ReadOnlyNamespaceConfig config, bool create, CancellationToken cancel) {
    checkConfig(config); cancelCheck(cancel);
    require(!impl.sources.contains(config.common.id), ErrorCode::NamespaceBusy);
    require(impl.sources.size() < config.limits.max_namespaces, ErrorCode::QuotaExceeded);
    auto source = std::make_shared<Source>(); source->subscription_limits = config.common.limits; source->config = std::move(config);
    auto private_root = openRoot(impl.private_root, true);
    checkNamespaceInventory(private_root.value, source->config.limits.max_namespaces, create);
    const auto name = uuid(source->config.common.id);
    source->recovery_bytes = recoveryCharge(private_root.value, name + ".reindex-backup", source->config.common.limits.max_total_bytes);
    if (create) { makeDirectory(private_root.value, name); }
    source->internal = openBeneath(private_root.value, name, O_RDONLY | O_DIRECTORY);
    identity(source->internal.value, true);
    source->owner_lock = openBeneath(source->internal.value, "owner.lock", O_RDWR | O_CREAT, 0600);
    identity(source->owner_lock.value);
    if (::flock(source->owner_lock.value, LOCK_EX | LOCK_NB) != 0) fail(ErrorCode::NamespaceBusy);
    source->excluded = prepareExclusions(*source, source->config.exclusions, impl.private_root);
    auto root = resolve(*source, QFile::encodeName(source->config.common.storage_root), false);
    require(S_ISDIR(root.identity.mode) && !root.alias, ErrorCode::InvalidArgument);
    source->root = std::move(root.fd); source->root_identity = root.identity;
    auto catalog = openBeneath(source->internal.value, "catalog.db", O_RDWR | (create ? O_CREAT | O_EXCL : 0), create ? 0600 : 0);
    identity(catalog.value);
    registerSqliteVfs();
    const auto uri = QByteArray("file:/dsfm/") + QByteArray::number(source->internal.value) + "/catalog.db?vfs=dsfm";
    require(sqlite3_open_v2(uri.constData(), &source->catalog.db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_URI | SQLITE_OPEN_FULLMUTEX, "dsfm") == SQLITE_OK,
        ErrorCode::StorageUnavailable);
    auto& db = source->catalog;
    require(installLabelOrder(db.db),ErrorCode::StorageUnavailable);
    sqlite3_db_config(db.db, SQLITE_DBCONFIG_DEFENSIVE, 1, nullptr);
    sqlite3_db_config(db.db, SQLITE_DBCONFIG_TRUSTED_SCHEMA, 0, nullptr);
    sqlite3_enable_load_extension(db.db, 0);
    sqlite3_limit(db.db, SQLITE_LIMIT_LENGTH, 1 << 20); sqlite3_limit(db.db, SQLITE_LIMIT_ATTACHED, 0);
    db.exec("PRAGMA journal_mode=DELETE"); db.exec("PRAGMA synchronous=FULL"); db.exec("PRAGMA temp_store=MEMORY"); db.exec("PRAGMA cache_size=-1024");
    const auto max_pages = QByteArray("PRAGMA max_page_count=") + QByteArray::number(source->config.common.limits.max_catalog_bytes / 4096);
    db.exec(max_pages.constData());
    if (create) {
        DbTransaction transaction(db);
        db.exec("CREATE TABLE meta(key TEXT PRIMARY KEY,value BLOB NOT NULL)");
        db.exec("CREATE TABLE entries(id TEXT PRIMARY KEY,parent TEXT,name TEXT COLLATE BINARY NOT NULL,type INTEGER NOT NULL,status INTEGER NOT NULL,size INTEGER NOT NULL,mtime INTEGER NOT NULL,sha BLOB,generation INTEGER NOT NULL,raw BLOB NOT NULL,dev TEXT NOT NULL,ino TEXT NOT NULL,mount TEXT NOT NULL,ctime INTEGER NOT NULL,mode INTEGER NOT NULL,links INTEGER NOT NULL,alias INTEGER NOT NULL,epoch INTEGER NOT NULL,seen INTEGER NOT NULL,birth INTEGER NOT NULL,UNIQUE(parent,raw))");
        db.exec("CREATE INDEX entries_parent ON entries(parent,name COLLATE BINARY,id)");
        db.exec("CREATE INDEX entries_ascii ON entries(parent,dsfm_ascii(name) COLLATE BINARY,id)");
        db.exec("CREATE INDEX entries_identity ON entries(dev,ino,birth,alias)");
        db.exec("CREATE INDEX entries_reconcile ON entries(parent,status,seen)");
        db.exec("CREATE INDEX entries_history ON entries(status,seen,id)");
        db.exec("CREATE TABLE scan_queue(id TEXT PRIMARY KEY)");
        db.exec("CREATE TABLE scan_targets(dev TEXT,ino TEXT,id TEXT NOT NULL,epoch INTEGER NOT NULL,PRIMARY KEY(dev,ino))");
        db.exec("CREATE TABLE hash_jobs(id TEXT PRIMARY KEY)");
        db.exec("CREATE TABLE leases(token BLOB PRIMARY KEY,id TEXT NOT NULL,size INTEGER NOT NULL,sha BLOB NOT NULL,expires INTEGER NOT NULL,exclusions INTEGER NOT NULL,dev TEXT NOT NULL,ino TEXT NOT NULL,mtime INTEGER NOT NULL,ctime INTEGER NOT NULL,birth INTEGER NOT NULL,stage_dev TEXT NOT NULL DEFAULT '',stage_ino TEXT NOT NULL DEFAULT '',source_generation INTEGER NOT NULL DEFAULT 0)");
        db.meta("schema", "readonly-1"); db.meta("config", configuration(source->config));
        db.meta("namespace", name.toUtf8()); db.meta("rootDevice", QByteArray::number(root.identity.dev));
        db.meta("rootInode", QByteArray::number(root.identity.ino)); db.meta("rootBirth", QByteArray::number(root.identity.birth));
        db.meta("rootCtime", QByteArray::number(root.identity.ctime));
        const auto catalog_identity = evidence(catalog.value), directory_identity = evidence(source->internal.value);
        db.meta("catalogDevice", QByteArray::number(catalog_identity.dev)); db.meta("catalogInode", QByteArray::number(catalog_identity.ino));
        db.meta("directoryDevice", QByteArray::number(directory_identity.dev)); db.meta("directoryInode", QByteArray::number(directory_identity.ino));
        source->root_id = QUuid::createUuid(); db.meta("rootId", uuid(source->root_id).toUtf8());
        writeEntry(*source, source->root_id, {}, {}, {duplicate(source->root.value), source->root_identity, false});
        transaction.commit();
    } else {
        require(db.meta("schema") == "readonly-1", ErrorCode::UnsupportedCapability);
        auto stored_config = QJsonDocument::fromJson(db.meta("config")).object();
        auto requested_config = QJsonDocument::fromJson(configuration(source->config)).object();
        const auto previous_exclusions = stored_config.value("exclusionGeneration").toString().toULongLong();
        require(db.meta("namespace") == name.toUtf8(), ErrorCode::Conflict);
        if (stored_config != requested_config) {
            for (const auto key : {"directories", "objects", "exclusionGeneration"}) { stored_config.remove(key); requested_config.remove(key); }
            require(stored_config == requested_config && source->config.exclusions.generation > previous_exclusions, ErrorCode::Conflict);
            db.meta("config", configuration(source->config));
        }
        const auto catalog_identity = evidence(catalog.value), directory_identity = evidence(source->internal.value);
        require(db.meta("catalogDevice").toULongLong() == catalog_identity.dev && db.meta("catalogInode").toULongLong() == catalog_identity.ino
            && db.meta("directoryDevice").toULongLong() == directory_identity.dev && db.meta("directoryInode").toULongLong() == directory_identity.ino,
            ErrorCode::Corruption);
        const auto stored_birth = db.meta("rootBirth").toLongLong();
        require(db.meta("rootDevice").toULongLong() == root.identity.dev && db.meta("rootInode").toULongLong() == root.identity.ino
            && (stored_birth && root.identity.birth ? stored_birth == root.identity.birth : db.meta("rootCtime").toLongLong() == root.identity.ctime), ErrorCode::ExternalModification);
        source->root_id = QUuid(QString::fromUtf8(db.meta("rootId"))); require(!source->root_id.isNull(), ErrorCode::Corruption);
        bool generation_ok = false, epoch_ok = false;
        source->generation = db.meta("generation").toULongLong(&generation_ok); source->epoch = db.meta("epoch").toULongLong(&epoch_ok); source->scan_epoch = db.meta("scanEpoch").toULongLong();
        require(generation_ok && epoch_ok && source->generation && source->epoch && source->generation < quint64(std::numeric_limits<qint64>::max())
            && source->epoch < quint64(std::numeric_limits<qint64>::max()) && source->scan_epoch && source->scan_epoch < quint64(std::numeric_limits<qint64>::max()), ErrorCode::Corruption);
    }
    Statement entries(db.db, "SELECT count(*) FROM entries"); entries.next(); source->entry_count = quint64(entries.number(0));
    db.exec("DELETE FROM hash_jobs");
    source->exclusion_generation = source->config.exclusions.generation; source->access_epoch->store(source->exclusion_generation);
    cleanupLeases(*source, true);
    validatePolicy(*source); beginScan(*source, true, true); cancelCheck(cancel);
    impl.operation_limit = std::min(impl.operation_limit.load(), source->config.common.limits.max_operations);
    impl.buffer_limit = std::min(impl.buffer_limit.load(), source->config.common.limits.max_buffered_bytes);
    impl.sources.emplace(source->config.common.id, source);
    { std::lock_guard lock(impl.registration_mutex); impl.known_sources[source->config.common.id] = source; }
    LFLOG_DEBUG << "Read-only namespace opened; initial discovery scheduled";
    return source;
}
} // namespace
ReadOnlyFileManager::ReadOnlyFileManager(QString private_root) : impl_(std::make_shared<Impl>()) {
    impl_->private_root = std::move(private_root);
    auto* event_thread = new QThread;
    auto* control = new QObject;
    QObject::connect(event_thread, &QThread::finished, control, &QObject::deleteLater);
    QObject::connect(event_thread, &QThread::finished, event_thread, &QObject::deleteLater);
    impl_->control = std::shared_ptr<QObject>(control, [event_thread](QObject*) { event_thread->quit(); });
    impl_->timer = new QTimer(impl_->control.get()); impl_->timer->setInterval(1000);
    impl_->watcher = new QFileSystemWatcher(impl_->control.get());
    std::weak_ptr<Impl> weak = impl_;
    QObject::connect(impl_->timer, &QTimer::timeout, impl_->control.get(), [weak] {
        if (auto impl = weak.lock()) impl->tick(true);
    });
    QObject::connect(impl_->watcher, &QFileSystemWatcher::directoryChanged, impl_->control.get(), [weak] {
        if (auto impl = weak.lock()) {
            impl->scheduler->enqueue({impl->lane, 0, [impl] {
                if (impl->stopped) return;
                for (auto& [id, source] : impl->sources) if (!source->scanning_active) beginScan(*source);
            }, impl->background_group}, 2, 8U << 20);
            impl->tick();
        }
    });
    control->moveToThread(event_thread); event_thread->start();
    QMetaObject::invokeMethod(control, [timer = impl_->timer] { timer->start(); }, Qt::QueuedConnection);
}
ReadOnlyFileManager::~ReadOnlyFileManager() {
    impl_->stopped = true;
    // Worker-owned sources/readers retain safe state until their queued work drains.
    impl_->scheduler->enqueue({impl_->lane, 0, [impl = impl_] {
        for (auto& [id, source] : impl->sources) {
            source->stopped = true; source->access_epoch->store(0); expireReaders(*source); std::lock_guard lock(source->observer_mutex);
            for (const auto& sub : source->observers) sub->active->store(false);
        }
        impl->sources.clear();
    }}, 4096, 8U << 20, true);
}
QCoro::Task<Result<NamespaceInfo>> ReadOnlyFileManager::createSource(ReadOnlyNamespaceConfig config, CancellationToken cancel) {
    auto impl = impl_; const auto watch_budget = config.limits.max_watches;
    auto result = co_await impl->run<NamespaceInfo>([config = std::move(config), cancel](Impl& state) mutable -> Result<NamespaceInfo> {
        return sourceInfo(*openSourceState(state, std::move(config), true, cancel));
    });
    if (result) {
        impl->watch(result.value().config.storage_root, true, watch_budget);
        impl->tick();
    }
    co_return result;
}
QCoro::Task<Result<NamespaceInfo>> ReadOnlyFileManager::openSource(ReadOnlyNamespaceConfig config, CancellationToken cancel) {
    auto impl = impl_; const auto watch_budget = config.limits.max_watches;
    auto result = co_await impl->run<NamespaceInfo>([config = std::move(config), cancel](Impl& state) mutable -> Result<NamespaceInfo> {
        return sourceInfo(*openSourceState(state, std::move(config), false, cancel));
    });
    if (result) {
        impl->watch(result.value().config.storage_root, true, watch_budget);
        impl->tick();
    }
    co_return result;
}
QCoro::Task<Result<NamespaceInfo>> ReadOnlyFileManager::createNamespace(NamespaceConfig config, CancellationToken cancel) {
    ReadOnlyNamespaceConfig source; source.common = std::move(config); return createSource(std::move(source), cancel);
}
QCoro::Task<Result<NamespaceInfo>> ReadOnlyFileManager::openNamespace(NamespaceConfig config, CancellationToken cancel) {
    ReadOnlyNamespaceConfig source; source.common = std::move(config); return openSource(std::move(source), cancel);
}
QCoro::Task<Result<NamespaceInfo>> ReadOnlyFileManager::namespaceInfo(NamespaceId ns) {
    return impl_->run<NamespaceInfo>([ns](Impl& impl) -> Result<NamespaceInfo> { return sourceInfo(*impl.source(ns)); });
}
QCoro::Task<Result<QVector<NamespaceInfo>>> ReadOnlyFileManager::namespaces() {
    return impl_->run<QVector<NamespaceInfo>>([](Impl& impl) -> Result<QVector<NamespaceInfo>> {
        QVector<NamespaceInfo> result; for (const auto& [id, source] : impl.sources) result.append(sourceInfo(*source)); return result;
    });
}
QCoro::Task<Result<void>> ReadOnlyFileManager::closeSource(NamespaceId ns) {
    return impl_->run<void>([ns](Impl& impl) -> Result<void> {
        auto source = impl.source(ns); impl.watch(source->config.common.storage_root, false);
        source->stopped = true; source->access_epoch->store(0); expireReaders(*source); source->scanning.reset();
        { std::lock_guard lock(source->observer_mutex); for (const auto& sub : source->observers) sub->active->store(false); source->observers.clear(); }
        require(sqlite3_close(source->catalog.db) == SQLITE_OK, ErrorCode::Busy); source->catalog.db = nullptr;
        source->owner_lock = {}; impl.sources.erase(ns);
        { std::lock_guard lock(impl.registration_mutex); impl.known_sources.erase(ns); }
        return {};
    });
}
QCoro::Task<Result<ReadOnlyNamespaceConfig>> ReadOnlyFileManager::sourceConfig(NamespaceId ns) {
    return impl_->run<ReadOnlyNamespaceConfig>([ns](Impl& impl) -> Result<ReadOnlyNamespaceConfig> { return impl.source(ns)->config; });
}
QCoro::Task<Result<NamespaceInfo>> ReadOnlyFileManager::reindexSource(ReadOnlyNamespaceConfig config, CancellationToken cancel) {
    auto impl = impl_;
    auto result = co_await impl->run<NamespaceInfo>([config = std::move(config), cancel](Impl& state) mutable -> Result<NamespaceInfo> {
        checkConfig(config); cancelCheck(cancel);
        require(!state.sources.contains(config.common.id), ErrorCode::NamespaceBusy);
        auto private_root = openRoot(state.private_root, false); const auto name = uuid(config.common.id);
        auto previous = openBeneath(private_root.value, name, O_RDONLY | O_DIRECTORY); identity(previous.value, true);
        auto owner_lock = openBeneath(previous.value, "owner.lock", O_RDWR); identity(owner_lock.value);
        if (::flock(owner_lock.value, LOCK_EX | LOCK_NB) != 0) fail(ErrorCode::NamespaceBusy);
        auto old_catalog = openBeneath(previous.value, "catalog.db", O_RDONLY); identity(old_catalog.value);
        // Preserve unknown schemas/objects without interpreting or deleting them.
        // A preexisting backup causes Conflict, bounding retained rebuild history.
        recoveryCharge(private_root.value, name, config.common.limits.max_total_bytes);
        renameSafe(private_root.value, name, name + ".reindex-backup", false);
        return sourceInfo(*openSourceState(state, std::move(config), true, cancel));
    });
    if (result) impl->tick();
    co_return result;
}
QCoro::Task<Result<void>> ReadOnlyFileManager::updateExclusions(NamespaceId ns, ExclusionPolicy policy) {
    return impl_->run<void>([ns, policy = std::move(policy)](Impl& impl) -> Result<void> {
        auto source = impl.source(ns);
        require(policy.generation > source->exclusion_generation, ErrorCode::Conflict);
        auto excluded = prepareExclusions(*source, policy, impl.private_root);
        auto config = source->config; config.exclusions = policy;
        source->catalog.meta("config", configuration(config));
        source->excluded = std::move(excluded); source->config = std::move(config); source->exclusion_generation = policy.generation; source->access_epoch->store(policy.generation);
        expireReaders(*source);
        beginScan(*source, true, true); notify(*source); return {};
    });
}
namespace {
struct ReadState {
    std::shared_ptr<Source> source; Fd fd; Evidence opened; FileId id; NamespaceId namespace_id; AccessContext access;
    quint64 exclusions = 0;
    Clock::time_point expires;
    bool closed = false, snapshot = false, hashing = false;
    ReadVerification verification = ReadVerification::Cached;
    std::optional<Error> stopped_error;
    Fd staging;
    QByteArray lease;
    Evidence snapshot_identity;
    quint64 progress = 0, selected_generation = 0;
    quint32 io_bytes = 0;
    std::unique_ptr<QCryptographicHash> hash;
    QByteArray expected;
    ~ReadState() { /* handle accounting is released on the serialized worker lane */ }
};
bool leasePinned(const Source& source, QByteArrayView token) {
    for (const auto& weak : source.readers) {
        auto reader = weak.lock();
        if (reader && !reader->closed && reader->lease == token && Clock::now() < reader->expires) return true;
    }
    return false;
}
void validateReader(ReadState& reader) {
    FilesystemCheckScope scope(reader.access.filesystem_check);
    if (reader.stopped_error) throw Failure{*reader.stopped_error};
    require(!reader.closed && !reader.source->stopped, ErrorCode::ShuttingDown);
    require(Clock::now() < reader.expires, ErrorCode::Cancelled);
    require(reader.exclusions == reader.source->exclusion_generation, ErrorCode::MissingEntry);
    auto item = entry(*reader.source, reader.id);
    if (!reader.snapshot) require(item.info.status == EntryStatus::Available, item.info.status == EntryStatus::Updating ? ErrorCode::Busy : ErrorCode::MissingEntry);
    if (reader.snapshot) {
        validatePolicy(*reader.source); authorize(reader.access, reader.source->config.common.id, reader.id, AccessOperation::Read);
        auto opened = resolve(*reader.source, physicalPath(*reader.source, reader.id));
        require(opened.identity.sameObject(item.identity), ErrorCode::MissingEntry);
        authorizeAlias(*reader.source, item, reader.access, AccessOperation::Read);
        require(evidence(reader.fd.value).sameVersion(reader.snapshot_identity), ErrorCode::ExternalModification);
    } else validateEntry(*reader.source, item, reader.access, AccessOperation::Read);
    if (!reader.snapshot) require(evidence(reader.fd.value).sameVersion(reader.opened), ErrorCode::ExternalModification);
}
QByteArray readBytes(ReadState& reader, quint64 offset, qsizetype max_bytes, CancellationToken cancel) {
    cancelCheck(cancel); validateReader(reader);
    const auto& limits = reader.source->config.common.limits;
    require(max_bytes >= 0 && quint64(max_bytes) <= limits.max_io_bytes && offset <= quint64(std::numeric_limits<qint64>::max())
        && quint64(max_bytes) <= quint64(std::numeric_limits<qint64>::max()) - offset, ErrorCode::InvalidArgument);
    const auto length = offset >= reader.opened.size ? 0 : std::min<quint64>(quint64(max_bytes), reader.opened.size - offset);
    QByteArray bytes(qsizetype(length), Qt::Uninitialized); qsizetype done = 0;
    while (done < bytes.size()) {
        cancelCheck(cancel);
        const auto count = ::pread(reader.fd.value, bytes.data() + done, size_t(bytes.size() - done), off_t(offset + quint64(done)));
        if (count < 0 && errno == EINTR) continue;
        ioCheck(count >= 0); require(count > 0, ErrorCode::ExternalModification); done += count;
    }
    validateReader(reader); cancelCheck(cancel); return bytes;
}
QByteArray snapshotName(QByteArray token) {
    require(token.size() == 32 && QByteArray::fromHex(token).size() == 16 && QByteArray::fromHex(token).toHex() == token, ErrorCode::InvalidArgument);
    return "snapshot-" + token;
}
void releaseReader(ReadState& reader) {
    if (reader.closed) return;
    reader.closed = true; reader.fd = {}; reader.staging = {};
    if (reader.hashing) {
        reader.source->catalog.exec("DELETE FROM hash_jobs WHERE id=?", {uuid(reader.id)});
        advance(*reader.source); reader.source->catalog.exec("UPDATE entries SET sha=NULL,generation=? WHERE id=?", {qlonglong(reader.source->generation), uuid(reader.id)});
        reader.hashing = false; notify(*reader.source);
    }
    if (!reader.snapshot && !reader.lease.isEmpty()) {
        const auto name = snapshotName(reader.lease);
        struct stat st{};
        if (::fstatat(reader.source->internal.value, name.constData(), &st, AT_SYMLINK_NOFOLLOW) == 0) {
            require(S_ISREG(st.st_mode) && quint64(st.st_dev) == reader.snapshot_identity.dev && quint64(st.st_ino) == reader.snapshot_identity.ino, ErrorCode::IntegrityError);
            ioCheck(::unlinkat(reader.source->internal.value, name.constData(), 0) == 0);
        } else ioCheck(errno == ENOENT);
        reader.source->catalog.exec("DELETE FROM leases WHERE token=?", {reader.lease});
    }
    if (reader.source->handles) --reader.source->handles;
}
void expireReaders(Source& source) {
    std::erase_if(source.readers, [&source](const auto& weak) {
        auto reader = weak.lock(); if (!reader || reader->closed) return true;
        if (source.stopped || reader->exclusions != source.exclusion_generation || Clock::now() >= reader->expires || !reader->access.current()) {
            const auto code = source.stopped ? ErrorCode::ShuttingDown : Clock::now() >= reader->expires ? ErrorCode::Cancelled : ErrorCode::MissingEntry;
            reader->stopped_error = Error{code}; releaseReader(*reader); return true;
        }
        return false;
    });
}
void stageChunk(ReadState& state, QByteArrayView bytes) {
    qsizetype done = 0;
    while (done < bytes.size()) {
        const auto count = ::write(state.staging.value, bytes.data() + done, size_t(bytes.size() - done));
        if (count < 0 && errno == EINTR) continue;
        ioCheck(count > 0); done += count;
    }
}
class ReadOnlyReader final : public IFileReader {
public:
    ReadOnlyReader(std::shared_ptr<ReadOnlyFileManager::Impl> impl, std::shared_ptr<ReadState> state) : impl_(std::move(impl)), state_(std::move(state)) {}
    std::function<bool()> accessGuard() const override {
        auto state = state_;
        return [state] {
            try {
                if (state->source->stopped || state->source->access_epoch->load() != state->exclusions
                    || Clock::now() >= state->expires || !state->access.current()) return false;
                authorize(state->access, state->namespace_id, state->id, AccessOperation::Read);
                return !state->access.filesystem_check || state->access.filesystem_check({});
            } catch (...) { return false; }
        };
    }
    ~ReadOnlyReader() override {
        auto state = state_;
        impl_->scheduler->enqueue({impl_->lane, 0, [state] {
            releaseReader(*state);
        }}, 4096, 8U << 20, true);
    }
    QCoro::Task<Result<QByteArray>> readAt(quint64 offset, qsizetype max_bytes, CancellationToken cancel) override {
        auto state = state_;
        if (max_bytes < 0 || quint64(max_bytes) > state->io_bytes) co_return Error{ErrorCode::InvalidArgument};
        auto result = co_await impl_->run<QByteArray>([state, offset, max_bytes, cancel](auto&) -> Result<QByteArray> {
            try { return readBytes(*state, offset, max_bytes, cancel); }
            catch (const Failure& failure) {
                if (failure.error.code == ErrorCode::ExternalModification || failure.error.code == ErrorCode::Busy) {
                    changed(*state->source, state->id); notify(*state->source);
                }
                throw;
            }
        }, quint64(max_bytes));
        if (result && (state->source->access_epoch->load() != state->exclusions || !state->access.current())) co_return Error{ErrorCode::MissingEntry};
        if (result && Clock::now() >= state->expires) co_return Error{ErrorCode::Cancelled};
        co_return result;
    }
    QCoro::Task<Result<void>> invalidate() override {
        auto state = state_;
        return impl_->run<void>([state](auto&) -> Result<void> {
            require(!state->closed && !state->source->stopped, ErrorCode::ShuttingDown);
            advance(*state->source);
            state->source->catalog.exec("UPDATE entries SET sha=NULL,status=3,generation=? WHERE id=?", {qlonglong(state->source->generation), uuid(state->id)});
            state->source->next_reconcile = Clock::now(); notify(*state->source); return {};
        });
    }
    QCoro::Task<Result<void>> close() override {
        auto state = state_;
        return impl_->run<void>([state](auto&) -> Result<void> {
            releaseReader(*state);
            return {};
        });
    }
private:
    std::shared_ptr<ReadOnlyFileManager::Impl> impl_;
    std::shared_ptr<ReadState> state_;
};
class AliasReader final : public IFileReader {
public:
    AliasReader(std::unique_ptr<IFileReader> guard, std::unique_ptr<IFileReader> target)
        : guard_(std::move(guard)), target_(std::move(target)) {}
    std::function<bool()> accessGuard() const override {
        auto guard = guard_->accessGuard(); auto target = target_->accessGuard();
        return [guard, target] { try { return guard() && target(); } catch (...) { return false; } };
    }
    QCoro::Task<Result<QByteArray>> readAt(quint64 offset, qsizetype maximum, CancellationToken cancel) override {
        auto check = co_await guard_->readAt(0, 0, cancel);
        if (!check) { co_await target_->close(); co_await guard_->close(); co_return check.error(); }
        auto result = co_await target_->readAt(offset, maximum, cancel);
        check = co_await guard_->readAt(0, 0, cancel);
        if (!check) { co_await target_->close(); co_await guard_->close(); co_return check.error(); }
        if (!result) { co_await target_->close(); co_await guard_->close(); }
        co_return result;
    }
    QCoro::Task<Result<void>> close() override {
        auto guard = co_await guard_->close(); auto target = co_await target_->close();
        co_return guard ? target : guard;
    }
    QCoro::Task<Result<void>> invalidate() override {
        auto guard = co_await guard_->invalidate(); auto target = co_await target_->invalidate();
        co_return guard ? target : guard;
    }
private:
    std::unique_ptr<IFileReader> guard_, target_;
};
std::optional<EntryTarget> fileDestination(Source& source, FileId id, const AccessContext& access) {
    auto item = entry(source, id); validateEntry(source, item, access, AccessOperation::Read);
    if (!item.alias) return {};
    if (!targetFor(access,{item.identity.dev,item.identity.ino,false}) && externalFileAllowed(source,item,access)) return {};
    authorizeAlias(source, item, access, AccessOperation::Read);
    auto target = targetFor(access, {item.identity.dev, item.identity.ino, false});
    require(target && access.remaining_aliases && (target.value().namespace_id != source.config.common.id || target.value().id != id), ErrorCode::MissingEntry);
    return target.value();
}
std::shared_ptr<ReadState> attachReader(ReadOnlyFileManager::Impl& impl, NamespaceId ns, FileId id, AccessContext access) {
    auto source = impl.source(ns); expireReaders(*source); cleanupLeases(*source); auto item = entry(*source, id);
    auto opened = validateEntry(*source, item, access, AccessOperation::Read);
    require(item.info.type == EntryType::File, ErrorCode::InvalidArgument);
    require(item.info.status == EntryStatus::Available, ErrorCode::Busy);
    require(item.info.size <= source->config.common.limits.max_file_size && source->handles < source->config.common.limits.max_handles, ErrorCode::QuotaExceeded);
    auto state = std::make_shared<ReadState>(); state->fd = contentFd(opened, false);
    state->source = source; state->namespace_id = ns; state->io_bytes = source->config.common.limits.max_io_bytes; state->verification = source->config.verification;
    if (state->verification == ReadVerification::Snapshot) state->selected_generation = item.info.generation;
    state->opened = opened.identity; state->id = id; state->access = std::move(access);
    state->exclusions = source->exclusion_generation;
    state->expires = Clock::now() + std::chrono::milliseconds(source->config.limits.reader_lifetime_ms);
    std::erase_if(source->readers, [](const auto& weak) { auto state = weak.lock(); return !state || state->closed; });
    ++source->handles; source->readers.push_back(state); return state;
}
FileInfo metadata(Source& source, QUuid id, const AccessContext& access) {
    FilesystemCheckScope scope(access.filesystem_check);
    authorize(access, source.config.common.id, id, AccessOperation::Inspect);
    auto item = entry(source, id);
    try { validateEntry(source, item, access, AccessOperation::Inspect); }
    catch (const Failure& f) {
        if (f.error.code != ErrorCode::Busy) throw;
        // Evidence changed, but privacy must still be established for the current
        // object before exposing even last-known metadata.
        auto opened = resolve(source, physicalPath(source, id));
        require(opened.identity.sameObject(item.identity) && opened.alias == item.alias, ErrorCode::MissingEntry);
        authorizeAlias(source, item, access, AccessOperation::Inspect);
        item.info.status = EntryStatus::Updating;
        if (!item.info.sha256.isEmpty()) item.info.hash_state = HashState::Stale;
    }
    if (item.info.type == EntryType::Directory) {
        auto found = source.directories.find(id);
        if (found == source.directories.end() && source.directories.size() >= source.config.limits.max_cached_directories) {
            auto victim = source.directories.end();
            for (auto it = source.directories.begin(); it != source.directories.end(); ++it)
                if (it->second.use_count() == 1 && (victim == source.directories.end() || it->second->touched < victim->second->touched)) victim = it;
            if (victim != source.directories.end()) source.directories.erase(victim);
        }
        if (source.directories.contains(id) || source.directories.size() < source.config.limits.max_cached_directories) {
            auto& cached = source.directories[id]; if (!cached) cached = std::make_shared<DirectoryCache>();
            cached->info = item.info; cached->raw = item.raw; cached->identity = item.identity; cached->alias = item.alias;
            cached->epoch = item.epoch; cached->catalog_generation = source.generation; cached->touched = Clock::now();
        }
    }
    return item.info;
}
} // namespace
QCoro::Task<Result<FileInfo>> ReadOnlyFileManager::stat(NamespaceId ns, QUuid id, AccessContext access) {
    auto check = impl_->deliveryCheck(ns, access);
    auto result = co_await statImpl(ns, id, std::move(access));
    if (result && !check()) co_return Error{ErrorCode::MissingEntry};
    co_return result;
}
QCoro::Task<Result<FileInfo>> ReadOnlyFileManager::lookup(NamespaceId ns, QString path, AccessContext access) {
    auto check = impl_->deliveryCheck(ns, access);
    auto result = co_await lookupImpl(ns, std::move(path), std::move(access));
    if (result && !check()) co_return Error{ErrorCode::MissingEntry};
    co_return result;
}
QCoro::Task<Result<DirectoryPage>> ReadOnlyFileManager::list(NamespaceId ns, DirectoryId parent, PageRequest page, AccessContext access) {
    auto check = impl_->deliveryCheck(ns, access);
    const auto denied = page.cursor.isEmpty() ? ErrorCode::MissingEntry : ErrorCode::StaleCursor;
    auto result = co_await listImpl(ns, parent, std::move(page), std::move(access));
    if (result && !check()) co_return Error{denied};
    co_return result;
}
QCoro::Task<Result<std::unique_ptr<IFileReader>>> ReadOnlyFileManager::openRead(NamespaceId ns, FileId id, AccessContext access) {
    auto check = impl_->deliveryCheck(ns, access);
    auto result = co_await openReadImpl(ns, id, std::move(access));
    if (result && !check()) co_return Error{ErrorCode::MissingEntry};
    co_return result;
}
QCoro::Task<Result<PreparedRead>> ReadOnlyFileManager::prepareRead(NamespaceId ns, FileId id, ReadPreparationOptions options, AccessContext access) {
    auto check = impl_->deliveryCheck(ns, access);
    auto result = co_await prepareReadImpl(ns, id, std::move(options), std::move(access));
    if (result && !check()) co_return Error{ErrorCode::MissingEntry};
    co_return result;
}
QCoro::Task<Result<FileInfo>> ReadOnlyFileManager::stat(NamespaceId ns, QUuid id) { return stat(ns, id, {}); }
QCoro::Task<Result<FileInfo>> ReadOnlyFileManager::statImpl(NamespaceId ns, QUuid id, AccessContext access) {
    return impl_->run<FileInfo>([ns, id, access = std::move(access)](Impl& impl) -> Result<FileInfo> {
        return metadata(*impl.source(ns), id, access);
    });
}
QCoro::Task<Result<DirectoryPage>> ReadOnlyFileManager::list(NamespaceId ns, DirectoryId parent, PageRequest page) {
    return list(ns, parent, std::move(page), {});
}
QCoro::Task<Result<void>> ReadOnlyFileManager::discardDirectoryCursor(NamespaceId ns, QByteArray cursor) {
    auto impl=impl_;
    co_return co_await impl->run<void>([ns,cursor=std::move(cursor)](Impl& state)->Result<void>{
        state.source(ns);
        for(auto& [id,source]:state.sources) source->cursors.remove(cursor);
        return {};
    });
}
QCoro::Task<Result<DirectoryPage>> ReadOnlyFileManager::listImpl(NamespaceId ns, DirectoryId parent, PageRequest page, AccessContext access) {
    if (!page.limit || page.limit > 1000 || page.cursor.size() > 64) co_return Error{ErrorCode::InvalidArgument};
    const auto page_bytes = quint64(page.limit) * 4096;
    auto impl = impl_;
    using DirectoryRoute = std::pair<std::optional<EntryTarget>, AccessContext>;
    auto route = co_await impl->run<DirectoryRoute>([ns, parent, access, page](Impl& state) -> Result<DirectoryRoute> {
        auto source = state.source(ns); auto item = entry(*source, parent);
        require(!access.check || !access.authorization_scope.isNull(), ErrorCode::InvalidArgument);
        if (!page.cursor.isEmpty()) require(access.current(), ErrorCode::StaleCursor);
        metadata(*source, parent, access); authorize(access, ns, parent, AccessOperation::List);
        if (!item.alias) return DirectoryRoute{{}, access};
        authorizeAlias(*source, item, access, AccessOperation::List);
        auto destination = targetFor(access, {item.identity.dev, item.identity.ino, true});
        require(destination && access.remaining_aliases, ErrorCode::MissingEntry);
        require(destination.value().namespace_id != ns || destination.value().id != parent, ErrorCode::MissingEntry);
        return DirectoryRoute{destination.value(), projectedAccess(*source, access)};
    });
    if (!route) co_return route.error();
    if (route.value().first) {
        const auto target = *route.value().first; access = std::move(route.value().second); --access.remaining_aliases;
        if (access.target_context) {
            Result<std::shared_ptr<AccessContext>> selected{Error{ErrorCode::MissingEntry}};
            try { selected = access.target_context(target); } catch (...) {}
            if (!selected || !selected.value()) co_return Error{ErrorCode::MissingEntry};
            auto next = *selected.value();
            if (next.authorization_scope != access.authorization_scope || next.generation != access.generation
                || next.current_generation != access.current_generation) co_return Error{ErrorCode::MissingEntry};
            next.filesystem_check = access.filesystem_check;
            next.filesystem_generation = access.filesystem_generation;
            next.remaining_aliases = access.remaining_aliases;
            access = std::move(next);
        }
        Result<DirectoryPage> result{Error{ErrorCode::MissingEntry}};
        if (access.backend) {
            auto backend = backendFor(access, target.namespace_id);
            if (!backend) co_return Error{ErrorCode::MissingEntry};
            result = co_await backend->list(target.namespace_id, target.id, std::move(page), std::move(access));
        } else result = co_await list(target.namespace_id, target.id, std::move(page), std::move(access));
        if (!result) co_return result.error();
        auto checked = co_await impl->run<void>([ns](Impl& state) -> Result<void> { validatePolicy(*state.source(ns)); return {}; });
        if (!checked) co_return checked.error();
        co_return result;
    }
    co_return co_await impl->run<DirectoryPage>([ns, parent, page = std::move(page), access = std::move(access)](Impl& impl) -> Result<DirectoryPage> {
        auto source = impl.source(ns);
        require(page.limit && page.limit <= source->config.common.limits.max_page_size && page.cursor.size() <= 64, ErrorCode::InvalidArgument);
        auto directory = entry(*source, parent);
        require(directory.info.type == EntryType::Directory, ErrorCode::InvalidArgument);
        authorize(access, ns, parent, AccessOperation::List);
        metadata(*source, parent, access);
        authorizeAlias(*source, directory, access, AccessOperation::List);
        QByteArray key;
        if (!page.cursor.isEmpty()) {
            const auto it = source->cursors.find(page.cursor); require(it != source->cursors.end(), ErrorCode::StaleCursor);
            const auto cursor = it.value(); source->cursors.erase(it);
            require(cursor.parent == parent && cursor.scope == access.authorization_scope && cursor.stable_order==page.stable_order && cursor.ascii_order==page.ascii_order && (page.stable_order || cursor.generation == source->generation) && cursor.policy == access.generation
                && cursor.exclusions == source->exclusion_generation && cursor.filesystem_generation == access.filesystem_generation && cursor.authority == access.current_generation
                && Clock::now() < cursor.expires && access.current(), ErrorCode::StaleCursor);
            key = cursor.key;
        }
        const auto actual_parent = listingParent(*source, directory);
        DirectoryPage result; QByteArray last = key; quint32 examined = 0;
        const auto order = page.stable_order && page.ascii_order ? QByteArray("dsfm_ascii(name) COLLATE BINARY") : QByteArray("name COLLATE BINARY");
        const auto sql = QByteArray("SELECT ") + entry_columns + " FROM entries WHERE parent=? AND " + order + ">? AND status IN(0,3) ORDER BY " + order + " LIMIT ?";
        // Bound work even when every candidate is hidden. Continuation tokens are
        // random opaque handles and never serialize hidden names/IDs/counts.
        Statement q(source->catalog.db, sql.constData(), {uuid(actual_parent), QString::fromUtf8(key), source->config.common.limits.max_batch});
        while (q.next()) {
            auto item = entryFrom(q); last = page.stable_order && page.ascii_order ? asciiLabel(item.info.name.toUtf8()) : item.info.name.toUtf8(); ++examined;
            try { auto visible = metadata(*source, item.info.id, access); visible.parent = parent; result.entries.append(std::move(visible)); }
            catch (const Failure& failure) {
                if (failure.error.code != ErrorCode::MissingEntry && failure.error.code != ErrorCode::PermissionDenied) throw;
            }
            if (result.entries.size() >= page.limit) break;
        }
        // Continuations may lead to an empty final page; no hidden cardinality is exposed.
        if (result.entries.size() == page.limit || examined == source->config.common.limits.max_batch) {
            for (auto it = source->cursors.begin(); it != source->cursors.end();) {
                if (Clock::now() >= it->expires) it = source->cursors.erase(it); else ++it;
            }
            require(source->cursors.size() < source->config.limits.max_cursors, ErrorCode::QuotaExceeded);
            result.next_cursor = QUuid::createUuid().toRfc4122().toHex();
            source->cursors.insert(result.next_cursor, {parent, access.authorization_scope, last, access.filesystem_generation, source->generation, access.generation, source->exclusion_generation,
                Clock::now() + std::chrono::minutes(1), access.current_generation, page.stable_order, page.ascii_order});
        }
        return result;
    }, page_bytes);
}
QCoro::Task<Result<FileInfo>> ReadOnlyFileManager::lookup(NamespaceId ns, QString path) { return lookup(ns, std::move(path), {}); }
QCoro::Task<Result<FileInfo>> ReadOnlyFileManager::lookupImpl(NamespaceId ns, QString path, AccessContext access) {
    using Route = std::variant<FileInfo, std::tuple<EntryTarget, QString, AccessContext>>;
    auto impl = impl_;
    auto route = co_await impl->run<Route>([ns, path = std::move(path), access](Impl& state) -> Result<Route> {
        auto source = state.source(ns);
        require(path.size() <= qsizetype(source->config.common.limits.max_depth) * (max_encoded_linux_name_bytes + 1)
            && !path.contains(QChar::Null), ErrorCode::InvalidArgument);
        if (path.isEmpty() || path == "/") return Route{metadata(*source, source->root_id, access)};
        auto components = path.split('/'); if (components.front().isEmpty()) components.removeFirst();
        require(components.size() <= source->config.common.limits.max_depth, ErrorCode::InvalidArgument);
        auto id = source->root_id;
        for (qsizetype i = 0; i < components.size(); ++i) {
            const auto& component = components[i];
            require(component.isValidUtf16() && !component.isEmpty() && component != "." && component != ".." && component.toUtf8().size() <= max_encoded_linux_name_bytes, ErrorCode::InvalidName);
            auto parent = entry(*source, id); metadata(*source, id, access);
            require(parent.info.type == EntryType::Directory, ErrorCode::MissingEntry);
            if (parent.alias) {
                authorizeAlias(*source, parent, access, AccessOperation::List);
                auto destination = targetFor(access, {parent.identity.dev, parent.identity.ino, true});
                require(destination && access.remaining_aliases, ErrorCode::MissingEntry);
                return Route{std::tuple{destination.value(), components.mid(i).join('/'), projectedAccess(*source, access)}};
            }
            const auto actual_parent = listingParent(*source, parent);
            Statement q(source->catalog.db, "SELECT id FROM entries WHERE parent=? AND name=? COLLATE BINARY AND status IN(0,3)", {uuid(actual_parent), component});
            require(q.next(), ErrorCode::MissingEntry); id = QUuid(q.text(0));
        }
        return Route{metadata(*source, id, access)};
    });
    if (!route) co_return route.error();
    if (auto* found = std::get_if<FileInfo>(&route.value())) co_return std::move(*found);
    auto [target, suffix, delegated_access] = std::get<std::tuple<EntryTarget, QString, AccessContext>>(std::move(route).value());
    access = std::move(delegated_access); --access.remaining_aliases;
    std::shared_ptr<IFileManager> owner; IFileManager* backend = this;
    if (access.backend) { owner = backendFor(access, target.namespace_id); if (!owner) co_return Error{ErrorCode::MissingEntry}; backend = owner.get(); }
    QStringList prefix; auto parent = target.id; bool root = false;
    for (quint32 depth = 0; depth <= 128; ++depth) {
        auto node = co_await backend->stat(target.namespace_id, parent, access); if (!node) co_return node.error();
        if (!node.value().parent) { root = true; break; }
        prefix.prepend(node.value().name); parent = *node.value().parent;
    }
    if (!root) co_return Error{ErrorCode::MissingEntry};
    prefix.append(suffix);
    auto result = co_await backend->lookup(target.namespace_id, prefix.join('/'), std::move(access));
    if (!result) co_return result.error();
    auto checked = co_await impl->run<void>([ns](Impl& state) -> Result<void> { validatePolicy(*state.source(ns)); return {}; });
    if (!checked) co_return checked.error();
    co_return result;
}
QCoro::Task<Result<std::unique_ptr<IFileReader>>> ReadOnlyFileManager::openRead(NamespaceId ns, FileId id) { return openRead(ns, id, {}); }
QCoro::Task<Result<std::unique_ptr<IFileReader>>> ReadOnlyFileManager::openReadImpl(NamespaceId ns, FileId id, AccessContext access) {
    auto impl = impl_;
    auto result = co_await impl->run<std::shared_ptr<ReadState>>([ns, id, access = std::move(access)](Impl& state) mutable -> Result<std::shared_ptr<ReadState>> {
        return attachReader(state, ns, id, std::move(access));
    });
    if (!result) co_return result.error();
    auto state = std::move(result).value(); std::unique_ptr<IFileReader> guard(new ReadOnlyReader(impl, state));
    auto destination = co_await impl->run<std::optional<EntryTarget>>([state](Impl&) -> Result<std::optional<EntryTarget>> { return fileDestination(*state->source, state->id, state->access); });
    if (!destination) co_return destination.error();
    if (!destination.value()) co_return guard;
    const auto target = *destination.value();
    auto projection = co_await impl->run<AccessContext>([state](Impl&) -> Result<AccessContext> { return projectedAccess(*state->source, state->access); });
    if (!projection) co_return projection.error();
    auto context = std::move(projection).value(); --context.remaining_aliases;
    std::shared_ptr<IFileManager> owner; IFileManager* backend = this;
    if (context.backend) { owner = backendFor(context, target.namespace_id); if (!owner) co_return Error{ErrorCode::MissingEntry}; backend = owner.get(); }
    auto opened = co_await backend->openRead(target.namespace_id, target.id, context); if (!opened) co_return opened.error();
    auto checked = co_await guard->readAt(0, 0); if (!checked) co_return checked.error();
    co_return std::unique_ptr<IFileReader>(new AliasReader(std::move(guard), std::move(opened).value()));
}
QCoro::Task<Result<PreparedRead>> ReadOnlyFileManager::prepareRead(NamespaceId ns, FileId id, ReadPreparationOptions options) {
    return prepareRead(ns, id, std::move(options), {});
}
QCoro::Task<Result<PreparedRead>> ReadOnlyFileManager::prepareReadImpl(NamespaceId ns, FileId id, ReadPreparationOptions options, AccessContext access, bool delegate_alias) {
    auto impl = impl_;
    auto destination = co_await impl->run<std::optional<EntryTarget>>([ns, id, access, delegate_alias](Impl& state) -> Result<std::optional<EntryTarget>> {
        if (!delegate_alias) return std::optional<EntryTarget>{};
        return fileDestination(*state.source(ns), id, access);
    });
    if (!destination) co_return destination.error();
    if (destination.value()) {
        auto guarded = co_await impl->run<std::shared_ptr<ReadState>>([ns, id, access, options](Impl& state) -> Result<std::shared_ptr<ReadState>> {
            auto source = state.source(ns); auto info = entry(*source, id).info;
            require(!options.resume_lease.isEmpty() || !options.expected_generation || *options.expected_generation == info.generation, ErrorCode::Conflict);
            return attachReader(state, ns, id, access);
        });
        if (!guarded) co_return guarded.error();
        auto guard_state = std::move(guarded).value(); std::unique_ptr<IFileReader> guard(new ReadOnlyReader(impl, guard_state));
        const auto target = *destination.value();
        auto projection = co_await impl->run<AccessContext>([guard_state](Impl&) -> Result<AccessContext> { return projectedAccess(*guard_state->source, guard_state->access); });
        if (!projection) co_return projection.error();
        access = std::move(projection).value(); --access.remaining_aliases;
        std::shared_ptr<IFileManager> owner; IFileManager* backend = this;
        if (access.backend) { owner = backendFor(access, target.namespace_id); if (!owner) co_return Error{ErrorCode::MissingEntry}; backend = owner.get(); }
        if (guard_state->verification != ReadVerification::Cached) {
            auto target_guard = co_await backend->openRead(target.namespace_id, target.id, access);
            if (!target_guard) co_return target_guard.error();
            auto closed = co_await guard->close(); if (!closed) co_return closed.error();
            auto prepared = co_await prepareReadImpl(ns, id, options, guard_state->access, false);
            if (!prepared) co_return prepared.error();
            auto checked = co_await target_guard.value()->readAt(0, 0, options.cancel); if (!checked) co_return checked.error();
            auto value = std::move(prepared).value();
            value.reader = std::make_unique<AliasReader>(std::move(target_guard).value(), std::move(value.reader));
            co_return value;
        }
        options.expected_generation.reset();
        auto prepared = co_await backend->prepareRead(target.namespace_id, target.id, options, access);
        if (!prepared) co_return prepared.error();
        auto checked = co_await guard->readAt(0, 0, options.cancel); if (!checked) co_return checked.error();
        auto value = std::move(prepared).value();
        value.reader = std::make_unique<AliasReader>(std::move(guard), std::move(value.reader));
        co_return value;
    }
    auto attached = co_await impl->run<std::shared_ptr<ReadState>>([ns, id, options, access = std::move(access)](Impl& state) mutable -> Result<std::shared_ptr<ReadState>> {
        require(options.expected_sha256.isEmpty() || options.expected_sha256.size() == 32, ErrorCode::InvalidArgument);
        cancelCheck(options.cancel);
        auto source = state.source(ns); const auto info = entry(*source, id).info;
        require(!options.resume_lease.isEmpty() || !options.expected_generation || *options.expected_generation == info.generation, ErrorCode::Conflict);
        require(options.resume_lease.isEmpty() || (source->config.verification == ReadVerification::Snapshot && options.expected_sha256.size() == 32), ErrorCode::InvalidArgument);
        auto reader = attachReader(state, ns, id, std::move(access));
        try {
            if (!options.resume_lease.isEmpty()) {
                const auto name = snapshotName(options.resume_lease);
                Statement lease(source->catalog.db, "SELECT size,sha,expires,exclusions,stage_dev,stage_ino,mtime,source_generation,dev,ino,birth FROM leases WHERE token=? AND id=?", {options.resume_lease, uuid(id)});
                require(lease.next() && lease.number(2) > QDateTime::currentMSecsSinceEpoch()
                    && quint64(lease.number(3)) == source->exclusion_generation && lease.bytes(1) == options.expected_sha256
                    && lease.number(0) >= 0 && lease.number(7) > 0 && quint64(lease.number(0)) <= source->config.common.limits.max_file_size, ErrorCode::MissingEntry);
                require(!options.expected_generation || *options.expected_generation == quint64(lease.number(7)), ErrorCode::Conflict);
                require(reader->opened.dev == lease.text(8).toULongLong() && reader->opened.ino == lease.text(9).toULongLong()
                    && (!reader->opened.birth || !lease.number(10) || reader->opened.birth == lease.number(10)), ErrorCode::MissingEntry);
                reader->opened.size = quint64(lease.number(0)); reader->opened.mtime = lease.number(6);
                reader->selected_generation = quint64(lease.number(7));
                reader->lease = options.resume_lease; reader->expected = lease.bytes(1); reader->snapshot = true;
                auto path = openBeneath(source->internal.value, QString::fromLatin1(name), O_PATH);
                identity(path.value, false, "snapshot lease");
                reader->snapshot_identity = evidence(path.value);
                require(reader->snapshot_identity.size == reader->opened.size && reader->snapshot_identity.dev == lease.text(4).toULongLong()
                    && reader->snapshot_identity.ino == lease.text(5).toULongLong(), ErrorCode::IntegrityError);
                Opened staged{std::move(path), reader->snapshot_identity, false}; reader->fd = contentFd(staged, false);
                // Rehash persisted staged bytes before trusting a resumed lease.
                reader->hash = std::make_unique<QCryptographicHash>(QCryptographicHash::Sha256);
                return reader;
            }
            if (source->config.verification == ReadVerification::Snapshot) {
                Statement usage(source->catalog.db, "SELECT count(*),COALESCE(sum(size),0) FROM leases"); usage.next();
                require(quint64(usage.number(0)) < source->config.common.limits.max_pending
                    && reader->opened.size <= source->config.common.limits.max_staging_bytes
                    && quint64(usage.number(1)) <= source->config.common.limits.max_staging_bytes - reader->opened.size
                    && source->recovery_bytes <= source->config.common.limits.max_total_bytes - reader->opened.size
                    && quint64(usage.number(1)) <= source->config.common.limits.max_total_bytes - reader->opened.size - source->recovery_bytes, ErrorCode::QuotaExceeded);
                reader->lease = QUuid::createUuid().toRfc4122().toHex(); const auto name = snapshotName(reader->lease);
                // Reserve durably before creating the object, so crash cleanup can
                // identify every owned file without enumerating/deleting strangers.
                source->catalog.exec("INSERT INTO leases(token,id,size,sha,expires,exclusions,dev,ino,mtime,ctime,birth,source_generation) VALUES(?,?,?,?,?,?,?,?,?,?,?,?)", {reader->lease, uuid(id), qlonglong(reader->opened.size), QByteArray{},
                    QDateTime::currentMSecsSinceEpoch() + source->config.limits.reader_lifetime_ms, qlonglong(source->exclusion_generation),
                    QString::number(reader->opened.dev), QString::number(reader->opened.ino), reader->opened.mtime, reader->opened.ctime, reader->opened.birth, qlonglong(reader->selected_generation)});
                reader->staging = openBeneath(source->internal.value, QString::fromLatin1(name), O_RDWR | O_CREAT | O_EXCL, 0600);
                auto stage_identity = evidence(reader->staging.value); reader->snapshot_identity = stage_identity;
                source->catalog.exec("UPDATE leases SET stage_dev=?,stage_ino=? WHERE token=?", {QString::number(stage_identity.dev), QString::number(stage_identity.ino), reader->lease});
            }
            reader->expected = info.sha256;
        if (source->config.verification != ReadVerification::Cached || info.hash_state != HashState::Verified) reader->expected.clear();
            if (reader->expected.isEmpty()) {
                Statement active(source->catalog.db, "SELECT 1 FROM hash_jobs WHERE id=?", {uuid(id)});
                require(!active.next(), ErrorCode::Busy);
                source->catalog.exec("INSERT INTO hash_jobs VALUES(?)", {uuid(id)}); reader->hashing = true;
                advance(*source); source->catalog.exec("UPDATE entries SET generation=? WHERE id=?", {qlonglong(source->generation), uuid(id)}); notify(*source);
                reader->hash = std::make_unique<QCryptographicHash>(QCryptographicHash::Sha256);
            }
            return reader;
        } catch (...) { releaseReader(*reader); throw; }
    });
    if (!attached) co_return attached.error();
    auto state = std::move(attached).value();
    std::unique_ptr<IFileReader> reader(new ReadOnlyReader(impl, state));
    const auto hash_chunk = quint32(std::min<quint64>(state->io_bytes, impl->buffer_limit.load()));
    while (state->hash) {
        auto step = co_await impl->run<bool>([state, options, hash_chunk](Impl&) -> Result<bool> {
            auto bytes = readBytes(*state, state->progress, hash_chunk, options.cancel);
            if (state->staging) stageChunk(*state, bytes);
            state->hash->addData(bytes); state->progress += quint64(bytes.size());
            if (state->progress != state->opened.size) return false;
            const auto digest = state->hash->result();
            if (state->snapshot) require(digest == state->expected, ErrorCode::IntegrityError);
            state->expected = digest; state->hash.reset();
            if (state->staging) {
                ioCheck(::fsync(state->staging.value) == 0); ioCheck(::fchmod(state->staging.value, 0400) == 0);
                identity(state->staging.value, false, "snapshot staging");
                Opened staged{duplicate(state->staging.value), evidence(state->staging.value), false}; state->fd = contentFd(staged, false);
                state->staging = {}; state->snapshot_identity = evidence(state->fd.value); state->snapshot = true;
                state->source->catalog.exec("UPDATE leases SET sha=? WHERE token=?", {digest, state->lease});
                ioCheck(::fsync(state->source->internal.value) == 0);
            }
            return true;
        }, hash_chunk);
        if (!step) co_return step.error();
        if (step.value()) break;
    }
    auto prepared = co_await impl->run<FileInfo>([state, options](Impl&) -> Result<FileInfo> {
        cancelCheck(options.cancel); validateReader(*state);
        require(options.expected_sha256.isEmpty() || options.expected_sha256 == state->expected, ErrorCode::IntegrityError);
        if (state->hashing) {
            state->source->catalog.exec("DELETE FROM hash_jobs WHERE id=?", {uuid(state->id)}); state->hashing = false;
            advance(*state->source);
            state->source->catalog.exec("UPDATE entries SET generation=? WHERE id=?", {qlonglong(state->source->generation), uuid(state->id)});
            if (!state->snapshot) state->source->catalog.exec("UPDATE entries SET sha=? WHERE id=?", {state->expected, uuid(state->id)});
            notify(*state->source);
        }
        auto info = entry(*state->source, state->id).info;
        if (state->snapshot) {
            info.status = EntryStatus::Available; // Describes the completed staged version.
            if (!state->selected_generation) {
                state->selected_generation = info.generation;
                state->source->catalog.exec("UPDATE leases SET source_generation=? WHERE token=?", {qlonglong(info.generation), state->lease});
            }
            info.generation = state->selected_generation;
            info.modified = QDateTime::fromMSecsSinceEpoch(state->opened.mtime / 1000000, QTimeZone::UTC);
        }
        info.size = state->opened.size; info.sha256 = state->expected; info.hash_state = HashState::Verified; return info;
    });
    if (!prepared) co_return prepared.error();
    co_return PreparedRead{std::move(prepared).value(), std::move(reader), state->snapshot ? ReadConsistency::Snapshot : ReadConsistency::DetectChanges, state->lease};
}
QCoro::Task<Result<DirectorySubscription>> ReadOnlyFileManager::subscribeDirectory(NamespaceId ns, DirectoryId directory, DirectoryChangeCallback callback) {
    return subscribeDirectory(ns, directory, std::move(callback), {});
}
QCoro::Task<Result<DirectorySubscription>> ReadOnlyFileManager::subscribeDirectory(NamespaceId ns, DirectoryId directory, DirectoryChangeCallback callback, AccessContext access) {
    std::shared_ptr<Observer> sub;
    try { sub = observer(ns, directory, std::move(callback), std::move(access)); }
    catch (const Failure& failure) { co_return failure.error; }
    co_return co_await impl_->run<DirectorySubscription>([ns, directory, sub](Impl& impl) -> Result<DirectorySubscription> {
        auto source = impl.source(ns);
        { std::lock_guard lock(source->observer_mutex); std::erase_if(source->observers, [](const auto& old) { return !old->active->load(); }); }
        const auto item = metadata(*source, directory, sub->access);
        require(item.type == EntryType::Directory, ErrorCode::InvalidArgument);
        authorize(sub->access, ns, directory, AccessOperation::List);
        auto cached = source->directories.find(directory);
        require(cached != source->directories.end(), ErrorCode::QuotaExceeded); sub->pin = cached->second;
        impl.registerObserver(source, sub);
        return DirectorySubscription(ns, directory, source->generation, sub->active, {});
    });
}
QCoro::Task<Result<ReconciliationPage>> ReadOnlyFileManager::reconcile(NamespaceId ns, ReconciliationRequest request, CancellationToken cancel) {
    auto impl = impl_;
    auto result = co_await impl->run<ReconciliationPage>([ns, request, cancel](Impl& state) -> Result<ReconciliationPage> {
        auto source = state.source(ns); cancelCheck(cancel);
        require(request.action == ReconciliationAction::Scan || request.action == ReconciliationAction::RetryValidation
            || request.action == ReconciliationAction::DiscardMissing, ErrorCode::InvalidArgument);
        if (request.action == ReconciliationAction::DiscardMissing) {
            source->catalog.exec("DELETE FROM entries WHERE id IN(SELECT e.id FROM entries e WHERE e.status=1 AND e.id!=? AND NOT EXISTS(SELECT 1 FROM entries c WHERE c.parent=e.id) LIMIT ?)", {uuid(source->root_id), source->config.common.limits.max_batch});
            Statement count(source->catalog.db, "SELECT count(*) FROM entries"); count.next(); source->entry_count = quint64(count.number(0));
        }
        beginScan(*source); notify(*source); return ReconciliationPage{};
    });
    if (result) impl->tick();
    co_return result;
}
QCoro::Task<Result<void>> ReadOnlyFileManager::shutdown() {
    auto impl = impl_;
    co_return co_await impl->run<void>([](Impl& state) -> Result<void> {
        for (auto& [id, source] : state.sources) {
            source->stopped = true; source->access_epoch->store(0); expireReaders(*source); std::lock_guard lock(source->observer_mutex);
            for (const auto& sub : source->observers) sub->active->store(false);
        }
        state.sources.clear(); state.stopped = true; return {};
    });
}
Result<quint64> ReadOnlyFileManager::subscribe(NamespaceId ns, QObject* context, std::function<void(Change)> callback) {
    try {
        require(bool(callback), ErrorCode::InvalidArgument);
        auto sub = observer(ns, {}, {context, [callback = std::move(callback)](DirectoryChange change) {
            callback({change.namespace_id, change.generation, true});
        }}, {});
        std::shared_ptr<Source> source;
        {
            std::lock_guard lock(impl_->registration_mutex);
            auto found = impl_->known_sources.find(ns); require(found != impl_->known_sources.end(), ErrorCode::MissingEntry);
            source = found->second.lock(); require(bool(source), ErrorCode::MissingEntry);
        }
        return impl_->registerObserver(source, sub);
    } catch (const Failure& failure) { return failure.error; }
}
void ReadOnlyFileManager::unsubscribe(quint64 token) {
    std::lock_guard lock(impl_->registration_mutex);
    auto found = impl_->registrations.find(token);
    if (found != impl_->registrations.end()) {
        if (auto sub = found->second.lock()) sub->active->store(false);
        impl_->registrations.erase(found);
    }
}
#define DSFM_RO_UNSUPPORTED(type, name, args) QCoro::Task<Result<type>> ReadOnlyFileManager::name args { co_return Error{ErrorCode::UnsupportedCapability}; }
DSFM_RO_UNSUPPORTED(DirectoryId, mkdir, (NamespaceId, DirectoryId, QString))
DSFM_RO_UNSUPPORTED(PendingFile, createFile, (NamespaceId, CreateOptions))
DSFM_RO_UNSUPPORTED(PendingFile, replaceFile, (NamespaceId, FileId, ContentConstraints))
DSFM_RO_UNSUPPORTED(PendingFile, openPending, (NamespaceId, PendingId))
DSFM_RO_UNSUPPORTED(void, abortPending, (NamespaceId, PendingId, CancellationToken))
DSFM_RO_UNSUPPORTED(PendingPage, listPending, (NamespaceId, PageRequest))
DSFM_RO_UNSUPPORTED(RangePage, listRanges, (NamespaceId, PendingId, RangeKind, PageRequest))
DSFM_RO_UNSUPPORTED(MutationResult, rename, (NamespaceId, QUuid, QString, CancellationToken))
DSFM_RO_UNSUPPORTED(MutationResult, move, (NamespaceId, QUuid, DirectoryId, CancellationToken))
DSFM_RO_UNSUPPORTED(MutationResult, remove, (NamespaceId, QUuid, CancellationToken))
DSFM_RO_UNSUPPORTED(OperationInfo, operationStatus, (NamespaceId, OperationId))
#undef DSFM_RO_UNSUPPORTED
} // namespace dsfm

namespace dsfm {
QCoro::Task<Result<AccessContext>> ReadOnlyFileManager::descendantAccess(NamespaceId ns, DirectoryId directory, AccessContext access) {
    auto check = impl_->deliveryCheck(ns, access);
    auto result = co_await impl_->run<AccessContext>([ns, directory, access](Impl& state) -> Result<AccessContext> {
        auto source = state.source(ns);
        auto info = metadata(*source, directory, access);
        require(info.type == EntryType::Directory, ErrorCode::InvalidArgument);
        return projectedAccess(*source, access);
    });
    if (result && !check()) co_return Error{ErrorCode::MissingEntry};
    co_return result;
}
}
