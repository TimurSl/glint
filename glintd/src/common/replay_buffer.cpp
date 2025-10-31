#include "replay_buffer.h"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <iomanip>
#include <sstream>
#include <format>

#include "buffer_merger.h"
#include "db.h"
#include "logger.h"

namespace {
std::string sanitize(const std::string& value) {
    std::string result;
    result.reserve(value.size());
    for (char ch : value) {
        if (std::isalnum(static_cast<unsigned char>(ch))) {
            result += ch;
        } else if (ch == ' ' || ch == '-' || ch == '_') {
            result += '_';
        }
    }
    if (result.empty()) {
        result = "session";
    }
    return result;
}

int64_t now_ms() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}
}

ReplayBuffer::ReplayBuffer(Options options)
    : options_(std::move(options)), rolling_enabled_(options_.rolling_mode) {}

void ReplayBuffer::attachRecorder(Recorder* recorder) {
    std::scoped_lock lock(mutex_);
    recorder_ = recorder;
    if (recorder_) {
        recorder_->setSegmentClosedCallback([this](SegmentInfo& info) { onSegmentClosed(info); });
        recorder_->setSegmentRemovedCallback([this](const SegmentInfo& info) { onSegmentRemoved(info); });
        recorder_->setRollingBufferEnabled(rolling_enabled_);
    }
}

void ReplayBuffer::applyOptions(const Options& options) {
    std::scoped_lock lock(mutex_);
    options_ = options;
    rolling_enabled_ = options_.rolling_mode;
    if (recorder_) {
        recorder_->setRollingBufferEnabled(rolling_enabled_);
    }
}

bool ReplayBuffer::start_session(const std::string& game) {
    if (running_) return true;
    std::scoped_lock lock(mutex_);
    running_ = true;
    current_game_ = game;
    session_segments_.clear();
    last_output_path_.clear();
    rolling_enabled_ = options_.rolling_mode;
    if (recorder_) {
        recorder_->setRollingBufferEnabled(rolling_enabled_);
    }
    auto sessionRes = DB::instance().createSession(game, now_ms(), options_.container);
    if (!sessionRes) {
        Logger::instance().error(std::format("ReplayBuffer: failed to create DB session: {}", sessionRes.error()));
        running_ = false;
        return false;
    }
    current_session_id_ = static_cast<int>(sessionRes.value());
    session_directory_ = buildSessionDirectory(current_session_id_);
    if (recorder_) {
        recorder_->beginSession(current_session_id_, session_directory_);
    }
    Logger::instance().info("ReplayBuffer: session started: " + game + " (#" + std::to_string(current_session_id_) + ")");
    return true;
}

void ReplayBuffer::stop_session() {
    int sessionId = -1;
    std::vector<SegmentInfo> segments;
    std::filesystem::path sessionDir;
    std::string game;
    bool rollingAtStop = true;
    {
        std::scoped_lock lock(mutex_);
        if (!running_) return;
        running_ = false;
        sessionId = current_session_id_;
        segments = session_segments_;
        sessionDir = session_directory_;
        game = current_game_;
        rollingAtStop = rolling_enabled_;
    }

    const int64_t stopped_at = now_ms();

    std::vector<SegmentInfo> validSegments;
    validSegments.reserve(segments.size());
    for (const auto& seg : segments) {
        if (seg.path.empty() || seg.end_ms <= seg.start_ms) {
            continue;
        }
        std::error_code ec;
        if (!std::filesystem::exists(seg.path, ec)) {
            Logger::instance().warn(std::format("ReplayBuffer: missing segment {}", seg.path.string()));
            continue;
        }
        validSegments.push_back(seg);
    }

    const bool hasSegments = !validSegments.empty();
    const bool shouldMerge = hasSegments && !rollingAtStop;

    std::filesystem::path outputPath;
    bool merged = false;
    if (shouldMerge && sessionId >= 0) {
        outputPath = buildOutputPath(game);
        BufferMerger merger(options_.temp_directory);
        merged = merger.merge(sessionId, validSegments, outputPath);
        if (merged) {
            Logger::instance().info(std::format("ReplayBuffer: merged session {} into {}", sessionId, outputPath.string()));
        } else {
            Logger::instance().warn(std::format("ReplayBuffer: merge failed for session {}", sessionId));
            outputPath.clear();
        }
    } else if (sessionId >= 0 && !hasSegments) {
        Logger::instance().warn(std::format("ReplayBuffer: no segments recorded for session {}", sessionId));
    }

    if (sessionId >= 0) {
        auto finalizeRes = DB::instance().finalizeSession(sessionId, stopped_at, merged ? outputPath.string() : std::string{});
        if (!finalizeRes) {
            Logger::instance().error(std::format("ReplayBuffer: failed to finalize session {}: {}", sessionId, finalizeRes.error()));
        }
    }

    if (!shouldMerge && sessionId >= 0 && hasSegments) {
        Logger::instance().info(std::format("ReplayBuffer: session {} ended with {} buffered segments", sessionId, validSegments.size()));
    }

    cleanupChunks(validSegments, sessionDir, merged && !rollingAtStop);

    {
        std::scoped_lock lock(mutex_);
        last_output_path_ = merged ? outputPath : std::filesystem::path{};
        session_segments_.clear();
        current_session_id_ = -1;
        session_directory_.clear();
        current_game_.clear();
    }
}

bool ReplayBuffer::export_last_clip(const std::filesystem::path& path) {
    std::scoped_lock lock(mutex_);
    if (last_output_path_.empty()) {
        Logger::instance().warn("ReplayBuffer: no clip available to export");
        return false;
    }
    try {
        std::filesystem::create_directories(path.parent_path());
        std::filesystem::copy_file(last_output_path_, path, std::filesystem::copy_options::overwrite_existing);
        Logger::instance().info(std::format("ReplayBuffer: exported clip to {}", path.string()));
        return true;
    } catch (const std::exception& ex) {
        Logger::instance().error(std::format("ReplayBuffer: export failed: {}", ex.what()));
        return false;
    }
}

bool ReplayBuffer::export_last_clip(const std::string& path) {
    return export_last_clip(std::filesystem::path(path));
}

bool ReplayBuffer::is_running() const { return running_.load(); }

void ReplayBuffer::setRollingBufferEnabled(bool enabled) {
    std::scoped_lock lock(mutex_);
    rolling_enabled_ = enabled;
    options_.rolling_mode = enabled;
    if (recorder_) {
        recorder_->setRollingBufferEnabled(enabled);
    }
}


void ReplayBuffer::onSegmentClosed(SegmentInfo& info) {
    std::scoped_lock lock(mutex_);
    if (current_session_id_ < 0) return;
    std::optional<int64_t> keyframe = info.keyframe_ms > 0 ? std::optional<int64_t>(info.keyframe_ms) : std::nullopt;
    auto chunkRes = DB::instance().insertChunk(current_session_id_, info.path.string(), info.start_ms, info.end_ms, keyframe);
    if (!chunkRes) {
        info.chunk_id = -1;
        Logger::instance().warn(std::format("ReplayBuffer: failed to record chunk {}: {}", info.path.string(), chunkRes.error()));
    } else {
        info.chunk_id = chunkRes.value();
    }
    session_segments_.push_back(info);
}

void ReplayBuffer::onSegmentRemoved(const SegmentInfo& info) {
    std::scoped_lock lock(mutex_);
    auto it = std::find_if(session_segments_.begin(), session_segments_.end(),
                           [&](const SegmentInfo& seg) { return seg.path == info.path; });
    if (it != session_segments_.end()) {
        if (it->chunk_id >= 0) {
            if (auto res = DB::instance().removeChunk(it->chunk_id); !res) {
                Logger::instance().warn(std::format("ReplayBuffer: failed to remove chunk {}: {}", it->chunk_id, res.error()));
            }
        }
        session_segments_.erase(it);
    }
}

void ReplayBuffer::cleanupChunks(const std::vector<SegmentInfo>& segments,
                                 const std::filesystem::path& directory,
                                 bool deleteFiles) {
    for (const auto& seg : segments) {
        if (seg.chunk_id >= 0) {
            if (auto res = DB::instance().removeChunk(seg.chunk_id); !res) {
                Logger::instance().warn(std::format("ReplayBuffer: failed to remove chunk {}: {}", seg.chunk_id, res.error()));
            }
        }
        if (deleteFiles) {
            std::error_code ec;
            std::filesystem::remove(seg.path, ec);
            if (ec) {
                Logger::instance().warn(std::format("ReplayBuffer: failed to remove {}: {}", seg.path.string(), ec.message()));
            }
        }
    }
    if (deleteFiles && !directory.empty()) {
        std::error_code ec;
        std::filesystem::remove_all(directory, ec);
        if (ec) {
            Logger::instance().warn(std::format("ReplayBuffer: failed to remove directory {}: {}", directory.string(), ec.message()));
        }
    }
}

std::filesystem::path ReplayBuffer::buildSessionDirectory(int sessionId) const {
    std::filesystem::path root = options_.segment_root;
    root /= "session_" + std::to_string(sessionId);
    return root;
}

std::filesystem::path ReplayBuffer::buildOutputPath(const std::string& game) const {
    auto now = std::chrono::system_clock::now();
    auto tt = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &tt);
#else
    localtime_r(&tt, &tm);
#endif
    std::ostringstream name;
    name << sanitize(game) << '_' << std::put_time(&tm, "%Y%m%d_%H%M%S") << ".mp4";
    return options_.output_directory / name.str();
}