#pragma once
#include "encoder.h"
#include <memory>

class FFmpegEncoder : public IEncoder {
public:
    bool initVideo(const std::string& codec, int w, int h, int fps, int br_kbps) override;
    bool initAudio(const std::string& codec, int sr, int ch, int br_kbps) override;
    bool open() override;
    bool pushVideoRGBA(const uint8_t* rgba, int w, int h, int stride, uint64_t pts_ms) override;
    bool pushAudioF32(const float* interleaved, int samples, int sr, int ch, uint64_t pts_ms, bool isMic) override;
    bool pull(std::vector<EncodedPacket>& out) override;
    void close() override;
private:

};
