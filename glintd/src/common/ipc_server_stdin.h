#pragma once
#include "ipc_server.h"

class StdinIpcServer : public IpcServer {
public:
    bool start(IpcHandler handler) override;
    void stop() override;
private:
    std::thread worker_;
    std::atomic<bool> running_{false};
};
