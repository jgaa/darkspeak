#include <cassert>
#include <ds/torcontroller.h>
#include "ds/tormgr.h"
#include "logfault/logfault.h"

namespace ds {
namespace tor {


TorMgr::TorMgr(const TorConfig& config)
    : config_{config}
{
}

TorController *TorMgr::getController() { return ctl_.get(); }

/* To start using a tor service, we need to connect over TCP to
 * the contol channel of a running server.
 * Then we need to get the authentication methods from the
 * PROTOCOLINFO command, and based on that,
 * perform one of the following authentication operations
 * (listed in the preffered order):
 *  - PASSWORD
 *  - SAFECOOKIE
 *  - COOKIE (depricated)
 *
 * We don't support NULL. Too suspicious!
 */

void TorMgr::start()
{
    if (config_.mode == TorConfig::Mode::SYSTEM) {
        startUseSystemInstance();
    } else {
        assert(false); // Not implemented
    }
}

void TorMgr::stop()
{
    if (ctl_) {
        ctl_->stop();
    }
}

void TorMgr::updateConfig(const TorConfig &config)
{
    config_ = config;
}

void TorMgr::createService(const QUuid& service)
{
    if (!ctl_ || !ctl_->isConnected()) {
        throw OfflineError("Tor is offline");
    }

    ctl_->createService(service);
}

void TorMgr::startService(const ServiceProperties &service)
{
    if (!ctl_ || !ctl_->isConnected()) {
        throw OfflineError("Tor is offline");
    }

    ctl_->startService(service);
}

void TorMgr::stopService(const QUuid& service)
{
    if (!ctl_ || !ctl_->isConnected()) {
        throw OfflineError("Tor is offline");
    }

    try {
        ctl_->stopService(service);
    } catch(const TorController::NoSuchServiceError&) {
        LFLOG_WARN << "Cannot stop non-existing service with id " << service.toString();
        emit serviceStopped(service);
    }


}

void TorMgr::onTorStateUpdate(TorController::TorState state, int progress, const QString &summary)
{
    emit torStateUpdate(state, progress, summary);
}

void TorMgr::onStateUpdate(TorController::CtlState state)
{
    emit stateUpdate(state);
    switch(state) {
    case TorController::CtlState::DISCONNECTED:
        emit offline();
        break;
    case TorController::CtlState::CONNECTING:
        break;
    case TorController::CtlState::AUTHENTICATING:
        break;
    case TorController::CtlState::CONNECTED:
        emit started();
        break;
    case TorController::CtlState::ONLINE:
        emit online();
        break;
    case TorController::CtlState::STOPPING:
        break;
    case TorController::CtlState::STOPPED:
        emit stopped();
        break;
    }
}

void TorMgr::onServiceCreated(const ServiceProperties &service)
{
    emit serviceCreated(service);
}

void TorMgr::onServiceFailed(const QUuid& service, const QByteArray &reason)
{
    emit serviceFailed(service, reason);
}

void TorMgr::onServiceStarted(const QUuid& service, const bool newService)
{
    emit serviceStarted(service, newService);
}

void TorMgr::onServiceStopped(const QUuid& service)
{
    emit serviceStopped(service);
}

void TorMgr::startUseSystemInstance()
{
    assert(!ctl_);
    ctl_ = std::make_unique<TorController>(config_);

    connect(ctl_.get(), &TorController::stateUpdate,
            this, &TorMgr::onStateUpdate);

    connect(ctl_.get(), &TorController::torStateUpdate,
            this, &TorMgr::onTorStateUpdate);

    connect(ctl_.get(), &TorController::serviceCreated,
            this, &TorMgr::onServiceCreated);

    connect(ctl_.get(), &TorController::serviceFailed,
            this, &TorMgr::onServiceFailed);

    connect(ctl_.get(), &TorController::serviceStarted,
            this, &TorMgr::onServiceStarted);

    connect(ctl_.get(), &TorController::serviceStopped,
            this, &TorMgr::onServiceStopped);

    ctl_->start();

}

}} // namespaces

