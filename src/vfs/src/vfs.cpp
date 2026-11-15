#include <dsvfs/vfs.h>
#include "work.h"
#include <sodium.h>
#include <logfault/logfault.h>
#include <vector>
#include <chrono>
#include <sqlite3.h>
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonParseError>
#include <QTimer>
#include <QTimeZone>
#include <QElapsedTimer>
#include <QScopeGuard>
#include <algorithm>
#include <array>
#include <mutex>
#include <set>
#include <sys/file.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>

namespace dsvfs {
using namespace dsfm;
namespace {
Error err(ErrorCode code) { return Error{code}; }
qint64 now() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch())
        .count();
}
QString uuid(QUuid id) { return id.toString(QUuid::WithoutBraces); }
bool validName(const QString &name, bool presented = false) {
    const auto maximum = max_encoded_linux_name_bytes * (presented ? 3 : 1);
    if (name.isEmpty() || name.size() > maximum || !name.isValidUtf16() || name == "." || name == "..")
        return false;
    for (auto c : name)
        if (c == '/' || c.isNull() || c.category() == QChar::Other_Control || c.category() == QChar::Other_Format)
            return false;
    return name.toUtf8().size() <= maximum;
}
QString presentName(QString name, const Request &request) {
    if (request.allow_utf8)
        return name;
    QString encoded;
    constexpr char hex[] = "0123456789ABCDEF";
    for (auto ch : name.toUtf8()) {
        const auto byte = quint8(ch);
        if (byte >= 32 && byte < 127 && byte != '%')
            encoded.append(QChar(byte));
        else {
            encoded.append('%');
            encoded.append(QLatin1Char(hex[byte >> 4]));
            encoded.append(QLatin1Char(hex[byte & 15]));
        }
    }
    return encoded;
}
QByteArray json(QJsonObject object) { return QJsonDocument(object).toJson(QJsonDocument::Compact); }
QJsonObject record(const NodeInfo &node) {
    return {{"id", QString::fromLatin1(node.id.toHex())},
            {"parent", QString::fromLatin1(node.parent.toHex())},
            {"name", node.name},
            {"type", int(node.type)},
            {"status", int(node.status)},
            {"size", QString::number(node.size)},
            {"modified", node.modified.isValid() ? node.modified.toString(Qt::ISODateWithMs) : QString()},
            {"sha256", QString::fromLatin1(node.sha256.toHex())},
            {"hashState", int(node.hash_state)},
            {"revision", QString::number(node.revision)},
            {"capabilities", int(node.capabilities.toInt())},
            {"symlinks", int(node.symlinks)}};
}
NodeInfo fromRecord(QJsonObject object) {
    NodeInfo node;
    node.id = QByteArray::fromHex(object["id"].toString().toLatin1());
    node.parent = QByteArray::fromHex(object["parent"].toString().toLatin1());
    node.name = object["name"].toString();
    node.type = EntryType(object["type"].toInt());
    node.status = EntryStatus(object["status"].toInt());
    node.size = object["size"].toString().toULongLong();
    node.modified = QDateTime::fromString(object["modified"].toString(), Qt::ISODateWithMs);
    node.sha256 = QByteArray::fromHex(object["sha256"].toString().toLatin1());
    node.hash_state = HashState(object["hashState"].toInt());
    node.revision = object["revision"].toString().toULongLong();
    node.capabilities = Capabilities::fromInt(object["capabilities"].toInt());
    node.symlinks = SymlinkPolicy(object["symlinks"].toInt());
    return node;
}
Capabilities restrictCapabilities(Capabilities target, Capabilities exposed) {
    if (!exposed.testFlag(Capability::Read))
        target.setFlag(Capability::Read, false);
    if (!exposed.testFlag(Capability::Write))
        target.setFlag(Capability::Write, false);
    if (!exposed.testFlag(Capability::PreparedRead)) {
        target.setFlag(Capability::PreparedRead, false);
        target.setFlag(Capability::SnapshotReads, false);
        target.setFlag(Capability::ResumeLeases, false);
    }
    if (!exposed.testFlag(Capability::DirectorySubscriptions))
        target.setFlag(Capability::DirectorySubscriptions, false);
    return target;
}
quint64 revision(NodeInfo node) {
    node.revision = 0;
    const auto bytes = QCryptographicHash::hash(json(record(node)), QCryptographicHash::Sha256);
    quint64 value = 0;
    for (int i = 0; i < 8; ++i)
        value = (value << 8) | quint8(bytes[i]);
    return value;
}
struct File {
    int fd = -1;
    ~File() {
        if (fd >= 0)
            ::close(fd);
    }
};
struct PrivateState {
    std::shared_ptr<File> root, lock;
    std::mutex mutex;
    sqlite3 *scratch = nullptr;
    bool has_registry = false;
    ~PrivateState() {
        if (scratch)
            sqlite3_close(scratch);
    }
};
Result<QByteArray> load(std::shared_ptr<PrivateState> state, QString path, quint64 max_bytes) {
    if (::mkdir(QFile::encodeName(path).constData(), 0700) < 0 && errno != EEXIST)
        return err(ErrorCode::StorageUnavailable);
    state->root = std::make_shared<File>();
    state->root->fd = ::open(QFile::encodeName(path).constData(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    struct stat st{};
    if (state->root->fd < 0 || ::fstat(state->root->fd, &st) || st.st_uid != ::geteuid() || (st.st_mode & 0077))
        return err(ErrorCode::PermissionDenied);
    state->lock = std::make_shared<File>();
    state->lock->fd = ::openat(state->root->fd, "owner.lock", O_RDWR | O_CREAT | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (state->lock->fd < 0 || ::flock(state->lock->fd, LOCK_EX | LOCK_NB))
        return err(ErrorCode::NamespaceBusy);
    File source;
    source.fd = ::openat(state->root->fd, "mounts.json", O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
    QByteArray bytes;
    state->has_registry = source.fd >= 0;
    if (source.fd >= 0) {
        if (::fstat(source.fd, &st) || !S_ISREG(st.st_mode) || st.st_uid != ::geteuid() || (st.st_mode & 0077) ||
            st.st_size < 0 || quint64(st.st_size) > std::min<quint64>(max_bytes, 2ULL << 20))
            return err(ErrorCode::Corruption);
        bytes.resize(st.st_size);
        qsizetype offset = 0;
        while (offset < bytes.size()) {
            const auto count = ::read(source.fd, bytes.data() + offset, bytes.size() - offset);
            if (count <= 0)
                return err(ErrorCode::IoError);
            offset += count;
        }
    } else if (errno != ENOENT)
        return err(ErrorCode::IoError);
    // Scratch contains only private derived views and is disposable on restart.
    if (::unlinkat(state->root->fd, "views.sqlite", 0) && errno != ENOENT)
        return err(ErrorCode::IoError);
    const auto db_path = QByteArray("/proc/self/fd/") + QByteArray::number(state->root->fd) + "/views.sqlite";
    if (sqlite3_open_v2(db_path.constData(), &state->scratch,
                        SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX, nullptr) != SQLITE_OK)
        return err(ErrorCode::IoError);
    ::fchmodat(state->root->fd, "views.sqlite", 0600, 0);
    sqlite3_limit(state->scratch, SQLITE_LIMIT_LENGTH, 1 << 20);
    sqlite3_db_config(state->scratch, SQLITE_DBCONFIG_DEFENSIVE, 1, nullptr);
    const auto sql = QByteArray("PRAGMA trusted_schema=OFF; PRAGMA journal_mode=MEMORY; PRAGMA temp_store=FILE; PRAGMA "
                                "cache_size=-1024; PRAGMA max_page_count=") +
                     QByteArray::number(std::max<quint64>(256, max_bytes / 4096)) +
                     "; CREATE TABLE entry(view BLOB NOT NULL,name BLOB NOT NULL,id BLOB NOT NULL,data BLOB NOT "
                     "NULL,PRIMARY KEY(view,name,id)) WITHOUT ROWID; CREATE TABLE ordered(view BLOB NOT NULL,ordinal "
                     "INTEGER NOT NULL,data BLOB NOT NULL,PRIMARY KEY(view,ordinal)) WITHOUT ROWID;";
    if (sqlite3_exec(state->scratch, sql.constData(), nullptr, nullptr, nullptr) != SQLITE_OK)
        return err(ErrorCode::IoError);
    return bytes;
}
Result<void> save(std::shared_ptr<PrivateState> state, QByteArray bytes) {
    const auto name = QByteArray("mounts-") + QUuid::createUuid().toRfc4122().toHex();
    File target;
    target.fd = ::openat(state->root->fd, name.constData(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (target.fd < 0)
        return err(ErrorCode::IoError);
    auto cleanup = qScopeGuard([&] { ::unlinkat(state->root->fd, name.constData(), 0); });
    qsizetype offset = 0;
    while (offset < bytes.size()) {
        auto count = ::write(target.fd, bytes.data() + offset, bytes.size() - offset);
        if (count <= 0)
            return err(ErrorCode::IoError);
        offset += count;
    }
    if (::fsync(target.fd) || ::renameat(state->root->fd, name.constData(), state->root->fd, "mounts.json") ||
        ::fsync(state->root->fd))
        return err(ErrorCode::IoError);
    return {};
}
struct Statement {
    sqlite3_stmt *stmt = nullptr;
    ~Statement() { sqlite3_finalize(stmt); }
    bool prepare(sqlite3 *db, const char *sql) { return sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) == SQLITE_OK; }
    void blob(int column, QByteArray value) {
        sqlite3_bind_blob(stmt, column, value.constData(), value.size(), SQLITE_TRANSIENT);
    }
};
struct ReadHandle {
    std::unique_ptr<IFileReader> reader;
    std::function<QCoro::Task<Result<void>>()> check;
    std::function<bool()> current;
    bool busy = false;
};
struct Life {
    std::atomic_bool active{true};
    std::atomic_bool conflicted{false};
    std::atomic_uint commits{0};
};
struct Mounted {
    QUuid identity;
    Mount mount;
    std::shared_ptr<Life> life = std::make_shared<Life>();
};
struct Selections {
    std::mutex mutex;
    QHash<QByteArray, Target> entries;
    QVector<Target> targets;
};
bool equalBytes(QByteArrayView a, QByteArrayView b) {
    return a.size() == b.size() && sodium_memcmp(a.data(), b.data(), a.size()) == 0;
}
QByteArray routeMac(QByteArray key, QByteArray input) {
    input.prepend("dsvfs-alias-route-v1");
    QByteArray mac(16, Qt::Uninitialized);
    crypto_generichash(reinterpret_cast<unsigned char *>(mac.data()), mac.size(),
                       reinterpret_cast<const unsigned char *>(input.constData()), input.size(),
                       reinterpret_cast<const unsigned char *>(key.constData()), key.size());
    return mac;
}
struct Route {
    NodeId origin;
    NodeId anchor;
    NodeId destination;
};
struct Backend {
    std::shared_ptr<IFileManager> manager;
    SymlinkPolicy symlinks;
    QUuid owner_identity;
};
struct Configuration {
    QHash<QUuid, QUuid> roots;
    QVector<Mounted> mounts;
    QHash<NamespaceId, Backend> backends;
    QVector<StorageResource> resources;
    QVector<StorageAssignment> assignments;
    QVector<MountDefinition> definitions;
    QHash<QString, QUuid> navigation_ids;
};
struct View {
    QByteArray id;
    QUuid identity;
    NodeId directory;
    Request request;
    QByteArray digest;
    qint64 expires = 0;
    bool ready = false;
};
} // namespace

struct EffectiveVfs {
    QUuid identity, contact;
    quint64 generation;
    std::shared_ptr<const std::atomic<quint64>> current_generation;
    QVector<Mounted> mounts;
    QHash<NodeId, NodeInfo> navigation;
    QVector<EffectiveMount> descriptors;
    QHash<QUuid, QString> paths;
    QHash<NodeId, QString> navigation_paths;
    QSet<QString> conflicts;
    QSet<QUuid> groups;
    std::shared_ptr<const PermissionPolicy> permissions;
};
struct Enumeration {
    QByteArray id, source_cursor, last_key;
    QUuid identity;
    NodeId directory;
    Request request;
    Target target;
    AccessContext access;
    std::shared_ptr<Selections> selected;
    QVector<NodeInfo> buffer, children;
    QSet<QString> shadowed;
    quint64 serial = 0;
    qint64 expires = 0;
    bool source_done = false, busy = false;
};
bool Request::current() const noexcept {
    return authorize && !scope.isNull() && current_generation && current_generation->load() == generation &&
           (!effective ||
            (effective->contact == scope && effective->groups == groups && effective->permissions == permissions &&
             effective->current_generation->load() == effective->generation));
}
struct Subscription::State {
    std::atomic_bool active{true};
    dsfm::DirectorySubscription source;
    std::vector<dsfm::DirectorySubscription> targets;
    QUuid identity;
    NodeId directory;
    Request request;
    ChangeCallback callback;
    QByteArray digest;
    std::atomic_bool dirty{false};
};
Subscription::Subscription(std::shared_ptr<State> state) : state_(std::move(state)) {}
Subscription::~Subscription() { reset(); }
Subscription::Subscription(Subscription &&other) noexcept = default;
Subscription &Subscription::operator=(Subscription &&other) noexcept {
    if (this != &other) {
        reset();
        state_ = std::move(other.state_);
    }
    return *this;
}
void Subscription::reset() noexcept {
    if (state_)
        state_->active = false;
    state_.reset();
}

struct Vfs::Impl : std::enable_shared_from_this<Impl> {
    QPointer<Vfs> owner;
    QString path;
    Limits limits;
    std::shared_ptr<PrivateState> disk = std::make_shared<PrivateState>();
    std::atomic<std::shared_ptr<const Configuration>> config{std::make_shared<Configuration>()};
    QByteArray key;
    std::shared_ptr<std::atomic<quint64>> topology_generation = std::make_shared<std::atomic<quint64>>(1);
    bool opened = false, changing = false, refreshing = false;
    std::atomic_bool stopping{false};
    quint32 active_requests = 0;
    QHash<QUuid, CancellationToken> active_cancellations;
    QHash<QByteArray, View> views;
    QHash<QByteArray, std::shared_ptr<Enumeration>> enumerations;
    QHash<QByteArray, QPair<QUuid, QString>> conflicts;
    struct Collision {
        NamespaceId ns;
        QUuid directory;
        QString name;
        quint64 generation;
        bool conflict;
    };
    QHash<QUuid, Collision> collision_cache;
    QVector<std::weak_ptr<Subscription::State>> subscriptions;
    QVector<std::weak_ptr<ReadHandle>> readers;
    QVector<std::shared_ptr<CatalogExport::State>> exports;
    std::shared_ptr<IFileManager> export_backend;
    NamespaceId export_ns;
    DirectoryId export_root;
    quint32 active_exports = 0;
    quint32 refresh_mount_cursor = 0, refresh_subscription_cursor = 0;
    explicit Impl(Vfs *object, QString root, Limits bounded) : owner(object), path(std::move(root)), limits(bounded) {}
    Result<void> available() const {
        if ((!owner || QThread::currentThread() != owner->thread()))
            return err(ErrorCode::InvalidArgument);
        if (stopping)
            return err(ErrorCode::ShuttingDown);
        if (!opened)
            return err(ErrorCode::StorageUnavailable);
        return {};
    }
    QVector<Mounted> viewMounts(const Request &request) const {
        return request.effective ? request.effective->mounts : config.load()->mounts;
    }
    std::optional<Mounted> mounted(QUuid identity, QUuid mount_id, const Request &request = {}) const {
        if (request.effective && request.effective->identity != identity)
            return {};
        for (auto item : viewMounts(request))
            if (item.identity == identity && item.mount.id == mount_id)
                return item;
        return {};
    }
    NodeId encode(Target target) const {
        const auto plain = target.identity.toRfc4122() + target.mount.toRfc4122() + target.namespace_id.toRfc4122() +
                           target.source.toRfc4122();
        QByteArray nonce(crypto_secretbox_NONCEBYTES, Qt::Uninitialized);
        const auto input = QByteArray("dsvfs-node-nonce-v1") + plain;
        std::array<unsigned char, 32> nonce_key{};
        crypto_kdf_derive_from_key(nonce_key.data(), nonce_key.size(), 1, "DSVFSv1_",
                                   reinterpret_cast<const unsigned char *>(key.constData()));
        crypto_generichash(reinterpret_cast<unsigned char *>(nonce.data()), nonce.size(),
                           reinterpret_cast<const unsigned char *>(input.constData()), input.size(), nonce_key.data(),
                           nonce_key.size());
        QByteArray cipher(plain.size() + crypto_secretbox_MACBYTES, Qt::Uninitialized);
        crypto_secretbox_easy(reinterpret_cast<unsigned char *>(cipher.data()),
                              reinterpret_cast<const unsigned char *>(plain.constData()), plain.size(),
                              reinterpret_cast<const unsigned char *>(nonce.constData()),
                              reinterpret_cast<const unsigned char *>(key.constData()));
        return QByteArray(1, char(1)) + nonce + cipher;
    }
    NodeId route(NodeId origin, NodeId anchor, NodeId destination) const {
        if (origin.size() > 2048 || anchor.size() != 105 || destination.size() != 105)
            return {};
        auto token =
            QByteArray(1, char(2)) + char(origin.size() >> 8) + char(origin.size()) + origin + anchor + destination;
        if (token.size() + 16 > 2048)
            return {};
        return token + routeMac(key, token);
    }
    Result<Route> parseRoute(NodeId node) const {
        if (node.size() < 334 || node.size() > 2048 || node[0] != 2 ||
            !equalBytes(routeMac(key, node.first(node.size() - 16)), node.last(16)))
            return err(ErrorCode::MissingEntry);
        auto length = (quint32(quint8(node[1])) << 8) | quint8(node[2]);
        if (node.size() != qsizetype(length) + 229)
            return err(ErrorCode::MissingEntry);
        return Route{node.sliced(3, length), node.sliced(3 + length, 105), node.sliced(108 + length, 105)};
    }
    Result<Target> decode(QUuid identity, NodeId node) const {
        if (!node.isEmpty() && node[0] == 2) {
            auto routing = parseRoute(node);
            if (!routing)
                return routing.error();
            auto origin = decode(identity, routing.value().origin);
            if (!origin)
                return origin.error();
            auto anchor = decode(identity, routing.value().anchor);
            if (!anchor)
                return anchor.error();
            auto destination = decode(identity, routing.value().destination);
            if (!destination)
                return destination.error();
            if (destination.value().mount != anchor.value().mount ||
                destination.value().namespace_id != anchor.value().namespace_id)
                return err(ErrorCode::MissingEntry);
            return destination;
        }
        if (node.size() != 105 || node[0] != 1)
            return err(ErrorCode::MissingEntry);
        QByteArray plain(64, Qt::Uninitialized);
        if (crypto_secretbox_open_easy(reinterpret_cast<unsigned char *>(plain.data()),
                                       reinterpret_cast<const unsigned char *>(node.constData() + 25), 80,
                                       reinterpret_cast<const unsigned char *>(node.constData() + 1),
                                       reinterpret_cast<const unsigned char *>(key.constData())) != 0)
            return err(ErrorCode::MissingEntry);
        Target target{QUuid::fromRfc4122(plain.first(16)), QUuid::fromRfc4122(plain.sliced(16, 16)),
                      QUuid::fromRfc4122(plain.sliced(32, 16)), QUuid::fromRfc4122(plain.sliced(48, 16))};
        if (target.identity != identity || encode(target) != node)
            return err(ErrorCode::MissingEntry);
        auto mount = mounted(identity, target.mount);
        if (!mount || mount->mount.namespace_id != target.namespace_id || !mount->life->active)
            return err(ErrorCode::MissingEntry);
        return target;
    }
    Result<QSet<QUuid>> dependencies(QUuid identity, NodeId node) const {
        if (node == root(identity))
            return QSet<QUuid>{};
        if (!node.isEmpty() && node[0] == 2) {
            auto routing = parseRoute(node);
            if (!routing)
                return routing.error();
            QSet<QUuid> result;
            for (auto part : {routing.value().origin, routing.value().anchor, routing.value().destination}) {
                auto nested = dependencies(identity, part);
                if (!nested)
                    return nested.error();
                result.unite(nested.value());
            }
            return result;
        }
        auto target = decode(identity, node);
        if (!target)
            return target.error();
        return QSet<QUuid>{target.value().mount};
    }
    struct Lease {
        QByteArray backend;
        quint64 generation = 0, revision = 0;
    };
    QByteArray wrapLease(QUuid identity, NodeId node, const Request &request, Lease lease) const {
        if (node.size() > 2048 || lease.backend.isEmpty() || lease.backend.size() > 128)
            return {};
        QByteArray plain = identity.toRfc4122() + request.scope.toRfc4122();
        plain.append(char(node.size() >> 8));
        plain.append(char(node.size()));
        plain.append(node);
        for (auto value : {lease.generation, lease.revision})
            for (int shift = 56; shift >= 0; shift -= 8)
                plain.append(char(value >> shift));
        plain.append(char(lease.backend.size() >> 8));
        plain.append(char(lease.backend.size()));
        plain.append(lease.backend);
        std::array<unsigned char, 32> lease_key{};
        crypto_kdf_derive_from_key(lease_key.data(), 32, 3, "DSVFSv1_",
                                   reinterpret_cast<const unsigned char *>(key.constData()));
        QByteArray nonce(24, Qt::Uninitialized);
        randombytes_buf(nonce.data(), nonce.size());
        QByteArray cipher(plain.size() + 16, Qt::Uninitialized);
        crypto_secretbox_easy(reinterpret_cast<unsigned char *>(cipher.data()),
                              reinterpret_cast<const unsigned char *>(plain.constData()), plain.size(),
                              reinterpret_cast<const unsigned char *>(nonce.constData()), lease_key.data());
        return QByteArray(1, char(0x11)) + nonce + cipher;
    }
    Result<Lease> unwrapLease(QUuid identity, NodeId node, const Request &request, QByteArray token) const {
        if (token.size() < 94 || token.size() > 4096 || token[0] != 0x11)
            return err(ErrorCode::MissingEntry);
        std::array<unsigned char, 32> lease_key{};
        crypto_kdf_derive_from_key(lease_key.data(), 32, 3, "DSVFSv1_",
                                   reinterpret_cast<const unsigned char *>(key.constData()));
        QByteArray plain(token.size() - 41, Qt::Uninitialized);
        if (crypto_secretbox_open_easy(
                reinterpret_cast<unsigned char *>(plain.data()),
                reinterpret_cast<const unsigned char *>(token.constData() + 25), token.size() - 25,
                reinterpret_cast<const unsigned char *>(token.constData() + 1), lease_key.data()))
            return err(ErrorCode::MissingEntry);
        if (plain.size() < 52 || QUuid::fromRfc4122(plain.first(16)) != identity ||
            QUuid::fromRfc4122(plain.sliced(16, 16)) != request.scope)
            return err(ErrorCode::MissingEntry);
        auto size = (quint32(quint8(plain[32])) << 8) | quint8(plain[33]);
        if (size > 2048 || plain.size() < 52 + size || plain.sliced(34, size) != node)
            return err(ErrorCode::MissingEntry);
        Lease result;
        auto offset = 34 + size;
        for (int i = 0; i < 8; ++i)
            result.generation = (result.generation << 8) | quint8(plain[offset++]);
        for (int i = 0; i < 8; ++i)
            result.revision = (result.revision << 8) | quint8(plain[offset++]);
        auto length = (quint32(quint8(plain[offset])) << 8) | quint8(plain[offset + 1]);
        offset += 2;
        if (!length || length > 128 || plain.size() != offset + length)
            return err(ErrorCode::MissingEntry);
        result.backend = plain.sliced(offset, length);
        return result;
    }
    NodeId root(QUuid identity) const {
        auto configuration = config.load();
        auto id = configuration->roots.value(identity);
        if (id.isNull()) {
            auto data = QByteArray("dsvfs-synthetic-root-v1") + identity.toRfc4122();
            QByteArray bytes(16, Qt::Uninitialized);
            crypto_generichash(reinterpret_cast<unsigned char *>(bytes.data()), bytes.size(),
                               reinterpret_cast<const unsigned char *>(data.constData()), data.size(),
                               reinterpret_cast<const unsigned char *>(key.constData()), key.size());
            id = QUuid::fromRfc4122(bytes);
        }
        return QByteArray(1, char(0)) + id.toRfc4122();
    }
    Result<bool> allowed(Target target, Operation operation, const Request &request) const {
        if (!request.current() || stopping)
            return false;
        if (!target.mount.isNull()) {
            auto selected = mounted(target.identity, target.mount, request);
            if (!selected || !selected->life->active ||
                (selected->life->conflicted && operation != Operation::ConfigureMounts))
                return false;
            if (operation != Operation::ConfigureMounts) {
                auto configuration = config.load();
                bool assigned = false;
                for (const auto &assignment : configuration->assignments)
                    if (assignment.id == selected->mount.assignment && assignment.identity == target.identity)
                        assigned = true;
                if (!assigned)
                    return false;
                if (!request.effective)
                    for (const auto &definition : configuration->definitions)
                        if (definition.identity == target.identity)
                            return false;
            }
        }
        try {
            return request.authorize(target, operation);
        } catch (...) {
            return false;
        }
    }
    QCoro::Task<Result<void>> membership(Target target, const Request &request, Operation operation,
                                         bool navigation = false) {
        auto mount = mounted(target.identity, target.mount, request);
        if (!mount)
            co_return err(ErrorCode::MissingEntry);
        auto registration = config.load()->backends.value(target.namespace_id);
        if (!registration.owner_identity.isNull() && registration.owner_identity != target.identity)
            co_return err(ErrorCode::MissingEntry);
        auto backend = registration.manager;
        if (!backend)
            co_return err(ErrorCode::StorageUnavailable);
        auto allowed_result = allowed(target, operation, request);
        if (!allowed_result || !allowed_result.value())
            co_return err(ErrorCode::MissingEntry);
        auto namespace_info = co_await backend->namespaceInfo(target.namespace_id);
        if (!namespace_info)
            co_return namespace_info.error();
        if (namespace_info.value().capabilities.testFlag(Capability::Write) &&
            registration.owner_identity != target.identity)
            co_return err(ErrorCode::MissingEntry);
        auto configuration = config.load();
        std::optional<StorageAssignment> assignment;
        for (const auto &a : configuration->assignments)
            if (a.id == mount->mount.assignment && a.identity == target.identity)
                assignment = a;
        if (!assignment)
            co_return err(ErrorCode::MissingEntry);
        bool resource_valid = false;
        for (const auto &r : configuration->resources)
            if (r.id == assignment->resource && r.namespace_id == target.namespace_id &&
                (r.owner_identity.isNull() || r.owner_identity == target.identity))
                resource_valid = true;
        if (!resource_valid)
            co_return err(ErrorCode::MissingEntry);
        auto ancestor = mount->mount.source_directory;
        bool assigned = false;
        QSet<QUuid> ancestors;
        for (quint32 depth = 0; depth < limits.max_depth; ++depth) {
            if (ancestor == assignment->directory) {
                assigned = true;
                break;
            }
            if (ancestors.contains(ancestor))
                break;
            ancestors.insert(ancestor);
            auto info = co_await backend->stat(target.namespace_id, ancestor);
            if (!info || !info.value().parent)
                break;
            ancestor = *info.value().parent;
        }
        if (!assigned)
            co_return err(ErrorCode::MissingEntry);
        QUuid cursor = target.source;
        QSet<QUuid> seen;
        for (quint32 depth = 0; depth < limits.max_depth; ++depth) {
            if (seen.contains(cursor))
                co_return err(ErrorCode::MissingEntry);
            seen.insert(cursor);
            auto visible =
                allowed({target.identity, target.mount, target.namespace_id, cursor}, Operation::Traverse, request);
            if (!visible || !visible.value())
                co_return err(ErrorCode::MissingEntry);
            if (cursor == mount->mount.source_directory) {
                if (request.permissions) {
                    auto path = co_await virtualPath(target.identity, encode(target), request);
                    auto decision = request.permissions->resolve(request.scope, request.groups, path, operation);
                    if (navigation) {
                        if (!request.permissions->navigable(request.scope, request.groups, path))
                            co_return err(ErrorCode::MissingEntry);
                    } else if (decision != Decision::Allow) {
                        auto info = co_await backend->stat(target.namespace_id, target.source);
                        if (operation != Operation::Inspect || !info || info.value().type != EntryType::Directory ||
                            !request.permissions->navigable(request.scope, request.groups, path))
                            co_return err(ErrorCode::MissingEntry);
                    }
                }
                co_return Result<void>{};
            }
            auto info = co_await backend->stat(target.namespace_id, cursor, context(target, request));
            if (!info || !info.value().parent)
                co_return err(ErrorCode::MissingEntry);
            if (!navigation && info.value().type == EntryType::Directory) {
                Target parent{target.identity, target.mount, target.namespace_id, *info.value().parent};
                auto recurse = allowed(parent, Operation::Recurse, request);
                if (!recurse || !recurse.value())
                    co_return err(ErrorCode::MissingEntry);
                if (request.permissions) {
                    auto path = co_await virtualPath(target.identity, encode(parent), request);
                    if (request.permissions->resolve(request.scope, request.groups, path, Operation::Recurse) !=
                        Decision::Allow)
                        co_return err(ErrorCode::MissingEntry);
                }
            }
            cursor = *info.value().parent;
        }
        co_return err(ErrorCode::MissingEntry);
    }
    AccessContext context(Target origin, Request request, std::shared_ptr<Selections> selected = {},
                          std::function<Result<bool>(AccessOperation)> prior_guard = {}) {
        AccessContext access;
        auto selected_mount = mounted(origin.identity, origin.mount, request);
        if (selected_mount)
            for (const auto &assignment : config.load()->assignments)
                if (assignment.id == selected_mount->mount.assignment && assignment.identity == origin.identity) {
                    access.allow_external_files = assignment.allow_external_files;
                    access.assignment_directory = assignment.directory;
                }
        access.authorization_scope = request.scope;
        access.generation = request.generation;
        access.current_generation = request.current_generation;
        const auto self = shared_from_this();
        if (!selected)
            selected = std::make_shared<Selections>();
        access.check = [self, origin, request, selected, prior_guard](NamespaceId ns, QUuid source,
                                                                      AccessOperation operation) -> Result<bool> {
            if (prior_guard) {
                auto permitted = prior_guard(operation);
                if (!permitted || !permitted.value())
                    return false;
            }
            Target target = origin;
            target.namespace_id = ns;
            target.source = source;
            {
                std::lock_guard guard(selected->mutex);
                auto key = ns.toRfc4122() + source.toRfc4122();
                if (selected->entries.contains(key))
                    target = selected->entries.value(key);
            }
            auto mount = self->mounted(target.identity, target.mount, request);
            if (!mount || mount->mount.namespace_id != ns)
                return false;
            if (target.mount != origin.mount &&
                (!request.within_subtree || !request.within_subtree(target, mount->mount.source_directory)))
                return false;
            return self->allowed(target,
                                 operation == AccessOperation::Read   ? Operation::Read
                                 : operation == AccessOperation::List ? Operation::List
                                                                      : Operation::Inspect,
                                 request);
        };
        access.resolve_target = [self, origin, request, selected](FilesystemIdentity physical) -> Result<EntryTarget> {
            if (!request.current() || !request.resolve_target || !request.within_subtree)
                return err(ErrorCode::MissingEntry);
            auto target = request.resolve_target(physical);
            if (!target || target.value().identity != origin.identity)
                return err(ErrorCode::MissingEntry);
            auto mount = self->mounted(origin.identity, target.value().mount, request);
            if (!mount || mount->mount.namespace_id != target.value().namespace_id ||
                !request.within_subtree(target.value(), mount->mount.source_directory))
                return err(ErrorCode::MissingEntry);
            auto approved = self->allowed(target.value(), Operation::Traverse, request);
            if (!approved || !approved.value())
                return err(ErrorCode::MissingEntry);
            {
                std::lock_guard guard(selected->mutex);
                const auto key = target.value().namespace_id.toRfc4122() + target.value().source.toRfc4122();
                if (selected->entries.contains(key)) {
                    if (selected->entries.value(key).mount != target.value().mount)
                        return err(ErrorCode::MissingEntry);
                } else {
                    if (selected->entries.size() >= int(self->limits.max_depth + self->limits.max_page_size + 1))
                        return err(ErrorCode::QuotaExceeded);
                    selected->entries.insert(key, target.value());
                    selected->targets.append(target.value());
                }
            }
            return EntryTarget{target.value().namespace_id, target.value().source};
        };
        access.target_context = [self, origin, request, selected,
                                 prior_guard](EntryTarget entry) -> Result<std::shared_ptr<AccessContext>> {
            Target target;
            {
                std::lock_guard guard(selected->mutex);
                const auto key = entry.namespace_id.toRfc4122() + entry.id.toRfc4122();
                if (!selected->entries.contains(key))
                    return err(ErrorCode::MissingEntry);
                target = selected->entries.value(key);
            }
            auto exposed = [self, origin, request, prior_guard](AccessOperation operation) -> Result<bool> {
                if (prior_guard) {
                    auto permitted = prior_guard(operation);
                    if (!permitted || !permitted.value())
                        return false;
                }
                return self->allowed(origin,
                                     operation == AccessOperation::Read   ? Operation::Read
                                     : operation == AccessOperation::List ? Operation::List
                                                                          : Operation::Inspect,
                                     request);
            };
            return std::make_shared<AccessContext>(self->context(target, request, {}, exposed));
        };
        access.backend = [self](NamespaceId ns) { return self->config.load()->backends.value(ns).manager; };
        access.remaining_aliases = std::min<quint32>(limits.max_depth, 64);
        return access;
    }
    NodeInfo node(Target target, FileInfo info, const Request &request) const {
        NodeInfo result;
        result.id = encode(target);
        result.name = info.name;
        result.type = info.type;
        // Physical directory freshness includes changes to children the requester
        // cannot see. Directories have no readable payload; expose their filtered
        // view revision instead of forwarding that physical freshness signal.
        result.status = info.type == EntryType::Directory ? EntryStatus::Available : info.status;
        result.size = info.type == EntryType::File ? info.size : 0;
        if (info.type == EntryType::File) {
            result.sha256 = info.sha256;
            result.hash_state = info.hash_state;
        }
        if (info.type == EntryType::File && info.modified.isValid()) {
            const auto day = info.modified.toMSecsSinceEpoch() / 86400000;
            result.modified = QDateTime::fromMSecsSinceEpoch(day * 86400000, QTimeZone::UTC);
        }
        auto mount = mounted(target.identity, target.mount, request);
        if (mount) {
            if (target.source == mount->mount.source_directory) {
                result.parent = mount->mount.parent;
                result.name = mount->mount.name;
            } else if (info.parent)
                result.parent = encode({target.identity, target.mount, target.namespace_id, *info.parent});
        }
        const auto backend = config.load()->backends.value(target.namespace_id);
        result.symlinks = backend.symlinks;
        result.name = presentName(result.name, request);
        result.revision = revision(result);
        return result;
    }
    QByteArray persistence(const Configuration &configuration) {
        QJsonArray rows;
        for (auto item : configuration.mounts)
            rows.append(QJsonObject{{"identity", uuid(item.identity)},
                                    {"id", uuid(item.mount.id)},
                                    {"parent", QString::fromLatin1(item.mount.parent.toHex())},
                                    {"namespace", uuid(item.mount.namespace_id)},
                                    {"source", uuid(item.mount.source_directory)},
                                    {"name", item.mount.name},
                                    {"presentation", item.mount.presentation},
                                    {"assignment", uuid(item.mount.assignment)}});
        QJsonArray roots;
        for (auto it = configuration.roots.cbegin(); it != configuration.roots.cend(); ++it)
            roots.append(QJsonObject{{"identity", uuid(it.key())}, {"root", uuid(it.value())}});
        QJsonArray resources, assignments, definitions, navigation;
        for (const auto &r : configuration.resources)
            resources.append(QJsonObject{
                {"id", uuid(r.id)}, {"namespace", uuid(r.namespace_id)}, {"owner", uuid(r.owner_identity)}});
        for (const auto &a : configuration.assignments)
            assignments.append(QJsonObject{{"id", uuid(a.id)},
                                           {"identity", uuid(a.identity)},
                                           {"resource", uuid(a.resource)},
                                           {"directory", uuid(a.directory)},
                                           {"externalFiles", a.allow_external_files}});
        for (const auto &d : configuration.definitions)
            definitions.append(QJsonObject{{"id", uuid(d.id)},
                                           {"identity", uuid(d.identity)},
                                           {"assignment", uuid(d.assignment)},
                                           {"directory", uuid(d.directory)},
                                           {"path", d.path},
                                           {"tier", int(d.tier)},
                                           {"principal", uuid(d.principal)}});
        for (auto it = configuration.navigation_ids.cbegin(); it != configuration.navigation_ids.cend(); ++it)
            navigation.append(QJsonObject{{"key", it.key()}, {"id", uuid(it.value())}});
        return json({{"version", 2},
                     {"nodeKey", QString::fromLatin1(key.toHex())},
                     {"roots", roots},
                     {"mounts", rows},
                     {"resources", resources},
                     {"assignments", assignments},
                     {"definitions", definitions},
                     {"navigation", navigation}});
    }
    QCoro::Task<Result<void>> publish(std::shared_ptr<Configuration> next) {
        QHash<QUuid, QSet<QUuid>> graph;
        QHash<QUuid, quint32> depths;
        for (const auto &item : next->mounts) {
            auto references = dependencies(item.identity, item.mount.parent);
            if (!references)
                co_return references.error();
            graph.insert(item.mount.id, references.value());
        }
        for (int pass = 0; !graph.isEmpty(); ++pass) {
            bool progress = false;
            for (auto it = graph.begin(); it != graph.end();) {
                bool ready = true;
                quint32 depth = 1;
                for (auto parent : it.value()) {
                    if (!depths.contains(parent)) {
                        ready = false;
                        break;
                    }
                    depth = std::max(depth, depths.value(parent) + 1);
                }
                if (!ready) {
                    ++it;
                    continue;
                }
                if (depth > limits.max_depth)
                    co_return err(ErrorCode::InvalidArgument);
                depths.insert(it.key(), depth);
                it = graph.erase(it);
                progress = true;
            }
            if (!progress || pass >= int(limits.max_mounts))
                co_return err(ErrorCode::Conflict);
        }
        QSet<QString> needed_navigation;
        for (const auto &d : next->definitions) {
            QString prefix;
            auto parts = d.path.sliced(1).split('/');
            for (int i = 0; i < parts.size() - 1; ++i) {
                prefix += '/' + parts[i];
                needed_navigation.insert(uuid(d.identity) + ":" + prefix);
            }
        }
        for (auto key : next->navigation_ids.keys())
            if (!needed_navigation.contains(key))
                next->navigation_ids.remove(key);
        if (next->navigation_ids.size() > int(limits.max_mounts * limits.max_depth))
            co_return err(ErrorCode::QuotaExceeded);
        const auto bytes = persistence(*next);
        if (bytes.size() > (2 << 20))
            co_return err(ErrorCode::QuotaExceeded);
        auto saved = co_await detail::work<void>([disk = disk, bytes] { return save(disk, bytes); });
        if (!saved) {
            if (saved.error().code == ErrorCode::IndeterminateOperation)
                stopping = true;
            co_return saved.error();
        }
        next->backends = config.load()->backends;
        config.store(std::move(next));
        ++*topology_generation;
        for (auto key : conflicts.keys())
            if (key.startsWith("overlay:")) {
                auto identity = conflicts.value(key).first;
                conflicts.remove(key);
                if (owner)
                    emit owner->conflictResolved(identity, key);
            }
        for (auto weak : subscriptions)
            if (auto state = weak.lock())
                state->dirty = true;
        co_return Result<void>{};
    }
    QCoro::Task<Result<void>> eraseView(QByteArray id) {
        views.remove(id);
        auto enumeration = enumerations.take(id);
        if (enumeration && !enumeration->source_cursor.isEmpty()) {
            auto backend = config.load()->backends.value(enumeration->target.namespace_id).manager;
            if (backend)
                co_await backend->discardDirectoryCursor(enumeration->target.namespace_id, enumeration->source_cursor);
            QVector<Target> destinations;
            if (enumeration->selected) {
                std::lock_guard lock(enumeration->selected->mutex);
                destinations = enumeration->selected->targets;
            }
            for (auto destination : destinations) {
                auto delegated = config.load()->backends.value(destination.namespace_id).manager;
                if (delegated)
                    co_await delegated->discardDirectoryCursor(destination.namespace_id, enumeration->source_cursor);
            }
        }
        co_return co_await detail::work<void>([disk = disk, id]() -> Result<void> {
            std::lock_guard guard(disk->mutex);
            Statement statement;
            if (!statement.prepare(disk->scratch, "DELETE FROM entry WHERE view=?"))
                return err(ErrorCode::IoError);
            statement.blob(1, id);
            if (sqlite3_step(statement.stmt) != SQLITE_DONE)
                return err(ErrorCode::IoError);
            Statement ordered;
            if (!ordered.prepare(disk->scratch, "DELETE FROM ordered WHERE view=?"))
                return err(ErrorCode::IoError);
            ordered.blob(1, id);
            if (sqlite3_step(ordered.stmt) != SQLITE_DONE)
                return err(ErrorCode::IoError);
            return {};
        });
    }
    QCoro::Task<Result<QByteArray>> buildViewOnce(QUuid identity, NodeId parent, Request request,
                                                  CancellationToken cancel = {});
    QCoro::Task<Result<QByteArray>> buildView(QUuid identity, NodeId parent, Request request,
                                              CancellationToken cancel = {});
    QCoro::Task<Result<void>> verifyAliases(Target target, Request request, Operation operation,
                                            AccessContext inherited);
    QCoro::Task<Result<AccessContext>> routedAccess(QUuid identity, NodeId node, Request request,
                                                    Operation operation = Operation::Inspect);
    QCoro::Task<Result<NodeInfo>> inspect(QUuid identity, NodeId id, Request request, bool conflicts_check = true);
    QCoro::Task<Result<bool>> conflict(QUuid identity, const Mount &mount, bool candidate = false,
                                       Request request = {});
    QCoro::Task<QString> virtualPath(QUuid identity, NodeId node, Request request = {});
    QCoro::Task<Result<QVector<NodeInfo>>> viewRows(QByteArray view, quint64 offset, quint32 count) {
        co_return co_await detail::work<QVector<NodeInfo>>(
            [disk = disk, view, offset, count]() -> Result<QVector<NodeInfo>> {
                std::lock_guard guard(disk->mutex);
                Statement stmt;
                if (!stmt.prepare(disk->scratch,
                                  "SELECT data FROM ordered WHERE view=? AND ordinal>=? ORDER BY ordinal LIMIT ?"))
                    return err(ErrorCode::IoError);
                stmt.blob(1, view);
                sqlite3_bind_int64(stmt.stmt, 2, offset);
                sqlite3_bind_int(stmt.stmt, 3, count);
                QVector<NodeInfo> rows;
                int status;
                while ((status = sqlite3_step(stmt.stmt)) == SQLITE_ROW) {
                    auto bytes = QByteArray(static_cast<const char *>(sqlite3_column_blob(stmt.stmt, 0)),
                                            sqlite3_column_bytes(stmt.stmt, 0));
                    rows.append(fromRecord(QJsonDocument::fromJson(bytes).object()));
                }
                if (status != SQLITE_DONE)
                    return err(ErrorCode::IoError);
                return rows;
            });
    }
};

namespace {
QCoro::Task<Result<void>> traverse(std::shared_ptr<Vfs::Impl> impl, Target target, Request request,
                                   Operation operation) {
    QSet<QUuid> mounts;
    bool navigation = false;
    for (quint32 depth = 0; depth < impl->limits.max_depth; ++depth) {
        auto selected = impl->mounted(target.identity, target.mount, request);
        if (!selected)
            co_return err(ErrorCode::MissingEntry);
        auto authorized = impl->allowed(target, operation, request);
        auto traversable = impl->allowed(target, Operation::Traverse, request);
        if (!authorized || !authorized.value() || !traversable || !traversable.value())
            co_return err(ErrorCode::MissingEntry);
        if (request.effective && request.effective->navigation.contains(selected->mount.parent)) {
            auto path = request.effective->paths.value(target.mount).section('/', 0, -2);
            if (request.permissions && !request.permissions->navigable(request.scope, request.groups, path))
                co_return err(ErrorCode::MissingEntry);
            auto checked = co_await impl->membership(target, request, operation, navigation);
            co_return checked;
        }
        if (selected->mount.parent != impl->root(target.identity)) {
            auto parent = impl->decode(target.identity, selected->mount.parent);
            if (!parent)
                co_return parent.error();
            auto visible = impl->allowed(parent.value(), Operation::Traverse, request);
            if (!visible || !visible.value())
                co_return err(ErrorCode::MissingEntry);
        }
        auto conflict = co_await impl->conflict(target.identity, selected->mount, false, request);
        if (!conflict)
            co_return conflict.error();
        if (conflict.value())
            co_return err(ErrorCode::MissingEntry);
        auto checked = co_await impl->membership(target, request, operation, navigation);
        if (!checked)
            co_return checked.error();
        if (mounts.contains(target.mount))
            co_return err(ErrorCode::MissingEntry);
        mounts.insert(target.mount);
        auto mount = impl->mounted(target.identity, target.mount, request);
        if (!mount)
            co_return err(ErrorCode::MissingEntry);
        if (mount->mount.parent == impl->root(target.identity)) {
            auto root_allowed = impl->allowed({target.identity, {}, {}, {}}, Operation::Traverse, request);
            if (!root_allowed || !root_allowed.value())
                co_return err(ErrorCode::MissingEntry);
            co_return Result<void>{};
        }
        auto parent = impl->decode(target.identity, mount->mount.parent);
        if (!parent)
            co_return parent.error();
        target = parent.value();
        operation = Operation::Traverse;
        navigation = true;
    }
    co_return err(ErrorCode::MissingEntry);
}
} // namespace
QCoro::Task<Result<AccessContext>> Vfs::Impl::routedAccess(QUuid identity, NodeId node_id, Request request,
                                                           Operation operation) {
    auto target = decode(identity, node_id);
    if (!target)
        co_return target.error();
    auto access = context(target.value(), request);
    if (node_id[0] != 2)
        co_return access;
    auto routing = parseRoute(node_id);
    if (!routing)
        co_return routing.error();
    auto origin = decode(identity, routing.value().origin);
    if (!origin)
        co_return origin.error();
    auto anchor = decode(identity, routing.value().anchor);
    if (!anchor)
        co_return anchor.error();
    auto permitted = co_await traverse(shared_from_this(), origin.value(), request, operation);
    if (!permitted)
        co_return permitted.error();
    auto inherited = co_await routedAccess(identity, routing.value().origin, request, operation);
    if (!inherited)
        co_return inherited.error();
    auto selections = std::make_shared<Selections>();
    auto origin_access = context(origin.value(), request, selections);
    origin_access.filesystem_check = inherited.value().filesystem_check;
    origin_access.filesystem_generation = inherited.value().filesystem_generation;
    auto backend = config.load()->backends.value(origin.value().namespace_id).manager;
    auto projected =
        co_await backend->descendantAccess(origin.value().namespace_id, origin.value().source, origin_access);
    if (!projected)
        co_return projected.error();
    bool matches = false;
    {
        std::lock_guard guard(selections->mutex);
        for (const auto &selected : selections->targets)
            if (selected.mount == anchor.value().mount && selected.namespace_id == anchor.value().namespace_id &&
                selected.source == anchor.value().source)
                matches = true;
    }
    if (!matches)
        co_return err(ErrorCode::MissingEntry);
    auto destination = config.load()->backends.value(target.value().namespace_id).manager;
    auto cursor = target.value().source;
    bool within = false;
    for (quint32 depth = 0; depth < limits.max_depth; ++depth) {
        if (cursor == anchor.value().source) {
            within = true;
            break;
        }
        auto info = co_await destination->stat(target.value().namespace_id, cursor, access);
        if (!info || !info.value().parent)
            break;
        cursor = *info.value().parent;
    }
    if (!within)
        co_return err(ErrorCode::MissingEntry);
    access.filesystem_check = projected.value().filesystem_check;
    access.filesystem_generation = projected.value().filesystem_generation;
    co_return access;
}
QCoro::Task<Result<void>> Vfs::Impl::verifyAliases(Target origin, Request request, Operation operation,
                                                   AccessContext inherited) {
    QVector<Target> pending{origin};
    QSet<QByteArray> visited;
    while (!pending.isEmpty()) {
        auto target = pending.takeLast();
        auto key = target.mount.toRfc4122() + target.namespace_id.toRfc4122() + target.source.toRfc4122();
        if (visited.contains(key))
            continue;
        if (visited.size() >= int(limits.max_depth))
            co_return err(ErrorCode::MissingEntry);
        visited.insert(key);
        auto approved = co_await traverse(shared_from_this(), target, request, operation);
        if (!approved)
            co_return approved.error();
        auto selected = std::make_shared<Selections>();
        auto access = context(target, request, selected);
        access.filesystem_check = inherited.filesystem_check;
        access.filesystem_generation = inherited.filesystem_generation;
        auto backend = config.load()->backends.value(target.namespace_id).manager;
        auto info = co_await backend->stat(target.namespace_id, target.source, access);
        if (!info)
            co_return info.error();
        QVector<Target> resolved;
        {
            std::lock_guard guard(selected->mutex);
            resolved = selected->targets;
        }
        for (auto destination : resolved)
            pending.append(destination);
    }
    co_return Result<void>{};
}
namespace {
struct Admission {
    std::shared_ptr<Vfs::Impl> impl;
    explicit Admission(std::shared_ptr<Vfs::Impl> value) : impl(std::move(value)) { ++impl->active_requests; }
    ~Admission() { --impl->active_requests; }
};
struct OperationCancellation {
    std::shared_ptr<Vfs::Impl> impl;
    QUuid id = QUuid::createUuid();
    OperationCancellation(std::shared_ptr<Vfs::Impl> owner, CancellationToken token) : impl(std::move(owner)) {
        impl->active_cancellations.insert(id, token);
    }
    ~OperationCancellation() { impl->active_cancellations.remove(id); }
};
struct ShutdownTurn {
    bool await_ready() const { return false; }
    void await_suspend(std::coroutine_handle<> continuation) {
        QTimer::singleShot(10, QAbstractEventDispatcher::instance(QThread::currentThread()),
                           [continuation] { continuation.resume(); });
    }
    void await_resume() const {}
};
QByteArray cursorMac(QByteArray key, QByteArray input) {
    QByteArray result(16, Qt::Uninitialized);
    input.prepend("dsvfs-page-v1");
    crypto_generichash(reinterpret_cast<unsigned char *>(result.data()), result.size(),
                       reinterpret_cast<const unsigned char *>(input.constData()), input.size(),
                       reinterpret_cast<const unsigned char *>(key.constData()), key.size());
    return result;
}
QByteArray makeCursor(QByteArray key, QByteArray view, quint64 offset) {
    for (int shift = 56; shift >= 0; shift -= 8)
        view.append(char(offset >> shift));
    return view + cursorMac(key, view);
}
} // namespace

Vfs::Vfs(QString private_root, Limits limits, QObject *parent)
    : QObject(parent), impl_(std::make_shared<Impl>(this, std::move(private_root), limits)) {
    auto *timer = new QTimer(this);
    timer->setInterval(1000);
    connect(timer, &QTimer::timeout, this, [this] {
        if (impl_->opened && !impl_->stopping && !impl_->refreshing && !impl_->active_requests && !impl_->changing)
            refresh();
    });
    timer->start();
}
Vfs::~Vfs() {
    impl_->stopping = true;
    impl_->enumerations.clear();
    for (auto weak : impl_->subscriptions)
        if (auto state = weak.lock()) {
            state->active = false;
            state->source.reset();
            state->targets.clear();
        }
    for (auto mounted : QVector<Mounted>(impl_->config.load()->mounts))
        mounted.life->active = false;
}
QCoro::Task<Result<void>> Vfs::open() {
    auto self = impl_;
    if (self->stopping)
        co_return err(ErrorCode::ShuttingDown);
    if (QThread::currentThread() != thread() || self->opened || self->changing)
        co_return err(ErrorCode::InvalidArgument);
    const auto &l = self->limits;
    if (!l.max_mounts || l.max_mounts > 1024 || !l.max_depth || l.max_depth > 128 || !l.max_page_size ||
        l.max_page_size > 1000 || !l.max_batch || l.max_batch > 1000 || !l.max_cursors || l.max_cursors > 1024 ||
        !l.max_subscriptions || l.max_subscriptions > 1024 || !l.max_requests || l.max_requests > 128 ||
        !l.max_readers || l.max_readers > 1024 || !l.reader_lifetime_ms || !l.max_exports || l.max_exports > 16 ||
        !l.max_export_entries || l.max_export_entries > 10000000 || l.max_export_bytes < 65536 ||
        l.max_export_bytes > (1ULL << 32) || l.max_record_bytes < 1024 || l.max_record_bytes > 65536 ||
        !l.request_timeout_ms || !l.export_timeout_ms || !l.export_lifetime_ms || !l.export_idle_ms ||
        !l.cursor_lifetime_ms)
        co_return err(ErrorCode::InvalidArgument);
    if (sodium_init() < 0 || !QDir::isAbsolutePath(self->path))
        co_return err(ErrorCode::StorageUnavailable);
    self->changing = true;
    auto guard = qScopeGuard([self] { self->changing = false; });
    auto data = co_await detail::work<QByteArray>(
        [self] { return load(self->disk, self->path, self->limits.max_export_bytes); });
    if (!data)
        co_return data.error();
    auto configuration = std::make_shared<Configuration>();
    if (data.value().isEmpty()) {
        if (self->disk->has_registry)
            co_return err(ErrorCode::Corruption);
        self->key.resize(32);
        randombytes_buf(self->key.data(), self->key.size());
        auto saved = co_await self->publish(configuration);
        if (!saved)
            co_return saved.error();
    } else {
        if (data.value().size() > (2 << 20))
            co_return err(ErrorCode::Corruption);
        QJsonParseError parse;
        auto document = QJsonDocument::fromJson(data.value(), &parse);
        if (parse.error != QJsonParseError::NoError || !document.isObject())
            co_return err(ErrorCode::Corruption);
        auto object = document.object();
        if (object["version"] != 2)
            co_return err(ErrorCode::UnsupportedCapability);
        if (object.size() != 8 || !object["mounts"].isArray() || !object["resources"].isArray() ||
            !object["assignments"].isArray() || !object["definitions"].isArray() || !object["navigation"].isArray())
            co_return err(ErrorCode::Corruption);
        self->key = QByteArray::fromHex(object["nodeKey"].toString().toLatin1());
        if (self->key.size() != 32 || self->key.toHex() != object["nodeKey"].toString().toLatin1())
            co_return err(ErrorCode::Corruption);
        if (!object["roots"].isArray() || object["roots"].toArray().size() > int(l.max_mounts))
            co_return err(ErrorCode::Corruption);
        QSet<QUuid> root_ids;
        for (auto value : object["roots"].toArray()) {
            auto row = value.toObject();
            auto identity = QUuid(row["identity"].toString()), root = QUuid(row["root"].toString());
            if (identity.isNull() || root.isNull() || configuration->roots.contains(identity) ||
                root_ids.contains(root))
                co_return err(ErrorCode::Corruption);
            configuration->roots.insert(identity, root);
            root_ids.insert(root);
        }
        auto rows = object["mounts"].toArray();
        if (rows.size() > int(l.max_mounts))
            co_return err(ErrorCode::QuotaExceeded);
        QSet<QUuid> ids;
        for (auto value : rows) {
            auto row = value.toObject();
            Mounted item;
            item.identity = QUuid(row["identity"].toString());
            item.mount = {QUuid(row["id"].toString()),
                          QByteArray::fromHex(row["parent"].toString().toLatin1()),
                          QUuid(row["namespace"].toString()),
                          QUuid(row["source"].toString()),
                          row["name"].toString(),
                          row["presentation"].toObject(),
                          QUuid(row["assignment"].toString())};
            if (!configuration->roots.contains(item.identity) || item.identity.isNull() || item.mount.id.isNull() ||
                item.mount.namespace_id.isNull() || item.mount.source_directory.isNull() ||
                !validName(item.mount.name) || ids.contains(item.mount.id) ||
                json(item.mount.presentation).size() > 4096)
                co_return err(ErrorCode::Corruption);
            ids.insert(item.mount.id);
            configuration->mounts.append(item);
        }
        QSet<QUuid> resource_ids, namespaces, assignment_ids;
        if (object["resources"].toArray().size() > int(l.max_mounts) ||
            object["assignments"].toArray().size() > int(l.max_mounts) ||
            object["definitions"].toArray().size() > int(l.max_mounts))
            co_return err(ErrorCode::QuotaExceeded);
        for (auto value : object["resources"].toArray()) {
            auto r = value.toObject();
            StorageResource resource{QUuid(r["id"].toString()), QUuid(r["namespace"].toString()),
                                     QUuid(r["owner"].toString())};
            if (resource.id.isNull() || resource.namespace_id.isNull() || resource_ids.contains(resource.id) ||
                namespaces.contains(resource.namespace_id))
                co_return err(ErrorCode::Corruption);
            resource_ids.insert(resource.id);
            namespaces.insert(resource.namespace_id);
            configuration->resources.append(resource);
        }
        for (auto value : object["assignments"].toArray()) {
            auto r = value.toObject();
            StorageAssignment a{QUuid(r["id"].toString()), QUuid(r["identity"].toString()),
                                QUuid(r["resource"].toString()), QUuid(r["directory"].toString()),
                                r["externalFiles"].toBool()};
            if (a.id.isNull() || a.identity.isNull() || a.directory.isNull() || !resource_ids.contains(a.resource) ||
                assignment_ids.contains(a.id) || !r["externalFiles"].isBool())
                co_return err(ErrorCode::Corruption);
            for (const auto &resource : configuration->resources)
                if (resource.id == a.resource && !resource.owner_identity.isNull() &&
                    (resource.owner_identity != a.identity || a.allow_external_files))
                    co_return err(ErrorCode::Corruption);
            assignment_ids.insert(a.id);
            configuration->assignments.append(a);
        }
        for (auto value : object["definitions"].toArray()) {
            auto r = value.toObject();
            MountDefinition d{QUuid(r["id"].toString()),
                              QUuid(r["identity"].toString()),
                              QUuid(r["assignment"].toString()),
                              QUuid(r["directory"].toString()),
                              r["path"].toString(),
                              Tier(r["tier"].toInt(-1)),
                              QUuid(r["principal"].toString())};
            if (d.id.isNull() || !validVirtualPath(d.path) || d.path == "/" || int(d.tier) < 0 || int(d.tier) > 2 ||
                (d.tier == Tier::Identity ? !d.principal.isNull() : d.principal.isNull()) || d.assignment.isNull())
                co_return err(ErrorCode::Corruption);
            auto found =
                std::find_if(configuration->mounts.cbegin(), configuration->mounts.cend(), [&](const Mounted &m) {
                    return m.mount.id == d.id && m.identity == d.identity && m.mount.assignment == d.assignment &&
                           m.mount.source_directory == d.directory;
                });
            if (found == configuration->mounts.cend())
                co_return err(ErrorCode::Corruption);
            configuration->definitions.append(d);
        }
        if (object["navigation"].toArray().size() > int(l.max_mounts * l.max_depth))
            co_return err(ErrorCode::QuotaExceeded);
        for (auto value : object["navigation"].toArray()) {
            auto r = value.toObject();
            auto key = r["key"].toString();
            auto id = QUuid(r["id"].toString());
            if (key.size() < 38 || QUuid(key.first(36)).isNull() || key[36] != ':' ||
                !validVirtualPath(QStringView(key).sliced(37)) || id.isNull() || root_ids.contains(id) ||
                configuration->navigation_ids.contains(key))
                co_return err(ErrorCode::Corruption);
            configuration->navigation_ids.insert(key, id);
            root_ids.insert(id);
        }
        self->config.store(configuration);
        for (const auto &item : configuration->mounts) {
            auto parent = item.mount.parent;
            QSet<QUuid> visited;
            for (quint32 depth = 0; parent != self->root(item.identity); ++depth) {
                if (depth >= l.max_depth)
                    co_return err(ErrorCode::Corruption);
                auto target = self->decode(item.identity, parent);
                if (!target || visited.contains(target.value().mount) || target.value().mount == item.mount.id)
                    co_return err(ErrorCode::Corruption);
                visited.insert(target.value().mount);
                parent = self->mounted(item.identity, target.value().mount)->mount.parent;
            }
            for (const auto &other : configuration->mounts)
                if (other.mount.id != item.mount.id && other.identity == item.identity &&
                    !std::any_of(
                        configuration->definitions.cbegin(), configuration->definitions.cend(),
                        [&](const MountDefinition &d) { return d.id == item.mount.id || d.id == other.mount.id; }) &&
                    other.mount.parent == item.mount.parent && other.mount.name == item.mount.name)
                    co_return err(ErrorCode::Corruption);
        }
    }
    LFLOG_DEBUG << "VFS private mount registry opened; mounts=" << configuration->mounts.size();
    self->opened = true;
    co_return Result<void>{};
}
Result<void> Vfs::registerNamespace(NamespaceId ns, std::shared_ptr<IFileManager> backend, SymlinkPolicy policy,
                                    QUuid owner_identity) {
    auto available = impl_->available();
    if (!available)
        return available.error();
    if (ns.isNull() || ns == impl_->export_ns || !backend || int(policy) < 0 || int(policy) > 2)
        return err(ErrorCode::InvalidArgument);
    for (const auto &resource : impl_->config.load()->resources)
        if (resource.namespace_id == ns && resource.owner_identity != owner_identity)
            return err(ErrorCode::PermissionDenied);
    if (impl_->config.load()->backends.contains(ns)) {
        auto current = impl_->config.load()->backends.value(ns);
        if (current.manager == backend && current.symlinks == policy && current.owner_identity == owner_identity)
            return {};
        return err(ErrorCode::Conflict);
    }
    if (impl_->config.load()->backends.size() >= int(impl_->limits.max_mounts))
        return err(ErrorCode::QuotaExceeded);
    auto next = std::make_shared<Configuration>(*impl_->config.load());
    next->backends.insert(ns, {std::move(backend), policy, owner_identity});
    impl_->config.store(next);
    return {};
}
Result<NodeId> Vfs::nodeId(QUuid identity, QUuid mount, QUuid source) const {
    auto available = impl_->available();
    if (!available)
        return available.error();
    auto selected = impl_->mounted(identity, mount);
    if (!selected || source.isNull())
        return err(ErrorCode::MissingEntry);
    return impl_->encode({identity, mount, selected->mount.namespace_id, source});
}
NodeId Vfs::root(QUuid identity) const {
    return identity.isNull() || !impl_->opened || impl_->stopping ? NodeId{} : impl_->root(identity);
}
Result<QVector<Mount>> Vfs::mounts(QUuid identity) const {
    auto available = impl_->available();
    if (!available)
        return available.error();
    QVector<Mount> result;
    for (const auto &item : QVector<Mounted>(impl_->config.load()->mounts))
        if (item.identity == identity)
            result.append(item.mount);
    return result;
}
QCoro::Task<QString> Vfs::Impl::virtualPath(QUuid identity, NodeId current, Request request) {
    if (request.effective && request.effective->navigation_paths.contains(current))
        co_return request.effective->navigation_paths.value(current);
    QStringList names;
    for (quint32 depth = 0; depth < limits.max_depth && current != root(identity); ++depth) {
        if (request.effective && request.effective->navigation_paths.contains(current)) {
            co_return request.effective->navigation_paths.value(current) +
                (names.isEmpty() ? QString{} : "/" + names.join('/'));
        }
        auto target = decode(identity, current);
        if (!target) {
            if (request.authorize)
                co_return QString{};
            names.prepend("[unavailable]");
            break;
        }
        auto selected = mounted(identity, target.value().mount, request);
        if (!selected)
            break;
        if (current[0] == 2) {
            auto routing = parseRoute(current);
            if (!routing)
                break;
            auto anchor = decode(identity, routing.value().anchor);
            if (!anchor)
                break;
            if (target.value().source == anchor.value().source) {
                current = routing.value().origin;
                continue;
            }
            auto backend = config.load()->backends.value(target.value().namespace_id).manager;
            auto info = co_await backend->stat(target.value().namespace_id, target.value().source,
                                               request.authorize ? context(target.value(), request) : AccessContext{});
            if (!info || !info.value().parent) {
                if (request.authorize)
                    co_return QString{};
                names.prepend("[unavailable]");
                current = routing.value().origin;
                continue;
            }
            names.prepend(info.value().name);
            auto parent = target.value();
            parent.source = *info.value().parent;
            current = parent.source == anchor.value().source
                          ? routing.value().origin
                          : route(routing.value().origin, routing.value().anchor, encode(parent));
        } else if (target.value().source == selected->mount.source_directory) {
            names.prepend(selected->mount.name);
            current = selected->mount.parent;
        } else {
            auto backend = config.load()->backends.value(target.value().namespace_id).manager;
            auto info = co_await backend->stat(target.value().namespace_id, target.value().source,
                                               request.authorize ? context(target.value(), request) : AccessContext{});
            if (!info || !info.value().parent) {
                if (request.authorize)
                    co_return QString{};
                names.prepend("[unavailable]");
                names.prepend(selected->mount.name);
                current = selected->mount.parent;
                continue;
            }
            names.prepend(info.value().name);
            auto parent = target.value();
            parent.source = *info.value().parent;
            current = encode(parent);
        }
    }
    co_return "/" + names.join('/');
}
QCoro::Task<Result<bool>> Vfs::Impl::conflict(QUuid identity, const Mount &mount, bool candidate, Request request) {
    if (stopping)
        co_return err(ErrorCode::ShuttingDown);
    if (mount.parent == root(identity) || (!mount.parent.isEmpty() && mount.parent[0] == 0))
        co_return false;
    auto target = decode(identity, mount.parent);
    if (!target)
        co_return target.error();
    auto backend = config.load()->backends.value(target.value().namespace_id).manager;
    if (!backend)
        co_return err(ErrorCode::StorageUnavailable);
    auto ns = co_await backend->namespaceInfo(target.value().namespace_id);
    if (!ns)
        co_return ns.error();
    auto cached = collision_cache.constFind(mount.id);
    const bool was_checked = cached != collision_cache.cend();
    if (!candidate && !mount.id.isNull() && cached != collision_cache.cend() &&
        cached->ns == target.value().namespace_id && cached->directory == target.value().source &&
        cached->name == mount.name && cached->generation == ns.value().generation)
        co_return cached->conflict;
    QStringList components;
    auto source = target.value().source;
    QSet<QUuid> seen;
    bool rooted = false;
    for (quint32 depth = 0; depth < limits.max_depth; ++depth) {
        if (source == ns.value().root) {
            rooted = true;
            break;
        }
        if (seen.contains(source))
            break;
        seen.insert(source);
        auto info = co_await backend->stat(target.value().namespace_id, source);
        if (!info || !info.value().parent)
            break;
        components.prepend(info.value().name);
        source = *info.value().parent;
    }
    if (!rooted)
        co_return err(ErrorCode::MissingEntry);
    components.append(mount.name);
    auto existing = co_await backend->lookup(target.value().namespace_id, components.join('/'));
    const bool found = bool(existing);
    if (!existing && existing.error().code != ErrorCode::MissingEntry &&
        existing.error().code != ErrorCode::PermissionDenied)
        co_return existing.error();
    if (candidate)
        co_return found;
    if (!mount.id.isNull()) {
        auto current = mounted(identity, mount.id, request);
        if (!current || current->mount.parent != mount.parent || current->mount.name != mount.name)
            co_return err(ErrorCode::Busy);
        collision_cache.insert(
            mount.id, {target.value().namespace_id, target.value().source, mount.name, ns.value().generation, found});
        if (!request.effective)
            if (auto selected = mounted(identity, mount.id))
                selected->life->conflicted = found;
    }
    auto key = mount.id.toRfc4122();
    if (found) {
        auto parent_path = co_await virtualPath(identity, mount.parent, request);
        if (!parent_path.endsWith('/'))
            parent_path.append('/');
        auto location = parent_path.left(3000) + mount.name;
        if (!conflicts.contains(key) || conflicts.value(key).second != location) {
            conflicts.insert(key, {identity, location});
            if (owner)
                emit owner->conflictDetected(identity, key, location);
        }
    } else if (conflicts.remove(key) || !was_checked) {
        if (owner)
            emit owner->conflictResolved(identity, key);
    }
    co_return found;
}
QCoro::Task<Result<NodeInfo>> Vfs::Impl::inspect(QUuid identity, NodeId id, Request request, bool conflicts_check) {
    if (stopping)
        co_return err(ErrorCode::ShuttingDown);
    if (id == root(identity)) {
        auto permitted = allowed({identity, {}, {}, {}}, Operation::Inspect, request);
        if (!permitted || !permitted.value())
            co_return err(ErrorCode::MissingEntry);
        NodeInfo result;
        result.id = id;
        result.name = "/";
        result.revision = revision(result);
        co_return result;
    }
    if (request.effective && request.effective->identity == identity && request.effective->navigation.contains(id)) {
        if (!request.current())
            co_return err(ErrorCode::MissingEntry);
        auto node = request.effective->navigation.value(id);
        if (request.permissions) {
            QString path;
            // Synthetic paths are stored separately to avoid exposing source metadata.
            for (auto it = request.effective->navigation_paths.cbegin();
                 it != request.effective->navigation_paths.cend(); ++it)
                if (it.key() == id)
                    path = it.value();
            if (!request.permissions->navigable(request.scope, request.groups, path))
                co_return err(ErrorCode::MissingEntry);
        }
        bool visible = false;
        auto path = request.effective->navigation_paths.value(id);
        for (const auto &mount : request.effective->mounts) {
            if (!pathContains(path, request.effective->paths.value(mount.mount.id)))
                continue;
            auto child = co_await inspect(
                identity, encode({identity, mount.mount.id, mount.mount.namespace_id, mount.mount.source_directory}),
                request);
            if (child) {
                visible = true;
                break;
            }
            if (child.error().code != ErrorCode::MissingEntry && child.error().code != ErrorCode::StorageUnavailable &&
                child.error().code != ErrorCode::PermissionDenied)
                co_return child.error();
        }
        if (!visible)
            co_return err(ErrorCode::MissingEntry);
        node.name = presentName(node.name, request);
        node.revision = revision(node);
        co_return node;
    }
    auto target = decode(identity, id);
    if (!target)
        co_return target.error();
    auto checked = co_await traverse(shared_from_this(), target.value(), request, Operation::Inspect);
    if (!checked)
        co_return checked.error();
    auto selected = mounted(identity, target.value().mount, request);
    if (!selected)
        co_return err(ErrorCode::MissingEntry);
    if (conflicts_check) {
        auto collided = co_await conflict(identity, selected->mount, false, request);
        if (!collided)
            co_return collided.error();
        if (collided.value())
            co_return err(ErrorCode::MissingEntry);
    }
    auto backend = config.load()->backends.value(target.value().namespace_id).manager;
    auto access = co_await routedAccess(identity, id, request);
    if (!access)
        co_return access.error();
    auto info = co_await backend->stat(target.value().namespace_id, target.value().source, access.value());
    if (!info)
        co_return info.error();
    auto aliases = co_await verifyAliases(target.value(), request, Operation::Inspect, access.value());
    if (!aliases)
        co_return aliases.error();
    if (!isListable(info.value().status) || !request.current() || !mounted(identity, target.value().mount, request))
        co_return err(ErrorCode::MissingEntry);
    auto result = node(target.value(), info.value(), request);
    result.id = id;
    if (!id.isEmpty() && id[0] == 2 && info.value().parent) {
        auto routing = parseRoute(id);
        if (!routing)
            co_return routing.error();
        auto anchor = decode(identity, routing.value().anchor);
        if (!anchor)
            co_return anchor.error();
        result.parent =
            *info.value().parent == anchor.value().source
                ? routing.value().origin
                : route(routing.value().origin, routing.value().anchor,
                        encode({identity, target.value().mount, target.value().namespace_id, *info.value().parent}));
    }
    auto namespace_info = co_await backend->namespaceInfo(target.value().namespace_id);
    if (!namespace_info)
        co_return namespace_info.error();
    result.capabilities = namespace_info.value().capabilities;
    if (id[0] == 2) {
        auto routing = parseRoute(id);
        if (!routing)
            co_return routing.error();
        auto origin_info = co_await inspect(identity, routing.value().origin, request);
        if (!origin_info)
            co_return origin_info.error();
        result.capabilities = restrictCapabilities(result.capabilities, origin_info.value().capabilities);
        result.symlinks = SymlinkPolicy(std::max(int(result.symlinks), int(origin_info.value().symlinks)));
    }
    result.revision = revision(result);
    co_return result;
}
QCoro::Task<Result<NodeInfo>> Vfs::stat(QUuid identity, NodeId node, Request request) {
    auto self = impl_;
    auto available = self->available();
    if (!available)
        co_return available.error();
    if (self->active_requests >= self->limits.max_requests)
        co_return err(ErrorCode::QueueFull);
    Admission admission(self);
    auto result = co_await self->inspect(identity, node, request);
    if (!result)
        co_return result.error();
    if (result.value().type == EntryType::Directory) {
        auto view = co_await self->buildView(identity, node, request);
        if (!view)
            co_return view.error();
        const auto digest = self->views[view.value()].digest;
        quint64 revision = 0;
        for (int i = 0; i < 8; ++i)
            revision = (revision << 8) | quint8(digest[i]);
        result.value().revision = revision;
        auto erased = co_await self->eraseView(view.value());
        if (!erased)
            co_return erased.error();
    }
    co_return result;
}
QCoro::Task<Result<QByteArray>> Vfs::Impl::buildViewOnce(QUuid identity, NodeId parent, Request request,
                                                         CancellationToken cancel) {
    auto info = co_await inspect(identity, parent, request);
    if (!info)
        co_return info.error();
    if (info.value().type != EntryType::Directory)
        co_return err(ErrorCode::InvalidArgument);
    Target target{identity, {}, {}, {}};
    if (parent != root(identity) && !(request.effective && request.effective->navigation.contains(parent))) {
        auto parsed = decode(identity, parent);
        if (!parsed)
            co_return parsed.error();
        target = parsed.value();
    }
    auto permitted = allowed(target, Operation::List, request);
    if (!permitted || !permitted.value())
        co_return err(ErrorCode::MissingEntry);
    QVector<QByteArray> expired_views;
    for (auto it = views.cbegin(); it != views.cend(); ++it)
        if (it->expires < now())
            expired_views.append(it.key());
    for (auto view : expired_views) {
        auto erased = co_await eraseView(view);
        if (!erased)
            co_return erased.error();
    }
    if (views.size() + enumerations.size() >= int(limits.max_cursors))
        co_return err(ErrorCode::QuotaExceeded);
    auto id = QUuid::createUuid().toRfc4122();
    views.insert(id, {id, identity, parent, request, {}, now() + limits.cursor_lifetime_ms, false});
    bool complete = false;
    auto cleanup = qScopeGuard([&] {
        if (!complete) {
            views[id].expires = 0;
        }
    });
    QElapsedTimer elapsed;
    elapsed.start();
    quint64 count = 0, bytes = 0;
    QHash<QString, Mounted> children;
    for (auto mounted : viewMounts(request))
        if (mounted.identity == identity && mounted.mount.parent == parent)
            children.insert(mounted.mount.name, mounted);
    QSet<QString> navigation_names;
    if (request.effective)
        for (const auto &node : request.effective->navigation)
            if (node.parent == parent)
                navigation_names.insert(node.name);
    QSet<QString> collisions;
    for (const auto &child : children) {
        auto collision = co_await conflict(identity, child.mount, false, request);
        if (!collision)
            co_return collision.error();
        if (collision.value())
            collisions.insert(child.mount.name);
    }
    auto insert = [self = shared_from_this(), id](QVector<NodeInfo> rows) -> QCoro::Task<Result<void>> {
        co_return co_await detail::work<void>([disk = self->disk, id, rows = std::move(rows)]() -> Result<void> {
            std::lock_guard guard(disk->mutex);
            Statement stmt;
            if (!stmt.prepare(disk->scratch, "INSERT INTO entry(view,name,id,data) VALUES(?,?,?,?)"))
                return err(ErrorCode::IoError);
            for (const auto &node : rows) {
                sqlite3_reset(stmt.stmt);
                sqlite3_clear_bindings(stmt.stmt);
                stmt.blob(1, id);
                stmt.blob(2, node.name.toUtf8());
                stmt.blob(3, node.id);
                stmt.blob(4, json(record(node)));
                const auto status = sqlite3_step(stmt.stmt);
                if (status != SQLITE_DONE)
                    return err(status == SQLITE_FULL ? ErrorCode::QuotaExceeded : ErrorCode::IoError);
            }
            return {};
        });
    };
    if (request.effective) {
        for (auto node : request.effective->navigation)
            if (node.parent == parent) {
                auto visible = co_await inspect(identity, node.id, request);
                if (visible) {
                    auto inserted = co_await insert({visible.value()});
                    if (!inserted)
                        co_return inserted.error();
                }
            }
    }
    bool source_list_allowed = true;
    if (!target.mount.isNull() && request.permissions) {
        auto path = co_await virtualPath(identity, parent, request);
        source_list_allowed =
            request.permissions->resolve(request.scope, request.groups, path, Operation::List) == Decision::Allow;
    }
    if (!target.mount.isNull() && source_list_allowed) {
        auto backend = config.load()->backends.value(target.namespace_id).manager;
        auto ns = co_await backend->namespaceInfo(target.namespace_id);
        if (!ns)
            co_return ns.error();
        auto selected = std::make_shared<Selections>();
        auto inherited = co_await routedAccess(identity, parent, request, Operation::List);
        if (!inherited)
            co_return inherited.error();
        auto access = context(target, request, selected);
        access.filesystem_check = inherited.value().filesystem_check;
        access.filesystem_generation = inherited.value().filesystem_generation;
        QByteArray cursor;
        do {
            if (cancel.cancelled())
                co_return err(ErrorCode::Cancelled);
            if (!request.current())
                co_return err(ErrorCode::MissingEntry);
            if (elapsed.elapsed() > limits.request_timeout_ms)
                co_return err(ErrorCode::Busy);
            // Separate administrative conflict checks see raw candidates; all
            // entries composed for this requester use the contextual boundary.
            {
                std::lock_guard guard(selected->mutex);
                selected->entries.clear();
                selected->targets.clear();
            }
            auto page = co_await backend->list(
                target.namespace_id, target.source,
                {std::min(limits.max_page_size, ns.value().config.limits.max_page_size), cursor}, access);
            if (!page)
                co_return page.error();
            cursor = page.value().next_cursor;
            QVector<NodeInfo> rows;
            for (auto item : page.value().entries) {
                if (navigation_names.contains(item.name))
                    continue;
                if (children.contains(item.name)) {
                    collisions.insert(item.name);
                    continue;
                }
                Target entry{identity, target.mount, target.namespace_id, item.id};
                std::optional<Target> anchor;
                {
                    std::lock_guard guard(selected->mutex);
                    if (item.namespace_id != target.namespace_id || (item.parent && *item.parent != target.source)) {
                        for (auto it = selected->targets.crbegin(); it != selected->targets.crend(); ++it)
                            if (it->namespace_id == item.namespace_id && item.parent && it->source == *item.parent) {
                                anchor = *it;
                                break;
                            }
                        if (!anchor)
                            co_return err(ErrorCode::MissingEntry);
                    }
                }
                if (anchor) {
                    entry = *anchor;
                    entry.source = item.id;
                    if (item.namespace_id != entry.namespace_id)
                        co_return err(ErrorCode::MissingEntry);
                }
                NodeId child_id = encode(entry);
                if (anchor)
                    child_id = route(parent, encode(*anchor), child_id);
                else if (!parent.isEmpty() && parent[0] == 2) {
                    auto routing = parseRoute(parent);
                    if (!routing)
                        co_return routing.error();
                    child_id = route(routing.value().origin, routing.value().anchor, child_id);
                }
                if (child_id.isEmpty())
                    co_return err(ErrorCode::QuotaExceeded);
                auto visible = allowed(entry, Operation::Inspect, request);
                if (!visible || !visible.value())
                    continue;
                auto filtered = co_await inspect(identity, child_id, request);
                if (!filtered) {
                    if (filtered.error().code == ErrorCode::MissingEntry ||
                        filtered.error().code == ErrorCode::PermissionDenied)
                        continue;
                    co_return filtered.error();
                }
                auto output = filtered.value();
                output.parent = parent;
                output.capabilities = restrictCapabilities(output.capabilities, ns.value().capabilities);
                output.revision = revision(output);
                bytes += quint64(json(record(output)).size());
                if (++count > limits.max_export_entries || bytes > limits.max_export_bytes)
                    co_return err(ErrorCode::QuotaExceeded);
                rows.append(output);
            }
            auto inserted = co_await insert(std::move(rows));
            if (!inserted)
                co_return inserted.error();
        } while (!cursor.isEmpty());
        auto final = co_await backend->namespaceInfo(target.namespace_id);
        if (!final)
            co_return final.error();
        if (final.value().generation != ns.value().generation)
            co_return err(ErrorCode::StaleCursor);
    }
    for (const auto &mounted : children) {
        const auto key = mounted.mount.id.toRfc4122();
        if (collisions.contains(mounted.mount.name)) {
            mounted.life->conflicted = true;
            if (!conflicts.contains(key)) {
                conflicts.insert(key, {identity, mounted.mount.name});
                if (owner)
                    emit owner->conflictDetected(identity, key, mounted.mount.name);
            }
            continue;
        }
        mounted.life->conflicted = false;
        if (conflicts.remove(key))
            if (owner)
                emit owner->conflictResolved(identity, key);
        auto info = co_await inspect(
            identity, encode({identity, mounted.mount.id, mounted.mount.namespace_id, mounted.mount.source_directory}),
            request, false);
        if (!info) {
            if (info.error().code == ErrorCode::MissingEntry || info.error().code == ErrorCode::StorageUnavailable ||
                info.error().code == ErrorCode::ShuttingDown)
                continue;
            co_return info.error();
        }
        auto inserted = co_await insert({info.value()});
        if (!inserted)
            co_return inserted.error();
    }
    auto ordered = co_await detail::work<void>([disk = disk, id]() -> Result<void> {
        std::lock_guard guard(disk->mutex);
        Statement statement;
        if (!statement.prepare(
                disk->scratch,
                "INSERT INTO ordered SELECT view,ROW_NUMBER() OVER(ORDER BY name,id)-1,data FROM entry WHERE view=?"))
            return err(ErrorCode::IoError);
        statement.blob(1, id);
        const auto status = sqlite3_step(statement.stmt);
        if (status != SQLITE_DONE)
            return err(status == SQLITE_FULL ? ErrorCode::QuotaExceeded : ErrorCode::IoError);
        return {};
    });
    if (!ordered)
        co_return ordered.error();
    auto hash = std::make_shared<QCryptographicHash>(QCryptographicHash::Sha256);
    auto initial_hash = co_await detail::work<void>([hash, directory = info.value()]() -> Result<void> {
        hash->addData(json(record(directory)) + '\n');
        return {};
    });
    if (!initial_hash)
        co_return initial_hash.error();
    quint64 offset = 0;
    for (;;) {
        auto rows = co_await viewRows(id, offset, limits.max_batch);
        if (!rows)
            co_return rows.error();
        if (rows.value().isEmpty())
            break;
        auto hashed = co_await detail::work<void>([hash, items = rows.value()]() -> Result<void> {
            for (const auto &item : items)
                hash->addData(json(record(item)) + '\n');
            return {};
        });
        if (!hashed)
            co_return hashed.error();
        offset += quint64(rows.value().size());
        if (elapsed.elapsed() > limits.request_timeout_ms || cancel.cancelled())
            co_return err(cancel.cancelled() ? ErrorCode::Cancelled : ErrorCode::Busy);
    }
    if (!request.current())
        co_return err(ErrorCode::MissingEntry);
    views[id].digest = hash->result();
    views[id].ready = true;
    complete = true;
    co_return id;
}
QCoro::Task<Result<QByteArray>> Vfs::Impl::buildView(QUuid identity, NodeId parent, Request request,
                                                     CancellationToken cancel) {
    QElapsedTimer elapsed;
    elapsed.start();
    for (int attempt = 0; attempt < 3; ++attempt) {
        if (elapsed.elapsed() >= limits.request_timeout_ms)
            co_return err(ErrorCode::Busy);
        auto result = co_await buildViewOnce(identity, parent, request, cancel);
        if (result || result.error().code != ErrorCode::StaleCursor)
            co_return result;
        QVector<QByteArray> abandoned;
        for (auto it = views.cbegin(); it != views.cend(); ++it)
            if (!it->ready && it->expires == 0)
                abandoned.append(it.key());
        for (auto view : abandoned)
            co_await eraseView(view);
    }
    co_return err(ErrorCode::Busy);
}
QCoro::Task<Result<DirectoryPage>> Vfs::list(QUuid identity, NodeId parent, PageRequest page, Request request) {
    auto self = impl_;
    auto available = self->available();
    if (!available)
        co_return available.error();
    if (!page.limit || page.limit > self->limits.max_page_size || page.cursor.size() > 40)
        co_return err(ErrorCode::InvalidArgument);
    if (self->active_requests >= self->limits.max_requests)
        co_return err(ErrorCode::QueueFull);
    Admission admission(self);
    std::shared_ptr<Enumeration> state;
    if (page.cursor.isEmpty()) {
        for (auto id : self->enumerations.keys())
            if (self->enumerations[id]->expires < now() || !self->enumerations[id]->request.current())
                self->enumerations.remove(id);
        if (self->enumerations.size() + self->views.size() >= int(self->limits.max_cursors))
            co_return err(ErrorCode::QuotaExceeded);
        auto info = co_await self->inspect(identity, parent, request);
        if (!info)
            co_return info.error();
        if (info.value().type != EntryType::Directory)
            co_return err(ErrorCode::InvalidArgument);
        state = std::make_shared<Enumeration>();
        state->id = QUuid::createUuid().toRfc4122();
        state->identity = identity;
        state->directory = parent;
        state->request = request;
        state->expires = now() + self->limits.cursor_lifetime_ms;
        state->target = {identity, {}, {}, {}};
        bool synthetic =
            parent == root(identity) || (request.effective && request.effective->navigation.contains(parent));
        if (!synthetic) {
            auto target = self->decode(identity, parent);
            if (!target)
                co_return target.error();
            state->target = target.value();
        }
        auto permitted = self->allowed(state->target, Operation::List, request);
        if (!permitted || !permitted.value())
            co_return err(ErrorCode::MissingEntry);
        state->source_done = synthetic;
        if (!synthetic && request.permissions) {
            auto path = co_await self->virtualPath(identity, parent, request);
            if (request.permissions->resolve(request.scope, request.groups, path, Operation::List) != Decision::Allow) {
                if (!request.permissions->navigable(request.scope, request.groups, path))
                    co_return err(ErrorCode::MissingEntry);
                state->source_done = true;
            }
        }
        for (auto mounted : self->viewMounts(request))
            if (mounted.identity == identity && mounted.mount.parent == parent) {
                state->shadowed.insert(presentName(mounted.mount.name, request));
                auto info = co_await self->inspect(identity,
                                                   self->encode({identity, mounted.mount.id, mounted.mount.namespace_id,
                                                                 mounted.mount.source_directory}),
                                                   request);
                if (info)
                    state->children.append(info.value());
                else if (info.error().code != ErrorCode::MissingEntry &&
                         info.error().code != ErrorCode::StorageUnavailable)
                    co_return info.error();
            }
        if (request.effective)
            for (auto node : request.effective->navigation)
                if (node.parent == parent) {
                    state->shadowed.insert(presentName(node.name, request));
                    auto visible = co_await self->inspect(identity, node.id, request);
                    if (visible)
                        state->children.append(visible.value());
                }
        auto compare = [](const NodeInfo &a, const NodeInfo &b) {
            auto an = a.name.toUtf8(), bn = b.name.toUtf8();
            return an == bn ? a.id < b.id : an < bn;
        };
        std::sort(state->children.begin(), state->children.end(), compare);
        if (!state->source_done) {
            state->selected = std::make_shared<Selections>();
            auto inherited = co_await self->routedAccess(identity, parent, request, Operation::List);
            if (!inherited)
                co_return inherited.error();
            state->access = self->context(state->target, request, state->selected);
            state->access.filesystem_check = inherited.value().filesystem_check;
            state->access.filesystem_generation = inherited.value().filesystem_generation;
            // Bind backend continuation mode as well as requester authority.
            state->access.filesystem_generation = QCryptographicHash::hash(
                state->access.filesystem_generation + QByteArray(request.allow_utf8 ? "live:utf8" : "live:ascii"),
                QCryptographicHash::Sha256);
        }
        self->enumerations.insert(state->id, state);
    } else {
        if (page.cursor.size() != 40 ||
            !equalBytes(cursorMac(self->key, page.cursor.first(24)), page.cursor.sliced(24)))
            co_return err(ErrorCode::StaleCursor);
        state = self->enumerations.value(page.cursor.first(16));
        quint64 serial = 0;
        for (int i = 16; i < 24; ++i)
            serial = (serial << 8) | quint8(page.cursor[i]);
        if (!state || state->serial != serial || state->expires < now() || state->identity != identity ||
            state->directory != parent || state->request.scope != request.scope ||
            state->request.generation != request.generation ||
            state->request.current_generation != request.current_generation ||
            state->request.allow_utf8 != request.allow_utf8 || state->request.effective != request.effective ||
            state->request.permissions != request.permissions || state->request.groups != request.groups ||
            !request.current())
            co_return err(ErrorCode::StaleCursor);
    }
    if (state->busy)
        co_return err(ErrorCode::Busy);
    state->busy = true;
    auto guard = qScopeGuard([state] { state->busy = false; });
    DirectoryPage output;
    bool fetched = false;
    quint32 examined = 0;
    const auto maximum = std::min(self->limits.max_batch, self->limits.max_page_size);
    while (output.entries.size() < page.limit && examined < maximum) {
        if (state->buffer.isEmpty() && !state->source_done) {
            if (fetched)
                break;
            fetched = true;
            auto backend = self->config.load()->backends.value(state->target.namespace_id).manager;
            if (!backend)
                co_return err(ErrorCode::StorageUnavailable);
            if (!backend->supportsStableEnumeration())
                co_return err(ErrorCode::UnsupportedCapability);
            auto ns = co_await backend->namespaceInfo(state->target.namespace_id);
            if (!ns)
                co_return ns.error();
            {
                std::lock_guard lock(state->selected->mutex);
                state->selected->entries.clear();
                state->selected->targets.clear();
            }
            auto source = co_await backend->list(state->target.namespace_id, state->target.source,
                                                 {std::min(maximum, ns.value().config.limits.max_page_size),
                                                  state->source_cursor, true, !request.allow_utf8},
                                                 state->access);
            if (!source) {
                self->enumerations.remove(state->id);
                co_return source.error();
            }
            state->source_cursor = source.value().next_cursor;
            state->source_done = state->source_cursor.isEmpty();
            for (const auto &item : source.value().entries) {
                Target target{identity, state->target.mount, state->target.namespace_id, item.id};
                NodeId id;
                if (item.namespace_id != target.namespace_id || (item.parent && *item.parent != state->target.source)) {
                    std::optional<Target> anchor;
                    {
                        std::lock_guard lock(state->selected->mutex);
                        for (const auto &t : state->selected->targets)
                            if (t.namespace_id == item.namespace_id && item.parent && t.source == *item.parent)
                                anchor = t;
                    }
                    if (!anchor)
                        co_return err(ErrorCode::MissingEntry);
                    target = *anchor;
                    target.source = item.id;
                    id = self->route(parent, self->encode(*anchor), self->encode(target));
                } else if (!parent.isEmpty() && parent[0] == 2) {
                    auto route = self->parseRoute(parent);
                    if (!route)
                        co_return route.error();
                    id = self->route(route.value().origin, route.value().anchor, self->encode(target));
                } else
                    id = self->encode(target);
                auto info = co_await self->inspect(identity, id, request);
                if (info) {
                    auto node = info.value();
                    node.parent = parent;
                    state->buffer.append(node);
                } else if (info.error().code != ErrorCode::MissingEntry &&
                           info.error().code != ErrorCode::PermissionDenied)
                    co_return info.error();
            }
            // Backend pages already bound inspected candidates, including hidden
            // entries. An empty continuation consumes this call's scan budget.
            if (state->buffer.isEmpty() && !state->source_done)
                break;
        }
        if (state->buffer.isEmpty() && state->children.isEmpty())
            break;
        bool child = state->buffer.isEmpty() || (!state->children.isEmpty() && state->children.front().name.toUtf8() <=
                                                                                   state->buffer.front().name.toUtf8());
        NodeInfo node = child ? state->children.takeFirst() : state->buffer.takeFirst();
        ++examined;
        if (!child && state->shadowed.contains(node.name))
            continue;
        auto key = node.name.toUtf8() + char(0) + node.id;
        if (!state->last_key.isEmpty() && key <= state->last_key)
            continue;
        auto visible = co_await self->inspect(identity, node.id, request);
        if (!visible) {
            if (visible.error().code == ErrorCode::MissingEntry || visible.error().code == ErrorCode::PermissionDenied)
                continue;
            co_return visible.error();
        }
        if (visible.value().revision != node.revision)
            node.status = EntryStatus::Updating;
        state->last_key = key;
        output.entries.append(node);
    }
    if (!request.current()) {
        self->enumerations.remove(state->id);
        co_return err(ErrorCode::MissingEntry);
    }
    if (!state->source_done || !state->buffer.isEmpty() || !state->children.isEmpty()) {
        ++state->serial;
        output.next_cursor = makeCursor(self->key, state->id, state->serial);
    } else
        self->enumerations.remove(state->id);
    co_return output;
}
QCoro::Task<Result<NodeInfo>> Vfs::lookup(QUuid identity, NodeId parent, QString name, Request request) {
    if (!validName(name, !request.allow_utf8))
        co_return err(ErrorCode::InvalidName);
    QElapsedTimer elapsed;
    elapsed.start();
    PageRequest page;
    page.limit = impl_->limits.max_page_size;
    do {
        if (elapsed.elapsed() > impl_->limits.request_timeout_ms)
            co_return err(ErrorCode::Busy);
        auto rows = co_await list(identity, parent, page, request);
        if (!rows)
            co_return rows.error();
        for (auto item : rows.value().entries)
            if (item.name == name) {
                if (!rows.value().next_cursor.isEmpty())
                    co_await impl_->eraseView(rows.value().next_cursor.first(16));
                co_return item;
            }
        page.cursor = rows.value().next_cursor;
    } while (!page.cursor.isEmpty());
    co_return err(ErrorCode::MissingEntry);
}

namespace {
Result<std::shared_ptr<void>> permit(std::shared_ptr<Vfs::Impl> self, Target target, Operation operation,
                                     const Request &request) {
    auto approved = self->allowed(target, operation, request);
    if (!approved || !approved.value())
        return err(ErrorCode::MissingEntry);
    if (!request.commit_permit)
        return err(ErrorCode::PermissionDenied);
    Result<std::shared_ptr<void>> lease = err(ErrorCode::PermissionDenied);
    try {
        lease = request.commit_permit(target, operation);
    } catch (...) {
        return err(ErrorCode::PermissionDenied);
    }
    if (!lease || !lease.value())
        return err(ErrorCode::PermissionDenied);
    if (!request.current())
        return err(ErrorCode::MissingEntry);
    std::shared_ptr<Life> life;
    if (!target.mount.isNull()) {
        auto mount = self->mounted(target.identity, target.mount, request);
        if (!mount || !mount->life->active)
            return err(ErrorCode::MissingEntry);
        life = mount->life;
        ++life->commits;
    }
    struct Held {
        std::shared_ptr<void> host;
        std::shared_ptr<Life> life;
        ~Held() {
            if (life)
                --life->commits;
        }
    };
    auto held = std::make_shared<Held>();
    held->host = lease.value();
    held->life = life;
    return std::static_pointer_cast<void>(held);
}
} // namespace
namespace {
QCoro::Task<Result<void>> sourceWithin(std::shared_ptr<IFileManager> backend, NamespaceId ns, QUuid source, QUuid root,
                                       quint32 maximum) {
    QSet<QUuid> seen;
    for (quint32 depth = 0; depth < maximum; ++depth) {
        if (source == root)
            co_return Result<void>{};
        if (seen.contains(source))
            break;
        seen.insert(source);
        auto info = co_await backend->stat(ns, source);
        if (!info || !info.value().parent)
            break;
        source = *info.value().parent;
    }
    co_return err(ErrorCode::MissingEntry);
}
QCoro::Task<Result<FileInfo>> logicalChild(std::shared_ptr<IFileManager> backend, NamespaceId ns, QUuid parent,
                                           QString name, quint32 maximum) {
    auto space = co_await backend->namespaceInfo(ns);
    if (!space)
        co_return space.error();
    QStringList names{name};
    QSet<QUuid> seen;
    for (quint32 depth = 0; depth < maximum; ++depth) {
        if (parent == space.value().root)
            co_return co_await backend->lookup(ns, names.join('/'));
        if (seen.contains(parent))
            break;
        seen.insert(parent);
        auto info = co_await backend->stat(ns, parent);
        if (!info || !info.value().parent)
            break;
        names.prepend(info.value().name);
        parent = *info.value().parent;
    }
    co_return err(ErrorCode::MissingEntry);
}
bool committing(const std::shared_ptr<Vfs::Impl> &self) {
    for (const auto &m : self->config.load()->mounts)
        if (m.life->commits)
            return true;
    return false;
}
} // namespace
Result<QVector<StorageResource>> Vfs::resources() const {
    auto result = impl_->available();
    if (!result)
        return result.error();
    return impl_->config.load()->resources;
}
Result<QVector<StorageAssignment>> Vfs::assignments(QUuid identity) const {
    auto result = impl_->available();
    if (!result)
        return result.error();
    QVector<StorageAssignment> output;
    for (auto a : impl_->config.load()->assignments)
        if (a.identity == identity)
            output.append(a);
    return output;
}
Result<QVector<MountDefinition>> Vfs::definitions(QUuid identity) const {
    auto result = impl_->available();
    if (!result)
        return result.error();
    QVector<MountDefinition> output;
    for (auto d : impl_->config.load()->definitions)
        if (d.identity == identity)
            output.append(d);
    return output;
}
QCoro::Task<Result<StorageResource>> Vfs::addResource(StorageResource resource, Request admin) {
    auto self = impl_;
    auto available = self->available();
    if (!available)
        co_return available.error();
    if (self->changing || committing(self))
        co_return err(ErrorCode::Busy);
    self->changing = true;
    auto guard = qScopeGuard([self] { self->changing = false; });
    auto held = permit(self, {resource.owner_identity, {}, {}, {}}, Operation::ConfigureMounts, admin);
    if (!held)
        co_return held.error();
    if (!resource.id.isNull() || resource.namespace_id.isNull())
        co_return err(ErrorCode::InvalidArgument);
    auto backend = self->config.load()->backends.value(resource.namespace_id);
    if (!backend.manager)
        co_return err(ErrorCode::StorageUnavailable);
    auto ns = co_await backend.manager->namespaceInfo(resource.namespace_id);
    if (!ns)
        co_return ns.error();
    if ((ns.value().capabilities.testFlag(Capability::Write) && resource.owner_identity.isNull()) ||
        backend.owner_identity != resource.owner_identity)
        co_return err(ErrorCode::PermissionDenied);
    auto next = std::make_shared<Configuration>(*self->config.load());
    if (next->resources.size() >= int(self->limits.max_mounts))
        co_return err(ErrorCode::QuotaExceeded);
    for (const auto &r : next->resources)
        if (r.namespace_id == resource.namespace_id)
            co_return err(ErrorCode::Conflict);
    resource.id = QUuid::createUuid();
    next->resources.append(resource);
    auto saved = co_await self->publish(next);
    if (!saved)
        co_return saved.error();
    LFLOG_DEBUG << "VFS storage resource registered";
    co_return resource;
}
QCoro::Task<Result<StorageAssignment>> Vfs::assignStorage(StorageAssignment assignment, Request admin) {
    auto self = impl_;
    auto available = self->available();
    if (!available)
        co_return available.error();
    if (self->changing || committing(self))
        co_return err(ErrorCode::Busy);
    self->changing = true;
    auto guard = qScopeGuard([self] { self->changing = false; });
    auto held = permit(self, {assignment.identity, {}, {}, {}}, Operation::ConfigureMounts, admin);
    if (!held)
        co_return held.error();
    if (!assignment.id.isNull() || assignment.identity.isNull() || assignment.directory.isNull())
        co_return err(ErrorCode::InvalidArgument);
    auto next = std::make_shared<Configuration>(*self->config.load());
    auto resource = std::find_if(next->resources.cbegin(), next->resources.cend(),
                                 [&](const StorageResource &r) { return r.id == assignment.resource; });
    if (resource == next->resources.cend())
        co_return err(ErrorCode::MissingEntry);
    if ((!resource->owner_identity.isNull() && resource->owner_identity != assignment.identity) ||
        (!resource->owner_identity.isNull() && assignment.allow_external_files))
        co_return err(ErrorCode::PermissionDenied);
    auto backend = next->backends.value(resource->namespace_id);
    if (!backend.manager)
        co_return err(ErrorCode::StorageUnavailable);
    if (assignment.allow_external_files && backend.symlinks != SymlinkPolicy::Always)
        co_return err(ErrorCode::PermissionDenied);
    auto info = co_await backend.manager->stat(resource->namespace_id, assignment.directory);
    if (!info || info.value().type != EntryType::Directory)
        co_return err(ErrorCode::MissingEntry);
    if (next->assignments.size() >= int(self->limits.max_mounts))
        co_return err(ErrorCode::QuotaExceeded);
    assignment.id = QUuid::createUuid();
    next->assignments.append(assignment);
    auto saved = co_await self->publish(next);
    if (!saved)
        co_return saved.error();
    LFLOG_DEBUG << "VFS identity storage assignment created";
    co_return assignment;
}
QCoro::Task<Result<void>> Vfs::withdrawAssignment(QUuid id, Request admin) {
    auto self = impl_;
    auto available = self->available();
    if (!available)
        co_return available.error();
    if (self->changing || committing(self))
        co_return err(ErrorCode::Busy);
    self->changing = true;
    auto guard = qScopeGuard([self] { self->changing = false; });
    auto next = std::make_shared<Configuration>(*self->config.load());
    auto a = std::find_if(next->assignments.cbegin(), next->assignments.cend(),
                          [id](const StorageAssignment &a) { return a.id == id; });
    if (a == next->assignments.cend())
        co_return err(ErrorCode::MissingEntry);
    auto held = permit(self, {a->identity, {}, {}, {}}, Operation::ConfigureMounts, admin);
    if (!held)
        co_return held.error();
    next->assignments.removeIf([id](const StorageAssignment &a) { return a.id == id; });
    auto saved = co_await self->publish(next);
    if (!saved)
        co_return saved.error();
    LFLOG_DEBUG << "VFS storage assignment withdrawn; delegated operations revoked";
    co_return Result<void>{};
}
QCoro::Task<Result<MountDefinition>> Vfs::defineMount(MountDefinition definition, Request admin) {
    auto self = impl_;
    auto available = self->available();
    if (!available)
        co_return available.error();
    if (self->changing || committing(self))
        co_return err(ErrorCode::Busy);
    self->changing = true;
    auto guard = qScopeGuard([self] { self->changing = false; });
    auto held = permit(self, {definition.identity, {}, {}, {}}, Operation::ConfigureMounts, admin);
    if (!held)
        co_return held.error();
    if (!definition.id.isNull() || definition.identity.isNull() || !validVirtualPath(definition.path) ||
        definition.path == "/" || definition.path.count('/') > int(self->limits.max_depth) ||
        int(definition.tier) < 0 || int(definition.tier) > 2 ||
        (definition.tier == Tier::Identity ? !definition.principal.isNull() : definition.principal.isNull()))
        co_return err(ErrorCode::InvalidArgument);
    auto next = std::make_shared<Configuration>(*self->config.load());
    for (const auto &d : next->definitions)
        if (d.identity == definition.identity && d.tier == definition.tier && d.principal == definition.principal &&
            d.path == definition.path)
            co_return err(ErrorCode::Conflict);
    auto assignment =
        std::find_if(next->assignments.cbegin(), next->assignments.cend(), [&](const StorageAssignment &a) {
            return a.id == definition.assignment && a.identity == definition.identity;
        });
    if (assignment == next->assignments.cend())
        co_return err(ErrorCode::PermissionDenied);
    auto resource = std::find_if(next->resources.cbegin(), next->resources.cend(),
                                 [&](const StorageResource &r) { return r.id == assignment->resource; });
    if (resource == next->resources.cend())
        co_return err(ErrorCode::MissingEntry);
    auto backend = next->backends.value(resource->namespace_id).manager;
    if (!backend)
        co_return err(ErrorCode::StorageUnavailable);
    if (definition.directory.isNull())
        definition.directory = assignment->directory;
    auto within = co_await sourceWithin(backend, resource->namespace_id, definition.directory, assignment->directory,
                                        self->limits.max_depth);
    auto info = co_await backend->stat(resource->namespace_id, definition.directory);
    if (!within || !info || info.value().type != EntryType::Directory)
        co_return err(ErrorCode::PermissionDenied);
    if (next->mounts.size() >= int(self->limits.max_mounts))
        co_return err(ErrorCode::QuotaExceeded);
    QString prefix;
    auto parts = definition.path.sliced(1).split('/');
    for (int i = 0; i < parts.size() - 1; ++i) {
        prefix += '/' + parts[i];
        auto key = uuid(definition.identity) + ":" + prefix;
        if (!next->navigation_ids.contains(key))
            next->navigation_ids.insert(key, QUuid::createUuid());
    }
    definition.id = QUuid::createUuid();
    next->definitions.append(definition);
    if (!next->roots.contains(definition.identity))
        next->roots.insert(definition.identity, QUuid::fromRfc4122(self->root(definition.identity).sliced(1)));
    next->mounts.append({definition.identity,
                         {definition.id,
                          self->root(definition.identity),
                          resource->namespace_id,
                          definition.directory,
                          definition.path.section('/', -1),
                          {},
                          definition.assignment}});
    auto saved = co_await self->publish(next);
    if (!saved)
        co_return saved.error();
    co_return definition;
}
QCoro::Task<Result<void>> Vfs::relocateDefinition(QUuid id, QString path, Request admin) {
    auto self = impl_;
    auto available = self->available();
    if (!available)
        co_return available.error();
    if (self->changing || committing(self))
        co_return err(ErrorCode::Busy);
    self->changing = true;
    auto guard = qScopeGuard([self] { self->changing = false; });
    if (!validVirtualPath(path) || path == "/" || path.count('/') > int(self->limits.max_depth))
        co_return err(ErrorCode::InvalidArgument);
    auto next = std::make_shared<Configuration>(*self->config.load());
    auto definition = std::find_if(next->definitions.begin(), next->definitions.end(),
                                   [id](const MountDefinition &d) { return d.id == id; });
    if (definition == next->definitions.end())
        co_return err(ErrorCode::MissingEntry);
    auto held = permit(self, {definition->identity, {}, {}, {}}, Operation::ConfigureMounts, admin);
    if (!held)
        co_return held.error();
    for (const auto &d : next->definitions)
        if (d.id != id && d.identity == definition->identity && d.tier == definition->tier &&
            d.principal == definition->principal && d.path == path)
            co_return err(ErrorCode::Conflict);
    QString prefix;
    auto parts = path.sliced(1).split('/');
    for (int i = 0; i < parts.size() - 1; ++i) {
        prefix += '/' + parts[i];
        auto key = uuid(definition->identity) + ":" + prefix;
        if (!next->navigation_ids.contains(key))
            next->navigation_ids.insert(key, QUuid::createUuid());
    }
    definition->path = path;
    for (auto &m : next->mounts)
        if (m.mount.id == id)
            m.mount.name = parts.last();
    co_return co_await self->publish(next);
}
QCoro::Task<Result<void>> Vfs::removeDefinition(QUuid id, Request admin) {
    auto self = impl_;
    auto available = self->available();
    if (!available)
        co_return available.error();
    if (self->changing || committing(self))
        co_return err(ErrorCode::Busy);
    self->changing = true;
    auto guard = qScopeGuard([self] { self->changing = false; });
    auto next = std::make_shared<Configuration>(*self->config.load());
    auto d = std::find_if(next->definitions.cbegin(), next->definitions.cend(),
                          [id](const MountDefinition &d) { return d.id == id; });
    if (d == next->definitions.cend())
        co_return err(ErrorCode::MissingEntry);
    auto held = permit(self, {d->identity, {}, {}, {}}, Operation::ConfigureMounts, admin);
    if (!held)
        co_return held.error();
    auto old = self->mounted(d->identity, id);
    next->definitions.removeIf([id](const MountDefinition &d) { return d.id == id; });
    next->mounts.removeIf([id](const Mounted &m) { return m.mount.id == id; });
    auto saved = co_await self->publish(next);
    if (!saved)
        co_return saved.error();
    if (old)
        old->life->active = false;
    co_return Result<void>{};
}
Result<QVector<EffectiveMount>> Vfs::effectiveMounts(QUuid identity, const Request &request) const {
    auto available = impl_->available();
    if (!available)
        return available.error();
    if (!request.current() || !request.effective || request.effective->identity != identity)
        return err(ErrorCode::MissingEntry);
    return request.effective->descriptors;
}
QCoro::Task<Result<Request>> Vfs::compose(QUuid identity, Request request) {
    auto self = impl_;
    auto available = self->available();
    if (!available)
        co_return available.error();
    if (self->active_requests >= self->limits.max_requests)
        co_return err(ErrorCode::QueueFull);
    Admission admission(self);
    QElapsedTimer elapsed;
    elapsed.start();
    request.effective.reset();
    if (!request.current() || identity.isNull() || request.groups.size() > 128)
        co_return err(ErrorCode::PermissionDenied);
    if (request.permissions) {
        auto valid = request.permissions->validate();
        if (!valid)
            co_return valid.error();
    }
    auto generation = self->topology_generation->load();
    auto topology = std::make_shared<EffectiveVfs>();
    topology->identity = identity;
    topology->contact = request.scope;
    topology->generation = generation;
    topology->current_generation = self->topology_generation;
    topology->groups = request.groups;
    topology->permissions = request.permissions;
    auto configuration = self->config.load();
    auto candidates = configuration->definitions;
    QSet<QUuid> definition_ids;
    for (const auto &d : candidates)
        definition_ids.insert(d.id);
    for (const auto &m : configuration->mounts)
        if (m.identity == identity && !definition_ids.contains(m.mount.id)) {
            auto path = co_await self->virtualPath(
                identity, self->encode({identity, m.mount.id, m.mount.namespace_id, m.mount.source_directory}));
            candidates.append(
                {m.mount.id, identity, m.mount.assignment, m.mount.source_directory, path, Tier::Identity, {}});
        }
    auto selected = resolveOverlay(candidates, identity, request.scope, request.groups);
    if (!selected)
        co_return selected.error();
    topology->conflicts = selected.value().conflicts;
    QHash<QString, NodeId> nodes;
    nodes.insert("/", root(identity));
    for (const auto &d : selected.value().mounts) {
        if (self->stopping)
            co_return err(ErrorCode::ShuttingDown);
        if (elapsed.elapsed() > self->limits.request_timeout_ms)
            co_return err(ErrorCode::Busy);
        auto m = self->mounted(identity, d.id);
        if (!m)
            co_return err(ErrorCode::Corruption);
        auto assignment =
            std::find_if(self->config.load()->assignments.cbegin(), self->config.load()->assignments.cend(),
                         [&](const StorageAssignment &a) { return a.id == d.assignment; });
        // Keep withdrawn definitions disabled; they cannot silently acquire a new assignment.
        if (assignment == self->config.load()->assignments.cend())
            continue;
        auto parts = d.path.sliced(1).split('/');
        QString prefix;
        NodeId parent = root(identity);
        for (int i = 0; i < parts.size() - 1; ++i) {
            if (self->stopping)
                co_return err(ErrorCode::ShuttingDown);
            if (elapsed.elapsed() > self->limits.request_timeout_ms)
                co_return err(ErrorCode::Busy);
            prefix += '/' + parts[i];
            if (nodes.contains(prefix)) {
                parent = nodes.value(prefix);
                continue;
            }
            bool recurse = true;
            if (request.permissions) {
                auto source_parent = prefix.section('/', 0, -2);
                if (source_parent.isEmpty())
                    source_parent = "/";
                recurse = request.permissions->resolve(request.scope, request.groups, source_parent,
                                                       Operation::Recurse) == Decision::Allow &&
                          request.permissions->resolve(request.scope, request.groups, prefix, Operation::List) ==
                              Decision::Allow;
            }
            if (recurse && parent != root(identity) && !topology->navigation.contains(parent)) {
                auto target = self->decode(identity, parent);
                if (target) {
                    auto backend = self->config.load()->backends.value(target.value().namespace_id).manager;
                    auto child = co_await logicalChild(backend, target.value().namespace_id, target.value().source,
                                                       parts[i], self->limits.max_depth);
                    if (child && child.value().type == EntryType::Directory) {
                        auto child_target = target.value();
                        child_target.source = child.value().id;
                        parent = self->encode(child_target);
                        nodes.insert(prefix, parent);
                        continue;
                    }
                    if (!child && child.error().code != ErrorCode::MissingEntry &&
                        child.error().code != ErrorCode::PermissionDenied)
                        co_return child.error();
                }
            }
            auto synthetic_id = self->config.load()->navigation_ids.value(uuid(identity) + ":" + prefix);
            if (synthetic_id.isNull())
                co_return err(ErrorCode::Corruption);
            auto bytes = synthetic_id.toRfc4122();
            NodeInfo node;
            node.id = QByteArray(1, char(0)) + bytes;
            node.parent = parent;
            node.name = parts[i];
            node.revision = revision(node);
            topology->navigation.insert(node.id, node);
            topology->navigation_paths.insert(node.id, prefix);
            parent = node.id;
            nodes.insert(prefix, parent);
        }
        m->mount.parent = parent;
        m->mount.name = parts.last();
        topology->mounts.append(*m);
        topology->paths.insert(d.id, d.path);
        EffectiveMount descriptor{m->mount, d, {}};
        for (const auto &candidate : candidates)
            if (candidate.identity == identity && candidate.path == d.path && candidate.tier == d.tier &&
                candidate.assignment == d.assignment && candidate.directory == d.directory &&
                (candidate.tier == Tier::Identity ||
                 (candidate.tier == Tier::Group ? request.groups.contains(candidate.principal)
                                                : candidate.principal == request.scope)))
                descriptor.contributors.append(candidate);
        topology->descriptors.append(descriptor);
        nodes.insert(d.path, self->encode({identity, d.id, m->mount.namespace_id, d.directory}));
    }
    if (self->stopping)
        co_return err(ErrorCode::ShuttingDown);
    if (generation != self->topology_generation->load() || !request.current())
        co_return err(ErrorCode::StaleCursor);
    // Conflicts are scoped to the effective recipient; repeated compositions coalesce.
    auto prefix = QByteArray("overlay:") + identity.toRfc4122();
    QSet<QByteArray> current;
    for (const auto &path : topology->conflicts) {
        auto key = prefix + QCryptographicHash::hash(path.toUtf8(), QCryptographicHash::Sha256);
        current.insert(key);
        if (!self->conflicts.contains(key)) {
            if (self->conflicts.size() >= int(self->limits.max_mounts))
                co_return err(ErrorCode::QuotaExceeded);
            self->conflicts.insert(key, {identity, path});
            emit conflictDetected(identity, key, path);
        }
    }

    request.effective = topology;
    co_return request;
}

namespace {
QCoro::Task<Result<bool>> containsMount(std::shared_ptr<Vfs::Impl> self, Target directory, const Request &request) {
    auto path = co_await self->virtualPath(directory.identity, self->encode(directory), request);
    if (!path.isEmpty())
        for (const auto &definition : self->config.load()->definitions)
            if (definition.identity == directory.identity && definition.path != path &&
                pathContains(path, definition.path))
                co_return true;
    for (const auto &item : self->viewMounts(request)) {
        if (item.identity != directory.identity || item.mount.parent == self->root(directory.identity))
            continue;
        auto parent = self->decode(directory.identity, item.mount.parent);
        if (!parent)
            continue;
        if (parent.value().namespace_id != directory.namespace_id)
            continue;
        auto backend = self->config.load()->backends.value(directory.namespace_id).manager;
        auto source = parent.value().source;
        for (quint32 depth = 0; depth < self->limits.max_depth; ++depth) {
            if (source == directory.source)
                co_return true;
            auto info = co_await backend->stat(directory.namespace_id, source);
            if (!info)
                co_return info.error();
            if (!info.value().parent)
                break;
            source = *info.value().parent;
        }
    }
    co_return false;
}
class Reader final : public IFileReader {
  public:
    using State = ReadHandle;
    Reader(std::unique_ptr<IFileReader> reader, std::function<QCoro::Task<Result<void>>()> check)
        : state(std::make_shared<State>()) {
        state->reader = std::move(reader);
        state->check = std::move(check);
    }
    std::function<bool()> accessGuard() const override {
        return state->current ? state->current : [] { return false; };
    }
    static QCoro::Task<Result<void>> closeState(std::shared_ptr<State> state) {
        if (state->busy)
            co_return err(ErrorCode::Busy);
        if (!state->reader)
            co_return Result<void>{};
        auto reader = std::move(state->reader);
        co_return co_await reader->close();
    }
    QCoro::Task<Result<QByteArray>> readAt(quint64 offset, qsizetype amount, CancellationToken cancel) override {
        auto held = state;
        if (!held->reader)
            co_return err(ErrorCode::MissingEntry);
        if (held->busy)
            co_return err(ErrorCode::Busy);
        held->busy = true;
        auto guard = qScopeGuard([held] { held->busy = false; });
        if (held->current && !held->current()) {
            held->busy = false;
            co_await closeState(held);
            co_return err(ErrorCode::MissingEntry);
        }
        auto before = co_await held->check();
        if (!before) {
            held->busy = false;
            co_await closeState(held);
            co_return before.error();
        }
        auto bytes = co_await held->reader->readAt(offset, amount, cancel);
        auto after = co_await held->check();
        if (!after) {
            held->busy = false;
            co_await closeState(held);
            co_return after.error();
        }
        co_return bytes;
    }
    QCoro::Task<Result<void>> close() override { co_return co_await closeState(state); }
    QCoro::Task<Result<void>> invalidate() override {
        auto held = state;
        if (!held->reader)
            co_return err(ErrorCode::MissingEntry);
        if (held->busy)
            co_return err(ErrorCode::Busy);
        held->busy = true;
        auto guard = qScopeGuard([held] { held->busy = false; });
        co_return co_await held->reader->invalidate();
    }
    std::shared_ptr<State> state;
};
class Writer final : public IFileWriter {
  public:
    Writer(std::unique_ptr<IFileWriter> writer, std::shared_ptr<Vfs::Impl> owner, Target target, Request request,
           Operation operation = Operation::Write)
        : writer_(std::move(writer)), owner_(std::move(owner)), target_(target), request_(std::move(request)),
          operation_(operation) {}
    QCoro::Task<Result<qsizetype>> writeAt(quint64 offset, QByteArray bytes, CancellationToken cancel) override {
        auto check = co_await access();
        if (!check)
            co_return check.error();
        auto result = co_await writer_->writeAt(offset, std::move(bytes), cancel);
        auto after = co_await access();
        if (!after)
            co_return after.error();
        co_return result;
    }
    QCoro::Task<Result<void>> setFinalSize(quint64 size) override {
        auto check = co_await access();
        if (!check)
            co_return check.error();
        co_return co_await writer_->setFinalSize(size);
    }
    QCoro::Task<Result<void>> markVerified(ByteRange range) override {
        auto check = co_await access();
        if (!check)
            co_return check.error();
        co_return co_await writer_->markVerified(range);
    }
    QCoro::Task<Result<void>> checkpoint(CancellationToken cancel) override {
        auto check = co_await access();
        if (!check)
            co_return check.error();
        co_return co_await writer_->checkpoint(cancel);
    }
    QCoro::Task<Result<void>> suspend(CancellationToken cancel) override {
        auto check = co_await access();
        if (!check)
            co_return check.error();
        co_return co_await writer_->suspend(cancel);
    }
    QCoro::Task<Result<CommitResult>> commit(CancellationToken cancel) override {
        auto check = co_await access();
        if (!check)
            co_return check.error();
        if (owner_->changing)
            co_return err(ErrorCode::Busy);
        auto held = permit(owner_, target_, operation_, request_);
        if (!held)
            co_return held.error();
        co_return co_await writer_->commit(cancel);
    }
    QCoro::Task<Result<void>> abort(CancellationToken cancel) override { co_return co_await writer_->abort(cancel); }

  private:
    QCoro::Task<Result<void>> access() {
        auto check = co_await traverse(owner_, target_, request_, operation_);
        if (!check) {
            co_await writer_->abort();
            co_return check.error();
        }
        co_return Result<void>{};
    }
    std::unique_ptr<IFileWriter> writer_;
    std::shared_ptr<Vfs::Impl> owner_;
    Target target_;
    Request request_;
    Operation operation_;
};
} // namespace
QCoro::Task<Result<Mount>> Vfs::addMount(QUuid identity, Mount mount, Request request) {
    auto self = impl_;
    auto available = self->available();
    if (!available)
        co_return available.error();
    if (self->changing)
        co_return err(ErrorCode::Busy);
    self->changing = true;
    auto changing = qScopeGuard([self] { self->changing = false; });
    if (identity.isNull() || !mount.id.isNull() || !validName(mount.name) || mount.source_directory.isNull() ||
        json(mount.presentation).size() > 4096)
        co_return err(ErrorCode::InvalidArgument);
    if (mount.parent.isEmpty())
        mount.parent = root(identity);
    if (self->config.load()->mounts.size() >= int(self->limits.max_mounts))
        co_return err(ErrorCode::QuotaExceeded);
    auto parent = co_await self->inspect(identity, mount.parent, request);
    if (!parent)
        co_return parent.error();
    if (parent.value().type != EntryType::Directory)
        co_return err(ErrorCode::InvalidArgument);
    auto backend = self->config.load()->backends.value(mount.namespace_id).manager;
    if (!backend)
        co_return err(ErrorCode::StorageUnavailable);
    auto registration = self->config.load()->backends.value(mount.namespace_id);
    auto namespace_info = co_await backend->namespaceInfo(mount.namespace_id);
    if (!namespace_info)
        co_return namespace_info.error();
    if ((!registration.owner_identity.isNull() && registration.owner_identity != identity) ||
        (namespace_info.value().capabilities.testFlag(Capability::Write) && registration.owner_identity != identity))
        co_return err(ErrorCode::PermissionDenied);
    auto source = co_await backend->stat(mount.namespace_id, mount.source_directory);
    if (!source)
        co_return source.error();
    if (source.value().type != EntryType::Directory)
        co_return err(ErrorCode::InvalidArgument);
    bool assigned = false;
    for (const auto &a : QVector<StorageAssignment>(self->config.load()->assignments)) {
        if (a.identity != identity || (!mount.assignment.isNull() && mount.assignment != a.id) ||
            (mount.assignment.isNull() && a.allow_external_files))
            continue;
        for (const auto &resource : self->config.load()->resources) {
            if (resource.id != a.resource || resource.namespace_id != mount.namespace_id)
                continue;
            auto within = co_await sourceWithin(backend, mount.namespace_id, mount.source_directory, a.directory,
                                                self->limits.max_depth);
            if (within) {
                mount.assignment = a.id;
                assigned = true;
                break;
            }
        }
        if (assigned)
            break;
    }
    if (!assigned)
        co_return err(ErrorCode::PermissionDenied);
    // Mount administration is explicitly authorized on the virtual parent.
    Target admin{identity, {}, {}, {}};
    if (mount.parent != root(identity)) {
        auto target = self->decode(identity, mount.parent);
        if (!target)
            co_return target.error();
        admin = target.value();
    }
    auto held = permit(self, admin, Operation::ConfigureMounts, request);
    if (!held)
        co_return held.error();
    for (const auto &item : QVector<Mounted>(self->config.load()->mounts))
        if (item.identity == identity && item.mount.parent == mount.parent && item.mount.name == mount.name)
            co_return err(ErrorCode::Conflict);
    auto collision = co_await self->conflict(identity, mount, true);
    if (!collision)
        co_return collision.error();
    if (collision.value())
        co_return err(ErrorCode::Conflict);
    mount.id = QUuid::createUuid();
    for (const auto &existing : self->config.load()->mounts)
        if (existing.mount.id == mount.id)
            co_return err(ErrorCode::Conflict);
    auto next = std::make_shared<Configuration>(*self->config.load());
    if (!next->roots.contains(identity)) {
        if (next->roots.size() >= int(self->limits.max_mounts))
            co_return err(ErrorCode::QuotaExceeded);
        next->roots.insert(identity, QUuid::fromRfc4122(self->root(identity).sliced(1)));
    }
    next->mounts.append({identity, mount});
    auto result = co_await self->publish(next);
    if (!result)
        co_return result.error();
    LFLOG_DEBUG << "VFS mount created";
    co_return mount;
}
QCoro::Task<Result<void>> Vfs::changeMount(QUuid identity, QUuid id, NodeId parent, QString name, Request request) {
    auto self = impl_;
    auto available = self->available();
    if (!available)
        co_return available.error();
    for (const auto &d : self->config.load()->definitions)
        if (d.id == id)
            co_return err(ErrorCode::UnsupportedCapability);
    if (self->changing)
        co_return err(ErrorCode::Busy);
    self->changing = true;
    auto changing = qScopeGuard([self] { self->changing = false; });
    auto existing = self->mounted(identity, id);
    if (!existing)
        co_return err(ErrorCode::MissingEntry);
    if (existing->life->commits)
        co_return err(ErrorCode::Busy);
    if (!validName(name))
        co_return err(ErrorCode::InvalidName);
    auto target = Target{identity, id, existing->mount.namespace_id, existing->mount.source_directory};
    auto held = permit(self, target, Operation::ConfigureMounts, request);
    if (!held)
        co_return held.error();
    auto destination = co_await self->inspect(identity, parent, request);
    if (!destination)
        co_return destination.error();
    if (destination.value().type != EntryType::Directory)
        co_return err(ErrorCode::InvalidArgument);
    Target destination_target{identity, {}, {}, {}};
    if (parent != root(identity)) {
        auto parsed = self->decode(identity, parent);
        if (!parsed)
            co_return parsed.error();
        destination_target = parsed.value();
    }
    auto destination_permit = permit(self, destination_target, Operation::Create, request);
    if (!destination_permit)
        co_return destination_permit.error();
    auto dependencies = self->dependencies(identity, parent);
    if (!dependencies)
        co_return dependencies.error();
    auto pending = dependencies.value();
    QSet<QUuid> seen;
    for (quint32 depth = 0; !pending.isEmpty(); ++depth) {
        if (depth >= self->limits.max_mounts)
            co_return err(ErrorCode::InvalidArgument);
        auto ancestor = *pending.cbegin();
        pending.remove(ancestor);
        if (ancestor == id)
            co_return err(ErrorCode::Conflict);
        if (seen.contains(ancestor))
            continue;
        seen.insert(ancestor);
        auto item = self->mounted(identity, ancestor);
        if (!item)
            co_return err(ErrorCode::MissingEntry);
        auto next = self->dependencies(identity, item->mount.parent);
        if (!next)
            co_return next.error();
        pending.unite(next.value());
    }
    for (const auto &item : QVector<Mounted>(self->config.load()->mounts))
        if (item.identity == identity && item.mount.id != id && item.mount.parent == parent && item.mount.name == name)
            co_return err(ErrorCode::Conflict);
    auto changed_mount = existing->mount;
    changed_mount.parent = parent;
    changed_mount.name = name;
    auto collision = co_await self->conflict(identity, changed_mount, true);
    if (!collision)
        co_return collision.error();
    if (collision.value())
        co_return err(ErrorCode::Conflict);
    auto next = std::make_shared<Configuration>(*self->config.load());
    for (auto &item : next->mounts)
        if (item.mount.id == id)
            item.mount = changed_mount;
    auto published = co_await self->publish(next);
    if (!published)
        co_return published.error();
    existing->life->conflicted = false;
    self->collision_cache.remove(id);
    if (self->conflicts.remove(id.toRfc4122()))
        emit conflictResolved(identity, id.toRfc4122());
    co_return Result<void>{};
}
QCoro::Task<Result<void>> Vfs::removeMount(QUuid identity, QUuid id, Request request) {
    auto self = impl_;
    auto available = self->available();
    if (!available)
        co_return available.error();
    for (const auto &d : self->config.load()->definitions)
        if (d.id == id)
            co_return err(ErrorCode::UnsupportedCapability);
    if (self->changing)
        co_return err(ErrorCode::Busy);
    self->changing = true;
    auto changing = qScopeGuard([self] { self->changing = false; });
    auto existing = self->mounted(identity, id);
    if (!existing)
        co_return err(ErrorCode::MissingEntry);
    if (existing->life->commits)
        co_return err(ErrorCode::Busy);
    auto held = permit(self, {identity, id, existing->mount.namespace_id, existing->mount.source_directory},
                       Operation::ConfigureMounts, request);
    if (!held)
        co_return held.error();
    for (const auto &child : QVector<Mounted>(self->config.load()->mounts)) {
        if (child.identity != identity || child.mount.parent == root(identity))
            continue;
        auto dependencies = self->dependencies(identity, child.mount.parent);
        if (!dependencies)
            co_return dependencies.error();
        if (dependencies.value().contains(id))
            co_return err(ErrorCode::DirectoryNotEmpty);
    }
    auto next = std::make_shared<Configuration>(*self->config.load());
    next->mounts.removeIf([id](const Mounted &item) { return item.mount.id == id; });
    auto result = co_await self->publish(next);
    if (!result)
        co_return result.error();
    existing->life->active = false;
    self->collision_cache.remove(id);
    LFLOG_DEBUG << "VFS mount removed and handles revoked";
    if (self->conflicts.remove(id.toRfc4122()))
        emit conflictResolved(identity, id.toRfc4122());
    co_return Result<void>{};
}
QCoro::Task<Result<PreparedRead>> Vfs::prepareRead(QUuid identity, NodeId node, ReadPreparationOptions options,
                                                   Request request) {
    auto self = impl_;
    auto available = self->available();
    if (!available)
        co_return available.error();
    if (self->active_requests >= self->limits.max_requests)
        co_return err(ErrorCode::QueueFull);
    Admission admission(self);
    OperationCancellation cancellation(self, options.cancel);
    self->readers.removeIf([](auto weak) {
        auto reader = weak.lock();
        return !reader || !reader->reader;
    });
    if (self->readers.size() >= int(self->limits.max_readers))
        co_return err(ErrorCode::QuotaExceeded);
    std::optional<Impl::Lease> resume;
    if (!options.resume_lease.isEmpty()) {
        if (options.expected_sha256.size() != 32)
            co_return err(ErrorCode::InvalidArgument);
        auto decoded = self->unwrapLease(identity, node, request, options.resume_lease);
        if (!decoded)
            co_return decoded.error();
        resume = decoded.value();
        if (options.expected_generation && *options.expected_generation != resume->revision)
            co_return err(ErrorCode::ExternalModification);
        options.resume_lease = resume->backend;
        options.expected_generation = resume->generation;
    }
    auto info = co_await self->inspect(identity, node, request);
    if (!info)
        co_return info.error();
    if (info.value().type != EntryType::File)
        co_return err(ErrorCode::InvalidArgument);
    if (!resume && !isReadable(info.value().status))
        co_return err(ErrorCode::Busy);
    if (!resume && options.expected_generation && *options.expected_generation != info.value().revision)
        co_return err(ErrorCode::ExternalModification);
    auto target = self->decode(identity, node);
    if (!target)
        co_return target.error();
    auto checked = co_await traverse(self, target.value(), request, Operation::Read);
    if (!checked)
        co_return checked.error();
    auto backend = self->config.load()->backends.value(target.value().namespace_id).manager;
    auto access = co_await self->routedAccess(identity, node, request, Operation::Read);
    if (!access)
        co_return access.error();
    if (!resume && options.expected_generation) {
        auto selected = co_await backend->stat(target.value().namespace_id, target.value().source, access.value());
        if (!selected)
            co_return selected.error();
        auto visible = self->node(target.value(), selected.value(), request);
        visible.id = node;
        visible.parent = info.value().parent;
        visible.name = info.value().name;
        visible.capabilities = info.value().capabilities;
        visible.symlinks = info.value().symlinks;
        if (revision(visible) != *options.expected_generation)
            co_return err(ErrorCode::ExternalModification);
        options.expected_generation = selected.value().generation;
    }
    auto prepared =
        co_await backend->prepareRead(target.value().namespace_id, target.value().source, options, access.value());
    if (!prepared)
        co_return prepared.error();
    auto post = co_await self->verifyAliases(target.value(), request, Operation::Read, access.value());
    if (!post) {
        co_await prepared.value().reader->close();
        co_return post.error();
    }
    auto output_info = self->node(target.value(), prepared.value().info, request);
    output_info.id = node;
    output_info.name = info.value().name;
    output_info.symlinks = info.value().symlinks;
    output_info.parent = info.value().parent;
    output_info.capabilities = info.value().capabilities;
    output_info.revision = revision(output_info);
    auto check = [self, target = target.value(), identity, node, request]() -> QCoro::Task<Result<void>> {
        auto routed = co_await self->routedAccess(identity, node, request, Operation::Read);
        if (!routed)
            co_return routed.error();
        co_return co_await self->verifyAliases(target, request, Operation::Read, routed.value());
    };
    if (self->readers.size() >= int(self->limits.max_readers)) {
        co_await prepared.value().reader->close();
        co_return err(ErrorCode::QuotaExceeded);
    }
    auto guarded = std::make_unique<Reader>(std::move(prepared.value().reader), check);
    const auto expires = now() + self->limits.reader_lifetime_ms;
    auto predicate = access.value().filesystem_check;
    auto backend_guard = guarded->state->reader->accessGuard();
    guarded->state->current = [self, identity, node, request, expires, predicate, backend_guard,
                               target = target.value()] {
        if (self->stopping || now() > expires || !request.current() || !self->decode(identity, node) ||
            !backend_guard() || (predicate && !predicate({})))
            return false;
        auto allowed = self->allowed(target, Operation::Read, request);
        return allowed && allowed.value();
    };
    self->readers.append(guarded->state);
    QByteArray lease;
    if (!prepared.value().resume_lease.isEmpty()) {
        lease =
            self->wrapLease(identity, node, request,
                            {prepared.value().resume_lease, prepared.value().info.generation, output_info.revision});
        if (lease.isEmpty()) {
            co_await guarded->close();
            co_return err(ErrorCode::QuotaExceeded);
        }
    }
    auto current = guarded->state->current;
    PreparedRead result{output_info, std::move(guarded), prepared.value().consistency, lease, current};
    co_return result;
}
QCoro::Task<Result<NodeInfo>> Vfs::mkdir(QUuid identity, NodeId parent, QString name, Request request) {
    auto self = impl_;
    auto available = self->available();
    if (!available)
        co_return available.error();
    if (self->changing)
        co_return err(ErrorCode::Busy);
    self->changing = true;
    auto guard = qScopeGuard([self] { self->changing = false; });
    auto parent_info = co_await self->inspect(identity, parent, request);
    if (!parent_info)
        co_return parent_info.error();
    if (!parent_info.value().capabilities.testFlag(Capability::Write))
        co_return err(ErrorCode::UnsupportedCapability);
    auto target = self->decode(identity, parent);
    if (!target)
        co_return target.error();
    auto checked = co_await traverse(self, target.value(), request, Operation::Create);
    if (!checked)
        co_return checked.error();
    for (const auto &mounted : self->viewMounts(request))
        if (mounted.identity == identity && mounted.mount.parent == parent && mounted.mount.name == name)
            co_return err(ErrorCode::Conflict);
    auto held = permit(self, target.value(), Operation::Create, request);
    if (!held)
        co_return held.error();
    auto backend = self->config.load()->backends.value(target.value().namespace_id).manager;
    auto created = co_await backend->mkdir(target.value().namespace_id, target.value().source, name);
    if (!created)
        co_return created.error();
    auto info = co_await backend->stat(target.value().namespace_id, created.value());
    if (!info)
        co_return info.error();
    auto child = target.value();
    child.source = created.value();
    auto created_info = self->node(child, info.value(), request);
    created_info.capabilities = parent_info.value().capabilities;
    created_info.revision = revision(created_info);
    co_return created_info;
}
QCoro::Task<Result<PendingFile>> Vfs::createFile(QUuid identity, NodeId parent, CreateOptions options,
                                                 Request request) {
    auto self = impl_;
    auto available = self->available();
    if (!available)
        co_return available.error();
    if (self->changing)
        co_return err(ErrorCode::Busy);
    self->changing = true;
    auto guard = qScopeGuard([self] { self->changing = false; });
    auto parent_info = co_await self->inspect(identity, parent, request);
    if (!parent_info)
        co_return parent_info.error();
    if (!parent_info.value().capabilities.testFlag(Capability::Write))
        co_return err(ErrorCode::UnsupportedCapability);
    auto target = self->decode(identity, parent);
    if (!target)
        co_return target.error();
    auto checked = co_await traverse(self, target.value(), request, Operation::Create);
    if (!checked)
        co_return checked.error();
    for (const auto &mounted : self->viewMounts(request))
        if (mounted.identity == identity && mounted.mount.parent == parent &&
            mounted.mount.name == options.suggested_name)
            co_return err(ErrorCode::Conflict);
    auto held = permit(self, target.value(), Operation::Create, request);
    if (!held)
        co_return held.error();
    options.parent = target.value().source;
    auto backend = self->config.load()->backends.value(target.value().namespace_id).manager;
    auto pending = co_await backend->createFile(target.value().namespace_id, options);
    if (!pending)
        co_return pending.error();
    for (const auto &mounted : self->viewMounts(request))
        if (mounted.identity == identity && mounted.mount.parent == parent &&
            mounted.mount.name == pending.value().info.name) {
            co_await pending.value().writer->abort();
            co_return err(ErrorCode::Conflict);
        }
    pending.value().writer =
        std::make_unique<Writer>(std::move(pending.value().writer), self, target.value(), request, Operation::Create);
    co_return std::move(pending).value();
}
QCoro::Task<Result<PendingFile>> Vfs::replaceFile(QUuid identity, NodeId node, ContentConstraints content,
                                                  Request request) {
    auto self = impl_;
    auto available = self->available();
    if (!available)
        co_return available.error();
    if (self->changing)
        co_return err(ErrorCode::Busy);
    self->changing = true;
    auto guard = qScopeGuard([self] { self->changing = false; });
    auto node_info = co_await self->inspect(identity, node, request);
    if (!node_info)
        co_return node_info.error();
    if (!node_info.value().capabilities.testFlag(Capability::Write))
        co_return err(ErrorCode::UnsupportedCapability);
    auto target = self->decode(identity, node);
    if (!target)
        co_return target.error();
    auto checked = co_await traverse(self, target.value(), request, Operation::Write);
    if (!checked)
        co_return checked.error();
    auto held = permit(self, target.value(), Operation::Write, request);
    if (!held)
        co_return held.error();
    auto backend = self->config.load()->backends.value(target.value().namespace_id).manager;
    auto pending = co_await backend->replaceFile(target.value().namespace_id, target.value().source, content);
    if (!pending)
        co_return pending.error();
    pending.value().writer = std::make_unique<Writer>(std::move(pending.value().writer), self, target.value(), request);
    co_return std::move(pending).value();
}
QCoro::Task<Result<MutationResult>> Vfs::rename(QUuid identity, NodeId node, QString name, Request request) {
    auto info = co_await stat(identity, node, request);
    if (!info)
        co_return info.error();
    if (!info.value().capabilities.testFlag(Capability::Write))
        co_return err(ErrorCode::UnsupportedCapability);
    auto target = impl_->decode(identity, node);
    if (!target)
        co_return target.error();
    if (target.value().source == impl_->mounted(identity, target.value().mount)->mount.source_directory)
        co_return err(ErrorCode::UnsupportedCapability);
    auto nested = co_await containsMount(impl_, target.value(), request);
    if (!nested)
        co_return nested.error();
    if (nested.value())
        co_return err(ErrorCode::DirectoryNotEmpty);
    for (const auto &mounted : impl_->viewMounts(request))
        if (mounted.identity == identity && mounted.mount.parent == info.value().parent && mounted.mount.name == name)
            co_return err(ErrorCode::Conflict);
    if (impl_->changing)
        co_return err(ErrorCode::Busy);
    impl_->changing = true;
    auto guard = qScopeGuard([self = impl_] { self->changing = false; });
    auto held = permit(impl_, target.value(), Operation::Rename, request);
    if (!held)
        co_return held.error();
    co_return co_await impl_->config.load()
        ->backends.value(target.value().namespace_id)
        .manager->rename(target.value().namespace_id, target.value().source, name);
}
QCoro::Task<Result<MutationResult>> Vfs::move(QUuid identity, NodeId node, NodeId parent, Request request) {
    auto info = co_await stat(identity, node, request);
    if (!info)
        co_return info.error();
    if (!info.value().capabilities.testFlag(Capability::Write))
        co_return err(ErrorCode::UnsupportedCapability);
    auto destination_info = co_await stat(identity, parent, request);
    if (!destination_info)
        co_return destination_info.error();
    if (!destination_info.value().capabilities.testFlag(Capability::Write))
        co_return err(ErrorCode::UnsupportedCapability);
    auto target = impl_->decode(identity, node), destination = impl_->decode(identity, parent);
    if (!target)
        co_return target.error();
    if (!destination)
        co_return destination.error();
    if (target.value().mount != destination.value().mount)
        co_return err(ErrorCode::CrossNamespaceMove);
    if (target.value().source == impl_->mounted(identity, target.value().mount)->mount.source_directory)
        co_return err(ErrorCode::UnsupportedCapability);
    auto nested = co_await containsMount(impl_, target.value(), request);
    if (!nested)
        co_return nested.error();
    if (nested.value())
        co_return err(ErrorCode::DirectoryNotEmpty);
    for (const auto &mounted : impl_->viewMounts(request))
        if (mounted.identity == identity && mounted.mount.parent == parent && mounted.mount.name == info.value().name)
            co_return err(ErrorCode::Conflict);
    if (impl_->changing)
        co_return err(ErrorCode::Busy);
    impl_->changing = true;
    auto guard = qScopeGuard([self = impl_] { self->changing = false; });
    auto held = permit(impl_, target.value(), Operation::Move, request),
         destination_held = permit(impl_, destination.value(), Operation::Create, request);
    if (!held)
        co_return held.error();
    if (!destination_held)
        co_return destination_held.error();
    co_return co_await impl_->config.load()
        ->backends.value(target.value().namespace_id)
        .manager->move(target.value().namespace_id, target.value().source, destination.value().source);
}
QCoro::Task<Result<MutationResult>> Vfs::remove(QUuid identity, NodeId node, Request request) {
    auto info = co_await stat(identity, node, request);
    if (!info)
        co_return info.error();
    if (!info.value().capabilities.testFlag(Capability::Write))
        co_return err(ErrorCode::UnsupportedCapability);
    auto target = impl_->decode(identity, node);
    if (!target)
        co_return target.error();
    if (target.value().source == impl_->mounted(identity, target.value().mount)->mount.source_directory)
        co_return err(ErrorCode::UnsupportedCapability);
    auto nested = co_await containsMount(impl_, target.value(), request);
    if (!nested)
        co_return nested.error();
    if (nested.value())
        co_return err(ErrorCode::DirectoryNotEmpty);
    if (impl_->changing)
        co_return err(ErrorCode::Busy);
    impl_->changing = true;
    auto guard = qScopeGuard([self = impl_] { self->changing = false; });
    auto held = permit(impl_, target.value(), Operation::Remove, request);
    if (!held)
        co_return held.error();
    co_return co_await impl_->config.load()
        ->backends.value(target.value().namespace_id)
        .manager->remove(target.value().namespace_id, target.value().source);
}

QCoro::Task<Result<Subscription>> Vfs::subscribeDirectory(QUuid identity, NodeId node, ChangeCallback callback,
                                                          Request request) {
    auto self = impl_;
    auto available = self->available();
    if (!available)
        co_return available.error();
    if (self->active_requests >= self->limits.max_requests)
        co_return err(ErrorCode::QueueFull);
    Admission admission(self);
    self->subscriptions.removeIf([](const auto &weak) {
        auto state = weak.lock();
        return !state || !state->active;
    });
    if (self->subscriptions.size() >= self->limits.max_subscriptions)
        co_return err(ErrorCode::QuotaExceeded);
    if (!callback.context || !callback.callback || callback.context->thread() != QThread::currentThread())
        co_return err(ErrorCode::InvalidArgument);
    auto baseline = co_await self->buildView(identity, node, request);
    if (!baseline)
        co_return baseline.error();
    auto state = std::make_shared<Subscription::State>();
    state->identity = identity;
    state->directory = node;
    state->request = request;
    state->callback = callback;
    state->digest = self->views[baseline.value()].digest;
    auto erased = co_await self->eraseView(baseline.value());
    if (!erased)
        co_return erased.error();
    if (node != root(identity) && !(request.effective && request.effective->navigation.contains(node))) {
        auto target = self->decode(identity, node);
        if (!target)
            co_return target.error();
        auto backend = self->config.load()->backends.value(target.value().namespace_id).manager;
        auto weak = std::weak_ptr<Subscription::State>(state);
        auto source = co_await backend->subscribeDirectory(target.value().namespace_id, target.value().source,
                                                           {this,
                                                            [weak](DirectoryChange change) {
                                                                if (auto state = weak.lock()) {
                                                                    state->dirty = true;
                                                                    if (change.kind == DirectoryChangeKind::Invalidated)
                                                                        state->active = false;
                                                                }
                                                            }},
                                                           self->context(target.value(), request));
        if (!source)
            co_return source.error();
        state->source = std::move(source).value();
    }
    if (node != root(identity) && !(request.effective && request.effective->navigation.contains(node))) {
        QVector<NodeId> origins{node};
        QSet<QByteArray> pinned;
        while (!origins.isEmpty()) {
            auto current = origins.takeLast();
            auto target = self->decode(identity, current);
            if (!target)
                co_return target.error();
            if (current[0] == 2) {
                auto routing = self->parseRoute(current);
                if (!routing)
                    co_return routing.error();
                origins.append(routing.value().origin);
            }
            auto selections = std::make_shared<Selections>();
            auto backend = self->config.load()->backends.value(target.value().namespace_id).manager;
            auto projected = co_await backend->descendantAccess(target.value().namespace_id, target.value().source,
                                                                self->context(target.value(), request, selections));
            if (!projected)
                co_return projected.error();
            QVector<Target> destinations;
            {
                std::lock_guard guard(selections->mutex);
                destinations = selections->targets;
            }
            for (auto destination : destinations) {
                auto key = destination.namespace_id.toRfc4122() + destination.source.toRfc4122();
                if (pinned.contains(key))
                    continue;
                pinned.insert(key);
                if (pinned.size() > int(self->limits.max_depth))
                    co_return err(ErrorCode::QuotaExceeded);
                auto manager = self->config.load()->backends.value(destination.namespace_id).manager;
                auto weak = std::weak_ptr<Subscription::State>(state);
                auto subscription =
                    co_await manager->subscribeDirectory(destination.namespace_id, destination.source,
                                                         {this, [weak](DirectoryChange change) {
                                                              if (auto state = weak.lock()) {
                                                                  state->dirty = true;
                                                                  if (change.kind == DirectoryChangeKind::Invalidated)
                                                                      state->active = false;
                                                              }
                                                          }});
                if (!subscription)
                    co_return subscription.error();
                state->targets.push_back(std::move(subscription).value());
            }
        }
    }
    if (self->subscriptions.size() >= self->limits.max_subscriptions)
        co_return err(ErrorCode::QuotaExceeded);
    self->subscriptions.append(state);
    co_return Subscription(state);
}
struct CatalogExport::State {
    std::weak_ptr<Vfs::Impl> owner;
    std::shared_ptr<IFileManager> backend;
    NamespaceId ns;
    FileId file;
    QByteArray lease = QUuid::createUuid().toRfc4122();
    FileInfo info;
    Request request;
    QUuid identity;
    QVector<Target> mounts;
    QHash<QUuid, NodeId> mount_parents;
    QVector<std::function<bool(FilesystemIdentity)>> privacy_guards;
    QVector<NodeId> route_origins;
    qint64 expires = 0;
    std::atomic<qint64> touched{0};
    std::atomic_bool closed{false};
    bool cleaning = false, cleaned = false;
    bool requires_utf8 = false;
    QVector<std::weak_ptr<Reader::State>> readers;
    bool authorized() const noexcept {
        try {
            auto service = owner.lock();
            if (!service || closed || service->stopping || !request.current() || now() > expires ||
                now() - touched > service->limits.export_idle_ms)
                return false;
            for (const auto &predicate : privacy_guards)
                if (predicate && !predicate({}))
                    return false;
            for (const auto &origin : route_origins)
                if (!service->decode(identity, origin))
                    return false;
            for (const auto &target : mounts) {
                if (!target.mount.isNull()) {
                    auto current = service->mounted(target.identity, target.mount, request);
                    if (!current || current->mount.parent != mount_parents.value(target.mount))
                        return false;
                }
                auto approved = service->allowed(target, Operation::Export, request);
                if (!approved || !approved.value())
                    return false;
            }
            return true;
        } catch (...) {
            return false;
        }
    }
};
CatalogExport::CatalogExport(std::shared_ptr<State> state) : state_(std::move(state)) {}
CatalogExport::~CatalogExport() {
    if (state_)
        state_->closed = true;
}
quint64 CatalogExport::size() const { return state_->info.size; }
QByteArray CatalogExport::sha256() const { return state_->info.sha256; }
QUuid CatalogExport::identity() const { return state_->identity; }
QUuid CatalogExport::authorizationScope() const { return state_->request.scope; }
bool CatalogExport::isAuthorized() const { return state_->authorized(); }
bool CatalogExport::requiresUtf8() const { return state_->requires_utf8; }
QCoro::Task<Result<dsfm::PreparedRead>> CatalogExport::prepareRead(ReadPreparationOptions options) {
    auto state = state_;
    state->readers.removeIf([](auto weak) {
        auto reader = weak.lock();
        return !reader || !reader->reader;
    });
    auto owner = state->owner.lock();
    if (!owner || state->readers.size() >= int(owner->limits.max_readers))
        co_return err(ErrorCode::QuotaExceeded);
    if (!state->authorized())
        co_return err(ErrorCode::MissingEntry);
    if (options.expected_sha256.isEmpty())
        options.expected_sha256 = state->info.sha256;
    if (options.expected_sha256 != state->info.sha256 ||
        (!options.resume_lease.isEmpty() && options.resume_lease != state->lease))
        co_return err(ErrorCode::ExternalModification);
    options.resume_lease.clear();
    if (options.expected_generation && *options.expected_generation != state->info.generation)
        co_return err(ErrorCode::Conflict);
    options.expected_generation = state->info.generation;
    auto reader = co_await state->backend->prepareRead(state->ns, state->file, options);
    if (!reader)
        co_return reader.error();
    if (!state->authorized()) {
        co_await reader.value().reader->close();
        co_return err(ErrorCode::MissingEntry);
    }
    state->touched = now();
    auto check = [state]() -> QCoro::Task<Result<void>> {
        if (!state->authorized())
            co_return err(ErrorCode::MissingEntry);
        state->touched = now();
        co_return Result<void>{};
    };
    if (state->readers.size() >= int(owner->limits.max_readers)) {
        co_await reader.value().reader->close();
        co_return err(ErrorCode::QuotaExceeded);
    }
    auto guarded = std::make_unique<Reader>(std::move(reader.value().reader), check);
    guarded->state->current = [state] { return state->authorized(); };
    state->readers.append(guarded->state);
    reader.value().reader = std::move(guarded);
    reader.value().resume_lease = state->lease;
    co_return std::move(reader).value();
}
QCoro::Task<Result<void>> CatalogExport::close() {
    auto state = state_;
    state->closed = true;
    for (auto weak : state->readers)
        if (auto reader = weak.lock())
            co_await Reader::closeState(reader);
    if (state->cleaning || state->cleaned)
        co_return Result<void>{};
    state->cleaning = true;
    auto result = co_await state->backend->remove(state->ns, state->file);
    state->cleaning = false;
    if (!result && result.error().code != ErrorCode::MissingEntry) {
        if (result.error().code != ErrorCode::Busy)
            co_return result.error();
    } else
        state->cleaned = true;
    co_return Result<void>{};
}

Result<void> Vfs::setExportStore(std::shared_ptr<IFileManager> backend, NamespaceId ns, DirectoryId directory) {
    auto available = impl_->available();
    if (!available)
        return available.error();
    if (!backend || ns.isNull() || directory.isNull() || impl_->active_exports ||
        impl_->config.load()->backends.contains(ns))
        return err(ErrorCode::InvalidArgument);
    impl_->export_backend = std::move(backend);
    impl_->export_ns = ns;
    impl_->export_root = directory;
    return {};
}
QCoro::Task<Result<std::shared_ptr<CatalogExport>>> Vfs::exportCatalog(QUuid identity, NodeId node, bool recursive,
                                                                       Request request, CancellationToken cancel) {
    auto self = impl_;
    auto available = self->available();
    if (!available)
        co_return available.error();
    if (!self->export_backend)
        co_return err(ErrorCode::StorageUnavailable);
    if (self->active_exports >= self->limits.max_exports || self->active_requests >= self->limits.max_requests)
        co_return err(ErrorCode::QueueFull);
    Target starting{identity, {}, {}, {}};
    if (node != root(identity) && !(request.effective && request.effective->navigation.contains(node))) {
        auto parsed = self->decode(identity, node);
        if (!parsed)
            co_return parsed.error();
        starting = parsed.value();
    }
    auto approved = self->allowed(starting, Operation::Export, request);
    if (!approved || !approved.value())
        co_return err(ErrorCode::MissingEntry);
    ++self->active_exports;
    Admission admission(self);
    OperationCancellation cancellation(self, cancel);
    bool retained = false;
    auto release = qScopeGuard([&] {
        if (!retained)
            --self->active_exports;
    });
    auto info = co_await self->inspect(identity, node, request);
    if (!info)
        co_return info.error();
    if (info.value().type != EntryType::Directory)
        co_return err(ErrorCode::InvalidArgument);
    auto export_info = co_await self->export_backend->namespaceInfo(self->export_ns);
    if (!export_info)
        co_return export_info.error();
    if (!export_info.value().config.limits.max_io_bytes)
        co_return err(ErrorCode::InvalidArgument);
    const auto io_chunk = std::min<quint32>(65536, export_info.value().config.limits.max_io_bytes);
    auto staged = co_await self->export_backend->createFile(
        self->export_ns, {self->export_root,
                          QStringLiteral("catalog-%1.jsonl").arg(uuid(QUuid::createUuid())),
                          ConflictPolicy::Fail,
                          {}});
    if (!staged)
        co_return staged.error();
    auto writer = std::move(staged.value().writer);
    QElapsedTimer elapsed;
    elapsed.start();
    quint64 offset = 0, entries = 0;
    QByteArray buffer;
    bool requires_utf8 = false;
    auto append = [&](QByteArray bytes) -> QCoro::Task<Result<void>> {
        if (cancel.cancelled())
            co_return err(ErrorCode::Cancelled);
        if (!request.current())
            co_return err(ErrorCode::MissingEntry);
        if (elapsed.elapsed() > self->limits.export_timeout_ms)
            co_return err(ErrorCode::Busy);
        if (bytes.size() > self->limits.max_record_bytes ||
            offset + quint64(buffer.size()) + quint64(bytes.size()) > self->limits.max_export_bytes)
            co_return err(ErrorCode::QuotaExceeded);
        for (auto byte : bytes)
            if (quint8(byte) >= 128)
                requires_utf8 = true;
        buffer.append(bytes);
        while (buffer.size() >= io_chunk) {
            auto part = buffer.first(io_chunk);
            auto written = co_await writer->writeAt(offset, part, cancel);
            if (!written)
                co_return written.error();
            if (written.value() != part.size())
                co_return err(ErrorCode::IoError);
            auto verified = co_await writer->markVerified({offset, quint64(part.size())});
            if (!verified)
                co_return verified.error();
            offset += quint64(part.size());
            buffer.remove(0, part.size());
        }
        co_return Result<void>{};
    };
    auto header = co_await append(json({{"format", "darkspeak-vfs-catalog"},
                                        {"version", 1},
                                        {"recursive", recursive},
                                        {"root", QString::fromLatin1(node.toHex())},
                                        {"consistency", "per-directory"}}) +
                                  '\n');
    if (!header) {
        co_await writer->abort();
        co_return header.error();
    }
    auto export_root_info = info.value();
    export_root_info.parent.clear();
    export_root_info.revision = revision(export_root_info);
    auto root_record = co_await append(json(record(export_root_info)) + '\n');
    if (!root_record) {
        co_await writer->abort();
        co_return root_record.error();
    }
    struct Frame {
        NodeId directory;
        QByteArray view;
        quint64 offset = 0;
        quint32 depth = 0;
        Target target;
    };
    auto first = co_await self->buildView(identity, node, request, cancel);
    if (!first) {
        co_await writer->abort();
        co_return first.error();
    }
    QVector<Frame> stack{{node, first.value(), 0, 0, starting}};
    QVector<Target> mounts{starting};
    QVector<std::function<bool(FilesystemIdentity)>> privacy_guards;
    QVector<NodeId> route_origins;
    QSet<QByteArray> captured_policies;
    auto capture_policy = [&, self](NodeId id) -> QCoro::Task<Result<void>> {
        if (id == self->root(identity) || (request.effective && request.effective->navigation.contains(id)))
            co_return Result<void>{};
        auto target = self->decode(identity, id);
        if (!target)
            co_return target.error();
        auto references = self->dependencies(identity, id);
        if (!references)
            co_return references.error();
        for (auto mount_id : references.value()) {
            bool known = false;
            for (const auto &mount : mounts)
                if (mount.mount == mount_id)
                    known = true;
            if (!known) {
                auto mount = self->mounted(identity, mount_id, request);
                if (!mount)
                    co_return err(ErrorCode::MissingEntry);
                mounts.append({identity, mount_id, mount->mount.namespace_id, mount->mount.source_directory});
            }
        }
        auto access = co_await self->routedAccess(identity, id, request, Operation::Export);
        if (!access)
            co_return access.error();
        auto backend = self->config.load()->backends.value(target.value().namespace_id).manager;
        auto projected =
            co_await backend->descendantAccess(target.value().namespace_id, target.value().source, access.value());
        if (!projected)
            co_return projected.error();
        const auto stamp = target.value().namespace_id.toRfc4122() + projected.value().filesystem_generation;
        if (!captured_policies.contains(stamp)) {
            if (captured_policies.size() >= int(self->limits.max_mounts))
                co_return err(ErrorCode::QuotaExceeded);
            captured_policies.insert(stamp);
            privacy_guards.append(projected.value().filesystem_check);
        }
        if (id[0] == 2) {
            auto route = self->parseRoute(id);
            if (!route)
                co_return route.error();
            if (!route_origins.contains(route.value().origin)) {
                if (route_origins.size() >= int(self->limits.max_mounts))
                    co_return err(ErrorCode::QuotaExceeded);
                route_origins.append(route.value().origin);
            }
        }
        co_return Result<void>{};
    };
    auto captured = co_await capture_policy(node);
    if (!captured) {
        co_await self->eraseView(first.value());
        co_await writer->abort();
        co_return captured.error();
    }
    auto fail = [&](Error error) -> QCoro::Task<Result<std::shared_ptr<CatalogExport>>> {
        for (const auto &frame : stack)
            co_await self->eraseView(frame.view);
        co_await writer->abort();
        co_return error;
    };
    while (!stack.isEmpty()) {
        auto rows = co_await self->viewRows(stack.last().view, stack.last().offset, 1);
        if (!rows)
            co_return co_await fail(rows.error());
        if (rows.value().isEmpty()) {
            auto erased = co_await self->eraseView(stack.last().view);
            if (!erased)
                co_return co_await fail(erased.error());
            stack.removeLast();
            continue;
        }
        ++stack.last().offset;
        auto item = rows.value().first();
        auto current = co_await self->inspect(identity, item.id, request);
        if (!current)
            co_return co_await fail(current.error());
        if (current.value().revision != item.revision)
            item.status = EntryStatus::Updating;
        item.revision = revision(item);
        if (++entries > self->limits.max_export_entries)
            co_return co_await fail(err(ErrorCode::QuotaExceeded));
        if (item.type == EntryType::Directory) {
            auto captured = co_await capture_policy(item.id);
            if (!captured)
                co_return co_await fail(captured.error());
        }
        auto added = co_await append(json(record(item)) + '\n');
        if (!added)
            co_return co_await fail(added.error());
        if (recursive && item.type == EntryType::Directory) {
            if (stack.last().depth + 1 >= self->limits.max_depth)
                co_return co_await fail(err(ErrorCode::QuotaExceeded));
            Result<Target> target =
                request.effective && request.effective->navigation.contains(item.id)
                    ? Result<Target>{Target{identity, {}, {}, QUuid::fromRfc4122(item.id.sliced(1))}}
                    : self->decode(identity, item.id);
            if (!target)
                co_return co_await fail(target.error());
            for (const auto &frame : stack)
                if (frame.target.mount == target.value().mount &&
                    frame.target.namespace_id == target.value().namespace_id &&
                    frame.target.source == target.value().source)
                    co_return co_await fail(err(ErrorCode::Conflict));
            auto grant = self->allowed(target.value(), Operation::Export, request);
            if (!grant || !grant.value())
                co_return co_await fail(err(ErrorCode::MissingEntry));
            bool known = false;
            for (const auto &mount : mounts)
                if (mount.mount == target.value().mount)
                    known = true;
            if (!known && !target.value().mount.isNull()) {
                auto root_target = target.value();
                root_target.source = self->mounted(identity, root_target.mount, request)->mount.source_directory;
                mounts.append(root_target);
            }
            auto captured = co_await capture_policy(item.id);
            if (!captured)
                co_return co_await fail(captured.error());
            auto child = co_await self->buildView(identity, item.id, request, cancel);
            if (!child)
                co_return co_await fail(child.error());
            stack.append({item.id, child.value(), 0, stack.last().depth + 1, target.value()});
        }
    }
    if (!buffer.isEmpty()) {
        auto written = co_await writer->writeAt(offset, buffer, cancel);
        if (!written || written.value() != buffer.size()) {
            co_await writer->abort();
            co_return written ? err(ErrorCode::IoError) : written.error();
        }
        auto verified = co_await writer->markVerified({offset, quint64(buffer.size())});
        if (!verified) {
            co_await writer->abort();
            co_return verified.error();
        }
        offset += quint64(buffer.size());
        buffer.clear();
    }
    if (!request.current() || cancel.cancelled()) {
        co_await writer->abort();
        co_return err(cancel.cancelled() ? ErrorCode::Cancelled : ErrorCode::MissingEntry);
    }
    auto sized = co_await writer->setFinalSize(offset);
    if (!sized) {
        co_await writer->abort();
        co_return sized.error();
    }
    auto committed = co_await writer->commit(cancel);
    if (!committed) {
        co_await writer->abort();
        co_return committed.error();
    }
    auto selected = co_await self->export_backend->stat(self->export_ns, staged.value().info.file_id);
    if (!selected || selected.value().sha256.size() != 32) {
        co_await self->export_backend->remove(self->export_ns, staged.value().info.file_id);
        co_return err(ErrorCode::IntegrityError);
    }
    auto state = std::make_shared<CatalogExport::State>();
    state->owner = self;
    state->backend = self->export_backend;
    state->ns = self->export_ns;
    state->file = selected.value().id;
    state->info = selected.value();
    state->requires_utf8 = requires_utf8;
    state->identity = identity;
    state->request = request;
    state->mounts = mounts;
    for (const auto &target : mounts)
        if (!target.mount.isNull()) {
            auto current = self->mounted(target.identity, target.mount, request);
            if (!current) {
                co_await self->export_backend->remove(state->ns, state->file);
                co_return err(ErrorCode::MissingEntry);
            }
            state->mount_parents.insert(target.mount, current->mount.parent);
        }
    state->privacy_guards = std::move(privacy_guards);
    state->route_origins = std::move(route_origins);
    state->expires = now() + self->limits.export_lifetime_ms;
    state->touched = now();
    // Retain cleanup state until the timer reclaims the managed export.
    if (!state->authorized()) {
        co_await self->export_backend->remove(state->ns, state->file);
        co_return err(ErrorCode::MissingEntry);
    }
    LFLOG_DEBUG << "VFS catalog export finalized; entries=" << entries << ", bytes=" << offset;
    self->exports.append(state);
    retained = true;
    co_return std::shared_ptr<CatalogExport>(new CatalogExport(state));
}
QCoro::Task<Result<void>> Vfs::refresh() {
    auto self = impl_;
    auto available = self->available();
    if (!available)
        co_return available.error();
    if (self->refreshing)
        co_return err(ErrorCode::Busy);
    self->refreshing = true;
    auto refreshing = qScopeGuard([self] { self->refreshing = false; });
    for (auto weak : QVector<std::weak_ptr<ReadHandle>>(self->readers))
        if (auto reader = weak.lock(); reader && reader->current && !reader->current())
            co_await Reader::closeState(reader);
    self->readers.removeIf([](auto weak) {
        auto reader = weak.lock();
        return !reader || !reader->reader;
    });
    for (auto state : QVector<std::shared_ptr<CatalogExport::State>>(self->exports)) {
        if (state->authorized())
            continue;
        state->closed = true;
        for (auto weak : state->readers)
            if (auto reader = weak.lock())
                co_await Reader::closeState(reader);
        if (state->cleaning || state->cleaned)
            continue;
        state->cleaning = true;
        auto removed = co_await state->backend->remove(state->ns, state->file);
        state->cleaning = false;
        if (removed || removed.error().code == ErrorCode::MissingEntry)
            state->cleaned = true;
    }
    self->exports.removeIf([self](auto state) {
        if (!state->cleaned)
            return false;
        --self->active_exports;
        return true;
    });
    for (auto id : self->enumerations.keys())
        if (self->enumerations[id]->expires < now() || !self->enumerations[id]->request.current())
            self->enumerations.remove(id);
    QVector<QByteArray> expired_views;
    for (auto it = self->views.cbegin(); it != self->views.cend(); ++it)
        if (it->expires < now())
            expired_views.append(it.key());
    for (auto view : expired_views) {
        auto erased = co_await self->eraseView(view);
        if (!erased)
            co_return erased.error();
    }
    QElapsedTimer elapsed;
    elapsed.start();
    auto mount_snapshot = self->config.load()->mounts;
    if (!mount_snapshot.isEmpty())
        std::rotate(mount_snapshot.begin(),
                    mount_snapshot.begin() + (self->refresh_mount_cursor++ % mount_snapshot.size()),
                    mount_snapshot.end());
    for (const auto &item : mount_snapshot) {
        if (item.mount.parent == root(item.identity))
            continue;
        auto result = co_await self->conflict(item.identity, item.mount);
        if (!result)
            LFLOG_DEBUG << "VFS conflict refresh deferred; code=" << int(result.error().code);
        if (elapsed.elapsed() > self->limits.request_timeout_ms)
            break;
    }
    auto subscription_snapshot = self->subscriptions;
    if (!subscription_snapshot.isEmpty())
        std::rotate(subscription_snapshot.begin(),
                    subscription_snapshot.begin() +
                        (self->refresh_subscription_cursor++ % subscription_snapshot.size()),
                    subscription_snapshot.end());
    for (auto weak : subscription_snapshot) {
        if (elapsed.elapsed() > self->limits.request_timeout_ms)
            break;
        auto state = weak.lock();
        if (!state)
            continue;
        bool invalid = !state->active || !state->request.current();
        QByteArray digest;
        if (!invalid && !state->dirty.exchange(false))
            continue;
        if (!invalid) {
            auto view = co_await self->buildView(state->identity, state->directory, state->request);
            if (!view) {
                if (view.error().code == ErrorCode::StaleCursor || view.error().code == ErrorCode::Busy) {
                    state->dirty = true;
                    continue;
                }
                invalid = true;
            } else {
                digest = self->views[view.value()].digest;
                co_await self->eraseView(view.value());
            }
        }
        if (!invalid && digest == state->digest)
            continue;
        if (invalid) {
            state->active = false;
            state->source.reset();
            state->targets.clear();
        }
        state->digest = digest;
        auto callback = state->callback;
        if (callback.context)
            QMetaObject::invokeMethod(
                callback.context,
                [weak, callback, change = Change{state->directory, digest, invalid}] {
                    if (auto state = weak.lock();
                        state && (change.invalidated || (state->active && state->request.current())))
                        callback.callback(change);
                },
                Qt::QueuedConnection);
    }
    self->subscriptions.removeIf([](auto weak) {
        auto state = weak.lock();
        return !state || !state->active;
    });
    co_return Result<void>{};
}
QCoro::Task<Result<void>> Vfs::shutdown() {
    auto self = impl_;
    if (self->stopping && !self->opened)
        co_return Result<void>{};
    if (QThread::currentThread() != thread())
        co_return err(ErrorCode::InvalidArgument);
    self->stopping = true;
    for (auto token : self->active_cancellations)
        token.cancel();
    for (;;) {
        bool committing = false;
        for (const auto &item : self->config.load()->mounts)
            if (item.life->commits)
                committing = true;
        if (!self->active_requests && !self->refreshing && !self->changing && !committing)
            break;
        co_await ShutdownTurn{};
    }
    for (auto mounted : QVector<Mounted>(self->config.load()->mounts)) {
        if (mounted.life->commits)
            co_return err(ErrorCode::Busy);
        mounted.life->active = false;
    }
    for (auto weak : self->subscriptions)
        if (auto state = weak.lock()) {
            state->active = false;
            state->source.reset();
            state->targets.clear();
        }
    self->subscriptions.clear();
    self->enumerations.clear();
    for (auto weak : QVector<std::weak_ptr<ReadHandle>>(self->readers))
        if (auto reader = weak.lock()) {
            auto closed = co_await Reader::closeState(reader);
            while (!closed && closed.error().code == ErrorCode::Busy) {
                co_await ShutdownTurn{};
                closed = co_await Reader::closeState(reader);
            }
            if (!closed)
                co_return closed.error();
        }
    self->readers.clear();
    for (auto state : self->exports) {
        state->closed = true;
        for (auto weak : state->readers)
            if (auto reader = weak.lock())
                co_await Reader::closeState(reader);
        auto removed = co_await state->backend->remove(state->ns, state->file);
        if (!removed && removed.error().code != ErrorCode::MissingEntry)
            co_return removed.error();
        state->cleaned = true;
    }
    self->exports.clear();
    self->active_exports = 0;
    self->opened = false;
    auto disk = std::move(self->disk);
    co_return co_await detail::work<void>([disk]() -> Result<void> {
        std::lock_guard guard(disk->mutex);
        if (disk->scratch) {
            sqlite3_close(disk->scratch);
            disk->scratch = nullptr;
        }
        disk->lock.reset();
        disk->root.reset();
        return {};
    });
}

namespace {
struct Validation {
    bool header = false, root_seen = false, recursive = false;
    NodeId root;
    QByteArray pending;
    struct Ancestor {
        NodeId id;
        QByteArray last_key;
    };
    QVector<Ancestor> ancestors;
    quint64 entries = 0;
    QCryptographicHash hash{QCryptographicHash::Sha256};
};
bool canonicalHex(const QJsonValue &value, qsizetype bytes) {
    if (!value.isString())
        return false;
    auto text = value.toString().toLatin1();
    return text.size() == bytes * 2 && QByteArray::fromHex(text).toHex() == text;
}
bool canonicalNumber(const QJsonValue &value) {
    if (!value.isString())
        return false;
    bool ok = false;
    auto n = value.toString().toULongLong(&ok);
    return ok && QString::number(n) == value.toString();
}
bool canonicalId(const QJsonValue &value) {
    if (!value.isString() || value.toString().size() > 4096)
        return false;
    auto text = value.toString().toLatin1();
    auto bytes = QByteArray::fromHex(text);
    if (bytes.toHex() != text || bytes.isEmpty())
        return false;
    return (bytes.size() == 17 && bytes[0] == 0) || (bytes.size() == 105 && bytes[0] == 1) ||
           (bytes.size() >= 334 && bytes.size() <= 2048 && bytes[0] == 2);
}

Result<void> parseLine(Validation &state, QByteArray line, const Limits &limits, PrivateState &disk, QByteArray view) {
    if (line.isEmpty() || line.size() > limits.max_record_bytes)
        return err(ErrorCode::Corruption);
    QJsonParseError error;
    auto document = QJsonDocument::fromJson(line, &error);
    if (error.error != QJsonParseError::NoError || !document.isObject() ||
        document.toJson(QJsonDocument::Compact) != line)
        return err(ErrorCode::Corruption);
    auto object = document.object();
    if (!state.header) {
        if (object.size() != 5 || object["format"] != "darkspeak-vfs-catalog" || object["version"] != 1 ||
            object["consistency"] != "per-directory" || !object["recursive"].isBool() || !canonicalId(object["root"]))
            return err(ErrorCode::Corruption);
        state.header = true;
        state.recursive = object["recursive"].toBool();
        state.root = QByteArray::fromHex(object["root"].toString().toLatin1());
        return {};
    }
    if (object.size() != 12 || !canonicalId(object["id"]) ||
        (!object["parent"].toString().isEmpty() && !canonicalId(object["parent"])) || !object["name"].isString() ||
        !canonicalNumber(object["size"]) || !canonicalNumber(object["revision"]) || !object["type"].isDouble() ||
        (object["type"] != 0 && object["type"] != 1) || !object["status"].isDouble() ||
        (object["status"] != 0 && object["status"] != 3) || !object["hashState"].isDouble() ||
        object["hashState"].toDouble() != object["hashState"].toInt() || object["hashState"].toInt() < 0 ||
        object["hashState"].toInt() > 3 || !object["symlinks"].isDouble() ||
        object["symlinks"].toDouble() != object["symlinks"].toInt() || object["symlinks"].toInt() < 0 ||
        object["symlinks"].toInt() > 2 || !object["capabilities"].isDouble() ||
        object["capabilities"].toDouble() != object["capabilities"].toInt() || object["capabilities"].toInt() < 0 ||
        object["capabilities"].toInt() > 63 || !object["modified"].isString() || !object["sha256"].isString() ||
        (!object["sha256"].toString().isEmpty() && !canonicalHex(object["sha256"], 32)))
        return err(ErrorCode::Corruption);
    auto node = fromRecord(object);
    if ((!node.modified.isValid() && !object["modified"].toString().isEmpty()) ||
        (node.type == EntryType::Directory && (node.size || !node.sha256.isEmpty())))
        return err(ErrorCode::Corruption);
    if (!state.root_seen) {
        if (node.id != state.root || node.type != EntryType::Directory || !node.parent.isEmpty() ||
            (node.name != "/" && !validName(node.name, true)))
            return err(ErrorCode::Corruption);
        state.root_seen = true;
        state.ancestors.append({node.id, {}});
    } else {
        if (!validName(node.name, true))
            return err(ErrorCode::Corruption);
        if (++state.entries > limits.max_export_entries)
            return err(ErrorCode::QuotaExceeded);
        while (!state.ancestors.isEmpty() && state.ancestors.last().id != node.parent)
            state.ancestors.removeLast();
        if (state.ancestors.isEmpty() || (!state.recursive && node.parent != state.root))
            return err(ErrorCode::Corruption);
        auto key = node.name.toUtf8() + char(0) + node.id;
        if (!state.ancestors.last().last_key.isEmpty() && key <= state.ancestors.last().last_key)
            return err(ErrorCode::Corruption);
        state.ancestors.last().last_key = key;
        if (node.type == EntryType::Directory && state.recursive) {
            if (state.ancestors.size() >= int(limits.max_depth))
                return err(ErrorCode::QuotaExceeded);
            state.ancestors.append({node.id, {}});
        }
    }
    Statement statement;
    if (!statement.prepare(disk.scratch, "INSERT INTO entry(view,name,id,data) VALUES(?,?,?,'{}')"))
        return err(ErrorCode::IoError);
    statement.blob(1, view);
    statement.blob(2, node.id);
    statement.blob(3, node.id);
    auto status = sqlite3_step(statement.stmt);
    if (status != SQLITE_DONE)
        return err(status == SQLITE_FULL ? ErrorCode::QuotaExceeded : ErrorCode::Corruption);
    return {};
}
} // namespace
QCoro::Task<Result<void>> Vfs::validateCatalog(std::shared_ptr<IFileReader> reader, quint64 size, QByteArray digest,
                                               CancellationToken cancel) {
    auto self = impl_;
    auto available = self->available();
    if (!available)
        co_return available.error();
    if (!reader || !size || size > self->limits.max_export_bytes || digest.size() != 32)
        co_return err(ErrorCode::InvalidArgument);
    if (self->active_requests >= self->limits.max_requests)
        co_return err(ErrorCode::QueueFull);
    Admission admission(self);
    OperationCancellation cancellation(self, cancel);
    auto state = std::make_shared<Validation>();
    const auto view = QUuid::createUuid().toRfc4122();
    quint64 offset = 0;
    QElapsedTimer elapsed;
    elapsed.start();
    std::optional<Error> failure;
    while (offset < size) {
        if (cancel.cancelled() || elapsed.elapsed() > self->limits.export_timeout_ms) {
            failure = err(cancel.cancelled() ? ErrorCode::Cancelled : ErrorCode::Busy);
            break;
        }
        auto bytes = co_await reader->readAt(offset, qsizetype(std::min<quint64>(8192, size - offset)), cancel);
        if (!bytes) {
            failure = bytes.error();
            break;
        }
        if (bytes.value().isEmpty() || quint64(bytes.value().size()) > size - offset) {
            failure = err(ErrorCode::Corruption);
            break;
        }
        offset += quint64(bytes.value().size());
        auto parsed =
            co_await detail::work<void>([self, state, view, bytes = std::move(bytes).value()]() -> Result<void> {
                std::lock_guard guard(self->disk->mutex);
                state->hash.addData(bytes);
                state->pending.append(bytes);
                qsizetype consumed = 0;
                for (;;) {
                    auto end = state->pending.indexOf('\n', consumed);
                    if (end < 0)
                        break;
                    auto result = parseLine(*state, state->pending.sliced(consumed, end - consumed), self->limits,
                                            *self->disk, view);
                    if (!result)
                        return result.error();
                    consumed = end + 1;
                }
                state->pending.remove(0, consumed);
                if (state->pending.size() > self->limits.max_record_bytes)
                    return err(ErrorCode::Corruption);
                return {};
            });
        if (!parsed) {
            failure = parsed.error();
            break;
        }
    }
    auto erased = co_await self->eraseView(view);
    if (!erased)
        co_return erased.error();
    if (failure)
        co_return *failure;
    if (!state->header || !state->root_seen || !state->pending.isEmpty() || state->hash.result() != digest)
        co_return err(ErrorCode::IntegrityError);
    co_return Result<void>{};
}
} // namespace dsvfs
