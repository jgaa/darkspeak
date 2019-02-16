#include "include/ds/notificationsmodel.h"

#include <QDateTime>

namespace ds {
namespace models {

using namespace std;
using namespace core;

NotificationsModel::NotificationsModel(QSettings &settings)
{
    Q_UNUSED(settings)
    refresh();
}

void NotificationsModel::refresh()
{
    setQuery(R"(select n.id, n.status, n.type, n.timestamp, n.message, n.data, n.identity, n.contact, i.name as iname, c.name as cname from notification as n left join identity as i on n.identity = i.id left join contact as c on n.contact = c.id order by i.name, c.name, n.priority, n.timestamp)");
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
        {H_ADDRESS, "address"}
    };

    return names;
}

void NotificationsModel::addNotification(const int identityId, const core::PeerAddmeReq &req)
{
    // TODO: Avoid duplicates. Only add the request if we have no active addme requests from
    //       the same contact.

    QDateTime when = QDateTime::fromTime_t((QDateTime::currentDateTime().toTime_t() / 60) * 60);

    QSqlQuery query{core::DsEngine::instance().getDb()};

    QVariantMap data;
    data.insert("address", req.address);
    data.insert("handle", req.handle);
    data.insert("nickName", req.nickName);

    query.prepare("INSERT INTO notification "
                  "(status, priority, identity, type, timestamp, message, data) "
                  "VALUES "
                  "(:status, :priority, :identity, :type, :timestamp, :message, :data)");
    query.bindValue(":status", ACTIVE);
    query.bindValue(":priority", NORMAL);
    query.bindValue(":identity", identityId);
    query.bindValue(":type", N_ADDME);
    query.bindValue(":timestamp", when);
    query.bindValue(":message", req.message);
    query.bindValue(":data", core::DsEngine::toJson(data));

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
        ContactReq cr;
        cr.whoInitiated = Contact::THEM;
        cr.name = cr.nickname = d.value("nickName").toString();
        cr.contactHandle = d.value("handle").toByteArray();
        cr.identity = data(index(row, H_IDENTITY), Qt::DisplayRole).toInt();

        auto handle =  d.value("handle").toByteArray();
        auto handle_str = handle.toStdString();

        cr.pubkey = crypto::b58tobin_check<QByteArray>(handle_str, 32, {249, 50});
        cr.address = d.value("address").toByteArray();

        auto check = crypto::b58check_enc<QByteArray>(cr.pubkey, {249, 50});
        assert(check == handle);

        auto contact = DsEngine::instance().prepareContact(cr);

        emit contactAccepted(contact);
        deleteRow(row);
    } else {
        // Send reject message and delete the notification

    }
}


}} // namespaces
