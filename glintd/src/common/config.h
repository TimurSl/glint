#pragma once
#include <string>
#include <cstdint>

struct VideoConfig {
    int width = 0;
    int height = 0;
    int fps = 60;
    int bitrate_kbps = 20000;
    bool use_nvenc = true;    // NVENC (Win) / VAAPI (Linux)
    std::string codec = "h264"; // "h264"|"hevc"
};

struct AudioConfig {
    int sample_rate = 48000;
    int channels = 2;
    std::string codec = "opus";
    int bitrate_kbps = 128;
    bool capture_loopback = true;
    bool capture_mic = true;
};

struct BufferRetention {
    // Лимиты роллинг буфера:
    int64_t max_total_ms = 5 * 60 * 1000;   // 5m
    uint64_t max_total_bytes = 2ull * 1024 * 1024 * 1024; // 2gb
};

struct BufferConfig {
    bool enabled = false;
    int segment_ms = 2000;
    std::string dir = "buffer";
    BufferRetention retention{};
};

struct RecordConfig {
    std::string out_dir = "recordings";
    std::string container = "mkv";
};

struct Config {
    VideoConfig video{};
    AudioConfig audio{};
    BufferConfig buffer{};
    RecordConfig record{};


    std::string db_path = "glint.db";
};

Config load_default_config();
