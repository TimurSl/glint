#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "frame_types.h"
#include "recorder.h"

using VideoCallback = std::function<void(const VideoFrame&)>;
using AudioCallback = std::function<void(const AudioFrame&, bool isMic)>;

class IVideoCapture {
public:
    virtual ~IVideoCapture() = default;
    virtual bool start(VideoCallback cb) = 0;
    virtual void stop() = 0;
};

class IAudioCapture {
public:
    virtual ~IAudioCapture() = default;
    virtual bool start(AudioCallback cb) = 0; // system/mic
    virtual void stop() = 0;
};

struct CaptureRuntimeOptions {
    bool rolling_buffer_enabled{true};
};

struct CaptureInitOptions {
    int target_fps{60};
    bool capture_cursor{true};
    RecorderConfig recorder;
};

class CaptureBase {
public:
    explicit CaptureBase(CaptureInitOptions options);
    virtual ~CaptureBase();

    bool init();
    bool start();
    void stop();

    void applyRuntimeOptions(const CaptureRuntimeOptions& opts);
    void setRecorderConfig(const RecorderConfig& config);
    void setCaptureOptions(const CaptureInitOptions& options);
    bool isRunning() const { return running_.load(); }

    Recorder& recorder();

protected:
    virtual std::unique_ptr<IVideoCapture> createVideoCapture(const CaptureInitOptions& options) = 0;
    virtual std::unique_ptr<IAudioCapture> createSystemAudioCapture(const CaptureInitOptions& options) = 0;
    virtual std::unique_ptr<IAudioCapture> createMicrophoneCapture(const CaptureInitOptions& options) = 0;
    virtual std::unique_ptr<IEncoder> createEncoder() = 0;
    virtual std::unique_ptr<IMuxer> createMuxer() = 0;

private:
    void onVideoFrame(const VideoFrame& frame);
    void onAudioFrame(const AudioFrame& frame, bool isMic);

    CaptureInitOptions options_;
    CaptureRuntimeOptions runtime_{};
    std::unique_ptr<IVideoCapture> video_;
    std::unique_ptr<IAudioCapture> system_audio_;
    std::unique_ptr<IAudioCapture> mic_audio_;
    std::unique_ptr<Recorder> recorder_;
    std::mutex recorder_mutex_;
    std::atomic<bool> running_{false};
};
