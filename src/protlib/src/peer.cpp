#include <array>
#include <algorithm>
#include <vector>
#include <assert.h>
#include <sodium.h>

#include <QJsonDocument>
#include <QtEndian>

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

uint64_t Peer::send(const QJsonDocument &json)
{
    auto jsonData = json.toJson(QJsonDocument::Compact);

    // Data format:
    // Two bytes length | one byte version | four bytes channel | 8 bytes id | data

    // The length is encrypted individually to allow the peer to read it before
    // fetching the payload.

    const size_t len = 1 + 4 + 8 + static_cast<size_t>(jsonData.size());
    vector<uint8_t> payload_len(2),
            buffer(len),
            cipherlen(2 + crypto_secretstream_xchacha20poly1305_ABYTES),
            ciphertext(len + crypto_secretstream_xchacha20poly1305_ABYTES - 2);
    mview_t version{buffer.data(), 1};
    mview_t channel{version.end(), 4};
    mview_t id{channel.end(), 8};
    mview_t json_payload{id.end(), static_cast<size_t>(jsonData.size())};

    assert(buffer.size() == (+ version.size()
                             + channel.size()
                             + id.size()
                             + json_payload.size()));

    union {
        array<unsigned char, 8> bytes;
        quint16 uint16;
        quint32 uint32;
        quint64 uint64;
    } number_u = {};

    number_u.uint16 = qToBigEndian(static_cast<quint16>(len - 2));
    for(size_t i = 0; i < payload_len.size(); ++i) {
        payload_len.at(i) = number_u.bytes.at(i);
    }

    version.at(0) = '\1';

    // Channel 0 is the control channel
    number_u.uint32 = qToBigEndian(static_cast<quint32>(0));
    for(size_t i = 0; i < channel.size(); ++i) {
        channel.at(i) = number_u.bytes.at(i);
    }

    number_u.uint64 = qToBigEndian(++request_id_);
    for(size_t i = 0; i < id.size(); ++i) {
        id.at(i) = number_u.bytes.at(i);
    }

    assert(static_cast<size_t>(jsonData.size()) == json_payload.size());
    memcpy(json_payload.data(), jsonData.constData(), static_cast<size_t>(jsonData.size()));

    // encrypt length
    if (crypto_secretstream_xchacha20poly1305_push(&stateOut,
                                               cipherlen.data(),
                                               nullptr,
                                               payload_len.data(),
                                               payload_len.size(),
                                               nullptr, 0, 0) != 0) {
        throw runtime_error("Stream encryption failed");
    }

    connection_->write(cipherlen);

    // Encrypt the payload
    if (crypto_secretstream_xchacha20poly1305_push(&stateOut,
                                               ciphertext.data(),
                                               nullptr,
                                               buffer.data(),
                                               buffer.size(),
                                               nullptr, 0, 0) != 0) {
        throw runtime_error("Stream encryption failed");
    }

    connection_->write(ciphertext);

    return request_id_;
}

// Incoming, stream-encrypted data
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

