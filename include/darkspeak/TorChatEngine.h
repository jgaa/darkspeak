#pragma once

#include <future>
#include <chrono>
#include <random>

#include <boost/property_tree/ptree.hpp>

#include "war_error_handling.h"

#include "darkspeak/darkspeak.h"
#include "darkspeak/ImProtocol.h"
#include "darkspeak/EventMonitor.h"

namespace darkspeak {
namespace impl {

class TorChatPeer;
class TorChatConnection;

/*! Tor Chat Protocol implementation
 *
 * One instance will handle one hidden service (TC id)
 */
class TorChatEngine : public ImProtocol,
    public std::enable_shared_from_this<TorChatEngine>
{
public:
    struct Request;

    struct Command {
        using cmd_fn_t =  std::function<void (const Request&)>;
        enum class Verb {
            UNKNOWN,
            ADD_ME,
            CLIENT,
            FILEDATA,
            FILEDATA_ERROR,
            FILEDATA_OK,
            FILENAME,
            FILE_STOP_SENDING,
            MESSAGE,
            PING,
            PONG,
            PROFILE_AVATAR,
            PROFILE_AVATAR_ALPHA,
            PROFILE_NAME,
            PROFILE_TEXT,
            REMOVE_ME,
            STATUS,
            VERSION,
        };

        enum class Valid {
            GREETING, // Initial connect / ping
            HANDSHAKE, // before pong
            AUTHENTICATED, // after pong
            ACCEPTED // when the upper layer have confirmed the buddy
        };

        Command(Verb ve,
                int a = 0,
                cmd_fn_t fn = nullptr,
                Valid va = Valid::AUTHENTICATED)
        : verb{ve}, args{a}, valid{va}, execute{fn} {}

        const Verb verb;
        const int args = 0;
        const Valid valid = Valid::AUTHENTICATED;
        const cmd_fn_t execute;
    };

    struct Request {
        const Command *cmd = nullptr;
        std::vector<std::string> params;
        std::string command_name;
        TorChatPeer *peer = nullptr;
        Direction direction = Direction::INCOMING;
        boost::asio::yield_context *yield = nullptr;
    };

    TorChatEngine(ImProtocol::get_pipeline_fn_t fn,
        Config& properties);

    ~TorChatEngine();

    // IM protocol implementation
    void Connect(Api::Buddy::ptr_t buddy) override;
    void SendMessage(Api::Buddy& buddy, const Api::Message::ptr_t& msg) override;
    void SendFile(Api::Buddy& buddy, const File& file,
        FileMonitor::ptr_t monitor) override;
    void Disconnect(Api::Buddy& buddy) override;
    void SetMonitor(std::shared_ptr<EventMonitor> monitor) override;
    void Listen(boost::asio::ip::tcp::endpoint endpoint) override;
    void Shutdown() override;
    void StartMonitor();

private:
    void SpawnConnect(const std::string& buddy_id);

    void Listen_(boost::asio::ip::tcp::endpoint endpoint,
                 std::promise<void>& promise);

    void Accept(boost::asio::ip::tcp::endpoint endpoint,
                std::weak_ptr<boost::asio::ip::tcp::acceptor> weak_acceptor,
                boost::asio::yield_context yield);

    void ConnectToPeer(TorChatPeer& peer,
                       boost::asio::yield_context& yield);

    void OnAccepted(std::shared_ptr< boost::asio::ip::tcp::socket > socket,
                    boost::asio::yield_context yield);

    std::vector<std::shared_ptr<EventMonitor>> GetMonitors();

    bool VerifyPing(boost::string_ref line, std::string& id,
                    std::string& cookie);

    void ProcessRequests(
        std::weak_ptr<TorChatPeer> weak_peer,
        Direction direction, bool sendPong,
        boost::asio::yield_context& yield);

    // For spawn
    void StartProcessingRequests(
        std::weak_ptr<TorChatPeer> weak_peer,
        Direction direction,
        boost::asio::yield_context yield);

    // For spawn
    void StartConnectToPeer(std::string& peer_id,
                            boost::asio::yield_context yield);

    std::shared_ptr<TorChatPeer> GetPeer(const std::string& id);

    std::shared_ptr<TorChatConnection> CreateConnection(
        Direction direction,
        std::shared_ptr< boost::asio::ip::tcp::socket > socket = nullptr) const;

    static void MonitorPeers(std::weak_ptr<TorChatEngine> weak_engine,
                      boost::asio::yield_context yield);

    void DisconnectPeer(TorChatPeer& peer);
    
    void DisconnectPeer(const std::string& id);

    std::unique_ptr<std::chrono::steady_clock::time_point>
    GetNewKeepAliveTime();

    auto GetNewStatusTimeout() {
        return std::make_unique<std::chrono::steady_clock::time_point>(
            std::chrono::steady_clock::now() + std::chrono::seconds(130));
    }

    std::unique_ptr<std::chrono::steady_clock::time_point>
    GetNewReconnectTime(TorChatPeer& peer);

    std::shared_ptr<TorChatPeer>CreatePeer(const std::string& id);

    void Reconnect(const std::shared_ptr<TorChatPeer>& peer);

    void CheckPeers(boost::asio::yield_context& yield);
    void CheckPeer(const std::shared_ptr<TorChatPeer>& peer,
                   boost::asio::yield_context& yield);

    // Event handlers
    bool EmitEventIncomingConnection(const EventMonitor::ConnectionInfo& info);
    bool EmitEventAddNewBuddy(const EventMonitor::BuddyInfo& info);
    void EmitEventBuddyStateUpdate(const EventMonitor::BuddyInfo& info);
    void EmitEventIncomingMessage(const EventMonitor::Message& msg);
    void EmitOtherEvent(const EventMonitor::Event& event);
    void EmitShutdownComplete(const EventMonitor::ShutdownInfo& info);
    void EmitEventListening(const EventMonitor::ListeningInfo& endpoint);

    // Requests
    void OnAddMe(const Request& req);
    void OnClient(const Request& req);
    void OnFileData(const Request& req);
    void OnFileDataError(const Request& req);
    void OnFileDataOk(const Request& req);
    void OnFilename(const Request& req);
    void OnFileStopSending(const Request& req);
    void OnMessage(const Request& req);
    void OnPing(const Request& req);
    void OnPong(const Request& req);
    void OnProfileAvatar(const Request& req);
    void OnProfileName(const Request& req);
    void OnProfileText(const Request& req);
    void OnRemoveMe(const Request& req);
    void OnStatus(const Request& req);
    void OnVersion(const Request& req);

    bool DoSendPing(TorChatPeer& peer, boost::asio::yield_context& yield);
    bool DoSendPong(TorChatPeer& peer, boost::asio::yield_context& yield);
    bool DoSendStatus(TorChatPeer& peer, boost::asio::yield_context& yield);
    bool DoSend(const std::string& command,
                std::initializer_list<std::string> args,
                TorChatPeer& peer, boost::asio::yield_context& yield,
                Direction direction = Direction::OUTGOING);
    boost::string_ref GetLine(TorChatConnection& conn,
                              boost::asio::yield_context& yield);
    static const std::string& ToString(Api::Status status);

    boost::asio::ip::tcp::endpoint GetTorEndpoint() const;

    Request Parse(boost::string_ref request) const;

    std::vector<std::weak_ptr<EventMonitor>> event_monitors_;
    std::shared_ptr<boost::asio::ip::tcp::acceptor> acceptor_;
    war::Pipeline& pipeline_;
    Config& config_;
    std::size_t connect_timeout_ = 1000 * 60 * 2; // 2 minutes in milliseconds
    std::map<std::string, std::shared_ptr<TorChatPeer>> peers_;
    std::map<std::string, Command> commands_;
    Stat current_stats_;
    std::mt19937 random_generator_;
    std::string id_;
};

} // impl
} // darkspeak

std::ostream& operator << (std::ostream& o,
                           const darkspeak::impl::TorChatEngine::Command::Valid& v);
