#pragma once

#include "CommandLineOptions.h"
#include "ICaptureBackend.h"

#include <memory>

class CaptureBackendFactory {
public:
    static std::unique_ptr<ICaptureBackend> create(const ProcessorConfig& config);
};
