
#include <assert.h>

#include <boost/algorithm/string.hpp>

#include "log/WarLog.h"

#include "darkspeak/TorChatConnection.h"
#include "darkspeak/SocketImpl.h"

#define LOCK std::lock_guard<std::mutex> lock(mutex_);

using namespace std;
using namespace war;

namespace darkspeak {
namespace impl {

TorChatConnection::TorChatConnection(war::Pipeline& pipeline)
: Connection(pipeline)
{
    SetSocket(make_shared<SocketImpl>(pipeline.GetIoService()));
}

TorChatConnection::TorChatConnection(
    war::Pipeline& pipeline,
    std::shared_ptr< boost::asio::ip::tcp::socket > socket)
: Connection(pipeline)
{
    SetSocket(make_shared<SocketImpl>(socket));
}

boost::string_ref TorChatConnection::GetLine(boost::asio::yield_context& yield)
{
    char *start = &read_buffer_[0];
    size_t bytes_wanted = read_buffer_.size();
    size_t total_bytes_read = 0;
    size_t fragments = 0;
    boost::string_ref line; // return value

    // Rearrange the buffer if required
    if (!remaining_data_.empty()) {
        LOG_TRACE4_FN << "Rearranging input buffer - have "
            << remaining_data_.size()
            << " bytes left over since last time: "
            << remaining_data_.to_string();


        memmove(start, remaining_data_.data(), remaining_data_.size());
        bytes_wanted -= remaining_data_.size();
        start += remaining_data_.size();
        total_bytes_read = remaining_data_.size();
        remaining_data_.clear();

        if (GetLineFromBuffer(
            {&read_buffer_[0], total_bytes_read},
            line, remaining_data_)) {

            LOG_TRACE4_FN << "Found complete line in remains: " << line.to_string();
            return line;
        }
    }

    while(true) {
        const auto bytes_received = GetSocket().AsyncReadSome(
            boost::asio::buffer(start, bytes_wanted), yield);

        WAR_ASSERT(bytes_received);
        total_bytes_read += bytes_received;
        boost::string_ref buffer{&read_buffer_[0], total_bytes_read};

        if (GetLineFromBuffer(
            {&read_buffer_[0], total_bytes_read},
            line, remaining_data_)) {

            LOG_TRACE4_FN << "Found complete line in remains: " << line.to_string();
            return line;
        }

        bytes_wanted -= bytes_received;
        start += bytes_received;

        if (!bytes_wanted) {
            WAR_THROW_T(ExceptionBufferFull, "Receive buffer is full")
        }

        if (++fragments > max_fragments_in_one_line_) {
            WAR_THROW_T(ExceptionTooFragmented,
                        "Too many fragments in IO Receive stream");
        }
    }
}

bool TorChatConnection::GetLineFromBuffer(
    boost::string_ref buffer,
    boost::string_ref& line,
    boost::string_ref& remaining) {

    auto pos = buffer.find(0x0a);
    if (pos != buffer.npos) {
        // We have a line
        line = {&buffer[0], pos};
        ++pos; // eom marker
        size_t remaining_bytes = buffer.size() - pos;

        remaining = {&buffer[0] + pos, remaining_bytes};

        LOG_TRACE4_FN << "Received line: " << log::Esc(line.to_string())
            << ". Have " << remaining.size()
            << " bytes left in the buffer: "
            << log::Esc(remaining.to_string());
        return true;
    }

    return false;
}

void TorChatConnection::SendLine(string line,
                                 boost::asio::yield_context& yield)
{
    Encode(line);
    line += static_cast<char>(0x0a);

    GetSocket().AsyncWrite(boost::asio::const_buffers_1(line.c_str(),
                                                        line.size()),
                                                        yield);

    LOG_DEBUG << "Sent line: " << log::Esc(line);
    LOG_TRACE4_FN << "Sent line: " << log::Esc(line);
}

void TorChatConnection::Encode(std::string& blob)
{
    boost::replace_all(blob, "\\", "\\/");
    boost::replace_all(blob, "\n", "\\n");
}

void TorChatConnection::Decode(std::string& blob)
{
    boost::replace_all(blob, "\\n", "\n");
    boost::replace_all(blob, "\\/", "\\");
}


} // impl
} // darkspeak
