
#pragma once

#include <memory>
#include <future>

#include <boost/asio.hpp>

#include "darkspeak/Api.h"
#include "darkspeak.h"


namespace darkspeak {

struct FileInfo {
        enum class State {
        PENDING,
        TRANSFERRING,
        DONE,
        ABORTED
    };

    std::string buddy_id;
    boost::uuids::uuid file_id;
    path_t path;
    std::int64_t length = -1; // Unknown size
    std::int64_t transferred = 0;
    Direction direction = Direction::INCOMING;
    State state = State::PENDING;
    std::string failure_reason;

    bool IsActive() const noexcept {
        return (state == State::PENDING)
            || (state == State::TRANSFERRING);
    }

    int PercentageComplete() const {
        const double l = length;
        const double t = transferred;
        const auto p = (t / l) * 100.0;
        return static_cast<int>(p);
    }
};

} // namespace

std::ostream& operator << (std::ostream& o, const darkspeak::FileInfo& v);
