#include "log/WarLog.h"

#include "ChatMessagesModel.h"
#include "darkspeak-gui.h"

using namespace std;
using namespace war;
using namespace darkspeak;

ChatMessagesModel::ChatMessagesModel(ContactsModel& cm,
                                     std::weak_ptr<darkspeak::Api::Buddy> buddy,
                                     QObject* parent)
: QAbstractListModel(parent), parent_{cm}, buddy_{buddy}
{
    monitor_ = make_shared<EventsMonitor>(*this);

    connect(this,
            SIGNAL(MessageReceived(darkspeak::Api::Message::ptr_t)),
            this, SLOT(AddMessage(darkspeak::Api::Message::ptr_t)));

    auto real_buddy = buddy_.lock();
    messages_ = real_buddy->GetMessages();
    real_buddy->SetMonitor(monitor_); // Get notifications from the buddy
}

ChatMessagesModel::~ChatMessagesModel()
{
    {
        monitor_.reset();
    }
}


QHash< int, QByteArray > ChatMessagesModel::roleNames() const
{
    static const QHash<int, QByteArray> names = {
        {ContentRole, "content"},
        {TimestampRole, "timestamp"},
        {DirectionRole, "direction"},
        {StatusRole, "status"},
        {UuidRole, "uuid"}
    };

    return names;
}

int ChatMessagesModel::rowCount(const QModelIndex& parent) const
{
    return GetNumMessages();
}

QVariant ChatMessagesModel::data(const QModelIndex& index, int role) const
{
    const auto row = index.row();
    Api::Message::ptr_t message;


    if(row < 0 || row >= GetNumMessages()) {
        return {};
    }

    message = messages_[row];


    switch(role) {
        case ContentRole:
            return Convert(message->body);
        case TimestampRole:
            return Convert(message->timestamp);
        case DirectionRole:
            return (message->direction == Api::Message::Direction::INCOMING)
                ? MD_INCOMING : MD_OUTGOING;
        case StatusRole:
            switch(message->status) {
                case Api::Message::Status::QUEUED:
                    return MS_QUEUED;
                case Api::Message::Status::SENT:
                    return MS_SENT;
                case Api::Message::Status::RECEIVED:
                    return MS_RECEIVED;
            }
            break;
        case UuidRole:
            return Convert(message->uuid);
    }

    LOG_ERROR << "*** Unknown role " << role;
    return {};
}


void ChatMessagesModel::sendMessage(const QString& message)
{
    auto real_buddy = buddy_.lock();
    try {
        AddMessage(real_buddy->SendMessage(message.toStdString()));
    } WAR_CATCH_NORMAL;
}


void ChatMessagesModel::remove(int index)
{

}


int ChatMessagesModel::GetNumMessages() const
{
    return static_cast<int>(messages_.size());
}

void ChatMessagesModel::AddMessage(Api::Message::ptr_t message)
{
    LOG_DEBUG << "Adding new message to UI";
    const auto count = GetNumMessages();

    emit beginInsertRows(QModelIndex(), count, count);

    {
        messages_.push_back(message);
    }

    emit endInsertRows();
}


void ChatMessagesModel::OnMessageReceived(const Api::Message::ptr_t& message)
{
    LOG_DEBUG << "Emitting message to QT mail thread.";
    emit MessageReceived(message);
}

void ChatMessagesModel::EventsMonitor::OnMessageReceived(const Api::Message::ptr_t& message)
{
    parent_.OnMessageReceived(message);
}


void ChatMessagesModel::EventsMonitor::OnOtherEvent(const EventMonitor::Event& event)
{

}

void ChatMessagesModel::EventsMonitor::OnStateChange(Api::Status status)
{

}

