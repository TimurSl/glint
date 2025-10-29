#include "replay_buffer.h"

#include <filesystem>

#include "logger.h"
#include "recorder.h"

ReplayBuffer::ReplayBuffer(Options options)
    : options_(std::move(options)), rolling_enabled_(options_.rolling_buffer_enabled) {}

void ReplayBuffer::attachRecorder(Recorder* recorder) {
    recorder_ = recorder;
    if (recorder_) {
        recorder_->setRollingBufferEnabled(rolling_enabled_);
    }
}

bool ReplayBuffer::start_session(const std::string& game) {
    if (running_) return true;
    running_ = true;
    current_game_ = game;
    Logger::instance().info("ReplayBuffer: session started: " + game);
    if (recorder_) {
        recorder_->setRollingBufferEnabled(rolling_enabled_);
    }
    return true;
}

void ReplayBuffer::stop_session() {
    if (!running_) return;
    running_ = false;
    Logger::instance().info("ReplayBuffer: session stopped");
}

bool ReplayBuffer::export_last_clip(const std::filesystem::path& path) {
    if (!recorder_) {
        Logger::instance().warn("ReplayBuffer: recorder not attached");
        return false;
    }
    auto segment = recorder_->exportLastSegment(path);
    if (!segment) {
        Logger::instance().warn("ReplayBuffer: no segment to export");
        return false;
    }
    Logger::instance().info("ReplayBuffer: exported clip to " + path.string());
    return true;
}

bool ReplayBuffer::export_last_clip(const std::string& path) {
    return export_last_clip(std::filesystem::path(path));
}

bool ReplayBuffer::is_running() const { return running_.load(); }

void ReplayBuffer::setRollingBufferEnabled(bool enabled) {
    rolling_enabled_ = enabled;
    if (recorder_) {
        recorder_->setRollingBufferEnabled(enabled);
    }
}
