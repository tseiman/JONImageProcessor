#include "MaskBackendFactory.h"

#include "MaskBackends.h"

std::unique_ptr<IMaskBackend> MaskBackendFactory::create(MaskBackendType type)
{
    switch (type) {
    case MaskBackendType::None:
        return std::make_unique<NoMaskBackend>();
    case MaskBackendType::Dummy:
        return std::make_unique<DummyMaskBackend>();
    case MaskBackendType::Jetson:
        return std::make_unique<JetsonSegmentationMaskBackend>();
    }

    return nullptr;
}
