#pragma once

#include "CommandLineOptions.h"

#include <opencv2/core.hpp>

#include <string>

struct DisplayArea {
    cv::Rect windowRect;
    cv::Size canvasSize;
    bool usedFallback = false;
};

struct DisplayRenderResult {
    cv::Mat frame;
    cv::Size canvasSize;
    cv::Rect destinationRect;
};

DisplayArea resolveDisplayArea(const std::string& windowName, cv::Size fallbackSize, bool forceFallbackSize);
DisplayRenderResult renderForDisplay(const cv::Mat& source, cv::Size targetSize, DisplayMode mode);
