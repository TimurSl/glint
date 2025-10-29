#pragma once

#include <cstdint>
#include <string>
#include <vector>

enum class EncodedStreamType {
    Video,
    SystemAudio,
    MicrophoneAudio
};

struct EncodedPacket {
    EncodedStreamType type{EncodedStreamType::Video};
    bool keyframe{false};
    std::vector<uint8_t> data;
    int64_t pts{0}; // milliseconds
};

struct EncoderStreamInfo {
    EncodedStreamType type{EncodedStreamType::Video};
    std::string codec_name;
    int timebase_num{1};
    int timebase_den{1000};
    int width{0};
    int height{0};
    int fps{0};
    int sample_rate{0};
    int channels{0};
    std::vector<uint8_t> extradata;
};

class IEncoder {
public:
    virtual ~IEncoder() = default;
    virtual bool initVideo(const std::string& codec, int w, int h, int fps, int bitrate_kbps) = 0;
    virtual bool initAudio(const std::string& codec, int sr, int ch, int bitrate_kbps, bool mic) = 0;
    virtual bool open() = 0;
    virtual bool pushVideoRGBA(const uint8_t* rgba, int w, int h, int stride, uint64_t pts_ms) = 0;
    virtual bool pushAudioF32(const float* interleaved, int samples, int sr, int ch, uint64_t pts_ms, bool mic) = 0;
    virtual bool pull(std::vector<EncodedPacket>& out) = 0;
    virtual void flush(std::vector<EncodedPacket>& out) = 0;
    virtual void close() = 0;
    virtual EncoderStreamInfo videoStream() const = 0;
    virtual EncoderStreamInfo audioStream(bool mic) const = 0;
};
