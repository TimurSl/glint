#include "../common/capture_base.h"
#include "../common/logger.h"
#include "../common/ff/encoder_ffmpeg.h"
#include "../common/ff/muxer_avformat.h"

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <pulse/simple.h>
#include <pulse/error.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <memory>
#include <thread>
#include <string>

namespace {
class X11VideoCapture : public IVideoCapture {
public:
    explicit X11VideoCapture(int fps) : fps_(fps) {}
    ~X11VideoCapture() override { stop(); }

    bool start(VideoCallback cb) override {
        if (running_) return true;
        display_ = XOpenDisplay(nullptr);
        if (!display_) {
            Logger::instance().error("X11VideoCapture: failed to open display");
            return false;
        }
        root_ = DefaultRootWindow(display_);
        XWindowAttributes attrs{};
        XGetWindowAttributes(display_, root_, &attrs);
        width_ = attrs.width;
        height_ = attrs.height;
        running_ = true;
        worker_ = std::thread([this, cb] { captureLoop(cb); });
        return true;
    }

    void stop() override {
        if (!running_) return;
        running_ = false;
        if (worker_.joinable()) worker_.join();
        if (display_) {
            XCloseDisplay(display_);
            display_ = nullptr;
        }
    }

private:
    void captureLoop(const VideoCallback& cb) {
        using namespace std::chrono;
        auto frame_interval = milliseconds(1000 / std::max(1, fps_));
        auto next_time = steady_clock::now();
        while (running_) {
            auto* image = XGetImage(display_, root_, 0, 0, width_, height_, AllPlanes, ZPixmap);
            if (!image) {
                Logger::instance().warn("X11VideoCapture: XGetImage failed");
                std::this_thread::sleep_for(frame_interval);
                continue;
            }
            VideoFrame frame;
            frame.width = width_;
            frame.height = height_;
            frame.stride = width_ * 4;
            frame.data.resize(static_cast<size_t>(frame.stride) * frame.height);
            const uint8_t* src = reinterpret_cast<const uint8_t*>(image->data);
            for (int y = 0; y < height_; ++y) {
                const uint8_t* row = src + y * image->bytes_per_line;
                uint8_t* dst = frame.data.data() + static_cast<size_t>(y) * frame.stride;
                for (int x = 0; x < width_; ++x) {
                    const uint8_t* pixel = row + x * 4;
                    dst[x * 4 + 0] = pixel[2];
                    dst[x * 4 + 1] = pixel[1];
                    dst[x * 4 + 2] = pixel[0];
                    dst[x * 4 + 3] = 255;
                }
            }
            XDestroyImage(image);
            frame.pts_ms = duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
            cb(frame);
            next_time += frame_interval;
            std::this_thread::sleep_until(next_time);
        }
    }

    Display* display_{nullptr};
    Window root_{};
    int width_{0};
    int height_{0};
    int fps_{60};
    std::atomic<bool> running_{false};
    std::thread worker_;
};

class PulseAudioCapture : public IAudioCapture {
public:
    PulseAudioCapture(std::string device, bool isMic, int sampleRate, int channels)
        : device_(std::move(device)), is_mic_(isMic), sample_rate_(sampleRate), channels_(channels) {}
    ~PulseAudioCapture() override { stop(); }

    bool start(AudioCallback cb) override {
        if (running_) return true;
        running_ = true;
        worker_ = std::thread([this, cb] { captureLoop(cb); });
        return true;
    }

    void stop() override {
        if (!running_) return;
        running_ = false;
        if (worker_.joinable()) worker_.join();
    }

private:
    void captureLoop(const AudioCallback& cb) {
        pa_sample_spec spec{PA_SAMPLE_FLOAT32LE, static_cast<uint32_t>(sample_rate_), static_cast<uint8_t>(channels_)};
        int error = 0;
        const char* deviceName = device_.empty() ? nullptr : device_.c_str();
        pa_simple* stream = pa_simple_new(nullptr, "glintd", PA_STREAM_RECORD, deviceName,
                                          is_mic_ ? "Microphone" : "Monitor",
                                          &spec, nullptr, nullptr, &error);
        if (!stream && deviceName) {
            Logger::instance().warn("PulseAudioCapture: retrying with default device: " + std::string(pa_strerror(error)));
            stream = pa_simple_new(nullptr, "glintd", PA_STREAM_RECORD, nullptr,
                                    is_mic_ ? "Microphone" : "Monitor",
                                    &spec, nullptr, nullptr, &error);
        }
        if (!stream) {
            Logger::instance().warn("PulseAudioCapture: failed to open stream: " + std::string(pa_strerror(error)));
            running_ = false;
            return;
        }
        const size_t frames = static_cast<size_t>(sample_rate_ / 100); // 10 ms chunks
        std::vector<float> buffer(frames * channels_);
        while (running_) {
            if (pa_simple_read(stream, buffer.data(), buffer.size() * sizeof(float), &error) < 0) {
                Logger::instance().warn("PulseAudioCapture: read error: " + std::string(pa_strerror(error)));
                break;
            }
            AudioFrame frame;
            frame.sample_rate = sample_rate_;
            frame.channels = channels_;
            frame.samples = static_cast<int>(frames);
            frame.pts_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                              std::chrono::steady_clock::now().time_since_epoch()).count();
            frame.interleaved = buffer;
            cb(frame, is_mic_);
        }
        pa_simple_free(stream);
    }

    std::string device_;
    bool is_mic_{false};
    int sample_rate_{48000};
    int channels_{2};
    std::atomic<bool> running_{false};
    std::thread worker_;
};

class LinuxCapture : public CaptureBase {
public:
    LinuxCapture()
        : CaptureBase(makeOptions()) {}

protected:
    std::unique_ptr<IVideoCapture> createVideoCapture(const CaptureInitOptions& options) override {
        return std::make_unique<X11VideoCapture>(options.target_fps);
    }

    std::unique_ptr<IAudioCapture> createSystemAudioCapture(const CaptureInitOptions& options) override {
        (void)options;
        return std::make_unique<PulseAudioCapture>("@DEFAULT_MONITOR@", false, options.recorder.audio_sample_rate,
                                                   options.recorder.audio_channels);
    }

    std::unique_ptr<IAudioCapture> createMicrophoneCapture(const CaptureInitOptions& options) override {
        (void)options;
        return std::make_unique<PulseAudioCapture>("@DEFAULT_SOURCE@", true, options.recorder.audio_sample_rate,
                                                   options.recorder.audio_channels);
    }

    std::unique_ptr<IEncoder> createEncoder() override {
        return std::make_unique<FFmpegEncoder>();
    }

    std::unique_ptr<IMuxer> createMuxer() override {
        return std::make_unique<AvMuxer>();
    }

private:
    static CaptureInitOptions makeOptions() {
        CaptureInitOptions opts;
        opts.target_fps = 60;
        opts.capture_cursor = true;
        Display* d = XOpenDisplay(nullptr);
        if (d) {
            Screen* s = DefaultScreenOfDisplay(d);
            if (s) {
                opts.recorder.width = s->width;
                opts.recorder.height = s->height;
            }
            XCloseDisplay(d);
        }
        opts.recorder.video_codec = "h264";
        opts.recorder.audio_codec = "aac";
        opts.recorder.rolling_directory = "buffer";
        opts.recorder.recordings_directory = "recordings";
        return opts;
    }
};
} // namespace

extern "C" CaptureBase* create_capture() {
    return new LinuxCapture();
}
