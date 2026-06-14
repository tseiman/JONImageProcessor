#include "CommandLineOptions.h"

#include "ConfigFile.h"
#include "Logger.h"

#include <getopt.h>
#include <sys/stat.h>

#include <cstdlib>
#include <cstring>
#include <cctype>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <string_view>
#include <algorithm>
#include <vector>

namespace {

enum OptionId {
    OptionHelp = 'h',
    OptionConfig = 'c',
    OptionTestConfig = 't',
    OptionInput = 'i',
    OptionDevice = 'd',
    OptionNoDaemon = 'n',
    OptionVerbose = 'v',
    OptionProcessingSize = 'p',
    OptionOutputSize = 'o',
    OptionSegmentationSize = 's',
    OptionMaskModel = 1000,
    OptionMaskThreshold,
    OptionMaskSmoothing,
    OptionMaskMorphology,
    OptionCameraFormat,
    OptionCameraConnectTimeout,
    OptionBackgroundEffect,
    OptionBackgroundImage,
    OptionBackgroundImageFolder,
    OptionBackgroundLoopIfVideo,
    OptionPauseImage,
    OptionPauseImageFolder,
    OptionPauseLoopIfVideo,
    OptionPauseImageEnabled,
    OptionPauseImageStatusText,
    OptionPauseImageTextColor,
    OptionPauseImageTextPosition,
    OptionPauseImageTextSize,
    OptionPauseImageFont,
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
    OptionDaemon,
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
        {OptionConfig, 'c', "config", required_argument, "path", "Read configuration from JSON file", ""},
        {OptionTestConfig, 't', "test-config", no_argument, "", "Parse and validate configuration, then exit", ""},
        {OptionInput, 'i', "input", required_argument, "path", "Use a video file as input", ""},
        {OptionDevice, 'd', "device", required_argument, "path", "Use a V4L2 camera device", "/dev/video0"},
        {OptionNoDaemon, 'n', "no-daemon", no_argument, "", "Run as foreground process; accepted for compatibility because this is now the default", ""},
        {OptionProcessingSize, 'p', "processing-size", required_argument, "WxH", "Processing size and requested camera size", "1920x1080"},
        {OptionOutputSize, 'o', "output-size", required_argument, "WxH", "Explicit display render size", "auto"},
        {OptionSegmentationSize, 's', "segmentation-size", required_argument, "WxH", "TensorRT segmentation size", "384x384"},
        {OptionMaskModel, 0, "mask-model", required_argument, "path", "TensorRT mask model path (.onnx or .engine)", ""},
        {OptionMaskThreshold, 0, "mask-threshold", required_argument, "0.0..1.0", "TensorRT foreground threshold", "0.5"},
        {OptionMaskSmoothing, 0, "mask-smoothing", required_argument, "0.0..1.0", "Temporal mask smoothing strength", "0.65"},
        {OptionMaskMorphology, 0, "mask-morphology", required_argument, "mode", "Mask morphology: off, light, or strong", "light"},
        {OptionCameraFormat, 0, "camera-format", required_argument, "format", "Camera pixel format: MJPG or YUYV", "MJPG"},
        {OptionCameraConnectTimeout, 0, "camera-connect-timeout", required_argument, "seconds", "Seconds to show Camera connecting before disconnected status", "10"},
        {OptionBackgroundEffect, 0, "background-effect", required_argument, "effect", "Background effect: color, blur, or image", "color"},
        {OptionBackgroundImage, 0, "background-image", required_argument, "path", "Image/video/html file for --background-effect image", ""},
        {OptionBackgroundImageFolder, 0, "background-image-folder", required_argument, "path", "Base folder for background images set through IPC", "."},
        {OptionBackgroundLoopIfVideo, 0, "background-loop-if-video", required_argument, "true|false", "Loop background file when it is a video", "false"},
        {OptionPauseImage, 0, "pause-image", required_argument, "path", "Image/video/html file for camera status screens", ""},
        {OptionPauseImageFolder, 0, "pause-image-folder", required_argument, "path", "Base folder for pause images set through IPC", "."},
        {OptionPauseLoopIfVideo, 0, "pause-loop-if-video", required_argument, "true|false", "Loop pause file when it is a video", "false"},
        {OptionPauseImageEnabled, 0, "pause-image-enabled", required_argument, "true|false", "Use pause image instead of generated camera status screens", "false"},
        {OptionPauseImageStatusText, 0, "pause-image-status-text", required_argument, "true|false", "Render camera status text over pause image", "true"},
        {OptionPauseImageTextColor, 0, "pause-image-text-color", required_argument, "RRGGBBAA", "Status text color for pause image overlay", "ffffffff"},
        {OptionPauseImageTextPosition, 0, "pause-image-text-position", required_argument, "XxY", "Status text position on pause image", "auto"},
        {OptionPauseImageTextSize, 0, "pause-image-text-size", required_argument, "value", "Status text size on pause image", "1.6"},
        {OptionPauseImageFont, 0, "pause-image-font", required_argument, "font", "Pause image font: plain, simplex, duplex, complex, triplex, complex-small, script-simplex, script-complex", "simplex"},
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
        {OptionDaemon, 0, "daemon", no_argument, "", "Detach into legacy self-daemon mode", ""},
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

bool parsePosition(const char* value, const std::string& optionName, Point2i& position, std::string& error)
{
    const std::string parsed(value);
    if (parsed == "auto") {
        position = Point2i {};
        return true;
    }
    const std::size_t separator = parsed.find('x');
    if (separator == std::string::npos || separator == 0 || separator == parsed.size() - 1 || parsed.find('x', separator + 1) != std::string::npos) {
        error = "Invalid value for " + optionName + ": " + parsed + " (expected XxY)";
        return false;
    }
    const std::string xPart = parsed.substr(0, separator);
    char* end = nullptr;
    const long x = std::strtol(xPart.c_str(), &end, 10);
    if (end == xPart.c_str() || *end != '\0' || x < 0 || x > 16384) {
        error = "Invalid value for " + optionName + ": " + parsed + " (expected non-negative XxY)";
        return false;
    }
    const std::string yPart = parsed.substr(separator + 1);
    const long y = std::strtol(yPart.c_str(), &end, 10);
    if (end == yPart.c_str() || *end != '\0' || y < 0 || y > 16384) {
        error = "Invalid value for " + optionName + ": " + parsed + " (expected non-negative XxY)";
        return false;
    }
    position.x = static_cast<int>(x);
    position.y = static_cast<int>(y);
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

bool parseTextSize(const char* value, const std::string& optionName, double& target, std::string& error)
{
    char* end = nullptr;
    const double parsed = std::strtod(value, &end);
    if (end == value || *end != '\0' || parsed < 0.1 || parsed > 10.0) {
        error = "Invalid value for " + optionName + ": " + value + " (allowed: 0.1..10.0)";
        return false;
    }
    target = parsed;
    return true;
}

bool parsePauseFont(const char* value, std::string& target, std::string& error)
{
    const std::string parsed(value);
    if (parsed == "plain" || parsed == "simplex" || parsed == "duplex"
        || parsed == "complex" || parsed == "triplex" || parsed == "complex-small"
        || parsed == "script-simplex" || parsed == "script-complex") {
        target = parsed;
        return true;
    }
    error = "Invalid pause image font: " + parsed;
    return false;
}

bool fileExists(const std::string& path)
{
    std::ifstream file(path);
    return static_cast<bool>(file);
}

bool isAbsolutePath(const std::string& path)
{
    return !path.empty() && path.front() == '/';
}

std::string resolveMediaPath(const std::string& folder, const std::string& path)
{
    if (path.empty() || isAbsolutePath(path) || folder.empty() || folder == ".") {
        return path;
    }
    if (folder.back() == '/') {
        return folder + path;
    }
    return folder + "/" + path;
}

bool looksLikeHtmlFile(const std::string& path)
{
    std::ifstream file(path);
    if (!file) return false;
    std::string prefix(512, '\0');
    file.read(prefix.data(), static_cast<std::streamsize>(prefix.size()));
    prefix.resize(static_cast<std::size_t>(file.gcount()));
    std::transform(prefix.begin(), prefix.end(), prefix.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    const auto first = prefix.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return false;
    prefix.erase(0, first);
    return prefix.rfind("<!doctype html", 0) == 0
        || prefix.rfind("<html", 0) == 0
        || prefix.find("<head") != std::string::npos
        || prefix.find("<body") != std::string::npos;
}

bool directoryExists(const std::string& path)
{
    struct stat statBuffer {};
    return ::stat(path.c_str(), &statBuffer) == 0 && S_ISDIR(statBuffer.st_mode);
}

void warnMissingConfiguredFiles(const ProcessorConfig& config)
{
    if (!config.noMask && !config.maskModelPath.empty() && !fileExists(config.maskModelPath)) {
        LOG_WARNING("Configured segmentation model does not exist: " << config.maskModelPath);
    }
    const std::string backgroundPath = resolveMediaPath(config.backgroundImageFolder, config.backgroundImagePath);
    if (config.backgroundEffect == BackgroundEffect::Image && !backgroundPath.empty() && !fileExists(backgroundPath)) {
        LOG_WARNING("Configured background media does not exist: " << backgroundPath);
    }
#if !defined(JON_ENABLE_WPE_HTML_RENDERER)
    if (config.backgroundEffect == BackgroundEffect::Image && !backgroundPath.empty() && looksLikeHtmlFile(backgroundPath)) {
        LOG_WARNING("Configured background HTML media is not supported in this build: " << backgroundPath);
    }
#endif
    if (!directoryExists(config.backgroundImageFolder)) {
        LOG_WARNING("Configured background image folder does not exist: " << config.backgroundImageFolder);
    }
    const std::string pausePath = resolveMediaPath(config.pauseImageFolder, config.pauseImagePath);
    if (config.pauseImageEnabled && !pausePath.empty() && !fileExists(pausePath)) {
        LOG_WARNING("Configured pause media does not exist: " << pausePath);
    }
#if !defined(JON_ENABLE_WPE_HTML_RENDERER)
    if (config.pauseImageEnabled && !pausePath.empty() && looksLikeHtmlFile(pausePath)) {
        LOG_WARNING("Configured pause HTML media is not supported in this build: " << pausePath);
    }
#endif
    if (!directoryExists(config.pauseImageFolder)) {
        LOG_WARNING("Configured pause image folder does not exist: " << config.pauseImageFolder);
    }
}

bool validateStartupFiles(const ProcessorConfig& config, std::string& error)
{
    if (!config.noMask && !config.maskModelPath.empty() && !fileExists(config.maskModelPath)) {
        error = "Segmentation model does not exist: " + config.maskModelPath;
        return false;
    }
    const std::string backgroundPath = resolveMediaPath(config.backgroundImageFolder, config.backgroundImagePath);
    if (config.backgroundEffect == BackgroundEffect::Image && !backgroundPath.empty() && !fileExists(backgroundPath)) {
        error = "Background media does not exist: " + backgroundPath;
        return false;
    }
#if !defined(JON_ENABLE_WPE_HTML_RENDERER)
    if (config.backgroundEffect == BackgroundEffect::Image && !backgroundPath.empty() && looksLikeHtmlFile(backgroundPath)) {
        error = "Background HTML media is not supported in this build: " + backgroundPath;
        return false;
    }
#endif
    if (!directoryExists(config.backgroundImageFolder)) {
        error = "Background image folder does not exist: " + config.backgroundImageFolder;
        return false;
    }
    if (!directoryExists(config.pauseImageFolder)) {
        error = "Pause image folder does not exist: " + config.pauseImageFolder;
        return false;
    }
    if (config.pauseImageEnabled) {
        if (config.pauseImagePath.empty()) {
            error = "Pause image is required when pause image is enabled.";
            return false;
        }
        const std::string pausePath = resolveMediaPath(config.pauseImageFolder, config.pauseImagePath);
        if (!fileExists(pausePath)) {
            error = "Pause media does not exist: " + pausePath;
            return false;
        }
#if !defined(JON_ENABLE_WPE_HTML_RENDERER)
        if (looksLikeHtmlFile(pausePath)) {
            error = "Pause HTML media is not supported in this build: " + pausePath;
            return false;
        }
#endif
    }
    return true;
}

bool validateConfiguredMediaTypes(const ProcessorConfig& config, std::string& error)
{
    const std::string backgroundPath = resolveMediaPath(config.backgroundImageFolder, config.backgroundImagePath);
#if !defined(JON_ENABLE_WPE_HTML_RENDERER)
    if (config.backgroundEffect == BackgroundEffect::Image && !backgroundPath.empty() && looksLikeHtmlFile(backgroundPath)) {
        error = "Background HTML media is not supported in this build: " + backgroundPath;
        return false;
    }
#endif
    const std::string pausePath = resolveMediaPath(config.pauseImageFolder, config.pauseImagePath);
#if !defined(JON_ENABLE_WPE_HTML_RENDERER)
    if (config.pauseImageEnabled && !pausePath.empty() && looksLikeHtmlFile(pausePath)) {
        error = "Pause HTML media is not supported in this build: " + pausePath;
        return false;
    }
#endif
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

int hexValue(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

bool parseRgbaHexColor(const char* value, RgbaColor& color, std::string& error)
{
    const std::string parsed(value);
    if (parsed.size() != 8) {
        error = "Invalid pause image text color: " + parsed + " (expected RRGGBBAA)";
        return false;
    }
    int parts[4] {};
    for (int index = 0; index < 4; ++index) {
        const int high = hexValue(parsed[static_cast<std::size_t>(index * 2)]);
        const int low = hexValue(parsed[static_cast<std::size_t>(index * 2 + 1)]);
        if (high < 0 || low < 0) {
            error = "Invalid pause image text color: " + parsed + " (expected RRGGBBAA)";
            return false;
        }
        parts[index] = high * 16 + low;
    }
    color = RgbaColor {parts[0], parts[1], parts[2], parts[3]};
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

bool parseBooleanText(const char* value, const std::string& optionName, bool& target, std::string& error)
{
    const std::string parsed(value);
    if (parsed == "true") {
        target = true;
        return true;
    }
    if (parsed == "false") {
        target = false;
        return true;
    }
    error = "Invalid value for " + optionName + ": " + parsed + " (allowed: true, false)";
    return false;
}

bool parseRangedInteger(const char* value, const std::string& optionName, int minValue, int maxValue, int& target, std::string& error)
{
    char* end = nullptr;
    const long parsed = std::strtol(value, &end, 10);
    if (end == value || *end != '\0' || parsed < minValue || parsed > maxValue) {
        error = "Invalid value for " + optionName + ": " + value;
        return false;
    }
    target = static_cast<int>(parsed);
    return true;
}

std::string requestedConfigPath(int argc, char** argv, bool& explicitConfig)
{
    explicitConfig = false;
    for (int index = 1; index < argc; ++index) {
        const std::string arg(argv[index]);
        if (arg == "-c" || arg == "--config") {
            explicitConfig = true;
            return index + 1 < argc ? argv[index + 1] : "";
        }
        constexpr const char* longPrefix = "--config=";
        if (arg.rfind(longPrefix, 0) == 0) {
            explicitConfig = true;
            return arg.substr(std::strlen(longPrefix));
        }
    }
    return {};
}

bool requestedTestConfig(int argc, char** argv)
{
    for (int index = 1; index < argc; ++index) {
        const std::string arg(argv[index]);
        if (arg == "-t" || arg == "--test-config") return true;
    }
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
    result.testConfig = requestedTestConfig(argc, argv);
    bool explicitConfig = false;
    std::string configPath = requestedConfigPath(argc, argv, explicitConfig);
    ConfigLoadResult configLoad;
    if (explicitConfig) {
        if (configPath.empty()) {
            error = "--config requires a path.";
            return false;
        }
        if (!loadJsonConfigFile(configPath, result.config, configLoad, error)) {
            result.showHelpOnError = false;
            return false;
        }
        result.configLoaded = true;
        result.configPath = configPath;
    } else {
        configPath = findDefaultConfigPath(argv[0]);
        if (!configPath.empty() && !loadJsonConfigFile(configPath, result.config, configLoad, error)) {
            result.showHelpOnError = false;
            return false;
        }
        result.configLoaded = !configPath.empty();
        result.configPath = configPath;
    }

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
        case OptionConfig:
            break;
        case OptionTestConfig:
            result.testConfig = true;
            break;
        case OptionInput:
            result.config.inputPath = optarg;
            break;
        case OptionDevice:
            result.config.devicePath = optarg;
            break;
        case OptionNoDaemon:
            result.config.noDaemon = true;
            break;
        case OptionDaemon:
            result.config.noDaemon = false;
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
        case OptionCameraConnectTimeout:
            if (!parseRangedInteger(optarg, "--camera-connect-timeout", 1, 300, result.config.cameraConnectTimeoutSeconds, error)) return false;
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
        case OptionBackgroundImageFolder:
            result.config.backgroundImageFolder = optarg;
            if (result.config.backgroundImageFolder.empty()) {
                error = "--background-image-folder must not be empty.";
                return false;
            }
            break;
        case OptionBackgroundLoopIfVideo:
            if (!parseBooleanText(optarg, "--background-loop-if-video", result.config.backgroundLoopIfVideo, error)) return false;
            break;
        case OptionPauseImage:
            result.config.pauseImagePath = optarg;
            if (result.config.pauseImagePath.empty()) {
                error = "--pause-image must not be empty.";
                return false;
            }
            break;
        case OptionPauseImageFolder:
            result.config.pauseImageFolder = optarg;
            if (result.config.pauseImageFolder.empty()) {
                error = "--pause-image-folder must not be empty.";
                return false;
            }
            break;
        case OptionPauseLoopIfVideo:
            if (!parseBooleanText(optarg, "--pause-loop-if-video", result.config.pauseLoopIfVideo, error)) return false;
            break;
        case OptionPauseImageEnabled:
            if (!parseBooleanText(optarg, "--pause-image-enabled", result.config.pauseImageEnabled, error)) return false;
            break;
        case OptionPauseImageStatusText:
            if (!parseBooleanText(optarg, "--pause-image-status-text", result.config.pauseImageShowStatusText, error)) return false;
            break;
        case OptionPauseImageTextColor:
            if (!parseRgbaHexColor(optarg, result.config.pauseImageTextColor, error)) return false;
            break;
        case OptionPauseImageTextPosition:
            if (!parsePosition(optarg, "--pause-image-text-position", result.config.pauseImageTextPosition, error)) return false;
            break;
        case OptionPauseImageTextSize:
            if (!parseTextSize(optarg, "--pause-image-text-size", result.config.pauseImageTextSize, error)) return false;
            break;
        case OptionPauseImageFont:
            if (!parsePauseFont(optarg, result.config.pauseImageFont, error)) return false;
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
    if (result.testConfig) {
        if (!validateConfiguredMediaTypes(result.config, error)) {
            result.showHelpOnError = false;
            return false;
        }
        warnMissingConfiguredFiles(result.config);
        return true;
    }
    if (!result.config.noDaemon) {
        if (configLoad.displayConfigured) {
            LOG_WARNING("Ignoring JSON display settings in daemon mode; using fullscreen DRM output");
        }
        result.config.displayBackend = DisplayBackendType::Drm;
        result.config.fullscreen = true;
    }
    if (!result.config.noMask && result.config.maskModelPath.empty()) {
        error = "--mask-model is required unless --no-mask is used.";
        return false;
    }
    if (result.config.backgroundEffect == BackgroundEffect::Image && result.config.backgroundImagePath.empty()) {
        error = "--background-image is required when --background-effect image is used.";
        return false;
    }
    if (!validateStartupFiles(result.config, error)) {
        result.showHelpOnError = false;
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
        stream << "  " << std::left << std::setw(48) << formatOptionName(definition)
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
