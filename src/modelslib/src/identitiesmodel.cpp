
#include <QSqlQuery>
#include <QSqlError>
#include <QRunnable>
#include <QDebug>
#include <QSqlRecord>
#include <QSqlField>
#include <QDateTime>
#include <QUuid>

#include "ds/identitiesmodel.h"
#include "ds/errors.h"
#include "ds/strategy.h"
#include "logfault/logfault.h"

using namespace ds::core;
using namespace std;

namespace ds {
namespace models {

IdentitiesModel::IdentitiesModel(QSettings &settings)
    : settings_{settings}
{
    setTable("identity");
    setSort(fieldIndex("name"), Qt::AscendingOrder);
    setEditStrategy(QSqlTableModel::OnFieldChange);

    h_id_ = fieldIndex("id");
    h_uuid_ = fieldIndex("uuid");
    h_hash_ = fieldIndex("hash");
    h_name_ = fieldIndex("name");
    h_cert_ = fieldIndex("cert");
    h_address_ = fieldIndex("address");
    h_address_data_ = fieldIndex("address_data");
    h_notes_= fieldIndex("notes");
    h_avatar_ = fieldIndex("avatar");
    h_created_ = fieldIndex("created");

    connect(&DsEngine::instance(), &DsEngine::identityCreated,
            this, &IdentitiesModel::saveIdentity);

    select();
}

void IdentitiesModel::createIdentity(QString name)
{
    IdentityReq req;
    req.name = name;
    DsEngine::instance().createIdentity(req);
}

void IdentitiesModel::saveIdentity(const ds::core::Identity &data)
{
    Strategy strategy(*this, QSqlTableModel::OnManualSubmit);
    QSqlRecord rec{DsEngine::instance().getDb().record(this->tableName())};

    rec.setValue(h_uuid_, QUuid().toString());
    rec.setValue(h_hash_, data.hash.toBase64());
    rec.setValue(h_name_, data.name);
    rec.setValue(h_cert_, data.cert.toBase64());
    rec.setValue(h_address_, data.address);
    rec.setValue(h_address_data_, data.addressData);
    if (!data.notes.isEmpty()) {
        rec.setValue(h_notes_, data.notes);
    }
    if (!data.avatar.isNull()) {
        rec.setValue(h_avatar_, data.avatar);
    }
    rec.setValue(h_created_, QDateTime::currentDateTime());

    if (!insertRecord(-1, rec)) {
        qWarning() << "Failed to insert identity: " << data.name
                   << this->lastError().text();
        return;
    }

    if (!submitAll()) {

        qWarning() << "Failed to flush identity: " << data.name
                   << this->lastError().text();
        return;
    }

    LFLOG_DEBUG << "Added identity " << data.name << " to the database";
}

QVariant IdentitiesModel::data(const QModelIndex &ix, int role) const {

    LFLOG_DEBUG << "IdentitiesModel::data(" << ix.row() << ", " << ix.column() << ", " << role << ") called";

    if (!ix.isValid()) {
        return {};
    }

    // Map the QML field name mapping back to normal column based access
    auto model_ix = ix;
    if (role >= Qt::UserRole) {
        model_ix = index(ix.row(), role - Qt::UserRole);
        role = Qt::DisplayRole;
    }

    if (role == Qt::DecorationRole) {
        if (model_ix.column() == h_name_) {
            // TODO: Add Onion status icon
            // TODO: Add note icon if we have a note
        }
    }

    if (role == Qt::DisplayRole) {
        if (model_ix.column() == h_created_) {
            return QSqlTableModel::data(index(model_ix.row(), h_created_), Qt::DisplayRole).toDate().toString();
        }
    }

    return QSqlTableModel::data(model_ix, role);
}

bool IdentitiesModel::setData(const QModelIndex &ix,
                              const QVariant &value,
                              int role) {
    return QSqlTableModel::setData(ix, value, role);
}

QHash<int, QByteArray> IdentitiesModel::roleNames() const
{
    QHash<int, QByteArray> names = {
        {h_name_ + Qt::UserRole, "name"},
        {h_created_ + Qt::UserRole, "created"}
    };

    return names;
}

//QVariant IdentitiesModel::headerData(int section, Qt::Orientation orientation, int role) const
//{
////    if (role == Qt::DisplayRole && orientation == Qt::Horizontal) {
////        if (section == h_name_ ) {
////            return tr("Nickname");
////        } else if (section == h_created_ ) {
////            return tr("Created");
////        }
////    }

//    return QSqlTableModel::headerData(section, orientation, role);
//}

//Qt::ItemFlags IdentitiesModel::flags(const QModelIndex &ix) const
//{
//    return QSqlTableModel::flags(ix); //& ~Qt::EditRole;
//}

bool IdentitiesModel::identityExists(QString name) const
{
    QSqlQuery query;
    query.prepare("SELECT 1 from identity where name=:name");
    query.bindValue(":name", name);
    if(!query.exec()) {
        throw Error(QStringLiteral("SQL query failed: %1").arg(query.lastError().text()));
    }
    return query.next();
}


}} // namepaces

