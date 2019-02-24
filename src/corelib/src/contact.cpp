
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
                 data_t data)
    : QObject{&parent}
    , id_{dbId}, online_{online}, data_{move(data)}
{
    connect(this, &Contact::sendAddMeLater,
            this, &Contact::onSendAddMeLater,
            Qt::QueuedConnection);

    connect(this, &Contact::sendAddmeAckLater,
            this, &Contact::onSendAddmeAckLater,
            Qt::QueuedConnection);

    connect(this, &Contact::processOnlineLater,
            this, &Contact::onProcessOnlineLater,
            Qt::QueuedConnection);
}

void Contact::connectToContact()
{
    if (auto identity = getIdentity()) {
        disconnectFromContact();

        ConnectData cd;
        cd.address = getAddress();
        cd.contactsCert = getCert();
        cd.identitysCert = identity->getCert();
        cd.service = identity->getUuid();

        const auto peer = identity->getProtocolManager().connectTo(cd);
        LFLOG_DEBUG << "Connecting to " << getName()
                    << " at " << getAddress()
                    << " with connection-id: " << peer->getConnectionId().toString();

        connection_ = make_unique<Connection>(peer, *this);
        setOnlineStatus(CONNECTING);

        connect(peer.get(), &PeerConnection::connectedToPeer,
                this, &Contact::onConnectedToPeer);

        connect(peer.get(), &PeerConnection::disconnectedFromPeer,
                this, &Contact::onDisconnectedFromPeer);
    }
}

void Contact::disconnectFromContact()
{
    connection_.reset();
}

int Contact::getId() const noexcept {
    return id_;
}

QString Contact::getName() const noexcept  {
    return data_->name;
}

void Contact::setName(const QString &name) {
    updateIf("name", name, data_->name, this, &Contact::nameChanged);
}

QString Contact::getNickName() const noexcept  {
    return data_->nickName;
}

void Contact::setNickName(const QString &name) {
    updateIf("name", name, data_->nickName, this, &Contact::nickNameChanged);
}

QString Contact::getGroup() const noexcept
{
    return data_->group;
}

void Contact::setGroup(const QString &name) {
    updateIf("name", name, data_->group, this, &Contact::groupChanged);
}

QByteArray Contact::getAddress() const noexcept {
    return data_->address;
}

void Contact::setAddress(const QByteArray &address) {
    updateIf("address", address, data_->address, this, &Contact::addressChanged);
}

QString Contact::getNotes() const noexcept {
    return data_->notes;
}

void Contact::setNotes(const QString &notes) {
    updateIf("notes", notes, data_->notes, this, &Contact::notesChanged);
}

QImage Contact::getAvatar() const noexcept  {
    return data_->avatar;
}

QString Contact::getAvatarUri() const noexcept
{
    if (data_->avatar.isNull()) {
        return "qrc:///images/anonymous.svg";
    }

    return QStringLiteral("image://identity/%s").arg(data_->uuid.toString());
}

void Contact::setAvatar(const QImage &avatar) {
    updateIf("notes", avatar, data_->avatar, this, &Contact::avatarChanged);
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
    return data_->uuid;
}

QByteArray Contact::getHash() const noexcept {
    return data_->hash;
}

QDateTime Contact::getCreated() const noexcept {
    return data_->created;
}

QDateTime Contact::getLastSeen() const noexcept
{
    return data_->lastSeen;
}

void Contact::setLastSeen(const QDateTime &when)
{
    updateIf("last_seen", when, data_->lastSeen, this, &Contact::lastSeenChanged);
}

void Contact::touchLastSeen()
{
    const auto when = QDateTime::fromTime_t((QDateTime::currentDateTime().toTime_t() / 60) * 60);
    setLastSeen(when);
}

crypto::DsCert::ptr_t Contact::getCert() const noexcept {
    return data_->cert;
}

QByteArray Contact::getB58EncodedIdetity() const noexcept
{
    return DsEngine::getIdentityAsBase58(data_->cert, data_->address);
}

QByteArray Contact::getHandle() const noexcept
{
    return data_->cert->getB58PubKey();
}

bool Contact::isAutoConnect() const noexcept
{
    return data_->autoConnect;
}

void Contact::setAutoConnect(bool value)
{
    updateIf("auto_connect", value, data_->autoConnect, this, &Contact::autoConnectChanged);
}

Contact::InitiatedBy Contact::getWhoInitiated() const noexcept
{
    return data_->whoInitiated;
}

Contact::ContactState Contact::getState() const noexcept
{
    return data_->state;
}

void Contact::setState(const ContactState state)
{
    updateIf("state", state, data_->state, this, &Contact::stateChanged);
}

QString Contact::getAddMeMessage() const noexcept
{
    return data_->addMeMessage;
}

void Contact::setAddMeMessage(const QString &msg)
{
    updateIf("addme_message", msg, data_->addMeMessage, this, &Contact::addMeMessageChanged);
}

bool Contact::isPeerVerified() const noexcept
{
    return data_->peerVerified;
}

void Contact::setPeerVerified(const bool verified)
{
    updateIf("peer_verified", verified, data_->peerVerified, this, &Contact::peerVerifiedChanged);
}

QString Contact::getOnlineIcon() const noexcept
{
    return onlineIcon_;
}

void Contact::setOnlineIcon(const QString &path)
{
    if (path != onlineIcon_) {
        onlineIcon_ = path;
        emit onlineIconChanged();
    }
}

Identity *Contact::getIdentity() const
{
    return DsEngine::instance().getIdentityManager()->identityFromId(data_->identity);
}

Contact::OnlineStatus Contact::getOnlineStatus() const noexcept
{
    return onlineStatus_;
}

void Contact::setOnlineStatus(const Contact::OnlineStatus status)
{
    if (onlineStatus_ != status) {
        onlineStatus_ = status;
        emit onlineStatusChanged();

        setOnline(onlineStatus_ == ONLINE);
    }
}

int Contact::getIdentityId() const noexcept
{
    return data_->identity;
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

    if (!query.next()) {
        throw Error(QStringLiteral("No result for contact: %1").arg(key.toString()));
    }

    auto data = make_unique<ContactData>();
    data->identity = query.value(identity).toInt();
    data->name = query.value(name).toString();
    data->uuid = query.value(uuid).toUuid();
    data->nickName = query.value(nickname).toString();
    data->hash = query.value(hash).toByteArray();
    data->notes = query.value(notes).toString();
    data->group = query.value(contact_group).toString();
    data->cert = DsCert::create(query.value(cert).toByteArray());
    data->address = query.value(address).toByteArray();
    data->avatar = QImage::fromData(query.value(avatar).toByteArray());
    data->created = query.value(created).toDateTime();
    data->whoInitiated = static_cast<InitiatedBy>(query.value(initiated_by).toInt());
    data->lastSeen = query.value(last_seen).toDateTime();
    data->state = static_cast<ContactState>(query.value(state).toInt());
    data->addMeMessage = query.value(addme_message).toString();
    data->autoConnect = query.value(auto_connect).toBool();
    data->peerVerified = query.value(peer_verified).toBool();

    return make_shared<Contact>(parent,
                               query.value(id).toInt(),
                               false, //Contacts that are connected are always in memory
                               move(data));
}

void Contact::addToDb()
{
    QSqlQuery query;

    query.prepare("INSERT INTO contact ("
                  "identity, uuid, name, nickname, cert, address, notes, contact_group, avatar, created, initiated_by, last_seen, state, addme_message, auto_connect, hash, peer_verified "
                  ") VALUES ("
                  ":identity, :uuid, :name, :nickname, :cert, :address, :notes, :contact_group, :avatar, :created, :initiated_by, :last_seen, :state, :addme_message, :auto_connect, :hash, :peer_verified "
                  ")");

    if (data_->group.isEmpty()) {
        data_->group = tr("Default");
    }

    if (!data_->created.isValid()) {
        data_->created = QDateTime::fromTime_t((QDateTime::currentDateTime().toTime_t() / 60) * 60);
    }

    if (data_->hash.isEmpty()) {
        data_->hash = data_->cert->getHash().toByteArray();
    }

    query.bindValue(":identity", data_->identity);
    query.bindValue(":uuid", data_->uuid);
    query.bindValue(":name", data_->name);
    query.bindValue(":nickname", data_->nickName);
    query.bindValue(":cert", data_->cert->getCert().toByteArray());
    query.bindValue(":address", data_->address);
    query.bindValue(":notes", data_->notes);
    query.bindValue(":contact_group", data_->group);
    query.bindValue(":avatar", data_->avatar);
    query.bindValue(":created", data_->created);
    query.bindValue(":initiated_by", data_->whoInitiated);
    query.bindValue(":last_seen", data_->lastSeen);
    query.bindValue(":state", data_->state);
    query.bindValue(":addme_message", data_->addMeMessage);
    query.bindValue(":auto_connect", data_->autoConnect);
    query.bindValue(":hash", data_->hash);
    query.bindValue(":peer_verified", data_->peerVerified);

    if(!query.exec()) {
        throw Error(QStringLiteral("Failed to add Contact: %1").arg(
                        query.lastError().text()));
    }

    id_ = query.lastInsertId().toInt();

    LFLOG_INFO << "Added contact " << data_->name
               << " with uuid " << data_->uuid.toString()
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

void Contact::onConnectedToPeer(const std::shared_ptr<PeerConnection>& peer)
{
    LFLOG_DEBUG << "Connection " << peer->getConnectionId().toString()
                << " to Contact " << getName()
                << " on Identity " << getIdentity()->getName()
                << " is successfully established.";

    if (!isPeerVerified()) {
        if (peer->getDirection() == PeerConnection::OUTGOING) {
            LFLOG_DEBUG << "Peer " << getName() << " is verified at handle " << getHandle();
            setPeerVerified(true); // We connected
        } else {
            LFLOG_DEBUG << "Peer " << getName() << " is not not verified. Disconnecting.";
            peer->close();
            return;
        }
    }

    switch (getState()) {
    case PENDING:
        [[fallthrough]];
    case WAITING_FOR_ACCEPTANCE:
        if (getWhoInitiated() == ME) {
            emit sendAddMeLater();
        } else {
            emit sendAddmeAckLater();
        }
        break;
    case ACCEPTED:
        emit processOnlineLater();
        break;
    case REJECTED:
        // Allow the connection, but only accept addme or addme/ack events.
        LFLOG_DEBUG << "Peer " << getName() << " rejected us but is now connecting. Will he send an addme?";
        break;
    case BLOCKED:
         LFLOG_DEBUG << "Peer " << getName() << " is BLOCKED. Disconnecting";
         peer->close();
         return;
    }

    if (!connection_ || (connection_->peer != peer)) {

        /* Rules:
         *
         * - A new incoming connection replaces an existing incoming connection
         * - A new outgoing connection replaces an existing outgoing connection
         * - If the existion connection is active, compare hashes and use the lesser one.
         */

        if (isOnline() && connection_ && (connection_->peer->getDirection() != peer->getDirection())) {
            if (getCert()->getHash().toByteArray() < peer->getPeerCert()->getHash().toByteArray()) {
                LFLOG_NOTICE << "Incmoing connection with id " << peer->getConnectionId().toString()
                             << " has a 'larger' hash than my hash. "
                             << "I will therefore prefer my current, active connection "
                             << connection_->peer->getConnectionId().toString();
                peer->close();
                return;
            }
        }

        LFLOG_DEBUG << "Switching connection for Contact " << getName()
                    << " at Identity " << getIdentity()->getName()
                    << " to " << peer->getConnectionId().toString();

        connection_ = make_unique<Connection>(peer, *this);
    }

    if (connection_->peer->getDirection() == PeerConnection::INCOMING) {
        // For outgoing connections, we already have a connect()
        connect(peer.get(), &PeerConnection::disconnectedFromPeer,
                this, &Contact::onDisconnectedFromPeer);
    }

    connect(connection_->peer.get(), &PeerConnection::receivedAck,
            this, &Contact::onReceivedAck);

    connect(connection_->peer.get(), &PeerConnection::addmeRequest,
            this, &Contact::onAddmeRequest);

    setOnlineStatus(ONLINE);

    touchLastSeen();
}

void Contact::onIncomingPeer(const std::shared_ptr<PeerConnection> &peer)
{
    if (getState() == BLOCKED) {
        LFLOG_DEBUG << "Peer " << getName() << " is BLOCKED. Disconnecting";
        peer->authorize(false);
        return;
    }

    connect(peer.get(), &PeerConnection::connectedToPeer,
            this, &Contact::onConnectedToPeer);

    peer->authorize(true);
}

void Contact::onDisconnectedFromPeer(const std::shared_ptr<PeerConnection>& peer)
{
    LFLOG_DEBUG << "Connection " << peer->getConnectionId().toString()
                << " to Contact " << getName()
                << " is disconnected.";

    connection_.reset();
    setOnlineStatus(DISCONNECTED);
}

void Contact::onSendAddMeLater()
{
    if (getOnlineStatus() != ONLINE) {
        return;
    }

    if (auto identity = getIdentity()) {
        AddmeReq ar{identity->getUuid(),
                   connection_->peer->getConnectionId(),
                   identity->getName(),
                   getAddMeMessage()};

        LFLOG_NOTICE << "Sending Addme from Identity " << identity->getName()
                     << " to Contact "
                     << getName()
                     << " over connection " << ar.connection.toString();

        try {
            getIdentity()->getProtocolManager().sendAddme(ar);
        } catch(const std::exception& ex) {
            LFLOG_WARN << "Failed to send addme from " << identity->getName()
                        << " to "
                        << getName()
                        << " over connection " << ar.connection.toString()
                        << ":" << ex.what();
            return;
        }

        if (getState() == PENDING) {
            setState(WAITING_FOR_ACCEPTANCE);
        }
    }
}

void Contact::onSendAddmeAckLater()
{
    if (getOnlineStatus() != ONLINE) {
        return;
    }

    assert(connection_);

    if (auto identity = getIdentity()) {

        LFLOG_NOTICE << "Sending Addme Ack from Identity " << identity->getName()
                     << " to Contact "
                     << getName()
                     << " over connection " << connection_->peer->getConnectionId().toString();

        try {
            connection_->peer->sendAck("AddMe", "Added");
        } catch(const std::exception& ex) {
            LFLOG_WARN << "Failed to send addme Ack from " << identity->getName()
                        << " to "
                        << getName()
                        << " over connection " << connection_->peer->getConnectionId().toString()
                        << ":" << ex.what();
            return;
        }

        setState(ACCEPTED);
    }
}

void Contact::onProcessOnlineLater()
{
    if (getOnlineStatus() != ONLINE) {
        return;
    }

    if (getState() != ACCEPTED) {
        return;
    }

    assert(connection_);

    // Send messages etc...
}

void Contact::onReceivedAck(const PeerAck &ack)
{
    touchLastSeen();

    if (!isPeerVerified()) {
        LFLOG_DEBUG << "Received Ack from unverified peer on connection "
                    << ack.connectionId.toString();
        ack.peer->close();
        return;
    }

    if (getState() == BLOCKED) {
        LFLOG_DEBUG << "Received Ack from blockwed peer on connection "
                    << ack.connectionId.toString();
        ack.peer->close();
        return;
    }

    if (ack.what == "AddMe") {
        if (ack.status == "Added") {
            setState(ACCEPTED);
            emit processOnlineLater();
        } else if (ack.status == "Pending") {
            setState(WAITING_FOR_ACCEPTANCE);
            ack.peer->close();
        } else if (ack.status == "Rejected") {
            setState(REJECTED);
            ack.peer->close();
        } else {
            LFLOG_WARN << "Received unsupported ack.status from Contact "
                       << getName()
                       << " at Identity "
                       << getIdentity()->getName()
                       << ". Ignoring the message.";
        }
    }
}

void Contact::onAddmeRequest(const PeerAddmeReq &req)
{
    Q_UNUSED(req)

    touchLastSeen();

    if (!isPeerVerified()) {
        if (connection_ && isOnline()) {
            connection_->peer->sendAck("AddMe", "Pending");
            connection_->peer->close();
            return;
        }
    }

    if (getState() == BLOCKED) {
        if (connection_ && isOnline()) {
            connection_->peer->sendAck("AddMe", "Rejected");
            connection_->peer->close();
            return;
        }
    }

    if (connection_ && isOnline()) {
        connection_->peer->sendAck("AddMe", "Accepted");
        setState(ACCEPTED);
        emit processOnlineLater();
    }
}

Contact::Connection::~Connection()
{
    if (auto identity = owner.getIdentity()) {
        if (peer->isConnected()) {
            LFLOG_DEBUG << "Disconnecting from " << owner.getName()
                        << " at " << owner.getAddress()
                        << " connection "  << peer->getConnectionId().toString();
        }
        owner.setOnlineStatus(DISCONNECTED);
    }
    peer->close();
}

}}

