#include "ds/conversation.h"
#include "ds/errors.h"
#include "ds/update_helper.h"
#include "ds/dsengine.h"
#include "ds/crypto.h"
#include "ds/identity.h"

#include "logfault/logfault.h"

#include <QStandardPaths>
#include <QUrl>

namespace ds {
namespace core {

using namespace std;
using namespace crypto;

Conversation::Conversation(QObject &parent)
    : QObject{&parent}
{

}

Conversation::Conversation(QObject &parent, const QString &name, const QString &topic, Contact *participant)
    : QObject{&parent}, identity_{participant->getIdentity()->getId()}
    , name_{name}, topic_{topic}, hash_{calculateHash(*participant)}
{
    participants_.insert(participant->getUuid());
}

void Conversation::incrementUnread()
{
    setUnread(getUnread() + 1);
}

void Conversation::sendMessage(const QString &text)
{
    LFLOG_DEBUG << "Sending message with content: " << text;

    const auto random = crypto::Crypto::getRandomBytes(8);
    const auto identity = getIdentity();
    assert(identity);

    MessageData data;
    data.conversation = hash_;
    data.composedTime = DsEngine::getSafeNow();
    data.content = text;
    data.encoding = Message::Encoding::UTF8;
    data.sender = identity->getHash();
    crypto::createHash(data.messageId, {
                           random,
                           data.content.toUtf8(),
                           data.conversation,
                           data.composedTime.toString().toUtf8()});

    DsEngine::instance().getMessageManager()->sendMessage(*this, data);
    touchLastActivity();
}

void Conversation::sendFile(const QVariantMap &args)
{

    const auto path = args.value("path").toString();
    if (path.isEmpty()) {
        LFLOG_DEBUG << "Ignoring request to send file with no path...";
        return;
    }

    auto data = make_unique<FileData>();
    data->name = args.value("name").toString();

    // Convert from "file:///" to local path now
    data->path = QUrl(path).toLocalFile();

    if (data->name.isEmpty()) {
        QFileInfo fi{data->path};
        data->name = fi.fileName();
    }

    data->conversation = getId();
    data->contact = getFirstParticipant()->getId();
    data->identity = getIdentityId();

    DsEngine::instance().getFileManager()->addFile(move(data));
    touchLastActivity();
}

void Conversation::incomingMessage(Contact *contact, const MessageData &data)
{
    // Add to the database
    if (!DsEngine::instance().getMessageManager()->receivedMessage(*this, data)) {
        contact->sendAck("Message", "Rejected", data.messageId.toBase64());
        return;
    }

    // Send ack
    contact->sendAck("Message", "Received", data.messageId.toBase64());

    touchLastActivity();
}

void Conversation::incomingFileOffer(Contact *contact, const PeerFileOffer &offer)
{
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
    return getFirstParticipant()->getUuid() == contact.getUuid();
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

    QDir path(home
              + getIdentity()->getUuid().toString()
              + "/" + getUuid().toString());

    path.makeAbsolute();
    if (!path.exists()) {
        LFLOG_DEBUG << "Creating download directory \""
                    << path.path()
                    << "\" for conversation "
                    << getName()
                    << " at identity " << getIdentity()->getName();
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
            hash_ = getParticipant()->getHash();
        } else {
            assert(false); // TODO: Implement
        }
    }

    if (uuid_.isNull()) {
        uuid_ = QUuid::createUuid();
    }

    if (name_.isEmpty()) {
        if (getType() == PRIVATE_P2P) {
            name_ = getParticipant()->getName();
        } else {
            name_ = "Group Conversation";
        }
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

    throw runtime_error("Not PRIVATE_P2P or participant not found");
}

QByteArray Conversation::calculateHash(const Contact &contact)
{
    const auto h1 = contact.getCert()->getHash().toByteArray();
    const auto h2 = contact.getIdentity()->getCert()->getHash().toByteArray();

    QByteArray hash;
    if (h1 > h2) {
        crypto::createHash(hash, {h1, h2});
    } else {
        crypto::createHash(hash, {h2, h1});
    }

    return hash;
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

