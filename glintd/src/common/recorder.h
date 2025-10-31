#pragma once

#include <atomic>
#include <chrono>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "encoder.h"
#include "frame_types.h"
#include "muxer.h"

struct RecorderConfig {
    int width{1920};
    int height{1080};
    int fps{60};
    int video_bitrate_kbps{12000};
    std::string video_codec{"h264_nvenc"};
    std::string video_encoder{"auto"};

    int audio_sample_rate{48000};
    int audio_channels{2};
    int audio_bitrate_kbps{192};
    std::string audio_codec{"aac"};
    bool enable_system_audio{true};
    bool enable_microphone_audio{true};
    std::string microphone_device{"default"};

    std::filesystem::path buffer_directory{"buffer"};
    std::filesystem::path recordings_directory{"recordings"};
    std::string segment_prefix{"seg_"};
    std::string segment_extension{".mkv"};
    std::string container{"matroska"};

    std::chrono::milliseconds segment_length{std::chrono::milliseconds(2000)};
    uint64_t rolling_size_limit_bytes{100ull * 1024ull * 1024ull};
};

struct SegmentInfo {
    std::filesystem::path path;
    int64_t start_ms{0};
    int64_t end_ms{0};
    int64_t keyframe_ms{0};
    uint64_t size_bytes{0};
    int64_t chunk_id{-1};
};

class Recorder {
public:
    using SegmentClosedCallback = std::function<void(SegmentInfo&)>;
    using SegmentRemovedCallback = std::function<void(const SegmentInfo&)>;

    Recorder(std::unique_ptr<IEncoder> encoder, std::unique_ptr<IMuxer> muxer);
    ~Recorder();

    bool initialize(const RecorderConfig& config);
    void beginSession(int sessionId, const std::filesystem::path& sessionDirectory);
    bool start(bool enableRollingBuffer);
    void stop();

    void setRollingBufferEnabled(bool enabled);
    void setSegmentClosedCallback(SegmentClosedCallback cb);
    void setSegmentRemovedCallback(SegmentRemovedCallback cb);

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
    void resetSessionState();

    std::unique_ptr<IEncoder> encoder_;
    std::unique_ptr<IMuxer> muxer_;
    RecorderConfig config_{};

    std::mutex mutex_;
    bool initialized_{false};
    bool rolling_enabled_{true};
    std::optional<ActiveSegment> current_segment_{};
    std::vector<SegmentInfo> completed_segments_{};
    uint32_t segment_index_{0};
    std::atomic<bool> running_{false};
    std::filesystem::path session_directory_{};
    int current_session_id_{-1};
    uint64_t buffered_size_bytes_{0};
    SegmentClosedCallback segment_closed_cb_{};
    SegmentRemovedCallback segment_removed_cb_{};
    bool rotate_pending_ = false;


};