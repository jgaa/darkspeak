
#include <assert.h>

#include "log/WarLog.h"

#include "darkspeak/Connection.h"
#include "darkspeak/SocketImpl.h"

#define LOCK std::lock_guard<std::mutex> lock(mutex_);

using namespace std;
using namespace war;

namespace darkspeak {

void Connection::Connect(std::string address, endpoint_t proxy,
                 boost::asio::yield_context& yield) {

	LOG_DEBUG_FN << "Connecting to " << log::Esc(address)
		<< " trough Socks 5 proxy at " << proxy;

    array<char, 512> rcv_buffer;
    WAR_ASSERT(!IsConnected());
    // Connect
    socket_ = make_shared<impl::SocketImpl>(pipeline_.GetIoService());
    socket_->AsyncConnect(proxy, yield);

    // Start Socks 5 handshake.
    std::array<char, 3> auth_hdr;
    auth_hdr[0] = 5; // Socks version
    auth_hdr[1] = 1; // Number of auth entries
    auth_hdr[2] = 0; // NO AUTHENTICATION REQUIRED
    socket_->AsyncWrite(boost::asio::const_buffers_1(
        &auth_hdr[0], auth_hdr.size()), yield);

    auto bytes = socket_->AsyncReadSome(
        boost::asio::buffer(rcv_buffer), yield);

    if (bytes != 2) {
        LOG_ERROR << "Failed to establish a Socks 5 connection to "
            << log::Esc(address)
            << ". The socks5 auth-reply was only " << bytes << " bytes.";
        WAR_THROW_T(war::ExceptionIoError, "Socks5 auth-reply too small");
    }

    if (rcv_buffer[0] != 5) {
        WAR_THROW_T(war::ExceptionIoError, "Socks server do not support Socks 5");
    }

    if (rcv_buffer[1] != 0) {
        LOG_ERROR << "Failed to establish a no-auth Socks 5 connection to "
            << log::Esc(address);
        WAR_THROW_T(war::ExceptionIoError, "Socks5 authentication failed");
    }

    std::vector<char> req_hdr;
    req_hdr.push_back(5); // version
    req_hdr.push_back(1); // connect
    req_hdr.push_back(0); // reserved
    req_hdr.push_back(3); // domanname
    req_hdr.push_back(static_cast<char>(address.size())); // string length
    for(char ch : address) {
        req_hdr.push_back(ch); // Domain name
    }
	req_hdr.push_back(0); // port
    req_hdr.push_back(0); // port
	std::uint16_t *port = reinterpret_cast<std::uint16_t *>(&req_hdr[req_hdr.size() - 2]);
    *port = htons(11009);
    socket_->AsyncWrite(boost::asio::const_buffers_1(
        &req_hdr[0], req_hdr.size()), yield);

    bytes = socket_->AsyncReadSome(
        boost::asio::buffer(rcv_buffer), yield);

    if (bytes < 6) {
        LOG_ERROR << "Failed to establish a Socks 5 connection to "
            << log::Esc(address)
            << ". The socks5 header was only " << bytes << " bytes.";
        WAR_THROW_T(war::ExceptionIoError, "Socks5 header too small");
    }

    if (rcv_buffer[0] != 5) {
        WAR_THROW_T(war::ExceptionIoError, "Socks server do not support Socks 5");
    }

    if (rcv_buffer[1] != 0) {
        LOG_ERROR << "Failed to establish a Socks 5 connection to "
            << log::Esc(address);
        WAR_THROW_T(war::ExceptionIoError, "Socks5 connect failed");
    }

    LOG_DEBUG_FN << "Established a Socks5 connection to " << address
        << " on socket " << GetSocket().GetSocket();
}

bool Connection::IsConnected() const noexcept {
    auto sck = socket_;
    if (sck) {
        return sck->GetSocket().is_open();
    }

    return false;
}

void Connection::Close()
{
    GetSocket().Close();
}


} // darkspeak
