#pragma once

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

} // namespace

std::ostream& operator << (std::ostream& o, const darkspeak::Direction& v);
