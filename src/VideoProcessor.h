#pragma once

#include "CommandLineOptions.h"

class VideoProcessor {
public:
    explicit VideoProcessor(ProcessorConfig config);

    int run();

private:
    ProcessorConfig config_;
};
