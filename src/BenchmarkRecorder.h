#pragma once

#include <chrono>
#include <cstddef>
#include <string>

enum class BenchmarkStage {
    Decode,
    Resize,
    Mask,
    MaskUpscale,
    Overlay,
    Display,
    Total,
    Count
};

class BenchmarkRecorder {
public:
    explicit BenchmarkRecorder(bool enabled);

    bool enabled() const;
    void add(BenchmarkStage stage, std::chrono::steady_clock::duration duration);
    void frameCompleted();
    void maybeLogProgress();
    void logSummary() const;

private:
    struct StageStats {
        std::chrono::steady_clock::duration total {};
    };

    double averageMilliseconds(BenchmarkStage stage) const;
    double totalAverageMilliseconds() const;
    double percentOfTotal(BenchmarkStage stage) const;
    std::chrono::steady_clock::duration measuredStageTotal() const;
    std::chrono::steady_clock::duration otherTotal() const;

    bool enabled_ = false;
    std::size_t frames_ = 0;
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
