#include "muxer_avformat.h"

#include <filesystem>
#include <cstring>

#include "config.h"

extern "C" {
#include <libavutil/channel_layout.h>
#include <libavutil/samplefmt.h>
#include <libavutil/version.h>
}

#include "logger.h"

namespace {
AVRational ms_time_base{1, 1000};

#if LIBAVUTIL_VERSION_MAJOR >= 57
    bool setChannelsNoLayout(AVCodecParameters* p, int ch) {
        av_channel_layout_uninit(&p->ch_layout);
        p->ch_layout.order = AV_CHANNEL_ORDER_UNSPEC; // без layout
        p->ch_layout.nb_channels = ch;
        return ch > 0;
    }
    bool setDefaultLayout(AVCodecParameters* p, int ch) {
        if (ch <= 0) return false;
        av_channel_layout_default(&p->ch_layout, ch); // void
        return true;
    }
#else
    bool setChannelsNoLayout(AVCodecParameters* p, int ch) {
        p->channel_layout = 0;
        p->channels = ch;
        return ch > 0;
    }
    bool setDefaultLayout(AVCodecParameters* p, int ch) {
        if (ch <= 0) return false;
        p->channels = ch;
        p->channel_layout = av_get_default_channel_layout(ch);
        return p->channel_layout != 0;
    }
#endif
} // namespace


AvMuxer::AvMuxer() = default;
AvMuxer::~AvMuxer() { close(); }

AvMuxer::StreamClock& AvMuxer::clkFor(EncodedStreamType type) {
    switch (type) {
        case EncodedStreamType::Video:           return vclk_;
        case EncodedStreamType::SystemAudio:     return a1clk_;
        case EncodedStreamType::MicrophoneAudio: return a2clk_;
    }
    return vclk_;
}

bool AvMuxer::addStream(const EncoderStreamInfo& info, int& index_out) {
    if (info.codec_name.empty()) { index_out = -1; return true; }

    if (info.type == EncodedStreamType::Video && info.extradata.empty()) {
        Logger::instance().warn("AvMuxer: video stream has no extradata (SPS/PPS) — write_header may fail");
    }

    const AVCodec* codec = avcodec_find_encoder_by_name(info.codec_name.c_str());
    if (!codec) {
        Logger::instance().warn("AvMuxer: codec not found for stream " + info.codec_name);
        codec = avcodec_find_encoder(AV_CODEC_ID_H264);
    }

    AVStream* stream = avformat_new_stream(ctx_, codec);
    if (!stream) {
        Logger::instance().error("AvMuxer: failed to create stream");
        return false;
    }

    stream->time_base = AVRational{info.timebase_num, info.timebase_den};
    AVCodecParameters* params = stream->codecpar;
    params->codec_type = (info.type == EncodedStreamType::Video) ? AVMEDIA_TYPE_VIDEO : AVMEDIA_TYPE_AUDIO;
    params->codec_id = codec ? codec->id : (info.type == EncodedStreamType::Video ? AV_CODEC_ID_H264 : AV_CODEC_ID_AAC);

    if (info.type == EncodedStreamType::Video) {
        params->width  = info.width;
        params->height = info.height;
        // params->format = AV_PIX_FMT_NV12;
    } else {
        params->sample_rate = (params->codec_id == AV_CODEC_ID_OPUS && info.sample_rate != 48000)
                                ? 48000 : info.sample_rate; // Opus → 48k

        bool ok = true;
        if (params->codec_id == AV_CODEC_ID_OPUS) {
            ok = setChannelsNoLayout(params, info.channels);
        } else {
            ok = setDefaultLayout(params, info.channels);
        }
        if (!ok) {
            Logger::instance().warn("AvMuxer: invalid channel setup for stream " + info.codec_name);
        }

        // params->format = AV_SAMPLE_FMT_FLTP;
    }

    if (!info.extradata.empty()) {
        params->extradata = static_cast<uint8_t*>(
            av_mallocz(info.extradata.size() + AV_INPUT_BUFFER_PADDING_SIZE));
        memcpy(params->extradata, info.extradata.data(), info.extradata.size());
        params->extradata_size = static_cast<int>(info.extradata.size());
    }

    index_out = stream->index;
    return true;
}

static bool codec_ok(const AVOutputFormat* fmt, AVCodecID id) {
    return avformat_query_codec(fmt, id, FF_COMPLIANCE_EXPERIMENTAL) > 0;
}

bool AvMuxer::open(const MuxerConfig& cfg,
                   const EncoderStreamInfo& video,
                   const EncoderStreamInfo& systemAudio,
                   const EncoderStreamInfo& micAudio) {
    close();
    config_ = cfg;

    Config config = load_default_config(); // TODO: Replace with actual loading
    const char* want_mux = config.buffer.enabled ? "matroska" : (cfg.container.empty() ? "mp4" : cfg.container.c_str());
    if (avformat_alloc_output_context2(&ctx_, nullptr, want_mux, cfg.path.c_str()) < 0 || !ctx_) {
        Logger::instance().error("AvMuxer: failed to allocate output context");
        return false;
    }
    ctx_->max_interleave_delta = INT64_MAX;
    ctx_->flags |= AVFMT_FLAG_FLUSH_PACKETS;

    if (!addStream(video, video_stream_)) return false;
    if (cfg.two_audio_tracks) {
        addStream(systemAudio, system_stream_);
        addStream(micAudio, mic_stream_);
    }

    auto *fmt = ctx_->oformat;
    auto check_or_switch_container = [&]() -> bool {
        const AVCodecParameters* vp = video_stream_ >= 0 ? ctx_->streams[video_stream_]->codecpar : nullptr;
        const AVCodecParameters* ap1 = system_stream_ >= 0 ? ctx_->streams[system_stream_]->codecpar : nullptr;
        const AVCodecParameters* ap2 = mic_stream_ >= 0 ? ctx_->streams[mic_stream_]->codecpar : nullptr;

        auto is_mp4 = strcmp(fmt->name, "mp4") == 0 || strcmp(fmt->name, "mov") == 0;

        auto has_opus = (ap1 && ap1->codec_id == AV_CODEC_ID_OPUS) || (ap2 && ap2->codec_id == AV_CODEC_ID_OPUS);
        if (is_mp4 && has_opus) {
            Logger::instance().warn("MP4 + Opus is not widely supported. Consider switch to Matroska container.");
            return false;
        }

        if (is_mp4 && vp && vp->codec_id == AV_CODEC_ID_H264 && vp->extradata_size == 0) {
            Logger::instance().error("MP4 want H.264 stream with extradata (avcC), but none provided.");
            return false;
        }
        return true;
    };
    if (!check_or_switch_container()) return false;

    if (!(ctx_->oformat->flags & AVFMT_NOFILE)) {
        if (avio_open(&ctx_->pb, cfg.path.c_str(), AVIO_FLAG_WRITE) < 0) {
            Logger::instance().error("AvMuxer: could not open output file");
            return false;
        }
    }
    av_dump_format(ctx_, 0, cfg.path.c_str(), 1);

    return true;
}

AVStream* AvMuxer::streamForType(EncodedStreamType type) const {
    if (!ctx_) return nullptr;
    switch (type) {
        case EncodedStreamType::Video:
            return (video_stream_ >= 0 && video_stream_ < ctx_->nb_streams) ? ctx_->streams[video_stream_] : nullptr;
        case EncodedStreamType::SystemAudio:
            return (system_stream_ >= 0 && system_stream_ < ctx_->nb_streams) ? ctx_->streams[system_stream_] : nullptr;
        case EncodedStreamType::MicrophoneAudio:
            return (mic_stream_ >= 0 && mic_stream_ < ctx_->nb_streams) ? ctx_->streams[mic_stream_] : nullptr;
    }
    return nullptr;
}

static const uint8_t* find_sc(const uint8_t* p, const uint8_t* end) {
    const uint8_t* a = p;
    while (a + 4 <= end) {
        if (a[0]==0 && a[1]==0 && a[2]==1) return a;
        if (a+4<=end && a[0]==0 && a[1]==0 && a[2]==0 && a[3]==1) return a+1;
        ++a;
    }
    return end;
}

std::vector<uint8_t> AvMuxer::extractH264ExtradataFromAnnexB(const uint8_t* data, int size) {
    std::vector<uint8_t> sps, pps, extradata;
    const uint8_t* end = data + size;
    const uint8_t* p = find_sc(data, end);
    while (p < end) {
        const uint8_t* next = find_sc(p + 3, end);
        const uint8_t* nal = p;
        while (nal < end && *nal == 0x00) ++nal;
        if (nal < end && *nal == 0x01) ++nal;
        if (nal >= end) break;

        uint8_t nalu_type = nal[0] & 0x1F;
        int payload_size = int(next - nal);
        if (payload_size > 0) {
            if (nalu_type == 7) sps.assign(nal, nal + payload_size); // SPS
            if (nalu_type == 8) pps.assign(nal, nal + payload_size); // PPS
        }
        p = next;
    }
    if (!sps.empty() && !pps.empty()) {
        static const uint8_t sc3[] = {0x00,0x00,0x01};
        extradata.insert(extradata.end(), std::begin(sc3), std::end(sc3));
        extradata.insert(extradata.end(), sps.begin(), sps.end());
        extradata.insert(extradata.end(), std::begin(sc3), std::end(sc3));
        extradata.insert(extradata.end(), pps.begin(), pps.end());
    }
    return extradata;
}

bool AvMuxer::write(const EncodedPacket& packet) {
    if (!ctx_) return false;
    AVStream* stream = streamForType(packet.type);
    if (!stream) return false;

    if (!header_written_) {
        if (packet.type != EncodedStreamType::Video) {
            return true;
        }
        AVStream* vs = (video_stream_ >= 0) ? ctx_->streams[video_stream_] : nullptr;
        if (vs && vs->codecpar->codec_id == AV_CODEC_ID_H264 && vs->codecpar->extradata_size == 0) {
            auto extra = extractH264ExtradataFromAnnexB(packet.data.data(), (int)packet.data.size());
            if (!extra.empty()) {
                vs->codecpar->extradata = (uint8_t*)av_mallocz(extra.size() + AV_INPUT_BUFFER_PADDING_SIZE);
                memcpy(vs->codecpar->extradata, extra.data(), extra.size());
                vs->codecpar->extradata_size = (int)extra.size();
                Logger::instance().info("AvMuxer: injected H.264 extradata (SPS/PPS) from first packet");
            }
        }

        int ret = avformat_write_header(ctx_, nullptr);
        if (ret < 0) {
            if (!header_tried_without_extradata_) {
                char err[256]; av_strerror(ret, err, sizeof(err));
                Logger::instance().error(std::string("AvMuxer: deferred write_header failed: ") + err);
                header_tried_without_extradata_ = true;
            }
            return false;
        }
        header_written_ = true;
        Logger::instance().info("AvMuxer: header written");
    }
    auto& clk = clkFor(packet.type);
    int64_t in_ms = (int64_t)packet.pts;
    if (clk.base_ms < 0) clk.base_ms = in_ms;

    int64_t norm_ms = in_ms - clk.base_ms;
    if (clk.last_ms >= 0 && norm_ms <= clk.last_ms) {
        norm_ms = clk.last_ms + 1;
    }
    clk.last_ms = norm_ms;

    AVPacket* pkt = av_packet_alloc();
    if (!pkt) return false;

    pkt->pts = av_rescale_q(norm_ms, AVRational{1,1000}, stream->time_base);
    pkt->dts = pkt->pts;
    pkt->duration = 0;
    if (stream->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
        pkt->duration = 1;
    }

    pkt->data = const_cast<uint8_t*>(packet.data.data());
    pkt->size = (int)packet.data.size();
    pkt->stream_index = stream->index;
    if (packet.keyframe) pkt->flags |= AV_PKT_FLAG_KEY;

    int ret = av_interleaved_write_frame(ctx_, pkt);
    av_packet_free(&pkt);
    if (ret < 0) {
        Logger::instance().warn("AvMuxer: write frame failed");
        return false;
    }
    return true;
}

bool AvMuxer::close() {
    if (!ctx_) return true;
    av_write_trailer(ctx_);
    if (!(ctx_->oformat->flags & AVFMT_NOFILE) && ctx_->pb) {
        avio_closep(&ctx_->pb);
    }
    avformat_free_context(ctx_);
    ctx_ = nullptr;
    video_stream_ = system_stream_ = mic_stream_ = -1;
    return true;
}