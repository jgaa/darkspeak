
#pragma once
#include <memory>
#include "darkspeak/Api.h"
#include "darkspeak/EventMonitor.h"

namespace darkspeak {

    /*! Events that a buddy can propagate out */
    class BuddyEventsMonitor {
    public:
        using ptr_t = std::shared_ptr<BuddyEventsMonitor>;

        virtual void OnStateChange(Api::Status status) = 0;
        virtual void OnOtherEvent(const EventMonitor::Event& event) = 0;
        virtual void OnMessageReceived(const Api::Message::ptr_t& message) = 0;
    };

}
