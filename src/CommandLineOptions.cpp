#include "CommandLineOptions.h"

#include <getopt.h>

#include <cstdlib>
#include <iomanip>
#include <sstream>
#include <string_view>
#include <vector>

namespace {

enum OptionId {
    OptionHelp = 'h',
    OptionInput = 'i',
    OptionDevice = 'd',
    OptionOutput = 'o',
    OptionVerbose = 'v',
    OptionOutputFile = 1000,
    OptionWidth,
    OptionHeight,
    OptionOutputWidth,
    OptionOutputHeight,
    OptionMaskWidth,
    OptionMaskHeight,
    OptionFullscreen,
    OptionDisplayMode,
    OptionVersion
};

struct OptionDefinition {
    int id;
    char shortName;
    std::string_view longName;
    int argument;
    std::string_view valueName;
    std::string_view description;
    std::string_view defaultValue;
};

const std::vector<OptionDefinition>& optionDefinitions()
{
    static const std::vector<OptionDefinition> definitions = {
        {OptionHelp, 'h', "help", no_argument, "", "Show help", ""},
        {OptionInput, 'i', "input", required_argument, "path", "Use a video file as input", ""},
        {OptionDevice, 'd', "device", required_argument, "path", "Use a camera device", "/dev/video0"},
        {OptionOutput, 'o', "output", required_argument, "mode", "Output mode: window or file", "window"},
        {OptionOutputFile, 0, "output-file", required_argument, "path", "Target file for --output file", "output.mp4"},
        {OptionWidth, 0, "width", required_argument, "pixels", "Processing width", "1920"},
        {OptionHeight, 0, "height", required_argument, "pixels", "Processing height", "1080"},
        {OptionOutputWidth, 0, "output-width", required_argument, "pixels", "Explicit display render width", "auto"},
        {OptionOutputHeight, 0, "output-height", required_argument, "pixels", "Explicit display render height", "auto"},
        {OptionMaskWidth, 0, "mask-width", required_argument, "pixels", "Width for later mask inference", "256"},
        {OptionMaskHeight, 0, "mask-height", required_argument, "pixels", "Height for later mask inference", "144"},
        {OptionFullscreen, 0, "fullscreen", no_argument, "", "Show the window fullscreen when using window output", ""},
        {OptionDisplayMode, 0, "display-mode", required_argument, "mode", "Display mode: fit, fill, or stretch", "fit"},
        {OptionVerbose, 'v', "verbose", no_argument, "", "Enable more detailed logs", ""},
        {OptionVersion, 0, "version", no_argument, "", "Show version information", ""},
    };
    return definitions;
}

std::vector<option> buildLongOptions()
{
    std::vector<option> options;
    options.reserve(optionDefinitions().size() + 1);

    for (const auto& definition : optionDefinitions()) {
        options.push_back(option {
            definition.longName.data(),
            definition.argument,
            nullptr,
            definition.id
        });
    }

    options.push_back(option {nullptr, 0, nullptr, 0});
    return options;
}

std::string buildShortOptions()
{
    std::string options;

    for (const auto& definition : optionDefinitions()) {
        if (definition.shortName == 0) {
            continue;
        }

        options.push_back(definition.shortName);
        if (definition.argument == required_argument) {
            options.push_back(':');
        } else if (definition.argument == optional_argument) {
            options.append("::");
        }
    }

    return options;
}

bool parsePositiveInteger(const char* value, const std::string& optionName, int& target, std::string& error)
{
    char* end = nullptr;
    const long parsed = std::strtol(value, &end, 10);

    if (end == value || *end != '\0' || parsed <= 0 || parsed > 16384) {
        error = "Invalid value for " + optionName + ": " + value;
        return false;
    }

    target = static_cast<int>(parsed);
    return true;
}

bool parseOutputMode(const char* value, OutputMode& mode, std::string& error)
{
    const std::string parsed(value);
    if (parsed == "window") {
        mode = OutputMode::Window;
        return true;
    }
    if (parsed == "file") {
        mode = OutputMode::File;
        return true;
    }

    error = "Invalid output mode: " + parsed + " (allowed: window, file)";
    return false;
}

bool parseDisplayMode(const char* value, DisplayMode& mode, std::string& error)
{
    const std::string parsed(value);
    if (parsed == "fit") {
        mode = DisplayMode::Fit;
        return true;
    }
    if (parsed == "fill") {
        mode = DisplayMode::Fill;
        return true;
    }
    if (parsed == "stretch") {
        mode = DisplayMode::Stretch;
        return true;
    }

    error = "Invalid display mode: " + parsed + " (allowed: fit, fill, stretch)";
    return false;
}

std::string formatOptionName(const OptionDefinition& definition)
{
    std::ostringstream stream;

    if (definition.shortName != 0) {
        stream << "-" << definition.shortName << ", ";
    } else {
        stream << "    ";
    }

    stream << "--" << definition.longName;
    if (!definition.valueName.empty()) {
        stream << " <" << definition.valueName << ">";
    }

    return stream.str();
}

} // namespace

bool parseCommandLine(int argc, char** argv, CommandLineResult& result, std::string& error)
{
    const std::vector<option> longOptions = buildLongOptions();
    const std::string shortOptions = buildShortOptions();

    opterr = 0;
    optind = 1;

    while (true) {
        int optionIndex = 0;
        const int option = getopt_long(argc, argv, shortOptions.c_str(), longOptions.data(), &optionIndex);

        if (option == -1) {
            break;
        }

        switch (option) {
        case OptionHelp:
            result.showHelp = true;
            break;
        case OptionInput:
            result.config.inputPath = optarg;
            break;
        case OptionDevice:
            result.config.devicePath = optarg;
            break;
        case OptionOutput:
            if (!parseOutputMode(optarg, result.config.outputMode, error)) {
                return false;
            }
            break;
        case OptionOutputFile:
            result.config.outputFile = optarg;
            break;
        case OptionWidth:
            if (!parsePositiveInteger(optarg, "--width", result.config.width, error)) {
                return false;
            }
            break;
        case OptionHeight:
            if (!parsePositiveInteger(optarg, "--height", result.config.height, error)) {
                return false;
            }
            break;
        case OptionOutputWidth:
            if (!parsePositiveInteger(optarg, "--output-width", result.config.outputWidth, error)) {
                return false;
            }
            break;
        case OptionOutputHeight:
            if (!parsePositiveInteger(optarg, "--output-height", result.config.outputHeight, error)) {
                return false;
            }
            break;
        case OptionMaskWidth:
            if (!parsePositiveInteger(optarg, "--mask-width", result.config.maskWidth, error)) {
                return false;
            }
            break;
        case OptionMaskHeight:
            if (!parsePositiveInteger(optarg, "--mask-height", result.config.maskHeight, error)) {
                return false;
            }
            break;
        case OptionFullscreen:
            result.config.fullscreen = true;
            break;
        case OptionDisplayMode:
            if (!parseDisplayMode(optarg, result.config.displayMode, error)) {
                return false;
            }
            break;
        case OptionVerbose:
            result.config.verbose = true;
            break;
        case OptionVersion:
            result.showVersion = true;
            break;
        case '?':
            if (optopt != 0) {
                error = std::string("Unknown or incomplete option: -") + static_cast<char>(optopt);
            } else if (optind > 0 && optind <= argc) {
                error = "Unknown or incomplete option: " + std::string(argv[optind - 1]);
            } else {
                error = "Unknown or incomplete option";
            }
            return false;
        default:
            error = "Unexpected parser state";
            return false;
        }
    }

    if (optind < argc) {
        error = "Unexpected argument: " + std::string(argv[optind]);
        return false;
    }

    if (result.config.inputPath.empty() && result.config.devicePath.empty()) {
        error = "Neither --input nor --device was specified.";
        return false;
    }

    if (result.config.outputMode == OutputMode::File && result.config.outputFile.empty()) {
        error = "--output-file must not be empty when using --output file.";
        return false;
    }

    if ((result.config.outputWidth > 0) != (result.config.outputHeight > 0)) {
        error = "--output-width and --output-height must be specified together.";
        return false;
    }

    return true;
}

std::string buildHelpText(const std::string& programName)
{
    std::ostringstream stream;
    stream << "Usage: " << programName << " [options]\n\n";
    stream << "Options:\n";

    for (const auto& definition : optionDefinitions()) {
        stream << "  " << std::left << std::setw(32) << formatOptionName(definition)
               << definition.description;

        if (!definition.defaultValue.empty()) {
            stream << " (Default: " << definition.defaultValue << ")";
        }

        stream << '\n';
    }

    return stream.str();
}

std::string outputModeToString(OutputMode mode)
{
    switch (mode) {
    case OutputMode::Window:
        return "window";
    case OutputMode::File:
        return "file";
    }

    return "unknown";
}

std::string displayModeToString(DisplayMode mode)
{
    switch (mode) {
    case DisplayMode::Fit:
        return "fit";
    case DisplayMode::Fill:
        return "fill";
    case DisplayMode::Stretch:
        return "stretch";
    }

    return "unknown";
}
