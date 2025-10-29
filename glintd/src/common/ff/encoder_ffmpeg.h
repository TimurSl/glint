#pragma once

#include <memory>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/channel_layout.h>
#include <libavutil/version.h>
#include <libavutil/frame.h>
#include <libavutil/opt.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
#include <libavutil/audio_fifo.h>
}

#include "encoder.h"

class FFmpegEncoder : public IEncoder {
public:
    FFmpegEncoder();
    ~FFmpegEncoder() override;

    bool initVideo(const std::string& codec, int w, int h, int fps, int br_kbps) override;
    bool initAudio(const std::string& codec, int sr, int ch, int br_kbps, bool mic) override;
    bool open() override;
    bool pushVideoRGBA(const uint8_t* rgba, int w, int h, int stride, uint64_t pts_ms) override;
    bool pushAudioF32(const float* interleaved, int samples, int sr, int ch, uint64_t pts_ms, bool mic) override;
    bool pull(std::vector<EncodedPacket>& out) override;
    void flush(std::vector<EncodedPacket>& out) override;
    void close() override;
    EncoderStreamInfo videoStream() const override;
    EncoderStreamInfo audioStream(bool mic) const override;

private:
    struct AudioEncoderState {
        AVCodecContext* ctx{nullptr};
        SwrContext* resampler{nullptr};
        AVFrame* frame{nullptr};
        AVAudioFifo* fifo{nullptr};
        int frame_samples{0};
        int input_channels{0};
        int input_sample_rate{0};
        std::string codec_name;
        bool enabled{false};
    };

    bool prepareVideoFrame(const uint8_t* rgba, int w, int h, int stride, uint64_t pts_ms);
    bool encodeFrame(AVCodecContext* ctx, AVFrame* frame, EncodedStreamType type, std::vector<EncodedPacket>& out);
    bool encodeAudioSamples(AudioEncoderState& state, const float* interleaved, int samples, int sr, int ch,
                            uint64_t pts_ms, EncodedStreamType type, std::vector<EncodedPacket>& out);
    static AVCodecContext* createContext(const std::string& codecName, bool allowHw);

    AVCodecContext* video_ctx_{nullptr};
    AVFrame* video_frame_{nullptr};
    SwsContext* scaler_{nullptr};
    int video_width_{0};
    int video_height_{0};
    int video_stride_{0};
    int video_fps_{0};
    std::string video_codec_;

    AudioEncoderState system_audio_;
    AudioEncoderState mic_audio_;

    std::vector<EncodedPacket> pending_packets_;

    EncoderStreamInfo video_stream_info_;
};
