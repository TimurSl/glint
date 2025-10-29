#include "muxer_avformat.h"

#include <filesystem>
#include <cstring>

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

bool AvMuxer::addStream(const EncoderStreamInfo& info, int& index_out) {
    if (info.codec_name.empty()) { index_out = -1; return true; }


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
        params->format = AV_PIX_FMT_NV12;
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

        params->format = AV_SAMPLE_FMT_FLTP;
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

bool AvMuxer::open(const MuxerConfig& cfg,
                   const EncoderStreamInfo& video,
                   const EncoderStreamInfo& systemAudio,
                   const EncoderStreamInfo& micAudio) {
    close();
    config_ = cfg;


    if (avformat_alloc_output_context2(&ctx_, nullptr, cfg.container.c_str(), cfg.path.c_str()) < 0 || !ctx_) {
        Logger::instance().error("AvMuxer: failed to allocate output context");
        return false;
    }

    if (!addStream(video, video_stream_)) return false;
    if (cfg.two_audio_tracks) {
        addStream(systemAudio, system_stream_);
        addStream(micAudio, mic_stream_);
    }

    if (!(ctx_->oformat->flags & AVFMT_NOFILE)) {
        if (avio_open(&ctx_->pb, cfg.path.c_str(), AVIO_FLAG_WRITE) < 0) {
            Logger::instance().error("AvMuxer: could not open output file");
            return false;
        }
    }
    Logger::instance().info("Muxer open: video=" + std::to_string(video_stream_) +
                        " sys=" + std::to_string(system_stream_) +
                        " mic=" + std::to_string(mic_stream_));

    for (unsigned i = 0; i < ctx_->nb_streams; ++i) {
        AVStream* s = ctx_->streams[i];
        auto* p = s->codecpar;
        Logger::instance().info("  Stream[" + std::to_string(i) + "]: codec=" + std::to_string(p->codec_id) +
                                " type=" + std::to_string(p->codec_type) +
                                " sr=" + std::to_string(p->sample_rate) +
                                " ch=" + std::to_string(p->ch_layout.nb_channels) +
                                " extradata=" + std::to_string(p->extradata_size));
    }


    int ret = avformat_write_header(ctx_, nullptr);
    if (ret < 0) {
        char err[256];
        av_strerror(ret, err, sizeof(err));
        Logger::instance().error(std::string("AvMuxer: write header failed: ") + err);
        return false;
    }
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

bool AvMuxer::write(const EncodedPacket& packet) {
    if (!ctx_) return false;
    AVStream* stream = streamForType(packet.type);
    if (!stream) return false;

    AVPacket* pkt = av_packet_alloc();
    if (!pkt) return false;
    pkt->data = const_cast<uint8_t*>(packet.data.data());
    pkt->size = static_cast<int>(packet.data.size());
    pkt->pts = av_rescale_q(packet.pts, ms_time_base, stream->time_base);
    pkt->dts = pkt->pts;
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