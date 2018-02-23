
#include <QFile>
#include <QFileInfo>
#include <QRandomGenerator>
#include <assert.h>

#include "include/ds/torcontroller.h"
#include "ds/torctlsocket.h"

namespace ds {
namespace tor {

TorController::TorController(const TorConfig &config)
    : config_{config}
{
}

void TorController::start()
{
    assert(!ctl_);

    ctl_ = std::make_unique<TorCtlSocket>();

    connect(ctl_.get(), SIGNAL(connected()), this, SLOT(startAuth()));
    connect(ctl_.get(), SIGNAL(disconnected()), this, SLOT(clear()));

    qDebug() << "Connecting to Torctl on "
             << config_.ctl_host
             << " port " << config_.ctl_port;

    setState(CtlState::CONNECTING);

    ctl_->connectToHost(config_.ctl_host, config_.ctl_port);
}

void TorController::stop()
{

}

void TorController::startAuth()
{
    qDebug() << "Connected to Torctl. Initiating Authentication Procedure.";

    setState(CtlState::AUTHENTICATING);

    ctl_->sendCommand("PROTOCOLINFO 1", [&](const TorCtlReply& reply) {
        if (reply.status == 250) {
            try {
                DoAuthentcate(reply);
            } catch (const std::runtime_error& ex) {
                qWarning() << "Authentication failed with exception: " << ex.what();
                ctl_->abort();
            }
        } else {
            qWarning() << "Unexpected status from Tor. Resetting.";
            ctl_->abort();
        }
    });
}

void TorController::close()
{
    if (ctl_ && ctl_->isOpen()) {
        qDebug() << "Torctl Closing connection";
        ctl_->abort();
    }
}

void TorController::clear()
{
    qDebug() << "Torctl connection was closed";
    setState(CtlState::DISCONNECTED);
    setState(TorState::UNKNOWN);
}

void TorController::setState(TorController::CtlState state)
{
    ctl_state_ = state;
    emit stateUpdate(ctl_state_);
}

void TorController::setState(TorController::TorState state)
{
    tor_state_ = state;
    emit torStateUpdate(tor_state_);
}

void TorController::DoAuthentcate(const TorCtlReply &reply)
{
    // Decide what kind of autentication we will use
    assert(reply.status == 250);
    auto map = reply.parse();
    const auto auth_map = map.at("AUTH").toMap();
    QString methods_str = auth_map.value("METHODS").toString();
    const auto methods = methods_str.split(',');

    QByteArray auth_data;

    if (config_.allowed_auth_methods.contains("SAFECOOKIE")
            && methods.contains("SAFECOOKIE", Qt::CaseInsensitive)) {
        const auto path = auth_map.value("COOKIEFILE").toString();
        cookie_ = GetCookie(path);

        // Fill 32 random bytes into client_nounce_.
        client_nounce_.clear();
        for(auto i = 0; i < 8; ++i) {
            union {
                quint32 uint;
                std::array<char, 4> bytes;
            } u;

            u.uint = QRandomGenerator::global()->generate();
            for(const auto ch : u.bytes) {
                client_nounce_ += ch;
            }
        }

        assert(client_nounce_.size() == 32);

        ctl_->sendCommand("AUTHCHALLENGE SAFECOOKIE " + client_nounce_.toHex(), [this](const TorCtlReply& reply){
            if (reply.status == 250) {
                qDebug() << "Torctl AUTHCHALLENGE successful - proceeding with authenticating";
            }

            const auto map = reply.parse();

            qDebug() << "Gakk";
        });

        return;
    }

    if (config_.allowed_auth_methods.contains("COOKIE")
            && methods.contains("COOKIE", Qt::CaseInsensitive)) {
        const auto path = auth_map.value("COOKIEFILE").toString();
        auth_data = GetCookie(path);
    } else {
        throw AuthError("No usable authentication methods");
    }

    // TODO: Add PASSWORD

    ctl_->sendCommand("AUTHENTICATE " + auth_data.toHex(), [this](const TorCtlReply& reply){

        qDebug() << QStringLiteral("Torctl AUTHENTICATE command returned status=%1").arg(reply.status);

        if (reply.status == 250) {
            setState(CtlState::CONNECTED);
            emit autenticated();
        } else if (reply.status == 515) {
            emit authFailed(QStringLiteral("Incorrect password"));
            close();
        } else {
            emit authFailed(QStringLiteral("uthentication failed error=%1").arg(reply.status));
            close();
        }
    });
}

QByteArray TorController::GetCookie(const QString &path)
{
    const auto size = QFileInfo(path).size();
    if (size != 32) {
        qWarning() << "Cookile file " << path
                   << " should be 32 bytes. It is "
                   << size << " bytes.";
        throw SecurityError("Suspicious cookie-file.");
    }

    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        qWarning() << "Failed to open cookie-file: " << path;
        throw IoError("Failed to open cookie-file for read");
    }

    QByteArray cookie = file.readAll();
    file.close();
    return cookie;
}

TorController::CtlState TorController::getCtlState() const
{
    return ctl_state_;
}

TorController::TorState TorController::getTorState() const
{
    return tor_state_;
}

}}

