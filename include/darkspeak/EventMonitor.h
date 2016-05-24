#pragma once

#include <memory>
#include <future>

#include <boost/asio.hpp>

#include "darkspeak/Api.h"


namespace darkspeak {

class FileMonitor;

/*! Interface to get incoming events
    *
    * This interface is used between the protocol layer and the
    * Im Manager layer, and between the Im Manager layer and the UI.
    *
    * \note These methods are called from worker threads and must
    *           return immediately.
    */
class EventMonitor {
public:
    using ptr_t =  std::shared_ptr<EventMonitor>;

    enum class Existing {
        DONT_KNOW,
        NO,
        YES
    };

    struct BuddyInfo {
        std::string buddy_id;
        std::string profile_name;
        std::string profile_text;
        std::string client;
        std::string client_version;
        Api::Presence precense = Api::Presence::OFF_LINE;
        Api::Status status = Api::Status::OFF_LINE;
        Existing exists = Existing::DONT_KNOW;
    };

    struct ConnectionInfo {
        std::string buddy_id;
        Existing exists = Existing::DONT_KNOW;
    };

    struct Message {
        std::string buddy_id;
        std::string message;
    };

    struct FileInfo {
        std::string buddy_id;
        std::string name;
        std::int64_t length = -1; // Unknown size
    };

    struct Event {
        enum class Type {
            UNKNOWN,
            MESSAGE_TRANSMITTED,
            PROTOCOL_CONNECTING,
            PROTOCOL_DISCONNECTING
        };

        Event() = default;
        Event(const Event&) = default;
        Event(Event &&) = default;
        Event(const std::string& buddyId, Type evType)
        : buddy_id{buddyId}, type{evType} {}

        Event(const std::string& buddyId, Type evType, boost::uuids::uuid uuidVal)
        : buddy_id{buddyId}, type{evType}, uuid{uuidVal} {}

        std::string buddy_id;
        Type type = Type::UNKNOWN;
        boost::uuids::uuid uuid;
    };

    struct ListeningInfo {
        boost::asio::ip::tcp::endpoint endpoint;
    };

    struct ShutdownInfo {

    };

    /*! Incoming connection from someone
        *
        * \return True if you want to proceed with the connection.
        */
    virtual bool OnIncomingConnection(const ConnectionInfo& info) = 0;

    /*! Someone requests to be added as a buddy
     *
     * \return true if the buddy was exists or was added
     */
    virtual bool OnAddNewBuddy(const BuddyInfo& info) = 0;

    /*! A new buddy was added
     *
     * \return true if the buddy was exists or was added. If false,
     *          the incoming connection will be closed.
     */
    virtual void OnNewBuddyAdded(const BuddyInfo& info) = 0;

    /*! A buddy's state was updates
        *
        */
    virtual void OnBuddyStateUpdate(const BuddyInfo& info) = 0;

    /*! An incoming message has arrived
        */
    virtual void OnIncomingMessage(const Message& message) = 0;

    /*! Someone requests to send us a file
        *
        * This action must be completed by calling back down
        * the layers to start the transfer.
        */
    virtual void OnIncomingFile(const FileInfo& file) = 0;

    /*! Some other event happened that the user may want to know about.
        */
    virtual void OnOtherEvent(const Event& event) = 0;

    /*! Listening for incoming connections */
    virtual void OnListening(const ListeningInfo& endpoint) = 0;

    /*! Shutdown Complete. All connections are closed */
    virtual void OnShutdownComplete(const ShutdownInfo& info) = 0;
};

} // namespace
