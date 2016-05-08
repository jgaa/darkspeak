#pragma once
#include <memory>
#include <mutex>
#include "darkspeak/Api.h"
#include "darkspeak/EventMonitor.h"

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
    using ptr_t = std::shared_ptr<BuddyImpl>;

    ~BuddyImpl();
    BuddyImpl(const Buddy::Info& info,
        std::weak_ptr<ImManager> manager);

    std::string GetId() const override;
    Info GetInfo() const override;
    void SetInfo(Buddy::Info info) override;
    Api::Presence GetPresence() const override;
    Api::Status GetStatus() const override;
    std::string GetUiName() const override;
    bool HasAutoConnect() const override;
    bool CanBeLogged() const override { return info_.CanBeLogged(); }
    void Connect() override;
    Api::Buddy::MessageSendResult SendMessage(
        const std::string& msg) override;
    void Disconnect() override;
    void Delete() override;

    void OnStateChange(Api::Status status);
    void OnOtherEvent(const EventMonitor::Event& event);
private:
    void SendQueuedMessage();

    std::shared_ptr<ImManager> GetManager();
    std::shared_ptr<ImProtocol> GetProtocol();

    mutable std::mutex mutex_;
    Api::Buddy::Info info_;
    Api::Status status_ = Api::Status::OFF_LINE;
    Api::Presence precense_ = Api::Presence::OFF_LINE;
    std::weak_ptr<ImManager> manager_;
    mutable std::mutex mq_mutex_;
    std::list<std::string> outgoing_message_queue_;
};

} // impl
} // darkspeak

