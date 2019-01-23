
#include <assert.h>
#include <array>

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
    connect(tor_.get(), &TorMgr::serviceStarted, this, &TorProtocolManager::onServiceStarted);
    connect(tor_.get(), &TorMgr::serviceStopped, this, &TorProtocolManager::onServiceStopped);
    connect(tor_.get(), &TorMgr::serviceFailed, this, &TorProtocolManager::onServiceFailed);

    connect(tor_.get(), &TorMgr::started, this, &TorProtocolManager::torMgrStarted);
    connect(tor_.get(), &TorMgr::online, this, &TorProtocolManager::torMgrOnline);
    connect(tor_.get(), &TorMgr::offline, this, &TorProtocolManager::torMgrOffline);
    connect(tor_.get(), &TorMgr::stopped, this, &TorProtocolManager::torMgrStopped);
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

void TorProtocolManager::sendMessage(const core::Message &)
{
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

void TorProtocolManager::torMgrStarted()
{
    setState(State::CONNECTED);
}

void TorProtocolManager::torMgrOnline()
{
    setState(State::ONLINE);
}

void TorProtocolManager::torMgrOffline()
{
    setState(State::ONLINE);
}

void TorProtocolManager::torMgrStopped()
{
    setState(State::OFFLINE);
}

void TorProtocolManager::createTransportHandle(const TransportHandleReq &req)
{
    tor_->createService(req.identityName.toLocal8Bit());
}

void TorProtocolManager::startService(const QByteArray &id, const QVariantMap& data)
{
    ServiceProperties sp;
    sp.id = id;
    sp.key = data["key"].toByteArray();
    sp.port = static_cast<decltype(sp.port)>(data["port"].toInt());
    sp.key_type = data["key_type"].toByteArray();
    sp.service_id = data["service_id"].toByteArray();

    tor_->startService(sp);
}

void TorProtocolManager::stopService(const QByteArray &id)
{
    tor_->stopService(id);
}

void TorProtocolManager::onServiceCreated(const ServiceProperties &service)
{
    // Convert he service data to a transport-handle
    TransportHandle th;

    th.identityName = service.id;
    th.handle = service.service_id + ':' + QByteArray::number(service.port);

    th.data["type"] = QByteArray("Tor hidden service");
    th.data["key"] = service.key;
    th.data["service_id"] = service.service_id;
    th.data["key_type"] = service.key_type;
    th.data["port"] = service.port;

    // Stop the service. We require explicit start request to make it available.
    try {
        tor_->stopService(service.id);
    } catch(const std::exception& ex) {
        qWarning() << "Failed to request hidden service " << service.id
                   << " to stop: " << ex.what();
    }

    emit transportHandleReady(th);
}

void TorProtocolManager::onServiceFailed(const QByteArray &id, const QByteArray &reason)
{
    emit serviceFailed(id, reason);
    emit transportHandleError({id, reason});
}

void TorProtocolManager::onServiceStarted(const QByteArray &id)
{
    emit serviceStarted(id);
}

void TorProtocolManager::onServiceStopped(const QByteArray &id)
{
    emit serviceStopped(id);
}

}} // namespaces


