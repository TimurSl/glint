#pragma once
#include "muxer.h"

class AvMuxer : public IMuxer {
public:
    bool open(const MuxerConfig& cfg) override;
    bool writeVideo(const uint8_t* data, size_t sz, int64_t pts, bool key) override;
    bool writeAudioSys(const uint8_t* data, size_t sz, int64_t pts) override;
    bool writeAudioMic(const uint8_t* data, size_t sz, int64_t pts) override;
    bool close() override;
};
