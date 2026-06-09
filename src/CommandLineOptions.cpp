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
    OptionVerbose = 'v',
    OptionProcessingSize = 'p',
    OptionOutputSize = 'o',
    OptionSegmentationSize = 's',
    OptionMaskModel = 1000,
    OptionMaskThreshold,
    OptionMaskSmoothing,
    OptionMaskMorphology,
    OptionCameraFormat,
    OptionBackgroundEffect,
    OptionBackgroundImage,
    OptionBackgroundOverlayColor,
    OptionBackgroundOverlayAlpha,
    OptionBlurStrength,
    OptionFullscreen,
    OptionDisplayBackend,
    OptionIpcSocket,
    OptionBenchmark,
    OptionNoDisplay,
    OptionNoMask,
    OptionNoOverlay,
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
        {OptionDevice, 'd', "device", required_argument, "path", "Use a V4L2 camera device", "/dev/video0"},
        {OptionProcessingSize, 'p', "processing-size", required_argument, "WxH", "Processing size and requested camera size", "1920x1080"},
        {OptionOutputSize, 'o', "output-size", required_argument, "WxH", "Explicit display render size", "auto"},
        {OptionSegmentationSize, 's', "segmentation-size", required_argument, "WxH", "TensorRT segmentation size", "384x384"},
        {OptionMaskModel, 0, "mask-model", required_argument, "path", "TensorRT mask model path (.onnx or .engine)", ""},
        {OptionMaskThreshold, 0, "mask-threshold", required_argument, "0.0..1.0", "TensorRT foreground threshold", "0.5"},
        {OptionMaskSmoothing, 0, "mask-smoothing", required_argument, "0.0..1.0", "Temporal mask smoothing strength", "0.65"},
        {OptionMaskMorphology, 0, "mask-morphology", required_argument, "mode", "Mask morphology: off, light, or strong", "light"},
        {OptionCameraFormat, 0, "camera-format", required_argument, "format", "Camera pixel format: MJPG or YUYV", "MJPG"},
        {OptionBackgroundEffect, 0, "background-effect", required_argument, "effect", "Background effect: color, blur, or image", "color"},
        {OptionBackgroundImage, 0, "background-image", required_argument, "path", "JPEG/PNG image for --background-effect image", ""},
        {OptionBackgroundOverlayColor, 0, "background-overlay-color", required_argument, "R,G,B", "Background color for --background-effect color; ignored for blur/image", "0,255,0"},
        {OptionBackgroundOverlayAlpha, 0, "background-overlay-alpha", required_argument, "0.0..1.0", "Background alpha for --background-effect color; ignored for blur/image", "0.35"},
        {OptionBlurStrength, 0, "blur-strength", required_argument, "value", "Blur strength for --background-effect blur", "15"},
        {OptionDisplayBackend, 0, "display-backend", required_argument, "backend", "Display backend: highgui or drm", "highgui"},
        {OptionIpcSocket, 0, "ipc-socket", required_argument, "path", "Unix domain socket path, or 'none' to disable IPC", "/tmp/jonimageprocessor.sock"},
        {OptionFullscreen, 0, "fullscreen", no_argument, "", "Show fullscreen when display output is enabled", ""},
        {OptionBenchmark, 0, "benchmark", no_argument, "", "Enable benchmark mode", ""},
        {OptionNoDisplay, 0, "no-display", no_argument, "", "Disable display output", ""},
        {OptionNoMask, 0, "no-mask", no_argument, "", "Disable TensorRT mask generation", ""},
        {OptionNoOverlay, 0, "no-overlay", no_argument, "", "Disable background effect rendering", ""},
        {OptionVerbose, 'v', "verbose", no_argument, "", "Enable detailed logs", ""},
        {OptionVersion, 0, "version", no_argument, "", "Show version information", ""},
    };
    return definitions;
}

std::vector<option> buildLongOptions()
{
    std::vector<option> options;
    options.reserve(optionDefinitions().size() + 1);
    for (const auto& definition : optionDefinitions()) {
        options.push_back(option {definition.longName.data(), definition.argument, nullptr, definition.id});
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

bool parseSize(const char* value, const std::string& optionName, int& width, int& height, std::string& error)
{
    const std::string parsed(value);
    const std::size_t separator = parsed.find('x');
    if (separator == std::string::npos || separator == 0 || separator == parsed.size() - 1 || parsed.find('x', separator + 1) != std::string::npos) {
        error = "Invalid value for " + optionName + ": " + parsed + " (expected WIDTHxHEIGHT)";
        return false;
    }

    int parsedWidth = 0;
    int parsedHeight = 0;
    if (!parsePositiveInteger(parsed.substr(0, separator).c_str(), optionName, parsedWidth, error)
        || !parsePositiveInteger(parsed.substr(separator + 1).c_str(), optionName, parsedHeight, error)) {
        error = "Invalid value for " + optionName + ": " + parsed + " (expected positive WIDTHxHEIGHT)";
        return false;
    }

    width = parsedWidth;
    height = parsedHeight;
    return true;
}

bool parseUnitDouble(const char* value, const std::string& optionName, double& target, std::string& error)
{
    char* end = nullptr;
    const double parsed = std::strtod(value, &end);
    if (end == value || *end != '\0' || parsed < 0.0 || parsed > 1.0) {
        error = "Invalid value for " + optionName + ": " + value + " (allowed: 0.0..1.0)";
        return false;
    }
    target = parsed;
    return true;
}

bool parseDisplayBackend(const char* value, DisplayBackendType& backend, std::string& error)
{
    const std::string parsed(value);
    if (parsed == "highgui") {
        backend = DisplayBackendType::HighGui;
        return true;
    }
    if (parsed == "drm") {
        backend = DisplayBackendType::Drm;
        return true;
    }
    error = "Invalid display backend: " + parsed + " (allowed: highgui, drm)";
    return false;
}

bool parseCameraFormat(const char* value, CameraFormat& format, std::string& error)
{
    const std::string parsed(value);
    if (parsed == "MJPG") {
        format = CameraFormat::MJPG;
        return true;
    }
    if (parsed == "YUYV") {
        format = CameraFormat::YUYV;
        return true;
    }
    error = "Invalid camera format: " + parsed + " (allowed: MJPG, YUYV)";
    return false;
}

bool parseBackgroundEffect(const char* value, BackgroundEffect& effect, std::string& error)
{
    const std::string parsed(value);
    if (parsed == "color") {
        effect = BackgroundEffect::Color;
        return true;
    }
    if (parsed == "blur") {
        effect = BackgroundEffect::Blur;
        return true;
    }
    if (parsed == "image") {
        effect = BackgroundEffect::Image;
        return true;
    }
    error = "Invalid background effect: " + parsed + " (allowed: color, blur, image)";
    return false;
}

bool parseOverlayColor(const char* value, RgbColor& color, std::string& error)
{
    int parsed[3] {};
    const char* current = value;
    char* end = nullptr;
    for (int index = 0; index < 3; ++index) {
        const long component = std::strtol(current, &end, 10);
        if (end == current || component < 0 || component > 255) {
            error = "Invalid background overlay color: " + std::string(value);
            return false;
        }
        parsed[index] = static_cast<int>(component);
        if (index < 2) {
            if (*end != ',') {
                error = "Invalid background overlay color: " + std::string(value);
                return false;
            }
            current = end + 1;
        } else if (*end != '\0') {
            error = "Invalid background overlay color: " + std::string(value);
            return false;
        }
    }
    color.r = parsed[0];
    color.g = parsed[1];
    color.b = parsed[2];
    return true;
}

bool parseMaskMorphology(const char* value, MaskMorphologyMode& mode, std::string& error)
{
    const std::string parsed(value);
    if (parsed == "off") {
        mode = MaskMorphologyMode::Off;
        return true;
    }
    if (parsed == "light") {
        mode = MaskMorphologyMode::Light;
        return true;
    }
    if (parsed == "strong") {
        mode = MaskMorphologyMode::Strong;
        return true;
    }
    error = "Invalid mask morphology: " + parsed + " (allowed: off, light, strong)";
    return false;
}

bool parseBlurStrength(const char* value, int& strength, std::string& error)
{
    char* end = nullptr;
    const long parsed = std::strtol(value, &end, 10);
    if (end == value || *end != '\0' || parsed <= 0 || parsed > 100) {
        error = "Invalid blur strength: " + std::string(value) + " (allowed: 1..100)";
        return false;
    }
    strength = static_cast<int>(parsed);
    return true;
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
        case OptionProcessingSize:
            if (!parseSize(optarg, "--processing-size", result.config.width, result.config.height, error)) return false;
            break;
        case OptionOutputSize:
            if (!parseSize(optarg, "--output-size", result.config.outputWidth, result.config.outputHeight, error)) return false;
            break;
        case OptionSegmentationSize:
            if (!parseSize(optarg, "--segmentation-size", result.config.segmentationWidth, result.config.segmentationHeight, error)) return false;
            break;
        case OptionMaskModel:
            result.config.maskModelPath = optarg;
            if (result.config.maskModelPath.empty()) {
                error = "--mask-model must not be empty.";
                return false;
            }
            break;
        case OptionMaskThreshold:
            if (!parseUnitDouble(optarg, "--mask-threshold", result.config.maskThreshold, error)) return false;
            break;
        case OptionMaskSmoothing:
            if (!parseUnitDouble(optarg, "--mask-smoothing", result.config.maskSmoothing, error)) return false;
            break;
        case OptionMaskMorphology:
            if (!parseMaskMorphology(optarg, result.config.maskMorphology, error)) return false;
            break;
        case OptionCameraFormat:
            if (!parseCameraFormat(optarg, result.config.cameraFormat, error)) return false;
            break;
        case OptionBackgroundEffect:
            if (!parseBackgroundEffect(optarg, result.config.backgroundEffect, error)) return false;
            break;
        case OptionBackgroundImage:
            result.config.backgroundImagePath = optarg;
            if (result.config.backgroundImagePath.empty()) {
                error = "--background-image must not be empty.";
                return false;
            }
            break;
        case OptionBackgroundOverlayColor:
            if (!parseOverlayColor(optarg, result.config.backgroundOverlayColor, error)) return false;
            break;
        case OptionBackgroundOverlayAlpha:
            if (!parseUnitDouble(optarg, "--background-overlay-alpha", result.config.backgroundOverlayAlpha, error)) return false;
            break;
        case OptionBlurStrength:
            if (!parseBlurStrength(optarg, result.config.blurStrength, error)) return false;
            break;
        case OptionDisplayBackend:
            if (!parseDisplayBackend(optarg, result.config.displayBackend, error)) return false;
            break;
        case OptionIpcSocket:
            result.config.ipcSocketPath = optarg;
            if (result.config.ipcSocketPath.empty()) {
                error = "--ipc-socket must not be empty.";
                return false;
            }
            break;
        case OptionFullscreen:
            result.config.fullscreen = true;
            break;
        case OptionBenchmark:
            result.config.benchmark = true;
            break;
        case OptionNoDisplay:
            result.config.noDisplay = true;
            break;
        case OptionNoMask:
            result.config.noMask = true;
            break;
        case OptionNoOverlay:
            result.config.noOverlay = true;
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
    if (result.showHelp || result.showVersion) {
        return true;
    }
    if (!result.config.noMask && result.config.maskModelPath.empty()) {
        error = "--mask-model is required unless --no-mask is used.";
        return false;
    }
    if (result.config.backgroundEffect == BackgroundEffect::Image && result.config.backgroundImagePath.empty()) {
        error = "--background-image is required when --background-effect image is used.";
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
        stream << "  " << std::left << std::setw(42) << formatOptionName(definition)
               << definition.description;
        if (!definition.defaultValue.empty()) {
            stream << " (Default: " << definition.defaultValue << ")";
        }
        stream << '\n';
    }
    return stream.str();
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

std::string displayBackendToString(DisplayBackendType backend)
{
    switch (backend) {
    case DisplayBackendType::HighGui:
        return "highgui";
    case DisplayBackendType::Drm:
        return "drm";
    }
    return "unknown";
}

std::string cameraFormatToString(CameraFormat format)
{
    switch (format) {
    case CameraFormat::MJPG:
        return "MJPG";
    case CameraFormat::YUYV:
        return "YUYV";
    }
    return "unknown";
}

std::string maskMorphologyModeToString(MaskMorphologyMode mode)
{
    switch (mode) {
    case MaskMorphologyMode::Off:
        return "off";
    case MaskMorphologyMode::Light:
        return "light";
    case MaskMorphologyMode::Strong:
        return "strong";
    }
    return "unknown";
}

std::string backgroundEffectToString(BackgroundEffect effect)
{
    switch (effect) {
    case BackgroundEffect::Color:
        return "color";
    case BackgroundEffect::Blur:
        return "blur";
    case BackgroundEffect::Image:
        return "image";
    }
    return "unknown";
}
