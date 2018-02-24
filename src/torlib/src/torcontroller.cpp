
#include <QFile>
#include <QFileInfo>
#include <QRandomGenerator>
#include <assert.h>

#include <openssl/evp.h>
#include <openssl/hmac.h>

#include "include/ds/torcontroller.h"
#include "ds/torctlsocket.h"

namespace ds {
namespace tor {

const QByteArray TorController::tor_safe_serverkey_
    = "Tor safe cookie authentication server-to-controller hash";
const QByteArray TorController::tor_safe_clientkey_
    = "Tor safe cookie authentication controller-to-server hash";

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
    qDebug() << "Closing the connection to the tor server.";
    setState(CtlState::STOPPING);
    ctl_->close();
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

    if (config_.allowed_auth_methods.contains("HASHEDPASSWORD")
            && methods.contains("HASHEDPASSWORD", Qt::CaseInsensitive)
            && !config_.ctl_passwd.isEmpty()) {
        QByteArray auth_data = '\"' + config_.ctl_passwd.toLocal8Bit().replace("\"", "\"") + '\"';
        Authenticate(auth_data);
    } else if (config_.allowed_auth_methods.contains("SAFECOOKIE")
            && methods.contains("SAFECOOKIE", Qt::CaseInsensitive)) {
        const auto path = auth_map.value("COOKIEFILE").toString();
        cookie_ = GetCookie(path);

        // Fill 32 random bytes into client_nounce_.
        client_nonce_.clear();
        for(auto i = 0; i < 8; ++i) {
            union {
                quint32 uint;
                std::array<char, 4> bytes;
            } u;

            u.uint = QRandomGenerator::global()->generate();
            for(const auto ch : u.bytes) {
                client_nonce_ += ch;
            }
        }

        assert(client_nonce_.size() == 32);

        ctl_->sendCommand("AUTHCHALLENGE SAFECOOKIE " + client_nonce_.toHex(), [this](const TorCtlReply& reply){
            if (reply.status == 250) {
                qDebug() << "Torctl AUTHCHALLENGE successful - proceeding with authenticating";
            }

            const auto map = reply.parse();
            auto ac = map.at("AUTHCHALLENGE").toMap();
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
    QByteArray rval;
    rval.resize(EVP_MAX_MD_SIZE);

    auto ctx = std::shared_ptr<HMAC_CTX>(HMAC_CTX_new(), HMAC_CTX_free);
    if (!ctx) {
        throw CryptoError("HMAC_CTX_new()");
    }

    auto len = static_cast<unsigned int>(rval.size());
    HMAC_Init_ex(ctx.get(), key.data(), key.size(), EVP_sha256(), NULL);

    HMAC_Update(ctx.get(),
                reinterpret_cast<const unsigned char *>(cookie_.data()),
                static_cast<size_t>(cookie_.size()));

    HMAC_Update(ctx.get(),
                reinterpret_cast<const unsigned char *>(client_nonce_.data()),
                static_cast<size_t>(client_nonce_.size()));

    HMAC_Update(ctx.get(),
                reinterpret_cast<const unsigned char *>(serverNonce.data()),
                static_cast<size_t>(serverNonce.size()));

    HMAC_Final(ctx.get(), reinterpret_cast<unsigned char *>(rval.data()), &len);

    rval.resize(static_cast<int>(len));

    return rval;
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

