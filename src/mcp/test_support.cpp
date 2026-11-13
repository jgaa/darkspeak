#include "test_support.h"
#include "ds/avatarhash.h"
#include "ds/dsengine.h"
#include "logfault/logfault.h"
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QFileInfo>
#include <QJsonArray>
#include <QRegularExpression>
#include <QSqlQuery>
#include <QTimer>
#include <QThread>
#include <sodium.h>

namespace ds::mcp {
using namespace ds::core;
namespace {
std::mutex diagnostic_mutex;
QJsonArray diagnostic_events;
class DiagnosticHandler final : public logfault::Handler {
  public:
    DiagnosticHandler() : Handler(logfault::LogLevel::WARN) {}
    void LogMessage(const logfault::Message& message) noexcept override {
        try {
            QJsonObject record{{"type", "application.diagnostic"}, {"level", int(message.level_)}};
            // Only this exact locally defined numeric message has retained data.
            // No other log message text is copied into the diagnostic projection.
            const auto text = QString::fromStdString(message.msg_);
            static const QRegularExpression import_error(
                "^Outgoing file import failed \\(storage code=([0-9]{1,2})\\)$");
            if (const auto match = import_error.match(text); match.hasMatch()) {
                record["type"] = "file.import_failed";
                record["code"] = match.captured(1).toInt();
            }
            if (text.startsWith("Protocol error on connection ")) {
                if (text.endsWith("File chunk has no active transfer") ||
                    text.endsWith("File chunk has no active channel"))
                    record["type"] = "protocol.file_channel_not_ready";
            }
            std::lock_guard lock(diagnostic_mutex);
            if (diagnostic_events.size() == 100)
                diagnostic_events.removeFirst();
            diagnostic_events.append(record);
        } catch (...) {
        }
    }
};
QByteArray verifier(const QByteArray& value) {
    return QCryptographicHash::hash(value, QCryptographicHash::Sha256);
}
bool validTransfer(const QString& value) {
    static const QRegularExpression expression(QStringLiteral("^[0-9a-f]{32}$"));
    return expression.match(value).hasMatch();
}
QJsonObject avatar(const QImage& image) {
    return {{"avatar_hash", QString::fromLatin1(avatarHash(image).toHex())},
            {"avatar_width", image.width()},
            {"avatar_height", image.height()}};
}
} // namespace
void installTestDiagnostics() {
    logfault::LogManager::Instance().AddHandler(std::make_unique<DiagnosticHandler>());
}
QJsonArray testDiagnostics() {
    std::lock_guard lock(diagnostic_mutex);
    return diagnostic_events;
}
TestSupport::TestSupport(DsEngine& engine, Service& service, QString input_root, QByteArray credential)
    : QObject(&service), engine_(engine), service_(service), input_root_(std::move(input_root)),
      verifier_(verifier(credential)) {
    sodium_memzero(credential.data(), credential.size());
    service_.setTestSupport(this);
    for (int row = 0; row < engine_.getIdentityManager()->rowCount({}); ++row)
        bio_wire_.insert(engine_.getIdentityManager()->identityFromRow(row)->getUuid(), {});
    connect(&engine_, &DsEngine::testBioWireQueued, this,
            [this](const PeerConnection* peer, bool response, bool content, bool hash) {
                if (QThread::currentThread() != engine_.thread())
                    qFatal("Test wire observer called outside application thread");
                auto stats = bio_wire_.find(peer->getIdentityId());
                auto* owner = engine_.getIdentityManager()->identityFromUuid(peer->getIdentityId());
                if (stats == bio_wire_.end() || !owner) return;
                const auto increment = [](int& value) { if (value < 1000000) ++value; };
                if (response) increment(stats->responses);
                if (content) increment(stats->content_frames);
                if (hash) increment(stats->hash_frames);
                if (owner->getPublicBio() || (!content && !hash)) return;
                bool approved = false;
                if (const auto cert = peer->getPeerCert()) {
                    QSqlQuery query(engine_.getDb());
                    query.prepare("SELECT state FROM contact WHERE identity=? AND hash=?");
                    query.addBindValue(owner->getId());
                    query.addBindValue(cert->getHash().toByteArray());
                    approved = query.exec() && query.next() && query.value(0).toInt() == Contact::ACCEPTED;
                }
                if (!approved) increment(stats->forbidden_frames);
            }, Qt::DirectConnection);
    connect(engine_.getFileManager(), &FileManager::fileStateChanged, this,
            [this](const File* file) {
                // Observe the normal negotiated resume transition. Never alter it.
                if (file->getState() == File::FS_QUEUED && file->getBytesTransferred() > 0 &&
                    resumed_offsets_.size() < 64)
                    resumed_offsets_.insert(QString::fromLatin1(file->getFileId().toHex()),
                                            file->getBytesTransferred());
            });
    // Explicit isolated controller is the test operator. Decisions still use
    // Service::decide, with the full production authorization checks.
    service_.setUiAvailable(true);
    connect(engine_.getContactManager(), &ContactManager::contactDeleted, this,
            [this] { preview_files_.clear(); });
    connect(engine_.getFileManager(), &FileManager::fileDeleted, this,
            [this](int id) { preview_files_.remove(id); });
    engine_.setProperty("mcpTestStorageReady", false);
    engine_.getFileManager()->storage().ready().then(
        [engine = &engine_](auto result) { engine->setProperty("mcpTestStorageReady", bool(result)); });
}
TestSupport::~TestSupport() {
    service_.setTestSupport(nullptr);
    service_.setUiAvailable(false);
}
bool TestSupport::isOperation(const QString& name) {
    return QStringList{"bio_probe", "bio_preview", "file_offer", "file_accept", "file_reject", "file_cancel_incoming", "file_cancel_outgoing", "file_resume", "files"}.contains(name);
}
QList<Tool> TestSupport::controllerTools() {
    return {
        {"controller_state", {}, {}, {}, {}},
        {"controller_pending", {}, {}, {}, {}},
        {"controller_decide", {}, {}, {"operation", "decision", "scope"}, {"operation", "decision", "scope"}},
        {"controller_online", {}, {}, {}, {}},
        {"controller_hash", {}, {}, {"transfer"}, {"transfer"}},
        {"controller_storage", {}, {}, {"transfer"}, {"transfer"}},
        {"controller_shutdown", {}, {}, {}, {}}};
}
QString TestSupport::authenticate(const std::string& bearer) const {
    if (bearer.size() > 256 || !bearer.starts_with("Bearer "))
        return {};
    const auto supplied = verifier(QByteArray::fromStdString(bearer.substr(7)));
    return sodium_memcmp(supplied.constData(), verifier_.constData(), verifier_.size()) == 0
               ? QStringLiteral("isolated-controller")
               : QString{};
}
void TestSupport::validate(const QString& name, const QJsonObject& args) const {
    if (name == "bio_preview" &&
        probes_.value(args.value("identity").toString()) != args.value("sharing_id").toString())
        throw Error("ProbeChanged");
    if (name == "file_offer" &&
        !QStringList{"earth.jpg", "apollo11.jpg", "sos.ogg", "resume.jpg", "blue.mkv"}.contains(args.value("asset").toString()))
        throw Error("InvalidAsset");
    if (args.contains("transfer") && !validTransfer(args.value("transfer").toString()))
        throw Error("InvalidTransfer");
}
QJsonObject TestSupport::execute(const QString& name, Identity* owner, const QJsonObject& args) {
    validate(name, args);
    if (name == "bio_probe") {
        if (!owner->isOnline())
            throw Error("PeerNotReady");
        owner->probeContact(args.value("sharing_id").toString());
        probes_[owner->getUuid().toString(QUuid::WithoutBraces)] = args.value("sharing_id").toString();
        return {{"started", true}};
    }
    if (name == "bio_preview")
        return {{"bio", owner->getProbeBio()}, {"has_avatar", owner->hasProbeAvatar()}};
    auto contact = engine_.getContactManager()->getContact(QUuid(args.value("contact").toString()));
    if (!contact || contact->getIdentity() != owner)
        throw Error("AccessDenied");
    if (name == "file_offer") {
        if (contact->getState() != Contact::ACCEPTED || !contact->canSendFiles())
            throw Error("PeerPolicyDenied");
        const auto asset = args.value("asset").toString();
        const QFileInfo input(QDir(input_root_).filePath(asset));
        if (!input.isFile() || input.isSymLink() || input.size() <= 0 || input.size() > 32 * 1024 * 1024 ||
            input.canonicalPath() != input_root_)
            throw Error("UnsafeInput");
        auto conversation = engine_.getConversationManager()->getConversation(contact.get());
        auto data = std::make_unique<FileData>();
        data->name = asset;
        data->source_path = input.canonicalFilePath();
        data->identity = owner->getId();
        data->contact = contact->getId();
        data->conversation = conversation->getId();
        // This is the same service used by Conversation::sendFile; import,
        // content sniffing, limits and peer preferences remain authoritative.
        const auto file = engine_.getFileManager()->addFile(std::move(data));
        return {{"reference", QString::fromLatin1(file->getFileId().toHex())}};
    }
    if (name == "file_reject" || name == "file_cancel_incoming" || name == "file_cancel_outgoing") {
        const auto file = engine_.getFileManager()->getFileFromId(
            QByteArray::fromHex(args.value("transfer").toString().toLatin1()), *contact);
        const auto direction = name == "file_cancel_outgoing" ? File::OUTGOING : File::INCOMING;
        if (!file || file->getDirection() != direction)
            throw Error("InvalidTransferState");
        const auto state = file->getState();
        if (name == "file_reject") {
            if (state != File::FS_OFFERED) throw Error("InvalidTransferState");
            file->reject();
        } else {
            if (state != File::FS_WAITING && state != File::FS_OFFERED &&
                state != File::FS_QUEUED && state != File::FS_TRANSFERRING)
                throw Error("InvalidTransferState");
            file->cancel();
        }
        return {{"reference", args.value("transfer")}};
    }
    if (name == "file_resume") {
        const auto file = engine_.getFileManager()->getFileFromId(
            QByteArray::fromHex(args.value("transfer").toString().toLatin1()), *contact);
        if (!file || file->getDirection() != File::INCOMING || file->getState() != File::FS_FAILED)
            throw Error("InvalidTransferState");
        file->resume();
        return {{"reference", args.value("transfer")}};
    }
    if (name == "file_accept") {
        const auto file = engine_.getFileManager()->getFileFromId(
            QByteArray::fromHex(args.value("transfer").toString().toLatin1()), *contact);
        if (!file || file->getDirection() != File::INCOMING || file->getState() != File::FS_OFFERED)
            throw Error("InvalidTransferState");
        QString reason;
        if (!engine_.getFileManager()->canAcceptIncoming(*file, reason))
            throw Error("PeerPolicyDenied");
        file->accept();
        return {{"reference", args.value("transfer")}};
    }
    if (name == "files") {
        QSqlQuery q(engine_.getDb());
        q.prepare("SELECT id FROM file WHERE identity_id=? AND contact_id=? ORDER "
                  "BY id LIMIT 65");
        q.addBindValue(owner->getId());
        q.addBindValue(contact->getId());
        if (!q.exec())
            throw Error("PersistenceFailed");
        QJsonArray result;
        while (q.next()) {
            if (result.size() == 64)
                throw Error("ResultLimit");
            const auto f = previewFile(q.value(0).toInt());
            result.append(QJsonObject{{"transfer", QString::fromLatin1(f->getFileId().toHex())},
                                      {"name", f->getName()},
                                      {"size", f->getSize()},
                                      {"direction", int(f->getDirection())},
                                      {"state", int(f->getState())},
                                      {"mime", f->getMimeType()},
                                      {"thumbnail_for", QString::fromLatin1(f->dataThumbnailFor().toHex())},
                                      {"preview_available", !f->previewSource().isEmpty()},
                                      {"bytes", f->getBytesTransferred()}});
        }
        return {{"items", result}};
    }
    throw Error("UnknownTool");
}
void TestSupport::submit(const QString& name, const QJsonObject& args, std::shared_ptr<Reply> reply) {
    try {
        const auto catalog = controllerTools();
        const auto found =
            std::find_if(catalog.begin(), catalog.end(), [&](const auto& t) { return t.name == name; });
        if (found == catalog.end())
            throw Error("UnknownTool");
        for (auto it = args.begin(); it != args.end(); ++it)
            if (!found->fields.contains(it.key()) || !it->isString() || it->toString().size() > 128)
                throw Error("InvalidArguments");
        for (const auto& field : found->required)
            if (!args.contains(field))
                throw Error("InvalidArguments");
        if (name == "controller_pending") {
            reply->finish({{"items", QJsonArray::fromVariantList(service_.pending())}});
            return;
        }
        if (name == "controller_decide") {
            const auto result =
                service_.decide(args.value("operation").toString(), args.value("decision").toString(),
                                args.value("scope").toString());
            if (!result)
                throw Error("DecisionRejected");
            reply->finish({{"decided", true}});
            return;
        }
        if (name == "controller_online") {
            if (engine_.getState() == DsEngine::INITIALIZING)
                engine_.start();
            reply->finish({{"started", true}});
            return;
        }
        if (name == "controller_shutdown") {
            reply->finish({{"stopping", true}});
            QTimer::singleShot(100, &engine_, [this] {
                engine_.close();
                engine_.getFileManager()->storage().shutdown().then([](auto) { QCoreApplication::quit(); });
            });
            return;
        }
        if (name == "controller_storage") {
            const auto transfer = args.value("transfer").toString();
            if (!validTransfer(transfer)) throw Error("InvalidTransfer");
            if (inspecting_) throw Error("Overloaded");
            inspecting_ = true;
            inspectStorage(transfer, reply);
            return;
        }
        if (name == "controller_hash") {
            const auto transfer = args.value("transfer").toString();
            if (!validTransfer(transfer))
                throw Error("InvalidTransfer");
            if (hashing_)
                throw Error("Overloaded");
            hashing_ = true;
            hashFile(transfer, reply);
            return;
        }
        QJsonArray identities, contacts, transfers;
        auto* manager = engine_.getIdentityManager();
        for (int index = 0; index < std::min(64, manager->rowCount({})); ++index) {
            auto* owner = manager->identityFromRow(index);
            auto data = avatar(owner->getAvatar());
            data["identity"] = owner->getUuid().toString(QUuid::WithoutBraces);
            data["public_key"] =
                QString::fromLatin1(owner->getCert()->getSigningPubKey().toByteArray().toHex());
            data["online"] = owner->isOnline();
            const auto wire = bio_wire_.value(owner->getUuid());
            data["bio_wire"] = QJsonObject{{"responses", wire.responses},
                {"content_frames", wire.content_frames}, {"hash_frames", wire.hash_frames},
                {"forbidden_frames", wire.forbidden_frames}};
            data["handle"] = QString::fromLatin1(owner->getCert()->getB58PubKey());
            data["sharing_id"] = QString::fromLatin1(owner->getB58EncodedIdetity());
            identities.append(data);
        }
        QSqlQuery q("SELECT uuid FROM contact ORDER BY id LIMIT 64", engine_.getDb());
        while (q.next()) {
            auto contact = engine_.getContactManager()->getContact(q.value(0).toUuid());
            auto data = avatar(contact->getAvatar());
            data["contact"] = contact->getUuid().toString(QUuid::WithoutBraces);
            data["identity"] = contact->getIdentity()->getUuid().toString(QUuid::WithoutBraces);
            data["state"] = int(contact->getState());
            data["online"] = contact->isOnline();
            contacts.append(data);
        }
        QSqlQuery files("SELECT file_id,state,direction,bytes_transferred,size,identity_id,contact_id,id FROM file ORDER BY id LIMIT 64", engine_.getDb());
        while (files.next()) {
            const auto file = previewFile(files.value(7).toInt());
            transfers.append(
                QJsonObject{{"transfer", QString::fromLatin1(files.value(0).toByteArray().toHex())},
                            {"state", files.value(1).toInt()},
                            {"direction", files.value(2).toInt()},
                            {"thumbnail_for", QString::fromLatin1(file->dataThumbnailFor().toHex())},
                            {"preview_available", !file->previewSource().isEmpty()},
                            {"bytes", files.value(3).toDouble()}, {"size", files.value(4).toDouble()},
                            {"resumed_from", resumed_offsets_.value(QString::fromLatin1(files.value(0).toByteArray().toHex()))},
                            {"identity", engine_.getIdentityManager()->identityFromId(files.value(5).toInt())->getUuid().toString(QUuid::WithoutBraces)},
                            {"contact", engine_.getContactManager()->getContact(files.value(6).toInt())->getUuid().toString(QUuid::WithoutBraces)}});
        }
        reply->finish({{"application_ready", engine_.property("mcpTestStorageReady").toBool()},
                       {"online", engine_.isOnline()},
                       {"transfers", transfers},
                       {"diagnostics", testDiagnostics()},
                       {"identities", identities},
                       {"contacts", contacts},
                       {"pending", service_.pending().size()},
                       {"journal_enabled", engine_.events().enabled()}});
    } catch (const Error& error) {
        const auto code = QString::fromUtf8(error.what());
        reply->finish({}, QStringList{"UnknownTool", "InvalidArguments", "InvalidTransfer", "Overloaded",
                                      "DecisionRejected"}
                                  .contains(code)
                              ? code
                              : QStringLiteral("TestOperationFailed"));
    } catch (...) {
        reply->finish({}, "TestOperationFailed");
    }
}
std::shared_ptr<File> TestSupport::previewFile(int id) {
    if (const auto found = preview_files_.find(id); found != preview_files_.end()) return found.value();
    auto file = engine_.getFileManager()->getFile(id);
    if (preview_files_.size() >= 64) preview_files_.erase(preview_files_.begin());
    preview_files_.insert(id, file);
    return file;
}

QCoro::Task<void> TestSupport::inspectStorage(QString transfer, std::shared_ptr<Reply> reply) {
    auto operation = engine_.getFileManager()->storage().holdOperation();
    try {
        const auto file = engine_.getFileManager()->getFileFromId(
            QByteArray::fromHex(transfer.toLatin1()), File::INCOMING);
        if (!file) throw Error("InvalidTransferState");
        bool published = false, pending = false;
        if (!file->storageId().isNull()) {
            auto info = co_await engine_.getFileManager()->storage().manager().stat(
                file->storageNamespace(), file->storageId());
            if (info) published = true;
            else if (info.error().code != dsfm::ErrorCode::MissingEntry) throw Error("StorageInspectionFailed");
            auto entries = co_await engine_.getFileManager()->storage().manager().listPending(
                file->storageNamespace(), dsfm::PageRequest{64, {}});
            if (!entries || !entries.value().next_cursor.isEmpty() || entries.value().entries.size() > 64)
                throw Error("StorageInspectionFailed");
            for (const auto& item : entries.value().entries)
                if (item.file_id == file->storageId()) pending = true;
        }
        reply->finish({{"published", published}, {"pending", pending}, {"state", int(file->getState())},
                       {"bytes", file->getBytesTransferred()}});
    } catch (...) {
        reply->finish({}, "StorageInspectionFailed");
    }
    inspecting_ = false;
}
QCoro::Task<void> TestSupport::hashFile(QString transfer, std::shared_ptr<Reply> reply) {
    auto operation = engine_.getFileManager()->storage().holdOperation();
    try {
        const auto file =
            engine_.getFileManager()->getFileFromId(QByteArray::fromHex(transfer.toLatin1()), File::INCOMING);
        if (!file || file->getState() != File::FS_DONE || file->getSize() > 32 * 1024 * 1024)
            throw Error("InvalidTransferState");
        auto opened = co_await engine_.getFileManager()->storage().manager().openRead(
            file->storageNamespace(), file->storageId());
        if (!opened)
            throw Error("ReadFailed");
        auto reader = std::move(opened.value());
        QCryptographicHash hash(QCryptographicHash::Sha256);
        quint64 offset = 0;
        while (offset < quint64(file->getSize())) {
            auto bytes = co_await reader->readAt(
                offset, qsizetype(std::min<quint64>(65536, file->getSize() - offset)));
            if (!bytes || bytes.value().isEmpty())
                throw Error("ReadFailed");
            offset += bytes.value().size();
            hash.addData(bytes.value());
        }
        co_await reader->close();
        reply->finish({{"sha256", QString::fromLatin1(hash.result().toHex())}, {"size", qint64(offset)}});
    } catch (...) {
        reply->finish({}, "FileVerificationFailed");
    }
    hashing_ = false;
}
} // namespace ds::mcp
