#include "MaskBackends.h"

#include "Logger.h"

#include <opencv2/imgproc.hpp>

#include <cstring>
#include <vector>

#ifdef JON_WITH_JETSON_INFERENCE
#include <cuda_runtime.h>
#include <jetson-inference/segNet.h>
#include <jetson-utils/imageFormat.h>
#endif

struct JetsonSegmentationMaskBackend::Impl {
#ifdef JON_WITH_JETSON_INFERENCE
    segNet* network = nullptr;
    unsigned char* inputCpu = nullptr;
    unsigned char* inputGpu = nullptr;
    unsigned char* classMaskCpu = nullptr;
    unsigned char* classMaskGpu = nullptr;
    cv::Size segmentationSize;
    int personClassId = -1;
#endif
};

namespace {

#ifdef JON_WITH_JETSON_INFERENCE

bool cudaOk(cudaError_t result, const char* operation)
{
    if (result == cudaSuccess) {
        return true;
    }

    LOG_ERROR(operation << " failed: " << cudaGetErrorString(result));
    return false;
}

bool allocateMappedBuffer(unsigned char*& cpuPtr, unsigned char*& gpuPtr, std::size_t bytes, const char* label)
{
    cpuPtr = nullptr;
    gpuPtr = nullptr;
    if (!cudaOk(cudaHostAlloc(reinterpret_cast<void**>(&cpuPtr), bytes, cudaHostAllocMapped), label)) {
        return false;
    }

    if (!cudaOk(cudaHostGetDevicePointer(reinterpret_cast<void**>(&gpuPtr), cpuPtr, 0), label)) {
        cudaFreeHost(cpuPtr);
        cpuPtr = nullptr;
        gpuPtr = nullptr;
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

    impl_->network = segNet::Create("fcn-resnet18-voc-320x320");
    if (impl_->network == nullptr) {
        LOG_ERROR("Failed to create jetson-inference segNet");
        return false;
    }

    impl_->personClassId = impl_->network->FindClassID("person");
    if (impl_->personClassId < 0) {
        LOG_ERROR("jetson-inference segNet model does not provide a 'person' class");
        return false;
    }
    LOG_VERBOSE("Jetson segmentation person class ID: " << impl_->personClassId);

    const std::size_t inputBytes = static_cast<std::size_t>(impl_->segmentationSize.width)
        * static_cast<std::size_t>(impl_->segmentationSize.height) * 3;
    const std::size_t maskBytes = static_cast<std::size_t>(impl_->segmentationSize.width)
        * static_cast<std::size_t>(impl_->segmentationSize.height);

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
    if (!resized.isContinuous()) {
        resized = resized.clone();
    }
    const std::size_t inputBytes = static_cast<std::size_t>(impl_->segmentationSize.width)
        * static_cast<std::size_t>(impl_->segmentationSize.height) * 3;
    std::memcpy(impl_->inputCpu, resized.data, inputBytes);
    timings.preprocess = std::chrono::steady_clock::now() - preprocessStartedAt;

    const auto inferenceStartedAt = std::chrono::steady_clock::now();
    if (!impl_->network->Process(
            impl_->inputGpu,
            static_cast<uint32_t>(impl_->segmentationSize.width),
            static_cast<uint32_t>(impl_->segmentationSize.height),
            IMAGE_BGR8)) {
        LOG_ERROR("jetson-inference segNet Process failed");
        return false;
    }
    timings.inference = std::chrono::steady_clock::now() - inferenceStartedAt;

    const auto postprocessStartedAt = std::chrono::steady_clock::now();
    if (!impl_->network->Mask(
            impl_->classMaskGpu,
            static_cast<uint32_t>(impl_->segmentationSize.width),
            static_cast<uint32_t>(impl_->segmentationSize.height))) {
        LOG_ERROR("jetson-inference segNet Mask failed");
        return false;
    }
    if (!cudaOk(cudaDeviceSynchronize(), "cudaDeviceSynchronize")) {
        return false;
    }

    personMask.create(impl_->segmentationSize, CV_8UC1);
    const int pixels = impl_->segmentationSize.width * impl_->segmentationSize.height;
    for (int index = 0; index < pixels; ++index) {
        personMask.data[index] = impl_->classMaskCpu[index] == impl_->personClassId ? 255 : 0;
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
