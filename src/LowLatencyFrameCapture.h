#pragma once

#include <opencv2/core.hpp>
#include <opencv2/videoio.hpp>

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <mutex>
#include <thread>

struct LowLatencyCaptureStats {
    std::size_t capturedFrames = 0;
    std::size_t droppedFrames = 0;
    std::chrono::steady_clock::duration elapsed {};
};

class LowLatencyFrameCapture {
public:
    LowLatencyFrameCapture() = default;
    ~LowLatencyFrameCapture();

    LowLatencyFrameCapture(const LowLatencyFrameCapture&) = delete;
    LowLatencyFrameCapture& operator=(const LowLatencyFrameCapture&) = delete;

    void start(cv::VideoCapture& capture);
    void stop();
    bool waitForLatestFrame(cv::Mat& frame);
    LowLatencyCaptureStats stats() const;

private:
    void captureLoop();

    cv::VideoCapture* capture_ = nullptr;
    mutable std::mutex mutex_;
    std::condition_variable frameAvailable_;
    std::thread thread_;
    cv::Mat latestFrame_;
    std::size_t latestSequence_ = 0;
    std::size_t deliveredSequence_ = 0;
    std::size_t capturedFrames_ = 0;
    std::size_t droppedFrames_ = 0;
    bool latestConsumed_ = true;
    bool stopRequested_ = false;
    bool stopped_ = true;
    std::chrono::steady_clock::time_point startedAt_;
    std::chrono::steady_clock::time_point stoppedAt_;
};
