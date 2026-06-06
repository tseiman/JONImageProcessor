#include "CaptureBackendFactory.h"

#include "OpenCvCaptureBackends.h"
#include "V4L2CameraCaptureBackend.h"

std::unique_ptr<ICaptureBackend> CaptureBackendFactory::create(const ProcessorConfig& config)
{
    if (!config.inputPath.empty()) {
        return std::make_unique<OpenCvFileCaptureBackend>();
    }

    switch (config.captureBackend) {
    case CaptureBackendType::OpenCv:
        return std::make_unique<OpenCvCameraCaptureBackend>();
    case CaptureBackendType::V4L2:
        return std::make_unique<V4L2CameraCaptureBackend>();
    }

    return nullptr;
}
