#pragma once

#include "CommandLineOptions.h"
#include "DisplayEnvironment.h"

#include <opencv2/core.hpp>

struct DisplayBackendConfig {
    DisplayMode displayMode = DisplayMode::Fit;
    cv::Size processingSize;
    cv::Size canvasFallbackSize;
    ScreenInfo screenInfo;
    bool fullscreen = false;
    bool forceCanvasFallbackSize = false;
    bool useScreenCanvasFallback = false;
};

class IDisplayBackend {
public:
    virtual ~IDisplayBackend() = default;

    virtual bool initialize(const DisplayBackendConfig& config) = 0;
    virtual bool render(const cv::Mat& frame) = 0;
    virtual void shutdown() = 0;
};
