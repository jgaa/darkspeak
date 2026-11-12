#include "internal.h"
#include <QAbstractEventDispatcher>
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QThread>
#include <logfault/logfault.h>
#include <fcntl.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/file.h>

namespace dsfm {
using namespace detail;
namespace {
struct Job { QUuid key; quint64 bytes; std::function<void()> run; QUuid group{}; };
struct Scheduler : std::enable_shared_from_this<Scheduler> {
    std::mutex mutex;
    std::condition_variable ready;
    std::map<QUuid, std::deque<Job>> queues;
    QSet<QUuid> running;
    QHash<QUuid, quint32> group_counts;
    QHash<QUuid, quint64> group_bytes;
    QUuid last;
    quint64 bytes = 0;
    quint32 count = 0;
    bool stopping = false;
    static std::shared_ptr<Scheduler> create() {
        auto state = std::make_shared<Scheduler>();
        for (int i = 0; i < 4; ++i) std::thread([state] { state->loop(); }).detach();
        return state;
    }
    bool enqueue(Job job, quint32 max_count, quint64 max_bytes, bool control = false) {
        std::lock_guard lock(mutex);
        if (job.group.isNull()) job.group = job.key;
        if (stopping || count >= (control ? 132096U : 256U) || (!control && group_counts.value(job.group) >= max_count)
            || job.bytes > max_bytes || job.bytes > (64ULL << 20) || bytes > (64ULL << 20) - job.bytes
            || group_bytes.value(job.group) > max_bytes - job.bytes) return false;
        ++count; bytes += job.bytes; ++group_counts[job.group]; group_bytes[job.group] += job.bytes;
        queues[job.key].push_back(std::move(job)); ready.notify_all(); return true;
    }
    void loop() {
        for (;;) {
            Job job;
            {
                std::unique_lock lock(mutex);
                ready.wait(lock, [&] { if (stopping && !count) return true; for (auto& [key, queue] : queues) if (!queue.empty() && !running.contains(key)) return true; return false; });
                if (stopping && !count) return;
                auto it = queues.upper_bound(last);
                for (size_t attempts = 0; attempts < queues.size(); ++attempts) {
                    if (it == queues.end()) it = queues.begin();
                    if (!it->second.empty() && !running.contains(it->first)) break;
                    ++it;
                }
                last = it->first; running.insert(last); job = std::move(it->second.front()); it->second.pop_front();
            }
            job.run();
            {
                std::lock_guard lock(mutex);
                running.remove(job.key); --count; bytes -= job.bytes;
                if (--group_counts[job.group] == 0) group_counts.remove(job.group);
                group_bytes[job.group] -= job.bytes; if (!group_bytes[job.group]) group_bytes.remove(job.group);
                auto it = queues.find(job.key); if (it != queues.end() && it->second.empty()) queues.erase(it);
            }
            ready.notify_all();
        }
    }
    void stop() { std::lock_guard lock(mutex); stopping = true; ready.notify_all(); }
};
template<typename T> struct AsyncState { std::optional<Result<T>> result; };
template<typename T> struct WorkAwaiter {
    std::shared_ptr<Scheduler> scheduler;
    QUuid key;
    quint64 bytes;
    quint32 max_count;
    quint64 max_bytes;
    std::function<Result<T>()> function;
    QUuid group;
    std::shared_ptr<AsyncState<T>> state = std::make_shared<AsyncState<T>>();
    bool await_ready() const { return false; }
    bool await_suspend(std::coroutine_handle<> continuation) {
        auto* dispatcher = QAbstractEventDispatcher::instance(QThread::currentThread());
        if (!dispatcher) { state->result.emplace(Error{ErrorCode::InvalidArgument}); return false; }
        auto result = state;
        QPointer<QObject> context(dispatcher);
        auto run = [result, context, continuation, fn = std::move(function)]() mutable {
            try { result->result.emplace(fn()); }
            catch (const Failure& failure) {
                LFLOG_DEBUG << "FileManager operation failed: code=" << int(failure.error.code) << ", native=" << failure.error.native_code;
                result->result.emplace(failure.error);
            } catch (const std::bad_alloc&) { result->result.emplace(Error{ErrorCode::QuotaExceeded}); }
            catch (...) { result->result.emplace(Error{ErrorCode::IoError}); }
            if (context) QMetaObject::invokeMethod(context, [result, continuation] { continuation.resume(); }, Qt::QueuedConnection);
        };
        if (!scheduler->enqueue({key, bytes, std::move(run), group}, max_count, max_bytes, max_count == 4096)) {
            state->result.emplace(Error{ErrorCode::QueueFull}); return false;
        }
        return true;
    }
    Result<T> await_resume() { return std::move(state->result.value()); }
};
template<typename T> QCoro::Task<Result<T>> execute(std::shared_ptr<Scheduler> scheduler, QUuid key, quint64 bytes,
        quint32 count, quint64 max_bytes, std::function<Result<T>()> function, QUuid group = {}) {
    // Named awaiter keeps nontrivial captures out of a temporary coroutine expression.
    WorkAwaiter<T> work{std::move(scheduler), key, bytes, count, max_bytes, std::move(function), group};
    co_return co_await work;
}
template<typename T> QCoro::Task<Result<T>> rejected(ErrorCode code) { co_return Error{code}; }
bool validPageEnvelope(const PageRequest& page) { return page.limit > 0 && page.limit <= 1000 && page.cursor.size() <= 4096; }
struct WriterState {
    std::shared_ptr<Namespace> ns;
    PendingId id;
    Fd fd;
    QVector<ByteRange> dirty;
    bool verifying = false;
    std::atomic_bool closed{false};
    QUuid lane = QUuid::createUuid();
};
struct ReaderState {
    std::shared_ptr<Namespace> ns;
    FileId id;
    Fd fd;
    Identity opened;
    std::atomic_bool closed{false};
    QUuid lane = QUuid::createUuid();
};
struct Subscriber {
    QUuid ns;
    QPointer<QObject> context;
    std::function<void(Change)> callback;
    std::atomic_bool queued{false};
    std::atomic_bool active{true};
    std::atomic<quint64> generation{0};
};
NamespaceInfo infoFor(Namespace& ns, Catalog& db) { return {ns.config, ns.root_id, db.generation(), charged(db), false, true, ns.untracked_staging, ns.staging_scan_truncated}; }
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
        const auto pending = db.scalar("SELECT id FROM pending WHERE parent=? AND name_key=? AND file_id!=?", {uuid(parent), key, uuid(except)});
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
void checkPendingFd(WriterState& writer, Catalog& db, bool allow_verifying = false) {
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
    void notify(NamespaceId id, quint64 generation) {
        std::lock_guard lock(mutex);
        for (auto& [key, sub] : subscribers) {
            Q_UNUSED(key);
            if (sub->ns != id || !sub->active || !sub->context) continue;
            sub->generation = generation;
            if (sub->queued.exchange(true)) continue;
            QMetaObject::invokeMethod(sub->context, [sub] {
                sub->queued = false;
                if (sub->active && sub->context) sub->callback({sub->ns, sub->generation.load(), true});
            }, Qt::QueuedConnection);
        }
    }
    template<typename T, typename Function> QCoro::Task<Result<T>> submit(NamespaceId id, Function function, quint64 bytes = 0, bool recovery = true, QUuid lane = {}) {
        auto self = shared_from_this();
        std::shared_ptr<Namespace> ns;
        try { ns = space(id); } catch (const Failure& error) { co_return error.error; }
        auto work = [self, ns, function = std::move(function), recovery]() mutable -> Result<T> {
            require(!self->shutting_down, ErrorCode::ShuttingDown);
            std::lock_guard lock(ns->mutex);
            auto current_root = openRoot(ns->config.storage_root, false);
            const auto expected_root = identity(ns->root.value, true); const auto actual_root = identity(current_root.value, true);
            require(expected_root.dev == actual_root.dev && expected_root.ino == actual_root.ino, ErrorCode::ExternalModification);
            auto current_internal = openBeneath(ns->root.value, ".filemanager", O_RDONLY | O_DIRECTORY);
            const auto expected_internal = identity(ns->internal.value, true); const auto actual_internal = identity(current_internal.value, true);
            require(expected_internal.dev == actual_internal.dev && expected_internal.ino == actual_internal.ino, ErrorCode::ExternalModification);
            Catalog db(ns->catalogPath(), ns->config.limits.max_catalog_bytes, false);
            if (recovery) recover(*ns, db);
            const auto before = db.generation();
            try {
                auto result = function(*ns, db);
                if (db.generation() != before) self->notify(ns->config.id, db.generation());
                return result;
            } catch (...) {
                if (db.generation() != before) self->notify(ns->config.id, db.generation());
                throw;
            }
        };
        co_return co_await execute<T>(scheduler, lane.isNull() ? id : lane, bytes, ns->config.limits.max_operations, ns->config.limits.max_buffered_bytes, std::move(work), id);
    }
    template<typename T, typename State, typename Function>
    QCoro::Task<Result<T>> handleSubmit(std::shared_ptr<State> state, Function function, quint64 bytes = 0) {
        return submit<T>(state->ns->config.id, std::move(function), bytes, true, state->lane);
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
            auto valid = subtractRange(db.ranges(state->id, RangeKind::StorageValid), affected);
            auto verified = subtractRange(db.ranges(state->id, RangeKind::ApplicationVerified), affected);
            auto dirty = addRange(subtractRange(state->dirty, affected), affected);
            auto final_ranges = valid;
            for (auto range : dirty) final_ranges = addRange(std::move(final_ranges), range);
            require(final_ranges.size() <= ns.config.limits.max_ranges && verified.size() <= ns.config.limits.max_ranges && dirty.size() <= ns.config.limits.max_ranges, ErrorCode::QuotaExceeded);
            const auto reserved = db.scalar("SELECT reserved FROM pending WHERE id=?", {uuid(state->id)}).toULongLong();
            const auto next_reserve = std::max(reserved, end); checkQuota(ns, db, next_reserve, reserved);
            {
                Transaction transaction(db);
                db.putRanges(state->id, RangeKind::StorageValid, valid);
                db.putRanges(state->id, RangeKind::ApplicationVerified, verified);
                db.query("UPDATE pending SET reserved=?,range_generation=range_generation+1,state=0 WHERE id=?", {qlonglong(next_reserve), uuid(state->id)});
                db.bump(); transaction.commit();
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
    QCoro::Task<Result<CommitResult>> commit(CancellationToken cancel) override;
    QCoro::Task<Result<void>> abort(CancellationToken cancel) override {
        auto state = state_;
        co_return co_await impl_->handleSubmit<void>(state, [state, cancel](Namespace& ns, Catalog& db) -> Result<void> {
            cancelCheck(cancel); checkPendingFd(*state, db);
            const auto st = identity(state->fd.value);
            const auto pending = db.pending(state->id);
            QJsonObject op{{"kind", "abort"}, {"file_id", uuid(pending.file_id)}, {"pending_id", uuid(state->id)},
                {"from", pendingPath(state->id)}, {"to", QStringLiteral(".filemanager/recovery/") + uuid(state->id)},
                {"dev", QString::number(st.dev)}, {"ino", QString::number(st.ino)}, {"directory", false}};
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
    auto state = std::make_shared<WriterState>(); state->ns = ns; state->id = id;
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
struct Verification {
    PendingInfo pending;
    Identity before;
    QCryptographicHash hash{QCryptographicHash::Sha256};
    quint64 offset = 0;
};
}
QCoro::Task<Result<CommitResult>> Writer::commit(CancellationToken cancel) {
    auto state = state_; auto impl = impl_;
    auto prepared = co_await impl->handleSubmit<std::shared_ptr<Verification>>(state, [state, cancel](Namespace& ns, Catalog& db) -> Result<std::shared_ptr<Verification>> {
        cancelCheck(cancel); checkpointWriter(*state, db);
        auto pending = db.pending(state->id);
        require(pending.final_size.has_value(), ErrorCode::IncompleteContent);
        require(covers(db.ranges(state->id, RangeKind::StorageValid), {0, *pending.final_size}), ErrorCode::IncompleteContent);
        if (pending.replacement) require(ns.readers.value(pending.file_id) == 0, ErrorCode::Busy);
        auto verification = std::make_shared<Verification>(); verification->pending = pending;
        verification->before = identity(state->fd.value);
        require(verification->before.size == *pending.final_size, ErrorCode::IncompleteContent);
        state->verifying = true;
        return verification;
    });
    if (!prepared) co_return prepared.error();
    auto verification = prepared.value();
    std::optional<Error> failure;
    // Each hashing chunk releases its worker/namespace turn before admitting the next.
    while (verification->offset < verification->before.size) {
        auto chunk = co_await impl->handleSubmit<void>(state, [state, verification, cancel](Namespace& ns, Catalog&) -> Result<void> {
            cancelCheck(cancel); require(!state->closed && state->verifying, ErrorCode::Busy);
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
        const auto expected = verification->hash.result();
        if (!pending.content.expected_sha256.isEmpty() && expected != pending.content.expected_sha256) {
            db.query("UPDATE pending SET state=2 WHERE id=?", {uuid(state->id)}); db.bump(); fail(ErrorCode::IntegrityError);
        }
        QString destination;
        if (ns.config.naming == PhysicalNaming::FileId) {
            const auto id = uuid(pending.file_id);
            const auto prefix = QStringLiteral("objects/") + id.left(2);
            try { auto dir = openBeneath(ns.root.value, prefix, O_RDONLY | O_DIRECTORY); identity(dir.value, true); }
            catch (const Failure& error) { if (error.error.code != ErrorCode::MissingEntry) throw; makeDirectory(ns.root.value, prefix); }
            destination = prefix + '/' + id;
        } else {
            const auto parent = db.path(pending.parent, ns.config.naming, ns.config.limits.max_depth);
            destination = parent == "." ? pending.name : parent + '/' + pending.name;
        }
        QJsonObject op{{"kind", pending.replacement ? "replace" : "create"}, {"file_id", uuid(pending.file_id)}, {"pending_id", uuid(pending.id)},
            {"parent", uuid(pending.parent)}, {"name", pending.name}, {"from", pendingPath(pending.id)}, {"to", destination},
            {"dev", QString::number(stat.dev)}, {"ino", QString::number(stat.ino)}, {"mtime", QString::number(stat.modified)},
            {"size", QString::number(*pending.final_size)}, {"sha", QString::fromLatin1(expected.toHex())}, {"directory", false}};
        if (pending.replacement) {
            const auto entry = db.entry(pending.file_id); validateEntry(ns, db, entry);
            auto old = openBeneath(ns.root.value, destination, O_RDONLY | O_NONBLOCK); const auto previous = identity(old.value);
            op["old_dev"] = QString::number(previous.dev); op["old_ino"] = QString::number(previous.ino);
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
NativeFileManager::~NativeFileManager() = default;
namespace {
Result<NamespaceInfo> acquireNamespace(std::shared_ptr<NativeFileManager::Impl> impl, NamespaceConfig config, bool create, CancellationToken cancel) {
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
        makeDirectory(ns->root.value, ".filemanager");
        makeDirectory(ns->root.value, ".filemanager/staging"); makeDirectory(ns->root.value, ".filemanager/recovery");
        if (config.naming == PhysicalNaming::FileId) makeDirectory(ns->root.value, "objects");
    }
    ns->internal = openBeneath(ns->root.value, ".filemanager", O_RDONLY | O_DIRECTORY); identity(ns->internal.value, true);
    ns->owner_lock = openBeneath(ns->internal.value, "owner.lock", O_RDWR | O_CREAT, 0600); identity(ns->owner_lock.value);
    if (::flock(ns->owner_lock.value, LOCK_EX | LOCK_NB) < 0) fail(ErrorCode::NamespaceBusy, errno);
    {
        auto file = openBeneath(ns->internal.value, "catalog.db", create ? O_RDWR | O_CREAT | O_EXCL : O_RDWR, create ? 0600 : 0); identity(file.value);
    }
    Catalog db(ns->catalogPath(), config.limits.max_catalog_bytes, create);
    if (create) {
        ns->root_id = QUuid::createUuid();
        Transaction transaction(db);
        db.setMeta("namespace", uuid(config.id)); db.setMeta("root", uuid(ns->root_id)); db.setMeta("naming", QString::number(int(config.naming)));
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
            auto q = db.query("SELECT id,dev,ino FROM pending WHERE id>? ORDER BY id LIMIT ?", {last, ns->config.limits.max_batch});
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
        auto q = db.query("SELECT id FROM pending LIMIT 4097");
        while (q.next()) { require(known.size() < 4096, ErrorCode::Corruption); known.insert(q.value(0).toString()); }
        auto fd = openBeneath(ns->root.value, ".filemanager/staging", O_RDONLY | O_DIRECTORY);
        auto* directory = ::fdopendir(std::exchange(fd.value, -1)); ioCheck(directory != nullptr);
        quint32 scanned = 0;
        while (auto* item = ::readdir(directory)) {
            const auto name = QString::fromUtf8(item->d_name);
            if (name == "." || name == "..") continue;
            if (++scanned > ns->config.limits.max_pending + ns->config.limits.max_batch) { ns->staging_scan_truncated = true; break; }
            if (!known.contains(name)) ++ns->untracked_staging;
        }
        ::closedir(directory);
    }
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
        for (auto& ns : spaces) { std::lock_guard lock(ns->mutex); Catalog db(ns->catalogPath(), ns->config.limits.max_catalog_bytes, false); result.append(infoFor(*ns, db)); }
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
        auto info = db.entry(id); validateEntry(space, db, info); return info;
    });
}
QCoro::Task<Result<DirectoryPage>> NativeFileManager::list(NamespaceId ns, DirectoryId parent, PageRequest page) {
    if (!validPageEnvelope(page)) return rejected<DirectoryPage>(ErrorCode::InvalidArgument);
    return impl_->submit<DirectoryPage>(ns, [parent, page = std::move(page)](Namespace& space, Catalog& db) -> Result<DirectoryPage> {
        const auto entry = db.entry(parent); require(entry.type == EntryType::Directory, ErrorCode::InvalidArgument); validateEntry(space, db, entry);
        const auto scope = "directory:" + uuid(parent); const auto generation = db.generation();
        const auto last = readCursor(page, generation, scope, space.config.limits.max_page_size);
        const auto separator = last.lastIndexOf(QChar::Null);
        const auto last_key = separator < 0 ? QString{} : last.left(separator);
        const auto last_id = separator < 0 ? QString{} : last.mid(separator + 1);
        auto q = db.query("SELECT id,name_key FROM entries WHERE parent=? AND (name_key>? OR (name_key=? AND id>?)) ORDER BY name_key,id LIMIT ?", {uuid(parent), last_key, last_key, last_id, page.limit + 1});
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
        require(db.scalar("SELECT (SELECT count(*) FROM entries)+(SELECT count(*) FROM pending WHERE replacement=0)").toULongLong() < space.config.limits.max_entries, ErrorCode::QuotaExceeded);
        const auto assigned = chooseName(space, db, parent, name, ConflictPolicy::Fail);
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
PendingId reservePending(Namespace& ns, Catalog& db, DirectoryId parent, QString name, ContentConstraints content, bool replacement, FileId file_id) {
    checkConstraints(content, ns.config.limits); checkParent(ns, db, parent);
    require(db.scalar("SELECT count(*) FROM pending").toUInt() < ns.config.limits.max_pending, ErrorCode::QuotaExceeded);
    require(ns.handles < ns.config.limits.max_handles && ns.active_writers.size() < ns.config.limits.max_writers, ErrorCode::QuotaExceeded);
    if (!replacement) require(db.scalar("SELECT (SELECT count(*) FROM entries)+(SELECT count(*) FROM pending WHERE replacement=0)").toULongLong() < ns.config.limits.max_entries, ErrorCode::QuotaExceeded);
    const auto reservation = content.expected_size.value_or(0); checkQuota(ns, db, reservation);
    const auto id = QUuid::createUuid();
    auto fd = openBeneath(ns.root.value, pendingPath(id), O_RDWR | O_CREAT | O_EXCL, 0600); const auto st = identity(fd.value);
    ioCheck(::fsync(fd.value) == 0); syncDirectory(ns.root.value, ".filemanager/staging");
    Transaction transaction(db);
    db.query("INSERT INTO pending(id,file_id,parent,name,name_key,expected_size,expected_sha,final_size,replacement,reserved,dev,ino) VALUES(?,?,?,?,?,?,?,?,?,?,?,?)",
        {uuid(id), uuid(file_id), uuid(parent), name, nameKey(name).value(), content.expected_size ? QVariant::fromValue(qlonglong(*content.expected_size)) : QVariant{},
         content.expected_sha256.isEmpty() ? QVariant{} : QVariant(content.expected_sha256), content.expected_size ? QVariant::fromValue(qlonglong(*content.expected_size)) : QVariant{}, replacement, qlonglong(reservation), QString::number(st.dev), QString::number(st.ino)});
    db.bump(); transaction.commit(); return id;
}
}
QCoro::Task<Result<PendingFile>> NativeFileManager::createFile(NamespaceId ns, CreateOptions options) {
    if (options.suggested_name.isEmpty() || options.suggested_name.size() > 255) return rejected<PendingFile>(ErrorCode::InvalidName);
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
        applyOperation(space, db, op); return {};
    });
}
QCoro::Task<Result<PendingPage>> NativeFileManager::listPending(NamespaceId ns, PageRequest page) {
    if (!validPageEnvelope(page)) return rejected<PendingPage>(ErrorCode::InvalidArgument);
    return impl_->submit<PendingPage>(ns, [page = std::move(page)](Namespace& space, Catalog& db) -> Result<PendingPage> {
        const auto generation = db.generation(); const auto last = readCursor(page, generation, "pending", space.config.limits.max_page_size);
        auto q = db.query("SELECT id FROM pending WHERE id>? ORDER BY id LIMIT ?", {last, page.limit + 1});
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
        auto entry = db.entry(id); require(entry.type == EntryType::File, ErrorCode::InvalidArgument); validateEntry(space, db, entry);
        require(space.handles < space.config.limits.max_handles, ErrorCode::QuotaExceeded);
        auto state = std::make_shared<ReaderState>(); state->ns = impl->space(space.config.id); state->id = id;
        state->fd = openBeneath(space.root.value, db.path(id, space.config.naming, space.config.limits.max_depth), O_RDONLY | O_NONBLOCK);
        state->opened = identity(state->fd.value); ++space.handles; ++space.readers[id];
        { std::lock_guard lock(impl->mutex);
            erase_if(impl->readers, [](const auto& weak) { auto item = weak.lock(); return !item || item->closed; });
            impl->readers.append(state);
        }
        return std::unique_ptr<IFileReader>(new Reader(impl, state));
    });
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
    if (!context || !callback || impl_->shutting_down) return Error{ErrorCode::InvalidArgument};
    std::lock_guard lock(impl_->mutex);
    auto space = impl_->spaces.value(ns); if (!space) return Error{ErrorCode::MissingEntry};
    quint32 count = 0;
    for (const auto& [id, existing] : impl_->subscribers) { Q_UNUSED(id); if (existing->ns == ns) ++count; }
    if (count >= space->config.limits.max_notifications || impl_->subscribers.size() >= 4096) return Error{ErrorCode::QuotaExceeded};
    auto sub = std::make_shared<Subscriber>(); sub->ns = ns; sub->context = context; sub->callback = std::move(callback);
    const auto id = impl_->next_subscription++; impl_->subscribers[id] = sub; return id;
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
    }
    for (auto lane : lanes) {
        auto drained = co_await execute<void>(impl->scheduler, lane, 0, 4096, 64ULL << 20, []() -> Result<void> { return {}; });
        if (!drained) failure = drained.error();
    }
    for (auto ns : spaces) {
        auto result = co_await execute<void>(impl->scheduler, ns->config.id, 0, 4096, 64ULL << 20, [impl, ns]() -> Result<void> {
            std::lock_guard lock(ns->mutex); Catalog db(ns->catalogPath(), ns->config.limits.max_catalog_bytes, false);
            std::optional<Error> error;
            try { recover(*ns, db); } catch (const Failure& failure) { error = failure.error; }
            QVector<std::shared_ptr<WriterState>> writers;
            { std::lock_guard guard(impl->mutex); for (auto& weak : impl->writers) if (auto writer = weak.lock(); writer && writer->ns == ns) writers.append(writer); }
            for (auto& writer : writers) if (!writer->closed) {
                writer->verifying = false;
                try { checkpointWriter(*writer, db); } catch (const Failure& failure) { error = failure.error; }
                closeWriter(*writer);
            }
            QVector<std::shared_ptr<ReaderState>> readers;
            { std::lock_guard guard(impl->mutex); for (auto& weak : impl->readers) if (auto reader = weak.lock(); reader && reader->ns == ns) readers.append(reader); }
            for (auto& reader : readers) if (!reader->closed) {
                reader->closed = true; reader->fd = Fd{}; if (--ns->readers[reader->id] == 0) ns->readers.remove(reader->id); --ns->handles;
            }
            if (error) return *error;
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
