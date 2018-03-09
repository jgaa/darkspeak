#include "ds/contactsmodel.h"
#include "ds/dsengine.h"
#include "ds/errors.h"

#include <QBuffer>
#include <QDateTime>
#include <QDebug>
#include <QSqlRecord>
#include <QUuid>

using namespace std;
using namespace ds::core;

namespace ds {
namespace models {

ContactsModel::ContactsModel(QSettings& settings)
    : settings_{settings}
{
    setTable("contact");
    setSort(fieldIndex("name"), Qt::AscendingOrder);
    setEditStrategy(QSqlTableModel::OnFieldChange);

    h_id_ = fieldIndex("id");
    h_uuid_ = fieldIndex("uuid");
    h_hash_ = fieldIndex("hash");
    h_name_ = fieldIndex("name");
    h_nickname_ = fieldIndex("nickname");
    h_pubkey_ = fieldIndex("pubkey");
    h_address_ = fieldIndex("address");
    h_notes_= fieldIndex("notes");
    h_group_ = fieldIndex("group");
    h_avatar_ = fieldIndex("avatar");
    h_created_ = fieldIndex("created");
    h_initiated_by = fieldIndex("initiated_by");
    h_last_seen_ = fieldIndex("last_seen_");
    h_online_ = fieldIndex("online");

    connect(&DsEngine::instance(), &DsEngine::contactCreated,
            this, &ContactsModel::onContactCreated, Qt::QueuedConnection);

    if (!select()) {
        qWarning() << "Failed to select fropm database: " << lastError().text();
    }
}

bool ContactsModel::hashExists(QByteArray hash) const
{
    qDebug() << "Checking if hash exists: " << hash.toBase64();

    QSqlQuery query;
    query.prepare("SELECT 1 from contact where hash=:hash");
    query.bindValue(":hash", hash);
    if(!query.exec()) {
        throw Error(QStringLiteral("SQL query failed: %1").arg(query.lastError().text()));
    }
    return query.next();
}

void ContactsModel::onContactCreated(const Contact &contact)
{

    // insertRecord() is fundamentally broken!

    QSqlQuery query;

    /*`uuid` TEXT NOT NULL UNIQUE, `hash` BLOB NOT NULL UNIQUE, `name` TEXT NOT NULL, `nickname` TEXT UNIQUE, `pubkey` TEXT NOT NULL UNIQUE,
     * `address` TEXT NOT NULL, `notes` TEXT, `group` INTEGER, `avatar` BLOB, `created` INTEGER NOT NULL, `initiated_by` TEXT NOT NULL, `last_seen` INTEGER, `online`
    */
    QString sql = "INSERT INTO contact (uuid, hash, name, nickname, pubkey, address, notes, contact_group, avatar, created, initiated_by) "
                  "VALUES (:uuid, :hash, :name, :nickname, :pubkey, :address, :notes, :contact_group, :avatar, :created, :initiated_by)";

    query.prepare(sql);
    query.bindValue(":uuid", QUuid().toString());
    query.bindValue(":hash", contact.hash);
    query.bindValue(":name", contact.name);
    if (contact.nickname.isEmpty()) {
        query.bindValue(":nickname", QVariant(QVariant::String));
    } else {
        query.bindValue(":nickname", contact.nickname);
    }
    query.bindValue(":pubkey", contact.pubkey);
    query.bindValue(":address", QString{contact.address});

    query.bindValue(":notes", contact.notes);
    if (contact.group.isEmpty()) {
        query.bindValue(":contact_group", QStringLiteral("uncategorized"));
    } else {
        query.bindValue(":contact_group", contact.group);
    }

    if (contact.avatar.isNull()) {
        query.bindValue(":avatar", QVariant(QVariant::ByteArray));
    } else {
        QByteArray ba;
        QBuffer buffer(&ba);
        buffer.open(QIODevice::WriteOnly);
        contact.avatar.save(&buffer, "PNG");
        query.bindValue(":avatar", ba);
    }
    query.bindValue(":created", QDateTime::currentDateTime().toTime_t());
    query.bindValue(":initiated_by", contact.getInitiatedBy());

    qDebug() << query.executedQuery();

    if (!query.exec()) {
        qWarning() << "Failed to add contact " << contact.name << ". Error:  " << query.lastError();
    }

    // Just reselect everything
    select();

//    QSqlRecord rec;
//    rec.setValue(QStringLiteral("uuid"), QUuid().toString());
//    if (!contact.nickname.isEmpty()) {
//        rec.setValue(QStringLiteral("nickname"), contact.nickname);
//    }
//    if (contact.group.isEmpty()) {
//        rec.setValue(QStringLiteral("contact_group"), QStringLiteral("uncategorized"));
//    } else {
//        rec.setValue(QStringLiteral("contact_group"), contact.group);
//    }
//    rec.setValue(QStringLiteral("initiated_by"), contact.getInitiatedBy());
//    rec.setValue(QStringLiteral("hash"), contact.hash);
//    rec.setValue(QStringLiteral("name"), contact.name);
//    if (!contact.notes.isEmpty()) {
//        rec.setValue(QStringLiteral("notes"), contact.notes);
//    }
////    if (!contact.avatar.isNull()) {
////        rec.setValue(QStringLiteral("avatar"), contact.avatar);
////    }
//    rec.setValue(QStringLiteral("created"), QDateTime::currentDateTime().toTime_t());
//    //setEditStrategy(QSqlTableModel::OnManualSubmit);
//    if (!insertRecord(-1, rec) || !submit()) {
//        qWarning() << "Failed to add contact: " << contact.name
//                   << this->lastError().text();
//        return;
//    }

    qDebug() << "Added contact " << contact.name << " to the database";
}

}} // namnespaces
