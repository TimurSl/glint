#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "encoder.h"

struct MuxerConfig {
    std::string container = "matroska";
    std::filesystem::path path;
    int tb_ms = 1;
    bool two_audio_tracks = true; // system + mic

    std::string video_codec;
    std::string audio_codec;
};

enum class MuxerError {
    None = 0,
    InvalidConfiguration,
    ContextAllocationFailed,
    StreamAllocationFailed,
    IoOpenFailed,
    HeaderWriteFailed,
    PacketWriteFailed,
    NotOpen,
    InvalidPacket,
    OutOfMemory
};

class IMuxer {
public:
    virtual ~IMuxer() = default;
    virtual bool open(const MuxerConfig& cfg,
                      const EncoderStreamInfo& video,
                      const EncoderStreamInfo& systemAudio,
                      const EncoderStreamInfo& micAudio) = 0;
    virtual bool write(const EncodedPacket& packet) = 0;
    virtual bool close() = 0;
};