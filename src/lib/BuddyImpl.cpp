
#include <assert.h>

#include "log/WarLog.h"

#include "darkspeak/darkspeak_impl.h"
#include "darkspeak/BuddyImpl.h"
#include "darkspeak/ImManager.h"
#include "darkspeak/ImProtocol.h"
#include "darkspeak/weak_container.h"

using namespace std;
using namespace war;

#define LOCK std::lock_guard<std::mutex> lock(mutex_);

std::ostream& operator << (std::ostream& o, const darkspeak::Api::Buddy& v) {
    static const std::string censored{"<censored>"};
    return o << "{buddy: "
        << (v.CanBeLogged() ? v.GetId() : censored)
        << ' '
        << v.GetUiName()
        << '}';
}

namespace darkspeak {
namespace impl {

BuddyImpl::BuddyImpl(const Buddy::Info& info,
    std::weak_ptr<ImManager> manager)
: info_{info}, manager_{manager}
{
    if (info_.created_time == 0) {
        info_.created_time = time(nullptr);
    }
}

BuddyImpl::~BuddyImpl()
{
}

std::shared_ptr< ImManager > BuddyImpl::GetManager()
{
    auto mgr = manager_.lock();
    if (!mgr) {
        WAR_THROW_T(war::ExceptionWeakPointerHasExpired, "No manager");
    }

    return mgr;
}

std::shared_ptr< ImProtocol > BuddyImpl::GetProtocol()
{
    return GetManager()->GetProtocol();
}


bool BuddyImpl::HasAutoConnect() const
{
    LOCK;
    return info_.auto_connect;
}

void BuddyImpl::Connect()
{
    if (CanBeLogged()) {
        LOG_DEBUG_FN << "Connecting with buddy : " << info_.id;
    } else {
        LOG_DEBUG_FN << "Connecting with anonymous buddy";
    }
    GetManager()->Connect(shared_from_this());
}

void BuddyImpl::Delete()
{
    GetManager()->RemoveBuddy(info_.id);
}

void BuddyImpl::Disconnect()
{
    if (CanBeLogged()) {
        LOG_DEBUG_FN << "Disconnecting buddy : " << info_.id;
    } else {
        LOG_DEBUG_FN << "Disconnecting anonymous buddy";
    }
    GetProtocol()->Disconnect(*this);
}

std::string BuddyImpl::GetId() const
{
    LOCK;
    return info_.id;
}


Api::Buddy::Info BuddyImpl::GetInfo() const
{
    LOCK;
    return info_;
}

Api::Status BuddyImpl::GetStatus() const
{
    LOCK;
    return status_;
}

Api::Presence BuddyImpl::GetPresence() const
{
    LOCK;
    return precense_;
}

std::string BuddyImpl::GetUiName() const
{
    LOCK;
    if (!info_.our_nickname.empty())
        return info_.our_nickname;
    if (!info_.profile_name.empty())
        return string("(") + info_.profile_name + ")";
    return info_.id;
}

std::shared_ptr<Api::Message> BuddyImpl::SendMessage(const std::string& msg)
{
    auto message = make_shared<Api::Message>(
        Api::Message::Direction::OUTGOING,
        Api::Message::Status::QUEUED,
        msg);

    {
        LOCK;
        conversation_.push_back(message);
    }

    try {
        GetProtocol()->SendMessage(*this, message);
    } catch(const ExceptionNotConnected&) {
        // Save the message
        LOG_DEBUG << "The message could not be delivered. Adding it to queue.";
        {
            std::lock_guard<std::mutex> lock(mq_mutex_);
            outgoing_message_queue_.push_back(message);
        }
    }

    return message;
}

void BuddyImpl::SetInfo(Buddy::Info info)
{
    LOCK;
    info_ = info;
}


void BuddyImpl::OnStateChange(Api::Status status)
{
    {
        LOCK;
        if (status == status_) {
            return;
        }

        status_ = status;
    }

    SendQueuedMessage();

    for(auto& monitor : GetMonitors()) {
        monitor->OnStateChange(status);
    }
}

void BuddyImpl::OnOtherEvent(const EventMonitor::Event& event)
{
    if (event.type == EventMonitor::Event::Type::MESSAGE_TRANSMITTED) {
        // TODO: Emit signal
        SendQueuedMessage();
    }

    for(auto& monitor : GetMonitors()) {
        monitor->OnOtherEvent(event);
    }
}

void BuddyImpl::OnMessageReceived(const Api::Message::ptr_t& message)
{
    {
        LOCK;
        conversation_.push_back(message);
    }

    for(auto& monitor : GetMonitors()) {
        monitor->OnMessageReceived(message);
    }
}



/* Send one message. When we are notified that the message is transferred,
 * the notification handler will call us again.
 */
void BuddyImpl::SendQueuedMessage()
{
    {
        LOCK;
        if (status_ == Api::Status::OFF_LINE) {
            return;
        }
    }

    std::lock_guard<std::mutex> lock(mq_mutex_);
    if (!outgoing_message_queue_.empty()) {
        try {
            LOG_DEBUG << "Trying to send one message from the outqueue.";
            auto message = outgoing_message_queue_.front();
            GetProtocol()->SendMessage(*this, message);
            outgoing_message_queue_.pop_front();
        } catch (const ExceptionNotConnected&) {
            ; // Do nothing
        }
    }
}

Api::message_list_t BuddyImpl::GetMessages(const boost::uuids::uuid* after)
{
    if (after == nullptr) {
        return conversation_;
    }

    bool found;
    Api::message_list_t list;

    LOCK;
    for(auto msg: conversation_) {
        if (msg->uuid == *after) {
            found = true;
        } else if (found) {
            list.push_back(msg);
        }
    }

    return list;
}

void BuddyImpl::SetMonitor(const weak_ptr<BuddyEventsMonitor> monitor)
{
    LOCK;
    event_monitors_.push_back(monitor);
}

vector< shared_ptr< BuddyEventsMonitor > > BuddyImpl::GetMonitors()
{
    LOCK;
    return GetValidObjects<BuddyEventsMonitor>(event_monitors_);
}


void BuddyImpl::UpdateLastSeenTimestamp()
{
    LOCK;
    info_.last_seen = time(nullptr);
    if (info_.first_contact == 0) {
        info_.first_contact = info_.last_seen;
    }
}

void BuddyImpl::Update(const EventMonitor::BuddyInfo& bi)
{
    LOCK;
    info_.client = bi.client + " " + bi.client_version;
    info_.profile_name = bi.profile_name;
    info_.profile_text = bi.profile_text;
}

void BuddyImpl::SendFile(const darkspeak::FileInfo& fi)
{
    GetProtocol()->SendFile(*this, fi);
}


} // impl
} // darkspeak

