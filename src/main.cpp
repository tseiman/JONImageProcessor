#include "CommandLineOptions.h"
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
        std::cerr << "Fehler: " << error << "\n\n";
        std::cerr << buildHelpText(argv[0]);
        return ExitUsageError;
    }

    if (commandLine.showHelp) {
        std::cout << buildHelpText(argv[0]);
        return ExitOk;
    }

    if (commandLine.showVersion) {
        std::cout << "JONImageProcessor " << JON_IMAGE_PROCESSOR_VERSION << '\n';
        return ExitOk;
    }

    VideoProcessor processor(commandLine.config);
    return processor.run();
}
