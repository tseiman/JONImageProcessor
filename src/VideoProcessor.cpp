#include "VideoProcessor.h"

#include "BenchmarkRecorder.h"
#include "CaptureBackendFactory.h"
#include "DisplayBackendFactory.h"
#include "DisplayEnvironment.h"
#include "ICaptureBackend.h"
#include "Logger.h"
#include "LowLatencyFrameCapture.h"
#include "MaskBackendFactory.h"
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

cv::Scalar overlayColorBgr(const RgbColor& color)
{
    return cv::Scalar(color.b, color.g, color.r);
}

cv::Mat applyBackgroundOverlay(
    const cv::Mat& frame,
    const cv::Mat& personMask,
    const RgbColor& color,
    double alpha)
{
    cv::Mat colorOverlay(frame.size(), frame.type(), overlayColorBgr(color));
    cv::Mat blended;
    cv::addWeighted(frame, 1.0 - alpha, colorOverlay, alpha, 0.0, blended);

    cv::Mat backgroundMask;
    cv::bitwise_not(personMask, backgroundMask);

    cv::Mat result = frame.clone();
    blended.copyTo(result, backgroundMask);

    std::vector<std::vector<cv::Point>> contours;
    cv::findContours(personMask, contours, cv::RETR_EXTERNAL, cv::CHAIN_APPROX_SIMPLE);
    cv::drawContours(result, contours, -1, cv::Scalar(255, 255, 255), 2, cv::LINE_AA);

    return result;
}

cv::Size fitSizePreservingAspect(cv::Size sourceSize, cv::Size bounds)
{
    if (sourceSize.width <= 0 || sourceSize.height <= 0 || bounds.width <= 0 || bounds.height <= 0) {
        return bounds;
    }

    const double sourceAspect = static_cast<double>(sourceSize.width) / static_cast<double>(sourceSize.height);
    const double boundsAspect = static_cast<double>(bounds.width) / static_cast<double>(bounds.height);

    if (sourceAspect > boundsAspect) {
        const int height = std::max(1, static_cast<int>(std::round(bounds.width / sourceAspect)));
        return cv::Size(bounds.width, height);
    }

    const int width = std::max(1, static_cast<int>(std::round(bounds.height * sourceAspect)));
    return cv::Size(width, bounds.height);
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

std::size_t effectiveDroppedFrames(const LowLatencyCaptureStats& stats, std::size_t processedFrames)
{
    const std::size_t unprocessedFrames = stats.capturedFrames > processedFrames
        ? stats.capturedFrames - processedFrames
        : 0;
    return std::max(stats.droppedFrames, unprocessedFrames);
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
    LOG_VERBOSE("Capture backend: " << captureBackendToString(config.captureBackend));
    LOG_VERBOSE("Processing size: " << config.width << "x" << config.height);
    LOG_VERBOSE("Mask backend: " << maskBackendToString(config.maskBackend));
    LOG_VERBOSE("Segmentation size: " << config.segmentationWidth << "x" << config.segmentationHeight);
    LOG_VERBOSE("Background overlay color: "
        << config.backgroundOverlayColor.r << ","
        << config.backgroundOverlayColor.g << ","
        << config.backgroundOverlayColor.b);
    LOG_VERBOSE("Background overlay alpha: " << config.backgroundOverlayAlpha);
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
    const CaptureBackendType effectiveCaptureBackend = usingCamera
        ? config_.captureBackend
        : CaptureBackendType::OpenCv;
    LOG_INFO("Capture backend: " << captureBackendToString(effectiveCaptureBackend));
    const bool lowLatencyMode = usingCamera;
    LOG_INFO("Low latency mode: " << (lowLatencyMode ? "enabled" : "disabled"));

    if (!config_.verbose) {
        cv::utils::logging::setLogLevel(cv::utils::logging::LOG_LEVEL_ERROR);
    }

    std::unique_ptr<ICaptureBackend> captureBackend = CaptureBackendFactory::create(config_);
    if (!captureBackend) {
        LOG_ERROR("Cannot create capture backend: " << captureBackendToString(effectiveCaptureBackend));
        return ExitRuntimeError;
    }

    if (!captureBackend->open(config_)) {
        return ExitRuntimeError;
    }

    const cv::Size outputSize(config_.width, config_.height);
    const bool hasExplicitDisplaySize = config_.outputWidth > 0 && config_.outputHeight > 0;
    const bool useScreenDisplaySize = config_.fullscreen && screenInfo.available && !hasExplicitDisplaySize;
    const cv::Size configuredDisplaySize = hasExplicitDisplaySize
        ? cv::Size(config_.outputWidth, config_.outputHeight)
        : (useScreenDisplaySize ? screenInfo.size : outputSize);
    const bool forceConfiguredDisplaySize = hasExplicitDisplaySize || useScreenDisplaySize;
    const bool writeFile = config_.outputMode == OutputMode::File && !config_.noDisplay;
    const bool showWindow = config_.outputMode == OutputMode::Window && !config_.noDisplay;
    const bool maskEnabled = !config_.noMask && config_.maskBackend != MaskBackendType::None;
    const bool overlayEnabled = !config_.noOverlay && maskEnabled;

    std::unique_ptr<IMaskBackend> maskBackend;
    if (maskEnabled) {
        maskBackend = MaskBackendFactory::create(config_.maskBackend);
        if (!maskBackend) {
            LOG_ERROR("Cannot create mask backend: " << maskBackendToString(config_.maskBackend));
            return ExitRuntimeError;
        }

        if (!maskBackend->initialize(config_)) {
            LOG_ERROR("Cannot initialize mask backend: " << maskBackendToString(config_.maskBackend));
            return ExitRuntimeError;
        }
        LOG_INFO("Mask backend: " << maskBackend->name());
    } else {
        LOG_INFO("Mask backend: none");
    }

    cv::VideoWriter writer;
    std::unique_ptr<IDisplayBackend> displayBackend;
    if (writeFile) {
        double fps = captureBackend->fps();
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
        lowLatencyCapture.start(*captureBackend);
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
            readOk = captureBackend->read(frame);
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

        const cv::Size frameProcessingSize = showWindow
            ? fitSizePreservingAspect(frame.size(), outputSize)
            : outputSize;

        cv::Mat resized;
        {
            BenchmarkScope timer(benchmark, BenchmarkStage::Resize);
            cv::resize(frame, resized, frameProcessingSize, 0.0, 0.0, cv::INTER_LINEAR);
        }

        cv::Mat outputMask;
        if (maskEnabled) {
            cv::Mat segmentationMask;
            MaskTimings maskTimings;
            if (!maskBackend->generate(resized, frameIndex, segmentationMask, maskTimings)) {
                LOG_ERROR("Mask backend failed to generate a mask");
                break;
            }
            benchmark.add(BenchmarkStage::SegmentationPreprocess, maskTimings.preprocess);
            benchmark.add(BenchmarkStage::SegmentationInference, maskTimings.inference);
            benchmark.add(BenchmarkStage::SegmentationPostprocess, maskTimings.postprocess);

            if (!segmentationMask.empty()) {
                BenchmarkScope timer(benchmark, BenchmarkStage::MaskUpscale);
                cv::resize(segmentationMask, outputMask, resized.size(), 0.0, 0.0, cv::INTER_NEAREST);
            }
        }

        cv::Mat outputFrame;
        if (overlayEnabled && !outputMask.empty()) {
            BenchmarkScope timer(benchmark, BenchmarkStage::Overlay);
            outputFrame = applyBackgroundOverlay(
                resized,
                outputMask,
                config_.backgroundOverlayColor,
                config_.backgroundOverlayAlpha);
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
    captureBackend->close();

    return ExitOk;
}
