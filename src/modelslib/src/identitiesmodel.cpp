
#include <QSqlQuery>
#include <QSqlError>
#include <QRunnable>
#include <QDebug>
#include <QSqlRecord>
#include <QDateTime>

#include "ds/identitiesmodel.h"
#include "ds/errors.h"

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
    QSqlRecord rec;
    rec.setValue(QStringLiteral("hash"), data.hash);
    rec.setValue(QStringLiteral("name"), data.name);
    rec.setValue(QStringLiteral("cert"), data.cert);
    rec.setValue(QStringLiteral("address"), data.address);
    rec.setValue(QStringLiteral("address_data"), data.addressData);
    if (!data.notes.isEmpty()) {
        rec.setValue(QStringLiteral("notes"), data.notes);
    }
    if (!data.avatar.isNull()) {
        rec.setValue(QStringLiteral("avatar"), data.avatar);
    }
    rec.setValue(QStringLiteral("created"), QDateTime::currentDateTime().toTime_t());

    if (!insertRecord(-1, rec)) {
        qWarning() << "Failed to add identity " << data.name;
    }

    qDebug() << "Added identity " << data.name << " to the database.";

    //this->select();
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

