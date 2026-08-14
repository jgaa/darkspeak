#include <QTimer>
#include <QNetworkProxy>
#include <algorithm>
#include <sodium.h>

#include "include/ds/dsclient.h"
#include "ds/handshake.h"
#include "logfault/logfault.h"

namespace ds::prot {

DsClient::DsClient(ConnectionSocket::ptr_t connection, core::ConnectData connectionData)
    : Peer{std::move(connection), std::move(connectionData)}
{
    if (!handshakeBudget_.acquire()) {
        if (const auto count = handshake::recordFloodEvent()) {
            LFLOG_WARN << "Handshake flood protection rejected " << *count
                       << " connection attempts in the last interval";
        }
        LFLOG_DEBUG << "Outgoing handshake rejected because the global handshake budget is full";
        close();
        return;
    }
    handshakeTimer_.setSingleShot(true);
    connect(&handshakeTimer_, &QTimer::timeout, this, [this]() {
        if (state_ != State::ENCRYPTED_STREAM || !capabilitiesReceived_) {
            LFLOG_WARN << "Outgoing peer handshake timed out";
            failHandshake();
        }
    });
    initConnections();
    startConnectRetryTimer();
}

void DsClient::advance()
{
    if (state_ == State::CONNECTED) {
        sayHello();
    }
}

void DsClient::advance(const data_t& data)
{
    if (state_ == State::ENCRYPTED_STREAM) {
        const bool hadCapabilities = capabilitiesReceived_;
        processStream(data);
        if (capabilitiesReceived_) {
            handshakeBudget_.release();
            handshakeTimer_.stop();
            if (!hadCapabilities) {
                // ServerHello authenticates the server identity and its
                // ephemeral key, but the server's possession of the derived
                // session key is confirmed only by the first authenticated
                // server-to-client application record. Capabilities is that
                // record, so do not expose the connection to the application
                // before processStream() has authenticated it.
                emit connectedToPeer(shared_from_this());
            }
        }
    } else if (state_ == State::CONNECTED) {
        advance();
    } else if (state_ == State::GET_SERVER_HELLO) {
        getServerHello(data);
    }
}

void DsClient::sayHello()
{
    if (!connection_->isWritable() || !connectionData_.contactsCert
            || !connectionData_.identitysCert) {
        return;
    }
    crypto_kx_keypair(ephemeralPublicKey_.data(), ephemeralSecretKey_.data());
    randombytes_buf(clientNonce_.data(), clientNonce_.size());

    QByteArray unsignedHello;
    unsignedHello.append(char(handshake::version));
    unsignedHello.append(reinterpret_cast<const char *>(ephemeralPublicKey_.data()),
                         ephemeralPublicKey_.size());
    unsignedHello.append(reinterpret_cast<const char *>(clientNonce_.data()),
                         clientNonce_.size());
    unsignedHello.append(reinterpret_cast<const char *>(
                             connectionData_.identitysCert->getSigningPubKey().cdata()),
                         crypto_sign_PUBLICKEYBYTES);

    QByteArray signatureInput = handshake::domain("DarkSpeak handshake client hello v2");
    signatureInput += char(handshake::version);
    signatureInput += connectionData_.contactsCert->getSigningPubKey().toByteArray();
    signatureInput += unsignedHello.mid(1);
    clientHello_ = unsignedHello + handshake::sign(*connectionData_.identitysCert, signatureInput);
    connection_->write(clientHello_);
    state_ = State::GET_SERVER_HELLO;
    handshakeTimer_.start(handshakeTimeoutMilliseconds);
    connection_->wantBytes(handshake::serverHelloBytes);
}

void DsClient::getServerHello(const data_t& data)
{
    if (data.size() != handshake::serverHelloBytes) {
        failHandshake();
        return;
    }
    const QByteArray serverHello = data.toByteArray();
    const QByteArray unsignedHello = serverHello.left(
        static_cast<int>(handshake::serverHelloBytes - crypto_sign_BYTES));
    const QByteArray signature = serverHello.right(crypto_sign_BYTES);
    QByteArray signatureInput = handshake::domain("DarkSpeak handshake server hello v2");
    signatureInput += handshake::sha256(clientHello_);
    signatureInput += unsignedHello;
    if (static_cast<unsigned char>(unsignedHello.at(0)) != handshake::version
            || !handshake::verify(connectionData_.contactsCert->getSigningPubKey().toByteArray(),
                                   signature, signatureInput)) {
        failHandshake();
        return;
    }

    std::array<unsigned char, crypto_kx_PUBLICKEYBYTES> serverPublic{};
    std::copy_n(reinterpret_cast<const unsigned char *>(unsignedHello.constData() + 1),
                serverPublic.size(), serverPublic.data());
    std::array<unsigned char, crypto_kx_SESSIONKEYBYTES> clientRx{};
    if (crypto_kx_client_session_keys(clientRx.data(), clientTx_.data(),
                                      ephemeralPublicKey_.data(), ephemeralSecretKey_.data(),
                                      serverPublic.data()) != 0) {
        failHandshake();
        return;
    }

    std::array<unsigned char, crypto_secretstream_xchacha20poly1305_HEADERBYTES> serverHeader{};
    std::copy_n(reinterpret_cast<const unsigned char *>(unsignedHello.constData() + 1
                 + crypto_kx_PUBLICKEYBYTES + 32), serverHeader.size(), serverHeader.data());
    mview_t serverHeaderView{serverHeader};
    mview_t clientRxView{clientRx};
    prepareDecryption(stateIn, serverHeaderView, clientRxView);
    sodium_memzero(clientRx.data(), clientRx.size());

    std::array<unsigned char, crypto_secretstream_xchacha20poly1305_HEADERBYTES> clientHeader{};
    if (crypto_secretstream_xchacha20poly1305_init_push(
            &stateOut, clientHeader.data(), clientTx_.data()) != 0) {
        failHandshake();
        return;
    }
    QByteArray transcript = handshake::sha256(clientHello_ + serverHello);
    QByteArray fields(1, char(handshake::version));
    fields.append(reinterpret_cast<const char *>(clientHeader.data()), clientHeader.size());
    QByteArray finish = fields;
    finish += handshake::sign(*connectionData_.identitysCert,
        handshake::signedInput("DarkSpeak handshake client finish v2", transcript + fields));
    QByteArray confirmation(crypto_auth_BYTES, Qt::Uninitialized);
    const QByteArray confirmationInput = handshake::signedInput(
        "DarkSpeak handshake client key confirmation v2", transcript + fields);
    crypto_auth(reinterpret_cast<unsigned char *>(confirmation.data()),
                reinterpret_cast<const unsigned char *>(confirmationInput.constData()),
                static_cast<unsigned long long>(confirmationInput.size()), clientTx_.data());
    finish += confirmation;
    connection_->write(finish);

    sodium_memzero(ephemeralSecretKey_.data(), ephemeralSecretKey_.size());
    sodium_memzero(clientTx_.data(), clientTx_.size());
    state_ = State::ENCRYPTED_STREAM;
    enableEncryptedStream();
    sendCapabilities();
}

void DsClient::failHandshake() noexcept
{
    handshakeBudget_.release();
    close();
}

void DsClient::startConnectRetryTimer()
{
    if (++numReconnects_ > maxReconnects_) {
        emit closeLater();
        return;
    }
    QTimer::singleShot(reconnectDelayMilliseconds_, this, [this]() {
        if (!notificationsDisabled_ && (connection_->state() == QAbstractSocket::ConnectingState
                                         || connection_->state() == QAbstractSocket::UnconnectedState)) {
            auto connection = std::make_shared<ConnectionSocket>(connection_->getDefaultHost(),
                                                                  connection_->getDefaultPort(),
                                                                  getConnectionId());
            connection->setProxy(connection_->proxy());
            connection_ = std::move(connection);
            useConnection(connection_.get());
            initConnections();
            startConnectRetryTimer();
            connection_->connectToDefaultHost();
        }
    });
}

void DsClient::initConnections()
{
    connect(connection_.get(), &QTcpSocket::connected, this, [this]() { advance(); });
    connect(connection_.get(), &QTcpSocket::readyRead, this, [this]() { advance(); });
    connect(getConnectionPtr().get(), &ConnectionSocket::haveBytes,
            this, [this](const auto& data) { advance(data); });
}

} // namespace ds::prot
