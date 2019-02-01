
#include <QNetworkProxy>

#include "ds/torserviceinterface.h"
#include "ds/dsserver.h"
#include "ds/dsclient.h"
#include "logfault/logfault.h"

namespace ds {
namespace prot {

using namespace std;

TorServiceInterface::TorServiceInterface(const crypto::DsCert::ptr_t& cert)
    : cert_{cert}
{
}

TorServiceInterface::~TorServiceInterface()
{

}

StartServiceResult TorServiceInterface::startService()
{
    StartServiceResult r;

    r.stopped = stopService();

    server_ = make_shared<TorSocketListener>(
                [this](ConnectionSocket::ptr_t connection) {
        onNewIncomingConnection(move(connection));
    });

    if (!server_->listen(QHostAddress::LocalHost)) {
        LFLOG_ERROR << "Failed to start listener: "
                    << server_->errorString();
        throw runtime_error("Failed to start listener");
    }
    r.port = server_->serverPort();

    LFLOG_NOTICE << "Started listening to " << server_->serverAddress()
                 << ":" << server_->serverPort();

    emit serviceStarted(r);

    return r;
}

StopServiceResult TorServiceInterface::stopService()
{
    StopServiceResult r;
    if (server_) {
        if (server_->isListening()) {

            LFLOG_NOTICE << "Will stop listening to " << server_->serverAddress()
                         << ":" << server_->serverPort();

            r.port = server_->serverPort();
            r.wasStopped = true;
            server_->close();
        }
        server_.reset();
    }

    return r;
}

QUuid TorServiceInterface::connectToService(const QByteArray &host, const uint16_t port,
                                            core::ConnectData cd)
{
    auto connection = make_shared<ConnectionSocket>();

    LFLOG_DEBUG << "Connecting to "
                << host << ":" << port
                << " with connection-id " << connection->getUuid().toString();

    connect(connection.get(), &ConnectionSocket::connectedToHost,
            this, &TorServiceInterface::onSocketConnected);
    connect(connection.get(), &ConnectionSocket::disconnectedFromHost,
            this, &TorServiceInterface::onSocketDisconnected);
    connect(connection.get(), &ConnectionSocket::socketFailed,
            this, &TorServiceInterface::onSocketFailed);

    auto client = make_shared<DsClient>(connection, move(cd));

    connection->setProxy(getTorProxy());
    connection->connectToHost(host, port);

    peers_[connection->getUuid()] = client;

    return connection->getUuid();
}

void TorServiceInterface::close(const QUuid &uuid)
{
    auto it = peers_.find(uuid);
    if (it == peers_.end()) {
        return;
    }

    LFLOG_DEBUG << "Closing connection for " << uuid.toString();
    if (it->second->getConnection().isOpen()) {
        it->second->getConnection().close();
    }
    peers_.erase(it);
}

ConnectionSocket &TorServiceInterface::getSocket(const QUuid &uuid)
{
    auto it = peers_.find(uuid);
    if (it == peers_.end()) {
        throw runtime_error("No such connection: "s + uuid.toString().toStdString());
    }

    return it->second->getConnection();
}

void TorServiceInterface::onSocketConnected(const QUuid &uuid)
{
    LFLOG_DEBUG << "Connected with id " << uuid.toString();
    emit connectedToService(uuid);
}

void TorServiceInterface::onSocketDisconnected(const QUuid &uuid)
{
    LFLOG_DEBUG << "Disconnected with id " << uuid.toString();
    emit disconnectedFromService(uuid);
}

void TorServiceInterface::onSocketFailed(const QUuid &uuid,
                                         const QAbstractSocket::SocketError &socketError)
{
    LFLOG_DEBUG << "Connection with id " << uuid.toString()
                << " failed with error: "
                << socketError;

    emit connectionFailed(uuid, socketError);
}

void TorServiceInterface::onNewIncomingConnection(
        const ConnectionSocket::ptr_t &connection)
{
    LFLOG_DEBUG << "Plugging in a new incoming connection: "
                << connection->getUuid().toString();

    connect(connection.get(), &ConnectionSocket::connectedToHost,
            this, &TorServiceInterface::onSocketConnected);
    connect(connection.get(), &ConnectionSocket::disconnectedFromHost,
            this, &TorServiceInterface::onSocketDisconnected);
    connect(connection.get(), &ConnectionSocket::socketFailed,
            this, &TorServiceInterface::onSocketFailed);

    core::ConnectData cd;
    cd.identitysCert = cert_;
    auto server = make_shared<DsServer>(connection, move(cd));

    peers_[connection->getUuid()] = server;
}

QNetworkProxy &TorServiceInterface::getTorProxy()
{
    // TODO: Get the actual host/port used by our tor server,
    //       preferably from the control connection.
    static QNetworkProxy proxy{QNetworkProxy::Socks5Proxy,
                "127.0.0.1", 9050};

    return proxy;
}

ConnectionSocket::ptr_t TorServiceInterface::getSocketPtr(const QUuid &uuid)
{
    auto it = peers_.find(uuid);
    if (it == peers_.end()) {
        return {};
    }

    return it->second->getConnectionPtr();
}


}} // namespaces
