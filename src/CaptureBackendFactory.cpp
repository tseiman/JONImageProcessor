#include "CaptureBackendFactory.h"

#include "OpenCvCaptureBackends.h"
#include "V4L2CameraCaptureBackend.h"

std::unique_ptr<ICaptureBackend> CaptureBackendFactory::create(const ProcessorConfig& config)
{
    if (!config.inputPath.empty()) {
        return std::make_unique<OpenCvFileCaptureBackend>();
    }

    return std::make_unique<V4L2CameraCaptureBackend>();
}
