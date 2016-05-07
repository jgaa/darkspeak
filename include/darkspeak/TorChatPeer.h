#pragma once

#include <memory>
#include <vector>
#include <iostream>
#include <mutex>

#include "war_error_handling.h"
#include "log/WarLog.h"

#include <boost/asio.hpp>
#include <boost/asio/spawn.hpp>

#include "darkspeak/Api.h"
#include "darkspeak/TorChatConnection.h"


namespace darkspeak {
namespace impl {

/*! A peer at the protocol level.
 *
 */
class TorChatPeer {
public:
    enum class State {
        UNINTIALIZED,
        ACCEPTING,
        CONNECTING,
        AUTENTICATING,
        AUTHENTICATED,
        READY
    };
    using ptr_t = std::shared_ptr<TorChatPeer>;

    TorChatPeer(const std::string id)
    :id_{id}
    {
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

    State GetState() {

        return state_;
    }

    void SetState(State state) {

        LOG_DEBUG_FN << "Setting state " << static_cast<int>(state)
            << ", old stat was " << static_cast<int>(state_);

        state_ = state;
    }

    const std::string& GetId() {

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

    std::string software_name;
    std::string software_version;
    std::string profile_name;
    std::string profile_text;
    std::string status_text;

private:
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
    std::string my_cookie = "veryveryverysecret1234567890"; // FIXME
    State state_ = State::UNINTIALIZED;
    bool sent_ping_ = false;
    bool sent_pong_ = false;
    bool got_ping_ = false;
    bool got_pong_ = false;
};

} // impl
} // darkspeak
