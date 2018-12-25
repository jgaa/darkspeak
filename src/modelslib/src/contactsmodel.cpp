#include <assert.h>

#include "ds/contactsmodel.h"
#include "ds/dsengine.h"
#include "ds/errors.h"
#include "ds/model_util.h"
#include "ds/strategy.h"

#include <QBuffer>
#include <QDateTime>
#include <QDebug>
#include <QSqlRecord>
#include <QUuid>

#include "logfault/logfault.h"

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
    h_identity_ = fieldIndex("identity");
    h_uuid_ = fieldIndex("uuid");
    h_hash_ = fieldIndex("hash");
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
    h_online_ = fieldIndex("online");

    connect(&DsEngine::instance(), &DsEngine::contactCreated,
            this, &ContactsModel::onContactCreated, Qt::QueuedConnection);
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

void ContactsModel::onContactCreated(const Contact &contact)
{
    Strategy strategy(*this, QSqlTableModel::OnManualSubmit);
    QSqlRecord rec{DsEngine::instance().getDb().record(tableName())};
    rec.setValue(h_identity_, contact.identity);
    rec.setValue(h_uuid_, QUuid().toString());
    if (!contact.nickname.isEmpty()) {
        rec.setValue(h_nickname_, contact.nickname);
    }
    if (contact.group.isEmpty()) {
        rec.setValue(h_group_, QStringLiteral("uncategorized"));
    } else {
        rec.setValue(h_group_, contact.group);
    }
    rec.setValue(h_initiated_by, contact.getInitiatedBy());
    rec.setValue(h_hash_, contact.hash);
    rec.setValue(h_pubkey_, contact.pubkey);
    rec.setValue(h_address_, contact.address);
    rec.setValue(h_name_, contact.name);
    rec.setValue(h_initiated_by, contact.getInitiatedBy());
    if (!contact.notes.isEmpty()) {
        rec.setValue(h_notes_, contact.notes);
    }
    if (!contact.avatar.isNull()) {
        rec.setValue(h_avatar_, DsEngine::imageToBytes(contact.avatar));
    }
    rec.setValue(h_created_, QDateTime::currentDateTime().toTime_t());

    if (!insertRecord(-1, rec) || !submitAll()) {
        qWarning() << "Failed to add contact: " << contact.name
                   << this->lastError().text();
        return;
    }

    LFLOG_DEBUG << "Added contact " << contact.name << " to the database";
}

}} // namnespaces
