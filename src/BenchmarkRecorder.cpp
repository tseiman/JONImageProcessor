#include "BenchmarkRecorder.h"

#include "Logger.h"

#include <algorithm>
#include <iomanip>
#include <sstream>

namespace {

constexpr std::chrono::seconds ProgressInterval(1);

std::size_t stageIndex(BenchmarkStage stage)
{
    return static_cast<std::size_t>(stage);
}

double milliseconds(std::chrono::steady_clock::duration duration)
{
    return std::chrono::duration<double, std::milli>(duration).count();
}

const char* stageName(BenchmarkStage stage)
{
    switch (stage) {
    case BenchmarkStage::Decode:
        return "Decode";
    case BenchmarkStage::Resize:
        return "Resize";
    case BenchmarkStage::Mask:
        return "Mask";
    case BenchmarkStage::MaskUpscale:
        return "Mask upscale";
    case BenchmarkStage::Overlay:
        return "Overlay";
    case BenchmarkStage::Display:
        return "Display";
    case BenchmarkStage::Total:
        return "Total frame";
    case BenchmarkStage::Count:
        break;
    }

    return "Unknown";
}

std::string formatMilliseconds(double value)
{
    std::ostringstream stream;
    stream << std::fixed << std::setprecision(2) << value << " ms";
    return stream.str();
}

std::string formatPercent(double value)
{
    std::ostringstream stream;
    stream << std::fixed << std::setprecision(1) << value << " %";
    return stream.str();
}

void logAverageLine(BenchmarkStage stage, double value)
{
    std::ostringstream stream;
    stream << std::left << std::setw(14) << (std::string(stageName(stage)) + ":")
           << formatMilliseconds(value);
    LOG_BENCH(stream.str());
}

void logDistributionLine(const std::string& label, double percent)
{
    std::ostringstream stream;
    stream << std::left << std::setw(14) << (label + ":") << formatPercent(percent);
    LOG_BENCH(stream.str());
}

} // namespace

BenchmarkRecorder::BenchmarkRecorder(bool enabled)
    : enabled_(enabled)
    , lastProgressLog_(std::chrono::steady_clock::now())
{
}

bool BenchmarkRecorder::enabled() const
{
    return enabled_;
}

void BenchmarkRecorder::add(BenchmarkStage stage, std::chrono::steady_clock::duration duration)
{
    if (!enabled_) {
        return;
    }

    stages_[stageIndex(stage)].total += duration;
}

void BenchmarkRecorder::frameCompleted()
{
    if (enabled_) {
        ++frames_;
    }
}

void BenchmarkRecorder::maybeLogProgress()
{
    if (!enabled_ || frames_ == 0) {
        return;
    }

    const auto now = std::chrono::steady_clock::now();
    if (now - lastProgressLog_ < ProgressInterval) {
        return;
    }

    LOG_BENCH("Frames processed: " << frames_
        << " avg-frame=" << formatMilliseconds(totalAverageMilliseconds())
        << " fps=" << (1000.0 / std::max(0.001, totalAverageMilliseconds())));
    lastProgressLog_ = now;
}

void BenchmarkRecorder::logSummary() const
{
    if (!enabled_) {
        return;
    }

    LOG_BENCH("Frames processed: " << frames_);
    if (frames_ == 0) {
        return;
    }

    LOG_BENCH("Average frame time:");
    logAverageLine(BenchmarkStage::Decode, averageMilliseconds(BenchmarkStage::Decode));
    logAverageLine(BenchmarkStage::Resize, averageMilliseconds(BenchmarkStage::Resize));
    logAverageLine(BenchmarkStage::Mask, averageMilliseconds(BenchmarkStage::Mask));
    logAverageLine(BenchmarkStage::MaskUpscale, averageMilliseconds(BenchmarkStage::MaskUpscale));
    logAverageLine(BenchmarkStage::Overlay, averageMilliseconds(BenchmarkStage::Overlay));
    logAverageLine(BenchmarkStage::Display, averageMilliseconds(BenchmarkStage::Display));
    LOG_BENCH("Total frame time: " << formatMilliseconds(totalAverageMilliseconds()));
    LOG_BENCH("Effective FPS: " << (1000.0 / std::max(0.001, totalAverageMilliseconds())));

    LOG_BENCH("Time distribution:");
    logDistributionLine("Decode", percentOfTotal(BenchmarkStage::Decode));
    logDistributionLine("Resize", percentOfTotal(BenchmarkStage::Resize));
    logDistributionLine("Mask", percentOfTotal(BenchmarkStage::Mask));
    logDistributionLine("Mask upscale", percentOfTotal(BenchmarkStage::MaskUpscale));
    logDistributionLine("Overlay", percentOfTotal(BenchmarkStage::Overlay));
    logDistributionLine("Display", percentOfTotal(BenchmarkStage::Display));
    const double totalMs = milliseconds(stages_[stageIndex(BenchmarkStage::Total)].total);
    const double otherMs = milliseconds(otherTotal());
    logDistributionLine("Other", totalMs > 0.0 ? (otherMs / totalMs) * 100.0 : 0.0);
}

double BenchmarkRecorder::averageMilliseconds(BenchmarkStage stage) const
{
    if (frames_ == 0) {
        return 0.0;
    }

    return milliseconds(stages_[stageIndex(stage)].total) / static_cast<double>(frames_);
}

double BenchmarkRecorder::totalAverageMilliseconds() const
{
    return averageMilliseconds(BenchmarkStage::Total);
}

double BenchmarkRecorder::percentOfTotal(BenchmarkStage stage) const
{
    const double totalMs = milliseconds(stages_[stageIndex(BenchmarkStage::Total)].total);
    if (totalMs <= 0.0) {
        return 0.0;
    }

    return (milliseconds(stages_[stageIndex(stage)].total) / totalMs) * 100.0;
}

std::chrono::steady_clock::duration BenchmarkRecorder::measuredStageTotal() const
{
    std::chrono::steady_clock::duration total {};
    total += stages_[stageIndex(BenchmarkStage::Decode)].total;
    total += stages_[stageIndex(BenchmarkStage::Resize)].total;
    total += stages_[stageIndex(BenchmarkStage::Mask)].total;
    total += stages_[stageIndex(BenchmarkStage::MaskUpscale)].total;
    total += stages_[stageIndex(BenchmarkStage::Overlay)].total;
    total += stages_[stageIndex(BenchmarkStage::Display)].total;
    return total;
}

std::chrono::steady_clock::duration BenchmarkRecorder::otherTotal() const
{
    const auto total = stages_[stageIndex(BenchmarkStage::Total)].total;
    const auto measured = measuredStageTotal();
    return total > measured ? total - measured : std::chrono::steady_clock::duration {};
}

BenchmarkScope::BenchmarkScope(BenchmarkRecorder& recorder, BenchmarkStage stage)
    : recorder_(recorder)
    , stage_(stage)
    , startedAt_(std::chrono::steady_clock::now())
{
}

BenchmarkScope::~BenchmarkScope()
{
    recorder_.add(stage_, std::chrono::steady_clock::now() - startedAt_);
}
