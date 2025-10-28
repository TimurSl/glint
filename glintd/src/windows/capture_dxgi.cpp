#include "../common/logger.h"
#include <d3d11.h>
#include <dxgi1_2.h>
#include <wrl/client.h>
#include <vector>
#include <stdexcept>
#include <chrono>

using Microsoft::WRL::ComPtr;

struct DxgiDup {
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> ctx;
    ComPtr<IDXGIOutputDuplication> dup;
    DXGI_OUTDUPL_DESC dupDesc{};
    int width = 0, height = 0;

    void init() {
        UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_SINGLETHREADED;
#ifdef _DEBUG
        // flags |= D3D11_CREATE_DEVICE_DEBUG;
#endif
        D3D_FEATURE_LEVEL fl_out;
        HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
            flags, nullptr, 0, D3D11_SDK_VERSION, &device, &fl_out, &ctx);
        if (FAILED(hr)) throw std::runtime_error("D3D11CreateDevice failed");

        ComPtr<IDXGIDevice> dxgiDevice;
        device.As(&dxgiDevice);
        ComPtr<IDXGIAdapter> adapter;
        dxgiDevice->GetAdapter(&adapter);
        ComPtr<IDXGIOutput> output;
        adapter->EnumOutputs(0, &output);
        ComPtr<IDXGIOutput1> output1;
        output.As(&output1);

        hr = output1->DuplicateOutput(device.Get(), &dup);
        if (FAILED(hr)) throw std::runtime_error("DuplicateOutput failed");

        dup->GetDesc(&dupDesc);
        width = dupDesc.ModeDesc.Width;
        height = dupDesc.ModeDesc.Height;
    }

    bool acquire(std::vector<uint8_t>& out_bgra, int& out_stride) {
        DXGI_OUTDUPL_FRAME_INFO fi{};
        ComPtr<IDXGIResource> res;
        auto hr = dup->AcquireNextFrame(16 /*ms*/, &fi, &res);
        if (hr == DXGI_ERROR_WAIT_TIMEOUT) return false;
        if (FAILED(hr)) throw std::runtime_error("AcquireNextFrame failed");

        ComPtr<ID3D11Texture2D> tex;
        res.As(&tex);

        D3D11_TEXTURE2D_DESC desc{};
        tex->GetDesc(&desc);
        desc.Usage = D3D11_USAGE_STAGING;
        desc.BindFlags = 0;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        desc.MiscFlags = 0;
        ComPtr<ID3D11Texture2D> staging;
        device->CreateTexture2D(&desc, nullptr, &staging);
        ctx->CopyResource(staging.Get(), tex.Get());

        D3D11_MAPPED_SUBRESOURCE map{};
        hr = ctx->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &map);
        if (FAILED(hr)) { dup->ReleaseFrame(); throw std::runtime_error("Map failed"); }

        out_stride = map.RowPitch;
        size_t need = (size_t)map.RowPitch * height;
        if (out_bgra.size() < need) out_bgra.resize(need);
        memcpy(out_bgra.data(), map.pData, need);

        ctx->Unmap(staging.Get(), 0);
        dup->ReleaseFrame();
        return true;
    }
};
