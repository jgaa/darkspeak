#pragma once
#include <mutex>
#include "darkspeak/Api.h"

namespace darkspeak {
namespace impl {


/*! Real implementation of the Api interface.
 *
 * This implementation owns the data and the
 * hidden service(s).
 */
class BuddyImpl : public Api::Buddy
{
public:
    ~BuddyImpl();
    BuddyImpl();

    Info GetInfo() const override;

    void SetInfo(Buddy::Info info) override;
    Api::Presence GetPresence() const override;
    Api::Status GetStatus() const override;
    std::string GetUiName() const override;
    void Connect() override;
    void SendMessage(const std::string& msg) override;
    void RegisterEventHandler() override;
    void Disconnect() override;
    void Delete() override;

private:
    mutable std::mutex mutex_;
    Api::Buddy::Info info_;
    Api::Status status_ = Api::Status::OFF_LINE;
    Api::Presence precense_ = Api::Presence::OFF_LINE;
};

} // impl
} // darkspeak

