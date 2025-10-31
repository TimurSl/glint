#include "recorder.h"

#include <algorithm>
#include <chrono>
#include <ctime>
#include <format>
#include <fstream>
#include <iomanip>
#include <numeric>
#include <sstream>

#include "logger.h"

Recorder::Recorder(std::unique_ptr<IEncoder> encoder, std::unique_ptr<IMuxer> muxer)
    : encoder_(std::move(encoder)), muxer_(std::move(muxer)) {}

Recorder::~Recorder() {
    stop();
}

bool Recorder::initialize(const RecorderConfig& config) {
    std::scoped_lock lock(mutex_);
    config_ = config;
    try {
        std::filesystem::create_directories(config_.buffer_directory);
        std::filesystem::create_directories(config_.recordings_directory);
    } catch (const std::exception& ex) {
        Logger::instance().error(std::string("Recorder: failed creating directories: ") + ex.what());
        return false;
    }

    if (!encoder_) {
        Logger::instance().error("Recorder: encoder missing");
        return false;
    }
    if (!muxer_) {
        Logger::instance().error("Recorder: muxer missing");
        return false;
    }

    if (!encoder_->initVideo(config_.video_codec, config_.width, config_.height,
                              config_.fps, config_.video_bitrate_kbps)) {
        Logger::instance().error("Recorder: failed to init video encoder");
        return false;
    }

    if (config_.enable_system_audio) {
        if (!encoder_->initAudio(config_.audio_codec, config_.audio_sample_rate,
                                  config_.audio_channels, config_.audio_bitrate_kbps, false)) {
            Logger::instance().warn("Recorder: system audio encoder disabled");
        }
    }
    if (config_.enable_microphone_audio) {
        if (!encoder_->initAudio(config_.audio_codec, config_.audio_sample_rate,
                                  config_.audio_channels, config_.audio_bitrate_kbps, true)) {
            Logger::instance().warn("Recorder: microphone audio encoder disabled");
        }
    }

    initialized_ = true;
    return true;
}

void Recorder::beginSession(int sessionId, const std::filesystem::path& sessionDirectory) {
    std::scoped_lock lock(mutex_);
    current_session_id_ = sessionId;
    session_directory_ = sessionDirectory;
    try {
        std::filesystem::create_directories(session_directory_);
    } catch (const std::exception& ex) {
        Logger::instance().error(std::string("Recorder: failed creating session directory: ") + ex.what());
    }
    resetSessionState();
}

bool Recorder::start(bool enableRollingBuffer) {
    std::scoped_lock lock(mutex_);
    if (!initialized_) return false;
    rolling_enabled_ = enableRollingBuffer;
    if (!ensureEncoderOpen()) {
        return false;
    }
    if (current_session_id_ < 0) {
        Logger::instance().warn("Recorder: starting without session id, using default directory");
        session_directory_ = config_.buffer_directory;
        std::filesystem::create_directories(session_directory_);
    }

    segment_index_ = 0;
    completed_segments_.clear();
    buffered_size_bytes_ = 0;

    running_ = true;
    return openNewSegment();
}

void Recorder::stop() {
    std::scoped_lock lock(mutex_);
    if (!running_.exchange(false)) {
        return;
    }

    Logger::instance().info("Recorder: stopping...");

    if (encoder_) {
        std::vector<EncodedPacket> packets;
        encoder_->flush(packets);
        handlePackets(packets);
        encoder_->close();
    }

    closeCurrentSegment();
}

void Recorder::setRollingBufferEnabled(bool enabled) {
    std::scoped_lock lock(mutex_);
    rolling_enabled_ = enabled;
}

void Recorder::setSegmentClosedCallback(SegmentClosedCallback cb) {
    std::scoped_lock lock(mutex_);
    segment_closed_cb_ = std::move(cb);
}

void Recorder::setSegmentRemovedCallback(SegmentRemovedCallback cb) {
    std::scoped_lock lock(mutex_);
    segment_removed_cb_ = std::move(cb);
}

void Recorder::pushVideoFrame(const VideoFrame& frame) {
    std::scoped_lock lock(mutex_);
    if (!running_ || !encoder_ || !current_segment_) return;

    if (!encoder_->pushVideoRGBA(frame.data.data(), frame.width, frame.height, frame.stride, frame.pts_ms)) {
        Logger::instance().error("Recorder: failed to push video frame");
        return;
    }
    std::vector<EncodedPacket> packets;
    encoder_->pull(packets);
    handlePackets(packets);
}

void Recorder::pushAudioFrame(const AudioFrame& frame, bool isMic) {
    std::scoped_lock lock(mutex_);
    if (!running_ || !encoder_ || !current_segment_) return;

    if (!encoder_->pushAudioF32(frame.interleaved.data(), frame.samples,
                                frame.sample_rate, frame.channels, frame.pts_ms, isMic)) {
        return;
    }
    std::vector<EncodedPacket> packets;
    encoder_->pull(packets);
    handlePackets(packets);
}
std::optional<SegmentInfo> Recorder::exportLastSegment(const std::filesystem::path& destination) {
    std::scoped_lock lock(mutex_);
    if (completed_segments_.empty()) {
        return std::nullopt;
    }
    const auto& last = completed_segments_.back();
    try {
        std::filesystem::create_directories(destination.parent_path());
        std::filesystem::copy_file(last.path, destination, std::filesystem::copy_options::overwrite_existing);
        return last;
    } catch (const std::exception& ex) {
        Logger::instance().error(std::string("Recorder: failed exporting clip: ") + ex.what());
        return std::nullopt;
    }
}

bool Recorder::ensureEncoderOpen() {
    if (!encoder_->open()) {
        Logger::instance().error("Recorder: encoder open failed");
        return false;
    }
    return true;
}

bool Recorder::openNewSegment() {
    ActiveSegment seg{};
    seg.muxer_cfg.container = config_.container;
    seg.muxer_cfg.two_audio_tracks = config_.enable_system_audio || config_.enable_microphone_audio;
    std::filesystem::path filePath = buildSegmentPath(segment_index_++);
    std::error_code ec;
    std::filesystem::create_directories(filePath.parent_path(), ec);
    if (ec) {
        Logger::instance().error("Recorder: failed to create segment directory: " + ec.message());
    }
    seg.muxer_cfg.path = filePath;
    seg.start_pts = 0;
    seg.last_pts = 0;
    seg.last_keyframe_pts = 0;
    seg.path = seg.muxer_cfg.path;

    auto videoInfo = encoder_->videoStream();
    EncoderStreamInfo sysInfo = config_.enable_system_audio ? encoder_->audioStream(false) : EncoderStreamInfo{};
    sysInfo.type = EncodedStreamType::SystemAudio;
    if (!config_.enable_system_audio) {
        sysInfo.codec_name.clear();
    }
    EncoderStreamInfo micInfo = config_.enable_microphone_audio ? encoder_->audioStream(true) : EncoderStreamInfo{};
    micInfo.type = EncodedStreamType::MicrophoneAudio;
    if (!config_.enable_microphone_audio) {
        micInfo.codec_name.clear();
    }

    if (!muxer_->open(seg.muxer_cfg, videoInfo, sysInfo, micInfo)) {
        Logger::instance().error("Recorder: muxer open failed");
        return false;
    }

    current_segment_ = seg;
    rotate_pending_ = false;
    return true;
}

void Recorder::closeCurrentSegment() {
    if (!current_segment_) return;
    muxer_->close();
    auto path = current_segment_->path;
    uint64_t size = 0;
    try {
        size = std::filesystem::file_size(path);
    } catch (...) {
        size = 0;
    }

    if (size > 0 || current_segment_->last_pts > current_segment_->start_pts) {
        SegmentInfo info;
        info.path = path;
        info.start_ms = current_segment_->start_pts;
        info.end_ms = current_segment_->last_pts;
        info.keyframe_ms = current_segment_->last_keyframe_pts;
        info.size_bytes = size;
        completed_segments_.push_back(info);
        buffered_size_bytes_ += size;

        Logger::instance().info(std::format(
            "Recorder: closed segment {} (size={} bytes, start={}ms, end={}ms, keyframe={}ms)",
            info.path.string(), info.size_bytes, info.start_ms, info.end_ms, info.keyframe_ms
        ));

        if (segment_closed_cb_) {
            segment_closed_cb_(completed_segments_.back());
        }
        if (rolling_enabled_) {
            pruneRollingBuffer();
        }
    }
    else {
        Logger::instance().warn(std::format(
            "Recorder: segment {} discarded (empty)", path.string()
        ));
    }
    current_segment_.reset();
}

void Recorder::handlePackets(std::vector<EncodedPacket>& packets) {
    for (auto& packet : packets) {
        if (!current_segment_) break;

        if (rotate_pending_ && packet.type == EncodedStreamType::Video && packet.keyframe) {
            Logger::instance().info("Recorder: rotating segment on keyframe");
            closeCurrentSegment();
            openNewSegment();
            rotate_pending_ = false;
            if (current_segment_) {
                current_segment_->start_pts = packet.pts;
            }
        }

        if (!muxer_->write(packet)) {
            if (auto error = muxer_->lastError()) {
                Logger::instance().error(std::format("Recorder: muxer write failed ({})", static_cast<int>(*error)));
            } else {
                Logger::instance().error("Recorder: muxer write failed");
            }
            continue;
        }

        if (current_segment_->start_pts == 0) {
            current_segment_->start_pts = packet.pts;
        }
        current_segment_->last_pts = std::max(current_segment_->last_pts, packet.pts);
        if (packet.type == EncodedStreamType::Video && packet.keyframe) {
            current_segment_->last_keyframe_pts = packet.pts;
        }

        rotateIfNeeded(packet.pts, packet.keyframe);
    }
}

void Recorder::rotateIfNeeded(int64_t pts_ms, bool /*keyframe*/) {
    if (!current_segment_) return;

    const auto duration = pts_ms - current_segment_->start_pts;
    const bool timeLimit = duration >= config_.segment_length.count();

    std::error_code ec;
    uint64_t size = 0;
    if (std::filesystem::exists(current_segment_->path, ec))
        size = std::filesystem::file_size(current_segment_->path, ec);

    const bool sizeLimit = size >= config_.rolling_size_limit_bytes;

    if ((timeLimit || sizeLimit) && !rotate_pending_) {
        rotate_pending_ = true;
        Logger::instance().debug("Recorder: rotation scheduled, waiting for next keyframe...");
    }
}
std::filesystem::path Recorder::buildSegmentPath(uint32_t index) const {
    std::filesystem::path base = session_directory_.empty() ? config_.buffer_directory : session_directory_;
    std::ostringstream oss;
    oss << config_.segment_prefix << std::setw(8) << std::setfill('0') << index << config_.segment_extension;
    return base / oss.str();
}

void Recorder::pruneRollingBuffer() {
    while (!completed_segments_.empty() && buffered_size_bytes_ > config_.rolling_size_limit_bytes) {
        auto seg = completed_segments_.front();
        try {
            std::filesystem::remove(seg.path);
        } catch (...) {
        }
        if (segment_removed_cb_) {
            segment_removed_cb_(seg);
        }
        buffered_size_bytes_ = buffered_size_bytes_ > seg.size_bytes ? buffered_size_bytes_ - seg.size_bytes : 0;
        completed_segments_.erase(completed_segments_.begin());
    }
}

void Recorder::resetSessionState() {
    segment_index_ = 0;
    completed_segments_.clear();
    buffered_size_bytes_ = 0;
    current_segment_.reset();
}