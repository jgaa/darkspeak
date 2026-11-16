#include <QtTest>
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
    ReceivingPeer() : Peer(std::make_shared<ds::prot::ConnectionSocket>(), {}) {}
    Direction getDirection() const noexcept override { return OUTGOING; }
    bool isConnected() const noexcept override { return true; }
    quint32 attach(const File::ptr_t &file) {
        const auto channel = createChannel(*file);
        file->setChannel(channel);
        return channel;
    }
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
class CheckpointReceivingPeer final : public ReceivingPeer {
  public:
    void begin(const File::ptr_t &file) {
        file_ = file;
        file->setState(File::FS_TRANSFERRING);
        channel_ = attach(file);
    }
    uint64_t sendCheckpointAck(const QByteArray &, QUuid, quint64 next_sequence, quint64 offset, bool final) override {
        acknowledged = offset;
        completed = final;
        if (final) {
            ++final_acks;
            auto key = file_->checkpointKey();
            final_journal_complete =
                waitTask(DsEngine::instance().getFileManager()->storage().transferProgress().load(key.value()))
                    .value()
                    .complete;
        }
        if (on_ack)
            on_ack(next_sequence, offset, final);
        return 0;
    }
    uint64_t sendFileCheckpoint(const QByteArray &, const QByteArray &, const dsfm::TransferAttempt &,
                                dsfm::DurableCheckpoint checkpoint, dsfm::CheckpointPhase phase) override {
        if (phase == dsfm::CheckpointPhase::Proposed) {
            proposal = checkpoint;
            ++proposals;
            if (on_proposal)
                on_proposal(checkpoint);
        } else if (phase == dsfm::CheckpointPhase::MutuallyConfirmed) {
            ++receipts;
            if (on_receipt)
                on_receipt(checkpoint);
        } else
            throw std::runtime_error("wrong checkpoint direction");
        return 0;
    }
    uint64_t sendFilePause(const QByteArray &, QUuid, dsfm::DurableCheckpoint, dsfm::TransferPauseReason) override {
        return 0;
    }
    void confirm(dsfm::DurableCheckpoint checkpoint) {
        ds::protocol::FileCheckpoint message;
        message.setFileId(file_->getFileId());
        message.setExpectedSha256(file_->getHash());
        message.setAttemptId(file_->checkpointAttempt()->id.toRfc4122());
        message.setCheckpoint(ds::prot::codec::checkpointReference(checkpoint));
        message.setPhase(ds::protocol::FileCheckpoint::Phase::CHECKPOINT_SENDER_CONFIRMED);
        inChannels_.at(channel_)->onCheckpoint(*this, message);
    }
    void receive(quint64 sequence, quint64 offset, const QByteArray &bytes, bool final) {
        ReceivingPeer::receive(channel_, sequence, offset, bytes, final);
    }
    size_t retainedCompletedCount() const { return completed_incoming_.size(); }
    size_t incomingChannelCount() const { return inChannels_.size(); }
    dsfm::DurableCheckpoint proposal;
    int proposals = 0, receipts = 0;
    bool final_journal_complete = false;
    int final_acks = 0;
    std::function<void(quint64, quint64, bool)> on_ack;
    std::function<void(dsfm::DurableCheckpoint)> on_proposal, on_receipt;

  private:
    File::ptr_t file_;
    quint32 channel_ = 0;
};
class SendingPeer final : public ds::prot::Peer {
  public:
    SendingPeer() : Peer(std::make_shared<ds::prot::ConnectionSocket>(), {}) {}
    Direction getDirection() const noexcept override { return OUTGOING; }
    bool isConnected() const noexcept override { return true; }
    void begin(const File::ptr_t &file) {
        channel_ = createChannel(*file);
        file->setChannel(channel_);
        file->setState(File::FS_TRANSFERRING);
        outChannels_.at(channel_)->onOutgoing(*this);
    }
    uint64_t sendFileChunk(const QByteArray &, quint64 sequence, quint64 offset, const QByteArray &bytes,
                           bool final) override {
        sent.append(bytes);
        if (!final && mutate) {
            auto action = std::exchange(mutate, {});
            action();
        }
        if (final) {
            final_sent = true;
            outChannels_.at(channel_)->onAck(*this, sequence, qlonglong(offset + quint64(bytes.size())), true);
        }
        return 0;
    }
    uint64_t sendFileCancel(const QByteArray &, FileCancelReason reason) override {
        cancelled = reason == FileCancelReason::SourceChanged;
        return 0;
    }
    QByteArray sent;
    std::function<void()> mutate;
    bool final_sent = false, cancelled = false;

  private:
    quint32 channel_ = 0;
};
class CheckpointSendingPeer final : public ds::prot::Peer {
  public:
    CheckpointSendingPeer() : Peer(std::make_shared<ds::prot::ConnectionSocket>(), {}) {}
    Direction getDirection() const noexcept override { return OUTGOING; }
    bool isConnected() const noexcept override { return true; }
    void begin(const File::ptr_t &file) {
        file_ = file;
        channel_ = createChannel(*file);
        file->setChannel(channel_);
        file->setState(File::FS_TRANSFERRING);
        outChannels_.at(channel_)->onOutgoing(*this);
    }
    uint64_t sendCheckpointChunk(const QByteArray &, QUuid, quint64 sequence, quint64 offset, const QByteArray &bytes,
                                 bool final) override {
        sent.append(bytes);
        if (on_chunk)
            on_chunk(sequence, offset, bytes, final);
        else if (!final)
            outChannels_.at(channel_)->onAck(*this, sequence, qlonglong(offset + quint64(bytes.size())), false);
        return 0;
    }
    uint64_t sendFileCheckpoint(const QByteArray &, const QByteArray &, const dsfm::TransferAttempt &,
                                dsfm::DurableCheckpoint checkpoint, dsfm::CheckpointPhase phase) override {
        if (phase != dsfm::CheckpointPhase::SenderConfirmed)
            throw std::runtime_error("wrong checkpoint direction");
        confirmation = checkpoint;
        ++confirmations;
        if (on_confirmation)
            on_confirmation(checkpoint);
        return 0;
    }
    uint64_t sendFilePause(const QByteArray &, QUuid, dsfm::DurableCheckpoint, dsfm::TransferPauseReason) override {
        return 0;
    }
    void checkpoint(dsfm::DurableCheckpoint checkpoint, ds::protocol::FileCheckpoint::Phase phase) {
        ds::protocol::FileCheckpoint message;
        message.setFileId(file_->getFileId());
        message.setAttemptId(file_->checkpointAttempt()->id.toRfc4122());
        message.setExpectedSha256(file_->getHash());
        message.setCheckpoint(ds::prot::codec::checkpointReference(checkpoint));
        message.setPhase(phase);
        outChannels_.at(channel_)->onCheckpoint(*this, message);
    }
    void ack(quint64 next_sequence, quint64 offset, bool final) {
        outChannels_.at(channel_)->onAck(*this, next_sequence - 1, qlonglong(offset), final);
    }
    std::function<void(quint64, quint64, const QByteArray &, bool)> on_chunk;
    std::function<void(dsfm::DurableCheckpoint)> on_confirmation;
    QByteArray sent;
    dsfm::DurableCheckpoint confirmation;
    int confirmations = 0;

  private:
    File::ptr_t file_;
    quint32 channel_ = 0;
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
class ResumePeer final : public ds::prot::Peer {
  public:
    ResumePeer() : Peer(std::make_shared<ds::prot::ConnectionSocket>(), {}) {}
    Direction getDirection() const noexcept override { return OUTGOING; }
    bool isConnected() const noexcept override { return true; }
    uint64_t sendUserInfo(const UserInfo &) override { return 0; }
    uint64_t sendFileResumeResult(quint64, const QByteArray &, FileResumeStatus status, quint64 offset,
                                  FileCancelReason) override {
        accepted = status == FileResumeStatus::Accepted;
        negotiated_offset = offset;
        ++responses;
        return 0;
    }
    uint64_t startTransfer(File &file) override {
        ++starts;
        started_offset = file.getBytesTransferred();
        file.setState(File::FS_TRANSFERRING);
        return 0;
    }
    int responses = 0, starts = 0;
    bool accepted = false;
    quint64 negotiated_offset = 0;
    qlonglong started_offset = -1;
};
class LoopbackListener final : public QTcpServer {
  public:
    std::function<void(ds::prot::ConnectionSocket::ptr_t)> accepted;

  protected:
    void incomingConnection(qintptr descriptor) override {
        auto socket = std::make_shared<ds::prot::ConnectionSocket>();
        if (socket->setSocketDescriptor(descriptor))
            accepted(std::move(socket));
    }
};
class CheckpointWireClient final : public ds::prot::DsClient {
  public:
    using DsClient::DsClient;
    quint32 preference = dsfm::default_checkpoint_interval;
    quint64 admitted_end = 0;
    uint64_t sendCheckpointChunk(const QByteArray &file_id, QUuid attempt, quint64 sequence,
                                quint64 offset, const QByteArray &bytes, bool final) override {
        admitted_end = qMax(admitted_end, offset + quint64(bytes.size()));
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
    std::function<void()> withheld;
    uint64_t sendFileCheckpoint(const QByteArray &file_id, const QByteArray &sha256,
                                const dsfm::TransferAttempt &attempt, dsfm::DurableCheckpoint checkpoint,
                                dsfm::CheckpointPhase phase) override {
        if ((withhold_proposals && phase == dsfm::CheckpointPhase::Proposed) ||
            (withhold_receipts && phase == dsfm::CheckpointPhase::MutuallyConfirmed)) {
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
    void ordinary_acks_do_not_extend_checkpoint_window() {
        const QByteArray payload(3 * dsfm::minimum_checkpoint_interval + 7, 'q');
        const auto path = directory_.filePath("checkpoint-source");
        QFile source(path);
        QVERIFY(source.open(QIODevice::WriteOnly));
        QCOMPARE(source.write(payload), payload.size());
        source.close();
        auto imported = waitTask(storage().importFile(path, "checkpoint-source"));
        QVERIFY(imported);
        auto data = std::make_unique<FileData>();
        data->state = File::FS_WAITING;
        data->identity = identity_->getId();
        data->contact = contact_->getId();
        data->conversation = conversation_->getId();
        data->storage_namespace = imported.value().namespace_id;
        data->storage_id = imported.value().file.id;
        data->name = "checkpoint-source";
        data->size = payload.size();
        data->hash = imported.value().file.sha256;
        auto file = engine_->getFileManager()->addFile(std::move(data));
        auto progress = waitTask(file->prepareCheckpointProgress(dsfm::minimum_checkpoint_interval));
        QVERIFY(progress);
        dsfm::TransferAttempt attempt{QUuid::createUuid(), dsfm::minimum_checkpoint_interval, 1,
                                      progress.value().checkpoints.first()};
        QVERIFY(file->setCheckpointAttempt(attempt));
        auto key = file->checkpointKey();
        QVERIFY(key);
        QVERIFY(waitTask(storage().transferProgress().beginAttempt(key.value(), attempt.id, attempt.selected)));
        auto peer = std::make_shared<CheckpointSendingPeer>();
        peer->begin(file);
        QTRY_COMPARE_WITH_TIMEOUT(peer->sent.size(), qsizetype(dsfm::minimum_checkpoint_interval), 5000);
        QTest::qWait(50);
        QCOMPARE(peer->sent.size(), qsizetype(dsfm::minimum_checkpoint_interval));
        auto unchanged = waitTask(storage().transferProgress().load(key.value()));
        QVERIFY(unchanged);
        QCOMPARE(unchanged.value().confirmed_offset, quint64(0));
        QSqlQuery mirror;
        mirror.prepare("SELECT bytes_transferred FROM file WHERE id=?");
        mirror.addBindValue(file->getId());
        QVERIFY(mirror.exec());
        QVERIFY(mirror.next());
        QCOMPARE(mirror.value(0).toLongLong(), qlonglong(0));
        mirror.finish();
        dsfm::DurableCheckpoint checkpoint{
            1, dsfm::minimum_checkpoint_interval,
            QCryptographicHash::hash(payload.first(dsfm::minimum_checkpoint_interval), QCryptographicHash::Sha256),
            false};
        peer->checkpoint(checkpoint, ds::protocol::FileCheckpoint::Phase::CHECKPOINT_PROPOSED);
        QTRY_COMPARE_WITH_TIMEOUT(peer->confirmations, 1, 5000);
        QTest::qWait(50);
        QCOMPARE(peer->sent.size(), qsizetype(dsfm::minimum_checkpoint_interval));
        peer->checkpoint(checkpoint, ds::protocol::FileCheckpoint::Phase::CHECKPOINT_RECEIVER_CONFIRMED);
        QTRY_COMPARE_WITH_TIMEOUT(peer->sent.size(), qsizetype(2 * dsfm::minimum_checkpoint_interval), 5000);
        QTest::qWait(50);
        QCOMPARE(peer->sent.size(), qsizetype(2 * dsfm::minimum_checkpoint_interval));
        auto durable = waitTask(storage().transferProgress().load(key.value()));
        QVERIFY(durable);
        QCOMPARE(durable.value().confirmed_offset, quint64(dsfm::minimum_checkpoint_interval));
        QVERIFY(mirror.exec());
        QVERIFY(mirror.next());
        QCOMPARE(mirror.value(0).toLongLong(), qlonglong(dsfm::minimum_checkpoint_interval));
        mirror.finish();
        QCOMPARE(file->getBytesTransferred(), qlonglong(2 * dsfm::minimum_checkpoint_interval));
        QSignalSpy source_integrity(file.get(), &File::sourceIntegrityFailed);
        auto wrong = checkpoint;
        wrong.sequence = 2;
        wrong.offset = 2 * dsfm::minimum_checkpoint_interval;
        wrong.sha256 = QByteArray(32, 'x');
        peer->checkpoint(wrong, ds::protocol::FileCheckpoint::Phase::CHECKPOINT_PROPOSED);
        QTRY_COMPARE_WITH_TIMEOUT(file->getState(), File::FS_FAILED, 5000);
        QCOMPARE(source_integrity.count(), 0);
        auto rejected = waitTask(storage().transferProgress().load(key.value()));
        QVERIFY(rejected);
        QCOMPARE(rejected.value().failure, dsfm::TransferFailureReason::ReceiverCheckpointMismatch);
        QCOMPARE(rejected.value().confirmed_offset, quint64(dsfm::minimum_checkpoint_interval));
        QTest::qWait(10);
        file->deleteFromDb();
    }
    void file_work_budget_survives_connection_teardown() {
        auto first = incoming("a", "work-budget-first");
        auto second = incoming("b", "work-budget-second");
        auto third = incoming("c", "work-budget-third");
        auto a = contact_->acquireFileWork(first);
        auto b = contact_->acquireFileWork(second);
        QVERIFY(a); QVERIFY(b);
        QVERIFY(!contact_->acquireFileWork(third));
        contact_->disconnectFromContact(true);
        QVERIFY(a.value()->cancel.cancelled());
        QVERIFY(b.value()->cancel.cancelled());
        QVERIFY(!contact_->acquireFileWork(third));
        QVERIFY(!contact_->acquireFileWork(first)); // Retired work for the same file still owns its slot.
        a.value().reset();
        auto c = contact_->acquireFileWork(third);
        QVERIFY(c);
        // Expiration also retains admission until the actual owner returns.
        b.value()->deadline = QDeadlineTimer(QDeadlineTimer::Forever);
        b.value()->deadline.setRemainingTime(0);
        QVERIFY(!b.value()->current());
        QVERIFY(!contact_->acquireFileWork(first));
        b.value().reset();
        QVERIFY(contact_->acquireFileWork(first));
        c.value().reset();
        first->deleteFromDb(); second->deleteFromDb(); third->deleteFromDb();
    }
    void checkpoint_channels_exchange_complete_payload_and_final_certificate() {
        const QByteArray bytes(2 * dsfm::minimum_checkpoint_interval + 7, 'e');
        const auto path = directory_.filePath("checkpoint-channel-exchange");
        QFile source(path);
        QVERIFY(source.open(QIODevice::WriteOnly));
        QCOMPARE(source.write(bytes), bytes.size());
        source.close();
        auto imported = waitTask(storage().importFile(path, "checkpoint-channel-exchange"));
        QVERIFY(imported);
        auto data = std::make_unique<FileData>();
        data->state = File::FS_WAITING;
        data->identity = identity_->getId();
        data->contact = contact_->getId();
        data->conversation = conversation_->getId();
        data->storage_namespace = imported.value().namespace_id;
        data->storage_id = imported.value().file.id;
        data->name = "checkpoint-channel-exchange";
        data->size = bytes.size();
        data->hash = imported.value().file.sha256;
        auto outgoing = engine_->getFileManager()->addFile(std::move(data));
        auto received = incoming(bytes, "checkpoint-channel-exchange.bin");
        QVERIFY(waitTask(received->prepareIncoming()));
        auto sent_progress = waitTask(outgoing->prepareCheckpointProgress(dsfm::minimum_checkpoint_interval));
        QVERIFY(sent_progress);
        auto received_progress = waitTask(received->prepareCheckpointProgress(dsfm::minimum_checkpoint_interval));
        QVERIFY(received_progress);
        const dsfm::TransferAttempt attempt{QUuid::createUuid(), dsfm::minimum_checkpoint_interval, 1,
                                            sent_progress.value().checkpoints.first()};
        QVERIFY(outgoing->setCheckpointAttempt(attempt));
        QVERIFY(received->setCheckpointAttempt(attempt));
        auto outgoing_key = outgoing->checkpointKey();
        auto incoming_key = received->checkpointKey();
        QVERIFY(outgoing_key);
        QVERIFY(incoming_key);
        QVERIFY(
            waitTask(storage().transferProgress().beginAttempt(outgoing_key.value(), attempt.id, attempt.selected)));
        QVERIFY(
            waitTask(storage().transferProgress().beginAttempt(incoming_key.value(), attempt.id, attempt.selected)));
        auto sender = std::make_shared<CheckpointSendingPeer>();
        auto receiver = std::make_shared<CheckpointReceivingPeer>();
        sender->on_chunk = [&](quint64 sequence, quint64 offset, const QByteArray &payload, bool final) {
            receiver->receive(sequence, offset, payload, final);
        };
        sender->on_confirmation = [&](dsfm::DurableCheckpoint checkpoint) { receiver->confirm(checkpoint); };
        receiver->on_proposal = [&](dsfm::DurableCheckpoint checkpoint) {
            sender->checkpoint(checkpoint, ds::protocol::FileCheckpoint::Phase::CHECKPOINT_PROPOSED);
        };
        receiver->on_receipt = [&](dsfm::DurableCheckpoint checkpoint) {
            sender->checkpoint(checkpoint, ds::protocol::FileCheckpoint::Phase::CHECKPOINT_RECEIVER_CONFIRMED);
        };
        receiver->on_ack = [&](quint64 sequence, quint64 offset, bool final) {
            sender->ack(sequence, offset, final);
            if (final)
                sender->ack(sequence, offset, final); // Duplicate while sender completion persists.
        };
        receiver->begin(received);
        sender->begin(outgoing);
        QTRY_COMPARE_WITH_TIMEOUT(received->getState(), File::FS_DONE, 10000);
        QTRY_COMPARE_WITH_TIMEOUT(outgoing->getState(), File::FS_DONE, 10000);
        QCOMPARE(sender->sent, bytes);
        QCOMPARE(receiver->proposals, 3);
        QVERIFY(receiver->final_journal_complete);
        QVERIFY(waitTask(storage().transferProgress().load(outgoing_key.value())).value().complete);
        QVERIFY(waitTask(storage().transferProgress().load(incoming_key.value())).value().complete);
        auto reader = waitTask(storage().manager().openRead(received->storageNamespace(), received->storageId()));
        QVERIFY(reader);
        QCOMPARE(waitTask(reader.value()->readAt(0, bytes.size())).value(), bytes);
        QVERIFY(waitTask(reader.value()->close()));
    }
    void incoming_checkpoint_requires_durable_confirmation_before_completion() {
        const QByteArray bytes(dsfm::minimum_checkpoint_interval + 7, 'c');
        auto file = incoming(bytes, "checkpoint-window.bin");
        QVERIFY(waitTask(file->prepareIncoming()));
        auto progress = waitTask(file->prepareCheckpointProgress(dsfm::minimum_checkpoint_interval));
        QVERIFY(progress);
        dsfm::TransferAttempt attempt{QUuid::createUuid(), dsfm::minimum_checkpoint_interval, 1,
                                      progress.value().checkpoints.first()};
        QVERIFY(file->setCheckpointAttempt(attempt));
        auto key = file->checkpointKey();
        QVERIFY(key);
        QVERIFY(waitTask(storage().transferProgress().beginAttempt(key.value(), attempt.id, attempt.selected)));
        auto peer = std::make_shared<CheckpointReceivingPeer>();
        peer->begin(file);
        for (quint64 n = 0; n < 8; ++n)
            peer->receive(n, n * 8192, bytes.mid(n * 8192, 8192), false);
        QTRY_COMPARE_WITH_TIMEOUT(peer->proposals, 1, 5000);
        QCOMPARE(peer->acknowledged, quint64(dsfm::minimum_checkpoint_interval));
        QCOMPARE(waitTask(storage().transferProgress().load(key.value())).value().confirmed_offset, quint64(0));
        QVERIFY_EXCEPTION_THROWN(peer->receive(8, dsfm::minimum_checkpoint_interval, bytes.last(7), true),
                                 std::exception);
        auto checkpoint = peer->proposal;
        QCOMPARE(checkpoint.sha256,
                 QCryptographicHash::hash(bytes.first(dsfm::minimum_checkpoint_interval), QCryptographicHash::Sha256));
        auto forged = checkpoint; forged.sha256 = QByteArray(32, 'x');
        QVERIFY_EXCEPTION_THROWN(peer->confirm(forged), std::exception);
        QCOMPARE(waitTask(storage().transferProgress().load(key.value())).value().confirmed_offset, quint64(0));
        peer->confirm(checkpoint);
        QTRY_COMPARE_WITH_TIMEOUT(peer->receipts, 1, 5000);
        peer->confirm(checkpoint);
        QCOMPARE(peer->receipts, 2);
        peer->receive(8, dsfm::minimum_checkpoint_interval, bytes.last(7), true);
        QTRY_COMPARE_WITH_TIMEOUT(peer->proposals, 2, 5000);
        QCOMPARE(file->getState(), File::FS_TRANSFERRING);
        QVERIFY(!peer->completed);
        auto durable = waitTask(storage().transferProgress().load(key.value()));
        QVERIFY(durable);
        QCOMPARE(durable.value().confirmed_offset, quint64(dsfm::minimum_checkpoint_interval));
        QVERIFY(!durable.value().complete);
        peer->confirm(peer->proposal);
        QTRY_COMPARE_WITH_TIMEOUT(file->getState(), File::FS_DONE, 5000);
        QVERIFY(peer->completed);
        QVERIFY(peer->final_journal_complete);
        QCoreApplication::processEvents(); // Terminal retry state survives queued channel removal.
        const auto final_acks = peer->final_acks;
        peer->receive(8, dsfm::minimum_checkpoint_interval, bytes.last(7), true);
        QCOMPARE(peer->final_acks, final_acks + 1);
        auto reader = waitTask(storage().manager().openRead(file->storageNamespace(), file->storageId()));
        QVERIFY(reader);
        QCOMPARE(waitTask(reader.value()->readAt(0, bytes.size())).value(), bytes);
        QVERIFY(waitTask(reader.value()->close()));
    }
    void incoming_checkpoint_clips_chunks_after_short_paused_prefix() {
        const QByteArray bytes(dsfm::minimum_checkpoint_interval + 5, 'r');
        auto file = incoming(bytes, "checkpoint-short-resume.bin");
        QVERIFY(waitTask(file->prepareIncoming()));
        auto progress = waitTask(file->prepareCheckpointProgress(dsfm::minimum_checkpoint_interval));
        QVERIFY(progress);
        auto key = file->checkpointKey();
        QVERIFY(key);
        auto old_attempt = QUuid::createUuid();
        QVERIFY(waitTask(
            storage().transferProgress().beginAttempt(key.value(), old_attempt, progress.value().checkpoints.first())));
        QVERIFY(waitTask(file->writeIncoming(0, bytes.first(3))));
        auto digest = waitTask(file->checkpointIncomingPrefix(3));
        QVERIFY(digest);
        dsfm::DurableCheckpoint selected{1, 3, digest.value(), false, true};
        QVERIFY(waitTask(file->recordCheckpoint(old_attempt, selected, dsfm::CheckpointPhase::MutuallyConfirmed)));
        dsfm::TransferAttempt attempt{QUuid::createUuid(), dsfm::minimum_checkpoint_interval, 2, selected};
        QVERIFY(waitTask(file->selectIncomingCheckpoint(attempt)));
        auto peer = std::make_shared<CheckpointReceivingPeer>();
        peer->begin(file);
        quint64 offset = 3;
        for (quint64 sequence = 0; sequence < 8; ++sequence) {
            const auto length = qMin<quint64>(8192, dsfm::minimum_checkpoint_interval - offset);
            peer->receive(sequence, offset, bytes.mid(offset, length), false);
            offset += length;
        }
        QTRY_COMPARE_WITH_TIMEOUT(peer->proposals, 1, 5000);
        QCOMPARE(peer->proposal.sequence, quint64(2));
        QCOMPARE(peer->proposal.offset, quint64(dsfm::minimum_checkpoint_interval));
        QCOMPARE(peer->proposal.sha256,
                 QCryptographicHash::hash(bytes.first(dsfm::minimum_checkpoint_interval), QCryptographicHash::Sha256));
        peer->confirm(peer->proposal);
        QTRY_COMPARE_WITH_TIMEOUT(peer->receipts, 1, 5000);
        peer->receive(8, dsfm::minimum_checkpoint_interval, bytes.last(5), true);
        QTRY_COMPARE_WITH_TIMEOUT(peer->proposals, 2, 5000);
        peer->confirm(peer->proposal);
        QTRY_COMPARE_WITH_TIMEOUT(file->getState(), File::FS_DONE, 5000);
        QVERIFY(peer->completed);
        QVERIFY(peer->final_journal_complete);
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
    void empty_incoming_requires_explicit_final_checkpoint() {
        auto file = incoming({}, "checkpoint-empty.bin");
        QVERIFY(waitTask(file->prepareIncoming()));
        auto progress = waitTask(file->prepareCheckpointProgress(dsfm::minimum_checkpoint_interval));
        QVERIFY(progress);
        dsfm::TransferAttempt attempt{QUuid::createUuid(), dsfm::minimum_checkpoint_interval, 1,
                                      progress.value().checkpoints.first()};
        QVERIFY(file->setCheckpointAttempt(attempt));
        auto key = file->checkpointKey();
        QVERIFY(key);
        QVERIFY(waitTask(storage().transferProgress().beginAttempt(key.value(), attempt.id, attempt.selected)));
        auto peer = std::make_shared<CheckpointReceivingPeer>();
        peer->begin(file);
        peer->receive(0, 0, {}, true);
        QTRY_COMPARE_WITH_TIMEOUT(peer->proposals, 1, 5000);
        QVERIFY(peer->proposal.final);
        QCOMPARE(peer->proposal.offset, quint64(0));
        QVERIFY(!peer->completed);
        QCOMPARE(file->getState(), File::FS_TRANSFERRING);
        peer->confirm(peer->proposal);
        QTRY_COMPARE_WITH_TIMEOUT(file->getState(), File::FS_DONE, 5000);
        QVERIFY(peer->completed);
        QVERIFY(peer->final_journal_complete);
    }
    void completed_checkpoint_channels_have_a_fixed_retention_bound() {
        auto peer = std::make_shared<CheckpointReceivingPeer>();
        for (int n = 0; n < 3; ++n) {
            auto file = incoming({}, QStringLiteral("checkpoint-retention-%1.bin").arg(n));
            QVERIFY(waitTask(file->prepareIncoming()));
            auto progress = waitTask(file->prepareCheckpointProgress(dsfm::minimum_checkpoint_interval)); QVERIFY(progress);
            dsfm::TransferAttempt attempt{QUuid::createUuid(), dsfm::minimum_checkpoint_interval, 1, progress.value().checkpoints.first()};
            QVERIFY(file->setCheckpointAttempt(attempt)); auto key = file->checkpointKey(); QVERIFY(key);
            QVERIFY(waitTask(storage().transferProgress().beginAttempt(key.value(), attempt.id, attempt.selected)));
            const auto proposals = peer->proposals;
            peer->begin(file); peer->receive(0, 0, {}, true);
            QTRY_COMPARE_WITH_TIMEOUT(peer->proposals, proposals + 1, 5000);
            peer->confirm(peer->proposal);
            QTRY_COMPARE_WITH_TIMEOUT(file->getState(), File::FS_DONE, 5000);
            QCoreApplication::processEvents();
            QCOMPARE(peer->retainedCompletedCount(), size_t(qMin(n + 1, 2)));
            QCOMPARE(peer->incomingChannelCount(), size_t(qMin(n + 1, 2)));
        }
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
        QCOMPARE(file->getState(), File::FS_CANCELLED);
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
        QTest::newRow("resume-prefix") << false << true;
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
        QTRY_COMPARE_WITH_TIMEOUT(interrupted.value()->getState(),File::FS_FAILED,5000); QVERIFY(denied_peer->cancelled); QVERIFY(!denied_peer->final_sent);
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
        file->setState(File::FS_TRANSFERRING); file->validateHash();
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
        file->setState(File::FS_TRANSFERRING); file->validateHash();
        QTRY_COMPARE_WITH_TIMEOUT(file->getState(), File::FS_FAILED, 10000);
        auto hidden = waitTask(storage().manager().openRead(file->storageNamespace(), file->storageId())); QVERIFY(!hidden);
        QVERIFY(waitTask(file->abortIncoming()));
    }
    void incoming_channel_writes_a_full_receive_window() {
        const QByteArray bytes(8192 * 8 + 7, 'w');
        auto file = incoming(bytes, "window.bin");
        QVERIFY(waitTask(file->prepareIncoming()));
        file->setState(File::FS_TRANSFERRING);
        auto peer = std::make_shared<ReceivingPeer>();
        const auto channel = peer->attach(file);
        for (quint64 sequence = 0; sequence < 8; ++sequence)
            peer->receive(channel, sequence, sequence * 8192, bytes.mid(sequence * 8192, 8192), false);
        QTRY_COMPARE_WITH_TIMEOUT(peer->acknowledged, quint64(8192 * 8), 10000);
        peer->receive(channel, 8, 8192 * 8, bytes.mid(8192 * 8), true);
        QTRY_COMPARE_WITH_TIMEOUT(file->getState(), File::FS_DONE, 10000);
        QVERIFY(peer->completed);
        auto reader = waitTask(storage().manager().openRead(file->storageNamespace(), file->storageId()));
        QVERIFY(reader);
        auto read = waitTask(reader.value()->readAt(0, bytes.size()));
        QVERIFY(read); QCOMPARE(read.value(), bytes);
        QVERIFY(waitTask(reader.value()->close()));
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
        auto socket = std::make_shared<ds::prot::ConnectionSocket>();
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
        server->sendFileChunk(QByteArray(16, 'x'), 0, 0, "invalid", true);
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
    }
    void authenticated_checkpoint_wire_transfer() {
        QFETCH(quint32, interval);
        QFETCH(bool, empty);
        QFETCH(bool, withhold);
        QFETCH(bool, signed_offer);
        QFETCH(QString, control);
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
        FileResumeStatus resume_status = FileResumeStatus::Rejected;
        dsfm::TransferPauseReason remote_pause = dsfm::TransferPauseReason::None;
        QObject observations;
        QVector<std::optional<dsfm::TransferAttempt>> signed_attempts;
        listener.accepted = [&](ds::prot::ConnectionSocket::ptr_t socket) {
            ConnectData data;
            data.service = receiver_identity->getUuid(); data.identitysCert = receiver_identity->getCert();
            server = std::make_shared<CheckpointWireServer>(std::move(socket), data);
            server->preference = interval;
            server->withhold_proposals = withhold && !resume_case && !recovering;
            server->withhold_receipts = resume_case && !recovering;
            connect(server.get(), &PeerConnection::receivedFileResumeResult, &observations, [&](const PeerFileResumeResult &result) {
                if (result.checkpoint_attempt) recovered_offset = result.checkpoint_attempt->selected.offset;
                resume_status = result.status;
                remote_pause = result.pause;
            });
            connect(server.get(), &PeerConnection::receivedMessage, &observations, [&](const PeerMessage &message) {
                signed_attempts = message.attachment_attempts;
            });
            connect(server.get(), &ds::prot::Peer::incomingPeer, receiver_contact.get(), &Contact::onIncomingPeer);
        };
        ds::prot::ConnectionSocket::ptr_t socket;
        std::shared_ptr<CheckpointWireClient> client;
        int repeated_readiness = 0;
        const auto connect_wire = [&] {
            socket = std::make_shared<ds::prot::ConnectionSocket>();
            ConnectData data;
            data.service = identity_->getUuid(); data.identitysCert = identity_->getCert();
            data.contactsCert = receiver_identity->getCert();
            client = std::make_shared<CheckpointWireClient>(socket, data);
            client->preference = dsfm::maximum_checkpoint_interval;
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
        received->accept();
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
    void incoming_channel_can_suspend_between_windows_and_resume() {
        const QByteArray bytes(8192 + 3, 'r');
        auto file = incoming(bytes, "channel-resume.bin");
        QVERIFY(waitTask(file->prepareIncoming()));
        file->setState(File::FS_TRANSFERRING);
        auto peer = std::make_shared<ReceivingPeer>();
        const auto channel = peer->attach(file);
        peer->receive(channel, 0, 0, bytes.left(8192), false);
        QTRY_COMPARE_WITH_TIMEOUT(peer->acknowledged, quint64(8192), 10000);
        file->setState(File::FS_OFFERED);
        auto suspending = file->suspendIncoming();
        auto duplicate = waitTask(file->suspendIncoming());
        QVERIFY(!duplicate); QCOMPARE(duplicate.error().code, dsfm::ErrorCode::Busy);
        QVERIFY(waitTask(std::move(suspending)));
        QCoreApplication::processEvents();
        auto resumed = waitTask(file->prepareIncoming());
        QVERIFY(resumed); QCOMPARE(resumed.value(), quint64(8192));
        file->setState(File::FS_TRANSFERRING);
        const auto resumed_channel = peer->attach(file);
        peer->receive(resumed_channel, 1, 8192, bytes.mid(8192), true);
        QTRY_COMPARE_WITH_TIMEOUT(file->getState(), File::FS_DONE, 10000);
        QVERIFY(peer->completed);
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
        file->setState(File::FS_TRANSFERRING); file->validateHash();
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
        auto peer = std::make_shared<ReceivingPeer>();
        peer->receive(peer->attach(thumbnail), 0, 0, jpeg, true);
        QTRY_COMPARE_WITH_TIMEOUT(thumbnail->getState(), File::FS_DONE, 10000);
        const auto thumbnail_ns = thumbnail->storageNamespace(), thumbnail_id = thumbnail->storageId();
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
    void incoming_original_preview_rejects_a_false_mime_claim() {
        QImage image(64, 32, QImage::Format_RGB32); image.fill(Qt::blue);
        QByteArray bytes; QBuffer buffer(&bytes); QVERIFY(buffer.open(QIODevice::WriteOnly)); QVERIFY(image.save(&buffer, "PNG"));
        auto file = incoming(bytes, "false-jpeg.jpg", "image/jpeg");
        QVERIFY(waitTask(file->prepareIncoming())); file->setState(File::FS_TRANSFERRING);
        auto peer = std::make_shared<ReceivingPeer>();
        peer->receive(peer->attach(file), 0, 0, bytes, true);
        QTRY_COMPARE_WITH_TIMEOUT(file->getState(), File::FS_DONE, 10000);
        auto preview = waitTask(storage().imagePreview(file->getId()));
        QVERIFY(!preview); QCOMPARE(preview.error().code, dsfm::ErrorCode::InvalidArgument);
        file->deleteFromDb(); QVERIFY(waitTask(storage().collectGarbage()));
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
        QCOMPARE(file->getState(), File::FS_CANCELLED);
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
        finished->setState(File::FS_TRANSFERRING); finished->validateHash();
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
    void resume_validation_reserves_slots_before_asynchronous_io() {
        auto peer_data = std::make_unique<ContactData>();
        peer_data->identity = identity_->getId(); peer_data->name = "Bounded resume peer";
        const auto cert = ds::crypto::DsCert::create();
        peer_data->cert = ds::crypto::DsCert::createFromPubkey(cert->getSigningPubKey().toByteArray());
        peer_data->address = "fixture-no-network"; peer_data->autoConnect = false;
        peer_data->state = Contact::ACCEPTED;
        auto* created = engine_->getContactManager()->addContact(std::move(peer_data));
        auto contact = engine_->getContactManager()->getContact(created->getUuid());
        auto conversation = engine_->getConversationManager()->getConversation(contact.get());
        const auto cleanup = qScopeGuard([&] {
            contact->disconnectFromContact(true);
            engine_->getContactManager()->deleteContact(contact->getUuid());
        });
        auto imported = waitTask(storage().importFile(QStringLiteral(DS_STORAGE_FIXTURES) + "/../assets/mcp/earth.jpg", "bounded-resume.jpg"));
        QVERIFY(imported);
        auto peer = std::make_shared<ResumePeer>();
        contact->onConnectedToPeer(peer);
        UserInfo info;
        info.nickName = "Fixture peer"; info.allowFiles = true; info.allowMimeTypes = {"image/jpeg"};
        emit peer->receivedUserInfo(PeerUserInfo(peer, peer->getConnectionId(), 0, info));
        std::array<File::ptr_t, 3> files;
        for (auto& file : files) {
            auto data = std::make_unique<FileData>();
            data->state = File::FS_FAILED; data->identity = identity_->getId(); data->contact = contact->getId();
            data->conversation = conversation->getId(); data->name = "bounded-resume.jpg"; data->mime_type = "image/jpeg";
            data->storage_namespace = imported.value().namespace_id; data->storage_id = imported.value().file.id;
            data->size = imported.value().file.size; data->hash = imported.value().file.sha256;
            file = engine_->getFileManager()->addFile(std::move(data));
        }
        // No event-loop turns between requests: the first validations are still
        // awaiting workers. The third request must fail before starting I/O.
        for (size_t n = 0; n < files.size(); ++n)
            emit peer->receivedFileResume(PeerFileResume(peer, peer->getConnectionId(), n + 1,
                files[n]->getFileId(), 8192, files[n]->getHash(), quint64(files[n]->getSize())));
        QCOMPARE(peer->responses, 1);
        QVERIFY(!peer->accepted);
        QCOMPARE(files[2]->getState(), File::FS_FAILED);
        QTRY_COMPARE_WITH_TIMEOUT(peer->responses, 3, 5000);
        QCOMPARE(peer->starts, 0);
        QCOMPARE(files[0]->getState(), File::FS_QUEUED);
        QCOMPARE(files[1]->getState(), File::FS_QUEUED);
        contact->disconnectFromContact(true);
        for (auto& file : files) QCOMPARE(file->getState(), File::FS_FAILED);
    }
    void hashing_and_incoming_slots_do_not_spin_the_scheduler() {
        auto data = std::make_unique<ContactData>();
        data->identity = identity_->getId(); data->name = "Hashing slot peer";
        auto cert = ds::crypto::DsCert::create();
        data->cert = ds::crypto::DsCert::createFromPubkey(cert->getSigningPubKey().toByteArray());
        data->address = "fixture-no-network"; data->autoConnect = false; data->state = Contact::ACCEPTED;
        auto *added = engine_->getContactManager()->addContact(std::move(data));
        auto contact = engine_->getContactManager()->getContact(added->getUuid());
        auto conversation = engine_->getConversationManager()->getConversation(contact.get());
        auto peer = std::make_shared<ResumePeer>();
        const auto cleanup = qScopeGuard([&] {
            contact->disconnectFromContact(true);
            engine_->getContactManager()->deleteContact(contact->getUuid());
        });
        contact->onConnectedToPeer(peer);
        UserInfo preferences;
        preferences.nickName = "Fixture"; preferences.allowFiles = true; preferences.allowMimeTypes = {"*"};
        emit peer->receivedUserInfo(PeerUserInfo(peer, peer->getConnectionId(), 0, preferences));
        std::array<File::ptr_t, 3> files;
        for (size_t index = 0; index < files.size(); ++index) {
            auto file = std::make_unique<FileData>();
            file->state = index < 2 ? File::FS_QUEUED : File::FS_OFFERED;
            file->direction = File::INCOMING; file->identity = identity_->getId(); file->contact = contact->getId();
            file->conversation = conversation->getId(); file->name = QStringLiteral("slot-%1").arg(index);
            file->size = 1; file->hash = QCryptographicHash::hash("x", QCryptographicHash::Sha256);
            files[index] = engine_->getFileManager()->addFile(std::move(file));
        }
        contact->queueFile(files[0]); contact->queueFile(files[1]);
        QTRY_COMPARE_WITH_TIMEOUT(peer->starts, 2, 5000);
        const auto first = std::less<File::ptr_t>{}(files[0], files[1]) ? files[0] : files[1];
        first->setState(File::FS_HASHING);
        emit peer->outputBufferEmptied(); // Must return with a hashing entry and a live incoming entry.
        QVERIFY(!contact->acquireFileWork(files[2])); // Hashing/publication still consumes its slot.
        first->setState(File::FS_DONE);
        QVERIFY(contact->acquireFileWork(files[2]));
    }
    void resumed_sender_waits_for_receiver_channel_readiness() {
        auto peer_data = std::make_unique<ContactData>();
        peer_data->identity = identity_->getId(); peer_data->name = "Resume readiness peer";
        const auto cert = ds::crypto::DsCert::create();
        peer_data->cert = ds::crypto::DsCert::createFromPubkey(cert->getSigningPubKey().toByteArray());
        peer_data->address = "fixture-resume-not-networked"; peer_data->autoConnect = false;
        peer_data->state = Contact::ACCEPTED;
        auto* created = engine_->getContactManager()->addContact(std::move(peer_data));
        auto contact = engine_->getContactManager()->getContact(created->getUuid());
        auto conversation = engine_->getConversationManager()->getConversation(contact.get());
        const auto cleanup = qScopeGuard([&] {
            contact->disconnectFromContact(true);
            engine_->getContactManager()->deleteContact(contact->getUuid());
        });
        auto data = std::make_unique<FileData>();
        data->identity = identity_->getId(); data->contact = contact->getId();
        data->conversation = conversation->getId(); data->name = "resume-readiness.jpg";
        data->source_path = QStringLiteral(DS_STORAGE_FIXTURES) + "/../assets/mcp/earth.jpg";
        auto file = engine_->getFileManager()->addFile(std::move(data));
        QTRY_COMPARE_WITH_TIMEOUT(file->getState(), File::FS_WAITING, 10000);
        file->setState(File::FS_FAILED);
        contact->setState(Contact::ACCEPTED);
        auto peer = std::make_shared<ResumePeer>();
        contact->onConnectedToPeer(peer);
        UserInfo info;
        info.nickName = "Fixture peer"; info.allowFiles = true; info.allowMimeTypes = {"image/jpeg"};
        emit peer->receivedUserInfo(PeerUserInfo(peer, peer->getConnectionId(), 0, info));
        emit peer->receivedFileResume(PeerFileResume(peer, peer->getConnectionId(), 1,
            file->getFileId(), 8192, file->getHash(), quint64(file->getSize())));
        QTRY_COMPARE_WITH_TIMEOUT(peer->responses, 1, 5000);
        QVERIFY(peer->accepted); QCOMPARE(peer->negotiated_offset, quint64(8192));
        QCOMPARE(peer->starts, 0); QCOMPARE(file->getState(), File::FS_QUEUED);
        // The receiver may still be awaiting asynchronous disk I/O here.
        QCoreApplication::processEvents();
        QCOMPARE(peer->starts, 0);
        auto stale = std::make_shared<ResumePeer>();
        emit peer->receivedFileOfferResponse(PeerFileOfferResponse(stale, stale->getConnectionId(), 0,
            file->getFileId(), FileOfferResponseStatus::Accepted));
        QCOMPARE(peer->starts, 0);
        emit peer->receivedFileOfferResponse(PeerFileOfferResponse(peer, peer->getConnectionId(), 0,
            file->getFileId(), FileOfferResponseStatus::Accepted));
        QTRY_COMPARE_WITH_TIMEOUT(peer->starts, 1, 5000);
        QCOMPARE(peer->started_offset, qlonglong(8192));
        contact->disconnectFromContact(true);
        QCOMPARE(file->getState(), File::FS_FAILED);
        contact->setState(Contact::PENDING);
        file->deleteFromDb();
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
        QVERIFY(waitTask(storage().shutdown()));
        conversation_.reset();
        engine_.reset();
    }
private:
    ManagedStorage& storage() { return engine_->getFileManager()->storage(); }
    File::ptr_t incoming(QByteArray bytes, QString name, QString mime_type = {}) {
        auto data = std::make_unique<FileData>();
        data->direction = File::INCOMING; data->state = File::FS_OFFERED;
        data->identity = identity_->getId(); data->contact = contact_->getId(); data->conversation = conversation_->getId();
        data->size = bytes.size(); data->name = std::move(name); data->hash = QCryptographicHash::hash(bytes, QCryptographicHash::Sha256);
        data->mime_type = std::move(mime_type);
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
