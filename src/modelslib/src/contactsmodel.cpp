#include <assert.h>

#include "ds/contactsmodel.h"
#include "ds/dsengine.h"
#include "ds/errors.h"
#include "ds/model_util.h"
#include "ds/base58.h"
#include "ds/strategy.h"
#include "ds/manager.h"

#include <QBuffer>
#include <QDateTime>
#include <QDebug>
#include <QSqlRecord>
#include <QUuid>

#include "logfault/logfault.h"

using namespace std;
using namespace ds::core;

// TODO: Handle incoming signals (connected, disconnected, failed;
//      even for connections for other than the selected identity)

namespace ds {
namespace models {

ContactsModel::ContactsModel(QSettings& settings)
    : settings_{settings}
{
    setTable("contact");
    setSort(fieldIndex("name"), Qt::AscendingOrder);
    setEditStrategy(QSqlTableModel::OnFieldChange);

    // We are not attached to an identity yet, so show nothing
    setFilter("id = -1");

    h_id_ = fieldIndex("id");
    h_identity_ = fieldIndex("identity");
    h_uuid_ = fieldIndex("uuid");
    h_name_ = fieldIndex("name");
    h_nickname_ = fieldIndex("nickname");
    h_pubkey_ = fieldIndex("pubkey");
    h_address_ = fieldIndex("address");
    h_notes_= fieldIndex("notes");
    h_group_ = fieldIndex("contact_group");
    h_avatar_ = fieldIndex("avatar");
    h_created_ = fieldIndex("created");
    h_initiated_by = fieldIndex("initiated_by");
    h_last_seen_ = fieldIndex("last_seen");
    h_state_ = fieldIndex("state");
    h_addme_message_ = fieldIndex("addme_message");
    h_autoconnect_ = fieldIndex("autoconnect");
    h_hash_ = fieldIndex("hash");

    connect(&DsEngine::instance(), &DsEngine::contactCreated,
            this, &ContactsModel::onContactCreated, Qt::QueuedConnection);

    connect(&DsEngine::instance(), &DsEngine::connectedTo,
            this, &ContactsModel::onConnectedTo);

    connect(&DsEngine::instance(), &DsEngine::disconnectedFrom,
            this, &ContactsModel::onDisconnectedFrom);

    connect(&DsEngine::instance(), &DsEngine::connectionFailed,
            this, &ContactsModel::onConnectionFailed);

    connect(&DsEngine::instance(), &DsEngine::receivedAck,
            this, &ContactsModel::onReceivedAck);
}

QVariant ContactsModel::data(const QModelIndex &ix, int role) const
{
    if (!ix.isValid()) {
        return {};
    }

    // Map the QML field name mapping back to normal column based access
    auto model_ix = ix;

    // Extra fields, generated fields
    switch(role) {
        case HANDLE_ROLE: {
            auto pkey_data = QSqlTableModel::data(index(model_ix.row(), h_pubkey_), Qt::DisplayRole).toByteArray();
            if (pkey_data.isEmpty()) {
                return {};
            }
            return crypto::DsCert::createFromPubkey(pkey_data)->getB58PubKey();
        }

        case ONLINE_ROLE:
            return getExtra(ix.row())->onlineStatus;

        case ONLINE_ICON_ROLE:
            return getOnlineIcon(ix.row());

        case UNREAD_MESSAGES_ROLE:
            return getExtra(ix.row())->unreadMessages;

        case HAVE_NOTES_ROLE:
            return  QSqlTableModel::data(
                        index(model_ix.row(), h_notes_), Qt::DisplayRole).toString().isEmpty() == true;

        case PENDING_DELIVERIES_ROLE:
            return getExtra(ix.row())->numPendingDeliveries;

        case AVATAR_ROLE:
            // TODO: Implement an image-provider for the avatars
            return "qrc:///images/anonymous.svg";

    };

    if (role >= Qt::UserRole) {
        model_ix = index(ix.row(), role - Qt::UserRole);
        role = Qt::DisplayRole;
    }

    if (role == Qt::DisplayRole) {
        if (model_ix.column() == h_created_) {
            return QSqlTableModel::data(index(model_ix.row(), h_created_), Qt::DisplayRole).toDate().toString();
        } else if (model_ix.column() == h_last_seen_) {
            return QSqlTableModel::data(index(model_ix.row(), h_last_seen_), Qt::DisplayRole).toDateTime().toString();
        }
    }

    return QSqlTableModel::data(model_ix, role);
}

bool ContactsModel::setData(const QModelIndex &ix, const QVariant &value, int role)
{
    return QSqlTableModel::setData(ix, value, role);
}

QHash<int, QByteArray> ContactsModel::roleNames() const
{
    QHash<int, QByteArray> names = {
        {col2Role(h_name_), "name"},
        {col2Role(h_created_), "created"},
        {col2Role(h_address_), "onion"},
        {col2Role(h_nickname_), "nickName"},
        {col2Role(h_notes_), "notes"},
        {col2Role(h_initiated_by), "initiatedBy"},
        {col2Role(h_last_seen_), "lastSeen"},
        {col2Role(h_state_), "state"},
        {col2Role(h_autoconnect_), "autoConnect"},
        {col2Role(h_addme_message_), "addmeMessage"},
        {col2Role(h_hash_), "hash"},

        // Virtual roles
        {HANDLE_ROLE, "handle"},
        {ONLINE_ROLE, "online"},
        {ONLINE_ICON_ROLE, "onlineIcon"},
        {HAVE_NOTES_ROLE, "haveNotes"},
        {AVATAR_ROLE, "avatarImage"},
        {PENDING_DELIVERIES_ROLE, "pendingDeliveries"},
        {UNREAD_MESSAGES_ROLE, "unreadMessages"},
    };

    return names;
}

bool ContactsModel::hashExists(QByteArray hash) const
{
    LFLOG_DEBUG << "Checking if hash exists: " << hash.toBase64();

    QSqlQuery query;
    query.prepare("SELECT 1 from contact where hash=:hash");
    query.bindValue(":hash", hash);
    if(!query.exec()) {
        throw Error(QStringLiteral("SQL query failed: %1").arg(query.lastError().text()));
    }
    return query.next();
}

bool ContactsModel::nameExists(QString name) const
{
    QSqlQuery query(database());
    query.prepare("SELECT 1 from contact where name=:name");
    query.bindValue(":name", name);
    if(!query.exec()) {
        throw Error(QStringLiteral("SQL query failed: %1").arg(query.lastError().text()));
    }
    return query.next();
}

void ContactsModel::onContactCreated(const Contact &contact)
{
    Strategy strategy(*this, QSqlTableModel::OnManualSubmit);
    QSqlRecord rec{DsEngine::instance().getDb().record(tableName())};
    rec.setValue(h_identity_, contact.identity);
    rec.setValue(h_uuid_, contact.uuid);
    if (!contact.nickname.isEmpty()) {
        rec.setValue(h_nickname_, contact.nickname);
    }
    if (contact.group.isEmpty()) {
        rec.setValue(h_group_, QStringLiteral("uncategorized"));
    } else {
        rec.setValue(h_group_, contact.group);
    }
    rec.setValue(h_pubkey_, contact.pubkey);
    rec.setValue(h_address_, contact.address);
    rec.setValue(h_hash_, contact.hash);
    rec.setValue(h_name_, contact.name);
    rec.setValue(h_initiated_by, contact.whoInitiated);
    if (!contact.notes.isEmpty()) {
        rec.setValue(h_notes_, contact.notes);
    }
    if (!contact.avatar.isNull()) {
        rec.setValue(h_avatar_, DsEngine::imageToBytes(contact.avatar));
    }
    rec.setValue(h_created_, QDateTime::currentDateTime());
    rec.setValue(h_state_, contact.state);
    rec.setValue(h_addme_message_, contact.addmeMessage);
    rec.setValue(h_autoconnect_, contact.autoConnect);

    if (!insertRecord(-1, rec) || !submitAll()) {
        qWarning() << "Failed to add contact: " << contact.name
                   << this->lastError().text();
        return;
    }

    LFLOG_DEBUG << "Added contact " << contact.name << " to the database";
}

void ContactsModel::onContactAccepted(const Contact &contact)
{
    LFLOG_DEBUG << "Contact << " << contact.name
                << " was accepted by the user."
                << " I Will now add the contact to the database.";

    onContactCreated(contact);
    if (contact.autoConnect && Manager::instance().isOnline()) {
        const auto id = getIdFromUuid(contact.uuid);
        auto extra = getExtra(id);
        connectTransport(id, *extra);
    }
}

ContactsModel::OnlineStatus ContactsModel::getOnlineStatus(int row) const
{
    if (auto e = getExtra(row)) {
        return e->onlineStatus;
    }

    return DISCONNECTED;
}

void ContactsModel::createContact(const QString &nickName,
                                  const QString &name,
                                  const QString &handle, // b58check encoded pubkey
                                  const QString &address,
                                  const QString &message,
                                  const QString &notes,
                                  bool autoConnect)
{
    assert(identity_ >= 0);

    try {
        ContactReq cr;
        cr.identity = identity_;
        // TODO: Add sequence number after name if we use the nickname and it is in use
        cr.name = name.isEmpty() ? nickName : name;
        cr.nickname = nickName;
        cr.pubkey = crypto::b58tobin_check<QByteArray>(
                    handle.toStdString(), 32, {249, 50});

        cr.address = address.toUtf8();
        cr.notes = notes;
        cr.autoConnect = autoConnect;
        cr.addmeMessage = message;

        DsEngine::instance().createContact(cr);

    } catch (const std::runtime_error&) {
        // TODO: Propagate the error to the UI
        return;
    }
}

void ContactsModel::onIdentityChanged(int identityId)
{
    if (identityId != identity_) {
        const auto old = identity_;
        identity_ = identityId;

        if (identity_ > 0) {
            identityUuid_ = Manager::instance().identitiesModel()->getUuidFromId(identity_);
        } else {
            identityUuid_ = QUuid{};
        }

        setFilter("identity = " + QString::number(identity_));
        select();

        LFLOG_DEBUG << "Changed identity from " << old << " to " << identity_;
        emit identityChanged(old, identity_);
    }
}

void ContactsModel::connectTransport(int row)
{
    doIfValid(row, [this](const QByteArray& id, ExtraInfo& extra) {
        connectTransport(id, extra);
    });
}

void ContactsModel::connectTransport(const QByteArray &id, ExtraInfo& extra)
{
    ConnectData cd;

    QSqlQuery query(database());
    query.prepare("SELECT c.address, c.pubkey, i.uuid from contact c left join identity i on c.identity = i.id where c.id=:id");
    query.bindValue(":id", id.toInt());
    if(!query.exec()) {
        throw Error(QStringLiteral("SQL query failed: %1").arg(query.lastError().text()));
    }
    if (!query.next()) {
        LFLOG_WARN << "Failed to fetch from contact with id=" << id;
        return;
    }

    cd.address = query.value(0).toByteArray();
    cd.contactsPubkey = query.value(1).toByteArray();
    cd.service = query.value(2).toByteArray();
    cd.identitysCert = Manager::instance().identitiesModel()->getCert(cd.service);

    extra.outbound_connection_uuid = DsEngine::instance().getProtocolMgr(
                ProtocolManager::Transport::TOR).connectTo(cd);

    setOnlineStatus(id, CONNECTING);
}

void ContactsModel::disconnectTransport(int row)
{
    doIfValid(row, [this, row](const QByteArray& id, ExtraInfo& extra) {
        Q_UNUSED(id)
        auto uuid = data(index(row, h_uuid_), Qt::DisplayRole).toUuid();

        DsEngine::instance().getProtocolMgr(
                    ProtocolManager::Transport::TOR).disconnectFrom(identityUuid_, uuid);

        extra.outbound_connection_uuid = {};
    });
}

void ContactsModel::onReceivedAck(const PeerAck &ack)
{
    auto id = getIdFromConnectionUuid(ack.connectionId);
    if (id.isEmpty()) {
        return;
    }

    if (ack.what == "AddMe") {
        onAddmeAck(id, ack);
    }
}

// TODO: Call this when we have an incoming connection as well
void ContactsModel::onConnectedTo(const QUuid &uuid,
                                  const ProtocolManager::Direction direction)
{
    auto connection_id = getIdFromConnectionUuid(uuid);
    if (connection_id.isEmpty()) {
        LFLOG_WARN << "Got connect notification for unknow connection-id "
                   << uuid.toString();

        DsEngine::instance().getProtocolMgr(
                    ProtocolManager::Transport::TOR).disconnectFrom(identityUuid_, uuid);
        return;
    }
    setOnlineStatus(connection_id, ONLINE);
    updateLastSeen(connection_id);

    // Send addme request or ack?
    Contact::State state = getState(connection_id);

    if (direction == ProtocolManager::Direction::INCOMING) {
        if (state == Contact::State::PENDING) {
            // We need to connect outwards before we can proceed
            LFLOG_DEBUG << "Disconnecting incoming peer on connection "
                        << uuid.toString()
                        << " because we need to connect outwards in order to confirm it's address.";

            disconnectPeer(connection_id, uuid);
            return;
        }

        if (state == Contact::State::BLOCKED) {
            // We need to connect outwards before we can proceed
            LFLOG_DEBUG << "Disconnecting incoming peer on connection "
                        << uuid.toString()
                        << " because the contact is blocked.";

            disconnectPeer(connection_id, uuid);
            return;
        }
    }

    if (state == Contact::State::PENDING || state == Contact::State::WAITING_FOR_ACCEPTANCE) {
        if (getInitiation(connection_id) == Contact::InitiatedBy::ME) {
            sendAddme(connection_id, uuid);
            if (state == Contact::State::PENDING) {
                assert(direction == ProtocolManager::Direction::OUTBOUND);
                // We have an outbound connection to the peer, so we know that the
                // address and pubkey are valid. We can therefore upgrade the
                // status to waiting.
                // If the send fails, it's not a problem as we will re-try later.
                setState(connection_id, Contact::State::WAITING_FOR_ACCEPTANCE);
            }
            return;
        } else {
            if (direction == ProtocolManager::Direction::OUTBOUND) {
                // We accepted this contact's request
                AckMsg ack {identityUuid_, uuid, "AddMe", "Added"};
                DsEngine::instance().getProtocolMgr(
                            ProtocolManager::Transport::TOR).sendAck(ack);

                // We have an outbound connection to the peer, so we know that the
                // address and pubkey are valid. We can therefore upgrade the
                // status to acepted.
                // If the send fails, it's not a problem as the peer will re-send
                // it's adddme, and we will reply with added.

                setState(connection_id, Contact::State::ACCEPTED);
            }
        }
    }

    // State may have changed
    state = getState(connection_id);
    if (state != Contact::State::ACCEPTED) {
        LFLOG_DEBUG << "Disconnecting peer on connection "
                    << uuid.toString()
                    << " because it is not in accepted state.";

        disconnectPeer(connection_id, uuid);
        return;
    }

    // TODO: Start sending queued messages
}

void ContactsModel::onDisconnectedFrom(const QUuid &uuid)
{
    auto connection_id = getIdFromConnectionUuid(uuid);
    if (!connection_id.isEmpty()) {
        setOnlineStatus(connection_id, DISCONNECTED);
    }
}

void ContactsModel::onConnectionFailed(const QUuid &uuid,
                                       const QAbstractSocket::SocketError &socketError)
{
    Q_UNUSED(socketError)

    auto connection_id = getIdFromConnectionUuid(uuid);
    if (!connection_id.isEmpty()) {
        setOnlineStatus(connection_id, DISCONNECTED);
    }
}

QByteArray ContactsModel::getIdFromRow(const int row) const
{
    auto id = data(index(row, h_id_), Qt::DisplayRole).toByteArray();

    if (id.isEmpty()) {
        LFLOG_WARN << "Unable to get Identity id from row " << row;
    }

    return id;
}

QByteArray ContactsModel::getIdFromUuid(const QUuid &uuid) const
{
    QSqlQuery query(database());
    query.prepare("SELECT id from contact where uuid=:uuid");
    query.bindValue(":uuid", uuid);
    if(!query.exec()) {
        throw Error(QStringLiteral("SQL query failed: %1").arg(query.lastError().text()));
    }
    if (!query.next()) {
        LFLOG_WARN << "Failed to fetch from contact with uuid=" << uuid.toString();
        throw runtime_error("Failed to fetch state from contact");
    }
    return query.value(0).toByteArray();
}

QByteArray ContactsModel::getIdFromConnectionUuid(const QUuid &uuid) const
{
    for(const auto& e : extras_) {
        if (e.second->outbound_connection_uuid == uuid) {
            return e.second->id;
        }
    }

    return {};
}

QUuid ContactsModel::getIdentityUuidFromId(const QByteArray &id) const
{
    QSqlQuery query(database());
    query.prepare("SELECT i.uuid from contact c left join identity i on c.identity = i.id where c.id=:id");
    query.bindValue(":id", id.toInt());
    if(!query.exec()) {
        throw Error(QStringLiteral("SQL query failed: %1").arg(query.lastError().text()));
    }
    if (!query.next()) {
        LFLOG_WARN << "Failed to fetch from contact with id=" << id;
        throw runtime_error("Failed to fetch state from contact");
    }
    return query.value(0).toUuid();
}

int ContactsModel::getRowFromId(const QByteArray &id) const
{
    for(int i = 0 ; i < rowCount(); ++i) {
        if (data(index(i, h_id_), Qt::DisplayRole).toByteArray() == id) {
            return i;
        }
    }

    return -1;
}

ContactsModel::ExtraInfo::ptr_t ContactsModel::getExtra(const int row) const
{
    const auto id = getIdFromRow(row);
    if (id.isEmpty()) {
        return {};
    }

    return getExtra(id);
}

ContactsModel::ExtraInfo::ptr_t ContactsModel::getExtra(const QByteArray &id) const
{
    auto it = extras_.find(id);
    if (it == extras_.end()) {
        auto ptr = make_shared<ExtraInfo>(id);
        extras_[id] = ptr;
        return ptr;
    }

    return it->second;
}

ContactsModel::ExtraInfo::ptr_t ContactsModel::getExtra(const QUuid &uuid) const
{
    for(auto& p : extras_) {
        if (p.second->outbound_connection_uuid == uuid) {
            return p.second;
        }
    }

    throw runtime_error("No such uuid "s + uuid.toString().toStdString());
}

QString ContactsModel::getOnlineIcon(int row) const
{
    Q_UNUSED(row);
    //auto status = getExtra(row)->onlineStatus;
    return "qrc:///images/onion-bw.svg";
}

void ContactsModel::disconnectPeer(const QByteArray &id, const QUuid& connection)
{
    const auto identity_uuid = getIdentityUuidFromId(id);
    DsEngine::instance().getProtocolMgr(
                ProtocolManager::Transport::TOR).disconnectFrom(identity_uuid, connection);
}

void ContactsModel::setOnlineStatus(const QByteArray& id,
                                    ContactsModel::OnlineStatus status)
{
    try {
        auto extra = getExtra(id);
        if (extra->onlineStatus != status) {
            LFLOG_DEBUG << "Contact with id " << extra->id
                        << " changes state from " << extra->onlineStatus
                        << " to " << status;
            extra->onlineStatus = status;
            const auto row = getRowFromId(extra->id);
            if (row >= 0) {
                auto ix = index(row, 0);
                emit dataChanged(ix, ix, { Qt::EditRole,
                                           ONLINE_ROLE,
                                           ONLINE_ICON_ROLE
                                 });
                emit onlineStatusChanged();
            }
        }
    } catch (const std::exception& ex) {
        LFLOG_WARN << "Caught exception: " << ex.what();
    }
}

void ContactsModel::doIfOnline(int row,
                               std::function<void (const QByteArray& id,
                                                   ContactsModel::ExtraInfo& extra)> fn,
                               bool throwIfNot)
{
    doIfValid(row, [&](const QByteArray& id, ContactsModel::ExtraInfo& extra) {
        if (!extra.isOnline()) {
            if (throwIfNot) {
                throw runtime_error("Not online");
            }
            return;
        }

        fn(id, extra);
    }, throwIfNot);
}

void ContactsModel::doIfValid(int row,
                               std::function<void (const QByteArray& id,
                                                   ContactsModel::ExtraInfo& extra)> fn,
                               bool throwIfNot)
{
    doIfValid(row, [&](const QByteArray& id) {
        fn(id, *getExtra(id));
    }, throwIfNot);
}
void ContactsModel::doIfValid(int row, std::function<void (const QByteArray& id)> fn,
                               bool throwIfNot)
{
    auto id = getIdFromRow(row);
    if (id.isEmpty()) {
        if (throwIfNot) {
            throw runtime_error("Invalid row");
        }
        return;
    }

    fn(id);
}

void ContactsModel::updateLastSeen(const QByteArray& id)
{
    QDateTime when = QDateTime::fromTime_t((QDateTime::currentDateTime().toTime_t() / 60) * 60);
    QSqlQuery query(database());
    query.prepare("UPDATE contact set last_seen=:when where id=:id");
    query.bindValue(":id", id.toInt());
    query.bindValue(":when", when);
    if(!query.exec()) {
        throw Error(QStringLiteral("SQL query failed: %1").arg(query.lastError().text()));
    }
    int row = getRowFromId(id);
    if (row >= 0) {
        auto ix = index(row, 0);
        emit dataChanged(ix, ix, {col2Role(h_last_seen_)});
    }
}

Contact::State ContactsModel::getState(const QByteArray& id) const
{
    QSqlQuery query(database());
    query.prepare("SELECT state from contact where id=:id");
    query.bindValue(":id", id.toInt());
    if(!query.exec()) {
        throw Error(QStringLiteral("SQL query failed: %1").arg(query.lastError().text()));
    }
    if (!query.next()) {
        LFLOG_WARN << "Failed to fetch from contact with id=" << id;
        throw runtime_error("Failed to fetch state from contact");
    }

    return static_cast<Contact::State>(query.value(0).toInt());
}

Contact::InitiatedBy ContactsModel::getInitiation(const QByteArray &id) const
{
    QSqlQuery query(database());
    query.prepare("SELECT initiated_by from contact where id=:id");
    query.bindValue(":id", id.toInt());
    if(!query.exec()) {
        throw Error(QStringLiteral("SQL query failed: %1").arg(query.lastError().text()));
    }
    if (!query.next()) {
        LFLOG_WARN << "Failed to fetch from contact with id=" << id;
        throw runtime_error("Failed to fetch state from contact");
    }

    return static_cast<Contact::InitiatedBy>(query.value(0).toInt());
}

void ContactsModel::setState(const QByteArray& id, const Contact::State state)
{
    QSqlQuery query(database());
    query.prepare("UPDATE contact set state=:state where uuid=:id");
    query.bindValue(":id", id.toInt());
    query.bindValue(":state", state);
    if(!query.exec()) {
        throw Error(QStringLiteral("SQL query failed: %1").arg(query.lastError().text()));
    }
    auto row = getRowFromId(id);
    if (row >= 0) {
        auto ix = index(row, 0);
        emit dataChanged(ix, ix, {col2Role(h_state_)});
    }
}

void ContactsModel::sendAddme(const QByteArray &id, const QUuid &connection)
{
    AddmeReq ar{identityUuid_, connection, "", ""};

    QSqlQuery query(database());
    query.prepare("SELECT addme_message from contact where id=:id");
    query.bindValue(":id", id.toInt());
    if(!query.exec() || !query.next()) {
        LFLOG_WARN << "Failed to fetch from contact with id=" << id;
        throw runtime_error("Failed to fetch state from contact");
    }

    ar.message = query.value(0).toString();

    query.prepare("SELECT name from identity where id=:id");
    query.bindValue(":id", identity_);
    if(!query.exec() || !query.next()) {
        LFLOG_WARN << "Failed to fetch from identity with id=" << identity_;
        throw runtime_error("Failed to fetch state from contact");
    }

    ar.nickName = query.value(0).toString();

    DsEngine::instance().getProtocolMgr(
                ProtocolManager::Transport::TOR).sendAddme(ar);
}

void ContactsModel::onAddmeAck(const QByteArray& id, const PeerAck &ack)
{
    auto state = getState(id);

    if (ack.status == "Added") {

        if (state == Contact::State::PENDING) {
            LFLOG_DEBUG << "Disconnecting peer on connection "
                        << ack.connectionId.toString()
                        << " after receicing AddMe/Accepted Ack."
                        << " Out of sequence acceptance. I need to connect to this peer.";
        }
        else if (state == Contact::State::WAITING_FOR_ACCEPTANCE || state == Contact::State::ACCEPTED) {
            LFLOG_DEBUG << "A peer accepted our AddMe request on connection "
                    << ack.connectionId.toString();

            if (state != Contact::State::ACCEPTED) {
                setState(id, Contact::State::ACCEPTED);

                // TODO: Trigger notification
                // TODO: Send outgoing messages
            }

            return;

        } else {
            LFLOG_DEBUG << "Disconnecting peer on connection "
                        << ack.connectionId.toString()
                        << " after receicing AddMe/Accepted Ack."
                        << " Out of sequence acceptance. Our state is: " << state;
        }

    } else if (ack.status == "Pending") {

        if (state == Contact::State::WAITING_FOR_ACCEPTANCE) {
            LFLOG_DEBUG << "Disconnecting peer on connection "
                        << ack.connectionId.toString()
                        << " after receicing AddMe/Pending Ack."
                        << " I am expecting the peer to call back when acceptance is decescided about.";

        } else if (state == Contact::State::PENDING) {
            LFLOG_DEBUG << "Disconnecting peer on connection "
                        << ack.connectionId.toString()
                        << " after receicing AddMe/Pending Ack."
                        << " I need to connect to this peer to validate it's address.";

        } else if (state == Contact::State::ACCEPTED) {
            LFLOG_WARN  << "Disconnecting peer on connection "
                        << ack.connectionId.toString()
                        << " after receicing AddMe/Pending Ack."
                        << " The connection is marked as Accepted."
                        << " I will downgrade the state to waiting.";

            setState(id, Contact::State::WAITING_FOR_ACCEPTANCE);
        } else {
            LFLOG_WARN << "Disconnecting peer on connection "
                        << ack.connectionId.toString()
                        << " after receicing AddMe/Pending Ack when I have state: " << state;
        }
    } else if (ack.status == "Rejected") {
        LFLOG_DEBUG << "Disconnecting peer on connection "
                    << ack.connectionId.toString()
                    << " after receicing AddMe/Pending Ack."
                    << " The connection is marked as Accepted."
                    << " I will marke the state as rejected.";

        setState(id, Contact::State::REJECTED);

        // TODO: Create a notification
    } else {
        LFLOG_DEBUG << "Disconnecting peer on connection "
                    << ack.connectionId.toString()
                    << " after receicing AddMe/Unknown Ack.";
    }

    disconnectPeer(id, ack.connectionId);
}


}} // namnespaces
