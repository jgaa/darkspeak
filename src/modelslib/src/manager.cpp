#include "ds/manager.h"

#include "logfault/logfault.h"

using namespace std;
using namespace ds::core;
using namespace ds::crypto;

namespace ds {
namespace models {

Manager::Manager()
{
    engine_ = make_unique<DsEngine>();

    bind(engine_.get(), &ds::core::DsEngine::stateChanged,
         [this] (const AppState /*from*/, const AppState to) {
        app_state_ = to;
        emit appStateChanged(to);
    });

    bind(engine_.get(), &ds::core::DsEngine::onlineStateChanged,
         [this] (const OnlineState /*from*/, const OnlineState to) {
        online_state_ = to;
        emit onlineStateChanged(to);
    });
}

Manager::AppState Manager::getAppState() const
{
    return app_state_;
}

Manager::OnlineState Manager::getOnlineState() const
{
    return online_state_;
}

QString Manager::programName() const
{
    return "DarkSpeak";
}

}} // namespaces
