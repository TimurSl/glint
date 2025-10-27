#pragma once
#include <atomic>
#include <string>

class ReplayBuffer {
public:
    bool start_session(const std::string& game);
    void stop_session();
    bool export_last_clip(const std::string& path);
    bool is_running() const;
private:
    std::atomic<bool> running_{false};
    std::string current_game_;
};
