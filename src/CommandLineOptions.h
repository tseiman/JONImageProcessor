#pragma once

#include <string>

enum class OutputMode {
    Window,
    File
};

enum class DisplayMode {
    Fit,
    Fill,
    Stretch
};

enum class DisplayBackendType {
    HighGui
};

enum class CaptureBackendType {
    OpenCv,
    V4L2
};

enum class CameraFormat {
    MJPG,
    YUYV
};

struct ProcessorConfig {
    std::string inputPath;
    std::string devicePath = "/dev/video0";
    OutputMode outputMode = OutputMode::Window;
    DisplayMode displayMode = DisplayMode::Fit;
    DisplayBackendType displayBackend = DisplayBackendType::HighGui;
    CaptureBackendType captureBackend = CaptureBackendType::OpenCv;
    std::string outputFile = "output.mp4";
    int width = 1920;
    int height = 1080;
    int outputWidth = 0;
    int outputHeight = 0;
    int maskWidth = 256;
    int maskHeight = 144;
    CameraFormat cameraFormat = CameraFormat::MJPG;
    int cameraFps = 30;
    int maxFrames = 0;
    bool fullscreen = false;
    bool verbose = false;
    bool benchmark = false;
    bool lowLatency = false;
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
std::string outputModeToString(OutputMode mode);
std::string displayModeToString(DisplayMode mode);
std::string displayBackendToString(DisplayBackendType backend);
std::string captureBackendToString(CaptureBackendType backend);
std::string cameraFormatToString(CameraFormat format);
