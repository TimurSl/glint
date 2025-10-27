#include "../common/capture_base.h"
#include "../common/logger.h"
#include <thread>
#include <atomic>
#include <chrono>

class LinuxCaptureStub : public CaptureBase {
    std::atomic<bool> running_{false};
    std::thread worker_;
public:
    bool init() override {
        Logger::instance().info("[LINUX] Capture init");
        return true;
    }
    bool start() override {
        if (running_) return true;
        running_ = true;
        worker_ = std::thread([this]{
            Logger::instance().info("[LINUX] Capture started");
            while (running_) {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
            Logger::instance().info("[LINUX] Capture thread exit");
        });
        return true;
    }
    void stop() override {
        if (!running_) return;
        running_ = false;
        if (worker_.joinable()) worker_.join();
        Logger::instance().info("[LINUX] Capture stopped");
    }
};

extern "C" CaptureBase* create_capture() {
    return new LinuxCaptureStub();
}
