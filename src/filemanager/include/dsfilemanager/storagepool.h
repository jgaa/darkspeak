#pragma once
#include <dsfilemanager/filemanager.h>
#include <array>

namespace dsfm {
using StoragePoolId = QUuid;
using ReservationId = QUuid;
inline constexpr quint64 storage_pool_journal_bytes = 64ULL << 20;
inline constexpr quint64 storage_pool_journal_reservation = 2 * storage_pool_journal_bytes + 65536;
struct StorageClassLimits {
    quint64 bytes = 64ULL << 30;
    quint32 objects = 100000;
    quint32 reservations = 4096;
};
struct StoragePoolLimits {
    quint64 minimum_available_bytes = 64ULL << 20;
    quint32 minimum_available_percent = 1;
    quint64 maximum_owned_bytes = 64ULL << 30;
    quint64 recovery_headroom = 16ULL << 20;
    std::array<StorageClassLimits, 4> classes{};
};
struct StorageReservation {
    ReservationId id;
    StoragePoolId pool;
    NamespaceId namespace_id;
    QUuid owner_scope;
    QUuid workflow;
    StorageClass storage_class = StorageClass::Incoming;
    quint64 reserved_total = 0;
    quint64 materialized_physical = 0;
};
struct StoragePoolUsage {
    quint64 materialized_physical = 0;
    quint64 outstanding = 0;
    quint32 reservations = 0;
};
namespace detail {
struct PoolAccess;
}
// Host-owned shared capacity authority. Calls are asynchronous and bounded;
// one profile's private journal and owner lock serialize its participating
// backends. Separate profiles are independent authorities; their allocations
// are external filesystem activity, observed through available space/DiskFull.
// Unknown allocations remain reserved until reconciled by their owning backend.
// IDs/usage are trusted local administration data.
class StoragePool final {
  public:
    struct Impl;
    explicit StoragePool(QString private_root, StoragePoolLimits defaults = {});
    ~StoragePool();
    QCoro::Task<Result<void>> open();
    QCoro::Task<Result<StoragePoolId>> registerNamespace(NamespaceId ns, QString root);
    QCoro::Task<Result<QVector<StoragePoolId>>> pools();
    QCoro::Task<Result<void>> setLimits(StoragePoolId pool, StoragePoolLimits limits);
    QCoro::Task<Result<StorageReservation>> reserve(StorageReservation request);
    QCoro::Task<Result<StorageReservation>> resize(ReservationId id, QUuid owner, quint64 total);
    QCoro::Task<Result<StorageReservation>> materialize(ReservationId id, QUuid owner, quint64 physical);
    QCoro::Task<Result<StorageReservation>> reclassify(ReservationId id, QUuid owner, StorageClass storage_class);
    QCoro::Task<Result<void>> release(ReservationId id, QUuid owner);
    QCoro::Task<Result<QVector<StorageReservation>>> reservations(NamespaceId ns, quint32 limit = 100,
                                                                  ReservationId after = {});
    QCoro::Task<Result<StoragePoolUsage>> usage(StoragePoolId pool);
    QCoro::Task<Result<void>> shutdown();

  private:
    friend struct detail::PoolAccess;
    std::shared_ptr<Impl> impl_;
};
} // namespace dsfm
