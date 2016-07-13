
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

    return o << "{FileTransfer: " << v.GetInfo().file_id
        << ", size " << v.GetInfo().length
        << ", name " << log::Esc(v.GetInfo().name)
        << "}";

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
            info_.state = EventMonitor::FileInfo::State::PENDING;
            break;
        case TorChatPeer::FileTransfer::State::ACTIVE:
            info_.state = EventMonitor::FileInfo::State::TRANSFERRING;
            break;
        case TorChatPeer::FileTransfer::State::DONE:
            info_.state = EventMonitor::FileInfo::State::DONE;
            break;
        case TorChatPeer::FileTransfer::State::ABORTED:
            info_.state = EventMonitor::FileInfo::State::ABORTED;
            break;
    }
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

    path_ = raw_path;

    if (!boost::filesystem::is_directory(path_)) {
        LOG_NOTICE << "Creating download directory " << log::Esc(path_.string());
        boost::filesystem::create_directories(path_);
    }

    path_ /= info_.name;
    if (!validate_filename_as_safe(path_)) {
        LOG_WARN_FN << "The path " << log::Esc(path_.string())
            << " is not safe. Aborting download.";
        AbortDownload("Unsafe filename");
        return;
    }

    file_.open(path_.c_str(), ios::out | ios::binary | ios::trunc);

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
        LOG_NOTICE << "The file " << log::Esc(info_.name)
            << " from " << parent_ << " is successfully received.";
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

        AbortDownload("Invalid file offset");
        return;
    }

    file_.seekg(offset);
    if (file_.tellg() != static_cast<streampos>(offset)) {
        LOG_ERROR_FN << "Failed to set file offset to " <<
            offset << " for " << *this;

        AbortDownload("Failed to set file offset");
        return;
    }
    file_.write(data.c_str(), data.size());
        info_.transferred += data.size();

    if (file_.fail() || file_.bad()) {
        LOG_ERROR_FN << "Failed to write data to offset " <<
            offset << " for " << *this;

        AbortDownload("Write failed");
        return;
    }

    info_.transferred += data.size();
    AddSegment(offset, data.size());
    SendAck(offset);
}

/* Tor Chat allows the sender to send and re-send segments at any offset
 * within the file. We therefore need to keep track of the segments that
 * are written so that we know when we have all the data in the file.
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

            AbortDownload("Unaligned data segment");
            return;
        }

        if (size > block_size_) {
            LOG_WARN_FN << "Got an oversized segment at offset "
                << offset
                << " with size " << size
                << " for " << *this
                << ". Aborting transfer.";

            AbortDownload("Oversized data segment");
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
    }

    if (!merged) {
        // Insert in ascending order
        segments_.insert(insert_it, {offset, size});
    }
}

/* We are complete when we have only one merged segment covering
 * the entire file.
 */
bool TorChatPeer::FileTransfer::IsComplete() const
{
    if (segments_.size() == 1) {
        const auto& seg = segments_.front();
        if ((seg.start == 0) && (static_cast<int64_t>(seg.size) == info_.length)) {
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

void TorChatPeer::FileTransfer::AbortDownload(const std::string& reason)
{
    static const string stop_sending{"file_stop_sending"};

    if (state_ == State::ABORTED) {
        return; // Already aborted
    }

    info_.state = EventMonitor::FileInfo::State::ABORTED;
    info_.failure_reason = reason;
    SetState(State::ABORTED);
    LOG_NOTICE << "Aborting " << *this << ". " << reason;

    if (file_.is_open()) {
        file_.close();
        boost::filesystem::remove(path_);
    }

    parent_.GetEngine().SendCommand(stop_sending,
                                    {GetCookie()},
                                    parent_.shared_from_this());

    for(auto& monitor : parent_.GetEngine().GetMonitors()) {
        monitor->OnFileTransferUpdate(info_);
    }

    parent_.RemoveFileTransfer(info_.file_id);
}


} // impl
} // darkspeak
