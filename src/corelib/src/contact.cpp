
#include "ds/contact.h"
#include "ds/dsengine.h"
#include "ds/identity.h"
#include "ds/errors.h"
#include "ds/update_helper.h"
#include "ds/errors.h"

#include "logfault/logfault.h"

namespace ds {
namespace core {

using namespace std;
using namespace crypto;

Contact::Contact(QObject &parent,
                 const int dbId,
                 const bool online,
                 ContactData data)
    : QObject{&parent}
    , id_{dbId}, online_{online}, data_{std::move(data)}
{
}

int Contact::getId() const noexcept {
    return id_;
}

QString Contact::getName() const noexcept  {
    return data_.name;
}

void Contact::setName(const QString &name) {
    updateIf("name", name, data_.name, this, &Contact::nameChanged);
}

QString Contact::getNickName() const noexcept  {
    return data_.nickName;
}

void Contact::setNickName(const QString &name) {
    updateIf("name", name, data_.nickName, this, &Contact::nickNameChanged);
}

QString Contact::getGroup() const noexcept
{
    return data_.group;
}

void Contact::setGroup(const QString &name) {
    updateIf("name", name, data_.group, this, &Contact::groupChanged);
}

QByteArray Contact::getAddress() const noexcept {
    return data_.address;
}

void Contact::setAddress(const QByteArray &address) {
    updateIf("address", address, data_.address, this, &Contact::addressChanged);
}

QString Contact::getNotes() const noexcept {
    return data_.notes;
}

void Contact::setNotes(const QString &notes) {
    updateIf("notes", notes, data_.notes, this, &Contact::notesChanged);
}

QImage Contact::getAvatar() const noexcept  {
    return data_.avatar;
}

QString Contact::getAvatarUri() const noexcept
{
    if (data_.avatar.isNull()) {
        return "qrc:///images/anonymous.svg";
    }

    return QStringLiteral("image://identity/%s").arg(data_.uuid.toString());
}

void Contact::setAvatar(const QImage &avatar) {
    updateIf("notes", avatar, data_.avatar, this, &Contact::avatarChanged);
}

bool Contact::isOnline() const noexcept {
    return online_;
}

void Contact::setOnline(const bool value) {
    if (value != online_) {
        online_ = value;
        emit onlineChanged();
    }
}

QUuid Contact::getUuid() const noexcept {
    return data_.uuid;
}

QByteArray Contact::getHash() const noexcept {
    return data_.hash;
}

QDateTime Contact::getCreated() const noexcept {
    return created_;
}

QDateTime Contact::getLastSeen() const noexcept
{
    return data_.lastSeen;
}

void Contact::setLastSeen(const QDateTime &when)
{
    updateIf("last_seen", when, data_.lastSeen, this, &Contact::lastSeenChanged);
}

crypto::DsCert::ptr_t Contact::getCert() const noexcept {
    return data_.cert;
}

QByteArray Contact::getB58EncodedIdetity() const noexcept
{
    return DsEngine::getIdentityAsBase58(data_.cert, data_.address);
}

QByteArray Contact::getHandle() const noexcept
{
    return data_.cert->getB58PubKey();
}

bool Contact::isAutoConnect() const noexcept
{
    return data_.autoConnect;
}

void Contact::setAutoConnect(bool value)
{
    updateIf("auto_connect", value, data_.autoConnect, this, &Contact::autoConnectChanged);
}

InitiatedBy Contact::getWhoInitiated() const noexcept
{
    return data_.whoInitiated;
}

ContactState Contact::getState() const noexcept
{
    return data_.state;
}

void Contact::setState(const ContactState state)
{
    updateIf("state", state, data_.state, this, &Contact::stateChanged);
}

QString Contact::getAddMeMessage() const noexcept
{
    return data_.addMeMessage;
}

void Contact::setAddMeMessage(const QString &msg)
{
    updateIf("addme_message", msg, data_.addMeMessage, this, &Contact::addMeMessageChanged);
}

bool Contact::isPeerVerified() const noexcept
{
    return data_.peerVerified;
}

void Contact::setPeerVerified(const bool verified)
{
    updateIf("peer_verified", verified, data_.peerVerified, this, &Contact::peerVerifiedChanged);
}

Contact::ptr_t Contact::load(QObject& parent, const QUuid &key)
{
    QSqlQuery query;

    enum Fields {
        id, identity, uuid, name, nickname, cert, address, notes, contact_group, avatar, created, initiated_by, last_seen, state, addme_message, auto_connect, hash, peer_verified
    };

    query.prepare("SELECT "
                  "id, identity, uuid, name, nickname, cert, address, notes, contact_group, avatar, created, initiated_by, last_seen, state, addme_message, auto_connect, hash, peer_verified "
                  " from contact where uuid=:uuid");
    query.bindValue(":uuid", key);
    if(!query.exec()) {
        throw Error(QStringLiteral("Failed to fetch contact: %1").arg(
                        query.lastError().text()));
    }

    ContactData data {
        query.value(identity).toInt(),
        query.value(name).toString(),
        query.value(uuid).toUuid(),
        query.value(nickname).toString(),
        query.value(hash).toByteArray(),
        query.value(notes).toString(),
        query.value(contact_group).toString(),
        DsCert::createFromPubkey(query.value(cert).toByteArray()),
        query.value(address).toByteArray(),
        QImage::fromData(query.value(avatar).toByteArray()),
        query.value(created).toDateTime(),
        static_cast<InitiatedBy>(query.value(initiated_by).toInt()),
        query.value(last_seen).toDateTime(),
        static_cast<ContactState>(query.value(state).toInt()),
        query.value(addme_message).toString(),
        query.value(auto_connect).toBool(),
        query.value(peer_verified).toBool()
    };

    return make_shared<Contact>(parent,
                               query.value(id).toInt(),
                               false, //Contacts that are connected are always in memory
                               data);
}

void Contact::addToDb()
{
    QSqlQuery query;

    query.prepare("INSERT INTO contact ("
                  "identity, uuid, name, nickname, cert, address, notes, contact_group, avatar, created, initiated_by, last_seen, state, addme_message, auto_connect, hash, peer_verified "
                  ") VALUES ("
                  ":identity, :uuid, :name, :nickname, :cert, :address, :notes, :contact_group, :avatar, :created, :initiated_by, :last_seen, :state, :addme_message, :auto_connect, :hash, :peer_verified "
                  ")");

    query.bindValue(":identity", data_.identity);
    query.bindValue(":uuid", data_.uuid);
    query.bindValue(":name", data_.name);
    query.bindValue(":nickname", data_.nickName);
    query.bindValue(":cert", data_.cert->getCert().toByteArray());
    query.bindValue(":address", data_.address);
    query.bindValue(":notes", data_.notes);
    query.bindValue(":contact_group", data_.group);
    query.bindValue(":avatar", data_.avatar);
    query.bindValue(":created", data_.created);
    query.bindValue(":initiated_by", data_.whoInitiated);
    query.bindValue(":last_seen", data_.lastSeen);
    query.bindValue(":state", data_.state);
    query.bindValue(":addme_message", data_.addMeMessage);
    query.bindValue(":auto_connect", data_.autoConnect);
    query.bindValue(":hash", data_.hash);
    query.bindValue(":peer_verified", data_.peerVerified);

    if(!query.exec()) {
        throw Error(QStringLiteral("Failed to add identity: %1").arg(
                        query.lastError().text()));
    }

    id_ = query.lastInsertId().toInt();

    LFLOG_INFO << "Added contact " << data_.name
               << " with uuid " << data_.uuid.toString()
               << " to the database with id " << id_;
}

void Contact::deleteFromDb() {
    if (id_ > 0) {
        QSqlQuery query;
        query.prepare("DELETE FROM contact WHERE id=:id");
        query.bindValue(":id", id_);
        if(!query.exec()) {
            throw Error(QStringLiteral("SQL Failed to delete identity: %1").arg(
                            query.lastError().text()));
        }
    }
}

}}

