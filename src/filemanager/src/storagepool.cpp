#include <dsfilemanager/storagepool.h>
#include "storagepool_p.h"
#include "async_p.h"
#include "privatejournal_p.h"
#include <QCryptographicHash>
#include <QDataStream>
#include <QDir>
#include <QJsonArray>
#include <QJsonDocument>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <sys/statvfs.h>

namespace dsfm {
using namespace detail;
namespace {
constexpr quint64 maximum_amount = quint64(std::numeric_limits<qint64>::max()) / 4;
quint64 plus(quint64 a, quint64 b) {
    require(b <= maximum_amount && a <= maximum_amount - b, ErrorCode::QuotaExceeded);
    return a + b;
}
quint64 times(quint64 a, quint64 b) {
    require(!b || a <= maximum_amount / b, ErrorCode::QuotaExceeded);
    return a * b;
}
void valid(const StoragePoolLimits &limits) {
    require(limits.minimum_available_percent <= 100 && limits.maximum_owned_bytes <= maximum_amount &&
                limits.minimum_available_bytes <= maximum_amount && limits.recovery_headroom <= maximum_amount,
            ErrorCode::InvalidArgument);
    for (const auto &c : limits.classes)
        require(c.bytes <= maximum_amount && c.objects <= 10000000 && c.reservations <= 100000,
                ErrorCode::InvalidArgument);
}
QByteArray encode(const StoragePoolLimits &limits) {
    QJsonArray classes;
    for (auto c : limits.classes)
        classes.append(QJsonObject{
            {"bytes", QString::number(c.bytes)}, {"objects", int(c.objects)}, {"reservations", int(c.reservations)}});
    return QJsonDocument(QJsonObject{{"version", 1},
                                     {"minimumBytes", QString::number(limits.minimum_available_bytes)},
                                     {"minimumPercent", int(limits.minimum_available_percent)},
                                     {"maximumOwned", QString::number(limits.maximum_owned_bytes)},
                                     {"recoveryHeadroom", QString::number(limits.recovery_headroom)},
                                     {"classes", classes}})
        .toJson(QJsonDocument::Compact);
}
StoragePoolLimits decode(QByteArray bytes) {
    require(bytes.size() <= 4096, ErrorCode::Corruption);
    auto object = QJsonDocument::fromJson(bytes).object();
    require(object["version"] == 1 && object.size() == 6 && object["classes"].toArray().size() == 4,
            ErrorCode::Corruption);
    StoragePoolLimits limits;
    auto number = [&](QString key) {
        bool ok = false;
        auto value = object[key].toString().toULongLong(&ok);
        require(ok, ErrorCode::Corruption);
        return value;
    };
    limits.minimum_available_bytes = number("minimumBytes");
    limits.minimum_available_percent = object["minimumPercent"].toInt(101);
    limits.maximum_owned_bytes = number("maximumOwned");
    limits.recovery_headroom = number("recoveryHeadroom");
    for (int i = 0; i < 4; ++i) {
        auto c = object["classes"].toArray()[i].toObject();
        bool ok = false;
        auto bytes = c["bytes"].toString().toULongLong(&ok);
        require(ok, ErrorCode::Corruption);
        limits.classes[i] = {bytes, quint32(c["objects"].toInt()), quint32(c["reservations"].toInt())};
    }
    valid(limits);
    return limits;
}
QByteArray filesystem(int fd) {
    struct statfs info{};
    ioCheck(::fstatfs(fd, &info) == 0);
    require(info.f_fsid.__val[0] || info.f_fsid.__val[1], ErrorCode::UnsupportedCapability);
    QByteArray bytes;
    QDataStream stream(&bytes, QIODevice::WriteOnly);
    stream.setByteOrder(QDataStream::BigEndian);
    stream << quint32(1) << quint64(info.f_type) << quint32(info.f_fsid.__val[0]) << quint32(info.f_fsid.__val[1]);
    return QCryptographicHash::hash(bytes, QCryptographicHash::Sha256);
}
StorageReservation row(JournalStatement &s) {
    return {QUuid(s.text(0)), QUuid(s.text(1)),          QUuid(s.text(2)), QUuid(s.text(3)),
            QUuid(s.text(4)), StorageClass(s.number(5)), s.number(6),      s.number(7)};
}
constexpr auto columns = "id,pool,ns,owner,workflow,class,total,materialized";
struct Binding {
    Fd root;
    QString path;
    Identity identity;
    QByteArray filesystem;
    StoragePoolId pool;
};
struct Disk {
    Fd root, owner_lock;
    PrivateJournal journal;
};
} // namespace
struct StoragePool::Impl : std::enable_shared_from_this<Impl> {
    QString path;
    StoragePoolLimits defaults;
    std::shared_ptr<Disk> disk = std::make_shared<Disk>();
    std::shared_ptr<Scheduler> scheduler = Scheduler::create();
    QUuid lane = QUuid::createUuid();
    std::mutex mutex;
    QHash<NamespaceId, std::shared_ptr<Binding>> bindings;
    bool opened = false;
    NamespaceId journal_namespace;
    StoragePoolId journal_pool;
    std::atomic_bool closing = false;
    Impl(QString path, StoragePoolLimits defaults) : path(std::move(path)), defaults(defaults) {}
    ~Impl() {
        auto retired = std::move(disk);
        scheduler->enqueue({lane, 0, [retired = std::move(retired)]() mutable { retired.reset(); }}, 4096, 8ULL << 20,
                           true);
        scheduler->stop();
    }
    template <class T, class F> QCoro::Task<Result<T>> run(F fn, bool shutdown = false) {
        if (closing && !shutdown)
            co_return Error{ErrorCode::ShuttingDown};
        auto self = shared_from_this();
        co_return co_await execute<T>(scheduler, lane, 0, 128, 8ULL << 20,
                                      [self, fn = std::move(fn)]() mutable -> Result<T> {
                                          std::lock_guard lock(self->mutex);
                                          return fn(*self);
                                      });
    }
    void ready() {
        require(opened && disk->journal.db, ErrorCode::StorageUnavailable);
        auto current = openRoot(path, false);
        auto actual = identity(current.value, true);
        auto anchored = identity(disk->root.value, true);
        require(actual.dev == anchored.dev && actual.ino == anchored.ino, ErrorCode::ExternalModification);
    }
    std::shared_ptr<Binding> binding(NamespaceId ns) {
        auto b = bindings.value(ns);
        require(bool(b), ErrorCode::StorageUnavailable);
        auto current = openRoot(b->path, false, false);
        auto id = identity(current.value, true);
        require(id.dev == b->identity.dev && id.ino == b->identity.ino && filesystem(current.value) == b->filesystem,
                ErrorCode::ExternalModification);
        return b;
    }
    StoragePoolLimits limits(StoragePoolId pool) {
        JournalStatement q(disk->journal.db, "SELECT limits FROM pools WHERE id=?");
        q.text(1, uuid(pool));
        require(q.next(), ErrorCode::MissingEntry);
        return decode(q.blob(0));
    }
    StorageReservation get(ReservationId id, QUuid owner) {
        auto sql = QByteArray("SELECT ") + columns + " FROM reservations WHERE id=?";
        JournalStatement q(disk->journal.db, sql.constData());
        q.text(1, uuid(id));
        require(q.next(), ErrorCode::MissingEntry);
        auto r = row(q);
        require(r.owner_scope == owner, ErrorCode::MissingEntry);
        return r;
    }
    void observeJournal() {
        if (journal_namespace.isNull())
            return;
        auto file = openBeneath(disk->root.value, "catalog.db", O_RDONLY | O_NONBLOCK);
        struct stat db_stat{}, root_stat{};
        ioCheck(::fstat(file.value, &db_stat) == 0 && ::fstat(disk->root.value, &root_stat) == 0);
        require(db_stat.st_blocks >= 0 && root_stat.st_blocks >= 0, ErrorCode::Corruption);
        const auto physical = (quint64(db_stat.st_blocks) + quint64(root_stat.st_blocks)) * 512;
        require(physical <= storage_pool_journal_reservation, ErrorCode::QuotaExceeded);
        JournalStatement update(disk->journal.db, "UPDATE reservations SET materialized=? WHERE id=? AND materialized!=?");
        update.number(1, physical);
        update.text(2, uuid(journal_namespace));
        update.number(3,physical);
        update.next();
    }
    StoragePoolUsage usage(StoragePoolId pool) {
        observeJournal();
        JournalStatement q(disk->journal.db,
                           "SELECT COALESCE(sum(materialized),0),COALESCE(sum(total-materialized),0),count(*) FROM "
                           "reservations WHERE pool=?");
        q.text(1, uuid(pool));
        require(q.next(), ErrorCode::Corruption);
        return {q.number(0), q.number(1), quint32(q.number(2))};
    }
    void admission(const StorageReservation &request, ReservationId excluding = {}) {
        require(!request.id.isNull() && !request.pool.isNull() && !request.namespace_id.isNull() &&
                    !request.owner_scope.isNull() && !request.workflow.isNull() && int(request.storage_class) >= 0 &&
                    int(request.storage_class) < 4 && request.reserved_total <= maximum_amount &&
                    request.materialized_physical <= request.reserved_total,
                ErrorCode::InvalidArgument);
        auto root = binding(request.namespace_id);
        require(root->pool == request.pool, ErrorCode::PermissionDenied);
        auto policy = limits(request.pool);
        observeJournal();
        JournalStatement q(disk->journal.db,
                           "SELECT COALESCE(sum(total),0),COALESCE(sum(total-materialized),0),COALESCE(sum(CASE WHEN "
                           "class=? THEN total ELSE 0 END),0),COALESCE(sum(CASE WHEN class=? THEN 1 ELSE 0 "
                           "END),0),count(*) FROM reservations WHERE pool=? AND id!=?");
        q.number(1, int(request.storage_class));
        q.number(2, int(request.storage_class));
        q.text(3, uuid(request.pool));
        q.text(4, uuid(excluding));
        require(q.next(), ErrorCode::Corruption);
        auto total = q.number(0), outstanding = q.number(1), class_total = q.number(2), count = q.number(3),
             all = q.number(4);
        auto budget = policy.classes[size_t(request.storage_class)];
        require(plus(total, request.reserved_total) <= policy.maximum_owned_bytes &&
                    plus(class_total, request.reserved_total) <= budget.bytes && count < budget.objects &&
                    count < budget.reservations && all < 100000,
                ErrorCode::QuotaExceeded);
        struct statvfs available{};
        ioCheck(::fstatvfs(root->root.value, &available) == 0);
        const auto unit = available.f_frsize ? available.f_frsize : available.f_bsize;
        auto free = times(available.f_bavail, unit);
        require(available.f_bfree >= available.f_bavail && available.f_blocks >= available.f_bfree - available.f_bavail,
                ErrorCode::StorageUnavailable);
        auto capacity = times(available.f_blocks - (available.f_bfree - available.f_bavail), unit);
        auto percent = plus(times(capacity / 100, policy.minimum_available_percent),
                            (times(capacity % 100, policy.minimum_available_percent) + 99) / 100);
        auto needed = plus(plus(outstanding, request.reserved_total - request.materialized_physical),
                           plus(policy.recovery_headroom, std::max(policy.minimum_available_bytes, percent)));
        require(free >= needed, ErrorCode::QuotaExceeded);
    }
    StorageReservation reserve(StorageReservation request) {
        ready();
        if (request.id.isNull())
            request.id = QUuid::createUuid();
        require(!request.materialized_physical, ErrorCode::InvalidArgument);
        JournalTransaction tx(disk->journal);
        JournalStatement found(disk->journal.db, "SELECT owner FROM reservations WHERE id=?");
        found.text(1, uuid(request.id));
        if (found.next()) {
            auto old = get(request.id, request.owner_scope);
            require(old.pool == request.pool && old.namespace_id == request.namespace_id &&
                        old.workflow == request.workflow && old.storage_class == request.storage_class &&
                        old.reserved_total == request.reserved_total,
                    ErrorCode::Conflict);
            tx.commit();
            return old;
        }
        admission(request);
        JournalStatement put(disk->journal.db, "INSERT INTO reservations VALUES(?,?,?,?,?,?,?,0)");
        put.text(1, uuid(request.id));
        put.text(2, uuid(request.pool));
        put.text(3, uuid(request.namespace_id));
        put.text(4, uuid(request.owner_scope));
        put.text(5, uuid(request.workflow));
        put.number(6, int(request.storage_class));
        put.number(7, request.reserved_total);
        put.next();
        fault("before_pool_reservation_commit");
        tx.commit();
        fault("after_pool_reservation_commit");
        return request;
    }
    StorageReservation update(ReservationId id, QUuid owner, std::optional<quint64> total,
                              std::optional<quint64> physical, std::optional<StorageClass> storage_class) {
        ready();
        JournalTransaction tx(disk->journal);
        require(id != journal_namespace, ErrorCode::PermissionDenied);
        auto old = get(id, owner);
        auto next = old;
        if (total)
            next.reserved_total = *total;
        if (physical)
            next.materialized_physical = *physical;
        if (storage_class)
            next.storage_class = *storage_class;
        require(next.materialized_physical <= next.reserved_total && next.reserved_total <= maximum_amount &&
                    int(next.storage_class) >= 0 && int(next.storage_class) < 4,
                ErrorCode::InvalidArgument);
        // Reclamation and reporting already allocated bytes never require free
        // space. New promises/class transitions must pass admission atomically.
        if (next.reserved_total > old.reserved_total || next.storage_class != old.storage_class)
            admission(next, id);
        else
            binding(next.namespace_id);
        if(next.reserved_total==old.reserved_total && next.materialized_physical==old.materialized_physical && next.storage_class==old.storage_class){tx.commit();return next;}
        JournalStatement put(disk->journal.db, "UPDATE reservations SET total=?,materialized=?,class=? WHERE id=?");
        put.number(1, next.reserved_total);
        put.number(2, next.materialized_physical);
        put.number(3, int(next.storage_class));
        put.text(4, uuid(id));
        put.next();
        tx.commit();
        return next;
    }
    StorageReservation reconcile(StorageReservation request) {
        ready();
        require(!request.id.isNull() && !request.owner_scope.isNull() && !request.workflow.isNull() &&
                    request.materialized_physical <= request.reserved_total &&
                    request.reserved_total <= maximum_amount && int(request.storage_class) >= 0 &&
                    int(request.storage_class) < 4,
                ErrorCode::InvalidArgument);
        require(binding(request.namespace_id)->pool == request.pool, ErrorCode::PermissionDenied);
        JournalTransaction tx(disk->journal);
        JournalStatement old(disk->journal.db, "SELECT owner,ns,total,workflow,pool FROM reservations WHERE id=?");
        old.text(1, uuid(request.id));
        if (old.next()) {
            require(QUuid(old.text(0)) == request.owner_scope && QUuid(old.text(1)) == request.namespace_id &&
                        QUuid(old.text(3)) == request.workflow && QUuid(old.text(4)) == request.pool,
                    ErrorCode::Conflict);
            // A participant may have crashed before recording its growth. Never
            // shrink a durable promise based on a less advanced participant.
            request.reserved_total = std::max(request.reserved_total, old.number(2));
        } else {
            JournalStatement count(disk->journal.db, "SELECT count(*) FROM reservations WHERE pool=?");
            count.text(1, uuid(request.pool));
            require(count.next() && count.number(0) < 100000, ErrorCode::QuotaExceeded);
        }
        JournalStatement put(disk->journal.db,
                             "INSERT INTO reservations VALUES(?,?,?,?,?,?,?,?) ON CONFLICT(id) DO UPDATE SET "
                             "total=excluded.total,materialized=excluded.materialized,class=excluded.class");
        put.text(1, uuid(request.id));
        put.text(2, uuid(request.pool));
        put.text(3, uuid(request.namespace_id));
        put.text(4, uuid(request.owner_scope));
        put.text(5, uuid(request.workflow));
        put.number(6, int(request.storage_class));
        put.number(7, request.reserved_total);
        put.number(8, request.materialized_physical);
        put.next();
        tx.commit();
        return request;
    }
    StoragePoolId bind(NamespaceId ns, QString path) {
        ready();
        require(!ns.isNull() && path.size() <= 32768 && QDir::isAbsolutePath(path), ErrorCode::InvalidArgument);
        auto binding = std::make_shared<Binding>();
        binding->root = openRoot(path, false, false);
        binding->identity = identity(binding->root.value, true);
        binding->path = path;
        binding->filesystem = filesystem(binding->root.value);
        JournalTransaction tx(disk->journal);
        JournalStatement q(disk->journal.db, "SELECT id FROM pools WHERE filesystem=?");
        q.blob(1, binding->filesystem);
        if (q.next())
            binding->pool = QUuid(q.text(0));
        else {
            JournalStatement count(disk->journal.db, "SELECT count(*) FROM pools");
            require(count.next() && count.number(0) < 128, ErrorCode::QuotaExceeded);
            binding->pool = QUuid::createUuid();
            JournalStatement insert(disk->journal.db, "INSERT INTO pools VALUES(?,?,?)");
            insert.text(1, uuid(binding->pool));
            insert.blob(2, binding->filesystem);
            insert.blob(3, encode(defaults));
            insert.next();
        }
        JournalStatement old(disk->journal.db, "SELECT pool,dev,ino FROM namespaces WHERE id=?");
        old.text(1, uuid(ns));
        if (old.next())
            require(QUuid(old.text(0)) == binding->pool && old.text(1) == QString::number(binding->identity.dev) &&
                        old.text(2) == QString::number(binding->identity.ino),
                    ErrorCode::ExternalModification);
        else {
            JournalStatement count(disk->journal.db, "SELECT count(*) FROM namespaces");
            require(count.next() && count.number(0) < 1024, ErrorCode::QuotaExceeded);
            JournalStatement insert(disk->journal.db, "INSERT INTO namespaces VALUES(?,?,?,?)");
            insert.text(1, uuid(ns));
            insert.text(2, uuid(binding->pool));
            insert.text(3, QString::number(binding->identity.dev));
            insert.text(4, QString::number(binding->identity.ino));
            insert.next();
        }
        tx.commit();
        bindings.insert(ns, binding);
        return binding->pool;
    }
    void release(ReservationId id, QUuid owner) {
        ready();
        require(id != journal_namespace, ErrorCode::PermissionDenied);
        JournalTransaction tx(disk->journal);
        JournalStatement q(disk->journal.db, "SELECT owner FROM reservations WHERE id=?");
        q.text(1, uuid(id));
        if (q.next()) {
            require(QUuid(q.text(0)) == owner, ErrorCode::MissingEntry);
            JournalStatement remove(disk->journal.db, "DELETE FROM reservations WHERE id=?");
            remove.text(1, uuid(id));
            remove.next();
        }
        fault("before_pool_release_commit");
        tx.commit();
    }
};
StoragePool::StoragePool(QString path, StoragePoolLimits defaults)
    : impl_(std::make_shared<Impl>(std::move(path), defaults)) {}
StoragePool::~StoragePool() = default;
QCoro::Task<Result<void>> StoragePool::open() {
    return impl_->run<void>([](Impl &self) -> Result<void> {
        require(!self.opened, ErrorCode::Busy);
        valid(self.defaults);
        require(QDir::isAbsolutePath(self.path) && self.path.size() <= 32768, ErrorCode::InvalidArgument);
        self.disk->root = openRoot(self.path, true);
        self.disk->owner_lock = openBeneath(self.disk->root.value, "owner.lock", O_RDWR | O_CREAT, 0600);
        identity(self.disk->owner_lock.value);
        require(::flock(self.disk->owner_lock.value, LOCK_EX | LOCK_NB) == 0, ErrorCode::NamespaceBusy);
        bool create = false;
        struct stat found{};
        if (::fstatat(self.disk->root.value, "catalog.db", &found, AT_SYMLINK_NOFOLLOW) < 0) {
            require(errno == ENOENT, ErrorCode::IoError);
            create = true;
        }
        self.disk->journal.open(self.disk->root.value, create);
        if (create) {
            JournalTransaction tx(self.disk->journal);
            self.disk->journal.exec(
                "CREATE TABLE meta(version INTEGER NOT NULL);INSERT INTO meta VALUES(1);CREATE TABLE pools(id TEXT "
                "PRIMARY KEY,filesystem BLOB UNIQUE NOT NULL,limits BLOB NOT NULL);CREATE TABLE namespaces(id TEXT "
                "PRIMARY KEY,pool TEXT NOT NULL,dev TEXT NOT NULL,ino TEXT NOT NULL);CREATE TABLE reservations(id TEXT "
                "PRIMARY KEY,pool TEXT NOT NULL,ns TEXT NOT NULL,owner TEXT NOT NULL,workflow TEXT NOT NULL,class "
                "INTEGER NOT NULL CHECK(class>=0 AND class<4),total INTEGER NOT NULL CHECK(total>=0),materialized "
                "INTEGER NOT NULL CHECK(materialized>=0 AND materialized<=total));CREATE INDEX reservation_pool ON "
                "reservations(pool,class);CREATE INDEX reservation_namespace ON reservations(ns,id);");
            tx.commit();
        }
        JournalStatement version(self.disk->journal.db, "SELECT version FROM meta");
        require(version.next() && version.number(0) == 1 && !version.next(), ErrorCode::UnsupportedCapability);
        self.opened = true;
        const auto root_identity = identity(self.disk->root.value, true);
        self.journal_namespace = QUuid::fromRfc4122(
            QCryptographicHash::hash(QByteArray("dsfm-pool-journal-v1") + filesystem(self.disk->root.value) +
                                         QByteArray::number(root_identity.dev) + ":" +
                                         QByteArray::number(root_identity.ino),
                                     QCryptographicHash::Sha256)
                .first(16));
        self.journal_pool = self.bind(self.journal_namespace, self.path);
        self.reserve({self.journal_namespace, self.journal_pool, self.journal_namespace, self.journal_namespace,
                      self.journal_namespace, StorageClass::Permanent, storage_pool_journal_reservation, 0});
        self.observeJournal();
        return {};
    });
}
QCoro::Task<Result<StoragePoolId>> StoragePool::registerNamespace(NamespaceId ns, QString path) {
    return impl_->run<StoragePoolId>(
        [ns, path = std::move(path)](Impl &self) -> Result<StoragePoolId> { return self.bind(ns, path); });
}
QCoro::Task<Result<QVector<StoragePoolId>>> StoragePool::pools() {
    return impl_->run<QVector<StoragePoolId>>([](Impl &self) -> Result<QVector<StoragePoolId>> {
        self.ready();
        JournalStatement q(self.disk->journal.db, "SELECT id FROM pools ORDER BY id LIMIT 129");
        QVector<StoragePoolId> result;
        while (q.next()) {
            require(result.size() < 128, ErrorCode::Corruption);
            result.append(QUuid(q.text(0)));
        }
        return result;
    });
}
QCoro::Task<Result<void>> StoragePool::setLimits(StoragePoolId pool, StoragePoolLimits limits) {
    return impl_->run<void>([pool, limits](Impl &self) -> Result<void> {
        self.ready();
        valid(limits);
        self.limits(pool);
        JournalTransaction tx(self.disk->journal);
        JournalStatement q(self.disk->journal.db, "UPDATE pools SET limits=? WHERE id=?");
        q.blob(1, encode(limits));
        q.text(2, uuid(pool));
        q.next();
        tx.commit();
        return {};
    });
}
QCoro::Task<Result<StorageReservation>> StoragePool::reserve(StorageReservation request) {
    return impl_->run<StorageReservation>([request](Impl &self) { return self.reserve(request); });
}
QCoro::Task<Result<StorageReservation>> StoragePool::resize(ReservationId id, QUuid owner, quint64 total) {
    return impl_->run<StorageReservation>([=](Impl &self) { return self.update(id, owner, total, {}, {}); });
}
QCoro::Task<Result<StorageReservation>> StoragePool::materialize(ReservationId id, QUuid owner, quint64 physical) {
    return impl_->run<StorageReservation>([=](Impl &self) { return self.update(id, owner, {}, physical, {}); });
}
QCoro::Task<Result<StorageReservation>> StoragePool::reclassify(ReservationId id, QUuid owner, StorageClass c) {
    return impl_->run<StorageReservation>([=](Impl &self) { return self.update(id, owner, {}, {}, c); });
}
QCoro::Task<Result<void>> StoragePool::release(ReservationId id, QUuid owner) {
    return impl_->run<void>([=](Impl &self) -> Result<void> {
        self.release(id, owner);
        return {};
    });
}
QCoro::Task<Result<QVector<StorageReservation>>> StoragePool::reservations(NamespaceId ns, quint32 limit,
                                                                           ReservationId after) {
    return impl_->run<QVector<StorageReservation>>([=](Impl &self) -> Result<QVector<StorageReservation>> {
        self.ready();
        require(limit && limit <= 1000, ErrorCode::InvalidArgument);
        auto sql = QByteArray("SELECT ") + columns + " FROM reservations WHERE ns=? AND id>? ORDER BY id LIMIT ?";
        JournalStatement q(self.disk->journal.db, sql.constData());
        q.text(1, uuid(ns));
        q.text(2, after.isNull() ? QString{} : uuid(after));
        q.number(3, limit);
        QVector<StorageReservation> result;
        while (q.next())
            result.append(row(q));
        return result;
    });
}
QCoro::Task<Result<StoragePoolUsage>> StoragePool::usage(StoragePoolId pool) {
    return impl_->run<StoragePoolUsage>([pool](Impl &self) -> Result<StoragePoolUsage> {
        self.ready();
        self.limits(pool);
        return self.usage(pool);
    });
}
QCoro::Task<Result<void>> StoragePool::shutdown() {
    impl_->closing = true;
    return impl_->run<void>(
        [](Impl &self) -> Result<void> {
            self.opened = false;
            self.bindings.clear();
            if (self.disk->journal.db) {
                sqlCheck(sqlite3_close(self.disk->journal.db));
                self.disk->journal.db = nullptr;
            }
            self.disk->owner_lock = Fd{};
            self.disk->root = Fd{};
            return {};
        },
        true);
}
namespace detail {
template <class T, class F> Result<T> poolSync(StoragePool::Impl &self, F fn) {
    try {
        std::lock_guard lock(self.mutex);
        require(!self.closing, ErrorCode::ShuttingDown);
        return fn();
    } catch (const Failure &f) {
        return f.error;
    } catch (...) {
        return Error{ErrorCode::IoError};
    }
}
Result<QVector<StorageReservation>> PoolAccess::reservations(StoragePool &pool, NamespaceId ns, ReservationId after) {
    return poolSync<QVector<StorageReservation>>(*pool.impl_, [&] {
        auto &self = *pool.impl_;
        self.ready();
        auto sql = QByteArray("SELECT ") + columns + " FROM reservations WHERE ns=? AND id>? ORDER BY id LIMIT 100";
        JournalStatement q(self.disk->journal.db, sql.constData());
        q.text(1, uuid(ns));
        q.text(2, after.isNull() ? QString{} : uuid(after));
        QVector<StorageReservation> result;
        while (q.next())
            result.append(row(q));
        return result;
    });
}
Result<StorageReservation> PoolAccess::reconcileAllocated(StoragePool &pool, StorageReservation request) {
    return poolSync<StorageReservation>(*pool.impl_, [&] { return pool.impl_->reconcile(request); });
}
Result<StoragePoolId> PoolAccess::registerNamespace(StoragePool &pool, NamespaceId ns, QString root) {
    return poolSync<StoragePoolId>(*pool.impl_, [&] { return pool.impl_->bind(ns, std::move(root)); });
}
Result<StorageReservation> PoolAccess::reserveOrGrow(StoragePool &pool, StorageReservation request) {
    return poolSync<StorageReservation>(*pool.impl_, [&] {
        auto &self = *pool.impl_;
        self.ready();
        std::optional<StorageReservation> found;
        try {
            found = self.get(request.id, request.owner_scope);
        } catch (const Failure &failure) {
            if (failure.error.code != ErrorCode::MissingEntry)
                throw;
        }
        if (!found)
            return self.reserve(request);
        require(found->namespace_id == request.namespace_id && found->pool == request.pool &&
                    found->workflow == request.workflow && found->storage_class == request.storage_class,
                ErrorCode::Conflict);
        return self.update(request.id, request.owner_scope, std::max(found->reserved_total, request.reserved_total), {},
                           {});
    });
}
Result<StorageReservation> PoolAccess::reserve(StoragePool &pool, StorageReservation r) {
    return poolSync<StorageReservation>(*pool.impl_, [&] { return pool.impl_->reserve(r); });
}
Result<void> PoolAccess::admitGrowth(StoragePool &pool, ReservationId id, QUuid owner) {
    return poolSync<void>(*pool.impl_, [&]() -> Result<void> {
        auto &self = *pool.impl_;
        self.ready();
        JournalTransaction tx(self.disk->journal);
        self.admission(self.get(id, owner), id);
        tx.commit();
        return {};
    });
}
Result<StorageReservation> PoolAccess::resize(StoragePool &pool, ReservationId id, QUuid owner, quint64 total) {
    return poolSync<StorageReservation>(*pool.impl_, [&] { return pool.impl_->update(id, owner, total, {}, {}); });
}
Result<StorageReservation> PoolAccess::materialize(StoragePool &pool, ReservationId id, QUuid owner, quint64 physical) {
    return poolSync<StorageReservation>(*pool.impl_, [&] { return pool.impl_->update(id, owner, {}, physical, {}); });
}
Result<StorageReservation> PoolAccess::reclassify(StoragePool &pool, ReservationId id, QUuid owner, StorageClass c) {
    return poolSync<StorageReservation>(*pool.impl_, [&] { return pool.impl_->update(id, owner, {}, {}, c); });
}
Result<void> PoolAccess::release(StoragePool &pool, ReservationId id, QUuid owner) {
    return poolSync<void>(*pool.impl_, [&]() -> Result<void> {
        pool.impl_->release(id, owner);
        return {};
    });
}
} // namespace detail
} // namespace dsfm
