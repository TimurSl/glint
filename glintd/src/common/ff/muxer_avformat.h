#pragma once

extern "C" {
#include <libavformat/avformat.h>
}

#include "muxer.h"

class AvMuxer : public IMuxer {
public:
    AvMuxer();
    ~AvMuxer() override;

    bool open(const MuxerConfig& cfg,
              const EncoderStreamInfo& video,
              const EncoderStreamInfo& systemAudio,
              const EncoderStreamInfo& micAudio) override;
    bool write(const EncodedPacket& packet) override;
    bool close() override;

private:
    bool addStream(const EncoderStreamInfo& info, int& index_out);
    AVStream* streamForType(EncodedStreamType type) const;

    AVFormatContext* ctx_{nullptr};
    MuxerConfig config_;
    int video_stream_{-1};
    int system_stream_{-1};
    int mic_stream_{-1};
};