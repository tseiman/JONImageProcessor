#include "OpenCvDisplayBackend.h"

#include "DisplayRenderer.h"
#include "Logger.h"

#include <opencv2/highgui.hpp>

#include <sstream>

namespace {

constexpr const char* WindowName = "JONImageProcessor";

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

std::string screenInfoToString(const ScreenInfo& screenInfo)
{
    if (!screenInfo.available) {
        return "unknown";
    }

    return sizeToString(screenInfo.size);
}

std::string rectToString(const cv::Rect& rect)
{
    std::ostringstream stream;
    stream << "x=" << rect.x << " y=" << rect.y << " width=" << rect.width << " height=" << rect.height;
    return stream.str();
}

bool displayStateChanged(
    cv::Size inputSize,
    cv::Size screenSize,
    cv::Size windowRectSize,
    cv::Size canvasSize,
    const cv::Rect& destinationRect,
    cv::Size& lastInputSize,
    cv::Size& lastScreenSize,
    cv::Size& lastWindowRectSize,
    cv::Size& lastCanvasSize,
    cv::Rect& lastDestinationRect)
{
    const bool changed = inputSize != lastInputSize
        || screenSize != lastScreenSize
        || windowRectSize != lastWindowRectSize
        || canvasSize != lastCanvasSize
        || destinationRect != lastDestinationRect;

    if (changed) {
        lastInputSize = inputSize;
        lastScreenSize = screenSize;
        lastWindowRectSize = windowRectSize;
        lastCanvasSize = canvasSize;
        lastDestinationRect = destinationRect;
    }

    return changed;
}

void logDisplayDiagnostics(
    cv::Size inputSize,
    cv::Size processingSize,
    const ScreenInfo& screenInfo,
    cv::Rect windowRect,
    cv::Size canvasSize,
    DisplayMode displayMode,
    const cv::Rect& destinationRect)
{
    LOG_VERBOSE("Display input frame: " << sizeToString(inputSize));
    LOG_VERBOSE("Processing size: " << sizeToString(processingSize));
    LOG_VERBOSE("Screen size: " << screenInfoToString(screenInfo));
    LOG_VERBOSE("Window size: " << sizeToString(windowRect.size()));
    LOG_VERBOSE("Canvas size: " << sizeToString(canvasSize));
    LOG_VERBOSE("Display mode: " << displayModeToString(displayMode));
    LOG_VERBOSE("Destination rect: " << rectToString(destinationRect));
}

} // namespace

bool OpenCvDisplayBackend::initialize(const DisplayBackendConfig& config)
{
    config_ = config;

    cv::namedWindow(WindowName, cv::WINDOW_NORMAL | cv::WINDOW_FREERATIO);
    cv::resizeWindow(WindowName, config_.canvasFallbackSize.width, config_.canvasFallbackSize.height);
    if (config_.fullscreen) {
        cv::setWindowProperty(WindowName, cv::WND_PROP_FULLSCREEN, cv::WINDOW_FULLSCREEN);
        cv::waitKey(1);
    }

    initialized_ = true;
    LOG_VERBOSE("Display backend initialized: highgui");
    return true;
}

bool OpenCvDisplayBackend::render(const cv::Mat& frame)
{
    if (!initialized_) {
        LOG_ERROR("Display backend is not initialized");
        return false;
    }

    const DisplayArea displayArea = resolveDisplayArea(
        WindowName,
        config_.canvasFallbackSize,
        config_.forceCanvasFallbackSize
    );
    const DisplayRenderResult displayResult = renderForDisplay(frame, displayArea.canvasSize, config_.displayMode);

    if (config_.useScreenCanvasFallback && !hasLoggedFullscreenScreenFallback_) {
        LOG_VERBOSE("Using detected screen size for fullscreen render canvas: " << sizeToString(config_.screenInfo.size));
        hasLoggedFullscreenScreenFallback_ = true;
    }

    if (displayArea.usedFallback && !config_.forceCanvasFallbackSize && !hasWarnedDisplayFallback_) {
        LOG_WARNING("Falling back to configured output size because HighGUI reported an unreliable window size: "
            << sizeToString(displayArea.windowRect.size()));
        hasWarnedDisplayFallback_ = true;
    }

    const cv::Size inputSize = frame.size();
    const cv::Size screenSize = config_.screenInfo.available ? config_.screenInfo.size : cv::Size();
    const cv::Size windowRectSize = displayArea.windowRect.size();
    const bool changed = displayStateChanged(
        inputSize,
        screenSize,
        windowRectSize,
        displayResult.canvasSize,
        displayResult.destinationRect,
        lastInputSize_,
        lastScreenSize_,
        lastWindowRectSize_,
        lastCanvasSize_,
        lastDestinationRect_
    );
    const bool shouldLog = !hasLoggedDisplayDiagnostics_ || changed;

    if (shouldLog) {
        logDisplayDiagnostics(
            inputSize,
            config_.processingSize,
            config_.screenInfo,
            displayArea.windowRect,
            displayResult.canvasSize,
            config_.displayMode,
            displayResult.destinationRect
        );
        hasLoggedDisplayDiagnostics_ = true;
    }

    cv::imshow(WindowName, displayResult.frame);
    return !shouldStopFromKey(cv::waitKey(1));
}

void OpenCvDisplayBackend::shutdown()
{
    initialized_ = false;
}
