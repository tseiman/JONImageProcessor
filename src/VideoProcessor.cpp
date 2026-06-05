#include "VideoProcessor.h"

#include "DisplayRenderer.h"
#include "Logger.h"
#include "Version.h"

#include <opencv2/core.hpp>
#include <opencv2/core/utils/logger.hpp>
#include <opencv2/highgui.hpp>
#include <opencv2/imgproc.hpp>
#include <opencv2/videoio.hpp>

#include <algorithm>
#include <chrono>
#include <sstream>
#include <string>
#include <sys/utsname.h>
#include <utility>
#include <vector>

namespace {

constexpr int ExitOk = 0;
constexpr int ExitRuntimeError = 2;
constexpr const char* WindowName = "JONImageProcessor";

cv::Mat createDummyMask(int width, int height, std::size_t frameIndex)
{
    cv::Mat mask(height, width, CV_8UC1, cv::Scalar(0));
    const int radius = std::max(8, std::min(width, height) / 5);
    const int usableWidth = std::max(1, width - 2 * radius);
    const int x = radius + static_cast<int>((frameIndex * 3) % usableWidth);
    const int y = height / 2;

    cv::circle(mask, cv::Point(x, y), radius, cv::Scalar(255), cv::FILLED, cv::LINE_AA);
    return mask;
}

cv::Mat applyMaskOverlay(const cv::Mat& frame, const cv::Mat& mask)
{
    cv::Mat colorOverlay(frame.size(), frame.type(), cv::Scalar(40, 40, 220));
    cv::Mat blended;
    cv::addWeighted(frame, 0.65, colorOverlay, 0.35, 0.0, blended);

    cv::Mat result = frame.clone();
    blended.copyTo(result, mask);

    std::vector<std::vector<cv::Point>> contours;
    cv::findContours(mask, contours, cv::RETR_EXTERNAL, cv::CHAIN_APPROX_SIMPLE);
    cv::drawContours(result, contours, -1, cv::Scalar(255, 255, 255), 2, cv::LINE_AA);

    return result;
}

bool shouldStopFromKey(int key)
{
    const int normalized = key & 0xff;
    return normalized == 27 || normalized == 'q' || normalized == 'Q';
}

std::string sizeToString(cv::Size size)
{
    std::ostringstream stream;
    stream << size.width << "x" << size.height;
    return stream.str();
}

std::string rectToString(const cv::Rect& rect)
{
    std::ostringstream stream;
    stream << "x=" << rect.x << " y=" << rect.y << " width=" << rect.width << " height=" << rect.height;
    return stream.str();
}

bool displayStateChanged(
    cv::Size inputSize,
    cv::Size windowRectSize,
    cv::Size canvasSize,
    const cv::Rect& destinationRect,
    cv::Size& lastInputSize,
    cv::Size& lastWindowRectSize,
    cv::Size& lastCanvasSize,
    cv::Rect& lastDestinationRect)
{
    const bool changed = inputSize != lastInputSize
        || windowRectSize != lastWindowRectSize
        || canvasSize != lastCanvasSize
        || destinationRect != lastDestinationRect;

    if (changed) {
        lastInputSize = inputSize;
        lastWindowRectSize = windowRectSize;
        lastCanvasSize = canvasSize;
        lastDestinationRect = destinationRect;
    }

    return changed;
}

void logDisplayDiagnostics(
    cv::Size inputSize,
    cv::Size processingSize,
    cv::Rect windowRect,
    cv::Size canvasSize,
    DisplayMode displayMode,
    const cv::Rect& destinationRect)
{
    LOG_VERBOSE("Input frame: " << sizeToString(inputSize));
    LOG_VERBOSE("Processing size: " << sizeToString(processingSize));
    LOG_VERBOSE("Window size: " << sizeToString(windowRect.size()));
    LOG_VERBOSE("Canvas size: " << sizeToString(canvasSize));
    LOG_VERBOSE("Display mode: " << displayModeToString(displayMode));
    LOG_VERBOSE("Destination rect: " << rectToString(destinationRect));
}

std::string operatingSystemString()
{
    utsname info {};
    if (uname(&info) != 0) {
        return "unknown";
    }

    std::ostringstream stream;
    stream << info.sysname << ' ' << info.release << ' ' << info.machine;
    return stream.str();
}

void logStartupInfo(const ProcessorConfig& config)
{
    const std::string inputSource = !config.inputPath.empty()
        ? config.inputPath
        : config.devicePath;

    LOG_INFO("JONImageProcessor starting");
    LOG_VERBOSE("Program version: " << JON_IMAGE_PROCESSOR_VERSION);
    LOG_VERBOSE("Build date: " << __DATE__ << " " << __TIME__);
    LOG_VERBOSE("Operating system: " << operatingSystemString());
    LOG_VERBOSE("OpenCV version: " << CV_VERSION);
    LOG_VERBOSE("Input source: " << inputSource);
    LOG_VERBOSE("Output mode: " << outputModeToString(config.outputMode));
    LOG_VERBOSE("Display mode: " << displayModeToString(config.displayMode));
    LOG_VERBOSE("Processing size: " << config.width << "x" << config.height);
    LOG_VERBOSE("Mask size: " << config.maskWidth << "x" << config.maskHeight);
    LOG_VERBOSE("Fullscreen: " << (config.fullscreen ? "true" : "false"));
    if (config.outputWidth > 0 && config.outputHeight > 0) {
        LOG_VERBOSE("Output canvas override: " << config.outputWidth << "x" << config.outputHeight);
    } else {
        LOG_VERBOSE("Output canvas override: auto");
    }
}

void logPerformance(
    std::size_t processedFrames,
    std::size_t intervalFrames,
    std::chrono::steady_clock::time_point startedAt,
    std::chrono::steady_clock::time_point intervalStartedAt,
    std::chrono::steady_clock::time_point now)
{
    const std::chrono::duration<double> intervalDuration = now - intervalStartedAt;
    const std::chrono::duration<double> totalDuration = now - startedAt;
    if (intervalDuration.count() <= 0.0 || totalDuration.count() <= 0.0) {
        return;
    }

    const double currentFps = static_cast<double>(intervalFrames) / intervalDuration.count();
    const double averageFps = static_cast<double>(processedFrames) / totalDuration.count();
    LOG_VERBOSE("FPS: " << currentFps << " avg=" << averageFps << " frames=" << processedFrames);
}

} // namespace

VideoProcessor::VideoProcessor(ProcessorConfig config)
    : config_(std::move(config))
{
}

int VideoProcessor::run()
{
    logStartupInfo(config_);

    if (!config_.verbose) {
        cv::utils::logging::setLogLevel(cv::utils::logging::LOG_LEVEL_ERROR);
    }

    cv::VideoCapture capture;

    if (!config_.inputPath.empty()) {
        LOG_INFO("Opening input video: " << config_.inputPath);
        capture.open(config_.inputPath);
    } else {
        LOG_INFO("Opening camera device: " << config_.devicePath);
        capture.open(config_.devicePath, cv::CAP_ANY);
    }

    if (!capture.isOpened()) {
        if (!config_.inputPath.empty()) {
            LOG_ERROR("Cannot open input file: " << config_.inputPath);
        } else {
            LOG_ERROR("Cannot open camera device: " << config_.devicePath);
        }
        return ExitRuntimeError;
    }

    const cv::Size outputSize(config_.width, config_.height);
    const bool hasExplicitDisplaySize = config_.outputWidth > 0 && config_.outputHeight > 0;
    const cv::Size configuredDisplaySize = hasExplicitDisplaySize
        ? cv::Size(config_.outputWidth, config_.outputHeight)
        : outputSize;
    const cv::Size maskSize(config_.maskWidth, config_.maskHeight);
    const bool writeFile = config_.outputMode == OutputMode::File;

    cv::VideoWriter writer;
    if (writeFile) {
        double fps = capture.get(cv::CAP_PROP_FPS);
        if (fps <= 1.0 || fps > 240.0) {
            fps = 30.0;
        }

        const int fourcc = cv::VideoWriter::fourcc('m', 'p', '4', 'v');
        writer.open(config_.outputFile, fourcc, fps, outputSize, true);
        if (!writer.isOpened()) {
            LOG_ERROR("Cannot open output file: " << config_.outputFile);
            return ExitRuntimeError;
        }

        LOG_INFO("Writing MP4 output: " << config_.outputFile << " @ " << fps << " fps");
    } else {
        cv::namedWindow(WindowName, cv::WINDOW_NORMAL | cv::WINDOW_FREERATIO);
        cv::resizeWindow(WindowName, configuredDisplaySize.width, configuredDisplaySize.height);
        if (config_.fullscreen) {
            cv::setWindowProperty(WindowName, cv::WND_PROP_FULLSCREEN, cv::WINDOW_FULLSCREEN);
            cv::waitKey(1);
        }
    }

    std::size_t frameIndex = 0;
    cv::Mat frame;
    cv::Size lastInputSize;
    cv::Size lastWindowRectSize;
    cv::Size lastCanvasSize;
    cv::Rect lastDestinationRect;
    bool hasLoggedDisplayDiagnostics = false;
    bool hasWarnedDisplayFallback = false;
    const auto startedAt = std::chrono::steady_clock::now();
    auto intervalStartedAt = startedAt;
    std::size_t intervalFrames = 0;

    while (capture.read(frame)) {
        if (frame.empty()) {
            continue;
        }

        cv::Mat resized;
        cv::resize(frame, resized, outputSize, 0.0, 0.0, cv::INTER_LINEAR);

        cv::Mat maskWorkCopy;
        cv::resize(resized, maskWorkCopy, maskSize, 0.0, 0.0, cv::INTER_AREA);

        cv::Mat smallMask = createDummyMask(maskWorkCopy.cols, maskWorkCopy.rows, frameIndex);
        cv::Mat outputMask;
        cv::resize(smallMask, outputMask, outputSize, 0.0, 0.0, cv::INTER_NEAREST);

        cv::Mat outputFrame = applyMaskOverlay(resized, outputMask);

        if (writeFile) {
            writer.write(outputFrame);
        } else {
            const DisplayArea displayArea = resolveDisplayArea(WindowName, configuredDisplaySize, hasExplicitDisplaySize);
            const DisplayRenderResult displayResult = renderForDisplay(outputFrame, displayArea.canvasSize, config_.displayMode);

            if (displayArea.usedFallback && !hasExplicitDisplaySize && !hasWarnedDisplayFallback) {
                LOG_WARNING("Falling back to configured output size because HighGUI reported an unreliable window size: "
                    << sizeToString(displayArea.windowRect.size()));
                hasWarnedDisplayFallback = true;
            }

            if (config_.verbose) {
                const cv::Size inputSize = frame.size();
                const cv::Size windowRectSize = displayArea.windowRect.size();
                const bool changed = displayStateChanged(
                    inputSize,
                    windowRectSize,
                    displayResult.canvasSize,
                    displayResult.destinationRect,
                    lastInputSize,
                    lastWindowRectSize,
                    lastCanvasSize,
                    lastDestinationRect
                );
                const bool shouldLog = !hasLoggedDisplayDiagnostics || changed;

                if (shouldLog) {
                    logDisplayDiagnostics(
                        inputSize,
                        outputSize,
                        displayArea.windowRect,
                        displayResult.canvasSize,
                        config_.displayMode,
                        displayResult.destinationRect
                    );
                    hasLoggedDisplayDiagnostics = true;
                }
            }

            cv::imshow(WindowName, displayResult.frame);
            if (shouldStopFromKey(cv::waitKey(1))) {
                break;
            }
        }

        ++frameIndex;
        ++intervalFrames;

        if (config_.verbose) {
            const auto now = std::chrono::steady_clock::now();
            if (now - intervalStartedAt >= std::chrono::seconds(1)) {
                logPerformance(frameIndex, intervalFrames, startedAt, intervalStartedAt, now);
                intervalStartedAt = now;
                intervalFrames = 0;
            }
        }
    }

    if (frameIndex == 0) {
        LOG_ERROR("No frames could be read");
        return ExitRuntimeError;
    }

    LOG_INFO("Processed frames: " << frameIndex);

    return ExitOk;
}
