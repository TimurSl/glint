#pragma once

#include "../capture_base.h"
#include "../ffmpeg_common.h"

#include <atomic>
#include <string>
#include <thread>
#include <vector>

struct FFmpegAudioCaptureOptions {
    std::string input_format;
    std::vector<std::string> device_candidates;
    int sample_rate{48000};
    int channels{2};
    bool is_microphone{true};
    std::string log_prefix;
};

class FFmpegAudioCapture : public IAudioCapture {
public:
    explicit FFmpegAudioCapture(FFmpegAudioCaptureOptions options);
    ~FFmpegAudioCapture() override;

    FFmpegAudioCapture(const FFmpegAudioCapture&) = delete;
    FFmpegAudioCapture& operator=(const FFmpegAudioCapture&) = delete;

    bool start(AudioCallback cb) override;
    void stop() override;

private:
    bool openWithFallback();
    bool openDevice(const std::string& device);
    void captureLoop(AudioCallback cb);
    void closeDevice();
    void resetState();

    FFmpegAudioCaptureOptions options_;
    std::atomic<bool> running_{false};
    std::thread worker_{};

    AVFormatContext* format_ctx_{nullptr};
    AVCodecContext* codec_ctx_{nullptr};
    AVPacket* packet_{nullptr};
    AVFrame* frame_{nullptr};
    SwrContext* swr_ctx_{nullptr};
#if LIBAVUTIL_VERSION_MAJOR >= 57
    AVChannelLayout out_layout_{};
#endif
    int audio_stream_index_{-1};
    std::string active_device_{};
    std::vector<float> buffer_{};
    int64_t samples_captured_{0};
};