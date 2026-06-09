#pragma once

#include "CommandLineOptions.h"

#include <string>

struct ConfigLoadResult {
    bool loaded = false;
    bool displayConfigured = false;
};

bool loadJsonConfigFile(const std::string& path, ProcessorConfig& config, ConfigLoadResult& result, std::string& error);
std::string findDefaultConfigPath(const char* argv0);
