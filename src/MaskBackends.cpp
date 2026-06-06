#include "MaskBackends.h"

#include "Logger.h"

#include <opencv2/imgproc.hpp>

#include <algorithm>

namespace {

cv::Mat createDummyPersonMask(int width, int height, std::size_t frameIndex)
{
    cv::Mat mask(height, width, CV_8UC1, cv::Scalar(0));
    const int radius = std::max(8, std::min(width, height) / 5);
    const int usableWidth = std::max(1, width - 2 * radius);
    const int x = radius + static_cast<int>((frameIndex * 3) % usableWidth);
    const int y = height / 2;

    cv::circle(mask, cv::Point(x, y), radius, cv::Scalar(255), cv::FILLED, cv::LINE_AA);
    return mask;
}

} // namespace

bool NoMaskBackend::initialize(const ProcessorConfig& config)
{
    (void)config;
    return true;
}

bool NoMaskBackend::generate(
    const cv::Mat& frame,
    std::size_t frameIndex,
    cv::Mat& personMask,
    MaskTimings& timings)
{
    (void)frame;
    (void)frameIndex;
    timings = {};
    personMask.release();
    return true;
}

std::string NoMaskBackend::name() const
{
    return "none";
}

bool DummyMaskBackend::initialize(const ProcessorConfig& config)
{
    segmentationSize_ = cv::Size(config.segmentationWidth, config.segmentationHeight);
    LOG_VERBOSE("Dummy mask size: " << segmentationSize_.width << "x" << segmentationSize_.height);
    return true;
}

bool DummyMaskBackend::generate(
    const cv::Mat& frame,
    std::size_t frameIndex,
    cv::Mat& personMask,
    MaskTimings& timings)
{
    (void)frame;
    const auto inferenceStartedAt = std::chrono::steady_clock::now();
    personMask = createDummyPersonMask(segmentationSize_.width, segmentationSize_.height, frameIndex);
    timings.preprocess = {};
    timings.inference = std::chrono::steady_clock::now() - inferenceStartedAt;
    timings.postprocess = {};
    return true;
}

std::string DummyMaskBackend::name() const
{
    return "dummy";
}
