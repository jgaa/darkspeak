#include "include/ds/notificationsmodel.h"

#include <QDateTime>

namespace ds {
namespace models {

using namespace std;

NotificationsModel::NotificationsModel(QSettings &settings)
{
    Q_UNUSED(settings)
    refresh();
}

void NotificationsModel::refresh()
{
    setQuery(R"(select n.id, n.status, n.type, n.timestamp, n.message, n.identity, n.contact, i.name as iname, c.name as cname from notification as n left join identity as i on n.identity = i.id left join contact as c on n.contact = c.id order by i.name, c.name, n.priority, n.timestamp)");
}


QVariant NotificationsModel::data(const QModelIndex &ix, int role) const
{
    if (!ix.isValid()) {
        return {};
    }

    // Map the QML field name mapping back to normal column based access
    auto model_ix = ix;

    if (role >= Qt::UserRole) {
        model_ix = index(ix.row(), role - Qt::UserRole);
        role = Qt::DisplayRole;
    }

    if (role == Qt::DisplayRole) {
        if (model_ix.column() == H_TIMESTAMP) {
            return QSqlQueryModel::data(index(model_ix.row(), H_TIMESTAMP), Qt::DisplayRole).toDateTime().toString();
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
        {col2Role(H_CONTACT_NAME), "contactName"}
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


}} // namespaces
