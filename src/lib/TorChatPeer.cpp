
#include <random>

#include <boost/algorithm/string.hpp>

#include "war_uuid.h"
#include "log/WarLog.h"
#include "war_helper.h"
#include "war_filecheck.h"

#include "darkspeak/darkspeak.h"
#include "darkspeak/TorChatPeer.h"
#include "darkspeak/TorChatEngine.h"
#include <darkspeak/Config.h>
#include "darkspeak/md5.h"

using namespace std;
using namespace war;

std::ostream& operator << (std::ostream& o, const darkspeak::impl::TorChatPeer& v) {
    return o << "{peer: " << v.GetId() << '}';
}

std::ostream& operator << (std::ostream& o,
                           const darkspeak::impl::TorChatPeer::State& v) {
    static const array<string, 7> names = {
        "UNINTIALIZED", "ACCEPTING", "CONNECTING", "AUTENTICATING",
        "AUTHENTICATED", "READY", "DONE"
    };

    return o << names.at(static_cast<int>(v));
}

std::ostream& operator << (std::ostream& o,
                           const darkspeak::impl::TorChatPeer::FileTransfer& v) {

    return o << v.GetInfo();

}

std::ostream& operator << (std::ostream& o,
                           darkspeak::impl::TorChatPeer::FileTransfer::State& v) {
    static const array<string, 5> names = {
        "UNINITIALIZED", "UNVERIFIED", "ACTIVE", "DONE", "ABORTED" };

    return o << names.at(static_cast<int>(v));
}

namespace darkspeak {
namespace impl {


TorChatPeer::State TorChatPeer::GetState() const
{
    LOG_TRACE4_FN << "State=" << state_
       << ", has_been_ready_=" << has_been_ready_;
    return state_;
}


void TorChatPeer::SetState(impl::TorChatPeer::State state)
{
    LOG_DEBUG_FN << "Setting state " << state
        << ", old stat was " << state_;

    state_ = state;
    if (state_ == State::READY) {
        LOG_TRACE1_FN << "Setting has_been_ready_";
        has_been_ready_ = true;
    }
}

void TorChatPeer::Close()
{
    if (compare_enum(state_, State::AUTHENTICATED) < 0) {
        peers_cookie.clear(); // We did not get a valid pong
    }

    LOG_DEBUG_FN << "Closing " << *this;
    SetState(State::DONE);

    if (conn_in_) {
        conn_in_->Close();
    }

    if (conn_out_) {
        conn_out_->Close();
    }

    sent_ping_ = false;
    sent_ping_ = false;
    got_ping_ = false;
    got_pong_ = false;

    info.status = Api::Status::OFF_LINE;
}

void TorChatPeer::InitCookie()
{
    static const std::string alphabet{"0123456789qwertyuiopasdfghjklzxcvbnm"
        "QWERTYUIOPASDFGHJKLZXCVBNM-_"};

    std::random_device device;
    std::mt19937 generator(device());
    std::uniform_int_distribution< size_t > distribution(0, alphabet.size() -1);

    for(int i = 0; i < 200; ++i) {
        my_cookie.push_back(alphabet[distribution(generator)]);
    }
}

void TorChatPeer::AddFileTransfer(TorChatPeer::FileTransfer::ptr_t transfer)
{
    LOG_DEBUG_FN << "Adding " << *transfer;
    WarMapAddUnique(file_transfers_, transfer->GetInfo().file_id, transfer);

    if (transfer->GetInfo().direction == Direction::OUTGOING) {
        transfer->StartSending();
    }
}

void TorChatPeer::RemoveFileTransfer(const boost::uuids::uuid& uuid)
{
    auto ft = GetFileTransfer(uuid);
    if (ft) {
        LOG_DEBUG_FN << "Removing " << *ft;
        file_transfers_.erase(uuid);
    }
}


TorChatPeer::FileTransfer::ptr_t TorChatPeer::GetFileTransfer(const string& cookie)
{
    for(auto& it : file_transfers_) {
        if (it.second->GetCookie().compare(cookie) == 0) {
            return it.second;
        }
    }
    return nullptr;
}

TorChatPeer::FileTransfer::ptr_t TorChatPeer::GetFileTransfer(
    const boost::uuids::uuid& uuid)
{
    auto it = file_transfers_.find(uuid);
    if (it == file_transfers_.end()) {
        return nullptr;
    }
    return it->second;
}


void TorChatPeer::FileTransfer::OnIncomingData(string&& data,
                                               unsigned int blockId)
{
    if (info_.direction != Direction::INCOMING) {
        WAR_THROW_T(war::ExceptionBadState, "This transfer is not inbound");
    }

    if (state_ == State::UNVERIFIED) {
        if (buffers_.size() > 42) {
            WAR_THROW_T(war::ExceptionOutOfRange, "The cache buffer is full");
        }

        LOG_TRACE1_FN << "Adding block #" << blockId << " to buffer "
            " for " << *this;


        buffers_.emplace_back(move(data), blockId);
        return;
    }

    Write(data, blockId);
    SendUpdateEvents();
}


///////////////////// FileTransfer /////////////////

void TorChatPeer::FileTransfer::SetState(TorChatPeer::FileTransfer::State newState)
{
    LOG_TRACE1_FN << "Changing from " << state_ << " to " << newState;
    state_ = newState;

    switch(state_) {
        case TorChatPeer::FileTransfer::State::UNINITIALIZED:
        case TorChatPeer::FileTransfer::State::UNVERIFIED:
            info_.state = FileInfo::State::PENDING;
            break;
        case TorChatPeer::FileTransfer::State::ACTIVE:
            info_.state = FileInfo::State::TRANSFERRING;
            break;
        case TorChatPeer::FileTransfer::State::DONE:
            info_.state = FileInfo::State::DONE;
            break;
        case TorChatPeer::FileTransfer::State::ABORTED:
            info_.state = FileInfo::State::ABORTED;
            break;
    }
}

void TorChatPeer::FileTransfer::StartSending()
{
    file_.open(info_.path.c_str(), ios::in | ios::binary);

    if (file_.fail()) {
        LOG_ERROR_FN << "Failed to open file " << log::Esc(info_.path.string())
            << " for read.";
        AbortTransfer("Failed to open file");
        return;
    }

    {
        //  filename <transfer_cookie> <file_size> <block_size> "filename"
        const static string filename{"filename"};
        parent_.GetEngine().SendCommand(filename,
            {GetCookie(), to_string(info_.length), to_string(block_size_),
                info_.path.filename().string()},
            parent_.shared_from_this());
    }


    read_buffer_.resize(block_size_);
    auto default_buffers = Config::MAX_OUT_BUFFERS_PER_FILE_TRANSFER_DEFAULT;
    const int max_buffers = parent_.GetEngine().GetConfig().Get<int>(
        Config::MAX_OUT_BUFFERS_PER_FILE_TRANSFER, default_buffers);
    for(int i = 0; i < max_buffers; ++i) {
        if (!SendBuffer()) {
            break; // No more data
        }
    }
}

bool TorChatPeer::FileTransfer::SendBuffer()
{
    auto bytes = DoSendBuffer(next_out_pos_);
    read_is_eof_ = bytes < block_size_;
    next_out_pos_ += bytes;
    return read_is_eof_;
}

size_t TorChatPeer::FileTransfer::DoSendBuffer(streampos offset)
{
    static const string filedata{"filedata"};
    static const string sp{" "};

    file_.seekg(offset);
    if (file_.fail()) {
        LOG_ERROR_FN << "Failed to set file offset to "
            << offset << " for " << *this
            << ". File offset is " << file_.tellg() ;


        AbortTransfer("Failed to set file offset");
        return 0;
    }
    file_.read(read_buffer_.data(), block_size_);
    auto read_bytes = file_.gcount();

    if (file_.fail()) {
        if (file_.eof()) {
            file_.clear();
        } else {
            LOG_ERROR_FN << "Failed to read data at offset " <<
                offset << " for " << *this;

            AbortTransfer("Read failed");
            return 0;
        }
    }

    if (read_bytes == 0) {
        // eof
        return 0;
    }

    read_buffer_.resize(read_bytes);

    MD5 md5;
    md5.update(reinterpret_cast<const unsigned char *>(read_buffer_.data()),
               read_bytes);
    md5.finalize();
    const auto checksum = md5.hex_digest();

    cmd_buffer_.reserve(read_bytes + 32 + GetCookie().size() );
    cmd_buffer_ = filedata + sp + GetCookie() + sp + to_string(offset)
        + sp + md5.hex_digest() + sp;
    cmd_buffer_.append(read_buffer_.data(), read_bytes);

    parent_.GetEngine().SendCommand(cmd_buffer_, {}, parent_.shared_from_this());
    sendt_pending_.emplace(offset, read_bytes);
    return read_bytes;
}

void TorChatPeer::FileTransfer::FiledataOk(streampos offset)
{
    auto it = sendt_pending_.find(offset);
    if (it == sendt_pending_.end()) {
        LOG_WARN_FN << "Got confirmation for an unknown block." << offset
            << ". Ignoring.";
        return;
    }

    info_.transferred += it->second; // bytes sent
    sendt_pending_.erase(it);

    if (state_ == State::UNVERIFIED) {
        SetState(State::ACTIVE);
    }

    if (!read_is_eof_) {
        SendBuffer();
    }

    SendUpdateEvents();
}

void TorChatPeer::FileTransfer::FiledataError(streampos offset)
{
    // Just Resend
    auto it = sendt_pending_.find(offset);
    if (it == sendt_pending_.end()) {
        LOG_WARN_FN << "Got error for an unknown block " << offset
            << ". Ignoring.";
        return;
    }

    LOG_DEBUG_FN << "Resending data at offset " << offset << " for " << *this;
    DoSendBuffer(offset);
}

void TorChatPeer::FileTransfer::StartDownload()
{
    // Set state
    SetState(TorChatPeer::FileTransfer::ACTIVE);

    // Open file
    auto raw_path = parent_.GetEngine().GetConfig().Get<string>(
        Config::DOWNLOAD_FOLDER, Config::DOWNLOAD_FOLDER_DEFAULT);

    // Expand macros
    boost::replace_all(raw_path, "{id}", info_.buddy_id);

    auto name = info_.path;
    info_.path = raw_path;

    if (!boost::filesystem::is_directory(info_.path)) {
        LOG_NOTICE << "Creating download directory " << log::Esc(info_.path.string());
        boost::filesystem::create_directories(info_.path);
    }

    info_.path /= name;
    if (!validate_filename_as_safe(info_.path)) {
        LOG_WARN_FN << "The path " << log::Esc(info_.path.string())
            << " is not safe. Aborting download.";
        AbortTransfer("Unsafe filename");
        return;
    }

    file_.open(info_.path.c_str(), ios::out | ios::binary | ios::trunc);

    for(const auto& buffer: buffers_) {
        Write(buffer.data, buffer.blkid);
    }
    buffers_.clear();
    SendUpdateEvents();
}

void TorChatPeer::FileTransfer::SendUpdateEvents()
{
    if (IsComplete()) {
        SetState(TorChatPeer::FileTransfer::State::DONE);
        LOG_NOTICE << "The file " << log::Esc(info_.path.string())
            << " from " << parent_ << " is successfully transferred.";
        parent_.RemoveFileTransfer(info_.file_id);
    }

    // Send event regarding progress or download complete
    for(auto& monitor : parent_.GetEngine().GetMonitors()) {
        monitor->OnFileTransferUpdate(info_);
    }
}

void TorChatPeer::FileTransfer::Write(const string data, uint64_t offset)
{
    if (static_cast<int64_t>(offset) > info_.length) {
        LOG_WARN_FN << "Trying to write after end of file. Aborting transfer. "
            << *this;

        AbortTransfer("Invalid file offset");
        return;
    }

    file_.seekg(offset);
    if (file_.fail()) {
        LOG_ERROR_FN << "Failed to set file offset to " <<
            offset << " for " << *this;

        AbortTransfer("Failed to set file offset");
        return;
    }
    file_.write(data.c_str(), data.size());

    if (file_.fail()) {
        LOG_ERROR_FN << "Failed to write data to offset " <<
            offset << " for " << *this;

        AbortTransfer("Write failed");
        return;
    }

    info_.transferred += data.size();
    AddSegment(offset, data.size());
    SendAck(offset);
}

/* Tor Chat allows the sender to send and re-send segments at any offset
 * within the file. We therefore need to keep track of the segments that
 * are written so that we know when we have all the data in the file.
 *
 * TODO: Add unit tests
 */
void TorChatPeer::FileTransfer::AddSegment(uint64_t offset, size_t size)
{
    // See if we can merge to an existing segment
    bool merged = false;
    auto insert_it = segments_.end();
    for (auto it = segments_.begin(); it != segments_.end(); ++it) {

        if ((offset % block_size_) != 0) {
            LOG_WARN_FN << "Got an unaligned segment at offset "
                << offset
                << " for " << *this
                << ". Aborting transfer.";

            AbortTransfer("Unaligned data segment");
            return;
        }

        if (size > block_size_) {
            LOG_WARN_FN << "Got an oversized segment at offset "
                << offset
                << " with size " << size
                << " for " << *this
                << ". Aborting transfer.";

            AbortTransfer("Oversized data segment");
            return;
        }

        // See if the new segment is alread inside this segment
        if ((it->start <= offset)
            && (it->next() >= (offset + size))) {
            // Duplicate / re-sent segment
            LOG_DEBUG_FN << "Duplicate segment at " << offset << " " << *this;
            continue;
        }

        // Try to merge before iterator
        if (it->prev() == offset) {
            it->start = offset;
            it->size += size;
            merged = true;
        }

        // Try to merge after iterator
        if (it->next() == offset) {
            it->size += size;
            merged = true;
            break;
        }

        if (it->start < offset) {
            insert_it = it;
        }

        // See if it can be merged with it +1
        auto next = it + 1;
        while (next != segments_.end() && (it->next() == next->start)) {
            it->size += next->size;
            segments_.erase(next);
            next = it + 1;
        }
    }

    if (!merged) {
        // Insert in ascending order
        segments_.insert(insert_it, {offset, size});
    }
}

bool TorChatPeer::FileTransfer::IsComplete() const
{
    if (info_.direction == Direction::INCOMING) {
        /* We are complete when we have only one merged segment covering
        * the entire file.
        */
        if (segments_.size() == 1) {
            const auto& seg = segments_.front();
            if ((seg.start == 0) && (static_cast<int64_t>(seg.size) == info_.length)) {
                return true;
            }
        }
    } else {
        // OUTGOING
        if (sendt_pending_.empty()) {
            return true;
        }
    }

    return false;
}


void TorChatPeer::FileTransfer::SendAck(std::uint64_t blockid)
{
    if (state_ == State::ABORTED) {
        return;
    }

    static const string ack{"filedata_ok"};
    parent_.GetEngine().SendCommand(ack,
                                    {GetCookie(), to_string(blockid)},
                                    parent_.shared_from_this());
}

void TorChatPeer::FileTransfer::AbortTransfer(const string& reason)
{
    static const string stop_sending{"file_stop_sending"};
    static const string stop_receiving{"file_stop_receiving"};

    if (state_ == State::ABORTED) {
        return; // Already aborted
    }

    info_.state = FileInfo::State::ABORTED;
    info_.failure_reason = reason;
    SetState(State::ABORTED);
    LOG_NOTICE << "Aborting " << *this << ". " << reason;

    if (file_.is_open()) {
        file_.close();
        if (info_.direction == darkspeak::Direction::INCOMING) {
            LOG_DEBUG_FN << "Removing file " << log::Esc(info_.path.string())
                << " because " << info_ << " is aborted.";
            boost::filesystem::remove(info_.path);
        }
    }

    parent_.GetEngine().SendCommand(
        info_.direction == darkspeak::Direction::INCOMING
        ? stop_sending : stop_receiving,
            {GetCookie()},
            parent_.shared_from_this());

    for(auto& monitor : parent_.GetEngine().GetMonitors()) {
        monitor->OnFileTransferUpdate(info_);
    }

    parent_.RemoveFileTransfer(info_.file_id);
}


} // impl
} // darkspeak
