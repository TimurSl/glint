#pragma once
#include <string>
#include <cstdint>
#include <vector>

struct MuxerConfig {
    std::string container = "matroska";
    std::string path;
    int tb_ms = 1;
    bool two_audio_tracks = true; // system + mic
};

class IMuxer {
public:
    virtual ~IMuxer() = default;
    virtual bool open(const MuxerConfig& cfg) = 0;
    virtual bool writeVideo(const uint8_t* data, size_t sz, int64_t pts, bool key) = 0;
    virtual bool writeAudioSys(const uint8_t* data, size_t sz, int64_t pts) = 0;
    virtual bool writeAudioMic(const uint8_t* data, size_t sz, int64_t pts) = 0;
    virtual bool close() = 0;
};
