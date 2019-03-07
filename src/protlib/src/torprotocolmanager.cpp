
#include <assert.h>
#include <array>

#include <QJsonObject>
#include <QJsonDocument>

#include "ds/torprotocolmanager.h"
#include "ds/errors.h"
#include "logfault/logfault.h"

using namespace std;
using namespace ds::core;
using namespace ds::tor;

namespace ds {
namespace prot {


TorProtocolManager::TorProtocolManager(QSettings &settings)
    : settings_{settings}
{
    tor_ = make_unique<TorMgr>(getConfig());

    connect(tor_.get(), &TorMgr::serviceCreated, this, &TorProtocolManager::onServiceCreated);

    connect(tor_.get(), &TorMgr::serviceStarted, this, [this](const QUuid& service, const bool newService) {
        emit serviceStarted(service, newService);
    });

    connect(tor_.get(), &TorMgr::serviceStopped, this, [this](const QUuid& service) {
        emit serviceStopped(service);
    });

    connect(tor_.get(), &TorMgr::serviceFailed, this, [this](const QUuid& service,
            const QByteArray &reason) {
        emit serviceFailed(service, reason);
        emit transportHandleError({"", service, reason});
    });

    connect(tor_.get(), &TorMgr::started, this, [this](){
        setState(State::CONNECTED);
    });

    connect(tor_.get(), &TorMgr::online, this, [this](){
        setState(State::ONLINE);
    });

    connect(tor_.get(), &TorMgr::offline, this, [this](){
        setState(State::OFFLINE);
    });

    connect(tor_.get(), &TorMgr::stopped, this, [this](){
        setState(State::OFFLINE);
    });
}

TorProtocolManager::State TorProtocolManager::getState() const
{
    return state_;
}

const QByteArray& TorProtocolManager::getName(const ProtocolManager::State state)
{
    static const std::array<QByteArray, 5> names = {{
       "OFFLINE",
       "CONNECTING",
       "CONNECTED",
       "ONLINE",
       "SHUTTINGDOWN"
   }};

   return names.at(static_cast<size_t>(state));
}

void TorProtocolManager::setState(ProtocolManager::State state)
{
    const auto old = state_;
    state_ = state;
    if (old != state_) {

        LFLOG_DEBUG << "TorProtocolManager changing state from " << getName(old)
                 << " to " << getName(state);

        emit stateChanged(old, state_);

        switch(state) {
        case ProtocolManager::State::OFFLINE:
            emit offline();
            break;
        case ProtocolManager::State::CONNECTING:
            emit connecting();
            break;
        case ProtocolManager::State::CONNECTED:
            emit connected();
            break;
        case ProtocolManager::State::ONLINE:
            emit online();
            break;
        case ProtocolManager::State::SHUTTINGDOWN:
            emit shutdown();
            break;
        }
    }
}

TorConfig TorProtocolManager::getConfig() const
{
     TorConfig config;

     config.ctl_host = QHostAddress(
                 settings_.value(QStringLiteral("tor.ctl.host"),
                                 config.ctl_host.toString()).toString());
     config.ctl_port = static_cast<uint16_t>(
                 settings_.value(QStringLiteral("tor.ctl.port"),
                                 config.ctl_port).toUInt());
     config.ctl_passwd = settings_.value(
                 QStringLiteral("tor.ctl.passwd"),
                 config.ctl_passwd).toString();
     config.service_from_port = static_cast<uint16_t>(
                 settings_.value(QStringLiteral("tor.service.from-port"),
                                 config.service_from_port).toUInt());
     config.service_to_port = static_cast<uint16_t>(
                 settings_.value(QStringLiteral("tor.service.to-port"),
                                 config.service_to_port).toUInt());
     config.app_host = QHostAddress(
                 settings_.value(QStringLiteral("tor.app.host"),
                                 config.app_host.toString()).toString());
     config.app_port = static_cast<uint16_t>(
                 settings_.value(QStringLiteral("tor.app.port"),
                                 config.app_port).toUInt());
     return config;
}

TorServiceInterface& TorProtocolManager::getService(const QUuid& service)
{
    auto it = services_.find(service);
    if (it == services_.end()) {
        const auto name = service.toByteArray().toStdString();
        throw runtime_error("No such service"s + name);
    }

    return *it->second;
}

uint64_t TorProtocolManager::sendAddme(const AddmeReq& req)
{
    QString addr;
    auto json = QJsonDocument{
        QJsonObject{
            {"type", "AddMe"},
            {"nick", req.nickName},
            {"address", getService(req.service).getAddress()},
            {"message", req.message}
        }
    };

    if (auto peer = getService(req.service).getPeer(req.connection)) {
        return peer->send(json);
    }

    throw runtime_error("Failed to access peer while sending addme");
}

uint64_t TorProtocolManager::sendAck(const AckMsg &ack)
{

    if (auto peer = getService(ack.service).getPeer(ack.connection)) {
        return peer->sendAck(ack.what, ack.status, ack.data);
    }

    throw runtime_error("Failed to access peer while sending addme");
}

QByteArray TorProtocolManager::getPeerHandle(const QUuid &service,
                                             const QUuid &connectionId)
{
    const auto& cd = getService(service).getPeer(connectionId)->getConnectData();
    return cd.contactsCert->getB58PubKey();
}

void TorProtocolManager::start()
{
    tor_->updateConfig(getConfig());
    tor_->start();
    setState(State::CONNECTING);
}

void TorProtocolManager::stop()
{
    tor_->stop();
    setState(State::SHUTTINGDOWN);
}

void TorProtocolManager::createTransportHandle(const TransportHandleReq &req)
{
    tor_->createService(req.uuid);
}

void TorProtocolManager::startService(const QUuid& serviceId,
                                      const crypto::DsCert::ptr_t& cert,
                                      const QVariantMap& data)
{
    ServiceProperties sp;
    sp.uuid = serviceId;
    sp.key = data["key"].toByteArray();
    sp.service_port = static_cast<decltype(sp.service_port)>(data["port"].toInt());
    sp.key_type = data["key_type"].toByteArray();
    sp.service_id = data["service_id"].toByteArray();

    auto service = make_shared<TorServiceInterface>(cert, data["address"].toByteArray(), serviceId);

    // Add listening port
    auto properties = service->startService();
    sp.app_port = properties.port;
    assert(properties.port);

    auto it = services_.find(serviceId);
    if (it != services_.end()) {
        it->second->stopService();
    }

    services_[serviceId] = service;

//    connect(service.get(), &TorServiceInterface::connectedToService,
//            this, [this, serviceId](const QUuid& uuid) {
//        emit connectedTo(serviceId, uuid, core::ProtocolManager::Direction::OUTBOUND);
//    });

//    connect(service.get(), &TorServiceInterface::disconnectedFromService,
//            this, [this, serviceId](const QUuid& uuid) {
//        emit disconnectedFrom(serviceId, uuid);
//    });

//    connect(service.get(), &TorServiceInterface::connectionFailed,
//            this, [this](const QUuid& uuid,
//            const QAbstractSocket::SocketError& socketError) {
//        emit connectionFailed(uuid, socketError);
//    });

    connect(service.get(), &TorServiceInterface::incomingPeer,
            this, [this] (const std::shared_ptr<PeerConnection>& peer) {
        emit incomingPeer(peer);
    });

//    connect(service.get(), &TorServiceInterface::receivedData,
//            this, [this, serviceId](const QUuid& connectionId, const quint32 channel,
//            const quint64 id, const QByteArray& data) {
//        emit receivedData(serviceId, connectionId, channel, id, data);
//    });

    tor_->startService(sp);
}

void TorProtocolManager::stopService(const QUuid& uuid)
{
    tor_->stopService(uuid);
}

core::PeerConnection::ptr_t TorProtocolManager::connectTo(core::ConnectData cd)
{
    // address may be "onion:hostname:port" or "hostname:port"
    auto parts = cd.address.split(':');
    if (parts.size() < 2 || parts.size() > 3) {
        throw runtime_error("Expected: [onion:]address:port");
    }

    const auto port = static_cast<uint16_t>(parts.at(parts.size() -1).toUInt());
    const auto host = parts.at(parts.size() - 2) + ".onion";
    return getService(cd.service).connectToService(host, port, move(cd));
}

//void TorProtocolManager::disconnectFrom(const QUuid &service, const QUuid &connection)
//{
//    return getService(service).close(connection);
//}

//void TorProtocolManager::autorizeConnection(const QUuid &service,
//                                            const QUuid &connection,
//                                            const bool allow)
//{
//    getService(service).autorizeConnection(connection, allow);
//}


void TorProtocolManager::onServiceCreated(const ServiceProperties &service)
{
    // Convert he service data to a transport-handle
    TransportHandle th;

    th.identityName = service.name;
    th.uuid = service.uuid;
    th.handle = service.service_id + ':' + QByteArray::number(service.service_port);

    th.data["type"] = QByteArray("Tor hidden service");
    th.data["key"] = service.key;
    th.data["service_id"] = service.service_id;
    th.data["key_type"] = service.key_type;
    th.data["port"] = service.service_port;
    th.data["address"] = QString("onion:") + th.handle;

    // Stop the service. We require explicit start request to make it available.
    try {
        tor_->stopService(service.uuid);
    } catch(const std::exception& ex) {
        LFLOG_WARN << "Failed to request hidden service " << th.handle
                   << " to stop: " << ex.what();
    }

    emit transportHandleReady(th);
}

}} // namespaces


