
#include <assert.h>
#include "darkspeak/BuddyImpl.h"

#define LOCK std::lock_guard<std::mutex> lock(mutex_);

namespace darkspeak {
namespace impl {

BuddyImpl::~BuddyImpl()
{
}

BuddyImpl::BuddyImpl()
{

}

void BuddyImpl::Connect()
{

}

void BuddyImpl::Delete()
{

}

void BuddyImpl::Disconnect()
{

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

void BuddyImpl::RegisterEventHandler()
{
    assert(false && "Not implemented");
}

void BuddyImpl::SendMessage(const std::__cxx11::string& msg)
{
    assert(false && "Not implemented");
}

void BuddyImpl::SetInfo(Buddy::Info info)
{
    LOCK;
    info_ = info;
}

} // impl
} // darkspeak

