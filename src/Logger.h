#pragma once

#include <sstream>
#include <string>

namespace Logger {

void setVerbose(bool enabled);
bool isVerbose();

void info(const std::string& message);
void warning(const std::string& message);
void error(const std::string& message);
void verbose(const std::string& message);
void bench(const std::string& message);

} // namespace Logger

#define LOG_INFO(message) \
    do { \
        std::ostringstream logStream; \
        logStream << message; \
        Logger::info(logStream.str()); \
    } while (false)

#define LOG_WARNING(message) \
    do { \
        std::ostringstream logStream; \
        logStream << message; \
        Logger::warning(logStream.str()); \
    } while (false)

#define LOG_ERROR(message) \
    do { \
        std::ostringstream logStream; \
        logStream << message; \
        Logger::error(logStream.str()); \
    } while (false)

#define LOG_VERBOSE(message) \
    do { \
        std::ostringstream logStream; \
        logStream << message; \
        Logger::verbose(logStream.str()); \
    } while (false)

#define LOG_BENCH(message) \
    do { \
        std::ostringstream logStream; \
        logStream << message; \
        Logger::bench(logStream.str()); \
    } while (false)
