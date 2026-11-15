#pragma once
#include "storagepool_p.h"
#include "internal.h"
#include <QCryptographicHash>
#include <sys/stat.h>
#include <fcntl.h>
namespace dsfm::detail {
inline quint64 poolEstimate(quint64 bytes) {
    constexpr quint64 unit = 65536;
    require(bytes <= quint64(std::numeric_limits<qint64>::max()) / 4 - unit * 2, ErrorCode::QuotaExceeded);
    return ((bytes + unit - 1) / unit) * unit + unit;
}
inline quint64 physicalBytes(int fd) {
    struct stat st{};
    ioCheck(::fstat(fd, &st) == 0);
    require(st.st_blocks >= 0 && quint64(st.st_blocks) <= quint64(std::numeric_limits<qint64>::max()) / 512,
            ErrorCode::ExternalModification);
    return quint64(st.st_blocks) * 512;
}
inline ReservationId metadataClaim(const Namespace &ns) {
    return QUuid::fromRfc4122(
        QCryptographicHash::hash(QByteArray("dsfm-metadata-v1") + ns.config.id.toRfc4122(), QCryptographicHash::Sha256)
            .first(16));
}
inline void requirePool(Result<StorageReservation> result) {
    if (!result)
        throw Failure{result.error()};
}
// Release only after both the namespace journal and its private payload paths
// prove that a previously admitted participant has no remaining allocation.
inline void recoverPoolClaims(Namespace &ns, Catalog &db) {
    if (!ns.config.storage_pool)
        return;
    ReservationId after;
    for (;;) {
        auto page = PoolAccess::reservations(*ns.config.storage_pool, ns.config.id, after);
        if (!page)
            throw Failure{page.error()};
        for (const auto &claim : page.value()) {
            after = claim.id;
            if (claim.id == metadataClaim(ns) || claim.workflow != claim.id)
                continue;
            const auto key = uuid(claim.id);
            if (db.scalar("SELECT 1 FROM pending WHERE id=? OR file_id=? UNION ALL SELECT 1 FROM pool_objects WHERE "
                          "reservation=? "
                          "UNION ALL SELECT 1 FROM temporaries WHERE id=? LIMIT 1",
                          {key, key, key, key})
                    .isValid())
                continue;
            if (db.scalar("SELECT 1 FROM operations WHERE phase NOT IN('Complete','RolledBack') AND "
                          "(json_extract(payload,'$.pool_reservation')=? OR "
                          "json_extract(payload,'$.old_pool_reservation')=?) LIMIT 1",
                          {key, key})
                    .isValid())
                continue;
            bool absent = true;
            for (auto directory : {QString("staging"), QString("recovery"), QString("artifacts")}) {
                auto parent = openBeneath(ns.internal.value, directory, O_RDONLY | O_DIRECTORY);
                struct stat st{};
                if (::fstatat(parent.value, key.toUtf8().constData(), &st, AT_SYMLINK_NOFOLLOW) == 0)
                    absent = false;
                else
                    ioCheck(errno == ENOENT);
                if (absent)
                    ioCheck(::fsync(parent.value) == 0);
            }
            if (!absent)
                continue;
            auto removed = PoolAccess::release(*ns.config.storage_pool, claim.id, claim.owner_scope);
            if (!removed)
                throw Failure{removed.error()};
        }
        if (page.value().size() < 100)
            break;
    }
}
inline quint64 nativeMetadataBound(const Namespace &ns, quint64 directories) {
    require(directories && directories <= ns.config.limits.max_entries, ErrorCode::Corruption);
    const auto physical_directories = directories + 5 + (ns.config.naming == PhysicalNaming::FileId ? 257 : 0);
    return ns.config.limits.max_catalog_bytes * 2 + physical_directories * 65536;
}
inline void poolMetadata(Namespace &ns, Catalog *catalog = nullptr, quint64 extra_directories = 0) {
    if (!ns.config.storage_pool)
        return;
    const auto amount = physicalBytes(ns.internal.value) + physicalBytes(ns.root.value);
    auto db = openBeneath(ns.internal.value, "catalog.db", O_RDONLY | O_NONBLOCK);
    const auto actual = amount + physicalBytes(db.value);
    if (catalog)
        ns.directory_count = catalog->meta("directory_count").toULongLong();
    const auto directories = ns.directory_count;
    require(directories <= ns.config.limits.max_entries, ErrorCode::Corruption);
    const auto total = std::max(actual, nativeMetadataBound(ns, directories + extra_directories));
    requirePool(
        PoolAccess::reserveOrGrow(*ns.config.storage_pool, {metadataClaim(ns), ns.pool_id, ns.config.id, ns.config.id,
                                                            ns.config.id, StorageClass::Permanent, total, 0}));
    requirePool(PoolAccess::materialize(*ns.config.storage_pool, metadataClaim(ns), ns.config.id, actual));
}
inline void poolPendingReserve(Namespace &ns, PendingId id, quint64 bytes) {
    if (!ns.config.storage_pool)
        return;
    poolMetadata(ns);
    requirePool(PoolAccess::reserve(*ns.config.storage_pool, {id, ns.pool_id, ns.config.id, ns.config.id, id,
                                                              StorageClass::Incoming, poolEstimate(bytes), 0}));
}
inline void poolPendingGrow(Namespace &ns, PendingId id, quint64 bytes) {
    if (!ns.config.storage_pool)
        return;
    poolMetadata(ns);
    requirePool(PoolAccess::resize(*ns.config.storage_pool, id, ns.config.id, poolEstimate(bytes)));
    auto allowed = PoolAccess::admitGrowth(*ns.config.storage_pool, id, ns.config.id);
    if (!allowed)
        throw Failure{allowed.error()};
}
inline void poolPendingObserve(Namespace &ns, PendingId id, int fd) {
    if (ns.config.storage_pool)
        requirePool(PoolAccess::materialize(*ns.config.storage_pool, id, ns.config.id, physicalBytes(fd)));
}
} // namespace dsfm::detail
