#ifndef NOTIFICATIONSMODEL_H
#define NOTIFICATIONSMODEL_H

#include <QSettings>
#include <QSqlQueryModel>

#include <unordered_map>

#include "ds/dsengine.h"
#include "ds/identity.h"
#include "logfault/logfault.h"

namespace ds {
namespace models {


class NotificationsModel : public QSqlQueryModel
{
    Q_OBJECT
    Q_PROPERTY(bool hasMessages READ hasMessages NOTIFY notificationStateChanged)
    Q_PROPERTY(bool hasContactRequests READ hasContactRequests NOTIFY notificationStateChanged)

public:
    enum Type {
        N_ADDME,
        N_FILE,
        N_MESSAGE
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
        H_CONVERSATION,
        H_UNREAD_COUNT,
        H_SORT_GROUP,
        H_SORT_ORDER,

        H_NICKNAME = Qt::UserRole + 100,
        H_HANDLE,
        H_ADDRESS,
        H_FILE_NAME,
        H_FILE_SIZE,
        H_AVATAR_URL,
        H_CONTACT_OBJECT,
        H_CONTACT_HEADER,
        H_REQUESTER_ID
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
    Q_INVOKABLE void clearConversationNotifications(int conversationId);
    void refreshNotifications();

public:
    NotificationsModel() = default;
    NotificationsModel(QSettings& settings);

    bool hasMessages() const noexcept { return has_messages_; }
    bool hasContactRequests() const noexcept { return has_contact_requests_; }

signals:
    void notificationStateChanged();

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
    bool isContactHeader(const QModelIndex& index) const;
    void updateNotificationState();

    // Keep the authenticated request alive while the user decides whether to
    // accept it.  The database notification remains the durable fallback.
    std::unordered_map<QString, core::PeerAddmeReq> pendingContactRequests_;
    bool has_messages_ = false;
    bool has_contact_requests_ = false;

};

}} // namespaces

#endif // NOTIFICATIONSMODEL_H
