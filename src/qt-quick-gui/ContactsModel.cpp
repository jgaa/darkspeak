

#include "log/WarLog.h"

#include "darkspeak/EventMonitor.h"

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

    connect(this,
            SIGNAL(onBuddyStateMayHaveChanged(std::string)),
            this, SLOT(refreshBuddyState(std::string)));

    connect(this,
            SIGNAL(onBuddyMayHaveNewMessage(std::string)),
            this, SLOT(refreshBuddyMessages(std::string)));

    connect(this,
            SIGNAL(onBuddyAdded(std::string)),
            this, SLOT(addBuddyToList(std::string)));

    connect(this,
            SIGNAL(onBuddyDeleted(std::string)),
            this, SLOT(deleteBuddyFromList(std::string)));

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
            return "darkred";
        case Api::Status::AVAILABLE:
            return "limegreen";
        case Api::Status::BUSY:
            return "darkred";
        case Api::Status::AWAY:
            return "orangered";
        case Api::Status::LONG_TIME_AWAY:
            return "chocolate";
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
    LOG_DEBUG << "Adding buddy: " << log::Esc(bi.id);
    api_.AddBuddy(bi);
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
        icache_.row = -1;
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
    /* We will get this event from the protocol lever when the buddy
     * is cleared for communication. In that case, the
     * info.exists will be != EventMonitor::Existing::YES
     *
     * Existing means that the buddy exists in the IM manager.
     */

    if (info.exists != EventMonitor::Existing::YES) {
        return;
    }

    LOG_DEBUG_FN << "Buddy " << info.buddy_id << " is added in the manager.";
    emit parent_.onBuddyAdded(info.buddy_id);
}

void ContactsModel::Events::OnBuddyStateUpdate(const EventMonitor::BuddyInfo &info)
{
    emit parent_.onBuddyStateMayHaveChanged(info.buddy_id);
}

void ContactsModel::Events::OnIncomingMessage(const EventMonitor::Message &message)
{
    emit parent_.onBuddyMayHaveNewMessage(message.buddy_id);
    emit parent_.onBuddyStateMayHaveChanged(message.buddy_id);
}

void ContactsModel::Events::OnIncomingFile(const EventMonitor::FileInfo &file)
{

}

void ContactsModel::Events::OnOtherEvent(const EventMonitor::Event &event)
{
    LOG_DEBUG_FN << "Got event " << event.type;
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

int ContactsModel::FindBuddy(const std::string& id)
{
    for(size_t i = 0; i < buddies_.size(); ++i) {
        if (id.compare(buddies_[i]->GetId()) == 0) {
            return i;
        }
    }

    WAR_THROW_T(war::ExceptionNotFound, id);
}


void ContactsModel::refreshBuddyState(string id)
{
    try {
        icache_.row = -1;
        auto row = FindBuddy(id);
        auto mi = index(0, row);
        emit dataChanged(mi, mi);
    } WAR_CATCH_NORMAL;
}

void ContactsModel::refreshBuddyMessages(string id)
{
    refreshBuddyState(id);
}

bool ContactsModel::HaveBuddy(const string& id)
{
    try {
        FindBuddy(id);
        return true;
    } catch (war::ExceptionNotFound) {
        ;
    }

    return false;
}


void ContactsModel::addBuddyToList(string id)
{
    LOG_DEBUG_FN << "Adding buddy " << id << "to the UI.";

    // First make sure that we don't already have the buddy.
    if (HaveBuddy(id)) {
        return;
    }

    // We better add it to the same location as it is in the
    // managers list.
    auto base_list = api_.GetBuddies();
    for(size_t i = 0; i < base_list.size(); ++i) {
        if (id.compare(base_list[i]->GetId()) == 0) {
            emit beginInsertRows(QModelIndex(), i, i);
            buddies_ = base_list;
            emit endInsertRows();
            return;
        }
    }

    LOG_ERROR_FN << "Could not find buddy " << id << " in the list";
}

void ContactsModel::deleteBuddyFromList(string id)
{
    int ix = 0;
    try {
        ix = FindBuddy(id);
    }   catch(war::ExceptionNotFound) {
        return;
    }

    emit beginRemoveRows(QModelIndex(), ix, ix);
    buddies_.erase(buddies_.begin() + ix);
    emit endRemoveRows();
}
