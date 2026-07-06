#include "BenchmarkRecorder.h"

#include "Logger.h"

#include <algorithm>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <sys/resource.h>
#include <unistd.h>

namespace {

constexpr std::chrono::seconds ProgressInterval(1);
constexpr std::chrono::seconds SnapshotInterval(1);

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
    case BenchmarkStage::BackgroundBlur:
        return "Background blur";
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

double secondsFromTimeval(const timeval& value)
{
    return static_cast<double>(value.tv_sec) + static_cast<double>(value.tv_usec) / 1000000.0;
}

double currentProcessCpuSeconds(rusage* outUsage = nullptr)
{
    rusage usage {};
    if (::getrusage(RUSAGE_SELF, &usage) != 0) {
        return 0.0;
    }
    if (outUsage) {
        *outUsage = usage;
    }
    return secondsFromTimeval(usage.ru_utime) + secondsFromTimeval(usage.ru_stime);
}

std::size_t currentResidentSetBytes()
{
    std::ifstream statm("/proc/self/statm");
    std::uint64_t totalPages = 0;
    std::uint64_t residentPages = 0;
    if (!(statm >> totalPages >> residentPages)) {
        return 0;
    }

    const long pageSize = ::sysconf(_SC_PAGESIZE);
    if (pageSize <= 0) {
        return 0;
    }
    return static_cast<std::size_t>(residentPages) * static_cast<std::size_t>(pageSize);
}

std::size_t peakResidentSetBytes(const rusage& usage)
{
    // Linux reports ru_maxrss in KiB.
    return usage.ru_maxrss > 0
        ? static_cast<std::size_t>(usage.ru_maxrss) * 1024U
        : 0U;
}

} // namespace

BenchmarkRecorder::BenchmarkRecorder(bool enabled, bool logEnabled)
    : enabled_(enabled)
    , logEnabled_(logEnabled)
    , startedAt_(std::chrono::steady_clock::now())
    , lastProgressLog_(startedAt_)
    , intervalStartedAt_(startedAt_)
    , intervalStartCpuSeconds_(currentProcessCpuSeconds())
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
        const auto now = std::chrono::steady_clock::now();
        if (now - intervalStartedAt_ >= SnapshotInterval) {
            const double elapsedSeconds = std::chrono::duration<double>(now - intervalStartedAt_).count();
            const double cpuSeconds = currentProcessCpuSeconds();
            currentFps_ = elapsedSeconds > 0.0
                ? static_cast<double>(frames_ - intervalStartFrames_) / elapsedSeconds
                : 0.0;
            cpuCurrentPercent_ = elapsedSeconds > 0.0
                ? ((cpuSeconds - intervalStartCpuSeconds_) / elapsedSeconds) * 100.0
                : 0.0;
            intervalStartFrames_ = frames_;
            intervalStartedAt_ = now;
            intervalStartCpuSeconds_ = cpuSeconds;
        }
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
    if (!enabled_ || !logEnabled_ || frames_ == 0) {
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
    if (!enabled_ || !logEnabled_) {
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
    logAverageLine(BenchmarkStage::BackgroundBlur, averageMilliseconds(BenchmarkStage::BackgroundBlur));
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
    logDistributionLine("Background blur", percentOfTotal(BenchmarkStage::BackgroundBlur));
    logDistributionLine("Overlay", percentOfTotal(BenchmarkStage::Overlay));
    logDistributionLine("Display", percentOfTotal(BenchmarkStage::Display));
    const double totalMs = milliseconds(stages_[stageIndex(BenchmarkStage::PipelineTotal)].total);
    const double otherMs = milliseconds(otherTotal());
    logDistributionLine("Unclassified other", totalMs > 0.0 ? (otherMs / totalMs) * 100.0 : 0.0);
}

BenchmarkSnapshot BenchmarkRecorder::snapshot() const
{
    BenchmarkSnapshot snapshot;
    snapshot.framesProcessed = frames_;
    snapshot.avgFrameMs = pipelineAverageMilliseconds();
    snapshot.fps = snapshot.avgFrameMs > 0.0 ? 1000.0 / snapshot.avgFrameMs : 0.0;
    const auto now = std::chrono::steady_clock::now();
    const double intervalElapsedSeconds = std::chrono::duration<double>(now - intervalStartedAt_).count();
    const double intervalCpuSeconds = currentProcessCpuSeconds();
    const std::size_t intervalFrames = frames_ - intervalStartFrames_;
    snapshot.currentFps = intervalElapsedSeconds > 0.0 && intervalFrames > 0
        ? static_cast<double>(intervalFrames) / intervalElapsedSeconds
        : currentFps_;
    snapshot.processingTotalMs = averageMilliseconds(BenchmarkStage::ProcessingTotal);
    snapshot.pipelineTotalMs = averageMilliseconds(BenchmarkStage::PipelineTotal);

    rusage usage {};
    snapshot.cpuTotalSeconds = currentProcessCpuSeconds(&usage);
    if (snapshot.cpuTotalSeconds > 0.0) {
        const double elapsedSeconds = std::chrono::duration<double>(now - startedAt_).count();
        snapshot.cpuPercent = elapsedSeconds > 0.0 ? (snapshot.cpuTotalSeconds / elapsedSeconds) * 100.0 : 0.0;
        snapshot.cpuCurrentPercent = intervalElapsedSeconds > 0.0 && intervalFrames > 0
            ? ((intervalCpuSeconds - intervalStartCpuSeconds_) / intervalElapsedSeconds) * 100.0
            : cpuCurrentPercent_;
        snapshot.memoryPeakRssBytes = peakResidentSetBytes(usage);
    }
    snapshot.memoryRssBytes = currentResidentSetBytes();
    return snapshot;
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
    total += stages_[stageIndex(BenchmarkStage::BackgroundBlur)].total;
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
