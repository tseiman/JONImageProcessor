#include "DisplayRenderer.h"

#include <opencv2/highgui.hpp>
#include <opencv2/imgproc.hpp>

#include <algorithm>
#include <cmath>

namespace {

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

cv::Mat renderForDisplay(const cv::Mat& source, cv::Size targetSize, DisplayMode mode)
{
    if (source.empty() || targetSize.width <= 0 || targetSize.height <= 0) {
        return source;
    }

    if (mode == DisplayMode::Stretch) {
        cv::Mat stretched;
        cv::resize(source, stretched, targetSize, 0.0, 0.0, cv::INTER_LINEAR);
        return stretched;
    }

    const cv::Rect targetRect = calculateTargetRect(source.size(), targetSize, mode);
    cv::Mat resized;
    cv::resize(source, resized, targetRect.size(), 0.0, 0.0, cv::INTER_LINEAR);

    cv::Mat canvas(targetSize, source.type(), cv::Scalar(0, 0, 0));
    cv::Rect visibleCanvasRect(0, 0, targetSize.width, targetSize.height);
    cv::Rect visibleTargetRect = targetRect & visibleCanvasRect;

    if (visibleTargetRect.empty()) {
        return canvas;
    }

    cv::Rect sourceRect(
        visibleTargetRect.x - targetRect.x,
        visibleTargetRect.y - targetRect.y,
        visibleTargetRect.width,
        visibleTargetRect.height
    );
    resized(sourceRect).copyTo(canvas(visibleTargetRect));
    return canvas;
}

cv::Size getWindowDisplaySize(const std::string& windowName, cv::Size fallbackSize)
{
    const cv::Rect imageRect = cv::getWindowImageRect(windowName);
    if (imageRect.width > 0 && imageRect.height > 0) {
        return imageRect.size();
    }

    return fallbackSize;
}
