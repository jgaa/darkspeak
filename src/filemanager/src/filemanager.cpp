#include "internal.h"
#include "async_p.h"
#include "storagepool_p.h"
#include "native_pool_p.h"
#include <QAbstractEventDispatcher>
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QThread>
#include <chrono>
#include <logfault/logfault.h>
#include <fcntl.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/file.h>

namespace dsfm {
using namespace detail;
DirectorySubscription::DirectorySubscription(NamespaceId ns, DirectoryId directory, quint64 generation,
        std::shared_ptr<std::atomic_bool> active, std::function<void()> release)
    : namespace_id_(ns), directory_id_(directory), generation_(generation), active_(std::move(active)), release_(std::move(release)) {}
DirectorySubscription::~DirectorySubscription() { reset(); }
DirectorySubscription::DirectorySubscription(DirectorySubscription&& other) noexcept { *this = std::move(other); }
DirectorySubscription& DirectorySubscription::operator=(DirectorySubscription&& other) noexcept {
    if (this != &other) {
        reset();
        namespace_id_ = std::exchange(other.namespace_id_, {});
        directory_id_ = std::exchange(other.directory_id_, {});
        generation_ = std::exchange(other.generation_, 0);
        active_ = std::move(other.active_);
        release_ = std::exchange(other.release_, {});
    }
    return *this;
}
void DirectorySubscription::reset() noexcept {
    if (active_) active_->store(false);
    if (release_) { auto release = std::exchange(release_, {}); release(); }
    active_.reset(); namespace_id_ = {}; directory_id_ = {}; generation_ = 0;
}
DirectorySubscription::operator bool() const noexcept { return active_ && active_->load(); }
QCoro::Task<Result<QByteArray>> IFileWriter::hashPrefix(quint64, CancellationToken) {
    co_return Error{ErrorCode::UnsupportedCapability};
}
QCoro::Task<Result<void>> IFileWriter::discardSuffix(quint64, CancellationToken) {
    co_return Error{ErrorCode::UnsupportedCapability};
}
struct IFileManager::AccessPages {
    struct Cursor {
        NamespaceId ns; DirectoryId parent; QUuid scope; QByteArray cursor, filesystem_generation;
        quint64 generation = 0, policy = 0;
        std::shared_ptr<const std::atomic<quint64>> authority;
        std::chrono::steady_clock::time_point expires;
        bool stable_order = false, ascii_order = false;
    };
    std::mutex mutex;
    QHash<QByteArray, Cursor> cursors;
};
IFileManager::IFileManager() : access_pages_(std::make_shared<AccessPages>()) {}
IFileManager::~IFileManager() = default;
QCoro::Task<Result<PreparedRead>> IFileManager::prepareRead(NamespaceId, FileId, ReadPreparationOptions) {
    co_return Error{ErrorCode::UnsupportedCapability};
}
QCoro::Task<Result<DirectorySubscription>> IFileManager::subscribeDirectory(NamespaceId, DirectoryId, DirectoryChangeCallback) {
    co_return Error{ErrorCode::UnsupportedCapability};
}
QCoro::Task<Result<TemporaryFile>> IFileManager::beginTemporary(NamespaceId,TemporaryOptions,TemporaryAccess){co_return Error{ErrorCode::UnsupportedCapability};}
QCoro::Task<Result<TemporaryInfo>> IFileManager::finalizeTemporary(NamespaceId,TempId,TemporaryAccess,CancellationToken){co_return Error{ErrorCode::UnsupportedCapability};}
QCoro::Task<Result<TemporaryRead>> IFileManager::leaseTemporary(NamespaceId,TempId,TemporaryLeaseOptions,TemporaryAccess){co_return Error{ErrorCode::UnsupportedCapability};}
QCoro::Task<Result<void>> IFileManager::releaseTemporary(NamespaceId,QByteArray,TemporaryAccess){co_return Error{ErrorCode::UnsupportedCapability};}
QCoro::Task<Result<void>> IFileManager::discardTemporary(NamespaceId,TempId,TemporaryAccess){co_return Error{ErrorCode::UnsupportedCapability};}
QCoro::Task<Result<void>> IFileManager::reclaimTemporary(NamespaceId,quint32){co_return Error{ErrorCode::UnsupportedCapability};}
IFileManager::FilesystemGuard IFileManager::filesystemGuard(NamespaceId, QUuid, std::function<bool(FilesystemIdentity)> check) {
    if (!check) return {};
    return []() -> QCoro::Task<Result<void>> { co_return Error{ErrorCode::UnsupportedCapability}; };
}
namespace {
bool permitted(const AccessContext& access, NamespaceId ns, QUuid id, AccessOperation operation) {
    if (!access.current()) return false;
    if (!access.check) return true;
    try { auto result = access.check(ns, id, operation); return result && result.value(); }
    catch (...) { return false; }
}
class AuthorizedReader final : public IFileReader {
public:
    AuthorizedReader(std::unique_ptr<IFileReader> reader, AccessContext access, NamespaceId ns, FileId id,
        std::function<QCoro::Task<Result<void>>()> guard)
        : reader_(std::move(reader)), access_(std::move(access)), ns_(ns), id_(id), guard_(std::move(guard)) {}
    std::function<bool()> accessGuard() const override {
        auto inner = reader_->accessGuard(); auto access = access_; auto ns = ns_; auto id = id_;
        return [inner, access, ns, id] {
            try { return permitted(access, ns, id, AccessOperation::Read) && inner()
                && (!access.filesystem_check || access.filesystem_check({})); }
            catch (...) { return false; }
        };
    }
    QCoro::Task<Result<QByteArray>> readAt(quint64 offset, qsizetype length, CancellationToken cancel) override {
        if (!permitted(access_, ns_, id_, AccessOperation::Read)) co_return Error{ErrorCode::MissingEntry};
        if (guard_) { auto check = co_await guard_(); if (!check) co_return check.error(); }
        auto result = co_await reader_->readAt(offset, length, cancel);
        if (guard_) { auto check = co_await guard_(); if (!check) co_return check.error(); }
        if (!permitted(access_, ns_, id_, AccessOperation::Read)) co_return Error{ErrorCode::MissingEntry};
        co_return result;
    }
    QCoro::Task<Result<void>> close() override { return reader_->close(); }
    QCoro::Task<Result<void>> invalidate() override { return reader_->invalidate(); }
private:
    std::unique_ptr<IFileReader> reader_; AccessContext access_; NamespaceId ns_; FileId id_;
    std::function<QCoro::Task<Result<void>>()> guard_;
};
}
QCoro::Task<Result<FileInfo>> IFileManager::stat(NamespaceId ns, QUuid id, AccessContext access) {
    if (!permitted(access, ns, id, AccessOperation::Inspect)) co_return Error{ErrorCode::MissingEntry};
    auto guard = filesystemGuard(ns, id, access.filesystem_check);
    if (guard) { auto check = co_await guard(); if (!check) co_return check.error(); }
    auto result = co_await stat(ns, id);
    if (guard) { auto check = co_await guard(); if (!check) co_return check.error(); }
    if (!permitted(access, ns, id, AccessOperation::Inspect)) co_return Error{ErrorCode::MissingEntry};
    co_return result;
}
QCoro::Task<Result<FileInfo>> IFileManager::lookup(NamespaceId ns, QString path, AccessContext access) {
    auto result = co_await lookup(ns, std::move(path));
    if (result && !permitted(access, ns, result.value().id, AccessOperation::Inspect)) co_return Error{ErrorCode::MissingEntry};
    if (result) { auto guard = filesystemGuard(ns, result.value().id, access.filesystem_check); if (guard) { auto check = co_await guard(); if (!check) co_return check.error(); } }
    if (result && !permitted(access, ns, result.value().id, AccessOperation::Inspect)) co_return Error{ErrorCode::MissingEntry};
    co_return result;
}
QCoro::Task<Result<void>> IFileManager::discardDirectoryCursor(NamespaceId ns, QByteArray cursor) {
    std::lock_guard lock(access_pages_->mutex);
    auto found=access_pages_->cursors.find(cursor);
    if(found!=access_pages_->cursors.end() && found->ns==ns) access_pages_->cursors.erase(found);
    co_return Result<void>{};
}
QCoro::Task<Result<DirectoryPage>> IFileManager::list(NamespaceId ns, DirectoryId parent, PageRequest page, AccessContext access) {
    if(page.stable_order && !supportsStableEnumeration()) co_return Error{ErrorCode::UnsupportedCapability};
    if (!access.check && !access.current_generation && !access.filesystem_check) co_return co_await list(ns, parent, std::move(page));
    if (access.authorization_scope.isNull() || page.cursor.size() > 64) co_return Error{ErrorCode::InvalidArgument};
    if (!access.current()) co_return Error{page.cursor.isEmpty() ? ErrorCode::MissingEntry : ErrorCode::StaleCursor};
    if (!permitted(access, ns, parent, AccessOperation::List)) co_return Error{ErrorCode::MissingEntry};
    auto info = co_await namespaceInfo(ns); if (!info) co_return info.error();
    if (!page.limit || page.limit > info.value().config.limits.max_page_size) co_return Error{ErrorCode::InvalidArgument};
    auto state = access_pages_; const auto now = std::chrono::steady_clock::now();
    if (!page.cursor.isEmpty()) {
        std::lock_guard lock(state->mutex); auto found = state->cursors.find(page.cursor);
        if (found == state->cursors.end()) co_return Error{ErrorCode::StaleCursor};
        const auto stored = found.value(); state->cursors.erase(found);
        if (stored.ns != ns || stored.parent != parent || stored.scope != access.authorization_scope || (!page.stable_order && stored.generation != info.value().generation) || stored.stable_order != page.stable_order || stored.ascii_order != page.ascii_order
            || stored.policy != access.generation || stored.filesystem_generation != access.filesystem_generation || stored.authority != access.current_generation || now >= stored.expires) co_return Error{ErrorCode::StaleCursor};
        page.cursor = stored.cursor;
    }
    auto parent_guard = filesystemGuard(ns, parent, access.filesystem_check);
    if (parent_guard) { auto check = co_await parent_guard(); if (!check) co_return check.error(); }
    auto result = co_await list(ns, parent, page); if (!result) co_return result.error();
    QVector<FileInfo> visible;
    for (auto& child : result.value().entries) {
        if (!permitted(access, ns, child.id, AccessOperation::Inspect)) continue;
        auto guard = filesystemGuard(ns, child.id, access.filesystem_check);
        if (guard) {
            auto check = co_await guard();
            if (!check) { if (check.error().code == ErrorCode::MissingEntry || check.error().code == ErrorCode::PermissionDenied) continue; co_return check.error(); }
        }
        visible.append(std::move(child));
    }
    result.value().entries = std::move(visible);
    if (parent_guard) { auto check = co_await parent_guard(); if (!check) co_return check.error(); }
    if (!permitted(access, ns, parent, AccessOperation::List)) co_return Error{ErrorCode::MissingEntry};
    if (!result.value().next_cursor.isEmpty()) {
        std::lock_guard lock(state->mutex);
        for (auto it = state->cursors.begin(); it != state->cursors.end();) {
            if (now >= it->expires) it = state->cursors.erase(it); else ++it;
        }
        if (state->cursors.size() >= std::min<quint32>(128, info.value().config.limits.max_notifications)) co_return Error{ErrorCode::QuotaExceeded};
        auto token = QUuid::createUuid().toRfc4122().toHex();
        state->cursors.insert(token, {ns, parent, access.authorization_scope, result.value().next_cursor, access.filesystem_generation, info.value().generation, access.generation,
            access.current_generation, now + std::chrono::minutes(1), page.stable_order, page.ascii_order});
        result.value().next_cursor = token;
    }
    co_return result;
}
QCoro::Task<Result<std::unique_ptr<IFileReader>>> IFileManager::openRead(NamespaceId ns, FileId id, AccessContext access) {
    if (!permitted(access, ns, id, AccessOperation::Read)) co_return Error{ErrorCode::MissingEntry};
    auto guard = filesystemGuard(ns, id, access.filesystem_check);
    if (guard) { auto check = co_await guard(); if (!check) co_return check.error(); }
    auto result = co_await openRead(ns, id);
    if (!result) co_return result.error();
    if (!permitted(access, ns, id, AccessOperation::Read)) co_return Error{ErrorCode::MissingEntry};
    co_return std::unique_ptr<IFileReader>(new AuthorizedReader(std::move(result).value(), std::move(access), ns, id, std::move(guard)));
}
QCoro::Task<Result<PreparedRead>> IFileManager::prepareRead(NamespaceId ns, FileId id, ReadPreparationOptions options, AccessContext access) {
    if (!permitted(access, ns, id, AccessOperation::Read)) co_return Error{ErrorCode::MissingEntry};
    auto guard = filesystemGuard(ns, id, access.filesystem_check);
    if (guard) { auto check = co_await guard(); if (!check) co_return check.error(); }
    auto result = co_await prepareRead(ns, id, std::move(options));
    if (!result) co_return result.error();
    if (guard) { auto check = co_await guard(); if (!check) co_return check.error(); }
    if (!permitted(access, ns, id, AccessOperation::Read)) co_return Error{ErrorCode::MissingEntry};
    auto value = std::move(result).value();
    value.reader = std::make_unique<AuthorizedReader>(std::move(value.reader), std::move(access), ns, id, std::move(guard));
    co_return value;
}
QCoro::Task<Result<DirectorySubscription>> IFileManager::subscribeDirectory(NamespaceId ns, DirectoryId directory, DirectoryChangeCallback callback, AccessContext access) {
    if (access.filesystem_check) co_return Error{ErrorCode::UnsupportedCapability};
    if (!permitted(access, ns, directory, AccessOperation::List)) co_return Error{ErrorCode::MissingEntry};
    auto deliver = std::move(callback.callback);
    callback.callback = [access, ns, directory, deliver = std::move(deliver)](DirectoryChange change) {
        if (permitted(access, ns, directory, AccessOperation::List)) deliver(change);
    };
    co_return co_await subscribeDirectory(ns, directory, std::move(callback));
}
namespace {
template<typename T> QCoro::Task<Result<T>> rejected(ErrorCode code) { co_return Error{code}; }
bool validPageEnvelope(const PageRequest& page) { return page.limit > 0 && page.limit <= 1000 && page.cursor.size() <= 4096; }
struct WriterState {
    std::atomic_bool expired{false};
    bool private_access = false;
    int private_kind = 0;
    std::shared_ptr<Namespace> ns;
    PendingId id;
    Fd fd;
    QVector<ByteRange> dirty;
    std::unique_ptr<QCryptographicHash> prefix_hash;
    quint64 prefix_offset = 0;
    Identity prefix_identity;
    bool verifying = false;
    std::atomic_bool closed{false};
    QUuid lane = QUuid::createUuid();
};
struct TemporaryReaderState;
struct ReaderState {
    bool private_access = false;
    int private_kind = 0;
    std::shared_ptr<Namespace> ns;
    FileId id;
    Fd fd;
    Identity opened;
    std::atomic_bool closed{false};
    QUuid lane = QUuid::createUuid();
};
struct Subscriber {
    QUuid ns;
    QPointer<QObject> context; // Accessed only on its Qt thread after registration.
    std::shared_ptr<QObject> dispatch;
    std::optional<DirectoryId> directory;
    std::atomic_bool invalidated{false};
    std::function<void(DirectoryChange)> callback;
    std::atomic_bool queued{false};
    std::atomic_bool active{true};
    std::atomic<quint64> generation{0};
};
std::shared_ptr<Subscriber> makeSubscriber(NamespaceId ns, QObject* context, std::function<void(Change)> callback) {
    auto sub = std::make_shared<Subscriber>();
    sub->ns = ns; sub->context = context;
    if (callback) sub->callback = [callback = std::move(callback)](DirectoryChange change) {
        callback({change.namespace_id, change.generation, true});
    };
    // Keep a separate delivery object alive across queued worker calls, even if the
    // subscriber's context is destroyed. Never dereference its QPointer on workers.
    sub->dispatch = std::shared_ptr<QObject>(new QObject, [](QObject* object) { object->deleteLater(); });
    std::weak_ptr<Subscriber> weak = sub;
    QObject::connect(context, &QObject::destroyed, sub->dispatch.get(), [weak] {
        if (auto state = weak.lock()) state->active = false;
    }, Qt::DirectConnection);
    return sub;
}
NamespaceInfo infoFor(Namespace& ns, Catalog& db) {
    NamespaceInfo result;
    result.config = ns.config; result.root = ns.root_id; result.generation = db.generation(); result.charged_bytes = charged(db);
    result.untracked_staging = ns.untracked_staging; result.staging_scan_truncated = ns.staging_scan_truncated;
    result.status = NamespaceStatus::Ready;
    result.capabilities = Capability::Read | Capability::Write | Capability::PreparedRead | Capability::DirectorySubscriptions;
    if(ns.config.storage_pool)result.capabilities|=Capability::TemporaryArtifacts;
    return result;
}
void checkBrowseDirectory(Namespace& ns, Catalog& db, DirectoryId id) {
    const auto entry = db.entry(id);
    require(entry.type == EntryType::Directory, ErrorCode::InvalidArgument);
    if (entry.status != EntryStatus::Updating) validateEntry(ns, db, entry);
}
void checkParent(Namespace& ns, Catalog& db, DirectoryId parent) {
    auto info = db.entry(parent); require(info.type == EntryType::Directory, ErrorCode::InvalidArgument);
    validateEntry(ns, db, info);
    quint32 depth = 0;
    while (info.parent) { require(++depth < ns.config.limits.max_depth, ErrorCode::QuotaExceeded); info = db.entry(*info.parent); }
}
QString chooseName(Namespace& ns, Catalog& db, DirectoryId parent, QString suggested, ConflictPolicy conflict, QUuid except = {}) {
    auto name = normalizeName(std::move(suggested)); if (!name) throw Failure{name.error()};
    const auto original = name.value();
    const auto dot = original.lastIndexOf('.');
    const auto stem = dot > 0 ? original.left(dot) : original;
    const auto extension = dot > 0 ? original.mid(dot) : QString{};
    for (quint32 attempt = 0; attempt < ns.config.limits.max_collision_attempts; ++attempt) {
        QString candidate = original;
        if (attempt) {
            const auto suffix = QStringLiteral(" (%1)").arg(attempt + 1);
            auto shortened = stem;
            auto ext = extension;
            if (ext.size() > 100 || ext.toUtf8().size() > 100) ext.clear();
            for (;;) {
                candidate = shortened + suffix + ext;
                if (normalizeName(candidate)) break;
                require(!shortened.isEmpty(), ErrorCode::InvalidName);
                shortened.chop(shortened.back().isLowSurrogate() ? 2 : 1);
            }
        }
        const auto key = nameKey(candidate).value();
        auto found = db.scalar("SELECT id FROM entries WHERE parent=? AND name_key=? AND id!=?", {uuid(parent), key, uuid(except)});
        const auto pending = db.scalar("SELECT id FROM pending WHERE private=0 AND parent=? AND name_key=? AND file_id!=?", {uuid(parent), key, uuid(except)});
        bool physical_collision = false;
        if (ns.config.naming == PhysicalNaming::OriginalName) {
            const auto parent_path = db.path(parent, ns.config.naming, ns.config.limits.max_depth);
            const auto path = parent_path == "." ? candidate : parent_path + '/' + candidate;
            auto directory = openBeneath(ns.root.value, parentPath(path), O_RDONLY | O_DIRECTORY);
            struct stat st{};
            if (::fstatat(directory.value, path.section('/', -1).toUtf8().constData(), &st, AT_SYMLINK_NOFOLLOW) == 0) {
                // A same-entry case-only rename is permitted; foreign destinations are never overwritten.
                physical_collision = true;
                if (!except.isNull()) {
                    auto own = db.query("SELECT dev,ino FROM entries WHERE id=?", {uuid(except)});
                    if (own.next() && quint64(st.st_dev) == own.value(0).toULongLong() && quint64(st.st_ino) == own.value(1).toULongLong()) physical_collision = false;
                }
            } else ioCheck(errno == ENOENT);
        }
        if (found.isNull() && pending.isNull() && !physical_collision) return candidate;
        if (conflict == ConflictPolicy::Fail) fail(ErrorCode::Conflict);
    }
    fail(ErrorCode::Conflict);
}
void checkConstraints(const ContentConstraints& content, const NamespaceLimits& limits) {
    require(content.expected_sha256.isEmpty() || content.expected_sha256.size() == 32, ErrorCode::InvalidArgument);
    require(!content.expected_size || *content.expected_size <= limits.max_file_size, ErrorCode::QuotaExceeded);
}
void checkPendingFd(WriterState& writer, Catalog& db, bool allow_verifying = false, bool cleanup = false) {
    require(cleanup || !writer.expired,ErrorCode::MissingEntry);
    require(allow_verifying || !writer.verifying, ErrorCode::Busy);
    require(!writer.closed, ErrorCode::Busy);
    auto q = db.query("SELECT dev,ino FROM pending WHERE id=?", {uuid(writer.id)}); require(q.next(), ErrorCode::MissingEntry);
    const auto found = identity(writer.fd.value);
    require(found.dev == q.value(0).toULongLong() && found.ino == q.value(1).toULongLong(), ErrorCode::ExternalModification);
    auto current = openBeneath(writer.ns->root.value, pendingPath(writer.id), O_RDONLY | O_NONBLOCK);
    const auto named = identity(current.value);
    require(named.dev == found.dev && named.ino == found.ino, ErrorCode::ExternalModification);
}
void checkpointWriter(WriterState& writer, Catalog& db) {
    checkPendingFd(writer, db);
    fault("before_checkpoint_sync");
    ioCheck(::fsync(writer.fd.value) == 0);
    fault("after_checkpoint_sync");
    auto ranges = db.ranges(writer.id, RangeKind::StorageValid);
    for (auto range : writer.dirty) ranges = addRange(std::move(ranges), range);
    require(ranges.size() <= writer.ns->config.limits.max_ranges, ErrorCode::QuotaExceeded);
    Transaction transaction(db);
    db.putRanges(writer.id, RangeKind::StorageValid, ranges);
    db.query("UPDATE pending SET range_generation=range_generation+1 WHERE id=?", {uuid(writer.id)});
    db.bump(); transaction.commit(); writer.dirty.clear();
    if(!writer.private_access)poolPendingObserve(*writer.ns,writer.id,writer.fd.value);
    fault("after_checkpoint_record");
}
void closeWriter(WriterState& writer) {
    if (writer.closed) return;
    writer.closed = true; writer.fd = Fd{};
    writer.ns->active_writers.remove(writer.id); --writer.ns->handles;
}
} // namespace

struct NativeFileManager::Impl : std::enable_shared_from_this<Impl> {
    std::shared_ptr<Scheduler> scheduler = Scheduler::create();
    std::mutex mutex;
    QHash<NamespaceId, std::shared_ptr<Namespace>> spaces;
    QVector<std::weak_ptr<WriterState>> writers;
    QVector<std::weak_ptr<ReaderState>> readers;
    QVector<std::weak_ptr<TemporaryReaderState>> temporary_readers;
    std::map<quint64, std::shared_ptr<Subscriber>> subscribers;
    quint64 next_subscription = 1;
    std::atomic_bool shutting_down{false};
    ~Impl() {
        auto retired = std::move(spaces);
        scheduler->enqueue({{}, 0, [retired = std::move(retired)]() mutable { retired.clear(); }}, 4096, 64ULL << 20, true);
        scheduler->stop();
    }
    std::shared_ptr<Namespace> space(NamespaceId id) {
        require(!shutting_down, ErrorCode::ShuttingDown);
        std::lock_guard lock(mutex); auto found = spaces.value(id); require(bool(found), ErrorCode::MissingEntry); return found;
    }
    void notify(NamespaceId id, Catalog& db) {
        const auto generation = db.generation();
        std::lock_guard lock(mutex);
        std::erase_if(subscribers, [](const auto& item) { return !item.second->active; });
        for (auto& [key, sub] : subscribers) {
            Q_UNUSED(key);
            if (sub->ns != id || !sub->active) continue;
            // Native notifications deliberately invalidate pages on any namespace
            // change. This bounded fallback also covers moves and reconciliation.
            sub->generation = generation;
            if (sub->directory && !db.scalar("SELECT id FROM entries WHERE id=?", {uuid(*sub->directory)}).isValid()) sub->invalidated = true;
            if (sub->queued.exchange(true)) continue;
            QMetaObject::invokeMethod(sub->dispatch.get(), [sub] {
                sub->queued = false;
                if (sub->active && sub->context) {
                    const bool terminal = sub->invalidated;
                    if (terminal) sub->active = false;
                    sub->callback({sub->ns, sub->directory.value_or(DirectoryId{}), sub->generation.load(),
                        terminal ? DirectoryChangeKind::Invalidated : DirectoryChangeKind::RescanRequired});
                }
            }, Qt::QueuedConnection);
        }
    }
    template<typename T, typename Function> QCoro::Task<Result<T>> submit(NamespaceId id, Function function, quint64 bytes = 0, bool recovery = true, QUuid lane = {}, bool private_access = false) {
        auto self = shared_from_this();
        std::shared_ptr<Namespace> ns;
        try { ns = space(id); } catch (const Failure& error) { co_return error.error; }
        auto work = [self, ns, function = std::move(function), recovery, private_access]() mutable -> Result<T> {
            require(!self->shutting_down, ErrorCode::ShuttingDown);
            std::lock_guard lock(ns->mutex);
            PermissionRepairScope permissions(ns->config.repair_permissions);
            auto current_root = openRoot(ns->config.storage_root, false);
            const auto expected_root = identity(ns->root.value, true); const auto actual_root = identity(current_root.value, true);
            require(expected_root.dev == actual_root.dev && expected_root.ino == actual_root.ino, ErrorCode::ExternalModification);
            auto current_internal = openBeneath(ns->root.value, ".filemanager", O_RDONLY | O_DIRECTORY);
            const auto expected_internal = identity(ns->internal.value, true); const auto actual_internal = identity(current_internal.value, true);
            require(expected_internal.dev == actual_internal.dev && expected_internal.ino == actual_internal.ino, ErrorCode::ExternalModification);
            Catalog db(ns->catalogPath(), ns->config.limits.max_catalog_bytes, false);
            db.private_access=private_access;
            const auto before = db.generation();
            try {
                if (recovery) recover(*ns, db);
                auto result = function(*ns, db);
                if (db.generation() != before) self->notify(ns->config.id, db);
                return result;
            } catch (...) {
                if (db.generation() != before) self->notify(ns->config.id, db);
                throw;
            }
        };
        co_return co_await execute<T>(scheduler, lane.isNull() ? id : lane, bytes, ns->config.limits.max_operations, ns->config.limits.max_buffered_bytes, std::move(work), id);
    }
    template<typename T, typename State, typename Function>
    QCoro::Task<Result<T>> handleSubmit(std::shared_ptr<State> state, Function function, quint64 bytes = 0) {
        return submit<T>(state->ns->config.id, [state,function=std::move(function)](Namespace& ns,Catalog& db)mutable->Result<T>{
            db.private_class=state->private_kind;return function(ns,db);
        }, bytes, true, state->lane, state->private_access);
    }
};

namespace {
// Only used outside SQLite transactions, with an outer namespace lock guard.
// The handle's scheduler lane prevents concurrent close/write on that handle.
struct UnlockIo {
    std::mutex& mutex;
    explicit UnlockIo(std::mutex& value) : mutex(value) { mutex.unlock(); }
    ~UnlockIo() { mutex.lock(); }
};
class Reader final : public IFileReader {
public:
    Reader(std::shared_ptr<NativeFileManager::Impl> impl, std::shared_ptr<ReaderState> state) : impl_(std::move(impl)), state_(std::move(state)) {}
    ~Reader() override {
        auto state = state_; auto impl = impl_;
        impl->scheduler->enqueue({state->lane, 0, [state] { std::lock_guard lock(state->ns->mutex); if (!state->closed) { state->closed = true; state->fd = Fd{}; if (--state->ns->readers[state->id] == 0) state->ns->readers.remove(state->id); --state->ns->handles; } }}, 4096, 64ULL << 20, true);
    }
    QCoro::Task<Result<QByteArray>> readAt(quint64 offset, qsizetype max_bytes, CancellationToken cancel) override {
        auto state = state_;
        if (max_bytes < 0 || quint64(max_bytes) > state->ns->config.limits.max_io_bytes || offset > quint64(std::numeric_limits<qint64>::max()) || quint64(max_bytes) > quint64(std::numeric_limits<qint64>::max()) - offset) co_return Error{ErrorCode::InvalidArgument};
        co_return co_await impl_->handleSubmit<QByteArray>(state, [state, offset, max_bytes, cancel](Namespace& ns, Catalog& db) -> Result<QByteArray> {
            require(!state->closed, ErrorCode::Busy); cancelCheck(cancel);
            validateEntry(ns, db, db.entry(state->id));
            const auto before = identity(state->fd.value);
            require(before.dev == state->opened.dev && before.ino == state->opened.ino && before.size == state->opened.size && before.modified == state->opened.modified, ErrorCode::ExternalModification);
            QByteArray data(max_bytes, Qt::Uninitialized);
            ssize_t count;
            { UnlockIo unlocked(ns.mutex);
                do { count = ::pread(state->fd.value, data.data(), size_t(max_bytes), off_t(offset)); } while (count < 0 && errno == EINTR && !cancel.cancelled());
            }
            ioCheck(count >= 0); data.resize(count);
            const auto after = identity(state->fd.value);
            require(before.size == after.size && before.modified == after.modified, ErrorCode::ExternalModification);
            return data;
        }, quint64(max_bytes));
    }
    QCoro::Task<Result<void>> invalidate() override {
        auto state = state_;
        co_return co_await impl_->handleSubmit<void>(state, [state](Namespace&, Catalog& db) -> Result<void> {
            require(!state->closed, ErrorCode::Busy);
            db.query("UPDATE entries SET status=2,sha=NULL WHERE id=?", {uuid(state->id)}); db.bump(); return {};
        });
    }
    QCoro::Task<Result<void>> close() override {
        auto state = state_;
        co_return co_await impl_->handleSubmit<void>(state, [state](Namespace& ns, Catalog&) -> Result<void> {
            if (!state->closed) { state->closed = true; state->fd = Fd{}; if (--ns.readers[state->id] == 0) ns.readers.remove(state->id); --ns.handles; } return {};
        });
    }
private:
    std::shared_ptr<NativeFileManager::Impl> impl_;
    std::shared_ptr<ReaderState> state_;
};
PreparedRead attachReader(const std::shared_ptr<NativeFileManager::Impl>& impl, Namespace& space, Catalog& db, FileId id) {
    auto entry = db.entry(id);
    require(entry.type == EntryType::File, ErrorCode::InvalidArgument);
    validateEntry(space, db, entry);
    require(space.handles < space.config.limits.max_handles, ErrorCode::QuotaExceeded);
    auto state = std::make_shared<ReaderState>(); state->ns = impl->space(space.config.id); state->id = id;
    state->fd = openBeneath(space.root.value, db.path(id, space.config.naming, space.config.limits.max_depth), O_RDONLY | O_NONBLOCK);
    state->opened = identity(state->fd.value);
    auto q = db.query("SELECT dev,ino,mtime FROM entries WHERE id=?", {uuid(id)});
    require(q.next(), ErrorCode::MissingEntry);
    require(state->opened.dev == q.value(0).toULongLong() && state->opened.ino == q.value(1).toULongLong()
        && state->opened.modified == q.value(2).toLongLong() && state->opened.size == entry.size, ErrorCode::ExternalModification);
    auto reader = std::make_unique<Reader>(impl, state);
    ++space.handles; ++space.readers[id];
    { std::lock_guard lock(impl->mutex);
        erase_if(impl->readers, [](const auto& weak) { auto item = weak.lock(); return !item || item->closed; });
        impl->readers.append(state);
    }
    return {std::move(entry), std::move(reader), ReadConsistency::DetectChanges, {}};
}
class Writer final : public IFileWriter {
public:
    Writer(std::shared_ptr<NativeFileManager::Impl> impl, std::shared_ptr<WriterState> state) : impl_(std::move(impl)), state_(std::move(state)) {}
    ~Writer() override {
        auto state = state_;
        impl_->scheduler->enqueue({state->lane, 0, [state] { std::lock_guard lock(state->ns->mutex); closeWriter(*state); }}, 4096, 64ULL << 20, true);
    }
    QCoro::Task<Result<qsizetype>> writeAt(quint64 offset, QByteArray data, CancellationToken cancel) override {
        auto state = state_;
        if (quint64(data.size()) > state->ns->config.limits.max_io_bytes || offset > state->ns->config.limits.max_file_size || quint64(data.size()) > state->ns->config.limits.max_file_size - offset) co_return Error{ErrorCode::QuotaExceeded};
        auto bytes = quint64(data.size());
        co_return co_await impl_->handleSubmit<qsizetype>(state, [state, offset, data = std::move(data), cancel](Namespace& ns, Catalog& db) -> Result<qsizetype> {
            cancelCheck(cancel); checkPendingFd(*state, db);
            if (data.isEmpty()) return qsizetype(0);
            const auto pending = db.pending(state->id);
            const auto end = offset + quint64(data.size());
            require(!pending.final_size || end <= *pending.final_size, ErrorCode::InvalidArgument);
            const ByteRange affected{offset, quint64(data.size())};
            const auto original_valid=db.ranges(state->id,RangeKind::StorageValid);
            const auto original_verified=db.ranges(state->id,RangeKind::ApplicationVerified);
            auto valid = subtractRange(original_valid, affected);
            auto verified = subtractRange(original_verified, affected);
            auto dirty = addRange(subtractRange(state->dirty, affected), affected);
            auto final_ranges = valid;
            for (auto range : dirty) final_ranges = addRange(std::move(final_ranges), range);
            require(final_ranges.size() <= ns.config.limits.max_ranges && verified.size() <= ns.config.limits.max_ranges && dirty.size() <= ns.config.limits.max_ranges, ErrorCode::QuotaExceeded);
            const auto reserved = db.scalar("SELECT reserved FROM pending WHERE id=?", {uuid(state->id)}).toULongLong();
            const auto next_reserve = std::max(reserved, end); checkQuota(ns, db, next_reserve, reserved);
            if(!state->private_access)poolPendingGrow(ns,state->id,next_reserve);
            if(valid!=original_valid || verified!=original_verified || reserved!=next_reserve || pending.state!=PendingState::Ready) {
                Transaction transaction(db);
                db.putRanges(state->id, RangeKind::StorageValid, valid);
                db.putRanges(state->id, RangeKind::ApplicationVerified, verified);
                db.query("UPDATE pending SET reserved=?,range_generation=range_generation+1,state=0 WHERE id=?", {qlonglong(next_reserve), uuid(state->id)});
                db.bump(); transaction.commit();
            }
            const auto before_write=identity(state->fd.value);
            if(state->prefix_hash && (offset<state->prefix_offset || before_write.dev!=state->prefix_identity.dev ||
                before_write.ino!=state->prefix_identity.ino || before_write.size!=state->prefix_identity.size ||
                before_write.modified!=state->prefix_identity.modified)) {
                state->prefix_hash.reset();state->prefix_offset=0;
            }
            state->dirty = subtractRange(state->dirty, affected);
            fault("after_range_invalidation");
            ssize_t count;
            auto request_size = size_t(data.size());
#ifdef DSFM_FAULT_INJECTION
            bool short_ok = false; const auto short_size = qgetenv("DSFM_SHORT_WRITE").toUInt(&short_ok);
            if (short_ok && short_size) request_size = std::min(request_size, size_t(short_size));
#endif
            fault("before_write");
            { UnlockIo unlocked(ns.mutex);
                do { count = ::pwrite(state->fd.value, data.constData(), request_size, off_t(offset)); } while (count < 0 && errno == EINTR && !cancel.cancelled());
            }
            ioCheck(count >= 0);
            state->dirty = addRange(std::move(state->dirty), {offset, quint64(count)});
            if(state->prefix_hash)state->prefix_identity=identity(state->fd.value);
            return qsizetype(count);
        }, bytes);
    }
    QCoro::Task<Result<void>> setFinalSize(quint64 size) override {
        auto state = state_;
        co_return co_await impl_->handleSubmit<void>(state, [state, size](Namespace& ns, Catalog& db) -> Result<void> {
            checkPendingFd(*state, db); auto pending = db.pending(state->id);
            require(size <= ns.config.limits.max_file_size, ErrorCode::QuotaExceeded);
            require(!pending.content.expected_size || *pending.content.expected_size == size, ErrorCode::InvalidArgument);
            require(identity(state->fd.value).size <= size, ErrorCode::InvalidArgument);
            const auto old = db.scalar("SELECT reserved FROM pending WHERE id=?", {uuid(state->id)}).toULongLong(); checkQuota(ns, db, size, old);
            if(!state->private_access)poolPendingGrow(ns,state->id,size);
            db.query("UPDATE pending SET final_size=?,reserved=?,range_generation=range_generation+1 WHERE id=?", {qlonglong(size), qlonglong(size), uuid(state->id)}); db.bump(); return {};
        });
    }
    QCoro::Task<Result<void>> markVerified(ByteRange range) override {
        auto state = state_;
        co_return co_await impl_->handleSubmit<void>(state, [state, range](Namespace& ns, Catalog& db) -> Result<void> {
            require(range.offset <= ns.config.limits.max_file_size && range.length <= ns.config.limits.max_file_size - range.offset, ErrorCode::InvalidArgument);
            checkpointWriter(*state, db);
            require(covers(db.ranges(state->id, RangeKind::StorageValid), range), ErrorCode::IncompleteContent);
            auto verified = addRange(db.ranges(state->id, RangeKind::ApplicationVerified), range);
            require(verified.size() <= ns.config.limits.max_ranges, ErrorCode::QuotaExceeded);
            Transaction transaction(db); db.putRanges(state->id, RangeKind::ApplicationVerified, verified);
            db.query("UPDATE pending SET range_generation=range_generation+1 WHERE id=?", {uuid(state->id)}); db.bump(); transaction.commit(); return {};
        });
    }
    QCoro::Task<Result<void>> checkpoint(CancellationToken cancel) override {
        auto state = state_;
        co_return co_await impl_->handleSubmit<void>(state, [state, cancel](Namespace&, Catalog& db) -> Result<void> { cancelCheck(cancel); checkpointWriter(*state, db); return {}; });
    }
    QCoro::Task<Result<void>> suspend(CancellationToken cancel) override {
        auto state = state_;
        co_return co_await impl_->handleSubmit<void>(state, [state, cancel](Namespace&, Catalog& db) -> Result<void> {
            cancelCheck(cancel); checkpointWriter(*state, db); db.query("UPDATE pending SET state=1 WHERE id=?", {uuid(state->id)}); db.bump(); closeWriter(*state); return {};
        });
    }
    QCoro::Task<Result<void>> discardSuffix(quint64 length,CancellationToken cancel) override {
        auto state=state_;
        co_return co_await impl_->handleSubmit<void>(state,[state,length,cancel](Namespace &ns,Catalog &db)->Result<void> {
            cancelCheck(cancel);checkPendingFd(*state,db);
            require(length<=ns.config.limits.max_file_size && length<=identity(state->fd.value).size,ErrorCode::InvalidArgument);
            auto valid=db.ranges(state->id,RangeKind::StorageValid);
            require(covers(valid,{0,length}),ErrorCode::IncompleteContent);
            const ByteRange suffix{length,ns.config.limits.max_file_size-length};
            auto verified=subtractRange(db.ranges(state->id,RangeKind::ApplicationVerified),suffix);
            valid=subtractRange(valid,suffix);
            {
                Transaction tx(db);db.putRanges(state->id,RangeKind::StorageValid,valid);
                db.putRanges(state->id,RangeKind::ApplicationVerified,verified);
                db.query("UPDATE pending SET range_generation=range_generation+1 WHERE id=?",{uuid(state->id)});
                db.bump();tx.commit();
            }
            state->dirty=subtractRange(state->dirty,suffix);
            const auto before=identity(state->fd.value);
            if(state->prefix_hash && (state->prefix_offset>length || state->prefix_identity.dev!=before.dev ||
                state->prefix_identity.ino!=before.ino || state->prefix_identity.size!=before.size || state->prefix_identity.modified!=before.modified)) {
                state->prefix_hash.reset();state->prefix_offset=0;
            }
            fault("after_suffix_invalidation");
            ioCheck(::ftruncate(state->fd.value,off_t(length))==0);ioCheck(::fsync(state->fd.value)==0);
            if(state->prefix_hash)state->prefix_identity=identity(state->fd.value);
            if(!state->private_access)poolPendingObserve(ns,state->id,state->fd.value);
            return {};
        });
    }
    QCoro::Task<Result<QByteArray>> hashPrefix(quint64 length, CancellationToken cancel) override;
    QCoro::Task<Result<CommitResult>> commit(CancellationToken cancel) override;
    QCoro::Task<Result<void>> abort(CancellationToken cancel) override {
        auto state = state_;
        co_return co_await impl_->handleSubmit<void>(state, [state, cancel](Namespace& ns, Catalog& db) -> Result<void> {
            cancelCheck(cancel); checkPendingFd(*state, db, false, true);
            const auto st = identity(state->fd.value);
            const auto pending = db.pending(state->id);
            QJsonObject op{{"kind", "abort"}, {"file_id", uuid(pending.file_id)}, {"pending_id", uuid(state->id)},
                {"from", pendingPath(state->id)}, {"to", QStringLiteral(".filemanager/recovery/") + uuid(state->id)},
                {"dev", QString::number(st.dev)}, {"ino", QString::number(st.ino)}, {"directory", false}};
            if(ns.config.storage_pool && !state->private_access)op["pool_reservation"]=uuid(state->id);
        applyOperation(ns, db, op); closeWriter(*state); return {};
        });
    }
private:
    std::shared_ptr<NativeFileManager::Impl> impl_;
    std::shared_ptr<WriterState> state_;
};
PendingFile attachWriter(std::shared_ptr<NativeFileManager::Impl> impl, std::shared_ptr<Namespace> ns, Catalog& db, PendingId id) {
    require(!ns->active_writers.contains(id), ErrorCode::Busy);
    require(ns->active_writers.size() < ns->config.limits.max_writers && ns->handles < ns->config.limits.max_handles, ErrorCode::QuotaExceeded);
    auto pending = db.pending(id); require(pending.state != PendingState::Inconsistent, ErrorCode::ExternalModification);
    auto state = std::make_shared<WriterState>();
    state->private_kind=db.scalar("SELECT private FROM pending WHERE id=?",{uuid(id)}).toInt();state->private_access=state->private_kind!=0; state->ns = ns; state->id = id;
    state->fd = openBeneath(ns->root.value, pendingPath(id), O_RDWR | O_NONBLOCK);
    checkPendingFd(*state, db);
    ns->active_writers.insert(id); ++ns->handles;
    { std::lock_guard lock(impl->mutex);
        erase_if(impl->writers, [](const auto& weak) { auto item = weak.lock(); return !item || item->closed; });
        impl->writers.append(state);
    }
    return {pending, std::make_unique<Writer>(impl, state)};
}
QJsonObject physicalOperation(Namespace& ns, Catalog& db, const QString& kind, const FileInfo& entry, const QString& to) {
    QJsonObject op{{"kind", kind}, {"file_id", uuid(entry.id)}, {"parent", entry.parent ? uuid(*entry.parent) : QString{}}, {"name", entry.name}, {"directory", entry.type == EntryType::Directory}, {"size", QString::number(entry.size)}};
    if(kind=="delete" && ns.config.storage_pool)op["pool_reservation"]=db.scalar("SELECT reservation FROM pool_objects WHERE file_id=?",{uuid(entry.id)}).toString();
    const bool catalog_only = ns.config.naming == PhysicalNaming::FileId && (entry.type == EntryType::Directory || kind == "move" || kind == "rename");
    op["catalog_only"] = catalog_only;
    if (!catalog_only) {
        const auto from = db.path(entry.id, ns.config.naming, ns.config.limits.max_depth);
        auto fd = openBeneath(ns.root.value, from, O_RDONLY | O_NONBLOCK | (entry.type == EntryType::Directory ? O_DIRECTORY : 0));
        const auto stat = identity(fd.value, entry.type == EntryType::Directory);
        op["from"] = from; op["to"] = to;
        op["dev"] = QString::number(stat.dev); op["ino"] = QString::number(stat.ino); op["mtime"] = QString::number(stat.modified);
    }
    return op;
}
void noReservations(Namespace& ns, Catalog& db, FileId id) {
    auto current = id;
    for (quint32 depth = 0; depth <= ns.config.limits.max_depth; ++depth) {
        if (db.scalar("SELECT id FROM pending WHERE file_id=? LIMIT 1", {uuid(current)}).isValid()) fail(ErrorCode::Busy);
        auto entry = db.entry(current); if (!entry.parent) return; current = *entry.parent;
    }
    fail(ErrorCode::Corruption);
}
void noDescendantReservations(Namespace& ns, Catalog& db, FileId id) {
    // Bounded depth recursion in SQLite; count short-circuits after one reservation.
    auto q = db.query("WITH RECURSIVE tree(id,depth) AS (SELECT ?,0 UNION ALL SELECT e.id,t.depth+1 FROM entries e JOIN tree t ON e.parent=t.id WHERE t.depth<?) SELECT p.id FROM pending p JOIN tree t ON p.parent=t.id OR p.file_id=t.id LIMIT 1", {uuid(id), ns.config.limits.max_depth});
    require(!q.next(), ErrorCode::Busy);
}
} // namespace

namespace {
struct PrefixVerification {
    Identity before;
    std::unique_ptr<QCryptographicHash> hash;
    quint64 offset=0;
};
struct Verification {
    PendingInfo pending;
    Identity before;
    QCryptographicHash hash{QCryptographicHash::Sha256};
    QByteArray cached_digest;
    quint64 offset = 0;
};
}
QCoro::Task<Result<QByteArray>> Writer::hashPrefix(quint64 length, CancellationToken cancel) {
    auto state = state_;
    auto impl = impl_;
    auto prepared = co_await impl->handleSubmit<std::shared_ptr<PrefixVerification>>(
        state, [state, length, cancel](Namespace &ns, Catalog &db) -> Result<std::shared_ptr<PrefixVerification>> {
            cancelCheck(cancel);
            require(length <= ns.config.limits.max_file_size, ErrorCode::InvalidArgument);
            checkPendingFd(*state, db);
            auto ranges = db.ranges(state->id, RangeKind::StorageValid);
            for (auto range : state->dirty)
                ranges = addRange(std::move(ranges), range);
            require(covers(ranges, {0, length}), ErrorCode::IncompleteContent);
            checkpointWriter(*state, db);
            require(covers(db.ranges(state->id, RangeKind::StorageValid), {0, length}), ErrorCode::IncompleteContent);
            auto v = std::make_shared<PrefixVerification>();
            v->before = identity(state->fd.value);
            require(length <= v->before.size, ErrorCode::IncompleteContent);
            if(state->prefix_hash && state->prefix_offset<=length &&
                state->prefix_identity.dev==v->before.dev && state->prefix_identity.ino==v->before.ino &&
                state->prefix_identity.size==v->before.size && state->prefix_identity.modified==v->before.modified) {
                v->hash=std::move(state->prefix_hash);v->offset=state->prefix_offset;
            } else {state->prefix_hash.reset();state->prefix_offset=0;v->hash=std::make_unique<QCryptographicHash>(QCryptographicHash::Sha256);}
            state->verifying = true;
            return v;
        });
    if (!prepared)
        co_return prepared.error();
    auto v = prepared.value();
    Result<QByteArray> result = Error{ErrorCode::Cancelled};
    std::optional<Error> failure;
    while (v->offset < length) {
        auto part = co_await impl->handleSubmit<void>(
            state, [state, v, length, cancel](Namespace &ns, Catalog &db) -> Result<void> {
                cancelCheck(cancel);
                checkPendingFd(*state, db, true);
                const auto amount = std::min<quint64>(ns.config.limits.max_io_bytes, length - v->offset);
                QByteArray bytes(qsizetype(amount), Qt::Uninitialized);
                fault("before_prefix_read");
                ssize_t got = 0;
                {
                    UnlockIo unlocked(ns.mutex);
                    do {
                        got = ::pread(state->fd.value, bytes.data(), size_t(amount), off_t(v->offset));
                    } while (got < 0 && errno == EINTR && !cancel.cancelled());
                    ioCheck(got >= 0);
                    require(got > 0, ErrorCode::ExternalModification);
                    v->hash->addData(QByteArrayView(bytes.constData(), got));
                }
                v->offset += quint64(got);
                return {};
            });
        if (!part) {
            failure = part.error();
            break;
        }
    }
    if (failure)
        result = *failure;
    else
        result = co_await impl->handleSubmit<QByteArray>(
            state, [state, v, cancel](Namespace &, Catalog &db) -> Result<QByteArray> {
                cancelCheck(cancel);
                checkPendingFd(*state, db, true);
                const auto after = identity(state->fd.value);
                require(after.dev == v->before.dev && after.ino == v->before.ino && after.size == v->before.size &&
                            after.modified == v->before.modified,
                        ErrorCode::ExternalModification);
                return v->hash->result();
            });
    auto cleared =
        co_await execute<void>(impl->scheduler, state->lane, 0, 4096, 64ULL << 20, [state,v,success=bool(result)]() -> Result<void> {
            std::lock_guard lock(state->ns->mutex);
            if(success && !state->closed && !state->expired) {
                state->prefix_hash=std::move(v->hash);state->prefix_offset=v->offset;state->prefix_identity=v->before;
            } else {state->prefix_hash.reset();state->prefix_offset=0;}
            state->verifying = false;
            return {};
        });
    Q_UNUSED(cleared);
    co_return result;
}
QCoro::Task<Result<CommitResult>> Writer::commit(CancellationToken cancel) {
    auto state = state_;
    auto impl = impl_;
    auto prepared = co_await impl->handleSubmit<std::shared_ptr<Verification>>(
        state, [state, cancel](Namespace &ns, Catalog &db) -> Result<std::shared_ptr<Verification>> {
            cancelCheck(cancel);
            checkpointWriter(*state, db);
            auto pending = db.pending(state->id);
            require(pending.final_size.has_value(), ErrorCode::IncompleteContent);
            require(covers(db.ranges(state->id, RangeKind::StorageValid), {0, *pending.final_size}),
                    ErrorCode::IncompleteContent);
            if (pending.replacement)
                require(ns.readers.value(pending.file_id) == 0, ErrorCode::Busy);
            auto verification = std::make_shared<Verification>();
            verification->pending = pending;
            verification->before = identity(state->fd.value);
            require(verification->before.size == *pending.final_size, ErrorCode::IncompleteContent);
            if(state->prefix_hash && state->prefix_offset==*pending.final_size &&
                state->prefix_identity.dev==verification->before.dev && state->prefix_identity.ino==verification->before.ino &&
                state->prefix_identity.size==verification->before.size && state->prefix_identity.modified==verification->before.modified)
                verification->cached_digest=state->prefix_hash->result();
            state->verifying = true;
            return verification;
        });
    if (!prepared) co_return prepared.error();
    auto verification = prepared.value();
    std::optional<Error> failure;
    // Each hashing chunk releases its worker/namespace turn before admitting the next.
    while (verification->cached_digest.isEmpty() && verification->offset < verification->before.size) {
        auto chunk = co_await impl->handleSubmit<void>(state, [state, verification, cancel](Namespace& ns, Catalog&) -> Result<void> {
            cancelCheck(cancel); require(!state->expired,ErrorCode::MissingEntry);require(!state->closed && state->verifying, ErrorCode::Busy);
            const auto count = std::min<quint64>(ns.config.limits.max_io_bytes, verification->before.size - verification->offset);
            QByteArray buffer(qsizetype(count), Qt::Uninitialized);
            ssize_t got;
            { UnlockIo unlocked(ns.mutex);
                do { got = ::pread(state->fd.value, buffer.data(), size_t(count), off_t(verification->offset)); } while (got < 0 && errno == EINTR && !cancel.cancelled());
                ioCheck(got >= 0); require(got > 0, ErrorCode::ExternalModification);
                verification->hash.addData(QByteArrayView(buffer.constData(), got));
            }
            verification->offset += quint64(got); return {};
        });
        if (!chunk) { failure = chunk.error(); break; }
    }
    Result<CommitResult> result = Error{ErrorCode::Cancelled};
    if (failure) result = *failure;
    else result = co_await impl->handleSubmit<CommitResult>(state, [state, verification, cancel](Namespace& ns, Catalog& db) -> Result<CommitResult> {
        cancelCheck(cancel); checkPendingFd(*state, db, true);
        auto pending = verification->pending;
        const auto stat = identity(state->fd.value);
        require(stat.dev == verification->before.dev && stat.ino == verification->before.ino && stat.size == verification->before.size && stat.modified == verification->before.modified, ErrorCode::ExternalModification);
        const auto expected = verification->cached_digest.isEmpty()?verification->hash.result():verification->cached_digest;
        if (!pending.content.expected_sha256.isEmpty() && expected != pending.content.expected_sha256) {
            db.query("UPDATE pending SET state=2 WHERE id=?", {uuid(state->id)}); db.bump(); fail(ErrorCode::IntegrityError);
        }
        QString destination;
        if(state->private_access) destination=QStringLiteral(".filemanager/artifacts/")+uuid(pending.file_id);
        else if (ns.config.naming == PhysicalNaming::FileId) {
            const auto id = uuid(pending.file_id);
            const auto prefix = QStringLiteral("objects/") + id.left(2);
            try { auto dir = openBeneath(ns.root.value, prefix, O_RDONLY | O_DIRECTORY); identity(dir.value, true); }
            catch (const Failure& error) { if (error.error.code != ErrorCode::MissingEntry) throw; makeDirectory(ns.root.value, prefix); }
            destination = prefix + '/' + id;
        } else {
            const auto parent = db.path(pending.parent, ns.config.naming, ns.config.limits.max_depth);
            destination = parent == "." ? pending.name : parent + '/' + pending.name;
        }
        QJsonObject op{{"kind", state->private_access ? "artifact" : pending.replacement ? "replace" : "create"}, {"file_id", uuid(pending.file_id)}, {"pending_id", uuid(pending.id)},
            {"parent", uuid(pending.parent)}, {"name", pending.name}, {"from", pendingPath(pending.id)}, {"to", destination},
            {"dev", QString::number(stat.dev)}, {"ino", QString::number(stat.ino)}, {"mtime", QString::number(stat.modified)},
            {"size", QString::number(*pending.final_size)}, {"sha", QString::fromLatin1(expected.toHex())}, {"directory", false}};
        if (pending.replacement) {
            const auto entry = db.entry(pending.file_id); validateEntry(ns, db, entry);
            auto old = openBeneath(ns.root.value, destination, O_RDONLY | O_NONBLOCK); const auto previous = identity(old.value);
            op["old_dev"] = QString::number(previous.dev); op["old_ino"] = QString::number(previous.ino);
        }
        if(ns.config.storage_pool && !state->private_access) {
            poolMetadata(ns);poolPendingObserve(ns,state->id,state->fd.value);
            requirePool(PoolAccess::reclassify(*ns.config.storage_pool,state->id,ns.config.id,StorageClass::Permanent));
            op["pool_reservation"]=uuid(state->id);
            if(pending.replacement)op["old_pool_reservation"]=db.scalar("SELECT reservation FROM pool_objects WHERE file_id=?",{uuid(pending.file_id)}).toString();
        }
        cancelCheck(cancel); // No cancellation after the durable operation intent.
        auto result = applyOperation(ns, db, op);
        closeWriter(*state);
        return result;
    });
    // Use reserved cleanup admission even if normal queues are full or shutdown began.
    auto cleared = co_await execute<void>(impl->scheduler, state->lane, 0, 4096, 64ULL << 20, [state]() -> Result<void> {
        std::lock_guard lock(state->ns->mutex); state->verifying = false; return {};
    });
    Q_UNUSED(cleared);
    co_return result;
}

NativeFileManager::NativeFileManager() : impl_(std::make_shared<Impl>()) {}
NativeFileManager::~NativeFileManager() {
    std::lock_guard lock(impl_->mutex);
    for (auto& [id, sub] : impl_->subscribers) { Q_UNUSED(id); sub->active = false; }
    impl_->subscribers.clear();
}
namespace {
Result<NamespaceInfo> acquireNamespace(std::shared_ptr<NativeFileManager::Impl> impl, NamespaceConfig config, bool create, CancellationToken cancel) {
    PermissionRepairScope permissions(config.repair_permissions);
    cancelCheck(cancel); require(!impl->shutting_down, ErrorCode::ShuttingDown);
    require(!config.id.isNull() && (config.naming == PhysicalNaming::OriginalName || config.naming == PhysicalNaming::FileId), ErrorCode::InvalidArgument);
    validateLimits(config.limits);
    {
        std::lock_guard lock(impl->mutex);
        require(!impl->spaces.contains(config.id), ErrorCode::NamespaceBusy);
        require(impl->spaces.size() < 32, ErrorCode::QuotaExceeded);
    }
    auto ns = std::make_shared<Namespace>(); ns->config = config;
    QVector<std::shared_ptr<Fd>> ancestor_locks;
    try { ns->root = openRoot(config.storage_root, false); }
    catch (const Failure& failure) {
        if (!create || failure.error.code != ErrorCode::MissingEntry) throw;
        auto parent = openRoot(parentPath(config.storage_root), false, false);
        ancestor_locks = lockRoots(parent.value, false);
        ns->root = openRoot(config.storage_root, true);
    }
    ns->locks = lockRoots(ns->root.value);
    if (create) {
        Fd duplicate(::fcntl(ns->root.value, F_DUPFD_CLOEXEC, 0));
        auto* directory = ::fdopendir(std::exchange(duplicate.value, -1)); ioCheck(directory != nullptr);
        bool empty = true;
        while (auto* entry = ::readdir(directory)) if (QByteArray(entry->d_name) != "." && QByteArray(entry->d_name) != "..") { empty = false; break; }
        ::closedir(directory); require(empty, ErrorCode::Conflict);
        if (config.storage_pool) {
            auto pool = PoolAccess::registerNamespace(*config.storage_pool, config.id, config.storage_root);
            if (!pool) throw Failure{pool.error()};
            ns->pool_id = pool.value();
            requirePool(PoolAccess::reserve(*config.storage_pool,
                {metadataClaim(*ns), ns->pool_id, config.id, config.id, config.id, StorageClass::Permanent,
                 nativeMetadataBound(*ns,1), 0}));
        }
        makeDirectory(ns->root.value, ".filemanager");
        makeDirectory(ns->root.value, ".filemanager/artifacts");
        makeDirectory(ns->root.value, ".filemanager/staging"); makeDirectory(ns->root.value, ".filemanager/recovery");
        if (config.naming == PhysicalNaming::FileId) makeDirectory(ns->root.value, "objects");
    }
    ns->internal = openBeneath(ns->root.value, ".filemanager", O_RDONLY | O_DIRECTORY); identity(ns->internal.value, true, "internal directory");
    ns->owner_lock = openBeneath(ns->internal.value, "owner.lock", O_RDWR | O_CREAT, 0600); identity(ns->owner_lock.value, false, "owner lock");
    if (::flock(ns->owner_lock.value, LOCK_EX | LOCK_NB) < 0) fail(ErrorCode::NamespaceBusy, errno);
    {
        auto file = openBeneath(ns->internal.value, "catalog.db", create ? O_RDWR | O_CREAT | O_EXCL : O_RDWR, create ? 0600 : 0); identity(file.value, false, "catalog");
    }
    Catalog db(ns->catalogPath(), config.limits.max_catalog_bytes, create);
    const auto stored_pool = QUuid(db.meta("pool"));
    require(stored_pool.isNull() || bool(config.storage_pool), ErrorCode::StorageUnavailable);
    if (config.storage_pool) {
        auto registered = PoolAccess::registerNamespace(*config.storage_pool, config.id, config.storage_root);
        if (!registered) throw Failure{registered.error()};
        require(stored_pool.isNull() || stored_pool == registered.value(), ErrorCode::Conflict);
        ns->pool_id = registered.value();
        db.setMeta("pool", uuid(ns->pool_id));
    }
    if (create) {
        ns->root_id = QUuid::createUuid();
        Transaction transaction(db);
        db.setMeta("namespace", uuid(config.id)); db.setMeta("root", uuid(ns->root_id)); db.setMeta("naming", QString::number(int(config.naming)));
        db.setMeta("directory_count","1");
        db.setMeta("name_policy", "1"); db.setMeta("unicode", QString::number(int(QChar::currentUnicodeVersion())));
        db.setMeta("limits", QString::fromUtf8(QJsonDocument(limitsJson(config.limits)).toJson(QJsonDocument::Compact)));
        const auto now = QDateTime::currentMSecsSinceEpoch();
        db.query("INSERT INTO entries(id,parent,name,name_key,type,created,modified,generation) VALUES(?,NULL,'','',1,?,?,0)", {uuid(ns->root_id), now, now});
        transaction.commit();
        syncDirectory(ns->root.value, ".filemanager");
    } else {
        require(db.meta("namespace") == uuid(config.id) && db.meta("naming") == QString::number(int(config.naming)), ErrorCode::Conflict);
        require(db.meta("name_policy") == "1" && db.meta("unicode") == QString::number(int(QChar::currentUnicodeVersion())), ErrorCode::UnsupportedCapability);
        ns->root_id = QUuid(db.meta("root")); require(!ns->root_id.isNull(), ErrorCode::Corruption);
        ns->config.limits = readLimits(QJsonDocument::fromJson(db.meta("limits").toUtf8()).object());
        if (config.limits_override) {
            validateLimits(*config.limits_override);
            ns->config.limits = *config.limits_override;
            ns->config.limits_override.reset();
            db.setMeta("limits", QString::fromUtf8(QJsonDocument(limitsJson(ns->config.limits)).toJson(QJsonDocument::Compact)));
        }
        recover(*ns, db);
        // Staging records are retained; validate in bounded SQL pages, without reading their contents.
        QString last;
        for (;;) {
            auto q = db.query("SELECT id,dev,ino FROM pending WHERE private=0 AND id>? ORDER BY id LIMIT ?", {last, ns->config.limits.max_batch});
            quint32 count = 0;
            while (q.next()) {
                cancelCheck(cancel); last = q.value(0).toString(); ++count;
                try {
                    auto fd = openBeneath(ns->root.value, pendingPath(QUuid(last)), O_RDWR | O_NONBLOCK); const auto found = identity(fd.value);
                    require(found.dev == q.value(1).toULongLong() && found.ino == q.value(2).toULongLong(), ErrorCode::ExternalModification);
                } catch (const Failure&) { db.query("UPDATE pending SET state=3 WHERE id=?", {last}); }
            }
            if (count < ns->config.limits.max_batch) break;
        }
    }
    {
        QSet<QString> known;
        auto q = db.query("SELECT id FROM pending LIMIT 12289");
        while (q.next()) { require(known.size() < 12288, ErrorCode::Corruption); known.insert(q.value(0).toString()); }
        auto fd = openBeneath(ns->root.value, ".filemanager/staging", O_RDONLY | O_DIRECTORY);
        auto* directory = ::fdopendir(std::exchange(fd.value, -1)); ioCheck(directory != nullptr);
        quint32 scanned = 0;
        while (auto* item = ::readdir(directory)) {
            const auto name = QString::fromUtf8(item->d_name);
            if (name == "." || name == "..") continue;
            if (++scanned > ns->config.limits.max_pending + ns->config.limits.max_temporary_objects + ns->config.limits.max_cache_objects + ns->config.limits.max_batch) { ns->staging_scan_truncated = true; break; }
            if (!known.contains(name)) ++ns->untracked_staging;
        }
        ::closedir(directory);
    }
    ns->directory_count = db.meta("directory_count").toULongLong();
    if(ns->config.storage_pool) {
        auto own_db=openBeneath(ns->internal.value,"catalog.db",O_RDONLY|O_NONBLOCK);
        const auto meta=physicalBytes(own_db.value)+physicalBytes(ns->internal.value)+physicalBytes(ns->root.value);
        requirePool(PoolAccess::reconcileAllocated(*ns->config.storage_pool,{metadataClaim(*ns),ns->pool_id,config.id,config.id,config.id,StorageClass::Permanent,meta,meta}));
        auto entries=db.query("SELECT id,type FROM entries WHERE type=0 ORDER BY id");
        while(entries.next()) {
            const auto file=QUuid(entries.value(0).toString());auto fd=openBeneath(ns->root.value,db.path(file,ns->config.naming,ns->config.limits.max_depth),O_RDONLY|O_NONBLOCK);
            auto claim=QUuid(db.scalar("SELECT reservation FROM pool_objects WHERE file_id=?",{uuid(file)}).toString());if(claim.isNull()){claim=QUuid::createUuid();db.query("INSERT INTO pool_objects VALUES(?,?)",{uuid(file),uuid(claim)});}
            auto actual=physicalBytes(fd.value);requirePool(PoolAccess::reconcileAllocated(*ns->config.storage_pool,{claim,ns->pool_id,config.id,config.id,claim,StorageClass::Permanent,actual,actual}));
        }
        auto pending=db.query("SELECT id,reserved FROM pending WHERE private=0 ORDER BY id");
        while(pending.next()) {
            const auto id=QUuid(pending.value(0).toString());auto fd=openBeneath(ns->root.value,pendingPath(id),O_RDONLY|O_NONBLOCK);auto actual=physicalBytes(fd.value);
            requirePool(PoolAccess::reconcileAllocated(*ns->config.storage_pool,{id,ns->pool_id,config.id,config.id,id,StorageClass::Incoming,std::max(actual,poolEstimate(pending.value(1).toULongLong())),actual}));
            // Persisted promises remain charged across restarts and quota reductions.
        }

    }
    recoverPoolClaims(*ns, db);
    LFLOG_DEBUG << "FileManager namespace ready; naming=" << int(config.naming) << ", recovery complete";
    auto info = infoFor(*ns, db);
    { std::lock_guard lock(impl->mutex); require(!impl->shutting_down, ErrorCode::ShuttingDown); impl->spaces.insert(config.id, ns); }
    return info;
}
}
QCoro::Task<Result<NamespaceInfo>> NativeFileManager::createNamespace(NamespaceConfig config, CancellationToken cancel) {
    if (impl_->shutting_down) co_return Error{ErrorCode::ShuttingDown};
    if (config.storage_root.isEmpty() || config.storage_root.size() > 32768) co_return Error{ErrorCode::InvalidArgument};
    auto impl = impl_; const auto key = config.id;
    co_return co_await execute<NamespaceInfo>(impl->scheduler, key, 0, 128, 8ULL << 20,
        [impl, config = std::move(config), cancel] { return acquireNamespace(impl, config, true, cancel); });
}
QCoro::Task<Result<NamespaceInfo>> NativeFileManager::openNamespace(NamespaceConfig config, CancellationToken cancel) {
    if (impl_->shutting_down) co_return Error{ErrorCode::ShuttingDown};
    if (config.storage_root.isEmpty() || config.storage_root.size() > 32768) co_return Error{ErrorCode::InvalidArgument};
    auto impl = impl_; const auto key = config.id;
    co_return co_await execute<NamespaceInfo>(impl->scheduler, key, 0, 128, 8ULL << 20,
        [impl, config = std::move(config), cancel] { return acquireNamespace(impl, config, false, cancel); });
}
QCoro::Task<Result<NamespaceInfo>> NativeFileManager::namespaceInfo(NamespaceId ns) {
    return impl_->submit<NamespaceInfo>(ns, [](Namespace& space, Catalog& db) -> Result<NamespaceInfo> { return infoFor(space, db); });
}
QCoro::Task<Result<QVector<NamespaceInfo>>> NativeFileManager::namespaces() {
    auto impl = impl_;
    if (impl->shutting_down) co_return Error{ErrorCode::ShuttingDown};
    co_return co_await execute<QVector<NamespaceInfo>>(impl->scheduler, {}, 0, 128, 8ULL << 20, [impl]() -> Result<QVector<NamespaceInfo>> {
        QVector<std::shared_ptr<Namespace>> spaces;
        { std::lock_guard lock(impl->mutex); for (auto& ns : impl->spaces) spaces.append(ns); }
        QVector<NamespaceInfo> result;
        for (auto& ns : spaces) {
            std::lock_guard lock(ns->mutex);
            PermissionRepairScope permissions(ns->config.repair_permissions);
            Catalog db(ns->catalogPath(), ns->config.limits.max_catalog_bytes, false);
            result.append(infoFor(*ns, db));
        }
        return result;
    });
}
QCoro::Task<Result<FileInfo>> NativeFileManager::stat(NamespaceId ns, QUuid id) {
    return impl_->submit<FileInfo>(ns, [id](Namespace& space, Catalog& db) -> Result<FileInfo> {
        auto info = db.entry(id);
        try { validateEntry(space, db, info); } catch (const Failure&) { return db.entry(id); }
        return info;
    });
}
QCoro::Task<Result<FileInfo>> NativeFileManager::lookup(NamespaceId ns, QString logical_path) {
    if (logical_path.size() > 32768) return rejected<FileInfo>(ErrorCode::InvalidArgument);
    return impl_->submit<FileInfo>(ns, [logical_path = std::move(logical_path)](Namespace& space, Catalog& db) -> Result<FileInfo> {
        require(logical_path.size() <= 32768 && !logical_path.startsWith('/'), ErrorCode::InvalidArgument);
        auto id = space.root_id;
        if (!logical_path.isEmpty()) {
            const auto components = logical_path.split('/'); require(components.size() <= space.config.limits.max_depth, ErrorCode::QuotaExceeded);
            for (auto component : components) {
                auto key = nameKey(component); if (!key) throw Failure{key.error()};
                auto found = db.scalar("SELECT id FROM entries WHERE parent=? AND name_key=?", {uuid(id), key.value()}); require(found.isValid(), ErrorCode::MissingEntry); id = uuid(found);
            }
        }
        auto info = db.entry(id);
        if (info.status != EntryStatus::Updating) validateEntry(space, db, info);
        return info;
    });
}
QCoro::Task<Result<DirectoryPage>> NativeFileManager::list(NamespaceId ns, DirectoryId parent, PageRequest page) {
    if (!validPageEnvelope(page)) return rejected<DirectoryPage>(ErrorCode::InvalidArgument);
    return impl_->submit<DirectoryPage>(ns, [parent, page = std::move(page)](Namespace& space, Catalog& db) -> Result<DirectoryPage> {
        checkBrowseDirectory(space, db, parent);
        const auto scope = (page.stable_order ? (page.ascii_order ? "directory-ascii:" : "directory-ordered:") : "directory:") + uuid(parent); const auto generation = page.stable_order ? 0 : db.generation();
        const auto last = readCursor(page, generation, scope, space.config.limits.max_page_size);
        const auto separator = last.lastIndexOf(QChar::Null);
        const auto last_key = separator < 0 ? QString{} : last.left(separator);
        const auto key = page.stable_order ? (page.ascii_order ? QStringLiteral("dsfm_ascii(name) COLLATE BINARY") : QStringLiteral("name COLLATE BINARY")) : QStringLiteral("name_key");
        // Names (and their injective ASCII presentation) are unique per parent;
        // a simple key range lets SQLite seek instead of scanning earlier pages.
        auto q = db.query(QStringLiteral("SELECT id,%1 FROM entries WHERE parent=? AND status IN(?,?) AND %1>? ORDER BY %1,id LIMIT ?").arg(key),
            {uuid(parent), int(EntryStatus::Available), int(EntryStatus::Updating), last_key, page.limit + 1});
        DirectoryPage result; QString next;
        while (q.next()) {
            if (result.entries.size() == page.limit) { result.next_cursor = cursor(generation, scope, next); break; }
            result.entries.append(db.entry(uuid(q.value(0)))); next = q.value(1).toString() + QChar::Null + q.value(0).toString();
        }
        return result;
    });
}
QCoro::Task<Result<DirectoryId>> NativeFileManager::mkdir(NamespaceId ns, DirectoryId parent, QString name) {
    if (name.isEmpty() || name.size() > 255) return rejected<DirectoryId>(ErrorCode::InvalidName);
    return impl_->submit<DirectoryId>(ns, [parent, name = std::move(name)](Namespace& space, Catalog& db) -> Result<DirectoryId> {
        checkParent(space, db, parent);
        require(db.scalar("SELECT (SELECT count(*) FROM entries)+(SELECT count(*) FROM pending WHERE replacement=0 AND private=0)").toULongLong() < space.config.limits.max_entries, ErrorCode::QuotaExceeded);
        const auto assigned = chooseName(space, db, parent, name, ConflictPolicy::Fail);
        poolMetadata(space,&db,1);
        const auto id = QUuid::createUuid();
        QJsonObject op{{"kind", "mkdir"}, {"file_id", uuid(id)}, {"parent", uuid(parent)}, {"name", assigned}, {"directory", true}, {"size", "0"}, {"catalog_only", space.config.naming == PhysicalNaming::FileId}};
        if (space.config.naming == PhysicalNaming::OriginalName) {
            const auto staging = QStringLiteral(".filemanager/staging/") + uuid(id); makeDirectory(space.root.value, staging);
            auto fd = openBeneath(space.root.value, staging, O_RDONLY | O_DIRECTORY); const auto st = identity(fd.value, true);
            const auto path = db.path(parent, space.config.naming, space.config.limits.max_depth);
            op["from"] = staging; op["to"] = path == "." ? assigned : path + '/' + assigned;
            op["dev"] = QString::number(st.dev); op["ino"] = QString::number(st.ino); op["mtime"] = QString::number(st.modified);
        }
        applyOperation(space, db, op); return id;
    });
}
namespace {
PendingId reservePending(Namespace& ns, Catalog& db, DirectoryId parent, QString name, ContentConstraints content, bool replacement, FileId file_id, quint64 private_limit = 0, int private_kind = 0, PendingId proposed = {}) {
    db.private_class=private_kind;
    poolMetadata(ns,&db);
    checkConstraints(content, ns.config.limits); checkParent(ns, db, parent);
    if (!private_kind) require(db.scalar("SELECT count(*) FROM pending WHERE private=0").toUInt() < ns.config.limits.max_pending, ErrorCode::QuotaExceeded);
    require(ns.handles < ns.config.limits.max_handles && ns.active_writers.size() < ns.config.limits.max_writers, ErrorCode::QuotaExceeded);
    if (!replacement && !private_kind) require(db.scalar("SELECT (SELECT count(*) FROM entries)+(SELECT count(*) FROM pending WHERE replacement=0 AND private=0)").toULongLong() < ns.config.limits.max_entries, ErrorCode::QuotaExceeded);
    const auto reservation = private_kind ? private_limit : content.expected_size.value_or(0); checkQuota(ns, db, reservation);
    const auto id = proposed.isNull() ? QUuid::createUuid() : proposed;
    if(!private_kind)poolPendingReserve(ns,id,reservation);
    auto fd = openBeneath(ns.root.value, pendingPath(id), O_RDWR | O_CREAT | O_EXCL, 0600); const auto st = identity(fd.value);
    ioCheck(::fsync(fd.value) == 0); syncDirectory(ns.root.value, ".filemanager/staging");
    Transaction transaction(db);
    db.query("INSERT INTO pending(id,file_id,parent,name,name_key,expected_size,expected_sha,final_size,replacement,reserved,dev,ino,private) VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?)",
        {uuid(id), uuid(file_id), uuid(parent), name, nameKey(name).value(), content.expected_size ? QVariant::fromValue(qlonglong(*content.expected_size)) : QVariant{},
         content.expected_sha256.isEmpty() ? QVariant{} : QVariant(content.expected_sha256), content.expected_size ? QVariant::fromValue(qlonglong(*content.expected_size)) : QVariant{}, replacement, qlonglong(reservation), QString::number(st.dev), QString::number(st.ino),private_kind});
    db.bump(); transaction.commit(); return id;
}
}
namespace {
TemporaryInfo temporaryInfo(Catalog &db, TempId id, const TemporaryAccess &access, bool finalized = true) {
    require(access.allowed(), ErrorCode::MissingEntry);
    auto q = db.query("SELECT owner,created,expires,state,size,sha,revision FROM temporaries WHERE id=?", {uuid(id)});
    require(q.next() && QUuid(q.value(0).toString()) == access.owner_scope, ErrorCode::MissingEntry);
    const auto now = QDateTime::currentMSecsSinceEpoch();
    require(now >= q.value(1).toLongLong() && now < q.value(2).toLongLong() && q.value(3).toInt() != 2,
            ErrorCode::MissingEntry);
    if (finalized)
        require(q.value(3).toInt() == 1, ErrorCode::Busy);
    return {id, q.value(4).toULongLong(), q.value(5).toByteArray(), q.value(6).toULongLong(),
            QDateTime::fromMSecsSinceEpoch(q.value(2).toLongLong(), QTimeZone::UTC)};
}
quint64 temporaryEstimate(quint64 bytes) {
    constexpr quint64 unit = 65536;
    require(bytes <= quint64(std::numeric_limits<qint64>::max()) / 4 - unit * 2, ErrorCode::QuotaExceeded);
    return ((bytes + unit - 1) / unit) * unit + unit;
}
void poolResult(Result<StorageReservation> result) {
    if (!result)
        throw Failure{result.error()};
}
class TemporaryWriter final : public IFileWriter {
    std::unique_ptr<IFileWriter> writer_;
    std::shared_ptr<NativeFileManager::Impl> impl_;
    NamespaceId ns_;
    TempId id_;
    quint64 maximum_;
    TemporaryAccess access_;
    QCoro::Task<Result<void>> reportPhysical() {
        co_return co_await impl_->submit<void>(
            ns_,
            [id = id_, access = access_](Namespace &ns, Catalog &db) -> Result<void> {
                auto q = db.query("SELECT p.id,p.dev,p.ino FROM pending p JOIN temporaries t ON t.pending=p.id WHERE "
                                  "t.id=? AND t.owner=?",
                                  {uuid(id), uuid(access.owner_scope)});
                require(q.next(), ErrorCode::MissingEntry);
                auto fd = openBeneath(ns.root.value, pendingPath(QUuid(q.value(0).toString())), O_RDONLY | O_NONBLOCK);
                auto evidence = identity(fd.value);
                require(evidence.dev == q.value(1).toULongLong() && evidence.ino == q.value(2).toULongLong(),
                        ErrorCode::ExternalModification);
                struct stat st{};
                ioCheck(::fstat(fd.value, &st) == 0);
                poolResult(PoolAccess::materialize(*ns.config.storage_pool, id, access.owner_scope,
                                                   quint64(st.st_blocks) * 512));
                return {};
            },
            0, true, {}, true);
    }
    QCoro::Task<Result<void>> admit(quint64 end) {
        if (!access_.allowed())
            co_return Error{ErrorCode::MissingEntry};
        if (end > maximum_)
            co_return Error{ErrorCode::QuotaExceeded};
        co_return co_await impl_->submit<void>(
            ns_,
            [id = id_, access = access_, end](Namespace &ns, Catalog &db) -> Result<void> {
                temporaryInfo(db, id, access, false);
                auto q = db.query("SELECT pending FROM temporaries WHERE id=?", {uuid(id)});
                q.next();
                auto old = db.scalar("SELECT reserved FROM pending WHERE id=?", {q.value(0)}).toULongLong();
                poolResult(PoolAccess::resize(*ns.config.storage_pool, id, access.owner_scope,
                                              temporaryEstimate(std::max(old, end))));
                auto allowed=PoolAccess::admitGrowth(*ns.config.storage_pool,id,access.owner_scope);
                if(!allowed)throw Failure{allowed.error()};
                return {};
            },
            0, true, {}, true);
    }

  public:
    TemporaryWriter(std::unique_ptr<IFileWriter> writer, std::shared_ptr<NativeFileManager::Impl> impl, NamespaceId ns,
                    TempId id, quint64 maximum, TemporaryAccess access)
        : writer_(std::move(writer)), impl_(std::move(impl)), ns_(ns), id_(id), maximum_(maximum),
          access_(std::move(access)) {}
    QCoro::Task<Result<qsizetype>> writeAt(quint64 offset, QByteArray data, CancellationToken cancel) override {
        if (offset > maximum_ || quint64(data.size()) > maximum_ - offset)
            co_return Error{ErrorCode::QuotaExceeded};
        auto allowed = co_await admit(offset + quint64(data.size()));
        if (!allowed)
            co_return allowed.error();
        auto result = co_await writer_->writeAt(offset, std::move(data), cancel);
        if (result) {
            auto measured = co_await reportPhysical();
            if (!measured)
                co_return measured.error();
        }
        if (!access_.allowed())
            co_return Error{ErrorCode::MissingEntry};
        co_return result;
    }
    QCoro::Task<Result<void>> setFinalSize(quint64 size) override {
        auto check = co_await admit(size);
        if (!check)
            co_return check.error();
        co_return co_await writer_->setFinalSize(size);
    }
    QCoro::Task<Result<void>> markVerified(ByteRange range) override {
        if (!access_.allowed())
            co_return Error{ErrorCode::MissingEntry};
        auto result = co_await writer_->markVerified(range);
        if (!result)
            co_return result.error();
        co_return co_await reportPhysical();
    }
    QCoro::Task<Result<void>> checkpoint(CancellationToken c) override {
        if (!access_.allowed())
            co_return Error{ErrorCode::MissingEntry};
        auto result = co_await writer_->checkpoint(c);
        if (!result)
            co_return result.error();
        co_return co_await reportPhysical();
    }
    QCoro::Task<Result<void>> discardSuffix(quint64 length,CancellationToken cancel) override {
        if(!access_.allowed())co_return Error{ErrorCode::MissingEntry};
        auto result=co_await writer_->discardSuffix(length,cancel);if(!result)co_return result.error();
        co_return co_await reportPhysical();
    }
    QCoro::Task<Result<QByteArray>> hashPrefix(quint64 length,CancellationToken cancel) override {
        if(!access_.allowed())co_return Error{ErrorCode::MissingEntry};
        auto digest=co_await writer_->hashPrefix(length,cancel);
        if(!access_.allowed())co_return Error{ErrorCode::MissingEntry};
        co_return digest;
    }
    QCoro::Task<Result<void>> suspend(CancellationToken c) override { return writer_->suspend(c); }
    QCoro::Task<Result<CommitResult>> commit(CancellationToken) override {
        co_return Error{ErrorCode::UnsupportedCapability};
    }
    QCoro::Task<Result<void>> abort(CancellationToken c) override { return writer_->abort(c); }
};
struct TemporaryReaderState {
    std::shared_ptr<Namespace> ns;
    TempId id;
    QByteArray lease;
    TemporaryAccess access;
    Fd fd;
    Identity opened;
    std::atomic_bool active{true}, closed{false};
    std::shared_ptr<std::atomic_bool> epoch;
    std::chrono::steady_clock::time_point deadline;
};
class TemporaryReader final : public IFileReader {
    std::shared_ptr<NativeFileManager::Impl> impl_;
    std::shared_ptr<TemporaryReaderState> state_;

  public:
    static void closeState(TemporaryReaderState &state) {
        if (!state.closed.exchange(true)) {
            state.active = false;
            state.fd = Fd{};
            if (--state.ns->readers[state.id] == 0)
                state.ns->readers.remove(state.id);
            --state.ns->handles;
        }
    }

  public:
    TemporaryReader(std::shared_ptr<NativeFileManager::Impl> impl, std::shared_ptr<TemporaryReaderState> state)
        : impl_(std::move(impl)), state_(std::move(state)) {}
    ~TemporaryReader() override {
        auto state = state_;
        impl_->scheduler->enqueue({state->id, 0,
                                   [state] {
                                       std::lock_guard lock(state->ns->mutex);
                                       closeState(*state);
                                   }},
                                  4096, 64ULL << 20, true);
    }
    std::function<bool()> accessGuard() const override {
        auto state = state_;
        auto impl = impl_;
        return [state, impl] {
            return state->active && state->epoch && *state->epoch && !state->closed && !impl->shutting_down &&
                   state->access.allowed() && std::chrono::steady_clock::now() < state->deadline;
        };
    }
    QCoro::Task<Result<QByteArray>> readAt(quint64 offset, qsizetype amount, CancellationToken cancel) override {
        auto state = state_;
        if (amount < 0 || quint64(amount) > state->ns->config.limits.max_io_bytes ||
            offset > quint64(std::numeric_limits<qint64>::max()) ||
            quint64(amount) > quint64(std::numeric_limits<qint64>::max()) - offset)
            co_return Error{ErrorCode::InvalidArgument};
        co_return co_await impl_->submit<QByteArray>(
            state->ns->config.id,
            [state, offset, amount, cancel](Namespace &ns, Catalog &db) -> Result<QByteArray> {
                cancelCheck(cancel);
                require(state->active && state->epoch && *state->epoch && !state->closed && state->access.allowed() &&
                            std::chrono::steady_clock::now() < state->deadline,
                        ErrorCode::MissingEntry);
                temporaryInfo(db, state->id, state->access);
                auto lease = db.query("SELECT expires FROM temporary_leases WHERE token=? AND artifact=? AND owner=?",
                                      {state->lease, uuid(state->id), uuid(state->access.owner_scope)});
                require(lease.next() && lease.value(0).toLongLong() > QDateTime::currentMSecsSinceEpoch(),
                        ErrorCode::MissingEntry);
                auto before = identity(state->fd.value);
                require(before.dev == state->opened.dev && before.ino == state->opened.ino &&
                            before.size == state->opened.size && before.modified == state->opened.modified,
                        ErrorCode::ExternalModification);
                QByteArray data(amount, Qt::Uninitialized);
                ssize_t count;
                {
                    UnlockIo unlocked(ns.mutex);
                    do {
                        count = ::pread(state->fd.value, data.data(), size_t(amount), off_t(offset));
                    } while (count < 0 && errno == EINTR && !cancel.cancelled());
                }
                ioCheck(count >= 0);
                data.resize(count);
                auto after = identity(state->fd.value);
                require(after.dev == before.dev && after.ino == before.ino && after.size == before.size &&
                            after.modified == before.modified,
                        ErrorCode::ExternalModification);
                require(state->active && state->epoch && *state->epoch && state->access.allowed() &&
                            std::chrono::steady_clock::now() < state->deadline,
                        ErrorCode::MissingEntry);
                return data;
            },
            quint64(amount), true, state->id);
    }
    QCoro::Task<Result<void>> close() override {
        auto state = state_;
        if (state->closed) co_return Result<void>{};
        co_return co_await impl_->submit<void>(
            state->ns->config.id,
            [state](Namespace &, Catalog &) -> Result<void> {
                closeState(*state);
                return {};
            },
            0, false, state->id);
    }
    QCoro::Task<Result<void>> invalidate() override {
        state_->active = false;
        co_return Result<void>{};
    }
};
} // namespace
QCoro::Task<Result<TemporaryFile>> NativeFileManager::beginTemporary(NamespaceId ns, TemporaryOptions options,
                                                                     TemporaryAccess access) {
    auto impl = impl_;
    co_return co_await impl->submit<TemporaryFile>(
        ns,
        [impl, options, access](Namespace &space, Catalog &db) -> Result<TemporaryFile> {
            require(access.allowed(), ErrorCode::MissingEntry);
            require(bool(space.config.storage_pool), ErrorCode::UnsupportedCapability);
            require(options.storage_class == StorageClass::Temporary || options.storage_class == StorageClass::Cache,
                    ErrorCode::InvalidArgument);
            require(options.lifetime_ms && options.lifetime_ms <= space.config.limits.max_temporary_lifetime_ms,
                    ErrorCode::InvalidArgument);
            const bool cache = options.storage_class == StorageClass::Cache;
            const auto bytes = cache ? space.config.limits.max_cache_bytes : space.config.limits.max_temporary_bytes;
            const auto objects =
                cache ? space.config.limits.max_cache_objects : space.config.limits.max_temporary_objects;
            require(options.size_limit <= bytes && options.size_limit <= space.config.limits.max_file_size &&
                        space.handles + 1 < space.config.limits.max_handles,
                    ErrorCode::QuotaExceeded);
            require(db.scalar("SELECT count(*) FROM temporaries WHERE class=?", {int(options.storage_class)})
                            .toULongLong() < objects,
                    ErrorCode::QuotaExceeded);
            const auto id = QUuid::createUuid();
            const auto pending = QUuid::createUuid();
            const auto now = QDateTime::currentMSecsSinceEpoch();
            // Durable ownership intent precedes the pool and native participants.
            db.query("INSERT INTO temporaries(id,pending,owner,class,limit_bytes,created,expires) VALUES(?,?,?,?,?,?,?)",
                {uuid(id), uuid(pending), uuid(access.owner_scope), int(options.storage_class),
                 qlonglong(options.size_limit), now, now + options.lifetime_ms});
            fault("after_artifact_intent");
            auto reserved = PoolAccess::reserve(*space.config.storage_pool,
                {id, space.pool_id, space.config.id, access.owner_scope, id, options.storage_class, temporaryEstimate(0), 0});
            if (!reserved) throw Failure{reserved.error()};
            fault("after_artifact_reservation");
            reservePending(space, db, space.root_id, uuid(id), {}, false, id, 0, int(options.storage_class), pending);
            auto writer = attachWriter(impl, impl->space(space.config.id), db, pending);
            return TemporaryFile{
                {id, 0, {}, 0, QDateTime::fromMSecsSinceEpoch(now + options.lifetime_ms, QTimeZone::UTC)},
                std::make_unique<TemporaryWriter>(std::move(writer.writer), impl, space.config.id, id,
                                                  options.size_limit, access)};
        },
        0, true, {}, true);
}
QCoro::Task<Result<TemporaryInfo>>
NativeFileManager::finalizeTemporary(NamespaceId ns, TempId id, TemporaryAccess access, CancellationToken cancel) {
    auto impl = impl_;
    struct Finalizer {
        std::shared_ptr<WriterState> state;
        std::unique_ptr<IFileWriter> reopened;
    };
    auto prepared = co_await impl->submit<Finalizer>(
        ns,
        [impl, id, access](Namespace &space, Catalog &db) -> Result<Finalizer> {
            auto info = temporaryInfo(db, id, access, false);
            if (!info.sha256.isEmpty())
                return Finalizer{};
            auto q = db.query("SELECT pending FROM temporaries WHERE id=?", {uuid(id)});
            q.next();
            auto pending = QUuid(q.value(0).toString());
            {
                std::lock_guard lock(impl->mutex);
                for (auto weak : impl->writers)
                    if (auto state = weak.lock(); state && !state->closed && state->id == pending)
                        return Finalizer{state, {}};
            }
            auto writer = attachWriter(impl, impl->space(space.config.id), db, pending);
            std::lock_guard lock(impl->mutex);
            for (auto weak : impl->writers)
                if (auto state = weak.lock(); state && !state->closed && state->id == pending)
                    return Finalizer{state, std::move(writer.writer)};
            fail(ErrorCode::Corruption);
        },
        0, true, {}, true);
    if (!prepared)
        co_return prepared.error();
    if (prepared.value().state) {
        auto state = prepared.value().state;
        auto sized =
            co_await impl->handleSubmit<void>(state, [state, id, access](Namespace &, Catalog &db) -> Result<void> {
                temporaryInfo(db, id, access, false);
                auto q = db.query("SELECT limit_bytes FROM temporaries WHERE id=?", {uuid(id)});
                q.next();
                auto size = identity(state->fd.value).size;
                require(size <= q.value(0).toULongLong(), ErrorCode::QuotaExceeded);
                auto pending = db.pending(state->id);
                require(!pending.content.expected_size || *pending.content.expected_size == size,
                        ErrorCode::InvalidArgument);
                db.query("UPDATE pending SET final_size=? WHERE id=?", {qlonglong(size), uuid(state->id)});
                return {};
            });
        if (!sized)
            co_return sized.error();
        Writer writer(impl, state);
        auto committed = co_await writer.commit(cancel);
        if (!committed)
            co_return committed.error();
    }
    co_return co_await impl->submit<TemporaryInfo>(
        ns, [id, access](Namespace &space, Catalog &db) -> Result<TemporaryInfo> {
            auto info = temporaryInfo(db, id, access);
            auto fd = openBeneath(space.root.value, ".filemanager/artifacts/" + uuid(id), O_RDONLY | O_NONBLOCK);
            ioCheck(::fchmod(fd.value, 0400) == 0);
            ioCheck(::fsync(fd.value) == 0);
            struct stat st{};
            ioCheck(::fstat(fd.value, &st) == 0);
            poolResult(PoolAccess::materialize(*space.config.storage_pool, id, access.owner_scope,
                                               quint64(st.st_blocks) * 512));
            return info;
        });
}
QCoro::Task<Result<TemporaryRead>>
NativeFileManager::leaseTemporary(NamespaceId ns, TempId id, TemporaryLeaseOptions options, TemporaryAccess access) {
    auto impl = impl_;
    co_return co_await impl->submit<TemporaryRead>(
        ns, [impl, id, options, access](Namespace &space, Catalog &db) -> Result<TemporaryRead> {
            auto info = temporaryInfo(db, id, access);
            require(options.lifetime_ms && options.lifetime_ms <= space.config.limits.max_temporary_lifetime_ms &&
                        options.resume_lease.size() <= 16,
                    ErrorCode::InvalidArgument);
            require(space.handles + 1 < space.config.limits.max_handles, ErrorCode::QuotaExceeded);
            const auto now = QDateTime::currentMSecsSinceEpoch();
            QByteArray token = options.resume_lease;
            qint64 expires;
            if (token.isEmpty()) {
                require(db.scalar("SELECT count(*) FROM temporary_leases").toUInt() <
                            space.config.limits.max_temporary_leases,
                        ErrorCode::QuotaExceeded);
                token = QUuid::createUuid().toRfc4122();
                expires = std::min(info.expires.toMSecsSinceEpoch(), now + options.lifetime_ms);
                db.query("INSERT INTO temporary_leases VALUES(?,?,?,?)",
                         {token, uuid(id), uuid(access.owner_scope), expires});
            } else {
                auto q = db.query("SELECT expires FROM temporary_leases WHERE token=? AND artifact=? AND owner=?",
                                  {token, uuid(id), uuid(access.owner_scope)});
                require(q.next() && q.value(0).toLongLong() > now, ErrorCode::MissingEntry);
                expires = q.value(0).toLongLong();
            }
            auto state = std::make_shared<TemporaryReaderState>();
            state->ns = impl->space(space.config.id);
            state->id = id;
            state->lease = token;
            state->access = access;
            state->fd = openBeneath(space.root.value, ".filemanager/artifacts/" + uuid(id), O_RDONLY | O_NONBLOCK);
            state->opened = identity(state->fd.value);
            auto evidence = db.query("SELECT dev,ino,mtime FROM temporaries WHERE id=?", {uuid(id)});
            evidence.next();
            require(state->opened.dev == evidence.value(0).toULongLong() &&
                        state->opened.ino == evidence.value(1).toULongLong() &&
                        state->opened.modified == evidence.value(2).toLongLong() && state->opened.size == info.size,
                    ErrorCode::ExternalModification);
            state->deadline = std::chrono::steady_clock::now() +
                              std::chrono::milliseconds(std::min<qint64>(expires - now, options.lifetime_ms));
            if (!space.temporary_leases.contains(token))
                space.temporary_leases.insert(token, std::make_shared<std::atomic_bool>(true));
            state->epoch = space.temporary_leases.value(token);
            {
                std::lock_guard lock(impl->mutex);
                impl->temporary_readers.removeIf([](auto weak) {
                    auto state = weak.lock();
                    return !state || state->closed;
                });
                impl->temporary_readers.append(state);
            }
            ++space.handles;
            ++space.readers[id];
            return TemporaryRead{info, std::make_unique<TemporaryReader>(impl, state), token};
        });
}
QCoro::Task<Result<void>> NativeFileManager::releaseTemporary(NamespaceId ns, QByteArray lease,
                                                              TemporaryAccess access) {
    if (lease.size() != 16 || access.owner_scope.isNull())
        co_return Error{ErrorCode::InvalidArgument};
    co_return co_await impl_->submit<void>(ns, [lease, access](Namespace &ns, Catalog &db) -> Result<void> {
        auto q =
            db.query("SELECT token FROM temporary_leases WHERE token=? AND owner=?", {lease, uuid(access.owner_scope)});
        if (q.next()) {
            if (auto epoch = ns.temporary_leases.take(lease))
                *epoch = false;
            db.query("DELETE FROM temporary_leases WHERE token=? AND owner=?", {lease, uuid(access.owner_scope)});
        }
        return {};
    });
}
QCoro::Task<Result<void>> NativeFileManager::discardTemporary(NamespaceId ns, TempId id, TemporaryAccess access) {
    co_return co_await impl_->submit<void>(
        ns,
        [id, access](Namespace &space, Catalog &db) -> Result<void> {
            require(!access.owner_scope.isNull(), ErrorCode::InvalidArgument);
            auto q = db.query("SELECT pending,owner,state,dev,ino,mtime FROM temporaries WHERE id=?", {uuid(id)});
            if (!q.next())
                return {};
            require(QUuid(q.value(1).toString()) == access.owner_scope, ErrorCode::MissingEntry);
            auto pending = QUuid(q.value(0).toString());
            require(!space.active_writers.contains(pending), ErrorCode::Busy);
            db.query("UPDATE temporaries SET state=2 WHERE id=?", {uuid(id)});
            auto leases = db.query("SELECT token FROM temporary_leases WHERE artifact=?", {uuid(id)});
            while (leases.next())
                if (auto epoch = space.temporary_leases.take(leases.value(0).toByteArray()))
                    *epoch = false;

            if (space.readers.value(id))
                return {};
            QJsonObject op{{"kind", "artifact_delete"},
                           {"file_id", uuid(id)},
                           {"pool_reservation", uuid(id)},
                           {"pool_owner", uuid(access.owner_scope)},
                           {"directory", false},
                           {"catalog_only", false},
                           {"from", ".filemanager/artifacts/" + uuid(id)},
                           {"to", ".filemanager/recovery/" + uuid(id)},
                           {"dev", q.value(3).toString()},
                           {"ino", q.value(4).toString()},
                           {"mtime", q.value(5).toString()}};
            if (q.value(3).isNull()) {
                if (db.scalar("SELECT id FROM pending WHERE id=?", {uuid(pending)}).isValid()) {
                    auto fd = openBeneath(space.root.value, pendingPath(pending), O_RDONLY | O_NONBLOCK);
                    auto evidence = identity(fd.value);
                    auto expected = db.query("SELECT dev,ino FROM pending WHERE id=?", {uuid(pending)});
                    expected.next();
                    require(evidence.dev == expected.value(0).toULongLong() &&
                                evidence.ino == expected.value(1).toULongLong(),
                            ErrorCode::ExternalModification);
                    op["from"] = pendingPath(pending);
                    op["pending_id"] = uuid(pending);
                    op["dev"] = QString::number(evidence.dev);
                    op["ino"] = QString::number(evidence.ino);
                    op["mtime"] = QString::number(evidence.modified);
                } else {
                    // No inode evidence exists for an interrupted creation. Do
                    // not erase an untracked or externally replaced object.
                    struct stat staged{};
                    const auto path = pendingPath(pending).toUtf8();
                    require(::fstatat(space.root.value, path.constData(), &staged, AT_SYMLINK_NOFOLLOW) < 0 && errno == ENOENT,
                            ErrorCode::ExternalModification);
                    op["catalog_only"] = true;
                }
            }
            applyOperation(space, db, op);
            auto released = PoolAccess::release(*space.config.storage_pool, id, access.owner_scope);
            if (!released)
                throw Failure{released.error()};
            return {};
        },
        0, true, {}, true);
}
QCoro::Task<Result<void>> NativeFileManager::reclaimTemporary(NamespaceId ns, quint32 limit) {
    if (!limit || limit > 1000)
        co_return Error{ErrorCode::InvalidArgument};
    auto impl = impl_;
    struct Retired {
        QVector<QPair<TempId, QUuid>> artifacts;
        QVector<std::shared_ptr<WriterState>> writers;
        QVector<std::shared_ptr<TemporaryReaderState>> readers;
    };
    auto retired = co_await impl->submit<Retired>(ns, [impl, limit](Namespace &space, Catalog &db) -> Result<Retired> {
        const auto now = QDateTime::currentMSecsSinceEpoch();
        auto leases = db.query("SELECT token FROM temporary_leases WHERE expires<=? LIMIT ?", {now, limit});
        QVector<QByteArray> expired_leases;
        while (leases.next())
            expired_leases.append(leases.value(0).toByteArray());
        leases.finish();
        for (auto token : expired_leases) {
            if (auto epoch = space.temporary_leases.take(token))
                *epoch = false;
            db.query("DELETE FROM temporary_leases WHERE token=?", {token});
        }
        auto q = db.query("SELECT id,owner,pending FROM temporaries WHERE state=2 OR expires<=? OR created>? "
                          "OR (state=0 AND NOT EXISTS(SELECT 1 FROM pending WHERE pending.id=temporaries.pending)) "
                          "ORDER BY expires LIMIT ?",
                          {now, now, limit});
        Retired result;
        QSet<PendingId> pending;
        while (q.next()) {
            result.artifacts.append({QUuid(q.value(0).toString()), QUuid(q.value(1).toString())});
            pending.insert(QUuid(q.value(2).toString()));
        }
        std::lock_guard lock(impl->mutex);
        for (auto weak : impl->writers)
            if (auto state = weak.lock();
                state && state->ns.get() == &space && pending.contains(state->id) && !state->closed) {
                state->expired = true;
                result.writers.append(state);
            }
        for (auto weak : impl->temporary_readers)
            if (auto reader = weak.lock(); reader && reader->ns.get() == &space && !reader->closed &&
                                           (!reader->active || !reader->access.allowed() || !reader->epoch ||
                                            !*reader->epoch || std::chrono::steady_clock::now() >= reader->deadline))
                result.readers.append(reader);
        return result;
    });
    if (!retired)
        co_return retired.error();
    for (auto state : retired.value().writers) {
        auto closed = co_await impl->handleSubmit<void>(state, [state](Namespace &, Catalog &) -> Result<void> {
            state->verifying = false;
            closeWriter(*state);
            return {};
        });
        if (!closed)
            co_return closed.error();
    }
    for (auto state : retired.value().readers) {
        auto closed = co_await impl->submit<void>(
            ns,
            [state](Namespace &, Catalog &) -> Result<void> {
                TemporaryReader::closeState(*state);
                return {};
            },
            0, false, state->id);
        if (!closed)
            co_return closed.error();
    }
    for (auto [id, owner] : retired.value().artifacts) {
        auto removed = co_await discardTemporary(ns, id, {owner, [] { return true; }});
        if (!removed && removed.error().code != ErrorCode::Busy)
            co_return removed.error();
    }
    co_return Result<void>{};
}
QCoro::Task<Result<PendingFile>> NativeFileManager::createFile(NamespaceId ns, CreateOptions options) {
    if (options.suggested_name.isEmpty() || options.suggested_name.size() > 255)
        return rejected<PendingFile>(ErrorCode::InvalidName);
    if (!options.content.expected_sha256.isEmpty() && options.content.expected_sha256.size() != 32) return rejected<PendingFile>(ErrorCode::InvalidArgument);
    auto impl = impl_;
    return impl->submit<PendingFile>(ns, [impl, options = std::move(options)](Namespace& space, Catalog& db) -> Result<PendingFile> {
        checkParent(space, db, options.parent);
        const auto name = chooseName(space, db, options.parent, options.suggested_name, options.conflict);
        const auto id = reservePending(space, db, options.parent, name, options.content, false, QUuid::createUuid());
        return attachWriter(impl, impl->space(space.config.id), db, id);
    });
}
QCoro::Task<Result<PendingFile>> NativeFileManager::replaceFile(NamespaceId ns, FileId id, ContentConstraints content) {
    if (!content.expected_sha256.isEmpty() && content.expected_sha256.size() != 32) return rejected<PendingFile>(ErrorCode::InvalidArgument);
    auto impl = impl_;
    return impl->submit<PendingFile>(ns, [impl, id, content = std::move(content)](Namespace& space, Catalog& db) -> Result<PendingFile> {
        const auto entry = db.entry(id); require(entry.type == EntryType::File && entry.parent.has_value(), ErrorCode::InvalidArgument);
        validateEntry(space, db, entry); noReservations(space, db, id);
        const auto pending = reservePending(space, db, *entry.parent, entry.name, content, true, id);
        return attachWriter(impl, impl->space(space.config.id), db, pending);
    });
}
QCoro::Task<Result<PendingFile>> NativeFileManager::openPending(NamespaceId ns, PendingId id) {
    auto impl = impl_;
    return impl->submit<PendingFile>(ns, [impl, id](Namespace& space, Catalog& db) -> Result<PendingFile> { return attachWriter(impl, impl->space(space.config.id), db, id); });
}
QCoro::Task<Result<void>> NativeFileManager::abortPending(NamespaceId ns, PendingId id, CancellationToken cancel) {
    return impl_->submit<void>(ns, [id, cancel](Namespace& space, Catalog& db) -> Result<void> {
        cancelCheck(cancel); require(!space.active_writers.contains(id), ErrorCode::Busy);
        const auto pending = db.pending(id);
        QJsonObject op{{"kind", "abort"}, {"file_id", uuid(pending.file_id)}, {"pending_id", uuid(id)}, {"directory", false}, {"catalog_only", true}};
        try {
            auto fd = openBeneath(space.root.value, pendingPath(id), O_RDONLY | O_NONBLOCK);
            const auto found = identity(fd.value);
            auto q = db.query("SELECT dev,ino FROM pending WHERE id=?", {uuid(id)}); q.next();
            if (found.dev == q.value(0).toULongLong() && found.ino == q.value(1).toULongLong()) {
                op["catalog_only"] = false; op["from"] = pendingPath(id); op["to"] = QStringLiteral(".filemanager/recovery/") + uuid(id);
                op["dev"] = QString::number(found.dev); op["ino"] = QString::number(found.ino);
            } else ++space.untracked_staging;
        } catch (const Failure& failure) {
            if (failure.error.code != ErrorCode::MissingEntry && failure.error.code != ErrorCode::ExternalModification) throw;
        }
        if(space.config.storage_pool)op["pool_reservation"]=uuid(id);
        applyOperation(space, db, op); return {};
    });
}
QCoro::Task<Result<PendingPage>> NativeFileManager::listPending(NamespaceId ns, PageRequest page) {
    if (!validPageEnvelope(page)) return rejected<PendingPage>(ErrorCode::InvalidArgument);
    return impl_->submit<PendingPage>(ns, [page = std::move(page)](Namespace& space, Catalog& db) -> Result<PendingPage> {
        const auto generation = db.generation(); const auto last = readCursor(page, generation, "pending", space.config.limits.max_page_size);
        auto q = db.query("SELECT id FROM pending WHERE private=0 AND id>? ORDER BY id LIMIT ?", {last, page.limit + 1});
        PendingPage result; QString next;
        while (q.next()) {
            if (result.entries.size() == page.limit) { result.next_cursor = cursor(generation, "pending", next); break; }
            next = q.value(0).toString(); result.entries.append(db.pending(QUuid(next)));
        }
        return result;
    });
}
QCoro::Task<Result<RangePage>> NativeFileManager::listRanges(NamespaceId ns, PendingId id, RangeKind kind, PageRequest page) {
    if (!validPageEnvelope(page)) return rejected<RangePage>(ErrorCode::InvalidArgument);
    return impl_->submit<RangePage>(ns, [id, kind, page = std::move(page)](Namespace& space, Catalog& db) -> Result<RangePage> {
        require(kind == RangeKind::StorageValid || kind == RangeKind::ApplicationVerified, ErrorCode::InvalidArgument);
        const auto pending = db.pending(id); const auto scope = "ranges:" + uuid(id) + ':' + QString::number(int(kind));
        const auto last = readCursor(page, pending.range_generation, scope, space.config.limits.max_page_size);
        auto q = db.query("SELECT start,end FROM ranges WHERE pending=? AND kind=? AND start>? ORDER BY start LIMIT ?", {uuid(id), int(kind), last.isEmpty() ? qlonglong(-1) : last.toLongLong(), page.limit + 1});
        RangePage result; QString next;
        while (q.next()) {
            if (result.entries.size() == page.limit) { result.next_cursor = cursor(pending.range_generation, scope, next); break; }
            next = q.value(0).toString(); result.entries.append({q.value(0).toULongLong(), q.value(1).toULongLong() - q.value(0).toULongLong()});
        }
        return result;
    });
}
QCoro::Task<Result<std::unique_ptr<IFileReader>>> NativeFileManager::openRead(NamespaceId ns, FileId id) {
    auto impl = impl_;
    return impl->submit<std::unique_ptr<IFileReader>>(ns, [impl, id](Namespace& space, Catalog& db) -> Result<std::unique_ptr<IFileReader>> {
        auto prepared = attachReader(impl, space, db, id);
        return std::move(prepared.reader);
    });
}
IFileManager::FilesystemGuard NativeFileManager::filesystemGuard(NamespaceId ns, QUuid id, std::function<bool(FilesystemIdentity)> check) {
    if (!check) return {};
    auto impl = impl_;
    return [impl, ns, id, check = std::move(check)]() -> QCoro::Task<Result<void>> {
        co_return co_await impl->submit<void>(ns, [id, check](Namespace& space, Catalog& db) -> Result<void> {
            auto allowed = [&](int fd) {
                struct stat found{}; ioCheck(::fstat(fd, &found) == 0);
                bool result = false; try { result = check({quint64(found.st_dev), quint64(found.st_ino), bool(S_ISDIR(found.st_mode))}); } catch (...) {}
                require(result, ErrorCode::MissingEntry);
            };
            auto current = Fd(::fcntl(space.root.value, F_DUPFD_CLOEXEC, 0));
            for (quint32 depth = 0; depth < 512; ++depth) {
                allowed(current.value); struct stat before{}, after{}; ioCheck(::fstat(current.value, &before) == 0);
                auto parent = Fd(::openat(current.value, "..", O_PATH | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
                ioCheck(::fstat(parent.value, &after) == 0);
                if (before.st_dev == after.st_dev && before.st_ino == after.st_ino) break;
                require(depth < 511, ErrorCode::MissingEntry); current = std::move(parent);
            }
            const auto item = db.entry(id);
            require(isListable(item.status), ErrorCode::MissingEntry);
            if (item.status != EntryStatus::Updating) validateEntry(space, db, item);
            if (space.config.naming == PhysicalNaming::FileId && item.type == EntryType::Directory) return {};
            const auto path = db.path(id, space.config.naming, space.config.limits.max_depth);
            QString prefix;
            for (const auto& component : path.split('/')) {
                if (!prefix.isEmpty()) prefix += '/';
                prefix += component;
                auto object = openBeneath(space.root.value, prefix, O_PATH); allowed(object.value);
            }
            return {};
        });
    };
}
QCoro::Task<Result<PreparedRead>> NativeFileManager::prepareRead(NamespaceId ns, FileId id, ReadPreparationOptions options) {
    auto impl = impl_;
    if (!options.resume_lease.isEmpty()) co_return Error{ErrorCode::UnsupportedCapability};
    if (!options.expected_sha256.isEmpty() && options.expected_sha256.size() != 32) co_return Error{ErrorCode::InvalidArgument};
    std::shared_ptr<Namespace> space;
    try { space = impl->space(ns); } catch (const Failure& failure) { co_return failure.error; }
    auto opened = co_await impl->submit<PreparedRead>(ns, [impl, id, options](Namespace& space, Catalog& db) -> Result<PreparedRead> {
        cancelCheck(options.cancel);
        const auto entry = db.entry(id);
        require(entry.type == EntryType::File, ErrorCode::InvalidArgument);
        require(!options.expected_generation || *options.expected_generation == entry.generation, ErrorCode::Conflict);
        require(entry.sha256.size() == 32, ErrorCode::IntegrityError);
        require(options.expected_sha256.isEmpty() || options.expected_sha256 == entry.sha256, ErrorCode::IntegrityError);
        return attachReader(impl, space, db, id);
    });
    if (!opened) co_return opened.error();
    auto prepared = std::move(opened).value();
    auto hash = std::make_shared<QCryptographicHash>(QCryptographicHash::Sha256);
    quint64 offset = 0;
    // Each bounded chunk yields its worker turn; the reader pins this committed
    // generation throughout hashing and subsequent streaming. No whole-file buffer.
    while (offset < prepared.info.size) {
        const auto count = std::min<quint64>(space->config.limits.max_io_bytes, prepared.info.size - offset);
        auto bytes = co_await prepared.reader->readAt(offset, qsizetype(count), options.cancel);
        if (!bytes) co_return bytes.error();
        if (bytes.value().isEmpty()) co_return Error{ErrorCode::ExternalModification};
        const auto bytes_count = quint64(bytes.value().size());
        auto hashed = co_await impl->submit<void>(ns, [hash, data = std::move(bytes).value(), cancel = options.cancel](Namespace&, Catalog&) -> Result<void> {
            cancelCheck(cancel); hash->addData(data); return {};
        }, bytes_count, false);
        if (!hashed) co_return hashed.error();
        offset += bytes_count;
    }
    // Also validate empty files and the final content identity after hashing.
    auto final_check = co_await prepared.reader->readAt(offset, 0, options.cancel);
    if (!final_check) co_return final_check.error();
    auto digest = co_await impl->submit<QByteArray>(ns, [hash, cancel = options.cancel](Namespace&, Catalog&) -> Result<QByteArray> {
        cancelCheck(cancel); return hash->result();
    }, 0, false);
    if (!digest) co_return digest.error();
    if (digest.value() != prepared.info.sha256) { co_await prepared.reader->invalidate(); co_return Error{ErrorCode::IntegrityError}; }
    LFLOG_DEBUG << "FileManager prepared verified read";
    co_return std::move(prepared);
}
QCoro::Task<Result<MutationResult>> NativeFileManager::rename(NamespaceId ns, QUuid id, QString name, CancellationToken cancel) {
    if (name.isEmpty() || name.size() > 255) return rejected<MutationResult>(ErrorCode::InvalidName);
    return impl_->submit<MutationResult>(ns, [id, name = std::move(name), cancel](Namespace& space, Catalog& db) -> Result<MutationResult> {
        cancelCheck(cancel); auto entry = db.entry(id); require(entry.parent.has_value(), ErrorCode::InvalidArgument);
        validateEntry(space, db, entry); noReservations(space, db, id); noDescendantReservations(space, db, id);
        const auto assigned = chooseName(space, db, *entry.parent, name, ConflictPolicy::Fail, id);
        if (assigned == entry.name) return MutationResult{{}, db.generation()};
        QString destination;
        if (space.config.naming == PhysicalNaming::OriginalName) {
            auto parent = db.path(*entry.parent, space.config.naming, space.config.limits.max_depth);
            destination = parent == "." ? assigned : parent + '/' + assigned;
        }
        auto op = physicalOperation(space, db, "rename", entry, destination); op["name"] = assigned;
        cancelCheck(cancel); return applyOperation(space, db, op);
    });
}
QCoro::Task<Result<MutationResult>> NativeFileManager::move(NamespaceId ns, QUuid id, DirectoryId destination, CancellationToken cancel) {
    return impl_->submit<MutationResult>(ns, [id, destination, cancel](Namespace& space, Catalog& db) -> Result<MutationResult> {
        cancelCheck(cancel); auto entry = db.entry(id); require(entry.parent.has_value(), ErrorCode::InvalidArgument);
        if (!db.scalar("SELECT id FROM entries WHERE id=?", {uuid(destination)}).isValid()) fail(ErrorCode::CrossNamespaceMove);
        checkParent(space, db, destination); validateEntry(space, db, entry); noReservations(space, db, id); noDescendantReservations(space, db, id);
        if (*entry.parent == destination) return MutationResult{{}, db.generation()};
        quint32 destination_depth = 0; auto ancestor = db.entry(destination);
        for (;;) {
            require(ancestor.id != id, ErrorCode::InvalidArgument);
            if (!ancestor.parent) break;
            require(++destination_depth <= space.config.limits.max_depth, ErrorCode::Corruption); ancestor = db.entry(*ancestor.parent);
        }
        const auto subtree_depth = db.scalar("WITH RECURSIVE tree(id,depth) AS (SELECT ?,0 UNION ALL SELECT e.id,t.depth+1 FROM entries e JOIN tree t ON e.parent=t.id WHERE t.depth<?) SELECT max(depth) FROM tree", {uuid(id), space.config.limits.max_depth}).toUInt();
        require(destination_depth + 1 + subtree_depth <= space.config.limits.max_depth, ErrorCode::QuotaExceeded);
        chooseName(space, db, destination, entry.name, ConflictPolicy::Fail);
        QString target;
        if (space.config.naming == PhysicalNaming::OriginalName) {
            auto parent = db.path(destination, space.config.naming, space.config.limits.max_depth); target = parent == "." ? entry.name : parent + '/' + entry.name;
        }
        auto op = physicalOperation(space, db, "move", entry, target); op["parent"] = uuid(destination);
        cancelCheck(cancel); return applyOperation(space, db, op);
    });
}
QCoro::Task<Result<MutationResult>> NativeFileManager::remove(NamespaceId ns, QUuid id, CancellationToken cancel) {
    return impl_->submit<MutationResult>(ns, [id, cancel](Namespace& space, Catalog& db) -> Result<MutationResult> {
        cancelCheck(cancel); auto entry = db.entry(id); require(entry.parent.has_value(), ErrorCode::InvalidArgument);
        validateEntry(space, db, entry); noReservations(space, db, id); noDescendantReservations(space, db, id);
        require(space.readers.value(id) == 0, ErrorCode::Busy);
        if (entry.type == EntryType::Directory) {
            require(!db.scalar("SELECT id FROM entries WHERE parent=? LIMIT 1", {uuid(id)}).isValid(), ErrorCode::DirectoryNotEmpty);
            if (space.config.naming == PhysicalNaming::OriginalName) {
                auto fd = openBeneath(space.root.value, db.path(id, space.config.naming, space.config.limits.max_depth), O_RDONLY | O_DIRECTORY);
                auto* directory = ::fdopendir(std::exchange(fd.value, -1)); ioCheck(directory != nullptr);
                bool empty = true;
                while (auto* item = ::readdir(directory)) if (QByteArray(item->d_name) != "." && QByteArray(item->d_name) != "..") { empty = false; break; }
                ::closedir(directory); require(empty, ErrorCode::DirectoryNotEmpty);
            }
        }
        const auto destination = QStringLiteral(".filemanager/recovery/") + uuid(QUuid::createUuid());
        auto op = physicalOperation(space, db, "delete", entry, destination);
        cancelCheck(cancel); return applyOperation(space, db, op);
    });
}
QCoro::Task<Result<OperationInfo>> NativeFileManager::operationStatus(NamespaceId ns, OperationId id) {
    return impl_->submit<OperationInfo>(ns, [id](Namespace&, Catalog& db) -> Result<OperationInfo> {
        auto q = db.query("SELECT phase,payload FROM operations WHERE id=?", {uuid(id)}); require(q.next(), ErrorCode::MissingEntry);
        const auto op = QJsonDocument::fromJson(q.value(1).toByteArray()).object();
        OperationInfo info{id, q.value(0).toString(), op["kind"].toString(), QUuid(op["file_id"].toString()), {}};
        if (op.contains("generation")) info.result = MutationResult{id, op["generation"].toString().toULongLong(), op["sync_failed"].toBool() ? Durability::SyncFailed : Durability::Synchronized};
        return info;
    }, 0, false);
}
QCoro::Task<Result<ReconciliationPage>> NativeFileManager::reconcile(NamespaceId ns, ReconciliationRequest request, CancellationToken cancel) {
    if (!validPageEnvelope(request.page)) return rejected<ReconciliationPage>(ErrorCode::InvalidArgument);
    return impl_->submit<ReconciliationPage>(ns, [request = std::move(request), cancel](Namespace& space, Catalog& db) -> Result<ReconciliationPage> {
        cancelCheck(cancel);
        if (request.action == ReconciliationAction::DiscardMissing) {
            auto entry = db.entry(request.id); require(entry.parent.has_value(), ErrorCode::InvalidArgument);
            try { validateEntry(space, db, entry); } catch (const Failure&) {}
            entry = db.entry(request.id); require(entry.status == EntryStatus::Missing, ErrorCode::ExternalModification);
            noReservations(space, db, request.id); noDescendantReservations(space, db, request.id);
            require(!db.scalar("SELECT id FROM entries WHERE parent=? LIMIT 1", {uuid(request.id)}).isValid(), ErrorCode::DirectoryNotEmpty);
            require(space.readers.value(request.id) == 0, ErrorCode::Busy);
            // Confirm absence rather than trusting a persisted Missing flag.
            const auto path = db.path(request.id, space.config.naming, space.config.limits.max_depth);
            try { auto fd = openBeneath(space.root.value, path, O_RDONLY | O_NONBLOCK | (entry.type == EntryType::Directory ? O_DIRECTORY : 0)); fail(ErrorCode::ExternalModification); }
            catch (const Failure& failure) { if (failure.error.code != ErrorCode::MissingEntry) throw; }
            Transaction transaction(db); db.query("DELETE FROM entries WHERE id=?", {uuid(request.id)}); db.bump(); transaction.commit(); return ReconciliationPage{};
        }
        if (request.action == ReconciliationAction::RetryValidation) {
            auto entry = db.entry(request.id);
            // Only the same original identity and, for files, original digest may restore availability.
            const auto path = db.path(request.id, space.config.naming, space.config.limits.max_depth);
            auto fd = openBeneath(space.root.value, path, O_RDONLY | O_NONBLOCK | (entry.type == EntryType::Directory ? O_DIRECTORY : 0));
            const auto found = identity(fd.value, entry.type == EntryType::Directory);
            auto q = db.query("SELECT dev,ino,sha FROM entries WHERE id=?", {uuid(request.id)}); q.next();
            require(found.dev == q.value(0).toULongLong() && found.ino == q.value(1).toULongLong(), ErrorCode::ExternalModification);
            if (entry.type == EntryType::File) {
                const auto digest = q.value(2).toByteArray(); require(digest.size() == 32, ErrorCode::ExternalModification);
                PendingInfo pending; pending.final_size = entry.size; pending.content.expected_sha256 = digest;
                verifyContent(space, db, pending, path, cancel);
            }
            db.query("UPDATE entries SET status=0,mtime=? WHERE id=?", {QString::number(found.modified), uuid(request.id)}); db.bump(); return ReconciliationPage{{db.entry(request.id)}, {}};
        }
        require(request.action == ReconciliationAction::Scan, ErrorCode::InvalidArgument);
        const auto generation = db.generation(); const auto last = readCursor(request.page, generation, "reconcile", space.config.limits.max_page_size);
        auto q = db.query("SELECT id FROM entries WHERE id>? ORDER BY id LIMIT ?", {last, request.page.limit + 1});
        ReconciliationPage result; QString next; bool more = false;
        while (q.next()) {
            if (result.entries.size() == request.page.limit) { more = true; break; }
            cancelCheck(cancel); next = q.value(0).toString(); auto entry = db.entry(QUuid(next));
            try { validateEntry(space, db, entry); } catch (const Failure&) { entry = db.entry(entry.id); }
            result.entries.append(entry);
        }
        if (more) result.next_cursor = cursor(db.generation(), "reconcile", next);
        return result;
    });
}
Result<quint64> NativeFileManager::subscribe(NamespaceId ns, QObject* context, std::function<void(Change)> callback) {
    if (!context || context->thread() != QThread::currentThread() || !callback || impl_->shutting_down) return Error{ErrorCode::InvalidArgument};
    std::lock_guard lock(impl_->mutex);
    auto space = impl_->spaces.value(ns); if (!space) return Error{ErrorCode::MissingEntry};
    std::erase_if(impl_->subscribers, [](const auto& item) { return !item.second->active; });
    quint32 count = 0;
    for (const auto& [id, existing] : impl_->subscribers) { Q_UNUSED(id); if (existing->ns == ns) ++count; }
    if (count >= space->config.limits.max_notifications || impl_->subscribers.size() >= 4096) return Error{ErrorCode::QuotaExceeded};
    auto sub = makeSubscriber(ns, context, std::move(callback));
    const auto id = impl_->next_subscription++; impl_->subscribers[id] = sub; return id;
}
QCoro::Task<Result<DirectorySubscription>> NativeFileManager::subscribeDirectory(
        NamespaceId ns, DirectoryId directory, DirectoryChangeCallback callback) {
    if (!callback.context || callback.context->thread() != QThread::currentThread() || !callback.callback)
        return rejected<DirectorySubscription>(ErrorCode::InvalidArgument);
    auto sub = makeSubscriber(ns, callback.context, {});
    sub->directory = directory;
    sub->callback = std::move(callback.callback);
    auto impl = impl_;
    return impl->submit<DirectorySubscription>(ns, [impl, sub, directory](Namespace& space, Catalog& db) -> Result<DirectorySubscription> {
        checkBrowseDirectory(space, db, directory);
        require(sub->active, ErrorCode::Cancelled);
        const auto generation = db.generation();
        std::lock_guard lock(impl->mutex);
        require(!impl->shutting_down, ErrorCode::ShuttingDown);
        std::erase_if(impl->subscribers, [](const auto& item) { return !item.second->active; });
        quint32 count = 0;
        for (const auto& [id, existing] : impl->subscribers) { Q_UNUSED(id); if (existing->ns == space.config.id) ++count; }
        require(count < space.config.limits.max_notifications && impl->subscribers.size() < 4096, ErrorCode::QuotaExceeded);
        const auto id = impl->next_subscription++;
        impl->subscribers[id] = sub;
        LFLOG_DEBUG << "FileManager directory subscription registered";
        // Atomic cancellation never waits for catalog I/O or a manager mutex.
        // Admission/notification prunes inactive records before applying budgets.
        return DirectorySubscription{space.config.id, directory, generation,
            std::shared_ptr<std::atomic_bool>(sub, &sub->active), {}};
    });
}
void NativeFileManager::unsubscribe(quint64 subscription) {
    std::lock_guard lock(impl_->mutex); auto found = impl_->subscribers.find(subscription);
    if (found != impl_->subscribers.end()) { found->second->active = false; impl_->subscribers.erase(found); }
}
QCoro::Task<Result<void>> NativeFileManager::shutdown() {
    auto impl = impl_;
    if (impl->shutting_down.exchange(true)) co_return Error{ErrorCode::ShuttingDown};
    QVector<std::shared_ptr<Namespace>> spaces;
    { std::lock_guard lock(impl->mutex); for (auto& [id, sub] : impl->subscribers) { Q_UNUSED(id); sub->active = false; } impl->subscribers.clear(); for (auto& ns : impl->spaces) spaces.append(ns); }
    std::optional<Error> failure;
    QVector<QUuid> lanes;
    { std::lock_guard guard(impl->scheduler->mutex);
        for (auto& [key, queue] : impl->scheduler->queues) { Q_UNUSED(queue); lanes.append(key); }
    }
    { std::lock_guard guard(impl->mutex);
        for (auto& weak : impl->writers) if (auto writer = weak.lock()) lanes.append(writer->lane);
        for (auto& weak : impl->readers) if (auto reader = weak.lock()) lanes.append(reader->lane);
        for (auto& weak : impl->temporary_readers) if (auto reader = weak.lock()) lanes.append(reader->id);
    }
    for (auto lane : lanes) {
        auto drained = co_await execute<void>(impl->scheduler, lane, 0, 4096, 64ULL << 20, []() -> Result<void> { return {}; });
        if (!drained) failure = drained.error();
    }
    for (auto ns : spaces) {
        auto result = co_await execute<void>(impl->scheduler, ns->config.id, 0, 4096, 64ULL << 20, [impl, ns]() -> Result<void> {
            std::lock_guard lock(ns->mutex);
            PermissionRepairScope permissions(ns->config.repair_permissions);
            Catalog db(ns->catalogPath(), ns->config.limits.max_catalog_bytes, false);
            db.private_access=true;
            std::optional<Error> error;
            try { recover(*ns, db); } catch (const Failure& failure) { error = failure.error; }
            QVector<std::shared_ptr<WriterState>> writers;
            { std::lock_guard guard(impl->mutex); for (auto& weak : impl->writers) if (auto writer = weak.lock(); writer && writer->ns == ns) writers.append(writer); }
            for (auto& writer : writers) if (!writer->closed) {
                writer->verifying = false;
                db.private_access=writer->private_access;db.private_class=writer->private_kind;
                try { checkpointWriter(*writer, db); } catch (const Failure& failure) { error = failure.error; }
                closeWriter(*writer);
            }
            QVector<std::shared_ptr<ReaderState>> readers;
            {
                std::lock_guard guard(impl->mutex);
                for (auto &weak : impl->readers)
                    if (auto reader = weak.lock(); reader && reader->ns == ns)
                        readers.append(reader);
            }
            for (auto &reader : readers)
                if (!reader->closed) {
                    reader->closed = true;
                    reader->fd = Fd{};
                    if (--ns->readers[reader->id] == 0)
                        ns->readers.remove(reader->id);
                    --ns->handles;
                }
            {
                std::lock_guard guard(impl->mutex);
                for (auto weak : impl->temporary_readers)
                    if (auto reader = weak.lock(); reader && reader->ns == ns)
                        TemporaryReader::closeState(*reader);
            }
            for (auto epoch : ns->temporary_leases)
                *epoch = false;
            ns->temporary_leases.clear();
            if (error)
                return *error;
            return {};
        });
        if (!result) failure = result.error();
    }
    for (auto ns : spaces) {
        auto released = co_await execute<void>(impl->scheduler, ns->config.id, 0, 4096, 64ULL << 20, [impl, ns]() -> Result<void> {
            std::lock_guard guard(ns->mutex);
            { std::lock_guard lock(impl->mutex);
                for (auto& weak : impl->writers) if (auto writer = weak.lock(); writer && writer->ns == ns) closeWriter(*writer);
                for (auto& weak : impl->readers) if (auto reader = weak.lock(); reader && reader->ns == ns && !reader->closed) {
                    reader->closed = true; reader->fd = Fd{}; --ns->handles;
                }
            }
            ns->readers.clear(); ns->locks.clear(); ns->owner_lock = Fd{}; ns->internal = Fd{}; ns->root = Fd{};
            return {};
        });
        if (!released) failure = released.error();
    }
    { std::lock_guard lock(impl->mutex); impl->spaces.clear(); }
    impl->scheduler->stop();
    LFLOG_DEBUG << "FileManager shutdown completed; success=" << !failure.has_value();
    if (failure) co_return *failure;
    co_return Result<void>{};
}
} // namespace dsfm

namespace dsfm {
QCoro::Task<Result<AccessContext>> IFileManager::descendantAccess(NamespaceId ns, DirectoryId directory, AccessContext access) {
    auto info = co_await stat(ns, directory, access);
    if (!info) co_return info.error();
    if (info.value().type != EntryType::Directory) co_return Error{ErrorCode::InvalidArgument};
    co_return access;
}
}
