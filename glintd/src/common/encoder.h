#pragma once
#include <string>
#include <cstdint>
#include <vector>

struct EncodedPacket {
    bool is_video;
    bool keyframe;
    std::vector<uint8_t> data;
    int64_t pts; // in stream timebase (e.g. 1/1000)
};

class IEncoder {
public:
    virtual ~IEncoder() = default;
    virtual bool initVideo(const std::string& codec, int w, int h, int fps, int bitrate_kbps) = 0;
    virtual bool initAudio(const std::string& codec, int sr, int ch, int bitrate_kbps) = 0;
    virtual bool open() = 0;
    virtual bool pushVideoRGBA(const uint8_t* rgba, int w, int h, int stride, uint64_t pts_ms) = 0;
    virtual bool pushAudioF32(const float* interleaved, int samples, int sr, int ch, uint64_t pts_ms, bool isMic) = 0;
    virtual bool pull(std::vector<EncodedPacket>& out) = 0;
    virtual void close() = 0;
};
