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

public:
    enum Type {
        N_ADDME,
        N_FILE
    };
    Q_ENUM(Type)

private:
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
        H_ADDRESS,
        H_FILE_NAME,
        H_FILE_SIZE,
        H_AVATAR_URL
    };

    QVariant data(const QModelIndex &index, int role) const override;
    virtual QHash<int, QByteArray> roleNames() const override;

signals:
    void contactAccepted(const ds::core::Contact& contact);

public slots:
    void onContactDeleted(const QUuid& uuid);
    void addNotification(core::Identity *identity, const core::PeerAddmeReq& req);
    void onFileAdded(const core::File::ptr_t& file);
    void onFileStateChanged(const core::File *file);
    void acceptContact(const int row, bool accept);
    void acceptFile(const int row, bool accept);

public:
    NotificationsModel() = default;
    NotificationsModel(QSettings& settings);

private:
    int col2Role(int col) const noexcept { return col + Qt::UserRole; }
    void refresh();
    void deleteRow(const int row);
    bool isHashPresent(const QString& hash) const ;
    void deleteFileNotification(const QByteArray& fileId);
    void purgeStaleFileNotifications();
    void requestNotificationAvatar(core::Identity *identity,
                                   const core::PeerAddmeReq& req,
                                   const QString& hash);

};

}} // namespaces

#endif // NOTIFICATIONSMODEL_H
