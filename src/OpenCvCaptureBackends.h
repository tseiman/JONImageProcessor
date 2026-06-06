#pragma once

#include "ICaptureBackend.h"

#include <opencv2/videoio.hpp>

class OpenCvFileCaptureBackend : public ICaptureBackend {
public:
    bool open(const ProcessorConfig& config) override;
    bool read(cv::Mat& frame) override;
    double fps() const override;
    void interrupt() override;
    void close() override;
    std::string name() const override;
    bool isCamera() const override;

private:
    cv::VideoCapture capture_;
};

class OpenCvCameraCaptureBackend : public ICaptureBackend {
public:
    bool open(const ProcessorConfig& config) override;
    bool read(cv::Mat& frame) override;
    double fps() const override;
    void interrupt() override;
    void close() override;
    std::string name() const override;
    bool isCamera() const override;

private:
    cv::VideoCapture capture_;
};
