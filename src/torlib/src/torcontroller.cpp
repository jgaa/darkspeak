
#include <QFile>
#include <QFileInfo>
#include <assert.h>

#include "include/ds/torcontroller.h"
#include "ds/torctlsocket.h"
#include "ds/crypto.h"

namespace ds {
namespace tor {

const QByteArray TorController::tor_safe_serverkey_
    = "Tor safe cookie authentication server-to-controller hash";
const QByteArray TorController::tor_safe_clientkey_
    = "Tor safe cookie authentication controller-to-server hash";

TorController::TorController(const TorConfig &config)
    : config_{config}, rnd_eng_{std::random_device()()}
{
    static bool registered = false;
    if (!registered) {
        registered = true;
        qRegisterMetaType<ds::tor::ServiceProperties>("ServiceProperties");
        qRegisterMetaType<ds::tor::ServiceProperties>("::ds::tor::ServiceProperties");
    }
}

void TorController::start()
{
    assert(!ctl_);

    ctl_ = std::make_unique<TorCtlSocket>();

    connect(ctl_.get(), &TorCtlSocket::connected, this, &TorController::startAuth);
    connect(ctl_.get(), &TorCtlSocket::disconnected, this, &TorController::clear);
    connect(ctl_.get(), &TorCtlSocket::torEvent, this, &TorController::torEvent);

    qDebug() << "Connecting to Torctl on "
             << config_.ctl_host
             << " port " << config_.ctl_port;

    setState(CtlState::CONNECTING);

    ctl_->connectToHost(config_.ctl_host, config_.ctl_port);
}

void TorController::stop()
{
    qDebug() << "Closing the connection to the tor server.";
    setState(CtlState::STOPPING);
    ctl_->close();
}

void TorController::createService(const QByteArray &id)
{
    std::uniform_int_distribution<quint16> distr(
                config_.service_from_port,
                config_.service_to_port);

    ServiceProperties sp;
    sp.id = id;
    sp.port = distr(rnd_eng_);

    ctl_->sendCommand(QStringLiteral("ADD_ONION NEW:BEST Port=%1").arg(sp.port).toLocal8Bit(),
                      [this, sp](const TorCtlReply& reply){

        if (reply.status == 250) {
            auto map = reply.parse();

            auto list = map.value("PRIVATEKEY").toByteArray().split(':');
            if (list.size() != 2) {
                throw ParseError("Unexpected format of PRIVATEKEY");
            }

            auto service = sp;
            service.service_id = map.value("SERVICEID").toByteArray();
            service.key_type = list.front();
            service.key = list.back();

            service_map_[service.id] = service.service_id;

            qDebug() << "Created Tor hidden service: " << service.service_id
                     << " with id " << service.id;

            emit serviceCreated(service);
            emit serviceStarted(service.id);
        } else {
            auto msg = std::to_string(reply.status) + ' ' + reply.lines.front();
            emit serviceFailed(sp.id, msg.c_str());
        }
    });
}

void TorController::startService(const ServiceProperties &sp)
{
    auto cmd = QStringLiteral("ADD_ONION %1:%2 Port=%3,%4:%5")
                .arg(QLatin1String{sp.key_type})
                .arg(QLatin1String{sp.key})
                .arg(sp.port)
                .arg(config_.app_host.toString())
                .arg(config_.app_port)
                .toLocal8Bit();

    service_map_[sp.id] = sp.service_id;
    const auto id = sp.id;
    const auto service_id = sp.service_id;

    ctl_->sendCommand(cmd, [this, id, service_id](const TorCtlReply& reply){

        if (reply.status == 250) {

            qDebug() << "Started Tor hidden service: " << service_id
                     << " with id " << id;

            emit serviceStarted(id);
        } else {
            auto msg = std::to_string(reply.status) + ' ' + reply.lines.front();
            emit serviceFailed(id, msg.c_str());
        }
    });
}

void TorController::stopService(const QByteArray &id)
{
    const auto service_id = service_map_.value(id);
    if (service_id.isEmpty()) {
        throw NoSuchServiceError(id.data());
    }

    ctl_->sendCommand(QStringLiteral("DEL_ONION %1").arg(QLatin1String{service_id}).toLocal8Bit(),
                      [this, id, service_id](const TorCtlReply& reply){
        if (reply.status == 250) {

            qDebug() << "Stopped Tor hidden service: " << service_id
                     << " with id " << id;


            emit serviceStopped(id);
        } else {
            auto msg = std::to_string(reply.status) + ' ' + reply.lines.front();
            emit serviceFailed(id, msg.c_str());
        }
    });
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
    setState(CtlState::STOPPED);
    setState(TorState::UNKNOWN);
    emit stopped();
}

void TorController::torEvent(const TorCtlReply &reply)
{
    const auto map = reply.parse();
    qDebug() << "Received tor event: " << reply.lines.front().c_str();
}

void TorController::setState(TorController::CtlState state)
{
    if (ctl_state_ != state) {
        ctl_state_ = state;
        emit stateUpdate(ctl_state_);
    }
}

void TorController::setState(TorController::TorState state,
                             int progress, const QString& summary)
{
    if (tor_state_!= state) {
        tor_state_ = state;
        emit torStateUpdate(tor_state_, progress, summary);
    }
}

void TorController::DoAuthentcate(const TorCtlReply &reply)
{
    // Decide what kind of autentication we will use
    assert(reply.status == 250);
    auto map = reply.parse();
    const auto auth_map = map.value("AUTH").toMap();
    QString methods_str = auth_map.value("METHODS").toString();
    const auto methods = methods_str.split(',');

    if (config_.allowed_auth_methods.contains("HASHEDPASSWORD")
            && methods.contains("HASHEDPASSWORD", Qt::CaseInsensitive)
            && !config_.ctl_passwd.isEmpty()) {
        QByteArray auth_data = '\"' + config_.ctl_passwd.toLocal8Bit().replace("\"", "\"") + '\"';
        qDebug() << "Authenticating torctl with password";
        Authenticate(auth_data);
    } else if (config_.allowed_auth_methods.contains("SAFECOOKIE")
            && methods.contains("SAFECOOKIE", Qt::CaseInsensitive)) {
        const auto path = auth_map.value("COOKIEFILE").toString();
        cookie_ = GetCookie(path);

        // Fill 32 random bytes into client_nounce_.
        client_nonce_.clear();
        std::uniform_int_distribution<unsigned char> distr(0, 255);
        for(auto i = 0; i < 32; ++i) {
            client_nonce_ += static_cast<char>(distr(rnd_eng_));
        }

        assert(client_nonce_.size() == 32);

        qDebug() << "Authenticating torctl with SAFECOOKIE";

        ctl_->sendCommand("AUTHCHALLENGE SAFECOOKIE " + client_nonce_.toHex(), [this](const TorCtlReply& reply){
            if (reply.status == 250) {
                qDebug() << "Torctl AUTHCHALLENGE successful - proceeding with authenticating";
            }

            const auto map = reply.parse();
            auto ac = map.value("AUTHCHALLENGE").toMap();
            QByteArray server_hash = QByteArray::fromHex(ac.value("SERVERHASH").toByteArray());
            QByteArray server_nonce = QByteArray::fromHex(ac.value("SERVERNONCE").toByteArray());

            if (server_hash.size() != 32 || server_nonce.size() != 32) {
                throw SecurityError("Invalid AUTHCHALLENGE data from server.");
            }

            const auto sh = ComputeHmac(tor_safe_serverkey_, server_nonce);
            if (sh != server_hash) {
                throw SecurityError("Incorrect SERVERHASH");
            }

            const auto ch = ComputeHmac(tor_safe_clientkey_, server_nonce);

            Authenticate(ch.toHex());
        });
   } else if (config_.allowed_auth_methods.contains("COOKIE")
            && methods.contains("COOKIE", Qt::CaseInsensitive)) {
        const auto path = auth_map.value("COOKIEFILE").toString();
        QByteArray auth_data = GetCookie(path).toHex();
        qDebug() << "Authenticating torctl with COOKIE";
        Authenticate(auth_data);
    } else {
        throw AuthError("No usable authentication methods");
    }
}

void TorController::Authenticate(const QByteArray &data)
{
    ctl_->sendCommand("AUTHENTICATE " + data,
                      std::bind(&TorController::OnAuthReply, this, std::placeholders::_1));
}

void TorController::OnAuthReply(const TorCtlReply &reply)
{
    qDebug() << QStringLiteral("Torctl AUTHENTICATE command returned status=%1").arg(reply.status);

    if (reply.status == 250) {
        setState(CtlState::CONNECTED);
        emit autenticated();
        ctl_->sendCommand("SETEVENTS STATUS_CLIENT", {});
        ctl_->sendCommand("GETINFO status/bootstrap-phase", [this](const TorCtlReply& reply) {
            if (reply.status == 250) {
                auto map = reply.parse();
                const auto summary = map.value("BOOTSTRAP").toMap().value("SUMMARY").toString();
                const auto progress = map.value("BOOTSTRAP").toMap().value("PROGRESS").toInt();

                setState(progress == 100 ? TorState::READY : TorState::INITIALIZING,
                         progress, summary);
                if (progress == 100) {
                    setState(CtlState::ONLINE);
                    emit ready();
                }
            } else {
                throw TorError("tor command: 'SETEVENTS EXTENDED STATUS_CLIENT failed'");
            }
        });
    } else if (reply.status == 515) {
        emit authFailed(QStringLiteral("Incorrect password"));
        close();
    } else {
        emit authFailed(QStringLiteral("uthentication failed error=%1").arg(reply.status));
        close();
    }
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

QByteArray TorController::ComputeHmac(const QByteArray &key,
                                      const QByteArray &serverNonce)
{
    return ds::crypto::Crypto::getHmacSha256(key, {&cookie_, &client_nonce_, &serverNonce});
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

