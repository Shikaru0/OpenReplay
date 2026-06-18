#pragma once
#include <cstdint>
#include <vector>
#include <atomic>
#include <thread>
#include <string>
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")

namespace OpenReplay {

class ScreenCapture {
public:
    ScreenCapture() = default;
    ~ScreenCapture() { shutdown(); }

    ScreenCapture(const ScreenCapture&) = delete;
    ScreenCapture& operator=(const ScreenCapture&) = delete;

    bool init(int monitorIdx);
    void shutdown();

    bool captureFrame(std::vector<uint8_t>& outPixels, int& outW, int& outH);

    bool isHdr() const { return m_isHdr; }
    int width() const { return m_width; }
    int height() const { return m_height; }

    static int enumerateMonitors();
    static const char* getMonitorName(int idx);
    static int getMonitorCount();

private:
    bool initD3D11();

    ID3D11Device* m_d3dDevice = nullptr;
    ID3D11DeviceContext* m_d3dCtx = nullptr;
    D3D_FEATURE_LEVEL m_featureLevel = D3D_FEATURE_LEVEL_11_0;

    IDXGIOutputDuplication* m_duplication = nullptr;
    ID3D11Texture2D* m_staging = nullptr;
    bool m_hasData = false;
    int m_width = 0;
    int m_height = 0;
    DXGI_FORMAT m_format = DXGI_FORMAT_B8G8R8A8_UNORM;

    bool m_initialized = false;
    bool m_reinitFailed = false;
    bool m_isHdr = false;
};

}
