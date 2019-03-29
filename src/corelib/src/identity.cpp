#include <random>

#include "ds/dsengine.h"
#include "ds/identity.h"
#include "ds/errors.h"
#include "ds/update_helper.h"
#include "ds/dscert.h"
#include "ds/base58.h"

#include "logfault/logfault.h"

#include <QTimer>

namespace ds {
namespace core {

using namespace std;

Identity::Identity(QObject& parent,
             const int dbId, // -1 if the identity is new
             const bool online,
             const QDateTime& created,
             IdentityData data)
    : QObject{&parent}
    , id_{dbId}, online_{online}, data_{std::move(data)}, created_{created}
{

    connect(this, &Identity::processOnlineLater,
            this, &Identity::onProcessOnlineLater,
            Qt::QueuedConnection);
}

void Identity::addContact(const QVariantMap &args)
{
    LFLOG_DEBUG << "Adding contact: " << args.value("name").toString();

    auto data = make_unique<ContactData>();

    data->identity = args.value("identity").toInt();
    data->name = args.value("name").toString();
    data->nickName = args.value("nickName").toString();
    auto handle = args.value("handle").toByteArray();
    data->cert = crypto::DsCert::createFromPubkey(crypto::b58tobin_check<QByteArray>(
                                    handle.toStdString(), 32, {249, 50}));
    data->addMeMessage = args.value("addmeMessage").toString();
    data->address = args.value("address").toByteArray();
    data->autoConnect = args.value("autoConnect").toBool();
    data->notes = args.value("notes").toString();

    DsEngine::instance().getContactManager()->addContact(move(data));
}

void Identity::startService()
{
    if (data_.addressData.isEmpty()) {
        LFLOG_ERROR << "The identity " << getName()
                    << " has no address. Cannot go online.";
        return;

    }
    getProtocolManager().startService(data_.uuid,
                                      data_.cert,
                                      DsEngine::fromJson(data_.addressData));
}

void Identity::stopService()
{
    disconnectContacts();
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

void Identity::onIncomingPeer(const std::shared_ptr<PeerConnection>& peer)
{
    if (auto contact = contactFromHash(peer->getPeerCert()->getHash().toByteArray())) {

        LFLOG_DEBUG << "Passing incoming connection "
                    << peer->getConnectionId().toString()
                    << " to Contact " << contact->getName()
                    << " on Identity " << getName();

        contact->onIncomingPeer(peer);
        return;
    }

    connect(peer.get(), &PeerConnection::addmeRequest,
            this, &Identity::onAddmeRequest);

    // At this point we leave the ownership and responsibility for the
    // connection at the protocol layer
    LFLOG_NOTICE << "incoming connection from unknow handle: " << peer->getPeerCert()->getB58PubKey();

    peer->authorize(true);
}

void Identity::onAddmeRequest(const PeerAddmeReq &req)
{
    if (auto contact = contactFromHandle(req.handle)) {
        contact->onAddmeRequest(req);
        return;
    }

    // We need to trigger a signal that can be caught by the Notification system
    // in a higher API layer than the current library.
    // Whoever needs this signal will connect to a related signal in the
    // IdentityManager.
    DsEngine::instance().getIdentityManager()->relayNewContactRequest(this, req);

    // No further input is required until we have deciced if we want to add the contact,
    // and we have verified its handle.
    req.peer->close();
}

void Identity::onProcessOnlineLater()
{
    connectContacts();
}

void Identity::connectContacts()
{
    QSqlQuery query;
    query.prepare("SELECT uuid FROM contact WHERE identity=:id AND auto_connect=1");
    query.bindValue(":id", getId());
    if(!query.exec()) {
        throw Error(QStringLiteral("Failed to fetch contact from hash: %1").arg(
                        query.lastError().text()));
    }

    while (query.next()) {
        auto contact = DsEngine::instance().getContactManager()->getContact(query.value(0).toUuid());

        // Connect to contacts with random delays to make it a tiny bit harder for
        // NSA, German intelligence and GRU to deduce what's going on,
        // based on based on the meta-date they collect from the transport
        // layer on the network.
        const auto delay = getRandomConnectDelay();

        LFLOG_DEBUG << "Identity " << getName()
                    << " will connect to Contact " << contact->getName()
                    << " in " << (delay / 1000) << " seconds.";

        auto uuid = contact->getUuid();
        QTimer::singleShot(delay, this, [this, uuid]() {
            // Get the contact again fromn the manager.
            // This will fail if the contact was deleted while the timer was running...
            if (auto contact = DsEngine::instance().getContactManager()->getContact(uuid)) {
                if (isOnline()
                      && contact->isAutoConnect()
                      && !contact->wasManuallyDisconnected()
                      && (contact->getOnlineStatus() == Contact::DISCONNECTED)
                      && ((contact->getState() == Contact::WAITING_FOR_ACCEPTANCE)
                       || (contact->getState() == Contact::ACCEPTED)
                       || (contact->getState() == Contact::PENDING))) {
                    contact->connectToContact();
                }
            }
        });
    }
}

int Identity::getRandomConnectDelay()
{
     static random_device rd;
     static mt19937 gen{rd()}; //Standard mersenne_twister_engine seeded with rd()
     static std::uniform_int_distribution<> dis(3000, 60000);
     return dis(gen);
}

void Identity::disconnectContacts()
{
    std::deque<Contact::ptr_t> contacts;
    for(const auto& it : connected_) {
        assert(it.second->contact);
        contacts.push_back(it.second->contact);
    }

    for(auto& contact : contacts) {
        LFLOG_DEBUG << "Identity " << getName()
                    << " is disconnected; disconnecting Contact: "
                    << contact->getName();
        contact->disconnectFromContact();
    }
}

Contact::ptr_t Identity::contactFromHandle(const QByteArray &handle)
{
    auto cert = crypto::DsCert::createFromPubkey(crypto::b58tobin_check<QByteArray>(
                                                     handle.toStdString(), 32, {249, 50}));

    return contactFromHash(cert->getHash().toByteArray());
}

Contact::ptr_t Identity::contactFromHash(const QByteArray &hash)
{
    QSqlQuery query;
    query.prepare("SELECT uuid FROM contact WHERE identity=:id AND hash=:hash");
    query.bindValue(":id", getId());
    query.bindValue(":hash", hash);
    if(!query.exec()) {
        throw Error(QStringLiteral("Failed to fetch contact from hash: %1").arg(
                        query.lastError().text()));
    }

    if (query.next()) {
        return contactFromUuid(query.value(0).toUuid());
    }

    return {};
}

Contact::ptr_t Identity::contactFromUuid(const QUuid &uuid)
{
    if (auto contact = DsEngine::instance().getContactManager()->getContact(uuid)) {
        if (contact->getIdentityId() == getId()) {
            return contact;
        }
    }

    return {};
}

Conversation::ptr_t Identity::convesationFromHash(const QByteArray &hash)
{
    QSqlQuery query;
    query.prepare("SELECT uuid FROM conversation WHERE identity=:id AND hash=:hash");
    query.bindValue(":id", getId());
    query.bindValue(":hash", hash);
    if(!query.exec()) {
        throw Error(QStringLiteral("Failed to fetch conversation from hash: %1").arg(
                        query.lastError().text()));
    }

    if (query.next()) {
        return DsEngine::instance().getConversationManager()->getConversation(query.value(0).toUuid());
    }

    return {};
}

void Identity::registerConnection(const Contact::ptr_t &contact)
{
    connected_[contact->getUuid()] = make_unique<Connected>(contact);
}

void Identity::unregisterConnection(const QUuid &uuid)
{
    connected_.erase(uuid);
}

int Identity::getId() const noexcept {
    return id_;
}

QString Identity::getName() const noexcept  {
    return data_.name;
}

void Identity::setName(const QString &name) {
    updateIf("name", name, data_.name, this, &Identity::nameChanged);
}

QByteArray Identity::getAddress() const noexcept {
    return data_.address;
}

void Identity::setAddress(const QByteArray &address) {
    updateIf("address", address, data_.address, this, &Identity::addressChanged);
}

QByteArray Identity::getAddressData() const noexcept {
    return data_.addressData;
}

void Identity::setAddressData(const QByteArray &data) {
    updateIf("address_data", data, data_.addressData, this, &Identity::addressDataChanged);
}

QString Identity::getNotes() const noexcept {
    return data_.notes;
}

void Identity::setNotes(const QString &notes) {
    updateIf("notes", notes, data_.notes, this, &Identity::notesChanged);
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
    updateIf("notes", avatar, data_.avatar, this, &Identity::avatarChanged);
}

bool Identity::isOnline() const noexcept {
    return online_;
}

void Identity::setOnline(const bool value) {
    if (value != online_) {
        online_ = value;
        emit onlineChanged();

        if (online_) {
            emit processOnlineLater();
        } else {
            disconnectContacts();
        }
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
    updateIf("auto_connect", value, data_.autoConnect, this, &Identity::autoConnectChanged);
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
