#ifndef NOTIFICATIONSMODEL_H
#define NOTIFICATIONSMODEL_H

#include <QSettings>
#include <QSqlQueryModel>

#include "ds/dsengine.h"
#include "logfault/logfault.h"

namespace ds {
namespace models {

class NotificationsModel : public QSqlQueryModel
{
    Q_OBJECT

    enum Roles {
        // Fields that don't exist in the database
        ONLINE_ROLE = Qt::UserRole + 100
    };

    enum Status {
        ACTIVE,
        DISMISSED
    };

    enum Priority {
        URGENT,
        NORMAL,
        LOW
    };

    enum Type {
        N_ADDME
    };

    enum Fields {
        H_ID,
        H_STATUS,
        H_TYPE,
        H_TIMESTAMP,
        H_MESSAGE,
        H_IDENTITY,
        H_CONTACT,
        H_IDENTITY_NAME,
        H_CONTACT_NAME
    };

    QVariant data(const QModelIndex &index, int role) const override;
    virtual QHash<int, QByteArray> roleNames() const override;

public slots:
    void addNotification(const int identityId, const core::PeerAddmeReq& req);

public:
    NotificationsModel() = default;
    NotificationsModel(QSettings& settings);

private:
    int col2Role(int col) const noexcept { return col + Qt::UserRole; }
    void refresh();

};

}} // namespaces

#endif // NOTIFICATIONSMODEL_H
