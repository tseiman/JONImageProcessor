#pragma once

#include <opencv2/core.hpp>

struct ScreenInfo {
    cv::Size size;
    bool available = false;
};

ScreenInfo detectPrimaryScreen();
