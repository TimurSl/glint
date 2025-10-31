#include "audio_capture_ffmpeg.h"
#include "../logger.h"

#include <algorithm>
#include <chrono>
#include <thread>
#include <utility>

namespace {
std::string describe_device(const std::string& device) {
    return device.empty() ? "(default)" : device;
}
}

FFmpegAudioCapture::FFmpegAudioCapture(FFmpegAudioCaptureOptions options)
    : options_(std::move(options)) {}

FFmpegAudioCapture::~FFmpegAudioCapture() {
    stop();
}

bool FFmpegAudioCapture::start(AudioCallback cb) {
    if (running_) return true;
    if (!cb) return false;

    resetState();
    if (!openWithFallback()) {
        Logger::instance().warn((options_.log_prefix.empty() ? "FFmpegAudioCapture" : options_.log_prefix) +
                                ": unable to open any audio device");
        return false;
    }

    samples_captured_ = 0;
    running_ = true;
    worker_ = std::thread([this, cb] { captureLoop(cb); });
    return true;
}

void FFmpegAudioCapture::stop() {
    bool was_running = running_.exchange(false);
    if (was_running && worker_.joinable()) {
        worker_.join();
    } else if (!was_running && worker_.joinable()) {
        worker_.join();
    }
    closeDevice();
}

void FFmpegAudioCapture::resetState() {
    closeDevice();
    samples_captured_ = 0;
}

bool FFmpegAudioCapture::openWithFallback() {
    std::vector<std::string> candidates = options_.device_candidates;
    candidates.erase(std::remove(candidates.begin(), candidates.end(), std::string{}), candidates.end());
    auto end_unique = std::unique(candidates.begin(), candidates.end());
    candidates.erase(end_unique, candidates.end());
    if (candidates.empty()) candidates.emplace_back("default");

    const std::string prefix = options_.log_prefix.empty() ? "FFmpegAudioCapture" : options_.log_prefix;
    for (size_t idx = 0; idx < candidates.size(); ++idx) {
        const std::string& candidate = candidates[idx];
        if (openDevice(candidate)) {
            active_device_ = candidate;
            Logger::instance().info(prefix + ": capturing from " + describe_device(candidate) +
                                    " via format " + options_.input_format);
            return true;
        }
        std::string message = prefix + ": failed to open audio device " + describe_device(candidate);
        if (idx + 1 < candidates.size()) message += ", trying fallback";
        Logger::instance().warn(message);
    }
    return false;
}

bool FFmpegAudioCapture::openDevice(const std::string& device) {
    closeDevice();
    ff_init();

    const AVInputFormat* input = av_find_input_format(options_.input_format.c_str());
    if (!input) {
        Logger::instance().error("FFmpegAudioCapture: unknown input format " + options_.input_format);
        return false;
    }

    AVFormatContext* ctx = nullptr;
    const char* url = device.empty() ? nullptr : device.c_str();
    int ret = avformat_open_input(&ctx, url, input, nullptr);
    if (ret < 0 || !ctx) {
        Logger::instance().warn("FFmpegAudioCapture: avformat_open_input failed: " + ff_errstr(ret));
        if (ctx) avformat_close_input(&ctx);
        return false;
    }

    format_ctx_ = ctx;
    if ((ret = avformat_find_stream_info(format_ctx_, nullptr)) < 0) {
        Logger::instance().warn("FFmpegAudioCapture: avformat_find_stream_info failed: " + ff_errstr(ret));
        closeDevice();
        return false;
    }

    audio_stream_index_ = av_find_best_stream(format_ctx_, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
    if (audio_stream_index_ < 0) {
        Logger::instance().warn("FFmpegAudioCapture: no audio stream available");
        closeDevice();
        return false;
    }

    AVStream* stream = format_ctx_->streams[audio_stream_index_];
    const AVCodecParameters* params = stream->codecpar;
    const AVCodec* codec = avcodec_find_decoder(params->codec_id);
    if (!codec) {
        Logger::instance().warn("FFmpegAudioCapture: decoder not found for stream");
        closeDevice();
        return false;
    }

    codec_ctx_ = avcodec_alloc_context3(codec);
    if (!codec_ctx_) {
        Logger::instance().warn("FFmpegAudioCapture: avcodec_alloc_context3 failed");
        closeDevice();
        return false;
    }

    if ((ret = avcodec_parameters_to_context(codec_ctx_, params)) < 0) {
        Logger::instance().warn("FFmpegAudioCapture: avcodec_parameters_to_context failed: " + ff_errstr(ret));
        closeDevice();
        return false;
    }

    if (codec_ctx_->sample_rate == 0)
        codec_ctx_->sample_rate = options_.sample_rate;

#if LIBAVUTIL_VERSION_MAJOR >= 58
    // FFmpeg 6.x+
    if (codec_ctx_->ch_layout.nb_channels == 0) {
        if (params->ch_layout.nb_channels > 0)
            av_channel_layout_copy(&codec_ctx_->ch_layout, &params->ch_layout);
        else
            av_channel_layout_default(&codec_ctx_->ch_layout, options_.channels);
    }
#else
    // Старые версии FFmpeg (до 6.0)
    if (codec_ctx_->channels == 0) {
        codec_ctx_->channels = options_.channels;
        codec_ctx_->channel_layout = av_get_default_channel_layout(options_.channels);
    }
#endif

    if ((ret = avcodec_open2(codec_ctx_, codec, nullptr)) < 0) {
        Logger::instance().warn("FFmpegAudioCapture: avcodec_open2 failed: " + ff_errstr(ret));
        closeDevice();
        return false;
    }

    packet_ = av_packet_alloc();
    frame_ = av_frame_alloc();
    if (!packet_ || !frame_) {
        Logger::instance().warn("FFmpegAudioCapture: failed allocating packet or frame");
        closeDevice();
        return false;
    }

#if LIBAVUTIL_VERSION_MAJOR >= 58
    // Новый API (6.x+)
    AVChannelLayout inLayout{};
    if (codec_ctx_->ch_layout.nb_channels > 0)
        av_channel_layout_copy(&inLayout, &codec_ctx_->ch_layout);
    else
        av_channel_layout_default(&inLayout, options_.channels);

    av_channel_layout_uninit(&out_layout_);
    av_channel_layout_default(&out_layout_, options_.channels);

    if (swr_alloc_set_opts2(&swr_ctx_, &out_layout_,
                            AV_SAMPLE_FMT_FLT, options_.sample_rate,
                            &inLayout, codec_ctx_->sample_fmt, codec_ctx_->sample_rate,
                            0, nullptr) < 0) {
        Logger::instance().warn("FFmpegAudioCapture: swr_alloc_set_opts2 failed");
        av_channel_layout_uninit(&inLayout);
        closeDevice();
        return false;
    }
    av_channel_layout_uninit(&inLayout);
#else
    // Старый API (до 6.x)
    uint64_t inLayout = codec_ctx_->channel_layout
        ? codec_ctx_->channel_layout
        : av_get_default_channel_layout(codec_ctx_->channels);
    uint64_t outLayout = av_get_default_channel_layout(options_.channels);

    swr_ctx_ = swr_alloc_set_opts(nullptr, outLayout, AV_SAMPLE_FMT_FLT, options_.sample_rate,
                                  inLayout, codec_ctx_->sample_fmt, codec_ctx_->sample_rate, 0, nullptr);
    if (!swr_ctx_) {
        Logger::instance().warn("FFmpegAudioCapture: swr_alloc_set_opts failed");
        closeDevice();
        return false;
    }
#endif

    if (!swr_ctx_ || swr_init(swr_ctx_) < 0) {
        Logger::instance().warn("FFmpegAudioCapture: swr_init failed");
        closeDevice();
        return false;
    }

    return true;
}

void FFmpegAudioCapture::captureLoop(AudioCallback cb) {
    const std::string prefix = options_.log_prefix.empty() ? "FFmpegAudioCapture" : options_.log_prefix;
    while (running_) {
        int ret = av_read_frame(format_ctx_, packet_);
        bool draining = false;

        if (ret == AVERROR(EAGAIN)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            continue;
        }
        if (ret == AVERROR_EOF) {
            draining = true;
            ret = avcodec_send_packet(codec_ctx_, nullptr);
        } else if (ret >= 0) {
            if (packet_->stream_index != audio_stream_index_) {
                av_packet_unref(packet_);
                continue;
            }
            ret = avcodec_send_packet(codec_ctx_, packet_);
        } else {
            Logger::instance().warn(prefix + ": av_read_frame failed: " + ff_errstr(ret));
            break;
        }

        if (ret < 0 && ret != AVERROR_EOF && ret != AVERROR(EAGAIN)) {
            Logger::instance().warn(prefix + ": avcodec_send_packet failed: " + ff_errstr(ret));
            av_packet_unref(packet_);
            break;
        }

        while (running_) {
            ret = avcodec_receive_frame(codec_ctx_, frame_);
            if (ret == AVERROR(EAGAIN)) break;
            if (ret == AVERROR_EOF) { draining = false; break; }
            if (ret < 0) {
                Logger::instance().warn(prefix + ": avcodec_receive_frame failed: " + ff_errstr(ret));
                running_ = false;
                break;
            }

            int max_out = swr_get_out_samples(swr_ctx_, frame_->nb_samples);
            if (max_out <= 0) { av_frame_unref(frame_); continue; }

            buffer_.resize(static_cast<size_t>(max_out) * options_.channels);
            uint8_t* out_planes[1] = { reinterpret_cast<uint8_t*>(buffer_.data()) };
            const uint8_t** in_planes = const_cast<const uint8_t**>(frame_->extended_data);
            int converted = swr_convert(swr_ctx_, out_planes, max_out, in_planes, frame_->nb_samples);
            if (converted <= 0) { av_frame_unref(frame_); continue; }

            AudioFrame out{};
            out.sample_rate = options_.sample_rate;
            out.channels = options_.channels;
            out.samples = converted;
            const int sample_rate = options_.sample_rate > 0 ? options_.sample_rate : 1;
            out.pts_ms = static_cast<uint64_t>((samples_captured_ * 1000) / sample_rate);
            samples_captured_ += converted;
            out.interleaved.assign(buffer_.begin(),
                                   buffer_.begin() + static_cast<size_t>(converted) * options_.channels);
            cb(out, options_.is_microphone);
            av_frame_unref(frame_);
        }

        av_packet_unref(packet_);
        if (draining) break;
    }

    closeDevice();
    running_ = false;
}

void FFmpegAudioCapture::closeDevice() {
    if (packet_) av_packet_free(&packet_);
    if (frame_) av_frame_free(&frame_);
    if (codec_ctx_) avcodec_free_context(&codec_ctx_);
    if (format_ctx_) avformat_close_input(&format_ctx_);
    if (swr_ctx_) swr_free(&swr_ctx_);
#if LIBAVUTIL_VERSION_MAJOR >= 58
    av_channel_layout_uninit(&out_layout_);
#endif
    audio_stream_index_ = -1;
    active_device_.clear();
    buffer_.clear();
    samples_captured_ = 0;
}
