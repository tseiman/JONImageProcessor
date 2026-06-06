#pragma once

#include "CommandLineOptions.h"

#include <opencv2/core.hpp>

#include <chrono>
#include <cstddef>
#include <string>

struct MaskTimings {
    std::chrono::steady_clock::duration preprocess {};
    std::chrono::steady_clock::duration inference {};
    std::chrono::steady_clock::duration postprocess {};
};

class IMaskBackend {
public:
    virtual ~IMaskBackend() = default;

    virtual bool initialize(const ProcessorConfig& config) = 0;
    virtual bool generate(const cv::Mat& frame, std::size_t frameIndex, cv::Mat& personMask, MaskTimings& timings) = 0;
    virtual std::string name() const = 0;
};
