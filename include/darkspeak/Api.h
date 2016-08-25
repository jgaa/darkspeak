#pragma once

#include <string>
#include <memory>
#include <deque>
#include <vector>
#include <chrono>
#include <iostream>
#include <atomic>
#include <cstdint>

#include <boost/filesystem.hpp>
#include <boost/uuid/uuid.hpp>
#include <boost/uuid/uuid_generators.hpp>

#ifdef SendMessage
// Thank you SO much Micro$oft!
#	undef SendMessage
#endif

namespace darkspeak {

using path_t = boost::filesystem::path;

class EventMonitor;
class BuddyEventsMonitor;
class FileInfo;

struct AcceptFileTransferData {
    std::string buddy_id;
    boost::uuids::uuid uuid;
};

struct AbortFileTransferData {
    std::string buddy_id;
    boost::uuids::uuid uuid;
    std::string reason;
    path_t delete_this; // If set, the file will be deleted
};


/*! \class Api Api.h darkspeak/Api.h
 *
 * Interface to the dark Instant Message world.
 *
 * The goal is to have one outer interface that can be used
 * directly on an implementation, or via RPC redirect to a remote
 * proxy-server.
 */
class Api
{
public:
    virtual ~Api() = default;

    /*! What data to store regarding our buddy */
    enum class AnonymityLevel : int {

        /// Our default level - Maps to NORMAL unless otherwise specified.
        DEFAULT,

        /// It does not matter - store the lot
        TRIVIAL,

        /*! Log nothing, but it's OK to keep static data
         *
         * Incoming and outgoing messages are stored until they can be
         * delivered.
         */
        NORMAL,

        /*! Store only id and our nickname
         *
         * - Outgoing off-line messages are not stored.
         * - Incoming messages are stored at the proxy until
         *      they can be delivered to a client.
         */
        HIGH,

        /*! Store nothing - when the connection is closed,
         * all is forgotten.
         *
         * The proxy will relay messages but store nothing.
         */
        CRITICAL
    };

    /*! The presence or availability of someone */
    enum class Presence : int {
        OFF_LINE,
        CONNECTING,
        ON_LINE,
    };

    enum class Status : int {
        OFF_LINE,
        AVAILABLE,
        BUSY,
        AWAY,
        LONG_TIME_AWAY
    };


    class Message {
        public:
        using ptr_t = std::shared_ptr<Message>;
        using timestamp_t = std::chrono::system_clock::time_point;
        enum class Direction {
            OUTGOING,
            INCOMING
        };

        enum class Status {
            QUEUED,
            SENT,
            RECEIVED
        };

        Message(Direction dir, Status initialStatus, std::string content)
        : direction(dir), body{std::move(content)}
        {
            status = initialStatus;
        }

        const timestamp_t timestamp = std::chrono::system_clock::now();
        const boost::uuids::uuid uuid = boost::uuids::random_generator()();
        const Direction direction;
        std::atomic<Status> status;
        const std::string body;
    };

    using message_list_t = std::deque<std::shared_ptr<Message>>;

    class Buddy {
    public:
        using ptr_t = std::shared_ptr<Buddy>;

        enum class MessageSendResult {
            SENT,
            QUEEUED,
            FAILED
        };

        virtual ~Buddy() = default;

        struct Info {
            /// The ID needed to communicate with the buddy
            std::string id;
            /// The profile-name the buddy himself use.
            std::string profile_name;
            /// The buddy's own profile text
            std::string profile_text;
            /// Our nickname for the buddy
            std::string our_nickname;
            /// Our own notes regarding the buddy
            std::string our_general_notes;
            /// When we created this record
            time_t created_time = 0;
            /// When we first had communication with the buddy
            time_t first_contact = 0;
            /// The last time we had a message or event from the buddy
            time_t last_seen = 0;
            /// The anonymity level we want to use for this buddys information
            AnonymityLevel anonymity = AnonymityLevel::DEFAULT;
            /*! The anonymity level requested by the buddy.
             *
             * If our buddy request an anonymity level higher than
             * what we would otherwise use, we will honor our buddys
             * request and use that level for this entry.
             */
            AnonymityLevel required_anonymity = AnonymityLevel::DEFAULT;

            /// Try to connect when we go on-line.
            bool auto_connect = true;

            /*! Store conversations on disk.
             *
             * This option is ignored if the current AnonymityLevel is
             * larger than NORMAL.
             */
            bool store_conversations = false;


            /*! The client software used by the buddy */
            std::string client;

            /*! Avatar in png format */
            std::vector<std::uint8_t> avatar;

            AnonymityLevel GetCurrentAnonymity() const noexcept {
                return static_cast<AnonymityLevel>(std::max(
                    static_cast<int>(anonymity),
                    static_cast<int>(required_anonymity)));
            }

            bool CanBeSaved() const noexcept {
                return static_cast<int>(GetCurrentAnonymity())
                    <= static_cast<int>(AnonymityLevel::HIGH);
            }

            bool CanBeLogged() const noexcept {
                return CanBeSaved();
            }
        };

        using buddy_def_t = Buddy::Info;

        /*! Get the id */
        virtual std::string GetId() const = 0;

        /*! Get the current information
         */
        virtual Info GetInfo() const = 0;

        /*! Update the current information */
        virtual void SetInfo(Info info) = 0;

        virtual Presence GetPresence() const = 0;

        virtual Status GetStatus() const = 0;

        /*! Get a string that the UI can use to list the buddy
         */
        virtual std::string GetUiName() const = 0;

        /*! Returns true if the buddy has the auto_connect flag */
        virtual bool HasAutoConnect() const = 0;

        virtual bool CanBeLogged() const = 0;

        /*! Connect to the buddy.
         *
         * This is normally done when starting up.
         */
        virtual void Connect() = 0;

        /*! Send a utf-8 encoded text message
         *
         * \returns Pointer to a Message instance. This can be queried regarding
         *      the status of the message.
         */
        virtual Message::ptr_t SendMessage(const std::string& msg) = 0;

        /*! Send a file to the buddy.
         *
         * Currently, files can only be sent to buddies that are online.
         *
         * \returns the uuid of the file transfer.
         */
        virtual void SendFile(const FileInfo& fi) = 0;

        /*! Disconnect from the buddy
         *
         * If we have a connection to this buddy the connection is closed
         * and we will not connect again unless Connect() is called, or
         * the application is re-started and the auto_connect flag is
         * set.
         */
        virtual void Disconnect() = 0;

        /*! Delete this buddy.
         *
         * Any communication is closed and the buddy along with all
         * saved information is deleted.
         */
        virtual void Delete() = 0;

        /*! Get the conversation.
         *
         * \param after If present, return messages after this uuid in the list.
         *
         *
         * \note The conversation list maintained internally in the Buddy instance
         *       may be cleand up based on maintainance policies (like max number of
         *       messages to keep in memory, max time to live for messages in memory
         *       etc.). This means that a Message pointer received in one call to
         *       GetMessages() may or may not be returned if the merthod is
         *       called again.
         */
        virtual message_list_t
        GetMessages(const boost::uuids::uuid *after = nullptr) = 0;

        virtual void SetAvatar(std::vector<std::uint8_t> avatar) = 0;

        /*! Register a monitor to receive notifications */
        virtual void SetMonitor(const std::weak_ptr<BuddyEventsMonitor> monitor) = 0;
    };

    /*! Information about the local user.
     */
    struct Info {
        std::string id;
        Status status = Status::OFF_LINE;
        std::string profile_name;
        std::string profile_text;
        std::vector<char> avatar; //binary argb
    };

    using buddy_list_t = std::deque<Buddy::ptr_t>;

    /*! Get a complete list of all our buddies
     */
    virtual buddy_list_t GetBuddies() = 0;

    /*! Manually add a buddy to the buddy listb
     */
    virtual Buddy::ptr_t AddBuddy(const Buddy::Info& def) = 0;

    /*! Get one buddy
     *
     * \return Pointer to a bddy or nullptr
     */
    virtual Buddy::ptr_t GetBuddy(const std::string& id) = 0;

    /*! Remove a buddy
     *
     */
    virtual void RemoveBuddy(const std::string& id) = 0;


    /*! Start the secret Tor service and listen for incoming connections.
     *
     * Connect all buddies with auto_connect set.
     */
    virtual void GoOnline() = 0;

    /*! Close all connections and stop listening for incoming calls.
     *
     * \param local_only If true, the local application will disconnect.
     *          If we are connected to a proxy-server, we will disconnect
     *          from the proxy but leave it running and connected.
     *          If false, we will ask the proxy to disconnect all connections
     *          as well. It's proxy-interface will still be available for
     *          us in case we want to go back on-line again.
     */
    virtual void Disconnect(bool local_only = true) = 0;

    /*! Register an event-monitor to receive notifications.
     *
     */
    virtual void SetMonitor(std::shared_ptr<EventMonitor> monitor) = 0;

    /*! Accept a file transfer
     */
    virtual void AcceptFileTransfer(const AcceptFileTransferData& aftd) = 0;

    /*! Reject a file transfer
     */
    virtual void RejectFileTransfer(const AcceptFileTransferData& aftd) = 0;

    /*! Abort an ongoing file transfer
     *
     * The file is deleted.
     */
    virtual void AbortFileTransfer(const AbortFileTransferData& aftd) = 0;


    /*! Panic button.
     *
     * All buddy's that are connected will be notified that we pressed
     * the panic button. Then all connections are shut down. If we
     * use a proxy-server, that will also shut down entirely.
     *
     * \param message Message to send to all connected buddy's.
     * \param erase_data If true, all darkspeak data-files will be
     *          erased on all connected devices. If we use a proxy-server,
     *          all data-files will be erased also there. That means that
     *          all buddy's, conversation logs, - everything will be deleted.
     */
    virtual void Panic(std::string message, bool erase_data) = 0; // TODO: Implement

    static std::shared_ptr<Api> CreateInstance(path_t conf_file);
};

} //namespace

std::ostream& operator << (std::ostream& o, const darkspeak::Api::Status& v);
std::ostream& operator << (std::ostream& o, const darkspeak::Api::Presence& v);
std::ostream& operator << (std::ostream& o, const darkspeak::Api::Buddy& v);
