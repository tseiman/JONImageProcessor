#pragma once

#include "IDisplayBackend.h"

class OpenCvDisplayBackend : public IDisplayBackend {
public:
    bool initialize(const DisplayBackendConfig& config) override;
    bool render(const cv::Mat& frame) override;
    void shutdown() override;

private:
    DisplayBackendConfig config_;
    cv::Size lastInputSize_;
    cv::Size lastScreenSize_;
    cv::Size lastWindowRectSize_;
    cv::Size lastCanvasSize_;
    cv::Rect lastDestinationRect_;
    bool initialized_ = false;
    bool hasLoggedDisplayDiagnostics_ = false;
    bool hasWarnedDisplayFallback_ = false;
    bool hasLoggedFullscreenScreenFallback_ = false;
};
