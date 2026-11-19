#include "ds/textpolicy.h"

#include <memory>
#include <iterator>
#include <array>
#include <algorithm>

#include <QTimer>
#include <QCoroTimer>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QCryptographicHash>
#include <QScopeGuard>

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
#include "ds/mediapreview.h"
#include "ds/incomingtransfer.h"
#include "ds/outgoingtransfer.h"

#include "logfault/logfault.h"

namespace ds {
namespace core {

using namespace std;
using namespace crypto;

namespace  {
dsfm::TransferFailureReason remoteCheckpointFailure(FileCancelReason reason) {
    switch (reason) {
    case FileCancelReason::SourceChanged: return dsfm::TransferFailureReason::RemoteSourceChanged;
    case FileCancelReason::ReceiverCheckpointMismatch: return dsfm::TransferFailureReason::ReceiverCheckpointMismatch;
    case FileCancelReason::UnsupportedTransferVersion: return dsfm::TransferFailureReason::UnsupportedVersion;
    case FileCancelReason::UserAction: return dsfm::TransferFailureReason::UserAction;
    }
    throw Error("Unknown remote checkpoint failure reason");
}
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
    completion_timer_.setSingleShot(true);
    connect(&completion_timer_, &QTimer::timeout, this, [this] {
        for (auto it = completed_replay_requests_.begin(); it != completed_replay_requests_.end();) {
            if (it->second.deadline.hasExpired()) it = completed_replay_requests_.erase(it);
            else ++it;
        }
        updateCompletionTimer();
    });
    connect(&DsEngine::instance().transfers(), &TransferManager::attemptFinished, this, [this](const dsfm::TransferKey &key) {
        if (key.local_scope == getIdentity()->getUuid() && key.remote_scope == getUuid()) processDeferredCompletionHints();
    });
    connect(getIdentity(), &Identity::securityPreferencesChanged, this, [this] {
        emit nickNameChanged();
        emit bioChanged();
    });
    connect(this, &Contact::blockedChanged, this, &Contact::peerPreferencesChanged);
    connect(getIdentity(), &Identity::onlineChanged, this, &Contact::scheduleQueuedConnection);
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
        if (manual) queued_connection_requested_ = false;
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
    const auto *identity = getIdentity();
    return identity && !identity->getAllowUtf8() && !text_policy::permittedAscii(data_->nickName, false)
        ? tr("Name hidden by text security preference") : data_->nickName;
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

QString Contact::getBio() const noexcept
{
    const auto *identity = getIdentity();
    return identity && !identity->getAllowUtf8() && !text_policy::permittedAscii(data_->bio)
        ? tr("Bio hidden by text security preference") : data_->bio;
}

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
    avatar_edited_ = true;
    const auto normalized = normalizeAvatar(avatar);
    if (normalized.isNull() && data_->avatar.isNull() && id_ > 0)
        DsEngine::instance().getFileManager()->storage().saveAvatar(ManagedStorage::contactAvatar(getUuid()), {});
    if (normalized != data_->avatar) {
        data_->avatar = normalized;
        if (id_ > 0) DsEngine::instance().getFileManager()->storage().saveAvatar(ManagedStorage::contactAvatar(getUuid()), normalized);
        emit avatarChanged();
        DsEngine::instance().events().append(EventJournal::AvatarChanged, data_->identity, getId(), EventJournal::Incoming, EventJournal::Success);
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
        emit peerPreferencesChanged();
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
        emit peerPreferencesChanged();
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
            queued_connection_requested_ = false;
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
        requestQueuedConnection();
        procesMessageQueue();
    }
}

void Contact::queueFile(const std::shared_ptr<File> &file)
{
    loadFileQueue();

    // Send offer or start transfer, depending on direction
    if (std::find(fileQueue_.begin(), fileQueue_.end(), file) == fileQueue_.end()) fileQueue_.push_back(file);
    if (file->getDirection() == File::OUTGOING) requestQueuedConnection();
    processFilesQueue();
}

size_t Contact::fileWorkSlots() const
{
    return DsEngine::instance().transfers().active(getIdentity()->getUuid(), getUuid());
}

void Contact::updateCompletionTimer()
{
    if (completed_replay_requests_.empty()) { completion_timer_.stop(); return; }
    qint64 remaining = 30000;
    for (const auto &[id, retry] : completed_replay_requests_) {
        Q_UNUSED(id);
        remaining = std::min(remaining, retry.deadline.remainingTime());
    }
    completion_timer_.start(int(std::max<qint64>(0, remaining)));
}

void Contact::requestQueuedConnection()
{
    if (isBlocked() || getState() != ACCEPTED || getOnlineStatus() != DISCONNECTED) return;
    queued_connection_requested_ = true;
    scheduleQueuedConnection();
}

void Contact::scheduleQueuedConnection()
{
    if (!queued_connection_requested_ || queued_connection_scheduled_) return;
    const auto identity = getIdentity();
    if (!identity || !identity->isOnline()) return;
    queued_connection_scheduled_ = true;
    QTimer::singleShot(0, this, [this] {
        queued_connection_scheduled_ = false;
        if (!queued_connection_requested_) return;
        if (isBlocked() || getState() != ACCEPTED || getOnlineStatus() != DISCONNECTED) {
            queued_connection_requested_ = false;
            return;
        }
        const auto identity = getIdentity();
        if (!identity || !identity->isOnline()) return;
        // One attempt per enqueue burst. Failed attempts do not create a retry loop.
        queued_connection_requested_ = false;
        LFLOG_DEBUG << "Connecting contact for queued outbound work";
        connectToContact();
    });
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

    auto contact = make_shared<Contact>(query.value(id).toInt(), false, move(data));
    std::weak_ptr<Contact> weak = contact;
    DsEngine::instance().getFileManager()->storage().loadAvatar(ManagedStorage::contactAvatar(contact->getUuid()), contact.get(), [weak](QImage image) {
        if (auto contact = weak.lock(); contact && !contact->avatar_edited_ && contact->data_->avatar.isNull() && contact->avatarVersion_ == 0) {
            contact->data_->avatar = std::move(image); ++contact->avatarVersion_;
            emit contact->avatarChanged(); emit contact->avatarUrlChanged();
        }
    });
    return contact;
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
    query.bindValue(":avatar", QVariant{});
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
    if (!data_->avatar.isNull()) DsEngine::instance().getFileManager()->storage().saveAvatar(ManagedStorage::contactAvatar(getUuid()), data_->avatar);

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
    have_peer_preferences_ = false;
    recovery_offers_repeated_ = false;
    emit peerPreferencesChanged();
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
            DsEngine::instance().events().append(EventJournal::ContactRequest, identity->getId(), getId(), EventJournal::Outgoing, EventJournal::Pending);
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

        DsEngine::instance().events().append(EventJournal::ContactDecision, identity->getId(), getId(), EventJournal::Incoming, EventJournal::Accepted);
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


bool Contact::procesMessageQueue()
{
    if (isOnline() && !messageQueue_.empty()) {
        if (!have_peer_preferences_) return false;
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

void Contact::startOutgoingFile(File &file, OutgoingTransferStart start)
{
    if (!connection_ || !isOnline() || !have_peer_preferences_) return;
    auto &engine = DsEngine::instance();
    const auto key = file.checkpointKey(); if (!key || engine.transfers().find(key.value())) return;
    auto owned = engine.getFileManager()->getFile(file.getId());
    const auto submitted = engine.transfers().submit(std::make_shared<OutgoingTransfer>(owned, connection_->peer, start));
    if (!submitted) LFLOG_DEBUG << "Outgoing session admission declined; code=" << int(submitted.error().code);
}

bool Contact::processFilesQueue()
{
    if (!connection_ || !isOnline() || !have_peer_preferences_ || fileQueue_.empty()) return false;
    auto file = fileQueue_.front();
    if (!connection_->peer->checkpointInterval() ||
        (file->getDirection() == File::INCOMING && !file->checkpointInterval())) return rejectUnsupportedCheckpoint(file);
    fileQueue_.pop_front();
    if (file->getDirection() == File::OUTGOING) {
        if (file->getState() == File::FS_WAITING || file->getState() == File::FS_QUEUED)
            startOutgoingFile(*file, OutgoingTransferStart::Offer);
    } else if (file->getState() == File::FS_QUEUED) {
        startIncomingFile(*file, file->pendingId().isNull() && file->storageId().isNull()
            ? IncomingTransferStart::Accept : IncomingTransferStart::Resume);
    }
    return true;
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
    recordPeerEvent(*response.peer, EventJournal::ContactDecision, EventJournal::Outgoing,
        response.status == ConnectionResponseStatus::Accepted ? EventJournal::Accepted
            : response.status == ConnectionResponseStatus::Pending ? EventJournal::Pending : EventJournal::Rejected);
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
    if (!connection_ || connection_->peer != receipt.peer) return;
    touchLastSeen();
    if (getState() != ACCEPTED || receipt.messageId.size() != 32) {
        receipt.peer->close();
        return;
    }
    QSqlQuery manifest;
    manifest.prepare("SELECT 1 FROM file_thumbnail_v1 t JOIN file f ON f.id=t.primary_id WHERE t.manifest_id=? AND f.contact_id=? AND f.direction=0");
    manifest.addBindValue(receipt.messageId); manifest.addBindValue(getId());
    if (manifest.exec() && manifest.next()) return;
    const auto message = DsEngine::instance().getMessageManager()->getMessage(
        receipt.messageId, Message::OUTGOING);
    if (!message || !message->getConversation()
            || !message->getConversation()->haveParticipant(*this)) {
        receipt.peer->close();
        return;
    }
    message->touchSentReceivedTime();
    message->setState(Message::MS_RECEIVED);
    recordPeerEvent(*receipt.peer, EventJournal::MessageReceipt, EventJournal::Incoming, EventJournal::Success, receipt.messageId);
}

void Contact::onReceivedFileOfferResponse(const PeerFileOfferResponse& response)
{
    if (!connection_ || connection_->peer != response.peer) return;
    touchLastSeen();
    const auto file = DsEngine::instance().getFileManager()->getFileFromId(response.fileId, *this, File::OUTGOING);
    if (!file || getState() != ACCEPTED) { response.peer->close(); return; }
    if (file->getState() == File::FS_DONE || file->getState() == File::FS_CANCELLED || file->getState() == File::FS_REJECTED) return;
    const auto key = file->checkpointKey();
    const auto session = key ? DsEngine::instance().transfers().find(key.value()) : nullptr;
    if (!session || session->connection() != response.connectionId) { response.peer->close(); return; }
    if (response.status == FileOfferResponseStatus::Rejected) {
        session->stop(TransferSession::Stop::Rejected);
        return;
    }
    if (!response.checkpoint_attempt || response.checkpoint_attempt->id != session->attempt()) { response.peer->close(); return; }
    TransferSession::Event event; event.kind = TransferSession::Event::Kind::Ready; event.selection = response.checkpoint_attempt;
    event.deadline = QDeadlineTimer{30000};
    session->deliver(response.connectionId, response.checkpoint_attempt->id, std::move(event));
}

void Contact::onReceivedFileResume(const PeerFileResume& resume)
{
    if (!connection_ || connection_->peer != resume.peer) return;
    touchLastSeen();
    const auto file = DsEngine::instance().getFileManager()->getFileFromId(resume.fileId, *this, File::OUTGOING);
    const auto reject = [&] { resume.peer->sendCheckpointResumeResult(resume.requestId, resume.fileId, FileResumeStatus::Rejected); };
    if (!file || getState() != ACCEPTED || iBlocked() || !resume.peer->checkpointInterval() ||
        file->checkpointInterval() != resume.checkpoint_interval || file->failureReason() != int(dsfm::TransferFailureReason::None) ||
        file->getSize() < 0 || quint64(file->getSize()) != resume.fileSize || file->getHash() != resume.sha256 ||
        resume.retained_checkpoints.isEmpty() || resume.retained_checkpoints.size() > dsfm::maximum_retained_checkpoints) { reject(); return; }
    const auto key = file->checkpointKey(); if (!key) { reject(); return; }
    auto &manager = DsEngine::instance().transfers();
    TransferSession::Event event; event.kind = TransferSession::Event::Kind::Resume;
    event.request = resume.requestId; event.candidates = resume.retained_checkpoints;
    event.deadline = QDeadlineTimer{30000};
    if (auto session = manager.find(key.value())) {
        if (session->connection() != resume.connectionId || session->phase() == TransferSession::Phase::Transferring ||
            !session->deliver(resume.connectionId, session->attempt(), std::move(event))) reject();
        return;
    }
    if (file->getState() != File::FS_PAUSED && file->getState() != File::FS_FAILED && file->getState() != File::FS_OFFERED) { reject(); return; }
    auto submitted = manager.submit(std::make_shared<OutgoingTransfer>(file, resume.peer, OutgoingTransferStart::Resume, std::move(event)));
    if (!submitted) reject();
}

bool Contact::rejectUnsupportedCheckpoint(const std::shared_ptr<File> &file)
{
    if (!file || !connection_ || !isOnline() || !have_peer_preferences_) return false;
    if (file->getState() != File::FS_DONE && file->getState() != File::FS_CANCELLED && file->getState() != File::FS_REJECTED) {
        if (file->getDirection() == File::INCOMING) startIncomingFile(*file, IncomingTransferStart::Resume);
        else startOutgoingFile(*file, OutgoingTransferStart::Offer);
    }
    std::erase(fileQueue_, file);
    return true;
}

void Contact::startIncomingFile(File &file, IncomingTransferStart start, std::optional<QDeadlineTimer> deadline)
{
    if (!connection_ || !isOnline() || getState() != ACCEPTED || iBlocked() || !canReceiveFile(file)) return;
    auto owned = DsEngine::instance().getFileManager()->getFile(file.getId());
    const auto key = file.checkpointKey();
    if (!key) return;
    auto &manager = DsEngine::instance().transfers();
    if (manager.find(key.value())) return;
    auto session = std::make_shared<IncomingTransfer>(owned, connection_->peer, start, deadline.value_or(QDeadlineTimer{QDeadlineTimer::Forever}));
    const auto admitted = manager.submit(std::move(session));
    if (!admitted) LFLOG_DEBUG << "Incoming session admission declined; code=" << int(admitted.error().code);
}

void Contact::onReceivedFileResumeResult(const PeerFileResumeResult& result)
{
    if (!connection_ || connection_->peer != result.peer) return;
    touchLastSeen();
    if (getState() != ACCEPTED || result.fileId.size() != 16) {
        result.peer->close();
        return;
    }
    const auto file = DsEngine::instance().getFileManager()->getFileFromId(result.fileId, *this, File::INCOMING);
    if (file) {
        const auto key = file->checkpointKey();
        const auto session = key ? DsEngine::instance().transfers().find(key.value()) : nullptr;
        if (session && session->connection() == result.connectionId) {
            if (result.status == FileResumeStatus::Paused) session->stop(TransferSession::Stop::Paused, result.pause);
            else if (result.status == FileResumeStatus::Cancelled)
                session->stop(result.error == FileCancelReason::UserAction ? TransferSession::Stop::Cancelled : TransferSession::Stop::RemoteFailure,
                              dsfm::TransferPauseReason::None, remoteCheckpointFailure(result.error));
            else {
                TransferSession::Event event; event.kind = TransferSession::Event::Kind::Resume;
                event.request = result.requestId; event.selection = result.checkpoint_attempt;
                session->deliver(result.connectionId, session->attempt(), std::move(event));
            }
            return;
        }
    }
    result.peer->close();
}

void Contact::onReceivedFileCancel(const PeerFileCancel& cancel)
{
    if (!connection_ || connection_->peer != cancel.peer) return;
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
    if (file->getState() == File::FS_DONE || file->getState() == File::FS_REJECTED ||
        file->getState() == File::FS_CANCELLED || file->failureReason() != int(dsfm::TransferFailureReason::None)) return;
    if (file->checkpointInterval() && cancel.reason == FileCancelReason::SourceChanged && file->getDirection() != File::INCOMING) {
        // Only the source can assert its own failure. It never establishes
        // locally proved corruption at the receiving endpoint.
        cancel.peer->close();
        return;
    }
    if (const auto key = file->checkpointKey(); key) {
        if (auto session = DsEngine::instance().transfers().find(key.value())) {
            if (session->connection() != cancel.connectionId) { cancel.peer->close(); return; }
            session->stop(cancel.reason == FileCancelReason::UserAction ? TransferSession::Stop::Cancelled : TransferSession::Stop::RemoteFailure,
                          dsfm::TransferPauseReason::None, remoteCheckpointFailure(cancel.reason));
            return;
        }
    }
    std::shared_ptr<TransferSession> session;
    if (file->getDirection() == File::INCOMING) session = std::make_shared<IncomingTransfer>(file, cancel.peer, IncomingTransferStart::Accept);
    else session = std::make_shared<OutgoingTransfer>(file, cancel.peer, OutgoingTransferStart::Offer);
    session->stop(cancel.reason == FileCancelReason::UserAction ? TransferSession::Stop::Cancelled : TransferSession::Stop::RemoteFailure,
                  dsfm::TransferPauseReason::None, remoteCheckpointFailure(cancel.reason));
    DsEngine::instance().transfers().submit(std::move(session));
}

void Contact::onReceivedMessage(const PeerMessage &msg)
{
    if (!connection_ || connection_->peer != msg.peer) return;
    if (iBlocked() || (getState() != ACCEPTED)) {
        LFLOG_WARN << "Rejecting message on connection "
                   << msg.peer->getConnectionId().toString()
                   << ". Not in ACCEPTED state or blocked.";

        recordPeerEvent(*msg.peer, EventJournal::PolicyRejection, EventJournal::Incoming, EventJournal::Rejected);
        msg.peer->close();
        return;
    }

    if (DsEngine::instance().getMessageManager()->hasConflictingMessage(msg.data)) {
        LFLOG_WARN << "Rejecting reused MessageId from "
                   << msg.peer->getConnectionId().toString();
        recordPeerEvent(*msg.peer, EventJournal::PolicyRejection, EventJournal::Incoming, EventJournal::Rejected);
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

    if (!conversation->incomingMessage(this, msg.data)) {
        msg.peer->close();
        return;
    }
    // Authenticate the complete manifest before dispatch, then admit primary
    // offers first. Thumbnail relationships can reference any primary index.
    for (const bool thumbnails : {false, true}) {
        for (size_t index = 0; index < msg.data.attachments.size(); ++index) {
            const auto &attachment = msg.data.attachments[index];
            if (attachment.thumbnail_for.has_value() != thumbnails) continue;
            PeerFileOffer offer{msg.peer, msg.connectionId, 0, msg.data.conversation,
                attachment.file_id, attachment.name, qlonglong(attachment.size), attachment.mime_type, attachment.sha256};
            offer.manifest_id = msg.data.messageId;
            if (index < size_t(msg.attachment_attempts.size())) offer.checkpoint_attempt = msg.attachment_attempts[index];
            if (attachment.thumbnail_for)
                offer.thumbnail_for = msg.data.attachments[*attachment.thumbnail_for].file_id;
            onReceivedFileOffer(offer);
        }
    }
}

void Contact::onReceivedUserInfo(const PeerUserInfo &uinfo)
{
    if (iBlocked()) {
        LFLOG_WARN << "Rejecting user-info on connection "
                   << uinfo.peer->getConnectionId().toString()
                   << ". Not in ACCEPTED state or blocked.";

        recordPeerEvent(*uinfo.peer, EventJournal::PolicyRejection, EventJournal::Incoming, EventJournal::Rejected);
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

        recordPeerEvent(*uinfo.peer, EventJournal::PolicyRejection, EventJournal::Incoming, EventJournal::Rejected);
        uinfo.peer->close();
        return;
    }

    setNickName(uinfo.userInfo.nickName);
    const auto oldAvatarHash = avatarHash(normalizeAvatar(getAvatar()));
    expectedAvatarHash_ = uinfo.userInfo.avatarHash;
    peerUserInfo_ = uinfo.userInfo;

    if (peerUserInfo_.bioHash.size() != 0
            && peerUserInfo_.bioHash.size() != QCryptographicHash::hashLength(QCryptographicHash::Sha256)) {
        recordPeerEvent(*uinfo.peer, EventJournal::PolicyRejection, EventJournal::Incoming, EventJournal::Rejected);
        uinfo.peer->close();
        return;
    }
    have_peer_preferences_ = true;
    emit peerPreferencesChanged();

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
    repeatRecoveryOffersIfReady();
    procesMessageQueue();
    processFilesQueue();

}

void Contact::onReceivedBio(const PeerBio &bio)
{
    if (iBlocked() || (getState() != ACCEPTED && !peerUserInfo_.publicBio)) {
        recordPeerEvent(*bio.peer, EventJournal::PolicyRejection, EventJournal::Incoming, EventJournal::Rejected);
        bio.peer->close();
        return;
    }
    const auto sanitized = core::bio::sanitize(bio.bio);
    const auto bytes = sanitized.toUtf8();
    if (!core::bio::withinLimits(sanitized)) {
        recordPeerEvent(*bio.peer, EventJournal::PolicyRejection, EventJournal::Incoming, EventJournal::Rejected);
        bio.peer->close();
        return;
    }
    const auto hash = QCryptographicHash::hash(bytes, QCryptographicHash::Sha256);
    if (!peerUserInfo_.bioHash.isEmpty() && hash != peerUserInfo_.bioHash) {
        recordPeerEvent(*bio.peer, EventJournal::PolicyRejection, EventJournal::Incoming, EventJournal::Rejected);
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
        recordPeerEvent(*request.peer, EventJournal::PolicyRejection, EventJournal::Incoming, EventJournal::Rejected);
        request.peer->sendBioResponse(request.requestId, {});
        return;
    }
    request.peer->sendBioResponse(request.requestId, identity->getBio());
}

bool Contact::canSendFiles() const
{
    return !isBlocked() && getState() == ACCEPTED
        && (!have_peer_preferences_ || (peerUserInfo_.allowFiles && !peerUserInfo_.allowMimeTypes.isEmpty()));
}

bool Contact::canSendFileType(const QString &mime_type, const QString &filename) const
{
    return canSendFiles() && (!have_peer_preferences_ || (peerAllowsMimeType(mime_type)
        && (peerUserInfo_.allowUtf8 || text_policy::permittedAscii(filename, false))));
}

bool Contact::canReceiveFile(const File &file) const
{
    const auto *identity = getIdentity();
    if (getState() != ACCEPTED || iBlocked() || !identity || !identity->getAllowFiles() ||
        file.getDirection() != File::INCOMING || file.getContactId() != getId() ||
        file.getIdentityId() != getIdentityId() || !mimeAllowed(identity->getAllowMimeTypes(), file.getMimeType()))
        return false;
    if (!file.isThumbnail()) return true;
    if (file.getMimeType() != "image/jpeg" || file.getSize() <= 0 || file.getSize() > media_preview::max_encoded_bytes)
        return false;
    File::ptr_t primary;
    try {
        primary = DsEngine::instance().getFileManager()->getFileFromId(file.dataThumbnailFor(), *this, File::INCOMING);
    } catch (const std::exception &) {
        LFLOG_WARN << "Scoped preview authorization lookup unavailable";
        return false;
    }
    return primary && primary->getConversationId() == file.getConversationId() && !primary->isThumbnail() &&
        primary->getState() != File::FS_CANCELLED && primary->getState() != File::FS_REJECTED &&
        primary->failureReason() == int(dsfm::TransferFailureReason::None) &&
        ((primary->getMimeType().startsWith("image/") && identity->getPreviewImages()) ||
         (media_preview::isVideoCandidate(primary->getMimeType()) && identity->getPreviewVideo()));
}

bool Contact::canSendText(const QString &text) const
{
    return !isBlocked() && (!have_peer_preferences_ || peerUserInfo_.allowUtf8 || text_policy::permittedAscii(text));
}

bool Contact::peerAllowsMimeType(const QString& mimeType) const
{
    return mimeAllowed(peerUserInfo_.allowMimeTypes, mimeType);
}

void Contact::onReceivedFileOffer(const PeerFileOffer &msg)
{
    if (!connection_ || connection_->peer != msg.peer) return;
    if (iBlocked()) {
        LFLOG_WARN << "Rejecting file on connection "
                   << msg.peer->getConnectionId().toString()
                   << ". Contact is blocked.";

        recordPeerEvent(*msg.peer, EventJournal::PolicyRejection, EventJournal::Incoming, EventJournal::Rejected);
        msg.peer->sendFileOfferResponse(msg.fileId, FileOfferResponseStatus::Rejected);
        recordPeerEvent(*msg.peer, EventJournal::PolicyRejection, EventJournal::Incoming, EventJournal::Rejected);
        msg.peer->close();
        return;
    }
    if (getState() != ACCEPTED) {
        LFLOG_WARN << "Rejecting file on connection "
                   << msg.peer->getConnectionId().toString()
                   << ". Not in ACCEPTED state.";

        recordPeerEvent(*msg.peer, EventJournal::PolicyRejection, EventJournal::Incoming, EventJournal::Rejected);
        msg.peer->sendFileOfferResponse(msg.fileId, FileOfferResponseStatus::Rejected);
        recordPeerEvent(*msg.peer, EventJournal::PolicyRejection, EventJournal::Incoming, EventJournal::Rejected);
        msg.peer->close();
        return;
    }

    const auto identity = getIdentity();
    if (!identity->getAllowFiles()) {
        LFLOG_WARN << "Rejecting file offer because file transfers are disabled by local security preferences";
        recordPeerEvent(*msg.peer, EventJournal::PolicyRejection, EventJournal::Incoming, EventJournal::Rejected);
        msg.peer->sendFileOfferResponse(msg.fileId, FileOfferResponseStatus::Rejected);
        return;
    }
    if (!mimeAllowed(identity->getAllowMimeTypes(), msg.type)) {
        LFLOG_WARN << "Rejecting file offer because MIME type " << msg.type
                   << " is not allowed by local security preferences";
        recordPeerEvent(*msg.peer, EventJournal::PolicyRejection, EventJournal::Incoming, EventJournal::Rejected);
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
        recordPeerEvent(*avatar.peer, EventJournal::PolicyRejection, EventJournal::Incoming, EventJournal::Rejected);
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
        recordPeerEvent(*request.peer, EventJournal::PolicyRejection, EventJournal::Incoming, EventJournal::Rejected);
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
        repeatRecoveryOffersIfReady();
        if (procesMessageQueue()) {
            return;
        }

        if (processFilesQueue()) {
            return;
        }

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
    query.prepare("SELECT id FROM file WHERE contact_id=:cid AND ((direction=:out AND state=:waiting AND NOT EXISTS(SELECT 1 FROM file_thumbnail_v1 t WHERE t.file_id=file.id)) OR (direction=:in AND state=:queued))");
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

void Contact::clearFileQueues()
{
    if (connection_ && connection_->peer) DsEngine::instance().transfers().retireConnection(connection_->peer->getConnectionId());
    fileQueue_.clear();
    completed_replay_requests_.clear();
    updateCompletionTimer();
    loadedFileQueue_ = false;
}

void Contact::prepareForNewConnection()
{
    clearFileQueues();
    have_peer_preferences_ = false;
    recovery_offers_repeated_ = false;
    emit peerPreferencesChanged();
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

    DsEngine::instance().events().append(EventJournal::ContactRequest, data_->identity, getId(), EventJournal::Incoming, EventJournal::Pending);
    if (!isPeerVerified()) {
        if (connection_ && isOnline()) {
            req.peer->sendConnectionResponse(req.requestId, ConnectionResponseStatus::Pending);
            connection_->peer->close();
            return;
        }
    }

    if (iBlocked()) {
        DsEngine::instance().events().append(EventJournal::ContactDecision, data_->identity, getId(), EventJournal::Incoming, EventJournal::Rejected);
        req.peer->sendConnectionResponse(
            req.requestId,
            getSendBlockNotice() ? ConnectionResponseStatus::Blocked
                                 : ConnectionResponseStatus::Rejected);
        req.peer->close();
        return;
    }

    if (getState() == REJECTED) {
        DsEngine::instance().events().append(EventJournal::ContactDecision, data_->identity, getId(), EventJournal::Incoming, EventJournal::Rejected);
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

    req.peer->setEventContext(data_->identity, getId());

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

void Contact::repeatRecoveryOffersIfReady()
{
    if (!connection_ || !isOnline() || !have_peer_preferences_ || recovery_offers_repeated_ ||
        !connection_->peer->checkpointInterval() || getState() != ACCEPTED || iBlocked() ||
        fileWorkSlots() >= maxConcurrentTransfers_) return;
    recovery_offers_repeated_ = true;
    LFLOG_TRACE << "Discovering interrupted file offers for contact #" << getId();
    repeatInterruptedOffers(connection_->peer);
}

void Contact::repeatInterruptedOffers(PeerConnection::ptr_t peer)
{
    if (!connection_ || connection_->peer != peer || !peer->isConnected()) return;
    QSqlQuery query;
    query.prepare("SELECT f.id FROM file f JOIN file_transfer_v1 t ON t.file_id=f.id "
                  "WHERE f.contact_id=? AND f.identity_id=? AND f.direction=? "
                  "AND (f.state=? OR (f.state=? AND t.pause_reason=?)) AND t.failure_reason=0 "
                  "AND NOT EXISTS (SELECT 1 FROM file_thumbnail_v1 x WHERE x.file_id=f.id AND f.state=?) "
                  "ORDER BY f.id DESC LIMIT 2");
    query.addBindValue(getId()); query.addBindValue(getIdentityId()); query.addBindValue(int(File::OUTGOING));
    query.addBindValue(int(File::FS_OFFERED)); query.addBindValue(int(File::FS_PAUSED));
    query.addBindValue(int(dsfm::TransferPauseReason::Disconnected)); query.addBindValue(int(File::FS_OFFERED));
    if (!query.exec()) return;
    while (query.next()) {
        auto file = DsEngine::instance().getFileManager()->getFile(query.value(0).toInt());
        startOutgoingFile(*file, OutgoingTransferStart::Recover);
    }
}

void Contact::deferCompletionHint(File &file, std::optional<QDeadlineTimer> deadline)
{
    if (!connection_ || !isOnline() || !connection_->peer->checkpointInterval() || !canReceiveFile(file) ||
        !file.checkpointInterval() || file.storageId().isNull() ||
        file.failureReason() != int(dsfm::TransferFailureReason::None) ||
        (file.getState() != File::FS_HASHING && file.getState() != File::FS_DONE && !file.isResumable())) return;
    for (auto it = completed_replay_requests_.begin(); it != completed_replay_requests_.end();) {
        if (it->second.deadline.hasExpired()) it = completed_replay_requests_.erase(it);
        else ++it;
    }
    if (completed_replay_requests_.contains(file.getFileId()) || completed_replay_requests_.size() >= maxConcurrentTransfers_) return;
    const auto expires = deadline.value_or(QDeadlineTimer{30000});
    if (expires.hasExpired()) return;
    const auto owned = DsEngine::instance().getFileManager()->getFile(file.getId());
    completed_replay_requests_.emplace(file.getFileId(), CompletionRetry{expires, std::weak_ptr<File>(owned)});
    updateCompletionTimer();
    LFLOG_DEBUG << "Completion hint deferred until publication and retired I/O finish";
}

void Contact::processDeferredCompletionHints()
{
    QVector<QByteArray> ready;
    for (auto it = completed_replay_requests_.begin(); it != completed_replay_requests_.end();) {
        if (!it->second.deferred) { ++it; continue; }
        const auto file = it->second.deferred->lock();
        if (!file || !connection_ || !isOnline() || !connection_->peer->checkpointInterval() ||
            !canReceiveFile(*file) || file->failureReason() != int(dsfm::TransferFailureReason::None) ||
            (file->getState() != File::FS_HASHING && file->getState() != File::FS_DONE && !file->isResumable())) {
            it = completed_replay_requests_.erase(it);
            continue;
        }
        if (file->getState() != File::FS_HASHING) ready.append(it->first);
        ++it;
    }
    for (const auto &id : ready) {
        if (fileWorkSlots() >= maxConcurrentTransfers_) break;
        const auto found = completed_replay_requests_.find(id);
        if (found == completed_replay_requests_.end() || !found->second.deferred) continue;
        const auto file = found->second.deferred->lock();
        if (!file) { completed_replay_requests_.erase(found); continue; }
        if (const auto key = file->checkpointKey(); key && DsEngine::instance().transfers().find(key.value())) continue;
        const auto expires = found->second.deadline;
        completed_replay_requests_.erase(found);
        if (file->getState() == File::FS_DONE) recoverCompletedFile(*file, expires);
        else if (file->isResumable()) recoverPublishedFile(*file, expires);
    }
}

void Contact::recoverPublishedFile(File &file, std::optional<QDeadlineTimer> deadline)
{
    if (!connection_ || !isOnline() || getState() != ACCEPTED || iBlocked() || !connection_->peer->checkpointInterval() ||
        file.getDirection() != File::INCOMING || !file.checkpointInterval() || !file.isResumable() || file.storageId().isNull() ||
        file.getContactId() != getId() || file.getIdentityId() != getIdentityId() || !canReceiveFile(file)) return;
    if (const auto key = file.checkpointKey(); key && DsEngine::instance().transfers().find(key.value())) {
        deferCompletionHint(file, deadline); return;
    }
    for (auto it = completed_replay_requests_.begin(); it != completed_replay_requests_.end();) {
        if (it->second.deadline.hasExpired()) it = completed_replay_requests_.erase(it);
        else ++it;
    }
    if (completed_replay_requests_.contains(file.getFileId()) || completed_replay_requests_.size() >= maxConcurrentTransfers_) return;
    if (fileWorkSlots() >= maxConcurrentTransfers_) { deferCompletionHint(file, deadline); return; }
    const auto expires = deadline.value_or(QDeadlineTimer{30000});
    if (expires.hasExpired()) return;
    completed_replay_requests_.emplace(file.getFileId(), CompletionRetry{expires, {}});
    updateCompletionTimer();
    startIncomingFile(file, IncomingTransferStart::PublishedRecovery, expires);
}


void Contact::acceptCheckpointFile(File &file)
{
    if (getState() != ACCEPTED || iBlocked() || !file.checkpointInterval() ||
        file.getState() != File::FS_OFFERED || file.getDirection() != File::INCOMING ||
        file.getContactId() != getId() || file.getIdentityId() != getIdentityId() || !canReceiveFile(file)) return;
    QString reason;
    if (!DsEngine::instance().getFileManager()->canAcceptIncoming(file, reason)) return;
    if (!connection_ || !isOnline()) { file.queueForTransfer(); return; }
    startIncomingFile(file, IncomingTransferStart::Accept);
}

void Contact::recoverCompletedFile(File &file, std::optional<QDeadlineTimer> deadline)
{
    if (!connection_ || !isOnline() || getState() != ACCEPTED || iBlocked() || !connection_->peer->checkpointInterval() ||
        !file.checkpointInterval() || file.getState() != File::FS_DONE || file.getDirection() != File::INCOMING ||
        file.getContactId() != getId() || file.getIdentityId() != getIdentityId() || !canReceiveFile(file)) return;
    if (const auto key = file.checkpointKey(); key && DsEngine::instance().transfers().find(key.value())) {
        deferCompletionHint(file, deadline); return;
    }
    const auto prior = completed_replay_requests_.find(file.getFileId());
    if (prior != completed_replay_requests_.end()) {
        if (!prior->second.deadline.hasExpired()) return;
        completed_replay_requests_.erase(prior);
    }
    for (auto it = completed_replay_requests_.begin(); it != completed_replay_requests_.end();) {
        if (it->second.deadline.hasExpired()) it = completed_replay_requests_.erase(it);
        else ++it;
    }
    if (completed_replay_requests_.size() >= maxConcurrentTransfers_) return;
    if (fileWorkSlots() >= maxConcurrentTransfers_) { deferCompletionHint(file, deadline); return; }
    const auto expires = deadline.value_or(QDeadlineTimer{30000});
    if (expires.hasExpired()) return;
    completed_replay_requests_.emplace(file.getFileId(), CompletionRetry{expires, {}});
    updateCompletionTimer();
    startIncomingFile(file, IncomingTransferStart::Completed, expires);
}

void Contact::resumeFile(File& file)
{
    if (file.checkpointInterval()) {
        startIncomingFile(file, IncomingTransferStart::Resume);
        return;
    }
    if (file.getDirection() == File::INCOMING && file.isResumable())
        rejectUnsupportedCheckpoint(DsEngine::instance().getFileManager()->getFile(file.getId()));
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
