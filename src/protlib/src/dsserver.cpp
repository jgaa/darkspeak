#include "include/ds/dsserver.h"

#include <algorithm>
#include <sodium.h>

#include "ds/handshake.h"
#include "logfault/logfault.h"

namespace ds::prot {

DsServer::DsServer(ConnectionSocket::ptr_t connection, core::ConnectData connectionData)
    : Peer{std::move(connection), std::move(connectionData)}
{
    if (!handshakeBudget_.acquire()) {
        if (const auto count = handshake::recordFloodEvent()) {
            LFLOG_WARN << "Handshake flood protection rejected " << *count
                       << " connection attempts in the last interval";
        }
        LFLOG_DEBUG << "Incoming handshake rejected because the global handshake budget is full";
        state_ = State::FAILED;
        close();
        return;
    }
    handshakeTimer_.setSingleShot(true);
    connect(&handshakeTimer_, &QTimer::timeout, this, [this]() {
        if (state_ == State::CONNECTED || state_ == State::WAITING_FOR_FINISH
                || state_ == State::WAITING_FOR_AUTHORIZATION) {
            state_ = State::FAILED;
            failHandshake();
        }
    });
    handshakeTimer_.start(handshakeTimeoutMilliseconds);
    connect(getConnectionPtr().get(), &ConnectionSocket::haveBytes,
            this, [this](const auto& data) { advance(data); });
    connection_->wantBytes(handshake::clientHelloBytes);
}

void DsServer::authorize(bool authorize)
{
    if (state_ != State::WAITING_FOR_AUTHORIZATION) {
        return;
    }
    handshakeTimer_.stop();
    if (!authorize) {
        state_ = State::UNAUTHORIZED;
        releaseUnapprovedBudget();
        close();
        return;
    }
    applicationAuthorizationPending_ = false;
    releaseUnapprovedBudget();
    state_ = State::ENCRYPTED_STREAM;
    emit connectedToPeer(shared_from_this());
}

void DsServer::advance(const data_t& data)
{
    try {
        if (state_ == State::ENCRYPTED_STREAM || state_ == State::WAITING_FOR_AUTHORIZATION) {
            processStream(data);
        } else if (state_ == State::CONNECTED) {
            getHello(data);
        } else if (state_ == State::WAITING_FOR_FINISH) {
            getClientFinish(data);
        }
    } catch (const std::exception& ex) {
        LFLOG_ERROR << "Handshake/protocol processing failed: " << ex.what();
        failHandshake();
    }
}

void DsServer::getHello(const data_t& data)
{
    if (data.size() != handshake::clientHelloBytes || !connectionData_.identitysCert) {
        failHandshake();
        return;
    }
    clientHello_ = data.toByteArray();
    const QByteArray unsignedHello = clientHello_.left(
        static_cast<int>(handshake::clientHelloBytes - crypto_sign_BYTES));
    clientSigningPublicKey_ = unsignedHello.mid(1 + crypto_kx_PUBLICKEYBYTES + 32,
                                                crypto_sign_PUBLICKEYBYTES);
    const QByteArray signature = clientHello_.right(crypto_sign_BYTES);
    QByteArray signatureInput = handshake::domain("DarkSpeak handshake client hello v2");
    signatureInput += char(handshake::version);
    signatureInput += connectionData_.identitysCert->getSigningPubKey().toByteArray();
    signatureInput += unsignedHello.mid(1);
    if (static_cast<unsigned char>(unsignedHello.at(0)) != handshake::version
            || !handshake::verify(clientSigningPublicKey_, signature, signatureInput)) {
        failHandshake();
        return;
    }

    crypto_kx_keypair(ephemeralPublicKey_.data(), ephemeralSecretKey_.data());
    randombytes_buf(serverNonce_.data(), serverNonce_.size());
    if (crypto_kx_server_session_keys(serverRx_.data(), serverTx_.data(),
                                      ephemeralPublicKey_.data(), ephemeralSecretKey_.data(),
                                      reinterpret_cast<const unsigned char *>(unsignedHello.constData() + 1)) != 0) {
        failHandshake();
        return;
    }

    std::array<unsigned char, crypto_secretstream_xchacha20poly1305_HEADERBYTES> header{};
    if (crypto_secretstream_xchacha20poly1305_init_push(&stateOut, header.data(), serverTx_.data()) != 0) {
        failHandshake();
        return;
    }
    QByteArray unsignedServerHello;
    unsignedServerHello.append(char(handshake::version));
    unsignedServerHello.append(reinterpret_cast<const char *>(ephemeralPublicKey_.data()),
                                ephemeralPublicKey_.size());
    unsignedServerHello.append(reinterpret_cast<const char *>(serverNonce_.data()),
                                serverNonce_.size());
    unsignedServerHello.append(reinterpret_cast<const char *>(header.data()), header.size());
    QByteArray signatureInput2 = handshake::domain("DarkSpeak handshake server hello v2");
    signatureInput2 += handshake::sha256(clientHello_);
    signatureInput2 += unsignedServerHello;
    serverHello_ = unsignedServerHello + handshake::sign(*connectionData_.identitysCert,
                                                         signatureInput2);
    connection_->write(serverHello_);
    state_ = State::WAITING_FOR_FINISH;
    connection_->wantBytes(handshake::clientFinishBytes);
}

void DsServer::getClientFinish(const data_t& data)
{
    if (data.size() != handshake::clientFinishBytes) {
        failHandshake();
        return;
    }
    const QByteArray finish = data.toByteArray();
    const QByteArray fields = finish.left(1 + crypto_secretstream_xchacha20poly1305_HEADERBYTES);
    const QByteArray signature = finish.mid(fields.size(), crypto_sign_BYTES);
    const QByteArray confirmation = finish.right(crypto_auth_BYTES);
    const QByteArray transcript = handshake::sha256(clientHello_ + serverHello_);
    if (static_cast<unsigned char>(fields.at(0)) != handshake::version) {
        failHandshake();
        return;
    }
    QByteArray signatureInput = handshake::signedInput(
        "DarkSpeak handshake client finish v2", transcript + fields);
    if (!handshake::verify(clientSigningPublicKey_, signature, signatureInput)) {
        failHandshake();
        return;
    }
    const QByteArray confirmationInput = handshake::signedInput(
        "DarkSpeak handshake client key confirmation v2", transcript + fields);
    if (crypto_auth_verify(reinterpret_cast<const unsigned char *>(confirmation.constData()),
                           reinterpret_cast<const unsigned char *>(confirmationInput.constData()),
                           static_cast<unsigned long long>(confirmationInput.size()),
                           serverRx_.data()) != 0) {
        failHandshake();
        return;
    }
    std::array<unsigned char, crypto_secretstream_xchacha20poly1305_HEADERBYTES> clientHeader{};
    std::copy_n(reinterpret_cast<const unsigned char *>(fields.constData() + 1),
                clientHeader.size(), clientHeader.data());
    mview_t headerView{clientHeader};
    mview_t keyView{serverRx_};
    prepareDecryption(stateIn, headerView, keyView);
    connectionData_.contactsCert = crypto::DsCert::createFromPubkey(clientSigningPublicKey_);
    sodium_memzero(ephemeralSecretKey_.data(), ephemeralSecretKey_.size());
    sodium_memzero(serverRx_.data(), serverRx_.size());
    sodium_memzero(serverTx_.data(), serverTx_.size());

    enableEncryptedStream();
    if (!claimUnapprovedBudget()) {
        LFLOG_WARN << "Rejecting encrypted unapproved connection: global limit reached";
        failHandshake();
        return;
    }
    state_ = State::WAITING_FOR_AUTHORIZATION;
    applicationAuthorizationPending_ = true;
    handshakeBudget_.release();
    sendCapabilities();
    handshakeTimer_.start(authorizationTimeoutMilliseconds);
    emit incomingPeer(shared_from_this());
}

void DsServer::failHandshake() noexcept
{
    handshakeBudget_.release();
    state_ = State::FAILED;
    close();
}

} // namespace ds::prot
