#pragma once

#include "CommandLineOptions.h"

#include <opencv2/core.hpp>

#include <string>

cv::Mat renderForDisplay(const cv::Mat& source, cv::Size targetSize, DisplayMode mode);
cv::Size getWindowDisplaySize(const std::string& windowName, cv::Size fallbackSize);
