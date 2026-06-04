#pragma once

#include <string>

enum class OutputMode {
    Window,
    File
};

struct ProcessorConfig {
    std::string inputPath;
    std::string devicePath = "/dev/video0";
    OutputMode outputMode = OutputMode::Window;
    std::string outputFile = "output.mp4";
    int width = 1920;
    int height = 1080;
    int maskWidth = 256;
    int maskHeight = 144;
    bool fullscreen = false;
    bool verbose = false;
};

struct CommandLineResult {
    ProcessorConfig config;
    bool showHelp = false;
    bool showVersion = false;
};

bool parseCommandLine(int argc, char** argv, CommandLineResult& result, std::string& error);
std::string buildHelpText(const std::string& programName);
std::string outputModeToString(OutputMode mode);
