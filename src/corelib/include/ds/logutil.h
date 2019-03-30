#ifndef UTIL_H
#define UTIL_H

#include "logfault/logfault.h"

namespace ds {
namespace core {

enum class LogSystem {
    LOGFILE,
    APPLICATION,
    STDOUT
};

bool isEnabled(const LogSystem system);
logfault::LogLevel getLogLevel(const LogSystem system);

}} // namepaces

#endif // UTIL_H
