#include "ScreenCapture.h"
#include <iostream>
#include <cstring>
#include <string>
#include <thread>
#include <algorithm>
#include <d3d11_1.h>
#include <dxgi1_6.h>

namespace OpenReplay {

constexpr DWORD kAcquireTimeoutMs = 1;

static int g_monitorCount = 0;
static std::string g_monitorNames[8];
static LUID g_adapterLuids[8];
static UINT g_adapterOutputIdx[8];

struct WinMon {
    std::wstring deviceName;
    RECT rect;
};

struct EnumMonCtx { std::vector<WinMon>* out; };

static BOOL CALLBACK enumMonCB(HMONITOR hMon, HDC, LPRECT, LPARAM lParam) {
    auto* ctx = (EnumMonCtx*)lParam;
    MONITORINFOEXW mi;
    mi.cbSize = sizeof(mi);
    if (GetMonitorInfoW(hMon, &mi)) {
        ctx->out->push_back({ mi.szDevice, mi.rcMonitor });
    }
    return TRUE;
}

bool ScreenCapture::init(int monitorIdx) {
    if (m_initialized) return true;
    m_monitorIdx = monitorIdx;

    if (enumerateMonitors() == 0) return false;
    if (monitorIdx < 0 || monitorIdx >= g_monitorCount) return false;

    IDXGIFactory1* factory = nullptr;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) return false;

    LUID targetLuid = g_adapterLuids[monitorIdx];
    UINT targetOutputIdx = g_adapterOutputIdx[monitorIdx];

    IDXGIAdapter* targetAdapter = nullptr;
    {
        IDXGIAdapter* adapter = nullptr;
        for (UINT ai = 0; factory->EnumAdapters(ai, &adapter) != DXGI_ERROR_NOT_FOUND; ++ai) {
            DXGI_ADAPTER_DESC adesc;
            if (SUCCEEDED(adapter->GetDesc(&adesc)) &&
                std::memcmp(&adesc.AdapterLuid, &targetLuid, sizeof(LUID)) == 0) {
                targetAdapter = adapter;
                targetAdapter->AddRef();
                adapter->Release();
                break;
            }
            adapter->Release();
        }
    }
    factory->Release();
    if (!targetAdapter) return false;

    D3D_FEATURE_LEVEL levels[] = {
        D3D_FEATURE_LEVEL_11_1,
        D3D_FEATURE_LEVEL_11_0
    };

    HRESULT hr = D3D11CreateDevice(targetAdapter, D3D_DRIVER_TYPE_UNKNOWN,
                                    nullptr, 0, levels, 2,
                                    D3D11_SDK_VERSION, &m_d3dDevice,
                                    &m_featureLevel, &m_d3dCtx);
    if (FAILED(hr) || !m_d3dDevice || !m_d3dCtx) {
        targetAdapter->Release();
        return false;
    }

    IDXGIOutput* output = nullptr;
    hr = targetAdapter->EnumOutputs(targetOutputIdx, &output);
    targetAdapter->Release();
    if (FAILED(hr)) { shutdown(); return false; }

    IDXGIOutput1* output1 = nullptr;
    hr = output->QueryInterface(IID_PPV_ARGS(&output1));
    output->Release();
    if (FAILED(hr)) { shutdown(); return false; }

    hr = output1->DuplicateOutput(m_d3dDevice, &m_duplication);
    output1->Release();
    if (FAILED(hr)) { shutdown(); return false; }

    DXGI_OUTDUPL_DESC dupDesc;
    m_duplication->GetDesc(&dupDesc);
    m_width = (int)dupDesc.ModeDesc.Width;
    m_height = (int)dupDesc.ModeDesc.Height;
    m_format = dupDesc.ModeDesc.Format;
    if (m_format == DXGI_FORMAT_R10G10B10A2_UNORM) m_isHdr = true;

    D3D11_TEXTURE2D_DESC stagingDesc = {};
    stagingDesc.Width = m_width;
    stagingDesc.Height = m_height;
    stagingDesc.MipLevels = 1;
    stagingDesc.ArraySize = 1;
    stagingDesc.SampleDesc.Count = 1;
    stagingDesc.Usage = D3D11_USAGE_STAGING;
    stagingDesc.BindFlags = 0;
    stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    stagingDesc.Format = m_format;

    hr = m_d3dDevice->CreateTexture2D(&stagingDesc, nullptr, &m_staging);
    if (FAILED(hr)) { shutdown(); return false; }

    m_initialized = true;
    return true;
}

void ScreenCapture::shutdown() {
    m_initialized = false;
    if (m_staging) { m_staging->Release(); m_staging = nullptr; }
    if (m_duplication) { m_duplication->Release(); m_duplication = nullptr; }
    if (m_d3dCtx) { m_d3dCtx->Release(); m_d3dCtx = nullptr; }
    if (m_d3dDevice) { m_d3dDevice->Release(); m_d3dDevice = nullptr; }
}

int ScreenCapture::enumerateMonitors() {
    if (g_monitorCount > 0) return g_monitorCount;

    std::vector<WinMon> winMons;
    EnumMonCtx ctx{ &winMons };
    EnumDisplayMonitors(nullptr, nullptr, enumMonCB, (LPARAM)&ctx);

    IDXGIFactory1* factory = nullptr;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) return 0;

    g_monitorCount = 0;

    for (auto& wm : winMons) {
        if (g_monitorCount >= 8) break;
        IDXGIAdapter* adapter = nullptr;
        for (UINT ai = 0; factory->EnumAdapters(ai, &adapter) != DXGI_ERROR_NOT_FOUND; ++ai) {
            DXGI_ADAPTER_DESC adesc;
            adapter->GetDesc(&adesc);

            IDXGIOutput* output = nullptr;
            for (UINT oi = 0; adapter->EnumOutputs(oi, &output) != DXGI_ERROR_NOT_FOUND; ++oi) {
                DXGI_OUTPUT_DESC od;
                if (SUCCEEDED(output->GetDesc(&od))) {
                    size_t winLen = wm.deviceName.length();
                    if (wcsncmp(wm.deviceName.c_str(), od.DeviceName, winLen) == 0) {
                        char buf[256];
                        int cw = WideCharToMultiByte(CP_UTF8, 0, wm.deviceName.c_str(), -1, buf, 256, nullptr, nullptr);
                        if (cw > 0) {
                            char label[384];
                            snprintf(label, sizeof(label), "%s (%dx%d)",
                                     buf,
                                     wm.rect.right - wm.rect.left,
                                     wm.rect.bottom - wm.rect.top);
                            g_monitorNames[g_monitorCount] = label;
                        } else {
                            g_monitorNames[g_monitorCount] = "Monitor " + std::to_string(g_monitorCount);
                        }
                        g_adapterLuids[g_monitorCount] = adesc.AdapterLuid;
                        g_adapterOutputIdx[g_monitorCount] = oi;
                        g_monitorCount++;
                        output->Release();
                        adapter->Release();
                        goto nextMon;
                    }
                }
                output->Release();
            }
            adapter->Release();
        }
nextMon:;
    }

    factory->Release();
    return g_monitorCount;
}

const char* ScreenCapture::getMonitorName(int idx) {
    if (idx >= 0 && idx < g_monitorCount) return g_monitorNames[idx].c_str();
    return nullptr;
}

int ScreenCapture::getMonitorCount() {
    return g_monitorCount;
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
    if (!m_initialized) {
        if (!m_reinitFailed) {
            std::cerr << "[ScreenCapture] Not initialized, attempting re-init\n";
            if (!init(m_monitorIdx)) {
                m_reinitFailed = true;
            }
        }
        return false;
    }

    outW = m_width;
    outH = m_height;

    int bpp = m_isHdr ? 8 : 4;
    int dstStride = m_width * bpp;
    outPixels.resize(m_height * dstStride, 0);

    HRESULT hr;
    IDXGIResource* desktopRes = nullptr;
    DXGI_OUTDUPL_FRAME_INFO frameInfo;
    hr = m_duplication->AcquireNextFrame(kAcquireTimeoutMs, &frameInfo, &desktopRes);

    if (hr == DXGI_ERROR_ACCESS_LOST || hr == DXGI_ERROR_DEVICE_REMOVED) {
        std::cerr << "[ScreenCapture] Device lost, re-initializing\n";
        shutdown();
        m_reinitFailed = false;
        return false;
    }

    if (SUCCEEDED(hr)) {
        ID3D11Texture2D* gpuTex = nullptr;
        hr = desktopRes->QueryInterface(IID_PPV_ARGS(&gpuTex));
        desktopRes->Release();
        if (SUCCEEDED(hr)) {
            m_d3dCtx->CopyResource(m_staging, gpuTex);
            gpuTex->Release();
            m_hasData = true;
        }
        m_duplication->ReleaseFrame();
    }

    if (!m_hasData) return false;

    D3D11_MAPPED_SUBRESOURCE mapped;
    hr = m_d3dCtx->Map(m_staging, 0, D3D11_MAP_READ, 0, &mapped);
    if (FAILED(hr)) return false;

    int srcStride = mapped.RowPitch;
    const uint8_t* src = (const uint8_t*)mapped.pData;
    uint8_t* dst = outPixels.data();

    if (m_isHdr) {
        for (int y = 0; y < m_height; ++y) {
            const uint32_t* src32 = (const uint32_t*)(src + y * srcStride);
            uint16_t* dst16 = (uint16_t*)(dst + y * dstStride);
            for (int x = 0; x < m_width; ++x) {
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
        int copyBytes = std::min(m_width * 4, srcStride);
        for (int y = 0; y < m_height; ++y) {
            std::memcpy(dst + y * dstStride, src + y * srcStride, copyBytes);
        }
    }

    m_d3dCtx->Unmap(m_staging, 0);
    return true;
}

}
