#pragma once

#include <atomic>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "recorder.h"

class ReplayBuffer {
public:
    struct Options {
        bool buffer_enabled{true};
        bool rolling_mode{true};
        uint64_t rolling_size_limit_bytes{100ull * 1024ull * 1024ull};
        std::filesystem::path segment_root{"buffer"};
        std::filesystem::path output_directory{"recordings"};
        std::filesystem::path temp_directory{"temp"};
        std::string container{"matroska"};
        std::string segment_prefix{"seg_"};
        std::string segment_extension{".mkv"};
    };

    explicit ReplayBuffer(Options options = Options{});

    void attachRecorder(Recorder* recorder);
    void applyOptions(const Options& options);

    bool start_session(const std::string& game);
    void stop_session();
    bool export_last_clip(const std::filesystem::path& path);
    bool export_last_clip(const std::string& path);
    bool is_running() const;

    void setRollingBufferEnabled(bool enabled);

private:
    void onSegmentClosed(SegmentInfo& info);
    void onSegmentRemoved(const SegmentInfo& info);
    void cleanupChunks(const std::vector<SegmentInfo>& segments, const std::filesystem::path& directory);
    std::filesystem::path buildSessionDirectory(int sessionId) const;
    std::filesystem::path buildOutputPath(const std::string& game) const;

    Options options_{};
    std::atomic<bool> running_{false};
    std::string current_game_{};
    Recorder* recorder_{nullptr};
    bool rolling_enabled_{true};
    int current_session_id_{-1};
    std::filesystem::path session_directory_{};
    std::filesystem::path last_output_path_{};
    std::vector<SegmentInfo> session_segments_{};
    mutable std::mutex mutex_;
};