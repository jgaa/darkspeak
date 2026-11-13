#include <QtTest>
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
#include <QtEndian>
#include <QTcpServer>
#include "ds/dsclient.h"
#include "ds/dsserver.h"
#include "ds/peer.h"
#include "ds/crypto.h"
#include "ds/dsengine.h"
#include "ds/managedstorage.h"
#include "ds/resourcelimits.h"

using namespace ds::core;
template<class T> T waitTask(QCoro::Task<T> task) {
    QEventLoop loop;
    std::optional<T> result;
    task.then([&](T value) { result.emplace(std::move(value)); loop.quit(); });
    if (!result) loop.exec();
    return std::move(*result);
}
class ReceivingPeer final : public ds::prot::Peer {
public:
    ReceivingPeer() : Peer(std::make_shared<ds::prot::ConnectionSocket>(), {}) {}
    Direction getDirection() const noexcept override { return OUTGOING; }
    bool isConnected() const noexcept override { return true; }
    quint32 attach(const File::ptr_t& file) {
        const auto channel = createChannel(*file);
        file->setChannel(channel);
        return channel;
    }
    void receive(quint32 channel, quint64 sequence, quint64 offset, const QByteArray& bytes, bool final) {
        QByteArray frame(file_chunk_header_bytes, Qt::Uninitialized);
        qToBigEndian(sequence, frame.data());
        qToBigEndian(offset, frame.data() + sizeof(quint64));
        frame.append(bytes);
        auto instance = inChannels_.at(channel);
        instance->onIncoming(*this, 0, mview_t{reinterpret_cast<uint8_t*>(frame.data()), size_t(frame.size())}, final);
    }
    uint64_t sendFileAck(const QByteArray&, quint64, quint64 offset, bool final) override {
        acknowledged = offset;
        completed = final;
        return 0;
    }
    quint64 acknowledged = 0;
    bool completed = false;
};
class LoopbackListener final : public QTcpServer {
public:
    std::function<void(ds::prot::ConnectionSocket::ptr_t)> accepted;
protected:
    void incomingConnection(qintptr descriptor) override {
        auto socket = std::make_shared<ds::prot::ConnectionSocket>();
        if (socket->setSocketDescriptor(descriptor)) accepted(std::move(socket));
    }
};
class QueueProtocol final : public ProtocolManager {
public:
    int attempts = 0;
    State getState() const override { return ONLINE; }
    QByteArray getPeerHandle(const QUuid&, const QUuid&) override { return {}; }
    void start() override {}
    void stop() override {}
    void createTransportHandle(const TransportHandleReq&) override {}
    void startService(const QUuid&, const ds::crypto::DsCert::ptr_t&, const QVariantMap&) override {}
    void stopService(const QUuid&) override {}
    PeerConnection::ptr_t connectTo(ConnectData) override {
        ++attempts;
        return std::make_shared<ReceivingPeer>();
    }
    uint64_t sendAddme(const AddmeReq&) override { return 0; }
};
class ManagedStorageTest final : public QObject {
    Q_OBJECT
private slots:
    void initTestCase() {
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
        identity.uuid = QUuid::createUuid(); identity.name = "Storage fixture";
        identity.cert = ds::crypto::DsCert::create(); identity.hash = identity.cert->getHash().toByteArray();
        identity.autoConnect = false;
        identity_ = engine_->getIdentityManager()->addIdentity(identity);
        auto contact = std::make_unique<ContactData>();
        contact->identity = identity_->getId(); contact->name = "Fixture peer";
        const auto cert = ds::crypto::DsCert::create();
        contact->cert = ds::crypto::DsCert::createFromPubkey(cert->getSigningPubKey().toByteArray());
        contact->address = "fixture-not-networked"; contact->autoConnect = false;
        contact_ = engine_->getContactManager()->addContact(std::move(contact));
        conversation_ = engine_->getConversationManager()->getConversation(contact_);
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
    File::ptr_t incoming(QByteArray bytes, QString name) {
        auto data = std::make_unique<FileData>();
        data->direction = File::INCOMING; data->state = File::FS_OFFERED;
        data->identity = identity_->getId(); data->contact = contact_->getId(); data->conversation = conversation_->getId();
        data->size = bytes.size(); data->name = std::move(name); data->hash = QCryptographicHash::hash(bytes, QCryptographicHash::Sha256);
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
