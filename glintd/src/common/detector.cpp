#include "detector.h"
#include "logger.h"
#include <chrono>
#include <thread>

void Detector::start(OnStart on_start, OnStop on_stop) {
    if (running_) return;
    running_ = true;
    worker_ = std::thread([=]{
        Logger::instance().info("Detector started (stub)");
        std::this_thread::sleep_for(std::chrono::seconds(1));
        if (!running_) return;
        on_start("FakeGame");
        std::this_thread::sleep_for(std::chrono::seconds(10));
        if (!running_) return;
        on_stop();
    });
}

void Detector::stop() {
    if (!running_) return;
    running_ = false;
    if (worker_.joinable()) worker_.join();
    Logger::instance().info("Detector stopped");
}
