#include <array>
#include <algorithm>
#include <vector>
#include <map>
#include <chrono>
#include <cassert>
#include <regex>
#include <limits>

#include <sodium.h>

#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>
#include <QRandomGenerator>
#include <QRegularExpression>
#include <QSet>
#include <QDate>
#include <QTime>
#include <QTimeZone>
#include <QtEndian>

#include "ds/peer.h"
#include "ds/bio.h"
#include "ds/message.h"
#include "ds/errors.h"
#include "ds/file.h"
#include "ds/resourcelimits.h"
#include "ds/dsengine.h"
#include "ds/imageutil.h"
#include "ds/textpolicy.h"
#include "ds/bytes.h"
#include "ds/mediapreview.h"

#include "logfault/logfault.h"

namespace ds {
namespace prot {

using namespace std;
using namespace core;

namespace  {
const std::vector<QString> encoding_names = {"us-ascii", "utf-8"};
const std::map<QString, Message::Encoding>  encoding_lookup = {
    {"us-ascii", Message::US_ACSII},
    {"utf-8", Message::UTF8}        
};

bool validMimeType(const QString& type)
{
    static const QRegularExpression mimePattern{
        QStringLiteral("^[a-z0-9!#$&^_.+\\-]+/[a-z0-9!#$&^_.+\\-]+$")};
    return type.toUtf8().size() <= 127 && mimePattern.match(type).hasMatch();
}

bool validMimePreferences(const QStringList& types)
{
    if (types.size() > 64) {
        return false;
    }
    QSet<QString> seen;
    for (const auto& type : types) {
        if (type == QStringLiteral("*")) {
            return types.size() == 1;
        }
        if (!validMimeType(type)
                || seen.contains(type)) {
            return false;
        }
        seen.insert(type);
    }
    return true;
}

bool validMessageTimestamp(const qint64 minutes)
{
    const auto minimum = QDateTime{QDate{1, 1, 1}, QTime{0, 0}, QTimeZone::UTC}
            .toSecsSinceEpoch() / 60;
    const auto maximum = QDateTime{QDate{9999, 12, 31}, QTime{23, 59}, QTimeZone::UTC}
            .toSecsSinceEpoch() / 60;
    return minutes >= minimum && minutes <= maximum;
}

bool validAsciiMessage(const QByteArray& content)
{
    for (const auto byte : content) {
        const auto value = static_cast<unsigned char>(byte);
        if (!((value >= 0x20 && value <= 0x7e) || value == 0x09 || value == 0x0a)) {
            return false;
        }
    }
    return true;
}

class IncomingFileChannel : public Peer::Channel {
public:
    explicit IncomingFileChannel(core::File::ptr_t file) : file_{std::move(file)} {
        next_offset_ = quint64(file_->getBytesTransferred());
        durable_offset_ = next_offset_;
        next_sequence_ = next_offset_ / Peer::file_chunk_bytes;
        durable_sequence_ = next_sequence_ ? next_sequence_ - 1 : 0;
    }
    ~IncomingFileChannel() override = default;
    void onIncoming(Peer& peer, quint64, const Peer::mview_t& data, bool final) override {
        if (data.size() < Peer::file_chunk_header_bytes) throw Error("Invalid file chunk header");
        quint64 sequence{}, offset{};
        memcpy(&sequence, data.cdata(), sizeof(sequence));
        memcpy(&offset, data.cdata() + sizeof(sequence), sizeof(offset));
        sequence = qFromBigEndian(sequence); offset = qFromBigEndian(offset);
        if (sequence < next_sequence_) {
            // Retransmissions cannot grow the write queue. Never acknowledge uncheckpointed bytes.
            if (durable_offset_ && file_->getState() == File::FS_TRANSFERRING)
                peer.sendFileAck(file_->getFileId(), durable_sequence_ + 1, durable_offset_, false);
            return;
        }
        const auto size = data.size() - Peer::file_chunk_header_bytes;
        const auto total = quint64(file_->getSize());
        if (file_->getState() != File::FS_TRANSFERRING || final_queued_
            || queue_.size() >= Peer::file_window_chunks || sequence != next_sequence_
            || offset != next_offset_ || size > Peer::file_chunk_bytes || offset > total || size > total - offset
            || (size == 0 && (!final || offset != total)) || (final && offset + size != total)
            || (!final && size != Peer::file_chunk_bytes)) throw Error("File chunk exceeds receive window or bounds");
        queue_.push_back({sequence, offset,
            QByteArray(reinterpret_cast<const char*>(data.cdata() + Peer::file_chunk_header_bytes), qsizetype(size)), final});
        ++next_sequence_; next_offset_ += size; final_queued_ = final;
        if (!draining_) {
            draining_ = true;
            drain(std::static_pointer_cast<IncomingFileChannel>(shared_from_this()), std::static_pointer_cast<Peer>(peer.shared_from_this()));
        }
    }
    uint64_t onOutgoing(Peer&) override { throw Error("Incoming file channel cannot send data"); }
    qlonglong resumeOffset() const noexcept override { return qlonglong(durable_offset_); }
    void onTransferDone(Peer& peer, bool success) override {
        if (success && final_queued_ && peer.isConnected()) {
            try { peer.sendFileAck(file_->getFileId(), durable_sequence_ + 1, durable_offset_, true); }
            catch (...) { LFLOG_DEBUG << "Final file acknowledgement could not be queued"; }
        }
    }
private:
    QCoro::Task<void> drain(std::shared_ptr<IncomingFileChannel> self, std::shared_ptr<Peer> peer) {
        try {
            while (!queue_.empty() && file_->getState() == File::FS_TRANSFERRING && peer->isConnected()) {
                auto chunk = std::move(queue_.front()); queue_.pop_front();
                const auto start_offset = chunk.offset;
                // Coalesce the available receive window into one bounded checkpoint and cumulative ACK.
                while (!queue_.empty() && !chunk.final && chunk.bytes.size() + queue_.front().bytes.size() <= qsizetype(Peer::file_window_chunks * Peer::file_chunk_bytes)) {
                    auto next = std::move(queue_.front()); queue_.pop_front();
                    chunk.bytes.append(next.bytes); chunk.sequence = next.sequence; chunk.final = next.final;
                }
                auto written = co_await file_->writeIncoming(start_offset, chunk.bytes);
                if (file_->getState() != File::FS_TRANSFERRING || !peer->isConnected()) break;
                if (!written || written.value() != chunk.bytes.size()) {
                    if (!written) {
                        LFLOG_DEBUG << "Incoming write failed: code=" << int(written.error().code)
                                    << ", native=" << written.error().native_code;
                    } else {
                        LFLOG_DEBUG << "Incoming write was short: requested=" << chunk.bytes.size()
                                    << ", written=" << written.value();
                    }
                    file_->transferFailed(QObject::tr("Unable to write the incoming file."), File::FS_FAILED); break;
                }
                auto checkpoint = co_await file_->checkpointIncoming();
                if (file_->getState() != File::FS_TRANSFERRING || !peer->isConnected()) break;
                if (!checkpoint) { file_->transferFailed(QObject::tr("Unable to checkpoint the incoming file."), File::FS_FAILED); break; }
                durable_offset_ = start_offset + quint64(chunk.bytes.size()); durable_sequence_ = chunk.sequence;
                file_->setBytesTransferred(qlonglong(durable_offset_));
                if (file_->getState() != File::FS_TRANSFERRING || !peer->isConnected()) break;
                if (chunk.final) file_->validateHash();
                else peer->sendFileAck(file_->getFileId(), chunk.sequence + 1, durable_offset_, false);
            }
        } catch (...) {
            file_->transferFailed(QObject::tr("The incoming transfer could not continue."), File::FS_FAILED);
        }
        queue_.clear(); draining_ = false;
        // An empty queue is normal between receive windows. Keep the writer open
        // until the transfer stops; later chunks still use this channel.
        if ((file_->getState() != File::FS_TRANSFERRING || !peer->isConnected())
            && file_->getState() != File::FS_HASHING && file_->getState() != File::FS_DONE)
            co_await file_->suspendIncoming();
    }
    struct Chunk { quint64 sequence; quint64 offset; QByteArray bytes; bool final; };
    File::ptr_t file_;
    std::deque<Chunk> queue_;
    quint64 next_sequence_ = 0, next_offset_ = 0, durable_offset_ = 0, durable_sequence_ = 0;
    bool draining_ = false, final_queued_ = false;
};

class OutgoingFileChannel : public Peer::Channel {
public:
    explicit OutgoingFileChannel(core::File::ptr_t file) : file_{std::move(file)} {
        const auto offset = file_->getBytesTransferred();
        if (file_->getDirection() != File::OUTGOING || offset < 0 || offset > file_->getSize()
            || offset % Peer::file_chunk_bytes || file_->storageId().isNull()) throw Error("Invalid managed outgoing file");
        nextOffset_ = quint64(offset);
        nextSequence_ = nextOffset_ / Peer::file_chunk_bytes;
    }

    // Channel interface
public:
    void onIncoming(Peer &peer, const quint64 id, const Peer::mview_t &data,
                    const bool final) override {
        Q_UNUSED(peer);
        Q_UNUSED(id)
        Q_UNUSED(data)
        Q_UNUSED(final)
        throw Error("Outgoing file channel cannot receive data");
    }

    uint64_t onOutgoing(Peer& peer) override {
        if (!pumping_ && !endQueued_ && inFlight_.size() < Peer::file_window_chunks) {
            pumping_ = true;
            pump(std::static_pointer_cast<OutgoingFileChannel>(shared_from_this()), std::static_pointer_cast<Peer>(peer.shared_from_this()));
        }
        return 0; // Completion schedules network work after asynchronous disk I/O.
    }
    QCoro::Task<void> pump(std::shared_ptr<OutgoingFileChannel> self, std::shared_ptr<Peer> peer) {
        auto& storage = DsEngine::instance().getFileManager()->storage();
        auto operation = storage.holdOperation();
        if (!reader_) {
            auto ready = co_await storage.ready();
            if (!ready) { pumping_ = false; file_->transferFailed(QObject::tr("File storage is unavailable."), File::FS_FAILED); co_return; }
            auto opened = co_await storage.manager().openRead(file_->storageNamespace(), file_->storageId());
            if (!opened) { pumping_ = false; reportSourceChanged(*peer); co_return; }
            reader_ = std::move(opened).value();
        }
        while (file_->getState() == File::FS_TRANSFERRING && peer->isConnected()
               && inFlight_.size() < Peer::file_window_chunks && !endQueued_) {
            auto read = co_await reader_->readAt(nextOffset_, Peer::file_chunk_bytes);
            if (!read) { reportSourceChanged(*peer); break; }
            if (file_->getState() != File::FS_TRANSFERRING || !peer->isConnected()) break;
            const auto count = quint64(read.value().size());
            if (nextOffset_ > quint64(file_->getSize()) || count > quint64(file_->getSize()) - nextOffset_
                || (!count && nextOffset_ != quint64(file_->getSize()))) { reportSourceChanged(*peer); break; }
            const bool final = nextOffset_ + count == quint64(file_->getSize());
            const auto sequence = nextSequence_++;
            const auto offset = nextOffset_;
            nextOffset_ += count;
            QByteArray payload(Peer::file_chunk_header_bytes, Qt::Uninitialized);
            const auto wire_sequence = qToBigEndian(sequence), wire_offset = qToBigEndian(offset);
            memcpy(payload.data(), &wire_sequence, sizeof(wire_sequence));
            memcpy(payload.data() + sizeof(wire_sequence), &wire_offset, sizeof(wire_offset));
            payload.append(read.value());
            // Register before send: a transport can synchronously dispatch an acknowledgement.
            inFlight_.emplace(sequence, PendingChunk{payload, final, 0, std::chrono::steady_clock::now(), offset, nextOffset_});
            endQueued_ = final;
            try { peer->sendFileChunk(file_->getFileId(), sequence, offset, read.value(), final); }
            catch (...) { file_->transferFailed(QObject::tr("Unable to send the file chunk."), File::FS_FAILED); break; }
        }
        pumping_ = false;
    }

    void onAck(Peer& peer, quint64 sequence, qlonglong offset, bool final) override
    {
        if (file_->getState() == File::FS_DONE) {
            return;
        }
        if (file_->getState() != File::FS_TRANSFERRING) {
            throw Error("Acknowledgement for inactive file transfer");
        }
        if (sequence >= nextSequence_ || offset < 0
                || static_cast<quint64>(offset) > static_cast<quint64>(file_->getSize())) {
            throw Error("Invalid file acknowledgement");
        }

        // Cumulative ACKs may be repeated. They must describe exactly the
        // same contiguous prefix and may not move the sender's offset.
        if (haveAcknowledgement_ && sequence <= acknowledgedSequence_) {
            if (offset != acknowledgedOffset_ || final) {
                throw Error("Inconsistent duplicate file acknowledgement");
            }
            return;
        }

        const auto pending = inFlight_.find(sequence);
        if (pending == inFlight_.end()
                || offset != static_cast<qlonglong>(pending->second.endOffset)) {
            throw Error("File acknowledgement does not match the chunk");
        }

        auto it = inFlight_.begin();
        while (it != inFlight_.end() && it->first <= sequence) {
            it = inFlight_.erase(it);
        }

        const auto acknowledged = static_cast<qlonglong>(offset);
        acknowledgedSequence_ = sequence;
        acknowledgedOffset_ = acknowledged;
        haveAcknowledgement_ = true;
        if (acknowledged > file_->getBytesTransferred()) {
            file_->setBytesTransferred(acknowledged);
        }

        if (final) {
            if (!endQueued_ || sequence + 1 != nextSequence_ || !inFlight_.empty()
                    || acknowledged != file_->getSize()) {
                throw Error("Invalid final file acknowledgement");
            }
            file_->transferComplete();
        } else {
            onOutgoing(peer);
        }
    }

    void onTimeout(Peer& peer) override
    {
        const auto now = std::chrono::steady_clock::now();
        for (auto &[sequence, pending] : inFlight_) {
            Q_UNUSED(sequence)
            if (now - pending.sent < std::chrono::seconds(2)) {
                continue;
            }
            if (pending.retries >= Peer::file_retry_limit) {
                file_->transferFailed("File transfer acknowledgement timeout", File::FS_FAILED);
                return;
            }
            try {
                peer.sendFileChunk(file_->getFileId(), sequence, pending.offset,
                                   pending.payload.mid(static_cast<int>(Peer::file_chunk_header_bytes)),
                                   pending.final);
            } catch (const std::exception& ex) {
                file_->transferFailed(QStringLiteral("Failed to retransmit file chunk: ")
                                      + ex.what(), File::FS_FAILED);
                return;
            }
            pending.sent = now;
            ++pending.retries;
        }
    }

private:
    void reportSourceChanged(Peer& peer)
    {
        if (sourceChanged_) {
            return;
        }
        sourceChanged_ = true;
        try { peer.sendFileCancel(file_->getFileId(), FileCancelReason::SourceChanged); }
        catch (...) { LFLOG_DEBUG << "Source-change cancellation could not be queued"; }
        file_->transferFailed("File source changed during transfer", File::FS_FAILED);
    }

    struct PendingChunk {
        QByteArray payload;
        bool final = false;
        int retries = {};
        std::chrono::steady_clock::time_point sent;
        quint64 offset = {};
        quint64 endOffset = {};
    };

    std::unique_ptr<dsfm::IFileReader> reader_;
    bool pumping_ = false;
    File::ptr_t file_;
    std::map<quint64, PendingChunk> inFlight_;
    quint64 nextSequence_ = {};
    quint64 nextOffset_ = {};
    bool endQueued_ = false;
    bool sourceChanged_ = false;
    bool haveAcknowledgement_ = false;
    quint64 acknowledgedSequence_ = {};
    qlonglong acknowledgedOffset_ = {};
};


Message::Encoding toEncoding(const QString& name) {
    const auto it = encoding_lookup.find(name);
    if (it == encoding_lookup.end()) {
        throw NotFoundError(name);
    }
    return it->second;
}

} // anonymous namespace

Peer::Peer(ConnectionSocket::ptr_t connection,
           core::ConnectData connectionData)
    : connection_{move(connection)}, connectionData_{move(connectionData)}
    , resourceLimits_{connectionData_.resourceLimits}, uuid_{connection_->getUuid()}
{
    useConnection(connection.get());
    keepAliveTimer_.setSingleShot(true);
    connect(&keepAliveTimer_, &QTimer::timeout,
            this, &Peer::sendKeepAlive);
    auto *transferTimer = new QTimer(this);
    transferTimer->setInterval(1000);
    connect(transferTimer, &QTimer::timeout, this, [this]() {
        expireOutstandingRequests();
        for (const auto& [channel, instance] : outChannels_) {
            Q_UNUSED(channel)
            instance->onTimeout(*this);
        }
    });
    transferTimer->start();
    connect(this, &Peer::closeLater,
            this, &Peer::onCloseLater,
            Qt::QueuedConnection);

    connect(this, &Peer::removeTransfer,
            this, [this](core::File::Direction direction, const quint32 id){

        LFLOG_TRACE << "Removing channel #" << id
                    << " from connection " << getConnectionId().toString();

        if (direction == File::INCOMING) {
            inChannels_.erase(id);
        } else {
            outChannels_.erase(id);
        }
    }, Qt::QueuedConnection);
}

Peer::~Peer()
{
    releaseUnapprovedBudget();
    // QTcpSocket may emit disconnected while it is being destroyed.  The
    // socket is a member of Peer, so with the implicit destructor it would be
    // destroyed after Peer's destructor had started, leaving Qt callbacks
    // targeting a partially destructed Peer.
    if (connection_) {
        QObject::disconnect(connection_.get(), nullptr, this, nullptr);
        connection_.reset();
    }
}

uint64_t Peer::nextRequestId(const RequestType type)
{
    expireOutstandingRequests();
    if (outstandingRequests_.size() >= resourceLimits_.outstandingRequests) {
        core::recordPeerEvent(*this, core::EventJournal::RateLimited, core::EventJournal::Outgoing, core::EventJournal::Rejected);
        throw Error("Outstanding request limit reached");
    }

    quint64 candidate = request_id_;
    do {
        ++candidate;
    } while (candidate == 0 || outstandingRequests_.contains(candidate));
    request_id_ = candidate;
    outstandingRequests_.emplace(candidate, PendingRequest{
        type,
        std::chrono::steady_clock::now() + std::chrono::minutes(2)});
    return candidate;
}

void Peer::expireOutstandingRequests()
{
    const auto now = std::chrono::steady_clock::now();
    for (auto it = outstandingRequests_.begin(); it != outstandingRequests_.end();) {
        if (it->second.expires <= now) {
            it = outstandingRequests_.erase(it);
        } else {
            ++it;
        }
    }
}

bool Peer::consumeRequest(const quint64 requestId, const RequestType type)
{
    expireOutstandingRequests();
    const auto it = outstandingRequests_.find(requestId);
    if (it == outstandingRequests_.end() || it->second.type != type) {
        return false;
    }
    outstandingRequests_.erase(it);
    return true;
}

bool Peer::claimUnapprovedBudget() noexcept
{
    auto current = unapprovedConnectionCount_.load(std::memory_order_relaxed);
    while (current < resourceLimits_.unapprovedConnections
           && !unapprovedConnectionCount_.compare_exchange_weak(
               current, current + 1, std::memory_order_acq_rel,
               std::memory_order_relaxed)) {
    }
    if (current >= resourceLimits_.unapprovedConnections) {
        return false;
    }
    unapprovedBudgetHeld_ = true;
    return true;
}

void Peer::releaseUnapprovedBudget() noexcept
{
    if (unapprovedBudgetHeld_) {
        unapprovedBudgetHeld_ = false;
        unapprovedConnectionCount_.fetch_sub(1, std::memory_order_acq_rel);
    }
}

uint64_t Peer::send(const ds::protocol::Envelope& envelope)
{
    if (!connection_->isOpen()) {
        throw runtime_error("Connection is closed");
    }
    resetIdleTimer();
    auto payload = codec::serialize(envelope);
    const auto sendCeiling = std::min<quint32>(static_cast<quint32>(max_record_bytes),
                                               peerMaxRecordBytes_);
    if (payload.isEmpty() || payload.size() > static_cast<qsizetype>(sendCeiling)) {
        throw Error("Envelope exceeds protocol record limit");
    }
    ds::protocol::TransportEnvelope transport;
    transport.setPayload(payload);
    QByteArray padding(QRandomGenerator::system()->bounded(0, 257), Qt::Uninitialized);
    randombytes_buf(padding.data(), static_cast<size_t>(padding.size()));
    transport.setPadding(padding);
    const auto plain = codec::serialize(transport);
    if (plain.size() > static_cast<qsizetype>(sendCeiling - crypt_bytes)) {
        throw Error("Transport record exceeds protocol record limit");
    }
    const auto wireBytes = static_cast<size_t>(plain.size()) + crypt_bytes + sizeof(quint32);
    if (connection_->queuedBytes() > resourceLimits_.socketOutputBytes
            || wireBytes > resourceLimits_.socketOutputBytes
            - connection_->queuedBytes()) {
        core::recordPeerEvent(*this, core::EventJournal::RateLimited, core::EventJournal::Outgoing, core::EventJournal::Rejected);
        throw runtime_error("Connection output queue limit exceeded");
    }
    QByteArray ciphertext(plain.size() + crypt_bytes, Qt::Uninitialized);
    if (crypto_secretstream_xchacha20poly1305_push(
            &stateOut, reinterpret_cast<unsigned char *>(ciphertext.data()), nullptr,
            reinterpret_cast<const unsigned char *>(plain.constData()),
            static_cast<unsigned long long>(plain.size()), nullptr, 0,
            crypto_secretstream_xchacha20poly1305_TAG_MESSAGE) != 0) {
        throw runtime_error("Stream encryption failed");
    }
    QByteArray wireFrame(sizeof(quint32), Qt::Uninitialized);
    const auto length = qToBigEndian(static_cast<quint32>(ciphertext.size()));
    memcpy(wireFrame.data(), &length, sizeof(length));
    wireFrame += ciphertext;
    connection_->write(wireFrame);
    using E = core::EventJournal;
    if (envelope.hasMessage()) core::recordPeerEvent(*this, E::Message, E::Outgoing, E::Success, envelope.message().messageId());
    else if (envelope.hasBioRequest()) core::recordPeerEvent(*this, E::BioRequest, E::Outgoing, E::Success);
    else if (envelope.hasBioResponse()) core::recordPeerEvent(*this, E::BioResponse, E::Outgoing, E::Success);
    else if (envelope.hasAvatarRequest()) core::recordPeerEvent(*this, E::AvatarRequest, E::Outgoing, E::Success);
    else if (envelope.hasAvatar() || envelope.hasAvatarResponse()) core::recordPeerEvent(*this, E::AvatarTransfer, E::Outgoing, E::Success);
    else if (envelope.hasUserInfo()) core::recordPeerEvent(*this, E::UserInfo, E::Outgoing, E::Success);
    else if (envelope.hasMessageReceipt()) core::recordPeerEvent(*this, E::MessageReceipt, E::Outgoing, E::Success, envelope.messageReceipt().messageId());
#ifdef DARKSPEAK_WITH_TEST_MCP
    if (envelope.hasBioResponse() || envelope.hasUserInfo()) {
        const bool response = envelope.hasBioResponse();
        const bool content = response
            ? envelope.bioResponse().hasBio() && !envelope.bioResponse().bio().bioText().isEmpty()
            : envelope.userInfo().hasBio() && !envelope.userInfo().bio().bioText().isEmpty();
        const bool hash = !response && !envelope.userInfo().bioHash().isEmpty();
        if (auto* engine = core::DsEngine::tryInstance())
            emit engine->testBioWireQueued(this, response, content, hash);
    }
#endif

    return ++frame_id_;
}

uint64_t Peer::sendFileChunk(const QByteArray& fileId, const quint64 sequence,
                             const quint64 offset, const QByteArray& data,
                             const bool final)
{
    if (fileId.size() != 16 || data.size() > static_cast<qsizetype>(file_chunk_bytes)) {
        throw Error("Invalid v3 file chunk");
    }
    ds::protocol::FileChunk chunk;
    chunk.setFileId(fileId);
    chunk.setSequence(sequence);
    chunk.setOffset(offset);
    chunk.setData(data);
    chunk.setFinal(final);
    ds::protocol::Envelope envelope;
    envelope.setProtocolVersion(codec::protocolVersion);
    envelope.setFileChunk(std::move(chunk));
    return send(envelope);
}

uint64_t Peer::sendFileAck(const QByteArray& fileId, const quint64 nextSequence,
                           const quint64 nextOffset, const bool final)
{
    if (fileId.size() != 16) {
        throw Error("Invalid v3 file acknowledgement identifier");
    }
    ds::protocol::FileAck ack;
    ack.setFileId(fileId);
    ack.setNextSequence(nextSequence);
    ack.setNextOffset(nextOffset);
    ack.setFinal(final);
    ds::protocol::Envelope envelope;
    envelope.setProtocolVersion(codec::protocolVersion);
    envelope.setFileAck(std::move(ack));
    return send(envelope);
}

uint64_t Peer::sendFileOfferResponse(const QByteArray& fileId,
                                     const core::FileOfferResponseStatus status)
{
    if (fileId.size() != 16) {
        throw Error("Invalid v3 file-offer identifier");
    }
    ds::protocol::FileOfferResponse response;
    response.setFileId(fileId);
    response.setStatus(status == core::FileOfferResponseStatus::Accepted
                       ? ds::protocol::FileOfferResponse::Status::FILE_OFFER_ACCEPTED
                       : ds::protocol::FileOfferResponse::Status::FILE_OFFER_REJECTED);
    ds::protocol::Envelope envelope;
    envelope.setProtocolVersion(codec::protocolVersion);
    envelope.setFileOfferResponse(std::move(response));
    return send(envelope);
}

uint64_t Peer::sendFileCancel(const QByteArray& fileId,
                              const core::FileCancelReason reason)
{
    if (fileId.size() != 16) {
        throw Error("Invalid v3 file-cancel identifier");
    }
    ds::protocol::FileCancel cancel;
    cancel.setFileId(fileId);
    cancel.setReason(reason == core::FileCancelReason::SourceChanged
                     ? ds::protocol::FileErrorGadget::FileError::SOURCE_CHANGED
                     : ds::protocol::FileErrorGadget::FileError::USER_ACTION);
    ds::protocol::Envelope envelope;
    envelope.setProtocolVersion(codec::protocolVersion);
    envelope.setFileCancel(std::move(cancel));
    return send(envelope);
}

uint64_t Peer::sendFileResume(const QByteArray& fileId, const quint64 nextOffset,
                              const QByteArray& sha256, const quint64 fileSize)
{
    if (fileId.size() != 16 || sha256.size() != crypto_hash_sha256_BYTES
            || fileSize > ResourceLimits::hardAcceptedFileBytes
            || nextOffset > fileSize || nextOffset % file_chunk_bytes != 0) {
        throw Error("Invalid v3 file resume request");
    }
    ds::protocol::FileResume resume;
    resume.setFileId(fileId);
    resume.setNextOffset(nextOffset);
    resume.setSha256(sha256);
    resume.setFileSize(fileSize);
    ds::protocol::Envelope envelope;
    envelope.setProtocolVersion(codec::protocolVersion);
    envelope.setRequestId(nextRequestId(RequestType::FileResumeResult));
    envelope.setFileResume(std::move(resume));
    return send(envelope);
}

uint64_t Peer::sendFileResumeResult(const quint64 responseToRequestId,
                                    const QByteArray& fileId,
                                    const core::FileResumeStatus status,
                                    const quint64 nextOffset,
                                    const core::FileCancelReason error)
{
    if (responseToRequestId == 0 || fileId.size() != 16
            || nextOffset > ResourceLimits::hardAcceptedFileBytes
            || nextOffset % file_chunk_bytes != 0) {
        throw Error("Invalid v3 file resume result");
    }
    ds::protocol::FileResumeResult result;
    result.setFileId(fileId);
    result.setStatus(status == core::FileResumeStatus::Accepted
                         ? ds::protocol::FileResumeResult::Status::RESUME_ACCEPTED
                         : status == core::FileResumeStatus::Cancelled
                               ? ds::protocol::FileResumeResult::Status::RESUME_CANCELLED
                               : ds::protocol::FileResumeResult::Status::RESUME_REJECTED);
    result.setNextOffset(nextOffset);
    result.setError(status == core::FileResumeStatus::Cancelled
                        ? (error == core::FileCancelReason::SourceChanged
                               ? ds::protocol::FileErrorGadget::FileError::SOURCE_CHANGED
                               : ds::protocol::FileErrorGadget::FileError::USER_ACTION)
                        : ds::protocol::FileErrorGadget::FileError::FILE_ERROR_UNSPECIFIED);
    ds::protocol::Envelope envelope;
    envelope.setProtocolVersion(codec::protocolVersion);
    envelope.setResponseToRequestId(responseToRequestId);
    envelope.setFileResumeResult(std::move(result));
    return send(envelope);
}

void Peer::onReceivedData(const quint32 channel, const quint64 id,
                          const Peer::mview_t& data, const bool final)
{
    if (id == 0 || id <= last_received_request_id_) {
        protocolError(QStringLiteral("Non-monotonic frame id"));
    }
    last_received_request_id_ = id;
    resetIdleTimer();

    if (channel == 0) {
        if (final) {
            protocolError(QStringLiteral("Control frame cannot be final"));
        }
        protocolError(QStringLiteral("legacy frame dispatch is disabled"));
    } else {
        auto it = inChannels_.find(channel);
        if (it == inChannels_.end()) {
            LFLOG_WARN << "Data to unknown channel #" << channel
                       << " on connection " << getConnectionId().toString();

            protocolError(QStringLiteral("Unknown channel"));
        }

        Channel::ptr_t channelInstance;
        if (final) {
            // The state change in inIncoming with final will erase the channel, so we need
            // an extra instance to stay safe until onIncoming() returns;
            channelInstance = it->second;
        }

        it->second->onIncoming(*this, id, data, final);

        if (final) {
            LFLOG_TRACE << "Transfer om channel #" << channel
                        << " on connection " << getConnectionId().toString()
                        << " is completed.";
        }
    }
}

// The former JSON dispatcher is retained only in the worktree as migration
// reference. It is unreachable and must not be used by the v3 stream.
#if 0
void Peer::onReceivedJson(const quint64 id, const Peer::mview_t& data)
{
    if (notificationsDisabled_) {
        return;
    }

    QJsonParseError parseError;
    const QJsonDocument json = QJsonDocument::fromJson(data.toByteArray(), &parseError);
    if (json.isNull() || !json.isObject()) {
        LFLOG_ERROR << "Incoming data on " << getConnectionId().toString()
                    << " with id=" << id
                    << " is supposed to be in Json format, but it is not.";
        protocolError(QStringLiteral("Control packet is not a JSON object"));
    }

    const auto object = json.object();
    if (object.value("protocol").toInt(-1) != control_version) {
        protocolError(QStringLiteral("Unsupported control protocol version"));
    }

    const auto type = object.value("type").toString();
    if (type.isEmpty()) {
        protocolError(QStringLiteral("Control packet has no type"));
    }
    if (!capabilitiesReceived_ && type != QStringLiteral("Capabilities")) {
        protocolError(QStringLiteral("Control packet received before capabilities"));
    }

    if (type == QStringLiteral("Capabilities")) {
        if (capabilitiesReceived_) {
            protocolError(QStringLiteral("Duplicate capabilities packet"));
        }
        const auto capabilities = object.value("capabilities");
        if (!capabilities.isArray()
                || !capabilities.toArray().contains(QJsonValue(QStringLiteral("frame-v2")))
                || !capabilities.toArray().contains(QJsonValue(QStringLiteral("file-window-v1")))
                || !capabilities.toArray().contains(QJsonValue(QStringLiteral("keepalive-v1")))
                || object.value("max-frame").toInt(-1) != static_cast<int>(max_frame_bytes)) {
            protocolError(QStringLiteral("Invalid capabilities packet"));
        }
        capabilitiesReceived_ = true;
        return;
    }

    if (applicationAuthorizationPending_) {
        protocolError(QStringLiteral("Application packet received before authorization"));
    }

    if (type == QStringLiteral("KeepAlive")) {
        const auto nonce = QByteArray::fromBase64(
            object.value("nonce").toString().toUtf8());
        const auto padding = QByteArray::fromBase64(
            object.value("padding").toString().toUtf8());
        if (nonce.size() != 16 || padding.size() > 256) {
            protocolError(QStringLiteral("Invalid keepalive packet"));
        }
        return;
    } else if (type == QStringLiteral("AddMe")) {
        PeerAddmeReq req{shared_from_this(), getConnectionId(), id,
                    object.value("nick").toString(),
                    object.value("message").toString(),
                    object.value("address").toString().toUtf8(),
                    getPeerCert()->getB58PubKey()};

        LFLOG_TRACE << "Emitting addmeRequest";
        emit addmeRequest(req);
    } else if (type == QStringLiteral("Ack")) {

        QVariantMap params;
        for(const auto& key : json.object().keys()) {
            static const std::regex irrelevant{"what|status|type|protocol"};
            if (std::regex_search(key.toStdString(), irrelevant)) {
                continue;
            }
            params.insert(key, json.object().value(key).toString());
        }

        LegacyAck ack{shared_from_this(), getConnectionId(), id,
                    object.value("what").toString().toUtf8(),
                    object.value("status").toString().toUtf8(),
                    params};

        if (ack.what.isEmpty() || ack.status.isEmpty()) {
            protocolError(QStringLiteral("Invalid acknowledgement packet"));
        }

        LFLOG_TRACE << "Emitting Ack";
        emit legacyAckSignalRemoved(ack);
    } else if (type == QStringLiteral("Message")) {
        PeerMessage msg{shared_from_this(), getConnectionId(), id,
                    QByteArray::fromBase64(object.value("conversation").toString().toUtf8()),
                    QByteArray::fromBase64(object.value("message-id").toString().toUtf8()),
                    QDateTime::fromString(object.value("date").toString(), Qt::ISODate),
                    object.value("content").toString(),
                    QByteArray::fromBase64(object.value("from").toString().toUtf8()),
                    toEncoding(object.value("encoding").toString()),
                    QByteArray::fromBase64(object.value("signature").toString().toUtf8())};

        if (msg.data.conversation.isEmpty() || msg.data.messageId.isEmpty()) {
            protocolError(QStringLiteral("Invalid message packet"));
        }

        LFLOG_TRACE << "Emitting PeerMessage";
        emit receivedMessage(msg);
    } else if (type == QStringLiteral("IncomingFile")) {
        bool sizeOk = false;
        bool restOk = false;
        const auto size = object.value("size").toString().toLongLong(&sizeOk);
        const auto rest = object.value("rest").toString().toLongLong(&restOk);
        PeerFileOffer msg{shared_from_this(), getConnectionId(), id,
                    QByteArray::fromBase64(object.value("conversation").toString().toUtf8()),
                    QByteArray::fromBase64(object.value("file-id").toString().toUtf8()),
                    object.value("name").toString(),
                    size,
                    rest,
                    object.value("file-type").toString(),
                    QByteArray::fromBase64(object.value("sha256").toString().toUtf8())};

        if (msg.conversation.isEmpty() || msg.fileId.size() != 32
                || msg.name.isEmpty() || msg.name.toUtf8().size() > 255
                || !sizeOk || !restOk || msg.size < 0 || msg.rest < 0
                || msg.rest > msg.size || msg.sha512.size() != crypto_hash_sha256_BYTES) {
            protocolError(QStringLiteral("Invalid file offer packet"));
        }

        LFLOG_TRACE << "Emitting PeerFileOffer";
        emit receivedFileOffer(msg);
    } else if (type == QStringLiteral("SetAvatar")) {
        PeerSetAvatarReq avatar{shared_from_this(), getConnectionId(), id,
                    toQimage(object)};

        LFLOG_TRACE << "Emitting PeerSetAvatarReq";
        emit receivedAvatar(avatar);
    } else if (type == QStringLiteral("UserInfo")) {
        PeerUserInfo uinfo{shared_from_this(), getConnectionId(), id,
                    object.value("nick-name").toString(),
                    QByteArray::fromBase64(object.value("avatar-hash")
                                           .toString().toUtf8())};

        LFLOG_TRACE << "Emitting PeerUserInfo";
        emit receivedUserInfo(uinfo);
    } else {
        protocolError(QStringLiteral("Unknown control packet type"));
    }
}
#endif

void Peer::onReceivedEnvelope(const ds::protocol::Envelope& envelope)
{
    LFLOG_TRACE << "Received v3 envelope on " << getConnectionId().toString()
                << "; body: capabilities=" << envelope.hasCapabilities()
                << ", keepalive=" << envelope.hasKeepAlive()
                << ", connectionRequest=" << envelope.hasConnectionRequest()
                << ", connectionResponse=" << envelope.hasConnectionResponse()
        << ", userInfo=" << envelope.hasUserInfo()
                << ", bioRequest=" << envelope.hasBioRequest()
                << ", bioResponse=" << envelope.hasBioResponse()
                << ", avatarRequest=" << envelope.hasAvatarRequest()
                << ", avatarResponse=" << envelope.hasAvatarResponse()
                << ", avatar=" << envelope.hasAvatar()
                << "; requestId=" << envelope.hasRequestId()
                << ", responseToRequestId=" << envelope.hasResponseToRequestId();
    for (const auto field : envelope.unknownFieldNumbers()) {
        if (field >= 10 && field <= 99) {
            protocolError(QStringLiteral("Unknown v3 Envelope body field %1").arg(field));
        }
    }
    const bool hasBody = envelope.hasCapabilities() || envelope.hasKeepAlive()
            || envelope.hasConnectionRequest() || envelope.hasConnectionResponse()
            || envelope.hasMessage() || envelope.hasFileOffer() || envelope.hasFileChunk()
            || envelope.hasFileAck() || envelope.hasUserInfo() || envelope.hasFileResume()
            || envelope.hasFileResumeResult() || envelope.hasFileCancel()
            || envelope.hasAvatarRequest() || envelope.hasAvatarResponse()
            || envelope.hasAvatar() || envelope.hasMessageReceipt()
            || envelope.hasFileOfferResponse() || envelope.hasBioRequest()
            || envelope.hasBioResponse();
    if (envelope.protocolVersion() != codec::protocolVersion || !hasBody) {
        protocolError(QStringLiteral("Invalid v3 envelope"));
    }
    if (!capabilitiesReceived_ && !envelope.hasCapabilities()) {
        protocolError(QStringLiteral("Envelope received before capabilities"));
    }
    if (envelope.hasCapabilities()) {
        if (capabilitiesReceived_ || !codec::validateCapabilities(envelope)) {
            protocolError(QStringLiteral("Invalid or duplicate capabilities"));
        }
        capabilitiesReceived_ = true;
        peerMaxRecordBytes_ = envelope.capabilities().maxRecordBytes();
        return;
    }
    if ((envelope.hasRequestId() && !envelope.hasConnectionRequest()
            && !envelope.hasAvatarRequest() && !envelope.hasBioRequest())
            && !envelope.hasFileResume()) {
        protocolError(QStringLiteral("Correlation does not match v3 request body"));
    }
    if (envelope.hasResponseToRequestId()
            && !envelope.hasConnectionResponse()
            && !envelope.hasAvatarResponse()
            && !envelope.hasFileResumeResult() && !envelope.hasBioResponse()) {
        protocolError(QStringLiteral("Correlation does not match v3 envelope body"));
    }
    if (applicationAuthorizationPending_) {
        protocolError(QStringLiteral("Application packet received before authorization"));
    }
    if (envelope.hasKeepAlive()) {
        return;
    }
    quint64 requestId = 0;
    if (envelope.hasRequestId()) {
        requestId = envelope.requestId();
    } else if (envelope.hasResponseToRequestId()) {
        requestId = envelope.responseToRequestId();
    }
    if (requestId == 0 && (envelope.hasConnectionRequest() || envelope.hasFileResume())) {
        protocolError(QStringLiteral("Request-bearing v3 object has no correlation id"));
    }
    if (envelope.hasConnectionRequest()) {
        const auto& request = envelope.connectionRequest();
        if (!envelope.hasRequestId() || requestId == 0) {
            protocolError(QStringLiteral("Connection request requires request_id"));
        }
        if (!request.hasContactEndpoint() || !request.contactEndpoint().hasOnionAddress()) {
            protocolError(QStringLiteral("Connection request has no supported endpoint"));
        }
        PeerAddmeReq addme{shared_from_this(), getConnectionId(), requestId,
                           request.nickname(), request.message(),
                           request.contactEndpoint().onionAddress().toUtf8(),
                           getPeerCert() ? getPeerCert()->getB58PubKey() : QByteArray{},
                           request.hasAvatarHash() ? request.avatarHash() : QByteArray{}};
        if (!addme.avatarHash.isEmpty() && addme.avatarHash.size() != crypto_hash_sha256_BYTES) {
            protocolError(QStringLiteral("Invalid connection-request avatar hash"));
        }
        advertisedAvatarHash_ = addme.avatarHash;
        emit addmeRequest(addme);
        return;
    }
    if (envelope.hasAvatarRequest()) {
        if (!envelope.hasRequestId() || requestId == 0) {
            protocolError(QStringLiteral("Avatar request requires request_id"));
        }
        core::recordPeerEvent(*this, core::EventJournal::AvatarRequest, core::EventJournal::Incoming);
        emit receivedAvatarRequest(PeerReq{shared_from_this(), getConnectionId(), requestId});
        return;
    }
    if (envelope.hasMessage()) {
        const auto& message = envelope.message();
        const auto composedMinutes = message.hasComposedAt()
                ? static_cast<qint64>(message.composedAt().unixMinutes()) : qint64{0};
        const auto contentBytes = message.content().toUtf8();
        const auto localIdentity = DsEngine::instance().getIdentityManager()
                ? DsEngine::instance().getIdentityManager()->identityFromUuid(getIdentityId())
                : nullptr;
        if (!message.hasConversationId() || message.messageId().isEmpty()
                || message.messageId().size() != 32
                || message.conversationId().size() != 32
                || message.senderId().size() != crypto_sign_PUBLICKEYBYTES
                || message.signature().size() != crypto_sign_BYTES
                || !message.hasComposedAt()
                || message.attachments().size() > 16
                || !validMessageTimestamp(composedMinutes)
                || contentBytes.size() > 65'536
                || (message.encoding() != ds::protocol::EncodingGadget::Encoding::UTF8
                    && message.encoding() != ds::protocol::EncodingGadget::Encoding::US_ASCII)) {
            protocolError(QStringLiteral("Invalid v3 message"));
        }
        if (message.encoding() == ds::protocol::EncodingGadget::Encoding::US_ASCII
                && !validAsciiMessage(contentBytes)) {
            protocolError(QStringLiteral("Message contains invalid US-ASCII content"));
        }
        if (message.encoding() == ds::protocol::EncodingGadget::Encoding::UTF8
                && localIdentity && !localIdentity->getAllowUtf8()) {
            protocolError(QStringLiteral("Message uses locally disallowed UTF-8"));
        }
        const auto encoding = message.encoding() == ds::protocol::EncodingGadget::Encoding::UTF8
                ? Message::UTF8 : Message::US_ACSII;
        core::MessageData signedData;
        signedData.conversation = message.conversationId();
        signedData.messageId = message.messageId();
        signedData.composedTime = QDateTime::fromSecsSinceEpoch(
            composedMinutes * 60, QTimeZone::UTC);
        signedData.content = message.content();
        signedData.sender = message.senderId();
        signedData.encoding = encoding;
        signedData.signature = message.signature();
        for (const auto& entry : message.attachments()) {
            if (!entry.hasFileOffer() || !entry.fileOffer().hasMessageId()
                || entry.fileOffer().messageId() != message.messageId())
                protocolError(QStringLiteral("Invalid attachment source"));
            const auto& offer = entry.fileOffer();
            core::MessageAttachment attachment{offer.fileId(), offer.name(), offer.size(),
                offer.sha256(), offer.mimeType(), {}};
            if (entry.hasThumbnailForAttachmentIndex())
                attachment.thumbnail_for = entry.thumbnailForAttachmentIndex();
            signedData.attachments.push_back(std::move(attachment));
        }
        const auto senderCert = crypto::DsCert::createFromPubkey(message.senderId());
        core::Message signedMessage{*this, signedData, Message::INCOMING, -1};
        if (!signedMessage.validate(*senderCert)) {
            protocolError(QStringLiteral("Invalid v3 message authorship signature"));
        }
        PeerMessage incoming{shared_from_this(), getConnectionId(), requestId,
                             message.conversationId(), message.messageId(),
                             QDateTime::fromSecsSinceEpoch(composedMinutes * 60,
                                                            QTimeZone::UTC),
                             message.content(), message.senderId(), encoding,
                             message.signature()};
        incoming.data.attachments = std::move(signedData.attachments);
        emit receivedMessage(incoming);
        return;
    }
    if (envelope.hasFileOffer()) {
        const auto& offer = envelope.fileOffer();
        if (!offer.hasConversationId() || offer.fileId().size() != 16
                || offer.conversationId().size() != 32
                || offer.sha256().size() != crypto_hash_sha256_BYTES
                || offer.name().isEmpty() || offer.name().toUtf8().size() > 255
                || !validMimeType(offer.mimeType())
                || offer.size() > ResourceLimits::hardAcceptedFileBytes
                || offer.size() > static_cast<quint64>(std::numeric_limits<qlonglong>::max())) {
            protocolError(QStringLiteral("Invalid v3 file offer"));
        }
        PeerFileOffer incoming{shared_from_this(), getConnectionId(), requestId,
                               offer.conversationId(), offer.fileId(), offer.name(),
                               static_cast<qlonglong>(offer.size()),
                               offer.mimeType(), offer.sha256()};
        emit receivedFileOffer(incoming);
        return;
    }
    if (envelope.hasFileResume()) {
        const auto& resume = envelope.fileResume();
        if (!envelope.hasRequestId() || resume.fileId().size() != 16
                || resume.sha256().size() != crypto_hash_sha256_BYTES
                || resume.fileSize() > ResourceLimits::hardAcceptedFileBytes
                || resume.fileSize() > static_cast<quint64>(std::numeric_limits<qlonglong>::max())
                || resume.nextOffset() > resume.fileSize()
                || resume.nextOffset() % file_chunk_bytes != 0) {
            protocolError(QStringLiteral("Invalid v3 file resume request"));
        }
        emit receivedFileResume(PeerFileResume{
            shared_from_this(), getConnectionId(), requestId, resume.fileId(),
            resume.nextOffset(), resume.sha256(), resume.fileSize()});
        return;
    }
    if (envelope.hasFileChunk()) {
        const auto& chunk = envelope.fileChunk();
        if (chunk.fileId().size() != 16 || chunk.data().size() > static_cast<qsizetype>(file_chunk_bytes)) {
            protocolError(QStringLiteral("Invalid v3 file chunk"));
        }
        const auto file = DsEngine::instance().getFileManager()->getFileFromId(
            chunk.fileId(), File::INCOMING);
        if (!file || !file->getContact() || !getPeerCert()
                || file->getContact()->getCert()->getHash().toByteArray()
                    != getPeerCert()->getHash().toByteArray()
                || file->getChannel() == 0) {
            protocolError(QStringLiteral("File chunk has no active transfer"));
        }
        auto channel = inChannels_.find(file->getChannel());
        if (channel == inChannels_.end()) {
            protocolError(QStringLiteral("File chunk has no active channel"));
        }
        quint64 sequence = qToBigEndian(chunk.sequence());
        quint64 offset = qToBigEndian(chunk.offset());
        QByteArray framed(static_cast<int>(file_chunk_header_bytes + chunk.data().size()),
                          Qt::Uninitialized);
        memcpy(framed.data(), &sequence, sizeof(sequence));
        memcpy(framed.data() + sizeof(sequence), &offset, sizeof(offset));
        memcpy(framed.data() + file_chunk_header_bytes, chunk.data().constData(),
               static_cast<size_t>(chunk.data().size()));
        channel->second->onIncoming(*this, requestId,
                                    mview_t{reinterpret_cast<uint8_t *>(framed.data()),
                                            static_cast<size_t>(framed.size())},
                                    chunk.final());
        return;
    }
    if (envelope.hasUserInfo()) {
        const auto& info = envelope.userInfo();
        if (!info.hasSecurityPrefs()
                || (info.hasAvatarHash() && !info.avatarHash().isEmpty()
                    && info.avatarHash().size() != crypto_hash_sha256_BYTES)
                || (!info.bioHash().isEmpty()
                    && info.bioHash().size() != crypto_hash_sha256_BYTES)
                || !validMimePreferences(info.securityPrefs().allowMimeTypes())) {
            protocolError(QStringLiteral("Invalid v3 UserInfo snapshot"));
        }
        PeerUserInfo incoming{shared_from_this(), getConnectionId(), requestId,
                              core::UserInfo{info.nickname(),
                                             info.hasAvatarHash() ? info.avatarHash() : QByteArray{},
                                             info.securityPrefs().allowUtf8(),
                                             info.securityPrefs().allowFiles(),
                                             info.securityPrefs().allowMimeTypes(),
                                             info.securityPrefs().allowReactions(),
                                             info.securityPrefs().publicBio(),
                                             info.securityPrefs().publicAvatar(),
                                             info.bioHash()}};
        if (info.hasBio() && (info.bio().bioText().size() > 1000
                              || info.bio().bioText().toUtf8().size() > 4096)) {
            protocolError(QStringLiteral("Bio exceeds the v3 size limit"));
        }
        core::recordPeerEvent(*this, core::EventJournal::UserInfo, core::EventJournal::Incoming);
        emit receivedUserInfo(incoming);
        return;
    }
    if (envelope.hasAvatarResponse()) {
        if (!envelope.hasResponseToRequestId() || requestId == 0
                || !consumeRequest(requestId, RequestType::AvatarResponse)) {
            protocolError(QStringLiteral("Unsolicited, duplicate, or mismatched avatar response"));
        }
        QImage image;
        if (envelope.avatarResponse().hasAvatar()) {
            const auto& avatar = envelope.avatarResponse().avatar();
            const auto width = avatar.width();
            const auto height = avatar.height();
            if ((width == 0) != (height == 0) || width > 512 || height > 512
                    || static_cast<quint64>(width) * height > 262144) {
                protocolError(QStringLiteral("Invalid avatar response dimensions"));
            }
            const auto pixels = static_cast<qsizetype>(width) * height;
            if ((width == 0 && (!avatar.red().isEmpty() || !avatar.green().isEmpty()
                                || !avatar.blue().isEmpty()))
                    || (width != 0 && (avatar.red().size() != pixels
                                       || avatar.green().size() != pixels
                                       || avatar.blue().size() != pixels))) {
                protocolError(QStringLiteral("Invalid avatar response data"));
            }
            image = QImage(static_cast<int>(width), static_cast<int>(height), QImage::Format_RGB888);
            for (quint32 y = 0; y < height; ++y) {
                auto *line = image.scanLine(static_cast<int>(y));
                for (quint32 x = 0; x < width; ++x) {
                    const auto index = static_cast<qsizetype>(y) * width + x;
                    line[3 * x] = static_cast<unsigned char>(avatar.red().at(index));
                    line[3 * x + 1] = static_cast<unsigned char>(avatar.green().at(index));
                    line[3 * x + 2] = static_cast<unsigned char>(avatar.blue().at(index));
                }
            }
        }
        core::recordPeerEvent(*this, core::EventJournal::AvatarTransfer, core::EventJournal::Incoming);
        emit receivedAvatarResponse(PeerSetAvatarReq{shared_from_this(), getConnectionId(),
                                                      requestId, std::move(image)});
        return;
    }
    if (envelope.hasFileAck()) {
        const auto& ack = envelope.fileAck();
        if (ack.fileId().size() != 16) {
            protocolError(QStringLiteral("Invalid v3 file acknowledgement"));
        }
        handleFileAck(ack.fileId(), ack.nextSequence(), ack.nextOffset(), ack.final());
        emit receivedFileAck(PeerFileAck{shared_from_this(), getConnectionId(), requestId,
                                         ack.fileId(), ack.nextSequence(), ack.nextOffset(),
                                         ack.final()});
        return;
    }
    if (envelope.hasMessageReceipt()) {
        if (envelope.messageReceipt().messageId().size() != 32) {
            protocolError(QStringLiteral("Invalid v3 message receipt"));
        }
        emit receivedMessageReceipt(PeerMessageReceipt{
            shared_from_this(), getConnectionId(), requestId,
            envelope.messageReceipt().messageId()});
        return;
    }
    if (envelope.hasFileOfferResponse()) {
        const auto& response = envelope.fileOfferResponse();
        if (response.fileId().size() != 16
                || (response.status() != ds::protocol::FileOfferResponse::Status::FILE_OFFER_ACCEPTED
                    && response.status() != ds::protocol::FileOfferResponse::Status::FILE_OFFER_REJECTED)) {
            protocolError(QStringLiteral("Invalid v3 file-offer response"));
        }
        auto file = DsEngine::instance().getFileManager()->getFileFromId(
            response.fileId(), File::OUTGOING);
        Q_UNUSED(file)
        emit receivedFileOfferResponse(PeerFileOfferResponse{
            shared_from_this(), getConnectionId(), requestId, response.fileId(),
            response.status() == ds::protocol::FileOfferResponse::Status::FILE_OFFER_ACCEPTED
                ? core::FileOfferResponseStatus::Accepted
                : core::FileOfferResponseStatus::Rejected});
        return;
    }
    if (envelope.hasFileResumeResult()) {
        const auto& result = envelope.fileResumeResult();
        if (!envelope.hasResponseToRequestId() || requestId == 0
                || result.fileId().size() != 16
                || (result.status() != ds::protocol::FileResumeResult::Status::RESUME_ACCEPTED
                    && result.status() != ds::protocol::FileResumeResult::Status::RESUME_REJECTED
                    && result.status() != ds::protocol::FileResumeResult::Status::RESUME_CANCELLED)
                || result.nextOffset() > ResourceLimits::hardAcceptedFileBytes
                || result.nextOffset() % file_chunk_bytes != 0) {
            protocolError(QStringLiteral("Invalid v3 file resume result"));
        }
        if (!consumeRequest(requestId, RequestType::FileResumeResult)) {
            protocolError(QStringLiteral("Unsolicited, duplicate, expired, or mismatched file resume result"));
        }
        FileResumeStatus status;
        switch (result.status()) {
        case ds::protocol::FileResumeResult::Status::RESUME_ACCEPTED:
            status = FileResumeStatus::Accepted;
            break;
        case ds::protocol::FileResumeResult::Status::RESUME_REJECTED:
            status = FileResumeStatus::Rejected;
            break;
        case ds::protocol::FileResumeResult::Status::RESUME_CANCELLED:
            status = FileResumeStatus::Cancelled;
            break;
        default:
            protocolError(QStringLiteral("Unknown file resume result status"));
        }
        if (result.error() != ds::protocol::FileErrorGadget::FileError::FILE_ERROR_UNSPECIFIED
                && result.error() != ds::protocol::FileErrorGadget::FileError::SOURCE_CHANGED
                && result.error() != ds::protocol::FileErrorGadget::FileError::USER_ACTION) {
            protocolError(QStringLiteral("Unknown file resume result error"));
        }
        if ((result.status() == ds::protocol::FileResumeResult::Status::RESUME_CANCELLED
             && result.error() == ds::protocol::FileErrorGadget::FileError::FILE_ERROR_UNSPECIFIED)
                || (result.status() != ds::protocol::FileResumeResult::Status::RESUME_CANCELLED
                    && result.error() != ds::protocol::FileErrorGadget::FileError::FILE_ERROR_UNSPECIFIED)) {
            protocolError(QStringLiteral("Invalid file resume result error"));
        }
        emit receivedFileResumeResult(PeerFileResumeResult{
            shared_from_this(), getConnectionId(), requestId, result.fileId(), status,
            result.nextOffset(), result.error() == ds::protocol::FileErrorGadget::FileError::SOURCE_CHANGED
                ? FileCancelReason::SourceChanged : FileCancelReason::UserAction});
        return;
    }
    if (envelope.hasConnectionResponse()) {
        if (!envelope.hasResponseToRequestId() || requestId == 0) {
            protocolError(QStringLiteral("Connection response lacks correlation"));
        }
        if (!consumeRequest(requestId, RequestType::ConnectionResponse)) {
            protocolError(QStringLiteral("Unsolicited, duplicate, expired, or mismatched connection response"));
        }
        const auto status = envelope.connectionResponse().status();
        core::ConnectionResponseStatus responseStatus;
        switch (status) {
        case ds::protocol::ConnectionResponse::Status::PENDING:
            responseStatus = core::ConnectionResponseStatus::Pending;
            break;
        case ds::protocol::ConnectionResponse::Status::ACCEPTED:
            responseStatus = core::ConnectionResponseStatus::Accepted;
            break;
        case ds::protocol::ConnectionResponse::Status::REJECTED:
            responseStatus = core::ConnectionResponseStatus::Rejected;
            break;
        case ds::protocol::ConnectionResponse::Status::BLOCKED:
            responseStatus = core::ConnectionResponseStatus::Blocked;
            break;
        default:
            protocolError(QStringLiteral("Unknown connection response status"));
        }
        emit receivedConnectionResponse(PeerConnectionResponse{
            shared_from_this(), getConnectionId(), requestId, responseStatus,
            envelope.connectionResponse().message()});
        return;
    }
    if (envelope.hasFileCancel()) {
        const auto& cancel = envelope.fileCancel();
        if (cancel.fileId().size() != 16
                || (cancel.reason() != ds::protocol::FileErrorGadget::FileError::SOURCE_CHANGED
                    && cancel.reason() != ds::protocol::FileErrorGadget::FileError::USER_ACTION)) {
            protocolError(QStringLiteral("Invalid v3 file cancellation"));
        }
        emit receivedFileCancel(PeerFileCancel{
            shared_from_this(), getConnectionId(), requestId, cancel.fileId(),
            cancel.reason() == ds::protocol::FileErrorGadget::FileError::SOURCE_CHANGED
                ? core::FileCancelReason::SourceChanged
                : core::FileCancelReason::UserAction});
        return;
    }
    if (envelope.hasAvatar()) {
        const auto& avatar = envelope.avatar();
        const auto width = avatar.width();
        const auto height = avatar.height();
        if ((width == 0) != (height == 0) || width > 512 || height > 512
                || static_cast<quint64>(width) * height > 262144) {
            protocolError(QStringLiteral("Invalid v3 avatar dimensions"));
        }
        const auto pixels = static_cast<qsizetype>(width) * static_cast<qsizetype>(height);
        if ((width == 0 && (!avatar.red().isEmpty() || !avatar.green().isEmpty()
                            || !avatar.blue().isEmpty()))
                || (width != 0 && (avatar.red().size() != pixels
                                   || avatar.green().size() != pixels
                                   || avatar.blue().size() != pixels))
                || avatar.red().size() + avatar.green().size() + avatar.blue().size() > 786432) {
            protocolError(QStringLiteral("Invalid v3 avatar data"));
        }
        QImage image(static_cast<int>(width), static_cast<int>(height), QImage::Format_RGB888);
        for (quint32 y = 0; y < height; ++y) {
            auto *line = image.scanLine(static_cast<int>(y));
            for (quint32 x = 0; x < width; ++x) {
                const auto index = static_cast<qsizetype>(y) * width + x;
                line[3 * x] = static_cast<unsigned char>(avatar.red().at(index));
                line[3 * x + 1] = static_cast<unsigned char>(avatar.green().at(index));
                line[3 * x + 2] = static_cast<unsigned char>(avatar.blue().at(index));
            }
        }
        PeerSetAvatarReq incoming{shared_from_this(), getConnectionId(), requestId,
                                  std::move(image)};
        core::recordPeerEvent(*this, core::EventJournal::AvatarTransfer, core::EventJournal::Incoming);
        emit receivedAvatar(incoming);
        return;
    }
    if (envelope.hasBioRequest()) {
        if (!envelope.hasRequestId() || requestId == 0) {
            protocolError(QStringLiteral("Bio request lacks correlation"));
        }
        core::recordPeerEvent(*this, core::EventJournal::BioRequest, core::EventJournal::Incoming);
        emit receivedBioRequest(PeerReq{shared_from_this(), getConnectionId(), requestId});
        return;
    }
    if (envelope.hasBioResponse()) {
        if (!envelope.hasResponseToRequestId() || requestId == 0
                || !consumeRequest(requestId, RequestType::BioResponse)) {
            protocolError(QStringLiteral("Unsolicited, duplicate, or mismatched bio response"));
        }
        const auto& response = envelope.bioResponse();
        if (response.hasBio() && !core::bio::withinLimits(
                    core::bio::sanitize(response.bio().bioText()))) {
            protocolError(QStringLiteral("Bio exceeds the v3 size limit"));
        }
        core::recordPeerEvent(*this, core::EventJournal::BioResponse, core::EventJournal::Incoming);
        emit receivedBio(PeerBio{shared_from_this(), getConnectionId(), requestId,
                                 response.hasBio() ? response.bio().bioText() : QString{}});
        return;
    }
    protocolError(QStringLiteral("Unsupported v3 envelope body"));
}

void Peer::sendCapabilities()
{
    send(codec::makeCapabilities({QStringLiteral("file-chunks-v3"),
                                  QStringLiteral("keepalive-v3")},
                                 static_cast<quint32>(max_record_bytes)));
}

[[noreturn]] void Peer::protocolError(const QString& reason)
{
    core::recordPeerEvent(*this, core::EventJournal::ProtocolRejection, core::EventJournal::Incoming, core::EventJournal::Rejected);
    LFLOG_ERROR << "Protocol error on connection " << getConnectionId().toString()
                << ": " << reason;
    close();
    throw Error(reason);
}

void Peer::onCloseLater()
{
    LFLOG_TRACE << "Closing peer connection " << getConnectionId().toString()
                << "; socket open: " << (connection_->isOpen() ? "yes" : "no")
                << "; encrypted input state: " << static_cast<int>(inState_);
    if (connection_->isOpen()) {
        connection_->close();
    }

    if (!notificationsDisabled_) {
        emit disconnectedFromPeer(shared_from_this());
    }
}

void Peer::enableEncryptedStream()
{
    if (inState_ == InState::CLOSING) {
        return;
    }

    if (inState_ != InState::DISABLED) {
        throw runtime_error("Encrypted stream is already enabled");
    }
    resetIdleTimer();
    scheduleKeepAlive();
    wantRecordSize();
}

void Peer::resetIdleTimer()
{
    // Keepalive scheduling is independent of ordinary traffic so its
    // interval remains randomized.
}

void Peer::scheduleKeepAlive()
{
    if (inState_ != InState::CLOSING && inState_ != InState::DISABLED) {
        constexpr int minimumDelayMs = 90 * 1000;
        constexpr int maximumDelayMs = 240 * 1000;
        keepAliveTimer_.start(QRandomGenerator::system()->bounded(
            minimumDelayMs, maximumDelayMs + 1));
    }
}

void Peer::sendKeepAlive()
{
    if (inState_ == InState::CLOSING || !connection_->isOpen()) {
        return;
    }

    try {
        ds::protocol::Envelope envelope;
        envelope.setProtocolVersion(codec::protocolVersion);
        envelope.setKeepAlive(ds::protocol::KeepAlive{});
        send(envelope);
    } catch (const std::exception& ex) {
        LFLOG_WARN << "Failed to send keepalive: " << ex.what();
    }
    scheduleKeepAlive();
}

void Peer::wantRecordSize()
{
    if (inState_ == InState::CLOSING) {
        return;
    }

    LFLOG_TRACE << "Want encrypted v3 record length on " << connection_->getUuid().toString();
    inState_ = InState::RECORD_SIZE;
    connection_->wantBytes(sizeof(quint32));
}

void Peer::wantRecordData(const size_t bytes)
{
    if (inState_ == InState::CLOSING) {
        return;
    }

    if (!codec::validRecordLength(static_cast<quint32>(bytes))) {
        throw runtime_error("Invalid encrypted frame length");
    }

    LFLOG_TRACE << "Want " << bytes << " encrypted record bytes on " << connection_->getUuid().toString();
    pendingRecordBytes_ = static_cast<quint32>(bytes);
    inState_ = InState::RECORD_DATA;
    connection_->wantBytes(bytes);
}

// Incoming, stream-encrypted data
void Peer::processStream(const Peer::data_t &ciphertext)
{
    if (inboundFinalReceived_) {
        protocolError(QStringLiteral("Encrypted record received after TAG_FINAL"));
    }
    if (inState_ == InState::CLOSING) {
        return;
    }

    if (inState_ == InState::RECORD_SIZE) {
        if (ciphertext.size() != sizeof(quint32)) {
            throw runtime_error("Invalid v3 record length prefix");
        }
        quint32 length = {};
        memcpy(&length, ciphertext.cdata(), sizeof(length));
        length = qFromBigEndian(length);
        LFLOG_TRACE << "Received encrypted v3 record length " << length
                    << " on " << getConnectionId().toString();
        if (!codec::validRecordLength(length)) {
            throw runtime_error("Invalid v3 record length");
        }
        wantRecordData(length);
        return;
    }
    if (inState_ == InState::RECORD_DATA) {
        if (ciphertext.size() != pendingRecordBytes_) {
            throw runtime_error("Invalid v3 record body length");
        }
        QByteArray plaintext(pendingRecordBytes_ - crypt_bytes, Qt::Uninitialized);
        mview_t plainView{reinterpret_cast<uint8_t *>(plaintext.data()),
                          static_cast<size_t>(plaintext.size())};
        bool final = {};
        decrypt(plainView, ciphertext, final);
        LFLOG_TRACE << "Authenticated encrypted v3 record on "
                    << getConnectionId().toString();
        ds::protocol::TransportEnvelope transport;
        if (!codec::deserialize(QByteArrayView{plaintext}, transport)
                || transport.payload().isEmpty()
                || transport.payload().size() > static_cast<qsizetype>(max_payload_bytes)
                || transport.padding().size() > 256) {
            throw runtime_error("Invalid v3 transport envelope");
        }
        const auto *local_identity = DsEngine::instance().getIdentityManager()->identityFromUuid(getIdentityId());
        if (local_identity && !local_identity->getAllowUtf8()
            && !core::text_policy::permittedAsciiEnvelope(QByteArrayView{transport.payload()})) {
            core::recordPeerEvent(*this, core::EventJournal::PolicyRejection, core::EventJournal::Incoming, core::EventJournal::Rejected);
            throw runtime_error("Application text violates the local ASCII policy");
        }
        ds::protocol::Envelope envelope;
        if (!codec::deserialize(QByteArrayView{transport.payload()}, envelope)) {
            throw runtime_error("Invalid v3 application envelope");
        }
        onReceivedEnvelope(envelope);
        if (final) {
            inboundFinalReceived_ = true;
            inState_ = InState::CLOSING;
            connection_->close();
            return;
        }
        wantRecordSize();
        return;
    }
#if 0
    {

        static const QByteArray binary = {"[binary]"};
        if (ciphertext.size() < crypt_bytes + frame_header_bytes
                || ciphertext.size() > crypt_bytes + max_frame_bytes) {
            throw runtime_error("Invalid encrypted frame size");
        }
        std::vector<uint8_t> buffer(ciphertext.size() - crypt_bytes);
        mview_t buffer_view{buffer};
        mview_t version{buffer.data(), 1};
        mview_t channel{version.end(), 4};
        mview_t id{channel.end(), 8};

        const int payload_size = static_cast<int>(buffer.size())
                - static_cast<int>(version.size())
                - static_cast<int>(channel.size())
                - static_cast<int>(id.size());

        if (payload_size < 0) {
            throw runtime_error("Payload size underflow");
        }

        mview_t payload{id.end(), static_cast<size_t>(payload_size)};

        if (buffer.size() != version.size() + channel.size()
                + id.size() + payload.size()) {
            throw runtime_error("Invalid encrypted frame layout");
        }

        decrypt(buffer_view, ciphertext, final);

        if (version.at(0) != static_cast<char>(frame_version)) {
            LFLOG_WARN << "Unknown chunk version" << static_cast<unsigned int>(version.at(0));
            throw runtime_error("Unknown chunk version");
        }

        const auto channel_id = qFromBigEndian(bytesToValue<quint32>(channel));
        const auto chunk_id = qFromBigEndian(bytesToValue<quint64>(id));

        LFLOG_TRACE << "Received chunk on "
                    << connection_->getUuid().toString()
                    << ", size=" << payload.size()
                    << ", channel=" << channel_id
                    << ", id=" << chunk_id
                    << ", payload=" << (channel_id ? binary : "[control]");

        try {
            onReceivedData(channel_id, chunk_id, payload, final);
        } catch (const std::exception& ex) {
            LFLOG_ERROR << "Caught exception while processing incoming message on connection "
                        << getConnectionId().toString()
                        << " :"
                        << ex.what();
            close();
            return;
        }

        wantRecordSize();
    } else {
        throw runtime_error("Unexpected InState");
    }
#endif
    throw runtime_error("Unexpected InState");
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

void Peer::decrypt(Peer::mview_t &data, const Peer::mview_t &ciphertext,  bool& final)
{
    if (ciphertext.size() < crypt_bytes
            || data.size() + crypt_bytes != ciphertext.size()) {
        throw runtime_error("Invalid encrypted stream buffer size");
    }
    unsigned char tag = {};
    if (crypto_secretstream_xchacha20poly1305_pull(&stateIn,
                                                   data.data(),
                                                   nullptr,
                                                   &tag,
                                                   ciphertext.cdata(),
                                                   ciphertext.size(),
                                                   nullptr, 0) != 0) {
        LFLOG_WARN << "Secretstream authentication failed on connection "
                   << getConnectionId().toString();
        throw runtime_error("Secretstream authentication failed");
    }

    switch (tag) {
    case crypto_secretstream_xchacha20poly1305_TAG_MESSAGE:
    case crypto_secretstream_xchacha20poly1305_TAG_REKEY:
        final = false;
        break;
    case crypto_secretstream_xchacha20poly1305_TAG_FINAL:
        final = true;
        break;
    case crypto_secretstream_xchacha20poly1305_TAG_PUSH:
        throw runtime_error("Unsupported secretstream TAG_PUSH");
    default:
        throw runtime_error("Unknown secretstream tag");
    }
}

quint32 Peer::createChannel(const File &file)
{
    quint32 channelId = 0;
    auto filePtr = core::DsEngine::instance().getFileManager()->getFile(file.getId());
    Channel::ptr_t ch;
    if (file.getDirection() == File::INCOMING) {
        ch = make_shared<IncomingFileChannel>(filePtr);
        if (inChannels_.find(nextInchannel_) != inChannels_.end()) {
            throw Error("Incoming channel id collision");
        }
        channelId = nextInchannel_;
        inChannels_[channelId] = ch;
        ++nextInchannel_;
    } else {
        channelId = file.getChannel();
        if (channelId == 0) {
            channelId = nextInchannel_++;
        }
        if (outChannels_.find(channelId) != outChannels_.end()) {
            throw Error("Invalid or duplicate outgoing channel id");
        }
        ch = make_shared<OutgoingFileChannel>(filePtr);
        outChannels_[channelId] = ch;
    }

    const auto direction = file.getDirection();
    auto fileCPtr = filePtr.get();
    connect(fileCPtr, &File::stateChanged,
            this, [this, channelId, direction, fileCPtr]() {

        if (fileCPtr->getState() != File::FS_TRANSFERRING
                && !(direction == File::INCOMING
                     && fileCPtr->getState() == File::FS_HASHING)) {
            emit removeTransfer(direction, channelId);
            }
    });
    connect(fileCPtr, &File::transferDone,
            this, [this, channelId, direction](File *, bool success) {
        if (direction != File::INCOMING) {
            return;
        }
        const auto it = inChannels_.find(channelId);
        if (it != inChannels_.end()) {
            it->second->onTransferDone(*this, success);
        }
    });

    return channelId;
}

uint64_t Peer::startReceive(File &file)
{
    QString reason;
    auto *fileManager = core::DsEngine::instance().getFileManager();
    if (!fileManager->canAcceptIncoming(file, reason)) {
        sendFileOfferResponse(file.getFileId(), core::FileOfferResponseStatus::Rejected);
        file.setState(File::FS_FAILED);
        throw Error(QStringLiteral("Incoming transfer no longer satisfies local limits: ")
                    + reason);
    }
    prepareReceive(fileManager->getFile(file.getId()), std::static_pointer_cast<Peer>(shared_from_this()));
    return 0;
}
QCoro::Task<void> Peer::prepareReceive(File::ptr_t file, std::shared_ptr<Peer> self) {
    // Reserve the transfer state before suspension so duplicate offers cannot start another writer.
    if (file->getState() == File::FS_TRANSFERRING || file->getState() == File::FS_HASHING) co_return;
    file->setState(File::FS_TRANSFERRING);
    auto offset = co_await file->prepareIncoming();
    if (!offset || !isConnected() || file->getState() != File::FS_TRANSFERRING) {
        if (file->getState() == File::FS_TRANSFERRING) file->transferFailed(QObject::tr("Unable to prepare incoming storage."), File::FS_FAILED);
        co_await file->suspendIncoming();
        co_return;
    }
    try {
        auto channel = createChannel(*file);
        file->setChannel(channel);
        sendFileOfferResponse(file->getFileId(), FileOfferResponseStatus::Accepted);
    } catch (...) { file->transferFailed(QObject::tr("Unable to start incoming transfer."), File::FS_FAILED); }
}

uint64_t Peer::startSend(File &file)
{
    auto channelId = createChannel(file);
    // The scheduler uses the file's channel id for all subsequent chunks.
    // Keep it in sync with the channel created above before sending the first
    // chunk.
    file.setChannel(channelId);
    file.setState(File::FS_TRANSFERRING);
    return outChannels_.at(channelId)->onOutgoing(*this);
}

void Peer::useConnection(ConnectionSocket *cc)
{
    Q_UNUSED(cc);
    connect(connection_.get(), &ConnectionSocket::connected,
            this, [this]() {

        LFLOG_DEBUG << "Peer " << getConnectionId().toString()
                    << " is connected";
    });

    connect(connection_.get(), &ConnectionSocket::disconnected,
            this, [this]() {

        LFLOG_DEBUG << "Peer " << getConnectionId().toString()
                    << " is disconnected";

        if (!notificationsDisabled_) {
            emit disconnectedFromPeer(shared_from_this());
        }
    });

    connect(connection_.get(), &ConnectionSocket::outputBufferEmptied,
            this, [this]() {

        if (!notificationsDisabled_) {
            emit outputBufferEmptied();
        }
    }, Qt::QueuedConnection);
}

QUuid Peer::getConnectionId() const
{
    return uuid_;
}

crypto::DsCert::ptr_t Peer::getPeerCert() const noexcept
{
    return connectionData_.contactsCert;
}

void Peer::close()
{
   LFLOG_TRACE << "Peer close requested for connection "
               << getConnectionId().toString()
               << "; encrypted input state: " << static_cast<int>(inState_);
   inState_ = InState::CLOSING;
   emit closeLater();
}

QUuid Peer::getIdentityId() const noexcept
{
    return connectionData_.service;
}

uint64_t Peer::sendConnectionResponse(const quint64 requestId,
                                       const core::ConnectionResponseStatus status,
                                       const QString& message)
{
    if (requestId == 0) {
        throw Error("Connection response requires a request id");
    }
    ds::protocol::ConnectionResponse response;
    switch (status) {
    case core::ConnectionResponseStatus::Pending:
        response.setStatus(ds::protocol::ConnectionResponse::Status::PENDING);
        break;
    case core::ConnectionResponseStatus::Accepted:
        response.setStatus(ds::protocol::ConnectionResponse::Status::ACCEPTED);
        break;
    case core::ConnectionResponseStatus::Rejected:
        response.setStatus(ds::protocol::ConnectionResponse::Status::REJECTED);
        break;
    case core::ConnectionResponseStatus::Blocked:
        response.setStatus(ds::protocol::ConnectionResponse::Status::BLOCKED);
        break;
    }
    response.setMessage(message);
    ds::protocol::Envelope envelope;
    envelope.setProtocolVersion(codec::protocolVersion);
    envelope.setResponseToRequestId(requestId);
    envelope.setConnectionResponse(std::move(response));
    return send(envelope);
}

uint64_t Peer::sendMessageReceipt(const QByteArray& messageId)
{
    if (messageId.size() != 32) {
        throw Error("Invalid message receipt identifier");
    }
    ds::protocol::MessageReceipt receipt;
    receipt.setMessageId(messageId);
    ds::protocol::Envelope envelope;
    envelope.setProtocolVersion(codec::protocolVersion);
    envelope.setMessageReceipt(std::move(receipt));
    return send(envelope);
}

bool Peer::isConnected() const noexcept
{
    return connection_ && connection_->isOpen();
}

uint64_t Peer::sendUserInfo(const UserInfo &userInfo)
{
    ds::protocol::UserInfo info;
    info.setNickname(userInfo.nickName);
    if (!userInfo.avatarHash.isEmpty()) {
        info.setAvatarHash(userInfo.avatarHash);
    }
    ds::protocol::UserSecurityPreferences preferences;
    preferences.setAllowUtf8(userInfo.allowUtf8);
    preferences.setAllowFiles(userInfo.allowFiles);
    preferences.setAllowMimeTypes(userInfo.allowMimeTypes);
    preferences.setAllowReactions(userInfo.allowReactions);
    preferences.setPublicBio(userInfo.publicBio);
    preferences.setPublicAvatar(userInfo.publicAvatar);
    if (!userInfo.bioHash.isEmpty()) {
        info.setBioHash(userInfo.bioHash);
    }
    info.setSecurityPrefs(std::move(preferences));
    ds::protocol::Envelope envelope;
    envelope.setProtocolVersion(codec::protocolVersion);
    envelope.setUserInfo(std::move(info));
    return send(envelope);
};

uint64_t Peer::sendMessage(const core::Message &message)
{
    if (!connectionData_.identitysCert || !message.validate(*connectionData_.identitysCert)) {
        throw Error("Invalid v3 outgoing message");
    }
    if (message.getData().encoding == Message::UTF8) {
        const auto localIdentity = DsEngine::instance().getIdentityManager()
                ? DsEngine::instance().getIdentityManager()->identityFromUuid(getIdentityId())
                : nullptr;
        if (localIdentity && !localIdentity->getAllowUtf8()) {
            throw Error("Outgoing UTF-8 is disabled by local security preferences");
        }
    }
    ds::protocol::Message wire;
    wire.setConversationId(message.getData().conversation);
    wire.setMessageId(message.getData().messageId);
    ds::protocol::Timestamp timestamp;
    timestamp.setUnixMinutes(message.getData().composedTime.toUTC().toSecsSinceEpoch() / 60);
    wire.setComposedAt(std::move(timestamp));
    wire.setContent(message.getData().content);
    wire.setSenderId(message.getData().sender);
    wire.setEncoding(message.getData().encoding == Message::UTF8
                     ? ds::protocol::EncodingGadget::Encoding::UTF8
                     : ds::protocol::EncodingGadget::Encoding::US_ASCII);
    wire.setSignature(message.getData().signature);
    QList<ds::protocol::MessageAttachment> attachments;
    for (const auto& attachment : message.getData().attachments) {
        ds::protocol::FileOffer offer;
        offer.setFileId(attachment.file_id); offer.setMessageId(message.getData().messageId);
        offer.setName(attachment.name); offer.setSize(attachment.size);
        offer.setSha256(attachment.sha256); offer.setMimeType(attachment.mime_type);
        ds::protocol::MessageAttachment entry;
        entry.setFileOffer(std::move(offer));
        if (attachment.thumbnail_for) entry.setThumbnailForAttachmentIndex(*attachment.thumbnail_for);
        attachments.push_back(std::move(entry));
    }
    wire.setAttachments(std::move(attachments));
    ds::protocol::Envelope envelope;
    envelope.setProtocolVersion(codec::protocolVersion);
    envelope.setMessage(std::move(wire));
    return send(envelope);
}

uint64_t Peer::requestBio()
{
    const auto requestId = nextRequestId(RequestType::BioResponse);
    ds::protocol::Envelope envelope;
    envelope.setProtocolVersion(codec::protocolVersion);
    envelope.setRequestId(requestId);
    ds::protocol::BioRequest request;
    envelope.setBioRequest(std::move(request));
    send(envelope);
    return requestId;
}

uint64_t Peer::sendBioResponse(const quint64 responseToRequestId, const QString& bio)
{
    const auto sanitized = core::bio::sanitize(bio);
    if (responseToRequestId == 0 || !core::bio::withinLimits(sanitized)) {
        throw Error("Invalid bio response");
    }
    ds::protocol::Envelope envelope;
    envelope.setProtocolVersion(codec::protocolVersion);
    envelope.setResponseToRequestId(responseToRequestId);
    ds::protocol::BioResponse response;
    if (!bio.isEmpty()) {
        ds::protocol::Bio wireBio;
        wireBio.setBioText(sanitized);
        response.setBio(std::move(wireBio));
    }
    envelope.setBioResponse(std::move(response));
    return send(envelope);
}

uint64_t Peer::sendAvatar(const QImage &avatar)
{
    const auto normalized = avatar.convertToFormat(QImage::Format_RGB888);
    ds::protocol::Avatar wire;
    if (!normalized.isNull()) {
        const auto width = normalized.width();
        const auto height = normalized.height();
        if (width <= 0 || height <= 0 || width > 512 || height > 512
                || static_cast<qint64>(width) * height > 262144) {
            throw Error("Avatar exceeds protocol v3 limits");
        }
        QByteArray red(width * height, Qt::Uninitialized);
        QByteArray green(width * height, Qt::Uninitialized);
        QByteArray blue(width * height, Qt::Uninitialized);
        for (int y = 0; y < height; ++y) {
            const auto *line = normalized.constScanLine(y);
            for (int x = 0; x < width; ++x) {
                const auto index = y * width + x;
                red[index] = static_cast<char>(line[3 * x]);
                green[index] = static_cast<char>(line[3 * x + 1]);
                blue[index] = static_cast<char>(line[3 * x + 2]);
            }
        }
        wire.setWidth(static_cast<quint32>(width));
        wire.setHeight(static_cast<quint32>(height));
        wire.setRed(std::move(red));
        wire.setGreen(std::move(green));
        wire.setBlue(std::move(blue));
    }
    ds::protocol::Envelope envelope;
    envelope.setProtocolVersion(codec::protocolVersion);
    envelope.setAvatar(std::move(wire));
    return send(envelope);
}

uint64_t Peer::requestAvatar()
{
    ds::protocol::Envelope envelope;
    envelope.setProtocolVersion(codec::protocolVersion);
    envelope.setRequestId(nextRequestId(RequestType::AvatarResponse));
    envelope.setAvatarRequest(ds::protocol::AvatarRequest{});
    return send(envelope);
}

uint64_t Peer::sendAvatarResponse(const quint64 responseToRequestId,
                                  const QImage &avatar, const bool authorized)
{
    if (responseToRequestId == 0) {
        throw Error("Avatar response requires a request id");
    }
    ds::protocol::AvatarResponse response;
    if (authorized) {
        const auto normalized = avatar.convertToFormat(QImage::Format_RGB888);
        ds::protocol::Avatar wire;
        if (!normalized.isNull()) {
            const auto width = normalized.width();
            const auto height = normalized.height();
            if (width <= 0 || height <= 0 || width > 512 || height > 512
                    || static_cast<qint64>(width) * height > 262144) {
                throw Error("Avatar exceeds protocol v3 limits");
            }
            QByteArray red(width * height, Qt::Uninitialized);
            QByteArray green(width * height, Qt::Uninitialized);
            QByteArray blue(width * height, Qt::Uninitialized);
            for (int y = 0; y < height; ++y) {
                const auto *line = normalized.constScanLine(y);
                for (int x = 0; x < width; ++x) {
                    const auto index = y * width + x;
                    red[index] = static_cast<char>(line[3 * x]);
                    green[index] = static_cast<char>(line[3 * x + 1]);
                    blue[index] = static_cast<char>(line[3 * x + 2]);
                }
            }
            wire.setWidth(static_cast<quint32>(width));
            wire.setHeight(static_cast<quint32>(height));
            wire.setRed(std::move(red));
            wire.setGreen(std::move(green));
            wire.setBlue(std::move(blue));
        }
        response.setAvatar(std::move(wire));
    }
    ds::protocol::Envelope envelope;
    envelope.setProtocolVersion(codec::protocolVersion);
    envelope.setResponseToRequestId(responseToRequestId);
    envelope.setAvatarResponse(std::move(response));
    return send(envelope);
}

uint64_t Peer::offerFile(const File &file)
{
    const auto mimeType = file.getMimeType();
    const auto reject = [&file](const char* reason) {
        LFLOG_TRACE << "Outgoing file offer validation failed; reason=" << reason
                    << ", transfer_id_bytes=" << file.getFileId().size()
                    << ", sha256_bytes=" << file.getHash().size()
                    << ", size=" << file.getSize()
                    << ", filename_bytes=" << file.getName().toUtf8().size();
        throw Error(QStringLiteral("File offer violates v3 policy: %1").arg(QLatin1String(reason)));
    };
    if (file.getFileId().size() != 16) reject("transfer ID must be 16 bytes");
    if (file.getConversation() == nullptr) reject("conversation is missing");
    if (file.getSize() < 0) reject("file size is negative");
    if (static_cast<quint64>(file.getSize()) > ResourceLimits::hardAcceptedFileBytes)
        reject("file size exceeds the hard limit");
    if (file.getHash().size() != crypto_hash_sha256_BYTES) reject("SHA-256 checksum must be 32 bytes");
    if (file.getName().isEmpty()) reject("filename is empty");
    if (file.getName().toUtf8().size() > 255) reject("filename exceeds 255 UTF-8 bytes");
    if (!validMimeType(mimeType)) reject("MIME type is invalid");
    auto* files = DsEngine::instance().getFileManager();
    if (!file.isThumbnail()) {
        const auto thumbnail = files->thumbnailFor(file);
        auto* contact = file.getContact();
        if (thumbnail && contact && connectionData_.identitysCert
            && thumbnail->getSize() > 0 && thumbnail->getSize() <= core::media_preview::max_encoded_bytes
            && thumbnail->getHash().size() == 32 && thumbnail->getFileId().size() == 16
            && thumbnail->pendingId().isNull() && !thumbnail->storageId().isNull()
            && (thumbnail->getState() == File::FS_OFFERED || thumbnail->getState() == File::FS_WAITING || thumbnail->getState() == File::FS_DONE)
            && contact->canSendFileType("image/jpeg", "preview.jpg")) {
            core::MessageData data;
            data.conversation = file.getConversation()->getHash();
            data.messageId = files->thumbnailManifest(file);
            data.composedTime = file.getCreated(); data.encoding = core::Message::US_ACSII;
            data.attachments = {
                {file.getFileId(), file.getName(), quint64(file.getSize()), file.getHash(), mimeType, {}},
                {thumbnail->getFileId(), "preview.jpg", quint64(thumbnail->getSize()), thumbnail->getHash(), "image/jpeg", quint32{0}}};
            core::Message manifest{*this, std::move(data), core::Message::OUTGOING, file.getConversationId()};
            manifest.sign(*connectionData_.identitysCert);
            if (thumbnail->getState() == File::FS_WAITING) thumbnail->setState(File::FS_OFFERED);
            return sendMessage(manifest);
        }
    }
    ds::protocol::FileOffer offer;
    offer.setFileId(file.getFileId());
    offer.setConversationId(file.getConversation()->getHash());
    offer.setName(file.getName());
    offer.setSize(static_cast<quint64>(file.getSize()));
    offer.setSha256(file.getHash());
    offer.setMimeType(mimeType);
    ds::protocol::Envelope envelope;
    envelope.setProtocolVersion(codec::protocolVersion);
    envelope.setFileOffer(std::move(offer));
    return send(envelope);
}

uint64_t Peer::startTransfer(File &file)
{
    if (file.getDirection() == File::INCOMING) {
        return startReceive(file);
    }

    return startSend(file);

}

uint64_t Peer::sendSome(File &file)
{
    try {
        if (file.getState() != File::FS_TRANSFERRING) {
            throw Error("File is not in transferring state");
        }

        auto it = outChannels_.find(file.getChannel());
        if (it == outChannels_.end()) {
            LFLOG_ERROR << "Asked to send data for file #"
                        << file.getId()
                        << " on channel "
                        << file.getChannel()
                        << " but I don't have that channel!";

            try {
                file.transferFailed("No active channel to send to");
            } catch (const std::exception& ex) {
                LFLOG_ERROR << "Failed to mark file #" << file.getId()
                            << " as failed: " << ex.what();
            } catch (...) {
                LFLOG_ERROR << "Failed to mark file #" << file.getId()
                            << " as failed: unknown exception";
            }
            return {};
        }

        auto instance = it->second;
        return instance->onOutgoing(*this);
    } catch (const std::exception& ex) {
        LFLOG_ERROR << "File #" << file.getId()
                    << " failed while sending data: " << ex.what();
        try {
            if (file.getState() == File::FS_TRANSFERRING) {
                file.transferFailed(QStringLiteral("File transfer failed: ") + ex.what(),
                                    File::FS_FAILED);
            }
        } catch (...) {
            LFLOG_ERROR << "Unable to record failure for file #" << file.getId();
        }
        return {};
    } catch (...) {
        LFLOG_ERROR << "File #" << file.getId()
                    << " failed while sending data: unknown exception";
        try {
            if (file.getState() == File::FS_TRANSFERRING) {
                file.transferFailed(QStringLiteral("File transfer failed"), File::FS_FAILED);
            }
        } catch (...) {
            LFLOG_ERROR << "Unable to record failure for file #" << file.getId();
        }
        return {};
    }
}

void Peer::handleFileAck(const QByteArray& fileId, const quint64 nextSequence,
                         const quint64 nextOffset, const bool final)
{
    const auto file = DsEngine::instance().getFileManager()->getFileFromId(
        fileId, File::OUTGOING);
    if (file && file->getContact() && getPeerCert()
            && file->getContact()->getCert()->getHash().toByteArray()
                == getPeerCert()->getHash().toByteArray()) {
        const auto it = outChannels_.find(file->getChannel());
        if (it != outChannels_.end()) {
            if (nextSequence == 0) {
                if (!final || nextOffset != 0) {
                    protocolError(QStringLiteral("Invalid initial file acknowledgement"));
                }
                return;
            }
            it->second->onAck(*this, nextSequence - 1,
                              static_cast<qlonglong>(nextOffset), final);
            emit outputBufferEmptied();
            return;
        }
    }

    protocolError(QStringLiteral("Acknowledgement for unknown file transfer"));
}

void Peer::disableNotifications()
{
    notificationsDisabled_ = true;
}


}} // namespace
