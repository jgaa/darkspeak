#include <array>
#include <algorithm>
#include <vector>
#include <sodium.h>
#include "ds/peer.h"
#include "logfault/logfault.h"

namespace ds {
namespace prot {

using namespace  std;

Peer::Peer(ConnectionSocket::ptr_t connection,
                   core::ConnectData connectionData)
    : connection_{move(connection)}, connectionData_{move(connectionData)}
{
}

void Peer::processStream(const Peer::data_t &data)
{

}

void Peer::prepareEncryption(Peer::stream_state_t &state,
                             mview_t& header,
                             Peer::mview_t &key)
{
    crypto_secretstream_xchacha20poly1305_keygen(key.data());

    if (crypto_secretstream_xchacha20poly1305_init_push(&state, header.data(), key.cdata()) != 0) {
        throw runtime_error("Failed to initialize encryption");
    }

}

void Peer::prepareDecryption(Peer::stream_state_t &state,
                             const mview_t &header,
                             const Peer::mview_t &key)
{
    if (crypto_secretstream_xchacha20poly1305_init_pull(&state, header.cdata(), key.cdata()) != 0) {
        LFLOG_WARN << "Invalid decryption key / header from: " << connection_->getUuid().toString();
        throw runtime_error("Failed to initialize decryption");
    }
}

}} // namespace

