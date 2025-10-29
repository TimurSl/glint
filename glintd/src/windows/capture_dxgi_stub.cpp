#include "capture_base.h"
#include "../common/logger.h"
#include "../common/ff/encoder_ffmpeg.h"
#include "../common/ff/muxer_avformat.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <memory>
#include <thread>
#include <vector>


#include <wrl/client.h>
#include <d3d11.h>
#include <dxgi1_2.h>

#include <mmdeviceapi.h>
#include <audioclient.h>
#include <mmreg.h>
#include <windows.h>

using Microsoft::WRL::ComPtr;

namespace {
class DxgiVideoCapture : public IVideoCapture {
public:
    DxgiVideoCapture(int targetFps, bool withCursor)
        : fps_(targetFps), capture_cursor_(withCursor) {}
    ~DxgiVideoCapture() override { stop(); }

    bool start(VideoCallback cb) override {
        if (running_) return true;
        if (!initializeDevice()) {
            Logger::instance().error("DxgiVideoCapture: failed to initialize");
            return false;
        }
        running_ = true;
        worker_ = std::thread([this, cb]{ captureLoop(cb); });
        return true;
    }

    void stop() override {
        if (!running_) return;
        running_ = false;
        if (worker_.joinable()) worker_.join();
        duplication_.Reset();
        context_.Reset();
        device_.Reset();
    }

private:
    bool initializeDevice() {
        UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
#if defined(_DEBUG)
        flags |= D3D11_CREATE_DEVICE_DEBUG;
#endif
        D3D_FEATURE_LEVEL levels[] = {
            D3D_FEATURE_LEVEL_11_1,
            D3D_FEATURE_LEVEL_11_0,
            D3D_FEATURE_LEVEL_10_1,
            D3D_FEATURE_LEVEL_10_0
        };
        D3D_FEATURE_LEVEL level{};
        if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags,
                                     levels, ARRAYSIZE(levels), D3D11_SDK_VERSION,
                                     &device_, &level, &context_))) {
            return false;
        }
        ComPtr<IDXGIDevice> dxgiDevice;
        if (FAILED(device_.As(&dxgiDevice))) return false;
        ComPtr<IDXGIAdapter> adapter;
        if (FAILED(dxgiDevice->GetAdapter(&adapter))) return false;
        ComPtr<IDXGIOutput> output;
        if (FAILED(adapter->EnumOutputs(0, &output))) return false;
        if (FAILED(output.As(&output1_))) return false;
        DXGI_OUTPUT_DESC desc{};
        output1_->GetDesc(&desc);
        width_ = desc.DesktopCoordinates.right - desc.DesktopCoordinates.left;
        height_ = desc.DesktopCoordinates.bottom - desc.DesktopCoordinates.top;
        if (FAILED(output1_->DuplicateOutput(device_.Get(), &duplication_))) {
            return false;
        }
        return true;
    }

    void captureLoop(const VideoCallback& cb) {
    using namespace std::chrono;
    auto interval = milliseconds(1000 / std::max(1, fps_));
    auto next = steady_clock::now();

    while (running_) {
        DXGI_OUTDUPL_FRAME_INFO frameInfo{};
        ComPtr<IDXGIResource> resource;
        HRESULT hr = duplication_->AcquireNextFrame(16, &frameInfo, &resource);

        if (hr == DXGI_ERROR_WAIT_TIMEOUT) {
            std::this_thread::sleep_for(interval);
            continue;
        }

        if (hr == DXGI_ERROR_ACCESS_LOST || hr == DXGI_ERROR_DEVICE_REMOVED) {
            Logger::instance().warn("DxgiVideoCapture: duplication lost, recreating...");
            duplication_.Reset();
            if (FAILED(output1_->DuplicateOutput(device_.Get(), &duplication_))) {
                std::this_thread::sleep_for(100ms);
                continue;
            }
            continue;
        }

        if (FAILED(hr)) {
            Logger::instance().warn("DxgiVideoCapture: AcquireNextFrame failed");
            std::this_thread::sleep_for(10ms);
            continue;
        }

        ComPtr<ID3D11Texture2D> texture;
        resource.As(&texture);

        D3D11_TEXTURE2D_DESC desc{};
        texture->GetDesc(&desc);
        desc.Usage = D3D11_USAGE_STAGING;
        desc.BindFlags = 0;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        desc.MiscFlags = 0;

        ComPtr<ID3D11Texture2D> staging;
        if (FAILED(device_->CreateTexture2D(&desc, nullptr, &staging))) {
            duplication_->ReleaseFrame();
            continue;
        }

        context_->CopyResource(staging.Get(), texture.Get());

        D3D11_MAPPED_SUBRESOURCE mapped{};
        if (FAILED(context_->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped))) {
            duplication_->ReleaseFrame();
            continue;
        }

        VideoFrame frame;
        frame.width = static_cast<int>(desc.Width);
        frame.height = static_cast<int>(desc.Height);
        frame.stride = frame.width * 4;
        frame.data.resize(static_cast<size_t>(frame.stride) * frame.height);

        const uint8_t* src = static_cast<const uint8_t*>(mapped.pData);
        for (int y = 0; y < frame.height; ++y) {
            const uint8_t* row = src + y * mapped.RowPitch;
            uint8_t* dst = frame.data.data() + y * frame.stride;
            memcpy(dst, row, frame.stride);
        }

        context_->Unmap(staging.Get(), 0);
        duplication_->ReleaseFrame();

        frame.pts_ms = duration_cast<milliseconds>(
            steady_clock::now().time_since_epoch()).count();
        cb(frame);

        next += interval;
        std::this_thread::sleep_until(next);
    }
}


    int fps_{60};
    bool capture_cursor_{true};
    std::atomic<bool> running_{false};
    std::thread worker_;
    ComPtr<ID3D11Device> device_;
    ComPtr<ID3D11DeviceContext> context_;
    ComPtr<IDXGIOutput1> output1_;
    ComPtr<IDXGIOutputDuplication> duplication_;
    int width_{0};
    int height_{0};
};

class WasapiCapture : public IAudioCapture {
public:
    WasapiCapture(bool mic, int sampleRate, int channels)
        : is_mic_(mic), sample_rate_(sampleRate), channels_(channels) {}
    ~WasapiCapture() override { stop(); }

    bool start(AudioCallback cb) override {
        if (running_) return true;
        running_ = true;
        worker_ = std::thread([this, cb]{ captureLoop(cb); });
        return true;
    }

    void stop() override {
        if (!running_) return;
        running_ = false;
        if (worker_.joinable()) worker_.join();
    }

private:
    void captureLoop(const AudioCallback& cb) {
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        ComPtr<IMMDeviceEnumerator> enumerator;
        if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                    IID_PPV_ARGS(&enumerator)))) {
            running_ = false;
            CoUninitialize();
            return;
        }
        ComPtr<IMMDevice> device;
        auto role = is_mic_ ? eCapture : eRender;
        if (FAILED(enumerator->GetDefaultAudioEndpoint(role, eConsole, &device))) {
            running_ = false;
            CoUninitialize();
            return;
        }
        ComPtr<IAudioClient> client;
        if (FAILED(device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, &client))) {
            running_ = false;
            CoUninitialize();
            return;
        }
        WAVEFORMATEX* mixFormat = nullptr;
        client->GetMixFormat(&mixFormat);
        REFERENCE_TIME bufferDuration = 20 * 10000; // 20ms
        DWORD streamFlags = AUDCLNT_STREAMFLAGS_EVENTCALLBACK;
        if (!is_mic_) streamFlags |= AUDCLNT_STREAMFLAGS_LOOPBACK;
        if (FAILED(client->Initialize(AUDCLNT_SHAREMODE_SHARED, streamFlags, bufferDuration, 0,
                                      mixFormat, nullptr))) {
            CoTaskMemFree(mixFormat);
            running_ = false;
            CoUninitialize();
            return;
        }
        HANDLE eventHandle = CreateEvent(nullptr, FALSE, FALSE, nullptr);
        client->SetEventHandle(eventHandle);
        ComPtr<IAudioCaptureClient> capture;
        client->GetService(IID_PPV_ARGS(&capture));
        client->Start();
        std::vector<float> buffer;
        auto t0 = std::chrono::steady_clock::now();
        while (running_) {
            WaitForSingleObject(eventHandle, 50);
            UINT32 packetFrames = 0;
            capture->GetNextPacketSize(&packetFrames);
            while (packetFrames != 0) {
                BYTE* data = nullptr;
                UINT32 frames = 0;
                DWORD flags = 0;
                capture->GetBuffer(&data, &frames, &flags, nullptr, nullptr);
                if (frames > 0) {
                    bool isFloat = (mixFormat->wFormatTag == WAVE_FORMAT_IEEE_FLOAT);
                    if (mixFormat->wFormatTag == WAVE_FORMAT_EXTENSIBLE) {
                        auto* ext = reinterpret_cast<WAVEFORMATEXTENSIBLE*>(mixFormat);
                        isFloat = ext->SubFormat == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;
                    }
                    const size_t sampleCount = static_cast<size_t>(frames) * mixFormat->nChannels;
                    buffer.resize(sampleCount);
                    if (isFloat) {
                        const float* src = reinterpret_cast<const float*>(data);
                        std::copy(src, src + sampleCount, buffer.begin());
                    } else {
                        const int16_t* src = reinterpret_cast<const int16_t*>(data);
                        for (size_t i = 0; i < sampleCount; ++i) {
                            buffer[i] = static_cast<float>(src[i]) / 32768.0f;
                        }
                    }
                    AudioFrame frame;
                    frame.sample_rate = mixFormat->nSamplesPerSec;
                    frame.channels = mixFormat->nChannels;
                    frame.samples = frames;
                    frame.pts_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                       std::chrono::steady_clock::now() - t0).count();
                    frame.interleaved = buffer;
                    cb(frame, is_mic_);
                }
                capture->ReleaseBuffer(frames);
                capture->GetNextPacketSize(&packetFrames);
            }
        }
        client->Stop();
        CloseHandle(eventHandle);
        CoTaskMemFree(mixFormat);
        CoUninitialize();
    }

    bool is_mic_{false};
    int sample_rate_{48000};
    int channels_{2};
    std::atomic<bool> running_{false};
    std::thread worker_;
};

class WindowsCapture : public CaptureBase {
public:
    WindowsCapture()
        : CaptureBase(makeOptions()) {}

protected:
    std::unique_ptr<IVideoCapture> createVideoCapture(const CaptureInitOptions& options) override {
        return std::make_unique<DxgiVideoCapture>(options.target_fps, options.capture_cursor);
    }

    std::unique_ptr<IAudioCapture> createSystemAudioCapture(const CaptureInitOptions& options) override {
        return std::make_unique<WasapiCapture>(false, options.recorder.audio_sample_rate,
                                               options.recorder.audio_channels);
    }

    std::unique_ptr<IAudioCapture> createMicrophoneCapture(const CaptureInitOptions& options) override {
        return std::make_unique<WasapiCapture>(true, options.recorder.audio_sample_rate,
                                              options.recorder.audio_channels);
    }

    std::unique_ptr<IEncoder> createEncoder() override {
        return std::make_unique<FFmpegEncoder>();
    }

    std::unique_ptr<IMuxer> createMuxer() override {
        return std::make_unique<MuxerAvFormat>();
    }

private:
    static CaptureInitOptions makeOptions() {
        CaptureInitOptions opts;
        opts.target_fps = 60;
        opts.capture_cursor = true;
        opts.recorder.video_codec = "h264_nvenc";
        opts.recorder.width = GetSystemMetrics(SM_CXSCREEN);
        opts.recorder.height = GetSystemMetrics(SM_CYSCREEN);
        opts.recorder.audio_codec = "aac";
        opts.recorder.buffer_directory = "buffer";
        opts.recorder.recordings_directory = "recordings";
        return opts;
    }
};
} // namespace

extern "C" CaptureBase* create_capture() {
    return new WindowsCapture();
}
