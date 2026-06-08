#include "ShutdownSignal.h"

#include <csignal>

namespace {

volatile std::sig_atomic_t ShutdownRequested = 0;

void handleShutdownSignal(int)
{
    ShutdownRequested = 1;
}

} // namespace

void installShutdownSignalHandlers()
{
    std::signal(SIGINT, handleShutdownSignal);
    std::signal(SIGTERM, handleShutdownSignal);
}

bool shutdownRequested()
{
    return ShutdownRequested != 0;
}
