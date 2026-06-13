#pragma once
#include <cstdint>
#include <vector>
#include <atomic>
#include <thread>
#include <array>
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

    bool init(int monitorIdx = 0);
    void shutdown();

    bool captureFrame(std::vector<uint8_t>& outPixels, int& outW, int& outH);

    bool isHdr() const { return m_isHdr; }
    void setCaptureCursor(bool capture) { m_captureCursor = capture; }

    static int enumerateMonitors();
    static const char* getMonitorName(int idx);

private:
    bool initD3D11();
    bool initDuplication(int monitorIdx);
    bool tryGetHdrMetaData();
    bool hasHdrSupport(IDXGIOutput* output) const;

    static constexpr int kNumStaging = 3;

    ID3D11Device* m_d3dDevice = nullptr;
    ID3D11DeviceContext* m_d3dCtx = nullptr;
    D3D_FEATURE_LEVEL m_featureLevel = D3D_FEATURE_LEVEL_11_0;
    IDXGIOutputDuplication* m_duplication = nullptr;

    struct StagingBuffer {
        ID3D11Texture2D* texture = nullptr;
        bool mapped = false;
    };
    std::array<StagingBuffer, kNumStaging> m_staging;
    unsigned int m_stagingHead = 0;

    DXGI_FORMAT m_dxgiFormat = DXGI_FORMAT_B8G8R8A8_UNORM;
    int m_outputWidth = 0;
    int m_outputHeight = 0;
    int m_monitorIdx = 0;
    bool m_initialized = false;
    bool m_reinitFailed = false;
    bool m_isHdr = false;
    bool m_captureCursor = true;
};

}
