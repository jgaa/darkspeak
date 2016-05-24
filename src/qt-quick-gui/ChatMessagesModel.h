#pragma once

#pragma once

#include "darkspeak/darkspeak.h"
#include "darkspeak/Api.h"
#include "darkspeak/EventMonitor.h"
#include "darkspeak/BuddyEventsMonitor.h"

#include <QtCore>

class ContactsModel;

// class EventNotifier : public QObject
// {
//     Q_OBJECT
// public:
//
//     OnReceivedMessage(const darkspeak::Api::Message::ptr_t);
//
// signals:
//     void ReceivedMessage(const darkspeak::Api::Message::ptr_t);
// };

class ChatMessagesModel : public QAbstractListModel
{
    Q_OBJECT

public:
    enum MessageDirection {
        MD_INCOMING,
        MD_OUTGOING
    };

    enum MessageStatus {
        MS_QUEUED,
        MS_SENT,
        MS_RECEIVED
    };

private:

    Q_ENUMS(MessageDirection)
    Q_ENUMS(MessageStatus)


    class EventsMonitor : public darkspeak::BuddyEventsMonitor,
        public QObject
    {
    public:
        EventsMonitor(ChatMessagesModel& parent)
        : parent_{parent} {}

        void OnMessageReceived(const darkspeak::Api::Message::ptr_t& message) override;
        void OnStateChange(darkspeak::Api::Status status) override;
        void OnOtherEvent(const darkspeak::EventMonitor::Event& event) override;


    /*public slots:
        void MessageReceived(const darkspeak::Api::Message::ptr_t& message);
    */


    private:
        ChatMessagesModel& parent_;
    };


public:
    // Define the role names to be used
    enum RoleNames {
        ContentRole = Qt::UserRole,
        TimestampRole,
        DirectionRole,
        StatusRole,
        UuidRole
    };

    explicit ChatMessagesModel(ContactsModel& cm,
                               std::weak_ptr<darkspeak::Api::Buddy> buddy,
                               QObject *parent = nullptr);
    ~ChatMessagesModel();

    void OnMessageReceived(const darkspeak::Api::Message::ptr_t& message);

    // QAbstractItemModel interface
public:
    virtual int rowCount(const QModelIndex &parent) const override;
    virtual QVariant data(const QModelIndex &index, int role) const override;

public:
    Q_INVOKABLE void sendMessage(const QString& message);
    Q_INVOKABLE void remove(int index);


public slots:
//     OnlineStatus getOnlineStatus() const;
//     void setOnlineStatus(OnlineStatus status);
//     QUrl getOnlineStatusIcon() const;
       void AddMessage(darkspeak::Api::Message::ptr_t message);

signals: // signals can be emitted
//     void onlineStatusChanged(const OnlineStatus &status);
//     void onlineStatusIconChanged();
       void MessageReceived(darkspeak::Api::Message::ptr_t);

protected:
    // return the roles mapping to be used by QML
    virtual QHash<int, QByteArray> roleNames() const override;
private:
    int GetNumMessages() const;

    ContactsModel& parent_;
    std::weak_ptr<darkspeak::Api::Buddy> buddy_;
    darkspeak::Api::message_list_t messages_;
    std::shared_ptr<EventsMonitor> monitor_;
};

