#pragma once

#include "CommandLineOptions.h"

#include <mutex>

struct BenchmarkSnapshot {
    std::size_t framesProcessed = 0;
    double fps = 0.0;
    double avgFrameMs = 0.0;
    double processingTotalMs = 0.0;
    double pipelineTotalMs = 0.0;
};

class RuntimeState {
public:
    explicit RuntimeState(ProcessorConfig config)
        : config_(std::move(config))
    {
    }

    ProcessorConfig configSnapshot() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return config_;
    }

    void updateConfig(const ProcessorConfig& config)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        config_ = config;
    }

    BenchmarkSnapshot benchmarkSnapshot() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return benchmark_;
    }

    void updateBenchmark(const BenchmarkSnapshot& benchmark)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        benchmark_ = benchmark;
    }

private:
    mutable std::mutex mutex_;
    ProcessorConfig config_;
    BenchmarkSnapshot benchmark_;
};
