
#include <random>

#include "darkspeak/darkspeak.h"
#include "darkspeak/TorChatPeer.h"

using namespace std;


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

namespace darkspeak {
namespace impl {


void TorChatPeer::SetState(impl::TorChatPeer::State state)
{
    LOG_DEBUG_FN << "Setting state " << state
        << ", old stat was " << state_;

    state_ = state;
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
    std::uniform_int_distribution< int > distribution(0, alphabet.size() -1);

    for(int i = 0; i < 200; ++i) {
        my_cookie.push_back(alphabet[distribution(generator)]);
    }
}




} // impl
} // darkspeak
