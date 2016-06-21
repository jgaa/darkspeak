
#include <random>

#include "war_uuid.h"
#include "log/WarLog.h"
#include "war_helper.h"

#include "darkspeak/darkspeak.h"
#include "darkspeak/TorChatPeer.h"
#include "darkspeak/TorChatEngine.h"

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
    static const array<string, 4> names = {
        "UNINITIALIZED", "UNVERIFIED", "ACTIVE", "DONE" };

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
        buffers_.push_back(move(data));
        return;
    }

    // TODO: Implement file write

    // TODO: Send ack
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
    }
}

void TorChatPeer::FileTransfer::StartDownload()
{
    // Set state

    // Open file

    // Send event that the file transfer is in progress

    // Flush buffers

    // Send event regarding progress or download complete
}

void TorChatPeer::FileTransfer::AbortDownload()
{
    static const string stop_sending{"file_stop_sending"};

    info_.state = EventMonitor::FileInfo::State::ABORTED;
    LOG_NOTICE << "Aborting " << *this;

    // Send abort
    parent_.GetEngine().SendCommand(stop_sending,
                                    {GetCookie()},
                                    parent_.shared_from_this());

    // Send Event
    for(auto& monitor : parent_.GetEngine().GetMonitors()) {
        monitor->OnFileTransferUpdate(info_);
    }
}


} // impl
} // darkspeak
