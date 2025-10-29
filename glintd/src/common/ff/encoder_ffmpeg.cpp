#include "encoder_ffmpeg.h"

#include <cstring>
#include <algorithm>
#include <iterator>
#include <stdexcept>

#include "logger.h"

extern "C" {
#include <libavcodec/bsf.h>
#include <libswscale/swscale.h>
}

namespace {
#if LIBAVUTIL_VERSION_MAJOR >= 57
    bool copyDefaultLayout(AVChannelLayout &target, int channels) {
        AVChannelLayout layout{};
        av_channel_layout_default(&layout, channels); // возвращает void
        bool ok = av_channel_layout_copy(&target, &layout) >= 0;
        av_channel_layout_uninit(&layout);
        return ok;
    }

    int channelCount(const AVCodecContext *ctx) {
        if (!ctx) return 0;
        return ctx->ch_layout.nb_channels;
    }
#else
    bool copyDefaultLayout(AVChannelLayout &target, int channels) {
        uint64_t mask = av_get_default_channel_layout(channels);
        if (!mask)
            return false;
        target.order = AV_CHANNEL_ORDER_NATIVE;
        target.nb_channels = channels;
        target.u.mask = mask;
        return true;
    }

    int channelCount(const AVCodecContext *ctx) {
        return ctx ? ctx->channels : 0;
    }
#endif

    AVSampleFormat chooseSampleFormat(const AVCodec *codec) {
        if (!codec || !codec->sample_fmts) return AV_SAMPLE_FMT_FLT;
        // Предпочтение: FLT → FLTP → S16 → S16P
        static const AVSampleFormat pref[] = {
            AV_SAMPLE_FMT_FLT, AV_SAMPLE_FMT_FLTP,
            AV_SAMPLE_FMT_S16, AV_SAMPLE_FMT_S16P
        };
        for (auto want: pref) {
            for (const AVSampleFormat *p = codec->sample_fmts; *p != AV_SAMPLE_FMT_NONE; ++p) {
                if (*p == want) return want;
            }
        }
        return codec->sample_fmts[0];
    }


    AVPixelFormat choosePixelFormat(const AVCodec *codec) {
        if (!codec || !codec->pix_fmts) {
            return AV_PIX_FMT_NV12;
        }
        for (const AVPixelFormat *p = codec->pix_fmts; *p != AV_PIX_FMT_NONE; ++p) {
            if (*p == AV_PIX_FMT_NV12 || *p == AV_PIX_FMT_P010 || *p == AV_PIX_FMT_YUV420P) {
                return *p;
            }
        }
        return codec->pix_fmts[0];
    }

    void copyExtradata(const AVCodecContext *ctx, EncoderStreamInfo &info) {
        if (ctx && ctx->extradata && ctx->extradata_size > 0) {
            info.extradata.assign(ctx->extradata, ctx->extradata + ctx->extradata_size);
        }
    }
}

FFmpegEncoder::FFmpegEncoder() = default;

FFmpegEncoder::~FFmpegEncoder() { close(); }

AVCodecContext *FFmpegEncoder::createContext(const std::string &codecName, bool allowHw) {
    const AVCodec *codec = avcodec_find_encoder_by_name(codecName.c_str());
    if (!codec && allowHw) {
        codec = avcodec_find_encoder_by_name("h264_nvenc");
        if (!codec) codec = avcodec_find_encoder_by_name("hevc_nvenc");
    }
    if (!codec) {
        codec = avcodec_find_encoder_by_name("h264");
    }
    if (!codec) {
        codec = avcodec_find_encoder(AV_CODEC_ID_H264);
    }
    if (!codec) return nullptr;
    return avcodec_alloc_context3(codec);
}

bool FFmpegEncoder::initVideo(const std::string &codec, int w, int h, int fps, int br_kbps) {
    video_ctx_ = createContext(codec, true);
    if (!video_ctx_) return false;
    video_codec_ = codec;
    video_width_ = w;
    video_height_ = h;
    video_fps_ = fps;

    video_ctx_->width = w;
    video_ctx_->height = h;
    video_ctx_->time_base = AVRational{1, fps};
    video_ctx_->framerate = AVRational{fps, 1};

    video_ctx_->bit_rate = static_cast<int64_t>(br_kbps) * 1000;
    video_ctx_->gop_size = fps * 2;
    video_ctx_->max_b_frames = 0;
    video_ctx_->pix_fmt = choosePixelFormat(video_ctx_->codec);
    video_ctx_->thread_count = 0;

    if (video_ctx_->codec && strstr(video_ctx_->codec->name, "nvenc")) {
        av_opt_set_int(video_ctx_->priv_data, "bf", 0, 0);
        av_opt_set_int(video_ctx_->priv_data, "rc-lookahead", 0, 0);
    }

    if (strcmp(video_ctx_->codec->name, "libx264") == 0) {
        av_opt_set(video_ctx_->priv_data, "tune", "zerolatency", 0);
        av_opt_set_int(video_ctx_->priv_data, "bframes", 0, 0);
        av_opt_set_int(video_ctx_->priv_data, "rc-lookahead", 0, 0);
    }

    video_frame_ = av_frame_alloc();
    if (!video_frame_) return false;
    video_frame_->format = video_ctx_->pix_fmt;
    video_frame_->width = w;
    video_frame_->height = h;

    if (av_frame_get_buffer(video_frame_, 32) < 0) {
        Logger::instance().error("FFmpegEncoder: failed allocating video frame buffer");
        return false;
    }

    scaler_ = sws_getContext(w, h, AV_PIX_FMT_BGRA, w, h, (AVPixelFormat) video_frame_->format, SWS_BICUBIC, nullptr,
                             nullptr, nullptr);

    if (!scaler_) {
        Logger::instance().error("FFmpegEncoder: failed creating scaler");
        return false;
    }
    if (avcodec_open2(video_ctx_, video_ctx_->codec, nullptr) < 0) {
        Logger::instance().error("FFmpegEncoder: failed to open video codec");
        return false;
    }

    video_stream_info_.codec_name = video_codec_;
    video_stream_info_.type = EncodedStreamType::Video;
    video_stream_info_.width = w;
    video_stream_info_.height = h;
    video_stream_info_.timebase_num = 1;
    video_stream_info_.timebase_den = 1000;

    if (video_ctx_->extradata && video_ctx_->extradata_size > 0) {
        video_stream_info_.extradata.assign(
            video_ctx_->extradata,
            video_ctx_->extradata + video_ctx_->extradata_size
        );
    } else {
        Logger::instance().warn("FFmpegEncoder: no extradata generated by codec");
    }

    if (video_ctx_->codec && strstr(video_ctx_->codec->name, "nvenc")) {
        av_opt_set_int(video_ctx_->priv_data, "repeat_headers", 1, 0);
        av_opt_set_int(video_ctx_->priv_data, "annexb", 1, 0);
    }
    return true;
}

bool FFmpegEncoder::initAudio(const std::string &codec, int sr, int ch, int br_kbps, bool mic) {
    auto &target = mic ? mic_audio_ : system_audio_;
    target.ctx = createContext(codec, false);
    if (!target.ctx) {
        Logger::instance().warn("FFmpegEncoder: audio codec not found: " + codec);
        return false;
    }
    target.codec_name = codec;
    target.ctx->strict_std_compliance = FF_COMPLIANCE_EXPERIMENTAL;
    if (target.ctx->codec && strcmp(target.ctx->codec->name, "opus") == 0) {
        if (const AVCodec *c = avcodec_find_encoder_by_name("libopus")) {
            avcodec_free_context(&target.ctx);
            target.ctx = avcodec_alloc_context3(c);
            target.codec_name = "libopus";
            // снова проставим поля ниже (sr, layout, fmt)
        }
    }
    if (target.ctx->codec_id == AV_CODEC_ID_OPUS) {
        target.ctx->sample_rate = 48000;
    }

    const AVCodec *avcodec = target.ctx->codec;
    if (!avcodec) {
        avcodec = avcodec_find_encoder_by_name(codec.c_str());
    }

    target.ctx->sample_rate = sr;
#if LIBAVUTIL_VERSION_MAJOR >= 57
    if (!copyDefaultLayout(target.ctx->ch_layout, ch)) {
        Logger::instance().warn("FFmpegEncoder: cannot set channel layout");
        return false;
    }
#else
    if (!copyDefaultLayout(target.ctx->channel_layout, ch)) {
        Logger::instance().warn("FFmpegEncoder: cannot set channel layout");
        return false;
    }
    target.ctx->channels = ch;
#endif
    target.ctx->time_base = AVRational{1, sr};
    target.ctx->bit_rate = static_cast<int64_t>(br_kbps) * 1000;
    target.ctx->sample_fmt = chooseSampleFormat(avcodec);
    target.input_channels = ch;
    target.input_sample_rate = sr;

    target.enabled = true;
    return true;
}

bool FFmpegEncoder::open() {
    if (video_ctx_ && avcodec_open2(video_ctx_, video_ctx_->codec, nullptr) < 0) {
        Logger::instance().error("FFmpegEncoder: cannot open video codec");
        return false;
    }

    if (video_ctx_) {
        AVFrame *dummy = av_frame_alloc();
        dummy->width = video_ctx_->width;
        dummy->height = video_ctx_->height;
        dummy->format = video_ctx_->pix_fmt;
        if (av_frame_get_buffer(dummy, 32) >= 0) {
            if (avcodec_send_frame(video_ctx_, dummy) >= 0) {
                AVPacket *pkt = av_packet_alloc();
                if (avcodec_receive_packet(video_ctx_, pkt) >= 0) {
                    copyExtradata(video_ctx_, video_stream_info_);
                    if (video_ctx_->extradata && video_ctx_->extradata_size > 0) {
                        Logger::instance().info("FFmpegEncoder: extradata refreshed after dummy frame");
                        video_stream_info_.extradata.assign(
                            video_ctx_->extradata,
                            video_ctx_->extradata + video_ctx_->extradata_size
                        );
                    }
                    av_packet_unref(pkt);
                }
                av_packet_free(&pkt);
            }
        }
        av_frame_free(&dummy);
    }

    auto setupAudio = [&](AudioEncoderState &state) {
        if (!state.enabled || !state.ctx) return true;
        if (avcodec_open2(state.ctx, state.ctx->codec, nullptr) < 0) {
            Logger::instance().warn("FFmpegEncoder: failed opening audio codec");
            state.enabled = false;
            return false;
        }
        if (video_ctx_) {
            copyExtradata(video_ctx_, video_stream_info_);
        }

        state.frame = av_frame_alloc();
        if (!state.frame) {
            state.enabled = false;
            return false;
        }
        state.frame_samples = state.ctx->frame_size > 0 ? state.ctx->frame_size : 960; // Opus = 960 samples
        state.fifo = av_audio_fifo_alloc(state.ctx->sample_fmt,
#if LIBAVUTIL_VERSION_MAJOR >= 57
                                         state.ctx->ch_layout.nb_channels,
#else
                                         state.ctx->channels,
#endif
                                         state.frame_samples * 8);
        if (!state.fifo) {
            Logger::instance().error("FFmpegEncoder: failed to alloc audio fifo");
            state.enabled = false;
            return false;
        }
        state.frame->nb_samples = state.frame_samples;
        state.frame->format = state.ctx->sample_fmt;
#if LIBAVUTIL_VERSION_MAJOR >= 57
        if (av_channel_layout_copy(&state.frame->ch_layout, &state.ctx->ch_layout) < 0) {
            Logger::instance().warn("FFmpegEncoder: frame channel layout copy failed");
            state.enabled = false;
            return false;
        }
#else
        state.frame->channel_layout = state.ctx->channel_layout;
        state.frame->channels = state.ctx->channels;
#endif
        state.frame->sample_rate = state.ctx->sample_rate;
        if (av_frame_get_buffer(state.frame, 0) < 0) {
            Logger::instance().warn("FFmpegEncoder: audio frame buffer failed");
            state.enabled = false;
            return false;
        }
#if LIBAVUTIL_VERSION_MAJOR >= 57
        AVChannelLayout inputLayout{};
        if (!copyDefaultLayout(inputLayout,
                               state.input_channels > 0 ? state.input_channels : channelCount(state.ctx))) {
            Logger::instance().warn("FFmpegEncoder: input layout unavailable");
            state.enabled = false;
            return false;
        }
        if (swr_alloc_set_opts2(&state.resampler,
                                &state.ctx->ch_layout,
                                state.ctx->sample_fmt,
                                state.ctx->sample_rate,
                                &inputLayout,
                                AV_SAMPLE_FMT_FLT,
                                state.input_sample_rate > 0 ? state.input_sample_rate : state.ctx->sample_rate,
                                0, nullptr) < 0) {
            Logger::instance().warn("FFmpegEncoder: resampler allocation failed");
            av_channel_layout_uninit(&inputLayout);
            state.enabled = false;
            return false;
        }
        av_channel_layout_uninit(&inputLayout);
#else
        uint64_t inputLayout = 0;
        if (!copyDefaultLayout(inputLayout,
                               state.input_channels > 0 ? state.input_channels : state.ctx->channels)) {
            Logger::instance().warn("FFmpegEncoder: input layout unavailable");
            state.enabled = false;
            return false;
        }
        state.resampler = swr_alloc_set_opts(nullptr,
                                             state.ctx->channel_layout,
                                             state.ctx->sample_fmt,
                                             state.ctx->sample_rate,
                                             inputLayout,
                                             AV_SAMPLE_FMT_FLT,
                                             state.input_sample_rate > 0
                                                 ? state.input_sample_rate
                                                 : state.ctx->sample_rate,
                                             0, nullptr);
#endif
        if (!state.resampler || swr_init(state.resampler) < 0) {
            Logger::instance().warn("FFmpegEncoder: resampler init failed");
            state.enabled = false;
            return false;
        }
        state.fifo = av_audio_fifo_alloc(state.ctx->sample_fmt,
#if LIBAVUTIL_VERSION_MAJOR >= 57
                                         state.ctx->ch_layout.nb_channels,

#else
                                         state.ctx->channels,
#endif
                                         state.frame_samples * 4);
        if (!state.fifo) {
            Logger::instance().warn("FFmpegEncoder: audio fifo alloc failed");
            state.enabled = false;
            return false;
        }
        return true;
    };

    setupAudio(system_audio_);
    setupAudio(mic_audio_);

    if (video_ctx_ && (!video_ctx_->extradata || video_ctx_->extradata_size == 0)) {
        AVPacket *pkt = av_packet_alloc();
        if (avcodec_receive_packet(video_ctx_, pkt) >= 0 && pkt->size > 4) {
            Logger::instance().info("FFmpegEncoder: received packet to extract extradata");

            const AVBitStreamFilter *bsf = av_bsf_get_by_name("extract_extradata");
            if (bsf) {
                AVBSFContext *ctx = nullptr;
                if (av_bsf_alloc(bsf, &ctx) == 0) {
                    avcodec_parameters_from_context(ctx->par_in, video_ctx_);
                    av_bsf_init(ctx);
                    av_bsf_send_packet(ctx, pkt);
                    AVPacket *out = av_packet_alloc();
                    if (av_bsf_receive_packet(ctx, out) == 0 && out->size > 0) {
                        video_stream_info_.extradata.assign(out->data, out->data + out->size);
                        Logger::instance().info("FFmpegEncoder: extradata extracted using bsf");
                    }
                    av_packet_free(&out);
                    av_bsf_free(&ctx);
                }
            }
        }
        av_packet_free(&pkt);
    }
    return true;
}

bool FFmpegEncoder::prepareVideoFrame(const uint8_t *rgba, int w, int h, int stride, uint64_t pts_ms) {
    if (!video_ctx_ || !video_frame_) return false;


    const AVPixelFormat srcFmt = AV_PIX_FMT_BGRA;
    const int useStride = (stride > 0) ? stride : (w * 4);

    SwsContext *newScaler = sws_getCachedContext(
        scaler_,
        w, h, srcFmt,
        video_frame_->width, video_frame_->height,
        (AVPixelFormat) video_frame_->format,
        SWS_BICUBIC, nullptr, nullptr, nullptr);
    if (!newScaler) {
        Logger::instance().warn("FFmpegEncoder: cannot create scaler");
        return false;
    }
    scaler_ = newScaler;

    const int *coeff = sws_getCoefficients(SWS_CS_ITU709);
    int srcRange = 1; // full
    int dstRange = 0; // limited

    int brightness = 0;
    int contrast   = 1 << 16; // 1.0 in fixed-point
    int saturation = 1 << 16; // 1.0

    if (scaler_) {
        int rc = sws_setColorspaceDetails(
            scaler_,
            coeff, srcRange,
            coeff, dstRange,
            brightness, contrast, saturation
        );
        if (rc < 0) {
            Logger::instance().warn("FFmpegEncoder: sws_setColorspaceDetails failed");
            return false;
        }
    }

    const uint8_t *src[4] = {rgba, nullptr, nullptr, nullptr};
    int srcStride[4] = {useStride, 0, 0, 0};
    int rc = sws_scale(scaler_, src, srcStride, 0, h, video_frame_->data, video_frame_->linesize);
    if (rc <= 0) {
        Logger::instance().warn("FFmpegEncoder: sws_scale failed");
        return false;
    }

    // video_frame_->pts = av_rescale_q((int64_t) pts_ms, AVRational{1, 1000}, video_ctx_->time_base);
    video_frame_->pts = video_pts_index_++;
    return true;
}

bool FFmpegEncoder::encodeFrame(AVCodecContext *ctx, AVFrame *frame, EncodedStreamType type,
                                std::vector<EncodedPacket> &out) {
    if (!ctx) return false;
    if (avcodec_send_frame(ctx, frame) < 0) {
        return false;
    }
    AVPacket *pkt = av_packet_alloc();
    if (!pkt) return false;
    while (true) {
        int ret = avcodec_receive_packet(ctx, pkt);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
            break;
        }
        if (ret < 0) {
            av_packet_unref(pkt);
            av_packet_free(&pkt);
            return false;
        }
        EncodedPacket encoded;
        encoded.type = type;
        encoded.keyframe = (pkt->flags & AV_PKT_FLAG_KEY) != 0;
        if (pkt->pts != AV_NOPTS_VALUE) {
            encoded.pts = av_rescale_q(pkt->pts, ctx->time_base, AVRational{1, 1000});
        } else {
            int64_t step_ms = (video_fps_ > 0) ? (1000 / video_fps_) : 16;
            static int64_t fallback_ms = 0;
            encoded.pts = fallback_ms;
            fallback_ms += step_ms;
        }
        if (pkt->dts != AV_NOPTS_VALUE)
            encoded.dts = av_rescale_q(pkt->dts, ctx->time_base, AVRational{1,1000});
        else
            encoded.dts = AV_NOPTS_VALUE;
        encoded.data.assign(pkt->data, pkt->data + pkt->size);
        out.emplace_back(std::move(encoded));
        av_packet_unref(pkt);
    }
    av_packet_free(&pkt);
    return true;
}

bool FFmpegEncoder::encodeAudioSamples(AudioEncoderState &state,
                                       const float *interleaved, int samples, int sr, int ch,
                                       uint64_t pts_ms, EncodedStreamType type, std::vector<EncodedPacket> &out) {
    if (!state.enabled || !state.ctx || !state.frame || !state.resampler) return false;

    const uint8_t *srcData[1] = {reinterpret_cast<const uint8_t *>(interleaved)};
    const int src_samples = samples;

    if (av_frame_make_writable(state.frame) < 0) return false;
    int got = swr_convert(state.resampler,
                          state.frame->data, state.frame->nb_samples,
                          srcData, src_samples);
    if (got < 0) return false;

    if (got > 0) {
        if (av_audio_fifo_write(state.fifo, (void **) state.frame->data, got) < got) {
            return false;
        }
    }

    while (av_audio_fifo_size(state.fifo) >= state.frame_samples) {
        if (av_frame_make_writable(state.frame) < 0) return false;
        if (av_audio_fifo_read(state.fifo, (void **) state.frame->data, state.frame_samples) < state.
            frame_samples) {
            return false;
        }
        state.frame->nb_samples = state.frame_samples;
        int64_t pts_ms_exact = (state.samples_sent * 1000) / state.ctx->sample_rate;
        state.frame->pts = av_rescale_q(pts_ms_exact, AVRational{1, 1000}, state.ctx->time_base);
        if (!encodeFrame(state.ctx, state.frame, type, out)) return false;
        state.samples_sent += state.frame_samples;
    }
    return true;
}

bool FFmpegEncoder::pushVideoRGBA(const uint8_t *rgba, int w, int h, int stride, uint64_t pts_ms) {
    if (!prepareVideoFrame(rgba, w, h, stride, pts_ms)) {
        return false;
    }
    return encodeFrame(video_ctx_, video_frame_, EncodedStreamType::Video, pending_packets_);
}

bool FFmpegEncoder::pushAudioF32(const float *interleaved, int samples, int sr, int ch, uint64_t pts_ms, bool mic) {
    auto &state = mic ? mic_audio_ : system_audio_;
    EncodedStreamType type = mic ? EncodedStreamType::MicrophoneAudio : EncodedStreamType::SystemAudio;
    if (state.input_sample_rate != sr || state.input_channels != ch) {
        state.input_sample_rate = sr;
        state.input_channels = ch;

#if LIBAVUTIL_VERSION_MAJOR >= 57
        AVChannelLayout inL{};
        av_channel_layout_default(&inL, ch);
        swr_free(&state.resampler);
        if (swr_alloc_set_opts2(&state.resampler,
                                &state.ctx->ch_layout, state.ctx->sample_fmt, state.ctx->sample_rate,
                                &inL, AV_SAMPLE_FMT_FLT, sr, 0, nullptr) < 0 || swr_init(state.resampler) < 0) {
            Logger::instance().warn("FFmpegEncoder: resampler reinit failed");
            return false;
        }
        av_channel_layout_uninit(&inL);
#else
        uint64_t inMask = av_get_default_channel_layout(ch);
        swr_free(&state.resampler);
        state.resampler = swr_alloc_set_opts(nullptr,
                                             state.ctx->channel_layout, state.ctx->sample_fmt,
                                             state.ctx->sample_rate,
                                             inMask, AV_SAMPLE_FMT_FLT, sr, 0, nullptr);
        if (!state.resampler || swr_init(state.resampler) < 0) {
            Logger::instance().warn("FFmpegEncoder: resampler reinit failed");
            return false;
        }
#endif
    }

    return encodeAudioSamples(state, interleaved, samples, sr, ch, pts_ms, type, pending_packets_);
}

bool FFmpegEncoder::pull(std::vector<EncodedPacket> &out) {
    if (pending_packets_.empty()) return false;
    out.insert(out.end(), std::make_move_iterator(pending_packets_.begin()),
               std::make_move_iterator(pending_packets_.end()));
    pending_packets_.clear();
    return true;
}

void FFmpegEncoder::flush(std::vector<EncodedPacket> &out) {
    encodeFrame(video_ctx_, nullptr, EncodedStreamType::Video, out);
    encodeFrame(system_audio_.ctx, nullptr, EncodedStreamType::SystemAudio, out);
    encodeFrame(mic_audio_.ctx, nullptr, EncodedStreamType::MicrophoneAudio, out);
}

void FFmpegEncoder::close() {
    if (video_frame_) {
        av_frame_free(&video_frame_);
        video_frame_ = nullptr;
    }
    if (video_ctx_) {
        avcodec_free_context(&video_ctx_);
        video_ctx_ = nullptr;
    }
    if (scaler_) {
        sws_freeContext(scaler_);
        scaler_ = nullptr;
    }
    auto cleanupAudio = [](AudioEncoderState &state) {
        if (state.frame) {
            av_frame_free(&state.frame);
            state.frame = nullptr;
        }
        if (state.ctx) {
            avcodec_free_context(&state.ctx);
            state.ctx = nullptr;
        }
        if (state.resampler) {
            swr_free(&state.resampler);
            state.resampler = nullptr;
        }
        if (state.fifo) {
            av_audio_fifo_free(state.fifo);
            state.fifo = nullptr;
        }
        state.input_channels = 0;
        state.input_sample_rate = 0;
        state.enabled = false;
    };
    cleanupAudio(system_audio_);
    cleanupAudio(mic_audio_);
    pending_packets_.clear();
}

EncoderStreamInfo FFmpegEncoder::videoStream() const {
    EncoderStreamInfo info = video_stream_info_;
    info.type = EncodedStreamType::Video;
    info.codec_name = video_codec_;
    info.width = video_width_;
    info.height = video_height_;
    info.fps = video_fps_;
    info.timebase_num = video_ctx_ ? video_ctx_->time_base.num : 1;
    info.timebase_den = video_ctx_ ? video_ctx_->time_base.den : 1000;
    return info;
}

EncoderStreamInfo FFmpegEncoder::audioStream(bool mic) const {
    EncoderStreamInfo info;
    info.type = mic ? EncodedStreamType::MicrophoneAudio : EncodedStreamType::SystemAudio;
    const auto &state = mic ? mic_audio_ : system_audio_;
    info.codec_name = state.codec_name;
    info.sample_rate = state.ctx ? state.ctx->sample_rate : 0;
    info.channels = channelCount(state.ctx);
    info.timebase_num = state.ctx ? state.ctx->time_base.num : 1;
    info.timebase_den = state.ctx ? state.ctx->time_base.den : 1000;
    copyExtradata(state.ctx, info);
    return info;
}
