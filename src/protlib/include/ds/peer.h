#ifndef PEER_H
#define PEER_H

#include <array>
#include <assert.h>

#include "ds/protocolmanager.h"
#include "ds/connectionsocket.h"
#include "ds/peerconnection.h"

namespace ds {
namespace prot {


class Peer : public core::PeerConnection
{
    Q_OBJECT
public:
    using ptr_t = std::shared_ptr<Peer>;
    using mview_t = crypto::MemoryView<uint8_t>;
    using data_t = crypto::MemoryView<uint8_t>;
    using stream_state_t = crypto_secretstream_xchacha20poly1305_state;
    static constexpr size_t crypt_bytes = crypto_secretstream_xchacha20poly1305_ABYTES;
    enum class InState {
        DISABLED,
        CHUNK_SIZE,
        CHUNK_DATA
    };

    struct Hello {
        static constexpr size_t bytes = 1 /* version */
                + crypto_secretstream_xchacha20poly1305_KEYBYTES
                + crypto_secretstream_xchacha20poly1305_HEADERBYTES
                + crypto_sign_PUBLICKEYBYTES /* pubkey */
                + crypto_sign_BYTES;
        Hello()
            : buffer{}, version{buffer.data(), 1}
            , key{version.end(), crypto_secretstream_xchacha20poly1305_KEYBYTES}
            , header{key.end(), crypto_secretstream_xchacha20poly1305_HEADERBYTES}
            , pubkey{header.end(), crypto_sign_PUBLICKEYBYTES}
            , signature{pubkey.end(), crypto_sign_BYTES}
        {
            assert((1 + key.size() + header.size() + pubkey.size() + signature.size())
                   == buffer.size());
        }

        std::array<uint8_t, bytes> buffer;
        mview_t version;
        mview_t key;
        mview_t header;
        mview_t pubkey;
        mview_t signature;
    };

    struct Olleh {
        static constexpr size_t bytes = 1 /* version */
                + crypto_secretstream_xchacha20poly1305_KEYBYTES
                + crypto_secretstream_xchacha20poly1305_HEADERBYTES
                + crypto_sign_BYTES;
        Olleh()
            : buffer{}, version{buffer.data(), 1}
            , key{version.end(), crypto_secretstream_xchacha20poly1305_KEYBYTES}
            , header{key.end(), crypto_secretstream_xchacha20poly1305_HEADERBYTES}
            , signature{header.end(), crypto_sign_BYTES}
        {
            assert((1 + key.size() + header.size() + signature.size())
                   == buffer.size());
        }

        std::array<uint8_t, bytes> buffer;
        mview_t version;
        mview_t key;
        mview_t header;
        mview_t signature;
    };

    Peer(ConnectionSocket::ptr_t connection,
         core::ConnectData connectionData);
    virtual ~Peer() = default;

    ConnectionSocket& getConnection() {
        if (!connection_) {
            throw std::runtime_error("No connection object");
        }
        return *connection_;
    }

    ConnectionSocket::ptr_t getConnectionPtr() {
        if (!connection_) {
            throw std::runtime_error("No connection object");
        }
        return connection_;
    }

    const core::ConnectData& getConnectData() const {
        return connectionData_;
    }

public slots:
    virtual void authorize(bool /*authorize*/) {}

    // Send a request to a connected peer over the encrypted stream
    // Returns a unique id for the request (within the scope of this peer)
    uint64_t send(const QJsonDocument& json);

signals:
    void incomingPeer(PeerConnection *peer);

protected:
    void enableEncryptedStream();
    void wantChunkSize();
    void wantChunkData(const size_t bytes);
    void processStream(const data_t& data);
    void prepareEncryption(stream_state_t& state, mview_t& header, mview_t& key);
    void prepareDecryption(stream_state_t& state, const mview_t& header, const mview_t& key);
    void decrypt(mview_t& data, const mview_t& ciphertext);
    QByteArray safePayload(const mview_t& data);

    InState inState_ = InState::DISABLED;
    ConnectionSocket::ptr_t connection_;
    core::ConnectData connectionData_;
    stream_state_t stateIn;
    stream_state_t stateOut;
    quint64 request_id_ = {}; // Counter for outgoing requests

    // PeerConnection interface
public:
    const QUuid uuid_;
    QUuid getConnectionId() const override;
    crypto::DsCert::ptr_t getPeerCert() const noexcept override;
    void close() override;
    QUuid getIdentityId() const noexcept override;
};

}} // namespaces


#endif // PEER_H
