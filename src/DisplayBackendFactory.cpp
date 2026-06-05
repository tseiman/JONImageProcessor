#include "DisplayBackendFactory.h"

#include "OpenCvDisplayBackend.h"

std::unique_ptr<IDisplayBackend> DisplayBackendFactory::create(DisplayBackendType backend)
{
    switch (backend) {
    case DisplayBackendType::HighGui:
        return std::make_unique<OpenCvDisplayBackend>();
    }

    return nullptr;
}
