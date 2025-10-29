#pragma once

#include <atomic>
#include <chrono>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "encoder.h"
#include "frame_types.h"
#include "muxer.h"
#include "replay_buffer.h"

struct RecorderConfig {
    int width{1920};
    int height{1080};
    int fps{60};
    int video_bitrate_kbps{12000};
    std::string video_codec{"h264"};

    int audio_sample_rate{48000};
    int audio_channels{2};
    int audio_bitrate_kbps{192};
    std::string audio_codec{"aac"};

    std::filesystem::path rolling_directory{"buffer"};
    std::filesystem::path recordings_directory{"recordings"};
    std::string segment_prefix{"seg_"};
    std::string segment_extension{".mkv"};

    std::chrono::milliseconds segment_length{std::chrono::milliseconds(2000)};
    std::chrono::milliseconds retention{std::chrono::minutes(5)};
    uint64_t max_total_size_bytes{2ull * 1024ull * 1024ull * 1024ull};
};

struct SegmentInfo {
    std::filesystem::path path;
    int64_t start_ms{0};
    int64_t end_ms{0};
    int64_t keyframe_ms{0};
    uint64_t size_bytes{0};
};

class Recorder {
public:
    Recorder(std::unique_ptr<IEncoder> encoder, std::unique_ptr<IMuxer> muxer);
    ~Recorder();

    bool initialize(const RecorderConfig& config);
    bool start(bool enableRollingBuffer);
    void stop();

    void setRollingBufferEnabled(bool enabled);

    void pushVideoFrame(const VideoFrame& frame);
    void pushAudioFrame(const AudioFrame& frame, bool isMic);

    std::optional<SegmentInfo> exportLastSegment(const std::filesystem::path& destination);

private:
    struct ActiveSegment {
        MuxerConfig muxer_cfg;
        int64_t start_pts{0};
        int64_t last_pts{0};
        int64_t last_keyframe_pts{0};
        std::filesystem::path path;
    };

    bool ensureEncoderOpen();
    bool openNewSegment();
    void closeCurrentSegment();
    void handlePackets(std::vector<EncodedPacket>& packets);
    void rotateIfNeeded(int64_t pts_ms, bool keyframe);
    std::filesystem::path buildSegmentPath(uint32_t index) const;
    void pruneRollingBuffer();

    std::unique_ptr<IEncoder> encoder_;
    std::unique_ptr<IMuxer> muxer_;
    RecorderConfig config_;

    std::mutex mutex_;
    bool initialized_{false};
    bool rolling_enabled_{true};
    std::optional<ActiveSegment> current_segment_;
    std::vector<SegmentInfo> completed_segments_;
    uint32_t segment_index_{0};
    std::atomic<bool> running_{false};
};