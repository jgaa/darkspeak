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
    // Explicit isolated controller is the test operator. Decisions still use
    // Service::decide, with the full production authorization checks.
    service_.setUiAvailable(true);
    engine_.setProperty("mcpTestStorageReady", false);
    engine_.getFileManager()->storage().ready().then(
        [engine = &engine_](auto result) { engine->setProperty("mcpTestStorageReady", bool(result)); });
}
TestSupport::~TestSupport() {
    service_.setTestSupport(nullptr);
    service_.setUiAvailable(false);
}
bool TestSupport::isOperation(const QString& name) {
    return QStringList{"bio_probe", "bio_preview", "file_offer", "file_accept", "files"}.contains(name);
}
QList<Tool> TestSupport::controllerTools() {
    return {
        {"controller_state", {}, {}, {}, {}},
        {"controller_pending", {}, {}, {}, {}},
        {"controller_decide", {}, {}, {"operation", "decision", "scope"}, {"operation", "decision", "scope"}},
        {"controller_online", {}, {}, {}, {}},
        {"controller_hash", {}, {}, {"transfer"}, {"transfer"}},
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
        !QStringList{"earth.jpg", "apollo11.jpg", "sos.ogg"}.contains(args.value("asset").toString()))
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
        return {{"bio", owner->getProbeBio()}};
    auto contact = engine_.getContactManager()->getContact(QUuid(args.value("contact").toString()));
    if (!contact || contact->getIdentity() != owner)
        throw Error("AccessDenied");
    if (name == "file_offer") {
        if (contact->getState() != Contact::ACCEPTED || !contact->canSendFiles())
            throw Error("PeerPolicyDenied");
        const auto asset = args.value("asset").toString();
        const QFileInfo input(QDir(input_root_).filePath(asset));
        if (!input.isFile() || input.isSymLink() || input.size() <= 0 || input.size() > 1024 * 1024 ||
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
            const auto f = engine_.getFileManager()->getFile(q.value(0).toInt());
            result.append(QJsonObject{{"transfer", QString::fromLatin1(f->getFileId().toHex())},
                                      {"name", f->getName()},
                                      {"size", f->getSize()},
                                      {"direction", int(f->getDirection())},
                                      {"state", int(f->getState())},
                                      {"mime", f->getMimeType()},
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
            data["handle"] = QString::fromLatin1(owner->getCert()->getB58PubKey());
            data["sharing_id"] = QString::fromLatin1(owner->getB58EncodedIdetity());
            identities.append(data);
        }
        QSqlQuery q("SELECT uuid FROM contact ORDER BY id LIMIT 64", engine_.getDb());
        while (q.next()) {
            auto contact = engine_.getContactManager()->getContact(q.value(0).toUuid());
            auto data = avatar(contact->getAvatar());
            data["contact"] = contact->getUuid().toString(QUuid::WithoutBraces);
            data["state"] = int(contact->getState());
            data["online"] = contact->isOnline();
            contacts.append(data);
        }
        QSqlQuery files("SELECT file_id,state,direction FROM file ORDER BY id LIMIT 64", engine_.getDb());
        while (files.next())
            transfers.append(
                QJsonObject{{"transfer", QString::fromLatin1(files.value(0).toByteArray().toHex())},
                            {"state", files.value(1).toInt()},
                            {"direction", files.value(2).toInt()}});
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
QCoro::Task<void> TestSupport::hashFile(QString transfer, std::shared_ptr<Reply> reply) {
    auto operation = engine_.getFileManager()->storage().holdOperation();
    try {
        const auto file =
            engine_.getFileManager()->getFileFromId(QByteArray::fromHex(transfer.toLatin1()), File::INCOMING);
        if (!file || file->getState() != File::FS_DONE || file->getSize() > 1024 * 1024)
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
