#include "include/ds/notificationsmodel.h"
#include "ds/crypto.h"


#include <QDateTime>
#include <QSqlError>
#include <QSqlQuery>

namespace ds {
namespace models {

using namespace std;
using namespace core;

NotificationsModel::NotificationsModel(QSettings &settings)
{
    Q_UNUSED(settings)

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
    query.exec();
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
        {H_FILE_SIZE, "fileSize"}
    };

    return names;
}

void NotificationsModel::addNotification(Identity *identity, const core::PeerAddmeReq &req)
{  
    QString hash, identityId = QString::number(identity->getId());
    crypto::createHash(hash, {req.handle, req.address, identityId});

    if (isHashPresent(hash)) {
        LFLOG_DEBUG << "Ignoring duplicate AddMe notification.";
        return;
    }

    QDateTime when = QDateTime::fromSecsSinceEpoch((QDateTime::currentSecsSinceEpoch() / 60) * 60 );
    QSqlQuery query{core::DsEngine::instance().getDb()};

    QVariantMap data;
    data.insert("address", req.address);
    data.insert("handle", req.handle);
    data.insert("nickName", req.nickName);

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

    query.exec();
    refresh();
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
        cr->name = d.value("nickName").toString();

        cr->identity = data(index(row, H_IDENTITY), Qt::DisplayRole).toInt();

        auto handle =  d.value("handle").toByteArray();
        auto handle_str = handle.toStdString();

        cr->cert = crypto::DsCert::createFromPubkey(crypto::b58tobin_check<QByteArray>(handle_str, 32, {249, 50}));
        cr->address = d.value("address").toByteArray();

        try {
            DsEngine::instance().getContactManager()->addContact(move(cr));
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
