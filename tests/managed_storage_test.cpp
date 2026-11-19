#include <QtTest>
#include <QCoroSignal>
#include <algorithm>
#include <QCryptographicHash>
#include <QEventLoop>
#include <QFile>
#include <QBuffer>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QSqlRecord>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QScopeGuard>
#include <QtEndian>
#include <QTcpServer>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <iostream>
#include <logfault/logfault.h>
#include "ds/dsclient.h"
#include "ds/dsserver.h"
#include "ds/peer.h"
#include "ds/crypto.h"
#include "ds/dsengine.h"
#include "ds/managedstorage.h"
#include "ds/incomingtransfer.h"
#include "ds/outgoingtransfer.h"
#include "ds/mediapreview.h"
#include "ds/resourcelimits.h"
#include "ds/notificationsmodel.h"

using namespace ds::core;
template<class T> T waitTask(QCoro::Task<T> task) {
    QEventLoop loop;
    std::optional<T> result;
    task.then([&](T value) { result.emplace(std::move(value)); loop.quit(); });
    if (!result) loop.exec();
    return std::move(*result);
}
class ReceivingPeer : public ds::prot::Peer {
  public:
    ReceivingPeer() : Peer(ds::prot::ConnectionSocket::create(), {}) {}
    Direction getDirection() const noexcept override { return OUTGOING; }
    bool isConnected() const noexcept override { return true; }
    void receive(quint32 channel, quint64 sequence, quint64 offset, const QByteArray &bytes, bool final) {
        QByteArray frame(file_chunk_header_bytes, Qt::Uninitialized);
        qToBigEndian(sequence, frame.data());
        qToBigEndian(offset, frame.data() + sizeof(quint64));
        frame.append(bytes);
        auto instance = inChannels_.at(channel);
        instance->onIncoming(*this, 0, mview_t{reinterpret_cast<uint8_t *>(frame.data()), size_t(frame.size())}, final);
    }
    uint64_t sendFileAck(const QByteArray &, quint64, quint64 offset, bool final) override {
        acknowledged = offset;
        completed = final;
        return 0;
    }
    quint64 acknowledged = 0;
    bool completed = false;
};
class OwnedReceivingPeer final : public ReceivingPeer {
public:
    File::ptr_t file;
    QByteArray payload;
    quint64 next_sequence = 0, next_offset = 0;
    bool sent_final = false, confirm_proposals = true, interrupt_checkpoint = false;
    int proposals = 0;
    size_t retainedCompletedCount() const { return completed_incoming_.size(); }
    size_t incomingChannelCount() const { return inChannels_.size(); }
    quint32 checkpointInterval() const noexcept override { return dsfm::minimum_checkpoint_interval; }
    uint64_t sendCheckpointOfferResponse(const QByteArray &, const dsfm::TransferAttempt &attempt) override {
        next_sequence = 0; sent_final = false;
        next_offset = checkpoint_fence_ = attempt.selected.offset;
        sendWindow(0); return 0;
    }
    uint64_t sendCheckpointResume(const QByteArray &, const QByteArray &, quint64, quint32 interval,
                                  QVector<dsfm::DurableCheckpoint> candidates, QDeadlineTimer) override {
        auto progress = waitTask(DsEngine::instance().getFileManager()->storage().transferProgress().load(file->checkpointKey().value()));
        const auto selected = *std::max_element(candidates.begin(), candidates.end(), [](const auto &a, const auto &b) { return a.offset < b.offset; });
        auto session = DsEngine::instance().transfers().find(file->checkpointKey().value());
        TransferSession::Event event; event.kind = TransferSession::Event::Kind::Resume; event.request = 1;
        event.selection = dsfm::TransferAttempt{QUuid::createUuid(), interval, progress.value().next_checkpoint_sequence, selected};
        session->deliver(session->connection(), session->attempt(), std::move(event));
        return 1;
    }
    uint64_t sendCheckpointAck(const QByteArray &, QUuid, quint64 sequence, quint64 offset, bool final) override {
        acknowledged = offset; completed = final;
        if (!final) sendWindow(sequence);
        return 0;
    }
    uint64_t sendFileCheckpoint(const QByteArray &, const QByteArray &, const dsfm::TransferAttempt &,
                                dsfm::DurableCheckpoint checkpoint, dsfm::CheckpointPhase phase) override {
        if (phase == dsfm::CheckpointPhase::Proposed) {
            ++proposals;
            if (confirm_proposals) {
                auto session = DsEngine::instance().transfers().find(file->checkpointKey().value());
                TransferSession::Event event; event.kind = TransferSession::Event::Kind::Checkpoint;
                event.checkpoint = checkpoint; event.checkpoint_phase = dsfm::CheckpointPhase::SenderConfirmed;
                session->deliver(session->connection(), session->attempt(), std::move(event));
            }
        } else if (phase == dsfm::CheckpointPhase::MutuallyConfirmed && !checkpoint.final) {
            checkpoint_fence_ = checkpoint.offset;
            if (interrupt_checkpoint) {
                DsEngine::instance().transfers().find(file->checkpointKey().value())->stop(TransferSession::Stop::Disconnected);
                return 0;
            }
            sendWindow(next_sequence);
        }
        return 0;
    }
    uint64_t sendFilePause(const QByteArray &, QUuid, dsfm::DurableCheckpoint, dsfm::TransferPauseReason) override { return 0; }
    uint64_t sendFileCancel(const QByteArray &, FileCancelReason) override { return 0; }
private:
    quint64 checkpoint_fence_ = 0;
    void sendWindow(quint64 acknowledged_sequence) {
        while (!sent_final && next_sequence - acknowledged_sequence < file_window_chunks &&
               next_offset - checkpoint_fence_ < dsfm::minimum_checkpoint_interval) {
            const auto length = qMin<quint64>(file_chunk_bytes, quint64(payload.size()) - next_offset);
            const auto offset = next_offset, sequence = next_sequence++;
            next_offset += length;
            sent_final = next_offset == quint64(payload.size());
            ReceivingPeer::receive(file->getChannel(), sequence, offset, payload.mid(qsizetype(offset), qsizetype(length)), sent_final);
        }
    }
};
class SendingPeer final : public ds::prot::Peer {
public:
    SendingPeer() : Peer(ds::prot::ConnectionSocket::create(), {}) {}
    Direction getDirection() const noexcept override { return OUTGOING; }
    bool isConnected() const noexcept override { return true; }
    quint32 checkpointInterval() const noexcept override { return dsfm::minimum_checkpoint_interval; }
    void begin(const File::ptr_t &file) {
        file_ = file;
        owner_ = std::make_shared<OutgoingTransfer>(file, std::static_pointer_cast<PeerConnection>(shared_from_this()), OutgoingTransferStart::Offer);
        if (!DsEngine::instance().transfers().submit(owner_)) throw std::runtime_error("Test sender admission failed");
    }
    uint64_t offerFile(const File &) override {
        TransferSession::Event event; event.kind = TransferSession::Event::Kind::Ready; event.selection = file_->checkpointAttempt();
        owner_->deliver(owner_->connection(), owner_->attempt(), std::move(event));
        return 0;
    }
    uint64_t sendCheckpointChunk(const QByteArray &, QUuid, quint64 sequence, quint64 offset, const QByteArray &bytes, bool final) override {
        sent.append(bytes); next_sequence_ = sequence + 1; next_offset_ = offset + quint64(bytes.size()); final_sent = final;
        if (!final && mutate) std::exchange(mutate, {})();
        if (!final) ack(false);
        if (final || next_offset_ % dsfm::minimum_checkpoint_interval == 0) {
            TransferSession::Event event; event.kind = TransferSession::Event::Kind::Checkpoint;
            event.checkpoint = {file_->getSize() == 0 ? 0 : checkpoint_sequence_++, next_offset_,
                QCryptographicHash::hash(sent, QCryptographicHash::Sha256), final, false};
            event.checkpoint_phase = dsfm::CheckpointPhase::Proposed;
            owner_->deliver(owner_->connection(), owner_->attempt(), std::move(event));
        }
        return 0;
    }
    uint64_t sendFileCheckpoint(const QByteArray &, const QByteArray &, const dsfm::TransferAttempt &,
                                dsfm::DurableCheckpoint checkpoint, dsfm::CheckpointPhase) override {
        TransferSession::Event event; event.kind = TransferSession::Event::Kind::Checkpoint;
        event.checkpoint = checkpoint; event.checkpoint_phase = dsfm::CheckpointPhase::MutuallyConfirmed;
        owner_->deliver(owner_->connection(), owner_->attempt(), std::move(event));
        if (checkpoint.final) ack(true);
        return 0;
    }
    uint64_t sendFileCancel(const QByteArray &, FileCancelReason reason) override { cancelled = reason == FileCancelReason::SourceChanged; return 0; }
    uint64_t sendFilePause(const QByteArray &, QUuid, dsfm::DurableCheckpoint, dsfm::TransferPauseReason) override { return 0; }
    QByteArray sent;
    bool final_sent = false, cancelled = false;
    std::function<void()> mutate;
private:
    void ack(bool final) {
        TransferSession::Event event; event.kind = TransferSession::Event::Kind::Ack;
        event.sequence = next_sequence_; event.offset = next_offset_; event.final = final;
        owner_->deliver(owner_->connection(), owner_->attempt(), std::move(event));
    }
    File::ptr_t file_;
    std::shared_ptr<OutgoingTransfer> owner_;
    quint64 next_sequence_ = 0, next_offset_ = 0, checkpoint_sequence_ = 1;
};
class InitialOfferPeer final : public ds::prot::Peer {
  public:
    InitialOfferPeer() : Peer(ds::prot::ConnectionSocket::create(), {}) {}
    Direction getDirection() const noexcept override { return OUTGOING; }
    quint32 checkpointInterval() const noexcept override { return dsfm::minimum_checkpoint_interval; }
    void encode(const File &file) {
        ds::protocol::FileOffer offer;
        setOfferedCheckpoint(offer, file);
    }
};
class CheckpointFixtureReader final : public dsfm::IFileReader {
  public:
    explicit CheckpointFixtureReader(QByteArray bytes) : bytes_(std::move(bytes)) {}
    QCoro::Task<dsfm::Result<QByteArray>> readAt(quint64 offset, qsizetype count, dsfm::CancellationToken cancel) override {
        if (cancel.cancelled()) co_return dsfm::Error{dsfm::ErrorCode::Cancelled};
        if (count < 0 || count > 65536 || offset > quint64(bytes_.size()))
            co_return dsfm::Error{dsfm::ErrorCode::InvalidArgument};
        co_return bytes_.mid(qsizetype(offset), count);
    }
    QCoro::Task<dsfm::Result<void>> close() override { co_return dsfm::Result<void>{}; }
  private:
    QByteArray bytes_;
};
class TransferIoGate : public QObject {
    Q_OBJECT
public:
    bool entered = false;
signals:
    void released();
};
class SuspendedTransferReader final : public dsfm::IFileReader {
public:
    SuspendedTransferReader(QByteArray bytes, std::shared_ptr<TransferIoGate> gate) : bytes_{std::move(bytes)}, gate_{std::move(gate)} {}
    QCoro::Task<dsfm::Result<QByteArray>> readAt(quint64 offset, qsizetype count, dsfm::CancellationToken) override {
        gate_->entered = true;
        co_await qCoro(gate_.get(), &TransferIoGate::released);
        co_return bytes_.mid(qsizetype(offset), count);
    }
    QCoro::Task<dsfm::Result<void>> close() override { co_return dsfm::Result<void>{}; }
private:
    QByteArray bytes_;
    std::shared_ptr<TransferIoGate> gate_;
};
class ResumePeer final : public ds::prot::Peer {
  public:
    ResumePeer() : Peer(ds::prot::ConnectionSocket::create(), {}) {}
    Direction getDirection() const noexcept override { return OUTGOING; }
    bool isConnected() const noexcept override { return true; }
    quint32 checkpointInterval() const noexcept override { return dsfm::minimum_checkpoint_interval; }
    uint64_t sendUserInfo(const UserInfo &) override { return 0; }

};
class LoopbackListener final : public QTcpServer {
  public:
    std::function<void(ds::prot::ConnectionSocket::ptr_t)> accepted;

  protected:
    void incomingConnection(qintptr descriptor) override {
        auto socket = ds::prot::ConnectionSocket::create();
        if (socket->setSocketDescriptor(descriptor))
            accepted(std::move(socket));
    }
};
class CheckpointWireClient final : public ds::prot::DsClient {
  public:
    using DsClient::DsClient;
    quint32 preference = dsfm::default_checkpoint_interval;
    quint64 admitted_end = 0;
    quint64 payload_bytes = 0;
    bool duplicate_recovery_offers = false;
    bool select_earlier_completion = false;
    uint64_t sendCheckpointResumeResult(quint64 request_id, const QByteArray &file_id, FileResumeStatus status,
                                         std::optional<dsfm::TransferAttempt> attempt, dsfm::TransferPauseReason pause,
                                         FileCancelReason error) override {
        if (select_earlier_completion && status == FileResumeStatus::Accepted && attempt)
            attempt->selected = {0, 0, QCryptographicHash::hash(QByteArray{}, QCryptographicHash::Sha256), false, false};
        return DsClient::sendCheckpointResumeResult(request_id, file_id, status, attempt, pause, error);
    }
    uint64_t repeatCheckpointOffer(const File &file, QUuid attempt) override {
        const auto result = DsClient::repeatCheckpointOffer(file, attempt);
        if (duplicate_recovery_offers)
            for (int repeat = 0; repeat < 7; ++repeat) DsClient::repeatCheckpointOffer(file, attempt);
        return result;
    }
    uint64_t sendCheckpointChunk(const QByteArray &file_id, QUuid attempt, quint64 sequence,
                                quint64 offset, const QByteArray &bytes, bool final) override {
        admitted_end = qMax(admitted_end, offset + quint64(bytes.size()));
        payload_bytes += quint64(bytes.size());
        return DsClient::sendCheckpointChunk(file_id, attempt, sequence, offset, bytes, final);
    }
  protected:
    quint32 localCheckpointPreference() const noexcept override { return preference; }
};
class CheckpointWireServer final : public ds::prot::DsServer {
  public:
    using DsServer::DsServer;
    quint32 preference = dsfm::default_checkpoint_interval;
    bool withhold_proposals = false;
    bool withhold_receipts = false;
    bool final_receipts_only = false;
    bool withhold_final_ack = false;
    QByteArray withheld_file_id;
    uint64_t sendCheckpointAck(const QByteArray &file_id, QUuid attempt, quint64 sequence,
                              quint64 offset, bool final) override {
        if (final && withhold_final_ack) return 0;
        return DsServer::sendCheckpointAck(file_id, attempt, sequence, offset, final);
    }
    std::function<void()> withheld;
    uint64_t sendFileCheckpoint(const QByteArray &file_id, const QByteArray &sha256,
                                const dsfm::TransferAttempt &attempt, dsfm::DurableCheckpoint checkpoint,
                                dsfm::CheckpointPhase phase) override {
        if ((withheld_file_id.isEmpty() || withheld_file_id == file_id) &&
            ((withhold_proposals && phase == dsfm::CheckpointPhase::Proposed) ||
            (withhold_receipts && phase == dsfm::CheckpointPhase::MutuallyConfirmed &&
             (!final_receipts_only || checkpoint.final)))) {
            withheld = [this, file_id, sha256, attempt, checkpoint, phase] {
                DsServer::sendFileCheckpoint(file_id, sha256, attempt, checkpoint, phase);
            };
            return 0;
        }
        return DsServer::sendFileCheckpoint(file_id, sha256, attempt, checkpoint, phase);
    }
  protected:
    quint32 localCheckpointPreference() const noexcept override { return preference; }
};
class QueueProtocol final : public ProtocolManager {
  public:
    int attempts = 0;
    State getState() const override { return ONLINE; }
    QByteArray getPeerHandle(const QUuid &, const QUuid &) override { return {}; }
    void start() override {}
    void stop() override {}
    void createTransportHandle(const TransportHandleReq &) override {}
    void startService(const QUuid &, const ds::crypto::DsCert::ptr_t &, const QVariantMap &) override {}
    void stopService(const QUuid &) override {}
    PeerConnection::ptr_t connectTo(ConnectData) override {
        ++attempts;
        return std::make_shared<ReceivingPeer>();
    }
    uint64_t sendAddme(const AddmeReq &) override { return 0; }
};
class ManagedStorageTest final : public QObject {
    Q_OBJECT
    auto approveFixtureContact() {
        const auto previous = contact_->getState();
        contact_->setState(Contact::ACCEPTED);
        return qScopeGuard([this, previous] { contact_->setState(previous); });
    }
  private slots:
    void initTestCase() {
        if (qEnvironmentVariableIsSet("DS_TEST_TRACE"))
            logfault::LogManager::Instance().AddHandler(
                std::make_unique<logfault::StreamHandler>(std::clog, logfault::LogLevel::TRACE));
        QVERIFY(directory_.isValid());
        qputenv("DARKSPEAK_PROFILE_ROOT", directory_.path().toUtf8());
        qputenv("DARKSPEAK_TEST_SECRETSTORE_PASSPHRASE", "managed-storage-disposable-fixture");
        crypto_ = std::make_unique<ds::crypto::Crypto>();
        auto settings = std::make_unique<QSettings>(directory_.filePath("settings.ini"), QSettings::IniFormat);
        settings->setValue("dbpath", directory_.filePath("app.sqlite"));
        settings->setValue("downloadLocation", directory_.filePath("downloads"));
        QDir().mkdir(directory_.filePath("downloads"));
        engine_ = std::make_unique<DsEngine>(std::move(settings));
        auto initialized = waitTask(storage().ready());
        QVERIFY2(initialized, "Managed namespaces failed to initialize");
        IdentityData identity;
        identity.uuid = QUuid::createUuid();
        identity.name = "Storage fixture";
        identity.cert = ds::crypto::DsCert::create();
        identity.hash = identity.cert->getHash().toByteArray();
        identity.autoConnect = false;
        identity_ = engine_->getIdentityManager()->addIdentity(identity);
        auto contact = std::make_unique<ContactData>();
        contact->identity = identity_->getId();
        contact->name = "Fixture peer";
        const auto cert = ds::crypto::DsCert::create();
        contact->cert = ds::crypto::DsCert::createFromPubkey(cert->getSigningPubKey().toByteArray());
        contact->address = "fixture-not-networked";
        contact->autoConnect = false;
        contact_ = engine_->getContactManager()->addContact(std::move(contact));
        conversation_ = engine_->getConversationManager()->getConversation(contact_);
    }
    void coroutine_receiver_verifies_and_publishes_content() {
        auto approved = approveFixtureContact();
        for (const auto payload : {QByteArray{}, QByteArray(2 * dsfm::minimum_checkpoint_interval + 7, 'o')}) {
            auto peer = std::make_shared<OwnedReceivingPeer>();
            auto file = incoming(payload, "owned-receiver.bin", "application/octet-stream", peer->getConnectionId());
            peer->file = file; peer->payload = payload;
            auto progress = waitTask(file->prepareCheckpointProgress(dsfm::minimum_checkpoint_interval)); QVERIFY(progress);
            dsfm::TransferAttempt attempt{QUuid::createUuid(), dsfm::minimum_checkpoint_interval, 1, progress.value().checkpoints.first()};
            QVERIFY(file->setCheckpointAttempt(attempt));
            auto session = std::make_shared<IncomingTransfer>(file, peer, IncomingTransferStart::Accept);
            QVERIFY(engine_->transfers().submit(session));
            QTRY_VERIFY_WITH_TIMEOUT(session->outcome(), 10000);
            QCOMPARE(session->outcome()->kind, TransferSession::Outcome::Kind::Completed);
            QCOMPARE(file->getState(), File::FS_DONE); QVERIFY(peer->completed);
            auto journal = waitTask(storage().transferProgress().load(file->checkpointKey().value())); QVERIFY(journal);
            QVERIFY(journal.value().complete);
            auto reader = waitTask(storage().manager().openRead(file->storageNamespace(), file->storageId())); QVERIFY(reader);
            QCOMPARE(waitTask(reader.value()->readAt(0, payload.size())).value(), payload);
            QVERIFY(waitTask(reader.value()->close()));
            QCOMPARE(engine_->transfers().active(identity_->getUuid(), contact_->getUuid()), size_t{0});
        }
    }
    void coroutine_receiver_cancel_joins_and_aborts_unpublished_content() {
        auto approved = approveFixtureContact();
        const QByteArray payload(dsfm::minimum_checkpoint_interval + 7, 'c');
        auto peer = std::make_shared<OwnedReceivingPeer>();
        auto file = incoming(payload, "owned-cancel.bin", "application/octet-stream", peer->getConnectionId());
        peer->file = file; peer->payload = payload; peer->confirm_proposals = false;
        auto progress = waitTask(file->prepareCheckpointProgress(dsfm::minimum_checkpoint_interval)); QVERIFY(progress);
        dsfm::TransferAttempt attempt{QUuid::createUuid(), dsfm::minimum_checkpoint_interval, 1, progress.value().checkpoints.first()};
        QVERIFY(file->setCheckpointAttempt(attempt));
        auto session = std::make_shared<IncomingTransfer>(file, peer, IncomingTransferStart::Accept);
        QVERIFY(engine_->transfers().submit(session));
        QTRY_COMPARE_WITH_TIMEOUT(peer->proposals, 1, 10000);
        file->cancel();
        QTRY_VERIFY_WITH_TIMEOUT(session->outcome(), 10000);
        QCOMPARE(session->outcome()->kind, TransferSession::Outcome::Kind::Cancelled);
        QCOMPARE(file->getState(), File::FS_CANCELLED); QVERIFY(!peer->completed);
        QVERIFY(!waitTask(storage().manager().stat(file->storageNamespace(), file->storageId())));
        QCOMPARE(engine_->transfers().active(identity_->getUuid(), contact_->getUuid()), size_t{0});
    }
    void coroutine_receiver_resumes_verified_durable_prefix() {
        auto approved = approveFixtureContact();
        const QByteArray payload(2 * dsfm::minimum_checkpoint_interval + 7, 'r');
        auto peer = std::make_shared<OwnedReceivingPeer>(); peer->payload = payload; peer->interrupt_checkpoint = true;
        auto file = incoming(payload, "owned-resume.bin", "application/octet-stream", peer->getConnectionId()); peer->file = file;
        auto progress = waitTask(file->prepareCheckpointProgress(dsfm::minimum_checkpoint_interval)); QVERIFY(progress);
        dsfm::TransferAttempt attempt{QUuid::createUuid(), dsfm::minimum_checkpoint_interval, 1, progress.value().checkpoints.first()};
        QVERIFY(file->setCheckpointAttempt(attempt));
        auto first = std::make_shared<IncomingTransfer>(file, peer, IncomingTransferStart::Accept);
        QVERIFY(engine_->transfers().submit(first));
        QTRY_VERIFY_WITH_TIMEOUT(first->outcome(), 10000);
        QCOMPARE(file->getState(), File::FS_PAUSED);
        auto saved = waitTask(storage().transferProgress().load(file->checkpointKey().value())); QVERIFY(saved);
        QCOMPARE(saved.value().verified_prefix, quint64(dsfm::minimum_checkpoint_interval));
        QTRY_VERIFY(!engine_->transfers().find(file->checkpointKey().value()));
        auto reconnected = std::make_shared<OwnedReceivingPeer>(); reconnected->file = file; reconnected->payload = payload;
        auto resumed = std::make_shared<IncomingTransfer>(file, reconnected, IncomingTransferStart::Resume);
        QVERIFY(engine_->transfers().submit(resumed));
        QTRY_VERIFY_WITH_TIMEOUT(resumed->outcome(), 10000);
        QCOMPARE(resumed->outcome()->kind, TransferSession::Outcome::Kind::Completed);
        QVERIFY(file->checkpointAttempt()->id != attempt.id);
        QCOMPARE(file->checkpointAttempt()->selected.offset, quint64(dsfm::minimum_checkpoint_interval));
        QCOMPARE(file->getState(), File::FS_DONE);
        QCOMPARE(reconnected->next_offset, quint64(payload.size()));
    }
    void coroutine_receiver_revocation_stops_admission_synchronously() {
        auto approved = approveFixtureContact();
        const QByteArray payload(dsfm::minimum_checkpoint_interval + 7, 'a');
        auto peer = std::make_shared<OwnedReceivingPeer>(); peer->payload = payload; peer->confirm_proposals = false;
        auto file = incoming(payload, "owned-revocation.bin", "application/octet-stream", peer->getConnectionId()); peer->file = file;
        auto progress = waitTask(file->prepareCheckpointProgress(dsfm::minimum_checkpoint_interval)); QVERIFY(progress);
        dsfm::TransferAttempt attempt{QUuid::createUuid(), dsfm::minimum_checkpoint_interval, 1, progress.value().checkpoints.first()};
        QVERIFY(file->setCheckpointAttempt(attempt));
        auto session = std::make_shared<IncomingTransfer>(file, peer, IncomingTransferStart::Accept);
        QVERIFY(engine_->transfers().submit(session));
        QTRY_COMPARE_WITH_TIMEOUT(peer->proposals, 1, 10000);
        identity_->setAllowFiles(false);
        QVERIFY(!session->accepting());
        QTRY_VERIFY_WITH_TIMEOUT(session->outcome(), 10000);
        QCOMPARE(session->outcome()->pause, dsfm::TransferPauseReason::AuthorizationWithdrawn);
        QCOMPARE(file->getState(), File::FS_PAUSED);
        identity_->setAllowFiles(true);
        QVERIFY(!waitTask(storage().manager().stat(file->storageNamespace(), file->storageId())));
    }
    void coroutine_receiver_rejects_unconfirmed_payload_and_forged_confirmation() {
        auto approved = approveFixtureContact();
        for (const bool payload_attack : {false, true}) {
            const QByteArray payload(dsfm::minimum_checkpoint_interval + 7, 'f');
            auto peer = std::make_shared<OwnedReceivingPeer>(); peer->payload = payload; peer->confirm_proposals = false;
            auto file = incoming(payload, "owned-forgery.bin", "application/octet-stream", peer->getConnectionId()); peer->file = file;
            auto progress = waitTask(file->prepareCheckpointProgress(dsfm::minimum_checkpoint_interval)); QVERIFY(progress);
            dsfm::TransferAttempt attempt{QUuid::createUuid(), dsfm::minimum_checkpoint_interval, 1, progress.value().checkpoints.first()};
            QVERIFY(file->setCheckpointAttempt(attempt));
            auto owner = std::make_shared<IncomingTransfer>(file, peer, IncomingTransferStart::Accept);
            QVERIFY(engine_->transfers().submit(owner));
            QTRY_COMPARE_WITH_TIMEOUT(peer->proposals, 1, 5000);
            TransferSession::Event event;
            if (payload_attack) {
                event.kind = TransferSession::Event::Kind::Chunk; event.sequence = 8;
                event.offset = dsfm::minimum_checkpoint_interval; event.bytes = payload.last(7); event.final = true;
            } else {
                event.kind = TransferSession::Event::Kind::Checkpoint;
                event.checkpoint = {1, dsfm::minimum_checkpoint_interval, QByteArray(32, 'x'), false, false};
                event.checkpoint_phase = dsfm::CheckpointPhase::SenderConfirmed;
            }
            QVERIFY(owner->deliver(owner->connection(), owner->attempt(), std::move(event)));
            QTRY_VERIFY_WITH_TIMEOUT(owner->outcome(), 5000);
            QVERIFY(!peer->completed);
            auto saved = waitTask(storage().transferProgress().load(file->checkpointKey().value())); QVERIFY(saved);
            QCOMPARE(saved.value().confirmed_offset, quint64{0}); QVERIFY(!saved.value().complete);
            QVERIFY(!waitTask(storage().manager().openRead(file->storageNamespace(), file->storageId())));
        }
    }
    void coroutine_receiver_cancellation_respects_native_publication_boundary() {
        auto approved = approveFixtureContact();
        for (const bool before : {true, false}) {
            const QByteArray payload("publication boundary");
            auto peer = std::make_shared<OwnedReceivingPeer>(); peer->payload = payload;
            auto file = incoming(payload, "owned-publication.bin", "application/octet-stream", peer->getConnectionId()); peer->file = file;
            auto progress = waitTask(file->prepareCheckpointProgress(dsfm::minimum_checkpoint_interval)); QVERIFY(progress);
            dsfm::TransferAttempt attempt{QUuid::createUuid(), dsfm::minimum_checkpoint_interval, 1, progress.value().checkpoints.first()};
            QVERIFY(file->setCheckpointAttempt(attempt));
            int cancellations = 0;
            QObject observation;
            connect(file.get(), &File::stateChanged, &observation, [&, file] {
                if (file->getState() == (before ? File::FS_HASHING : File::FS_DONE)) { ++cancellations; file->cancel(); }
            });
            auto owner = std::make_shared<IncomingTransfer>(file, peer, IncomingTransferStart::Accept);
            QVERIFY(engine_->transfers().submit(owner));
            QTRY_VERIFY_WITH_TIMEOUT(owner->outcome(), 5000);
            QCOMPARE(cancellations, 1);
            QCOMPARE(owner->outcome()->kind, before ? TransferSession::Outcome::Kind::Cancelled : TransferSession::Outcome::Kind::Completed);
            auto readable = waitTask(storage().manager().openRead(file->storageNamespace(), file->storageId()));
            QCOMPARE(bool(readable), !before);
            if (readable) { QCOMPARE(waitTask(readable.value()->readAt(0, payload.size())).value(), payload); QVERIFY(waitTask(readable.value()->close())); }
        }
    }
    void coroutine_completed_receipts_have_a_fixed_retention_bound() {
        auto approved = approveFixtureContact();
        auto peer = std::make_shared<OwnedReceivingPeer>();
        for (int i = 0; i < 3; ++i) {
            auto file = incoming({}, QStringLiteral("owned-receipt-%1.bin").arg(i)); peer->file = file; peer->payload = {};
            QVERIFY(waitTask(file->prepareCheckpointProgress(dsfm::minimum_checkpoint_interval)));
            auto owner = std::make_shared<IncomingTransfer>(file, peer, IncomingTransferStart::Resume);
            QVERIFY(engine_->transfers().submit(owner));
            QTRY_VERIFY_WITH_TIMEOUT(owner->outcome(), 5000);
            QCOMPARE(owner->outcome()->kind, TransferSession::Outcome::Kind::Completed);
        }
        QCOMPARE(peer->retainedCompletedCount(), size_t{2}); QCOMPARE(peer->incomingChannelCount(), size_t{2});
    }
    void coroutine_sender_retains_admission_through_cancelled_io() {
        auto approved = approveFixtureContact();
        for (const bool preparation : {true, false}) {
            const QByteArray payload(16384, 'i');
            auto gate = std::make_shared<TransferIoGate>();
            auto data = std::make_unique<FileData>(); data->state = File::FS_WAITING;
            data->identity = identity_->getId(); data->contact = contact_->getId(); data->conversation = conversation_->getId();
            data->name = "owned-held-io.bin"; data->size = payload.size(); data->hash = QCryptographicHash::hash(payload, QCryptographicHash::Sha256);
            auto file = engine_->getFileManager()->addFile(std::move(data));
            QVERIFY(file->setPreparedSource(identity_->getUuid(), contact_->getUuid(), [] { return true; },
                [gate, preparation, payload, hash = file->getHash()](dsfm::ReadPreparationOptions) -> QCoro::Task<dsfm::Result<dsfm::PreparedRead>> {
                    if (preparation) { gate->entered = true; co_await qCoro(gate.get(), &TransferIoGate::released); }
                    dsfm::FileInfo info; info.size = quint64(payload.size()); info.sha256 = hash;
                    if (preparation) co_return dsfm::PreparedRead{info, std::make_unique<CheckpointFixtureReader>(payload)};
                    co_return dsfm::PreparedRead{info, std::make_unique<SuspendedTransferReader>(payload, gate)};
                }));
            auto peer = std::make_shared<SendingPeer>(); peer->begin(file);
            QTRY_VERIFY_WITH_TIMEOUT(gate->entered, 5000);
            auto owner = engine_->transfers().find(file->checkpointKey().value()); QVERIFY(owner);
            file->cancel(); QVERIFY(!owner->accepting());
            QVERIFY(!engine_->transfers().reserve(file->checkpointKey().value()));
            QCOMPARE(engine_->transfers().active(identity_->getUuid(), contact_->getUuid()), size_t{1});
            QVERIFY(!owner->outcome());
            emit gate->released();
            QTRY_VERIFY_WITH_TIMEOUT(owner->outcome(), 5000);
            QCOMPARE(owner->outcome()->kind, TransferSession::Outcome::Kind::Cancelled);
            QCOMPARE(engine_->transfers().active(identity_->getUuid(), contact_->getUuid()), size_t{0});
        }
    }
    void initial_offer_budget_reclaims_evicted_files_data() {
        QTest::addColumn<QString>("outcome");
        QTest::newRow("cancel") << QString("cancel");
        QTest::newRow("failure") << QString("failure");
        QTest::newRow("delete") << QString("delete");
    }
    void initial_offer_budget_reclaims_evicted_files() {
        QFETCH(QString, outcome);
        auto peer = std::make_shared<InitialOfferPeer>();
        QVector<int> rows;
        auto cleanup = qScopeGuard([&] {
            for (const auto id : rows) {
                QSqlQuery query; query.prepare("DELETE FROM file WHERE id=?"); query.addBindValue(id); query.exec();
            }
        });
        const auto make = [&] {
            auto data = std::make_unique<FileData>(); data->state = File::FS_OFFERED;
            data->identity = identity_->getId(); data->contact = contact_->getId(); data->conversation = conversation_->getId();
            data->name = "initial-offer-budget"; data->size = 1;
            data->hash = QCryptographicHash::hash(QByteArray("x"), QCryptographicHash::Sha256);
            const auto file = engine_->getFileManager()->addFile(std::move(data)); rows.append(file->getId());
            auto progress = waitTask(file->prepareCheckpointProgress(dsfm::minimum_checkpoint_interval));
            if (!progress) throw std::runtime_error("Cannot create initial-offer fixture progress");
            const dsfm::TransferAttempt attempt{QUuid::createUuid(), dsfm::minimum_checkpoint_interval, 1,
                                                progress.value().checkpoints.first()};
            if (!waitTask(storage().transferProgress().beginAttempt(file->checkpointKey().value(), attempt.id, attempt.selected)) ||
                !file->setCheckpointAttempt(attempt)) throw std::runtime_error("Cannot prepare initial-offer fixture attempt");
            return file;
        };
        std::weak_ptr<File> first;
        QByteArray first_id;
        for (quint32 i = 0; i < ResourceLimits::hardQueuedFileOffers; ++i) {
            auto file = make();
            if (!i) { first = file; first_id = file->getFileId(); }
            peer->encode(*file);
        }
        QVERIFY(first.expired()); // The offer registry must survive ordinary cache eviction.
        auto extra = make();
        QVERIFY_EXCEPTION_THROWN(peer->encode(*extra), std::runtime_error);
        // A distinct incoming row with the same wire identity cannot release
        // this outgoing admission slot.
        auto other_data = std::make_unique<FileData>(); other_data->state = File::FS_OFFERED;
        other_data->direction = File::INCOMING; other_data->fileId = first_id;
        other_data->identity = identity_->getId(); other_data->contact = contact_->getId(); other_data->conversation = conversation_->getId();
        other_data->name = "opposite-direction";
        auto other = engine_->getFileManager()->addFile(std::move(other_data)); rows.append(other->getId());
        other->setState(File::FS_CANCELLED);
        QVERIFY_EXCEPTION_THROWN(peer->encode(*extra), std::runtime_error);
        other->deleteFromDb();
        QVERIFY_EXCEPTION_THROWN(peer->encode(*extra), std::runtime_error);
        const auto restored = engine_->getFileManager()->getFile(rows.first());
        QVERIFY(!restored->checkpointAttempt());
        if (outcome == "cancel") { restored->cancel(); QTRY_COMPARE_WITH_TIMEOUT(restored->getState(), File::FS_CANCELLED, 5000); }
        else if (outcome == "failure") restored->transferFailed("Initial-offer fixture failure", File::FS_FAILED);
        else restored->deleteFromDb();
        try { peer->encode(*extra); }
        catch (const std::exception &error) { QFAIL(error.what()); }
    }
    void rejected_checkpoint_selection_preserves_partial_content() {
        const QByteArray bytes("retained partial content");
        auto file = incoming(bytes, "checkpoint-rejected-selection.bin");
        QVERIFY(waitTask(file->prepareIncoming()));
        auto progress = waitTask(file->prepareCheckpointProgress(dsfm::minimum_checkpoint_interval));
        QVERIFY(progress);
        const auto key = file->checkpointKey().value();
        const auto zero = progress.value().checkpoints.first();
        const dsfm::TransferAttempt original{QUuid::createUuid(), dsfm::minimum_checkpoint_interval, 1, zero};
        QVERIFY(waitTask(storage().transferProgress().beginAttempt(key, original.id, zero)));
        QVERIFY(file->setCheckpointAttempt(original));
        QVERIFY(waitTask(file->writeIncoming(0, bytes)));
        QVERIFY(waitTask(file->checkpointIncomingPrefix(bytes.size())));
        const dsfm::DurableCheckpoint prefix{1, 3,
            QCryptographicHash::hash(bytes.first(3), QCryptographicHash::Sha256), false, true};
        QVERIFY(waitTask(file->recordCheckpoint(original.id, prefix, dsfm::CheckpointPhase::MutuallyConfirmed)));

        // Valid digest and boundary shape do not authorize deleting a suffix.
        const dsfm::DurableCheckpoint unknown{2, 2,
            QCryptographicHash::hash(bytes.first(2), QCryptographicHash::Sha256), false, true};
        dsfm::TransferAttempt replacement{QUuid::createUuid(), original.interval, 3, unknown};
        auto rejected = waitTask(file->selectIncomingCheckpoint(replacement));
        QVERIFY(!rejected);
        QCOMPARE(rejected.error().code, dsfm::ErrorCode::IntegrityError);
        auto whole = waitTask(file->checkpointIncomingPrefix(bytes.size()));
        QVERIFY(whole);
        QCOMPARE(whole.value(), file->getHash());
        QCOMPARE(*file->checkpointAttempt(), original);

        // Even a retained zero must pass sequence admission before truncation.
        replacement.selected = zero;
        replacement.next_sequence = 1;
        rejected = waitTask(file->selectIncomingCheckpoint(replacement));
        QVERIFY(!rejected);
        QCOMPARE(rejected.error().code, dsfm::ErrorCode::InvalidArgument);
        whole = waitTask(file->checkpointIncomingPrefix(bytes.size()));
        QVERIFY(whole);
        QCOMPARE(whole.value(), file->getHash());
        auto durable = waitTask(storage().transferProgress().load(key));
        QVERIFY(durable);
        QCOMPARE(durable.value().attempt, original.id);
        QCOMPARE(durable.value().confirmed_offset, prefix.offset);

        replacement.selected = prefix;
        replacement.next_sequence = 2;
        QVERIFY(waitTask(file->selectIncomingCheckpoint(replacement)));
        QCOMPARE(*file->checkpointAttempt(), replacement);
        QCOMPARE(file->getBytesTransferred(), qlonglong(prefix.offset));
        QVERIFY(!waitTask(file->checkpointIncomingPrefix(bytes.size())));
        QVERIFY(waitTask(file->checkpointIncomingPrefix(prefix.offset)));
        file->cancel();
        file->deleteFromDb();
    }
    void superseded_checkpoint_selection_does_not_restore_an_old_attempt() {
        const QByteArray bytes("partial content");
        auto file = incoming(bytes, "checkpoint-superseded-selection.bin");
        QVERIFY(waitTask(file->prepareIncoming()));
        auto progress = waitTask(file->prepareCheckpointProgress(dsfm::minimum_checkpoint_interval));
        QVERIFY(progress);
        const auto key = file->checkpointKey().value();
        const dsfm::TransferAttempt original{QUuid::createUuid(), dsfm::minimum_checkpoint_interval, 1,
                                             progress.value().checkpoints.first()};
        QVERIFY(waitTask(storage().transferProgress().beginAttempt(key, original.id, original.selected)));
        QVERIFY(file->setCheckpointAttempt(original));
        QVERIFY(waitTask(file->writeIncoming(0, bytes)));
        QVERIFY(waitTask(file->checkpointIncomingPrefix(bytes.size())));
        auto replacement = original;
        replacement.id = QUuid::createUuid();
        std::optional<dsfm::Result<void>> result;
        file->selectIncomingCheckpoint(replacement).then([&](auto value) { result.emplace(std::move(value)); });
        QVERIFY(!result.has_value());
        QCOMPARE(*file->checkpointAttempt(), original); // Validation never installs an unverified attempt.
        auto superseding = original;
        superseding.id = QUuid::createUuid();
        QVERIFY(file->setCheckpointAttempt(superseding));
        QTRY_VERIFY_WITH_TIMEOUT(result.has_value(), 5000);
        QVERIFY(!*result);
        QCOMPARE(result->error().code, dsfm::ErrorCode::Cancelled);
        QCOMPARE(*file->checkpointAttempt(), superseding);
        auto whole = waitTask(file->checkpointIncomingPrefix(bytes.size()));
        QVERIFY(whole);
        QCOMPARE(whole.value(), file->getHash());
        auto durable = waitTask(storage().transferProgress().load(key));
        QVERIFY(durable);
        QCOMPARE(durable.value().attempt, original.id);
        file->cancel();
        file->deleteFromDb();
    }
    void checkpoint_pause_retains_partial_and_rejects_stale_attempt() {
        auto file = incoming("abcdef", "pause-fixture");
        auto saved = waitTask(file->prepareCheckpointProgress(dsfm::minimum_checkpoint_interval));
        QVERIFY(saved);
        auto key = file->checkpointKey().value();
        dsfm::TransferAttempt attempt{QUuid::createUuid(), dsfm::minimum_checkpoint_interval, 1,
                                      saved.value().checkpoints.first()};
        QVERIFY(waitTask(storage().transferProgress().beginAttempt(key, attempt.id, attempt.selected)));
        QVERIFY(file->setCheckpointAttempt(attempt));
        QVERIFY(waitTask(file->prepareIncoming()));
        QVERIFY(waitTask(file->writeIncoming(0, "abc")));
        QVERIFY(waitTask(file->checkpointIncomingPrefix(3)));
        const auto pending = file->pendingId();
        file->setState(File::FS_TRANSFERRING);
        QVERIFY(waitTask(file->pauseTransfer(attempt.id, dsfm::TransferPauseReason::SourceUnavailable)));
        QCOMPARE(file->getState(), File::FS_PAUSED);
        QVERIFY(file->isResumable());
        QCOMPARE(file->dataPauseReason(), dsfm::TransferPauseReason::SourceUnavailable);
        QCOMPARE(file->pendingId(), pending);
        auto durable = waitTask(storage().transferProgress().load(key));
        QVERIFY(durable);
        QCOMPARE(durable.value().pause, dsfm::TransferPauseReason::SourceUnavailable);
        QVERIFY(!durable.value().complete);
        // Reopen independently from the live object's runtime attempt.
        auto loaded = File::load(*engine_->getFileManager(), file->getId());
        QCOMPARE(loaded->getState(), File::FS_PAUSED);
        QCOMPARE(loaded->checkpointInterval(), dsfm::minimum_checkpoint_interval);
        QCOMPARE(loaded->dataPauseReason(), dsfm::TransferPauseReason::SourceUnavailable);
        QVERIFY(!loaded->checkpointAttempt());
        QVERIFY(waitTask(file->prepareIncoming()));
        auto replacement = attempt; replacement.id = QUuid::createUuid();
        QVERIFY(waitTask(storage().transferProgress().beginAttempt(key, replacement.id, replacement.selected)));
        QVERIFY(file->setCheckpointAttempt(replacement));
        file->setState(File::FS_TRANSFERRING);
        QVERIFY(!waitTask(file->pauseTransfer(attempt.id, dsfm::TransferPauseReason::Disconnected)));
        QCOMPARE(file->getState(), File::FS_TRANSFERRING);
        QVERIFY(waitTask(file->pauseTransfer(replacement.id, dsfm::TransferPauseReason::ResourceUnavailable)));
        file->cancel();
        QTRY_COMPARE_WITH_TIMEOUT(file->getState(), File::FS_CANCELLED, 5000);
        QVERIFY(!file->isResumable());
        auto remaining = waitTask(storage().manager().listPending(file->storageNamespace()));
        QVERIFY(remaining);
        QVERIFY(std::none_of(remaining.value().entries.begin(), remaining.value().entries.end(),
                            [&](const auto& item) { return item.id == pending; }));
        auto cancelled = waitTask(storage().transferProgress().load(key));
        QVERIFY(cancelled);
        QCOMPARE(cancelled.value().failure, dsfm::TransferFailureReason::UserAction);
        file->deleteFromDb();
    }
    void durable_pause_survives_application_mirror_failure() {
        auto file = incoming("paused", "pause-mirror-fixture");
        auto saved = waitTask(file->prepareCheckpointProgress(dsfm::minimum_checkpoint_interval));
        QVERIFY(saved);
        dsfm::TransferAttempt attempt{QUuid::createUuid(), dsfm::minimum_checkpoint_interval, 1,
                                      saved.value().checkpoints.first()};
        const auto key = file->checkpointKey().value();
        QVERIFY(waitTask(storage().transferProgress().beginAttempt(key, attempt.id, attempt.selected)));
        QVERIFY(file->setCheckpointAttempt(attempt));
        file->setState(File::FS_TRANSFERRING);
        QSqlQuery query;
        QVERIFY(query.exec("CREATE TEMP TRIGGER reject_pause_mirror BEFORE UPDATE ON file_transfer_v1 "
                           "BEGIN SELECT RAISE(FAIL,'test pause mirror failure'); END"));
        const auto cleanup = qScopeGuard([] { QSqlQuery query; query.exec("DROP TRIGGER IF EXISTS reject_pause_mirror"); });
        QVERIFY(waitTask(file->pauseTransfer(attempt.id, dsfm::TransferPauseReason::SourceUnavailable)));
        QCOMPARE(file->getState(), File::FS_PAUSED);
        QCOMPARE(file->dataPauseReason(), dsfm::TransferPauseReason::SourceUnavailable);
        auto durable = waitTask(storage().transferProgress().load(key));
        QVERIFY(durable);
        QCOMPARE(durable.value().pause, dsfm::TransferPauseReason::SourceUnavailable);
        QVERIFY(query.exec("DROP TRIGGER reject_pause_mirror"));
        auto loaded = File::load(*engine_->getFileManager(), file->getId());
        QTRY_COMPARE_WITH_TIMEOUT(loaded->dataPauseReason(), dsfm::TransferPauseReason::SourceUnavailable, 5000);
        QCOMPARE(loaded->getState(), File::FS_PAUSED);
        file->deleteFromDb();
    }
    void network_transfer_lookup_is_scoped_before_selecting_an_id() {
        auto first = incoming("first", "first");
        auto cert = ds::crypto::DsCert::create();
        auto data = std::make_unique<ContactData>();
        data->identity = identity_->getId(); data->name = "Collision peer";
        data->cert = ds::crypto::DsCert::createFromPubkey(cert->getSigningPubKey().toByteArray());
        data->address = "fixture-not-networked"; data->autoConnect = false;
        auto* other = engine_->getContactManager()->addContact(std::move(data));
        auto second_data = std::make_unique<FileData>();
        second_data->direction = File::INCOMING; second_data->state = File::FS_OFFERED;
        second_data->identity = identity_->getId(); second_data->contact = other->getId();
        second_data->conversation = engine_->getConversationManager()->getConversation(other)->getId();
        second_data->fileId = first->getFileId(); second_data->name = "second"; second_data->size = 1;
        second_data->hash = QCryptographicHash::hash("x", QCryptographicHash::Sha256);
        auto* manager = engine_->getFileManager();
        auto second = manager->addFile(std::move(second_data));
        QCOMPARE(manager->getFileFromId(first->getFileId(), *contact_, File::INCOMING), first);
        QCOMPARE(manager->getFileFromId(first->getFileId(), *other, File::INCOMING), second);
        QVERIFY(!manager->getFileFromId(first->getFileId(), *other, File::OUTGOING));
        QVERIFY(!manager->getFileFromPeer(first->getFileId(), QUuid::createUuid(), other->getHash(), File::INCOMING));
        IdentityData identity_data;
        identity_data.uuid = QUuid::createUuid(); identity_data.name = "Scoped collision identity";
        identity_data.cert = ds::crypto::DsCert::create(); identity_data.hash = identity_data.cert->getHash().toByteArray();
        identity_data.autoConnect = false;
        auto* identity = engine_->getIdentityManager()->addIdentity(identity_data);
        auto third_contact = std::make_unique<ContactData>();
        third_contact->identity = identity->getId(); third_contact->name = "Same peer on another identity";
        third_contact->cert = ds::crypto::DsCert::createFromPubkey(cert->getSigningPubKey().toByteArray());
        third_contact->address = "fixture-not-networked"; third_contact->autoConnect = false;
        auto* other_identity_peer = engine_->getContactManager()->addContact(std::move(third_contact));
        auto third_data = std::make_unique<FileData>();
        third_data->direction = File::INCOMING; third_data->state = File::FS_OFFERED;
        third_data->identity = identity->getId(); third_data->contact = other_identity_peer->getId();
        third_data->conversation = engine_->getConversationManager()->getConversation(other_identity_peer)->getId();
        third_data->fileId = first->getFileId(); third_data->name = "third"; third_data->size = 1;
        third_data->hash = QCryptographicHash::hash("x", QCryptographicHash::Sha256);
        auto third = manager->addFile(std::move(third_data));
        QCOMPARE(manager->getFileFromPeer(first->getFileId(), identity_->getUuid(), other->getHash(), File::INCOMING), second);
        QCOMPARE(manager->getFileFromPeer(first->getFileId(), identity->getUuid(), other->getHash(), File::INCOMING), third);
        first->deleteFromDb(); second->deleteFromDb(); third->deleteFromDb();
    }
    void generic_progress_is_durable_and_feature_neutral() {
        auto make_file = [&](File::Direction direction) {
            auto data = std::make_unique<FileData>();
            data->direction = direction;
            data->state = File::FS_WAITING;
            data->identity = identity_->getId();
            data->contact = contact_->getId();
            data->conversation = conversation_->getId();
            data->name = "checkpoint-fixture";
            data->size = 6;
            data->hash = QCryptographicHash::hash("abcdef", QCryptographicHash::Sha256);
            return engine_->getFileManager()->addFile(std::move(data));
        };
        auto incoming = make_file(File::INCOMING);
        auto saved = waitTask(incoming->prepareCheckpointProgress(dsfm::default_checkpoint_interval));
        QVERIFY(saved);
        QCOMPARE(saved.value().description.side, dsfm::TransferSide::Receiver);
        auto key = incoming->checkpointKey();
        QVERIFY(key);
        QCOMPARE(key.value().local_scope, identity_->getUuid());
        QCOMPARE(key.value().remote_scope, contact_->getUuid());
        auto attempt = QUuid::createUuid();
        QVERIFY(waitTask(
            storage().transferProgress().beginAttempt(key.value(), attempt, saved.value().checkpoints.first())));
        QVERIFY(waitTask(incoming->prepareIncoming()));
        QVERIFY(waitTask(incoming->writeIncoming(0, "abc")));
        auto prefix = waitTask(incoming->checkpointIncomingPrefix(3));
        QVERIFY(prefix);
        QCOMPARE(prefix.value(), QCryptographicHash::hash("abc", QCryptographicHash::Sha256));
        QSignalSpy confirmed(incoming.get(), &File::durableCheckpointConfirmed);
        dsfm::DurableCheckpoint checkpoint{1, 3, prefix.value(), false, true};
        QVERIFY(waitTask(incoming->recordCheckpoint(attempt, checkpoint, dsfm::CheckpointPhase::Proposed)));
        QCOMPARE(confirmed.count(), 0);
        QVERIFY(waitTask(incoming->recordCheckpoint(attempt, checkpoint, dsfm::CheckpointPhase::MutuallyConfirmed)));
        QCOMPARE(confirmed.count(), 1);
        auto durable = waitTask(storage().transferProgress().load(key.value()));
        QVERIFY(durable);
        QCOMPARE(durable.value().confirmed_offset, quint64(3));
        QSignalSpy alleged(incoming.get(), &File::sourceIntegrityFailed);
        QVERIFY(!waitTask(incoming->failCheckpoint(dsfm::TransferFailureReason::SourceIntegrityMismatch)));
        QVERIFY(waitTask(incoming->failCheckpoint(dsfm::TransferFailureReason::ReceiverCheckpointMismatch)));
        QCOMPARE(alleged.count(), 0);
        QVERIFY(waitTask(incoming->suspendIncoming()));
        auto outgoing = make_file(File::OUTGOING);
        QVERIFY(waitTask(outgoing->prepareCheckpointProgress(dsfm::default_checkpoint_interval)));
        QSignalSpy proven(outgoing.get(), &File::sourceIntegrityFailed);
        QVERIFY(waitTask(outgoing->failCheckpoint(dsfm::TransferFailureReason::SourceIntegrityMismatch)));
        QCOMPARE(proven.count(), 1);
        outgoing->setState(File::FS_FAILED);
        QVERIFY(!outgoing->isResumable());
        auto restored = File::load(*engine_->getFileManager(), outgoing->getId());
        QCOMPARE(restored->failureReason(), int(dsfm::TransferFailureReason::SourceIntegrityMismatch));
        QVERIFY(!restored->isResumable());
        incoming->deleteFromDb();
        outgoing->deleteFromDb();
    }
    void read_only_host_exclusions_are_mandatory() {
        QTemporaryDir external; QVERIFY(external.isValid());
        QFile public_file(external.filePath("public")); QVERIFY(public_file.open(QIODevice::WriteOnly)); QCOMPARE(public_file.write("public"), 6); public_file.close();
        const auto database = engine_->settings().value("dbpath").toString();
        QVERIFY(QFile::link(database, external.filePath("private-database")));
        QVERIFY(QFile::link(dsfm::NativeFileManager::lockRegistryRoot(), external.filePath("private-locks")));
        dsfm::ReadOnlyNamespaceConfig config; config.common = {QUuid::createUuid(), external.path()}; config.symlinks = dsfm::SymlinkPolicy::Always;
        auto opened = waitTask(storage().openReadOnlySource(config, true)); QVERIFY(opened);
        auto* manager = storage().readOnlyManager(); QVERIFY(manager);
        bool ready = false;
        for (int i = 0; i < 1000; ++i) {
            auto info = waitTask(manager->namespaceInfo(config.common.id)); QVERIFY(info); QVERIFY(!info.value().readiness_error);
            if (info.value().status == dsfm::NamespaceStatus::Ready) { ready = true; break; }
            QCoreApplication::processEvents();
        }
        QVERIFY(ready);
        auto public_entry = waitTask(manager->lookup(config.common.id, "public")); QVERIFY(public_entry);
        auto hidden = waitTask(manager->lookup(config.common.id, "private-database")); QVERIFY(!hidden); QCOMPARE(hidden.error().code, dsfm::ErrorCode::MissingEntry);
        hidden = waitTask(manager->lookup(config.common.id, "private-locks")); QVERIFY(!hidden); QCOMPARE(hidden.error().code, dsfm::ErrorCode::MissingEntry);
        auto prepared = waitTask(manager->prepareRead(config.common.id, public_entry.value().id)); QVERIFY(prepared);
        auto data = waitTask(prepared.value().reader->readAt(0, 64)); QVERIFY(data); QCOMPARE(data.value(), QByteArray("public"));
        dsfm::ExclusionPolicy policy; policy.objects = {external.filePath("public")};
        QVERIFY(waitTask(storage().updateReadOnlyExclusions(config.common.id, policy)));
        auto revoked = waitTask(prepared.value().reader->readAt(0, 64)); QVERIFY(!revoked); QCOMPARE(revoked.error().code, dsfm::ErrorCode::MissingEntry);
        hidden = waitTask(manager->lookup(config.common.id, "private-database")); QVERIFY(!hidden);
        QVERIFY(waitTask(prepared.value().reader->close())); prepared.value().reader.reset();
        QVERIFY(waitTask(storage().closeReadOnlySource(config.common.id)));
    }
    void schema_matches_versioned_fixture() {
        QFile fixture(QStringLiteral(DS_STORAGE_FIXTURES) + "/managed-storage-v2.json"); QVERIFY(fixture.open(QIODevice::ReadOnly));
        const auto data = QJsonDocument::fromJson(fixture.readAll()).object();
        QSqlQuery version("SELECT version FROM ds"); QVERIFY(version.next()); QCOMPARE(version.value(0).toInt(), data.value("application_schema_version").toInt());
        const auto tables = data.value("tables").toObject();
        for (auto it = tables.begin(); it != tables.end(); ++it) {
            QSqlQuery query("SELECT * FROM " + it.key() + " LIMIT 0");
            for (const auto column : it.value().toArray()) QVERIFY(query.record().indexOf(column.toString()) >= 0);
        }
    }
    void permission_repair_defaults_on() {
        for (auto area : {ManagedStorage::Downloads, ManagedStorage::Private}) {
            auto info = waitTask(storage().manager().namespaceInfo(storage().namespaceId(area)));
            QVERIFY(info); QVERIFY(info.value().config.repair_permissions);
        }
    }
    void media_preview_schema_and_limits_match_versioned_fixture() {
        QFile fixture(QStringLiteral(DS_STORAGE_FIXTURES) + "/media-preview-v1.json"); QVERIFY(fixture.open(QIODevice::ReadOnly));
        const auto data = QJsonDocument::fromJson(fixture.readAll()).object();
        QCOMPARE(data.value("version").toInt(), media_preview::version);
        QCOMPARE(data.value("maximum_edge").toInt(), media_preview::edge);
        QCOMPARE(data.value("maximum_encoded_bytes").toInt(), media_preview::max_encoded_bytes);
        QCOMPARE(data.value("maximum_source_bytes").toInt(), media_preview::max_source_bytes);
        QCOMPARE(data.value("maximum_video_source_bytes").toInt(), media_preview::max_video_source_bytes);
        QCOMPARE(data.value("video_deadline_ms").toInt(), media_preview::video_deadline_ms);
        QCOMPARE(data.value("maximum_source_pixels").toInteger(), media_preview::max_source_pixels);
        QCOMPARE(data.value("maximum_cached_previews").toInt(), media_preview::max_cached_previews);
        const auto tables = data.value("tables").toObject();
        for (auto it = tables.begin(); it != tables.end(); ++it) {
            QSqlQuery query("SELECT * FROM " + it.key() + " LIMIT 0");
            for (const auto column : it.value().toArray()) QVERIFY(query.record().indexOf(column.toString()) >= 0);
        }
    }
    void desktop_notification_preferences_are_private_per_identity() {
        QVERIFY(!identity_->getNotificationShowSender());
        QVERIFY(!identity_->getNotificationShowContent());
        QmlIdentityReq request;
        QVERIFY(!request.notificationShowSender()); QVERIFY(!request.notificationShowContent());
        request.setNotificationShowSender(true); request.setNotificationShowContent(true);
        QVERIFY(request.value.notificationShowSender); QVERIFY(request.value.notificationShowContent);
        IdentityData second;
        second.uuid = QUuid::createUuid(); second.name = "Second local identity";
        second.cert = ds::crypto::DsCert::create(); second.hash = second.cert->getHash().toByteArray();
        second.autoConnect = false;
        auto* other = engine_->getIdentityManager()->addIdentity(second);
        QVERIFY(other);
        QSignalSpy changes(identity_, &Identity::securityPreferencesChanged);
        identity_->setNotificationShowSender(true);
        identity_->setNotificationShowContent(true);
        QCOMPARE(changes.count(), 2);
        QVERIFY(!other->getNotificationShowSender()); QVERIFY(!other->getNotificationShowContent());
        const auto prefix = QStringLiteral("identity/%1/").arg(identity_->getUuid().toString(QUuid::WithoutBraces));
        engine_->settings().sync();
        QSettings persisted(engine_->settings().fileName(), QSettings::IniFormat);
        QVERIFY(persisted.value(prefix + "notificationShowSender").toBool());
        QVERIFY(persisted.value(prefix + "notificationShowContent").toBool());
        identity_->setNotificationShowSender(false); identity_->setNotificationShowContent(false);
        QCOMPARE(changes.count(), 4);
    }
    void import_is_a_managed_copy_and_export_is_verified() {
        const auto path = directory_.filePath("source.txt");
        const QByteArray bytes("a managed outgoing snapshot");
        QFile source(path); QVERIFY(source.open(QIODevice::WriteOnly)); QCOMPARE(source.write(bytes), bytes.size()); source.close();
        auto imported = waitTask(storage().importFile(path, "source.txt")); QVERIFY(imported);
        QVERIFY(source.open(QIODevice::WriteOnly | QIODevice::Truncate)); source.write("changed source"); source.close();
        auto opened = waitTask(storage().manager().openRead(imported.value().namespace_id, imported.value().file.id)); QVERIFY(opened);
        auto data = waitTask(opened.value()->readAt(0, 1024)); QVERIFY(data); QCOMPARE(data.value(), bytes);
        QVERIFY(waitTask(opened.value()->close()));
        auto exported = waitTask(storage().exportFile(imported.value().namespace_id, imported.value().file.id, "source.txt")); QVERIFY(exported);
        QFile copy(exported.value()); QVERIFY(copy.open(QIODevice::ReadOnly)); QCOMPARE(copy.readAll(), bytes);
        QVERIFY(copy.permissions().testFlag(QFileDevice::ReadOwner));
        QVERIFY(!copy.permissions().testFlag(QFileDevice::ReadGroup));
    }
    void sender_verifies_actual_payload_data() {
        QTest::addColumn<bool>("tamper"); QTest::addColumn<bool>("resume");
        QTest::newRow("complete") << false << false;

        QTest::newRow("changed-with-restored-mtime") << true << false;
    }
    void sender_verifies_actual_payload() {
        QFETCH(bool, tamper); QFETCH(bool, resume);
        const QByteArray original(2 * ds::prot::Peer::file_chunk_bytes, 'a');
        const auto source_path = directory_.filePath("sender-source");
        QFile source(source_path); QVERIFY(source.open(QIODevice::WriteOnly)); QCOMPARE(source.write(original), original.size()); source.close();
        auto imported = waitTask(storage().importFile(source_path, "sender-source")); QVERIFY(imported);
        auto data = std::make_unique<FileData>(); data->state = File::FS_WAITING;
        data->identity = identity_->getId(); data->contact = contact_->getId(); data->conversation = conversation_->getId();
        data->storage_namespace = imported.value().namespace_id; data->storage_id = imported.value().file.id;
        data->name = "sender-source"; data->hash = imported.value().file.sha256; data->size = original.size();
        if (resume) data->bytesTransferred = ds::prot::Peer::file_chunk_bytes;
        auto file = engine_->getFileManager()->addFile(std::move(data)); QVERIFY(file);
        auto approved = approveFixtureContact();
        auto peer = std::make_shared<SendingPeer>(); bool mutation_ok = false;
        if (tamper) {
            const auto id = file->storageId().toString(QUuid::WithoutBraces);
            auto ns = waitTask(storage().manager().namespaceInfo(file->storageNamespace())); QVERIFY(ns);
            const auto physical = ns.value().config.storage_root + "/objects/" + id.left(2) + '/' + id;
            peer->mutate = [physical, &mutation_ok] {
                const auto fd = ::open(QFile::encodeName(physical).constData(), O_RDWR | O_CLOEXEC | O_NOFOLLOW);
                if (fd < 0) return;
                struct stat before{};
                if (::fstat(fd, &before) == 0 && ::pwrite(fd, "z", 1, off_t(ds::prot::Peer::file_chunk_bytes)) == 1) {
                    const timespec times[]{before.st_atim, before.st_mtim}; mutation_ok = ::futimens(fd, times) == 0;
                }
                ::close(fd);
            };
        }
        QSignalSpy done(file.get(), &File::transferDone); peer->begin(file);
        QTRY_VERIFY_WITH_TIMEOUT(file->getState() == File::FS_DONE || file->getState() == File::FS_FAILED, 5000);
        if (tamper) {
            QVERIFY(mutation_ok); QCOMPARE(file->getState(), File::FS_FAILED); QVERIFY(peer->cancelled); QVERIFY(!peer->final_sent);
            auto info = waitTask(storage().manager().stat(file->storageNamespace(), file->storageId())); QVERIFY(info); QCOMPARE(info.value().hash_state, dsfm::HashState::Missing);
        } else {
            QCOMPARE(file->getState(), File::FS_DONE); QVERIFY(peer->final_sent); QVERIFY(!peer->cancelled);
            QCOMPARE(peer->sent, resume ? original.mid(ds::prot::Peer::file_chunk_bytes) : original);
        }
        QCOMPARE(done.count(), 1); QCOMPARE(done.front()[1].toBool(), !tamper);
    }
    void vfs_home_conflicts_and_catalog_transport() {
        const auto encoded = FileManager::transferName(QString(700,QChar('%')));
        QVERIFY(encoded.toUtf8().size() <= 255); QVERIFY(dsfm::normalizeName(encoded));
        QCOMPARE(encoded,FileManager::transferName(QString(700,QChar('%'))));
        QCOMPARE(FileManager::transferName("plain.txt"),QString("plain.txt"));
        QVERIFY(dsfm::normalizeName(FileManager::transferName("name:part")));
        const auto previous_state = contact_->getState(); contact_->setState(Contact::ACCEPTED);
        auto restore_state = qScopeGuard([this,previous_state] { contact_->setState(previous_state); });
        auto opened = waitTask(storage().openVfs()); QVERIFY(opened); auto* vfs = opened.value();
        QSettings unused; ds::models::NotificationsModel notifications(unused);
        QTemporaryDir external; QVERIFY(external.isValid()); QVERIFY(QDir().mkdir(external.filePath("dir")));
        for (int i = 0; i < 20; ++i) {
            QFile file(external.filePath(QStringLiteral("entry-%1").arg(i,3,10,QChar('0'))));
            QVERIFY(file.open(QIODevice::WriteOnly)); QCOMPARE(file.write("hello"),5);
        }
        dsfm::ReadOnlyNamespaceConfig config; config.common = {QUuid::createUuid(),external.path()};
        auto source = waitTask(storage().openReadOnlySource(config,true)); QVERIFY(source);
        auto* backend = storage().readOnlyManager(); QVERIFY(backend);
        for (int turn = 0; turn < 1000; ++turn) {
            auto info = waitTask(backend->namespaceInfo(config.common.id)); QVERIFY(info);
            if (info.value().status == dsfm::NamespaceStatus::Ready) break;
        }
        dsvfs::Request request; request.scope = contact_->getUuid();
        auto generation = std::make_shared<std::atomic<quint64>>(1); request.current_generation = generation; request.generation = 1;
        request.authorize = [](dsvfs::Target,dsvfs::Operation) -> dsfm::Result<bool> { return true; };
        request.commit_permit = [](dsvfs::Target,dsvfs::Operation) -> dsfm::Result<std::shared_ptr<void>> { return std::static_pointer_cast<void>(std::make_shared<int>(1)); };
        const auto identity = identity_->getUuid();
        auto resource=waitTask(vfs->addResource({{},config.common.id,{}},request)); QVERIFY(resource);
        auto assignment=waitTask(vfs->assignStorage({{},identity,resource.value().id,source.value().root,false},request)); QVERIFY(assignment);
        auto mounted = waitTask(vfs->addMount(identity,{{},{},config.common.id,source.value().root,"vfs-fixture",{}},request)); QVERIFY(mounted);
        auto root = waitTask(vfs->lookup(identity,vfs->root(identity),"vfs-fixture",request)); QVERIFY(root);
        auto entry = waitTask(vfs->lookup(identity,root.value().id,"entry-000",request)); QVERIFY(entry);
        auto file_download = waitTask(engine_->getFileManager()->addVfsTransfer(*conversation_,*vfs,entry.value().id,request)); QVERIFY(file_download);
        QVERIFY(file_download.value()->hasPreparedSource()); QVERIFY(file_download.value()->storageId().isNull());
        auto file_peer = std::make_shared<SendingPeer>(); file_peer->begin(file_download.value());
        QTRY_COMPARE_WITH_TIMEOUT(file_download.value()->getState(),File::FS_DONE,5000); QCOMPARE(file_peer->sent,QByteArray("hello"));
        auto catalog = waitTask(vfs->exportCatalog(identity,root.value().id,false,request)); QVERIFY(catalog);
        auto outgoing = engine_->getFileManager()->addCatalogTransfer(*conversation_,catalog.value()); QVERIFY(outgoing);
        QVERIFY(outgoing.value()->storageId().isNull()); QVERIFY(outgoing.value()->hasCatalogSource());
        auto peer = std::make_shared<SendingPeer>(); peer->begin(outgoing.value());
        QTRY_COMPARE_WITH_TIMEOUT(outgoing.value()->getState(),File::FS_DONE,5000); QVERIFY(peer->final_sent);
        QCOMPARE(QCryptographicHash::hash(peer->sent,QCryptographicHash::Sha256),catalog.value()->sha256());
        auto second = waitTask(vfs->exportCatalog(identity,root.value().id,false,request)); QVERIFY(second);
        auto interrupted = engine_->getFileManager()->addCatalogTransfer(*conversation_,second.value()); QVERIFY(interrupted);
        auto denied_peer = std::make_shared<SendingPeer>(); denied_peer->mutate = [generation] { ++*generation; }; denied_peer->begin(interrupted.value());
        QTRY_COMPARE_WITH_TIMEOUT(interrupted.value()->getState(),File::FS_PAUSED,5000);
        QCOMPARE(interrupted.value()->dataPauseReason(), dsfm::TransferPauseReason::AuthorizationWithdrawn);
        QVERIFY(!denied_peer->cancelled); QVERIFY(!denied_peer->final_sent);
        auto closed = waitTask(catalog.value()->close()); QVERIFY(closed); closed = waitTask(second.value()->close()); QVERIFY(closed);
        request.generation = generation->load();
        auto directory = waitTask(vfs->lookup(identity,root.value().id,"dir",request)); QVERIFY(directory);
        auto nested = waitTask(vfs->addMount(identity,{{},directory.value().id,config.common.id,source.value().root,"nested",{}},request)); QVERIFY(nested);
        QFile collision(external.filePath("dir/nested")); QVERIFY(collision.open(QIODevice::WriteOnly)); collision.write("conflict"); collision.close();
        auto scan = waitTask(backend->reconcile(config.common.id,{})); QVERIFY(scan);
        for (int turn = 0; turn < 1000; ++turn) { auto info = waitTask(backend->namespaceInfo(config.common.id)); QVERIFY(info); if (info.value().status == dsfm::NamespaceStatus::Ready) break; }
        auto refreshed = waitTask(vfs->refresh()); QVERIFY(refreshed);
        QSqlQuery query; QVERIFY(query.exec("SELECT COUNT(*) FROM notification WHERE type=3")); QVERIFY(query.next()); QCOMPARE(query.value(0).toInt(),1);
        refreshed = waitTask(vfs->refresh()); QVERIFY(refreshed);
        QVERIFY(query.exec("SELECT COUNT(*) FROM notification WHERE type=3")); QVERIFY(query.next()); QCOMPARE(query.value(0).toInt(),1);
        QVERIFY(notifications.rowCount() > 0);
        QVERIFY(query.exec("SELECT message FROM notification WHERE type=3")); QVERIFY(query.next()); QVERIFY(query.value(0).toString().contains("/vfs-fixture/dir/nested"));
        QVERIFY(collision.remove()); scan = waitTask(backend->reconcile(config.common.id,{})); QVERIFY(scan);
        for (int turn = 0; turn < 1000; ++turn) { auto info = waitTask(backend->namespaceInfo(config.common.id)); QVERIFY(info); if (info.value().status == dsfm::NamespaceStatus::Ready) break; }
        refreshed = waitTask(vfs->refresh()); QVERIFY(refreshed);
        QVERIFY(query.exec("SELECT COUNT(*) FROM notification WHERE type=3")); QVERIFY(query.next()); QCOMPARE(query.value(0).toInt(),0);
        QVERIFY(waitTask(vfs->removeMount(identity,nested.value().id,request)));
        QVERIFY(waitTask(vfs->removeMount(identity,mounted.value().id,request)));
        QVERIFY(waitTask(storage().closeReadOnlySource(config.common.id)));
    }
    void outgoing_media_import_uses_normal_file_service() {
        auto data = std::make_unique<FileData>();
        data->identity = identity_->getId();
        data->contact = contact_->getId();
        data->conversation = conversation_->getId();
        data->name = QStringLiteral("earth.jpg");
        data->source_path = QStringLiteral(DS_STORAGE_FIXTURES) + "/../assets/mcp/earth.jpg";
        auto file = engine_->getFileManager()->addFile(std::move(data));
        QVERIFY(file);
        QTRY_VERIFY_WITH_TIMEOUT(file->getState() != File::FS_HASHING, 5000);
        // No network/peer permissions are configured here: rejection after import
        // is expected, but the import must have published its verified snapshot.
        QCOMPARE(file->getHash().size(), 32);
        QCOMPARE(file->getSize(), 536295);
        QVERIFY(!file->storageId().isNull());
    }
    void vorbis_import_advertises_standard_ogg_media_type() {
        const auto path = QStringLiteral(DS_STORAGE_FIXTURES) + "/../assets/mcp/sos.ogg";
        auto imported = waitTask(storage().importFile(path, "vorbis.ogg"));
        QVERIFY(imported);
        QCOMPARE(imported.value().mime_type, QStringLiteral("audio/ogg"));
    }
    void outgoing_offer_validation_diagnostics_data() {
        QTest::addColumn<QString>("field");
        QTest::addColumn<QString>("reason");
        QTest::newRow("transfer-id") << QString("id") << QString("transfer ID must be 16 bytes");
        QTest::newRow("negative-size") << QString("negative") << QString("file size is negative");
        QTest::newRow("oversize") << QString("oversize") << QString("file size exceeds the hard limit");
        QTest::newRow("checksum") << QString("hash") << QString("SHA-256 checksum must be 32 bytes");
        QTest::newRow("empty-name") << QString("empty") << QString("filename is empty");
        QTest::newRow("utf8-name") << QString("long") << QString("filename exceeds 255 UTF-8 bytes");
        QTest::newRow("mime") << QString("mime") << QString("MIME type is invalid");
    }
    void outgoing_offer_validation_diagnostics() {
        QFETCH(QString, field);
        QFETCH(QString, reason);
        auto data = std::make_unique<FileData>();
        data->identity = identity_->getId(); data->contact = contact_->getId();
        data->conversation = conversation_->getId(); data->fileId = QByteArray(16, 'i');
        data->hash = QByteArray(32, 'h'); data->size = 1;
        data->name = "private-name"; data->mime_type = "application/octet-stream";
        if (field == "id") data->fileId = QByteArray(32, 'i');
        else if (field == "negative") data->size = -1;
        else if (field == "oversize") data->size = qlonglong(ResourceLimits::hardAcceptedFileBytes) + 1;
        else if (field == "hash") data->hash = QByteArray(64, 'h');
        else if (field == "empty") data->name.clear();
        else if (field == "long") data->name = QString(128, QChar(0x00e9));
        else if (field == "mime") data->mime_type = "invalid-mime";
        File file(*engine_, std::move(data));
        ReceivingPeer peer;
        try {
            peer.offerFile(file);
            QFAIL("Invalid metadata was accepted");
        } catch (const std::runtime_error& error) {
            QCOMPARE(QString::fromUtf8(error.what()), "File offer violates v3 policy: " + reason);
        }
    }
    void incoming_is_unpublished_until_checksum_verified_and_resumes() {
        const QByteArray bytes(8192 * 2 + 3, 's');
        auto file = incoming(bytes, "incoming.bin");
        auto prepared = waitTask(file->prepareIncoming()); QVERIFY(prepared); QCOMPARE(prepared.value(), quint64(0));
        const auto id = file->storageId(); const auto pending = file->pendingId();
        auto hidden = waitTask(storage().manager().openRead(file->storageNamespace(), id)); QVERIFY(!hidden);
        QVERIFY(waitTask(file->writeIncoming(0, bytes.left(8192))));
        QVERIFY(waitTask(file->checkpointIncoming())); file->setBytesTransferred(8192);
        QVERIFY(waitTask(file->suspendIncoming()));
        auto reopened = waitTask(file->prepareIncoming()); QVERIFY(reopened); QCOMPARE(reopened.value(), quint64(8192));
        QCOMPARE(file->storageId(), id); QCOMPARE(file->pendingId(), pending);
        QVERIFY(waitTask(file->writeIncoming(8192, bytes.mid(8192))));
        QVERIFY(waitTask(file->checkpointIncoming()));
        auto approved = approveFixtureContact();
        auto peer = receiveOwned(file, bytes);
        QTRY_COMPARE_WITH_TIMEOUT(file->getState(), File::FS_DONE, 10000);
        auto reader = waitTask(storage().manager().openRead(file->storageNamespace(), id)); QVERIFY(reader);
        auto read = waitTask(reader.value()->readAt(0, bytes.size())); QVERIFY(read); QCOMPARE(read.value(), bytes);
        QVERIFY(waitTask(reader.value()->close()));
        QVERIFY(file->pendingId().isNull()); QVERIFY(!file->property("path").isValid());
    }
    void checksum_failure_never_publishes() {
        auto file = incoming(QByteArray("expected"), "bad.bin");
        QVERIFY(waitTask(file->prepareIncoming()));
        QVERIFY(waitTask(file->writeIncoming(0, "tampered")));
        QVERIFY(waitTask(file->suspendIncoming()));
        auto pending = waitTask(storage().manager().openPending(file->storageNamespace(), file->pendingId())); QVERIFY(pending);
        auto committed = waitTask(pending.value().writer->commit());
        QVERIFY(!committed); QCOMPARE(committed.error().code, dsfm::ErrorCode::IntegrityError);
        QVERIFY(waitTask(pending.value().writer->abort()));
        auto hidden = waitTask(storage().manager().openRead(file->storageNamespace(), file->storageId())); QVERIFY(!hidden);
    }
    void client_closes_on_authenticated_invalid_file_chunk() {
        LoopbackListener listener;
        QVERIFY(listener.listen(QHostAddress::LocalHost, 0));
        const auto remote_cert = ds::crypto::DsCert::create();
        std::shared_ptr<ds::prot::DsServer> server;
        listener.accepted = [&](ds::prot::ConnectionSocket::ptr_t socket) {
            ConnectData data;
            data.service = identity_->getUuid();
            data.identitysCert = remote_cert;
            server = std::make_shared<ds::prot::DsServer>(std::move(socket), data);
            connect(server.get(), &ds::prot::Peer::incomingPeer, server.get(),
                    [peer = server.get()] { peer->authorize(true); });
        };
        auto socket = ds::prot::ConnectionSocket::create();
        ConnectData data;
        data.service = identity_->getUuid();
        data.identitysCert = identity_->getCert();
        data.contactsCert = remote_cert;
        auto client = std::make_shared<ds::prot::DsClient>(socket, data);
        QSignalSpy connected(client.get(), &ds::prot::Peer::connectedToPeer);
        QSignalSpy closing(client.get(), &ds::prot::Peer::closeLater);
        socket->connectToHost(QHostAddress::LocalHost, listener.serverPort());
        QTRY_VERIFY_WITH_TIMEOUT(connected.count() > 0, 5000);
        QVERIFY(server);
        // The authenticated sender has never offered this transfer. Dispatch
        // must reject it without allowing an exception to escape the Qt slot.
        ds::protocol::FileChunk invalid;
        invalid.setFileId(QByteArray(16, 'x')); invalid.setData("invalid"); invalid.setFinal(true);
        ds::protocol::Envelope envelope; envelope.setProtocolVersion(ds::prot::codec::protocolVersion); envelope.setFileChunk(invalid);
        server->send(envelope);
        QTRY_VERIFY_WITH_TIMEOUT(closing.count() > 0, 5000);
        QTRY_VERIFY_WITH_TIMEOUT(!socket->isOpen(), 5000);
        server->close();
        QCoreApplication::processEvents();
    }
    void authenticated_checkpoint_wire_transfer_data() {
        QTest::addColumn<quint32>("interval");
        QTest::addColumn<bool>("empty");
        QTest::addColumn<bool>("withhold");
        QTest::addColumn<bool>("signed_offer");
        QTest::addColumn<QString>("control");
        QTest::newRow("64KiB") << dsfm::minimum_checkpoint_interval << false << false << false << QString{};
        QTest::newRow("1MiB") << dsfm::default_checkpoint_interval << false << false << false << QString{};
        QTest::newRow("16MiB") << dsfm::maximum_checkpoint_interval << false << false << false << QString{};
        QTest::newRow("empty") << dsfm::minimum_checkpoint_interval << true << false << false << QString{};
        QTest::newRow("withheld") << dsfm::minimum_checkpoint_interval << false << true << false << QString{};
        QTest::newRow("signed-manifest") << dsfm::minimum_checkpoint_interval << false << false << true << QString{};
        QTest::newRow("stale-ack") << dsfm::minimum_checkpoint_interval << false << true << false << QStringLiteral("stale-ack");
        QTest::newRow("resume-fields") << dsfm::minimum_checkpoint_interval << false << true << false << QStringLiteral("resume-fields");
        QTest::newRow("advisory-pause") << dsfm::minimum_checkpoint_interval << false << true << false << QStringLiteral("pause");
        QTest::newRow("duplicate-readiness") << dsfm::minimum_checkpoint_interval << false << false << false << QStringLiteral("duplicate-readiness");
        QTest::newRow("receiver-mismatch") << dsfm::minimum_checkpoint_interval << false << true << false << QStringLiteral("receiver-mismatch");
        QTest::newRow("unsupported-version") << dsfm::minimum_checkpoint_interval << false << true << false << QStringLiteral("unsupported-version");
        QTest::newRow("remote-source-cancel") << dsfm::minimum_checkpoint_interval << false << true << false << QStringLiteral("remote-source-cancel");
        QTest::newRow("resume-lost") << dsfm::minimum_checkpoint_interval << false << true << false << QStringLiteral("resume");
        QTest::newRow("resume-missing") << dsfm::minimum_checkpoint_interval << false << true << false << QStringLiteral("resume-missing");
        QTest::newRow("resume-corrupt") << dsfm::minimum_checkpoint_interval << false << true << false << QStringLiteral("resume-corrupt");
        QTest::newRow("resume-source") << dsfm::minimum_checkpoint_interval << false << true << false << QStringLiteral("resume-source");
        QTest::newRow("resume-metadata") << dsfm::minimum_checkpoint_interval << false << true << false << QStringLiteral("resume-metadata");
        QTest::newRow("resume-rewind-source") << dsfm::minimum_checkpoint_interval << false << true << false << QStringLiteral("resume-rewind-source");
        QTest::newRow("resume-short-prefix") << dsfm::minimum_checkpoint_interval << false << true << false << QStringLiteral("resume-short-prefix");
        QTest::newRow("eof-receipt") << dsfm::minimum_checkpoint_interval << false << false << false << QStringLiteral("eof-receipt");
        QTest::newRow("eof-ack") << dsfm::minimum_checkpoint_interval << false << false << false << QStringLiteral("eof-ack");
        QTest::newRow("eof-empty") << dsfm::minimum_checkpoint_interval << true << false << false << QStringLiteral("eof-receipt");
        QTest::newRow("eof-unavailable") << dsfm::minimum_checkpoint_interval << false << false << false << QStringLiteral("eof-unavailable");
        QTest::newRow("eof-corrupt") << dsfm::minimum_checkpoint_interval << false << false << false << QStringLiteral("eof-corrupt");
        QTest::newRow("eof-earlier") << dsfm::minimum_checkpoint_interval << false << false << false << QStringLiteral("eof-earlier");
        QTest::newRow("preview-eof") << dsfm::minimum_checkpoint_interval << false << false << true << QStringLiteral("preview-eof");
        QTest::newRow("preview-offered") << dsfm::minimum_checkpoint_interval << false << false << true << QStringLiteral("preview-offered");
        QTest::newRow("offered-reconnect") << dsfm::minimum_checkpoint_interval << false << false << false << QStringLiteral("offered-reconnect");
    }
    void authenticated_checkpoint_wire_transfer() {
        QFETCH(quint32, interval);
        QFETCH(bool, empty);
        QFETCH(bool, withhold);
        QFETCH(bool, signed_offer);
        QFETCH(QString, control);
        const bool eof_case = control.startsWith("eof-");
        const bool preview_case = control.startsWith("preview-");
        const bool resume_case = control == "resume" || control == "resume-missing" || control == "resume-corrupt" ||
                                 control == "resume-source" || control == "resume-metadata" || control == "resume-rewind-source" ||
                                 control == "resume-short-prefix";
        const auto old_auto_accept = engine_->settings().value("autoAcceptIncomingFiles");
        engine_->settings().setValue("autoAcceptIncomingFiles", false);
        IdentityData receiver_data;
        receiver_data.uuid = QUuid::createUuid();
        receiver_data.name = QStringLiteral("Wire rx %1").arg(QTest::currentDataTag());
        receiver_data.cert = ds::crypto::DsCert::create(); receiver_data.hash = receiver_data.cert->getHash().toByteArray();
        receiver_data.autoConnect = false;
        auto *receiver_identity = engine_->getIdentityManager()->addIdentity(receiver_data);
        QVERIFY(receiver_identity);
        receiver_identity->setPreviewImages(signed_offer);
        const auto add_contact = [&](Identity *owner, Identity *remote) {
            auto data = std::make_unique<ContactData>();
            data->identity = owner->getId(); data->name = "Checkpoint wire peer";
            data->cert = ds::crypto::DsCert::createFromPubkey(remote->getCert()->getSigningPubKey().toByteArray());
            data->address = "fixture-no-public-network"; data->autoConnect = false; data->state = Contact::ACCEPTED;
            auto *contact = engine_->getContactManager()->addContact(std::move(data));
            contact->setPeerVerified(true);
            return engine_->getContactManager()->getContact(contact->getUuid());
        };
        auto sender_contact = add_contact(identity_, receiver_identity);
        auto receiver_contact = add_contact(receiver_identity, identity_);
        auto sender_conversation = engine_->getConversationManager()->getConversation(sender_contact.get());
        const auto cleanup = qScopeGuard([&] {
            sender_contact->disconnectFromContact(true);
            receiver_contact->disconnectFromContact(true);
            engine_->settings().setValue("autoAcceptIncomingFiles", old_auto_accept);
            engine_->getContactManager()->deleteContact(sender_contact->getUuid());
            engine_->getContactManager()->deleteContact(receiver_contact->getUuid());
        });
        LoopbackListener listener;
        QVERIFY(listener.listen(QHostAddress::LocalHost, 0));
        std::shared_ptr<CheckpointWireServer> server;
        bool recovering = false;
        quint64 recovered_offset = std::numeric_limits<quint64>::max();
        int recovery_requests = 0;
        FileResumeStatus resume_status = FileResumeStatus::Rejected;
        dsfm::TransferPauseReason remote_pause = dsfm::TransferPauseReason::None;
        QObject observations;
        QVector<std::optional<dsfm::TransferAttempt>> signed_attempts;
        QByteArray manifest_signature, manifest_id;
        bool replayed_manifest_matches = false;
        listener.accepted = [&](ds::prot::ConnectionSocket::ptr_t socket) {
            ConnectData data;
            data.service = receiver_identity->getUuid(); data.identitysCert = receiver_identity->getCert();
            server = std::make_shared<CheckpointWireServer>(std::move(socket), data);
            server->preference = interval;
            server->withhold_proposals = withhold && !resume_case && !recovering;
            server->withhold_receipts = (resume_case || preview_case || (eof_case && control != "eof-ack")) && !recovering;
            server->final_receipts_only = eof_case || preview_case;
            server->withhold_final_ack = control == "eof-ack" && !recovering;
            connect(server.get(), &PeerConnection::receivedFileResumeResult, &observations, [&](const PeerFileResumeResult &result) {
                if (result.checkpoint_attempt) recovered_offset = result.checkpoint_attempt->selected.offset;
                resume_status = result.status;
                remote_pause = result.pause;
            });
            connect(server.get(), &PeerConnection::receivedMessage, &observations, [&](const PeerMessage &message) {
                signed_attempts = message.attachment_attempts;
                if (!recovering) { manifest_signature = message.data.signature; manifest_id = message.data.messageId; }
                else replayed_manifest_matches = message.data.signature == manifest_signature && message.data.messageId == manifest_id;
            });
            connect(server.get(), &ds::prot::Peer::incomingPeer, receiver_contact.get(), &Contact::onIncomingPeer);
        };
        ds::prot::ConnectionSocket::ptr_t socket;
        std::shared_ptr<CheckpointWireClient> client;
        int repeated_readiness = 0;
        const auto connect_wire = [&] {
            socket = ds::prot::ConnectionSocket::create();
            ConnectData data;
            data.service = identity_->getUuid(); data.identitysCert = identity_->getCert();
            data.contactsCert = receiver_identity->getCert();
            client = std::make_shared<CheckpointWireClient>(socket, data);
            client->preference = dsfm::maximum_checkpoint_interval;
            client->duplicate_recovery_offers = eof_case;
            client->select_earlier_completion = control == "eof-earlier";
            connect(client.get(), &PeerConnection::receivedFileResume, &observations, [&](const PeerFileResume &) {
                if (recovering && eof_case) ++recovery_requests;
            });
            connect(client.get(), &PeerConnection::receivedFileOfferResponse, &observations,
                    [&](const PeerFileOfferResponse &response) {
                if (control == "duplicate-readiness" && !repeated_readiness &&
                    response.status == FileOfferResponseStatus::Accepted) {
                    // This observer precedes Contact's signal connection. Start
                    // its journal commit, then let normal dispatch deliver the
                    // same exact readiness before that asynchronous work ends.
                    ++repeated_readiness;
                    emit client->receivedFileOfferResponse(response);
                }
            });
            connect(client.get(), &ds::prot::Peer::connectedToPeer, sender_contact.get(), &Contact::onConnectedToPeer);
            socket->connectToHost(QHostAddress::LocalHost, listener.serverPort());
        };
        connect_wire();
        QTRY_COMPARE_WITH_TIMEOUT(client->checkpointInterval(), interval, 5000);
        QVERIFY(server);
        QTRY_COMPARE_WITH_TIMEOUT(server->checkpointInterval(), interval, 5000);
        QByteArray bytes(empty ? 0 : qsizetype(interval) + 7, 'w');
        QImage image(16, 16, QImage::Format_RGB32);
        image.fill(Qt::green);
        if (signed_offer) {
            bytes.clear();
            QBuffer encoded(&bytes); QVERIFY(encoded.open(QIODevice::WriteOnly));
            QVERIFY(image.save(&encoded, "PNG"));
        }
        const auto source_name = QStringLiteral("checkpoint-wire-source-%1").arg(QTest::currentDataTag());
        const auto path = directory_.filePath(source_name);
        QFile source(path);
        QVERIFY(source.open(QIODevice::WriteOnly));
        QCOMPARE(source.write(bytes), bytes.size()); source.close();
        auto imported = waitTask(storage().importFile(path, source_name));
        QVERIFY(imported);
        auto file_data = std::make_unique<FileData>();
        file_data->state = File::FS_WAITING; file_data->identity = identity_->getId();
        file_data->contact = sender_contact->getId(); file_data->conversation = sender_conversation->getId();
        file_data->storage_namespace = imported.value().namespace_id; file_data->storage_id = imported.value().file.id;
        file_data->name = QStringLiteral("wire-transfer-%1.bin").arg(QTest::currentDataTag());
        file_data->size = bytes.size();
        file_data->mime_type = signed_offer ? "image/png" : "application/octet-stream";
        file_data->hash = imported.value().file.sha256;
        auto outgoing = engine_->getFileManager()->addFile(std::move(file_data));
        QSignalSpy local_proof(outgoing.get(), &File::sourceIntegrityFailed);
        File::ptr_t thumbnail;
        if (signed_offer) {
            QByteArray jpeg; QBuffer encoded(&jpeg); QVERIFY(encoded.open(QIODevice::WriteOnly));
            QVERIFY(image.save(&encoded, "JPEG"));
            auto preview = std::make_unique<FileData>();
            preview->state = File::FS_HASHING; preview->identity = identity_->getId();
            preview->contact = sender_contact->getId(); preview->conversation = sender_conversation->getId();
            preview->name = "preview.jpg"; preview->mime_type = "image/jpeg"; preview->size = jpeg.size();
            preview->hash = QCryptographicHash::hash(jpeg, QCryptographicHash::Sha256);
            preview->thumbnail_for = outgoing->getFileId(); preview->manifest_id = ds::crypto::Crypto::getRandomBytes(32);
            thumbnail = engine_->getFileManager()->addFile(std::move(preview));
            QVERIFY(waitTask(thumbnail->importThumbnail(jpeg)));
            if (preview_case) server->withheld_file_id = thumbnail->getFileId();
        }
        sender_contact->queueFile(outgoing);
        File::ptr_t received;
        QTRY_VERIFY_WITH_TIMEOUT((received = engine_->getFileManager()->getFileFromId(
            outgoing->getFileId(), *receiver_contact, File::INCOMING)), 5000);
        QCOMPARE(outgoing->checkpointInterval(), interval);
        QCOMPARE(received->checkpointInterval(), interval);
        QVERIFY(received->checkpointAttempt());
        QCOMPARE(*received->checkpointAttempt(), *outgoing->checkpointAttempt());
        if (signed_offer) {
            QCOMPARE(signed_attempts.size(), 2);
            QVERIFY(signed_attempts[0]); QVERIFY(signed_attempts[1]);
            QCOMPARE(*signed_attempts[0], *outgoing->checkpointAttempt());
            QCOMPARE(*signed_attempts[1], *thumbnail->checkpointAttempt());
        }
        const auto attempt = *outgoing->checkpointAttempt();
        const auto sender_key = outgoing->checkpointKey().value();
        auto before = waitTask(storage().transferProgress().load(sender_key));
        QVERIFY(before);
        QCOMPARE(before.value().attempt, attempt.id); // Persisted before initial offer.
        if (control != "preview-offered" && control != "offered-reconnect") received->accept();
        if (control == "offered-reconnect") {
            sender_contact->disconnectFromContact(true);
            QTRY_VERIFY_WITH_TIMEOUT(!receiver_contact->isOnline(), 5000);
            QCOMPARE(outgoing->getState(), File::FS_OFFERED);
            QCOMPARE(received->getState(), File::FS_OFFERED);
            received->accept(); // Approval while offline remains queued.
            QCOMPARE(received->getState(), File::FS_QUEUED);
            recovering = true;
            connect_wire();
            QTRY_COMPARE_WITH_TIMEOUT(client->checkpointInterval(), interval, 5000);
            QTRY_COMPARE_WITH_TIMEOUT(outgoing->getState(), File::FS_DONE, 5000);
            QTRY_COMPARE_WITH_TIMEOUT(received->getState(), File::FS_DONE, 5000);
            QVERIFY(outgoing->checkpointAttempt()->id != attempt.id);
        }
        if (preview_case) {
            auto received_preview = engine_->getFileManager()->getFileFromId(thumbnail->getFileId(), *receiver_contact, File::INCOMING);
            QVERIFY(received_preview);
            QTRY_COMPARE_WITH_TIMEOUT(received_preview->getState(), File::FS_DONE, 5000);
            QVERIFY(thumbnail->getState() != File::FS_DONE);
            const auto parent_state = control == "preview-offered" ? File::FS_OFFERED : File::FS_DONE;
            QTRY_COMPARE_WITH_TIMEOUT(outgoing->getState(), parent_state, 5000);
            QCOMPARE(received->getState(), parent_state);
            const auto published = received_preview->storageId();
            sender_contact->disconnectFromContact(true);
            QTRY_COMPARE_WITH_TIMEOUT(thumbnail->getState(), File::FS_PAUSED, 5000);
            QCOMPARE(outgoing->getState(), parent_state); QCOMPARE(received->getState(), parent_state);
            QSignalSpy source_parent_states(outgoing.get(), &File::stateChanged);
            QSignalSpy receiver_parent_states(received.get(), &File::stateChanged);
            QSignalSpy preview_states(received_preview.get(), &File::stateChanged);
            recovering = true;
            connect_wire();
            QTRY_COMPARE_WITH_TIMEOUT(client->checkpointInterval(), interval, 5000);
            QTRY_COMPARE_WITH_TIMEOUT(thumbnail->getState(), File::FS_DONE, 5000);
            QVERIFY(replayed_manifest_matches);
            QCOMPARE(client->payload_bytes, quint64(0));
            QCOMPARE(received_preview->getState(), File::FS_DONE);
            QCOMPARE(received_preview->storageId(), published);
            QVERIFY(received_preview->pendingId().isNull());
            QCOMPARE(source_parent_states.count(), 0); QCOMPARE(receiver_parent_states.count(), 0);
            QCOMPARE(preview_states.count(), 0);
            if (control == "preview-offered") { outgoing->cancel(); received->cancel(); }
            thumbnail->deleteFromDb(); received_preview->deleteFromDb();
            outgoing->deleteFromDb(); received->deleteFromDb();
            return;
        }
        if (eof_case) {
            QTRY_COMPARE_WITH_TIMEOUT(received->getState(), File::FS_DONE, 5000);
            QVERIFY(outgoing->getState() != File::FS_DONE);
            const auto published = received->storageId();
            const auto published_namespace = received->storageNamespace();
            QVERIFY(received->pendingId().isNull());
            auto saved = waitTask(storage().transferProgress().load(sender_key));
            QVERIFY(saved);
            QVERIFY(std::any_of(saved.value().accepted_checkpoints.begin(), saved.value().accepted_checkpoints.end(),
                                [&](const auto &checkpoint) { return checkpoint.final && checkpoint.offset == quint64(bytes.size()); }));
            sender_contact->disconnectFromContact(true);
            QTRY_COMPARE_WITH_TIMEOUT(outgoing->getState(), File::FS_PAUSED, 5000);
            QCOMPARE(received->getState(), File::FS_DONE);
            QSignalSpy receiver_states(received.get(), &File::stateChanged);
            if (control == "eof-unavailable") QVERIFY(waitTask(storage().manager().remove(published_namespace, published)));
            if (control == "eof-corrupt") {
                const auto id = published.toString(QUuid::WithoutBraces);
                auto ns = waitTask(storage().manager().namespaceInfo(published_namespace)); QVERIFY(ns);
                const auto path = ns.value().config.naming == dsfm::PhysicalNaming::OriginalName
                    ? QDir(ns.value().config.storage_root).filePath(received->getName())
                    : ns.value().config.storage_root + "/objects/" + id.left(2) + '/' + id;
                const auto fd = ::open(QFile::encodeName(path).constData(), O_RDWR | O_CLOEXEC | O_NOFOLLOW);
                QVERIFY(fd >= 0);
                auto close_fd = qScopeGuard([fd] { ::close(fd); });
                struct stat before{}; QVERIFY(::fstat(fd, &before) == 0);
                QCOMPARE(::pwrite(fd, "x", 1, 0), ssize_t(1));
                const timespec times[]{before.st_atim, before.st_mtim}; QVERIFY(::futimens(fd, times) == 0);
            }
            recovering = true;
            connect_wire();
            QTRY_COMPARE_WITH_TIMEOUT(client->checkpointInterval(), interval, 5000);
            QTRY_COMPARE_WITH_TIMEOUT(server->checkpointInterval(), interval, 5000);
            QTRY_COMPARE_WITH_TIMEOUT(recovery_requests, 1, 5000); // Reconnect alone triggers the repair.
            QTRY_COMPARE_WITH_TIMEOUT(resume_status, FileResumeStatus::Accepted, 5000);
            QCOMPARE(recovered_offset, control == "eof-earlier" ? quint64(0) : quint64(bytes.size()));
            const bool refused = control == "eof-unavailable" || control == "eof-corrupt" || control == "eof-earlier";
            if (refused) {
                QTest::qWait(150);
                QVERIFY(outgoing->getState() != File::FS_DONE);
                if (control == "eof-earlier") QTRY_VERIFY_WITH_TIMEOUT(!server->getConnection().isOpen(), 5000);
            } else {
                QTRY_COMPARE_WITH_TIMEOUT(outgoing->getState(), File::FS_DONE, 5000);
            }
            QCOMPARE(received->getState(), File::FS_DONE);
            QCOMPARE(received->storageId(), published);
            QCOMPARE(received->storageNamespace(), published_namespace);
            QVERIFY(received->pendingId().isNull());
            QCOMPARE(receiver_states.count(), 0);
            QCOMPARE(client->payload_bytes, quint64(0));
            if (refused) {
                outgoing->cancel(); outgoing->deleteFromDb(); received->deleteFromDb();
                return;
            }
        }
        if (withhold) {
            QTRY_VERIFY_WITH_TIMEOUT(bool(server->withheld), 5000);
            QTRY_COMPARE_WITH_TIMEOUT(outgoing->getBytesTransferred(), qlonglong(interval), 5000);
            QCOMPARE(client->admitted_end, quint64(interval));
            QTest::qWait(100);
            QCOMPARE(client->admitted_end, quint64(interval)); // Ordinary ACKs cannot release the durable window.
            if (control == "receiver-mismatch" || control == "unsupported-version" || control == "remote-source-cancel") {
                QSignalSpy remote_proof(received.get(), &File::sourceIntegrityFailed);
                const auto reason = control == "receiver-mismatch" ? dsfm::TransferFailureReason::ReceiverCheckpointMismatch
                    : control == "unsupported-version" ? dsfm::TransferFailureReason::UnsupportedVersion
                                                       : dsfm::TransferFailureReason::RemoteSourceChanged;
                if (control == "receiver-mismatch") {
                    const dsfm::DurableCheckpoint incorrect{1, interval, QByteArray(32, 'x'), false};
                    server->withhold_proposals = false;
                    server->sendFileCheckpoint(outgoing->getFileId(), outgoing->getHash(), attempt, incorrect,
                                               dsfm::CheckpointPhase::Proposed);
                    QTRY_COMPARE_WITH_TIMEOUT(outgoing->getState(), File::FS_FAILED, 5000);
                    QCOMPARE(outgoing->failureReason(), int(reason));
                } else {
                    client->sendFileCancel(outgoing->getFileId(), control == "unsupported-version"
                        ? FileCancelReason::UnsupportedTransferVersion : FileCancelReason::SourceChanged);
                }
                QTRY_COMPARE_WITH_TIMEOUT(received->getState(), File::FS_FAILED, 5000);
                QCOMPARE(received->failureReason(), int(reason));
                QCOMPARE(local_proof.count(), 0); QCOMPARE(remote_proof.count(), 0);
                QVERIFY(!received->isResumable());
                auto saved = waitTask(storage().transferProgress().load(received->checkpointKey().value()));
                QVERIFY(saved); QCOMPARE(saved.value().failure, reason);
                QCOMPARE(saved.value().verified_prefix, quint64(0));
                QVERIFY(socket->isOpen()); // These are supported terminal outcomes.
                outgoing->cancel();
                if (control == "receiver-mismatch") {
                    QTRY_VERIFY_WITH_TIMEOUT(!engine_->transfers().find(sender_key), 5000);
                    const auto settled = waitTask(storage().transferProgress().load(sender_key));
                    QVERIFY(settled); QCOMPARE(settled.value().failure, reason);
                    QCOMPARE(outgoing->getState(), File::FS_FAILED);
                }
                outgoing->deleteFromDb(); received->deleteFromDb();
                return;
            }
            if (control == "stale-ack") {
                server->sendCheckpointAck(outgoing->getFileId(), QUuid::createUuid(), 8, interval, false);
                QTRY_VERIFY_WITH_TIMEOUT(!socket->isOpen(), 5000);
                auto saved = waitTask(storage().transferProgress().load(sender_key));
                QVERIFY(saved);
                QCOMPARE(saved.value().confirmed_offset, quint64(0));
                QCOMPARE(saved.value().failure, dsfm::TransferFailureReason::None);
                outgoing->cancel(); received->cancel();
                outgoing->deleteFromDb(); received->deleteFromDb();
                return;
            }
            if (control == "resume-fields") {
                QSignalSpy dispatched(client.get(), &PeerConnection::receivedFileResume);
                ds::protocol::FileResume request;
                request.setFileId(outgoing->getFileId()); request.setFileSize(bytes.size());
                request.setSha256(outgoing->getHash()); request.setCheckpointVersion(1);
                request.setCheckpointIntervalBytes(interval);
                const auto zero = ds::prot::codec::checkpointReference(attempt.selected);
                request.setAvailableCheckpoints({zero, zero});
                ds::protocol::Envelope envelope;
                envelope.setProtocolVersion(ds::prot::codec::protocolVersion);
                envelope.setRequestId(server->nextRequestId(ds::prot::Peer::RequestType::FileResumeResult));
                envelope.setFileResume(std::move(request));
                server->send(envelope);
                QTRY_VERIFY_WITH_TIMEOUT(!socket->isOpen(), 5000);
                QCOMPARE(dispatched.count(), 0); // Malformed checkpoint metadata never enters the legacy path.
                outgoing->cancel(); received->cancel();
                outgoing->deleteFromDb(); received->deleteFromDb();
                return;
            }
            if (control == "pause") {
                // Even a well-shaped advisory EOF cannot establish progress or
                // turn a peer's source assertion into locally proved corruption.
                const dsfm::DurableCheckpoint advisory{2, quint64(bytes.size()), outgoing->getHash(), true};
                server->sendFilePause(outgoing->getFileId(), attempt.id, advisory, dsfm::TransferPauseReason::SourceUnavailable);
                QTRY_COMPARE_WITH_TIMEOUT(outgoing->getState(), File::FS_PAUSED, 5000);
                auto saved = waitTask(storage().transferProgress().load(sender_key));
                QVERIFY(saved);
                QCOMPARE(saved.value().confirmed_offset, quint64(0));
                QCOMPARE(saved.value().failure, dsfm::TransferFailureReason::None);
                QCOMPARE(saved.value().pause, dsfm::TransferPauseReason::SourceUnavailable);
                QVERIFY(!saved.value().complete);
                server->sendFilePause(outgoing->getFileId(), attempt.id, advisory, dsfm::TransferPauseReason::SourceUnavailable);
                QTest::qWait(50);
                QVERIFY(socket->isOpen());
                outgoing->cancel(); received->cancel();
                outgoing->deleteFromDb(); received->deleteFromDb();
                return;
            }
            if (resume_case) {
                auto saved = waitTask(storage().transferProgress().load(sender_key));
                QVERIFY(saved);
                QCOMPARE(saved.value().confirmed_offset, quint64(0));
                QVERIFY(std::any_of(saved.value().accepted_checkpoints.begin(), saved.value().accepted_checkpoints.end(),
                                    [&](const auto &checkpoint) { return checkpoint.offset == interval; }));
                if (control == "resume-short-prefix") {
                    const dsfm::DurableCheckpoint shorter{2, 8190,
                        QCryptographicHash::hash(bytes.first(8190), QCryptographicHash::Sha256), false, true};
                    QVERIFY(waitTask(storage().transferProgress().record(sender_key, attempt.id, shorter,
                                                                         dsfm::CheckpointPhase::SenderConfirmed)));
                }
                sender_contact->disconnectFromContact(true);
                QTRY_COMPARE_WITH_TIMEOUT(outgoing->getState(), File::FS_PAUSED, 5000);
                QTRY_COMPARE_WITH_TIMEOUT(received->getState(), File::FS_PAUSED, 5000);
                QSignalSpy remote_proof(received.get(), &File::sourceIntegrityFailed);
                QVERIFY(!outgoing->isResumable()); // Only the receiver initiates checkpoint recovery.
                if (control == "resume-missing")
                    QVERIFY(waitTask(storage().manager().remove(outgoing->storageNamespace(), outgoing->storageId())));
                if (control == "resume-corrupt" || control == "resume-rewind-source" || control == "resume-short-prefix") {
                    QVERIFY(waitTask(received->prepareIncoming()));
                    QVERIFY(waitTask(received->writeIncoming(0, "x")));
                    QVERIFY(waitTask(received->checkpointIncoming()));
                    QVERIFY(waitTask(received->suspendIncoming()));
                }
                if (control == "resume-source" || control == "resume-metadata" || control == "resume-rewind-source") {
                    auto changed = bytes;
                    changed[0] = 'x';
                    const auto expected = outgoing->getHash();
                    const bool metadata_refusal = control == "resume-metadata";
                    const bool changes_after_selection = control == "resume-rewind-source";
                    auto preparations = std::make_shared<int>(0);
                    QVERIFY(outgoing->setPreparedSource(identity_->getUuid(), sender_contact->getUuid(), [] { return true; },
                        [bytes, changed, expected, metadata_refusal, changes_after_selection, preparations](dsfm::ReadPreparationOptions options) -> QCoro::Task<dsfm::Result<dsfm::PreparedRead>> {
                            if (options.cancel.cancelled()) co_return dsfm::Error{dsfm::ErrorCode::Cancelled};
                            if (metadata_refusal) co_return dsfm::Error{dsfm::ErrorCode::IntegrityError}; // Conservative backend/cache refusal.
                            dsfm::FileInfo info;
                            info.size = changed.size(); info.sha256 = expected;
                            const auto &content = changes_after_selection && ++*preparations == 1 ? bytes : changed;
                            co_return dsfm::PreparedRead{info, std::make_unique<CheckpointFixtureReader>(content)};
                        }));
                }
                recovering = true;
                connect_wire();
                QTRY_COMPARE_WITH_TIMEOUT(client->checkpointInterval(), interval, 5000);
                QTRY_COMPARE_WITH_TIMEOUT(server->checkpointInterval(), interval, 5000);
                received->resume();
                if (control == "resume-rewind-source") {
                    QTRY_COMPARE_WITH_TIMEOUT(recovered_offset, quint64(0), 5000);
                    QTRY_COMPARE_WITH_TIMEOUT(outgoing->getState(), File::FS_FAILED, 5000);
                    QTRY_COMPARE_WITH_TIMEOUT(received->getState(), File::FS_FAILED, 5000);
                    QCOMPARE(outgoing->failureReason(), int(dsfm::TransferFailureReason::SourceIntegrityMismatch));
                    QCOMPARE(received->failureReason(), int(dsfm::TransferFailureReason::RemoteSourceChanged));
                    QCOMPARE(local_proof.count(), 1); QCOMPARE(remote_proof.count(), 0);
                    // Catch the trusted retained-prefix mismatch before its
                    // boundary is sent, rather than waiting for full EOF.
                    QVERIFY(client->admitted_end < quint64(interval));
                    outgoing->deleteFromDb(); received->deleteFromDb();
                    return;
                }
                if (control == "resume-source") {
                    QTRY_COMPARE_WITH_TIMEOUT(resume_status, FileResumeStatus::Cancelled, 5000);
                    QTRY_COMPARE_WITH_TIMEOUT(received->getState(), File::FS_FAILED, 5000);
                    QCOMPARE(outgoing->getState(), File::FS_FAILED);
                    QCOMPARE(outgoing->failureReason(), int(dsfm::TransferFailureReason::SourceIntegrityMismatch));
                    QCOMPARE(received->failureReason(), int(dsfm::TransferFailureReason::RemoteSourceChanged));
                    QCOMPARE(local_proof.count(), 1);
                    QCOMPARE(remote_proof.count(), 0);
                    QVERIFY(!received->isResumable());
                    outgoing->deleteFromDb(); received->deleteFromDb();
                    return;
                }
                if (control == "resume-missing" || control == "resume-metadata") {
                    const auto expected_pause = control == "resume-missing" ? dsfm::TransferPauseReason::SourceUnavailable
                                                                           : dsfm::TransferPauseReason::SourceRevalidationRequired;
                    QTRY_COMPARE_WITH_TIMEOUT(resume_status, FileResumeStatus::Paused, 5000);
                    QCOMPARE(remote_pause, expected_pause);
                    QTRY_COMPARE_WITH_TIMEOUT(received->getState(), File::FS_PAUSED, 5000);
                    QCOMPARE(outgoing->getState(), File::FS_PAUSED);
                    auto local = waitTask(storage().transferProgress().load(sender_key));
                    auto remote = waitTask(storage().transferProgress().load(received->checkpointKey().value()));
                    QVERIFY(local); QVERIFY(remote);
                    QCOMPARE(local.value().pause, expected_pause);
                    QCOMPARE(remote.value().pause, expected_pause);
                    QCOMPARE(local.value().failure, dsfm::TransferFailureReason::None);
                    QCOMPARE(remote.value().failure, dsfm::TransferFailureReason::None);
                    QCOMPARE(local.value().confirmed_offset, quint64(0));
                    QCOMPARE(local_proof.count(), 0); QCOMPARE(remote_proof.count(), 0);
                    QVERIFY(!received->pendingId().isNull());
                    outgoing->cancel(); received->cancel();
                    outgoing->deleteFromDb(); received->deleteFromDb();
                    return;
                }
                QTRY_COMPARE_WITH_TIMEOUT(recovered_offset,
                    control == "resume-corrupt" || control == "resume-short-prefix" ? quint64(0) : quint64(interval), 5000);
                QTRY_VERIFY_WITH_TIMEOUT(outgoing->checkpointAttempt()->id != attempt.id, 5000);
                QTRY_VERIFY_WITH_TIMEOUT(received->checkpointAttempt()->id != attempt.id, 5000);
                QCOMPARE(outgoing->getFileId(), received->getFileId());
            } else {
                server->withhold_proposals = false;
                auto deliver = std::move(server->withheld);
                deliver();
            }
        }
        QTRY_COMPARE_WITH_TIMEOUT(received->getState(), File::FS_DONE, 60000);
        QTRY_COMPARE_WITH_TIMEOUT(outgoing->getState(), File::FS_DONE, 5000);
        if (control == "duplicate-readiness") {
            QCOMPARE(repeated_readiness, 1);
            QVERIFY(socket->isOpen());
        }
        server->sendFileOfferResponse(outgoing->getFileId(), FileOfferResponseStatus::Rejected);
        QTest::qWait(50);
        QCOMPARE(outgoing->getState(), File::FS_DONE); // A delayed offer rejection cannot rewrite a final outcome.
        auto sent = waitTask(storage().transferProgress().load(sender_key));
        auto accepted = waitTask(storage().transferProgress().load(received->checkpointKey().value()));
        QVERIFY(sent); QVERIFY(accepted);
        QVERIFY(sent.value().complete); QVERIFY(accepted.value().complete);
        QCOMPARE(sent.value().confirmed_offset, quint64(bytes.size()));
        QCOMPARE(accepted.value().confirmed_offset, quint64(bytes.size()));
        auto reader = waitTask(storage().manager().openRead(received->storageNamespace(), received->storageId()));
        QVERIFY(reader);
        // Verify complete content in locally bounded reads, including empty EOF.
        for (qsizetype offset = 0; offset < bytes.size(); offset += 65536) {
            auto block = waitTask(reader.value()->readAt(offset, qMin<qsizetype>(65536, bytes.size() - offset)));
            QVERIFY(block); QCOMPARE(block.value(), bytes.mid(offset, block.value().size()));
        }
        QVERIFY(waitTask(reader.value()->close()));
        if (thumbnail) {
            QTRY_COMPARE_WITH_TIMEOUT(thumbnail->getState(), File::FS_DONE, 5000);
            thumbnail->deleteFromDb();
        }
        outgoing->deleteFromDb(); received->deleteFromDb();
    }
    void unstarted_receiver_admission_pause_survives_reload() {
        for (const bool has_journal : {false, true}) {
            auto data = std::make_unique<FileData>(); data->state = File::FS_OFFERED; data->direction = File::INCOMING;
            data->identity = identity_->getId(); data->contact = contact_->getId(); data->conversation = conversation_->getId();
            data->name = "admission-pause.bin"; data->mime_type = "application/octet-stream"; data->size = 6;
            data->hash = QCryptographicHash::hash("source", QCryptographicHash::Sha256);
            data->checkpoint_interval = dsfm::minimum_checkpoint_interval;
            auto file = engine_->getFileManager()->addFile(std::move(data));
            if (has_journal) QVERIFY(waitTask(file->prepareCheckpointProgress(file->checkpointInterval())));
            QVERIFY(waitTask(file->pauseCheckpointProgress(dsfm::TransferPauseReason::ResourceUnavailable)));
            auto loaded = File::load(*engine_->getFileManager(), file->getId());
            QCOMPARE(loaded->getState(), File::FS_PAUSED);
            QCOMPARE(loaded->dataPauseReason(), dsfm::TransferPauseReason::ResourceUnavailable);
            QVERIFY(loaded->isResumable()); QVERIFY(!loaded->checkpointAttempt());
            QVERIFY(loaded->storageId().isNull()); QVERIFY(loaded->pendingId().isNull());
            auto progress = waitTask(storage().transferProgress().load(file->checkpointKey().value()));
            if (has_journal) {
                QVERIFY(progress); QVERIFY(progress.value().attempt.isNull());
                QCOMPARE(progress.value().verified_prefix, quint64(0));
                QCOMPARE(progress.value().pause, dsfm::TransferPauseReason::ResourceUnavailable);
            } else { QVERIFY(!progress); QCOMPARE(progress.error().code, dsfm::ErrorCode::MissingEntry); }
            file->deleteFromDb();
        }
    }
    void cancelling_unstarted_receiver_does_not_promote_offer_hint() {
        const auto file = incoming("unstarted", "unstarted-cancel.bin", "application/octet-stream");
        auto saved = waitTask(file->prepareCheckpointProgress(dsfm::minimum_checkpoint_interval));
        QVERIFY(saved);
        const dsfm::TransferAttempt hint{QUuid::createUuid(), dsfm::minimum_checkpoint_interval, 1,
                                         saved.value().checkpoints.first()};
        QVERIFY(file->setCheckpointAttempt(hint));
        file->setState(File::FS_QUEUED);
        QSignalSpy integrity(file.get(), &File::sourceIntegrityFailed);
        file->cancel();
        QTRY_COMPARE_WITH_TIMEOUT(file->getState(), File::FS_CANCELLED, 5000);
        QTRY_COMPARE_WITH_TIMEOUT(file->failureReason(), int(dsfm::TransferFailureReason::UserAction), 5000);
        auto cancelled = waitTask(storage().transferProgress().load(file->checkpointKey().value()));
        QVERIFY(cancelled);
        QCOMPARE(cancelled.value().failure, dsfm::TransferFailureReason::UserAction);
        QVERIFY(cancelled.value().attempt.isNull());
        QCOMPARE(cancelled.value().verified_prefix, quint64(0));
        QCOMPARE(cancelled.value().confirmed_offset, quint64(0));
        QCOMPARE(integrity.count(), 0);
        auto loaded = File::load(*engine_->getFileManager(), file->getId());
        QCOMPARE(loaded->getState(), File::FS_CANCELLED);
        QCOMPARE(loaded->failureReason(), int(dsfm::TransferFailureReason::UserAction));
        QVERIFY(file->storageId().isNull()); QVERIFY(file->pendingId().isNull());
        file->deleteFromDb();
    }
    void durable_completion_repairs_outgoing_application_state() {
        auto data = std::make_unique<FileData>(); data->state = File::FS_PAUSED;
        data->identity = identity_->getId(); data->contact = contact_->getId(); data->conversation = conversation_->getId();
        data->name = "completed-certificate.bin"; data->mime_type = "application/octet-stream";
        data->size = 6; data->hash = QCryptographicHash::hash("source", QCryptographicHash::Sha256);
        auto file = engine_->getFileManager()->addFile(std::move(data));
        auto saved = waitTask(file->prepareCheckpointProgress(dsfm::minimum_checkpoint_interval)); QVERIFY(saved);
        const auto attempt = QUuid::createUuid(); const auto key = file->checkpointKey().value();
        QVERIFY(waitTask(storage().transferProgress().beginAttempt(key, attempt, saved.value().checkpoints.first())));
        QVERIFY(waitTask(storage().transferProgress().record(key, attempt, {1, 6, file->getHash(), true}, dsfm::CheckpointPhase::MutuallyConfirmed)));
        QVERIFY(waitTask(storage().transferProgress().complete(key, attempt, true, true)));
        QSqlQuery stale;
        stale.prepare("UPDATE file_transfer_v1 SET pause_reason=1 WHERE file_id=?"); stale.addBindValue(file->getId()); QVERIFY(stale.exec());
        auto loaded = File::load(*engine_->getFileManager(), file->getId());
        QTRY_COMPARE_WITH_TIMEOUT(loaded->getState(), File::FS_DONE, 5000);
        QCOMPARE(loaded->dataPauseReason(), dsfm::TransferPauseReason::None);
        QVERIFY(!loaded->checkpointAttempt()); // A certificate repairs history without recreating a live channel.
        auto terminal = waitTask(storage().transferProgress().load(key)); QVERIFY(terminal); QVERIFY(terminal.value().complete);
        file->deleteFromDb();
    }
    void remote_source_failure_is_terminal_without_local_integrity_proof() {
        auto file = incoming("bytes", "remote-source-failure.bin");
        auto progress = waitTask(file->prepareCheckpointProgress(dsfm::minimum_checkpoint_interval));
        QVERIFY(progress);
        QSignalSpy proof(file.get(), &File::sourceIntegrityFailed);
        QVERIFY(!waitTask(file->failCheckpoint(dsfm::TransferFailureReason::SourceIntegrityMismatch)));
        QVERIFY(waitTask(file->failCheckpoint(dsfm::TransferFailureReason::RemoteSourceChanged)));
        file->transferFailed("remote source changed", File::FS_FAILED);
        QCOMPARE(proof.count(), 0);
        QVERIFY(!file->isResumable());
        auto loaded = File::load(*engine_->getFileManager(), file->getId());
        QCOMPARE(loaded->failureReason(), int(dsfm::TransferFailureReason::RemoteSourceChanged));
        QCOMPARE(loaded->getState(), File::FS_FAILED);
        QVERIFY(!loaded->isResumable());
        auto saved = waitTask(storage().transferProgress().load(file->checkpointKey().value()));
        QVERIFY(saved);
        QCOMPARE(saved.value().failure, dsfm::TransferFailureReason::RemoteSourceChanged);
        file->deleteFromDb();
    }
    void application_reference_recovers_after_publication() {
        const QByteArray bytes("published before application completion was saved");
        auto file = incoming(bytes, "recover.bin");
        QVERIFY(waitTask(file->prepareIncoming()));
        QVERIFY(waitTask(file->writeIncoming(0, bytes)));
        QVERIFY(waitTask(file->suspendIncoming()));
        auto pending = waitTask(storage().manager().openPending(file->storageNamespace(), file->pendingId())); QVERIFY(pending);
        QVERIFY(waitTask(pending.value().writer->commit()));
        file->setState(File::FS_HASHING);
        auto recovered = File::load(*engine_->getFileManager(), file->getId());
        QTRY_COMPARE_WITH_TIMEOUT(recovered->getState(), File::FS_DONE, 10000);
        QVERIFY(recovered->pendingId().isNull()); QCOMPARE(recovered->getBytesTransferred(), qlonglong(bytes.size()));
    }
    void empty_incoming_file_is_verified() {
        auto file = incoming({}, "empty.txt"); QVERIFY(waitTask(file->prepareIncoming()));
        auto approved = approveFixtureContact();
        auto peer = receiveOwned(file, {});
        QTRY_COMPARE_WITH_TIMEOUT(file->getState(), File::FS_DONE, 10000);
    }
    void malformed_incoming_checksum_and_hostile_names_are_rejected() {
        auto file = incoming("bytes", "valid.bin"); file->setHash({});
        auto result = waitTask(file->prepareIncoming()); QVERIFY(!result); QCOMPARE(result.error().code, dsfm::ErrorCode::InvalidArgument);
        auto invalid = waitTask(storage().importFile("/dev/zero", "zero")); QVERIFY(!invalid);
        auto traversal = waitTask(storage().exportFile({}, {}, "../escape")); QVERIFY(!traversal);
    }
    void asynchronous_file_operations_own_the_file_lifetime() {
        auto file = incoming("lifetime", "lifetime.bin");
        auto uncached = File::load(*engine_->getFileManager(), file->getId());
        std::weak_ptr<File> weak = uncached;
        auto preparing = uncached->prepareIncoming();
        uncached.reset(); QVERIFY(!weak.expired());
        auto result = waitTask(std::move(preparing)); QVERIFY(result);
        QTRY_VERIFY_WITH_TIMEOUT(weak.expired(), 10000);
        file->deleteFromDb(); QVERIFY(waitTask(storage().collectGarbage()));
    }
    void concurrent_receivers_share_one_managed_conversation_directory() {
        engine_->settings().setValue("includeConversationNameInDownloadPath", true);
        auto first = incoming("one", "first.txt"), second = incoming("two", "second.txt");
        auto preparing_first = first->prepareIncoming(), preparing_second = second->prepareIncoming();
        QVERIFY(waitTask(std::move(preparing_first))); QVERIFY(waitTask(std::move(preparing_second)));
        QVERIFY(waitTask(first->abortIncoming())); QVERIFY(waitTask(second->abortIncoming()));
        engine_->settings().setValue("includeConversationNameInDownloadPath", false);
    }
    void avatars_use_managed_storage_not_database_blobs() {
        const auto key = ManagedStorage::identityAvatar(identity_->getUuid());
        QImage image(32, 32, QImage::Format_RGB32); image.fill(Qt::red);
        identity_->setAvatar(image);
        std::optional<dsfm::Result<QByteArray>> bytes;
        QTRY_VERIFY_WITH_TIMEOUT(([&] { bytes = waitTask(storage().readAsset(key)); return bool(*bytes); })(), 10000);
        QVERIFY(!QImage::fromData(bytes->value()).isNull());
        QSqlQuery query("SELECT avatar FROM identity"); QVERIFY(query.next()); QVERIFY(query.value(0).isNull());
        identity_->setAvatar({});
        QTRY_VERIFY_WITH_TIMEOUT(([&] { auto asset = waitTask(storage().readAsset(key)); return !asset && asset.error().code == dsfm::ErrorCode::MissingEntry; })(), 10000);
    }
    void generic_matroska_video_preview_is_cached_and_opt_in() {
        auto imported = waitTask(storage().importFile(QStringLiteral(DS_STORAGE_FIXTURES) + "/media-preview-v1/blue.mkv", "blue.mkv"));
        QVERIFY(imported); QVERIFY(media_preview::isVideoCandidate(imported.value().mime_type));
        QVERIFY(media_preview::isVideoCandidate(QStringLiteral("application/x-matroska")));
        auto data = std::make_unique<FileData>();
        data->state = File::FS_DONE; data->name = "blue.mkv"; data->mime_type = imported.value().mime_type;
        data->identity = identity_->getId(); data->contact = contact_->getId(); data->conversation = conversation_->getId();
        data->storage_namespace = imported.value().namespace_id; data->storage_id = imported.value().file.id;
        data->hash = imported.value().file.sha256; data->size = qlonglong(imported.value().file.size);
        auto file = engine_->getFileManager()->addFile(std::move(data));
        QVERIFY(!identity_->getPreviewImages()); QVERIFY(!identity_->getPreviewVideo());
        QVERIFY(file->previewSource().isEmpty());
        identity_->setPreviewVideo(true);
        auto reset = qScopeGuard([this] { identity_->setPreviewVideo(false); });
        file->previewSource();
        QTRY_VERIFY_WITH_TIMEOUT(!file->previewSource().isEmpty(), 10000);
        auto cache = waitTask(storage().readAsset(ManagedStorage::filePreview(file->getId(), file->getHash()))); QVERIFY(cache);
        const auto frame = media_preview::decode(cache.value()); QCOMPARE(frame.size(), QSize(320, 180));
        QVERIFY(frame.pixelColor(100, 100).blue() > 200);
        identity_->setPreviewVideo(false); QVERIFY(file->previewSource().isEmpty());
        file->deleteFromDb(); QVERIFY(waitTask(storage().collectGarbage()));
    }
    void previews_use_the_snapshot_cache_and_follow_identity_policy() {
        const auto path = directory_.filePath("preview-source.png");
        QImage image(960, 480, QImage::Format_RGB32); image.fill(Qt::green);
        image.setText("Private", "thumbnail metadata must not survive");
        QVERIFY(image.save(path, "PNG"));
        auto imported = waitTask(storage().importFile(path, "preview-source.png")); QVERIFY(imported);
        auto data = std::make_unique<FileData>();
        data->state = File::FS_WAITING;
        data->name = "preview-source.png";
        data->identity = identity_->getId(); data->contact = contact_->getId(); data->conversation = conversation_->getId();
        data->storage_namespace = imported.value().namespace_id; data->storage_id = imported.value().file.id;
        data->hash = imported.value().file.sha256; data->size = qlonglong(imported.value().file.size);
        data->mime_type = imported.value().mime_type;
        auto file = engine_->getFileManager()->addFile(std::move(data));
        const auto key = ManagedStorage::filePreview(file->getId(), file->getHash());
        QVERIFY(!identity_->getPreviewImages());
        QVERIFY(file->previewSource().isEmpty());
        auto absent = waitTask(storage().readAsset(key)); QVERIFY(!absent);
        identity_->setPreviewImages(true);
        // Changing the external source cannot change the imported snapshot.
        image.fill(Qt::red); QVERIFY(image.save(path, "PNG"));
        file->previewSource();
        QTRY_VERIFY_WITH_TIMEOUT(!file->previewSource().isEmpty(), 10000);
        const auto uri = file->previewSource();
        auto cached = waitTask(storage().readAsset(key)); QVERIFY(cached);
        const auto decoded = media_preview::decode(cached.value());
        QCOMPARE(decoded.size(), QSize(320, 160));
        QVERIFY(decoded.pixelColor(100, 100).green() > 200);
        QSqlQuery query;
        query.prepare("SELECT file_id,owner_identity,owner_contact FROM media_asset WHERE asset_key=:key");
        query.bindValue(":key", key); QVERIFY(query.exec()); QVERIFY(query.next());
        const auto cache_id = query.value(0).toUuid();
        QCOMPARE(query.value(1).toInt(), identity_->getId()); QCOMPARE(query.value(2).toInt(), contact_->getId()); query.finish();
        // A new File object reloads the same persistent JPEG, without writing a
        // duplicate asset or retaining an exported physical path.
        auto reloaded = File::load(*engine_->getFileManager(), file->getId());
        reloaded->previewSource();
        QTRY_COMPARE_WITH_TIMEOUT(reloaded->previewSource(), uri, 10000);
        query.prepare("SELECT file_id FROM media_asset WHERE asset_key=:key");
        query.bindValue(":key", key); QVERIFY(query.exec()); QVERIFY(query.next()); QCOMPARE(query.value(0).toUuid(), cache_id); query.finish();
        identity_->setPreviewImages(false);
        QVERIFY(file->previewSource().isEmpty()); QVERIFY(reloaded->previewSource().isEmpty());
        file->deleteFromDb();
        QVERIFY(waitTask(storage().collectGarbage()));
        absent = waitTask(storage().readAsset(key)); QVERIFY(!absent);
        QVERIFY(!waitTask(storage().manager().stat(storage().namespaceId(ManagedStorage::Private), cache_id)));
    }
    void unfinished_incoming_images_are_never_decoded() {
        QImage image(64, 64, QImage::Format_RGB32); image.fill(Qt::blue);
        QByteArray bytes; QBuffer buffer(&bytes); QVERIFY(buffer.open(QIODevice::WriteOnly)); QVERIFY(image.save(&buffer, "PNG"));
        auto file = incoming(bytes, "unaccepted.png");
        QSqlQuery query;
        query.prepare("UPDATE file SET mime_type='image/png' WHERE id=:id"); query.bindValue(":id", file->getId()); QVERIFY(query.exec());
        file = File::load(*engine_->getFileManager(), file->getId());
        identity_->setPreviewImages(true);
        auto restore = qScopeGuard([this] { identity_->setPreviewImages(false); });
        QVERIFY(file->previewSource().isEmpty());
        auto preview = waitTask(storage().imagePreview(file->getId())); QVERIFY(!preview);
        QVERIFY(waitTask(file->prepareIncoming()));
        QVERIFY(waitTask(file->writeIncoming(0, bytes)));
        QVERIFY(waitTask(file->suspendIncoming()));
        preview = waitTask(storage().imagePreview(file->getId())); QVERIFY(!preview);
        QVERIFY(file->previewSource().isEmpty());
        QVERIFY(waitTask(file->abortIncoming()));
    }
    void queued_preview_requests_drain_under_lifetime_lease_pressure() {
        QImage image(64, 32, QImage::Format_RGB32); image.fill(Qt::yellow);
        const auto path = directory_.filePath("concurrent-preview.png"); QVERIFY(image.save(path, "PNG"));
        auto imported = waitTask(storage().importFile(path, "concurrent-preview.png")); QVERIFY(imported);
        auto data = std::make_unique<FileData>();
        data->state = File::FS_DONE; data->name = "concurrent-preview.png"; data->mime_type = "image/png";
        data->identity = identity_->getId(); data->contact = contact_->getId(); data->conversation = conversation_->getId();
        data->storage_namespace = imported.value().namespace_id; data->storage_id = imported.value().file.id;
        data->hash = imported.value().file.sha256; data->size = qlonglong(imported.value().file.size);
        auto file = engine_->getFileManager()->addFile(std::move(data));
        struct Batch {
            std::vector<std::optional<dsfm::Result<QByteArray>>> replies{16};
            std::optional<dsfm::Result<QByteArray>> overload;
            int completed = 0;
        };
        auto batch = std::make_shared<Batch>();
        std::vector<ManagedStorage::Operation> leases;
        for (int i = 0; i < 16; ++i) leases.push_back(storage().holdOperation());
        for (int i = 0; i < 16; ++i) {
            storage().imagePreview(file->getId()).then([batch, i](auto reply) {
                batch->replies.at(i).emplace(std::move(reply)); ++batch->completed;
            });
        }
        storage().imagePreview(file->getId()).then([batch](auto reply) { batch->overload.emplace(std::move(reply)); });
        QVERIFY(batch->overload); QVERIFY(!*batch->overload);
        QCOMPARE(batch->overload->error().code, dsfm::ErrorCode::QueueFull);
        QTRY_COMPARE_WITH_TIMEOUT(batch->completed, 16, 10000);
        for (const auto& reply : batch->replies) {
            QVERIFY(reply && *reply); QVERIFY(!media_preview::decode(reply->value()).isNull());
        }
        leases.clear();
        file->deleteFromDb(); QVERIFY(waitTask(storage().collectGarbage()));
    }
    void received_thumbnail_is_visible_before_primary_acceptance_and_is_deleted_with_it() {
        QImage image(320, 160, QImage::Format_RGB32); image.fill(Qt::cyan);
        QByteArray source; QBuffer buffer(&source); QVERIFY(buffer.open(QIODevice::WriteOnly)); QVERIFY(image.save(&buffer, "PNG"));
        const auto jpeg = media_preview::generate(source); QVERIFY(!jpeg.isEmpty());
        auto primary_data = std::make_unique<FileData>();
        primary_data->state = File::FS_OFFERED; primary_data->direction = File::INCOMING;
        primary_data->identity = identity_->getId(); primary_data->contact = contact_->getId(); primary_data->conversation = conversation_->getId();
        primary_data->name = "large-photo.jpg"; primary_data->size = 512 * 1024 * 1024;
        primary_data->mime_type = "image/jpeg"; primary_data->hash = QByteArray(32, 'h');
        auto primary = engine_->getFileManager()->addFile(std::move(primary_data));
        auto thumbnail_data = std::make_unique<FileData>();
        thumbnail_data->state = File::FS_OFFERED; thumbnail_data->direction = File::INCOMING;
        thumbnail_data->identity = identity_->getId(); thumbnail_data->contact = contact_->getId(); thumbnail_data->conversation = conversation_->getId();
        thumbnail_data->name = "preview.jpg"; thumbnail_data->size = jpeg.size(); thumbnail_data->mime_type = "image/jpeg";
        thumbnail_data->hash = QCryptographicHash::hash(jpeg, QCryptographicHash::Sha256);
        thumbnail_data->thumbnail_for = primary->getFileId(); thumbnail_data->manifest_id = QByteArray(32, 'm');
        auto thumbnail = engine_->getFileManager()->addFile(std::move(thumbnail_data));
        QVERIFY(thumbnail->isThumbnail());
        QVERIFY(waitTask(thumbnail->prepareIncoming())); thumbnail->setState(File::FS_TRANSFERRING);
        auto approved = approveFixtureContact();
        identity_->setPreviewImages(true);
        auto restore_policy = qScopeGuard([this] { identity_->setPreviewImages(false); });
        auto peer = receiveOwned(thumbnail, jpeg);
        QTRY_COMPARE_WITH_TIMEOUT(thumbnail->getState(), File::FS_DONE, 10000);
        const auto thumbnail_ns = thumbnail->storageNamespace(), thumbnail_id = thumbnail->storageId();
        identity_->setPreviewImages(false);
        QVERIFY(primary->previewSource().isEmpty());
        identity_->setPreviewImages(true);
        auto restore = qScopeGuard([this] { identity_->setPreviewImages(false); });
        primary->previewSource();
        QTRY_VERIFY_WITH_TIMEOUT(!primary->previewSource().isEmpty(), 10000);
        QCOMPARE(primary->getState(), File::FS_OFFERED); QCOMPARE(primary->getBytesTransferred(), qlonglong(0));
        QVERIFY(primary->storageId().isNull());
        QVERIFY(thumbnail->previewSource().isEmpty()); // Never a separate media card.
        auto reloaded = File::load(*engine_->getFileManager(), thumbnail->getId());
        QVERIFY(reloaded->isThumbnail()); QCOMPARE(reloaded->dataThumbnailFor(), primary->getFileId());
        primary->deleteFromDb();
        QSqlQuery exists; exists.prepare("SELECT 1 FROM file WHERE id=?"); exists.addBindValue(thumbnail->getId());
        QVERIFY(exists.exec()); QVERIFY(!exists.next());
        QVERIFY(waitTask(storage().collectGarbage()));
        QVERIFY(!waitTask(storage().manager().stat(thumbnail_ns, thumbnail_id)));
        QVERIFY(!waitTask(storage().readAsset(ManagedStorage::filePreview(primary->getId(), primary->getHash()))));
    }
    void thumbnail_admission_rechecks_original_purpose_and_scope() {
        const auto state = contact_->getState();
        const auto images = identity_->getPreviewImages(), videos = identity_->getPreviewVideo();
        const auto allowed = identity_->getAllowFiles();
        const auto types = identity_->getAllowMimeTypes();
        auto restore = qScopeGuard([&] {
            identity_->setPreviewImages(images); identity_->setPreviewVideo(videos);
            identity_->setAllowFiles(allowed); identity_->setAllowMimeTypes(types);
            contact_->setState(state);
        });
        contact_->setState(Contact::ACCEPTED);
        identity_->setAllowFiles(true); identity_->setAllowMimeTypes({"*"});
        for (const auto &mime : {QStringLiteral("image/png"), QStringLiteral("video/mp4")}) {
            const auto primary = incoming("original", "original", mime);
            auto data = std::make_unique<FileData>();
            data->direction = File::INCOMING; data->state = File::FS_OFFERED;
            data->identity = identity_->getId(); data->contact = contact_->getId(); data->conversation = conversation_->getId();
            data->name = "preview.jpg"; data->mime_type = "image/jpeg"; data->size = 128; data->hash = QByteArray(32, 't');
            data->thumbnail_for = primary->getFileId(); data->manifest_id = ds::crypto::Crypto::getRandomBytes(32);
            const auto preview = engine_->getFileManager()->addFile(std::move(data));
            identity_->setPreviewImages(false); identity_->setPreviewVideo(false);
            QVERIFY(!contact_->canReceiveFile(*preview));
            QVERIFY(contact_->canReceiveFile(*primary)); // Original approval is independent.
            identity_->setPreviewImages(mime.startsWith("image/")); identity_->setPreviewVideo(mime.startsWith("video/"));
            QVERIFY(contact_->canReceiveFile(*preview));
            identity_->setAllowMimeTypes({mime});
            QVERIFY(!contact_->canReceiveFile(*preview)); // JPEG permission is still required.
            identity_->setAllowMimeTypes({"image/jpeg"});
            QVERIFY(contact_->canReceiveFile(*preview)); // Original MIME permission is not preview permission.
            identity_->setAllowFiles(false); QVERIFY(!contact_->canReceiveFile(*preview));
            identity_->setAllowFiles(true);
            primary->setState(File::FS_CANCELLED); QVERIFY(!contact_->canReceiveFile(*preview));
            primary->setState(File::FS_OFFERED);
            contact_->setState(Contact::REJECTED); QVERIFY(!contact_->canReceiveFile(*preview));
            contact_->setState(Contact::ACCEPTED); identity_->setAllowMimeTypes({"*"});
            primary->deleteFromDb();
        }
    }
    void incoming_original_preview_rejects_a_false_mime_claim() {
        QImage image(64, 32, QImage::Format_RGB32); image.fill(Qt::blue);
        QByteArray bytes; QBuffer buffer(&bytes); QVERIFY(buffer.open(QIODevice::WriteOnly)); QVERIFY(image.save(&buffer, "PNG"));
        auto file = incoming(bytes, "false-jpeg.jpg", "image/jpeg");
        QVERIFY(waitTask(file->prepareIncoming())); file->setState(File::FS_TRANSFERRING);
        auto approved = approveFixtureContact();
        auto peer = receiveOwned(file, bytes);
        QTRY_COMPARE_WITH_TIMEOUT(file->getState(), File::FS_DONE, 10000);
        auto preview = waitTask(storage().imagePreview(file->getId()));
        QVERIFY(!preview); QCOMPARE(preview.error().code, dsfm::ErrorCode::InvalidArgument);
        file->deleteFromDb(); QVERIFY(waitTask(storage().collectGarbage()));
    }
    void outgoing_manifest_requires_scoped_transfer() {
        const auto inbound = incoming("incoming", "attachment.bin", "application/octet-stream");
        ConnectData connection;
        connection.service = identity_->getUuid();
        connection.identitysCert = identity_->getCert();
        connection.contactsCert = contact_->getCert();
        auto socket = ds::prot::ConnectionSocket::create();
        auto peer = std::make_shared<CheckpointWireClient>(socket, connection);
        for (const auto &file_id : {ds::crypto::Crypto::getRandomBytes(16), inbound->getFileId()}) {
            MessageData data;
            data.conversation = conversation_->getHash();
            data.messageId = ds::crypto::Crypto::getRandomBytes(32);
            data.composedTime = DsEngine::getSafeNow();
            data.attachments = {{file_id, inbound->getName(), quint64(inbound->getSize()),
                                 inbound->getHash(), "application/octet-stream", {}}};
            Message message(*engine_, data, Message::OUTGOING, conversation_->getId());
            message.sign(*identity_->getCert());
            QVERIFY(message.validate(*identity_->getCert()));
            try {
                peer->sendMessage(message);
                QFAIL("An unresolved attachment was sent");
            } catch (const ds::core::Error &error) {
                QCOMPARE(QString::fromUtf8(error.what()),
                         QStringLiteral("Outgoing attachment has no unambiguous scoped transfer"));
            }
        }
        QCOMPARE(inbound->getState(), File::FS_OFFERED);
        QVERIFY(!inbound->checkpointAttempt());
    }
    void signed_manifest_persistence_is_atomic_and_round_trips() {
        MessageData data;
        data.conversation = conversation_->getHash(); data.messageId = ds::crypto::Crypto::getRandomBytes(32);
        data.composedTime = DsEngine::getSafeNow();
        data.attachments = {{QByteArray(16, 'a'), "photo.png", 10000, QByteArray(32, 'h'), "image/png", {}},
                            {QByteArray(16, 'b'), "preview.jpg", 1000, QByteArray(32, 't'), "image/jpeg", quint32{0}}};
        Message message(*engine_, data, Message::OUTGOING, conversation_->getId());
        message.sign(*identity_->getCert()); message.addToDb();
        auto restored = Message::load(*engine_, message.getId());
        QVERIFY(restored->validate(*identity_->getCert()));
        QCOMPARE(restored->getData().attachments.size(), size_t(2));
        QVERIFY(restored->getData().attachments == message.getData().attachments);
        auto relayed = message.getData(); relayed.attachments[0].file_id = QByteArray(16, 'r');
        QVERIFY(!engine_->getMessageManager()->hasConflictingMessage(relayed));
        relayed.attachments[0].name = "tampered.png";
        QVERIFY(engine_->getMessageManager()->hasConflictingMessage(relayed));
        QSqlQuery fault;
        QVERIFY(fault.exec("CREATE TEMP TRIGGER fail_second_attachment BEFORE INSERT ON message_attachment_v1 WHEN NEW.position=1 BEGIN SELECT RAISE(ABORT,'fixture'); END"));
        auto drop = qScopeGuard([] { QSqlQuery cleanup; cleanup.exec("DROP TRIGGER fail_second_attachment"); });
        data.messageId = ds::crypto::Crypto::getRandomBytes(32);
        Message failing(*engine_, data, Message::OUTGOING, conversation_->getId());
        failing.sign(*identity_->getCert());
        QVERIFY_EXCEPTION_THROWN(failing.addToDb(), ds::core::Error);
        fault.prepare("SELECT 1 FROM message WHERE message_id=?"); fault.addBindValue(data.messageId);
        QVERIFY(fault.exec()); QVERIFY(!fault.next());
    }
    void failed_thumbnail_relationship_does_not_leave_a_file_row() {
        auto primary = incoming("primary", "primary.bin");
        QSqlQuery fault;
        QVERIFY(fault.exec("CREATE TEMP TRIGGER fail_thumbnail_link BEFORE INSERT ON file_thumbnail_v1 BEGIN SELECT RAISE(ABORT,'fixture'); END"));
        auto drop = qScopeGuard([] { QSqlQuery cleanup; cleanup.exec("DROP TRIGGER fail_thumbnail_link"); });
        auto data = std::make_unique<FileData>();
        data->direction = File::INCOMING; data->state = File::FS_OFFERED;
        data->identity = identity_->getId(); data->contact = contact_->getId(); data->conversation = conversation_->getId();
        data->name = "preview.jpg"; data->mime_type = "image/jpeg";
        data->fileId = QByteArray(16, 't'); data->hash = QByteArray(32, 'h'); data->size = 1000;
        data->thumbnail_for = primary->getFileId(); data->manifest_id = QByteArray(32, 'm');
        QVERIFY_EXCEPTION_THROWN(engine_->getFileManager()->addFile(std::move(data)), ds::core::Error);
        fault.prepare("SELECT 1 FROM file WHERE file_id=?"); fault.addBindValue(QByteArray(16, 't'));
        QVERIFY(fault.exec()); QVERIFY(!fault.next());
        primary->deleteFromDb();
    }
    void clearing_avatar_during_restore_does_not_restore_old_content() {
        QImage image(16, 16, QImage::Format_RGB32); image.fill(Qt::blue);
        QByteArray encoded; QBuffer buffer(&encoded); QVERIFY(buffer.open(QIODevice::WriteOnly)); QVERIFY(image.save(&buffer, "PNG"));
        const auto key = ManagedStorage::contactAvatar(contact_->getUuid());
        QVERIFY(waitTask(storage().writeAsset(key, encoded)));
        auto loaded = Contact::load(contact_->getUuid());
        loaded->setAvatar({});
        QTRY_VERIFY_WITH_TIMEOUT(([&] { auto bytes = waitTask(storage().readAsset(key)); return !bytes && bytes.error().code == dsfm::ErrorCode::MissingEntry; })(), 10000);
        QCoreApplication::processEvents(); QVERIFY(loaded->getAvatar().isNull());
    }
    void contact_deletion_tombstones_managed_avatar() {
        auto data = std::make_unique<ContactData>(); data->identity = identity_->getId(); data->name = "Deleted fixture peer";
        const auto cert = ds::crypto::DsCert::create(); data->cert = ds::crypto::DsCert::createFromPubkey(cert->getSigningPubKey().toByteArray());
        data->address = "not-networked"; data->autoConnect = false;
        auto* contact = engine_->getContactManager()->addContact(std::move(data));
        const auto uuid = contact->getUuid(); const auto key = ManagedStorage::contactAvatar(uuid);
        QImage image(16, 16, QImage::Format_RGB32); image.fill(Qt::green); contact->setAvatar(image);
        QTRY_VERIFY_WITH_TIMEOUT(([&] { return bool(waitTask(storage().readAsset(key))); })(), 10000);
        QSqlQuery query; query.prepare("SELECT file_id FROM media_asset WHERE asset_key=:key"); query.bindValue(":key", key); QVERIFY(query.exec()); QVERIFY(query.next());
        const auto id = query.value(0).toUuid(); query.finish();
        engine_->getContactManager()->deleteContact(uuid);
        QVERIFY(waitTask(storage().collectGarbage()));
        auto missing = waitTask(storage().manager().stat(storage().namespaceId(ManagedStorage::Private), id)); QVERIFY(!missing);
    }
    void deletion_retains_durable_cleanup_intent() {
        auto file = incoming("cleanup", "cleanup.bin");
        QVERIFY(waitTask(file->prepareIncoming()));
        const auto id = file->storageId(), ns = file->storageNamespace(), pending = file->pendingId();
        QVERIFY(waitTask(file->suspendIncoming()));
        file->deleteFromDb();
        QSqlQuery query("SELECT COUNT(*) FROM storage_gc"); QVERIFY(query.next()); QVERIFY(query.value(0).toInt() > 0); query.finish();
        QVERIFY(waitTask(storage().collectGarbage()));
        auto reopened = waitTask(storage().manager().openPending(ns, pending)); QVERIFY(!reopened);
        auto missing = waitTask(storage().manager().stat(ns, id)); QVERIFY(!missing);
    }
    void cancellation_does_not_publish_an_import() {
        dsfm::CancellationToken cancel; cancel.cancel();
        const auto source = directory_.filePath("cancel.txt"); QFile file(source); QVERIFY(file.open(QIODevice::WriteOnly)); file.write("cancelled"); file.close();
        auto result = waitTask(storage().importFile(source, "cancel.txt", cancel)); QVERIFY(!result); QCOMPARE(result.error().code, dsfm::ErrorCode::Cancelled);
    }
    void abandoned_unreferenced_pending_is_reclaimed() {
        dsfm::CreateOptions options{storage().root(ManagedStorage::Private), "abandoned.bin"};
        options.content.expected_size = 0;
        auto created = waitTask(storage().manager().createFile(storage().namespaceId(ManagedStorage::Private), options)); QVERIFY(created);
        const auto id = created.value().info.id;
        QVERIFY(waitTask(created.value().writer->suspend()));
        QVERIFY(waitTask(storage().collectGarbage()));
        auto missing = waitTask(storage().manager().openPending(storage().namespaceId(ManagedStorage::Private), id)); QVERIFY(!missing);
    }
    void peer_cancellation_discards_the_incoming_partial() {
        auto peer_data = std::make_unique<ContactData>();
        peer_data->identity = identity_->getId(); peer_data->name = "Remote cancellation peer";
        const auto cert = ds::crypto::DsCert::create();
        peer_data->cert = ds::crypto::DsCert::createFromPubkey(cert->getSigningPubKey().toByteArray());
        peer_data->address = "fixture-cancel-not-networked"; peer_data->autoConnect = false;
        peer_data->state = Contact::ACCEPTED;
        auto* created = engine_->getContactManager()->addContact(std::move(peer_data));
        auto contact = engine_->getContactManager()->getContact(created->getUuid());
        auto conversation = engine_->getConversationManager()->getConversation(contact.get());
        const QByteArray bytes(16384, 'c');
        auto data = std::make_unique<FileData>();
        data->direction = File::INCOMING; data->state = File::FS_OFFERED;
        data->identity = identity_->getId(); data->contact = contact->getId();
        data->conversation = conversation->getId(); data->name = "remote-cancel.bin";
        data->size = bytes.size(); data->hash = QCryptographicHash::hash(bytes, QCryptographicHash::Sha256);
        auto file = engine_->getFileManager()->addFile(std::move(data));
        const auto cleanup = qScopeGuard([&] {
            waitTask(file->abortIncoming());
            contact->disconnectFromContact(true);
            engine_->getContactManager()->deleteContact(contact->getUuid());
        });
        QVERIFY(waitTask(file->prepareIncoming()));
        QVERIFY(waitTask(file->writeIncoming(0, bytes.left(8192))));
        QVERIFY(waitTask(file->checkpointIncoming())); file->setBytesTransferred(8192);
        file->setState(File::FS_TRANSFERRING);
        auto peer = std::make_shared<ResumePeer>();
        contact->onConnectedToPeer(peer);
        auto stale = std::make_shared<ResumePeer>();
        emit peer->receivedFileCancel(PeerFileCancel(stale, stale->getConnectionId(), 0,
            file->getFileId(), FileCancelReason::UserAction));
        QCOMPARE(file->getState(), File::FS_TRANSFERRING);
        emit peer->receivedFileCancel(PeerFileCancel(peer, peer->getConnectionId(), 0,
            file->getFileId(), FileCancelReason::UserAction));
        QTRY_COMPARE_WITH_TIMEOUT(file->getState(), File::FS_CANCELLED, 5000);
        QTRY_VERIFY_WITH_TIMEOUT(([&] {
            auto pending = waitTask(storage().manager().listPending(file->storageNamespace()));
            return pending && std::none_of(pending.value().entries.begin(), pending.value().entries.end(),
                [&](const auto& entry) { return entry.file_id == file->storageId(); });
        })(), 5000);
        QVERIFY(!waitTask(storage().manager().openRead(file->storageNamespace(), file->storageId())));
        // A delayed cancellation cannot revoke a completed received file.
        auto finished_data = std::make_unique<FileData>();
        finished_data->direction = File::INCOMING; finished_data->state = File::FS_OFFERED;
        finished_data->identity = identity_->getId(); finished_data->contact = contact->getId();
        finished_data->conversation = conversation->getId(); finished_data->name = "finished-before-cancel.bin";
        finished_data->size = bytes.size();
        finished_data->hash = QCryptographicHash::hash(bytes, QCryptographicHash::Sha256);
        auto finished = engine_->getFileManager()->addFile(std::move(finished_data));
        QVERIFY(waitTask(finished->prepareIncoming()));
        QVERIFY(waitTask(finished->writeIncoming(0, bytes)));
        auto completed_peer = receiveOwned(finished, bytes);
        QTRY_COMPARE_WITH_TIMEOUT(finished->getState(), File::FS_DONE, 5000);
        emit peer->receivedFileCancel(PeerFileCancel(peer, peer->getConnectionId(), 0,
            finished->getFileId(), FileCancelReason::UserAction));
        QCOMPARE(finished->getState(), File::FS_DONE);
        auto readable = waitTask(storage().manager().openRead(finished->storageNamespace(), finished->storageId()));
        QVERIFY(readable);
        auto content = waitTask(readable.value()->readAt(0, bytes.size()));
        QVERIFY(content); QCOMPARE(content.value(), bytes);
        QVERIFY(waitTask(readable.value()->close()));
    }
    void queued_sends_reconnect_without_connection_churn() {
        auto protocol = std::make_shared<QueueProtocol>();
        ProtocolManager::addFactory(ProtocolManager::Transport::TOR, [protocol](QSettings&) { return protocol; });
        engine_->start();
        contact_->setState(Contact::ACCEPTED);
        contact_->setAutoConnect(false);
        identity_->setOnline(false);
        contact_->disconnectFromContact(true);
        // Enqueueing preserves offline identity state, then connects as it comes online.
        QVERIFY(conversation_->sendMessage("queued while identity offline"));
        QCoreApplication::processEvents();
        QCOMPARE(protocol->attempts, 0); QVERIFY(!identity_->isOnline());
        identity_->setOnline(true);
        QTRY_COMPARE_WITH_TIMEOUT(protocol->attempts, 1, 2000);
        QCOMPARE(contact_->getOnlineStatus(), Contact::CONNECTING);
        QVERIFY(!contact_->wasManuallyDisconnected());
        QVERIFY(conversation_->sendMessage("another message during connection"));
        QCoreApplication::processEvents(); QCOMPARE(protocol->attempts, 1);
        // An explicit disconnect after enqueueing cancels the pending attempt.
        contact_->disconnectFromContact(true);
        QVERIFY(conversation_->sendMessage("cancel pending connection"));
        contact_->disconnectFromContact(true);
        QCoreApplication::processEvents(); QCOMPARE(protocol->attempts, 1);
        // Locally imported files also initiate a connection, without offering
        // metadata before fresh peer preferences arrive.
        QVERIFY(contact_->canSendFiles());
        File::ptr_t queued;
        const auto added = connect(engine_->getFileManager(), &FileManager::fileAdded, this,
            [&queued](const File::ptr_t& file) { queued = file; });
        conversation_->sendFile({{"path", QStringLiteral(DS_STORAGE_FIXTURES) + "/../assets/mcp/earth.jpg"}});
        disconnect(added);
        QVERIFY(queued);
        QTRY_COMPARE_WITH_TIMEOUT(queued->getState(), File::FS_WAITING, 5000);
        QTRY_COMPARE_WITH_TIMEOUT(protocol->attempts, 2, 2000);
        QVERIFY(!queued->getHash().isEmpty());
        QCoreApplication::processEvents(); QCOMPARE(protocol->attempts, 2);
        // Blocked contacts and inbound work never trigger an outgoing dial.
        contact_->disconnectFromContact(true);
        contact_->setBlocked(true);
        QVERIFY(!conversation_->sendMessage("blocked message"));
        QVERIFY(!contact_->canSendFiles());
        QCoreApplication::processEvents(); QCOMPARE(protocol->attempts, 2);
        contact_->setBlocked(false);
        const auto inbound = incoming("inbound", "inbound.bin");
        inbound->setState(File::FS_QUEUED);
        contact_->queueFile(inbound);
        QCoreApplication::processEvents(); QCOMPARE(protocol->attempts, 2);
        contact_->setState(Contact::REJECTED);
        QVERIFY(conversation_->sendMessage("unaccepted contact stays disconnected"));
        QCoreApplication::processEvents(); QCOMPARE(protocol->attempts, 2);
        identity_->setOnline(false);
        contact_->setState(Contact::PENDING);
    }
    void restart_and_download_location_change_preserve_managed_ids() {
        const QByteArray bytes(8192, 'r');
        auto file = incoming(bytes, "restart.bin");
        QVERIFY(waitTask(file->prepareIncoming()));
        QVERIFY(waitTask(file->writeIncoming(0, bytes)));
        QVERIFY(waitTask(file->suspendIncoming()));
        const auto ns = file->storageNamespace(), pending_id = file->pendingId(), id = file->storageId();
        QVERIFY(waitTask(storage().shutdown()));
        const auto next_downloads = directory_.filePath("next-downloads"); QVERIFY(QDir().mkdir(next_downloads));
        engine_->settings().setValue("downloadLocation", next_downloads);
        engine_->settings().setValue("storage/originalFilenames", false);
        engine_->settings().setValue("storage/repairPermissions", false);
        ManagedStorage reopened(engine_->settings());
        QVERIFY(waitTask(reopened.ready()));
        QVERIFY(reopened.isDownloadNamespace(ns)); QVERIFY(reopened.namespaceId(ManagedStorage::Downloads) != ns);
        auto pending = waitTask(reopened.manager().openPending(ns, pending_id)); QVERIFY(pending);
        QCOMPARE(pending.value().info.file_id, id);
        QVERIFY(waitTask(pending.value().writer->commit()));
        auto reader = waitTask(reopened.manager().openRead(ns, id)); QVERIFY(reader);
        auto data = waitTask(reader.value()->readAt(0, 8192)); QVERIFY(data); QCOMPARE(data.value(), bytes);
        QVERIFY(waitTask(reader.value()->close()));
        auto info = waitTask(reopened.manager().namespaceInfo(reopened.namespaceId(ManagedStorage::Downloads))); QVERIFY(info);
        QVERIFY(!info.value().config.repair_permissions);
        QCOMPARE(info.value().config.naming, dsfm::PhysicalNaming::FileId);
        std::optional<ManagedStorage::Operation> operation(reopened.holdOperation());
        std::optional<dsfm::Result<void>> stopped;
        reopened.shutdown().then([&](dsfm::Result<void> result) { stopped.emplace(std::move(result)); });
        QCoreApplication::processEvents(); QVERIFY(!stopped);
        operation.reset();
        QTRY_VERIFY_WITH_TIMEOUT(stopped.has_value(), 10000); QVERIFY(*stopped);
    }
    void cleanupTestCase() {
        auto joined = engine_->transfers().shutdown();
        QTRY_VERIFY_WITH_TIMEOUT(joined.isReady(), 10000);
        QVERIFY(waitTask(storage().shutdown()));
        conversation_.reset();
        engine_.reset();
    }
private:
    ManagedStorage& storage() { return engine_->getFileManager()->storage(); }
    std::shared_ptr<OwnedReceivingPeer> receiveOwned(const File::ptr_t &file, QByteArray bytes) {
        if (!waitTask(file->suspendIncoming())) throw std::runtime_error("Test partial could not suspend");
        if (!waitTask(file->prepareCheckpointProgress(dsfm::minimum_checkpoint_interval))) throw std::runtime_error("Test journal could not prepare");
        auto peer = std::make_shared<OwnedReceivingPeer>(); peer->file = file; peer->payload = std::move(bytes);
        auto session = std::make_shared<IncomingTransfer>(file, peer, IncomingTransferStart::Resume);
        if (!engine_->transfers().submit(session)) throw std::runtime_error("Test receiver admission failed");
        return peer;
    }
    File::ptr_t incoming(QByteArray bytes, QString name, QString mime_type = {}, QUuid connection = {}) {
        auto data = std::make_unique<FileData>();
        data->direction = File::INCOMING; data->state = File::FS_OFFERED;
        data->identity = identity_->getId(); data->contact = contact_->getId(); data->conversation = conversation_->getId();
        data->size = bytes.size(); data->name = std::move(name); data->hash = QCryptographicHash::hash(bytes, QCryptographicHash::Sha256);
        data->mime_type = std::move(mime_type);
        data->offered_connection = connection;
        return engine_->getFileManager()->addFile(std::move(data));
    }
    QTemporaryDir directory_;
    std::unique_ptr<ds::crypto::Crypto> crypto_;
    std::unique_ptr<DsEngine> engine_;
    Identity* identity_ = nullptr;
    Contact* contact_ = nullptr;
    Conversation::ptr_t conversation_;
};
QTEST_GUILESS_MAIN(ManagedStorageTest)
#include "managed_storage_test.moc"
