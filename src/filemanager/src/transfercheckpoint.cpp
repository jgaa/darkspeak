#include <dsfilemanager/transfercheckpoint.h>
#include "async_p.h"
#include "privatejournal_p.h"
#include "native_pool_p.h"
#include <QFile>
#include <QCryptographicHash>
#include <QDir>
#include <sys/file.h>
#include <sys/stat.h>
namespace dsfm {
using namespace detail;
bool validCheckpointInterval(quint32 value) noexcept {
    return value >= minimum_checkpoint_interval && value <= maximum_checkpoint_interval &&
           value % minimum_checkpoint_interval == 0;
}
Result<quint32> negotiateCheckpointInterval(quint32 local, quint32 remote) {
    if (!validCheckpointInterval(local) || !validCheckpointInterval(remote))
        return Error{ErrorCode::InvalidArgument};
    return std::min(local, remote);
}
CheckpointWindow::CheckpointWindow(quint64 size, quint32 interval, quint64 confirmed)
    : size_(size), produced_(confirmed), confirmed_(confirmed), interval_(interval),
      valid_(validCheckpointInterval(interval) && confirmed <= size &&
             size <= quint64(std::numeric_limits<qint64>::max())) {}
quint64 CheckpointWindow::allowance() const noexcept {
    if (!valid_ || produced_ < confirmed_ || produced_ - confirmed_ > interval_)
        return 0;
    const auto next_boundary=(confirmed_/interval_+1)*quint64(interval_);
    if(produced_>=next_boundary)return 0;
    return std::min({size_-produced_,quint64(interval_)-(produced_-confirmed_),next_boundary-produced_});
}
Result<void> CheckpointWindow::admit(quint64 offset, quint64 length) {
    if (!valid_ || offset != produced_ || length > allowance())
        return Error{ErrorCode::InvalidArgument};
    produced_ += length;
    return {};
}
Result<void> CheckpointWindow::confirm(quint64 offset, bool pause_boundary) {
    if (!valid_ || offset < confirmed_ || offset > produced_ ||
        (offset%interval_!=0 && offset!=size_ && !pause_boundary))
        return Error{ErrorCode::InvalidArgument};
    confirmed_ = offset;
    return {};
}
namespace {
QByteArray keyBytes(TransferKey key) {
    require(!key.local_scope.isNull() && !key.remote_scope.isNull() && !key.transfer.isNull(),
            ErrorCode::InvalidArgument);
    return key.local_scope.toRfc4122() + key.remote_scope.toRfc4122() + key.transfer.toRfc4122();
}
QByteArray emptyDigest() { return QCryptographicHash::hash(QByteArray{}, QCryptographicHash::Sha256); }
bool validDescription(const TransferDescription &d) {
    return !d.key.local_scope.isNull() && !d.key.remote_scope.isNull() && !d.key.transfer.isNull() &&
           d.size <= (8ULL << 40) && d.sha256.size() == 32 && validCheckpointInterval(d.interval) && int(d.side) >= 0 &&
           int(d.side) <= 1;
}
bool validCheckpoint(const TransferDescription &d, const DurableCheckpoint &c) {
    return c.offset <= d.size && c.sha256.size() == 32 && (!c.final || c.offset == d.size) &&
           (c.offset % d.interval == 0 || c.final || c.pause_boundary) &&
           (c.offset == 0 ? c.sequence == 0 : c.sequence > 0) && c.sequence < (1ULL << 60) &&
           (c.offset != 0 || c.sha256 == emptyDigest());
}
DurableCheckpoint checkpointRow(JournalStatement &q) {
    return {q.number(0), q.number(1), q.blob(2), bool(q.number(3)), bool(q.number(4))};
}
} // namespace
struct TransferProgressStore::Impl : std::enable_shared_from_this<TransferProgressStore::Impl> {
    QString path;
    quint32 maximum;
    std::shared_ptr<StoragePool> pool;
    StoragePoolId pool_id;
    NamespaceId namespace_id;
    std::shared_ptr<Scheduler> scheduler = Scheduler::create();
    QUuid lane = QUuid::createUuid();
    struct Disk {
        Fd root, owner_lock;
        PrivateJournal journal;
    };
    std::shared_ptr<Disk> disk = std::make_shared<Disk>();
    bool opened = false;
    std::atomic_bool closing{false};
    Impl(QString p, quint32 m, std::shared_ptr<StoragePool> authority)
        : path(std::move(p)), maximum(m), pool(std::move(authority)) {
        namespace_id = QUuid::fromRfc4122(
            QCryptographicHash::hash(QByteArray("dsfm-transfer-progress-v1") + QFile::encodeName(path),
                                     QCryptographicHash::Sha256)
                .first(16));
    }
    ~Impl() {
        scheduler->enqueue({lane, 0, [retired = std::move(disk)]() mutable { retired.reset(); }}, 4096, 1U << 20, true);
        scheduler->stop();
    }
    template <class T, class F> QCoro::Task<Result<T>> run(F fn, bool cleanup = false) {
        auto self = shared_from_this();
        if (closing && !cleanup)
            co_return Error{ErrorCode::ShuttingDown};
        co_return co_await execute<T>(scheduler, lane, 0, 128, 1U << 20,
                                      [self, fn = std::move(fn)] { return fn(*self); });
    }
    void ready() {
        require(opened && disk->journal.db, ErrorCode::StorageUnavailable);
        auto current = openRoot(path, false);
        auto actual = identity(current.value, true);
        auto anchored = identity(disk->root.value, true);
        require(actual.dev == anchored.dev && actual.ino == anchored.ino, ErrorCode::ExternalModification);
    }
    DurableTransferProgress load(TransferKey key) {
        ready();
        auto token = keyBytes(key);
        JournalStatement q(disk->journal.db,
                           "SELECT size,sha,interval,attempt,confirmed,complete,pause,failure,next_sequence,side FROM "
                           "transfers WHERE key=?");
        q.blob(1, token);
        require(q.next(), ErrorCode::MissingEntry);
        DurableTransferProgress result;
        result.description = {key, q.number(0), q.blob(1), quint32(q.number(2))};
        require(q.number(9) <= 1, ErrorCode::Corruption);
        result.description.side = TransferSide(q.number(9));
        require(validDescription(result.description), ErrorCode::Corruption);
        result.attempt = QUuid::fromRfc4122(q.blob(3));
        result.confirmed_offset = q.number(4);
        result.complete = q.number(5);
        require(q.number(6) <= quint64(TransferPauseReason::FeaturePaused) &&
                    q.number(7) <= quint64(TransferFailureReason::UserAction),
                ErrorCode::Corruption);
        result.next_checkpoint_sequence = q.number(8);
        result.pause = TransferPauseReason(q.number(6));
        result.failure = TransferFailureReason(q.number(7));
        JournalStatement cp(
            disk->journal.db,
            "SELECT sequence,offset,sha,final,pause_boundary,phase FROM checkpoints WHERE key=? ORDER BY offset,sequence");
        cp.blob(1, token);
        while (cp.next()) {
            require(result.durable_checkpoints.size() < maximum_retained_checkpoints, ErrorCode::Corruption);
            auto c = checkpointRow(cp);
            require(validCheckpoint(result.description, c), ErrorCode::Corruption);
            require(cp.number(5)<=2,ErrorCode::Corruption);
            result.durable_checkpoints.append(c);
            if(cp.number(5)>=quint64(CheckpointPhase::SenderConfirmed))result.accepted_checkpoints.append(c);
            if(cp.number(5)==quint64(CheckpointPhase::MutuallyConfirmed))result.checkpoints.append(c);
        }
        require(!result.checkpoints.isEmpty() && result.checkpoints.front().offset == 0, ErrorCode::Corruption);
        return result;
    }
};
TransferProgressStore::TransferProgressStore(QString root, quint32 maximum, std::shared_ptr<StoragePool> authority)
    : impl_(std::make_shared<Impl>(std::move(root), maximum, std::move(authority))) {}
TransferProgressStore::~TransferProgressStore() = default;
QCoro::Task<Result<void>> TransferProgressStore::open() {
    return impl_->run<void>([](Impl &s) -> Result<void> {
        require(!s.opened && s.maximum && s.maximum <= 65536 && QDir::isAbsolutePath(s.path) && s.path.size() <= 32768,
                ErrorCode::InvalidArgument);
        s.disk->root = openRoot(s.path, true);
        s.disk->owner_lock = openBeneath(s.disk->root.value, "owner.lock", O_RDWR | O_CREAT, 0600);
        identity(s.disk->owner_lock.value);
        require(::flock(s.disk->owner_lock.value, LOCK_EX | LOCK_NB) == 0, ErrorCode::NamespaceBusy);
        struct stat st{};
        bool create = false;
        if (::fstatat(s.disk->root.value, "catalog.db", &st, AT_SYMLINK_NOFOLLOW) < 0) {
            ioCheck(errno == ENOENT);
            create = true;
        }
        if (s.pool) {
            auto bound = PoolAccess::registerNamespace(*s.pool, s.namespace_id, s.path);
            if (!bound)
                throw Failure{bound.error()};
            s.pool_id = bound.value();
            requirePool(PoolAccess::reserveOrGrow(*s.pool, {s.namespace_id, s.pool_id, s.namespace_id, s.namespace_id,
                                                            s.namespace_id, StorageClass::Permanent,
                                                            storage_pool_journal_reservation, 0}));
        }
        s.disk->journal.open(s.disk->root.value, create);
        if (create) {
            JournalTransaction tx(s.disk->journal);
            s.disk->journal.exec(
                "CREATE TABLE meta(version INTEGER NOT NULL);INSERT INTO meta VALUES(1);"
                "CREATE TABLE transfers(key BLOB PRIMARY KEY,size INTEGER NOT NULL,sha BLOB NOT NULL,interval INTEGER "
                "NOT NULL,"
                "attempt BLOB,confirmed INTEGER NOT NULL DEFAULT 0,complete INTEGER NOT NULL DEFAULT 0,pause INTEGER "
                "NOT NULL DEFAULT 0,failure INTEGER NOT NULL DEFAULT 0,next_sequence INTEGER NOT NULL DEFAULT 1,side "
                "INTEGER NOT NULL DEFAULT 0);"
                "CREATE TABLE checkpoints(key BLOB NOT NULL REFERENCES transfers(key),sequence INTEGER NOT NULL,offset "
                "INTEGER NOT NULL,sha BLOB NOT NULL,"
                "final INTEGER NOT NULL,pause_boundary INTEGER NOT NULL,phase INTEGER NOT NULL,PRIMARY "
                "KEY(key,sequence));");
            tx.commit();
            ioCheck(::fsync(s.disk->root.value) == 0);
        }
        JournalStatement version(s.disk->journal.db, "SELECT version FROM meta");
        require(version.next() && version.number(0) == checkpoint_format_version && !version.next(),
                ErrorCode::UnsupportedCapability);
        auto file = openBeneath(s.disk->root.value, "catalog.db", O_RDONLY | O_NONBLOCK);
        if (s.pool)
            requirePool(PoolAccess::materialize(*s.pool, s.namespace_id, s.namespace_id,
                                                physicalBytes(file.value) + physicalBytes(s.disk->root.value)));
        s.opened = true;
        LFLOG_DEBUG << "Generic durable transfer-progress journal ready";
        return {};
    });
}
QCoro::Task<Result<DurableTransferProgress>> TransferProgressStore::create(TransferDescription d) {
    return impl_->run<DurableTransferProgress>([d = std::move(d)](Impl &s) -> Result<DurableTransferProgress> {
        s.ready();
        require(validDescription(d), ErrorCode::InvalidArgument);
        auto key = keyBytes(d.key);
        JournalTransaction tx(s.disk->journal);
        JournalStatement old(s.disk->journal.db, "SELECT 1 FROM transfers WHERE key=?");
        old.blob(1, key);
        if (old.next()) {
            auto result = s.load(d.key);
            require(result.description.size == d.size && result.description.sha256 == d.sha256 &&
                        result.description.interval == d.interval && result.description.side == d.side,
                    ErrorCode::Conflict);
            tx.commit();
            return result;
        }
        JournalStatement count(s.disk->journal.db, "SELECT count(*) FROM transfers");
        require(count.next() && count.number(0) < s.maximum, ErrorCode::QuotaExceeded);
        JournalStatement put(s.disk->journal.db, "INSERT INTO transfers(key,size,sha,interval,side) VALUES(?,?,?,?,?)");
        put.blob(1, key);
        put.number(2, d.size);
        put.blob(3, d.sha256);
        put.number(4, d.interval);
        put.number(5, int(d.side));
        put.next();
        JournalStatement zero(s.disk->journal.db, "INSERT INTO checkpoints VALUES(?,0,0,?,0,0,2)");
        zero.blob(1, key);
        zero.blob(2, emptyDigest());
        zero.next();
        fault("before_transfer_create_commit");
        tx.commit();
        return s.load(d.key);
    });
}
QCoro::Task<Result<DurableTransferProgress>> TransferProgressStore::load(TransferKey key) {
    return impl_->run<DurableTransferProgress>([key](Impl &s) { return s.load(key); });
}
QCoro::Task<Result<DurableCheckpoint>> TransferProgressStore::selectResume(TransferKey key,
                                                                           QVector<DurableCheckpoint> retained) {
    if (retained.isEmpty() || retained.size() > maximum_retained_checkpoints)
        co_return Error{ErrorCode::InvalidArgument};
    co_return co_await impl_->run<DurableCheckpoint>([key, retained = std::move(retained)](
                                                         Impl &s) -> Result<DurableCheckpoint> {
        auto saved = s.load(key);
        require(saved.failure == TransferFailureReason::None, ErrorCode::Conflict);
        std::optional<DurableCheckpoint> selected;
        QSet<quint64> seen;
        for (const auto &c : retained) {
            require(validCheckpoint(saved.description, c) && !seen.contains(c.sequence), ErrorCode::InvalidArgument);
            seen.insert(c.sequence);
            for (const auto &local : saved.description.side==TransferSide::Sender ? saved.accepted_checkpoints : saved.checkpoints)
                if (local.sequence == c.sequence && local.offset == c.offset && local.sha256 == c.sha256 &&
                    (!selected || local.offset > selected->offset || (local.offset==selected->offset && local.sequence>selected->sequence)))
                    selected = local;
        }
        require(selected.has_value(), ErrorCode::IntegrityError);
        return *selected;
    });
}
QCoro::Task<Result<void>> TransferProgressStore::beginAttempt(TransferKey key, QUuid attempt,
                                                              DurableCheckpoint selected, quint64 next_sequence) {
    return impl_->run<void>([=](Impl &s) -> Result<void> {
        require(!attempt.isNull(), ErrorCode::InvalidArgument);
        s.ready();
        JournalTransaction tx(s.disk->journal);
        auto saved = s.load(key);
        require(saved.failure == TransferFailureReason::None &&
            (!saved.complete || (selected.final && selected.offset==saved.description.size && selected.sha256==saved.description.sha256)),ErrorCode::Conflict);
        bool found = false;
        for (auto &cp : saved.description.side==TransferSide::Sender ? saved.accepted_checkpoints : saved.durable_checkpoints)
            if (cp == selected)
                found = true;
        require(found, ErrorCode::IntegrityError);
        if (saved.attempt == attempt) {
            require(saved.confirmed_offset == selected.offset && (!next_sequence || next_sequence==saved.next_checkpoint_sequence), ErrorCode::Conflict);
            tx.commit();
            return {};
        }
        quint64 highest_confirmed=0;
        for(const auto &checkpoint:saved.accepted_checkpoints)highest_confirmed=std::max(highest_confirmed,checkpoint.sequence);
        const auto next=next_sequence?next_sequence:saved.next_checkpoint_sequence;
        require(next>highest_confirmed && next<(1ULL<<60),
            ErrorCode::InvalidArgument);
        JournalStatement stale(s.disk->journal.db,"DELETE FROM checkpoints WHERE key=? AND phase=0 AND sequence!=?");
        stale.blob(1,keyBytes(key));stale.number(2,selected.sequence);stale.next();
        // A resumed stream starts at a sender-selected, previously mutual prefix.
        JournalStatement put(s.disk->journal.db, "UPDATE transfers SET attempt=?,confirmed=?,next_sequence=?,pause=0 WHERE key=?");
        put.blob(1, attempt.toRfc4122());
        put.number(2, selected.offset);
        put.number(3,next);
        put.blob(4, keyBytes(key));
        put.next();
        tx.commit();
        return {};
    });
}
QCoro::Task<Result<void>> TransferProgressStore::record(TransferKey key, QUuid attempt, DurableCheckpoint cp,
                                                        CheckpointPhase phase) {
    return impl_->run<void>([=](Impl &s) -> Result<void> {
        s.ready();
        JournalTransaction tx(s.disk->journal);
        auto saved = s.load(key);
        auto token = keyBytes(key);
        require(!attempt.isNull() && saved.attempt == attempt &&
                    (!saved.complete || (cp.final && cp.offset==saved.description.size && cp.sha256==saved.description.sha256)) &&
                    saved.failure == TransferFailureReason::None && saved.pause == TransferPauseReason::None,
                ErrorCode::Conflict);
        require(validCheckpoint(saved.description, cp) && int(phase) >= 0 && int(phase) <= 2,
                ErrorCode::InvalidArgument);
        JournalStatement old(
            s.disk->journal.db,
            "SELECT offset,sha,final,pause_boundary,phase FROM checkpoints WHERE key=? AND sequence=?");
        old.blob(1, token);
        old.number(2, cp.sequence);
        if (old.next()) {
            require(old.number(0) == cp.offset && old.blob(1) == cp.sha256 &&
                        (old.number(2) == quint64(cp.final) || cp.offset == 0) &&
                        old.number(3) == quint64(cp.pause_boundary),
                    ErrorCode::IntegrityError);
            if (cp.offset < saved.confirmed_offset && old.number(4) == quint64(CheckpointPhase::MutuallyConfirmed)) {
                tx.commit();
                return {};
            }
        } else {
            require(cp.sequence == saved.next_checkpoint_sequence, ErrorCode::InvalidArgument);
            JournalStatement next(s.disk->journal.db, "UPDATE transfers SET next_sequence=next_sequence+1 WHERE key=?");
            next.blob(1, token);
            next.next();
        }
        require(cp.offset >= saved.confirmed_offset && cp.offset - saved.confirmed_offset <= saved.description.interval,
                ErrorCode::InvalidArgument);
        JournalStatement put(s.disk->journal.db,
                             "INSERT INTO checkpoints VALUES(?,?,?,?,?,?,?) ON CONFLICT(key,sequence) DO UPDATE SET "
                             "phase=max(phase,excluded.phase),final=max(final,excluded.final)");
        put.blob(1, token);
        put.number(2, cp.sequence);
        put.number(3, cp.offset);
        put.blob(4, cp.sha256);
        put.number(5, cp.final && (cp.offset != 0 || phase == CheckpointPhase::MutuallyConfirmed));
        put.number(6, cp.pause_boundary);
        put.number(7, int(phase));
        put.next();
        if (phase == CheckpointPhase::MutuallyConfirmed) {
            JournalStatement update(s.disk->journal.db, "UPDATE transfers SET confirmed=? WHERE key=?");
            update.number(1, cp.offset);
            update.blob(2, token);
            update.next();
        }
        // Keep zero plus the newest bounded set. Never retain unbounded peer history.
        JournalStatement trim(s.disk->journal.db,
                              "DELETE FROM checkpoints WHERE key=? AND sequence!=0 AND sequence NOT IN(SELECT sequence "
                              "FROM checkpoints WHERE key=? AND sequence!=0 ORDER BY sequence DESC LIMIT 63)");
        trim.blob(1, token);
        trim.blob(2, token);
        trim.next();
        fault("before_transfer_checkpoint_commit");
        tx.commit();
        fault("after_transfer_checkpoint_commit");
        LFLOG_TRACE << "Generic transfer checkpoint persisted; offset=" << cp.offset << ", phase=" << int(phase);
        return {};
    });
}
QCoro::Task<Result<void>> TransferProgressStore::complete(TransferKey key, QUuid attempt, bool verified,
                                                          bool acknowledged) {
    return impl_->run<void>([=](Impl &s) -> Result<void> {
        s.ready();
        JournalTransaction tx(s.disk->journal);
        auto saved = s.load(key);
        require(verified && acknowledged && saved.attempt == attempt && !attempt.isNull() &&
                    saved.failure == TransferFailureReason::None && saved.pause == TransferPauseReason::None,
                ErrorCode::IncompleteContent);
        bool final = false;
        for (const auto &cp : saved.checkpoints)
            if (cp.final && cp.offset == saved.description.size && cp.sha256 == saved.description.sha256)
                final = true;
        require(final && saved.confirmed_offset == saved.description.size, ErrorCode::IncompleteContent);
        JournalStatement update(s.disk->journal.db, "UPDATE transfers SET complete=1,pause=0 WHERE key=?");
        update.blob(1, keyBytes(key));
        update.next();
        tx.commit();
        return {};
    });
}
QCoro::Task<Result<void>> TransferProgressStore::pause(TransferKey key, QUuid attempt, TransferPauseReason reason) {
    return impl_->run<void>([=](Impl &s) -> Result<void> {
        require(int(reason) > 0 && reason <= TransferPauseReason::FeaturePaused, ErrorCode::InvalidArgument);
        s.ready();
        JournalTransaction tx(s.disk->journal);
        auto saved = s.load(key);
        require(!attempt.isNull() && saved.attempt == attempt && !saved.complete &&
                    saved.failure == TransferFailureReason::None, ErrorCode::Conflict);
        JournalStatement update(s.disk->journal.db, "UPDATE transfers SET pause=? WHERE key=?");
        LFLOG_DEBUG << "Generic transfer state changed; reason=" << int(reason);
        update.number(1, int(reason));
        update.blob(2, keyBytes(key));
        update.next();
        tx.commit();
        return {};
    });
}
QCoro::Task<Result<void>> TransferProgressStore::fail(TransferKey key, TransferFailureReason reason) {
    return impl_->run<void>([=](Impl &s) -> Result<void> {
        require(int(reason) > 0 && reason <= TransferFailureReason::UserAction, ErrorCode::InvalidArgument);
        s.ready();
        JournalTransaction tx(s.disk->journal);
        auto saved = s.load(key);
        require(!saved.complete && (saved.failure == TransferFailureReason::None || saved.failure == reason),
                ErrorCode::Conflict);
        JournalStatement update(s.disk->journal.db, "UPDATE transfers SET failure=?,pause=0 WHERE key=?");
        LFLOG_DEBUG << "Generic transfer state changed; reason=" << int(reason);
        update.number(1, int(reason));
        update.blob(2, keyBytes(key));
        update.next();
        tx.commit();
        return {};
    });
}
QCoro::Task<Result<void>> TransferProgressStore::shutdown() {
    impl_->closing = true;
    return impl_->run<void>(
        [](Impl &s) -> Result<void> {
            s.opened = false;
            if (s.disk->journal.db) {
                sqlCheck(sqlite3_close(s.disk->journal.db));
                s.disk->journal.db = nullptr;
            }
            s.disk->owner_lock = {};
            s.disk->root = {};
            return {};
        },
        true);
}
} // namespace dsfm
