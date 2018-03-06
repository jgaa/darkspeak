#include <assert.h>
#include "ds/tormgr.h"

namespace ds {
namespace tor {


TorMgr::TorMgr(const TorConfig& config)
    : config_{config}
{
}

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

void TorMgr::onTorCtlStopped()
{
    emit stopped();
    ctl_.reset();
}

void TorMgr::onTorCtlAutenticated()
{
    emit started();
}

void TorMgr::onTorCtlstateUpdate(TorController::CtlState state)
{
    if (state == TorController::CtlState::CONNECTED) {
        emit online();
    } else if (state == TorController::CtlState::DISCONNECTED) {
        emit offline();
    }
}

void TorMgr::startUseSystemInstance()
{
    assert(!ctl_);
    ctl_ = std::make_unique<TorController>(config_);

    connect(ctl_.get(), &TorController::stopped,
            this, &TorMgr::onTorCtlStopped,
            Qt::QueuedConnection);

    connect(ctl_.get(), &TorController::autenticated,
            this, &TorMgr::onTorCtlAutenticated);

    connect(ctl_.get(), &TorController::stateUpdate,
            this, &TorMgr::onTorCtlstateUpdate);

}

}} // namespaces

