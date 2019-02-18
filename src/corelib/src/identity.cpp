

#include "ds/dsengine.h"
#include "ds/identity.h"
#include "ds/errors.h"

#include "logfault/logfault.h"

namespace ds {
namespace core {

Identity::Identity(QObject& parent,
             const int dbId, // -1 if the identity is new
             const bool online,
             const QDateTime& created,
             IdentityData data)
    : QObject{&parent}
    , id_{dbId}, online_{online}, data_{std::move(data)}, created_{created}
{}

void Identity::startService()
{
    getProtocolManager().startService(data_.uuid,
                                      data_.cert,
                                      DsEngine::fromJson(data_.addressData));
}

void Identity::stopService()
{
    getProtocolManager().stopService(data_.uuid);
}

void Identity::changeTransport()
{
     LFLOG_NOTICE << "Requesting a new Tor service for " << getName();
     DsEngine::instance().createNewTransport(
                 data_.name.toUtf8(),
                 data_.uuid);
}

ProtocolManager& Identity::getProtocolManager() {
    return DsEngine::instance().getProtocolMgr(
                ProtocolManager::Transport::TOR);
}

int Identity::getId() const noexcept {
    return id_;
}

QString Identity::getName() const noexcept  {
    return data_.name;
}

void Identity::setName(const QString &name) {
    updateIf("name", name, data_.name, &Identity::nameChanged);
}

QByteArray Identity::getAddress() const noexcept {
    return data_.address;
}

void Identity::setAddress(const QByteArray &address) {
    updateIf("address", address, data_.address, &Identity::addressChanged);
}

QByteArray Identity::getAddressData() const noexcept {
    return data_.addressData;
}

void Identity::setAddressData(const QByteArray &data) {
    updateIf("address_data", data, data_.addressData, &Identity::addressDataChanged);
}

QString Identity::getNotes() const noexcept {
    return data_.notes;
}

void Identity::setNotes(const QString &notes) {
    updateIf("notes", notes, data_.notes, &Identity::notesChanged);
}

QImage Identity::getAvatar() const noexcept  {
    return data_.avatar;
}

QString Identity::getAvatarUri() const noexcept
{
    if (data_.avatar.isNull()) {
        return "qrc:///images/anonymous.svg";
    }

    return QStringLiteral("image://identity/%s").arg(data_.uuid.toString());
}

void Identity::setAvatar(const QImage &avatar) {
    updateIf("notes", avatar, data_.avatar, &Identity::avatarChanged);
}

bool Identity::isOnline() const noexcept {
    return online_;
}

void Identity::setOnline(const bool value) {
    if (value != online_) {
        online_ = value;
        emit onlineChanged();
    }
}

QUuid Identity::getUuid() const noexcept {
    return data_.uuid;
}

QByteArray Identity::getHash() const noexcept {
    return data_.hash;
}

QDateTime Identity::getCreated() const noexcept {
    return created_;
}

crypto::DsCert::ptr_t Identity::getCert() const noexcept {
    return data_.cert;
}

QByteArray Identity::getB58EncodedIdetity() const noexcept
{
    return DsEngine::getIdentityAsBase58(data_.cert, data_.address);
}

QByteArray Identity::getHandle() const noexcept
{
    return data_.cert->getB58PubKey();
}

bool Identity::isAutoConnect() const noexcept
{
    return data_.autoConnect;
}

void Identity::setAutoConnect(bool value)
{
    updateIf("auto_connect", value, data_.autoConnect, &Identity::autoConnectChanged);
}

void Identity::addToDb() {
    QSqlQuery query;

    query.prepare("INSERT INTO identity ("
                  "uuid, hash, name, cert, address, address_data, notes, avatar, created, auto_connect"
                  ") VALUES ("
                  ":uuid, :hash, :name, :cert, :address, :address_data, :notes, :avatar, :created, :auto_connect"
                  ")");
    query.bindValue(":uuid", data_.uuid);
    query.bindValue(":hash", data_.hash);
    query.bindValue(":name", data_.name);
    query.bindValue(":cert", data_.cert->getCert().toByteArray());
    query.bindValue(":address", data_.address);
    query.bindValue(":address_data", data_.addressData);
    query.bindValue(":notes", data_.notes);
    query.bindValue(":avatar", data_.avatar);
    query.bindValue(":created", created_);
    query.bindValue(":auto_connect", data_.autoConnect);

    if(!query.exec()) {
        throw Error(QStringLiteral("Failed to add identity: %1").arg(
                        query.lastError().text()));
    }

    id_ = query.lastInsertId().toInt();

    LFLOG_INFO << "Added identity " << data_.name
               << " with uuid " << data_.uuid.toString()
               << " to the database with id " << id_;

}

void Identity::deleteFromDb() {
    if (id_ > 0) {
        QSqlQuery query;
        query.prepare("DELETE FROM identity WHERE id=:id");
        query.bindValue(":id", id_);
        if(!query.exec()) {
            throw Error(QStringLiteral("SQL Failed to delete identity: %1").arg(
                            query.lastError().text()));
        }
    }
}


}} // namespaces
