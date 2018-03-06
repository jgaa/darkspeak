
#include <assert.h>

#include "include/ds/torprotocolmanager.h"

using namespace std;
using namespace ds::core;
using namespace ds::tor;

namespace ds {
namespace prot {


TorProtocolManager::TorProtocolManager(QSettings &settings)
    : settings_{settings}
{
    tor_ = make_unique<TorMgr>(getConfig());
}

TorProtocolManager::State TorProtocolManager::getState() const
{
    return state_;
}

void TorProtocolManager::setState(ProtocolManager::State state)
{
    const auto old = state_;
    state_ = state;
    if (old != state_) {
        emit stateChanged(old, state_);
    }
}

TorConfig TorProtocolManager::getConfig() const
{
     TorConfig config;

     config.ctl_host = QHostAddress(
                 settings_.value(QStringLiteral("tor.ctl.host"),
                                 config.app_host.toString()).toString());
     config.ctl_port = static_cast<uint16_t>(
                 settings_.value(QStringLiteral("tor.ctl.port"),
                                 config.app_port).toUInt());
     config.ctl_passwd = settings_.value(
                 QStringLiteral("tor.ctl.passwd"),
                 config.app_host.toString()).toString();
     config.ctl_passwd = settings_.value(
                 QStringLiteral("tor.ctl.passwd"),
                 config.app_host.toString()).toString();
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

}} // namespaces




void ds::prot::TorProtocolManager::start()
{
    tor_->updateConfig(getConfig());
    tor_->start();
}

void ds::prot::TorProtocolManager::stop()
{
    tor_->stop();
}

void ds::prot::TorProtocolManager::torMgrStarted()
{

}

void ds::prot::TorProtocolManager::torMgrOnline()
{
    setState(State::ONLINE);
    emit online();
}

void ds::prot::TorProtocolManager::torMgrOffline()
{
    setState(State::ONLINE);
    emit offline();
}

void ds::prot::TorProtocolManager::torMgrStopped()
{

}


void ds::prot::TorProtocolManager::createTransportHandle(const TransportHandleReq &)
{
    // TODO: Implement
    assert(false);
}
