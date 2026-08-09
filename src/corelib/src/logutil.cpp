
#include <array>

#include "include/ds/logutil.h"

#include "ds/dsengine.h"

namespace ds {
namespace core {

namespace  {

    QString getLogKey(const LogSystem system) {
        static const std::array<QString, 3> keys = {"logLevelFile", "logLevelApp", "logLevelStdout"};

        return keys.at(static_cast<size_t>(system));
    }

} // anonymous namespace

bool isEnabled(const QSettings& settings, const LogSystem system)
{
    return getLogLevel(settings, system) != logfault::LogLevel::DISABLED;
}

logfault::LogLevel getLogLevel(const QSettings& settings, const LogSystem system)
{
    static const std::array<logfault::LogLevel, 7> levels = { logfault::LogLevel::DISABLED,
                                                              logfault::LogLevel::ERROR,
                                                              logfault::LogLevel::WARN,
                                                              logfault::LogLevel::NOTICE,
                                                              logfault::LogLevel::INFO,
                                                              logfault::LogLevel::DEBUGGING,
                                                              logfault::LogLevel::TRACE };

    const auto level = settings.value(getLogKey(system), 0).toInt();
    return levels.at(static_cast<size_t>(level));
}

void ensureLogSettings(QSettings& settings)
{
    if (!settings.contains(QStringLiteral("logLevelApp"))) {
        settings.setValue(QStringLiteral("logLevelApp"),
                          static_cast<int>(logfault::LogLevel::DISABLED));
    }
    if (!settings.contains(QStringLiteral("logLevelFile"))) {
        settings.setValue(QStringLiteral("logLevelFile"),
                          static_cast<int>(logfault::LogLevel::DISABLED));
    }
    if (!settings.contains(QStringLiteral("logLevelStdout"))) {
        settings.setValue(QStringLiteral("logLevelStdout"),
                          static_cast<int>(logfault::LogLevel::DISABLED));
    }
    settings.sync();
}

}}
