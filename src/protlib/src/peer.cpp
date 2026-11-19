#include <dsfilemanager/transferverifier.h>
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
#include <QCryptographicHash>
#include <QScopeGuard>

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

using WireFileError = ds::protocol::FileErrorGadget::FileError;
WireFileError wireFileError(FileCancelReason reason) {
    switch (reason) {
    case FileCancelReason::SourceChanged: return WireFileError::SOURCE_CHANGED;
    case FileCancelReason::UserAction: return WireFileError::USER_ACTION;
    case FileCancelReason::ReceiverCheckpointMismatch: return WireFileError::RECEIVER_CHECKPOINT_MISMATCH;
    case FileCancelReason::UnsupportedTransferVersion: return WireFileError::UNSUPPORTED_TRANSFER_VERSION;
    }
    throw Error("Unknown local transfer failure reason");
}
std::optional<FileCancelReason> localFileError(WireFileError reason) {
    switch (reason) {
    case WireFileError::SOURCE_CHANGED: return FileCancelReason::SourceChanged;
    case WireFileError::USER_ACTION: return FileCancelReason::UserAction;
    case WireFileError::RECEIVER_CHECKPOINT_MISMATCH: return FileCancelReason::ReceiverCheckpointMismatch;
    case WireFileError::UNSUPPORTED_TRANSFER_VERSION: return FileCancelReason::UnsupportedTransferVersion;
    default: return {};
    }
}

using WirePauseReason = ds::protocol::FilePauseReasonGadget::FilePauseReason;
WirePauseReason wirePauseReason(dsfm::TransferPauseReason reason) {
    switch (reason) {
    case dsfm::TransferPauseReason::None: return WirePauseReason::FILE_PAUSE_UNSPECIFIED;
    case dsfm::TransferPauseReason::SourceUnavailable: return WirePauseReason::SOURCE_UNAVAILABLE;
    case dsfm::TransferPauseReason::SourceRevalidationRequired: return WirePauseReason::SOURCE_REVALIDATION_REQUIRED;
    case dsfm::TransferPauseReason::ResourceUnavailable: return WirePauseReason::TRANSFER_RESOURCE_UNAVAILABLE;
    case dsfm::TransferPauseReason::AuthorizationWithdrawn: return WirePauseReason::TRANSFER_AUTHORIZATION_WITHDRAWN;
    case dsfm::TransferPauseReason::Disconnected: return WirePauseReason::TRANSFER_DISCONNECTED;
    case dsfm::TransferPauseReason::FeaturePaused: return WirePauseReason::TRANSFER_FEATURE_PAUSED;
    }
    throw Error("Unknown local transfer pause reason");
}
std::optional<dsfm::TransferPauseReason> localPauseReason(WirePauseReason reason) {
    switch (reason) {
    case WirePauseReason::SOURCE_UNAVAILABLE: return dsfm::TransferPauseReason::SourceUnavailable;
    case WirePauseReason::SOURCE_REVALIDATION_REQUIRED: return dsfm::TransferPauseReason::SourceRevalidationRequired;
    case WirePauseReason::TRANSFER_RESOURCE_UNAVAILABLE: return dsfm::TransferPauseReason::ResourceUnavailable;
    case WirePauseReason::TRANSFER_AUTHORIZATION_WITHDRAWN: return dsfm::TransferPauseReason::AuthorizationWithdrawn;
    case WirePauseReason::TRANSFER_DISCONNECTED: return dsfm::TransferPauseReason::Disconnected;
    case WirePauseReason::TRANSFER_FEATURE_PAUSED: return dsfm::TransferPauseReason::FeaturePaused;
    default: return {};
    }
}

// A routing binding, not an execution controller. After completion it may
// replay only the immutable receipt supplied by the session owner.
class SessionChannel final : public Peer::Channel {
public:
    SessionChannel(std::shared_ptr<TransferSession> session, File::ptr_t file)
        : session_{session}, file_{std::move(file)}, connection_{session->connection()}, attempt_{session->attempt()} {}
    void onIncoming(Peer &peer, quint64, const Peer::mview_t &data, bool final) override {
        if (data.size() < Peer::file_chunk_header_bytes) throw Error("Invalid file chunk header");
        quint64 sequence{}, offset{};
        memcpy(&sequence, data.cdata(), sizeof(sequence));
        memcpy(&offset, data.cdata() + sizeof(sequence), sizeof(offset));
        sequence = qFromBigEndian(sequence); offset = qFromBigEndian(offset);
        const auto size = data.size() - Peer::file_chunk_header_bytes;
        if (size > Peer::file_chunk_bytes) throw Error("File chunk exceeds local bound");
        if (receipt_) {
            if (!final || sequence + 1 != next_sequence_ || offset != final_start_ || offset > receipt_->offset || size != receipt_->offset - offset)
                throw Error("Invalid completed transfer retransmission");
            replay(peer);
            return;
        }
        TransferSession::Event event; event.kind = TransferSession::Event::Kind::Chunk;
        event.sequence = sequence; event.offset = offset; event.final = final;
        event.bytes = QByteArray{reinterpret_cast<const char *>(data.cdata() + Peer::file_chunk_header_bytes), qsizetype(size)};
        if (auto session = session_.lock()) session->deliver(connection_, attempt_, std::move(event));
    }
    void onAck(Peer &, quint64 sequence, qlonglong offset, bool final) override {
        if (offset < 0 || sequence == std::numeric_limits<quint64>::max()) throw Error("Invalid transfer acknowledgement");
        TransferSession::Event event; event.kind = TransferSession::Event::Kind::Ack;
        event.sequence = sequence + 1; event.offset = quint64(offset); event.final = final;
        if (auto session = session_.lock()) session->deliver(connection_, attempt_, std::move(event));
    }
    void onCheckpoint(Peer &peer, const ds::protocol::FileCheckpoint &message) override {
        const auto &ref = message.checkpoint();
        const dsfm::DurableCheckpoint checkpoint{ref.sequence(), ref.nextOffset(), ref.prefixSha256(), ref.final(), ref.pauseBoundary()};
        if (receipt_) {
            if (checkpoint != *receipt_ || message.phase() != ds::protocol::FileCheckpoint::Phase::CHECKPOINT_SENDER_CONFIRMED)
                throw Error("Invalid completed checkpoint retransmission");
            replay(peer); return;
        }
        TransferSession::Event event; event.kind = TransferSession::Event::Kind::Checkpoint;
        event.checkpoint = checkpoint;
        event.checkpoint_phase = dsfm::CheckpointPhase(int(message.phase()) - 1);
        if (auto session = session_.lock()) session->deliver(connection_, attempt_, std::move(event));
    }
    void retain(dsfm::DurableCheckpoint checkpoint, quint64 next_sequence, quint64 final_start) {
        receipt_ = checkpoint; next_sequence_ = next_sequence; final_start_ = final_start;
        session_.reset();
    }
    bool owns(const File &file) const {
        return file_->getId() == file.getId() && file_->getIdentityId() == file.getIdentityId() &&
            file_->getContactId() == file.getContactId() && file_->getDirection() == file.getDirection() &&
            file.checkpointAttempt() && file.checkpointAttempt()->id == attempt_;
    }
private:
    void replay(Peer &peer) {
        if (!file_->checkpointAttempt() || file_->checkpointAttempt()->id != attempt_) return;
        peer.sendFileCheckpoint(file_->getFileId(), file_->getHash(), *file_->checkpointAttempt(), *receipt_, dsfm::CheckpointPhase::MutuallyConfirmed);
        peer.sendCheckpointAck(file_->getFileId(), attempt_, next_sequence_, receipt_->offset, true);
    }
    std::weak_ptr<TransferSession> session_;
    File::ptr_t file_;
    QUuid connection_, attempt_;
    std::optional<dsfm::DurableCheckpoint> receipt_;
    quint64 next_sequence_ = 0, final_start_ = 0;
};

Message::Encoding toEncoding(const QString &name) {
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
        const auto now = std::chrono::steady_clock::now();
        for (auto it = completed_incoming_.begin(); it != completed_incoming_.end();) {
            if (now - it->second >= std::chrono::seconds(30)) {
                inChannels_.erase(it->first);
                it = completed_incoming_.erase(it);
            } else ++it;
        }
    });
    transferTimer->start();
    connect(this, &Peer::closeLater,
            this, &Peer::onCloseLater,
            Qt::QueuedConnection);


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

uint64_t Peer::sendFileChunk(const QByteArray&, quint64, quint64, const QByteArray&, bool)
{
    throw Error("Unversioned file transfers are unsupported");
}

uint64_t Peer::sendCheckpointChunk(const QByteArray &fileId,QUuid attempt,quint64 sequence,
    quint64 offset,const QByteArray &data,bool final) {
    if (!checkpointInterval()) throw Error("Checkpoint payload is not negotiated");
    if(fileId.size()!=16 || attempt.isNull() || data.size()>qsizetype(file_chunk_bytes))throw Error("Invalid checkpoint chunk");
    ds::protocol::FileChunk chunk;chunk.setFileId(fileId);chunk.setAttemptId(attempt.toRfc4122());
    chunk.setSequence(sequence);chunk.setOffset(offset);chunk.setData(data);chunk.setFinal(final);
    ds::protocol::Envelope envelope;envelope.setProtocolVersion(codec::protocolVersion);envelope.setFileChunk(std::move(chunk));return send(envelope);
}
uint64_t Peer::sendCheckpointAck(const QByteArray &fileId,QUuid attempt,quint64 nextSequence,quint64 nextOffset,bool final) {
    if (!checkpointInterval()) throw Error("Checkpoint acknowledgement is not negotiated");
    if(fileId.size()!=16 || attempt.isNull())throw Error("Invalid checkpoint acknowledgement");
    ds::protocol::FileAck ack;ack.setFileId(fileId);ack.setAttemptId(attempt.toRfc4122());ack.setNextSequence(nextSequence);
    ack.setNextOffset(nextOffset);ack.setFinal(final);
    ds::protocol::Envelope envelope;envelope.setProtocolVersion(codec::protocolVersion);envelope.setFileAck(std::move(ack));return send(envelope);
}
uint64_t Peer::sendFileCheckpoint(const QByteArray &fileId,const QByteArray &expectedSha256,const dsfm::TransferAttempt &attempt,
    dsfm::DurableCheckpoint checkpoint,dsfm::CheckpointPhase phase) {
    if (!checkpointInterval()) throw Error("Checkpoint confirmation is not negotiated");
    if(fileId.size()!=16 || expectedSha256.size()!=32 || attempt.id.isNull() || int(phase)<0 || int(phase)>2)throw Error("Invalid checkpoint confirmation");
    ds::protocol::FileCheckpoint message;message.setFileId(fileId);message.setAttemptId(attempt.id.toRfc4122());
    message.setCheckpoint(codec::checkpointReference(checkpoint));message.setExpectedSha256(expectedSha256);
    message.setPhase(ds::protocol::FileCheckpoint::Phase(int(phase)+1));
    ds::protocol::Envelope envelope;envelope.setProtocolVersion(codec::protocolVersion);envelope.setFileCheckpoint(std::move(message));return send(envelope);
}
uint64_t Peer::sendFilePause(const QByteArray &fileId,QUuid attempt,dsfm::DurableCheckpoint checkpoint,
    dsfm::TransferPauseReason reason) {
    if (!checkpointInterval()) throw Error("Checkpoint pause is not negotiated");
    if(fileId.size()!=16 || attempt.isNull() || reason==dsfm::TransferPauseReason::None)throw Error("Invalid transfer pause");
    ds::protocol::FilePause message;message.setFileId(fileId);message.setAttemptId(attempt.toRfc4122());
    message.setConfirmedCheckpoint(codec::checkpointReference(checkpoint));
    using Reason=ds::protocol::FilePauseReasonGadget::FilePauseReason;
    switch(reason) {
    case dsfm::TransferPauseReason::SourceUnavailable:message.setReason(Reason::SOURCE_UNAVAILABLE);break;
    case dsfm::TransferPauseReason::SourceRevalidationRequired:message.setReason(Reason::SOURCE_REVALIDATION_REQUIRED);break;
    case dsfm::TransferPauseReason::ResourceUnavailable:message.setReason(Reason::TRANSFER_RESOURCE_UNAVAILABLE);break;
    case dsfm::TransferPauseReason::AuthorizationWithdrawn:message.setReason(Reason::TRANSFER_AUTHORIZATION_WITHDRAWN);break;
    case dsfm::TransferPauseReason::Disconnected:message.setReason(Reason::TRANSFER_DISCONNECTED);break;
    case dsfm::TransferPauseReason::FeaturePaused:message.setReason(Reason::TRANSFER_FEATURE_PAUSED);break;
    default:throw Error("Invalid transfer pause reason");
    }
    ds::protocol::Envelope envelope;envelope.setProtocolVersion(codec::protocolVersion);envelope.setFilePause(std::move(message));return send(envelope);
}

uint64_t Peer::sendFileAck(const QByteArray&, quint64, quint64, bool)
{
    throw Error("Unversioned file transfers are unsupported");
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

uint64_t Peer::sendCheckpointOfferResponse(const QByteArray &file_id, const dsfm::TransferAttempt &attempt)
{
    if (!checkpointInterval() || file_id.size() != 16 ||
        !codec::validateCheckpointParameters(codec::checkpointParameters(attempt)))
        throw Error("Invalid checkpoint readiness parameters");
    ds::protocol::FileOfferResponse response;
    response.setFileId(file_id);
    response.setStatus(ds::protocol::FileOfferResponse::Status::FILE_OFFER_ACCEPTED);
    response.setCheckpoints(codec::checkpointParameters(attempt));
    response.setSelectedCheckpoint(codec::checkpointReference(attempt.selected));
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
    cancel.setReason(wireFileError(reason));
    ds::protocol::Envelope envelope;
    envelope.setProtocolVersion(codec::protocolVersion);
    envelope.setFileCancel(std::move(cancel));
    return send(envelope);
}

uint64_t Peer::sendFileResume(const QByteArray&, quint64, const QByteArray&, quint64)
{
    throw Error("Unversioned file transfers are unsupported");
}

uint64_t Peer::sendCheckpointResume(const QByteArray &file_id, const QByteArray &sha256, quint64 size, quint32 interval,
                                   QVector<dsfm::DurableCheckpoint> retained, QDeadlineTimer deadline)
{
    if (!checkpointInterval() || deadline.hasExpired() || retained.size() > dsfm::maximum_retained_checkpoints)
        throw Error("Checkpoint resume is not admitted");
    ds::protocol::FileResume resume;
    resume.setFileId(file_id); resume.setSha256(sha256); resume.setFileSize(size);
    resume.setCheckpointVersion(dsfm::checkpoint_format_version); resume.setCheckpointIntervalBytes(interval);
    QList<ds::protocol::CheckpointReference> references;
    quint64 retained_end = 0;
    for (const auto &checkpoint : retained) {
        references.append(codec::checkpointReference(checkpoint));
        retained_end = std::max(retained_end, checkpoint.offset);
    }
    resume.setNextOffset(retained_end); resume.setAvailableCheckpoints(std::move(references));
    if (!codec::validateResumeCheckpoints(resume)) throw Error("Invalid retained checkpoint resume metadata");
    const auto request_id = nextRequestId(RequestType::FileResumeResult);
    auto &pending = outstandingRequests_.at(request_id);
    pending.file_id = file_id; pending.file_size = size; pending.checkpoint_interval = interval;
    pending.expires = std::chrono::steady_clock::now() + std::chrono::milliseconds(deadline.remainingTime());
    ds::protocol::Envelope envelope;
    envelope.setProtocolVersion(codec::protocolVersion); envelope.setRequestId(request_id);
    envelope.setFileResume(std::move(resume));
    send(envelope);
    return request_id;
}

uint64_t Peer::sendCheckpointResumeResult(quint64 request_id, const QByteArray &file_id, FileResumeStatus status,
                                         std::optional<dsfm::TransferAttempt> attempt, dsfm::TransferPauseReason pause,
                                         FileCancelReason error)
{
    if (!checkpointInterval() || !request_id || file_id.size() != 16 ||
        (status == FileResumeStatus::Accepted ? !attempt : bool(attempt)) ||
        (status == FileResumeStatus::Paused ? pause == dsfm::TransferPauseReason::None : pause != dsfm::TransferPauseReason::None))
        throw Error("Invalid checkpoint resume outcome");
    using Status = ds::protocol::FileResumeResult::Status;
    ds::protocol::FileResumeResult result;
    result.setFileId(file_id);
    switch (status) {
    case FileResumeStatus::Accepted: result.setStatus(Status::RESUME_ACCEPTED); break;
    case FileResumeStatus::Rejected: result.setStatus(Status::RESUME_REJECTED); break;
    case FileResumeStatus::Cancelled: result.setStatus(Status::RESUME_CANCELLED); break;
    case FileResumeStatus::Paused: result.setStatus(Status::RESUME_PAUSED); break;
    }
    if (attempt) {
        if (!codec::validateCheckpointParameters(codec::checkpointParameters(*attempt)))
            throw Error("Invalid selected resume attempt");
        result.setCheckpoints(codec::checkpointParameters(*attempt));
        result.setSelectedCheckpoint(codec::checkpointReference(attempt->selected));
        result.setNextOffset(attempt->selected.offset);
    }
    result.setPauseReason(wirePauseReason(pause));
    if (status == FileResumeStatus::Cancelled)
        result.setError(wireFileError(error));
    ds::protocol::Envelope envelope;
    envelope.setProtocolVersion(codec::protocolVersion); envelope.setResponseToRequestId(request_id);
    envelope.setFileResumeResult(std::move(result));
    return send(envelope);
}

uint64_t Peer::sendFileResumeResult(quint64, const QByteArray&, core::FileResumeStatus, quint64, core::FileCancelReason)
{
    throw Error("Unversioned file transfers are unsupported");
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
            || envelope.hasBioResponse() || envelope.hasFileCheckpoint() || envelope.hasFilePause();
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
        peer_checkpoint_interval_ = envelope.capabilities().checkpointIntervalBytes();
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
    // File transfers require checkpoint version one. Text and other unrelated
    // features remain usable on a connection without this capability.
    const bool transfer_fields = envelope.hasFileOffer() || envelope.hasFileChunk() || envelope.hasFileAck() ||
        envelope.hasFileResume() || envelope.hasFileResumeResult() || envelope.hasFileCheckpoint() || envelope.hasFilePause() ||
        (envelope.hasFileOfferResponse() &&
            (envelope.fileOfferResponse().status() == ds::protocol::FileOfferResponse::Status::FILE_OFFER_ACCEPTED ||
             envelope.fileOfferResponse().hasCheckpoints() || envelope.fileOfferResponse().hasSelectedCheckpoint()));
    if (transfer_fields && !checkpointInterval())
        protocolError(QStringLiteral("File transfers require negotiated checkpoint support"));
    if ((envelope.hasFileOffer() && !envelope.fileOffer().hasCheckpoints()) ||
        (envelope.hasFileChunk() && envelope.fileChunk().attemptId().isEmpty()) ||
        (envelope.hasFileAck() && envelope.fileAck().attemptId().isEmpty()) ||
        (envelope.hasFileResume() && envelope.fileResume().checkpointVersion() != dsfm::checkpoint_format_version) ||
        (envelope.hasFileOfferResponse() &&
            envelope.fileOfferResponse().status() == ds::protocol::FileOfferResponse::Status::FILE_OFFER_ACCEPTED &&
            (!envelope.fileOfferResponse().hasCheckpoints() || !envelope.fileOfferResponse().hasSelectedCheckpoint())))
        protocolError(QStringLiteral("Unversioned file transfer is unsupported"));
    if(envelope.hasMessage()) {
        const auto &attachments=envelope.message().attachments();
        if(attachments.size()>16)protocolError(QStringLiteral("Too many message attachments"));
        for(const auto &attachment:attachments) {
            if(!attachment.hasFileOffer())protocolError(QStringLiteral("Missing attachment offer"));
            if (!checkpointInterval() || !attachment.fileOffer().hasCheckpoints())
                protocolError(QStringLiteral("Attachments require negotiated checkpoints"));
        }
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
        for (const auto &entry : message.attachments())
            incoming.attachment_attempts.append(offeredCheckpointAttempt(entry.fileOffer()));
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
        incoming.checkpoint_attempt = offeredCheckpointAttempt(offer);
        emit receivedFileOffer(incoming);
        return;
    }
    if (envelope.hasFileResume()) {
        const auto& resume = envelope.fileResume();
        const bool checkpoint_mode = resume.checkpointVersion() || resume.checkpointIntervalBytes() || !resume.availableCheckpoints().isEmpty();
        if (checkpoint_mode) {
            if (!envelope.hasRequestId() || !codec::validateResumeCheckpoints(resume))
                protocolError(QStringLiteral("Invalid checkpoint resume request"));
            PeerFileResume incoming{shared_from_this(), getConnectionId(), requestId, resume.fileId(),
                                    resume.nextOffset(), resume.sha256(), resume.fileSize()};
            incoming.checkpoint_interval = resume.checkpointIntervalBytes();
            for (const auto &reference : resume.availableCheckpoints())
                incoming.retained_checkpoints.append({reference.sequence(), reference.nextOffset(), reference.prefixSha256(),
                                                      reference.final(), reference.pauseBoundary()});
            emit receivedFileResume(incoming);
            return;
        }
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
        const auto file = DsEngine::instance().getFileManager()->getFileFromPeer(
            chunk.fileId(), getIdentityId(), getPeerCert() ? getPeerCert()->getHash().toByteArray() : QByteArray{}, File::INCOMING);
        if (!chunk.attemptId().isEmpty()) {
            checkpointFile(chunk.fileId(), chunk.attemptId(), File::INCOMING);
        }
        if (!file || (file->checkpointInterval() && chunk.attemptId().isEmpty()) || !file->getContact() || !getPeerCert()
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
        if (!ack.attemptId().isEmpty()) {
            const auto file = checkpointFile(ack.fileId(), ack.attemptId(), File::OUTGOING);
            if (!ack.nextSequence() || ack.nextOffset() > quint64(file->getSize()))
                protocolError(QStringLiteral("Invalid checkpoint flow acknowledgement"));
            const auto channel = outChannels_.find(file->getChannel());
            if (channel == outChannels_.end())
                protocolError(QStringLiteral("Checkpoint acknowledgement has no active channel"));
            channel->second->onAck(*this, ack.nextSequence() - 1, qlonglong(ack.nextOffset()), ack.final());
            emit outputBufferEmptied();
        } else {
            protocolError(QStringLiteral("Unversioned file acknowledgement is unsupported"));
        }
        emit receivedFileAck(PeerFileAck{shared_from_this(), getConnectionId(), requestId,
                                         ack.fileId(), ack.nextSequence(), ack.nextOffset(),
                                         ack.final()});
        return;
    }
    if (envelope.hasFileCheckpoint()) {
        const auto &checkpoint = envelope.fileCheckpoint();
        using Phase = ds::protocol::FileCheckpoint::Phase;
        const auto direction = checkpoint.phase() == Phase::CHECKPOINT_SENDER_CONFIRMED
                                   ? File::INCOMING : File::OUTGOING;
        const auto file = checkpointFile(checkpoint.fileId(), checkpoint.attemptId(), direction);
        if (!codec::validateFileCheckpoint(checkpoint, quint64(file->getSize()), file->checkpointInterval()) ||
            checkpoint.expectedSha256() != file->getHash())
            protocolError(QStringLiteral("Invalid checkpoint confirmation"));
        const auto &channels = direction == File::INCOMING ? inChannels_ : outChannels_;
        const auto channel = channels.find(file->getChannel());
        if (channel == channels.end())
            protocolError(QStringLiteral("Checkpoint confirmation has no active channel"));
        channel->second->onCheckpoint(*this, checkpoint);
        return;
    }
    if (envelope.hasFilePause()) {
        const auto &pause = envelope.filePause();
        if (!checkpointInterval() || !getPeerCert() || pause.fileId().size() != 16 || pause.attemptId().size() != 16)
            protocolError(QStringLiteral("Invalid checkpoint pause identity"));
        File::ptr_t selected;
        // Pause has no direction field. Resolve the nonce within each scoped
        // direction and reject ambiguity rather than selecting the first ID.
        for (const auto direction : {File::INCOMING, File::OUTGOING}) {
            const auto file = DsEngine::instance().getFileManager()->getFileFromPeer(
                pause.fileId(), getIdentityId(), getPeerCert() ? getPeerCert()->getHash().toByteArray() : QByteArray{}, direction);
            if (!file || !file->checkpointAttempt() ||
                file->checkpointAttempt()->id.toRfc4122() != pause.attemptId()) continue;
            if (selected) protocolError(QStringLiteral("Ambiguous checkpoint pause"));
            selected = file;
        }
        if (!selected || selected->checkpointAttempt()->interval != selected->checkpointInterval() ||
            !selected->getContact() || selected->getContact()->iBlocked() ||
            selected->getContact()->getState() != Contact::ACCEPTED ||
            !codec::validateFilePause(pause, quint64(selected->getSize()), selected->checkpointInterval()))
            protocolError(QStringLiteral("Invalid checkpoint pause"));
        // This reference is advisory. It never writes or advances progress.
        using Reason = ds::protocol::FilePauseReasonGadget::FilePauseReason;
        dsfm::TransferPauseReason reason;
        switch (pause.reason()) {
        case Reason::SOURCE_UNAVAILABLE: reason = dsfm::TransferPauseReason::SourceUnavailable; break;
        case Reason::SOURCE_REVALIDATION_REQUIRED: reason = dsfm::TransferPauseReason::SourceRevalidationRequired; break;
        case Reason::TRANSFER_RESOURCE_UNAVAILABLE: reason = dsfm::TransferPauseReason::ResourceUnavailable; break;
        case Reason::TRANSFER_AUTHORIZATION_WITHDRAWN: reason = dsfm::TransferPauseReason::AuthorizationWithdrawn; break;
        case Reason::TRANSFER_DISCONNECTED: reason = dsfm::TransferPauseReason::Disconnected; break;
        case Reason::TRANSFER_FEATURE_PAUSED: reason = dsfm::TransferPauseReason::FeaturePaused; break;
        default: protocolError(QStringLiteral("Unknown checkpoint pause reason"));
        }
        if (selected->getState() == File::FS_PAUSED && selected->dataPauseReason() == reason) return;
        if (const auto key = selected->checkpointKey(); key) {
            if (auto session = DsEngine::instance().transfers().find(key.value())) {
                if (session->connection() != getConnectionId() || session->attempt().toRfc4122() != pause.attemptId())
                    protocolError(QStringLiteral("Pause belongs to another connection or attempt"));
                session->stop(TransferSession::Stop::Paused, reason);
                return;
            }
        }
        protocolError(QStringLiteral("Checkpoint pause has no owning session"));
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
        PeerFileOfferResponse incoming{
            shared_from_this(), getConnectionId(), requestId, response.fileId(),
            response.status() == ds::protocol::FileOfferResponse::Status::FILE_OFFER_ACCEPTED
                ? core::FileOfferResponseStatus::Accepted
                : core::FileOfferResponseStatus::Rejected};
        if (response.hasCheckpoints() || response.hasSelectedCheckpoint()) {
            if (!response.hasCheckpoints() || !response.hasSelectedCheckpoint() ||
                incoming.status != FileOfferResponseStatus::Accepted)
                protocolError(QStringLiteral("Incomplete checkpoint readiness"));
            const auto file = DsEngine::instance().getFileManager()->getFileFromPeer(
                response.fileId(), getIdentityId(), getPeerCert() ? getPeerCert()->getHash().toByteArray() : QByteArray{}, File::OUTGOING);
            if (!file || !file->checkpointInterval())
                protocolError(QStringLiteral("Checkpoint readiness has no pending transfer"));
            auto attempt = codec::checkpointAttempt(response.checkpoints(), response.selectedCheckpoint(), quint64(file->getSize()));
            if (!attempt || attempt.value().interval != file->checkpointInterval())
                protocolError(QStringLiteral("Invalid checkpoint readiness parameters"));
            incoming.checkpoint_attempt = attempt.value();
            if (file->getState() == File::FS_OFFERED) {
                const auto offered = initial_offers_.find(response.fileId());
                if (offered == initial_offers_.end() || offered->second.attempt != attempt.value().id)
                    protocolError(QStringLiteral("Initial readiness belongs to another connection"));
            }
            if (!file->checkpointAttempt()) protocolError(QStringLiteral("Readiness has no owning transfer attempt"));
            if (attempt.value() != *file->checkpointAttempt())
                protocolError(QStringLiteral("Checkpoint readiness differs from selected attempt"));
        }
        emit receivedFileOfferResponse(incoming);
        forgetInitialOffer(incoming.fileId);
        return;
    }
    if (envelope.hasFileResumeResult()) {
        const auto& result = envelope.fileResumeResult();
        expireOutstandingRequests();
        const auto request = outstandingRequests_.find(requestId);
        if (request == outstandingRequests_.end() || request->second.type != RequestType::FileResumeResult ||
            request->second.file_id != result.fileId())
            protocolError(QStringLiteral("Resume outcome does not match its pending transfer"));
        const auto pending = request->second;
        const bool checkpoint_mode = pending.checkpoint_interval != 0;
        if (!checkpoint_mode) protocolError(QStringLiteral("Unversioned resume outcome is unsupported"));
        if (!envelope.hasResponseToRequestId() || requestId == 0
                || result.fileId().size() != 16
                || (result.status() != ds::protocol::FileResumeResult::Status::RESUME_ACCEPTED
                    && result.status() != ds::protocol::FileResumeResult::Status::RESUME_REJECTED
                    && result.status() != ds::protocol::FileResumeResult::Status::RESUME_CANCELLED
                    && !(checkpoint_mode && result.status() == ds::protocol::FileResumeResult::Status::RESUME_PAUSED))
                || result.nextOffset() > ResourceLimits::hardAcceptedFileBytes
                || result.nextOffset() > pending.file_size
                || (!checkpoint_mode && result.nextOffset() % file_chunk_bytes != 0)) {
            protocolError(QStringLiteral("Invalid v3 file resume result"));
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
        case ds::protocol::FileResumeResult::Status::RESUME_PAUSED:
            status = FileResumeStatus::Paused;
            break;
        default:
            protocolError(QStringLiteral("Unknown file resume result status"));
        }
        const auto error = localFileError(result.error());
        if (result.error() != WireFileError::FILE_ERROR_UNSPECIFIED && !error) {
            protocolError(QStringLiteral("Unknown file resume result error"));
        }
        if ((result.status() == ds::protocol::FileResumeResult::Status::RESUME_CANCELLED
             && result.error() == ds::protocol::FileErrorGadget::FileError::FILE_ERROR_UNSPECIFIED)
                || (result.status() != ds::protocol::FileResumeResult::Status::RESUME_CANCELLED
                    && result.error() != ds::protocol::FileErrorGadget::FileError::FILE_ERROR_UNSPECIFIED)) {
            protocolError(QStringLiteral("Invalid file resume result error"));
        }
        PeerFileResumeResult incoming{
            shared_from_this(), getConnectionId(), requestId, result.fileId(), status,
            result.nextOffset(), error.value_or(FileCancelReason::UserAction)};
        if (checkpoint_mode && status == FileResumeStatus::Accepted) {
            if (!result.hasCheckpoints() || !result.hasSelectedCheckpoint() ||
                result.checkpoints().intervalBytes() != pending.checkpoint_interval || int(result.pauseReason()))
                protocolError(QStringLiteral("Incomplete checkpoint resume outcome"));
            auto attempt = codec::checkpointAttempt(result.checkpoints(), result.selectedCheckpoint(), pending.file_size);
            if (!attempt || result.nextOffset() != attempt.value().selected.offset)
                protocolError(QStringLiteral("Invalid selected resume checkpoint"));
            incoming.checkpoint_attempt = attempt.value();
        } else if (result.hasCheckpoints() || result.hasSelectedCheckpoint()) {
            protocolError(QStringLiteral("Unexpected checkpoint resume selection"));
        }
        if (status == FileResumeStatus::Paused) {
            const auto reason = localPauseReason(result.pauseReason());
            if (!checkpoint_mode || !reason) protocolError(QStringLiteral("Invalid checkpoint resume pause"));
            incoming.pause = *reason;
        } else if (int(result.pauseReason())) protocolError(QStringLiteral("Unexpected resume pause reason"));
        if (checkpoint_mode && status != FileResumeStatus::Accepted && result.nextOffset())
            protocolError(QStringLiteral("Unaccepted resume outcome carries progress"));
        if (!consumeRequest(requestId, RequestType::FileResumeResult))
            protocolError(QStringLiteral("Expired checkpoint resume outcome"));
        emit receivedFileResumeResult(incoming);
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
        const auto reason = localFileError(cancel.reason());
        if (cancel.fileId().size() != 16 || !reason) {
            protocolError(QStringLiteral("Invalid v3 file cancellation"));
        }
        emit receivedFileCancel(PeerFileCancel{
            shared_from_this(), getConnectionId(), requestId, cancel.fileId(),
            *reason});
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
    const auto preference = localCheckpointPreference();
    if (preference && !dsfm::validCheckpointInterval(preference))
        throw Error("Invalid local checkpoint preference");
    QStringList capabilities{QStringLiteral("file-chunks-v3"), QStringLiteral("keepalive-v3")};
    if (preference) capabilities.append(QStringLiteral("file-checkpoint-v1"));
    send(codec::makeCapabilities(capabilities, static_cast<quint32>(max_record_bytes), preference));
    advertised_checkpoint_interval_ = preference;
}

quint32 Peer::checkpointInterval() const noexcept
{
    if (!capabilitiesReceived_ || !advertised_checkpoint_interval_ || !peer_checkpoint_interval_)
        return 0;
    return std::min(advertised_checkpoint_interval_, peer_checkpoint_interval_);
}

File::ptr_t Peer::checkpointFile(const QByteArray &file_id, const QByteArray &attempt_id,
                                File::Direction direction)
{
    if (!checkpointInterval() || file_id.size() != 16 || file_id == QByteArray(16, '\0') ||
        attempt_id.size() != 16 || attempt_id == QByteArray(16, '\0') || !getPeerCert())
        protocolError(QStringLiteral("Checkpoint transfer has no negotiated scope"));
    const auto file = DsEngine::instance().getFileManager()->getFileFromPeer(
        file_id, getIdentityId(), getPeerCert()->getHash().toByteArray(), direction);
    if (!file || !file->checkpointInterval() || !file->checkpointAttempt() ||
        file->checkpointAttempt()->interval != file->checkpointInterval() ||
        file->checkpointAttempt()->id.toRfc4122() != attempt_id || !file->getChannel() ||
        !file->getContact() || file->getContact()->iBlocked() || file->getContact()->getState() != Contact::ACCEPTED)
        protocolError(QStringLiteral("Checkpoint transfer does not match the active authorized attempt"));
    return file;
}

std::optional<dsfm::TransferAttempt> Peer::offeredCheckpointAttempt(const ds::protocol::FileOffer &offer)
{
    if (!offer.hasCheckpoints()) protocolError(QStringLiteral("File transfers require checkpoint version one"));
    if (!checkpointInterval() || offer.fileId().size() != 16 || offer.fileId() == QByteArray(16, '\0') ||
        !codec::validateCheckpointParameters(offer.checkpoints()) || offer.checkpoints().nextCheckpointSequence() != 1)
        protocolError(QStringLiteral("Invalid initial checkpoint offer parameters"));
    if (offer.checkpoints().intervalBytes() > checkpointInterval()) {
        // A lower preference on this connection applies to new transfers. A
        // repeated offer may only retain a larger interval already frozen in
        // our own scoped record for exactly this immutable content.
        const auto existing = DsEngine::instance().getFileManager()->getFileFromPeer(
            offer.fileId(), getIdentityId(), getPeerCert() ? getPeerCert()->getHash().toByteArray() : QByteArray{}, File::INCOMING);
        if (!existing || existing->checkpointInterval() != offer.checkpoints().intervalBytes() ||
            existing->getSize() < 0 || quint64(existing->getSize()) != offer.size() ||
            existing->getHash() != offer.sha256() || existing->getName() != offer.name() ||
            existing->getMimeType() != offer.mimeType())
            protocolError(QStringLiteral("Checkpoint offer exceeds the negotiated interval"));
    }
    ds::protocol::CheckpointReference zero;
    zero.setPrefixSha256(QCryptographicHash::hash(QByteArray{}, QCryptographicHash::Sha256));
    auto attempt = codec::checkpointAttempt(offer.checkpoints(), zero, offer.size());
    if (!attempt) protocolError(QStringLiteral("Invalid initial checkpoint offer"));
    return attempt.value();
}

void Peer::setOfferedCheckpoint(ds::protocol::FileOffer &offer, const File &file)
{
    if (file.getId() <= 0) throw Error("Initial file offer has no persistent identity");
    if (!file.checkpointInterval()) throw Error("File transfers require prepared checkpoint parameters");
    if (!checkpointInterval() || !file.checkpointAttempt() ||
        file.checkpointAttempt()->interval != file.checkpointInterval() ||
        file.checkpointAttempt()->interval > checkpointInterval() ||
        file.checkpointAttempt()->selected.offset != 0 || file.checkpointAttempt()->selected.final ||
        file.checkpointAttempt()->next_sequence != 1)
        throw Error("Checkpoint initial offer has no prepared negotiated attempt");
    offer.setCheckpoints(codec::checkpointParameters(*file.checkpointAttempt()));
    if (!initial_offers_.contains(file.getFileId()) && initial_offers_.size() >= ResourceLimits::hardQueuedFileOffers)
        throw Error("Too many pending initial file offers");
    if (!initial_offer_states_) {
        auto *files = DsEngine::instance().getFileManager();
        initial_offer_states_ = connect(files, &FileManager::fileStateChanged, this, [this](const File *changed) {
            const auto found = initial_offers_.find(changed->getFileId());
            if (found != initial_offers_.end() && found->second.file_db_id == changed->getId() &&
                changed->getState() != File::FS_OFFERED) forgetInitialOffer(found->first);
        });
        initial_offer_deletions_ = connect(files, &FileManager::fileDeleted, this, [this](int id) {
            // Deletion carries only the local row ID. The registry is bounded
            // to 64 entries, and matching never loads another File or journal.
            for (auto it = initial_offers_.begin(); it != initial_offers_.end();) {
                if (it->second.file_db_id == id) it = initial_offers_.erase(it);
                else ++it;
            }
        });
    }
    initial_offers_.insert_or_assign(file.getFileId(), InitialOffer{file.checkpointAttempt()->id, file.getId()});
}

void Peer::forgetInitialOffer(const QByteArray &file_id)
{
    const auto found = initial_offers_.find(file_id);
    if (found == initial_offers_.end()) return;
    initial_offers_.erase(found);
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

void Peer::retainCompletedIncoming(quint32 channel)
{
    if (!completed_incoming_.contains(channel) && completed_incoming_.size() >= 2) {
        const auto oldest = std::min_element(completed_incoming_.begin(), completed_incoming_.end(),
            [](const auto &left, const auto &right) { return left.second < right.second; });
        inChannels_.erase(oldest->first);
        completed_incoming_.erase(oldest);
    }
    completed_incoming_.emplace(channel, std::chrono::steady_clock::now());
}

quint32 Peer::bindTransfer(File &file, std::shared_ptr<TransferSession> session)
{
    const auto key = file.checkpointKey();
    if (!session || session->connection() != getConnectionId() || !file.checkpointAttempt() ||
        session->attempt() != file.checkpointAttempt()->id || !key || key.value() != session->key())
        throw Error("Transfer routing has no active attempt");
    const auto id = nextInchannel_++;
    if (!id || inChannels_.contains(id) || outChannels_.contains(id)) throw Error("Transfer channel collision");
    auto file_ptr = DsEngine::instance().getFileManager()->getFile(file.getId());
    auto &channels = file.getDirection() == File::INCOMING ? inChannels_ : outChannels_;
    channels.emplace(id, std::make_shared<SessionChannel>(std::move(session), std::move(file_ptr)));
    return id;
}

bool Peer::transferWritable() const noexcept
{
    return isConnected() && connection_ && connection_->queuedBytes() < resourceLimits_.socketOutputBytes / 2;
}

void Peer::releaseTransfer(File &file, bool completed, dsfm::DurableCheckpoint checkpoint,
                           quint64 next_sequence, quint64 final_start)
{
    auto &channels = file.getDirection() == File::INCOMING ? inChannels_ : outChannels_;
    const auto found = channels.find(file.getChannel());
    if (found == channels.end()) return;
    const auto binding = std::dynamic_pointer_cast<SessionChannel>(found->second);
    if (!binding || !binding->owns(file)) return;
    if (completed && file.getDirection() == File::INCOMING && checkpoint.final) {
        binding->retain(checkpoint, next_sequence, final_start);
        retainCompletedIncoming(file.getChannel());
    } else channels.erase(found);
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
   while (!initial_offers_.empty()) forgetInitialOffer(initial_offers_.begin()->first);
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
    return sendMessageWithAttempts(message, {});
}

uint64_t Peer::sendMessageWithAttempts(const core::Message &message,
                                      const QVector<std::optional<dsfm::TransferAttempt>> &attempts)
{
    if (!attempts.isEmpty() && attempts.size() != qsizetype(message.getData().attachments.size()))
        throw Error("Recovery manifest attempt count does not match its attachments");
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
    qsizetype attachment_index = 0;
    for (const auto& attachment : message.getData().attachments) {
        ds::protocol::FileOffer offer;
        offer.setFileId(attachment.file_id); offer.setMessageId(message.getData().messageId);
        offer.setName(attachment.name); offer.setSize(attachment.size);
        offer.setSha256(attachment.sha256); offer.setMimeType(attachment.mime_type);
        const auto file = DsEngine::instance().getFileManager()->getFileFromPeer(
            attachment.file_id, getIdentityId(), getPeerCert() ? getPeerCert()->getHash().toByteArray() : QByteArray{}, File::OUTGOING);
        if (!file)
            throw Error("Outgoing attachment has no unambiguous scoped transfer");
        if (!attempts.isEmpty()) {
            const auto &attempt = attempts[attachment_index];
            if (!file || !attempt || !checkpointInterval() || attempt->interval != file->checkpointInterval() ||
                attempt->next_sequence != 1 ||
                !codec::validateCheckpointParameters(codec::checkpointParameters(*attempt)))
                throw Error("Invalid recovery manifest checkpoint parameters");
            offer.setCheckpoints(codec::checkpointParameters(*attempt));
        } else setOfferedCheckpoint(offer, *file);
        ++attachment_index;
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
    return sendFileOfferMetadata(file);
}

uint64_t Peer::repeatCheckpointOffer(const File &file, QUuid attempt)
{
    if (!checkpointInterval() || !file.checkpointInterval() || attempt.isNull() || file.isThumbnail() || file.getState() != File::FS_PAUSED)
        throw Error("Completion recovery offer is not admitted");
    return sendFileOfferMetadata(file, attempt);
}

uint64_t Peer::repeatCheckpointPreviewOffer(const File &primary, const File &preview,
                                           QUuid primary_attempt, QUuid preview_attempt)
{
    auto *contact = primary.getContact();
    auto *files = DsEngine::instance().getFileManager();
    const auto linked = files->thumbnailFor(primary);
    if (!checkpointInterval() || !connectionData_.identitysCert || primary_attempt.isNull() || preview_attempt.isNull() ||
        primary.isThumbnail() || !preview.isThumbnail() || !linked || linked->getId() != preview.getId() ||
        primary.getDirection() != File::OUTGOING || preview.getDirection() != File::OUTGOING ||
        primary.getContactId() != preview.getContactId() || primary.getIdentityId() != preview.getIdentityId() ||
        primary.getConversationId() != preview.getConversationId() || preview.dataThumbnailFor() != primary.getFileId() ||
        primary.getFileId() == preview.getFileId() || preview.getState() != File::FS_PAUSED ||
        primary.getState() == File::FS_CANCELLED || primary.getState() == File::FS_REJECTED ||
        primary.failureReason() != int(dsfm::TransferFailureReason::None) || preview.failureReason() != int(dsfm::TransferFailureReason::None) ||
        !primary.outgoingSourceCurrent() || !preview.outgoingSourceCurrent() || !contact ||
        !contact->canSendFileType(primary.getMimeType(), primary.getName()) || !contact->canSendFileType("image/jpeg", "preview.jpg") ||
        preview.getMimeType() != "image/jpeg" || preview.getSize() <= 0 || preview.getSize() > core::media_preview::max_encoded_bytes ||
        !primary.getConversation() || !primary.checkpointInterval() || !preview.checkpointInterval())
        throw Error("Preview completion recovery is not authorized");
    core::MessageData data;
    data.conversation = primary.getConversation()->getHash();
    data.messageId = files->thumbnailManifest(primary);
    data.composedTime = primary.getCreated(); data.encoding = core::Message::US_ACSII;
    data.attachments = {
        {primary.getFileId(), primary.getName(), quint64(primary.getSize()), primary.getHash(), primary.getMimeType(), {}},
        {preview.getFileId(), "preview.jpg", quint64(preview.getSize()), preview.getHash(), "image/jpeg", quint32{0}}};
    core::Message manifest{*this, std::move(data), core::Message::OUTGOING, primary.getConversationId()};
    manifest.sign(*connectionData_.identitysCert);
    return sendMessageWithAttempts(manifest, {
        dsfm::TransferAttempt{primary_attempt, primary.checkpointInterval(), 1, {}},
        dsfm::TransferAttempt{preview_attempt, preview.checkpointInterval(), 1, {}}});
}

uint64_t Peer::sendFileOfferMetadata(const File &file, QUuid repeated_attempt)
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
    if (!file.outgoingSourceCurrent()) reject("content authorization was revoked or expired");
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
    if (!file.isThumbnail() && repeated_attempt.isNull()) {
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
    if (repeated_attempt.isNull()) setOfferedCheckpoint(offer, file);
    else {
        const dsfm::TransferAttempt attempt{repeated_attempt, file.checkpointInterval(), 1, {}};
        if (!checkpointInterval() || !codec::validateCheckpointParameters(codec::checkpointParameters(attempt)))
            throw Error("Invalid completion recovery offer parameters");
        offer.setCheckpoints(codec::checkpointParameters(attempt));
    }
    ds::protocol::Envelope envelope;
    envelope.setProtocolVersion(codec::protocolVersion);
    envelope.setFileOffer(std::move(offer));
    return send(envelope);
}

void Peer::disableNotifications()
{
    notificationsDisabled_ = true;
}


}} // namespace
