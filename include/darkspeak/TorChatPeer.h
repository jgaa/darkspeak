#pragma once

#include <memory>
#include <vector>
#include <iostream>
#include <mutex>
#include <chrono>

#include "war_error_handling.h"
#include "log/WarLog.h"

#include <boost/asio.hpp>
#include <boost/asio/spawn.hpp>

#include "darkspeak/darkspeak_impl.h"
#include "darkspeak/Api.h"
#include "darkspeak/TorChatConnection.h"
#include "darkspeak/EventMonitor.h"

namespace darkspeak {
namespace impl {

/*! A peer at the protocol level.
 *
 */
class TorChatPeer {
public:

    class FileTransfer
    {
    public:
        EventMonitor::FileInfo info;
        unsigned int block_size = 1024 * 8;
        std::string cookie;
        boost::filesystem::path path;
        std::fstream file;
        int last_good_block = -1; // None
        std::deque<std::unique_ptr<char *>> buffered;
    };

    enum class State {
        UNINTIALIZED,
        ACCEPTING,
        CONNECTING,
        AUTENTICATING,
        AUTHENTICATED,
        READY,
        DONE
    };
    using ptr_t = std::shared_ptr<TorChatPeer>;

    TorChatPeer(const std::string id)
    :id_{id}
    {
        info.buddy_id = id_;
        InitCookie();
    }

    TorChatConnection::ptr_t GetInConnection() {

        return conn_in_;
    }

    TorChatConnection::ptr_t GetOutConnection() {

        return conn_out_;
    }

    /*! Sets the inbound connection.
     *
     * \param conn If nullptr, the existing connection (if any) will be
     *          closed and forgotten. If instance, conn will be set as
     *          the inbound connectin only if there is no inbound connection.
     *
     * \returns true if the connection was set or removed.
     */
    bool SetInConnection(TorChatConnection::ptr_t conn)
    {
        return SetConnection(conn_in_, conn);
    }

    /*! Sets the outbound connection.
     *
     * \param conn If nullptr, the existing connection (if any) will be
     *          closed and forgotten. If instance, conn will be set as
     *          the outbound connectin only if there is no outbound connection.
     *
     * \returns true if the connection was set or removed.
     */
    bool SetOutConnection(TorChatConnection::ptr_t conn)
    {
        return SetConnection(conn_out_, conn);
    }

    State GetState() const;

    void SetState(State state);

    bool UpgradeState(State state) {

        if (compare_enum(state_,state) < 0) {
            SetState(state);
            return true;
        }
        return false;
    }

    const std::string& GetId() const {

        return id_;
    }

    std::string GetMyCookie() const {

        return my_cookie;
    }

    void SetMyCookie(std::string cookie) {

        my_cookie = cookie;
    }

    std::string GetPeerCookie() const {

        return peers_cookie;
    }

    void SetPeerCookie(std::string cookie) {

        peers_cookie = cookie;
    }

    bool HaveInConnection() const {

        return conn_in_ && conn_in_->IsConnected();
    }

    bool HaveOutConnection() const {

        return conn_out_ && conn_out_->IsConnected();
    }

    bool GetSentPing() const {

        return sent_ping_;
    }

    bool GetSentPong() const {

        return sent_pong_;
    }

    bool GetReceivedPing() const {

        return got_ping_;
    }

    bool GetReceivedPong() const {

        return got_pong_;
    }

    void SetSentPing(bool value = true) {

        sent_ping_ = value;
    }

    void SetSentPong(bool value = true) {

        sent_pong_ = true;
    }

    void SetReceivedPing(bool value = true) {

        got_ping_ = true;
    }

    void SetReceivedPong(bool value = true) {

        got_pong_ = true;
    }

    /*! Returns true if the peer has been in ready state.
     * This means that the upper layer has approved the
     * ID, and we don't have to wait for an add_me message
     * to enter READY state.
     */
    bool HasBeenReady() const {
        return has_been_ready_;
    }

    void Close();

    EventMonitor::BuddyInfo info;
    Direction initiative = Direction::INCOMING;
    std::unique_ptr<std::chrono::steady_clock::time_point> next_keep_alive_time;
    std::unique_ptr<std::chrono::steady_clock::time_point> received_status_timeout;
    std::unique_ptr<std::chrono::steady_clock::time_point> retry_connect_time;
    unsigned reconnect_count = 0;

private:
    void InitCookie();

    bool SetConnection(TorChatConnection::ptr_t& existing,
        TorChatConnection::ptr_t conn) {


        if (existing && !existing->IsConnected()) {
            // Dead connection.
            existing.reset();
        }

        if (existing) {
            if (conn) {
                // Conflict. Can not replace an open connection.
                return false;
            } else {
                // Remove the existing connection
                existing->GetSocket().GetSocket().close();
                existing.reset();
                return true;
            }
        }

        WAR_ASSERT(!existing);
        existing = move(conn);
        return true;
    }

    const std::string id_;
    TorChatConnection::ptr_t conn_in_; // Incoming connection
    TorChatConnection::ptr_t conn_out_; // Outgoing connection
    std::string peers_cookie;
    std::string my_cookie;
    State state_ = State::UNINTIALIZED;
    bool sent_ping_ = false;
    bool sent_pong_ = false;
    bool got_ping_ = false;
    bool got_pong_ = false;
    bool has_been_ready_ = false;
};

} // impl
} // darkspeak

std::ostream& operator << (std::ostream& o, const darkspeak::impl::TorChatPeer& v);
std::ostream& operator << (std::ostream& o,
                           const darkspeak::impl::TorChatPeer::State& v);
