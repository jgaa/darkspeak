
#pragma once

#include <chrono>

#include <boost/uuid/uuid.hpp>
#include <boost/uuid/uuid_io.hpp>

#include <QString>
#include <QDateTime>

#include "darkspeak/Api.h"

#ifdef SendMessage
// Thank you SO much Micro$oft!
#	undef SendMessage
#endif

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

    switch (status) {
        case darkspeak::Api::Status::OFF_LINE:
            return "Off-line";
        case darkspeak::Api::Status::AVAILABLE:
            return "Available";
        case  darkspeak::Api::Status::AWAY:
            return "Away";
        case darkspeak::Api::Status::LONG_TIME_AWAY:
            return "Extended Away";
        case darkspeak::Api::Status::BUSY:
            return "Busy";
        case darkspeak::Api::Status::REMOVED_ME:
            return "Removed me!";
    }

    return "Unknown";
}

inline QString Convert(const darkspeak::Api::AnonymityLevel level)
{
    switch(level) {
        case darkspeak::Api::AnonymityLevel::DEFAULT:
            // pass trough
        case darkspeak::Api::AnonymityLevel::NORMAL:
            return "Normal";
        case darkspeak::Api::AnonymityLevel::TRIVIAL:
            return "Trivial";
        case darkspeak::Api::AnonymityLevel::HIGH:
            return "High";
        case darkspeak::Api::AnonymityLevel::CRITICAL:
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
