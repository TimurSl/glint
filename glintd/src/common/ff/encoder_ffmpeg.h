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
    struct CodecContextDeleter {
        void operator()(AVCodecContext* ctx) const noexcept { avcodec_free_context(&ctx); }
    };

    struct FrameDeleter {
        void operator()(AVFrame* frame) const noexcept { av_frame_free(&frame); }
    };

    struct SwrContextDeleter {
        void operator()(SwrContext* ctx) const noexcept { swr_free(&ctx); }
    };

    struct AudioFifoDeleter {
        void operator()(AVAudioFifo* fifo) const noexcept { av_audio_fifo_free(fifo); }
    };

    struct PacketDeleter {
        void operator()(AVPacket* pkt) const noexcept { av_packet_free(&pkt); }
    };

    using CodecContextPtr = std::unique_ptr<AVCodecContext, CodecContextDeleter>;
    using FramePtr = std::unique_ptr<AVFrame, FrameDeleter>;
    using SwrContextPtr = std::unique_ptr<SwrContext, SwrContextDeleter>;
    using AudioFifoPtr = std::unique_ptr<AVAudioFifo, AudioFifoDeleter>;
    using PacketPtr = std::unique_ptr<AVPacket, PacketDeleter>;

    class SwsContextHandle {
    public:
        SwsContextHandle() = default;
        ~SwsContextHandle();
        SwsContextHandle(const SwsContextHandle&) = delete;
        SwsContextHandle& operator=(const SwsContextHandle&) = delete;
        SwsContextHandle(SwsContextHandle&& other) noexcept;
        SwsContextHandle& operator=(SwsContextHandle&& other) noexcept;

        void reset(SwsContext* ctx = nullptr) noexcept;
        [[nodiscard]] SwsContext* get() const noexcept { return ctx_; }

    private:
        SwsContext* ctx_{nullptr};
    };

    struct AudioEncoderState {
        CodecContextPtr ctx{};
        SwrContextPtr resampler{};
        FramePtr frame{};
        AudioFifoPtr fifo{};
        int frame_samples{0};
        int input_channels{0};
        int input_sample_rate{0};
        int64_t samples_sent{0};
        std::string codec_name;
        bool enabled{false};
    };

    bool prepareVideoFrame(const uint8_t* rgba, int w, int h, int stride, uint64_t pts_ms);
    bool encodeFrame(AVCodecContext* ctx, AVFrame* frame, EncodedStreamType type, std::vector<EncodedPacket>& out);
    bool encodeAudioSamples(AudioEncoderState& state, const float* interleaved, int samples, int sr, int ch,
                            uint64_t pts_ms, EncodedStreamType type, std::vector<EncodedPacket>& out);
    static CodecContextPtr createContext(const std::string& codecName, bool allowHw);

    CodecContextPtr video_ctx_{};
    FramePtr video_frame_{};
    SwsContextHandle scaler_{};
    int video_width_{0};
    int video_height_{0};
    int video_stride_{0};
    int video_fps_{0};
    std::string video_codec_;
    int64_t last_video_pts_{GLINT_NOPTS_VALUE};

    AudioEncoderState system_audio_;
    AudioEncoderState mic_audio_;

    std::vector<EncodedPacket> pending_packets_;

    EncoderStreamInfo video_stream_info_;
};