#include "include/ds/dsserver.h"

#include "logfault/logfault.h"

namespace ds {
namespace prot {

using namespace  std;

DsServer::DsServer(ds::prot::ConnectionSocket::ptr_t
                             connection, ds::core::ConnectData connectionData)
    : Peer{move(connection), move(connectionData)}
{

    connect(getConnectionPtr().get(), &ConnectionSocket::haveBytes,
            this, [this](const auto& data) {
        advance(data);
    });

    LFLOG_DEBUG << "Peer connected on port " << connection_->localPort()
                << " with id " << connection_->getUuid().toString()
                << ". Starting DS protocol.";

    // Get exactely the hello payload.
    connection_->wantBytes(Hello::bytes + crypto_box_SEALBYTES);

    // TODO: Set up a timer so we time out if things don't progress
}

void DsServer::authorize(bool authorize)
{
    if (!authorize) {
        LFLOG_DEBUG << "Connection " << connection_->getUuid().toString()
                    << " was not authorized to proceed. Closing.";
        state_ = State::UNAUTHORIZED;
        close();
        return;
    }

    LFLOG_DEBUG << "Connection " << connection_->getUuid().toString()
                << " is authorized to proceed. Setting up secure streams.";

    Olleh olleh;
    olleh.version.at(0) = '\1';
    prepareEncryption(stateOut, olleh.header, olleh.key);

    // Sign the payload
    connectionData_.identitysCert->sign(
                olleh.signature,
                {olleh.version, olleh.key, olleh.header});

    array<uint8_t, olleh.buffer.size() + crypto_box_SEALBYTES> ciphertext = {};
    connectionData_.contactsCert->encrypt(ciphertext, olleh.buffer);

    // Send the message to the server.
    connection_->write(ciphertext);
    state_ = State::ENCRYPTED_STREAM;

    LFLOG_DEBUG << "The data-stream to " << connection_->getUuid().toString()
                << " is fully switched to stream-encryption.";
    enableEncryptedStream();
    emit connectedToPeer(shared_from_this());
}


void DsServer::advance(const data_t& data)
{
    try {
        switch(state_) {
            case State::ENCRYPTED_STREAM:
                processStream(data);
                break;
            case State::CONNECTED:
                getHello(data);
                break;
            case State::WAITING_FOR_AUTHORIZATION:
            case State::FAILED:
            case State::UNAUTHORIZED:
                return;
        }
    } catch(const std::exception& ex) {
        LFLOG_ERROR << "Caught exception while advancing IO on "
                    << connection_->getUuid().toString()
                    << ": " << ex.what();

        state_ = State::FAILED;
        connection_->close();
    }
}

void DsServer::getHello(const data_t& data)
{

    // Data is encrypted with our pubkey. Decrypt it.
    Hello hello;
    assert(hello.buffer.size() == data.size() - crypto_box_SEALBYTES);

    if (!connectionData_.identitysCert->decrypt(hello.buffer, data)) {
        LFLOG_ERROR << "Failed to decrypt hello payload from " << connection_->getUuid().toString();
        connection_->close();
        return;
    }
    
    // Check version
    if (hello.version.at(0) != 1) {
        LFLOG_ERROR << "Unsupported Hello version "
                    << static_cast<unsigned int>(hello.version.at(0))
                    << " from " << connection_->getUuid().toString();
        connection_->close();
        return;
    }

    // Validate the signature
    auto client_cert = crypto::DsCert::createFromPubkey(hello.pubkey.toByteArray());
    if (!client_cert->verify(
                hello.signature,
                {hello.version, hello.key, hello.header, hello.pubkey})) {
        LFLOG_ERROR << "Signature in Hello message was forged from "
            << connection_->getUuid().toString();
        connection_->close();
        return;
    }

    connectionData_.contactsCert = crypto::DsCert::createFromPubkey(hello.pubkey.toByteArray());

    // At this point, any further inbound data is assumed to be encrypted
    prepareDecryption(stateIn, hello.header, hello.key);

    // Stall further IO until we get authorization to proceed
    connection_->wantBytes(0);
    state_ = State::WAITING_FOR_AUTHORIZATION;

    LFLOG_DEBUG << "We are ready to proceed with connection from "
                << client_cert->getB58PubKey()
                << " on " << connection_->getUuid().toString();

    // Send a signal to authorize the connection (it may be blacklisted)
    emit incomingPeer(shared_from_this());
}

}} // namespaces
