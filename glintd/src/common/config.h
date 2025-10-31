#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <atomic>
#include <vector>

struct VideoSettings {
    int width{1920};
    int height{1080};
    int fps{60};
    int bitrate_kbps{18000};
    std::string codec{"h264"};
    std::string encoder{"software"}; // "auto" | "nvenc" | "vaapi" | "software"
};

struct AudioSettings {
    int sample_rate{48000};
    int channels{2};
    std::string codec{"aac"};
    int bitrate_kbps{192};
    bool enable_system{true};
    bool enable_microphone{true};
    bool enable_applications{false};
};

struct BufferSettings {
    bool enabled{true};
    bool rolling_mode{false};
    uint64_t size_limit_bytes{100ull * 1024ull * 1024ull};
    std::filesystem::path segment_directory{"buffer"};
    std::filesystem::path output_directory{"recordings"};
    std::string segment_prefix{"seg_"};
    std::string segment_extension{".mkv"};
    std::string container{"matroska"};
};

struct GeneralSettings {
    std::filesystem::path temp_path{"temp"};
    std::filesystem::path db_path{"glintd.db"};
    std::filesystem::path log_path{"glintd.log"};
    bool file_logging{true};
    std::string log_level{"info"};
};

struct ProfileConfig {
    VideoSettings video{};
    AudioSettings audio{};
    BufferSettings buffer{};
};

struct AppConfig {
    std::string active_profile{"default"};
    std::map<std::string, ProfileConfig> profiles{};
    GeneralSettings general{};

    const ProfileConfig& activeProfile() const;
};

AppConfig load_config(const std::filesystem::path& path);
void save_config(const std::filesystem::path& path, const AppConfig& config);

class ConfigHotReloader {
public:
    using Callback = std::function<void(const AppConfig&)>;

    ConfigHotReloader(std::filesystem::path path, AppConfig initial, Callback callback);
    ~ConfigHotReloader();

    void start();
    void stop();

    [[nodiscard]] AppConfig current() const;

private:
    void watchLoop();
    void reloadIfNeeded();

    std::filesystem::path path_;
    Callback callback_;
    mutable std::mutex mutex_;
    AppConfig current_{};
    std::string serialized_{};
    std::optional<std::filesystem::file_time_type> last_write_{};
    std::atomic<bool> running_{false};
    std::thread worker_{};
};