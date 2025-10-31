#include "encoder_ffmpeg.h"

#include <cstring>
#include <algorithm>
#include <format>
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

FFmpegEncoder::CodecContextPtr FFmpegEncoder::createContext(const std::string &codecName, bool allowHw) {
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
    return CodecContextPtr(avcodec_alloc_context3(codec));
}

bool FFmpegEncoder::initVideo(const std::string &codec, int w, int h, int fps, int br_kbps) {
    video_ctx_ = createContext(codec, true);
    if (!video_ctx_) {
        Logger::instance().error(std::format("FFmpegEncoder: video codec {} not available", codec));
        return false;
    }

    auto* ctx = video_ctx_.get();
    video_width_ = w;
    video_height_ = h;
    const int targetFps = fps > 0 ? fps : 60;
    video_fps_ = targetFps;
    video_codec_ = ctx->codec ? ctx->codec->name : codec;

    ctx->width = w;
    ctx->height = h;
    ctx->time_base = AVRational{1, targetFps};
    ctx->framerate = AVRational{targetFps, 1};
    ctx->bit_rate = static_cast<int64_t>(br_kbps) * 1000;
    ctx->gop_size = targetFps * 2;
    ctx->max_b_frames = 0;
    ctx->pix_fmt = choosePixelFormat(ctx->codec);
    ctx->thread_count = 0;

    if (ctx->codec && std::strstr(ctx->codec->name, "nvenc")) {
        av_opt_set_int(ctx->priv_data, "bf", 0, 0);
        av_opt_set_int(ctx->priv_data, "rc-lookahead", 0, 0);
    }

    if (ctx->codec && std::strcmp(ctx->codec->name, "libx264") == 0) {
        av_opt_set(ctx->priv_data, "tune", "zerolatency", 0);
        av_opt_set_int(ctx->priv_data, "bframes", 0, 0);
        av_opt_set_int(ctx->priv_data, "rc-lookahead", 0, 0);
    }

    video_stream_info_ = EncoderStreamInfo{
        .type = EncodedStreamType::Video,
        .codec_name = video_codec_,
        .timebase_num = ctx->time_base.num,
        .timebase_den = ctx->time_base.den,
        .width = w,
        .height = h,
        .fps = targetFps,
        .sample_rate = 0,
        .channels = 0,
        .extradata = {}
    };
    video_stream_info_.extradata.clear();
    last_video_pts_ = GLINT_NOPTS_VALUE;

    if (ctx->codec && std::strstr(ctx->codec->name, "nvenc")) {
        av_opt_set_int(ctx->priv_data, "repeat_headers", 1, 0);
        av_opt_set_int(ctx->priv_data, "annexb", 1, 0);
    }

    return true;
}

FFmpegEncoder::SwsContextHandle::~SwsContextHandle() {
    reset();
}

FFmpegEncoder::SwsContextHandle::SwsContextHandle(SwsContextHandle&& other) noexcept {
    ctx_ = other.ctx_;
    other.ctx_ = nullptr;
}

FFmpegEncoder::SwsContextHandle& FFmpegEncoder::SwsContextHandle::operator=(SwsContextHandle&& other) noexcept {
    if (this != &other) {
        reset(other.ctx_);
        other.ctx_ = nullptr;
    }
    return *this;
}

void FFmpegEncoder::SwsContextHandle::reset(SwsContext* ctx) noexcept {
    if (ctx_ && ctx_ != ctx) {
        sws_freeContext(ctx_);
    }
    ctx_ = ctx;
}

bool FFmpegEncoder::initAudio(const std::string &codec, int sr, int ch, int br_kbps, bool mic) {
    auto &target = mic ? mic_audio_ : system_audio_;
    target = AudioEncoderState{};
    target.ctx = createContext(codec, false);
    if (!target.ctx) {
        Logger::instance().warn(std::format("FFmpegEncoder: audio codec not found: {}", codec));
        return false;
    }
    auto* ctx = target.ctx.get();
    target.codec_name = ctx->codec ? ctx->codec->name : codec;
    ctx->strict_std_compliance = FF_COMPLIANCE_EXPERIMENTAL;
    if (ctx->codec && std::strcmp(ctx->codec->name, "opus") == 0) {
        if (const AVCodec *c = avcodec_find_encoder_by_name("libopus")) {
            target.ctx.reset(avcodec_alloc_context3(c));
            ctx = target.ctx.get();
            target.codec_name = "libopus";
            ctx->strict_std_compliance = FF_COMPLIANCE_EXPERIMENTAL;
        }
    }

    if (ctx->codec_id == AV_CODEC_ID_OPUS) {
        ctx->sample_rate = 48000;
    }

    const AVCodec *avcodec = ctx->codec;
    if (!avcodec) {
        avcodec = avcodec_find_encoder_by_name(target.codec_name.c_str());
    }

    ctx->sample_rate = sr;
#if LIBAVUTIL_VERSION_MAJOR >= 57
    if (!copyDefaultLayout(ctx->ch_layout, ch)) {
        Logger::instance().warn("FFmpegEncoder: cannot set channel layout");
        return false;
    }
#else
    if (!copyDefaultLayout(ctx->channel_layout, ch)) {
        Logger::instance().warn("FFmpegEncoder: cannot set channel layout");
        return false;
    }
    ctx->channels = ch;
#endif
    ctx->time_base = AVRational{1, sr};
    ctx->bit_rate = static_cast<int64_t>(br_kbps) * 1000;
    ctx->sample_fmt = chooseSampleFormat(avcodec);
    target.input_channels = ch;
    target.input_sample_rate = sr;

    target.enabled = true;
    return true;
}


bool FFmpegEncoder::open() {
    if (video_ctx_) {
        if (avcodec_open2(video_ctx_.get(), video_ctx_->codec, nullptr) < 0) {
            Logger::instance().error("FFmpegEncoder: cannot open video codec");
            return false;
        }

        video_frame_.reset(av_frame_alloc());
        if (!video_frame_) {
            Logger::instance().error("FFmpegEncoder: failed allocating video frame buffer");
            return false;
        }
        video_frame_->format = video_ctx_->pix_fmt;
        video_frame_->width = video_ctx_->width;
        video_frame_->height = video_ctx_->height;
        if (av_frame_get_buffer(video_frame_.get(), 32) < 0) {
            Logger::instance().error("FFmpegEncoder: failed allocating video frame buffer");
            return false;
        }
        last_video_pts_ = GLINT_NOPTS_VALUE;
        copyExtradata(video_ctx_.get(), video_stream_info_);
    }

    auto setupAudio = [&](AudioEncoderState &state) {
        if (!state.enabled || !state.ctx) {
            return true;
        }
        if (avcodec_open2(state.ctx.get(), state.ctx->codec, nullptr) < 0) {
            Logger::instance().warn("FFmpegEncoder: failed opening audio codec");
            state.enabled = false;
            return false;
        }

        state.frame.reset(av_frame_alloc());
        if (!state.frame) {
            state.enabled = false;
            return false;
        }
        state.frame_samples = state.ctx->frame_size > 0 ? state.ctx->frame_size : 960;
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
        if (av_frame_get_buffer(state.frame.get(), 0) < 0) {
            Logger::instance().warn("FFmpegEncoder: audio frame buffer failed");
            state.enabled = false;
            return false;
        }

#if LIBAVUTIL_VERSION_MAJOR >= 57
        AVChannelLayout inputLayout{};
        if (!copyDefaultLayout(inputLayout,
                               state.input_channels > 0 ? state.input_channels : channelCount(state.ctx.get()))) {
            Logger::instance().warn("FFmpegEncoder: input layout unavailable");
            state.enabled = false;
            return false;
        }
        SwrContext* resampler = nullptr;
        if (swr_alloc_set_opts2(&resampler,
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
        state.resampler.reset(resampler);
#else
        uint64_t inputLayout = 0;
        if (!copyDefaultLayout(inputLayout,
                               state.input_channels > 0 ? state.input_channels : state.ctx->channels)) {
            Logger::instance().warn("FFmpegEncoder: input layout unavailable");
            state.enabled = false;
            return false;
        }
        state.resampler.reset(swr_alloc_set_opts(nullptr,
                                                 state.ctx->channel_layout,
                                                 state.ctx->sample_fmt,
                                                 state.ctx->sample_rate,
                                                 inputLayout,
                                                 AV_SAMPLE_FMT_FLT,
                                                 state.input_sample_rate > 0 ? state.input_sample_rate : state.ctx->sample_rate,
                                                 0, nullptr));
#endif
        if (!state.resampler || swr_init(state.resampler.get()) < 0) {
            Logger::instance().warn("FFmpegEncoder: resampler init failed");
            state.enabled = false;
            return false;
        }

        state.fifo.reset(av_audio_fifo_alloc(state.ctx->sample_fmt,
#if LIBAVUTIL_VERSION_MAJOR >= 57
                                             state.ctx->ch_layout.nb_channels,
#else
                                             state.ctx->channels,
#endif
                                             state.frame_samples * 4));
        if (!state.fifo) {
            Logger::instance().warn("FFmpegEncoder: audio fifo alloc failed");
            state.enabled = false;
            return false;
        }
        state.samples_sent = 0;
        return true;
    };

    setupAudio(system_audio_);
    setupAudio(mic_audio_);

    if (video_ctx_ && video_stream_info_.extradata.empty()) {
        FramePtr dummy(av_frame_alloc());
        if (dummy) {
            dummy->width = video_ctx_->width;
            dummy->height = video_ctx_->height;
            dummy->format = video_ctx_->pix_fmt;
            if (av_frame_get_buffer(dummy.get(), 32) >= 0) {
                if (avcodec_send_frame(video_ctx_.get(), dummy.get()) >= 0) {
                    PacketPtr pkt(av_packet_alloc());
                    if (pkt && avcodec_receive_packet(video_ctx_.get(), pkt.get()) >= 0) {
                        copyExtradata(video_ctx_.get(), video_stream_info_);
                        if (!video_stream_info_.extradata.empty()) {
                            Logger::instance().info("FFmpegEncoder: extradata refreshed after dummy frame");
                        }
                        av_packet_unref(pkt.get());
                    }
                }
                avcodec_flush_buffers(video_ctx_.get());
            }
        }
    }

    if (video_ctx_ && video_stream_info_.extradata.empty()) {
        PacketPtr pkt(av_packet_alloc());
        if (pkt && avcodec_receive_packet(video_ctx_.get(), pkt.get()) >= 0 && pkt->size > 4) {
            Logger::instance().info("FFmpegEncoder: received packet to extract extradata");
            const AVBitStreamFilter *bsf = av_bsf_get_by_name("extract_extradata");
            if (bsf) {
                AVBSFContext *bsfCtx = nullptr;
                if (av_bsf_alloc(bsf, &bsfCtx) == 0) {
                    avcodec_parameters_from_context(bsfCtx->par_in, video_ctx_.get());
                    av_bsf_init(bsfCtx);
                    av_bsf_send_packet(bsfCtx, pkt.get());
                    PacketPtr out(av_packet_alloc());
                    if (out && av_bsf_receive_packet(bsfCtx, out.get()) == 0 && out->size > 0) {
                        video_stream_info_.extradata.assign(out->data, out->data + out->size);
                        Logger::instance().info("FFmpegEncoder: extradata extracted using bsf");
                    }
                    av_bsf_free(&bsfCtx);
                }
            }
            av_packet_unref(pkt.get());
        }
    }
    return true;
}

bool FFmpegEncoder::prepareVideoFrame(const uint8_t *rgba, int w, int h, int stride, uint64_t pts_ms) {
    if (!video_ctx_ || !video_frame_) {
        return false;
    }

    const AVPixelFormat srcFmt = AV_PIX_FMT_BGRA;
    const int useStride = (stride > 0) ? stride : (w * 4);

    SwsContext *newScaler = sws_getCachedContext(
        scaler_.get(),
        w, h, srcFmt,
        video_frame_->width, video_frame_->height,
        static_cast<AVPixelFormat>(video_frame_->format),
        SWS_BICUBIC, nullptr, nullptr, nullptr);
    if (!newScaler) {
        Logger::instance().warn("FFmpegEncoder: cannot create scaler");
        return false;
    }
    scaler_.reset(newScaler);

    const int *coeff = sws_getCoefficients(SWS_CS_ITU709);
    int srcRange = 1; // full
    int dstRange = 0; // limited

    int brightness = 0;
    int contrast   = 1 << 16; // 1.0 in fixed-point
    int saturation = 1 << 16; // 1.0

    if (auto* scaler = scaler_.get()) {
        int rc = sws_setColorspaceDetails(
            scaler,
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
    int rc = sws_scale(scaler_.get(), src, srcStride, 0, h, video_frame_->data, video_frame_->linesize);
    if (rc <= 0) {
        Logger::instance().warn("FFmpegEncoder: sws_scale failed");
        return false;
    }

    int64_t scaled_pts = av_rescale_q(static_cast<int64_t>(pts_ms), AVRational{1, 1000}, video_ctx_->time_base);
    if (last_video_pts_ != GLINT_NOPTS_VALUE && scaled_pts <= last_video_pts_) {
        scaled_pts = last_video_pts_ + 1;
    }
    video_frame_->pts = scaled_pts;
    last_video_pts_ = scaled_pts;
    return true;
}

bool FFmpegEncoder::encodeFrame(AVCodecContext *ctx, AVFrame *frame, EncodedStreamType type,
                                std::vector<EncodedPacket> &out) {
    if (!ctx) {
        return true;
    }
    const int send = avcodec_send_frame(ctx, frame);
    if (send < 0 && send != AVERROR_EOF) {
        Logger::instance().warn(std::format("FFmpegEncoder: avcodec_send_frame failed ({})", send));
        return false;
    }

    PacketPtr pkt(av_packet_alloc());
    if (!pkt) {
        Logger::instance().error("FFmpegEncoder: failed to allocate packet");
        return false;
    }

    while (true) {
        int ret = avcodec_receive_packet(ctx, pkt.get());
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
            break;
        }
        if (ret < 0) {
            Logger::instance().warn(std::format("FFmpegEncoder: avcodec_receive_packet failed ({})", ret));
            return false;
        }

        EncodedPacket encoded;
        encoded.type = type;
        encoded.keyframe = (pkt->flags & AV_PKT_FLAG_KEY) != 0;
        if (pkt->pts != AV_NOPTS_VALUE) {
            encoded.pts = av_rescale_q(pkt->pts, ctx->time_base, AVRational{1, 1000});
        } else {
            encoded.pts = (type == EncodedStreamType::Video && last_video_pts_ != GLINT_NOPTS_VALUE && video_ctx_)
                ? av_rescale_q(last_video_pts_, video_ctx_->time_base, AVRational{1, 1000})
                : 0;
        }
        if (pkt->dts != AV_NOPTS_VALUE) {
            encoded.dts = av_rescale_q(pkt->dts, ctx->time_base, AVRational{1, 1000});
        } else {
            encoded.dts = GLINT_NOPTS_VALUE;
        }
        encoded.data.assign(pkt->data, pkt->data + pkt->size);

        if (type == EncodedStreamType::Video && video_stream_info_.extradata.empty() && ctx->extradata && ctx->extradata_size > 0) {
            copyExtradata(ctx, video_stream_info_);
        }

        out.emplace_back(std::move(encoded));
        av_packet_unref(pkt.get());
    }
    return true;
}

bool FFmpegEncoder::encodeAudioSamples(AudioEncoderState &state,
                                       const float *interleaved, int samples, int sr, int ch,
                                       uint64_t pts_ms, EncodedStreamType type, std::vector<EncodedPacket> &out) {
    if (!state.enabled || !state.ctx || !state.frame || !state.resampler || !state.fifo) {
        return false;
    }

    if (state.input_sample_rate != sr || state.input_channels != ch) {
        state.input_sample_rate = sr;
        state.input_channels = ch;
#if LIBAVUTIL_VERSION_MAJOR >= 57
        AVChannelLayout inLayout{};
        if (!copyDefaultLayout(inLayout, ch)) {
            Logger::instance().warn("FFmpegEncoder: input layout unavailable");
            return false;
        }
        state.resampler.reset();
        SwrContext* resampler = nullptr;
        if (swr_alloc_set_opts2(&resampler,
                                &state.ctx->ch_layout, state.ctx->sample_fmt, state.ctx->sample_rate,
                                &inLayout, AV_SAMPLE_FMT_FLT, sr, 0, nullptr) < 0) {
            Logger::instance().warn("FFmpegEncoder: resampler reinit failed");
            av_channel_layout_uninit(&inLayout);
            return false;
        }
        av_channel_layout_uninit(&inLayout);
        state.resampler.reset(resampler);
#else
        uint64_t inMask = av_get_default_channel_layout(ch);
        state.resampler.reset();
        state.resampler.reset(swr_alloc_set_opts(nullptr,
                                                 state.ctx->channel_layout, state.ctx->sample_fmt, state.ctx->sample_rate,
                                                 inMask, AV_SAMPLE_FMT_FLT, sr, 0, nullptr));
#endif
        if (!state.resampler || swr_init(state.resampler.get()) < 0) {
            Logger::instance().warn("FFmpegEncoder: resampler reinit failed");
            return false;
        }
    }

    const uint8_t *srcData[1] = {reinterpret_cast<const uint8_t *>(interleaved)};
    if (av_frame_make_writable(state.frame.get()) < 0) {
        return false;
    }
    int got = swr_convert(state.resampler.get(),
                          state.frame->data, state.frame->nb_samples,
                          srcData, samples);
    if (got < 0) {
        return false;
    }

    if (got > 0) {
        if (av_audio_fifo_write(state.fifo.get(), reinterpret_cast<void **>(state.frame->data), got) < got) {
            return false;
        }
    }

    while (av_audio_fifo_size(state.fifo.get()) >= state.frame_samples) {
        if (av_frame_make_writable(state.frame.get()) < 0) {
            return false;
        }
        if (av_audio_fifo_read(state.fifo.get(), reinterpret_cast<void **>(state.frame->data), state.frame_samples) <
            state.frame_samples) {
            return false;
        }
        state.frame->nb_samples = state.frame_samples;
        int64_t pts_ms_exact = (state.samples_sent * 1000) / state.ctx->sample_rate;
        state.frame->pts = av_rescale_q(pts_ms_exact, AVRational{1, 1000}, state.ctx->time_base);
        if (!encodeFrame(state.ctx.get(), state.frame.get(), type, out)) {
            return false;
        }
        state.samples_sent += state.frame_samples;
    }
    return true;
}


bool FFmpegEncoder::pushVideoRGBA(const uint8_t *rgba, int w, int h, int stride, uint64_t pts_ms) {
    if (!prepareVideoFrame(rgba, w, h, stride, pts_ms)) {
        return false;
    }
    return encodeFrame(video_ctx_.get(), video_frame_.get(), EncodedStreamType::Video, pending_packets_);
}

bool FFmpegEncoder::pushAudioF32(const float *interleaved, int samples, int sr, int ch, uint64_t pts_ms, bool mic) {
    auto &state = mic ? mic_audio_ : system_audio_;
    EncodedStreamType type = mic ? EncodedStreamType::MicrophoneAudio : EncodedStreamType::SystemAudio;
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
    encodeFrame(video_ctx_.get(), nullptr, EncodedStreamType::Video, out);
    encodeFrame(system_audio_.ctx.get(), nullptr, EncodedStreamType::SystemAudio, out);
    encodeFrame(mic_audio_.ctx.get(), nullptr, EncodedStreamType::MicrophoneAudio, out);
}


void FFmpegEncoder::close() {
    video_frame_.reset();
    video_ctx_.reset();
    scaler_.reset();
    auto cleanupAudio = [](AudioEncoderState &state) {
        state.frame.reset();
        state.ctx.reset();
        state.resampler.reset();
        state.fifo.reset();
        state.input_channels = 0;
        state.input_sample_rate = 0;
        state.samples_sent = 0;
        state.codec_name.clear();
        state.enabled = false;
    };
    cleanupAudio(system_audio_);
    cleanupAudio(mic_audio_);
    pending_packets_.clear();
    video_stream_info_.extradata.clear();
    last_video_pts_ = GLINT_NOPTS_VALUE;
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
    info.channels = channelCount(state.ctx.get());
    info.timebase_num = state.ctx ? state.ctx->time_base.num : 1;
    info.timebase_den = state.ctx ? state.ctx->time_base.den : 1000;
    copyExtradata(state.ctx.get(), info);
    return info;
}