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
    OptionSegmentationWidth,
    OptionSegmentationHeight,
    OptionJetsonSegmentationModel,
    OptionMaskSmoothing,
    OptionMaskMorphology,
    OptionCameraFormat,
    OptionCameraFps,
    OptionMaskBackend,
    OptionBackgroundOverlayColor,
    OptionBackgroundOverlayAlpha,
    OptionFullscreen,
    OptionDisplayMode,
    OptionDisplayBackend,
    OptionCaptureBackend,
    OptionBenchmark,
    OptionLowLatency,
    OptionMaxFrames,
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
        {OptionDevice, 'd', "device", required_argument, "path", "Use a camera device", "/dev/video0"},
        {OptionOutput, 'o', "output", required_argument, "mode", "Output mode: window or file", "window"},
        {OptionOutputFile, 0, "output-file", required_argument, "path", "Target file for --output file", "output.mp4"},
        {OptionWidth, 0, "width", required_argument, "pixels", "Processing width", "1920"},
        {OptionHeight, 0, "height", required_argument, "pixels", "Processing height", "1080"},
        {OptionOutputWidth, 0, "output-width", required_argument, "pixels", "Explicit display render width", "auto"},
        {OptionOutputHeight, 0, "output-height", required_argument, "pixels", "Explicit display render height", "auto"},
        {OptionMaskWidth, 0, "mask-width", required_argument, "pixels", "Width for later mask inference", "256"},
        {OptionMaskHeight, 0, "mask-height", required_argument, "pixels", "Height for later mask inference", "144"},
        {OptionSegmentationWidth, 0, "segmentation-width", required_argument, "pixels", "Segmentation inference width", "256"},
        {OptionSegmentationHeight, 0, "segmentation-height", required_argument, "pixels", "Segmentation inference height", "144"},
        {OptionJetsonSegmentationModel, 0, "jetson-segmentation-model", required_argument, "model", "jetson-inference segNet model", "fcn-resnet18-voc-320x320"},
        {OptionMaskSmoothing, 0, "mask-smoothing", required_argument, "0.0..1.0", "Temporal mask smoothing strength", "0.65"},
        {OptionMaskMorphology, 0, "mask-morphology", required_argument, "mode", "Mask morphology: off, light, or strong", "light"},
        {OptionCameraFormat, 0, "camera-format", required_argument, "format", "Camera pixel format: MJPG or YUYV", "MJPG"},
        {OptionCameraFps, 0, "camera-fps", required_argument, "fps", "Requested camera frame rate", "30"},
        {OptionMaskBackend, 0, "mask-backend", required_argument, "backend", "Mask backend: none, dummy, or jetson", "dummy"},
        {OptionBackgroundOverlayColor, 0, "background-overlay-color", required_argument, "R,G,B", "Background overlay color", "0,0,255"},
        {OptionBackgroundOverlayAlpha, 0, "background-overlay-alpha", required_argument, "0.0..1.0", "Background overlay alpha", "0.35"},
        {OptionFullscreen, 0, "fullscreen", no_argument, "", "Show the window fullscreen when using window output", ""},
        {OptionDisplayMode, 0, "display-mode", required_argument, "mode", "Display mode: fit, fill, or stretch", "fit"},
        {OptionDisplayBackend, 0, "display-backend", required_argument, "backend", "Display backend: highgui", "highgui"},
        {OptionCaptureBackend, 0, "capture-backend", required_argument, "backend", "Capture backend for camera input: opencv or v4l2", "opencv"},
        {OptionBenchmark, 0, "benchmark", no_argument, "", "Enable benchmark mode", ""},
        {OptionLowLatency, 0, "low-latency", no_argument, "", "Enable low-latency live camera capture", ""},
        {OptionMaxFrames, 0, "max-frames", required_argument, "n", "Process at most n frames", ""},
        {OptionNoDisplay, 0, "no-display", no_argument, "", "Disable window and file output", ""},
        {OptionNoMask, 0, "no-mask", no_argument, "", "Disable mask generation", ""},
        {OptionNoOverlay, 0, "no-overlay", no_argument, "", "Disable overlay rendering", ""},
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

bool parseDisplayBackend(const char* value, DisplayBackendType& backend, std::string& error)
{
    const std::string parsed(value);
    if (parsed == "highgui") {
        backend = DisplayBackendType::HighGui;
        return true;
    }

    error = "Invalid display backend: " + parsed + " (allowed: highgui)";
    return false;
}

bool parseCaptureBackend(const char* value, CaptureBackendType& backend, std::string& error)
{
    const std::string parsed(value);
    if (parsed == "opencv") {
        backend = CaptureBackendType::OpenCv;
        return true;
    }
    if (parsed == "v4l2") {
        backend = CaptureBackendType::V4L2;
        return true;
    }

    error = "Invalid capture backend: " + parsed + " (allowed: opencv, v4l2)";
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

    error = "Invalid camera format: " + parsed;
    return false;
}

bool parseCameraFps(const char* value, int& target, std::string& error)
{
    char* end = nullptr;
    const long parsed = std::strtol(value, &end, 10);

    if (end == value || *end != '\0' || parsed <= 0 || parsed > 1000) {
        error = "Invalid camera FPS: " + std::string(value);
        return false;
    }

    target = static_cast<int>(parsed);
    return true;
}

bool parseMaskBackend(const char* value, MaskBackendType& backend, std::string& error)
{
    const std::string parsed(value);
    if (parsed == "none") {
        backend = MaskBackendType::None;
        return true;
    }
    if (parsed == "dummy") {
        backend = MaskBackendType::Dummy;
        return true;
    }
    if (parsed == "jetson") {
        backend = MaskBackendType::Jetson;
        return true;
    }

    error = "Invalid mask backend: " + parsed + " (allowed: none, dummy, jetson)";
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

bool parseOverlayAlpha(const char* value, double& alpha, std::string& error)
{
    char* end = nullptr;
    const double parsed = std::strtod(value, &end);
    if (end == value || *end != '\0' || parsed < 0.0 || parsed > 1.0) {
        error = "Invalid background overlay alpha: " + std::string(value);
        return false;
    }

    alpha = parsed;
    return true;
}

bool parseMaskSmoothing(const char* value, double& smoothing, std::string& error)
{
    char* end = nullptr;
    const double parsed = std::strtod(value, &end);
    if (end == value || *end != '\0' || parsed < 0.0 || parsed > 1.0) {
        error = "Invalid mask smoothing: " + std::string(value);
        return false;
    }

    smoothing = parsed;
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
            result.config.segmentationWidth = result.config.maskWidth;
            break;
        case OptionMaskHeight:
            if (!parsePositiveInteger(optarg, "--mask-height", result.config.maskHeight, error)) {
                return false;
            }
            result.config.segmentationHeight = result.config.maskHeight;
            break;
        case OptionSegmentationWidth:
            if (!parsePositiveInteger(optarg, "--segmentation-width", result.config.segmentationWidth, error)) {
                return false;
            }
            result.config.maskWidth = result.config.segmentationWidth;
            break;
        case OptionSegmentationHeight:
            if (!parsePositiveInteger(optarg, "--segmentation-height", result.config.segmentationHeight, error)) {
                return false;
            }
            result.config.maskHeight = result.config.segmentationHeight;
            break;
        case OptionJetsonSegmentationModel:
            result.config.jetsonSegmentationModel = optarg;
            if (result.config.jetsonSegmentationModel.empty()) {
                error = "--jetson-segmentation-model must not be empty.";
                return false;
            }
            break;
        case OptionMaskSmoothing:
            if (!parseMaskSmoothing(optarg, result.config.maskSmoothing, error)) {
                return false;
            }
            break;
        case OptionMaskMorphology:
            if (!parseMaskMorphology(optarg, result.config.maskMorphology, error)) {
                return false;
            }
            break;
        case OptionCameraFormat:
            if (!parseCameraFormat(optarg, result.config.cameraFormat, error)) {
                return false;
            }
            break;
        case OptionCameraFps:
            if (!parseCameraFps(optarg, result.config.cameraFps, error)) {
                return false;
            }
            break;
        case OptionMaskBackend:
            if (!parseMaskBackend(optarg, result.config.maskBackend, error)) {
                return false;
            }
            break;
        case OptionBackgroundOverlayColor:
            if (!parseOverlayColor(optarg, result.config.backgroundOverlayColor, error)) {
                return false;
            }
            break;
        case OptionBackgroundOverlayAlpha:
            if (!parseOverlayAlpha(optarg, result.config.backgroundOverlayAlpha, error)) {
                return false;
            }
            break;
        case OptionMaxFrames:
            if (!parsePositiveInteger(optarg, "--max-frames", result.config.maxFrames, error)) {
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
        case OptionDisplayBackend:
            if (!parseDisplayBackend(optarg, result.config.displayBackend, error)) {
                return false;
            }
            break;
        case OptionCaptureBackend:
            if (!parseCaptureBackend(optarg, result.config.captureBackend, error)) {
                return false;
            }
            break;
        case OptionVerbose:
            result.config.verbose = true;
            break;
        case OptionBenchmark:
            result.config.benchmark = true;
            break;
        case OptionLowLatency:
            result.config.lowLatency = true;
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
        stream << "  " << std::left << std::setw(42) << formatOptionName(definition)
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

std::string displayBackendToString(DisplayBackendType backend)
{
    switch (backend) {
    case DisplayBackendType::HighGui:
        return "highgui";
    }

    return "unknown";
}

std::string captureBackendToString(CaptureBackendType backend)
{
    switch (backend) {
    case CaptureBackendType::OpenCv:
        return "opencv";
    case CaptureBackendType::V4L2:
        return "v4l2";
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

std::string maskBackendToString(MaskBackendType backend)
{
    switch (backend) {
    case MaskBackendType::None:
        return "none";
    case MaskBackendType::Dummy:
        return "dummy";
    case MaskBackendType::Jetson:
        return "jetson";
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
