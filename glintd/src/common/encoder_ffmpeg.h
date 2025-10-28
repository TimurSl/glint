#pragma once
#include "ffmpeg_common.h"
#include <vector>
#include <memory>

struct VideoEncoder {
    AVCodecContext* ctx = nullptr;
    const AVCodec* codec = nullptr;
    AVFrame* frame = nullptr;
    SwsContext* sws = nullptr;
    AVRational time_base{1, 1000}; // input PTS в мс
    int src_w = 0, src_h = 0;
    AVPixelFormat src_fmt = AV_PIX_FMT_BGRA; // из DXGI
    AVPixelFormat enc_fmt = AV_PIX_FMT_NV12; // для NVENC/VAAPI пойдет

    bool open(int w, int h, int fps, int bitrate_kbps, const std::string& codec_name = "h264", bool prefer_hw = true);
    bool convert_and_send(const uint8_t* const bgra, int stride, int64_t pts_ms);
    bool flush();
    void close();
};

struct AudioEncoder {
    AVCodecContext* ctx = nullptr;
    const AVCodec* codec = nullptr;
    SwrContext* swr = nullptr;
    AVFrame* frame = nullptr;
    AVRational time_base{1, 48000}; // samples
    AVSampleFormat in_fmt = AV_SAMPLE_FMT_FLT; // WASAPI float32
    AVSampleFormat enc_fmt = AV_SAMPLE_FMT_FLTP;
    int in_sr = 48000;
    int in_ch = 2;
    int frame_samples = 960; // 20ms @48k для Opus

    bool open(const std::string& codec_name, int sample_rate, int channels, int bitrate_kbps);
    // interleaved float32 input (nsamples*channels)
    bool convert_and_send(const float* interleaved, int nsamples, int64_t pts_samples);
    bool flush();
    void close();
};

struct Muxer {
    AVFormatContext* fmt = nullptr;
    AVStream* vstream = nullptr;
    std::vector<AVStream*> astreams;
    std::string path;

    bool open(const std::string& filepath, AVCodecContext* vctx, const std::vector<AVCodecContext*>& actxs, const std::string& container = "matroska");
    bool write(AVPacket* pkt, int stream_index);
    bool close(bool write_trailer=true);
};
