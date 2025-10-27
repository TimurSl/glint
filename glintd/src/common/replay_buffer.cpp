#include "replay_buffer.h"
#include "logger.h"
#include <fstream>

bool ReplayBuffer::start_session(const std::string& game) {
    if (running_) return true;
    current_game_ = game;
    running_ = true;
    Logger::instance().info("ReplayBuffer: session started: " + game);
    return true;
}

void ReplayBuffer::stop_session() {
    if (!running_) return;
    running_ = false;
    Logger::instance().info("ReplayBuffer: session stopped");
}

bool ReplayBuffer::export_last_clip(const std::string& path) {
    std::ofstream f(path);
    if (!f) return false;
    f << "FAKE CLIP for game: " << current_game_ << "\n";
    f.close();
    Logger::instance().info("ReplayBuffer: exported clip to " + path);
    return true;
}

bool ReplayBuffer::is_running() const {
    return running_.load();
}
