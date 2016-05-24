

#include "log/WarLog.h"

#include "ContactsModel.h"
#include "ChatMessagesModel.h"
#include "ContactData.h"
#include "darkspeak-gui.h"

using namespace std;
using namespace darkspeak;
using namespace war;


ContactsModel::ContactsModel(Api& api, QObject *parent)
: QAbstractListModel(parent), api_{api}
{
    buddies_ = api_.GetBuddies();
    event_listener_ = make_shared<Events>(*this);
    api_.SetMonitor(event_listener_);
}

ContactsModel::~ContactsModel()
{
    LOG_DEBUG_FN << "Someone killed me.";
    event_listener_.reset();
}

int ContactsModel::rowCount(const QModelIndex &parent) const
{
    return buddies_.size();
}

QVariant ContactsModel::data(const QModelIndex &index, int role) const
{
    const auto row = index.row();
        if(row < 0 || row >= static_cast<decltype(row)>(buddies_.size())) {
        return {};
    }

    if (icache_.row != row) {
        icache_.row = row;
        icache_.info = buddies_.at(row)->GetInfo();
    }

    string rval;

    switch(role) {
    case VisualNameRole:
        rval = buddies_[row]->GetUiName();
        break;
    case IdRole:
        rval = icache_.info.id;
        break;
    case LastSeenRole:
        return Convert(icache_.info.last_seen);
    case StatusColorRole:
        switch(buddies_[row]->GetStatus()) {
        case Api::Status::OFF_LINE: // maps to xa in legacy TC
            return "dark red";
        case Api::Status::AVAILABLE:
            return "dark green";
        case Api::Status::BUSY:
            return "orange";
        case Api::Status::AWAY:
            return "dark yellow";
        case Api::Status::LONG_TIME_AWAY:
            return "brown";
        }
        break;
    case StatusRole:
        return Convert(buddies_[row]->GetStatus());
    case AnonymityLevelRole:
        return Convert(icache_.info.GetCurrentAnonymity());
    }

    return {rval.c_str()};
}

void ContactsModel::append(const QString &bid)
{
    Api::Buddy::Info bi;
    bi.id = bid.toStdString();
    bi.created_time = time(nullptr);

    emit beginInsertRows(QModelIndex(), 0,
                         static_cast<int>(buddies_.size()));
    LOG_DEBUG << "Adding buddy: " << log::Esc(bi.id);
    api_.AddBuddy(bi);
    buddies_ = api_.GetBuddies();
    emit endInsertRows();
}

void ContactsModel::remove(int index)
{

}

ContactsModel::OnlineStatus ContactsModel::getOnlineStatus() const
{
    return online_status_;
}

QHash<int, QByteArray> ContactsModel::roleNames() const
{
    static const QHash<int, QByteArray> names = {
        {VisualNameRole, "nickname"},
        {IdRole, "handle"},
        {LastSeenRole, "last_seen"},
        {StatusColorRole, "status_color"},
        {StatusRole, "status"},
        {AnonymityLevelRole, "alevel"}
    };

    return names;
}

void ContactsModel::setOnlineStatus(ContactsModel::OnlineStatus status)
{
    if (online_status_ != status) {
        LOG_DEBUG_FN << "Changing status - emitting handlers.";
        online_status_ = status;
        emit onlineStatusChanged(status);
        emit onlineStatusIconChanged();
    }
}

QUrl ContactsModel::getOnlineStatusIcon() const
{
    static const std::array<QUrl, 4> icons = {
        QUrl("qrc:/images/Disconnected-32.png"),
        QUrl("qrc:/images/Connecting-32.png"),
        QUrl("qrc:/images//Connected-32.png"),
        QUrl("qrc:/images//Disconnecting-32.png")
    };

    return icons.at(online_status_);
}



ContactsModel::Events::Events(ContactsModel &parent)
    : parent_{parent}
{

}

bool ContactsModel::Events::OnIncomingConnection(const EventMonitor::ConnectionInfo &info)
{
    return true;
}

bool ContactsModel::Events::OnAddNewBuddy(const EventMonitor::BuddyInfo &info)
{
    // TODO: Check if the buddy exists. If it does, return true, else
    //  return false and propose for the user to add the contact.
    return true;
}

void ContactsModel::Events::OnNewBuddyAdded(const EventMonitor::BuddyInfo &info)
{
    // Trigger list-refresh
}

void ContactsModel::Events::OnBuddyStateUpdate(const EventMonitor::BuddyInfo &info)
{
    // TODO: Find buddy and propagate the state change
}

void ContactsModel::Events::OnIncomingMessage(const EventMonitor::Message &message)
{
    // Notify the contact list entry.
}

void ContactsModel::Events::OnIncomingFile(const EventMonitor::FileInfo &file)
{

}

void ContactsModel::Events::OnOtherEvent(const EventMonitor::Event &event)
{
    switch(event.type) {
        case EventMonitor::Event::Type::PROTOCOL_CONNECTING:
            parent_.setOnlineStatus(OS_CONNECTING);
            break;
        case EventMonitor::Event::Type::PROTOCOL_DISCONNECTING:
            parent_.setOnlineStatus(OS_DISCONNECTING);
            break;
        case EventMonitor::Event::Type::MESSAGE_TRANSMITTED:
            break;
        case EventMonitor::Event::Type::UNKNOWN:
            break;
    }
}

void ContactsModel::Events::OnListening(const EventMonitor::ListeningInfo &endpoint)
{
    LOG_DEBUG_FN << "We are on-line";
    parent_.setOnlineStatus(OS_ONLINE);
}

void ContactsModel::Events::OnShutdownComplete(const EventMonitor::ShutdownInfo &info)
{
    parent_.setOnlineStatus(OS_OFF_LINE);
}

ChatMessagesModel* ContactsModel::getMessagesModel(int index)
{
    try {
        return new ChatMessagesModel(*this, buddies_.at(index));
    } catch (std::exception&) {
        ; // Out of bound index ?
    }

    return nullptr;

}

ContactData *ContactsModel::getContactData(int index)
{
    try {
        return new ContactData(buddies_.at(index), this);
    } catch (std::exception&) {
        ; // Out of bound index ?
    }

    return nullptr;
}
