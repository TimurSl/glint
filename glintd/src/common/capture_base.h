#pragma once
#include <functional>
#include <cstdint>
#include <vector>

struct VideoFrame {
    int width, height;
    int stride;
    uint64_t pts_ms;
    std::vector<uint8_t> data; // RGBA
};

struct AudioFrame {
    int sample_rate;
    int channels;
    int samples;
    uint64_t pts_ms;
    std::vector<float> interleaved; // float PCM
};

using VideoCallback = std::function<void(const VideoFrame&)>;
using AudioCallback = std::function<void(const AudioFrame&, bool isMic)>;

class IVideoCapture {
public:
    virtual ~IVideoCapture() = default;
    virtual bool start(VideoCallback cb) = 0;
    virtual void stop() = 0;
};

class IAudioCapture {
public:
    virtual ~IAudioCapture() = default;
    virtual bool start(AudioCallback cb) = 0; // system/mic
    virtual void stop() = 0;
};
