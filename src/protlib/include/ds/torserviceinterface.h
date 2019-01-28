#ifndef TORSERVICESOCKET_H
#define TORSERVICESOCKET_H

#include <memory>

#include <QObject>
#include <QTcpServer>
#include <QTcpSocket>
#include <QUuid>

#include "ds/connectionsocket.h"

namespace ds {
namespace prot {

struct StopServiceResult {
    // The port the service was using
    uint16_t port = {};

    // True if the servive was running.
    bool wasStopped = false;
};

struct StartServiceResult {
    uint16_t port = {};
    StopServiceResult stopped;
};


/*! Provides an interface to the Tor network.
 *
 * An instance of this class can listen for icoming connections
 * and connect to Tor hidden services. It is designed to handle
 * the needs of one Identity.
 */
class TorServiceInterface : public QObject
{
    Q_OBJECT

public:
    using ptr_t = std::shared_ptr<TorServiceInterface>;

    TorServiceInterface();
    virtual ~TorServiceInterface();

    /*! Start a service.
     * Any existing service will be terminated.
     */
    StartServiceResult startService();

    /*! Stop the service if it is running */
    StopServiceResult stopService();

    /*! Connect to a Tor hidden service */
    QUuid connectToService(const QByteArray& host, const std::uint16_t port);

    /*! Close / destroy the socket to a hidden service.
     *
     * Also call this for failed connections to clean up
     * resources.
     *
     * After this, the uuid will no longer be recognized by the instance
     */
    void close(const QUuid& uuid);

    ConnectionSocket& getSocket(const QUuid& uuid);
    ConnectionSocket::ptr_t getSocketPtr(const QUuid& uuid);

signals:
    void serviceStarted(const StartServiceResult& ssr);
    void serviceStopped(const StopServiceResult& ssr);
    void connectedToService(const QUuid& uuid);
    void disconnectedFromService(const QUuid& uuid);
    void connectionFailed(const QUuid& uuid,
                          const QAbstractSocket::SocketError& socketError);

private slots:
    void onSocketConnected(const QUuid& uuid);
    void onSocketDisconnected(const QUuid& uuid);
    void onSocketFailed(const QUuid& uuid,
                        const QAbstractSocket::SocketError& socketError);
    void onNewConnection();

private:
    static QNetworkProxy& getTorProxy();

    std::shared_ptr<QTcpServer> server_;
    std::map<QUuid, ConnectionSocket::ptr_t> connections_;
};

}} //namespaces

#endif // TORSERVICESOCKET_H
