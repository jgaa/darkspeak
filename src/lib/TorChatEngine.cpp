#include <assert.h>
#include <memory>
#include <sstream>

#include "darkspeak/darkspeak_impl.h"
#include "darkspeak/TorChatEngine.h"
#include "darkspeak/TorChatConnection.h"
#include "darkspeak/TorChatPeer.h"
#include "darkspeak/IoTimer.h"

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


namespace darkspeak {

template <typename T>
int compare_enum(T left, T right) {
    return static_cast<int>(left) - static_cast<int>(right);
}

namespace impl {

TorChatEngine::TorChatEngine(ImProtocol::get_pipeline_fn_t fn,
    const boost::property_tree::ptree& config)
: pipeline_{fn()}, config_{config}
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
            [this] (const Request& req) { OnPing(req); },
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
    boost::asio::spawn(
        pipeline_.GetIoService(),
        bind(&TorChatEngine::StartConnectToPeer, this,
                buddy->GetInfo().id,
                ref(pipeline_),
                std::placeholders::_1));
}


void TorChatEngine::Disconnect(Api::Buddy& buddy)
{
    assert(false && "Not implemented");
}


void TorChatEngine::SendMessage(Api::Buddy& buddy, const ImProtocol::Message& msg)
{
    assert(false && "Not implemented");
}

void TorChatEngine::SetMonitor(ImProtocol::EventMonitor::ptr_t monitor)
{
    event_monitors_.push_back(monitor);
}

vector< ImProtocol::EventMonitor::ptr_t > TorChatEngine::GetMonitors()
{
    vector<ImProtocol::EventMonitor::ptr_t> list;

    auto prev = event_monitors_.end();
    for(auto it = event_monitors_.begin(); it != event_monitors_.end();) {
        auto ptr = it->lock();
        if (!ptr) {
            // dead monitor
            event_monitors_.erase(it);
            it = prev;
            if (it == event_monitors_.end()) {
                it = event_monitors_.begin();
            } else {
                ++it;
            }
        } else {
            list.push_back(move(ptr));
            prev = it;
            ++it;
        }
    }

    return list;
}


void TorChatEngine::Listen(boost::asio::ip::tcp::endpoint endpoint)
{
    std::promise<void> promise;
    auto future = promise.get_future();

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
        acceptor_ = make_unique<boost::asio::ip::tcp::acceptor>(pipeline_.GetIoService());
        acceptor_->open(endpoint.protocol());
        acceptor_->set_option(boost::asio::ip::tcp::acceptor::reuse_address(true));
        acceptor_->bind(endpoint);
        acceptor_->listen();

        boost::asio::spawn(
            pipeline_.GetIoService(),
            bind(&TorChatEngine::Accept,
                this, endpoint, std::placeholders::_1));

        promise.set_value();
    } WAR_CATCH_ALL_EF(promise.set_exception(std::current_exception()));

}

void TorChatEngine::Accept(boost::asio::ip::tcp::endpoint endpoint,
                           boost::asio::yield_context yield)
{
    WAR_ASSERT(acceptor_->is_open());
    LOG_NOTICE << "Starting listening on " << endpoint;

    while(acceptor_->is_open()) {
        boost::system::error_code ec;

        auto socket = make_shared<boost::asio::ip::tcp::socket>(
            pipeline_.GetIoService());
        LOG_TRACE1_FN << "Created socket " << *socket
            << " on " << pipeline_
            << " from endpoint " << endpoint;

        acceptor_->async_accept(*socket, yield[ec]);

        if (!ec) {
            LOG_TRACE1_FN << "Incoming connection: " << *socket;
            try {
                boost::asio::spawn(
                    pipeline_.GetIoService(),
                    bind(&TorChatEngine::OnAccepted, this,
                         ref(pipeline_),
                         socket,
                         std::placeholders::_1));

            } WAR_CATCH_ERROR;
        } else /* ec */ {
            LOG_WARN_FN << "Accept error: " << ec;
        }
    } // while acceptor is open

    LOG_DEBUG_FN << "Acceptor at " << endpoint << " is done.";
}

void TorChatEngine::OnAccepted(
    war::Pipeline& pipeline,
    shared_ptr< boost::asio::ip::tcp::socket > socket,
    boost::asio::yield_context yield)
{
    auto in_conn = make_shared<TorChatConnection>(pipeline, socket);

    try  {
        /* Before we do anything with this connection, we must
         * get the greeting ping
         */
        auto timer = IoTimer::Create(connect_timeout_, in_conn);
        auto ping_line = in_conn->GetLine(yield);

        string id, cookie;
        if (!VerifyPing(ping_line, id, cookie)) {
            return;
        }

        TorChatPeer::ptr_t peer;
        {
            auto it = peers_.find(id);
            if (it != peers_.end()) {
                peer = it->second;
                if (cookie.compare(peer->GetPeerCookie()) != 0) {
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
                peer->SetInConnection(nullptr); // reset
                peer->SetInConnection(in_conn);
                peer->SetReceivedPong(false);
                peer->SetState(TorChatPeer::State::ACCEPTING);
            } else {
                peer = make_shared<TorChatPeer>(id);
                peer->SetPeerCookie(cookie);
                peer->SetInConnection(in_conn);
                peers_[id] = peer;
            }
            WAR_ASSERT(peer);

            if (compare_enum(peer->GetState(), TorChatPeer::State::ACCEPTING) < 0) {
                peer->SetState(TorChatPeer::State::ACCEPTING);
            }
        }

        peer->SetReceivedPing();
        std::weak_ptr<TorChatPeer> weak_peer = peer;

        if (!peer->HaveOutConnection()) {
            ConnectToPeer(*peer, pipeline, yield);

            // Enter event loop in new context
            boost::asio::spawn(
                pipeline.GetIoService(),
                bind(
                    &TorChatEngine::StartProcessingRequests, this,
                    weak_peer, Direction::OUTGOING,
                    placeholders::_1));
        }

        // Enter event loop in this context
        peer.reset();
        ProcessRequests(weak_peer, Direction::INCOMING, yield);

    } WAR_CATCH_ALL_EF(return);
}

void TorChatEngine::StartProcessingRequests(weak_ptr< TorChatPeer > weak_peer,
                                            TorChatEngine::Direction direction,
                                            boost::asio::yield_context yield)
{
    try {
        ProcessRequests(weak_peer, direction, yield);
    } WAR_CATCH_ALL_E;
}

void TorChatEngine::ProcessRequests(weak_ptr< TorChatPeer > weak_peer,
                                    TorChatEngine::Direction direction,
                                    boost::asio::yield_context& yield)
{
    static const string not_implemented{"not_implemented "};

    while(true) {
        auto peer = weak_peer.lock();
        if (!peer) {
            LOG_TRACE1_FN << "The peer is gone. Exiting request loop.";
            return;
        }

        TorChatConnection::ptr_t conn;

        if (direction == Direction::INCOMING) {
            conn = peer->GetInConnection();
        } else {
            conn = peer->GetOutConnection();
        }

        if (!conn || !conn->IsConnected()) {
            LOG_TRACE1_FN << "The connection is gone. Exiting request loop.";
            return;
        }

        peer.reset(); // Release reference while we wait for IO
        auto req = conn->GetLine(yield);

        peer = weak_peer.lock();
        if (!peer) {
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
                    << static_cast<int>(request.cmd->valid)
                    << " and the command has validity "
                    <<  static_cast<int>(peer->GetState());

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
                                       Pipeline& pipeline,
                                       boost::asio::yield_context yield)
{
    auto peer = GetPeer(peer_id);
    if (!peer) {
        LOG_DEBUG << "Failed to find peer " << log::Esc(peer_id)
            << ". Creating instance.";

        peer = make_shared<TorChatPeer>(peer_id);
        peers_[peer_id] = peer;
    }

    try {
        if (compare_enum(peer->GetState(), TorChatPeer::State::CONNECTING) < 0) {
            peer->SetState(TorChatPeer::State::CONNECTING);
        }
        ConnectToPeer(*peer, pipeline, yield);
        std::weak_ptr<TorChatPeer> weak_peer = peer;
        peer.reset();

        ProcessRequests(weak_peer, Direction::OUTGOING, yield);
    } WAR_CATCH_ALL_E;
}

void TorChatEngine::ConnectToPeer(TorChatPeer& peer, Pipeline& pipeline,
                                  boost::asio::yield_context& yield)
{
    auto outbound = make_shared<TorChatConnection>(pipeline);
    if (!peer.SetOutConnection(outbound)) {
        LOG_WARN_FN << "Peer already have an outbound connection.";
        return;
    }

    if (compare_enum(peer.GetState(), TorChatPeer::State::CONNECTING) < 0) {
        peer.SetState(TorChatPeer::State::CONNECTING);
    }
    auto timer = IoTimer::Create(connect_timeout_, outbound);
    auto onion_host = peer.GetId() + ".onion";
    outbound->Connect(onion_host, GetTorEndpoint(), yield);
    DoSendPing(peer, yield);
    if (peer.GetReceivedPing()) {
        DoSendPong(peer, yield);
    }

    if (peer.GetOutConnection()->IsConnected()) {
        if (compare_enum(peer.GetState(), TorChatPeer::State::AUTENTICATING) < 0) {
            peer.SetState(TorChatPeer::State::AUTENTICATING);
        }
    }
}


boost::asio::ip::tcp::endpoint TorChatEngine::GetTorEndpoint() const
{
    const auto hostname = config_.get("tor.hostname", "127.0.0.1");
    const auto port = config_.get<unsigned short>("tor.hostname", 9050);

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

    LOG_DEBUG << "Got ping "
        << " from " << log::Esc(id)
        << " with cookie " << log::Esc(cookie);

    return true;
}

void TorChatEngine::SendFile(Api::Buddy& buddy, const ImProtocol::File& file, ImProtocol::FileMonitor::ptr_t monitor)
{
    assert(false && "Not implemented");
}


void TorChatEngine::Shutdown()
{
    assert(false && "Not implemented");
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

    LOG_DEBUG << "Parsed command " << log::Esc(rval.command_name)
        << " with " << rval.params.size() << " arguments.";

    return rval;
}

void TorChatEngine::OnAddMe(const TorChatEngine::Request& req)
{
    // TODO: Add event
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

void TorChatEngine::OnClient(const TorChatEngine::Request& req)
{
    req.peer->software_name = req.params.at(0);
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

    if (!req.peer->GetSentPing()) {
        WAR_THROW_T(ExceptionDisconnectNow, "Received pong before I sent ping!");
    }

    if (req.peer->GetMyCookie().compare(req.params.at(0)) != 0) {
        WAR_THROW_T(ExceptionDisconnectNow, "Received pong with wrong cookie!");
    }

    if (compare_enum(req.peer->GetState(), TorChatPeer::State::AUTHENTICATED) < 0) {
            req.peer->SetState(TorChatPeer::State::AUTHENTICATED);
    }

    // TODO: Set a timer for how long we will keep the connection berfore we
    //          get to READY state (status or add_me?)

    LOG_DEBUG_FN << "Sending initial info...";

    // It's the first pong message. Let's identify ourself.
    WAR_ASSERT(req.peer && req.yield);
    DoSendPing(*req.peer, *req.yield);
    DoSend("client", {GetClientName()}, *req.peer, *req.yield);
    DoSend("version", {GetClientVersion()}, *req.peer, *req.yield);
    if (!local_info_.profile_name.empty()) {
        DoSend("profile_name", {local_info_.profile_name}, *req.peer, *req.yield);
    }
    if (!local_info_.profile_text.empty()) {
        DoSend("profile_text", {local_info_.profile_text}, *req.peer, *req.yield);
    }
    // TODO: Add avatar
    DoSend("add_me", {""}, *req.peer, *req.yield);
    DoSendStatus(*req.peer, *req.yield);

    req.peer->SetReceivedPong();

    // TODO: Add event
}

bool TorChatEngine::DoSend(const string& command,
                           initializer_list< string > args,
                           TorChatPeer& peer,
                           boost::asio::yield_context& yield)
{
    auto outbound = peer.GetOutConnection();
    if (!outbound) {
        return false;
    }

    std::ostringstream buffer;
    buffer << command;
    for(const auto& arg : args) {
        buffer << ' ' << arg;
    }

    outbound->SendLine(buffer.str(), yield);
    return true;
}


bool TorChatEngine::DoSendStatus(TorChatPeer& peer, boost::asio::yield_context& yield) {
    static const string status{"status"};

    // TODO: Do not send status until we are in TorChatPeer::State::READY state
    return DoSend(status, {ToString(local_info_.status)}, peer, yield);
}

bool TorChatEngine::DoSendPing(TorChatPeer& peer, boost::asio::yield_context& yield) {
    static const string ping{"ping"};

    auto rval = DoSend(ping,
                       {
                           config_.get<string>("service.dark_id"),
                           peer.GetMyCookie()
                       },
                       peer, yield);

    if (rval) {
        peer.SetSentPing();
    }

    return rval;
}

bool TorChatEngine::DoSendPong(TorChatPeer& peer, boost::asio::yield_context& yield) {
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
    req.peer->profile_name = req.params.at(0);
}

void TorChatEngine::OnProfileText(const TorChatEngine::Request& req)
{
    req.peer->profile_text = req.params.at(0);
}

void TorChatEngine::OnRemoveMe(const TorChatEngine::Request& req)
{

}

void TorChatEngine::OnStatus(const TorChatEngine::Request& req)
{
    req.peer->status_text = req.params.at(0);
}

void TorChatEngine::OnVersion(const TorChatEngine::Request& req)
{
    req.peer->software_version = req.params.at(0);
}

void TorChatEngine::SetInfo(const Api::Info& info)
{
    pipeline_.Dispatch({[info, this](){
        local_info_ = info;
    }, "Set Info"});
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


} // impl

ImProtocol::ptr_t ImProtocol::CreateProtocol(
    get_pipeline_fn_t fn, const boost::property_tree::ptree& properties) {
    return make_shared<impl::TorChatEngine>(fn, properties);
}

} // darkspeak
