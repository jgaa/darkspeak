
#include <regex>

#include <QClipboard>
#include <QGuiApplication>
#include <QtEndian>

#include "ds/manager.h"
#include "ds/base58.h"
#include "ds/base32.h"

#include "logfault/logfault.h"

using namespace std;
using namespace ds::core;
using namespace ds::crypto;

namespace ds {
namespace models {

Manager *Manager::instance_;

LogModel *Manager::logModel()
{
    return log_.get();
}

IdentitiesModel *Manager::identitiesModel()
{
    return identities_.get();
}

ContactsModel *Manager::contactsModel()
{
    return contacts_.get();
}

NotificationsModel *Manager::notificationsModel()
{
    return notifications_.get();
}

void Manager::textToClipboard(QString text)
{
    auto cb = QGuiApplication::clipboard();
    cb->setText(text);
}

QVariantMap Manager::getIdenityFromClipboard() const
{
    // name : base58 for version, pubkey and legacy tor service and port
    static const regex legacy_onion_pattern(R"(^(.+)\:([123456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz]{69})$)");
    std::smatch match;

    QVariantMap values;

    const auto text =  QGuiApplication::clipboard()->text().toStdString();
    if (!text.empty() && regex_match(text, match, legacy_onion_pattern)) {
        if (match.size() == 3) {
            const auto nick = match[1].str();
            const auto blob = match[2].str();
            values["nickName"] = nick.c_str();

            // See IdentitiesModel::getIdentityAsBase58 for data-format

            // 16 bytes onion address
            auto const assumed_len = 1 + 32 + 10 + 2;
            auto data = b58tobin_check<QByteArray>(blob, assumed_len,  {11, 176});
            assert(data.size() == assumed_len);
            if (data.at(0) != 1) { // Version
                LFLOG_NOTICE << "Unsupported version of identity payload: v="
                             << static_cast<int>(data.at(0));
                return {};
            }

            const auto pubkey = data.mid(1, 32);
            const auto host = data.mid(33, 10);
            const auto port = data.mid(43,2);

            const auto address = onion16encode(host);

            union {
                char bytes[2];
                uint16_t port;
            } port_u;

            port_u.bytes[0] = port.at(0);
            port_u.bytes[1] = port.at(1);

            values["address"] = QString("onion:") + address
                    + ":" + QString::number(qFromBigEndian(port_u.port));

            values["handle"] = b58check_enc<QByteArray>(pubkey, {249, 50});
        }
    }

    return values;
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
    contacts_ = make_unique<ContactsModel>(engine_->settings());
    notifications_ = make_unique<NotificationsModel>(engine_->settings());

    instance_ = this;

    connect(identities_.get(), &IdentitiesModel::addmeRequest,
            notifications_.get(), &NotificationsModel::addNotification);
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
