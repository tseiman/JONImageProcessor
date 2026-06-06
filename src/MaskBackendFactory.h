#pragma once

#include "IMaskBackend.h"

#include <memory>

class MaskBackendFactory {
public:
    static std::unique_ptr<IMaskBackend> create(MaskBackendType type);
};
