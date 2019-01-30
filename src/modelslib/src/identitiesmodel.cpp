
#include <array>

#include <QSqlQuery>
#include <QSqlError>
#include <QRunnable>
#include <QDebug>
#include <QSqlRecord>
#include <QSqlField>
#include <QDateTime>
#include <QUuid>
#include <QJsonObject>
#include <QJsonDocument>
#include <QtEndian>

#include "ds/identitiesmodel.h"
#include "ds/errors.h"
#include "ds/strategy.h"
#include "ds/base32.h"
#include "ds/base58.h"
#include "logfault/logfault.h"
#include "ds/dscert.h"

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

    connect(&DsEngine::instance(),
            &ds::core::DsEngine::serviceStarted,
            this, &IdentitiesModel::onServiceStarted);

    connect(&DsEngine::instance(),
            &ds::core::DsEngine::serviceStopped,
            this, &IdentitiesModel::onServiceStopped);

    connect(&DsEngine::instance(),
            &ds::core::DsEngine::serviceFailed,
            this, &IdentitiesModel::onServiceFailed);

    connect(&DsEngine::instance(),
            &ds::core::DsEngine::transportHandleReady,
            this, &IdentitiesModel::onTransportHandleReady);

    select();
}

bool IdentitiesModel::getOnlineStatus(int row) const
{
    if (auto e = getExtra(row)) {
        return e->online;
    }

    return false;
}

void IdentitiesModel::createIdentity(QString name)
{
    IdentityReq req;
    req.name = name;
    DsEngine::instance().createIdentity(req);
}

void IdentitiesModel::deleteIdentity(int row)
{
    // TODO: Delete contacts and messages
    // TODO: Delete files, folders owned by this identity

    LFLOG_DEBUG << "Deleting identity "
                << data(index(row,0), h_name_ + Qt::UserRole).toString();

    removeRows(row, 1);
    if (!submitAll()) {

        qWarning() << "Failed to delete identity at index: " << row
                   << ": " << this->lastError().text();
        return;
    }

    select();
}


void IdentitiesModel::startService(int row)
{
    auto id = getIdFromRow(row);
    if (!id.isEmpty()) {
        auto d = DsEngine::fromJson(data(index(row, h_address_data_), Qt::DisplayRole).toByteArray());
        DsEngine::instance().getProtocolMgr(
                    ProtocolManager::Transport::TOR).startService(id, d);
    }
}

void IdentitiesModel::stopService(int row)
{
    auto id = getIdFromRow(row);
    if (!id.isEmpty()) {
        DsEngine::instance().getProtocolMgr(
                    ProtocolManager::Transport::TOR).stopService(id);
    }
}

void IdentitiesModel::saveIdentity(const ds::core::Identity &data)
{
    Strategy strategy(*this, QSqlTableModel::OnManualSubmit);
    QSqlRecord rec{DsEngine::instance().getDb().record(this->tableName())};

    rec.setValue(h_uuid_, data.uuid);
    rec.setValue(h_hash_, data.hash);
    rec.setValue(h_name_, data.name);
    rec.setValue(h_cert_, data.cert.toByteArray()); // TODO: Se if we can avoid a temporary string
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

void IdentitiesModel::onServiceStarted(const QByteArray &id)
{
    auto e = getExtra(id);
    e->online = true;

    auto row = getRowFromId(id);
    auto ix = index(row, 0);
    emit dataChanged(ix, ix, { Qt::EditRole, ONLINE_ROLE});
}

void IdentitiesModel::onServiceStopped(const QByteArray &id)
{
    auto e = getExtra(id);

    e->online = false;

    auto row = getRowFromId(id);
    auto ix = index(row, 0);
    emit dataChanged(ix, ix, { Qt::EditRole, ONLINE_ROLE});
}

void IdentitiesModel::onServiceFailed(const QByteArray &id,
                                      const QByteArray &reason)
{
    Q_UNUSED(id);
    Q_UNUSED(reason);

    // TODO: Show error notification?
}

void IdentitiesModel::onTransportHandleReady(const TransportHandle &th)
{
    static const QRegExp re("\\d+");
    if (!re.exactMatch(th.identityName)) {
        LFLOG_DEBUG << "Discarding transport. Name is not a database index id: "
                    << th.identityName;
        return;
    }

    const auto row = getRowFromId(th.identityName.toUtf8());
    if (row < 0) {
        LFLOG_WARN << "Discarding transport. Cannot map id ("
                    << th.identityName
                    << ") to row.";
        return;
    }

    auto rec = record(row);

    rec.setValue(h_address_, th.handle);
    rec.setValue(h_address_data_, DsEngine::toJson(th.data));

    if (!setRecord(row, rec)) {
        LFLOG_WARN << "Failed to update identity: " << rec.value(h_name_).toString()
                   << lastError().text();
        return;
    }

    if (!submitAll()) {

        LFLOG_WARN << "Failed to flush identity: " << rec.value(h_name_).toString()
                   << this->lastError().text();
        return;
    }

    LFLOG_DEBUG << "Updated identity " << rec.value(h_name_).toString() << " in the database";

}

// https://stackoverflow.com/questions/24906202/get-data-from-a-specific-column-in-a-tableview-qml
QVariantMap IdentitiesModel::get(int idx) const {
    QVariantMap map;
    foreach(int k, roleNames().keys()) {
        map[roleNames().value(k)] = data(index(idx, 0), k);
    }
    return map;
}

QString IdentitiesModel::getIdentityAsBase58(int row) const
{
    // Format:
    //  1 byte version (1)
    //  32 byte pubkey
    //  10 bytes onion address
    //  2 bytes port

    // B58 tag: {11, 176}

    QByteArray bytes;
    auto d = DsEngine::fromJson(data(index(row, h_address_data_), Qt::DisplayRole).toByteArray());

    bytes.append('\1'); // Version
    ds::crypto::DsCert::safe_array_t cert_data{QSqlTableModel::data(index(row, h_cert_), Qt::DisplayRole).toByteArray()};
    if (cert_data.isEmpty()) {
        return {};
    }

    bytes += crypto::DsCert::create(cert_data)->getPubKey().toByteArray();

    auto onion = d["service_id"].toByteArray();

    if (onion.size() < 16) {
        LFLOG_ERROR << "getIdentityAsBase58: Invalid Tor addresses!";
        return {};
    }

    if (onion.size() == 16) {
        // Legacy onion address
        bytes += crypto::onion16decode(onion);
    } else {
        // TODO: Implement
        LFLOG_ERROR << "getIdentityAsBase58 NOT IMPLEMENTED for modern Tor addresses!";
        return "Sorry, Not implemented";
    }

    // Add the port
    union {
        char bytes[2];
        uint16_t port;
    } port_u;

    port_u.port = qToBigEndian(static_cast<uint16_t>(d["port"].toInt()));
    bytes += port_u.bytes[0];
    bytes += port_u.bytes[1];

    QByteArray result;

    return crypto::b58check_enc<QByteArray>(bytes, {11, 176});
}

void IdentitiesModel::createNewTransport(int row)
{
    LFLOG_NOTICE << "Requesting a new Tor service for "
                 << data(index(row, h_name_), Qt::DisplayRole).toString();

    DsEngine::instance().createNewTransport(getIdFromRow(row));
}

int IdentitiesModel::row2Id(int row)
{
    return data(index(row, h_id_), Qt::DisplayRole).toInt();
}

QVariant IdentitiesModel::data(const QModelIndex &ix, int role) const {

    //LFLOG_DEBUG << "IdentitiesModel::data(" << ix.row() << ", " << ix.column() << ", " << role << ") called";

    if (!ix.isValid()) {
        return {};
    }

    // Map the QML field name mapping back to normal column based access
    auto model_ix = ix;

    // Extra fields, generated fields
    switch(role) {
        case HANDLE_ROLE: {
            ds::crypto::DsCert::safe_array_t cert_data{QSqlTableModel::data(index(model_ix.row(), h_cert_), Qt::DisplayRole).toByteArray()};
            if (cert_data.isEmpty()) {
                return {};
            }
            return crypto::DsCert::create(cert_data)->getB58PubKey();
        }

        case ONLINE_ROLE:
            return getOnlineStatus(ix.row());
    };

    if (role >= Qt::UserRole) {
        model_ix = index(ix.row(), role - Qt::UserRole);
        role = Qt::DisplayRole;
    }

    if (role == Qt::DisplayRole) {
        if (model_ix.column() == h_created_) {
            return QSqlTableModel::data(index(model_ix.row(), h_created_), Qt::DisplayRole).toDate().toString();
        }

        if (model_ix.column() == h_id_) {
            return QSqlTableModel::data(index(model_ix.row(), h_id_), Qt::DisplayRole).toInt();
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
        {h_id_ + Qt::UserRole, "identityId"},
        {h_name_ + Qt::UserRole, "name"},
        {h_created_ + Qt::UserRole, "created"},
        {h_address_ + Qt::UserRole, "onion"},
        {HANDLE_ROLE, "handle"},
        {ONLINE_ROLE, "online"}
    };

    return names;
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

crypto::DsCert::ptr_t IdentitiesModel::getCert(const int identityId)
{
    return getExtra(QByteArray::number(identityId))->cert;
}

QByteArray IdentitiesModel::getIdFromRow(const int row) const
{
    auto id = data(index(row, h_id_), Qt::DisplayRole).toByteArray();

    if (id.isEmpty()) {
        LFLOG_WARN << "Unable to get Identity id from row " << row;
    }

    return id;
}

int IdentitiesModel::getRowFromId(const QByteArray &id) const
{
    for(int i = 0 ; i < rowCount(); ++i) {
        if (data(index(i, h_id_), Qt::DisplayRole).toByteArray() == id) {
            return i;
        }
    }

    return -1;
}

IdentitiesModel::ExtraInfo::ptr_t IdentitiesModel::getExtra(const int row) const
{
    const auto id = getIdFromRow(row);
    if (id.isEmpty()) {
        return {};
    }

    return getExtra(id);
}

IdentitiesModel::ExtraInfo::ptr_t IdentitiesModel::getExtra(const QByteArray &id) const
{
    auto it = extras_.find(id);
    if (it == extras_.end()) {
        auto extra = make_shared<ExtraInfo>();

        extra->cert = crypto::DsCert::create(data(index(id.toInt(), h_cert_),
                                                  Qt::DisplayRole).toByteArray());
        extras_[id] = extra;
        return extra;
    }

    return extras_[id];
}

}} // namepaces

