#ifndef PEER_H
#define PEER_H

#include <array>
#include <limits>
#include <map>
#include <chrono>
#include <atomic>
#include <QTimer>
#include <QByteArray>

#include "ds/protocolmanager.h"
#include "ds/connectionsocket.h"
#include "ds/peerconnection.h"
#include "ds/file.h"
#include "ds/protocolcodec.h"

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
    static constexpr size_t max_record_bytes = codec::maxRecordBytes;
    static constexpr size_t max_payload_bytes = max_record_bytes;
    static constexpr size_t file_chunk_header_bytes = sizeof(quint64) + sizeof(quint64);
    static constexpr size_t file_chunk_bytes = 1024 * 8;
    static constexpr size_t file_window_chunks = 8;
    static constexpr int file_retry_limit = 5;
    enum class InState {
        DISABLED,
        RECORD_SIZE,
        RECORD_DATA,
        CLOSING
    };

    class Channel {
    public:
        using ptr_t = std::shared_ptr<Channel>;

        virtual ~Channel() = default;
        virtual void onIncoming(Peer& peer, const quint64 id,
                                const mview_t& data, const bool final) = 0;

        // Return 0 on EOF
        virtual uint64_t onOutgoing(Peer& peer) = 0;
        virtual void onAck(Peer& peer, quint64 sequence,
                           qlonglong offset, bool final) {
            Q_UNUSED(peer)
            Q_UNUSED(sequence)
            Q_UNUSED(offset)
            Q_UNUSED(final)
        }
        virtual void onTimeout(Peer& peer) { Q_UNUSED(peer) }
        virtual qlonglong resumeOffset() const noexcept { return 0; }
        virtual void onTransferDone(Peer& peer, bool success) {
            Q_UNUSED(peer)
            Q_UNUSED(success)
        }
    };

    Peer(ConnectionSocket::ptr_t connection,
         core::ConnectData connectionData);
    ~Peer() override;

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

    enum class RequestType {
        ConnectionResponse,
        FileResumeResult,
        AvatarResponse
    };

public slots:
    virtual void authorize(bool /*authorize*/) override {}

    // Send a request to a connected peer over the encrypted stream
    // Returns a unique id for the request (within the scope of this peer)
    uint64_t send(const ds::protocol::Envelope& envelope);
    uint64_t nextRequestId(RequestType type = RequestType::ConnectionResponse);
    uint64_t sendFileChunk(const QByteArray& fileId, quint64 sequence,
                           quint64 offset, const QByteArray& data, bool final);
    uint64_t sendFileAck(const QByteArray& fileId, quint64 nextSequence,
                         quint64 nextOffset, bool final) override;
    uint64_t sendFileCancel(const QByteArray& fileId,
                            core::FileCancelReason reason) override;
    uint64_t sendFileResume(const QByteArray& fileId, quint64 nextOffset,
                            const QByteArray& sha256, quint64 fileSize) override;
    uint64_t sendFileResumeResult(quint64 responseToRequestId,
                                  const QByteArray& fileId,
                                  core::FileResumeStatus status,
                                  quint64 nextOffset,
                                  core::FileCancelReason error) override;
    uint64_t sendFileOfferResponse(const QByteArray& fileId,
                                   core::FileOfferResponseStatus status) override;

signals:
    void incomingPeer(const std::shared_ptr<PeerConnection>& peer);
    void closeLater();
    void removeTransfer(core::File::Direction direction, const quint32 id);

private slots:
    void onCloseLater();

protected:
    void onReceivedData(const quint32 channel, const quint64 id,
                        const mview_t& data, const bool final);
    void onReceivedEnvelope(const ds::protocol::Envelope& envelope);
    void sendCapabilities();
    [[noreturn]] void protocolError(const QString& reason);
    void resetIdleTimer();
    void scheduleKeepAlive();
    void sendKeepAlive();
    void enableEncryptedStream();
    void wantRecordSize();
    void wantRecordData(const size_t bytes);
    void handleFileAck(const QByteArray& fileId, quint64 nextSequence,
                       quint64 nextOffset, bool final);
    void processStream(const data_t& data);
    void prepareEncryption(stream_state_t& state, mview_t& header, mview_t& key);
    void prepareDecryption(stream_state_t& state, const mview_t& header, const mview_t& key);
    void decrypt(mview_t& data, const mview_t& ciphertext, bool& final);
    quint32 createChannel(const core::File& file);
    uint64_t startReceive(core::File& file);
    uint64_t startSend(core::File& file);
    void useConnection(ConnectionSocket *cc);
    bool claimUnapprovedBudget() noexcept;
    void releaseUnapprovedBudget() noexcept;
    void expireOutstandingRequests();
    bool consumeRequest(quint64 requestId, RequestType type);

    struct PendingRequest {
        RequestType type;
        std::chrono::steady_clock::time_point expires;
    };

    InState inState_ = InState::DISABLED;
    ConnectionSocket::ptr_t connection_;
    core::ConnectData connectionData_;
    core::ResourceLimits resourceLimits_;
    stream_state_t stateIn = {};
    stream_state_t stateOut = {};
    quint64 request_id_ = {}; // Counter for outgoing requests
    quint64 frame_id_ = {}; // Counter for outgoing encrypted transport records
    quint64 last_received_request_id_ = {};
    QByteArray advertisedAvatarHash_;
    std::map<quint64, PendingRequest> outstandingRequests_;
    quint32 nextInchannel_ = 1;
    std::map<quint32, Channel::ptr_t> outChannels_;
    std::map<quint32, Channel::ptr_t> inChannels_;
    bool notificationsDisabled_ = false;
    bool capabilitiesReceived_ = false;
    bool inboundFinalReceived_ = false;
    bool applicationAuthorizationPending_ = false;
    bool unapprovedBudgetHeld_ = false;
    quint32 peerMaxRecordBytes_ = static_cast<quint32>(max_record_bytes);
    quint32 pendingRecordBytes_ = {};
    QTimer keepAliveTimer_;
    inline static std::atomic<size_t> unapprovedConnectionCount_ = 0;

    // PeerConnection interface
public:
    const QUuid uuid_;
    QUuid getConnectionId() const override;
    crypto::DsCert::ptr_t getPeerCert() const noexcept override;
    void close() override;
    QUuid getIdentityId() const noexcept override;
    uint64_t sendConnectionResponse(quint64 requestId,
                                    core::ConnectionResponseStatus status,
                                    const QString& message = {}) override;
    uint64_t sendMessageReceipt(const QByteArray& messageId) override;
    bool isConnected() const noexcept override;
    uint64_t sendUserInfo(const core::UserInfo &userInfo) override;
    uint64_t sendMessage(const core::Message &message) override;
    uint64_t sendAvatar(const QImage& avatar) override;
    uint64_t requestAvatar() override;
    uint64_t offerFile(const core::File& file) override;
    uint64_t startTransfer(core::File& file) override;
    uint64_t sendSome(core::File& file) override;
    void disableNotifications() override;

private:
    uint64_t sendAvatarResponse(quint64 responseToRequestId,
                                const QImage& avatar, bool authorized);
};

}} // namespaces


#endif // PEER_H
