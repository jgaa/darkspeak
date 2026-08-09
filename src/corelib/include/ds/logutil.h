#ifndef UTIL_H
#define UTIL_H

#include <QSettings>

#include "logfault/logfault.h"

namespace ds {
namespace core {

enum class LogSystem {
    LOGFILE,
    APPLICATION,
    STDOUT
};

bool isEnabled(const QSettings& settings, const LogSystem system);
logfault::LogLevel getLogLevel(const QSettings& settings, const LogSystem system);
void ensureLogSettings(QSettings& settings);

}} // namepaces

#endif // UTIL_H
