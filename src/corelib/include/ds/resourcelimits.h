#ifndef DS_RESOURCE_LIMITS_H
#define DS_RESOURCE_LIMITS_H

#include <algorithm>
#include <cstdint>

#include <QSettings>

namespace ds::core {

struct ResourceLimits {
    static constexpr quint32 hardIncompleteHandshakes = 64;
    static constexpr quint32 defaultIncompleteHandshakes = 16;
    static constexpr quint32 hardUnapprovedConnections = 32;
    static constexpr quint32 defaultUnapprovedConnections = 8;
    static constexpr quint32 hardOutstandingRequests = 256;
    static constexpr quint32 defaultOutstandingRequests = 64;
    static constexpr quint32 hardPendingConnectionRequests = 256;
    static constexpr quint32 defaultPendingConnectionRequests = 64;
    static constexpr quint32 hardActiveFileTransfers = 2;
    static constexpr quint32 defaultActiveFileTransfers = 2;
    static constexpr quint32 hardQueuedFileOffers = 64;
    static constexpr quint32 defaultQueuedFileOffers = 16;
    static constexpr quint32 hardRetainedInterruptedTransfers = 32;
    static constexpr quint32 defaultRetainedInterruptedTransfers = 8;
    static constexpr quint64 hardAcceptedFileBytes = 1ULL * 1024 * 1024 * 1024 * 1024;
    static constexpr quint64 defaultAcceptedFileBytes = 10ULL * 1024 * 1024 * 1024;
    static constexpr quint64 hardUnfinishedIncomingBytes = 2ULL * 1024 * 1024 * 1024 * 1024;
    static constexpr quint64 defaultUnfinishedIncomingBytes = 20ULL * 1024 * 1024 * 1024;
    // The protocol recommends a desktop global unfinished-transfer budget of
    // 50 GiB, or a stricter fraction of available disk space.
    static constexpr quint64 hardIncomingDiskBytes = 50ULL * 1024 * 1024 * 1024;
    static constexpr quint64 defaultIncomingDiskBytes = hardIncomingDiskBytes;
    static constexpr quint32 hardMinimumFreeSpacePercent = 100;
    static constexpr quint32 defaultMinimumFreeSpacePercent = 10;
    static constexpr quint32 hardSocketOutputBytes = 256 * 1024;
    static constexpr quint32 defaultSocketOutputBytes = 256 * 1024;

    quint32 unapprovedConnections = defaultUnapprovedConnections;
    quint32 outstandingRequests = defaultOutstandingRequests;
    quint32 pendingConnectionRequests = defaultPendingConnectionRequests;
    quint32 activeFileTransfers = defaultActiveFileTransfers;
    quint32 queuedFileOffers = defaultQueuedFileOffers;
    quint32 retainedInterruptedTransfers = defaultRetainedInterruptedTransfers;
    quint64 acceptedFileBytes = defaultAcceptedFileBytes;
    quint64 unfinishedIncomingBytes = defaultUnfinishedIncomingBytes;
    quint64 incomingDiskBytes = defaultIncomingDiskBytes;
    quint32 minimumFreeSpacePercent = defaultMinimumFreeSpacePercent;
    quint32 socketOutputBytes = defaultSocketOutputBytes;

    static ResourceLimits fromSettings(const QSettings& settings)
    {
        ResourceLimits result;
        result.unapprovedConnections = bounded(settings, "resourceUnapprovedConnections",
                                               quint32{1}, hardUnapprovedConnections,
                                               defaultUnapprovedConnections);
        result.outstandingRequests = bounded(settings, "resourceOutstandingRequests",
                                             quint32{1}, hardOutstandingRequests,
                                             defaultOutstandingRequests);
        result.pendingConnectionRequests = bounded(settings, "resourcePendingConnectionRequests",
                                                   quint32{1}, hardPendingConnectionRequests,
                                                   defaultPendingConnectionRequests);
        result.activeFileTransfers = bounded(settings, "resourceActiveFileTransfers",
                                             quint32{1}, hardActiveFileTransfers,
                                             defaultActiveFileTransfers);
        result.queuedFileOffers = bounded(settings, "resourceQueuedFileOffers",
                                          quint32{1}, hardQueuedFileOffers,
                                          defaultQueuedFileOffers);
        result.retainedInterruptedTransfers = bounded(settings,
                                                      "resourceRetainedInterruptedTransfers",
                                                      quint32{1}, hardRetainedInterruptedTransfers,
                                                      defaultRetainedInterruptedTransfers);
        result.acceptedFileBytes = bounded(settings, "resourceAcceptedFileBytes",
                                           1ULL * 1024 * 1024, hardAcceptedFileBytes,
                                           defaultAcceptedFileBytes);
        result.unfinishedIncomingBytes = bounded(settings, "resourceUnfinishedIncomingBytes",
                                                 1ULL * 1024 * 1024,
                                                 hardUnfinishedIncomingBytes,
                                                 defaultUnfinishedIncomingBytes);
        result.incomingDiskBytes = bounded(settings, "resourceIncomingDiskBytes",
                                           1ULL * 1024 * 1024 * 1024,
                                           hardIncomingDiskBytes,
                                           defaultIncomingDiskBytes);
        result.minimumFreeSpacePercent = bounded(settings,
                                                 "resourceMinimumFreeSpacePercent",
                                                 quint32{1}, hardMinimumFreeSpacePercent,
                                                 defaultMinimumFreeSpacePercent);
        result.socketOutputBytes = bounded(settings, "resourceSocketOutputBytes",
                                           quint32{16 * 1024}, hardSocketOutputBytes,
                                           defaultSocketOutputBytes);
        return result;
    }

private:
    template <typename T>
    static T bounded(const QSettings& settings, const char* key, T minimum, T maximum,
                     T fallback)
    {
        bool ok = false;
        const auto value = settings.value(QLatin1String(key));
        const auto number = value.toString().toULongLong(&ok);
        if (!ok || number < minimum) {
            return fallback;
        }
        return static_cast<T>(std::min<quint64>(number, maximum));
    }
};

} // namespace ds::core

#endif // DS_RESOURCE_LIMITS_H
