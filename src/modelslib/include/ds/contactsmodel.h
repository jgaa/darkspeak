#ifndef CONTACTSMODEL_H
#define CONTACTSMODEL_H

#include <memory>

#include <QSettings>
#include <QSqlTableModel>
#include <QImage>
#include <QMetaType>

#include "ds/contact.h"

namespace ds {
namespace models {

class ContactsModel : public QSqlTableModel
{
    Q_OBJECT

    enum Roles {
        // Fields that don't exist in the database
        ONLINE_ROLE = Qt::UserRole + 100,
        ONLINE_ICON_ROLE,
        HANDLE_ROLE,
        HAVE_NOTES_ROLE,
        AVATAR_ROLE,
        PENDING_DELIVERIES_ROLE,
        UNREAD_MESSAGES_ROLE
    };

public:
    enum OnlineStatus {
        DISCONNECTED, // We are not trying to connect so we don't know
        OFFLINE, // We are unable to connect
        CONNECTING,
        ONLINE,
        REJECTED // The contact rejected our addme request
    };

    Q_ENUM(OnlineStatus)

private:
    // Details kept in memory
    class ExtraInfo {
    public:
        using ptr_t = std::shared_ptr<ExtraInfo>;

        OnlineStatus onlineStatus = OnlineStatus::DISCONNECTED;
        int numPendingDeliveries = {}; // messages waiting to be sent
        bool unreadMessages = false;
    };

public:

    ContactsModel(QSettings& settings);

    // QAbstractItemModel interface
    QVariant data(const QModelIndex &index, int role = Qt::DisplayRole) const override;
    bool setData(const QModelIndex &index, const QVariant &value, int role) override;
    virtual QHash<int, QByteArray> roleNames() const override;

    bool hashExists(QByteArray hash) const;
    bool nameExists(QString name) const;

public slots:
    void onContactCreated(const ds::core::Contact& contact);
    OnlineStatus getOnlineStatus(int row) const;
    void createContact(const QString &nickName,
                       const QString& name, const QString& handle,
                       const QString& address, const QString& message,
                       const QString& notes, bool autoConnect);

    void onIdentityChanged(int identityId);

signals:
    void identityChanged(int from, int to);

private:
    QByteArray getIdFromRow(const int row) const;
    int getRowFromId(const QByteArray& id) const;
    ExtraInfo::ptr_t getExtra(const int row) const;
    ExtraInfo::ptr_t getExtra(const QByteArray& id) const;
    QString getOnlineIcon(int row) const;

    int col2Role(int col) const noexcept { return col + Qt::UserRole; }

    int h_id_ = {};
    int h_identity_ = {};
    int h_uuid_ = {};
    int h_name_ = {};
    int h_nickname_ = {};
    int h_pubkey_ = {};
    int h_address_ = {};
    int h_notes_= {};
    int h_group_ = {};
    int h_avatar_ = {};
    int h_created_ = {};
    int h_initiated_by = {};
    int h_last_seen_ = {};
    int h_blocked_ = {};
    int h_rejected_ = {};
    int h_autoconnect_ = {};
    int h_hash_ = {};

    QSettings& settings_;
    mutable std::map<QByteArray, ExtraInfo::ptr_t> extras_;
    int identity_ = -1;
};

}} // namespaces

#endif // CONTACTSMODEL_H
