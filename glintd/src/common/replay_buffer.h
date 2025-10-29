#pragma once

#include <atomic>
#include <filesystem>
#include <optional>
#include <string>

class Recorder;

class ReplayBuffer {
public:
    struct Options {
        std::filesystem::path export_directory{"exports"};
        std::filesystem::path last_clip_path{"buffer/last_clip.mkv"};
        bool rolling_buffer_enabled{true};
    };

    explicit ReplayBuffer(Options options = Options{"exports", "buffer/last_clip.mkv", true});;

    void attachRecorder(Recorder* recorder);

    bool start_session(const std::string& game);
    void stop_session();
    bool export_last_clip(const std::filesystem::path& path);
    bool export_last_clip(const std::string& path);
    bool is_running() const;

    void setRollingBufferEnabled(bool enabled);

private:
    Options options_;
    std::atomic<bool> running_{false};
    std::string current_game_;
    Recorder* recorder_{nullptr};
    bool rolling_enabled_{true};
};
