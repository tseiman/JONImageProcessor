#include "DisplayRenderer.h"

#include <opencv2/highgui.hpp>
#include <opencv2/imgproc.hpp>

#include <algorithm>
#include <cmath>

namespace {

constexpr int MinimumReliableWindowExtent = 64;

cv::Rect calculateTargetRect(cv::Size sourceSize, cv::Size targetSize, DisplayMode mode)
{
    if (mode == DisplayMode::Stretch) {
        return cv::Rect(0, 0, targetSize.width, targetSize.height);
    }

    const double sourceAspect = static_cast<double>(sourceSize.width) / sourceSize.height;
    const double targetAspect = static_cast<double>(targetSize.width) / targetSize.height;
    const bool fitByWidth = mode == DisplayMode::Fit ? sourceAspect > targetAspect : sourceAspect < targetAspect;

    int width = targetSize.width;
    int height = targetSize.height;
    if (fitByWidth) {
        height = std::max(1, static_cast<int>(std::round(targetSize.width / sourceAspect)));
    } else {
        width = std::max(1, static_cast<int>(std::round(targetSize.height * sourceAspect)));
    }

    const int x = (targetSize.width - width) / 2;
    const int y = (targetSize.height - height) / 2;
    return cv::Rect(x, y, width, height);
}

} // namespace

DisplayArea resolveDisplayArea(const std::string& windowName, cv::Size fallbackSize, bool forceFallbackSize)
{
    const cv::Rect imageRect = cv::getWindowImageRect(windowName);
    if (forceFallbackSize) {
        return DisplayArea {imageRect, fallbackSize, true};
    }

    const bool hasReliableWindowRect = imageRect.width >= MinimumReliableWindowExtent
        && imageRect.height >= MinimumReliableWindowExtent;

    if (hasReliableWindowRect) {
        return DisplayArea {imageRect, imageRect.size(), false};
    }

    return DisplayArea {imageRect, fallbackSize, true};
}

DisplayRenderResult renderForDisplay(const cv::Mat& source, cv::Size targetSize, DisplayMode mode)
{
    if (source.empty() || targetSize.width <= 0 || targetSize.height <= 0) {
        return DisplayRenderResult {source, targetSize, cv::Rect()};
    }

    const cv::Rect targetRect = calculateTargetRect(source.size(), targetSize, mode);

    if (mode == DisplayMode::Stretch) {
        cv::Mat stretched;
        cv::resize(source, stretched, targetSize, 0.0, 0.0, cv::INTER_LINEAR);
        return DisplayRenderResult {stretched, targetSize, targetRect};
    }

    cv::Mat resized;
    cv::resize(source, resized, targetRect.size(), 0.0, 0.0, cv::INTER_LINEAR);

    cv::Mat canvas(targetSize, source.type(), cv::Scalar(0, 0, 0));
    cv::Rect visibleCanvasRect(0, 0, targetSize.width, targetSize.height);
    cv::Rect visibleTargetRect = targetRect & visibleCanvasRect;

    if (visibleTargetRect.empty()) {
        return DisplayRenderResult {canvas, targetSize, targetRect};
    }

    cv::Rect sourceRect(
        visibleTargetRect.x - targetRect.x,
        visibleTargetRect.y - targetRect.y,
        visibleTargetRect.width,
        visibleTargetRect.height
    );
    resized(sourceRect).copyTo(canvas(visibleTargetRect));
    return DisplayRenderResult {canvas, targetSize, targetRect};
}
