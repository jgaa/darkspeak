
#include <assert.h>

#include <boost/algorithm/string.hpp>

#include "log/WarLog.h"

#include "darkspeak/TorChatConnection.h"
#include "darkspeak/SocketImpl.h"

#define LOCK std::lock_guard<std::mutex> lock(mutex_);

using namespace std;
using namespace war;

std::ostream& operator << (std::ostream& o, const darkspeak::impl::TorChatConnection& v) {
    return o << "{connection: " << v.GetName() << '}';
}

namespace darkspeak {
namespace impl {

TorChatConnection::TorChatConnection(std::string name, war::Pipeline& pipeline)
: Connection(pipeline), name_{move(name)}
{
    SetSocket(make_shared<SocketImpl>(pipeline.GetIoService()));
    LOG_DEBUG << *this << " now communicates trough " << GetSocket().GetSocket();
}

TorChatConnection::TorChatConnection(
    std::string name,
    war::Pipeline& pipeline,
    std::shared_ptr< boost::asio::ip::tcp::socket > socket)
: Connection(pipeline), name_{move(name)}
{
    SetSocket(make_shared<SocketImpl>(socket));
    LOG_DEBUG << *this << " now communicates trough " << GetSocket().GetSocket();
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
        LOG_TRACE4_FN << *this << " Rearranging input buffer - have "
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

            LOG_TRACE4_FN << *this << " Found complete line in remains: " << line.to_string();
            return DecodeCurrent(line);
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

            LOG_TRACE4_FN << *this << " Got complete line: " << line.to_string();
            return DecodeCurrent(line);
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
    boost::string_ref& remaining) const {

    auto pos = buffer.find(0x0a);
    if (pos != buffer.npos) {
        // We have a line
        line = {&buffer[0], pos};
        ++pos; // eom marker
        size_t remaining_bytes = buffer.size() - pos;

        remaining = {&buffer[0] + pos, remaining_bytes};

        LOG_TRACE4_FN << *this
            << " Received line: " << log::Esc(line.to_string())
            << ". Have " << remaining.size()
            << " bytes left in the buffer: "
            << log::Esc(remaining.to_string());
        return true;
    }

    return false;
}

size_t TorChatConnection::SendLine(string line,
                                 boost::asio::yield_context& yield)
{
    Encode(line);
    line += static_cast<char>(0x0a);

    GetSocket().AsyncWrite(boost::asio::const_buffers_1(line.c_str(),
                                                        line.size()),
                                                        yield);

    LOG_DEBUG << *this  << " Sent line: " << log::Esc(line);
    return line.size();
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

std::string TorChatConnection::Decode(boost::string_ref& blob)
{
    std::string s;
    s.reserve(blob.size());

    for(auto c = blob.cbegin(); c != blob.cend(); ++c) {
        if (*c == '\\') {
            auto cc = c;
            ++cc;
            if (cc != blob.end()) {
                if (*cc == 'n') {
                    s.push_back('\n');
                } else if (*cc == '/') {
                    s.push_back('\\');
                } else {
                    s.push_back(*c);
                    s.push_back(*cc);
                }
                ++c;
            } else {
                s.push_back(*c);
            }
        } else {
            s.push_back(*c);
        }
    }

    return s;
}

boost::string_ref TorChatConnection::DecodeCurrent(boost::string_ref& blob)
{
    if (blob.find('\\') != blob.npos) {
        current_line_ = Decode(blob);
        return {current_line_.c_str(), current_line_.size()};
    }
    return blob;
}


} // impl
} // darkspeak
