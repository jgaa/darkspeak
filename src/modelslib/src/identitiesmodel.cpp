
#include <QSqlQuery>
#include <QSqlError>
#include <QRunnable>
#include <QDebug>
#include <QSqlRecord>
#include <QSqlField>
#include <QDateTime>

#include "ds/identitiesmodel.h"
#include "ds/errors.h"
#include "ds/strategy.h"

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
    h_hash_ = fieldIndex("hash");
    h_name_ = fieldIndex("name");
    h_cert_ = fieldIndex("cert");
    h_address_ = fieldIndex("address");
    h_address_data_ = fieldIndex("address_data");
    h_notes_= fieldIndex("notes");
    h_avatar_ = fieldIndex("avatar");
    h_created_ = fieldIndex("created");

    connect(&DsEngine::instance(), &DsEngine::identityCreated,
            this, &IdentitiesModel::createIdentity);
}

void IdentitiesModel::createIdentity(const ds::core::Identity &data)
{
    Strategy strategy(*this, QSqlTableModel::OnManualSubmit);
    QSqlRecord rec{DsEngine::instance().getDb().record(this->tableName())};

    rec.setValue(h_hash_, data.hash);
    rec.setValue(h_name_, data.name);
    rec.setValue(h_cert_, data.cert);
    rec.setValue(h_address_, data.address);
    rec.setValue(h_address_data_, data.addressData);
    if (!data.notes.isEmpty()) {
        rec.setValue(h_notes_, data.notes);
    }
    if (!data.avatar.isNull()) {
        rec.setValue(h_avatar_, data.avatar);
    }
    rec.setValue(h_created_, QDateTime::currentDateTime().toTime_t());

    if (!insertRecord(-1, rec) || !submitAll()) {
        qWarning() << "Failed to add identity: " << data.name
                   << this->lastError().text();
        return;
    }

    qDebug() << "Added identity " << data.name << " to the database";
}

QVariant IdentitiesModel::data(const QModelIndex &ix, int role) const
{
    return QSqlTableModel::data(ix, role);
}

bool IdentitiesModel::setData(const QModelIndex &ix, const QVariant &value, int role)
{
    return QSqlTableModel::setData(ix, value, role);
}

QVariant IdentitiesModel::headerData(int section, Qt::Orientation orientation, int role) const
{
    return QSqlTableModel::headerData(section, orientation, role);
}

Qt::ItemFlags IdentitiesModel::flags(const QModelIndex &ix) const
{
    return QSqlTableModel::flags(ix);
}

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

