#include "Logger.h"

#include <iostream>
#include <mutex>
#include <syslog.h>

namespace {

bool verboseEnabled = false;
bool syslogEnabled = false;
std::mutex logMutex;

void writeLog(const char* level, int priority, const std::string& message)
{
    std::lock_guard<std::mutex> lock(logMutex);
    if (syslogEnabled) {
        syslog(priority, "%s", (std::string("[") + level + "] " + message).c_str());
        return;
    }

    std::ostream& stream = priority == LOG_ERR ? std::cerr : std::cout;
    stream << '[' << level << "] " << message << '\n';
}

} // namespace

namespace Logger {

void setVerbose(bool enabled)
{
    std::lock_guard<std::mutex> lock(logMutex);
    verboseEnabled = enabled;
}

bool isVerbose()
{
    std::lock_guard<std::mutex> lock(logMutex);
    return verboseEnabled;
}

void setSyslog(bool enabled)
{
    std::lock_guard<std::mutex> lock(logMutex);
    syslogEnabled = enabled;
    if (syslogEnabled) {
        openlog("JONImageProcessor", LOG_PID, LOG_DAEMON);
    }
}

void shutdown()
{
    std::lock_guard<std::mutex> lock(logMutex);
    if (syslogEnabled) {
        closelog();
        syslogEnabled = false;
    }
}

void info(const std::string& message)
{
    writeLog("INFO", LOG_INFO, message);
}

void warning(const std::string& message)
{
    writeLog("WARNING", LOG_WARNING, message);
}

void error(const std::string& message)
{
    writeLog("ERROR", LOG_ERR, message);
}

void verbose(const std::string& message)
{
    {
        std::lock_guard<std::mutex> lock(logMutex);
        if (!verboseEnabled) {
            return;
        }
    }
    writeLog("VERBOSE", LOG_DEBUG, message);
}

void bench(const std::string& message)
{
    writeLog("BENCH", LOG_INFO, message);
}

} // namespace Logger
