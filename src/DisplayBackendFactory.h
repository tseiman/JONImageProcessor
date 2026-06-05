#pragma once

#include "CommandLineOptions.h"
#include "IDisplayBackend.h"

#include <memory>

class DisplayBackendFactory {
public:
    static std::unique_ptr<IDisplayBackend> create(DisplayBackendType backend);
};
