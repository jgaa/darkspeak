
#include <assert.h>

#include "log/WarLog.h"

#include "darkspeak/darkspeak_impl.h"
#include "darkspeak/BuddyImpl.h"
#include "darkspeak/ImManager.h"
#include "darkspeak/ImProtocol.h"

#define LOCK std::lock_guard<std::mutex> lock(mutex_);

namespace darkspeak {
namespace impl {

BuddyImpl::BuddyImpl(const Buddy::Info& info,
    std::weak_ptr<ImManager> manager)
: info_{info}, manager_{manager}
{

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
    return info_.id;
}

Api::Buddy::MessageSendResult
BuddyImpl::SendMessage(const std::string& msg)
{
    ImProtocol::Message message;

    try {
        message.text = msg;
        GetProtocol()->SendMessage(*this, message);
        return Api::Buddy::MessageSendResult::SENT;
    } catch(const ExceptionNotConnected& ex) {
        // Save the message
        LOG_DEBUG << "The message could not be delivered. Adding it to queue.";
        {
            std::lock_guard<std::mutex> lock(mq_mutex_);
            outgoing_message_queue_.push_back(msg);
        }
        return Api::Buddy::MessageSendResult::SENT;
    }

    //return Api::Buddy::MessageSendResult::FAILED;
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
}

void BuddyImpl::OnOtherEvent(const EventMonitor::Event& event)
{
    if (event.type == EventMonitor::Event::Type::MESSAGE_TRANSMITTED) {
        SendQueuedMessage();
    }
}


/* Send one message. When we are notified that the message is transferred,
 * the notification handler will call us again.
 */
void BuddyImpl::SendQueuedMessage()
{
    {
        LOCK;
        if (status_ != Api::Status::AVAILABLE) {
                return;
        }
    }

    std::lock_guard<std::mutex> lock(mq_mutex_);
    if (!outgoing_message_queue_.empty()) {
        try {
            LOG_DEBUG << "Trying to send one message from the outqueue.";
            ImProtocol::Message message;
            message.text = outgoing_message_queue_.front();
            GetProtocol()->SendMessage(*this, message);
            outgoing_message_queue_.pop_front();
        } catch (const ExceptionNotConnected& ex) {

        }
    }
}


} // impl
} // darkspeak

