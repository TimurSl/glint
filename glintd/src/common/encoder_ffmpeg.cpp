#include "encoder_ffmpeg.h"
#include <filesystem>
using namespace std;

bool VideoEncoder::open(int w, int h, int fps, int bitrate_kbps, const string& codec_name, bool prefer_hw) {
    ff_init();
    src_w = w; src_h = h;

#ifdef GLINT_ENABLE_NVENC
    string name = codec_name == "hevc" ? "hevc_nvenc" : "h264_nvenc";
#else
    string name = codec_name == "hevc" ? "libx265" : "libx264";
#endif
#ifndef _WIN32
    if (prefer_hw) {

    }
#endif
    codec = avcodec_find_encoder_by_name(name.c_str());
    if (!codec) codec = avcodec_find_encoder_by_name(codec_name=="hevc"?"libx265":"libx264");
    if (!codec) throw runtime_error("Video codec not found: " + name);

    ctx = avcodec_alloc_context3(codec);
    if (!ctx) throw bad_alloc();
    ctx->width = w;
    ctx->height = h;
    ctx->time_base = AVRational{1, fps};
    ctx->framerate = AVRational{fps, 1};
    ctx->pix_fmt = enc_fmt;
    ctx->gop_size = fps * 2;
    ctx->max_b_frames = 0;
    ctx->bit_rate = (int64_t)bitrate_kbps * 1000;

#ifdef GLINT_ENABLE_NVENC
    av_opt_set(ctx->priv_data, "preset", "p5", 0); // NVENC HQ
    av_opt_set(ctx->priv_data, "rc", "vbr", 0);
#endif

    int rc = avcodec_open2(ctx, codec, nullptr);
    if (rc < 0) throw runtime_error("avcodec_open2 video: " + ff_errstr(rc));

    sws = sws_getContext(w, h, src_fmt, w, h, enc_fmt, SWS_BILINEAR, nullptr, nullptr, nullptr);

    frame = av_frame_alloc();
    frame->format = enc_fmt;
    frame->width = w;
    frame->height = h;
    av_frame_get_buffer(frame, 32);

    time_base = AVRational{1, 1000};
    return true;
}

bool VideoEncoder::convert_and_send(const uint8_t* const bgra, int stride, int64_t pts_ms) {
    // src frame wrapper
    uint8_t* src_data[4] = {(uint8_t*)bgra, nullptr, nullptr, nullptr};
    int src_linesize[4] = {stride, 0, 0, 0};

    AVFrame* tmp = av_frame_alloc();
    tmp->format = enc_fmt;
    tmp->width = frame->width;
    tmp->height = frame->height;
    av_frame_get_buffer(tmp, 32);
    sws_scale(sws, src_data, src_linesize, 0, src_h, tmp->data, tmp->linesize);

    tmp->pts = av_rescale_q(pts_ms, AVRational{1,1000}, ctx->time_base);
    int rc = avcodec_send_frame(ctx, tmp);
    av_frame_free(&tmp);
    if (rc < 0) return false;
    return true;
}

bool VideoEncoder::flush() {
    int rc = avcodec_send_frame(ctx, nullptr);
    return rc >= 0;
}
void VideoEncoder::close() {
    if (frame) { av_frame_free(&frame); frame=nullptr; }
    if (sws) { sws_freeContext(sws); sws=nullptr; }
    if (ctx) { avcodec_free_context(&ctx); ctx=nullptr; }
}

bool AudioEncoder::open(const string& codec_name, int sample_rate, int channels, int bitrate_kbps) {
    ff_init();
    in_sr = sample_rate; in_ch = channels;

    string name = (codec_name=="aac")?"aac":"libopus";
    codec = avcodec_find_encoder_by_name(name.c_str());
    if (!codec) throw runtime_error("Audio codec not found: " + name);

    ctx = avcodec_alloc_context3(codec);
    ctx->sample_rate = sample_rate;
    ctx->ch_layout = (AVChannelLayout)AV_CHANNEL_LAYOUT_STEREO;
    ctx->channels = channels;
    ctx->time_base = AVRational{1, sample_rate};
    ctx->sample_fmt = codec->sample_fmts ? codec->sample_fmts[0] : AV_SAMPLE_FMT_FLTP;
    enc_fmt = ctx->sample_fmt;
    ctx->bit_rate = (int64_t)bitrate_kbps * 1000;

    if (string(codec->name).find("opus")!=string::npos) {
        av_opt_set_int(ctx, "application", 2049/*audio*/, 0);
    }

    int rc = avcodec_open2(ctx, codec, nullptr);
    if (rc < 0) throw runtime_error("avcodec_open2 audio: " + ff_errstr(rc));

    swr = swr_alloc_set_opts2(nullptr,
          &ctx->ch_layout, enc_fmt, sample_rate,
          &ctx->ch_layout, in_fmt,  sample_rate, 0, nullptr);
    swr_init(swr);

    frame = av_frame_alloc();
    frame_samples = ctx->frame_size ? ctx->frame_size : 960;
    frame->nb_samples = frame_samples;
    frame->format = enc_fmt;
    frame->sample_rate = sample_rate;
    av_channel_layout_copy(&frame->ch_layout, &ctx->ch_layout);
    av_frame_get_buffer(frame, 0);
    return true;
}

bool AudioEncoder::convert_and_send(const float* interleaved, int nsamples, int64_t pts_samples) {
    // interleaved float32 -> planar enc_fmt
    const uint8_t* in_data[1] = {(const uint8_t*)interleaved};
    int in_linesize[1] = { nsamples * (int)sizeof(float) * in_ch };

    int consumed = 0;
    while (consumed < nsamples) {
        int chunk = std::min(frame_samples, nsamples - consumed);
        const float* ptr = interleaved + consumed * in_ch;

        uint8_t* out_data[AV_NUM_DATA_POINTERS]{};
        for (int i=0;i<AV_NUM_DATA_POINTERS;i++) out_data[i]=frame->data[i];

        int rc = swr_convert(swr,
                             out_data, frame_samples,
                             &((const uint8_t*)ptr), chunk);
        if (rc < 0) return false;

        frame->pts = av_rescale_q(pts_samples + consumed, AVRational{1, in_sr}, ctx->time_base);
        rc = avcodec_send_frame(ctx, frame);
        if (rc < 0) return false;

        consumed += chunk;
    }
    return true;
}

bool AudioEncoder::flush() {
    int rc = avcodec_send_frame(ctx, nullptr);
    return rc >= 0;
}

void AudioEncoder::close() {
    if (frame) { av_frame_free(&frame); frame=nullptr; }
    if (swr) { swr_free(&swr); swr=nullptr; }
    if (ctx) { avcodec_free_context(&ctx); ctx=nullptr; }
}

bool Muxer::open(const string& filepath, AVCodecContext* vctx, const vector<AVCodecContext*>& actxs, const string& container) {
    ff_init();
    path = filepath;

    int rc = avformat_alloc_output_context2(&fmt, nullptr, container.c_str(), filepath.c_str());
    if (rc < 0 || !fmt) throw runtime_error("alloc_output_context: " + ff_errstr(rc));

    if (vctx) {
        vstream = avformat_new_stream(fmt, nullptr);
        avcodec_parameters_from_context(vstream->codecpar, vctx);
        vstream->time_base = vctx->time_base;
    }

    for (auto* actx : actxs) {
        AVStream* as = avformat_new_stream(fmt, nullptr);
        avcodec_parameters_from_context(as->codecpar, actx);
        as->time_base = actx->time_base;
        astreams.push_back(as);
    }

    if (!(fmt->oformat->flags & AVFMT_NOFILE)) {
        rc = avio_open(&fmt->pb, filepath.c_str(), AVIO_FLAG_WRITE);
        if (rc < 0) throw runtime_error("avio_open: " + ff_errstr(rc));
    }
    rc = avformat_write_header(fmt, nullptr);
    if (rc < 0) throw runtime_error("write_header: " + ff_errstr(rc));

    return true;
}

bool Muxer::write(AVPacket* pkt, int stream_index) {
    if (!fmt) return false;
    pkt->stream_index = stream_index;
    return av_interleaved_write_frame(fmt, pkt) >= 0;
}

bool Muxer::close(bool write_trailer) {
    if (!fmt) return true;
    if (write_trailer) av_write_trailer(fmt);
    if (!(fmt->oformat->flags & AVFMT_NOFILE) && fmt->pb) avio_closep(&fmt->pb);
    avformat_free_context(fmt);
    fmt = nullptr; vstream=nullptr; astreams.clear();
    return true;
}
