#pragma once

#include "ipc/RuntimeState.h"

#include <atomic>
#include <string>
#include <thread>

class IPCServer {
public:
    IPCServer(RuntimeState& runtimeState, std::string socketPath);
    ~IPCServer();

    bool start();
    void stop();

private:
    void run();
    void handleClient(int clientFd);
    std::string handleLine(const std::string& line);

    RuntimeState& runtimeState_;
    std::string socketPath_;
    std::atomic<bool> running_ {false};
    int serverFd_ = -1;
    std::thread thread_;
};
