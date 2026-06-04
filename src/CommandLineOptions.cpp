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
    OptionMaskWidth,
    OptionMaskHeight,
    OptionFullscreen,
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
        {OptionHelp, 'h', "help", no_argument, "", "Hilfe anzeigen", ""},
        {OptionInput, 'i', "input", required_argument, "path", "Videodatei als Input verwenden", ""},
        {OptionDevice, 'd', "device", required_argument, "path", "Kamera-Device verwenden", "/dev/video0"},
        {OptionOutput, 'o', "output", required_argument, "mode", "Output-Modus: window oder file", "window"},
        {OptionOutputFile, 0, "output-file", required_argument, "path", "Zieldatei bei --output file", "output.mp4"},
        {OptionWidth, 0, "width", required_argument, "pixels", "Verarbeitungsbreite", "1920"},
        {OptionHeight, 0, "height", required_argument, "pixels", "Verarbeitungshoehe", "1080"},
        {OptionMaskWidth, 0, "mask-width", required_argument, "pixels", "Breite der spaeteren Masken-Inferenz", "256"},
        {OptionMaskHeight, 0, "mask-height", required_argument, "pixels", "Hoehe der spaeteren Masken-Inferenz", "144"},
        {OptionFullscreen, 0, "fullscreen", no_argument, "", "Fenster fullscreen anzeigen, falls Output window benutzt wird", ""},
        {OptionVerbose, 'v', "verbose", no_argument, "", "Ausfuehrlichere Logs ausgeben", ""},
        {OptionVersion, 0, "version", no_argument, "", "Versionsinformation anzeigen", ""},
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
        error = "Ungueltiger Wert fuer " + optionName + ": " + value;
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

    error = "Ungueltiger Output-Modus: " + parsed + " (erlaubt: window, file)";
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
        case OptionVerbose:
            result.config.verbose = true;
            break;
        case OptionVersion:
            result.showVersion = true;
            break;
        case '?':
            if (optopt != 0) {
                error = std::string("Unbekannte oder unvollstaendige Option: -") + static_cast<char>(optopt);
            } else if (optind > 0 && optind <= argc) {
                error = "Unbekannte oder unvollstaendige Option: " + std::string(argv[optind - 1]);
            } else {
                error = "Unbekannte oder unvollstaendige Option";
            }
            return false;
        default:
            error = "Unerwarteter Parser-Zustand";
            return false;
        }
    }

    if (optind < argc) {
        error = "Unerwartetes Argument: " + std::string(argv[optind]);
        return false;
    }

    if (result.config.inputPath.empty() && result.config.devicePath.empty()) {
        error = "Es wurde weder --input noch --device angegeben.";
        return false;
    }

    if (result.config.outputMode == OutputMode::File && result.config.outputFile.empty()) {
        error = "--output-file darf bei --output file nicht leer sein.";
        return false;
    }

    return true;
}

std::string buildHelpText(const std::string& programName)
{
    std::ostringstream stream;
    stream << "Usage: " << programName << " [options]\n\n";
    stream << "Optionen:\n";

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
