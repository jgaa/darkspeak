#include <QTimer>
#include <QHostAddress>
#include <QNetworkProxy>

#include <vector>
#include <sodium.h>
#include "include/ds/dsclient.h"
#include "logfault/logfault.h"

namespace ds {
namespace prot {

using namespace  std;

DsClient::DsClient(ConnectionSocket::ptr_t connection,
                   core::ConnectData connectionData)
    : Peer{move(connection), move(connectionData)}
{
    initConnections();
    startConnectRetryTimer();
}

void DsClient::advance()
{
    switch(state_) {
        case State::CONNECTED:
            LFLOG_DEBUG << "Connected to " << connectionData_.address
                        << " with id " << connection_->getUuid().toString()
                        << ". Starting DS protocol.";
            sayHello();
            break;
        case State::GET_OLLEH:
        case State::ENCRYPTED_STREAM:
            break;
    }
}

void DsClient::advance(const Peer::data_t &data)
{
    switch(state_) {
    case State::ENCRYPTED_STREAM:
            processStream(data);
            break;
        case State::CONNECTED:
            advance();
            break;
        case State::GET_OLLEH:
            getHelloReply(data);
            break;
    }
}

/* Create an initial message to the server
 *
 * This message will initialize stream-based out-bound encryption
 * for the connection.
 */
void DsClient::sayHello()
{
    if (!connection_->isWritable()) {
        return;
    }

    Hello hello;
    hello.version.at(0) = '\1'; // Version of the hello structure

    prepareEncryption(stateOut, hello.header, hello.key);

    // Copy our pubkey
    {
        const auto& our_pubkey = connectionData_.identitysCert->getSigningPubKey();
        assert(hello.pubkey.size() == our_pubkey.size());
        copy(our_pubkey.cbegin(), our_pubkey.cend(), hello.pubkey.begin());
    }

    // Sign the payload, so the server know we have the private key for our announced pubkey.
    connectionData_.identitysCert->sign(
                hello.signature,
                {hello.version, hello.key, hello.header, hello.pubkey});

    // Encrypt the payload with the receipients public encryption key
    array<uint8_t, hello.buffer.size() + crypto_box_SEALBYTES> ciphertext = {};
    connectionData_.contactsCert->encrypt(ciphertext, hello.buffer);

    // Send the message to the server.
    connection_->write(ciphertext);
    state_ = State::GET_OLLEH;
    connection_->wantBytes(Olleh::bytes + crypto_box_SEALBYTES);
    LFLOG_DEBUG << "Said hello to " << connection_->getUuid().toString();
}

void DsClient::getHelloReply(const Peer::data_t &data)
{
    Olleh olleh;
    assert(olleh.buffer.size() == (data.size() - crypto_box_SEALBYTES));

    if (!connectionData_.identitysCert->decrypt(olleh.buffer, data)) {
        LFLOG_ERROR << "Failed to decrypt Hello reply payload from " << connection_->getUuid().toString();
        connection_->close();
        return;
    }

    // Check version
    if (olleh.version.at(0) != 1) {
        LFLOG_ERROR << "Unsupported Olleh version "
                    << static_cast<unsigned int>(olleh.version.at(0))
                    << " from " << connection_->getUuid().toString();
        connection_->close();
        return;
    }

    // Validate the signature
    if (!connectionData_.contactsCert->verify(
                olleh.signature,
                {olleh.version, olleh.key, olleh.header})) {
        LFLOG_ERROR << "Signature in Olleh message was forged from "
            << connection_->getUuid().toString();
        connection_->close();
        return;
    }

    // At this point, any further outbound data must be encrypted
    prepareDecryption(stateIn, olleh.header, olleh.key);
    state_ = State::ENCRYPTED_STREAM;
    LFLOG_DEBUG << "The data-stream to " << connection_->getUuid().toString()
                << " is fully switched to stream-encryption.";

    enableEncryptedStream();

    emit connectedToPeer(shared_from_this());
}

void DsClient::startConnectRetryTimer()
{
    if (++numReconnects_ > maxReconnects_) {
        LFLOG_DEBUG << "Unable to connect on connection " << getConnectionId().toString();
        emit closeLater();
        return;
    }
    QTimer::singleShot(reconnectDelayMilliseconds_, this, [this]() {
        if (!notificationsDisabled_
                && ((connection_->state() == QAbstractSocket::ConnectingState)
                 || (connection_->state() == QAbstractSocket::UnconnectedState))) {
            LFLOG_DEBUG << "Retrying connect on connection " << getConnectionId().toString();

            auto connection = make_shared<ConnectionSocket>(
                        connection_->getDefaultHost(),
                        connection_->getDefaultPort(),
                        getConnectionId());

            connection->setProxy(connection_->proxy());
            connection_ = move(connection);
            useConnection(connection_.get());
            initConnections();
            startConnectRetryTimer();
            connection_->connectToDefaultHost();
        } else {
            if (connection_) {
                LFLOG_TRACE << "Not reconnecting " << getConnectionId().toString()
                            << " , state is " << connection_->state();
            }
        }
    });
}

void DsClient::initConnections()
{
    connect(connection_.get(), &QTcpSocket::connected,
            this, [this]() {
        advance();
    });

    connect(connection_.get(), &QTcpSocket::readyRead,
            this, [this]() {
        advance();
    });

    connect(getConnectionPtr().get(), &ConnectionSocket::haveBytes,
            this, [this](const auto& data) {
        advance(data);
    });
}

}} // namespaces
