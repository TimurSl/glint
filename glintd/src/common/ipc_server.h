#pragma once
#include <functional>
#include <string>
#include <thread>
#include <atomic>


using IpcHandler = std::function<std::string(const std::string&)>;

class IpcServer {
public:
    virtual ~IpcServer() = default;
    virtual bool start(IpcHandler handler) = 0;
    virtual void stop() = 0;
};
