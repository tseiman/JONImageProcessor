#include "MaskBackends.h"

#include "Logger.h"

#include <opencv2/imgproc.hpp>

#include <cstring>
#include <vector>

#ifdef JON_WITH_JETSON_INFERENCE
#include <cuda_runtime.h>
#include <jetson-utils/cudaMappedMemory.h>
#include <jetson-inference/segNet.h>
#include <jetson-utils/imageFormat.h>
#include <jetson-utils/logging.h>
#endif

struct JetsonSegmentationMaskBackend::Impl {
#ifdef JON_WITH_JETSON_INFERENCE
    segNet* network = nullptr;
    unsigned char* inputCpu = nullptr;
    unsigned char* inputGpu = nullptr;
    unsigned char* classMaskCpu = nullptr;
    unsigned char* classMaskGpu = nullptr;
    cv::Size segmentationSize;
    cv::Size classGridSize;
    int personClassId = -1;
#endif
};

namespace {

#ifdef JON_WITH_JETSON_INFERENCE

bool allocateMappedBuffer(unsigned char*& cpuPtr, unsigned char*& gpuPtr, std::size_t bytes, const char* label)
{
    cpuPtr = nullptr;
    gpuPtr = nullptr;
    if (!cudaAllocMapped(reinterpret_cast<void**>(&cpuPtr), reinterpret_cast<void**>(&gpuPtr), bytes)) {
        LOG_ERROR(label << " failed");
        return false;
    }

    return true;
}

#endif

} // namespace

JetsonSegmentationMaskBackend::JetsonSegmentationMaskBackend()
    : impl_(std::make_unique<Impl>())
{
}

JetsonSegmentationMaskBackend::~JetsonSegmentationMaskBackend()
{
#ifdef JON_WITH_JETSON_INFERENCE
    if (impl_->network != nullptr) {
        delete impl_->network;
        impl_->network = nullptr;
    }
    if (impl_->inputCpu != nullptr) {
        cudaFreeHost(impl_->inputCpu);
        impl_->inputCpu = nullptr;
        impl_->inputGpu = nullptr;
    }
    if (impl_->classMaskCpu != nullptr) {
        cudaFreeHost(impl_->classMaskCpu);
        impl_->classMaskCpu = nullptr;
        impl_->classMaskGpu = nullptr;
    }
#endif
}

bool JetsonSegmentationMaskBackend::initialize(const ProcessorConfig& config)
{
#ifdef JON_WITH_JETSON_INFERENCE
    impl_->segmentationSize = cv::Size(config.segmentationWidth, config.segmentationHeight);
    LOG_INFO("Initializing Jetson segmentation backend with jetson-inference segNet");
    LOG_VERBOSE("Jetson segmentation network: fcn-resnet18-voc-320x320");
    LOG_VERBOSE("Jetson segmentation size: " << impl_->segmentationSize.width << "x" << impl_->segmentationSize.height);

    Log::SetLevel(config.verbose ? Log::INFO : Log::WARNING);

    LOG_INFO("Creating Jetson segNet model: fcn-resnet18-voc-320x320, precision FP16");
    impl_->network = segNet::Create(
        "fcn-resnet18-voc-320x320",
        DEFAULT_MAX_BATCH_SIZE,
        TYPE_FP16,
        DEVICE_GPU,
        true);
    if (impl_->network == nullptr) {
        LOG_ERROR("Failed to create jetson-inference segNet");
        return false;
    }
    LOG_INFO("Jetson segNet model created");

    impl_->personClassId = impl_->network->FindClassID("person");
    if (impl_->personClassId < 0) {
        LOG_ERROR("jetson-inference segNet model does not provide a 'person' class");
        return false;
    }

    impl_->classGridSize = cv::Size(
        static_cast<int>(impl_->network->GetGridWidth()),
        static_cast<int>(impl_->network->GetGridHeight()));

    if (impl_->classGridSize.width <= 0 || impl_->classGridSize.height <= 0) {
        LOG_ERROR("jetson-inference segNet reported an invalid class grid size: "
            << impl_->classGridSize.width << "x" << impl_->classGridSize.height);
        return false;
    }

    LOG_INFO("Jetson segmentation ready: person class ID "
        << impl_->personClassId << ", grid "
        << impl_->classGridSize.width << "x" << impl_->classGridSize.height);

    const std::size_t inputBytes = static_cast<std::size_t>(impl_->segmentationSize.width)
        * static_cast<std::size_t>(impl_->segmentationSize.height) * 3;
    const std::size_t maskBytes = static_cast<std::size_t>(impl_->classGridSize.width)
        * static_cast<std::size_t>(impl_->classGridSize.height);

    if (!allocateMappedBuffer(impl_->inputCpu, impl_->inputGpu, inputBytes, "cudaHostAlloc input")) {
        return false;
    }
    if (!allocateMappedBuffer(impl_->classMaskCpu, impl_->classMaskGpu, maskBytes, "cudaHostAlloc mask")) {
        return false;
    }

    return true;
#else
    (void)config;
    LOG_ERROR("Mask backend 'jetson' was requested, but this binary was built without jetson-inference support. Reconfigure with -DJON_ENABLE_JETSON_INFERENCE=ON on Jetson or in a matching Jetson cross-compile environment.");
    return false;
#endif
}

bool JetsonSegmentationMaskBackend::generate(
    const cv::Mat& frame,
    std::size_t frameIndex,
    cv::Mat& personMask,
    MaskTimings& timings)
{
    (void)frameIndex;
#ifdef JON_WITH_JETSON_INFERENCE
    if (impl_->network == nullptr || frame.empty()) {
        return false;
    }

    const auto preprocessStartedAt = std::chrono::steady_clock::now();
    cv::Mat resized;
    cv::resize(frame, resized, impl_->segmentationSize, 0.0, 0.0, cv::INTER_LINEAR);
    cv::Mat resizedRgb;
    cv::cvtColor(resized, resizedRgb, cv::COLOR_BGR2RGB);
    if (!resizedRgb.isContinuous()) {
        resizedRgb = resizedRgb.clone();
    }
    const std::size_t inputBytes = static_cast<std::size_t>(impl_->segmentationSize.width)
        * static_cast<std::size_t>(impl_->segmentationSize.height) * 3;
    std::memcpy(impl_->inputCpu, resizedRgb.data, inputBytes);
    timings.preprocess = std::chrono::steady_clock::now() - preprocessStartedAt;

    const auto inferenceStartedAt = std::chrono::steady_clock::now();
    if (!impl_->network->Process(
            impl_->inputGpu,
            static_cast<uint32_t>(impl_->segmentationSize.width),
            static_cast<uint32_t>(impl_->segmentationSize.height),
            IMAGE_RGB8)) {
        LOG_ERROR("jetson-inference segNet Process failed");
        return false;
    }
    timings.inference = std::chrono::steady_clock::now() - inferenceStartedAt;

    const auto postprocessStartedAt = std::chrono::steady_clock::now();
    if (!impl_->network->Mask(
            impl_->classMaskCpu,
            static_cast<uint32_t>(impl_->classGridSize.width),
            static_cast<uint32_t>(impl_->classGridSize.height))) {
        LOG_ERROR("jetson-inference segNet Mask failed");
        return false;
    }

    cv::Mat classGridMask(impl_->classGridSize, CV_8UC1);
    const int pixels = impl_->classGridSize.width * impl_->classGridSize.height;
    for (int index = 0; index < pixels; ++index) {
        classGridMask.data[index] = impl_->classMaskCpu[index] == impl_->personClassId ? 255 : 0;
    }

    if (classGridMask.size() == impl_->segmentationSize) {
        personMask = classGridMask;
    } else {
        cv::resize(classGridMask, personMask, impl_->segmentationSize, 0.0, 0.0, cv::INTER_LINEAR);
    }
    timings.postprocess = std::chrono::steady_clock::now() - postprocessStartedAt;
    return true;
#else
    (void)frame;
    (void)personMask;
    (void)timings;
    return false;
#endif
}

std::string JetsonSegmentationMaskBackend::name() const
{
    return "jetson";
}
