
#include <QNetworkProxy>

#include "ds/torserviceinterface.h"
#include "ds/dsserver.h"
#include "ds/dsclient.h"
#include "logfault/logfault.h"

namespace ds {
namespace prot {

using namespace std;

TorServiceInterface::TorServiceInterface(crypto::DsCert::ptr_t cert,
                                         const QByteArray& address,
                                         const QUuid& identityId)
    : cert_{move(cert)}, address_{address}, identityId_{identityId}
{
}


StartServiceResult TorServiceInterface::startService()
{
    StartServiceResult r;

    r.stopped = stopService();

    server_ = make_shared<TorSocketListener>(
                [this](const ConnectionSocket::ptr_t& connection) {
        onNewIncomingConnection(connection);
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

core::PeerConnection::ptr_t
TorServiceInterface::connectToService(const QByteArray &host, const uint16_t port,
                                      core::ConnectData cd)
{
    auto connection = make_shared<ConnectionSocket>(host, port);

    LFLOG_DEBUG << "Connecting to "
                << host << ":" << port
                << " with connection-id " << connection->getUuid().toString();

    auto client = make_shared<DsClient>(connection, move(cd));

    connect(client.get(), &core::PeerConnection::disconnectedFromPeer,
            this, [this](const std::shared_ptr<core::PeerConnection>& peer) {
        peers_.erase(peer->getConnectionId());
    }, Qt::QueuedConnection);

    connection->setProxy(getTorProxy());
    connection->connectToDefaultHost();

    peers_[connection->getUuid()] = client;

    return move(client);
}

void TorServiceInterface::close(const QUuid &uuid)
{
    if (auto peer = getPeer(uuid)) {
        peer->close();
    }
}

ConnectionSocket &TorServiceInterface::getSocket(const QUuid &uuid)
{
    if (auto peer = getPeer(uuid)) {
        return peer->getConnection();
    }

    throw runtime_error("No such connection: "s + uuid.toString().toStdString());
}

void TorServiceInterface::onNewIncomingConnection(
        const ConnectionSocket::ptr_t &connection)
{
    LFLOG_DEBUG << "Plugging in a new incoming connection: "
                << connection->getUuid().toString();

    core::ConnectData cd;
    cd.identitysCert = cert_;
    cd.service = identityId_;
    auto server = make_shared<DsServer>(connection, move(cd));

    connect(server.get(), &Peer::incomingPeer,
            this, [this](const std::shared_ptr<core::PeerConnection>& peer) {
        emit incomingPeer(peer);
    });

    peers_[connection->getUuid()] = server;
}

void TorServiceInterface::autorizeConnection(const QUuid &connection, const bool allow)
{
    if (auto peer = getPeer(connection)) {
        peer->authorize(allow);
    } else {
        LFLOG_WARN << "No peer with id " << connection.toString();
    }
}


QNetworkProxy &TorServiceInterface::getTorProxy()
{
    // TODO: Get the actual host/port used by our tor server,
    //       preferably from the control connection.
    static QNetworkProxy proxy{QNetworkProxy::Socks5Proxy,
                "127.0.0.1", 9050};

    return proxy;
}

Peer::ptr_t TorServiceInterface::getPeer(const QUuid &uuid) const
{
    auto it = peers_.find(uuid);
    if (it != peers_.end()) {
        return it->second;
    }

    return {};
}

ConnectionSocket::ptr_t TorServiceInterface::getSocketPtr(const QUuid &uuid)
{
    if (auto peer = getPeer(uuid)) {
        return peer->getConnectionPtr();
    }

    return {};
}


}} // namespaces
