#include "MaskBackends.h"

#include "Logger.h"

#include <opencv2/imgproc.hpp>

#include <algorithm>
#include <cstdint>
#include <cmath>
#include <cstring>
#include <fstream>
#include <memory>
#include <numeric>
#include <sstream>
#include <string>
#include <vector>

#ifdef JON_WITH_TENSORRT_MASK
#include <NvInfer.h>
#include <NvOnnxParser.h>
#include <cuda_runtime_api.h>
#endif

namespace {

#ifdef JON_WITH_TENSORRT_MASK

class TensorRtLogger final : public nvinfer1::ILogger {
public:
    void log(Severity severity, const char* message) noexcept override
    {
        if (severity <= Severity::kWARNING) {
            LOG_WARNING("TensorRT: " << message);
        }
    }
};

TensorRtLogger& tensorRtLogger()
{
    static TensorRtLogger logger;
    return logger;
}

struct CudaDeleter {
    void operator()(void* ptr) const
    {
        if (ptr != nullptr) {
            cudaFree(ptr);
        }
    }
};

template <typename T>
using TrtUniquePtr = std::unique_ptr<T>;

bool checkCuda(cudaError_t result, const char* label)
{
    if (result != cudaSuccess) {
        LOG_ERROR(label << " failed: " << cudaGetErrorString(result));
        return false;
    }
    return true;
}

std::vector<char> readBinaryFile(const std::string& path)
{
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        return {};
    }

    file.seekg(0, std::ios::end);
    const std::streamoff size = file.tellg();
    if (size <= 0) {
        return {};
    }

    std::vector<char> data(static_cast<std::size_t>(size));
    file.seekg(0, std::ios::beg);
    file.read(data.data(), size);
    return data;
}

bool writeBinaryFile(const std::string& path, const void* data, std::size_t bytes)
{
    std::ofstream file(path, std::ios::binary);
    if (!file) {
        return false;
    }

    file.write(static_cast<const char*>(data), static_cast<std::streamsize>(bytes));
    return static_cast<bool>(file);
}

bool hasSuffix(const std::string& value, const std::string& suffix)
{
    return value.size() >= suffix.size()
        && value.compare(value.size() - suffix.size(), suffix.size(), suffix) == 0;
}

std::string dimsToString(const nvinfer1::Dims& dims)
{
    std::ostringstream stream;
    for (int index = 0; index < dims.nbDims; ++index) {
        if (index > 0) {
            stream << "x";
        }
        stream << dims.d[index];
    }
    return stream.str();
}

std::string dataTypeToString(nvinfer1::DataType type)
{
    switch (type) {
    case nvinfer1::DataType::kFLOAT:
        return "float32";
    case nvinfer1::DataType::kHALF:
        return "float16";
    case nvinfer1::DataType::kINT8:
        return "int8";
    case nvinfer1::DataType::kINT32:
        return "int32";
    case nvinfer1::DataType::kBOOL:
        return "bool";
    case nvinfer1::DataType::kUINT8:
        return "uint8";
    case nvinfer1::DataType::kFP8:
        return "float8";
    case nvinfer1::DataType::kINT64:
        return "int64";
    case nvinfer1::DataType::kBF16:
        return "bfloat16";
    case nvinfer1::DataType::kINT4:
        return "int4";
    }

    return "unknown";
}

std::size_t elementCount(const nvinfer1::Dims& dims)
{
    if (dims.nbDims <= 0) {
        return 0;
    }

    std::size_t count = 1;
    for (int index = 0; index < dims.nbDims; ++index) {
        if (dims.d[index] <= 0) {
            return 0;
        }
        count *= static_cast<std::size_t>(dims.d[index]);
    }
    return count;
}

std::string firstTensorName(nvinfer1::ICudaEngine& engine, nvinfer1::TensorIOMode mode)
{
    for (int index = 0; index < engine.getNbIOTensors(); ++index) {
        const char* name = engine.getIOTensorName(index);
        if (name != nullptr && engine.getTensorIOMode(name) == mode) {
            return name;
        }
    }
    return {};
}

bool inferInputShape(const nvinfer1::Dims& dims, int& channels, int& height, int& width)
{
    if (dims.nbDims == 4) {
        channels = static_cast<int>(dims.d[1]);
        height = static_cast<int>(dims.d[2]);
        width = static_cast<int>(dims.d[3]);
        return channels == 3 && height > 0 && width > 0;
    }

    if (dims.nbDims == 3) {
        channels = static_cast<int>(dims.d[0]);
        height = static_cast<int>(dims.d[1]);
        width = static_cast<int>(dims.d[2]);
        return channels == 3 && height > 0 && width > 0;
    }

    return false;
}

nvinfer1::Dims resolveDynamicInputDims(nvinfer1::Dims dims, int targetWidth, int targetHeight)
{
    for (int index = 0; index < dims.nbDims; ++index) {
        if (dims.d[index] >= 0) {
            continue;
        }

        if (index == 0 && dims.nbDims == 4) {
            dims.d[index] = 1;
        } else if (index == dims.nbDims - 2) {
            dims.d[index] = targetHeight;
        } else if (index == dims.nbDims - 1) {
            dims.d[index] = targetWidth;
        }
    }

    return dims;
}

bool hasDynamicDimension(const nvinfer1::Dims& dims)
{
    for (int index = 0; index < dims.nbDims; ++index) {
        if (dims.d[index] < 0) {
            return true;
        }
    }
    return false;
}

std::string engineCachePath(const std::string& onnxPath, int width, int height)
{
    std::ostringstream stream;
    stream << onnxPath << "."
           << width << "x" << height
           << ".fp16.engine";
    return stream.str();
}

bool convertOutputToMask(
    const std::vector<float>& output,
    const nvinfer1::Dims& dims,
    double threshold,
    cv::Mat& personMask)
{
    int channels = 1;
    int height = 0;
    int width = 0;
    std::size_t offset = 0;

    if (dims.nbDims == 4) {
        channels = static_cast<int>(dims.d[1]);
        height = static_cast<int>(dims.d[2]);
        width = static_cast<int>(dims.d[3]);
    } else if (dims.nbDims == 3) {
        channels = static_cast<int>(dims.d[0]);
        height = static_cast<int>(dims.d[1]);
        width = static_cast<int>(dims.d[2]);
    } else if (dims.nbDims == 2) {
        height = static_cast<int>(dims.d[0]);
        width = static_cast<int>(dims.d[1]);
    } else {
        LOG_ERROR("Unsupported TensorRT mask output dimensions: " << dimsToString(dims));
        return false;
    }

    if (channels <= 0 || height <= 0 || width <= 0) {
        LOG_ERROR("Invalid TensorRT mask output dimensions: " << dimsToString(dims));
        return false;
    }

    personMask = cv::Mat(height, width, CV_8UC1, cv::Scalar(0));
    const std::size_t pixels = static_cast<std::size_t>(height) * static_cast<std::size_t>(width);

    if (channels == 1) {
        const bool looksLikeLogits = std::any_of(output.begin(), output.end(), [](float value) {
            return value < 0.0F || value > 1.0F;
        });
        for (std::size_t index = 0; index < pixels; ++index) {
            float value = output[offset + index];
            if (looksLikeLogits) {
                value = 1.0F / (1.0F + std::exp(-value));
            }
            personMask.data[index] = value >= threshold ? 255 : 0;
        }
        return true;
    }

    for (std::size_t pixel = 0; pixel < pixels; ++pixel) {
        int bestClass = 0;
        float bestValue = output[pixel];
        for (int channel = 1; channel < channels; ++channel) {
            const std::size_t index = static_cast<std::size_t>(channel) * pixels + pixel;
            if (output[index] > bestValue) {
                bestValue = output[index];
                bestClass = channel;
            }
        }

        personMask.data[pixel] = bestClass > 0 ? 255 : 0;
    }

    return true;
}

#endif

} // namespace

struct TensorRtMaskBackend::Impl {
#ifdef JON_WITH_TENSORRT_MASK
    TrtUniquePtr<nvinfer1::IRuntime> runtime;
    TrtUniquePtr<nvinfer1::ICudaEngine> engine;
    TrtUniquePtr<nvinfer1::IExecutionContext> context;
    cudaStream_t stream = nullptr;
    std::unique_ptr<void, CudaDeleter> inputDevice;
    std::unique_ptr<void, CudaDeleter> outputDevice;
    std::string inputName;
    std::string outputName;
    nvinfer1::Dims inputDims {};
    nvinfer1::Dims outputDims {};
    int inputChannels = 0;
    int inputHeight = 0;
    int inputWidth = 0;
    std::size_t inputElements = 0;
    std::size_t outputElements = 0;
    std::vector<float> inputHost;
    std::vector<float> outputHost;
    double threshold = 0.5;
#endif
};

TensorRtMaskBackend::TensorRtMaskBackend()
    : impl_(std::make_unique<Impl>())
{
}

TensorRtMaskBackend::~TensorRtMaskBackend()
{
#ifdef JON_WITH_TENSORRT_MASK
    if (impl_->stream != nullptr) {
        cudaStreamDestroy(impl_->stream);
        impl_->stream = nullptr;
    }
#endif
}

bool TensorRtMaskBackend::initialize(const ProcessorConfig& config)
{
#ifdef JON_WITH_TENSORRT_MASK
    if (config.maskModelPath.empty()) {
        LOG_ERROR("--mask-model is required for mask backend 'tensorrt'");
        return false;
    }

    LOG_INFO("Initializing TensorRT mask backend");
    LOG_INFO("TensorRT mask model: " << config.maskModelPath);
    impl_->threshold = config.maskThreshold;

    TrtUniquePtr<nvinfer1::IBuilder> builder;
    TrtUniquePtr<nvinfer1::INetworkDefinition> network;
    TrtUniquePtr<nvonnxparser::IParser> parser;
    TrtUniquePtr<nvinfer1::IBuilderConfig> builderConfig;
    std::string generatedEngineCachePath;

    if (hasSuffix(config.maskModelPath, ".engine") || hasSuffix(config.maskModelPath, ".plan")) {
        const std::vector<char> engineData = readBinaryFile(config.maskModelPath);
        if (engineData.empty()) {
            LOG_ERROR("Cannot read TensorRT engine: " << config.maskModelPath);
            return false;
        }
        impl_->runtime.reset(nvinfer1::createInferRuntime(tensorRtLogger()));
        if (!impl_->runtime) {
            LOG_ERROR("Failed to create TensorRT runtime");
            return false;
        }
        impl_->engine.reset(impl_->runtime->deserializeCudaEngine(engineData.data(), engineData.size()));
    } else if (hasSuffix(config.maskModelPath, ".onnx")) {
        generatedEngineCachePath = engineCachePath(config.maskModelPath, config.segmentationWidth, config.segmentationHeight);
        const std::vector<char> cachedEngineData = readBinaryFile(generatedEngineCachePath);
        if (!cachedEngineData.empty()) {
            LOG_INFO("Loading cached TensorRT engine: " << generatedEngineCachePath);
            impl_->runtime.reset(nvinfer1::createInferRuntime(tensorRtLogger()));
            if (!impl_->runtime) {
                LOG_ERROR("Failed to create TensorRT runtime");
                return false;
            }
            impl_->engine.reset(impl_->runtime->deserializeCudaEngine(cachedEngineData.data(), cachedEngineData.size()));
            if (!impl_->engine) {
                LOG_WARNING("Failed to deserialize cached TensorRT engine, rebuilding from ONNX");
            }
        }
    }

    if (!impl_->engine && hasSuffix(config.maskModelPath, ".onnx")) {
        LOG_INFO("No reusable TensorRT engine cache found");
        LOG_INFO("Building TensorRT engine from ONNX. This can take several minutes on the first run.");
        LOG_INFO("Engine cache will be saved to: " << generatedEngineCachePath);
        builder.reset(nvinfer1::createInferBuilder(tensorRtLogger()));
        if (!builder) {
            LOG_ERROR("Failed to create TensorRT builder");
            return false;
        }
        network.reset(builder->createNetworkV2(1U << static_cast<uint32_t>(nvinfer1::NetworkDefinitionCreationFlag::kEXPLICIT_BATCH)));
        builderConfig.reset(builder->createBuilderConfig());
        parser.reset(nvonnxparser::createParser(*network, tensorRtLogger()));
        if (!network || !builderConfig || !parser) {
            LOG_ERROR("Failed to create TensorRT ONNX parser objects");
            return false;
        }
        if (!parser->parseFromFile(config.maskModelPath.c_str(), static_cast<int>(nvinfer1::ILogger::Severity::kWARNING))) {
            LOG_ERROR("Failed to parse ONNX mask model: " << config.maskModelPath);
            return false;
        }
        LOG_INFO("TensorRT ONNX parse complete");
        for (int inputIndex = 0; inputIndex < network->getNbInputs(); ++inputIndex) {
            nvinfer1::ITensor* inputTensor = network->getInput(inputIndex);
            if (inputTensor == nullptr) {
                continue;
            }

            const nvinfer1::Dims modelDims = inputTensor->getDimensions();
            if (!hasDynamicDimension(modelDims)) {
                continue;
            }

            const nvinfer1::Dims fixedDims = resolveDynamicInputDims(
                modelDims,
                config.segmentationWidth,
                config.segmentationHeight);
            if (elementCount(fixedDims) == 0) {
                LOG_ERROR("Cannot resolve dynamic TensorRT input dimensions for "
                    << inputTensor->getName() << ": " << dimsToString(modelDims));
                return false;
            }

            nvinfer1::IOptimizationProfile* profile = builder->createOptimizationProfile();
            if (profile == nullptr
                || !profile->setDimensions(inputTensor->getName(), nvinfer1::OptProfileSelector::kMIN, fixedDims)
                || !profile->setDimensions(inputTensor->getName(), nvinfer1::OptProfileSelector::kOPT, fixedDims)
                || !profile->setDimensions(inputTensor->getName(), nvinfer1::OptProfileSelector::kMAX, fixedDims)) {
                LOG_ERROR("Failed to create TensorRT optimization profile for "
                    << inputTensor->getName() << ": " << dimsToString(fixedDims));
                return false;
            }

            builderConfig->addOptimizationProfile(profile);
            LOG_INFO("TensorRT optimization profile: " << inputTensor->getName()
                << "=" << dimsToString(fixedDims));
        }
        builderConfig->setFlag(nvinfer1::BuilderFlag::kFP16);
        LOG_INFO("TensorRT engine build started");
        TrtUniquePtr<nvinfer1::IHostMemory> serializedEngine(builder->buildSerializedNetwork(*network, *builderConfig));
        if (!serializedEngine) {
            LOG_ERROR("Failed to build serialized TensorRT engine from ONNX model");
            return false;
        }
        LOG_INFO("TensorRT engine build complete");
        if (writeBinaryFile(generatedEngineCachePath, serializedEngine->data(), serializedEngine->size())) {
            LOG_INFO("Saved TensorRT engine cache: " << generatedEngineCachePath);
        } else {
            LOG_WARNING("Failed to save TensorRT engine cache: " << generatedEngineCachePath);
        }
        impl_->runtime.reset(nvinfer1::createInferRuntime(tensorRtLogger()));
        if (!impl_->runtime) {
            LOG_ERROR("Failed to create TensorRT runtime");
            return false;
        }
        impl_->engine.reset(impl_->runtime->deserializeCudaEngine(serializedEngine->data(), serializedEngine->size()));
    } else if (!impl_->engine) {
        LOG_ERROR("Unsupported TensorRT mask model extension: " << config.maskModelPath);
        return false;
    }

    if (!impl_->engine) {
        LOG_ERROR("Failed to create TensorRT mask engine");
        return false;
    }

    impl_->context.reset(impl_->engine->createExecutionContext());
    if (!impl_->context) {
        LOG_ERROR("Failed to create TensorRT execution context");
        return false;
    }

    impl_->inputName = firstTensorName(*impl_->engine, nvinfer1::TensorIOMode::kINPUT);
    impl_->outputName = firstTensorName(*impl_->engine, nvinfer1::TensorIOMode::kOUTPUT);
    if (impl_->inputName.empty() || impl_->outputName.empty()) {
        LOG_ERROR("TensorRT mask engine must have at least one input and one output tensor");
        return false;
    }
    const nvinfer1::DataType inputType = impl_->engine->getTensorDataType(impl_->inputName.c_str());
    const nvinfer1::DataType outputType = impl_->engine->getTensorDataType(impl_->outputName.c_str());
    if (inputType != nvinfer1::DataType::kFLOAT || outputType != nvinfer1::DataType::kFLOAT) {
        LOG_ERROR("TensorRT mask backend currently requires FP32 input/output tensors. input="
            << dataTypeToString(inputType) << " output=" << dataTypeToString(outputType));
        return false;
    }

    impl_->inputDims = resolveDynamicInputDims(
        impl_->engine->getTensorShape(impl_->inputName.c_str()),
        config.segmentationWidth,
        config.segmentationHeight);

    if (!inferInputShape(impl_->inputDims, impl_->inputChannels, impl_->inputHeight, impl_->inputWidth)) {
        LOG_ERROR("Unsupported TensorRT mask input dimensions: " << dimsToString(impl_->inputDims));
        return false;
    }

    if (!impl_->context->setInputShape(impl_->inputName.c_str(), impl_->inputDims)) {
        LOG_ERROR("Failed to set TensorRT input shape: " << dimsToString(impl_->inputDims));
        return false;
    }
    impl_->outputDims = impl_->context->getTensorShape(impl_->outputName.c_str());

    impl_->inputElements = elementCount(impl_->inputDims);
    impl_->outputElements = elementCount(impl_->outputDims);
    if (impl_->inputElements == 0 || impl_->outputElements == 0) {
        LOG_ERROR("Invalid TensorRT tensor sizes. input=" << dimsToString(impl_->inputDims)
            << " output=" << dimsToString(impl_->outputDims));
        return false;
    }

    impl_->inputHost.resize(impl_->inputElements);
    impl_->outputHost.resize(impl_->outputElements);

    void* inputDevice = nullptr;
    void* outputDevice = nullptr;
    if (!checkCuda(cudaMalloc(&inputDevice, impl_->inputElements * sizeof(float)), "cudaMalloc input")) {
        return false;
    }
    if (!checkCuda(cudaMalloc(&outputDevice, impl_->outputElements * sizeof(float)), "cudaMalloc output")) {
        cudaFree(inputDevice);
        return false;
    }
    impl_->inputDevice.reset(inputDevice);
    impl_->outputDevice.reset(outputDevice);

    if (!checkCuda(cudaStreamCreate(&impl_->stream), "cudaStreamCreate")) {
        return false;
    }
    if (!impl_->context->setTensorAddress(impl_->inputName.c_str(), impl_->inputDevice.get())
        || !impl_->context->setTensorAddress(impl_->outputName.c_str(), impl_->outputDevice.get())) {
        LOG_ERROR("Failed to bind TensorRT tensor addresses");
        return false;
    }

    LOG_INFO("TensorRT mask backend ready: input " << impl_->inputName << "=" << dimsToString(impl_->inputDims)
        << ", output " << impl_->outputName << "=" << dimsToString(impl_->outputDims));
    return true;
#else
    (void)config;
    LOG_ERROR("Mask backend 'tensorrt' was requested, but this binary was built without TensorRT mask support. Reconfigure with -DJON_ENABLE_TENSORRT_MASK=ON in a matching Jetson build or cross-build environment.");
    return false;
#endif
}

bool TensorRtMaskBackend::generate(
    const cv::Mat& frame,
    std::size_t frameIndex,
    cv::Mat& personMask,
    MaskTimings& timings)
{
    (void)frameIndex;
#ifdef JON_WITH_TENSORRT_MASK
    if (!impl_->context || frame.empty()) {
        return false;
    }

    const auto preprocessStartedAt = std::chrono::steady_clock::now();
    cv::Mat resized;
    cv::resize(frame, resized, cv::Size(impl_->inputWidth, impl_->inputHeight), 0.0, 0.0, cv::INTER_LINEAR);
    cv::Mat rgb;
    cv::cvtColor(resized, rgb, cv::COLOR_BGR2RGB);
    cv::Mat rgbFloat;
    rgb.convertTo(rgbFloat, CV_32FC3, 1.0 / 255.0);

    std::vector<cv::Mat> channels;
    cv::split(rgbFloat, channels);
    const std::size_t planeSize = static_cast<std::size_t>(impl_->inputHeight) * static_cast<std::size_t>(impl_->inputWidth);
    for (int channel = 0; channel < impl_->inputChannels; ++channel) {
        std::memcpy(
            impl_->inputHost.data() + static_cast<std::size_t>(channel) * planeSize,
            channels[channel].data,
            planeSize * sizeof(float));
    }
    timings.preprocess = std::chrono::steady_clock::now() - preprocessStartedAt;

    const auto inferenceStartedAt = std::chrono::steady_clock::now();
    if (!checkCuda(cudaMemcpyAsync(
            impl_->inputDevice.get(),
            impl_->inputHost.data(),
            impl_->inputElements * sizeof(float),
            cudaMemcpyHostToDevice,
            impl_->stream),
            "cudaMemcpyAsync input")) {
        return false;
    }
    if (!impl_->context->enqueueV3(impl_->stream)) {
        LOG_ERROR("TensorRT enqueueV3 failed");
        return false;
    }
    if (!checkCuda(cudaMemcpyAsync(
            impl_->outputHost.data(),
            impl_->outputDevice.get(),
            impl_->outputElements * sizeof(float),
            cudaMemcpyDeviceToHost,
            impl_->stream),
            "cudaMemcpyAsync output")) {
        return false;
    }
    if (!checkCuda(cudaStreamSynchronize(impl_->stream), "cudaStreamSynchronize")) {
        return false;
    }
    timings.inference = std::chrono::steady_clock::now() - inferenceStartedAt;

    const auto postprocessStartedAt = std::chrono::steady_clock::now();
    if (!convertOutputToMask(impl_->outputHost, impl_->outputDims, impl_->threshold, personMask)) {
        return false;
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

std::string TensorRtMaskBackend::name() const
{
    return "tensorrt";
}
