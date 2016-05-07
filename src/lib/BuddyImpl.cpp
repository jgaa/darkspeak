
#include <assert.h>

#include "log/WarLog.h"

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

void BuddyImpl::SendMessage(const std::string& msg)
{
    ImProtocol::Message message;
    message.text = msg;
    GetProtocol()->SendMessage(*this, message);
}

void BuddyImpl::SetInfo(Buddy::Info info)
{
    LOCK;
    info_ = info;
}

void BuddyImpl::RegisterEventHandler()
{

}


} // impl
} // darkspeak

