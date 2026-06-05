#include "Logger.h"

#include <iostream>

namespace {

bool verboseEnabled = false;

void writeLog(const char* level, const std::string& message)
{
    std::cout << '[' << level << "] " << message << '\n';
}

} // namespace

namespace Logger {

void setVerbose(bool enabled)
{
    verboseEnabled = enabled;
}

bool isVerbose()
{
    return verboseEnabled;
}

void info(const std::string& message)
{
    writeLog("INFO", message);
}

void warning(const std::string& message)
{
    writeLog("WARNING", message);
}

void error(const std::string& message)
{
    writeLog("ERROR", message);
}

void verbose(const std::string& message)
{
    if (verboseEnabled) {
        writeLog("VERBOSE", message);
    }
}

} // namespace Logger
