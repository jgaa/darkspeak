
#pragma once

#include <chrono>

#include <boost/uuid/uuid.hpp>
#include <boost/uuid/uuid_io.hpp>

#include "darkspeak/Api.h"


inline QString Convert(const time_t &when)
{
    if (when == 0) {
        return "Never";
    }
    QDateTime dt;
    dt.setTime_t(static_cast<uint>(when));
    return dt.toString(Qt::DefaultLocaleShortDate);
}

inline QString Convert(const std::chrono::system_clock::time_point &when)
{
    auto time_t_when = std::chrono::system_clock::to_time_t(when);
    return Convert(time_t_when);
}


inline QString Convert(const darkspeak::Api::Status &status)
{
    if (status == darkspeak::Api::Status::OFF_LINE) {
        return "Away";
    }

    if (status == darkspeak::Api::Status::AVAILABLE) {
        return "Available";
    }

    if (status == darkspeak::Api::Status::AWAY) {
        return "Away";
    }

    if (status == darkspeak::Api::Status::LONG_TIME_AWAY) {
        return "Extended Away";
    }

    if (status == darkspeak::Api::Status::BUSY) {
        return "Busy";
    }

    return "Unknown";
}

inline QString Convert(const darkspeak::Api::AnonymityLevel level)
{
    if (level == darkspeak::Api::AnonymityLevel::NORMAL) {
        return "Normal";
    }

    if (level == darkspeak::Api::AnonymityLevel::TRIVIAL) {
        return "Trivial";
    }

    if (level == darkspeak::Api::AnonymityLevel::HIGH) {
        return "High";
    }

    if (level == darkspeak::Api::AnonymityLevel::NORMAL) {
        return "Normal";
    }

    if (level == darkspeak::Api::AnonymityLevel::CRITICAL) {
        return "Critical";
    }

    return "Unknown";
}

inline QString Convert(const std::string& str) {
    return QString(str.c_str());
}

inline QString Convert(const boost::uuids::uuid& uuid) {
    auto str = boost::uuids::to_string(uuid);
    return Convert(str);
}
