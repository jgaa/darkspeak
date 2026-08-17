#include "include/ds/notificationsmodel.h"
#include "ds/crypto.h"


#include <QDateTime>
#include <QBuffer>
#include <QSqlError>
#include <QSqlQuery>
#include <QTimer>

#include <functional>

#include "ds/avatarhash.h"
#include "ds/avatarutil.h"

namespace ds {
namespace models {

using namespace std;
using namespace core;

NotificationsModel::NotificationsModel(QSettings &settings)
{
    Q_UNUSED(settings)

    LFLOG_TRACE << "Creating NotificationsModel and connecting contact-request listener";
    connect(core::DsEngine::instance().getIdentityManager(), &IdentityManager::newContactRequest,
            this, &NotificationsModel::addNotification);
    connect(core::DsEngine::instance().getContactManager(), &ContactManager::contactDeleted,
            this, &NotificationsModel::onContactDeleted);
    connect(core::DsEngine::instance().getFileManager(), &FileManager::fileAdded,
            this, &NotificationsModel::onFileAdded);
    connect(core::DsEngine::instance().getFileManager(), &FileManager::fileStateChanged,
            this, &NotificationsModel::onFileStateChanged);

    refresh();
}

void NotificationsModel::onContactDeleted(const QUuid &uuid)
{
    Q_UNUSED(uuid);
    refresh();
}

void NotificationsModel::refresh()
{
    purgeStaleFileNotifications();
    setQuery(R"(select n.id, n.status, n.type, n.timestamp, n.message, n.data, n.identity, n.contact, i.name as iname, c.name as cname from notification as n left join identity as i on n.identity = i.id left join contact as c on n.contact = c.id order by i.name, c.name, n.priority, n.timestamp)");
    if (lastError().isValid()) {
        LFLOG_ERROR << "Failed to refresh notification model: " << lastError().text();
    } else {
        LFLOG_TRACE << "Refreshed notification model; rows: " << rowCount();
    }
}

void NotificationsModel::purgeStaleFileNotifications()
{
    QSqlQuery query;
    query.prepare("SELECT id, data FROM notification WHERE type=:type");
    query.bindValue(":type", N_FILE);
    if (!query.exec()) {
        LFLOG_WARN << "Failed to inspect file notifications: "
                   << query.lastError().text();
        return;
    }

    QList<int> staleIds;
    while (query.next()) {
        const auto data = core::DsEngine::fromJson(query.value(1).toByteArray());
        const auto fileId = QByteArray::fromBase64(data.value("fileId").toString().toUtf8());
        const auto file = fileId.isEmpty()
                ? core::File::ptr_t{}
                : core::DsEngine::instance().getFileManager()->getFileFromId(
                      fileId, core::File::INCOMING);
        if (!file || file->getState() != core::File::FS_OFFERED) {
            staleIds.push_back(query.value(0).toInt());
        }
    }

    if (staleIds.isEmpty()) {
        return;
    }

    QSqlQuery remove;
    remove.prepare("DELETE FROM notification WHERE id=:id");
    for (const auto id : staleIds) {
        remove.bindValue(":id", id);
        if (!remove.exec()) {
            LFLOG_WARN << "Failed to remove stale file notification #" << id
                       << ": " << remove.lastError().text();
        }
    }
}

void NotificationsModel::deleteRow(const int row)
{
    const auto id = data(index(row, 0), Qt::DisplayRole).toInt();
    if (id > 0) {
        QSqlQuery query;
        query.prepare("DELETE FROM notification WHERE id=:id");
        query.bindValue(":id", id);
        query.exec();
        refresh();
    }
}

bool NotificationsModel::isHashPresent(const QString &hash) const
{
    QSqlQuery query;
    query.prepare("SELECT count(0) FROM notification WHERE hash=:hash");
    query.bindValue(":hash", hash);
    if (!query.exec()) {
        LFLOG_ERROR << "Failed to check for duplicate contact notification: "
                    << query.lastError().text();
        return false;
    }
    return query.next() && (query.value(0).toInt() > 0);
}

void NotificationsModel::deleteFileNotification(const QByteArray& fileId)
{
    QSqlQuery query;
    query.prepare("DELETE FROM notification WHERE type=:type AND data LIKE :fileId");
    query.bindValue(":type", N_FILE);
    query.bindValue(":fileId", QStringLiteral("%")
                    + QString::fromLatin1(fileId.toBase64()) + QStringLiteral("%"));
    query.exec();
    refresh();
}

void NotificationsModel::onFileAdded(const core::File::ptr_t& file)
{
    if (!file || file->getDirection() != File::INCOMING
            || file->getState() != File::FS_OFFERED) {
        return;
    }

    const auto hash = QStringLiteral("incoming-file-")
            + QString::fromLatin1(file->getFileId().toBase64());
    if (isHashPresent(hash)) {
        return;
    }

    QVariantMap data;
    data.insert("fileId", QString::fromLatin1(file->getFileId().toBase64()));
    data.insert("fileName", file->getName());
    data.insert("fileSize", file->getSize());

    QSqlQuery query;
    query.prepare("INSERT INTO notification (status, priority, identity, contact, type, timestamp, message, data, hash) "
                  "VALUES (:status, :priority, :identity, :contact, :type, :timestamp, :message, :data, :hash)");
    query.bindValue(":status", ACTIVE);
    query.bindValue(":priority", NORMAL);
    query.bindValue(":identity", file->getIdentityId());
    query.bindValue(":contact", file->getContactId());
    query.bindValue(":type", N_FILE);
    query.bindValue(":timestamp", QDateTime::currentDateTime());
    query.bindValue(":message", tr("Incoming file"));
    query.bindValue(":data", core::DsEngine::toJson(data));
    query.bindValue(":hash", hash);
    if (query.exec()) {
        refresh();
    }
}

void NotificationsModel::onFileStateChanged(const core::File *file)
{
    if (file && file->getDirection() == File::INCOMING
            && file->getState() != File::FS_OFFERED) {
        deleteFileNotification(file->getFileId());
    }
}


QVariant NotificationsModel::data(const QModelIndex &ix, int role) const
{
    if (!ix.isValid()) {
        return {};
    }

    // Map the QML field name mapping back to normal column based access
    auto model_ix = ix;

    if (role >= H_NICKNAME) {
        const auto data = core::DsEngine::fromJson(QSqlQueryModel::data(index(model_ix.row(), H_DATA), Qt::DisplayRole).toByteArray());
        switch (role) {
            case H_NICKNAME:
                return data.value("nickName").toString();
            case H_HANDLE:
                return data.value("handle").toString();
            case H_ADDRESS:
                return data.value("address").toString();
            case H_FILE_NAME:
                return data.value("fileName").toString();
            case H_FILE_SIZE:
                return data.value("fileSize").toLongLong();
            case H_AVATAR_URL:
                return data.value("avatarUrl").toString();
            default:
                assert(false);
                return {};
        }
    }

    if (role >= Qt::UserRole) {
        model_ix = index(ix.row(), role - Qt::UserRole);
        role = Qt::DisplayRole;
    }

    if (role == Qt::DisplayRole) {
        if (model_ix.column() == H_TIMESTAMP) {
            return QSqlQueryModel::data(index(model_ix.row(), H_TIMESTAMP), Qt::DisplayRole).toDateTime().toString();
        }

        if (model_ix.column() == H_DATA) {
            auto d = core::DsEngine::fromJson(QSqlQueryModel::data(index(model_ix.row(), H_DATA), Qt::DisplayRole).toByteArray());
            return d;
        }
    }

    return QSqlQueryModel::data(model_ix, role);
}

QHash<int, QByteArray> NotificationsModel::roleNames() const
{
    QHash<int, QByteArray> names = {
        {col2Role(H_STATUS), "status"},
        {col2Role(H_TYPE), "type"},
        {col2Role(H_TIMESTAMP), "when"},
        {col2Role(H_MESSAGE), "message"},
        {col2Role(H_IDENTITY), "identityId"},
        {col2Role(H_CONTACT), "contactId"},
        {col2Role(H_IDENTITY_NAME), "identityName"},
        {col2Role(H_CONTACT_NAME), "contactName"},
        {H_NICKNAME, "nickName"},
        {H_HANDLE, "handle"},
        {H_ADDRESS, "address"},
        {H_FILE_NAME, "fileName"},
        {H_FILE_SIZE, "fileSize"},
        {H_AVATAR_URL, "avatarUrl"}
    };

    return names;
}

void NotificationsModel::addNotification(Identity *identity, const core::PeerAddmeReq &req)
{  
    LFLOG_TRACE << "Notification slot invoked for contact request: identity "
                << identity->getName() << " (#" << identity->getId()
                << "), handle " << req.handle << ", request id " << req.requestId;

    QString hash, identityId = QString::number(identity->getId());
    crypto::createHash(hash, {req.handle, req.address, identityId});

    // Keep the live request separately from the persisted notification.  The
    // latter cannot contain the authenticated PeerConnection or request id.
    pendingContactRequests_[hash] = req;

    if (isHashPresent(hash)) {
        QSqlQuery existing;
        existing.prepare("SELECT data FROM notification WHERE hash=:hash");
        existing.bindValue(":hash", hash);
        if (existing.exec() && existing.next()) {
            auto data = core::DsEngine::fromJson(existing.value(0).toByteArray());
            if (req.avatarHash.size() == 32
                    && req.avatarHash != data.value("avatarHash").toByteArray()) {
                data.insert("avatarHash", req.avatarHash);
                data.insert("avatarAttempts", 0);
                data.insert("avatarResolved", false);
                existing.prepare("UPDATE notification SET data=:data WHERE hash=:hash");
                existing.bindValue(":data", core::DsEngine::toJson(data));
                existing.bindValue(":hash", hash);
                existing.exec();
            }
            if (!data.value("avatarResolved").toBool()
                    && data.value("avatarAttempts").toInt() < 3
                    && req.avatarHash.size() == 32) {
                requestNotificationAvatar(identity, req, hash);
            }
        }
        LFLOG_TRACE << "Ignoring duplicate contact notification for identity "
                    << identity->getName();
        return;
    }

    QDateTime when = QDateTime::fromSecsSinceEpoch((QDateTime::currentSecsSinceEpoch() / 60) * 60 );
    QSqlQuery query{core::DsEngine::instance().getDb()};

    QVariantMap data;
    data.insert("address", req.address);
    data.insert("handle", req.handle);
    data.insert("nickName", req.nickName);
    data.insert("avatarHash", req.avatarHash);
    data.insert("avatarAttempts", 0);
    data.insert("avatarResolved", req.avatarHash.isEmpty());
    data.insert("avatarUrl", QString{});

    query.prepare("INSERT INTO notification "
                  "(status, priority, identity, type, timestamp, message, data, hash) "
                  "VALUES "
                  "(:status, :priority, :identity, :type, :timestamp, :message, :data, :hash)");
    query.bindValue(":status", ACTIVE);
    query.bindValue(":priority", NORMAL);
    query.bindValue(":identity", identity->getId());
    query.bindValue(":type", N_ADDME);
    query.bindValue(":timestamp", when);
    query.bindValue(":message", req.message);
    query.bindValue(":data", core::DsEngine::toJson(data));
    query.bindValue(":hash", hash);

    if (!query.exec()) {
        LFLOG_ERROR << "Failed to insert contact notification for identity "
                    << identity->getName() << ": " << query.lastError().text();
        return;
    }

    LFLOG_TRACE << "Inserted contact notification for identity "
                << identity->getName() << ", database row id "
                << query.lastInsertId().toLongLong();
    refresh();

    if (req.avatarHash.size() == 32) {
        requestNotificationAvatar(identity, req, hash);
    } else {
        req.peer->close();
    }
}

void NotificationsModel::requestNotificationAvatar(core::Identity *identity,
                                                    const core::PeerAddmeReq &req,
                                                    const QString &hash)
{
    int storedAttempts = 0;
    QSqlQuery attemptsQuery;
    attemptsQuery.prepare("SELECT data FROM notification WHERE hash=:hash");
    attemptsQuery.bindValue(":hash", hash);
    if (attemptsQuery.exec() && attemptsQuery.next()) {
        storedAttempts = core::DsEngine::fromJson(
            attemptsQuery.value(0).toByteArray()).value("avatarAttempts").toInt();
    }
    const auto attempts = std::make_shared<int>(storedAttempts);
    const auto resolved = std::make_shared<bool>(false);
    const auto request = std::make_shared<std::function<void()>>();

    connect(req.peer.get(), &core::PeerConnection::receivedAvatarResponse,
            this, [this, identity, req, hash, resolved](const core::PeerSetAvatarReq& response) {
        if (*resolved) {
            return;
        }
        *resolved = true;

        QSqlQuery query;
        query.prepare("SELECT data FROM notification WHERE hash=:hash");
        query.bindValue(":hash", hash);
        if (!query.exec() || !query.next()) {
            const auto receivedHash = response.avatar.isNull()
                    ? QByteArray{} : core::avatarHash(core::normalizeAvatar(response.avatar));
            if (!response.avatar.isNull() && receivedHash == req.avatarHash) {
                if (auto contact = identity->contactFromHandle(req.handle)) {
                    contact->setAvatar(response.avatar);
                }
            }
            req.peer->close();
            return;
        }

        auto data = core::DsEngine::fromJson(query.value(0).toByteArray());
        const auto expected = data.value("avatarHash").toByteArray();
        auto received = response.avatar;
        const auto receivedHash = received.isNull()
                ? QByteArray{} : core::avatarHash(core::normalizeAvatar(received));
        if (!received.isNull() && receivedHash == expected) {
            QByteArray encoded;
            QBuffer buffer(&encoded);
            buffer.open(QIODevice::WriteOnly);
            received.save(&buffer, "PNG");
            data.insert("avatar", encoded);
            data.insert("avatarReceivedHash", receivedHash);
            data.insert("avatarUrl", QStringLiteral("data:image/png;base64,")
                        + QString::fromLatin1(encoded.toBase64()));
        }
        data.insert("avatarResolved", true);
        query.prepare("UPDATE notification SET data=:data WHERE hash=:hash");
        query.bindValue(":data", core::DsEngine::toJson(data));
        query.bindValue(":hash", hash);
        query.exec();
        refresh();
        req.peer->close();
    });

    *request = [this, req, hash, attempts, resolved, request]() {
        if (*resolved || *attempts >= 3 || !req.peer->isConnected()) {
            if (!*resolved) {
                req.peer->close();
            }
            return;
        }

        ++*attempts;
        QSqlQuery query;
        query.prepare("SELECT data FROM notification WHERE hash=:hash");
        query.bindValue(":hash", hash);
        if (query.exec() && query.next()) {
            auto data = core::DsEngine::fromJson(query.value(0).toByteArray());
            data.insert("avatarAttempts", *attempts);
            query.prepare("UPDATE notification SET data=:data WHERE hash=:hash");
            query.bindValue(":data", core::DsEngine::toJson(data));
            query.bindValue(":hash", hash);
            query.exec();
        }

        try {
            req.peer->requestAvatar();
        } catch (const std::exception& ex) {
            LFLOG_WARN << "Avatar request failed: " << ex.what();
        }
        QTimer::singleShot(1500 * *attempts, this, [request]() { (*request)(); });
    };
    (*request)();
}

void NotificationsModel::acceptContact(const int row, bool accept)
{
    const auto d = core::DsEngine::fromJson(QSqlQueryModel::data(index(row, H_DATA), Qt::DisplayRole).toByteArray());
    if (d.isEmpty()) {
        return;
    }

    if (accept) {
        auto cr = make_unique<ContactData>();

        cr->identity = data(index(row, H_IDENTITY), Qt::DisplayRole).toInt();


        cr->whoInitiated = Contact::THEM;
        // Accepting the notification is the user's acceptance of the peer;
        // retain that state even if the original request connection has gone
        // away before the next reconnect.
        cr->state = Contact::ACCEPTED;
        // The request connection is intentionally short-lived (it may only
        // have been kept open for avatar retrieval).  An accepted contact
        // must therefore establish the follow-up contact connection itself.
        cr->autoConnect = true;
        // The request was authenticated against the contact certificate.  A
        // later reconnect must not downgrade this accepted contact to Pending
        // merely because the short-lived request connection was closed.
        cr->peerVerified = true;
        cr->name = d.value("nickName").toString();

        cr->identity = data(index(row, H_IDENTITY), Qt::DisplayRole).toInt();

        auto handle =  d.value("handle").toByteArray();
        auto handle_str = handle.toStdString();

        cr->cert = crypto::DsCert::createFromPubkey(crypto::b58tobin_check<QByteArray>(handle_str, 32, {249, 50}));
        cr->address = d.value("address").toByteArray();
        cr->avatar = QImage::fromData(d.value("avatar").toByteArray(), "PNG");

        Contact *added = nullptr;
        try {
            added = DsEngine::instance().getContactManager()->addContact(move(cr));

            const auto identityId = QString::number(
                data(index(row, H_IDENTITY), Qt::DisplayRole).toInt());
            QString hash;
            crypto::createHash(hash, {d.value("handle").toString(),
                                      d.value("address").toString(), identityId});
            if (const auto it = pendingContactRequests_.find(hash);
                    it != pendingContactRequests_.end()) {
                if (added) {
                    added->acceptIncomingRequest(it->second);
                }
                pendingContactRequests_.erase(it);
            }
        } catch(const std::exception& ex) {
            LFLOG_ERROR << "Error adding contact: " << ex.what();
        }

        deleteRow(row);
    } else {
        // TODO: Send reject message and delete the notification
    }
}

void NotificationsModel::acceptFile(const int row, bool accept)
{
    if (data(index(row, H_TYPE), Qt::DisplayRole).toInt() != N_FILE) {
        return;
    }

    const auto json = QSqlQueryModel::data(index(row, H_DATA), Qt::DisplayRole).toByteArray();
    const auto notificationData = core::DsEngine::fromJson(json);
    const auto fileId = QByteArray::fromBase64(
        notificationData.value("fileId").toString().toLatin1());
    auto file = core::DsEngine::instance().getFileManager()->getFileFromId(
        fileId, File::INCOMING);
    if (file) {
        if (accept) {
            file->accept();
        } else {
            file->reject();
        }
    } else {
        deleteRow(row);
    }
}


}} // namespaces
