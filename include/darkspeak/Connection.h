#pragma once

#include <memory>

#include <boost/asio.hpp>
#include <boost/asio/yield.hpp>

namespace darkspeak {

/*! Transport layer connection.
 */
class Connection
{
public:
    using ptr_t = std::shared_ptr<Connection>;
    using endpoint_t = boost::asio::ip::tcp::endpoint;

    /*! Basically a wrapper around a boost::asio TCP socket
     *
     * We use this in order to make it simple
     * to add tls and other features later on.
     */
    class Socket
    {
    public:
        using ptr_t = std::shared_ptr<Socket>;

        virtual ~Socket() = default;

        virtual boost::asio::ip::tcp::socket& GetSocket() = 0;

        virtual const boost::asio::ip::tcp::socket& GetSocket() const = 0;

        virtual std::size_t AsyncReadSome(boost::asio::mutable_buffers_1 buffers,
                                            boost::asio::yield_context& yield) = 0;

        virtual std::size_t AsyncRead(boost::asio::mutable_buffers_1 buffers,
                                        boost::asio::yield_context& yield) = 0;

        virtual void AsyncWrite(const boost::asio::const_buffers_1& buffers,
            boost::asio::yield_context& yield) = 0;

        virtual void AsyncWrite(const write_buffers_t& buffers,
            boost::asio::yield_context& yield) = 0;

        virtual void AsyncConnect(const boost::asio::ip::tcp::endpoint& ep,
            boost::asio::yield_context& yield) = 0;

        virtual void AsyncShutdown(boost::asio::yield_context& yield) = 0;
    };

    /*! Create an outbound connection
     *
     * \param address The address we want to connect to,
     *          for example an onion address.
     *
     * \param proxy Socks5 proxy to use for this connection.
     */
    void Connect(std::string address, endpoint_t proxy);

    bool IsConnected() const noexcept;

    /*! Get the connection instance.
     *
     * \exception Throws if there is no instance
     */
    Socket& GetConnection();
};

} //namespace
