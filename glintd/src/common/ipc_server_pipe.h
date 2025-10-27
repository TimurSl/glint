#pragma once
#include "ipc_server.h"
#include <string>
#include <thread>
#include <atomic>

class IpcServerPipe : public IpcServer {
public:
    explicit IpcServerPipe(std::string endpoint) : endpoint_(std::move(endpoint)) {}
    bool start(IpcHandler handler) override;
    void stop() override;

private:
    void run(IpcHandler handler);
    std::string endpoint_;
    std::thread worker_;
    std::atomic<bool> running_{false};
};
