
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
    // TODO: Set timeout
    connect(connection_.get(), &QTcpSocket::connected,
            this, [this]() {
        advance();
    });

    connect(connection_.get(), &QTcpSocket::readyRead,
            this, [this]() {
        advance();
    });

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
        case State::GET_SERVER_HELLO:
            getHelloReply();
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

    // We don't need a nounce here, as we have a randomly generated key

    hello.version.at(0) = '\1'; // Version of the hello structure

    /* Shared secret key required to encrypt/decrypt the stream */
    crypto_secretstream_xchacha20poly1305_keygen(hello.key.data());

    /* Set up a new stream: initialize the state and create the header */
    crypto_secretstream_xchacha20poly1305_init_push(
                &stateOut, hello.header.data(), hello.key.data());

    // Copy our pubkey
    assert(hello.pubkey.size()
           == connectionData_.identitysCert->getSigningPubKey().size());
    memcpy(hello.pubkey.data(),
           connectionData_.identitysCert->getSigningPubKey().cdata(),
           hello.pubkey.size());

    // Sign the payload, so the server know we have the private key for our announced pubkey.
    connectionData_.identitysCert->sign(
                hello.signature,
                {hello.version, hello.key, hello.header, hello.pubkey});

    // Encrypt the payload with the receipients public encryption key
    const auto peer_cert = crypto::DsCert::createFromPubkey(connectionData_.contactsPubkey);    
    array<uint8_t, hello.buffer.size() + crypto_box_SEALBYTES> ciphertext;
    peer_cert->encrypt(ciphertext, hello.buffer);

    // Send the message to the server.
    connection_->write(ciphertext);
    state_ = State::GET_SERVER_HELLO;
    LFLOG_DEBUG << "Said hello to " << connection_->getUuid().toString();
}

void DsClient::getHelloReply()
{

}



}} // namespaces
