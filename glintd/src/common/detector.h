#pragma once
#include <atomic>
#include <thread>
#include <functional>
#include <string>

class Detector {
public:
    using OnStart = std::function<void(const std::string&)>;
    using OnStop  = std::function<void()>;

    void start(OnStart on_start, OnStop on_stop);
    void stop();
private:
    std::atomic<bool> running_{false};
    std::thread worker_;
};
