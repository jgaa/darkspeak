#pragma once

#include "darkspeak/darkspeak.h"
#include "darkspeak/Api.h"
#include "darkspeak/EventMonitor.h"
#include "darkspeak/FileInfo.h"

#include <ImageProvider.h>

#include <QtCore>

class ChatMessagesModel;
class ContactData;

class ContactsModel : public QAbstractListModel
{
    Q_OBJECT

public:
    enum OnlineStatus {
        OS_OFF_LINE,
        OS_CONNECTING,
        OS_ONLINE,
        OS_DISCONNECTING
    };

private:
    Q_ENUMS(OnlineStatus)

    Q_PROPERTY(OnlineStatus onlineStatus
        READ getOnlineStatus
        NOTIFY onlineStatusChanged)

    Q_PROPERTY(QUrl onlineStatusIcon
        READ getOnlineStatusIcon
        NOTIFY onlineStatusIconChanged)

    class Events : public darkspeak::EventMonitor
    {
    public:
        Events(ContactsModel& parent);


        // EventMonitor interface
        bool OnIncomingConnection(const ConnectionInfo &info) override;
        bool OnAddNewBuddy(const BuddyInfo &info) override;
        void OnNewBuddyAdded(const BuddyInfo &info) override;
        void OnBuddyDeleted(const DeletedBuddyInfo &info) override;
        void OnBuddyStateUpdate(const BuddyInfo &info) override;
        void OnIncomingMessage(const Message &message) override;
        void OnIncomingFile(const darkspeak::FileInfo &file) override;
        void OnFileTransferUpdate(const darkspeak::FileInfo& file) override;
        void OnOtherEvent(const Event &event) override;
        void OnListening(const ListeningInfo &endpoint) override;
        void OnShutdownComplete(const ShutdownInfo &info) override;
        void OnAvatarReceived(const AvatarInfo& info) override;

    private:
        ContactsModel& parent_;
    };

public:
    // Define the role names to be used
    enum RoleNames {
        VisualNameRole = Qt::UserRole,
        IdRole,
        LastSeenRole,
        StatusColorRole,
        StatusRole,
        AnonymityLevelRole,
        AvatarRole
    };

    explicit ContactsModel(darkspeak::Api& api, QObject *parent = nullptr);
    ~ContactsModel();
    ContactsModel(const ContactsModel&) = delete;
    ContactsModel(ContactsModel&&) = delete;
    ContactsModel& operator = (const ContactsModel&) = delete;
    ContactsModel& operator = (ContactsModel&&) = delete;


    // QAbstractItemModel interface
public:
    virtual int rowCount(const QModelIndex &parent) const override;
    virtual QVariant data(const QModelIndex &index, int role) const override;

public:
    Q_INVOKABLE void append(const QString& vid);
    Q_INVOKABLE void remove(int index);
    Q_INVOKABLE ChatMessagesModel *getMessagesModel(int index);
    Q_INVOKABLE ContactData *getContactData(int index);

    ImageProvider *GetAvatarProvider() {
        return avatars_;
    }

public slots:
    OnlineStatus getOnlineStatus() const;
    void setOnlineStatus(OnlineStatus status);
    QUrl getOnlineStatusIcon() const;
    void refreshBuddyState(std::string id);
    void refreshBuddyMessages(std::string id);
    void addBuddyToList(std::string id);
    void deleteBuddyFromList(std::string id);
    void IncomingFileRequest(darkspeak::FileInfo file);

signals: // signals can be emitted
    void onlineStatusChanged(const OnlineStatus status);
    void onlineStatusIconChanged();
    void onBuddyStateChanged(std::string id);
    void onBuddyHasNewMessage(std::string id);

    // Inter therad communication.
    void onBuddyStateMayHaveChanged(std::string id);
    void onBuddyMayHaveNewMessage(std::string id);
    void onBuddyAdded(std::string id);
    void onBuddyDeleted(std::string id);
    void onIncomingFileRequest(darkspeak::FileInfo file);

    // Signals we want accessible from QML
    // First letter must be lower case
    void incomingFile(QString fileName, QString fileId, ContactData *contact);

protected:
    // return the roles mapping to be used by QML
    virtual QHash<int, QByteArray> roleNames() const override;
private:
//     QDateTime Convert(const std::time_t& when) const;
//     QString Convert(const darkspeak::Api::Status& status) const;
//     QString Convert(const darkspeak::Api::AnonymityLevel level) const;

    int FindBuddy(const std::string& id);
    bool HaveBuddy(const std::string& id);

    darkspeak::Api& api_;
    darkspeak::Api::buddy_list_t buddies_;

    struct InfoCache {
        int row = -1;
        darkspeak::Api::Buddy::Info info;
        void clear() {row = -1;}
    };
    mutable InfoCache icache_;
    OnlineStatus online_status_ = OS_OFF_LINE;
    std::shared_ptr<Events> event_listener_;
    ImageProvider *avatars_ {};
};

