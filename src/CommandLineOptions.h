#pragma once

#include <string>

enum class DisplayMode {
    Fit,
    Fill,
    Stretch
};

enum class DisplayBackendType {
    HighGui,
    Drm
};

enum class CameraFormat {
    MJPG,
    YUYV
};

enum class MaskMorphologyMode {
    Off,
    Light,
    Strong
};

enum class BackgroundEffect {
    Color,
    Blur,
    Image
};

struct RgbColor {
    int r = 0;
    int g = 255;
    int b = 0;
};

struct ProcessorConfig {
    std::string inputPath;
    std::string devicePath = "/dev/video0";
    DisplayMode displayMode = DisplayMode::Fill;
    DisplayBackendType displayBackend = DisplayBackendType::HighGui;
    int width = 1920;
    int height = 1080;
    int outputWidth = 0;
    int outputHeight = 0;
    int segmentationWidth = 384;
    int segmentationHeight = 384;
    std::string maskModelPath;
    double maskThreshold = 0.5;
    double maskSmoothing = 0.65;
    MaskMorphologyMode maskMorphology = MaskMorphologyMode::Light;
    CameraFormat cameraFormat = CameraFormat::MJPG;
    BackgroundEffect backgroundEffect = BackgroundEffect::Color;
    std::string backgroundImagePath;
    RgbColor backgroundOverlayColor;
    double backgroundOverlayAlpha = 0.35;
    int blurStrength = 15;
    bool fullscreen = false;
    bool verbose = false;
    bool benchmark = false;
    bool noDisplay = false;
    bool noMask = false;
    bool noOverlay = false;
};

struct CommandLineResult {
    ProcessorConfig config;
    bool showHelp = false;
    bool showVersion = false;
};

bool parseCommandLine(int argc, char** argv, CommandLineResult& result, std::string& error);
std::string buildHelpText(const std::string& programName);
std::string displayModeToString(DisplayMode mode);
std::string displayBackendToString(DisplayBackendType backend);
std::string cameraFormatToString(CameraFormat format);
std::string maskMorphologyModeToString(MaskMorphologyMode mode);
std::string backgroundEffectToString(BackgroundEffect effect);
