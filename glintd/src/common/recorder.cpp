#include "recorder.h"

#include <algorithm>
#include <chrono>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <numeric>
#include <sstream>

#include "db.h"
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
        std::filesystem::create_directories(config_.rolling_directory);
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

    if (!encoder_->initAudio(config_.audio_codec, config_.audio_sample_rate,
                              config_.audio_channels, config_.audio_bitrate_kbps, false)) {
        Logger::instance().warn("Recorder: system audio encoder disabled");
    }
    if (!encoder_->initAudio(config_.audio_codec, config_.audio_sample_rate,
                              config_.audio_channels, config_.audio_bitrate_kbps, true)) {
        Logger::instance().warn("Recorder: microphone audio encoder disabled");
    }

    initialized_ = true;
    return true;
}

bool Recorder::start(bool enableRollingBuffer) {
    std::scoped_lock lock(mutex_);
    if (!initialized_) return false;
    rolling_enabled_ = enableRollingBuffer;
    if (!ensureEncoderOpen()) {
        return false;
    }

    segment_index_ = 0;
    completed_segments_.clear();

    running_ = true;
    return openNewSegment();
}

void Recorder::stop() {
    std::scoped_lock lock(mutex_);
    if (!running_.exchange(false)) {
        return;
    }

    closeCurrentSegment();
    if (encoder_) {
        std::vector<EncodedPacket> packets;
        encoder_->flush(packets);
        handlePackets(packets);
        encoder_->close();
    }
}

void Recorder::setRollingBufferEnabled(bool enabled) {
    std::scoped_lock lock(mutex_);
    rolling_enabled_ = enabled;
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
    seg.muxer_cfg.container = "matroska";
    seg.muxer_cfg.two_audio_tracks = true;
    std::filesystem::path filePath;
    if (rolling_enabled_) {
        filePath = buildSegmentPath(segment_index_++);
    } else {
        auto now = std::chrono::system_clock::now();
        auto time = std::chrono::system_clock::to_time_t(now);
        std::tm tm{};
#ifdef _WIN32
        localtime_s(&tm, &time);
#else
        localtime_r(&time, &tm);
#endif
        std::ostringstream oss;
        oss << "recording_" << std::put_time(&tm, "%Y%m%d_%H%M%S") << config_.segment_extension;
        filePath = config_.recordings_directory / oss.str();
    }
    seg.muxer_cfg.path = filePath.string();
    seg.start_pts = 0;
    seg.last_pts = 0;
    seg.last_keyframe_pts = 0;
    seg.path = seg.muxer_cfg.path;

    auto videoInfo = encoder_->videoStream();
    auto sysInfo = encoder_->audioStream(false);
    auto micInfo = encoder_->audioStream(true);

    if (!muxer_->open(seg.muxer_cfg, videoInfo, sysInfo, micInfo)) {
        Logger::instance().error("Recorder: muxer open failed");
        return false;
    }

    current_segment_ = seg;
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

    SegmentInfo info;
    info.path = path;
    info.start_ms = current_segment_->start_pts;
    info.end_ms = current_segment_->last_pts;
    info.keyframe_ms = current_segment_->last_keyframe_pts;
    info.size_bytes = size;
    completed_segments_.push_back(info);
    DB::instance().insertChunk(0, info.path.string(), info.start_ms, info.end_ms, info.keyframe_ms, info.size_bytes);
    if (rolling_enabled_) {
        pruneRollingBuffer();
    }
    current_segment_.reset();
}

void Recorder::handlePackets(std::vector<EncodedPacket>& packets) {
    for (auto& packet : packets) {
        if (!current_segment_) break;
        muxer_->write(packet);
        if (current_segment_->start_pts == 0) {
            current_segment_->start_pts = packet.pts;
        }
        current_segment_->last_pts = std::max(current_segment_->last_pts, packet.pts);
        if (packet.type == EncodedStreamType::Video && packet.keyframe) {
            current_segment_->last_keyframe_pts = packet.pts;
            rotateIfNeeded(packet.pts, true);
        } else {
            rotateIfNeeded(packet.pts, false);
        }
    }
}

void Recorder::rotateIfNeeded(int64_t pts_ms, bool keyframe) {
    if (!rolling_enabled_) {
        return;
    }
    if (!current_segment_) return;
    auto elapsed = pts_ms - current_segment_->start_pts;
    if (elapsed >= config_.segment_length.count() && keyframe) {
        closeCurrentSegment();
        openNewSegment();
        if (current_segment_) {
            current_segment_->start_pts = pts_ms;
        }
    }
}

std::filesystem::path Recorder::buildSegmentPath(uint32_t index) const {
    std::ostringstream oss;
    oss << config_.segment_prefix << std::setw(8) << std::setfill('0') << index << config_.segment_extension;
    return config_.rolling_directory / oss.str();
}

void Recorder::pruneRollingBuffer() {
    uint64_t total_size = 0;
    for (const auto& info : completed_segments_) {
        total_size += info.size_bytes;
    }
    auto total_duration = completed_segments_.empty() ? 0 :
                          (completed_segments_.back().end_ms - completed_segments_.front().start_ms);

    bool removed = false;
    while (!completed_segments_.empty() &&
           (total_size > config_.max_total_size_bytes || total_duration > config_.retention.count())) {
        const auto seg = completed_segments_.front();
        try {
            std::filesystem::remove(seg.path);
        } catch (...) {
        }
        total_size -= seg.size_bytes;
        completed_segments_.erase(completed_segments_.begin());
        total_duration = completed_segments_.empty() ? 0 :
                         (completed_segments_.back().end_ms - completed_segments_.front().start_ms);
        removed = true;
    }

    if (removed) {
        Logger::instance().info("Recorder: rolling buffer pruned");
    }
}
