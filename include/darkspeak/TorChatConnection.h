#pragma once

#include <memory>
#include <vector>
#include <iostream>

#include "war_error_handling.h"

#include <boost/asio.hpp>
#include <boost/asio/spawn.hpp>
#include <boost/utility/string_ref.hpp>

#include "darkspeak/Api.h"
#include "darkspeak/Connection.h"


namespace darkspeak {
namespace impl {

/*! Connection with some Tor Chat protocol traits
 *
 */
class TorChatConnection : public Connection
{
public:
    using ptr_t = std::shared_ptr<TorChatConnection>;

    struct ExceptionBufferFull : public war::ExceptionBase {};
    struct ExceptionTooFragmented : public war::ExceptionBase {};

    TorChatConnection(std::string name,
                      war::Pipeline& pipeline);
    TorChatConnection(std::string name,
                      war::Pipeline& pipeline,
                      std::shared_ptr<boost::asio::ip::tcp::socket> socket);

    /*! Asynchronously get one line from the peer */
    boost::string_ref GetLine(boost::asio::yield_context& yield);

    /*! Send the line.
     *
     * Appends newline and encodes the data
     *
     * \return bytes sent
     */
    std::size_t SendLine(std::string line, boost::asio::yield_context& yield);

    void Encode(std::string& blob);
    void Decode(std::string& blob);

    const std::string& GetName() const noexcept {
        return name_;
    }

private:
    bool GetLineFromBuffer(boost::string_ref buffer,
                                  boost::string_ref& line,
                                  boost::string_ref& remaining) const;

    std::array<char, 4096> read_buffer_;
    boost::string_ref remaining_data_;
    static const std::size_t max_fragments_in_one_line_ = 16;
    const std::string name_;
};

} // impl
} // darkspeak

std::ostream& operator << (std::ostream& o, const darkspeak::impl::TorChatConnection& v);
