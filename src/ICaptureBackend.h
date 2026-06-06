#pragma once

#include "CommandLineOptions.h"

#include <opencv2/core.hpp>

#include <string>

class ICaptureBackend {
public:
    virtual ~ICaptureBackend() = default;

    virtual bool open(const ProcessorConfig& config) = 0;
    virtual bool read(cv::Mat& frame) = 0;
    virtual double fps() const = 0;
    virtual void interrupt() = 0;
    virtual void close() = 0;
    virtual std::string name() const = 0;
    virtual bool isCamera() const = 0;
};
