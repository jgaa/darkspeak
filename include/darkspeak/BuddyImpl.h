#pragma once
#include <memory>
#include <mutex>
#include "darkspeak/Api.h"

namespace darkspeak {

class ImProtocol;

namespace impl {

class ImManager;

/*! Real implementation of the Api interface.
 *
 * This implementation owns the data and the
 * hidden service(s).
 */
class BuddyImpl : public Api::Buddy,
    public std::enable_shared_from_this<BuddyImpl>
{
public:
    ~BuddyImpl();
    BuddyImpl(const Buddy::Info& info,
        std::weak_ptr<ImManager> manager);

    Info GetInfo() const override;

    void SetInfo(Buddy::Info info) override;
    Api::Presence GetPresence() const override;
    Api::Status GetStatus() const override;
    std::string GetUiName() const override;
    bool HasAutoConnect() const override;
    bool CanBeLogged() const override { return info_.CanBeLogged(); }
    void Connect() override;
    void SendMessage(const std::string& msg) override;
    void RegisterEventHandler() override;
    void Disconnect() override;
    void Delete() override;

private:
    std::shared_ptr<ImManager> GetManager();
    std::shared_ptr<ImProtocol> GetProtocol();

    mutable std::mutex mutex_;
    Api::Buddy::Info info_;
    Api::Status status_ = Api::Status::OFF_LINE;
    Api::Presence precense_ = Api::Presence::OFF_LINE;
    std::weak_ptr<ImManager> manager_;
};

} // impl
} // darkspeak

