#include "ipc_server_stdin.h"
#include "logger.h"
#include <iostream>

bool StdinIpcServer::start(IpcHandler handler) {
    if (running_) return true;
    running_ = true;
    worker_ = std::thread([this, handler]{
        Logger::instance().info("IPC (stdin) ready. Type commands.");
        std::string line;
        while (running_ && std::getline(std::cin, line)) {
            if (line.empty()) continue;
            auto reply = handler(line);
            std::cout << reply << std::endl;
        }
    });
    return true;
}

void StdinIpcServer::stop() {
    if (!running_) return;
    running_ = false;
    if (worker_.joinable()) worker_.join();
}
