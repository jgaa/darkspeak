#pragma once
#include <dsfilemanager/storagepool.h>
namespace dsfm::detail {
// Synchronous backend hooks are only invoked on FileManager workers. They
// serialize under the same authority as public asynchronous pool operations.
struct PoolAccess {
    // Backend recovery only: restore promises proved by the namespace journal,
    // even if a lowered quota prevents new admissions.
    static Result<QVector<StorageReservation>> reservations(StoragePool &pool, NamespaceId ns,
                                                            ReservationId after = {});
    static Result<StorageReservation> reconcileAllocated(StoragePool &pool, StorageReservation request);
    static Result<StoragePoolId> registerNamespace(StoragePool &pool, NamespaceId ns, QString root);
    static Result<StorageReservation> reserveOrGrow(StoragePool &pool, StorageReservation request);
    static Result<StorageReservation> reserve(StoragePool &pool, StorageReservation request);
    static Result<void> admitGrowth(StoragePool &pool, ReservationId id, QUuid owner);
    static Result<StorageReservation> resize(StoragePool &pool, ReservationId id, QUuid owner, quint64 total);
    static Result<StorageReservation> materialize(StoragePool &pool, ReservationId id, QUuid owner, quint64 physical);
    static Result<StorageReservation> reclassify(StoragePool &pool, ReservationId id, QUuid owner,
                                                 StorageClass storage_class);
    static Result<void> release(StoragePool &pool, ReservationId id, QUuid owner);
};
} // namespace dsfm::detail
