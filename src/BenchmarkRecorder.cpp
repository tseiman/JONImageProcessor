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
    case BenchmarkStage::CaptureWait:
        return "Capture wait";
    case BenchmarkStage::FrameHandover:
        return "Frame handover";
    case BenchmarkStage::Decode:
        return "Decode";
    case BenchmarkStage::Resize:
        return "Resize";
    case BenchmarkStage::SegmentationPreprocess:
        return "Segmentation preprocess";
    case BenchmarkStage::SegmentationInference:
        return "Segmentation inference";
    case BenchmarkStage::SegmentationPostprocess:
        return "Segmentation postprocess";
    case BenchmarkStage::MaskPostprocess:
        return "Mask postprocess";
    case BenchmarkStage::MaskUpscale:
        return "Mask upscale";
    case BenchmarkStage::Overlay:
        return "Overlay";
    case BenchmarkStage::Display:
        return "Display";
    case BenchmarkStage::ProcessingTotal:
        return "Processing total";
    case BenchmarkStage::PipelineTotal:
        return "Pipeline total";
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
    stream << std::left << std::setw(30) << (std::string(stageName(stage)) + ":")
           << formatMilliseconds(value);
    LOG_BENCH(stream.str());
}

void logDistributionLine(const std::string& label, double percent)
{
    std::ostringstream stream;
    stream << std::left << std::setw(30) << (label + ":") << formatPercent(percent);
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

void BenchmarkRecorder::setCaptureStats(
    std::size_t capturedFrames,
    std::size_t droppedFrames,
    std::chrono::steady_clock::duration elapsed)
{
    if (!enabled_) {
        return;
    }

    capturedFrames_ = capturedFrames;
    droppedFrames_ = droppedFrames;
    captureElapsed_ = elapsed;
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
        << " avg-frame=" << formatMilliseconds(pipelineAverageMilliseconds())
        << " fps=" << (1000.0 / std::max(0.001, pipelineAverageMilliseconds())));
    lastProgressLog_ = now;
}

void BenchmarkRecorder::logSummary() const
{
    if (!enabled_) {
        return;
    }

    LOG_BENCH("Frames processed: " << frames_);
    if (capturedFrames_ > 0) {
        const double captureSeconds = std::chrono::duration<double>(captureElapsed_).count();
        const double captureFps = captureSeconds > 0.0 ? static_cast<double>(capturedFrames_) / captureSeconds : 0.0;
        const double processingFps = captureSeconds > 0.0 ? static_cast<double>(frames_) / captureSeconds : 0.0;
        LOG_BENCH("Captured frames: " << capturedFrames_);
        LOG_BENCH("Processed frames: " << frames_);
        LOG_BENCH("Dropped/overwritten frames: " << droppedFrames_);
        LOG_BENCH("Capture FPS: " << captureFps);
        LOG_BENCH("Processing FPS: " << processingFps);
    }
    if (frames_ == 0) {
        return;
    }

    LOG_BENCH("Average frame time:");
    logAverageLine(BenchmarkStage::CaptureWait, averageMilliseconds(BenchmarkStage::CaptureWait));
    logAverageLine(BenchmarkStage::FrameHandover, averageMilliseconds(BenchmarkStage::FrameHandover));
    logAverageLine(BenchmarkStage::Decode, averageMilliseconds(BenchmarkStage::Decode));
    logAverageLine(BenchmarkStage::Resize, averageMilliseconds(BenchmarkStage::Resize));
    logAverageLine(BenchmarkStage::SegmentationPreprocess, averageMilliseconds(BenchmarkStage::SegmentationPreprocess));
    logAverageLine(BenchmarkStage::SegmentationInference, averageMilliseconds(BenchmarkStage::SegmentationInference));
    logAverageLine(BenchmarkStage::SegmentationPostprocess, averageMilliseconds(BenchmarkStage::SegmentationPostprocess));
    logAverageLine(BenchmarkStage::MaskPostprocess, averageMilliseconds(BenchmarkStage::MaskPostprocess));
    logAverageLine(BenchmarkStage::MaskUpscale, averageMilliseconds(BenchmarkStage::MaskUpscale));
    logAverageLine(BenchmarkStage::Overlay, averageMilliseconds(BenchmarkStage::Overlay));
    logAverageLine(BenchmarkStage::Display, averageMilliseconds(BenchmarkStage::Display));
    logAverageLine(BenchmarkStage::ProcessingTotal, averageMilliseconds(BenchmarkStage::ProcessingTotal));
    logAverageLine(BenchmarkStage::PipelineTotal, averageMilliseconds(BenchmarkStage::PipelineTotal));
    LOG_BENCH("Effective FPS: " << (1000.0 / std::max(0.001, pipelineAverageMilliseconds())));

    LOG_BENCH("Time distribution:");
    logDistributionLine("Capture wait", percentOfTotal(BenchmarkStage::CaptureWait));
    logDistributionLine("Frame handover", percentOfTotal(BenchmarkStage::FrameHandover));
    logDistributionLine("Decode", percentOfTotal(BenchmarkStage::Decode));
    logDistributionLine("Resize", percentOfTotal(BenchmarkStage::Resize));
    logDistributionLine("Segmentation preprocess", percentOfTotal(BenchmarkStage::SegmentationPreprocess));
    logDistributionLine("Segmentation inference", percentOfTotal(BenchmarkStage::SegmentationInference));
    logDistributionLine("Segmentation postprocess", percentOfTotal(BenchmarkStage::SegmentationPostprocess));
    logDistributionLine("Mask postprocess", percentOfTotal(BenchmarkStage::MaskPostprocess));
    logDistributionLine("Mask upscale", percentOfTotal(BenchmarkStage::MaskUpscale));
    logDistributionLine("Overlay", percentOfTotal(BenchmarkStage::Overlay));
    logDistributionLine("Display", percentOfTotal(BenchmarkStage::Display));
    const double totalMs = milliseconds(stages_[stageIndex(BenchmarkStage::PipelineTotal)].total);
    const double otherMs = milliseconds(otherTotal());
    logDistributionLine("Unclassified other", totalMs > 0.0 ? (otherMs / totalMs) * 100.0 : 0.0);
}

double BenchmarkRecorder::averageMilliseconds(BenchmarkStage stage) const
{
    if (frames_ == 0) {
        return 0.0;
    }

    return milliseconds(stages_[stageIndex(stage)].total) / static_cast<double>(frames_);
}

double BenchmarkRecorder::pipelineAverageMilliseconds() const
{
    return averageMilliseconds(BenchmarkStage::PipelineTotal);
}

double BenchmarkRecorder::percentOfTotal(BenchmarkStage stage) const
{
    const double totalMs = milliseconds(stages_[stageIndex(BenchmarkStage::PipelineTotal)].total);
    if (totalMs <= 0.0) {
        return 0.0;
    }

    return (milliseconds(stages_[stageIndex(stage)].total) / totalMs) * 100.0;
}

std::chrono::steady_clock::duration BenchmarkRecorder::measuredStageTotal() const
{
    std::chrono::steady_clock::duration total {};
    total += stages_[stageIndex(BenchmarkStage::CaptureWait)].total;
    total += stages_[stageIndex(BenchmarkStage::FrameHandover)].total;
    total += stages_[stageIndex(BenchmarkStage::Decode)].total;
    total += stages_[stageIndex(BenchmarkStage::Resize)].total;
    total += stages_[stageIndex(BenchmarkStage::SegmentationPreprocess)].total;
    total += stages_[stageIndex(BenchmarkStage::SegmentationInference)].total;
    total += stages_[stageIndex(BenchmarkStage::SegmentationPostprocess)].total;
    total += stages_[stageIndex(BenchmarkStage::MaskPostprocess)].total;
    total += stages_[stageIndex(BenchmarkStage::MaskUpscale)].total;
    total += stages_[stageIndex(BenchmarkStage::Overlay)].total;
    total += stages_[stageIndex(BenchmarkStage::Display)].total;
    return total;
}

std::chrono::steady_clock::duration BenchmarkRecorder::otherTotal() const
{
    const auto total = stages_[stageIndex(BenchmarkStage::PipelineTotal)].total;
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
