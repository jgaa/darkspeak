
#include <memory>

#include "ds/contact.h"
#include "ds/dsengine.h"
#include "ds/identity.h"
#include "ds/errors.h"
#include "ds/update_helper.h"
#include "ds/errors.h"
#include "ds/conversation.h"

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
}

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

    LFLOG_TRACE << "Contact #" << getId() << " " << getName()
                << " is being constructed.";

}

Contact::~Contact()
{
    LFLOG_TRACE << "Contact #" << getId() << " " << getName()
                << " is being destructed.";

    clearFileQueues();
    if (isOnline()) {
        disconnectFromContact();
    }
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

        getIdentity()->registerConnection(shared_from_this());

    }

    setManuallyDisconnected(false);
}

void Contact::disconnectFromContact(bool manual)
{
    connection_.reset();
    setOnlineStatus(DISCONNECTED);
    setManuallyDisconnected(manual);
    getIdentity()->unregisterConnection(getUuid());
}

Conversation *Contact::getDefaultConversation()
{
    // The conversation is cached by the LRU, so it's safe to return the pointer
    return DsEngine::instance().getConversationManager()->getConversation(this).get();
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

QString Contact::getAvatarUrl() const noexcept
{
    if (avatarUrlChanging_) {
        return "";
    }

    if (data_->avatar.isNull()) {
        return "qrc:///images/anonymous.svg";
    }

    return QStringLiteral("image://contact/%1").arg(data_->uuid.toString());
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
    if (updateIf("avatar", avatar, data_->avatar, this, &Contact::avatarChanged)) {
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

bool Contact::wasManuallyDisconnected() const noexcept
{
    return data_->manuallyDisconnected;
}

void Contact::setManuallyDisconnected(bool state)
{
    updateIf("manually_disconnected", state, data_->manuallyDisconnected,
             this, &Contact::manuallyDisconnectedChanged);
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
    if (!isOnline()) {
        return;
    }

    // TODO: Check if the connection is ready to receive data
    connection_->peer->sendAvatar(avatar);
    sentAvatarPendingAck_ = true;
}

Contact::ptr_t Contact::load(QObject& parent, const QUuid &key)
{
    QSqlQuery query;

    enum Fields {
        id, identity, uuid, name, nickname, cert, address, notes, contact_group, avatar, created, initiated_by, last_seen, state, addme_message, auto_connect, hash, peer_verified, manually_disconnected, download_path
    };

    query.prepare("SELECT "
                  "id, identity, uuid, name, nickname, cert, address, notes, contact_group, avatar, created, initiated_by, last_seen, state, addme_message, auto_connect, hash, peer_verified, manually_disconnected, download_path "
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
    data->manuallyDisconnected = query.value(manually_disconnected).toBool();
    data->downloadPath = query.value(download_path).toString();

    return make_shared<Contact>(parent,
                               query.value(id).toInt(),
                               false, //Contacts that are connected are always in memory
                               move(data));
}

void Contact::addToDb()
{
    QSqlQuery query;

    query.prepare("INSERT INTO contact ("
                  "identity, uuid, name, nickname, cert, address, notes, contact_group, avatar, created, initiated_by, last_seen, state, addme_message, auto_connect, hash, peer_verified, manually_disconnected, download_path "
                  ") VALUES ("
                  ":identity, :uuid, :name, :nickname, :cert, :address, :notes, :contact_group, :avatar, :created, :initiated_by, :last_seen, :state, :addme_message, :auto_connect, :hash, :peer_verified, :manually_disconnected, :download_path "
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
    query.bindValue(":manually_disconnected", data_->manuallyDisconnected);
    query.bindValue(":download_path", data_->downloadPath);

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

    getIdentity()->registerConnection(shared_from_this());
    setManuallyDisconnected(false); // No longer relevant
    sentAvatarPendingAck_ = false; // No longer relevant

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

        if (connection_ && connection_->peer) {
            connection_->peer->disableNotifications();
        }
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

    connect(connection_->peer.get(), &PeerConnection::receivedMessage,
            this, &Contact::onReceivedMessage);

    connect(connection_->peer.get(), &PeerConnection::receivedAvatar,
            this, &Contact::onReceivedAvatar);

    connect(connection_->peer.get(), &PeerConnection::receivedFileOffer,
            this, &Contact::onReceivedFileOffer);

    connect(connection_->peer.get(), &PeerConnection::outputBufferEmptied,
            this, &Contact::onOutputBufferEmptied);

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
    clearFileQueues();
    getIdentity()->unregisterConnection(getUuid());
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

    loadMessageQueue();
    loadFileQueue();
    onOutputBufferEmptied();
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
        LFLOG_DEBUG << "Received Ack from blocked peer on connection "
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
    }if (ack.what == "AddAvatar") {
        sentAvatarPendingAck_ = false;
        if (ack.status == "Accepted") {
            LFLOG_TRACE << "Contact " << getName()
                        << " accepted the avatar sent from Identity "
                        << getIdentity()->getName();
        } else {
            LFLOG_TRACE << "Contact " << getName()
                        << " rejected the avatar sent from Identity "
                        << getIdentity()->getName();
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
                           << conversation->getUuid().toString();;
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

        auto file = DsEngine::instance().getFileManager()->getFileFromId(fileId, *this);

        if (file->getState() == File::FS_CANCELLED) {
            LFLOG_DEBUG << "Ignoring ack for cancelled file #" << file->getId();

            if (ack.status == "Proceed" || ack.status == "Resume") {
                sendAck("IncomingFile", "Abort", file->getFileId().toBase64());
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
            // TODO: Handle REST

            if (file->getDirection() != File::OUTGOING) {
                LFLOG_WARN << "Received ack/go-on for file #" << file->getId()
                           << " but the file is not outbound! Failing.";
                file->setState(File::FS_FAILED);
                sendAck("IncomingFile", "Failed", file->getFileId().toBase64());
                return;
            }

            if (file->getState() != File::FS_OFFERED) {
                LFLOG_WARN << "Received ack/go-on for file #" << file->getId()
                           << " but the file is not in FS_OFFERED state (state=" << getState()
                           << ")! Failing.";
                file->setState(File::FS_FAILED);
                sendAck("IncomingFile", "Failed", file->getFileId().toBase64());
                return;
            }

            const auto channel = static_cast<quint32>(ack.data.value("channel").toInt());
            if (channel == 0) {
                LFLOG_WARN << "Received ack for file #" << file->getId()
                           << " with invalid channel-id: "
                           << channel;
                file->setState(File::FS_FAILED);
                sendAck("IncomingFile", "Failed", file->getFileId().toBase64());
                return;
            }
            file->setChannel(channel);
            file->setState(File::FS_QUEUED);
            queueFile(file);
        }
    }
}

bool Contact::procesMessageQueue()
{
    if (isOnline() && !messageQueue_.empty()) {
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
again:
    if (isOnline() && !fileQueue_.empty()) {
        // TODO: Check ready status on socket
        // TODO: Offer up to /n/ files in one message, to pipeline them
        //       and to make metadata leaks less lileky to idnetify file numbers
        //       and file sizes.
        auto file = fileQueue_.front();
        try {
            if (file->getDirection() == File::OUTGOING) {
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
                    goto again; // Yes, goto!
                }

            } else /* File::INCOMING */ {
                switch(file->getState()) {
                case File::FS_QUEUED:
                    queueTransfer(file);
                    break;
                default:
                    // The file don't belong in the queue.
                    fileQueue_.erase(fileQueue_.begin());
                    goto again;
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
again:
    for(auto& file : transferringFileQueue_) {
        if (!isOnline()) {
            break;
        }

        if (file->getState() != File::FS_TRANSFERRING) {
            transferringFileQueue_.erase(file);
            goto again; // Yes, goto
        }

        if (file->getDirection() != File::OUTGOING) {
            continue;
        }

        if (connection_->peer->sendSome(*file) > 0) {
            return true;
        }

        if (file->getState() == File::FS_TRANSFERRING) {
            break; // We could not send, and the file is not complete or failed
        }
    }

    return false;
}

void Contact::onReceivedMessage(const PeerMessage &msg)
{
    if (getState() != ACCEPTED) {
        LFLOG_WARN << "Rejecting message on connection "
                   << msg.peer->getConnectionId().toString()
                   << ". Not in ACCEPTED state.";

        msg.peer->sendAck("Message", "Rejected", msg.data.messageId.toBase64());
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

void Contact::onReceivedFileOffer(const PeerFileOffer &msg)
{
    if (getState() != ACCEPTED) {
        LFLOG_WARN << "Rejecting file on connection "
                   << msg.peer->getConnectionId().toString()
                   << ". Not in ACCEPTED state.";

        msg.peer->sendAck("IncomingFile", "Rejected", msg.fileId.toBase64());
        msg.peer->close();
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
    setAvatar(avatar.avatar);
    avatar.peer->sendAck("SetAvatar", "Accepted");
}

void Contact::onOutputBufferEmptied()
{
    if (isOnline()) {
        if (procesMessageQueue()) {
            return;
        }

        if (!isAvatarSent() && !sentAvatarPendingAck_) {
            sendAvatar(getIdentity()->getAvatar());
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
                QFileInfo fi{path};
                file->setName(fi.fileName());
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
        if (file->getDirection() == File::OUTGOING) {
            file->setState(File::FS_OFFERED);
        } else /* incoming */ {
            file->setState(File::FS_QUEUED);
        }
        file->setChannel(0);
    }

    transferringFileQueue_.clear();
    loadedFileQueue_ = false; // No longer loaded
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

            peer.sendAck(what, "Rejected", id.toBase64());
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

        peer.sendAck(what, "Rejected", id.toBase64());
        peer.close();
        return {};
    }

    return conversation;
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
        connection_->peer->sendAck("AddMe", "Added");
        setState(ACCEPTED);
        emit processOnlineLater();
    }
}

void Contact::sendAck(const QString &what, const QString &status, const QString &data)
{
    if (isOnline()) {
        connection_->peer->sendAck(what, status, data);
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

