#include "capture_base.h"

#include "logger.h"

CaptureBase::CaptureBase(CaptureInitOptions options)
    : options_(std::move(options)) {}

CaptureBase::~CaptureBase() {
    stop();
}

bool CaptureBase::init() {
    std::scoped_lock lock(recorder_mutex_);
    if (!video_) {
        video_ = createVideoCapture(options_);
        if (!video_) {
            Logger::instance().error("CaptureBase: failed to create video capture");
            return false;
        }
    }
    if (options_.recorder.enable_system_audio) {
        if (!system_audio_) {
            system_audio_ = createSystemAudioCapture(options_);
            if (!system_audio_) {
                Logger::instance().error("CaptureBase: failed to create system audio capture");
                return false;
            }
        }
    } else {
        system_audio_.reset();
    }
    if (options_.recorder.enable_microphone_audio) {
        if (!mic_audio_) {
            mic_audio_ = createMicrophoneCapture(options_);
            if (!mic_audio_) {
                Logger::instance().warn("CaptureBase: failed to create microphone capture");
            }
        }
    } else {
        mic_audio_.reset();
    }

    if (!recorder_) {
        recorder_ = std::make_unique<Recorder>(createEncoder(), createMuxer());
        if (!recorder_->initialize(options_.recorder)) {
            Logger::instance().error("CaptureBase: failed to initialize recorder");
            recorder_.reset();
            return false;
        }
    }

    return true;
}

bool CaptureBase::start() {
    if (running_.exchange(true)) {
        return true;
    }

    if (!recorder_) {
        if (!init()) {
            running_ = false;
            return false;
        }
    }

    if (!recorder_->start(runtime_.rolling_buffer_enabled)) {
        running_ = false;
        return false;
    }

    auto videoStarted = video_->start([this](const VideoFrame& frame) { onVideoFrame(frame); });
    if (!videoStarted) {
        Logger::instance().error("CaptureBase: failed to start video capture");
        running_ = false;
        recorder_->stop();
        return false;
    }

    if (options_.recorder.enable_system_audio && system_audio_) {
        auto sysStarted = system_audio_->start([this](const AudioFrame& frame, bool) {
            onAudioFrame(frame, false);
        });
        if (!sysStarted) {
            Logger::instance().warn("CaptureBase: system audio capture unavailable");
        }
    }

    if (options_.recorder.enable_microphone_audio && mic_audio_) {
        auto micStarted = mic_audio_->start([this](const AudioFrame& frame, bool) {
            onAudioFrame(frame, true);
        });
        if (!micStarted) {
            Logger::instance().warn("CaptureBase: microphone capture unavailable");
        }
    }

    Logger::instance().info("CaptureBase: capture started");
    return true;
}

void CaptureBase::stop() {
    if (!running_.exchange(false)) {
        return;
    }

    if (video_) video_->stop();
    if (system_audio_) system_audio_->stop();
    if (mic_audio_) mic_audio_->stop();

    std::scoped_lock lock(recorder_mutex_);
    if (recorder_) {
        recorder_->stop();
    }
    Logger::instance().info("CaptureBase: capture stopped");
}


void CaptureBase::setRecorderConfig(const RecorderConfig& config) {
    std::scoped_lock lock(recorder_mutex_);
    options_.recorder = config;
    if (recorder_) {
        recorder_->initialize(config);
    }
}

void CaptureBase::setCaptureOptions(const CaptureInitOptions& options) {
    std::scoped_lock lock(recorder_mutex_);
    options_ = options;
    if (recorder_) {
        recorder_->initialize(options_.recorder);
    }
}

void CaptureBase::applyRuntimeOptions(const CaptureRuntimeOptions& opts) {
    std::scoped_lock lock(recorder_mutex_);
    runtime_ = opts;
    if (recorder_) {
        recorder_->setRollingBufferEnabled(opts.rolling_buffer_enabled);
    }
}

Recorder& CaptureBase::recorder() {
    std::scoped_lock lock(recorder_mutex_);
    if (!recorder_) {
        recorder_ = std::make_unique<Recorder>(createEncoder(), createMuxer());
        recorder_->initialize(options_.recorder);
    }
    return *recorder_;
}

void CaptureBase::onVideoFrame(const VideoFrame& frame) {
    std::scoped_lock lock(recorder_mutex_);
    if (recorder_) {
        recorder_->pushVideoFrame(frame);
    }
}

void CaptureBase::onAudioFrame(const AudioFrame& frame, bool isMic) {
    std::scoped_lock lock(recorder_mutex_);
    if (recorder_) {
        recorder_->pushAudioFrame(frame, isMic);
    }
}
