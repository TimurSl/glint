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
    struct StreamClock {
        int64_t base_ms = -1;
        int64_t last_ms = -1;
    };
    StreamClock vclk_, a1clk_, a2clk_;
    StreamClock& clkFor(EncodedStreamType type);

    bool addStream(const EncoderStreamInfo& info, int& index_out);
    AVStream* streamForType(EncodedStreamType type) const;

    static std::vector<uint8_t> extractH264ExtradataFromAnnexB(const uint8_t* data, int size);

    AVFormatContext* ctx_{nullptr};
    MuxerConfig config_;
    int video_stream_{-1};
    int system_stream_{-1};
    int mic_stream_{-1};

    bool header_written_{false};
    bool header_tried_without_extradata_{false};
};