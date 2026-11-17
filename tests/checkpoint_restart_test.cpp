#include <QCoreApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QBuffer>
#include <QImage>
#include <QCryptographicHash>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QSaveFile>
#include <QSettings>
#include <QTcpServer>
#include <QTemporaryDir>
#include <QThread>
#include <QCoroTask>
#include <QCoroTimer>
#include <QTimer>
#include <unistd.h>
#include <iostream>
#include <logfault/logfault.h>
#include <stdexcept>
#include "ds/crypto.h"
#include "ds/dsclient.h"
#include "ds/dsserver.h"
#include "ds/dsengine.h"
#include "ds/managedstorage.h"

using namespace ds::core;
namespace {
void check(bool ok, const char *reason) { if (!ok) throw std::runtime_error(reason); }
template<class T> T get(QCoro::Task<dsfm::Result<T>> task) {
    auto result = QCoro::waitFor(std::move(task));
    check(bool(result), "Storage operation failed");
    return std::move(result).value();
}
void get(QCoro::Task<dsfm::Result<void>> task) {
    check(bool(QCoro::waitFor(std::move(task))), "Storage operation failed");
}
template<class Predicate> void until(Predicate predicate, const char *reason) {
    QElapsedTimer timer; timer.start();
    while (!predicate()) {
        check(timer.elapsed() < 10000, reason);
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        QThread::msleep(1);
    }
}
void save(const QString &root, const QJsonObject &state) {
    QSaveFile file(root + "/fixture.json"); check(file.open(QIODevice::WriteOnly), "Cannot save restart fixture");
    const auto bytes = QJsonDocument(state).toJson(QJsonDocument::Compact);
    check(file.write(bytes) == bytes.size() && file.commit(), "Cannot commit restart fixture");
}
QJsonObject load(const QString &root) {
    QFile file(root + "/fixture.json"); check(file.open(QIODevice::ReadOnly), "Missing restart fixture");
    return QJsonDocument::fromJson(file.readAll()).object();
}
class Listener final : public QTcpServer {
  public:
    std::function<void(ds::prot::ConnectionSocket::ptr_t)> accepted;
  private:
    void incomingConnection(qintptr descriptor) override {
        auto socket = ds::prot::ConnectionSocket::create();
        check(socket->setSocketDescriptor(descriptor), "Cannot adopt loopback socket");
        accepted(std::move(socket));
    }
};
class Client final : public ds::prot::DsClient {
  public:
    using DsClient::DsClient;
    quint64 payload = 0, legacy_payload = 0;
    bool user_info = false;
    bool suppress_offer = false;
    bool fail_offer = false;
    uint64_t offerFile(const File &file) override {
        if (fail_offer) throw std::runtime_error("Injected offer queue failure");
        if (suppress_offer) return 0; // Sender queued it, receiver never learned it.
        return DsClient::offerFile(file);
    }
    std::function<void()> chunk_sent;
    quint32 preference = dsfm::minimum_checkpoint_interval;
    uint64_t sendFileChunk(const QByteArray &file, quint64 sequence, quint64 offset,
                           const QByteArray &bytes, bool final) override {
        legacy_payload += quint64(bytes.size());
        return DsClient::sendFileChunk(file, sequence, offset, bytes, final);
    }
    uint64_t sendCheckpointChunk(const QByteArray &file, QUuid attempt, quint64 sequence,
                                quint64 offset, const QByteArray &bytes, bool final) override {
        payload += quint64(bytes.size());
        const auto result = DsClient::sendCheckpointChunk(file, attempt, sequence, offset, bytes, final);
        if (chunk_sent) chunk_sent();
        return result;
    }
  private:
    quint32 localCheckpointPreference() const noexcept override { return preference; }
};
class Server final : public ds::prot::DsServer {
  public:
    using DsServer::DsServer;
    bool hold_receipts = false, final_only = false, held = false, hint_seen = false, manifest_seen = false;
    bool corrupt_readiness = false, user_info = false;
    int readiness_failures = 0;
    quint32 preference = dsfm::minimum_checkpoint_interval;
    uint64_t sendCheckpointOfferResponse(const QByteArray &file, const dsfm::TransferAttempt &attempt) override {
        if (readiness_failures) { --readiness_failures; throw std::runtime_error("Injected readiness queue failure"); }
        auto response = attempt;
        if (corrupt_readiness) response.id = QUuid::createUuid();
        return DsServer::sendCheckpointOfferResponse(file, response);
    }
    uint64_t sendFileCheckpoint(const QByteArray &file, const QByteArray &sha, const dsfm::TransferAttempt &attempt,
                               dsfm::DurableCheckpoint checkpoint, dsfm::CheckpointPhase phase) override {
        if (hold_receipts && phase == dsfm::CheckpointPhase::MutuallyConfirmed && (!final_only || checkpoint.final)) {
            held = true; return 0;
        }
        return DsServer::sendFileCheckpoint(file, sha, attempt, checkpoint, phase);
    }
  private:
    quint32 localCheckpointPreference() const noexcept override { return preference; }
};
Identity *addIdentity(DsEngine &engine, const QString &name) {
    IdentityData data; data.uuid = QUuid::createUuid(); data.name = name;
    data.cert = ds::crypto::DsCert::create(); data.hash = data.cert->getHash().toByteArray(); data.autoConnect = false;
    return engine.getIdentityManager()->addIdentity(data);
}
Contact::ptr_t addContact(DsEngine &engine, Identity &local, Identity &remote) {
    auto data = std::make_unique<ContactData>(); data->identity = local.getId(); data->name = "Restart loopback peer";
    data->cert = ds::crypto::DsCert::createFromPubkey(remote.getCert()->getSigningPubKey().toByteArray());
    data->address = "isolated-no-public-network"; data->autoConnect = false; data->state = Contact::ACCEPTED;
    auto *contact = engine.getContactManager()->addContact(std::move(data)); contact->setPeerVerified(true);
    return engine.getContactManager()->getContact(contact->getId());
}
int child(const QString &root, const QString &scenario, bool recovering) {
    const bool lost_offer = scenario.startsWith("lost-offer");
    const bool preview_scenario = scenario.startsWith("preview") || scenario == "lost-offer-preview";
    const bool eof_scenario = scenario.startsWith("eof");
    const bool unsupported = scenario.startsWith("unsupported-");
    const bool offered = scenario == "offered" || scenario == "offer-acceptance-race" || scenario == "unsupported-offered" || scenario == "unsupported-waiting";
    const bool queued = scenario == "queued" || scenario == "unsupported-queued";
    const bool partial = scenario == "partial" || scenario == "unsupported-partial" || scenario == "readiness-retry";
    const bool changed_preference = scenario.endsWith("preference");
    const auto preference = unsupported && recovering ? 0U : changed_preference && !recovering
        ? dsfm::default_checkpoint_interval : dsfm::minimum_checkpoint_interval;
    qputenv("DARKSPEAK_PROFILE_ROOT", root.toUtf8());
    qputenv("DARKSPEAK_TEST_SECRETSTORE_PASSPHRASE", "checkpoint-restart-disposable-fixture");
    ds::crypto::Crypto crypto;
    auto settings = std::make_unique<QSettings>(root + "/settings.ini", QSettings::IniFormat);
    settings->setValue("dbpath", root + "/app.sqlite"); settings->setValue("downloadLocation", root + "/downloads");
    settings->setValue("autoAcceptIncomingFiles", false); settings->sync();
    check(QDir().mkpath(root + "/downloads"), "Cannot create isolated download directory");
    auto engine = std::make_unique<DsEngine>(std::move(settings));
    auto &storage = engine->getFileManager()->storage(); get(storage.ready());
    try {
    QJsonObject state;
    Identity *sender = nullptr, *receiver = nullptr;
    Contact::ptr_t source_contact, target_contact;
    File::ptr_t outgoing, incoming;
    QByteArray bytes(dsfm::minimum_checkpoint_interval + 7, 'r'), preview_bytes;
    bool acceptance_during_preparation = false;
    if (preview_scenario) {
        QImage image(128, 128, QImage::Format_RGB32);
        for (int y = 0; y < image.height(); ++y)
            for (int x = 0; x < image.width(); ++x)
                image.setPixel(x, y, qRgb((x * 73 + y * 131) % 256, (x * 173 + y * 31) % 256, (x * 53 + y * 97) % 256));
        bytes.clear(); QBuffer png(&bytes); check(png.open(QIODevice::WriteOnly) && image.save(&png, "PNG"), "Cannot encode original image");
        QBuffer jpeg(&preview_bytes); check(jpeg.open(QIODevice::WriteOnly) && image.save(&jpeg, "JPEG", 90), "Cannot encode preview");
        check(preview_bytes.size() > 8192 && preview_bytes.size() <= 48 * 1024, "Preview does not exercise multiple bounded chunks");
    }
    if (recovering) {
        state = load(root);
        sender = engine->getIdentityManager()->identityFromId(state["sender"].toInt());
        receiver = engine->getIdentityManager()->identityFromId(state["receiver"].toInt());
        source_contact = engine->getContactManager()->getContact(state["source_contact"].toInt());
        target_contact = engine->getContactManager()->getContact(state["target_contact"].toInt());
        outgoing = engine->getFileManager()->getFile(state["outgoing"].toInt());
        if (!lost_offer) incoming = engine->getFileManager()->getFile(state["incoming"].toInt());
        check(sender && receiver && source_contact && target_contact && outgoing && (incoming || lost_offer), "Cannot reopen scoped transfer");
        check(!outgoing->checkpointAttempt() && (!incoming || !incoming->checkpointAttempt()), "Restart restored a live attempt");
        check(!incoming || incoming->offeredConnection().isNull(), "Restart retained a connection token");
        if (scenario == "preview-policy") receiver->setPreviewImages(false);
        if (scenario == "preview-active") receiver->setPreviewImages(true);
    } else {
        sender = addIdentity(*engine, "Restart sender"); receiver = addIdentity(*engine, "Restart receiver");
        check(sender && receiver, "Cannot create isolated identities");
        if (preview_scenario) receiver->setPreviewImages(true);
        source_contact = addContact(*engine, *sender, *receiver); target_contact = addContact(*engine, *receiver, *sender);
        state = {{"sender", sender->getId()}, {"receiver", receiver->getId()},
                 {"source_contact", source_contact->getId()}, {"target_contact", target_contact->getId()}};
        QFile source(root + "/source.bin"); check(source.open(QIODevice::WriteOnly), "Cannot create fixture source");
        check(source.write(bytes) == bytes.size(), "Cannot write fixture source"); source.close();
        auto imported = get(storage.importFile(source.fileName(), "restart-source"));
        auto data = std::make_unique<FileData>(); data->state = File::FS_WAITING;
        data->identity = sender->getId(); data->contact = source_contact->getId();
        data->conversation = engine->getConversationManager()->getConversation(source_contact.get())->getId();
        data->storage_namespace = imported.namespace_id; data->storage_id = imported.file.id;
        data->name = "restart.bin"; data->mime_type = preview_scenario ? "image/png" : "application/octet-stream";
        data->size = bytes.size(); data->hash = imported.file.sha256;
        outgoing = engine->getFileManager()->addFile(std::move(data));
        if (preview_scenario) {
            auto preview = std::make_unique<FileData>(); preview->state = File::FS_HASHING;
            preview->identity = sender->getId(); preview->contact = source_contact->getId(); preview->conversation = outgoing->getConversationId();
            preview->name = "preview.jpg"; preview->mime_type = "image/jpeg"; preview->size = preview_bytes.size();
            preview->hash = QCryptographicHash::hash(preview_bytes, QCryptographicHash::Sha256);
            preview->thumbnail_for = outgoing->getFileId(); preview->manifest_id = ds::crypto::Crypto::getRandomBytes(32);
            auto thumbnail = engine->getFileManager()->addFile(std::move(preview)); get(thumbnail->importThumbnail(preview_bytes));
        }
    }
    if (!recovering && scenario == "lost-offer-preparing") {
        check(bool(outgoing->setPreparedSource(sender->getUuid(), source_contact->getUuid(), [] { return true; },
            [&](dsfm::ReadPreparationOptions) -> QCoro::Task<dsfm::Result<dsfm::PreparedRead>> {
                check(outgoing->getState() == File::FS_QUEUED && outgoing->checkpointAttempt(), "Missed pre-offer crash boundary");
                state["outgoing"] = outgoing->getId(); state["attempt"] = outgoing->checkpointAttempt()->id.toString();
                save(root, state); ::_exit(86);
                co_return dsfm::Error{dsfm::ErrorCode::Cancelled};
            })), "Cannot bind pre-offer crash source");
    }
    if (recovering && scenario == "offer-acceptance-race") {
        const auto ns = outgoing->storageNamespace(); const auto id = outgoing->storageId();
        check(bool(outgoing->setPreparedSource(sender->getUuid(), source_contact->getUuid(), [] { return true; },
            [&, ns, id, accepted = incoming](dsfm::ReadPreparationOptions options) -> QCoro::Task<dsfm::Result<dsfm::PreparedRead>> {
                if (!acceptance_during_preparation) {
                    acceptance_during_preparation = true;
                    accepted->accept();
                    QTimer timer; timer.setSingleShot(true); timer.start(500);
                    co_await qCoro(timer).waitForTimeout();
                }
                co_return co_await storage.manager().prepareRead(ns, id, options);
            })), "Cannot bind delayed retry source");
    }
    Listener listener; check(listener.listen(QHostAddress::LocalHost, 0), "Cannot listen on loopback");
    std::shared_ptr<Server> server;
    bool stale_readiness_sent = false;
    listener.accepted = [&](ds::prot::ConnectionSocket::ptr_t socket) {
        ConnectData data; data.service = receiver->getUuid(); data.identitysCert = receiver->getCert();
        server = std::make_shared<Server>(std::move(socket), data);
        server->preference = preference;
        if (recovering && scenario == "readiness-retry") server->readiness_failures = 1;
        QObject::connect(server.get(), &PeerConnection::receivedUserInfo, server.get(), [&](const PeerUserInfo &) { server->user_info = true; });
        QObject::connect(server.get(), &PeerConnection::receivedFileOffer, server.get(), [&](const PeerFileOffer &) { server->hint_seen = true; });
        QObject::connect(server.get(), &PeerConnection::receivedMessage, server.get(), [&](const PeerMessage &) { server->manifest_seen = true; });
        server->hold_receipts = !recovering && (partial || eof_scenario || preview_scenario);
        server->final_only = eof_scenario || preview_scenario;
        server->corrupt_readiness = !recovering && scenario == "preview-stale";
        QObject::connect(server.get(), &ds::prot::Peer::incomingPeer, target_contact.get(), &Contact::onIncomingPeer);
        if (recovering && scenario == "lost-offer-stale")
            QObject::connect(server.get(), &ds::prot::Peer::connectedToPeer, server.get(), [&](const auto &) {
                const dsfm::TransferAttempt old{QUuid(state["attempt"].toString()), outgoing->checkpointInterval(), 1,
                    {0, 0, QCryptographicHash::hash(QByteArray{}, QCryptographicHash::Sha256), false, false}};
                server->sendCheckpointOfferResponse(outgoing->getFileId(), old);
                stale_readiness_sent = true;
            });
    };
    auto socket = ds::prot::ConnectionSocket::create();
    ConnectData data; data.service = sender->getUuid(); data.identitysCert = sender->getCert(); data.contactsCert = receiver->getCert();
    auto client = std::make_shared<Client>(socket, data);
    client->suppress_offer = !recovering && lost_offer;
    client->fail_offer = !recovering && scenario == "lost-offer-failed";
    client->preference = preference;
    QObject::connect(client.get(), &PeerConnection::receivedUserInfo, client.get(), [&](const PeerUserInfo &) { client->user_info = true; });
    if (!recovering && scenario == "preview-active")
        client->chunk_sent = [receiver] { receiver->setPreviewImages(false); };
    QObject::connect(client.get(), &ds::prot::Peer::connectedToPeer, source_contact.get(), &Contact::onConnectedToPeer);
    socket->connectToHost(QHostAddress::LocalHost, listener.serverPort());
    if (recovering && scenario == "lost-offer-stale") {
        until([&] { return stale_readiness_sent && !client->isConnected(); }, "Old-connection initial readiness was admitted");
        const auto progress = get(storage.transferProgress().load(outgoing->checkpointKey().value()));
        check(progress.attempt == QUuid(state["attempt"].toString()) && !progress.confirmed_offset &&
              !progress.verified_prefix && !outgoing->checkpointAttempt() && !client->payload,
              "Stale initial readiness changed authority or payload progress");
        source_contact->disconnectFromContact(true); target_contact->disconnectFromContact(true);
        get(storage.shutdown()); return 0;
    }
    until([&] { return client->user_info && server && server->user_info && (unsupported && recovering
        ? !client->checkpointInterval() && !server->checkpointInterval() : client->checkpointInterval() && server->checkpointInterval()) &&
        source_contact->canSendFileType("application/octet-stream", "restart.bin") &&
        target_contact->canSendFileType("application/octet-stream", "restart.bin"); }, "Authenticated loopback did not become ready");
    if (recovering && scenario == "preview-policy") {
        const auto before = get(storage.transferProgress().load(incoming->checkpointKey().value()));
        check(before.complete && !target_contact->canReceiveFile(*incoming), "Restart did not withdraw preview permission");
        target_contact->recoverCompletedFile(*incoming);
        {
            const auto probe = target_contact->acquireFileWork(incoming);
            check(bool(probe), "Withdrawn preview permission admitted recovery work");
        }
        until([&] { return server->manifest_seen; }, "No repeated preview manifest after restart");
        const auto after = get(storage.transferProgress().load(incoming->checkpointKey().value()));
        check(after.complete && after.attempt == before.attempt && incoming->getState() == File::FS_DONE &&
              incoming->storageId() == QUuid(state["published"].toString()) && incoming->pendingId().isNull() &&
              client->payload == 0 && outgoing->getState() != File::FS_DONE,
              "Withdrawn preview policy changed durable completion or sent payload");
        const auto original = engine->getFileManager()->getFile(state["primary_incoming"].toInt());
        check(original->getState() == File::FS_OFFERED && original->storageId().isNull(), "Preview policy accepted its original");
        source_contact->disconnectFromContact(true); target_contact->disconnectFromContact(true);
        get(storage.shutdown()); return 0;
    }
    if (!recovering) {
        source_contact->queueFile(outgoing);
        if (lost_offer) {
            until([&] { return outgoing->getState() == (client->fail_offer ? File::FS_PAUSED : File::FS_OFFERED); }, "No persisted lost offer");
            const auto progress = get(storage.transferProgress().load(outgoing->checkpointKey().value()));
            check(!progress.attempt.isNull() && client->payload == 0 &&
                  !engine->getFileManager()->getFileFromId(outgoing->getFileId(), *target_contact, File::INCOMING),
                  "Lost offer reached the receiver");
            state["outgoing"] = outgoing->getId(); state["attempt"] = progress.attempt.toString();
            if (preview_scenario) {
                const auto preview = engine->getFileManager()->thumbnailFor(*outgoing);
                check(bool(preview), "Missing undelivered preview");
                state["preview_attempt"] = get(storage.transferProgress().load(preview->checkpointKey().value())).attempt.toString();
            }
            save(root, state); ::_exit(86);
        }
        until([&] { incoming = engine->getFileManager()->getFileFromId(outgoing->getFileId(), *target_contact, File::INCOMING); return bool(incoming); }, "No incoming offer");
        if (preview_scenario) {
            state["primary_outgoing"] = outgoing->getId(); state["primary_incoming"] = incoming->getId();
            outgoing = engine->getFileManager()->thumbnailFor(*outgoing); check(bool(outgoing), "Missing preview transfer");
            until([&] { incoming = engine->getFileManager()->getFileFromId(outgoing->getFileId(), *target_contact, File::INCOMING); return bool(incoming); }, "No signed preview offer");
        }
        state["outgoing"] = outgoing->getId(); state["incoming"] = incoming->getId();
        const auto offered_progress = get(storage.transferProgress().load(outgoing->checkpointKey().value()));
        check(!offered_progress.attempt.isNull(), "Offer had no persisted sender attempt");
        state["attempt"] = offered_progress.attempt.toString();
        state["published"] = QString{}; save(root, state);
        if (scenario == "preview-stale") {
            until([&] { return !socket->isOpen(); }, "Unknown readiness nonce was not rejected by the sender");
            const auto progress = get(storage.transferProgress().load(outgoing->checkpointKey().value()));
            check(progress.attempt == QUuid(state["attempt"].toString()) && progress.verified_prefix == 0 &&
                  progress.failure == dsfm::TransferFailureReason::None && !outgoing->checkpointAttempt() && client->payload == 0,
                  "Unverified readiness changed the sender's authority or progress");
            ::_exit(86);
        }
        if (scenario == "preview-active") {
            until([&] { return incoming->getState() == File::FS_PAUSED; }, "Preview revocation did not stop its active channel");
            const auto progress = get(storage.transferProgress().load(incoming->checkpointKey().value()));
            check(progress.pause == dsfm::TransferPauseReason::AuthorizationWithdrawn && progress.verified_prefix == 0 &&
                  !progress.complete && !incoming->pendingId().isNull() && incoming->getBytesTransferred() == 0,
                  "Revoked preview advanced progress or failed to retain its partial");
            ::_exit(86);
        }
        if (preview_scenario) {
            until([&] { return incoming->getState() == File::FS_DONE && server->held; }, "Preview did not publish before lost receipt");
            state["published"] = incoming->storageId().toString(); save(root, state); engine->settings().sync();
            ::_exit(86);
        }
        if (offered) {
            if (scenario == "unsupported-waiting") outgoing->setState(File::FS_WAITING);
            ::_exit(86);
        }
        if (queued) {
            source_contact->disconnectFromContact(true);
            until([&] { return !target_contact->isOnline(); }, "Peer did not disconnect before offline approval");
            incoming->accept();
            check(incoming->getState() == File::FS_QUEUED, "Offline approval was not retained");
            const auto progress = QCoro::waitFor(storage.transferProgress().load(incoming->checkpointKey().value()));
            check(!progress && progress.error().code == dsfm::ErrorCode::MissingEntry, "Offline approval invented durable progress");
            if (scenario == "unsupported-queued") {
                // Keep the actual offline-approved queue in this process;
                // restart recovery otherwise correctly converts it to Paused.
                socket = ds::prot::ConnectionSocket::create();
                client = std::make_shared<Client>(socket, data); client->preference = 0;
                QObject::connect(client.get(), &PeerConnection::receivedUserInfo, client.get(),
                    [&](const PeerUserInfo &) { client->user_info = true; });
                QObject::connect(client.get(), &ds::prot::Peer::connectedToPeer, source_contact.get(), &Contact::onConnectedToPeer);
                socket->connectToHost(QHostAddress::LocalHost, listener.serverPort());
                until([&] { return client->user_info && server->user_info && !client->checkpointInterval() &&
                    !server->checkpointInterval(); }, "Non-capable reconnect did not become ready");
                until([&] { return incoming->getState() == File::FS_FAILED &&
                    incoming->failureReason() == int(dsfm::TransferFailureReason::UnsupportedVersion); },
                    "Offline-approved checkpoint stalled its queue on a non-capable reconnect");
                check(client->payload == 0 && client->legacy_payload == 0, "Queued checkpoint was silently downgraded");
            }
            ::_exit(86);
        }
        if (scenario == "publication-hint") {
            QObject::connect(incoming.get(), &File::stateChanged, incoming.get(), [&] {
                if (incoming->getState() != File::FS_HASHING) return;
                state["published"] = incoming->storageId().toString();
                source_contact->disconnectFromContact(true);
                until([&] { return !target_contact->isOnline() && outgoing->getState() == File::FS_PAUSED; },
                      "Publication reconnect did not retire the old connection");
                socket = ds::prot::ConnectionSocket::create();
                client = std::make_shared<Client>(socket, data); client->preference = preference;
                QObject::connect(client.get(), &PeerConnection::receivedUserInfo, client.get(),
                    [&](const PeerUserInfo &) { client->user_info = true; });
                QObject::connect(client.get(), &ds::prot::Peer::connectedToPeer, source_contact.get(), &Contact::onConnectedToPeer);
                socket->connectToHost(QHostAddress::LocalHost, listener.serverPort());
                // This nested fixture loop holds validateHash before commitIncoming
                // starts. The real reconnect offer must arrive during Hashing.
                until([&] { return client->user_info && server->user_info && server->hint_seen; },
                      "No recovery hint while publication was pending");
                check(incoming->getState() == File::FS_HASHING && !incoming->pendingId().isNull(),
                      "A hint changed the pending publication");
            });
        }
        if (scenario == "published" || scenario == "final-partial") {
            QObject::connect(incoming.get(), &File::stateChanged, incoming.get(), [&] {
                if (incoming->getState() != File::FS_HASHING) return;
                if (scenario == "final-partial") {
                    state["published"] = incoming->storageId().toString(); save(root, state);
                    ::_exit(86); // Final prefix is durable, but no publication has begun.
                }
                // Stop at the real cross-catalog boundary: publish with the
                // participant API, then kill before File commits its outcome.
                get(incoming->suspendIncoming());
                auto pending = get(storage.manager().openPending(incoming->storageNamespace(), incoming->pendingId()));
                get(pending.writer->commit());
                auto committed = get(storage.manager().stat(incoming->storageNamespace(), incoming->storageId()));
                check(committed.id == incoming->storageId(), "Publication changed transfer content identity");
                auto progress = get(storage.transferProgress().load(incoming->checkpointKey().value()));
                check(!progress.complete && !incoming->pendingId().isNull(), "Missed publication crash boundary");
                state["published"] = committed.id.toString(); save(root, state);
                ::_exit(86);
            });
        }
        incoming->accept();
        if (scenario == "publication-hint") {
            until([&] { return incoming->getState() == File::FS_DONE && outgoing->getState() == File::FS_DONE; },
                  "Completion hint was lost while publication finished after reconnect");
            check(client->payload == 0 && client->legacy_payload == 0 &&
                  incoming->storageId() == QUuid(state["published"].toString()) && incoming->pendingId().isNull(),
                  "Deferred recovery retransmitted or republished content");
            check(outgoing->checkpointAttempt() && outgoing->checkpointAttempt()->id != QUuid(state["attempt"].toString()),
                  "Deferred recovery reused the old attempt");
            save(root, state); ::_exit(86);
        } else if (partial) until([&] { return server->held; }, "No durable partial boundary");
        else if (eof_scenario) {
            until([&] { return incoming->getState() == File::FS_DONE && server->held; }, "No lost final receipt boundary");
            check(outgoing->getState() != File::FS_DONE, "Sender completed without final receipt");
            state["published"] = incoming->storageId().toString(); save(root, state);
        } else until([] { return false; }, "Publication crash boundary was not reached");
        ::_exit(86);
    }
    if (unsupported) {
        const auto failed = scenario == "unsupported-waiting" ? outgoing : incoming;
        const auto before = QCoro::waitFor(storage.transferProgress().load(failed->checkpointKey().value()));
        const auto pending_before = failed->pendingId();
        if (scenario == "unsupported-offered") incoming->accept();
        else if (scenario == "unsupported-partial") incoming->resume();
        until([&] { return failed->getState() == File::FS_FAILED &&
            failed->failureReason() == int(dsfm::TransferFailureReason::UnsupportedVersion); },
            "Non-capable connection did not reject the checkpoint transfer");
        const auto saved = get(storage.transferProgress().load(failed->checkpointKey().value()));
        check(saved.failure == dsfm::TransferFailureReason::UnsupportedVersion && !saved.complete &&
              failed->checkpointInterval() == dsfm::minimum_checkpoint_interval && client->payload == 0,
              "Unsupported transfer was downgraded or advanced");
        if (before) check(saved.verified_prefix == before.value().verified_prefix &&
            saved.confirmed_offset == before.value().confirmed_offset && saved.attempt == before.value().attempt,
            "Unsupported rejection changed retained checkpoint evidence");
        if (scenario == "unsupported-offered" || scenario == "unsupported-queued")
            check(saved.attempt.isNull() && saved.confirmed_offset == 0 && saved.verified_prefix == 0,
                  "Unstarted receiver rejection invented an attempt or progress");
        check(failed->pendingId() == pending_before, "Unsupported rejection replaced or deleted its partial");
        const auto reopened = File::load(*engine->getFileManager(), failed->getId());
        check(reopened->getState() == File::FS_FAILED &&
              reopened->failureReason() == int(dsfm::TransferFailureReason::UnsupportedVersion), "Unsupported failure was not persistent");
        if (scenario == "unsupported-waiting")
            check(incoming->getState() == File::FS_OFFERED, "Source rejection accepted the original");
        // Fresh ordinary attachments also require checkpoint support. They
        // abort locally without exposing an offer or falling back to legacy.
        const QByteArray ordinary_bytes("ordinary attachment after unsupported checkpoint");
        QFile source(root + "/ordinary.bin"); check(source.open(QIODevice::WriteOnly), "Cannot create ordinary fixture");
        check(source.write(ordinary_bytes) == ordinary_bytes.size(), "Cannot write ordinary fixture"); source.close();
        const auto imported = get(storage.importFile(source.fileName(), "ordinary.bin"));
        auto ordinary = std::make_unique<FileData>(); ordinary->state = File::FS_WAITING;
        ordinary->identity = sender->getId(); ordinary->contact = source_contact->getId();
        ordinary->conversation = engine->getConversationManager()->getConversation(source_contact.get())->getId();
        ordinary->storage_namespace = imported.namespace_id; ordinary->storage_id = imported.file.id;
        ordinary->name = "ordinary.bin"; ordinary->mime_type = "application/octet-stream";
        ordinary->size = ordinary_bytes.size(); ordinary->hash = imported.file.sha256;
        const auto sent = engine->getFileManager()->addFile(std::move(ordinary)); source_contact->queueFile(sent);
        until([&] { return sent->getState() == File::FS_FAILED &&
            sent->failureReason() == int(dsfm::TransferFailureReason::UnsupportedVersion); },
            "Fresh transfer did not abort on a peer without checkpoints");
        check(!engine->getFileManager()->getFileFromId(sent->getFileId(), *target_contact, File::INCOMING),
              "Unsupported fresh transfer exposed an offer");
        check(client->legacy_payload == 0 && client->payload == 0, "Unsupported connection emitted file payload");
        source_contact->disconnectFromContact(true); target_contact->disconnectFromContact(true);
        get(storage.shutdown()); return 0;
    }
    if (lost_offer) {
        until([&] { incoming = engine->getFileManager()->getFileFromId(outgoing->getFileId(), *target_contact, File::INCOMING); return bool(incoming); },
              "Restart did not repeat an undelivered initial offer");
        check(incoming->getState() == File::FS_OFFERED && incoming->storageId().isNull() && (preview_scenario || client->payload == 0),
              "Repeated offer silently accepted or allocated payload storage");
        if (preview_scenario) {
            state["primary_incoming"] = incoming->getId();
            outgoing = engine->getFileManager()->thumbnailFor(*outgoing);
            check(bool(outgoing), "Initial retry lost its signed preview relationship");
            state["attempt"] = state["preview_attempt"];
            until([&] { incoming = engine->getFileManager()->getFileFromId(outgoing->getFileId(), *target_contact, File::INCOMING); return bool(incoming); },
                  "Initial retry lost its signed preview offer");
        } else incoming->accept();
    } else if (scenario == "offer-acceptance-race") {
        until([&] { return acceptance_during_preparation; }, "No delayed reconnect offer preparation");
    } else if (scenario == "offered") {
        check(incoming->getState() == File::FS_OFFERED, "Restart silently accepted an offer");
        incoming->accept();
    } else if (partial || scenario == "final-partial" || scenario == "queued" || scenario == "preview-stale" || scenario == "preview-active") {
        if (scenario == "final-partial") {
            until([&] { return server->hint_seen; }, "No duplicate offer for the retained final prefix");
            check(incoming->getState() == File::FS_PAUSED && !incoming->pendingId().isNull(), "Hint automatically resumed an unpublished partial");
        }
        until([&] {
            if (incoming->getState() == File::FS_PAUSED) incoming->resume();
            return incoming->getState() != File::FS_PAUSED;
        }, "Unpublished partial could not be manually resumed");
        if (scenario == "readiness-retry") {
            until([&] { return incoming->getState() == File::FS_FAILED && server->readiness_failures == 0; },
                  "Readiness transport failure was not exercised");
            until([&] {
                if (incoming->getState() == File::FS_FAILED) incoming->resume();
                return incoming->getState() != File::FS_FAILED;
            }, "Failed readiness retained its request or reservation and blocked retry");
        }
    }
    try {
        until([&] { return outgoing->getState() == File::FS_DONE && incoming->getState() == File::FS_DONE; }, "Restart did not complete the same transfer");
    } catch (...) {
        std::cerr << "Recovery states: source=" << outgoing->getState() << ", receiver=" << incoming->getState()
                  << ", pending=" << !incoming->pendingId().isNull() << ", online=" << source_contact->isOnline() << ',' << target_contact->isOnline() << '\n';
        throw;
    }
    check(outgoing->getFileId() == incoming->getFileId(), "Recovery changed the transfer identity");
    if (scenario != "publication-hint")
        check(outgoing->checkpointAttempt() && outgoing->checkpointAttempt()->id != QUuid(state["attempt"].toString()),
              "Recovery did not replace the lost attempt");
    if (eof_scenario || scenario == "published" || scenario == "final-partial" || scenario == "publication-hint" || (preview_scenario && !lost_offer && scenario != "preview-stale" && scenario != "preview-active")) {
        check(client->payload == 0, "Published content was retransmitted");
        check(incoming->storageId() == QUuid(state["published"].toString()), "Recovery republished content");
    }
    if (partial) check(client->payload == 7, "Restart discarded its durable resume prefix");
    if (scenario == "queued") check(client->payload == quint64(bytes.size()), "Queued approval did not start a fresh stream");
    if (changed_preference) {
        check(client->checkpointInterval() == dsfm::minimum_checkpoint_interval, "New preference was not negotiated");
        check(outgoing->checkpointInterval() == dsfm::default_checkpoint_interval &&
              incoming->checkpointInterval() == dsfm::default_checkpoint_interval, "Recovery changed the frozen interval");
    }
    check(incoming->pendingId().isNull(), "Recovery retained a pending writer");
    check(get(storage.transferProgress().load(outgoing->checkpointKey().value())).complete, "Missing sender durable outcome");
    check(get(storage.transferProgress().load(incoming->checkpointKey().value())).complete, "Missing receiver durable outcome");
    if (preview_scenario) {
        auto primary = engine->getFileManager()->getFile(state["primary_incoming"].toInt());
        check(primary->getState() == File::FS_OFFERED && primary->storageId().isNull() && primary->pendingId().isNull(), "Preview recovery accepted its original");
        bytes = preview_bytes;
    }
    auto reader = get(storage.manager().openRead(incoming->storageNamespace(), incoming->storageId()));
    for (qsizetype offset = 0; offset < bytes.size(); offset += 65536) {
        const auto count = std::min<qsizetype>(65536, bytes.size() - offset);
        check(get(reader->readAt(offset, count)) == bytes.mid(offset, count), "Recovered content differs");
    }
    get(reader->close());
    if (changed_preference) {
        // The stored interval exception cannot authorize unfamiliar or changed
        // content, even for an authenticated, approved peer.
        int dispatched = 0;
        QObject observations;
        QObject::connect(server.get(), &PeerConnection::receivedFileOffer, &observations,
                         [&](const PeerFileOffer &) { ++dispatched; });
        ds::protocol::FileOffer forged;
        forged.setFileId(preview_scenario ? incoming->getFileId() : ds::crypto::Crypto::getRandomBytes(16));
        forged.setConversationId(incoming->getConversation()->getHash());
        forged.setName(incoming->getName());
        forged.setSize(quint64(incoming->getSize()));
        forged.setSha256(preview_scenario ? QByteArray(32, 'x') : incoming->getHash());
        forged.setMimeType(incoming->getMimeType());
        ds::protocol::CheckpointParameters parameters;
        parameters.setVersion(1);
        parameters.setIntervalBytes(dsfm::default_checkpoint_interval);
        parameters.setAttemptId(QUuid::createUuid().toRfc4122());
        parameters.setNextCheckpointSequence(1);
        forged.setCheckpoints(parameters);
        ds::protocol::Envelope envelope;
        envelope.setProtocolVersion(ds::prot::codec::protocolVersion); envelope.setFileOffer(forged);
        client->send(envelope);
        until([&] { return !server->isConnected(); }, "Oversized interval for unknown or changed content was accepted");
        check(dispatched == 0 && incoming->getState() == File::FS_DONE,
              "Invalid interval exception reached application dispatch or changed completion");
    }
    source_contact->disconnectFromContact(true); target_contact->disconnectFromContact(true);
    get(storage.shutdown());
    return 0;
    } catch (const std::exception &error) {
        // Preserve the original diagnostic before destroying an engine with
        // intentionally interrupted participants and queued network callbacks.
        std::cerr << error.what() << '\n'; std::cerr.flush(); ::_exit(1);
    }
}
}
int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    if (qEnvironmentVariableIsSet("DS_TEST_TRACE"))
        logfault::LogManager::Instance().AddHandler(std::make_unique<logfault::StreamHandler>(std::clog, logfault::LogLevel::TRACE));
    try {
        const auto args = app.arguments();
        if (args.size() == 5 && args[1] == "--child") return child(args[2], args[3], args[4] == "recover");
        for (const auto &scenario : {"offered", "queued", "partial", "eof", "published", "final-partial", "preview", "preview-stale", "eof-preference", "preview-preference", "preview-policy", "preview-active", "unsupported-offered", "unsupported-queued", "unsupported-partial", "unsupported-waiting", "readiness-retry", "publication-hint", "lost-offer", "lost-offer-preview", "lost-offer-stale", "offer-acceptance-race", "lost-offer-failed", "lost-offer-preparing"}) {
            QTemporaryDir root; check(root.isValid(), "Cannot create private restart profile");
            for (const auto &stage : {"prepare", "recover"}) {
                QProcess process;
                process.setProcessChannelMode(QProcess::MergedChannels);
                process.start(QCoreApplication::applicationFilePath(), {"--child", root.path(), scenario, stage});
                check(process.waitForStarted(5000), "Cannot start restart child");
                if (!process.waitForFinished(30000)) { process.kill(); process.waitForFinished(5000); throw std::runtime_error("Restart child timed out"); }
                const int expected = QString(stage) == "prepare" ? 86 : 0;
                if (process.exitCode() != expected) {
                    std::cerr << scenario << '/' << stage << ": " << process.readAll().toStdString();
                    throw std::runtime_error("Restart child failed");
                }
            }
        }
        std::cout << "Checkpoint process restart cases passed\n";
        return 0;
    } catch (const std::exception &error) { std::cerr << error.what() << '\n'; return 1; }
}
