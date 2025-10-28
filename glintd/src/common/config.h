#pragma once
#include <string>

struct Config {
    double pre_seconds = 20.0;
    double post_seconds = 10.0;

    // Video
    int    fps = 60;
    int    width = 0;       // 0 => auto (native)
    int    height = 0;      // 0 => auto (native)
    std::string vcodec = "h264_nvenc"; // fallback: "libx264"
    int    video_bitrate_kbps = 20000;
    bool   record_cursor = true;

    // Audio
    bool   audio_system = true;
    bool   audio_mic = true;
    std::string acodec = "aac"; // fallback: "libmp3lame"
    int    audio_bitrate_kbps = 192;
    int    audio_sample_rate = 48000;
    int    audio_channels = 2;

    // Output
    std::string container = "matroska"; // "mp4" тоже ок, но mkv удобнее
    std::string output_dir = "captures";

    // Rolling buffer
    bool   rolling_enabled = false;
    int    segment_ms = 2000;
    int    retain_seconds = 120;  // по времени
    int    retain_size_mb = 2048; // по размеру
    std::string buffer_dir = "buffer";
};

Config load_default_config();
