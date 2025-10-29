#pragma once

#include <array>
#include <deque>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

extern "C" {
#include <libavformat/avformat.h>
}

#include "muxer.h"

class MuxerAvFormat : public IMuxer {
public:
    MuxerAvFormat();
    ~MuxerAvFormat() override;

    bool open(const MuxerConfig& cfg,
              const EncoderStreamInfo& video,
              const EncoderStreamInfo& systemAudio,
              const EncoderStreamInfo& micAudio) override;
    bool write(const EncodedPacket& packet) override;
    bool close() override;

    [[nodiscard]] std::optional<MuxerError> lastError() const noexcept;
    [[nodiscard]] bool checkSanity() const noexcept;

private:
    struct StreamClock {
        int64_t base_ms{GLINT_NOPTS_VALUE};
        int64_t last_dts_ms{GLINT_NOPTS_VALUE};
    };

    struct StreamState {
        StreamClock clock{};
        AVRational fallback_tb{1, 1000};
        uint64_t packets_written{0};
    };

    struct FormatContextDeleter {
        void operator()(AVFormatContext* ctx) const noexcept;
    };

    struct PacketDeleter {
        void operator()(AVPacket* pkt) const noexcept;
    };

    using FormatContextPtr = std::unique_ptr<AVFormatContext, FormatContextDeleter>;
    using PacketPtr = std::unique_ptr<AVPacket, PacketDeleter>;

    StreamState& stateFor(EncodedStreamType type) noexcept;
    const StreamState& stateFor(EncodedStreamType type) const noexcept;
    AVStream* streamFor(EncodedStreamType type) const noexcept;

    bool initializeContext(const MuxerConfig& cfg);
    bool createStream(const EncoderStreamInfo& info, int& index_out);
    bool ensureHeader(const EncodedPacket& packet, AVStream* stream);
    bool flushPendingPacketsUnlocked();
    bool writePacketUnlocked(const EncodedPacket& packet, AVStream* stream);
    void injectExtradataIfNeeded(const EncodedPacket& packet) const;

    static AVRational sanitizeTimeBase(const EncoderStreamInfo& info, AVRational fallback) noexcept;
    static AVRational ensureValid(const AVRational& value, AVRational fallback) noexcept;
    static int streamIndex(EncodedStreamType type) noexcept;
    static void logAvError(int err, const std::string& context);
    static std::vector<uint8_t> extractH264ExtradataFromAnnexB(const uint8_t* data, int size);
    static std::string determineContainer(const MuxerConfig& cfg, const std::filesystem::path& outputPath);

    void resetStateUnlocked();
    void setError(MuxerError error) noexcept;

    mutable std::mutex mutex_;
    FormatContextPtr ctx_{};
    std::filesystem::path output_path_{};
    MuxerConfig config_{};
    std::optional<MuxerError> last_error_{};
    bool header_written_{false};
    bool header_failed_{false};
    int video_stream_{-1};
    int system_stream_{-1};
    int mic_stream_{-1};
    std::deque<EncodedPacket> pending_packets_{};
    std::array<StreamState, 3> stream_states_{};
};