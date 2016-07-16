#pragma once

#include <iostream>
#include <thread>
#include <future>

#include <boost/utility/string_ref.hpp>

#include "darkspeak/Connection.h"

namespace darkspeak {
namespace impl {

class SocketImpl : public Connection::Socket {
public:

    SocketImpl(boost::asio::io_service& io_service)
    : socket_{std::make_shared<boost::asio::ip::tcp::socket>(io_service)}
    {
    }

    SocketImpl(std::shared_ptr<boost::asio::ip::tcp::socket> socket)
    : socket_{std::move(socket)}
    {
    }

    boost::asio::ip::tcp::socket& GetSocket() override {
        return *socket_;
    }

    const boost::asio::ip::tcp::socket& GetSocket() const override {
        return *socket_;
    }

    std::size_t AsyncReadSome(boost::asio::mutable_buffers_1 buffers,
                              boost::asio::yield_context& yield) override {
        return socket_->async_read_some(buffers, yield);
    }

    std::size_t AsyncRead(boost::asio::mutable_buffers_1 buffers,
                          boost::asio::yield_context& yield) override {
        return boost::asio::async_read(*socket_, buffers, yield);
    }

    void AsyncWrite(const boost::asio::const_buffers_1& buffers,
                    boost::asio::yield_context& yield) override {
        boost::asio::async_write(*socket_, buffers, yield);
    }

    void AsyncWrite(const Connection::write_buffers_t& buffers,
                    boost::asio::yield_context& yield)override {
         boost::asio::async_write(*socket_, buffers, yield);
    }

    void AsyncWrite(const boost::asio::const_buffers_1& buffers,
                    asio_handler_t handler) override {
        boost::asio::async_write(*socket_, buffers, handler);
    }

    void AsyncConnect(const boost::asio::ip::tcp::endpoint& ep,
                      boost::asio::yield_context& yield) override {
        socket_->async_connect(ep, yield);
    }

    void AsyncShutdown(boost::asio::yield_context& yield) override {
        // Do nothing.
    }

    void Close() override {
        if (socket_ && socket_->is_open()) {
            socket_->close();
        }
    }

private:
    std::shared_ptr<boost::asio::ip::tcp::socket> socket_;
};

} // impl
} // restc_cpp

