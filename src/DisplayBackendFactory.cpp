#include "DisplayBackendFactory.h"

#include "DrmKmsDisplayBackend.h"
#include "OpenCvDisplayBackend.h"

#include "Logger.h"

std::unique_ptr<IDisplayBackend> DisplayBackendFactory::create(DisplayBackendType backend)
{
    switch (backend) {
    case DisplayBackendType::HighGui:
        return std::make_unique<OpenCvDisplayBackend>();
    case DisplayBackendType::Drm:
#if defined(JON_ENABLE_DRM_DISPLAY)
        return std::make_unique<DrmKmsDisplayBackend>();
#else
        LOG_ERROR("DRM display backend was not enabled at build time");
        return nullptr;
#endif
    }

    return nullptr;
}
