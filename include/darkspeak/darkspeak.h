#pragma once

#include <boost/asio.hpp>

#include "war_error_handling.h"

namespace darkspeak {

struct ExceptionNotAllowed : public war::ExceptionBase {};
struct ExceptionDisconnectNow : public war::ExceptionBase {};
struct ExceptionNotConnected : public war::ExceptionBase {};

constexpr const char *GetClientName() {
    return "DarkSpeak";
}

constexpr const char *GetClientVersion() {
    return "0.01";
}

enum class Direction {
    INCOMING,
    OUTGOING
};

using asio_handler_t
        = std::function<void(const boost::system::error_code&, std::size_t)>;

} // namespace

std::ostream& operator << (std::ostream& o, const darkspeak::Direction& v);
