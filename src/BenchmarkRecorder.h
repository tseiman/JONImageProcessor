#pragma once

#include <chrono>
#include <cstddef>
#include <string>

#include "ipc/RuntimeState.h"

enum class BenchmarkStage {
    CaptureWait,
    FrameHandover,
    Decode,
    Resize,
    SegmentationPreprocess,
    SegmentationInference,
    SegmentationPostprocess,
    MaskPostprocess,
    MaskUpscale,
    BackgroundBlur,
    Overlay,
    Display,
    ProcessingTotal,
    PipelineTotal,
    Count
};

class BenchmarkRecorder {
public:
    explicit BenchmarkRecorder(bool enabled, bool logEnabled);

    bool enabled() const;
    void add(BenchmarkStage stage, std::chrono::steady_clock::duration duration);
    void frameCompleted();
    void setCaptureStats(
        std::size_t capturedFrames,
        std::size_t droppedFrames,
        std::chrono::steady_clock::duration elapsed);
    void maybeLogProgress();
    void logSummary() const;
    BenchmarkSnapshot snapshot() const;

private:
    struct StageStats {
        std::chrono::steady_clock::duration total {};
    };

    double averageMilliseconds(BenchmarkStage stage) const;
    double pipelineAverageMilliseconds() const;
    double percentOfTotal(BenchmarkStage stage) const;
    std::chrono::steady_clock::duration measuredStageTotal() const;
    std::chrono::steady_clock::duration otherTotal() const;

    bool enabled_ = false;
    bool logEnabled_ = false;
    std::size_t frames_ = 0;
    std::size_t capturedFrames_ = 0;
    std::size_t droppedFrames_ = 0;
    std::chrono::steady_clock::duration captureElapsed_ {};
    StageStats stages_[static_cast<std::size_t>(BenchmarkStage::Count)];
    std::chrono::steady_clock::time_point lastProgressLog_;
};

class BenchmarkScope {
public:
    BenchmarkScope(BenchmarkRecorder& recorder, BenchmarkStage stage);
    ~BenchmarkScope();

    BenchmarkScope(const BenchmarkScope&) = delete;
    BenchmarkScope& operator=(const BenchmarkScope&) = delete;

private:
    BenchmarkRecorder& recorder_;
    BenchmarkStage stage_;
    std::chrono::steady_clock::time_point startedAt_;
};
