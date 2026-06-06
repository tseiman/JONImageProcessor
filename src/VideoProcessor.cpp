#include "VideoProcessor.h"

#include "BenchmarkRecorder.h"
#include "DisplayBackendFactory.h"
#include "DisplayEnvironment.h"
#include "Logger.h"
#include "LowLatencyFrameCapture.h"
#include "Version.h"

#include <opencv2/core.hpp>
#include <opencv2/core/utils/logger.hpp>
#include <opencv2/imgproc.hpp>
#include <opencv2/videoio.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <memory>
#include <sstream>
#include <string>
#include <sys/utsname.h>
#include <utility>
#include <vector>

namespace {

constexpr int ExitOk = 0;
constexpr int ExitRuntimeError = 2;

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

std::string sizeToString(cv::Size size)
{
    std::ostringstream stream;
    stream << size.width << "x" << size.height;
    return stream.str();
}

std::string screenInfoToString(const ScreenInfo& screenInfo)
{
    if (!screenInfo.available) {
        return "unknown";
    }

    return sizeToString(screenInfo.size);
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

int cameraFormatFourcc(CameraFormat format)
{
    switch (format) {
    case CameraFormat::MJPG:
        return cv::VideoWriter::fourcc('M', 'J', 'P', 'G');
    case CameraFormat::YUYV:
        return cv::VideoWriter::fourcc('Y', 'U', 'Y', 'V');
    }

    return cv::VideoWriter::fourcc('M', 'J', 'P', 'G');
}

std::string fourccToString(double fourccValue)
{
    const auto fourcc = static_cast<int>(std::llround(fourccValue));
    std::string text;
    text.push_back(static_cast<char>(fourcc & 0xff));
    text.push_back(static_cast<char>((fourcc >> 8) & 0xff));
    text.push_back(static_cast<char>((fourcc >> 16) & 0xff));
    text.push_back(static_cast<char>((fourcc >> 24) & 0xff));

    for (char& character : text) {
        if (character < 32 || character > 126) {
            character = '?';
        }
    }

    return text;
}

bool almostEqual(double left, double right, double tolerance)
{
    return std::fabs(left - right) <= tolerance;
}

std::size_t effectiveDroppedFrames(const LowLatencyCaptureStats& stats, std::size_t processedFrames)
{
    const std::size_t unprocessedFrames = stats.capturedFrames > processedFrames
        ? stats.capturedFrames - processedFrames
        : 0;
    return std::max(stats.droppedFrames, unprocessedFrames);
}

void logRequestedCameraConfig(const ProcessorConfig& config)
{
    LOG_VERBOSE("Requested camera device: " << config.devicePath);
    LOG_VERBOSE("Requested camera format: " << cameraFormatToString(config.cameraFormat));
    LOG_VERBOSE("Requested camera size: " << config.width << "x" << config.height);
    LOG_VERBOSE("Requested camera FPS: " << config.cameraFps);
}

void configureCameraCapture(cv::VideoCapture& capture, const ProcessorConfig& config)
{
    capture.set(cv::CAP_PROP_FOURCC, cameraFormatFourcc(config.cameraFormat));
    capture.set(cv::CAP_PROP_FRAME_WIDTH, config.width);
    capture.set(cv::CAP_PROP_FRAME_HEIGHT, config.height);
    capture.set(cv::CAP_PROP_FPS, config.cameraFps);

    const std::string activeFormat = fourccToString(capture.get(cv::CAP_PROP_FOURCC));
    const double activeWidth = capture.get(cv::CAP_PROP_FRAME_WIDTH);
    const double activeHeight = capture.get(cv::CAP_PROP_FRAME_HEIGHT);
    const double activeFps = capture.get(cv::CAP_PROP_FPS);

    LOG_VERBOSE("Active camera format: " << activeFormat);
    LOG_VERBOSE("Active camera size: " << static_cast<int>(std::llround(activeWidth))
        << "x" << static_cast<int>(std::llround(activeHeight)));
    LOG_VERBOSE("Active camera FPS: " << activeFps);

    const std::string requestedFormat = cameraFormatToString(config.cameraFormat);
    if (activeFormat != requestedFormat) {
        LOG_WARNING("Camera did not accept requested format. Requested "
            << requestedFormat << ", active " << activeFormat);
    }

    if (!almostEqual(activeWidth, config.width, 0.5) || !almostEqual(activeHeight, config.height, 0.5)) {
        LOG_WARNING("Camera did not accept requested size. Requested "
            << config.width << "x" << config.height << ", active "
            << static_cast<int>(std::llround(activeWidth)) << "x" << static_cast<int>(std::llround(activeHeight)));
    }

    if (!almostEqual(activeFps, config.cameraFps, 0.5)) {
        LOG_WARNING("Camera did not accept requested FPS. Requested "
            << config.cameraFps << ", active " << activeFps);
    }
}

void logStartupInfo(const ProcessorConfig& config, const ScreenInfo& screenInfo)
{
    const std::string inputSource = !config.inputPath.empty()
        ? config.inputPath
        : config.devicePath;

    LOG_INFO("JONImageProcessor starting");
    LOG_VERBOSE("Program version: " << jonImageProcessorReleaseVersionOrUnreleased());
    LOG_INFO("Git version: " << JON_IMAGE_PROCESSOR_GIT_VERSION);
    LOG_INFO("Build date: " << __DATE__ << " " << __TIME__ << " on " << JON_IMAGE_PROCESSOR_BUILD_HOST);
    LOG_INFO("Operating system: " << operatingSystemString());
    LOG_INFO("OpenCV version: " << CV_VERSION);
    LOG_VERBOSE("Primary screen size: " << screenInfoToString(screenInfo));
    LOG_VERBOSE("Input source: " << inputSource);
    LOG_VERBOSE("Output mode: " << outputModeToString(config.outputMode));
    LOG_VERBOSE("Display mode: " << displayModeToString(config.displayMode));
    LOG_VERBOSE("Display backend: " << displayBackendToString(config.displayBackend));
    LOG_VERBOSE("Processing size: " << config.width << "x" << config.height);
    LOG_VERBOSE("Mask size: " << config.maskWidth << "x" << config.maskHeight);
    LOG_VERBOSE("Camera format: " << cameraFormatToString(config.cameraFormat));
    LOG_VERBOSE("Camera FPS: " << config.cameraFps);
    LOG_VERBOSE("Fullscreen: " << (config.fullscreen ? "true" : "false"));
    LOG_VERBOSE("Benchmark: " << (config.benchmark ? "true" : "false"));
    LOG_VERBOSE("Low latency requested: " << (config.lowLatency ? "true" : "false"));
    LOG_VERBOSE("Max frames: " << (config.maxFrames > 0 ? std::to_string(config.maxFrames) : "unlimited"));
    LOG_VERBOSE("No display: " << (config.noDisplay ? "true" : "false"));
    LOG_VERBOSE("No mask: " << (config.noMask ? "true" : "false"));
    LOG_VERBOSE("No overlay: " << (config.noOverlay ? "true" : "false"));
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
    const ScreenInfo screenInfo = detectPrimaryScreen();
    logStartupInfo(config_, screenInfo);
    LOG_INFO("Display backend: " << displayBackendToString(config_.displayBackend));
    const bool usingCamera = config_.inputPath.empty();
    const bool lowLatencyMode = usingCamera;
    LOG_INFO("Low latency mode: " << (lowLatencyMode ? "enabled" : "disabled"));

    if (!config_.verbose) {
        cv::utils::logging::setLogLevel(cv::utils::logging::LOG_LEVEL_ERROR);
    }

    cv::VideoCapture capture;

    if (!config_.inputPath.empty()) {
        LOG_INFO("Opening input video: " << config_.inputPath);
        capture.open(config_.inputPath);
    } else {
        LOG_INFO("Opening camera device: " << config_.devicePath);
        logRequestedCameraConfig(config_);
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

    if (config_.inputPath.empty()) {
        LOG_VERBOSE("Camera capture buffer size requested: 1");
        capture.set(cv::CAP_PROP_BUFFERSIZE, 1);
        configureCameraCapture(capture, config_);
    }

    const cv::Size outputSize(config_.width, config_.height);
    const bool hasExplicitDisplaySize = config_.outputWidth > 0 && config_.outputHeight > 0;
    const bool useScreenDisplaySize = config_.fullscreen && screenInfo.available && !hasExplicitDisplaySize;
    const cv::Size configuredDisplaySize = hasExplicitDisplaySize
        ? cv::Size(config_.outputWidth, config_.outputHeight)
        : (useScreenDisplaySize ? screenInfo.size : outputSize);
    const bool forceConfiguredDisplaySize = hasExplicitDisplaySize || useScreenDisplaySize;
    const cv::Size maskSize(config_.maskWidth, config_.maskHeight);
    const bool writeFile = config_.outputMode == OutputMode::File && !config_.noDisplay;
    const bool showWindow = config_.outputMode == OutputMode::Window && !config_.noDisplay;
    const bool maskEnabled = !config_.noMask;
    const bool overlayEnabled = !config_.noOverlay && maskEnabled;

    cv::VideoWriter writer;
    std::unique_ptr<IDisplayBackend> displayBackend;
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
    } else if (showWindow) {
        displayBackend = DisplayBackendFactory::create(config_.displayBackend);
        if (!displayBackend) {
            LOG_ERROR("Cannot create display backend: " << displayBackendToString(config_.displayBackend));
            return ExitRuntimeError;
        }

        DisplayBackendConfig displayConfig;
        displayConfig.displayMode = config_.displayMode;
        displayConfig.processingSize = outputSize;
        displayConfig.canvasFallbackSize = configuredDisplaySize;
        displayConfig.screenInfo = screenInfo;
        displayConfig.fullscreen = config_.fullscreen;
        displayConfig.forceCanvasFallbackSize = forceConfiguredDisplaySize;
        displayConfig.useScreenCanvasFallback = useScreenDisplaySize;

        if (!displayBackend->initialize(displayConfig)) {
            LOG_ERROR("Cannot initialize display backend: " << displayBackendToString(config_.displayBackend));
            return ExitRuntimeError;
        }
    }

    std::size_t frameIndex = 0;
    cv::Mat frame;
    BenchmarkRecorder benchmark(config_.benchmark);
    LowLatencyFrameCapture lowLatencyCapture;
    if (lowLatencyMode) {
        lowLatencyCapture.start(capture);
    }
    const auto startedAt = std::chrono::steady_clock::now();
    auto intervalStartedAt = startedAt;
    std::size_t intervalFrames = 0;

    while (true) {
        const auto pipelineStartedAt = std::chrono::steady_clock::now();
        bool readOk = false;
        if (lowLatencyMode) {
            std::chrono::steady_clock::duration captureWait {};
            std::chrono::steady_clock::duration frameHandover {};
            readOk = lowLatencyCapture.waitForLatestFrame(frame, captureWait, frameHandover);
            if (readOk) {
                benchmark.add(BenchmarkStage::CaptureWait, captureWait);
                benchmark.add(BenchmarkStage::FrameHandover, frameHandover);
            }
        } else {
            const auto decodeStartedAt = std::chrono::steady_clock::now();
            readOk = capture.read(frame);
            const auto decodeEndedAt = std::chrono::steady_clock::now();
            if (readOk) {
                benchmark.add(BenchmarkStage::Decode, decodeEndedAt - decodeStartedAt);
            }
        }

        if (!readOk) {
            break;
        }

        if (frame.empty()) {
            continue;
        }

        const auto processingStartedAt = std::chrono::steady_clock::now();

        cv::Mat resized;
        {
            BenchmarkScope timer(benchmark, BenchmarkStage::Resize);
            cv::resize(frame, resized, outputSize, 0.0, 0.0, cv::INTER_LINEAR);
        }

        cv::Mat outputMask;
        if (maskEnabled) {
            cv::Mat maskWorkCopy;
            {
                BenchmarkScope timer(benchmark, BenchmarkStage::Mask);
                cv::resize(resized, maskWorkCopy, maskSize, 0.0, 0.0, cv::INTER_AREA);
                cv::Mat smallMask = createDummyMask(maskWorkCopy.cols, maskWorkCopy.rows, frameIndex);
                maskWorkCopy = smallMask;
            }

            {
                BenchmarkScope timer(benchmark, BenchmarkStage::MaskUpscale);
                cv::resize(maskWorkCopy, outputMask, outputSize, 0.0, 0.0, cv::INTER_NEAREST);
            }
        }

        cv::Mat outputFrame;
        if (overlayEnabled) {
            BenchmarkScope timer(benchmark, BenchmarkStage::Overlay);
            outputFrame = applyMaskOverlay(resized, outputMask);
        } else {
            outputFrame = resized;
        }

        if (writeFile) {
            BenchmarkScope timer(benchmark, BenchmarkStage::Display);
            writer.write(outputFrame);
        } else if (showWindow) {
            BenchmarkScope timer(benchmark, BenchmarkStage::Display);
            if (!displayBackend->render(outputFrame)) {
                break;
            }
        }

        ++frameIndex;
        ++intervalFrames;
        const auto frameEndedAt = std::chrono::steady_clock::now();
        benchmark.add(BenchmarkStage::ProcessingTotal, frameEndedAt - processingStartedAt);
        benchmark.add(BenchmarkStage::PipelineTotal, frameEndedAt - pipelineStartedAt);
        benchmark.frameCompleted();
        benchmark.maybeLogProgress();

        if (config_.verbose) {
            const auto now = std::chrono::steady_clock::now();
            if (now - intervalStartedAt >= std::chrono::seconds(1)) {
                logPerformance(frameIndex, intervalFrames, startedAt, intervalStartedAt, now);
                if (lowLatencyMode) {
                    const auto stats = lowLatencyCapture.stats();
                    LOG_VERBOSE("Frames captured: " << stats.capturedFrames);
                    LOG_VERBOSE("Frames processed: " << frameIndex);
                    LOG_VERBOSE("Frames dropped/overwritten: " << effectiveDroppedFrames(stats, frameIndex));
                }
                intervalStartedAt = now;
                intervalFrames = 0;
            }
        }

        if (config_.maxFrames > 0 && frameIndex >= static_cast<std::size_t>(config_.maxFrames)) {
            break;
        }
    }

    if (lowLatencyMode) {
        lowLatencyCapture.stop();
        const auto stats = lowLatencyCapture.stats();
        const std::size_t droppedFrames = effectiveDroppedFrames(stats, frameIndex);
        LOG_VERBOSE("Frames captured: " << stats.capturedFrames);
        LOG_VERBOSE("Frames processed: " << frameIndex);
        LOG_VERBOSE("Frames dropped/overwritten: " << droppedFrames);
        benchmark.setCaptureStats(stats.capturedFrames, droppedFrames, stats.elapsed);
    }

    if (frameIndex == 0) {
        LOG_ERROR("No frames could be read");
        return ExitRuntimeError;
    }

    LOG_INFO("Processed frames: " << frameIndex);
    benchmark.logSummary();
    if (displayBackend) {
        displayBackend->shutdown();
    }

    return ExitOk;
}
