#include "LowLatencyFrameCapture.h"

#include "Logger.h"

LowLatencyFrameCapture::~LowLatencyFrameCapture()
{
    stop();
}

void LowLatencyFrameCapture::start(cv::VideoCapture& capture)
{
    stop();

    {
        std::lock_guard<std::mutex> lock(mutex_);
        capture_ = &capture;
        latestFrame_.release();
        latestSequence_ = 0;
        deliveredSequence_ = 0;
        capturedFrames_ = 0;
        droppedFrames_ = 0;
        latestConsumed_ = true;
        stopRequested_ = false;
        stopped_ = false;
        startedAt_ = std::chrono::steady_clock::now();
        stoppedAt_ = startedAt_;
    }

    thread_ = std::thread(&LowLatencyFrameCapture::captureLoop, this);
    LOG_VERBOSE("Capture thread started");
}

void LowLatencyFrameCapture::stop()
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stopRequested_ = true;
    }
    frameAvailable_.notify_all();

    if (thread_.joinable()) {
        thread_.join();
    }
}

bool LowLatencyFrameCapture::waitForLatestFrame(cv::Mat& frame)
{
    std::unique_lock<std::mutex> lock(mutex_);
    frameAvailable_.wait(lock, [this] {
        return stopped_ || latestSequence_ != deliveredSequence_;
    });

    if (latestSequence_ == deliveredSequence_) {
        return false;
    }

    frame = latestFrame_.clone();
    deliveredSequence_ = latestSequence_;
    latestConsumed_ = true;
    return true;
}

LowLatencyCaptureStats LowLatencyFrameCapture::stats() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    const auto endTime = stopped_ ? stoppedAt_ : std::chrono::steady_clock::now();
    return LowLatencyCaptureStats {
        capturedFrames_,
        droppedFrames_,
        endTime - startedAt_
    };
}

void LowLatencyFrameCapture::captureLoop()
{
    while (true) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (stopRequested_) {
                break;
            }
        }

        cv::Mat frame;
        if (capture_ == nullptr || !capture_->read(frame)) {
            break;
        }

        if (frame.empty()) {
            continue;
        }

        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (stopRequested_) {
                break;
            }

            if (latestSequence_ > deliveredSequence_ && !latestConsumed_) {
                ++droppedFrames_;
            }

            latestFrame_ = frame.clone();
            ++latestSequence_;
            ++capturedFrames_;
            latestConsumed_ = false;
        }
        frameAvailable_.notify_one();
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        stopped_ = true;
        stoppedAt_ = std::chrono::steady_clock::now();
    }
    frameAvailable_.notify_all();
}
