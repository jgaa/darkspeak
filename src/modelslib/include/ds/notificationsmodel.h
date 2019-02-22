#ifndef NOTIFICATIONSMODEL_H
#define NOTIFICATIONSMODEL_H

#include <QSettings>
#include <QSqlQueryModel>

#include "ds/dsengine.h"
#include "ds/identity.h"
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
        H_DATA,
        H_IDENTITY,
        H_CONTACT,
        H_IDENTITY_NAME,
        H_CONTACT_NAME,

        H_NICKNAME = Qt::UserRole + 100,
        H_HANDLE,
        H_ADDRESS
    };

    QVariant data(const QModelIndex &index, int role) const override;
    virtual QHash<int, QByteArray> roleNames() const override;

signals:
    void contactAccepted(const ds::core::Contact& contact);

public slots:
    void addNotification(core::Identity *identity, const core::PeerAddmeReq& req);
    void acceptContact(const int row, bool accept);

public:
    NotificationsModel() = default;
    NotificationsModel(QSettings& settings);

private:
    int col2Role(int col) const noexcept { return col + Qt::UserRole; }
    void refresh();
    void deleteRow(const int row);
    bool isHashPresent(const QByteArray& hash) const ;

};

}} // namespaces

#endif // NOTIFICATIONSMODEL_H
