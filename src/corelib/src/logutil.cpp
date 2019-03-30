
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

bool isEnabled(const LogSystem system)
{
    return getLogLevel(system) != logfault::LogLevel::DISABLED;
}

logfault::LogLevel getLogLevel(const LogSystem system)
{
    static const std::array<logfault::LogLevel, 7> levels = { logfault::LogLevel::DISABLED,
                                                              logfault::LogLevel::ERROR,
                                                              logfault::LogLevel::WARN,
                                                              logfault::LogLevel::NOTICE,
                                                              logfault::LogLevel::INFO,
                                                              logfault::LogLevel::DEBUGGING,
                                                              logfault::LogLevel::TRACE };

    const auto level = DsEngine::instance().settings().value(getLogKey(system), 0).toInt();
    return levels.at(static_cast<size_t>(level));
}

}}
