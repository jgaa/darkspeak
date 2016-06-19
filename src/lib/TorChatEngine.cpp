#include <assert.h>
#include <memory>
#include <sstream>

#include "war_uuid.h"

#include "darkspeak/darkspeak_impl.h"
#include "darkspeak/TorChatEngine.h"
#include "darkspeak/TorChatConnection.h"
#include "darkspeak/TorChatPeer.h"
#include "darkspeak/IoTimer.h"
#include "darkspeak/weak_container.h"
#include "darkspeak/Config.h"

using namespace std;
using namespace war;

//#define LOCK std::lock_guard<std::mutex> lock(mutex_);

std::ostream& operator << (std::ostream& o,
                           const boost::asio::ip::tcp::socket& v) {

    o << "{Socket #"
        << const_cast<boost::asio::ip::tcp::socket&>(v).native_handle();

    if (v.is_open()) {
        o << " local "
            << v.local_endpoint()
            << " <--> remote "
            << v.remote_endpoint();
    }

    return o << '}';
}

std::ostream& operator << (std::ostream& o,
                           const darkspeak::impl::TorChatEngine::Command::Valid& v) {
    static const array<string, 4> names
        = { "GREETING", "HANDSHAKE", "AUTHENTICATED", "ACCEPTED" };

    return o << names.at(static_cast<int>(v));
}



namespace darkspeak {

namespace impl {

TorChatEngine::TorChatEngine(ImProtocol::get_pipeline_fn_t fn,
    Config& config)
: pipeline_{fn()}, config_{config}, random_generator_{std::random_device()()}
{
    commands_ = {
        {"", {Command::Verb::UNKNOWN}},
        {"add_me", {Command::Verb::ADD_ME, 0,
            [this] (const Request& req) { OnAddMe(req); }}},
        {"client", {Command::Verb::CLIENT, 1,
            [this] (const Request& req) { OnClient(req); }}},
        {"filedata", {Command::Verb::FILEDATA, 2,
            [this] (const Request& req) { OnFileData(req); },
            Command::Valid::ACCEPTED }},
        {"filedata_error", {Command::Verb::FILEDATA_ERROR, 2,
            [this] (const Request& req) { OnFileDataError(req); },
            Command::Valid::ACCEPTED }},
        {"filedata_ok", {Command::Verb::FILEDATA_OK, 2,
            [this] (const Request& req) { OnFileDataOk(req); },
            Command::Valid::ACCEPTED }},
        {"filename", {Command::Verb::FILENAME, 4,
            [this] (const Request& req) { OnFilename(req); },
            Command::Valid::ACCEPTED }},
        {"file_stop_sending", {Command::Verb::FILE_STOP_SENDING, 1,
            [this] (const Request& req) { OnFileStopSending(req); },
            Command::Valid::ACCEPTED }},
        {"message", {Command::Verb::MESSAGE, -1,
            [this] (const Request& req) { OnMessage(req); },
            Command::Valid::ACCEPTED}},
        {"ping", {Command::Verb::PING, 2,
            [this] (const Request& req) { OnPing(req); },
            Command::Valid::GREETING }},
        {"pong", {Command::Verb::PONG, 1,
            [this] (const Request& req) { OnPong(req); },
            Command::Valid::HANDSHAKE }},
        {"profile_avatar", {Command::Verb::PROFILE_AVATAR, -1,
            [this] (const Request& req) { OnProfileAvatar(req); }}},
        {"profile_avatar_alpha", {Command::Verb::PROFILE_AVATAR_ALPHA, -1,
            [this] (const Request& req) { OnProfileAvatar(req); }}},
        {"profile_name", {Command::Verb::PROFILE_NAME, -1,
            [this] (const Request& req) { OnProfileName(req); }}},
        {"profile_text", {Command::Verb::PROFILE_TEXT, -1,
            [this] (const Request& req) { OnProfileText(req); }}},
        {"remove_me", {Command::Verb::REMOVE_ME, 0,
            [this] (const Request& req) { OnRemoveMe(req); },
            Command::Valid::ACCEPTED}},
        {"status", {Command::Verb::STATUS, 1,
            [this] (const Request& req) { OnStatus(req); }}},
        {"version", {Command::Verb::VERSION, 1,
            [this] (const Request& req) { OnVersion(req); }}}
    };
}

TorChatEngine::~TorChatEngine()
{

}


void TorChatEngine::Connect(Api::Buddy::ptr_t buddy)
{
    LOG_NOTICE << "Connecting to " << *buddy;
    SpawnConnect(buddy->GetInfo().id);
}

void TorChatEngine::SpawnConnect(const string& buddy_id)
{
    boost::asio::spawn(
        pipeline_.GetIoService(),
        bind(&TorChatEngine::StartConnectToPeer, this,
                buddy_id,
                std::placeholders::_1));
}

// TODO: Make thread safe
void TorChatEngine::Disconnect(Api::Buddy& buddy)
{
    LOG_NOTICE << "Disconnecting from " << buddy;
    auto id = buddy.GetId();
    auto peer = GetPeer(id);
    if (peer) {
       DisconnectPeer(*peer);
    }
}

void TorChatEngine::DisconnectPeer(TorChatPeer& peer)
{
    LOG_DEBUG << "Disconnecting " << peer;
    peer.Close();
    peer.info.status = Api::Status::OFF_LINE;
    peer.info.precense = Api::Presence::OFF_LINE;
    EmitEventBuddyStateUpdate(peer.info);
    peers_.erase(peer.GetId());
}


void TorChatEngine::SendMessage(Api::Buddy& buddy,
                                const Api::Message::ptr_t& msg)
{
    pipeline_.PostSynchronously({[&]() {
        auto id = buddy.GetId();
        auto peer = GetPeer(id);

        if (!peer || (peer->GetState() != TorChatPeer::State::READY)) {
            WAR_THROW_T(ExceptionNotConnected, "Peer is not connected");
        }

        boost::asio::spawn(pipeline_.GetIoService(),
            [this, peer, msg](boost::asio::yield_context yield) {
                static const string message_verb{"message"};
                LOG_DEBUG << "Sending message to " << *peer;

                DoSend(message_verb, {msg->body}, *peer, yield);
                msg->status = Api::Message::Status::SENT;

                EmitOtherEvent({peer->GetId(),
                    EventMonitor::Event::Type::MESSAGE_TRANSMITTED,
                    msg->uuid});
        });
    }, "SendMessage"});
}

void TorChatEngine::SetMonitor(shared_ptr<EventMonitor> monitor)
{
    pipeline_.PostSynchronously({
        [&]() {
            event_monitors_.push_back(monitor);
        }, "SetMonitor"});
}

vector<shared_ptr<EventMonitor>> TorChatEngine::GetMonitors()
{
    return GetValidObjects<EventMonitor>(event_monitors_);
}

void TorChatEngine::Listen(boost::asio::ip::tcp::endpoint endpoint)
{
    std::promise<void> promise;
    auto future = promise.get_future();

    pipeline_.Post({[this]() {
         EventMonitor::Event event;
         event.type = EventMonitor::Event::Type::PROTOCOL_CONNECTING;
    }, "Emitting Connecting state"});

    pipeline_.Dispatch({[this, endpoint, &promise]() {
        Listen_(endpoint, promise);
    }, "Listen"});

    future.wait();
}

void TorChatEngine::Listen_(boost::asio::ip::tcp::endpoint endpoint,
                                    std::promise<void>& promise)
{

    WAR_ASSERT(!acceptor_);

    try {
        acceptor_ = make_shared<boost::asio::ip::tcp::acceptor>(pipeline_.GetIoService());
        acceptor_->open(endpoint.protocol());
        acceptor_->set_option(boost::asio::ip::tcp::acceptor::reuse_address(true));
        acceptor_->bind(endpoint);
        acceptor_->listen();

        std::weak_ptr<boost::asio::ip::tcp::acceptor> weak_acceptor = acceptor_;

        boost::asio::spawn(
            pipeline_.GetIoService(),
            bind(&TorChatEngine::Accept,
                this, endpoint, weak_acceptor, std::placeholders::_1));

        promise.set_value();
    } WAR_CATCH_ALL_EF(promise.set_exception(std::current_exception()));

}

void TorChatEngine::Accept(boost::asio::ip::tcp::endpoint endpoint,
                           std::weak_ptr<boost::asio::ip::tcp::acceptor> weak_acceptor,
                           boost::asio::yield_context yield)
{
    LOG_NOTICE << "Starting listening on " << endpoint;

    {
        EventMonitor::ListeningInfo endp;
        endp.endpoint = endpoint;
        EmitEventListening(endp);
    }

    while(true) {
        auto acceptor = weak_acceptor.lock();
        if (!acceptor || !acceptor->is_open()) {
            break; // done
        }

        boost::system::error_code ec;

        auto socket = make_shared<boost::asio::ip::tcp::socket>(
            pipeline_.GetIoService());
        LOG_TRACE1_FN << "Created socket " << *socket
            << " on " << pipeline_
            << " from endpoint " << endpoint;

        acceptor->async_accept(*socket, yield[ec]);

        if (!acceptor || !acceptor->is_open()) {
            break; // done
        }

        if (!ec) {
            ++current_stats_.num_incoming_connections;
            LOG_DEBUG << "Incoming connection: " << *socket;
            try {
                boost::asio::spawn(
                    pipeline_.GetIoService(),
                    bind(&TorChatEngine::OnAccepted, this,
                         socket,
                         std::placeholders::_1));

            } WAR_CATCH_ERROR;
        } else /* ec */ {
            LOG_WARN_FN << "Accept error: " << ec;
        }
    } // while acceptor exists and is open

    LOG_DEBUG_FN << "Acceptor at " << endpoint << " is done.";
}

TorChatConnection::ptr_t
TorChatEngine::CreateConnection(
    Direction direction,
    shared_ptr< boost::asio::ip::tcp::socket > socket) const
{
    std::string name
        = string(direction == Direction::INCOMING ? "incoming" : "outgoing")
        + "/" + get_uuid_as_string();

    if (socket) {
        return  make_shared<TorChatConnection>(name, pipeline_, socket);
    }
    return make_shared<TorChatConnection>(name, pipeline_);
}

boost::string_ref TorChatEngine::GetLine(TorChatConnection& conn,
                                         boost::asio::yield_context& yield)
{
    auto rval = conn.GetLine(yield);
    ++current_stats_.packets_received;
    current_stats_.bytes_received += rval.size();
    return rval;
}


void TorChatEngine::OnAccepted(
    shared_ptr< boost::asio::ip::tcp::socket > socket,
    boost::asio::yield_context yield)
{
    auto in_conn = CreateConnection(Direction::INCOMING, socket);

    try  {
        /* Before we do anything with this connection, we must
         * get the greeting ping
         */
        auto timer = IoTimer::Create(connect_timeout_, in_conn);
        auto ping_line = GetLine(*in_conn, yield);

        string id, cookie;
        if (!VerifyPing(ping_line, id, cookie)) {
            LOG_DEBUG << "Ping verification failed. Dropping " << *socket;
            return;
        }

        auto peer = GetPeer(id);
        if (peer) {
            LOG_DEBUG << "Connection from known peer " << *peer;
            const auto peer_cookie = peer->GetPeerCookie();
            if (!peer_cookie.empty() && cookie.compare(peer_cookie) != 0) {
                LOG_WARN << "I received a ping from a peer "
                    << " with another cookie ("
                    << log::Esc(cookie)
                    << ") than the one currently in use ("
                    << log::Esc(peer->GetPeerCookie())
                    << "). I will disconnect the connecting peer, assuming that "
                    <<" the existing one is legitimate.";
                return;
            }
            auto existing_conn = peer->GetInConnection();
            if (existing_conn) {
                LOG_WARN << "I got a new connection from a peer. I will"
                    " switch to that connection now.";
            }
            if (peer_cookie.empty()) {
                peer->SetPeerCookie(cookie);
            }
            peer->SetInConnection(nullptr); // reset
            peer->SetInConnection(in_conn);
            peer->SetReceivedPong(false);

            // Do not downgrade the state if the incoming connection is
            // related to an outgoing connect.
            if (peer->initiative == Direction::INCOMING) {
                peer->SetState(TorChatPeer::State::ACCEPTING);
            }
        } else {
            if (!EmitEventIncomingConnection(EventMonitor::ConnectionInfo(id))) {
                LOG_DEBUG << "Dropping connection to " << *in_conn
                    << ": Rejected.";
                in_conn->Close();
                return;
            }
            peer = CreatePeer(id);
            peer->SetPeerCookie(cookie);
            peer->SetInConnection(in_conn);
            peers_[id] = peer;
        }
        WAR_ASSERT(peer);
        peer->UpgradeState(TorChatPeer::State::ACCEPTING);

        peer->SetReceivedPing();
        std::weak_ptr<TorChatPeer> weak_peer = peer;

        bool send_pong = false;
        if (peer->HaveOutConnection()) {
            send_pong = true;
        } else {
            ConnectToPeer(*peer, yield);

            // Enter event loop in new context
            boost::asio::spawn(
                pipeline_.GetIoService(),
                bind(
                    &TorChatEngine::StartProcessingRequests, this,
                    weak_peer, Direction::OUTGOING,
                    placeholders::_1));
        }

        // Enter event loop in this context
        peer.reset();
        timer->Cancel();
        timer.reset();
        ProcessRequests(weak_peer, Direction::INCOMING, send_pong, yield);

    } WAR_CATCH_ALL_EF(return);
}

shared_ptr< TorChatPeer > TorChatEngine::CreatePeer(const string& id)
{
    LOG_TRACE1_FN << "Creating peer " << id;
    auto peer =  make_shared<TorChatPeer>(id);
    peer->received_status_timeout = GetNewStatusTimeout();
    peers_[id] = peer;
    return peer;
}


void TorChatEngine::StartProcessingRequests(weak_ptr< TorChatPeer > weak_peer,
                                            Direction direction,
                                            boost::asio::yield_context yield)
{
    try {
        ProcessRequests(weak_peer, direction, false, yield);
    } WAR_CATCH_ALL_E;
}

void TorChatEngine::ProcessRequests(weak_ptr< TorChatPeer > weak_peer,
                                    Direction direction,
                                    bool sendPong,
                                    boost::asio::yield_context& yield)
{
    static const string not_implemented{"not_implemented "};

    while(true) {
        auto peer = weak_peer.lock();
        if (!peer || (peer->GetState() == TorChatPeer::State::DONE)) {
            LOG_TRACE1_FN << "The peer is gone. Exiting request loop.";
            return;
        }

        if (sendPong) {
            DoSendPong(*peer, yield);
            sendPong = false;
        }

        TorChatConnection::ptr_t conn;

        if (direction == Direction::INCOMING) {
            conn = peer->GetInConnection();
        } else {
            conn = peer->GetOutConnection();
        }

        if (!conn || !conn->IsConnected()) {
            LOG_TRACE1_FN << "The peer is gone. Exiting request loop.";
            return;
        }

        peer.reset(); // Release reference while we wait for IO
        auto req = GetLine(*conn, yield);
        peer = weak_peer.lock();
        if (!peer || (peer->GetState() == TorChatPeer::State::DONE)) {
            continue; // exit
        }

        Request request = Parse(req);
        request.peer = peer.get();
        request.direction = direction;
        request.yield = &yield;

        // Minimum state required to execute the request
        static const std::array<TorChatPeer::State, 4> state_map = {
             TorChatPeer::State::ACCEPTING,     // GREETING
             TorChatPeer::State::AUTENTICATING, // HANDSHAKE
             TorChatPeer::State::AUTHENTICATED, // AUTHENTICATED
             TorChatPeer::State::READY          // ACCEPTED
        };

        if (compare_enum(peer->GetState(),
            state_map.at(static_cast<int>(request.cmd->valid))) < 0) {

            LOG_WARN << "The command " << log::Esc(request.command_name)
                    << " is not allowed when the peer is at state "
                    << request.cmd->valid
                    << " and the command has validity "
                    <<  peer->GetState();

            continue;
        }

        try {
            if (request.cmd->execute) {

                LOG_DEBUG << "Executing incoming request "
                    << log::Esc(request.command_name)
                    << " from peer "
                    << peer->GetId();

                request.cmd->execute(request);
                continue;
            }
        } catch (ExceptionDisconnectNow& ex) {
            LOG_DEBUG << "Disconnecting " << *peer << ":" << ex;

        } WAR_CATCH_ALL_E;


        auto outconn = peer->GetOutConnection();
        if (outconn) {
            std::string buffer = not_implemented + request.command_name;
            outconn->SendLine(buffer, yield);  // Do we need to catch errors?
        }
    } // loop
}

TorChatPeer::ptr_t TorChatEngine::GetPeer(const string& id)
{
    auto it = peers_.find(id);
    if (it != peers_.end()) {
        return it->second;
    }
    return {};
}


void TorChatEngine::StartConnectToPeer(std::string& peer_id,
                                       boost::asio::yield_context yield)
{
    auto peer = GetPeer(peer_id);
    if (!peer) {
        LOG_DEBUG << "Failed to find peer " << log::Esc(peer_id)
            << ". Creating instance.";

        peer = CreatePeer(peer_id);
        peer->initiative = Direction::OUTGOING;
    }

    try {
        peer->info.precense = Api::Presence::CONNECTING;
        peer->UpgradeState(TorChatPeer::State::CONNECTING);
        ConnectToPeer(*peer, yield);
        std::weak_ptr<TorChatPeer> weak_peer = peer;
        peer.reset();
        ProcessRequests(weak_peer, Direction::OUTGOING, false, yield);
    } WAR_CATCH_ALL_E;
}

void TorChatEngine::ConnectToPeer(TorChatPeer& peer,
                                  boost::asio::yield_context& yield)
{
    auto outbound = CreateConnection(Direction::OUTGOING);
    if (!peer.SetOutConnection(outbound)) {
        LOG_WARN_FN << "Peer already have an outbound connection.";
        return;
    }
    id_ = config_.Get<string>(Config::HANDLE);
    peer.retry_connect_time = GetNewReconnectTime(peer);
    peer.UpgradeState(TorChatPeer::State::CONNECTING);
    auto timer = IoTimer::Create(connect_timeout_, outbound);
    auto onion_host = peer.GetId() + ".onion";
    outbound->Connect(onion_host, GetTorEndpoint(), yield);
    ++current_stats_.num_outgoing_connections;
    if (peer.GetOutConnection()->IsConnected()) {
        peer.UpgradeState(TorChatPeer::State::AUTENTICATING);
    }
    DoSendPing(peer, yield);
    if (peer.GetReceivedPing()) {
        DoSendPong(peer, yield);
    }

    timer->Cancel();
}


boost::asio::ip::tcp::endpoint TorChatEngine::GetTorEndpoint() const
{
    const auto hostname = config_.Get(Config::TOR_HOSTNAME,
                                      Config::TOR_HOSTNAME_DEFAULT);

    const auto port = config_.Get<unsigned short>(Config::TOR_PORT,
                                                  Config::TOR_PORT_DEFAULT);

    return {boost::asio::ip::address::from_string(hostname), port};
}


bool TorChatEngine::VerifyPing(boost::string_ref line,
        string& id, string& cookie)
{
    Request req = Parse(line);

    if (req.cmd->verb != Command::Verb::PING) {
        LOG_WARN_FN << "Received greeting that was not ping. Yack!";
        return false;
    }

    if (req.params.size() != 2) {
        LOG_WARN_FN << "Received ping with param_count <> 2. Yack!";
        return false;
    }

    id = req.params.at(0);
    cookie = req.params.at(1);


    // TODO: Add regex to verify the validity of the id.
    if (id.size() != 16) {
        LOG_WARN_FN << "Received id with wrong size: "
            << log::Esc(id);
        return false;
    }

    LOG_DEBUG << "Got ping from " << log::Esc(id)
        << " with cookie " << log::Esc(cookie);

    return true;
}

void TorChatEngine::SendFile(Api::Buddy& buddy, const ImProtocol::File& file, ImProtocol::FileMonitor::ptr_t monitor)
{
    assert(false && "Not implemented");
}


void TorChatEngine::Shutdown()
{
    pipeline_.Post({[this]() {
         EventMonitor::Event event;
         event.type = EventMonitor::Event::Type::PROTOCOL_DISCONNECTING;
    }, "Emitting Disconnecting state"});

    pipeline_.Post({[&]() {

        LOG_NOTICE << "Shutting down " << id_;

        // Stop the listening socket
        LOG_TRACE1_FN << "Closing listener.";
        if (acceptor_) {
            acceptor_->close();
            acceptor_.reset();
        }

        LOG_TRACE1_FN << "Removing peers.";
        while (!peers_.empty()) {
            auto peer = peers_.begin()->second;
            DisconnectPeer(*peer);
        }

        EmitShutdownComplete({});

    // Close all peers
    }, "Shutting down TorChatEngine"});
}

TorChatEngine::Request TorChatEngine::Parse(boost::string_ref request) const
{
    boost::string_ref verb;
    auto pos = request.find(' ');
    if (pos == request.npos) {
        verb = request;
    } else {
        verb = { request.data(), pos };
        ++pos;
        request = { request.data() + pos, request.size() - pos};
    }

    Request rval;
    rval.command_name = verb.to_string();

    auto cmd_it = commands_.find(rval.command_name);
    if (cmd_it != commands_.end()) {
        rval.cmd = &cmd_it->second;
    } else {
        rval.cmd = &commands_.find("")->second; // The unknown command
    }

    const auto num_args = rval.cmd->args;

    if (num_args == -1) {
        // One optional argument
        if (request.empty()) {
            rval.params.resize(1); // One empty arg
        } else {
            rval.params.emplace_back(request.data(), request.size());
        }
    } else {
        for(int argc = 0; argc < num_args; ++argc) {
            LOG_TRACE4_FN << "request is: " << log::Esc(request.to_string());
            const bool last_arg = argc == (num_args -1);

            auto pos = last_arg ? request.size() : request.find(' ');

            if (pos == request.npos) {
                WAR_THROW_T(war::ExceptionParseError, "Too few arguments");
            }

            rval.params.emplace_back(request.data(), pos);

            if (!last_arg) {
                ++pos;
                request = {request.data() + pos, request.size() - pos};
            }
        }
    }

    LOG_TRACE1 << "Parsed command " << log::Esc(rval.command_name)
        << " with " << rval.params.size() << " arguments.";

    return rval;
}

void TorChatEngine::OnAddMe(const TorChatEngine::Request& req)
{
    if (!EmitEventAddNewBuddy(req.peer->info)) {
        WAR_THROW_T(ExceptionDisconnectNow, "Buddy was rejected");
    }

    req.peer->UpgradeState(TorChatPeer::State::READY);
    req.peer->info.precense = Api::Presence::ON_LINE;
}

void TorChatEngine::OnFileData(const TorChatEngine::Request& req)
{

}

void TorChatEngine::OnFileDataError(const TorChatEngine::Request& req)
{

}

void TorChatEngine::OnFileDataOk(const TorChatEngine::Request& req)
{

}

void TorChatEngine::OnFilename(const TorChatEngine::Request& req)
{
    // TODO: Add event
}

void TorChatEngine::OnFileStopSending(const TorChatEngine::Request& req)
{

}

void TorChatEngine::OnMessage(const TorChatEngine::Request& req)
{
    ++current_stats_.messages_received;
    EventMonitor::Message msg;
    msg.buddy_id = req.peer->GetId();
    msg.message = move(req.params.at(0));
    EmitEventIncomingMessage(msg);
}

void TorChatEngine::OnClient(const TorChatEngine::Request& req)
{
    req.peer->info.client = req.params.at(0);
}

void TorChatEngine::OnPing(const TorChatEngine::Request& req)
{
    DoSendPong(*req.peer, *req.yield);
}

void TorChatEngine::OnPong(const TorChatEngine::Request& req)
{
    LOG_DEBUG_FN << "Got pong request from "
        << (req.direction == Direction::INCOMING ? "incoming" : "outgoing")
        << " connection from peer " << req.peer->GetId();

    req.peer->reconnect_count = 0;
    req.peer->received_status_timeout = GetNewStatusTimeout();
    req.peer->retry_connect_time.reset();

    if (req.direction == Direction::OUTGOING) {
        // Not so intertesting. We want to see the pong in the inbound connection.
        LOG_DEBUG_FN << "Ignoring pong from outbound connection.";
        return;
    }

    if (!req.peer->GetSentPing()) {
        WAR_THROW_T(ExceptionDisconnectNow, "Received pong before I sent ping!");
    }

    if (req.peer->GetMyCookie().compare(req.params.at(0)) != 0) {
        WAR_THROW_T(ExceptionDisconnectNow, "Received pong with wrong cookie!");
    }

    if (req.peer->GetState() == TorChatPeer::State::READY) {
        LOG_DEBUG_FN << "We are already in READY state so I'm ignoring this pong.";
        return;
    }

    if (req.peer->HasBeenReady()) {
        req.peer->UpgradeState(TorChatPeer::State::READY);
        req.peer->info.precense = Api::Presence::ON_LINE;
    } else {
        req.peer->UpgradeState(TorChatPeer::State::AUTHENTICATED);
    }

    // TODO: Set a timer for how long we will keep the connection berfore we
    //          get to READY state (status or add_me?)

    LOG_DEBUG_FN << "Sending initial info...";

    // It's the first pong message. Let's identify ourself.
    WAR_ASSERT(req.peer && req.yield);
    DoSendPing(*req.peer, *req.yield);
    if (req.peer->initiative == Direction::OUTGOING) {
        DoSendPong(*req.peer, *req.yield);
    }
    DoSend("client", {GetClientName()}, *req.peer, *req.yield);
    DoSend("version", {GetClientVersion()}, *req.peer, *req.yield);

    const auto info = config_.GetInfo();

    if (!info.profile_name.empty()) {
        DoSend("profile_name", {info.profile_name}, *req.peer, *req.yield);
    }
    if (!info.profile_text.empty()) {
        DoSend("profile_text", {info.profile_text}, *req.peer, *req.yield);
    }
    // TODO: Add avatar
    DoSend("add_me", {}, *req.peer, *req.yield);
    DoSendStatus(*req.peer, *req.yield);

    req.peer->SetReceivedPong();

}

bool TorChatEngine::DoSend(const string& command,
                           initializer_list< string > args,
                           TorChatPeer& peer,
                           boost::asio::yield_context& yield,
                           Direction direction)
{
    auto conn = (direction == Direction::OUTGOING)
        ? peer.GetOutConnection()
        : peer.GetInConnection();

    if (!conn) {
        return false;
    }

    std::ostringstream buffer;
    buffer << command;
    for(const auto& arg : args) {
        buffer << ' ' << arg;
    }

    current_stats_.bytes_sent += conn->SendLine(buffer.str(), yield);
    ++current_stats_.messages_sent;
    return true;
}


bool TorChatEngine::DoSendStatus(TorChatPeer& peer, boost::asio::yield_context& yield) {
    static const string status{"status"};

    // TODO: Do not send status until we are in TorChatPeer::State::READY state
    if (DoSend(status, {ToString(config_.GetInfo().status)}, peer, yield)) {
        peer.next_keep_alive_time = GetNewKeepAliveTime();
        return true;
    }
    return false;
}

bool TorChatEngine::DoSendPing(TorChatPeer& peer, boost::asio::yield_context& yield) {
    static const string ping{"ping"};

    auto rval = DoSend(ping,
                       {
                           id_,
                           peer.GetMyCookie()
                       },
                       peer, yield);

    if (rval) {
        peer.SetSentPing();
    }

    return rval;
}

bool TorChatEngine::DoSendPong(TorChatPeer& peer,
                               boost::asio::yield_context& yield) {
    static const string pong{"pong"};

    auto rval = DoSend(pong, { peer.GetPeerCookie() }, peer, yield);

    if (rval) {
        peer.SetSentPong();
    }

    return rval;
}


void TorChatEngine::OnProfileAvatar(const TorChatEngine::Request& req)
{

}

void TorChatEngine::OnProfileName(const TorChatEngine::Request& req)
{
    req.peer->info.profile_name = req.params.at(0);
}

void TorChatEngine::OnProfileText(const TorChatEngine::Request& req)
{
    req.peer->info.profile_text = req.params.at(0);
}

void TorChatEngine::OnRemoveMe(const TorChatEngine::Request& req)
{
    // TODO: sEND EVENT

    LOG_DEBUG_FN << "Removing peer " << *req.peer;

    if (auto conn = req.peer->GetInConnection()) {
        conn->Close();
    }

    if (auto conn = req.peer->GetOutConnection()) {
        conn->Close();
    }

    peers_.erase(req.peer->GetId());
}

void TorChatEngine::OnStatus(const TorChatEngine::Request& req)
{
    static const std::map<string, Api::Status> status_map = {
        {"available", Api::Status::AVAILABLE},
        {"away", Api::Status::AWAY},
        {"xa", Api::Status::LONG_TIME_AWAY}
    };

    auto state_name = req.params.at(0);
    auto it = status_map.find(state_name);
    if (it == status_map.end()) {
        LOG_WARN_FN << "Unknown state: " << log::Esc(state_name)
            << " from " << *req.peer;
        WAR_THROW_T(ExceptionDisconnectNow, "Unknown state");
    }

    req.peer->info.status = it->second;
    req.peer->received_status_timeout = GetNewStatusTimeout();
    EmitEventBuddyStateUpdate(req.peer->info);
}

void TorChatEngine::OnVersion(const TorChatEngine::Request& req)
{
    req.peer->info.client_version = req.params.at(0);
}

const string& TorChatEngine::ToString(Api::Status status)
{
    static const std::array<string, 5> legacy_names = {
        "xa",
        "available",
        "away",
        "away",
        "xa"
    };

    return legacy_names.at(static_cast<int>(status));
}

bool TorChatEngine::EmitEventIncomingConnection(
    const EventMonitor::ConnectionInfo& info)
{
    for(auto& monitor : GetMonitors()) {
        if (!monitor->OnIncomingConnection(info)) {
            return false;
        }
    }

    return true;
}

bool TorChatEngine::EmitEventAddNewBuddy(const EventMonitor::BuddyInfo& info)
{
    for(auto& monitor : GetMonitors()) {
        if (!monitor->OnAddNewBuddy(info)) {
            return false;
        }
    }

    return true;
}

void TorChatEngine::EmitEventBuddyStateUpdate(const EventMonitor::BuddyInfo& info)
{
    LOG_DEBUG_FN << "Status update for " << info.buddy_id << " status is "
        << info.status;
    for(auto& monitor : GetMonitors()) {
        monitor->OnBuddyStateUpdate(info);
    }
}

void TorChatEngine::EmitEventIncomingMessage(const EventMonitor::Message& msg)
{
    for(auto& monitor : GetMonitors()) {
        monitor->OnIncomingMessage(msg);
    }
}

void TorChatEngine::EmitOtherEvent(const EventMonitor::Event& event)
{
    for(auto& monitor : GetMonitors()) {
        monitor->OnOtherEvent(event);
    }
}

void TorChatEngine::EmitShutdownComplete(const EventMonitor::ShutdownInfo& info)
{
    for(auto& monitor : GetMonitors()) {
        monitor->OnShutdownComplete(info);
    }
}

void TorChatEngine::EmitEventListening(const EventMonitor::ListeningInfo& endpoint)
{
    for(auto& monitor : GetMonitors()) {
        monitor->OnListening(endpoint);
    }
}



void TorChatEngine::StartMonitor()
{
    weak_ptr< TorChatEngine > weak_engine = shared_from_this();
    boost::asio::spawn(
        pipeline_.GetIoService(),
        bind(&TorChatEngine::MonitorPeers,
                weak_engine,
                std::placeholders::_1));
}

void TorChatEngine::MonitorPeers(weak_ptr< TorChatEngine > weak_engine,
                                 boost::asio::yield_context yield)
{

    while(true) {
        auto engine = weak_engine.lock();
        if (!engine
            && !engine->pipeline_.IsClosing()
            && !engine->pipeline_.IsClosed()) {
            return;
        }

        engine->CheckPeers(yield);

        boost::asio::deadline_timer timer(engine->pipeline_.GetIoService());
        timer.expires_from_now(
            boost::posix_time::milliseconds(5000 /* seconds */));

        engine.reset(); // We don't hold the reference while sleeping
        LOG_TRACE4_FN << "Going to sleep.";
        try {
            timer.async_wait(yield);
        } WAR_CATCH_ALL_E;
        LOG_TRACE4_FN << "Woke up.";
    }

    LOG_DEBUG_FN << "Peer monitoring is finished.";
}

void TorChatEngine::CheckPeers(boost::asio::yield_context& yield)
{
    for(auto& it : peers_) {
        auto& peer = it.second;
        try {
            CheckPeer(peer, yield);
        } WAR_CATCH_ALL_LOG(peer);
    }
}

void TorChatEngine::CheckPeer(const std::shared_ptr<TorChatPeer>& peer,
                              boost::asio::yield_context& yield)
{
    const auto now = std::chrono::steady_clock::now();

    LOG_TRACE4_FN << "Examining " << *peer;

    // Handle reconnects for unreachable peers
    if (!peer->HaveInConnection() && !peer->HaveOutConnection()) {
        if (!peer->retry_connect_time) {
            peer->retry_connect_time = GetNewReconnectTime(*peer);
        }
    }

    if (peer->retry_connect_time
        && (*peer->retry_connect_time <= now)) {
        if ((peer->GetState() == TorChatPeer::State::READY)
            && peer->HaveInConnection()
            && peer->HaveOutConnection()) {
            LOG_DEBUG_FN << "The peer " << *peer
                << " has an expired reconnect timer enabled. However the "
                << "connection seems OK, so I'm cancelling the timer.";
            peer->retry_connect_time.reset();
        } else {
            LOG_DEBUG_FN << "The peer " << *peer
                << " has an expired reconnect timer. "
                << " State=" << peer->GetState()
                << ", HaveInConnection=" << peer->HaveInConnection()
                << ", HaveOutConnection=" <<  peer->HaveOutConnection();
            Reconnect(peer);
            return;
        }
    }

    // Check inbound keep-alive for established connections
    if (peer->GetState() == TorChatPeer::State::READY) {
        assert(peer->received_status_timeout);
        if (*peer->received_status_timeout <= now) {
            assert(peer->reconnect_count == 0);
            LOG_DEBUG_FN << "The peer " << *peer
                << " has an expired status timer. Will reconnect now.";
            Reconnect(peer);
            return;
        }
    }

    // Send keep-alive (status)
    if (peer->next_keep_alive_time) {
        if (*peer->next_keep_alive_time <= now) {
            DoSendStatus(*peer, yield);
        }
    }

    // Check if we have to re-try an outgoing connection
    // TODO: Implement
}


void TorChatEngine::Reconnect(const shared_ptr< TorChatPeer >& peer)
{
    LOG_DEBUG_FN << "Reconecting peer " << *peer;
    peer->Close();
    peer->SetState(TorChatPeer::State::UNINTIALIZED);
    peer->initiative = Direction::OUTGOING;
    peer->info.status = Api::Status::OFF_LINE;
    peer->info.precense = Api::Presence::CONNECTING;
    SpawnConnect(peer->GetId());
    EmitEventBuddyStateUpdate(peer->info);
    ++peer->reconnect_count;
}

unique_ptr< chrono::steady_clock::time_point >
TorChatEngine::GetNewReconnectTime(TorChatPeer& peer)
{
    unsigned seconds = std::min<unsigned int>(
        max<unsigned int>(1, peer.reconnect_count) * 20, 60 * 15);

    LOG_TRACE1_FN << "Reconnect time for " << peer
        << ": " << seconds << " seconds.";

    return std::make_unique<std::chrono::steady_clock::time_point>(
        std::chrono::steady_clock::now() + std::chrono::seconds(seconds));
}


unique_ptr< chrono::steady_clock::time_point >
TorChatEngine::GetNewKeepAliveTime()
{
    std::uniform_int_distribution< int > distribution(5,110);
    const auto seconds = distribution(random_generator_);
    LOG_TRACE4_FN << "Next status update in " << seconds << " seconds.";

    return std::make_unique<std::chrono::steady_clock::time_point>(
        std::chrono::steady_clock::now()
        + std::chrono::seconds(seconds));
}


} // impl

ImProtocol::ptr_t ImProtocol::CreateProtocol(
    get_pipeline_fn_t fn, Config& properties) {
    auto engine = make_shared<impl::TorChatEngine>(fn, properties);
    engine->StartMonitor();
    return engine;
}

} // darkspeak
