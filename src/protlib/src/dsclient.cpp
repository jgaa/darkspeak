
#include <array>
#include <vector>
#include <sodium.h>
#include "include/ds/dsclient.h"
#include "logfault/logfault.h"

namespace ds {
namespace prot {

using namespace  std;

DsClient::DsClient(ConnectionSocket::ptr_t connection, core::ConnectData connectionData)
    : connection_{move(connection)}, connectionData_{move(connectionData)}
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

    array<uint8_t, 1 /* version */
            + crypto_secretstream_xchacha20poly1305_KEYBYTES
            + crypto_secretstream_xchacha20poly1305_HEADERBYTES
            + crypto_sign_PUBLICKEYBYTES /* our pubkey */
            + crypto_sign_BYTES> hello;

    // We don't need a nounce here, as we have a randomly generated key

    hello[0] = '\1'; // Version of the hello structure
    crypto::MemoryView<uint8_t> version(hello.data(), 1);
    crypto::MemoryView<uint8_t> key{version.end(), crypto_secretstream_xchacha20poly1305_KEYBYTES};
    crypto::MemoryView<uint8_t> header{key.end(), crypto_secretstream_xchacha20poly1305_HEADERBYTES};
    crypto::MemoryView<uint8_t> pubkey{header.end(), crypto_sign_PUBLICKEYBYTES};
    crypto::MemoryView<uint8_t> signature{pubkey.end(), crypto_sign_BYTES};

    assert((1 + key.size() + header.size() + pubkey.size() + signature.size()) == hello.size());

    /* Shared secret key required to encrypt/decrypt the stream */
    crypto_secretstream_xchacha20poly1305_keygen(key.data());

    /* Set up a new stream: initialize the state and create the header */
    crypto_secretstream_xchacha20poly1305_init_push(&stateOut, header.data(), key.data());

    // Copy our pubkey
    assert(pubkey.size() == connectionData_.identitysCert->getSigningPubKey().size());
    memcpy(pubkey.data(), connectionData_.identitysCert->getSigningPubKey().cdata(), pubkey.size());

    // Sign the payload, so the server know we have the private key for our announced pubkey.
    connectionData_.identitysCert->sign(signature, {version, key, header, pubkey});

    // Encrypt the payload with the receipients public encryption key
    const auto peer_cert = crypto::DsCert::createFromPubkey(connectionData_.contactsPubkey);
    array<uint8_t, hello.size() + crypto_box_SEALBYTES> ciphertext;
    if (crypto_box_seal(
                ciphertext.data(),
                hello.data(),
                hello.size(),
                peer_cert->getEncryptionPubKey().cdata()) != 0) {
        throw runtime_error("Failed to encrypt hello message");
    }

    // Send the message to the server.
    connection_->write(ciphertext);
    state_ = State::GET_SERVER_HELLO;
    LFLOG_DEBUG << "Said hello to " << connection_->getUuid().toString();
}

void DsClient::getHelloReply()
{

}



}} // namespaces
