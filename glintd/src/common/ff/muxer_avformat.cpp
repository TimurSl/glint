#include "muxer_avformat.h"

#include <algorithm>
#include <cctype>
#include <climits>
#include <cstdint>
#include <cstring>
#include <format>
#include <limits>
#include <system_error>

extern "C" {
#include <libavutil/channel_layout.h>
#include <libavutil/samplefmt.h>
#include <libavutil/version.h>
}

#include "logger.h"

namespace {
constexpr AVRational kMsTimeBase{1, 1000};

#if LIBAVUTIL_VERSION_MAJOR >= 57
bool setChannelsNoLayout(AVCodecParameters* params, int channels) {
    if (!params || channels <= 0) {
        return false;
    }
    av_channel_layout_uninit(&params->ch_layout);
    params->ch_layout.order = AV_CHANNEL_ORDER_UNSPEC;
    params->ch_layout.nb_channels = channels;
    return true;
}

bool setDefaultLayout(AVCodecParameters* params, int channels) {
    if (!params || channels <= 0) {
        return false;
    }
    av_channel_layout_default(&params->ch_layout, channels);
    return params->ch_layout.nb_channels == channels;
}
#else
bool setChannelsNoLayout(AVCodecParameters* params, int channels) {
    if (!params || channels <= 0) {
        return false;
    }
    params->channel_layout = 0;
    params->channels = channels;
    return true;
}

bool setDefaultLayout(AVCodecParameters* params, int channels) {
    if (!params || channels <= 0) {
        return false;
    }
    params->channels = channels;
    params->channel_layout = av_get_default_channel_layout(channels);
    return params->channel_layout != 0;
}
#endif

std::string toLower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return value;
}

} // namespace

void MuxerAvFormat::FormatContextDeleter::operator()(AVFormatContext* ctx) const noexcept {
    if (!ctx) {
        return;
    }

    if (!(ctx->oformat->flags & AVFMT_NOFILE) && ctx->pb) {
        avio_closep(&ctx->pb);
    }
    avformat_free_context(ctx);
}

void MuxerAvFormat::PacketDeleter::operator()(AVPacket* pkt) const noexcept {
    av_packet_free(&pkt);
}

MuxerAvFormat::MuxerAvFormat() = default;

MuxerAvFormat::~MuxerAvFormat() {
    close();
}

std::optional<MuxerError> MuxerAvFormat::lastError() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    return last_error_;
}

bool MuxerAvFormat::checkSanity() const noexcept {
    std::scoped_lock<std::mutex> lock(mutex_);
    if (!ctx_) {
        return false;
    }

    if (!(ctx_->oformat->flags & AVFMT_NOFILE) && ctx_->pb == nullptr) {
        return false;
    }

    for (auto type : {EncodedStreamType::Video, EncodedStreamType::SystemAudio, EncodedStreamType::MicrophoneAudio}) {
        const AVStream* stream = streamFor(type);
        if (!stream) {
            continue;
        }
        if (stream->time_base.num <= 0 || stream->time_base.den <= 0) {
            return false;
        }
    }
    return true;
}

int MuxerAvFormat::streamIndex(EncodedStreamType type) noexcept {
    switch (type) {
        case EncodedStreamType::Video: return 0;
        case EncodedStreamType::SystemAudio: return 1;
        case EncodedStreamType::MicrophoneAudio: return 2;
        default: return 0;
    }
}

MuxerAvFormat::StreamState& MuxerAvFormat::stateFor(EncodedStreamType type) noexcept {
    return stream_states_[static_cast<std::size_t>(streamIndex(type))];
}

const MuxerAvFormat::StreamState& MuxerAvFormat::stateFor(EncodedStreamType type) const noexcept {
    return stream_states_[static_cast<std::size_t>(streamIndex(type))];
}

AVStream* MuxerAvFormat::streamFor(EncodedStreamType type) const noexcept {
    if (!ctx_) {
        return nullptr;
    }

    int index = -1;
    switch (type) {
        case EncodedStreamType::Video: index = video_stream_; break;
        case EncodedStreamType::SystemAudio: index = system_stream_; break;
        case EncodedStreamType::MicrophoneAudio: index = mic_stream_; break;
    }

    if (index < 0 || index >= static_cast<int>(ctx_->nb_streams)) {
        return nullptr;
    }
    return ctx_->streams[index];
}

void MuxerAvFormat::resetStateUnlocked() {
    ctx_.reset();
    header_written_ = false;
    header_failed_ = false;
    video_stream_ = -1;
    system_stream_ = -1;
    mic_stream_ = -1;
    pending_packets_.clear();
    last_error_.reset();
    for (auto& state : stream_states_) {
        state = StreamState{};
    }
}

void MuxerAvFormat::setError(MuxerError error) noexcept {
    if (error == MuxerError::None) {
        last_error_.reset();
        return;
    }
    last_error_ = error;
}

bool MuxerAvFormat::open(const MuxerConfig& cfg,
                         const EncoderStreamInfo& video,
                         const EncoderStreamInfo& systemAudio,
                         const EncoderStreamInfo& micAudio) {
    close();

    std::scoped_lock<std::mutex> lock(mutex_);
    resetStateUnlocked();
    config_ = cfg;
    output_path_ = cfg.path;

    if (output_path_.empty()) {
        Logger::instance().error("MuxerAvFormat: output path is empty");
        setError(MuxerError::InvalidConfiguration);
        return false;
    }

    std::error_code ec;
    auto parent = output_path_.parent_path();
    if (!parent.empty()) {
        std::filesystem::create_directories(parent, ec);
        if (ec) {
            Logger::instance().error(std::format("MuxerAvFormat: failed to create directories for {}: {}", parent.string(), ec.message()));
            setError(MuxerError::InvalidConfiguration);
            return false;
        }
    }

    if (!initializeContext(cfg)) {
        return false;
    }

    if (!createStream(video, video_stream_)) {
        return false;
    }

    if (cfg.two_audio_tracks) {
        if (!createStream(systemAudio, system_stream_)) {
            return false;
        }
        if (!createStream(micAudio, mic_stream_)) {
            return false;
        }
    }

    const AVOutputFormat* fmt = ctx_ ? ctx_->oformat : nullptr;
    if (fmt) {
        const AVCodecParameters* vp = streamFor(EncodedStreamType::Video) ? streamFor(EncodedStreamType::Video)->codecpar : nullptr;
        const AVCodecParameters* ap1 = streamFor(EncodedStreamType::SystemAudio) ? streamFor(EncodedStreamType::SystemAudio)->codecpar : nullptr;
        const AVCodecParameters* ap2 = streamFor(EncodedStreamType::MicrophoneAudio) ? streamFor(EncodedStreamType::MicrophoneAudio)->codecpar : nullptr;

        const std::string fmt_name = fmt->name ? fmt->name : "";
        const bool is_mp4 = fmt_name == "mp4" || fmt_name == "mov";
        const bool has_opus = (ap1 && ap1->codec_id == AV_CODEC_ID_OPUS) || (ap2 && ap2->codec_id == AV_CODEC_ID_OPUS);
        if (is_mp4 && has_opus) {
            Logger::instance().warn("MuxerAvFormat: MP4 container with Opus audio is unsupported, aborting");
            setError(MuxerError::InvalidConfiguration);
            return false;
        }
        if (is_mp4 && vp && vp->codec_id == AV_CODEC_ID_H264 && vp->extradata_size == 0) {
            Logger::instance().warn("MuxerAvFormat: MP4 container requires H.264 extradata, waiting for first keyframe to inject");
        }
    }

    av_dump_format(ctx_.get(), 0, reinterpret_cast<const char*>(output_path_.u8string().c_str()), 1);

    setError(MuxerError::None);
    return true;
}

bool MuxerAvFormat::initializeContext(const MuxerConfig& cfg) {
    const std::string path_utf8(reinterpret_cast<const char*>(output_path_.u8string().c_str()));
    const std::string container = determineContainer(cfg, output_path_);

    AVFormatContext* raw_ctx = nullptr;
    int ret = avformat_alloc_output_context2(&raw_ctx, nullptr, container.empty() ? nullptr : container.c_str(), path_utf8.c_str());
    if (ret < 0 || !raw_ctx) {
        logAvError(ret, "MuxerAvFormat: avformat_alloc_output_context2");
        setError(MuxerError::ContextAllocationFailed);
        return false;
    }

    ctx_.reset(raw_ctx);
    ctx_->max_interleave_delta = INT64_MAX;
    ctx_->flags |= AVFMT_FLAG_FLUSH_PACKETS;

    if (!(ctx_->oformat->flags & AVFMT_NOFILE)) {
        AVIOContext* pb = nullptr;
        ret = avio_open2(&pb, path_utf8.c_str(), AVIO_FLAG_WRITE, nullptr, nullptr);
        if (ret < 0) {
            logAvError(ret, "MuxerAvFormat: avio_open2");
            ctx_.reset();
            setError(MuxerError::IoOpenFailed);
            return false;
        }
        ctx_->pb = pb;
    }

    return true;
}

AVRational MuxerAvFormat::sanitizeTimeBase(const EncoderStreamInfo& info, AVRational fallback) noexcept {
    AVRational candidate{info.timebase_num, info.timebase_den};
    return ensureValid(candidate, fallback);
}

AVRational MuxerAvFormat::ensureValid(const AVRational& value, AVRational fallback) noexcept {
    AVRational result = value;
    if (result.num <= 0 || result.den <= 0) {
        result = fallback;
    }
    if (result.num <= 0 || result.den <= 0) {
        result = {1, 1000};
    }
    return result;
}

bool MuxerAvFormat::createStream(const EncoderStreamInfo& info, int& index_out) {
    index_out = -1;
    if (!ctx_) {
        setError(MuxerError::NotOpen);
        return false;
    }

    if (info.codec_name.empty()) {
        return true;
    }

    const AVCodec* codec = avcodec_find_encoder_by_name(info.codec_name.c_str());
    if (!codec) {
        Logger::instance().warn(std::format("MuxerAvFormat: encoder {} not found, falling back to defaults", info.codec_name));
        codec = avcodec_find_encoder(info.type == EncodedStreamType::Video ? AV_CODEC_ID_H264 : AV_CODEC_ID_AAC);
    }

    AVStream* stream = avformat_new_stream(ctx_.get(), codec);
    if (!stream) {
        Logger::instance().error("MuxerAvFormat: failed to create AVStream");
        setError(MuxerError::StreamAllocationFailed);
        return false;
    }

    AVRational fallback = info.type == EncodedStreamType::Video
        ? AVRational{1, info.fps > 0 ? info.fps : 60}
        : AVRational{1, info.sample_rate > 0 ? info.sample_rate : 48000};

    stream->time_base = sanitizeTimeBase(info, fallback);
    stream->avg_frame_rate = (info.fps > 0) ? AVRational{info.fps, 1} : AVRational{0, 1};

    AVCodecParameters* params = stream->codecpar;
    params->codec_type = (info.type == EncodedStreamType::Video) ? AVMEDIA_TYPE_VIDEO : AVMEDIA_TYPE_AUDIO;
    params->codec_id = codec ? codec->id : (info.type == EncodedStreamType::Video ? AV_CODEC_ID_H264 : AV_CODEC_ID_AAC);

    if (info.type == EncodedStreamType::Video) {
        params->width  = info.width > 0 ? info.width : 1920;
        params->height = info.height > 0 ? info.height : 1080;
        params->format = AV_PIX_FMT_YUV420P;

        if (!info.extradata.empty()) {
            params->extradata = (uint8_t*)av_mallocz(info.extradata.size() + AV_INPUT_BUFFER_PADDING_SIZE);
            memcpy(params->extradata, info.extradata.data(), info.extradata.size());
            params->extradata_size = (int)info.extradata.size();

        } else if (!cached_video_extradata_.empty()) {
            params->extradata = (uint8_t*)av_mallocz(cached_video_extradata_.size() + AV_INPUT_BUFFER_PADDING_SIZE);
            memcpy(params->extradata, cached_video_extradata_.data(), cached_video_extradata_.size());
            params->extradata_size = (int)cached_video_extradata_.size();

            Logger::instance().info("MuxerAvFormat: reused cached SPS/PPS extradata for new segment");
        }
    }
    else {
        // audio
        params->sample_rate = info.sample_rate > 0 ? info.sample_rate : 48000;
        setDefaultLayout(params, info.channels > 0 ? info.channels : 2);
    }

    if (!info.extradata.empty()) {
        params->extradata = static_cast<uint8_t*>(av_mallocz(info.extradata.size() + AV_INPUT_BUFFER_PADDING_SIZE));
        if (params->extradata) {
            std::memcpy(params->extradata, info.extradata.data(), info.extradata.size());
            params->extradata_size = static_cast<int>(info.extradata.size());
        }
    }

    stateFor(info.type).fallback_tb = ensureValid(stream->time_base, fallback);

    index_out = stream->index;
    return true;
}

std::vector<uint8_t> MuxerAvFormat::extractH264ExtradataFromAnnexB(const uint8_t* data, int size) {
    std::vector<uint8_t> sps;
    std::vector<uint8_t> pps;
    std::vector<uint8_t> extradata;

    const uint8_t* end = data + size;
    const uint8_t* p = data;
    while (p + 4 <= end) {
        const uint8_t* start_code = p;
        while (start_code + 3 < end && (start_code[0] != 0 || start_code[1] != 0 || (start_code[2] != 1 && (start_code[2] != 0 || start_code[3] != 1)))) {
            ++start_code;
        }
        if (start_code + 3 >= end) {
            break;
        }
        int offset = (start_code[2] == 1) ? 3 : 4;
        const uint8_t* nal = start_code + offset;
        const uint8_t* next = start_code + offset;
        while (next + 3 < end && (next[0] != 0 || next[1] != 0 || (next[2] != 1 && (next[2] != 0 || next[3] != 1)))) {
            ++next;
        }
        uint8_t nal_type = nal[0] & 0x1F;
        int payload_size = static_cast<int>(next - nal);
        if (payload_size > 0) {
            if (nal_type == 7) {
                sps.assign(nal, nal + payload_size);
            } else if (nal_type == 8) {
                pps.assign(nal, nal + payload_size);
            }
        }
        p = next;
    }

    if (!sps.empty() && !pps.empty()) {
        static const uint8_t sc3[3] = {0x00, 0x00, 0x01};
        extradata.insert(extradata.end(), std::begin(sc3), std::end(sc3));
        extradata.insert(extradata.end(), sps.begin(), sps.end());
        extradata.insert(extradata.end(), std::begin(sc3), std::end(sc3));
        extradata.insert(extradata.end(), pps.begin(), pps.end());
    }
    return extradata;
}

void MuxerAvFormat::injectExtradataIfNeeded(const EncodedPacket& packet) const {
    AVStream* stream = streamFor(EncodedStreamType::Video);
    if (!stream) {
        return;
    }
    AVCodecParameters* params = stream->codecpar;
    if (!params || params->codec_id != AV_CODEC_ID_H264 || params->extradata_size > 0) {
        return;
    }

    auto extra = extractH264ExtradataFromAnnexB(packet.data.data(), static_cast<int>(packet.data.size()));
    if (!extra.empty()) {
        params->extradata = static_cast<uint8_t*>(av_mallocz(extra.size() + AV_INPUT_BUFFER_PADDING_SIZE));
        if (params->extradata) {
            std::memcpy(params->extradata, extra.data(), extra.size());
            params->extradata_size = static_cast<int>(extra.size());

            const_cast<MuxerAvFormat*>(this)->cached_video_extradata_ = extra;
            Logger::instance().info("MuxerAvFormat: injected SPS/PPS extradata from first video packet");
        }
    }
}

bool MuxerAvFormat::ensureHeader(const EncodedPacket& packet, AVStream* stream) {
    if (header_written_) {
        return true;
    }
    if (header_failed_) {
        return false;
    }

    if (packet.type == EncodedStreamType::Video) {
        injectExtradataIfNeeded(packet);

        if (stream->codecpar->extradata_size == 0) {
            Logger::instance().warn("MuxerAvFormat: skipping header until SPS/PPS is available");
            return true;
        }
    }

    for (unsigned int i = 0; i < ctx_->nb_streams; ++i) {
        AVStream* s = ctx_->streams[i];
        if (!s || !s->codecpar) {
            Logger::instance().error("MuxerAvFormat: invalid stream state before header write");
            header_failed_ = true;
            setError(MuxerError::HeaderWriteFailed);
            return false;
        }

        if (s->time_base.num == 0 || s->time_base.den == 0) {
            s->time_base = AVRational{1, 1000};
        }
    }



    int ret = avformat_write_header(ctx_.get(), nullptr);
    if (ret < 0) {
        logAvError(ret, "MuxerAvFormat: avformat_write_header");
        header_failed_ = true;
        setError(MuxerError::HeaderWriteFailed);
        return false;
    }


    header_written_ = true;
    Logger::instance().info("MuxerAvFormat: header written to " + output_path_.string());

    return flushPendingPacketsUnlocked();
}

bool MuxerAvFormat::flushPendingPacketsUnlocked() {
    if (pending_packets_.empty()) {
        return true;
    }

    auto pending = std::move(pending_packets_);
    pending_packets_.clear();

    for (const auto& pkt : pending) {
        AVStream* stream = streamFor(pkt.type);
        if (!stream) {
            Logger::instance().warn("MuxerAvFormat: dropping queued packet for unavailable stream");
            continue;
        }
        if (!writePacketUnlocked(pkt, stream)) {
            return false;
        }
    }
    return true;
}

bool MuxerAvFormat::writePacketUnlocked(const EncodedPacket& packet, AVStream* stream) {
    if (!ctx_ || !stream) {
        setError(MuxerError::NotOpen);
        return false;
    }

    if (packet.data.empty()) {
        Logger::instance().warn("MuxerAvFormat: received empty packet");
        return true;
    }

    if (packet.data.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        Logger::instance().error("MuxerAvFormat: packet too large to write");
        setError(MuxerError::InvalidPacket);
        return false;
    }

    if (!(ctx_->oformat->flags & AVFMT_NOFILE) && ctx_->pb == nullptr) {
        Logger::instance().error("MuxerAvFormat: output IO context is not initialized");
        setError(MuxerError::NotOpen);
        return false;
    }

    StreamState& state = stateFor(packet.type);
    const int64_t source_dts = (packet.dts != GLINT_NOPTS_VALUE) ? packet.dts : packet.pts;
    if (source_dts == GLINT_NOPTS_VALUE) {
        Logger::instance().error("MuxerAvFormat: packet missing DTS/PTS");
        setError(MuxerError::InvalidPacket);
        return false;
    }

    if (state.clock.base_ms == GLINT_NOPTS_VALUE) {
        state.clock.base_ms = source_dts;
        state.clock.last_dts_ms = GLINT_NOPTS_VALUE;
    }

    int64_t normalized_dts_ms = std::max<int64_t>(0, source_dts - state.clock.base_ms);
    if (state.clock.last_dts_ms != GLINT_NOPTS_VALUE && normalized_dts_ms <= state.clock.last_dts_ms) {
        normalized_dts_ms = state.clock.last_dts_ms + 1;
    }
    state.clock.last_dts_ms = normalized_dts_ms;

    int64_t pts_source = (packet.pts != GLINT_NOPTS_VALUE) ? packet.pts : source_dts;
    int64_t normalized_pts_ms = std::max<int64_t>(normalized_dts_ms, pts_source - state.clock.base_ms);

    const AVRational stream_tb = ensureValid(stream->time_base, state.fallback_tb);
    const int64_t pkt_dts = av_rescale_q(normalized_dts_ms, kMsTimeBase, stream_tb);
    const int64_t pkt_pts = av_rescale_q(normalized_pts_ms, kMsTimeBase, stream_tb);

    int64_t pkt_duration = 0;
    if (packet.type == EncodedStreamType::Video) {
        pkt_duration = av_rescale_q(1, ensureValid(state.fallback_tb, stream_tb), stream_tb);
        if (pkt_duration <= 0) {
            pkt_duration = 1;
        }
    }

    PacketPtr pkt(av_packet_alloc());
    if (!pkt) {
        Logger::instance().error("MuxerAvFormat: failed to allocate AVPacket");
        setError(MuxerError::OutOfMemory);
        return false;
    }

    pkt->stream_index = stream->index;
    pkt->pts = pkt_pts;
    pkt->dts = pkt_dts;
    pkt->duration = pkt_duration;
    pkt->flags = 0;
    if (packet.keyframe) {
        pkt->flags |= AV_PKT_FLAG_KEY;
    }
    pkt->data = const_cast<uint8_t*>(packet.data.data());
    pkt->size = static_cast<int>(packet.data.size());

    const int ret = av_interleaved_write_frame(ctx_.get(), pkt.get());
    if (ret < 0) {
        logAvError(ret, "MuxerAvFormat: av_interleaved_write_frame");
        setError(MuxerError::PacketWriteFailed);
        return false;
    }

    ++state.packets_written;
    return true;
}

bool MuxerAvFormat::write(const EncodedPacket& packet) {
    std::scoped_lock<std::mutex> lock(mutex_);
    if (!ctx_) {
        Logger::instance().error("MuxerAvFormat: write called before open");
        setError(MuxerError::NotOpen);
        return false;
    }

    AVStream* stream = streamFor(packet.type);
    if (!stream) {
        Logger::instance().warn("MuxerAvFormat: dropping packet for inactive stream");
        setError(MuxerError::InvalidPacket);
        return false;
    }

    if (!header_written_) {
        if (video_stream_ >= 0 && packet.type != EncodedStreamType::Video) {
            pending_packets_.push_back(packet);
            return true;
        }
        if (!ensureHeader(packet, stream)) {
            return false;
        }
        if (!header_written_) {
            pending_packets_.push_back(packet);
            return true;
        }
    }

    return writePacketUnlocked(packet, stream);
}

bool MuxerAvFormat::close() {
    std::scoped_lock<std::mutex> lock(mutex_);
    bool ok = true;

    if (ctx_) {
        if (header_written_) {
            int ret = av_write_trailer(ctx_.get());
            if (ret < 0) {
                logAvError(ret, "MuxerAvFormat: av_write_trailer");
                ok = false;
            }
        }
        if (ctx_->pb) {
            avio_flush(ctx_->pb);
        }
    }

    resetStateUnlocked();
    return ok;
}

void MuxerAvFormat::logAvError(int err, const std::string& context) {
    char buf[256];
    av_strerror(err, buf, sizeof(buf));
    Logger::instance().error(std::format("MuxerAvFormat: {}: {}", context, buf));
}

std::string MuxerAvFormat::determineContainer(const MuxerConfig& cfg, const std::filesystem::path& outputPath) {
    if (!cfg.container.empty()) {
        return cfg.container;
    }

    std::string ext = toLower(outputPath.extension().string());
    if (ext == ".mp4" || ext == ".mov") {
        return "mp4";
    }
    if (ext == ".mkv" || ext == ".matroska") {
        return "matroska";
    }
    return "matroska";
}