#pragma once

#include <boost/asio.hpp>
#include <boost/asio/spawn.hpp>

#include "log/WarLog.h"

#include "darkspeak/darkspeak.h"
#include "darkspeak/Api.h"

namespace darkspeak {

template <typename T>
int compare_enum(T left, T right) {
    return static_cast<int>(left) - static_cast<int>(right);
}

} // namespace
