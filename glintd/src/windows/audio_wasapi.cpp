#include <mmdeviceapi.h>
#include <audioclient.h>
#include <avrt.h>
#include <wrl/client.h>
#include <vector>
#include <thread>
#include <atomic>
#include <functional>
#include <stdexcept>

using Microsoft::WRL::ComPtr;

struct WasapiDevice {
    ComPtr<IAudioClient> client;
    ComPtr<IAudioCaptureClient> capture;
    WAVEFORMATEX* wf = nullptr;
    HANDLE event = nullptr;
    std::thread th;
    std::atomic<bool> run{false};

    std::function<void(const float*, int)> on_pcm;

    void init(EDataFlow flow) {
        ComPtr<IMMDeviceEnumerator> e;
        CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&e));
        ComPtr<IMMDevice> dev;
        e->GetDefaultAudioEndpoint(flow, eConsole, &dev);

        dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, &client);

        HRESULT hr = client->GetMixFormat(&wf);
        if (FAILED(hr)) throw std::runtime_error("GetMixFormat failed");

        REFERENCE_TIME dur = 10000000; // 1s
        DWORD flags = AUDCLNT_STREAMFLAGS_LOOPBACK;
        if (flow == eCapture) flags = 0;

        hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED, flags | AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
                                dur, 0, wf, nullptr);
        if (FAILED(hr)) throw std::runtime_error("Initialize IAudioClient failed");

        event = CreateEvent(nullptr, FALSE, FALSE, nullptr);
        client->SetEventHandle(event);

        hr = client->GetService(IID_PPV_ARGS(&capture));
        if (FAILED(hr)) throw std::runtime_error("GetService IAudioCaptureClient failed");
    }

    void start() {
        run = true;
        client->Start();
        th = std::thread([this](){ loop(); });
    }
    void stop() {
        run = false;
        if (th.joinable()) th.join();
        client->Stop();
        if (event) { CloseHandle(event); event=nullptr; }
        if (wf) { CoTaskMemFree(wf); wf=nullptr; }
    }

    void loop() {
        while (run) {
            DWORD r = WaitForSingleObject(event, 20);
            if (r != WAIT_OBJECT_0) continue;

            UINT32 frames=0, pack=0;
            BYTE* data=nullptr;
            DWORD flags=0;
            if (capture->GetBuffer(&data, &frames, &flags, nullptr, nullptr) == S_OK) {
                if (frames>0 && on_pcm) {
                    on_pcm((const float*)data, (int)frames);
                }
                capture->ReleaseBuffer(frames);
            }
        }
    }
};

struct WasapiSystemAndMic {
    WasapiDevice sys;
    WasapiDevice mic;
    void init(bool enable_sys, bool enable_mic,
              std::function<void(const float*,int)> on_sys,
              std::function<void(const float*,int)> on_mic) {
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        if (enable_sys) { sys.on_pcm = on_sys; sys.init(eRender); }
        if (enable_mic) { mic.on_pcm = on_mic; mic.init(eCapture); }
    }
    void start() { if (sys.client) sys.start(); if (mic.client) mic.start(); }
    void stop()  { if (sys.client) sys.stop();  if (mic.client) mic.stop(); CoUninitialize(); }
};
