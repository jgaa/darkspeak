
#include <QClipboard>
#include <QGuiApplication>

#include "ds/manager.h"

#include "logfault/logfault.h"

using namespace std;
using namespace ds::core;
using namespace ds::crypto;

namespace ds {
namespace models {

LogModel *Manager::logModel()
{
    return log_.get();
}

IdentitiesModel *Manager::identitiesModel()
{
    return identities_.get();
}

void Manager::textToClipboard(QString text)
{
    auto cb = QGuiApplication::clipboard();
    cb->setText(text);
}

Manager::Manager()
{
    engine_ = make_unique<DsEngine>();

    connect(engine_.get(), &ds::core::DsEngine::stateChanged,
            [this] (const AppState /*from*/, const AppState to) {
        app_state_ = to;
        emit appStateChanged(to);
    });

    connect(engine_.get(), &ds::core::DsEngine::onlineStateChanged,
            [this] (const ds::core::ProtocolManager::State /*from*/,
            const ds::core::ProtocolManager::State to) {

        if (to != online_state_) {
            const auto was_online = isOnline();
            online_state_ = to;
            LFLOG_DEBUG << "Manager: Online state changed to " << online_state_;
            emit onlineStateChanged(to);
            emit onlineStatusIconChanged();
            if (was_online != isOnline()) {
                emit onlineChanged();
            }
        }
    });

    log_ = make_unique<LogModel>(engine_->settings());
    identities_ = make_unique<IdentitiesModel>(engine_->settings());
}

Manager::AppState Manager::getAppState() const
{
    return app_state_;
}

Manager::OnlineState Manager::getOnlineState() const
{
    return online_state_;
}

bool Manager::isOnline() const
{
    return online_state_ == ProtocolManager::State::ONLINE;
}

QString Manager::getProgramName() const
{
    return "DarkSpeak";
}

QString Manager::getProgramNameAndVersion() const
{
    return getProgramName() + " " + PROGRAM_VERSION;
}

int Manager::getCurrentPage()
{
    return page_;
}

void Manager::setCurrentPage(int page)
{
    if (page != page_) {
        LFLOG_DEBUG << "Changing to page " << page;
        page_ = page;
        emit currentPageChanged(page);
    }
}

QUrl Manager::getOnlineStatusIcon() const
{
    static const std::array<QUrl, 5> icons = {
       QUrl("qrc:/images/network_offline.svg"),
       QUrl("qrc:/images/network_connecting.svg"),
       QUrl("qrc:/network_connected.svg"),
       QUrl("qrc:/images/network_online.svg"),
       //QUrl("qrc:/images/network_disconnect.svg")
       QUrl("qrc:/images/network_offline.svg"),
   };

    return icons.at(online_state_);
}

void Manager::goOnline()
{
    engine_->start();
}

void Manager::goOffline()
{
    engine_->close();
}

}} // namespaces
