#include "VideoProcessor.h"

#include "BenchmarkRecorder.h"
#include "CaptureBackendFactory.h"
#include "DisplayBackendFactory.h"
#include "DisplayEnvironment.h"
#include "ICaptureBackend.h"
#include "Logger.h"
#include "LowLatencyFrameCapture.h"
#include "MaskBackends.h"
#include "ShutdownSignal.h"
#include "Version.h"
#include "ipc/IPCServer.h"
#include "ipc/RuntimeState.h"

#include <opencv2/core.hpp>
#include <opencv2/core/utils/logger.hpp>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

#include <algorithm>
#include <array>
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

struct BackgroundEffectBuffers {
    cv::Mat downscaledFrame;
    cv::Mat downscaledBlurredFrame;
    cv::Mat blurredFrame;
    cv::Mat scaledBackgroundImage;
    cv::Mat result;
};

cv::Mat applyBackgroundOverlay(
    const cv::Mat& frame,
    const cv::Mat& personMask,
    const RgbColor& color,
    double alpha)
{
    if (frame.empty() || personMask.empty()) {
        return frame;
    }

    cv::Mat mask;
    if (personMask.type() == CV_8UC1 && personMask.size() == frame.size()) {
        mask = personMask;
    } else {
        cv::resize(personMask, mask, frame.size(), 0.0, 0.0, cv::INTER_LINEAR);
        if (mask.type() != CV_8UC1) {
            cv::cvtColor(mask, mask, cv::COLOR_BGR2GRAY);
        }
    }

    if (frame.type() != CV_8UC3 || mask.type() != CV_8UC1) {
        cv::Mat normalizedPersonMask;
        mask.convertTo(normalizedPersonMask, CV_32FC1, 1.0 / 255.0);

        cv::Mat backgroundAlpha = (1.0 - normalizedPersonMask) * alpha;
        std::vector<cv::Mat> alphaChannels(frame.channels(), backgroundAlpha);
        cv::Mat backgroundAlpha3;
        cv::merge(alphaChannels, backgroundAlpha3);

        cv::Mat frameFloat;
        frame.convertTo(frameFloat, CV_32FC3);

        cv::Mat colorOverlay(frame.size(), frame.type(), cv::Scalar(color.b, color.g, color.r));
        cv::Mat colorFloat;
        colorOverlay.convertTo(colorFloat, CV_32FC3);

        cv::Mat inverseAlpha3 = cv::Scalar::all(1.0) - backgroundAlpha3;
        cv::Mat resultFloat = frameFloat.mul(inverseAlpha3) + colorFloat.mul(backgroundAlpha3);
        cv::Mat result;
        resultFloat.convertTo(result, frame.type());
        return result;
    }

    const int alphaScale = std::clamp(static_cast<int>(std::round(alpha * 255.0)), 0, 255);
    const int overlayB = std::clamp(color.b, 0, 255);
    const int overlayG = std::clamp(color.g, 0, 255);
    const int overlayR = std::clamp(color.r, 0, 255);

    std::array<unsigned char, 256> overlayAlphaByMask {};
    std::array<unsigned char, 256> sourceAlphaByMask {};
    for (int maskValue = 0; maskValue < 256; ++maskValue) {
        const int overlayAlpha = ((255 - maskValue) * alphaScale + 127) / 255;
        overlayAlphaByMask[maskValue] = static_cast<unsigned char>(overlayAlpha);
        sourceAlphaByMask[maskValue] = static_cast<unsigned char>(255 - overlayAlpha);
    }

    cv::Mat result(frame.size(), frame.type());
    cv::parallel_for_(cv::Range(0, frame.rows), [&](const cv::Range& range) {
        for (int y = range.start; y < range.end; ++y) {
            const auto* source = frame.ptr<cv::Vec3b>(y);
            const auto* maskRow = mask.ptr<unsigned char>(y);
            auto* destination = result.ptr<cv::Vec3b>(y);

            for (int x = 0; x < frame.cols; ++x) {
                const int overlayAlpha = overlayAlphaByMask[maskRow[x]];
                const int sourceAlpha = sourceAlphaByMask[maskRow[x]];
                destination[x][0] = static_cast<unsigned char>((source[x][0] * sourceAlpha + overlayB * overlayAlpha + 127) / 255);
                destination[x][1] = static_cast<unsigned char>((source[x][1] * sourceAlpha + overlayG * overlayAlpha + 127) / 255);
                destination[x][2] = static_cast<unsigned char>((source[x][2] * sourceAlpha + overlayR * overlayAlpha + 127) / 255);
            }
        }
    });

    return result;
}

int blurKernelSize(int blurStrength)
{
    const int radius = std::clamp(blurStrength, 1, 100);
    return radius * 2 + 1;
}

int blurDownscaleFactor(int blurStrength)
{
    if (blurStrength >= 32) {
        return 4;
    }
    if (blurStrength >= 12) {
        return 2;
    }
    return 1;
}

cv::Mat applyBackgroundBlur(
    const cv::Mat& frame,
    const cv::Mat& personMask,
    int blurStrength,
    BackgroundEffectBuffers& buffers)
{
    if (frame.empty() || personMask.empty()) {
        return frame;
    }

    cv::Mat mask;
    if (personMask.type() == CV_8UC1 && personMask.size() == frame.size()) {
        mask = personMask;
    } else {
        cv::resize(personMask, mask, frame.size(), 0.0, 0.0, cv::INTER_LINEAR);
        if (mask.type() != CV_8UC1) {
            cv::cvtColor(mask, mask, cv::COLOR_BGR2GRAY);
        }
    }

    if (frame.type() != CV_8UC3 || mask.type() != CV_8UC1) {
        return frame;
    }

    const int downscaleFactor = blurDownscaleFactor(blurStrength);
    if (downscaleFactor > 1) {
        const cv::Size downscaledSize(
            std::max(1, frame.cols / downscaleFactor),
            std::max(1, frame.rows / downscaleFactor));
        cv::resize(frame, buffers.downscaledFrame, downscaledSize, 0.0, 0.0, cv::INTER_AREA);

        const int downscaledKernel = blurKernelSize(std::max(1, blurStrength / downscaleFactor));
        cv::GaussianBlur(
            buffers.downscaledFrame,
            buffers.downscaledBlurredFrame,
            cv::Size(downscaledKernel, downscaledKernel),
            0.0);
        cv::resize(buffers.downscaledBlurredFrame, buffers.blurredFrame, frame.size(), 0.0, 0.0, cv::INTER_LINEAR);
    } else {
        const int kernelSize = blurKernelSize(blurStrength);
        cv::GaussianBlur(frame, buffers.blurredFrame, cv::Size(kernelSize, kernelSize), 0.0);
    }

    buffers.result.create(frame.size(), frame.type());

    cv::parallel_for_(cv::Range(0, frame.rows), [&](const cv::Range& range) {
        for (int y = range.start; y < range.end; ++y) {
            const auto* source = frame.ptr<cv::Vec3b>(y);
            const auto* blurred = buffers.blurredFrame.ptr<cv::Vec3b>(y);
            const auto* maskRow = mask.ptr<unsigned char>(y);
            auto* destination = buffers.result.ptr<cv::Vec3b>(y);

            for (int x = 0; x < frame.cols; ++x) {
                const int personAlpha = maskRow[x];
                const int backgroundAlpha = 255 - personAlpha;
                destination[x][0] = static_cast<unsigned char>((source[x][0] * personAlpha + blurred[x][0] * backgroundAlpha + 127) / 255);
                destination[x][1] = static_cast<unsigned char>((source[x][1] * personAlpha + blurred[x][1] * backgroundAlpha + 127) / 255);
                destination[x][2] = static_cast<unsigned char>((source[x][2] * personAlpha + blurred[x][2] * backgroundAlpha + 127) / 255);
            }
        }
    });

    return buffers.result;
}

cv::Mat applyBackgroundImage(
    const cv::Mat& frame,
    const cv::Mat& personMask,
    const cv::Mat& backgroundImage,
    BackgroundEffectBuffers& buffers)
{
    if (frame.empty() || personMask.empty() || backgroundImage.empty()) {
        return frame;
    }

    cv::Mat mask;
    if (personMask.type() == CV_8UC1 && personMask.size() == frame.size()) {
        mask = personMask;
    } else {
        cv::resize(personMask, mask, frame.size(), 0.0, 0.0, cv::INTER_LINEAR);
        if (mask.type() != CV_8UC1) {
            cv::cvtColor(mask, mask, cv::COLOR_BGR2GRAY);
        }
    }

    if (frame.type() != CV_8UC3 || mask.type() != CV_8UC1) {
        return frame;
    }

    if (buffers.scaledBackgroundImage.size() != frame.size()
        || buffers.scaledBackgroundImage.type() != frame.type()) {
        cv::resize(backgroundImage, buffers.scaledBackgroundImage, frame.size(), 0.0, 0.0, cv::INTER_LINEAR);
    }

    buffers.result.create(frame.size(), frame.type());

    cv::parallel_for_(cv::Range(0, frame.rows), [&](const cv::Range& range) {
        for (int y = range.start; y < range.end; ++y) {
            const auto* source = frame.ptr<cv::Vec3b>(y);
            const auto* background = buffers.scaledBackgroundImage.ptr<cv::Vec3b>(y);
            const auto* maskRow = mask.ptr<unsigned char>(y);
            auto* destination = buffers.result.ptr<cv::Vec3b>(y);

            for (int x = 0; x < frame.cols; ++x) {
                const int personAlpha = maskRow[x];
                const int backgroundAlpha = 255 - personAlpha;
                destination[x][0] = static_cast<unsigned char>((source[x][0] * personAlpha + background[x][0] * backgroundAlpha + 127) / 255);
                destination[x][1] = static_cast<unsigned char>((source[x][1] * personAlpha + background[x][1] * backgroundAlpha + 127) / 255);
                destination[x][2] = static_cast<unsigned char>((source[x][2] * personAlpha + background[x][2] * backgroundAlpha + 127) / 255);
            }
        }
    });

    return buffers.result;
}

cv::Mat applyMaskPostprocessing(
    const cv::Mat& mask,
    cv::Mat& previousMask,
    const ProcessorConfig& config)
{
    if (mask.empty()) {
        previousMask.release();
        return mask;
    }

    cv::Mat processed;
    if (mask.type() == CV_8UC1) {
        processed = mask.clone();
    } else {
        cv::cvtColor(mask, processed, cv::COLOR_BGR2GRAY);
    }

    if (config.maskMorphology != MaskMorphologyMode::Off) {
        const int closeSize = config.maskMorphology == MaskMorphologyMode::Strong ? 9 : 5;
        const int dilateSize = config.maskMorphology == MaskMorphologyMode::Strong ? 7 : 3;
        const int blurSize = config.maskMorphology == MaskMorphologyMode::Strong ? 9 : 5;

        cv::Mat binary;
        cv::threshold(processed, binary, 127, 255, cv::THRESH_BINARY);

        const cv::Mat closeKernel = cv::getStructuringElement(
            cv::MORPH_ELLIPSE,
            cv::Size(closeSize, closeSize));
        cv::morphologyEx(binary, binary, cv::MORPH_CLOSE, closeKernel);

        const cv::Mat dilateKernel = cv::getStructuringElement(
            cv::MORPH_ELLIPSE,
            cv::Size(dilateSize, dilateSize));
        cv::dilate(binary, processed, dilateKernel);

        cv::GaussianBlur(processed, processed, cv::Size(blurSize, blurSize), 0.0);
    }

    if (config.maskSmoothing > 0.0 && !previousMask.empty() && previousMask.size() == processed.size()) {
        cv::Mat smoothed;
        cv::addWeighted(
            previousMask,
            config.maskSmoothing,
            processed,
            1.0 - config.maskSmoothing,
            0.0,
            smoothed);
        processed = smoothed;
    }

    previousMask = processed.clone();
    return processed;
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
    LOG_VERBOSE("Display mode: " << displayModeToString(DisplayMode::Fill));
    LOG_VERBOSE("Display backend: " << displayBackendToString(config.displayBackend));
    LOG_VERBOSE("Processing size: " << config.width << "x" << config.height);
    LOG_VERBOSE("Mask backend: tensorrt");
    LOG_VERBOSE("Segmentation size: " << config.segmentationWidth << "x" << config.segmentationHeight);
    LOG_VERBOSE("Mask model: " << (config.maskModelPath.empty() ? "none" : config.maskModelPath));
    LOG_VERBOSE("Mask threshold: " << config.maskThreshold);
    LOG_VERBOSE("Mask smoothing: " << config.maskSmoothing);
    LOG_VERBOSE("Mask morphology: " << maskMorphologyModeToString(config.maskMorphology));
    LOG_VERBOSE("Background effect: " << backgroundEffectToString(config.backgroundEffect));
    LOG_VERBOSE("Background image: " << (config.backgroundImagePath.empty() ? "none" : config.backgroundImagePath));
    LOG_VERBOSE("Background overlay color: "
        << config.backgroundOverlayColor.r << ","
        << config.backgroundOverlayColor.g << ","
        << config.backgroundOverlayColor.b);
    LOG_VERBOSE("Background overlay alpha: " << config.backgroundOverlayAlpha);
    LOG_VERBOSE("Blur strength: " << config.blurStrength);
    LOG_VERBOSE("Camera format: " << cameraFormatToString(config.cameraFormat));
    LOG_VERBOSE("Fullscreen: " << (config.fullscreen ? "true" : "false"));
    LOG_VERBOSE("Benchmark: " << (config.benchmark ? "true" : "false"));
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
    RuntimeState runtimeState(config_);
    IPCServer ipcServer(runtimeState, config_.ipcSocketPath);
    if (!ipcServer.start()) {
        return ExitRuntimeError;
    }
    LOG_INFO("Display backend: " << displayBackendToString(config_.displayBackend));
    const bool usingCamera = config_.inputPath.empty();
    LOG_INFO("Capture backend: " << (usingCamera ? "v4l2" : "opencv-file"));
    const bool lowLatencyMode = usingCamera;
    LOG_INFO("Low latency mode: " << (lowLatencyMode ? "enabled" : "disabled"));

    if (!config_.verbose) {
        cv::utils::logging::setLogLevel(cv::utils::logging::LOG_LEVEL_ERROR);
    }

    std::unique_ptr<ICaptureBackend> captureBackend = CaptureBackendFactory::create(config_);
    if (!captureBackend) {
        LOG_ERROR("Cannot create capture backend");
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
    const bool showWindow = !config_.noDisplay;
    const bool maskEnabled = !config_.noMask;
    const bool overlayEnabled = !config_.noOverlay && maskEnabled;

    std::unique_ptr<IMaskBackend> maskBackend;
    if (maskEnabled) {
        maskBackend = std::make_unique<TensorRtMaskBackend>();

        if (!maskBackend->initialize(config_)) {
            LOG_ERROR("Cannot initialize mask backend: tensorrt");
            return ExitRuntimeError;
        }
        LOG_INFO("Mask backend: " << maskBackend->name());
    } else {
        LOG_INFO("Mask backend: none");
    }

    std::unique_ptr<IDisplayBackend> displayBackend;
    if (showWindow) {
        displayBackend = DisplayBackendFactory::create(config_.displayBackend);
        if (!displayBackend) {
            LOG_ERROR("Cannot create display backend: " << displayBackendToString(config_.displayBackend));
            return ExitRuntimeError;
        }

        DisplayBackendConfig displayConfig;
        displayConfig.displayMode = DisplayMode::Fill;
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
    cv::Mat previousOutputMask;
    BackgroundEffectBuffers backgroundEffectBuffers;
    cv::Mat backgroundImage;
    std::string loadedBackgroundImagePath;
    if (config_.backgroundEffect == BackgroundEffect::Image && overlayEnabled) {
        backgroundImage = cv::imread(config_.backgroundImagePath, cv::IMREAD_COLOR);
        if (backgroundImage.empty()) {
            LOG_ERROR("Cannot read background image: " << config_.backgroundImagePath);
            return ExitRuntimeError;
        }
        loadedBackgroundImagePath = config_.backgroundImagePath;
        LOG_INFO("Background image loaded: " << config_.backgroundImagePath);
    }

    bool stoppedBySignal = false;
    while (true) {
        if (shutdownRequested()) {
            stoppedBySignal = true;
            break;
        }

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
            if (shutdownRequested()) {
                stoppedBySignal = true;
            }
            break;
        }

        if (frame.empty()) {
            continue;
        }
        ProcessorConfig runtimeConfig = runtimeState.configSnapshot();

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
        const bool runtimeMaskEnabled = !runtimeConfig.noMask;
        const bool runtimeOverlayEnabled = !runtimeConfig.noOverlay && runtimeMaskEnabled;
        if (runtimeMaskEnabled && !maskBackend) {
            maskBackend = std::make_unique<TensorRtMaskBackend>();
            if (!maskBackend->initialize(runtimeConfig)) {
                LOG_ERROR("Cannot initialize mask backend: tensorrt");
                break;
            }
            LOG_INFO("Mask backend: " << maskBackend->name());
        }

        if (runtimeMaskEnabled && maskBackend) {
            maskBackend->updateConfig(runtimeConfig);
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
                cv::resize(segmentationMask, outputMask, resized.size(), 0.0, 0.0, cv::INTER_LINEAR);
            }

            if (!outputMask.empty()) {
                BenchmarkScope timer(benchmark, BenchmarkStage::MaskPostprocess);
                outputMask = applyMaskPostprocessing(outputMask, previousOutputMask, runtimeConfig);
            } else {
                previousOutputMask.release();
            }
        } else {
            previousOutputMask.release();
        }

        cv::Mat outputFrame;
        if (runtimeOverlayEnabled && !outputMask.empty()) {
            if (runtimeConfig.backgroundEffect == BackgroundEffect::Blur) {
                BenchmarkScope timer(benchmark, BenchmarkStage::BackgroundBlur);
                outputFrame = applyBackgroundBlur(
                    resized,
                    outputMask,
                    runtimeConfig.blurStrength,
                    backgroundEffectBuffers);
            } else if (runtimeConfig.backgroundEffect == BackgroundEffect::Image) {
                if (loadedBackgroundImagePath != runtimeConfig.backgroundImagePath) {
                    backgroundImage = cv::imread(runtimeConfig.backgroundImagePath, cv::IMREAD_COLOR);
                    loadedBackgroundImagePath = runtimeConfig.backgroundImagePath;
                    if (backgroundImage.empty()) {
                        LOG_WARNING("Cannot read background image: " << runtimeConfig.backgroundImagePath);
                    } else {
                        backgroundEffectBuffers.scaledBackgroundImage.release();
                    }
                }
                BenchmarkScope timer(benchmark, BenchmarkStage::Overlay);
                outputFrame = applyBackgroundImage(
                    resized,
                    outputMask,
                    backgroundImage,
                    backgroundEffectBuffers);
            } else {
                BenchmarkScope timer(benchmark, BenchmarkStage::Overlay);
                outputFrame = applyBackgroundOverlay(
                    resized,
                    outputMask,
                    runtimeConfig.backgroundOverlayColor,
                    runtimeConfig.backgroundOverlayAlpha);
            }
        } else {
            outputFrame = resized;
        }

        if (showWindow) {
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
        runtimeState.updateBenchmark(benchmark.snapshot());
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
    }

    if (shutdownRequested()) {
        stoppedBySignal = true;
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

    if (stoppedBySignal) {
        LOG_INFO("Shutdown requested, stopping JONImageProcessor");
    }

    if (frameIndex == 0 && !stoppedBySignal) {
        LOG_ERROR("No frames could be read");
        return ExitRuntimeError;
    }

    LOG_INFO("Processed frames: " << frameIndex);
    benchmark.logSummary();
    ipcServer.stop();
    if (displayBackend) {
        displayBackend->shutdown();
    }
    captureBackend->close();

    return ExitOk;
}
