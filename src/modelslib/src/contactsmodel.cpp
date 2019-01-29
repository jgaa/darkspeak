#include <assert.h>

#include "ds/contactsmodel.h"
#include "ds/dsengine.h"
#include "ds/errors.h"
#include "ds/model_util.h"
#include "ds/base58.h"
#include "ds/strategy.h"

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
    h_blocked_ = fieldIndex("blocked");
    h_rejected_ = fieldIndex("rejected");
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
            return crypto::DsCert::createFromPubkey(QByteArray::fromBase64(pkey_data))->getB58PubKey();
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
        {col2Role(h_blocked_), "blocked"},
        {col2Role(h_autoconnect_), "autoConnect"},
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
    QSqlQuery query;
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
    rec.setValue(h_pubkey_, contact.pubkey.toBase64());
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
    rec.setValue(h_blocked_, contact.blocked);
    rec.setValue(h_rejected_, contact.rejected);
    rec.setValue(h_autoconnect_, contact.autoConnect);

    if (!insertRecord(-1, rec) || !submitAll()) {
        qWarning() << "Failed to add contact: " << contact.name
                   << this->lastError().text();
        return;
    }

    LFLOG_DEBUG << "Added contact " << contact.name << " to the database";
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
                                  const QString &handle,
                                  const QString &address,
                                  const QString & /*message*/, // TODO: Store it somewhere
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

        setFilter("identity = " + QString::number(identity_));
        select();

        LFLOG_DEBUG << "Changed identity from " << old << " to " << identity_;
        emit identityChanged(old, identity_);
    }
}

void ContactsModel::connectTransport(int row)
{
    doIfValid(row, [this, row](const QByteArray& id, ExtraInfo& extra) {
        auto addr = data(index(row, h_address_), Qt::DisplayRole).toByteArray();

        extra.outbound_connection_uuid = DsEngine::instance().getProtocolMgr(
                    ProtocolManager::Transport::TOR).connectTo(id, addr);

        setOnlineStatus(extra.outbound_connection_uuid, CONNECTING);
    });
}

void ContactsModel::disconnectTransport(int row)
{
    doIfValid(row, [this, row](const QByteArray& id, ExtraInfo& extra) {
        auto addr = data(index(row, h_address_), Qt::DisplayRole).toByteArray();

        DsEngine::instance().getProtocolMgr(
                    ProtocolManager::Transport::TOR).disconnectedFrom(id, addr);

        extra.outbound_connection_uuid = {};
    });
}

void ContactsModel::onConnectedTo(const QByteArray &serviceId, const QUuid &uuid)
{
    Q_UNUSED(serviceId)
    setOnlineStatus(uuid, ONLINE);

    // TODO: Update last seen
    // TODO: Send addme request?
    // TODO: Send queued messages
}

void ContactsModel::onDisconnectedFrom(const QByteArray &serviceId, const QUuid &uuid)
{
    Q_UNUSED(serviceId)
    setOnlineStatus(uuid, DISCONNECTED);
}

void ContactsModel::onConnectionFailed(const QByteArray &serviceId, const QUuid &uuid,
                                       const QAbstractSocket::SocketError &socketError)
{
    Q_UNUSED(serviceId)
    Q_UNUSED(socketError)
    setOnlineStatus(uuid, DISCONNECTED);
}

QByteArray ContactsModel::getIdFromRow(const int row) const
{
    auto id = data(index(row, h_id_), Qt::DisplayRole).toByteArray();

    if (id.isEmpty()) {
        LFLOG_WARN << "Unable to get Identity id from row " << row;
    }

    return id;
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

void ContactsModel::setOnlineStatus(const QUuid &uuid, ContactsModel::OnlineStatus status)
{
    try {
        auto extra = getExtra(uuid);
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


}} // namnespaces
