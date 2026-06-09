#pragma once

#include "IMaskBackend.h"

#include <memory>

class TensorRtMaskBackend : public IMaskBackend {
public:
    TensorRtMaskBackend();
    ~TensorRtMaskBackend() override;

    TensorRtMaskBackend(const TensorRtMaskBackend&) = delete;
    TensorRtMaskBackend& operator=(const TensorRtMaskBackend&) = delete;

    bool initialize(const ProcessorConfig& config) override;
    void updateConfig(const ProcessorConfig& config) override;
    bool generate(const cv::Mat& frame, std::size_t frameIndex, cv::Mat& personMask, MaskTimings& timings) override;
    std::string name() const override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
