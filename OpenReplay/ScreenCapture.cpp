#include "ScreenCapture.h"
#include <iostream>
#include <cstring>
#include <string>
#include <thread>
#include <d3d11_1.h>
#include <dxgi1_6.h>

namespace OpenReplay {

constexpr DWORD kAcquireTimeoutMs = 1;

bool ScreenCapture::init(int monitorIdx) {
    if (m_initialized) return true;

    if (!initD3D11()) {
        std::cerr << "[ScreenCapture] D3D11 init failed\n";
        shutdown();
        return false;
    }
    if (!initDuplication(monitorIdx)) {
        std::cerr << "[ScreenCapture] Output duplication init failed\n";
        shutdown();
        return false;
    }

    tryGetHdrMetaData();

    m_monitorIdx = monitorIdx;
    m_initialized = true;
    return true;
}

bool ScreenCapture::hasHdrSupport(IDXGIOutput* output) const {
    IDXGIOutput6* output6 = nullptr;
    if (FAILED(output->QueryInterface(IID_PPV_ARGS(&output6))))
        return false;
    DXGI_OUTPUT_DESC1 desc1;
    HRESULT hr = output6->GetDesc1(&desc1);
    output6->Release();
    return SUCCEEDED(hr) && desc1.ColorSpace == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020;
}

bool ScreenCapture::tryGetHdrMetaData() {
    if (!m_duplication) return false;

    IDXGIDevice* dxgiDevice = nullptr;
    if (FAILED(m_d3dDevice->QueryInterface(IID_PPV_ARGS(&dxgiDevice))))
        return false;

    IDXGIAdapter* adapter = nullptr;
    dxgiDevice->GetParent(IID_PPV_ARGS(&adapter));
    dxgiDevice->Release();
    if (!adapter) return false;

    IDXGIOutput* output = nullptr;
    if (SUCCEEDED(adapter->EnumOutputs(m_monitorIdx, &output))) {
        m_isHdr = hasHdrSupport(output);
        output->Release();
    }
    adapter->Release();
    return m_isHdr;
}

void ScreenCapture::shutdown() {
    m_initialized = false;
    for (auto& sb : m_staging) {
        if (sb.texture) { sb.texture->Release(); sb.texture = nullptr; }
    }
    if (m_duplication) { m_duplication->Release(); m_duplication = nullptr; }
    if (m_d3dCtx) { m_d3dCtx->Release(); m_d3dCtx = nullptr; }
    if (m_d3dDevice) { m_d3dDevice->Release(); m_d3dDevice = nullptr; }
}

bool ScreenCapture::initD3D11() {
    HRESULT hr;

    IDXGIFactory1* factory = nullptr;
    hr = CreateDXGIFactory1(IID_PPV_ARGS(&factory));
    if (FAILED(hr)) return false;

    size_t bestMemory = 0;
    IDXGIAdapter* bestAdapter = nullptr;
    IDXGIAdapter* adapter = nullptr;

    for (UINT i = 0; factory->EnumAdapters(i, &adapter) != DXGI_ERROR_NOT_FOUND; ++i) {
        DXGI_ADAPTER_DESC desc;
        if (SUCCEEDED(adapter->GetDesc(&desc))) {
            if (desc.VendorId == 0x1414) {
                adapter->Release();
                continue;
            }
            if (desc.DedicatedVideoMemory > bestMemory) {
                bestMemory = desc.DedicatedVideoMemory;
                if (bestAdapter) bestAdapter->Release();
                bestAdapter = adapter;
                adapter->AddRef();
            }
        }
        adapter->Release();
    }

    factory->Release();

    if (!bestAdapter) return false;

    D3D_FEATURE_LEVEL levels[] = {
        D3D_FEATURE_LEVEL_11_1,
        D3D_FEATURE_LEVEL_11_0
    };

    UINT flags = 0;
    hr = D3D11CreateDevice(bestAdapter, D3D_DRIVER_TYPE_UNKNOWN,
                           nullptr, flags, levels, 2,
                           D3D11_SDK_VERSION, &m_d3dDevice,
                           &m_featureLevel, &m_d3dCtx);

    DXGI_ADAPTER_DESC aDesc;
    bestAdapter->GetDesc(&aDesc);
    bestAdapter->Release();

    if (FAILED(hr) || !m_d3dDevice || !m_d3dCtx) return false;
    return true;
}

static int g_monitorCount = 0;
static std::string g_monitorNames[8];

int ScreenCapture::enumerateMonitors() {
    if (g_monitorCount > 0) return g_monitorCount;
    IDXGIFactory1* factory = nullptr;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) return 0;
    IDXGIAdapter* adapter = nullptr;
    g_monitorCount = 0;
    for (UINT ai = 0; factory->EnumAdapters(ai, &adapter) != DXGI_ERROR_NOT_FOUND; ++ai) {
        IDXGIOutput* output = nullptr;
        for (UINT oi = 0; adapter->EnumOutputs(oi, &output) != DXGI_ERROR_NOT_FOUND; ++oi) {
            DXGI_OUTPUT_DESC od;
            if (SUCCEEDED(output->GetDesc(&od)) && g_monitorCount < 8) {
                int w = WideCharToMultiByte(CP_UTF8, 0, od.DeviceName, -1, nullptr, 0, nullptr, nullptr);
                if (w > 0) {
                    g_monitorNames[g_monitorCount].resize(w);
                    WideCharToMultiByte(CP_UTF8, 0, od.DeviceName, -1,
                                        g_monitorNames[g_monitorCount].data(), w, nullptr, nullptr);
                }
                g_monitorCount++;
            }
            output->Release();
        }
        adapter->Release();
    }
    factory->Release();
    return g_monitorCount;
}

const char* ScreenCapture::getMonitorName(int idx) {
    if (idx >= 0 && idx < g_monitorCount) return g_monitorNames[idx].c_str();
    return nullptr;
}

bool ScreenCapture::initDuplication(int monitorIdx) {
    HRESULT hr;

    IDXGIDevice* dxgiDevice = nullptr;
    hr = m_d3dDevice->QueryInterface(IID_PPV_ARGS(&dxgiDevice));
    if (FAILED(hr)) return false;

    IDXGIAdapter* adapter = nullptr;
    hr = dxgiDevice->GetParent(IID_PPV_ARGS(&adapter));
    dxgiDevice->Release();
    if (FAILED(hr)) return false;

    IDXGIOutput* output = nullptr;
    hr = adapter->EnumOutputs(monitorIdx, &output);
    adapter->Release();
    if (FAILED(hr)) return false;

    IDXGIOutput1* output1 = nullptr;
    hr = output->QueryInterface(IID_PPV_ARGS(&output1));
    output->Release();
    if (FAILED(hr)) return false;

    hr = output1->DuplicateOutput(m_d3dDevice, &m_duplication);
    output1->Release();
    if (FAILED(hr)) return false;

    DXGI_OUTDUPL_DESC dupDesc;
    m_duplication->GetDesc(&dupDesc);
    m_outputWidth = (int)dupDesc.ModeDesc.Width;
    m_outputHeight = (int)dupDesc.ModeDesc.Height;
    m_dxgiFormat = dupDesc.ModeDesc.Format;

    if (m_dxgiFormat == DXGI_FORMAT_R10G10B10A2_UNORM) {
        m_isHdr = true;
    }

    D3D11_TEXTURE2D_DESC stagingDesc = {};
    stagingDesc.Width = m_outputWidth;
    stagingDesc.Height = m_outputHeight;
    stagingDesc.MipLevels = 1;
    stagingDesc.ArraySize = 1;
    stagingDesc.SampleDesc.Count = 1;
    stagingDesc.Usage = D3D11_USAGE_STAGING;
    stagingDesc.BindFlags = 0;
    stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    stagingDesc.Format = dupDesc.ModeDesc.Format;

    for (int i = 0; i < kNumStaging; ++i) {
        hr = m_d3dDevice->CreateTexture2D(&stagingDesc, nullptr, &m_staging[i].texture);
        if (FAILED(hr)) return false;
    }

    return true;
}

static uint16_t floatToHalf(float f) {
    uint32_t i;
    std::memcpy(&i, &f, sizeof(i));
    int s = (i >> 16) & 0x8000;
    int e = ((i >> 23) & 0xff) - 127 + 15;
    int m = i & 0x007fffff;

    if (e <= 0) {
        if (e < -10) return (uint16_t)s;
        m = (m | 0x00800000) >> (1 - e);
        return (uint16_t)(s | (m >> 13));
    } else if (e == 0xff - 127 + 15) {
        return (uint16_t)(s | 0x7c00 | (m >> 13));
    }

    if (e > 30) return (uint16_t)(s | 0x7c00);
    return (uint16_t)(s | (e << 10) | (m >> 13));
}

bool ScreenCapture::captureFrame(std::vector<uint8_t>& outPixels, int& outW, int& outH) {
    if (!m_initialized || !m_duplication) {
        if (!m_initialized && !m_reinitFailed) {
            std::cerr << "[ScreenCapture] Not initialized, attempting re-init\n";
            if (init(m_monitorIdx)) {
                m_reinitFailed = false;
            } else {
                m_reinitFailed = true;
            }
        }
        return false;
    }

    HRESULT hr;

    IDXGIResource* desktopRes = nullptr;
    DXGI_OUTDUPL_FRAME_INFO frameInfo;
    hr = m_duplication->AcquireNextFrame(kAcquireTimeoutMs, &frameInfo, &desktopRes);

    if (hr == DXGI_ERROR_WAIT_TIMEOUT) {
        return false;
    }
    if (hr == DXGI_ERROR_ACCESS_LOST || hr == DXGI_ERROR_DEVICE_REMOVED) {
        std::cerr << "[ScreenCapture] Device lost (0x" << std::hex << hr
                  << std::dec << "), re-initializing\n";
        shutdown();
        m_reinitFailed = false;
        if (!init(m_monitorIdx)) {
            std::cerr << "[ScreenCapture] Re-init failed\n";
            m_reinitFailed = true;
        }
        return false;
    }
    if (FAILED(hr)) {
        return false;
    }

    ID3D11Texture2D* gpuTex = nullptr;
    hr = desktopRes->QueryInterface(IID_PPV_ARGS(&gpuTex));
    desktopRes->Release();
    if (FAILED(hr)) {
        m_duplication->ReleaseFrame();
        return false;
    }

    unsigned int stagingIdx = m_stagingHead % kNumStaging;
    m_stagingHead++;

    m_d3dCtx->CopyResource(m_staging[stagingIdx].texture, gpuTex);
    gpuTex->Release();

    int readIdx = (m_stagingHead >= 3)
        ? (int)((m_stagingHead - 3) % kNumStaging)
        : -1;

    if (readIdx < 0 || !m_staging[readIdx].texture) {
        m_duplication->ReleaseFrame();
        return false;
    }

    D3D11_MAPPED_SUBRESOURCE mapped;
    hr = m_d3dCtx->Map(m_staging[readIdx].texture, 0,
                       D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &mapped);

    if (hr == DXGI_ERROR_WAS_STILL_DRAWING) {
        hr = m_d3dCtx->Map(m_staging[readIdx].texture, 0,
                           D3D11_MAP_READ, 0, &mapped);
    }

    if (FAILED(hr)) {
        m_duplication->ReleaseFrame();
        return false;
    }

    outW = m_outputWidth;
    outH = m_outputHeight;

    bool is10Bit = (m_dxgiFormat == DXGI_FORMAT_R10G10B10A2_UNORM);
    int dstBpp = is10Bit ? 8 : 4;
    outPixels.resize(m_outputWidth * m_outputHeight * dstBpp);

    const uint8_t* src = (const uint8_t*)mapped.pData;
    uint8_t* dst = outPixels.data();
    int srcStride = mapped.RowPitch;

    if (m_isHdr && is10Bit) {
        for (int y = 0; y < m_outputHeight; ++y) {
            const uint32_t* src32 = (const uint32_t*)(src + y * srcStride);
            uint16_t* dst16 = (uint16_t*)(dst + y * m_outputWidth * 8);
            for (int x = 0; x < m_outputWidth; ++x) {
                uint32_t px = src32[x];
                float r = ((px >> 0) & 0x3FF) / 1023.0f;
                float g = ((px >> 10) & 0x3FF) / 1023.0f;
                float b = ((px >> 20) & 0x3FF) / 1023.0f;
                float a = ((px >> 30) & 0x3) / 3.0f;
                dst16[x * 4 + 0] = floatToHalf(r);
                dst16[x * 4 + 1] = floatToHalf(g);
                dst16[x * 4 + 2] = floatToHalf(b);
                dst16[x * 4 + 3] = floatToHalf(a);
            }
        }
    } else {
        int dstStride = m_outputWidth * 4;
        int copyBytes = dstStride < srcStride ? dstStride : srcStride;
        for (int y = 0; y < m_outputHeight; ++y) {
            std::memcpy(dst + y * dstStride, src + y * srcStride, copyBytes);
        }
    }

    m_d3dCtx->Unmap(m_staging[readIdx].texture, 0);
    m_duplication->ReleaseFrame();

    return true;
}



}
