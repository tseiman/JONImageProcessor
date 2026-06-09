#include "CommandLineOptions.h"
#include "Daemon.h"
#include "Logger.h"
#include "ShutdownSignal.h"
#include "Version.h"
#include "VideoProcessor.h"

#include <iostream>

namespace {

constexpr int ExitOk = 0;
constexpr int ExitUsageError = 1;

} // namespace

int main(int argc, char** argv)
{
    CommandLineResult commandLine;
    std::string error;

    if (!parseCommandLine(argc, argv, commandLine, error)) {
        LOG_ERROR(error);
        std::cout << '\n' << buildHelpText(argv[0]);
        return ExitUsageError;
    }

    Logger::setVerbose(commandLine.config.verbose);

    if (commandLine.showHelp) {
        std::cout << buildHelpText(argv[0]);
        return ExitOk;
    }

    if (commandLine.showVersion) {
        std::cout << jonImageProcessorVersionText() << '\n';
        return ExitOk;
    }

    if (!commandLine.config.noDaemon) {
        std::string daemonError;
        if (!daemonizeProcess(daemonError)) {
            LOG_ERROR("Cannot daemonize process: " << daemonError);
            return 2;
        }
        Logger::setSyslog(true);
    }

    installShutdownSignalHandlers();
    VideoProcessor processor(commandLine.config);
    const int result = processor.run();
    Logger::shutdown();
    return result;
}
