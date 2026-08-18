#include "ds/conversation.h"
#include "ds/errors.h"
#include "ds/update_helper.h"
#include "ds/dsengine.h"
#include "ds/crypto.h"
#include "ds/identity.h"

#include "logfault/logfault.h"

#include <QStandardPaths>
#include <QRegularExpression>
#include <QUrl>
#include <algorithm>

namespace ds {
namespace core {

using namespace std;
using namespace crypto;

Conversation::Conversation(QObject &parent)
    : QObject{&parent}
{

}

Conversation::Conversation(QObject &parent, QString name, QString topic, Contact *participant)
    : QObject{&parent}, identity_{participant->getIdentity()->getId()}
    , name_{move(name)}, topic_{move(topic)}, hash_{calculateHash(*participant)}
{
    participants_.insert(participant->getUuid());
}

void Conversation::incrementUnread()
{
    try {
        setUnread(getUnread() + 1);
    } catch (const std::exception& ex) {
        LFLOG_ERROR << "Failed to update unread count: " << ex.what();
        DsEngine::instance().reportStatus(
            tr("Failed to update unread count: %1").arg(QString::fromUtf8(ex.what())),
            DsEngine::StatusLevel::ERROR);
    } catch (...) {
        LFLOG_ERROR << "Failed to update unread count: unknown exception";
    }
}

void Conversation::sendMessage(const QString &text)
{
    try {
        LFLOG_DEBUG << "Sending message text payload of " << text.size() << " characters";

        const auto identity = getIdentity();
        assert(identity);

        MessageData data;
        data.conversation = hash_;
        data.composedTime = DsEngine::getSafeNow();
        data.content = text;
        data.encoding = Message::Encoding::UTF8;
        data.sender = identity->getCert()->getSigningPubKey().toByteArray();
        const auto random = crypto::Crypto::getRandomBytes(32);
        data.messageId = crypto::Crypto::getSha256(
            std::string_view{"DarkSpeak message id v3"}, data.sender, random);

        DsEngine::instance().getMessageManager()->sendMessage(*this, data);
        touchLastActivity();
    } catch (const std::exception& ex) {
        LFLOG_ERROR << "Failed to send message: " << ex.what();
        DsEngine::instance().reportStatus(
            tr("Failed to send message: %1").arg(QString::fromUtf8(ex.what())),
            DsEngine::StatusLevel::ERROR);
    } catch (...) {
        LFLOG_ERROR << "Failed to send message: unknown exception";
    }
}

void Conversation::sendFile(const QVariantMap &args)
{
    try {

        const auto path = args.value("path").toString();
    if (path.isEmpty()) {
        LFLOG_DEBUG << "Ignoring request to send file with no path...";
        return;
    }

    auto data = make_unique<FileData>();
    data->name = args.value("name").toString();

    // Accept both native paths from QFileDialog and file URLs from QML.
    const auto url = QUrl::fromUserInput(path);
    data->path = url.isLocalFile() ? url.toLocalFile() : path;

    if (data->path.isEmpty()) {
        DsEngine::instance().reportStatus(
            tr("Failed to send file: invalid path"),
            DsEngine::StatusLevel::ERROR);
        return;
    }

    if (data->name.isEmpty()) {
        QFileInfo fi{data->path};
        data->name = fi.fileName();
    }

    data->conversation = getId();
    data->contact = getFirstParticipant()->getId();
    data->identity = getIdentityId();

    DsEngine::instance().getFileManager()->addFile(move(data));
        touchLastActivity();
    } catch (const std::exception& ex) {
        LFLOG_ERROR << "Failed to send file: " << ex.what();
        DsEngine::instance().reportStatus(
            tr("Failed to send file: %1").arg(QString::fromUtf8(ex.what())),
            DsEngine::StatusLevel::ERROR);
    } catch (...) {
        LFLOG_ERROR << "Failed to send file: unknown exception";
    }
}

void Conversation::incomingMessage(Contact *contact, const MessageData &data)
{
    // Add to the database
    if (!DsEngine::instance().getMessageManager()->receivedMessage(*this, data)) {
        return;
    }

    if (data.messageId.size() == 32) {
        contact->sendMessageReceipt(data.messageId);
    }

    incrementUnread();
    touchLastActivity();
}

void Conversation::incomingFileOffer(Contact *contact, const PeerFileOffer &offer)
{
    Q_UNUSED(contact)
    // Add to the database
    // The file manager will deal with the ack
    DsEngine::instance().getFileManager()->receivedFileOffer(*this, offer);

    touchLastActivity();
}

int Conversation::getId() const noexcept {
    return id_;
}

QString Conversation::getName() const noexcept  {
    return name_;
}

QString Conversation::getContactName() const
{
    if (const auto participant = getFirstParticipant()) {
        if (!participant->getName().isEmpty()) {
            return participant->getName();
        }
        return participant->getNickName();
    }
    return {};
}

void Conversation::setName(const QString &name) {
    updateIf("name", name, name_, this, &Conversation::nameChanged);
}

QUuid Conversation::getUuid() const noexcept
{
    return uuid_;
}

Conversation::participants_t Conversation::getParticipants() const
{
    // Lazy load
    if (real_perticipants_.empty()) {
        real_perticipants_.reserve(participants_.size());

        for(const auto& uuid  : participants_) {
            real_perticipants_.emplace_back(DsEngine::instance().getContactManager()->getContact(uuid));
        }
    }

    return real_perticipants_;
}

Contact *Conversation::getFirstParticipant() const
{
    return getParticipant().get();
}

QString Conversation::getTopic() const noexcept  {
    return name_;
}

void Conversation::setTopic(const QString &topic) {
    updateIf("topic", topic, topic_, this, &Conversation::nameChanged);
}

Conversation::Type Conversation::getType() const noexcept
{
    return type_;
}

QByteArray Conversation::getHash() const noexcept
{
    return hash_;
}

QDateTime Conversation::getCreated() const noexcept {
    return created_;
}

QDateTime Conversation::getLastActivity() const noexcept
{
    return lastActivity_;
}

void Conversation::setLastActivity(const QDateTime &when)
{
    updateIf("updated", when, lastActivity_, this, &Conversation::lastActivityChanged);
}

void Conversation::touchLastActivity()
{
    setLastActivity(DsEngine::getSafeNow());
}

int Conversation::getUnread() const noexcept
{
    return unread_;
}

void Conversation::setUnread(const int value)
{
    updateIf("unread", value, unread_, this, &Conversation::unredChanged);
}

int Conversation::getIdentityId() const noexcept
{
    return identity_;
}

Identity *Conversation::getIdentity() const
{
    return DsEngine::instance().getIdentityManager()->identityFromId(getIdentityId());
}

bool Conversation::haveParticipant(const Contact &contact) const
{
    try {
        return getFirstParticipant()->getUuid() == contact.getUuid();
    } catch (const std::exception& ex) {
        LFLOG_ERROR << "Failed to check participant: " << ex.what();
    }
    return false;
}

QString Conversation::getFilesLocation() const
{
    auto home = DsEngine::instance().settings().value("downloadLocation").toString();

    if (home.isEmpty()) {
        throw Error("No download location specified in settings");
    }

    if (!home.endsWith('/')) {
        home += "/";
    }

    // Keep the configured download directory user-facing and predictable.
    // File-name collision handling is performed by Contact before a receive
    // starts, so per-identity/conversation UUID directories are unnecessary.
    QDir path(home);
    if (DsEngine::instance().settings().value(
                "includeConversationNameInDownloadPath", false).toBool()) {
        auto folderName = getName().trimmed();
        folderName.replace(QRegularExpression(QStringLiteral("[^A-Za-z0-9 _.-]")),
                           QStringLiteral("_"));
        folderName.replace(QStringLiteral(".."), QStringLiteral("_"));
        folderName = folderName.trimmed();
        if (folderName.isEmpty() || folderName == QStringLiteral(".")) {
            folderName = QStringLiteral("conversation");
        }
        path = QDir(path.filePath(folderName));
    }

    path.makeAbsolute();
    if (!path.exists()) {
        LFLOG_DEBUG << "Creating download directory \""
                    << path.path() << "\" for incoming files";
        path.mkpath(path.path());
    }

    return path.path();
}

void Conversation::addToDb()
{
    QSqlQuery query;

    query.prepare("INSERT INTO conversation ("
                  "identity, type, name, uuid, hash, participants, topic, created, updated, unread"
                  ") VALUES ("
                  ":identity, :type, :name, :uuid, :hash, :participants, :topic, :created, :updated, :unread"
                  ")");


    if (!created_.isValid()) {
        created_ = DsEngine::getSafeNow();
    }

    if (!lastActivity_.isValid()) {
        lastActivity_ = created_;
    }

    if (hash_.isEmpty()) {
        if (getType() == PRIVATE_P2P) {
            if (const auto& p = getParticipant()) {
                hash_ = p->getHash();
            }
        } else {
            assert(false); // TODO: Implement
        }
    }

    if (uuid_.isNull()) {
        uuid_ = QUuid::createUuid();
    }

    if (name_.isEmpty()) {
        if (const auto& p = getParticipant()) {
            if (getType() == PRIVATE_P2P) {
                name_ = p->getName();
                if (name_.isEmpty()) {
                    name_ = p->getNickName();
                }
                if (name_.isEmpty()) {
                    name_ = QStringLiteral("Unnamed contact");
                }
            } else {
                name_ = "Group Conversation";
            }
        }
    }

    // The database requires a non-null conversation name, even if a
    // participant could not be resolved while creating the conversation.
    if (name_.isEmpty()) {
        name_ = QStringLiteral("Unnamed conversation");
    }

    // TODO: We better put multiple participants in a separate table
    QString participants;
    for(const auto& uuid : participants_) {
        if (!participants.isEmpty()) {
            participants += ',';
        }
        participants += uuid.toString();
    }

    query.bindValue(":identity", identity_);
    query.bindValue(":uuid", uuid_);
    query.bindValue(":name", name_);
    query.bindValue(":topic", topic_);
    query.bindValue(":created", created_);
    query.bindValue(":updated", lastActivity_);
    query.bindValue(":type", type_);
    query.bindValue(":participants", participants);
    query.bindValue(":unread", unread_);
    query.bindValue(":hash", hash_);

    if(!query.exec()) {
        throw Error(QStringLiteral("Failed to save Conversation: %1").arg(
                        query.lastError().text()));
    }

    id_ = query.lastInsertId().toInt();

    LFLOG_INFO << "Added conversation \"" << getName()
               << "\" with uuid " << uuid_.toString()
               << " to the database with id " << id_;

}

void Conversation::deleteFromDb()
{
    if (id_ > 0) {
        QSqlQuery query;
        query.prepare("DELETE FROM conversation WHERE id=:id");
        query.bindValue(":id", id_);
        if(!query.exec()) {
            throw Error(QStringLiteral("SQL Failed to delete conversation: %1").arg(
                            query.lastError().text()));
        }
    }
}

Conversation::ptr_t Conversation::load(QObject& parent, const QUuid &uuid)
{
    return load(parent, [uuid](QSqlQuery& query) {
        query.prepare(getSelectStatement("uuid=:uuid"));
        query.bindValue(":uuid", uuid);
    });
}

Conversation::ptr_t Conversation::load(QObject &parent, int identity, const QByteArray &hash)
{
    return load(parent, [hash, identity](QSqlQuery& query) {
        query.prepare(getSelectStatement("hash=:hash AND identity=:identity"));
        query.bindValue(":hash", hash);
        query.bindValue(":identity", identity);
    });
}

Contact::ptr_t Conversation::getParticipant() const
{
    if (getType() == PRIVATE_P2P) {
        auto participants = getParticipants();
        if (participants.size() == 1) {
            if (auto contact = participants.front()) {
                return contact;
            }
        }
    }

    LFLOG_ERROR << "Not PRIVATE_P2P or participant not found";
    return {};
}

QByteArray Conversation::calculateHash(const Contact &contact)
{
    const auto userA = contact.getCert()->getSigningPubKey().toByteArray();
    const auto userB = contact.getIdentity()->getCert()->getSigningPubKey().toByteArray();
    const auto first = std::min(userA, userB);
    const auto second = std::max(userA, userB);
    return crypto::Crypto::getSha256(
        std::string_view{"DarkSpeak default conversation v3"}, first, second);
}

QString Conversation::getSelectStatement(const QString &where)
{
     return QStringLiteral("SELECT id, identity, type, name, uuid, hash, participants, topic, created, updated, unread FROM conversation WHERE %1")
             .arg(where);
}

Conversation::ptr_t Conversation::load(QObject &parent, const std::function<void (QSqlQuery &)> &prepare)
{
    QSqlQuery query;

    enum Fields {
        id, identity, type, name, uuid, hash, participants, topic, created, updated, unread
    };

    prepare(query);

    if(!query.exec()) {
        throw Error(QStringLiteral("Failed to fetch conversation: %1").arg(
                        query.lastError().text()));
    }

    if (!query.next()) {
        throw NotFoundError(QStringLiteral("Conversation not found!"));
    }

    auto ptr = make_shared<Conversation>(parent);
    ptr->id_ = query.value(id).toInt();
    ptr->identity_ = query.value(identity).toInt();
    ptr->name_ = query.value(name).toString();
    ptr->uuid_ = query.value(uuid).toUuid();
    ptr->participants_.insert(query.value(participants).toUuid());
    ptr->topic_ = query.value(topic).toString();
    ptr->type_ = static_cast<Type>(query.value(type).toInt());
    ptr->hash_ = query.value(hash).toByteArray();
    ptr->created_ = query.value(created).toDateTime();
    ptr->lastActivity_ = query.value(updated).toDateTime();
    ptr->unread_ = query.value(unread).toInt();

    return ptr;
}


}} // namespaces
