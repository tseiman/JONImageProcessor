#pragma once

#include "ICaptureBackend.h"

#include <cstddef>
#include <atomic>
#include <vector>

class V4L2CameraCaptureBackend : public ICaptureBackend {
public:
    V4L2CameraCaptureBackend() = default;
    ~V4L2CameraCaptureBackend() override;

    V4L2CameraCaptureBackend(const V4L2CameraCaptureBackend&) = delete;
    V4L2CameraCaptureBackend& operator=(const V4L2CameraCaptureBackend&) = delete;

    bool open(const ProcessorConfig& config) override;
    bool read(cv::Mat& frame) override;
    double fps() const override;
    void interrupt() override;
    void close() override;
    std::string name() const override;
    bool isCamera() const override;

private:
    struct Buffer {
        void* start = nullptr;
        std::size_t length = 0;
    };

    bool initializeDevice(const ProcessorConfig& config);
    bool initializeMmap();
    bool startStreaming();
    bool decodeFrame(const void* data, std::size_t bytesUsed, cv::Mat& frame);
    void cleanupBuffers();
    void logActiveFormat(const ProcessorConfig& config) const;

    int fd_ = -1;
    int width_ = 0;
    int height_ = 0;
    double fps_ = 0.0;
    unsigned int pixelFormat_ = 0;
    bool streaming_ = false;
    std::atomic<bool> stopRequested_ {false};
    std::vector<Buffer> buffers_;
};
