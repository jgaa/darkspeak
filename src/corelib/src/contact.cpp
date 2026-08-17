
#include <memory>
#include <iterator>

#include <QTimer>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QCryptographicHash>

#include "ds/contact.h"
#include "ds/dsengine.h"
#include "ds/identity.h"
#include "ds/errors.h"
#include "ds/update_helper.h"
#include "ds/errors.h"
#include "ds/conversation.h"
#include "ds/avatarhash.h"
#include "ds/avatarutil.h"
#include "ds/bio.h"

#include "logfault/logfault.h"

namespace ds {
namespace core {

using namespace std;
using namespace crypto;

namespace  {
bool comparesEqual(const QByteArray& left, const QByteArray& right) {
    if (left.size() != right.size()) {
        return false;
    }
    return memcmp(left.constData(), right.constData(), static_cast<size_t>(left.size())) == 0;
}

bool mimeAllowed(const QStringList& allowed, const QString& mimeType)
{
    return allowed.contains(QStringLiteral("*")) || allowed.contains(mimeType);
}
}

Contact::Contact(const int dbId,
                 const bool online,
                 data_t data)
    : id_{dbId}, online_{online}, data_{move(data)}
{
    connect(this, &Contact::sendAddMeLater,
            this, &Contact::onSendAddMeLater,
            Qt::QueuedConnection);

    connect(this, &Contact::sendAddmeAckLater,
            this, &Contact::onSendAddmeAckLater,
            Qt::QueuedConnection);

    LFLOG_TRACE << "Contact #" << getId() << " " << getName()
                << " is being constructed.";

}

Contact::~Contact()
{
    LFLOG_TRACE << "Contact #" << getId() << " " << getName()
                << " is being destructed.";

    try {
        clearFileQueues();
        if (isOnline()) {
            disconnectFromContact();
        }
    } catch(const exception& ex) {
        LFLOG_ERROR << "Caught exception: " << ex.what();
    }
}

void Contact::connectToContact()
{
    try {
    if (iBlocked()) {
        LFLOG_DEBUG << "Will not connect to " << getName()
                    << " at " << getAddress()
                    << ": Contact is blocked!";
        return;
    }

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

        prepareForNewConnection();
        connection_ = make_unique<Connection>(peer, *this);
        setOnlineStatus(CONNECTING);

        // The remote side may send its ConnectionRequest before the
        // transport emits connectedToPeer.  Install this before the socket
        // handshake starts so a fast response/request cannot be lost.
        connect(peer.get(), &PeerConnection::addmeRequest,
                this, &Contact::onAddmeRequest, Qt::UniqueConnection);
        connect(peer.get(), &PeerConnection::receivedAvatarRequest,
                this, &Contact::onReceivedAvatarRequest, Qt::UniqueConnection);

        connect(peer.get(), &PeerConnection::connectedToPeer,
                this, &Contact::onConnectedToPeer, Qt::UniqueConnection);

        connect(peer.get(), &PeerConnection::disconnectedFromPeer,
                this, &Contact::onDisconnectedFromPeer, Qt::UniqueConnection);

        getIdentity()->registerConnection(shared_from_this());

    }

        setManuallyDisconnected(false);
    } catch (const std::exception& ex) {
        LFLOG_ERROR << "Failed to connect to Contact " << getName()
                    << ": " << ex.what();
        DsEngine::instance().reportStatus(
            QStringLiteral("Failed to connect to %1: %2").arg(getName(), QString::fromUtf8(ex.what())),
            DsEngine::StatusLevel::ERROR);
        setOnlineStatus(DISCONNECTED);
    } catch (...) {
        LFLOG_ERROR << "Failed to connect to Contact " << getName()
                    << ": unknown exception";
        setOnlineStatus(DISCONNECTED);
    }
}

void Contact::disconnectFromContact(bool manual)
{
    try {
        prepareForNewConnection();
        connection_.reset();
        setOnlineStatus(DISCONNECTED);
        setManuallyDisconnected(manual);
        getIdentity()->unregisterConnection(getUuid());
    } catch (const std::exception& ex) {
        LFLOG_ERROR << "Failed to disconnect Contact " << getName()
                    << ": " << ex.what();
        DsEngine::instance().reportStatus(
            QStringLiteral("Failed to disconnect from %1: %2").arg(getName(), QString::fromUtf8(ex.what())),
            DsEngine::StatusLevel::ERROR);
    } catch (...) {
        LFLOG_ERROR << "Failed to disconnect Contact " << getName()
                    << ": unknown exception";
    }
}

Conversation *Contact::getDefaultConversation()
{
    try {
        // The conversation is cached by the LRU, so it's safe to return the pointer
        return DsEngine::instance().getConversationManager()->getConversation(this).get();
    } catch (const std::exception& ex) {
        LFLOG_ERROR << "Failed to get default conversation for " << getName()
                    << ": " << ex.what();
        DsEngine::instance().reportStatus(
            QStringLiteral("Failed to get conversation for %1: %2")
                .arg(getName(), QString::fromUtf8(ex.what())),
            DsEngine::StatusLevel::ERROR);
        return nullptr;
    } catch (...) {
        LFLOG_ERROR << "Failed to get default conversation for " << getName();
        return nullptr;
    }
}

int Contact::getId() const noexcept {
    return id_;
}

QString Contact::getName() const noexcept  {
    return data_->name;
}

void Contact::setName(QString name) {
    if (name.isEmpty()) {
        name = nullptr;
    }
    updateIf("name", name, data_->name, this, &Contact::nameChanged);
}

QString Contact::getNickName() const noexcept  {
    return data_->nickName;
}

void Contact::setNickName(QString name) {
    if (name.isEmpty()) {
        name = nullptr;
    }
    updateIf("nickname", name, data_->nickName, this, &Contact::nickNameChanged);
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

QString Contact::getBio() const noexcept { return data_->bio; }

bool Contact::hasBio() const noexcept { return !data_->bio.isEmpty(); }

void Contact::setNotes(QString notes) {
    if (notes.isEmpty()) {
        notes = nullptr;
    }
    updateIf("notes", notes, data_->notes, this, &Contact::notesChanged);
}

QImage Contact::getAvatar() const noexcept  {
    return data_->avatar;
}

bool Contact::hasAvatar() const noexcept
{
    return !data_->avatar.isNull();
}

QString Contact::getAvatarUrl() const noexcept
{
    if (avatarUrlChanging_) {
        return "";
    }

    return QStringLiteral("image://contact/%1?v=%2")
        .arg(data_->uuid.toString())
        .arg(avatarVersion_);
}

bool Contact::isAvatarSent() const noexcept
{
    return data_->sentAvatar;
}

void Contact::setSentAvatar(const bool value)
{
    updateIf("sent_avatar", value, data_->sentAvatar, this, &Contact::sentAvatarChanged);
}

void Contact::setAvatar(const QImage &avatar) {
    const auto normalized = normalizeAvatar(avatar);
    if (updateIf("avatar", normalized, data_->avatar, this, &Contact::avatarChanged)) {
        ++avatarVersion_;
        avatarUrlChanging_ = true;
        emit avatarUrlChanged();
        avatarUrlChanging_ = false;
        emit avatarUrlChanged();
    }
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
    const auto when = QDateTime::fromSecsSinceEpoch((QDateTime::currentSecsSinceEpoch() / 60) * 60 );
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
    if (updateIf("state", state, data_->state, this, &Contact::stateChanged)) {

        // Just to make sure the icons and states are updated in the UI
        emit blockedChanged();
        emit onlineChanged();
    }
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
    if (isBlocked()) {
        return "qrc:///images/blocked.svg";
    }
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

bool Contact::wasManuallyDisconnected() const noexcept
{
    return data_->manuallyDisconnected;
}

void Contact::setManuallyDisconnected(bool state)
{
    updateIf("manually_disconnected", state, data_->manuallyDisconnected,
             this, &Contact::manuallyDisconnectedChanged);
}

bool Contact::isBlocked() const
{
    return iBlocked() || theyBlocked();
}

bool Contact::iBlocked() const
{
    return data_->isBlocked;
}

bool Contact::theyBlocked() const
{
    return data_->state == ContactState::BLOCKED;
}

void Contact::setBlocked(bool value)
{
    if (updateIf("blocked", value, data_->isBlocked,
             this, &Contact::blockedChanged)) {
        if (isBlocked()) {
            if (isOnline()) {
                sendBlockNotification();
                connection_->peer->close();
            }
        } else {
            if (isAutoConnect()) {
                connectToContact();
            }
        }

        emit onlineIconChanged();
    }
}

bool Contact::getSendBlockNotice() const
{
    return data_->sendBlockNotice;
}

void Contact::setSendBlockNotice(bool value)
{
    updateIf("notify_blocked", value, data_->sendBlockNotice,
             this, &Contact::sendBlockNoticeChanged);
    data_->sendBlockNotice = value;
}

void Contact::queueMessage(const Message::ptr_t &message)
{
    loadMessageQueue();

    if (message->getDirection() == Message::OUTGOING) {
        messageQueue_.push_back(message);
        message->setState(Message::MS_QUEUED);
        procesMessageQueue();
    }
}

void Contact::queueFile(const std::shared_ptr<File> &file)
{
    loadFileQueue();

    // Send offer or start transfer, depending on direction
    fileQueue_.push_back(file);
    processFilesQueue();
}

void Contact::sendAvatar(const QImage &avatar)
{
    if (!isOnline()
            || (getState() != ACCEPTED && !getIdentity()->getPublicAvatar())
            || (getState() != ACCEPTED && !peerUserInfo_.publicAvatar)) {
        return;
    }

    const auto normalized = normalizeAvatar(avatar);
    const auto hash = avatarHash(normalized);
    if (hash.isEmpty() || hash == lastSentAvatarHash_) {
        return;
    }

    // TODO: Check if the connection is ready to receive data
    connection_->peer->sendAvatar(normalized);
    lastSentAvatarHash_ = hash;
    sentAvatarPendingAck_ = true;
}

Contact::ptr_t Contact::load(const QUuid &key)
{
    QSqlQuery query;

    enum Fields {
        id, identity, uuid, name, nickname, cert, address, notes, contact_group, avatar, created, initiated_by, last_seen, state, addme_message, auto_connect, hash, peer_verified, manually_disconnected, download_path, sent_avatar, blocked, notify_blocked, bio, bio_hash
    };

    query.prepare("SELECT "
                  "id, identity, uuid, name, nickname, cert, address, notes, contact_group, avatar, created, initiated_by, last_seen, state, addme_message, auto_connect, hash, peer_verified, manually_disconnected, download_path, sent_avatar, blocked, notify_blocked, bio, bio_hash "
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
    // Contacts store the public signing key only. The encryption public key
    // is derived by createFromPubkey(). Identity certificates are the only
    // records that contain private certificate material.
    data->cert = DsCert::createFromPubkey(query.value(cert).toByteArray());
    data->address = query.value(address).toByteArray();
    data->avatar = QImage::fromData(query.value(avatar).toByteArray());
    data->created = query.value(created).toDateTime();
    data->whoInitiated = static_cast<InitiatedBy>(query.value(initiated_by).toInt());
    data->lastSeen = query.value(last_seen).toDateTime();
    data->state = static_cast<ContactState>(query.value(state).toInt());
    data->addMeMessage = query.value(addme_message).toString();
    data->autoConnect = query.value(auto_connect).toBool();
    data->peerVerified = query.value(peer_verified).toBool();
    data->manuallyDisconnected = query.value(manually_disconnected).toBool();
    data->downloadPath = query.value(download_path).toString();
    data->sentAvatar = query.value(sent_avatar).toBool();
    data->isBlocked = query.value(blocked).toBool();
    data->sendBlockNotice = query.value(notify_blocked).toBool();
    data->bio = query.value(bio).toString();
    data->bioHash = query.value(bio_hash).toByteArray();

    return make_shared<Contact>(query.value(id).toInt(),
                               false, //Contacts that are connected are always in memory
                               move(data));
}

void Contact::addToDb()
{
    QSqlQuery query;

    query.prepare("INSERT INTO contact ("
                  "identity, uuid, name, nickname, cert, address, notes, contact_group, avatar, created, initiated_by, last_seen, state, addme_message, auto_connect, hash, peer_verified, manually_disconnected, download_path, blocked, notify_blocked, bio, bio_hash "
                  ") VALUES ("
                  ":identity, :uuid, :name, :nickname, :cert, :address, :notes, :contact_group, :avatar, :created, :initiated_by, :last_seen, :state, :addme_message, :auto_connect, :hash, :peer_verified, :manually_disconnected, :download_path, :blocked, :notify_blocked, :bio, :bio_hash "
                  ")");

    if (data_->group.isEmpty()) {
        data_->group = tr("Default");
    }

    if (!data_->created.isValid()) {
        data_->created = QDateTime::fromSecsSinceEpoch((QDateTime::currentSecsSinceEpoch() / 60) * 60 );
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
    query.bindValue(":manually_disconnected", data_->manuallyDisconnected);
    query.bindValue(":download_path", data_->downloadPath);
    query.bindValue(":blocked", data_->isBlocked);
    query.bindValue(":notify_blocked", data_->sendBlockNotice);
    query.bindValue(":bio", data_->bio);
    query.bindValue(":bio_hash", data_->bioHash);

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
        deleteFromDb(data_->uuid);
    }
}

void Contact::deleteFromDb(const QUuid& uuid)
{
    auto db = QSqlDatabase::database();
    if (!db.transaction()) {
        throw Error(QStringLiteral("SQL Failed to start contact deletion: %1")
                        .arg(db.lastError().text()));
    }

    try {
        QSqlQuery query(db);

        // Conversations currently store their participant UUIDs as text. A
        // contact's conversations and messages therefore need explicit
        // cleanup until participants are normalized into a join table.
        query.prepare("DELETE FROM message WHERE conversation_id IN "
                      "(SELECT id FROM conversation WHERE participants=:uuid)");
        query.bindValue(":uuid", uuid.toString());
        if (!query.exec()) {
            throw Error(QStringLiteral("SQL Failed to delete contact messages: %1")
                            .arg(query.lastError().text()));
        }

        query.prepare("DELETE FROM file WHERE contact_id IN "
                      "(SELECT id FROM contact WHERE uuid=:uuid) "
                      "OR conversation_id IN "
                      "(SELECT id FROM conversation WHERE participants=:uuid)");
        query.bindValue(":uuid", uuid.toString());
        if (!query.exec()) {
            throw Error(QStringLiteral("SQL Failed to delete contact files: %1")
                            .arg(query.lastError().text()));
        }

        query.prepare("DELETE FROM notification WHERE contact IN "
                      "(SELECT id FROM contact WHERE uuid=:uuid)");
        query.bindValue(":uuid", uuid);
        if (!query.exec()) {
            throw Error(QStringLiteral("SQL Failed to delete contact notifications: %1")
                            .arg(query.lastError().text()));
        }

        query.prepare("DELETE FROM conversation WHERE participants=:uuid");
        query.bindValue(":uuid", uuid.toString());
        if (!query.exec()) {
            throw Error(QStringLiteral("SQL Failed to delete contact conversations: %1")
                            .arg(query.lastError().text()));
        }

        query.prepare("DELETE FROM contact WHERE uuid=:uuid");
        query.bindValue(":uuid", uuid);
        if (!query.exec()) {
            throw Error(QStringLiteral("SQL Failed to delete contact: %1")
                            .arg(query.lastError().text()));
        }

        if (!db.commit()) {
            throw Error(QStringLiteral("SQL Failed to commit contact deletion: %1")
                            .arg(db.lastError().text()));
        }
    } catch (...) {
        db.rollback();
        throw;
    }
}

void Contact::onConnectedToPeer(const std::shared_ptr<PeerConnection>& peer)
{
    LFLOG_DEBUG << "Connection " << peer->getConnectionId().toString()
                << " to Contact " << getName()
                << " on Identity " << getIdentity()->getName()
                << " is successfully established.";

    getIdentity()->registerConnection(shared_from_this());
    setManuallyDisconnected(false); // No longer relevant
    sentAvatarPendingAck_ = false; // No longer relevant
    userInfoSentOnConnection_ = false;
    lastSentAvatarHash_.clear();

    // A request ID belongs to one connection.  If a new connection replaces
    // the connection on which the Addme was sent, the request must be sent
    // again on the new connection instead of being suppressed as a duplicate.
    if (connection_ && connection_->peer != peer) {
        addMeDispatchQueued_ = false;
        outgoingConnectionRequestId_ = 0;
    }

    if (getState() == ContactState::BLOCKED) {
        LFLOG_DEBUG << "Peer " << getName() << " was BLOCKED.";
        if (isPeerVerified()) {
            setState(ContactState::ACCEPTED);
        } else {
            setState(ContactState::PENDING);
        }
    }

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
        if (probeOnly_) {
            break;
        }
        if (getWhoInitiated() == ME) {
            if (!addMeDispatchQueued_ && outgoingConnectionRequestId_ == 0) {
                addMeDispatchQueued_ = true;
                emit sendAddMeLater();
            } else {
                LFLOG_TRACE << "Suppressing duplicate queued Addme for Contact "
                            << getName();
            }
        } else {
            emit sendAddmeAckLater();
        }
        break;
    case ACCEPTED:
        scheduleProcessOnlineLater();
        break;
    case REJECTED:
        // Allow the connection, but only accept addme or addme/ack events.
        LFLOG_DEBUG << "Peer " << getName() << " rejected us but is now connecting. Will he send an addme?";
        break;
    case BLOCKED:
        // We should not get here...
        LFLOG_DEBUG << "Peer " << getName() << " is BLOCKED. Disconnecting.";
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

        prepareForNewConnection();
        connection_ = make_unique<Connection>(peer, *this);
    }

    if (connection_->peer->getDirection() == PeerConnection::INCOMING) {
        // For outgoing connections, we already have a connect()
        connect(peer.get(), &PeerConnection::disconnectedFromPeer,
                this, &Contact::onDisconnectedFromPeer, Qt::UniqueConnection);
    }

    connect(connection_->peer.get(), &PeerConnection::receivedConnectionResponse,
            this, &Contact::onReceivedConnectionResponse, Qt::UniqueConnection);
    connect(connection_->peer.get(), &PeerConnection::receivedMessageReceipt,
            this, &Contact::onReceivedMessageReceipt, Qt::UniqueConnection);
    connect(connection_->peer.get(), &PeerConnection::receivedFileOfferResponse,
            this, &Contact::onReceivedFileOfferResponse, Qt::UniqueConnection);
    connect(connection_->peer.get(), &PeerConnection::receivedFileCancel,
            this, &Contact::onReceivedFileCancel, Qt::UniqueConnection);
    connect(connection_->peer.get(), &PeerConnection::receivedFileResume,
            this, &Contact::onReceivedFileResume, Qt::UniqueConnection);
    connect(connection_->peer.get(), &PeerConnection::receivedFileResumeResult,
            this, &Contact::onReceivedFileResumeResult, Qt::UniqueConnection);

    connect(connection_->peer.get(), &PeerConnection::addmeRequest,
            this, &Contact::onAddmeRequest, Qt::UniqueConnection);
    connect(connection_->peer.get(), &PeerConnection::receivedAvatarRequest,
            this, &Contact::onReceivedAvatarRequest, Qt::UniqueConnection);

    connect(connection_->peer.get(), &PeerConnection::receivedMessage,
            this, &Contact::onReceivedMessage, Qt::UniqueConnection);

    connect(connection_->peer.get(), &PeerConnection::receivedUserInfo,
            this, &Contact::onReceivedUserInfo, Qt::UniqueConnection);
    connect(connection_->peer.get(), &PeerConnection::receivedBio,
            this, &Contact::onReceivedBio, Qt::UniqueConnection);
    connect(connection_->peer.get(), &PeerConnection::receivedBioRequest,
            this, &Contact::onReceivedBioRequest, Qt::UniqueConnection);

    connect(connection_->peer.get(), &PeerConnection::receivedAvatar,
            this, &Contact::onReceivedAvatar, Qt::UniqueConnection);

    connect(connection_->peer.get(), &PeerConnection::receivedAvatarResponse,
            this, &Contact::onReceivedAvatarResponse, Qt::UniqueConnection);

    connect(connection_->peer.get(), &PeerConnection::receivedFileOffer,
            this, &Contact::onReceivedFileOffer, Qt::UniqueConnection);

    connect(connection_->peer.get(), &PeerConnection::outputBufferEmptied,
            this, &Contact::onOutputBufferEmptied, Qt::UniqueConnection);

    setOnlineStatus(ONLINE);
    touchLastSeen();

    // A newly added contact may expose a public bio before accepting the
    // contact request. Pull it opportunistically; the peer may return an
    // empty response according to its security policy.
    if (getState() != ACCEPTED) {
        try {
            connection_->peer->requestBio();
            if (probeOnly_) {
                connection_->peer->requestAvatar();
            }
        } catch (const std::exception& ex) {
            LFLOG_DEBUG << "Bio pull for unapproved Contact was not sent: " << ex.what();
        }
    }
}

void Contact::onIncomingPeer(const std::shared_ptr<PeerConnection> &peer)
{
    if (iBlocked() && !(getState() == ContactState::ACCEPTED && getSendBlockNotice())) {
        LFLOG_DEBUG << "Peer " << getName() << " is BLOCKED. Disconnecting";
        peer->authorize(false);
        return;
    }

    // Application records can arrive before the transport emits
    // connectedToPeer (the server authorizes the stream after the first
    // record has already been read).  Install the approval handler before
    // authorizing the peer or a fast ConnectionRequest can be lost.
    connect(peer.get(), &PeerConnection::addmeRequest,
            this, &Contact::onAddmeRequest, Qt::UniqueConnection);
    connect(peer.get(), &PeerConnection::receivedAvatarRequest,
            this, &Contact::onReceivedAvatarRequest, Qt::UniqueConnection);

    connect(peer.get(), &PeerConnection::connectedToPeer,
            this, &Contact::onConnectedToPeer, Qt::UniqueConnection);

    peer->authorize(true);
}

void Contact::onDisconnectedFromPeer(const std::shared_ptr<PeerConnection>& peer)
{
    LFLOG_DEBUG << "Connection " << peer->getConnectionId().toString()
                << " to Contact " << getName()
                << " is disconnected.";

    // Ignore a delayed disconnect from a connection that has already been
    // replaced.  Its request ID must not invalidate the active connection.
    if (connection_ && connection_->peer != peer) {
        return;
    }

    connection_.reset();
    addMeDispatchQueued_ = false;
    outgoingConnectionRequestId_ = 0;
    setOnlineStatus(DISCONNECTED);
    clearFileQueues();
    getIdentity()->unregisterConnection(getUuid());
}

void Contact::onSendAddMeLater()
{
    addMeDispatchQueued_ = false;

    if (getOnlineStatus() != ONLINE) {
        return;
    }

    if (outgoingConnectionRequestId_ != 0) {
        LFLOG_TRACE << "Suppressing duplicate Addme for Contact " << getName()
                    << "; request " << outgoingConnectionRequestId_
                    << " is already outstanding";
        return;
    }

    if (auto identity = getIdentity()) {
        AddmeReq ar{identity->getUuid(),
                    connection_->peer->getConnectionId(),
                    identity->getName(),
                    getAddMeMessage(),
                    identity->getPublicAvatar()
                        ? avatarHash(normalizeAvatar(identity->getAvatar())) : QByteArray{}};

        LFLOG_NOTICE << "Sending Addme from Identity " << identity->getName()
                     << " to Contact "
                     << getName()
                     << " over connection " << ar.connection.toString();

        try {
            outgoingConnectionRequestId_ = getIdentity()->getProtocolManager().sendAddme(ar);
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
            if (pendingConnectionRequestId_ != 0) {
                connection_->peer->sendConnectionResponse(
                    pendingConnectionRequestId_, ConnectionResponseStatus::Accepted);
                pendingConnectionRequestId_ = 0;
            } else {
                LFLOG_TRACE << "Suppressing duplicate Addme Ack for Contact "
                            << getName();
                return;
            }
        } catch(const std::exception& ex) {
            LFLOG_WARN << "Failed to send addme Ack from " << identity->getName()
                        << " to "
                        << getName()
                        << " over connection " << connection_->peer->getConnectionId().toString()
                        << ":" << ex.what();
            return;
        }

        setState(ACCEPTED);
        scheduleProcessOnlineLater();
    }
}

void Contact::processOnlineLater()
{
    if (getOnlineStatus() != ONLINE) {
        return;
    }

    if (iBlocked()) {
        if (isOnline() && getSendBlockNotice()) {
            sendBlockNotification();
            connection_->peer->close();
        } else {
            disconnectFromContact();
        }
        return;
    }

    if (getState() != ACCEPTED) {
        return;
    }

    assert(connection_);

    sendUserInfo();
    loadMessageQueue();
    loadFileQueue();
    onOutputBufferEmptied();
}

void Contact::sendBlockNotification()
{
    if (connection_ && isOnline() && pendingConnectionRequestId_ != 0) {
        connection_->peer->sendConnectionResponse(
            pendingConnectionRequestId_, ConnectionResponseStatus::Blocked);
    }
}

#if 0
void Contact::legacyAckAdapterRemoved(const LegacyAck &ack)
{
    touchLastSeen();

    // An authenticated Addme from a contact that the user has already
    // accepted must not be downgraded to Pending because an older database
    // record lacks the peerVerified flag.
    if (getState() == ACCEPTED) {
        LFLOG_NOTICE << "Sending accepted response for Contact " << getName()
                     << " on request connection "
                     << req.peer->getConnectionId().toString();
        req.peer->sendConnectionResponse(req.requestId,
                                         ConnectionResponseStatus::Accepted);
        return;
    }

    if (!isPeerVerified()) {
        LFLOG_DEBUG << "Received Ack from unverified peer on connection "
                    << ack.connectionId.toString();
        ack.peer->close();
        return;
    }

    if (iBlocked()) {
        LFLOG_DEBUG << "Received Ack from blocked peer on connection "
                    << ack.connectionId.toString();
        ack.peer->close();
        return;
    }

    if (ack.what == "AddMe") {
        if (ack.status == "Added") {
            setState(ACCEPTED);
            scheduleProcessOnlineLater();
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
    }if (ack.what == "SetAvatar") {
        sentAvatarPendingAck_ = false;
        if (ack.status == "Request") {
            sendAvatar(getIdentity()->getAvatar());
        } else if (ack.status == "Accepted") {
            LFLOG_TRACE << "Contact " << getName()
                        << " accepted the avatar sent from Identity "
                        << getIdentity()->getName();
            setSentAvatar(true);
        } else {
            LFLOG_TRACE << "Contact " << getName()
                        << " rejected the avatar sent from Identity "
                        << getIdentity()->getName();
            setSentAvatar(false);
        }
    } else if (ack.what == "Message") {
        // The message must exist.
        // The message must belong in an existing conversation
        // The conversation must relate to this contact
        // The message-state must not be MS_REJECTED

        const auto messageId = QByteArray::fromBase64(ack.data.value("data").toString().toUtf8());
        if (messageId.isEmpty()) {
            LFLOG_WARN << "Received ack with empty or invalid message-id: " << messageId.toHex();
            return;
        }

        auto message = DsEngine::instance().getMessageManager()->getMessage(messageId, Message::OUTGOING);
        if (!message) {
            LFLOG_WARN << "Received ack for non-existing message " << messageId.toHex();
            return;
        }

        if (auto conversation = message->getConversation()) {
            if (!conversation->haveParticipant(*this)) {
                LFLOG_WARN << "Received ack for message #" << message->getId()
                           << " that belonds to another contacts conversation: "
                           << conversation->getUuid().toString();
                return;
            }
        } else {
            LFLOG_WARN << "Received ack for message #" << message->getId()
                       << " with non-existing conversation: "
                       << conversation->getUuid().toString();
            return;
        }

        if (message->getState() == Message::MS_REJECTED) {
             LFLOG_DEBUG << "Received ack for already rejected message " << messageId.toHex().toHex();
             return;
        }

        message->touchSentReceivedTime();

        if (ack.status == "Received") {
            message->setState(Message::MS_RECEIVED);
        } else if (ack.status == "Rejected" || ack.status == "Rejected-Encoding") {
            message->setState(Message::MS_REJECTED);
        }
    } else if (ack.what == "IncomingFile") {
        // The file must exist.
        // The file must belong to an existing conversation
        // The conversation must relate to this contact
        // The file-status must not be FS_CANCELLED

        const auto fileId = QByteArray::fromBase64(ack.data.value("data").toString().toUtf8());
        if (fileId.isEmpty()) {
            LFLOG_WARN << "Received ack with empty or invalid file-id: " << fileId.toHex();
            return;
        }

        if (ack.status == "ChunkAck") {
            bool sequenceOk = false;
            bool offsetOk = false;
            const auto sequence = ack.data.value("sequence").toString().toULongLong(&sequenceOk);
            const auto offset = ack.data.value("offset").toString().toLongLong(&offsetOk);
            const auto final = ack.data.value("final").toString() == "1";
            auto outgoing = DsEngine::instance().getFileManager()->getFileFromId(
                fileId, File::OUTGOING);
            if (!sequenceOk || !offsetOk || !outgoing
                    || outgoing->getContactId() != getId()) {
                LFLOG_WARN << "Invalid file chunk acknowledgement from " << getName();
                ack.peer->close();
                return;
            }
            ack.peer->legacyFileAckAdapterRemoved(fileId, sequence, offset, final);
            return;
        }

        auto file = DsEngine::instance().getFileManager()->getFileFromId(fileId, *this);
        if (!file) {
            LFLOG_WARN << "Received ack for non-existing file " << fileId.toHex();
            return;
        }

        if (file->getState() == File::FS_CANCELLED) {
            LFLOG_DEBUG << "Ignoring ack for cancelled file #" << file->getId();

            if (ack.status == "Proceed" || ack.status == "Resume") {
                sendFileCancel(file->getFileId(), FileCancelReason::UserAction);
            }
            return; // The transfer is cancelled.
        }

        if (auto conversation = file->getConversation()) {
            if (!conversation->haveParticipant(*this)) {
                LFLOG_WARN << "Received ack for file #" << file->getId()
                           << " that belonds to another contacts conversation: "
                           << conversation->getUuid().toString();
                return;
            }
        } else {
            LFLOG_WARN << "Received ack for file #" << file->getId()
                       << " with non-existing conversation: "
                       << conversation->getUuid().toString();
            return;
        }

        file->touchAckTime();

        if (ack.status == "Received") {
            file->setState(File::FS_OFFERED);
        } else if (ack.status == "Rejected") {
            file->setState(File::FS_REJECTED);
        } else if (ack.status == "Failed") {
            if (file->getState() == File::FS_TRANSFERRING) {
                file->transferFailed("Peer reported that the tranfer Failed");
            } else {
                file->setState(File::FS_FAILED);
            }
        } else if (ack.status == "Abort") {
            if (file->getState() == File::FS_TRANSFERRING) {
                file->transferFailed("Cancelled/Aborted by Peer", File::FS_CANCELLED);
            } else {
                file->setState(File::FS_CANCELLED);
            }
        } else if (ack.status == "Proceed" || ack.status == "Resume") {
            if (file->getDirection() != File::OUTGOING) {
                LFLOG_WARN << "Received ack/go-on for file #" << file->getId()
                           << " but the file is not outbound! Failing.";
                file->setState(File::FS_FAILED);
                sendFileCancel(file->getFileId(), FileCancelReason::SourceChanged);
                return;
            }

            if (file->getState() != File::FS_OFFERED) {
                LFLOG_WARN << "Received ack/go-on for file #" << file->getId()
                           << " but the file is not in FS_OFFERED state (state=" << getState()
                           << ")! Failing.";
                file->setState(File::FS_FAILED);
                sendFileCancel(file->getFileId(), FileCancelReason::SourceChanged);
                return;
            }

            bool channelOk = false;
            bool restOk = false;
            const auto channel = ack.data.value("channel").toString().toUInt(&channelOk);
            const auto rest = ack.data.value("rest").toString().toLongLong(&restOk);
            if (channel == 0) {
                LFLOG_WARN << "Received ack for file #" << file->getId()
                           << " with invalid channel-id: "
                           << channel;
                file->setState(File::FS_FAILED);
                sendFileCancel(file->getFileId(), FileCancelReason::SourceChanged);
                return;
            }
            if (!channelOk || !restOk || rest < 0 || rest > file->getSize()
                    || rest % (1024 * 8) != 0) {
                LFLOG_WARN << "Received invalid resume offset for file #" << file->getId();
                file->setState(File::FS_FAILED);
                sendFileCancel(file->getFileId(), FileCancelReason::SourceChanged);
                return;
            }
            file->setChannel(channel);
            file->setBytesTransferred(rest);
            file->setState(File::FS_QUEUED);
            queueFile(file);
        }
    } else if (ack.what == "Hello") {
        if (ack.status == "Blocked") {
            LFLOG_NOTICE << "Contact " << getName()
                         << " has blocked us "
                         << getIdentity()->getName();
            setState(ContactState::BLOCKED);
            disconnectFromContact();
        }
    }
}

#endif

bool Contact::procesMessageQueue()
{
    if (isOnline() && !messageQueue_.empty()) {
        if (messageQueue_.front()->getData().encoding == Message::UTF8
                && !peerUserInfo_.allowUtf8) {
            LFLOG_WARN << "Rejecting outgoing UTF-8 message because the peer disallows UTF-8";
            messageQueue_.front()->setState(Message::MS_REJECTED);
            messageQueue_.erase(messageQueue_.begin());
            return true;
        }
        // Send one message. Wait for the socket's buffer to be clear before proceeding with the next

        // TODO: Check ready status on socket
        try {
            connection_->peer->sendMessage(*messageQueue_.front());
            messageQueue_.front()->setState(Message::MS_SENT);
        } catch(const std::exception& ex) {
            LFLOG_WARN << "Caught exception while sending message: " << ex.what();
            return false;
        }

        unconfirmedMessageQueue_.push_back(move(messageQueue_.front()));
        messageQueue_.erase(messageQueue_.begin());
        return true;
    }

    return false;
}

bool Contact::processFilesQueue()
{
    if (isOnline() && !fileQueue_.empty()) {
        // TODO: Check ready status on socket
        // TODO: Offer up to /n/ files in one message, to pipeline them
        //       and to make metadata leaks less lileky to idnetify file numbers
        //       and file sizes.
        auto file = fileQueue_.front();
        try {
            if (file->getState() == File::FS_QUEUED
                    && transferringFileQueue_.size() >= maxConcurrentTransfers_) {
                return false;
            }

            if (file->getDirection() == File::OUTGOING) {
                if (!peerAllowsMimeType(file->getMimeType())
                        || !peerUserInfo_.allowFiles) {
                    file->setState(File::FS_REJECTED);
                    fileQueue_.erase(fileQueue_.begin());
                    return true;
                }
                switch(file->getState()) {
                case File::FS_WAITING:
                    connection_->peer->offerFile(*file);
                    file->setState(File::FS_OFFERED);
                    break;
                case File::FS_QUEUED:
                    queueTransfer(file);
                    break;
                default:
                    // The file don't belong in the queue.
                    fileQueue_.erase(fileQueue_.begin());
                    return processFilesQueue(); // Try again...
                }

            } else /* File::INCOMING */ {
                switch(file->getState()) {
                case File::FS_QUEUED:
                    queueTransfer(file);
                    break;
                default:
                    // The file don't belong in the queue.
                    fileQueue_.erase(fileQueue_.begin());
                    return processFilesQueue(); // Try again...
                }
            }
        } catch(const std::exception& ex) {
            LFLOG_WARN << "Caught exception while sending file request: " << ex.what();
            return false;
        }

        // At this point the file may already be removed due to state change events'
        fileQueue_.erase(find(fileQueue_.begin(), fileQueue_.end(), file));
        return true;
    }

    return false;
}

bool Contact::processFileBlocks()
{
    try {
    if (transferringFileQueue_.empty()) {
        return false;
    }

    auto start = transferringFileQueue_.begin();
    if (auto last = lastFileBlock_.lock()) {
        start = transferringFileQueue_.upper_bound(last);
        if (start == transferringFileQueue_.end()) {
            start = transferringFileQueue_.begin();
        }
    }

    auto current = start;
    do {
        auto file = *current;
        if (!isOnline()) {
            break;
        }

        if (file->getState() != File::FS_TRANSFERRING) {
            auto next = std::next(current);
            transferringFileQueue_.erase(current);
            if (next == transferringFileQueue_.end()) {
                next = transferringFileQueue_.begin();
            }
            if (transferringFileQueue_.empty()) {
                return false;
            }
            current = next;
            continue;
        }

        if (file->getDirection() != File::OUTGOING) {
            current = std::next(current);
            if (current == transferringFileQueue_.end()) {
                current = transferringFileQueue_.begin();
            }
            continue;
        }

        if (connection_->peer->sendSome(*file) > 0) {
            lastFileBlock_ = file;
            return true;
        }

        if (file->getState() == File::FS_TRANSFERRING) {
            break; // We could not send, and the file is not complete or failed
        }
        current = std::next(current);
        if (current == transferringFileQueue_.end()) {
            current = transferringFileQueue_.begin();
        }
    } while (current != start);

    return false;
    } catch (const std::exception& ex) {
        LFLOG_ERROR << "Exception while processing file blocks for Contact "
                    << getName() << ": " << ex.what();
        return false;
    } catch (...) {
        LFLOG_ERROR << "Unknown exception while processing file blocks for Contact "
                    << getName();
        return false;
    }
}

void Contact::onReceivedConnectionResponse(const PeerConnectionResponse& response)
{
    touchLastSeen();
    if (getState() != WAITING_FOR_ACCEPTANCE || outgoingConnectionRequestId_ == 0
            || response.requestId != outgoingConnectionRequestId_) {
        response.peer->close();
        return;
    }
    outgoingConnectionRequestId_ = 0;
    switch (response.status) {
    case ConnectionResponseStatus::Accepted:
        setState(ACCEPTED);
        scheduleProcessOnlineLater();
        break;
    case ConnectionResponseStatus::Pending:
        setState(WAITING_FOR_ACCEPTANCE);
        response.peer->close();
        break;
    case ConnectionResponseStatus::Rejected:
        setState(REJECTED);
        response.peer->close();
        break;
    case ConnectionResponseStatus::Blocked:
        setState(BLOCKED);
        response.peer->close();
        break;
    }
}

void Contact::onReceivedMessageReceipt(const PeerMessageReceipt& receipt)
{
    touchLastSeen();
    if (getState() != ACCEPTED || receipt.messageId.size() != 32) {
        receipt.peer->close();
        return;
    }
    const auto message = DsEngine::instance().getMessageManager()->getMessage(
        receipt.messageId, Message::OUTGOING);
    if (!message || !message->getConversation()
            || !message->getConversation()->haveParticipant(*this)) {
        receipt.peer->close();
        return;
    }
    message->touchSentReceivedTime();
    message->setState(Message::MS_RECEIVED);
}

void Contact::onReceivedFileOfferResponse(const PeerFileOfferResponse& response)
{
    touchLastSeen();
    if (getState() != ACCEPTED || response.fileId.size() != 16) {
        response.peer->close();
        return;
    }
    const auto file = DsEngine::instance().getFileManager()->getFileFromId(
        response.fileId, File::OUTGOING);
    if (!file || file->getContactId() != getId()) {
        response.peer->close();
        return;
    }
    if (response.status == FileOfferResponseStatus::Rejected) {
        file->setState(File::FS_REJECTED);
        return;
    }
    if (file->getState() == File::FS_OFFERED) {
        file->setBytesTransferred(0);
        file->setState(File::FS_QUEUED);
        queueFile(file);
    }
}

void Contact::onReceivedFileResume(const PeerFileResume& resume)
{
    touchLastSeen();
    if (getState() != ACCEPTED || resume.fileId.size() != 16
            || resume.sha256.size() != crypto_hash_sha256_BYTES) {
        resume.peer->close();
        return;
    }
    const auto file = DsEngine::instance().getFileManager()->getFileFromId(
        resume.fileId, File::OUTGOING);
    if (!file || file->getContactId() != getId() || file->getState() != File::FS_FAILED
            || file->getSize() < 0
            || static_cast<quint64>(file->getSize()) != resume.fileSize
            || file->getHash() != resume.sha256
            || resume.nextOffset > resume.fileSize
            || resume.nextOffset % (8 * 1024) != 0) {
        resume.peer->sendFileResumeResult(resume.requestId, resume.fileId,
                                          FileResumeStatus::Rejected, 0,
                                          FileCancelReason::UserAction);
        return;
    }

    QString reason;
    if (!file->validateSourceSnapshot(reason)) {
        LFLOG_WARN << "Cancelling file resume because the source changed: " << reason;
        resume.peer->sendFileResumeResult(resume.requestId, resume.fileId,
                                          FileResumeStatus::Cancelled, 0,
                                          FileCancelReason::SourceChanged);
        file->setState(File::FS_FAILED);
        return;
    }

    file->setBytesTransferred(static_cast<qlonglong>(resume.nextOffset));
    resume.peer->sendFileResumeResult(resume.requestId, resume.fileId,
                                      FileResumeStatus::Accepted,
                                      resume.nextOffset,
                                      FileCancelReason::UserAction);
    file->setState(File::FS_QUEUED);
    queueTransfer(file);
}

void Contact::onReceivedFileResumeResult(const PeerFileResumeResult& result)
{
    touchLastSeen();
    if (getState() != ACCEPTED || result.fileId.size() != 16) {
        result.peer->close();
        return;
    }
    const auto file = DsEngine::instance().getFileManager()->getFileFromId(
        result.fileId, File::INCOMING);
    if (!file || file->getContactId() != getId() || file->getState() != File::FS_FAILED) {
        result.peer->close();
        return;
    }
    if (result.status == FileResumeStatus::Accepted) {
        if (result.nextOffset > static_cast<quint64>(file->getSize())
                || result.nextOffset % (8 * 1024) != 0) {
            result.peer->close();
            return;
        }
        file->setBytesTransferred(static_cast<qlonglong>(result.nextOffset));
        file->setState(File::FS_QUEUED);
        queueFile(file);
        return;
    }
    if (result.status == FileResumeStatus::Cancelled
            && result.error == FileCancelReason::UserAction) {
        file->setState(File::FS_CANCELLED);
    } else {
        file->setState(File::FS_FAILED);
    }
}

void Contact::onReceivedFileCancel(const PeerFileCancel& cancel)
{
    touchLastSeen();
    if (getState() != ACCEPTED || cancel.fileId.size() != 16) {
        cancel.peer->close();
        return;
    }
    const auto file = DsEngine::instance().getFileManager()->getFileFromId(
        cancel.fileId, *this);
    if (!file) {
        cancel.peer->close();
        return;
    }
    if (file->getState() == File::FS_TRANSFERRING) {
        file->setState(File::FS_CANCELLED);
    } else {
        file->setState(File::FS_CANCELLED);
    }
}

void Contact::onReceivedMessage(const PeerMessage &msg)
{
    if (iBlocked() || (getState() != ACCEPTED)) {
        LFLOG_WARN << "Rejecting message on connection "
                   << msg.peer->getConnectionId().toString()
                   << ". Not in ACCEPTED state or blocked.";

            msg.peer->close();
        return;
    }

    if (DsEngine::instance().getMessageManager()->hasConflictingMessage(msg.data)) {
        LFLOG_WARN << "Rejecting reused MessageId from "
                   << msg.peer->getConnectionId().toString();
        msg.peer->close();
        return;
    }

    auto conversation = getRequestedOrDefaultConversation(
                msg.data.conversation, *msg.peer, "Message",
                msg.data.messageId);

    if (!conversation) {
        return;
    }

    LFLOG_DEBUG << "Accepting incoming message with id: " << msg.data.messageId.toHex()
                << " over connection " << msg.peer->getConnectionId().toString()
                << " to " << getName()
                << " for local delivery to conversation " << conversation->getName();

    conversation->incomingMessage(this, msg.data);
}

void Contact::onReceivedUserInfo(const PeerUserInfo &uinfo)
{
    if (iBlocked()) {
        LFLOG_WARN << "Rejecting user-info on connection "
                   << uinfo.peer->getConnectionId().toString()
                   << ". Not in ACCEPTED state or blocked.";

        uinfo.peer->close();
        return;
    }

    if (getState() != ACCEPTED) {
        // UserInfo is only actionable after both peers have completed Addme,
        // but it may arrive on a parallel connection before the response to
        // our Addme is delivered.  Ignore it without closing the connection;
        // closing here can discard the connection that carries the pending
        // acceptance response.
        LFLOG_TRACE << "Ignoring user-info on connection "
                    << uinfo.peer->getConnectionId().toString()
                    << " until Contact " << getName() << " is ACCEPTED.";
        return;
    }

    if (!validateNick(uinfo.userInfo.nickName)) {
        LFLOG_WARN << "Rejecting user-info on connection "
                   << uinfo.peer->getConnectionId().toString()
                   << ". Invaid nick-name.";

        uinfo.peer->close();
        return;
    }

    setNickName(uinfo.userInfo.nickName);
    const auto oldAvatarHash = avatarHash(normalizeAvatar(getAvatar()));
    expectedAvatarHash_ = uinfo.userInfo.avatarHash;
    peerUserInfo_ = uinfo.userInfo;

    if (peerUserInfo_.bioHash.size() != 0
            && peerUserInfo_.bioHash.size() != QCryptographicHash::hashLength(QCryptographicHash::Sha256)) {
        uinfo.peer->close();
        return;
    }
    if (peerUserInfo_.bioHash != data_->bioHash) {
        try {
            // UserInfo contains only the hash. The separate pull is bounded
            // and is authorized by the peer's public/contact policy.
            uinfo.peer->requestBio();
        } catch (const std::exception& ex) {
            LFLOG_WARN << "Failed to request changed bio from Contact " << getName()
                       << ": " << ex.what();
        }
    }

    LFLOG_TRACE << "Received UserInfo from Contact " << getName()
                << "; publicAvatar=" << peerUserInfo_.publicAvatar
                << ", avatar hash bytes=" << expectedAvatarHash_.size();

    // UserInfo authorizes the peer's avatar and gives us the authorization
    // needed to send ours.  This also initializes the exchange when the
    // avatar was already set before peering.
    if (!userInfoSentOnConnection_) {
        sendUserInfo();
        if (!userInfoSentOnConnection_) {
            return;
        }
    }
    sendAvatar(getIdentity()->getAvatar());

    if (!expectedAvatarHash_.isEmpty() && expectedAvatarHash_ != oldAvatarHash) {
        try {
            LFLOG_TRACE << "Requesting changed avatar from Contact " << getName();
            connection_->peer->requestAvatar();
        } catch (const std::exception& ex) {
            LFLOG_WARN << "Failed to request changed avatar from Contact "
                       << getName() << ": " << ex.what();
        }
    }
}

void Contact::onReceivedBio(const PeerBio &bio)
{
    if (iBlocked() || (getState() != ACCEPTED && !peerUserInfo_.publicBio)) {
        bio.peer->close();
        return;
    }
    const auto sanitized = core::bio::sanitize(bio.bio);
    const auto bytes = sanitized.toUtf8();
    if (!core::bio::withinLimits(sanitized)) {
        bio.peer->close();
        return;
    }
    const auto hash = QCryptographicHash::hash(bytes, QCryptographicHash::Sha256);
    if (!peerUserInfo_.bioHash.isEmpty() && hash != peerUserInfo_.bioHash) {
        bio.peer->close();
        return;
    }
    if (data_->bio != sanitized || data_->bioHash != hash) {
        data_->bio = sanitized;
        data_->bioHash = hash;
        QSqlQuery query;
        query.prepare("UPDATE contact SET bio=:bio, bio_hash=:hash WHERE id=:id");
        query.bindValue(":bio", data_->bio);
        query.bindValue(":hash", data_->bioHash);
        if (id_ > 0 && !query.exec()) {
            LFLOG_WARN << "Failed to save bio for Contact " << getName()
                       << ": " << query.lastError().text();
            return;
        }
        emit bioChanged();
    }
}

void Contact::onReceivedBioRequest(const PeerReq &request)
{
    const auto identity = getIdentity();
    if (!identity || iBlocked()
            || (getState() != ACCEPTED && !identity->getPublicBio())) {
        request.peer->sendBioResponse(request.requestId, {});
        return;
    }
    request.peer->sendBioResponse(request.requestId, identity->getBio());
}

bool Contact::peerAllowsMimeType(const QString& mimeType) const
{
    return mimeAllowed(peerUserInfo_.allowMimeTypes, mimeType);
}

void Contact::onReceivedFileOffer(const PeerFileOffer &msg)
{
    if (iBlocked()) {
        LFLOG_WARN << "Rejecting file on connection "
                   << msg.peer->getConnectionId().toString()
                   << ". Contact is blocked.";

        msg.peer->sendFileOfferResponse(msg.fileId, FileOfferResponseStatus::Rejected);
        msg.peer->close();
        return;
    }
    if (getState() != ACCEPTED) {
        LFLOG_WARN << "Rejecting file on connection "
                   << msg.peer->getConnectionId().toString()
                   << ". Not in ACCEPTED state.";

        msg.peer->sendFileOfferResponse(msg.fileId, FileOfferResponseStatus::Rejected);
        msg.peer->close();
        return;
    }

    const auto identity = getIdentity();
    if (!identity->getAllowFiles()) {
        LFLOG_WARN << "Rejecting file offer because file transfers are disabled by local security preferences";
        msg.peer->sendFileOfferResponse(msg.fileId, FileOfferResponseStatus::Rejected);
        return;
    }
    if (!mimeAllowed(identity->getAllowMimeTypes(), msg.type)) {
        LFLOG_WARN << "Rejecting file offer because MIME type " << msg.type
                   << " is not allowed by local security preferences";
        msg.peer->sendFileOfferResponse(msg.fileId, FileOfferResponseStatus::Rejected);
        return;
    }

    auto conversation = getRequestedOrDefaultConversation(
                msg.conversation, *msg.peer, "IncomingFile", msg.fileId);
    if (!conversation) {
        return;
    }

    LFLOG_DEBUG << "Accepting incoming file offer with id: " << msg.fileId.toHex()
                << " over connection " << msg.peer->getConnectionId().toString()
                << " to " << getName()
                << " for local delivery to conversation " << conversation->getName();

    conversation->incomingFileOffer(this, msg);
}

void Contact::onReceivedAvatar(const PeerSetAvatarReq &avatar)
{
    if ((!probeOnly_ && getState() != ACCEPTED)
            || (!probeOnly_ && getState() != ACCEPTED && !peerUserInfo_.publicAvatar)) {
        LFLOG_WARN << "Rejecting avatar from Contact " << getName()
                   << ": avatar is not authorized";
        avatar.peer->close();
        return;
    }
    const auto receivedHash = avatarHash(avatar.avatar);
    LFLOG_TRACE << "Accepted avatar from Contact " << getName()
                << "; learned hash " << receivedHash.toHex();
    expectedAvatarHash_ = receivedHash;
    peerUserInfo_.avatarHash = receivedHash;
    setAvatar(avatar.avatar);
}

void Contact::onReceivedAvatarRequest(const PeerReq &request)
{
    if (iBlocked() || (getState() != ACCEPTED && !getIdentity()->getPublicAvatar())) {
        request.peer->sendAvatarResponse(request.requestId, {}, false);
        return;
    }
    request.peer->sendAvatarResponse(request.requestId,
                                     getIdentity()->getAvatar(), true);
}

void Contact::onReceivedAvatarResponse(const PeerSetAvatarReq &avatar)
{
    if (!avatar.avatar.isNull()) {
        onReceivedAvatar(avatar);
    }
}

void Contact::onOutputBufferEmptied()
{
    if (isOnline()) {
        if (procesMessageQueue()) {
            return;
        }

        if (processFilesQueue()) {
            return;
        }

        processFileBlocks();
    }
}

void Contact::loadMessageQueue()
{
    if (loadedMessageQueue_) {
        return;
    }

    QSqlQuery query;
    query.prepare("SELECT m.id FROM message AS m LEFT JOIN conversation AS c ON m.conversation_id = c.id WHERE c.participants = :contact AND m.received_time IS NULL ORDER BY m.id");
    query.bindValue(":contact", getUuid().toString());

    if(!query.exec()) {
        throw Error(QStringLiteral("Failed to query message-queue: %1").arg(
                        query.lastError().text()));
    }

    auto messageMgr = DsEngine::instance().getMessageManager();

    while(query.next()) {
        messageQueue_.push_back(messageMgr->getMessage(query.value(0).toInt()));
    }

    if (!messageQueue_.empty()) {
        LFLOG_DEBUG << "Loaded " << messageQueue_.size()
                    << " queued messages for contact "
                    << getName()
                    << " on identity " << getIdentity()->getName();
    }

    loadedMessageQueue_ = true;
}

void Contact::loadFileQueue()
{
    if (loadedFileQueue_) {
        return;
    }

    QSqlQuery query;
    query.prepare("SELECT id FROM file WHERE contact_id=:cid AND ((direction=:out AND state=:waiting) OR (direction=:in AND state=:queued))");
    query.bindValue(":cid", getId());
    query.bindValue(":out", static_cast<int>(File::OUTGOING));
    query.bindValue(":in", static_cast<int>(File::INCOMING));
    query.bindValue(":waiting", static_cast<int>(File::FS_WAITING));
    query.bindValue(":queued", static_cast<int>(File::FS_QUEUED));

    if(!query.exec()) {
        throw Error(QStringLiteral("Failed to query file-queue: %1").arg(
                        query.lastError().text()));
    }

    auto fileMgr = DsEngine::instance().getFileManager();

    while(query.next()) {
        fileQueue_.push_back(fileMgr->getFile(query.value(0).toInt()));
    }

    if (!fileQueue_.empty()) {
        LFLOG_DEBUG << "Loaded " << fileQueue_.size()
                    << " queued files for contact "
                    << getName()
                    << " on identity " << getIdentity()->getName();
    }

    loadedFileQueue_ = true;
}

void Contact::queueTransfer(const std::shared_ptr<File> &file)
{
    if (!isOnline()) {
        return;
    }

    if (transferringFileQueue_.insert(file).second) {
        std::weak_ptr<File> weak = file;
        QMetaObject::Connection conn;
        connect(file.get(), &File::stateChanged,
                this, [this, weak, conn=move(conn)]() {
            if (auto file = weak.lock()) {
                if (file->getState() != File::FS_TRANSFERRING) {
                    transferringFileQueue_.erase(file);
                    disconnect(conn);
                }
            }
        });

        if (file->getDirection() == File::INCOMING) {
            QString path;
            if (!File::findUnusedName(file->getPath(), path)) {
                LFLOG_ERROR << "Rejecting file #" << file->getId()
                            << " from peer " << file->getContact()->getName()
                            << " on identity " << file->getConversation()->getIdentity()->getName()
                            << ": Failes to deduce unused file-name!";

                file->transferFailed("Failed to deduce unused file-name!");
                return;
            }

            if (file->getPath() != path) {
                LFLOG_NOTICE << "Renaming incoming file #" << file->getId()
                             << " from Concact " << file->getContact()->getName()
                             << " to Identity " << file->getConversation()->getIdentity()
                             << " from \"" << file->getPath()
                             << "\" to \"" << path << "\"";

                file->setPath(path);
            }
        }

        connection_->peer->startTransfer(*file);

    }
}

void Contact::clearFileQueues()
{
    fileQueue_.clear();

    // transferringFileQueue_ may be modified by file state change events
    auto tmpTransfers = transferringFileQueue_;

    for(auto& file : tmpTransfers) {
        if (file->getState() == File::FS_TRANSFERRING) {
            // Preserve the verified prefix for the next offer/accept exchange.
            // The sender will advertise the acknowledged offset again.
            file->setState(file->getDirection() == File::OUTGOING
                           ? File::FS_WAITING : File::FS_OFFERED);
        }
    }

    transferringFileQueue_.clear();
    loadedFileQueue_ = false; // No longer loaded
}

void Contact::prepareForNewConnection()
{
    addMeDispatchQueued_ = false;
    outgoingConnectionRequestId_ = 0;

    if (connection_ && connection_->peer) {
        connection_->peer->disableNotifications();
        connection_->peer->close();
    }
}

void Contact::scheduleProcessOnlineLater()
{
    QTimer::singleShot(1000, this, [this]() {
        processOnlineLater();
    });
}

Conversation *Contact::getRequestedOrDefaultConversation(const QByteArray &hash,
                                                         PeerConnection &peer,
                                                         const QString &what,
                                                         const QByteArray& id)
{
    auto conversation = DsEngine::instance().getConversationManager()->getConversation(
                hash, this).get();

    if (!conversation) {
        // Try the default conversation and see if it match the peers conversation hash
        auto defaultHash = Conversation::calculateHash(*this);
        if (!comparesEqual(defaultHash, hash)) {
            LFLOG_WARN << "Rejecting incoming " << what << " on connection "
                       << peer.getConnectionId().toString()
                       << ". Unknown conversation: " << hash.toHex();

            return {};
        }

        LFLOG_DEBUG << "Will create a new default conversation now for contact " << getName()
                    << " at identity " << getIdentity()->getName();

        conversation = getDefaultConversation();
    }

    if (!conversation->haveParticipant(*this)) {
        LFLOG_WARN << "Rejecting message on connection "
                   << peer.getConnectionId().toString()
                   << ". Contact " << getName()
                   << " on Identyty " << getIdentity()->getName()
                   << " is not part of converation " << hash.toHex();

        peer.close();
        return {};
    }

    return conversation;
}

void Contact::onAddmeRequest(const PeerAddmeReq &req)
{
    if (req.requestId == 0) {
        req.peer->close();
        return;
    }

    touchLastSeen();
    LFLOG_TRACE << "Handling Addme for Contact " << getName()
                << " on connection " << req.connectionId.toString()
                << "; local state=" << static_cast<int>(getState())
                << ", peer verified=" << isPeerVerified();

    // ACCEPTED is durable local authorization.  A reconnect, a duplicate
    // request, or a request arriving after the short-lived avatar probe must
    // never turn it back into PENDING.  In particular, do not put this
    // request in pendingConnectionRequestId_: it is a response to an
    // already-approved contact, not a new approval event.
    if (getState() == ACCEPTED) {
        try {
            req.peer->sendConnectionResponse(req.requestId,
                                             ConnectionResponseStatus::Accepted);
            LFLOG_TRACE << "Answered Addme from already accepted Contact "
                        << getName() << " with ACCEPTED on connection "
                        << req.connectionId.toString();
            scheduleProcessOnlineLater();
        } catch (const std::exception& ex) {
            LFLOG_WARN << "Failed to answer repeat Addme from accepted Contact "
                       << getName() << ": " << ex.what();
        }
        return;
    }

    if (!isPeerVerified()) {
        if (connection_ && isOnline()) {
            req.peer->sendConnectionResponse(req.requestId, ConnectionResponseStatus::Pending);
            connection_->peer->close();
            return;
        }
    }

    if (iBlocked()) {
        req.peer->sendConnectionResponse(
            req.requestId,
            getSendBlockNotice() ? ConnectionResponseStatus::Blocked
                                 : ConnectionResponseStatus::Rejected);
        req.peer->close();
        return;
    }

    if (getState() == REJECTED) {
        req.peer->sendConnectionResponse(req.requestId,
                                         ConnectionResponseStatus::Rejected);
        req.peer->close();
        return;
    }

    pendingConnectionRequestId_ = req.requestId;

    if (connection_ && isOnline()) {
        req.peer->sendConnectionResponse(req.requestId, ConnectionResponseStatus::Pending);
        req.peer->close();
    }
}

void Contact::acceptIncomingRequest(const PeerAddmeReq &req)
{
    if (!req.peer || req.requestId == 0) {
        return;
    }

    // The request was authenticated by the identity before it reached the
    // notification model.  Mark the contact verified before attaching an
    // already-connected incoming peer; onConnectedToPeer deliberately rejects
    // unverified incoming connections.
    setPeerVerified(true);
    setState(ACCEPTED);

    // Avatar retrieval may already have closed this request connection.  In
    // that case the durable ACCEPTED state is enough; the normal reconnect
    // path will answer the next Addme request.
    if (!req.peer->isConnected()) {
        return;
    }

    if (!connection_ || connection_->peer != req.peer) {
        onConnectedToPeer(req.peer);
    }

    if (connection_ && isOnline()) {
        connection_->peer->sendConnectionResponse(
            req.requestId, ConnectionResponseStatus::Accepted);
        scheduleProcessOnlineLater();
    }
}

void Contact::sendMessageReceipt(const QByteArray& messageId)
{
    if (isOnline() && getState() == ACCEPTED && messageId.size() == 32) {
        connection_->peer->sendMessageReceipt(messageId);
    }
}

void Contact::sendFileOfferResponse(const QByteArray& fileId,
                                    const FileOfferResponseStatus status)
{
    if (isOnline() && getState() == ACCEPTED) {
        connection_->peer->sendFileOfferResponse(fileId, status);
    }
}

void Contact::sendFileCancel(const QByteArray& fileId, const FileCancelReason reason)
{
    if (isOnline() && getState() == ACCEPTED) {
        connection_->peer->sendFileCancel(fileId, reason);
    }
}

void Contact::resumeFile(File& file)
{
    if (!isOnline() || getState() != ACCEPTED || file.getDirection() != File::INCOMING
            || file.getState() != File::FS_FAILED) {
        return;
    }
    connection_->peer->sendFileResume(file.getFileId(),
                                      static_cast<quint64>(file.getBytesTransferred()),
                                      file.getHash(),
                                      static_cast<quint64>(file.getSize()));
}

bool Contact::validateNick(const QString &nickName)
{
    if (nickName.isEmpty() || (nickName.size() > 32)) {
        return false;
    }

    // TODO: Check for unicode trickery and potential exploits

    return true;
}

void Contact::sendUserInfo()
{
    if (isOnline()) {
        assert(connection_);
        assert(connection_->peer);

        UserInfo ui;
        ui.nickName = getIdentity()->getName();
        const bool avatarVisible = getIdentity()->getPublicAvatar()
                || getState() == ContactState::ACCEPTED;
        ui.avatarHash = avatarVisible
                ? avatarHash(normalizeAvatar(getIdentity()->getAvatar())) : QByteArray{};
        ui.allowUtf8 = getIdentity()->getAllowUtf8();
        ui.allowFiles = getIdentity()->getAllowFiles();
        ui.allowMimeTypes = getIdentity()->getAllowMimeTypes();
        ui.allowReactions = getIdentity()->getAllowReactions();
        ui.publicBio = getIdentity()->getPublicBio();
        ui.publicAvatar = avatarVisible;
        if (ui.publicBio || getState() == ACCEPTED) {
            ui.bioHash = QCryptographicHash::hash(core::bio::sanitize(getIdentity()->getBio()).toUtf8(),
                                                  QCryptographicHash::Sha256);
        }

        try {
            LFLOG_TRACE << "Sending UserInfo to Contact " << getName()
                        << "; publicAvatar=" << ui.publicAvatar
                        << ", avatar hash bytes=" << ui.avatarHash.size();
            connection_->peer->sendUserInfo(ui);
            userInfoSentOnConnection_ = true;
        } catch(const std::exception& ex) {
            LFLOG_DEBUG << "Failed to send suer info: " << ex.what();
        }
    }
}

Contact::Connection::~Connection()
{
    if (owner.getIdentity()) {
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
