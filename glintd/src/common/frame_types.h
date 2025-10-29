#pragma once

#include <cstdint>
#include <vector>

struct VideoFrame {
    int width{};
    int height{};
    int stride{};
    uint64_t pts_ms{};
    std::vector<uint8_t> data; // RGBA
};

struct AudioFrame {
    int sample_rate{};
    int channels{};
    int samples{};
    uint64_t pts_ms{};
    std::vector<float> interleaved; // float PCM
};
