#ifndef CONTACTSMODEL_H
#define CONTACTSMODEL_H

#include <memory>

#include <QSettings>
#include <QSqlTableModel>
#include <QImage>
#include <QMetaType>
#include <QUuid>
#include <QAbstractSocket>

#include "ds/contact.h"
#include "ds/protocolmanager.h"
#include "ds/dsengine.h"

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
        ExtraInfo(const QByteArray& dbId) : id{dbId} {}
        using ptr_t = std::shared_ptr<ExtraInfo>;

        OnlineStatus onlineStatus = DISCONNECTED;
        int numPendingDeliveries = {}; // messages waiting to be sent
        bool unreadMessages = false;
        QUuid outbound_connection_uuid;
        const QByteArray id; // Database ID

        bool isOnline() const noexcept {
            return onlineStatus == ONLINE;
        }
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
    void onContactAccepted(const ds::core::Contact& contact);
    OnlineStatus getOnlineStatus(int row) const;
    void createContact(const QString &nickName,
                       const QString& name,
                       const QString& handle,
                       const QString& address,
                       const QString& message,
                       const QString& notes,
                       bool autoConnect);

    void onIdentityChanged(int identityId);
    void connectTransport(int row);
    void connectTransport(const QByteArray& id, ExtraInfo& extra);
    void disconnectTransport(int row);
    void onReceivedAck(const core::PeerAck& ack);

signals:
    void identityChanged(int from, int to);
    void onlineStatusChanged();

private slots:
    void onConnectedTo(const QUuid& uuid, const core::ProtocolManager::Direction direction);
    void onDisconnectedFrom(const QUuid& uuid);
    void onConnectionFailed(const QUuid& uuid,
                            const QAbstractSocket::SocketError& socketError);

private:
    QByteArray getIdFromRow(const int row) const;
    QByteArray getIdFromUuid(const QUuid &uuid) const;
    QByteArray getIdFromConnectionUuid(const QUuid &uuid) const;
    QUuid getIdentityUuidFromId(const QByteArray& id) const;
    int getRowFromId(const QByteArray& id) const;
    ExtraInfo::ptr_t getExtra(const int row) const;
    ExtraInfo::ptr_t getExtra(const QByteArray& id) const;
    ExtraInfo::ptr_t getExtra(const QUuid& uuid) const;
    QString getOnlineIcon(int row) const;
    void disconnectPeer(const QByteArray& id, const QUuid& connection);
    void setOnlineStatus(const QByteArray& id, OnlineStatus status);
    void doIfOnline(int row,
                    std::function<void (const QByteArray&, ExtraInfo&)> fn,
                    bool throwIfNot = false);
    void doIfValid(int row,
                    std::function<void (const QByteArray&, ExtraInfo&)> fn,
                    bool throwIfNot = false);
    void doIfValid(int row,
                    std::function<void (const QByteArray&)> fn,
                    bool throwIfNot = false);
    int col2Role(int col) const noexcept { return col + Qt::UserRole; }
    void updateLastSeen(const QByteArray& id);
    ds::core::Contact::State getState(const QByteArray& id) const;
    ds::core::Contact::InitiatedBy getInitiation(const QByteArray& id) const;
    void setState(const QByteArray& id, const core::Contact::State state);
    void sendAddme(const QByteArray& id, const QUuid &connection);
    void onAddmeAck(const QByteArray& id, const core::PeerAck& ack);

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
    int h_state_ = {};
    int h_addme_message_ = {};
    int h_autoconnect_ = {};
    int h_hash_ = {};

    QSettings& settings_;

    // Keeps infor about contacts for all online identities.
    mutable std::map<QByteArray, ExtraInfo::ptr_t> extras_;
    int identity_ = -1;
    QUuid identityUuid_;
};

}} // namespaces

#endif // CONTACTSMODEL_H
