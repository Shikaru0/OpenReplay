#pragma once
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <vector>
#include <string>
#include <mutex>

struct ID3D11Device;
struct ID3D11DeviceContext;
struct ID3D11Texture2D;

namespace OpenReplay {

    class ScreenCapture {
    public:
        ScreenCapture();
        ~ScreenCapture();

        ScreenCapture(const ScreenCapture&) = delete;
        ScreenCapture& operator=(const ScreenCapture&) = delete;

        bool init(int monitorIndex,
            ID3D11Device* existingDevice = nullptr,
            ID3D11DeviceContext* existingContext = nullptr);

        void shutdown();

        bool captureFrame(std::vector<uint8_t>& framePixels, int& outWidth, int& outHeight);

        int width() const { return m_width; }
        int height() const { return m_height; }
        bool isHdr() const { return m_hdr; }

        static int enumerateMonitors();
        static const char* getMonitorName(int monitorIndex);

    private:
        void cleanupDeviceOnly();
        bool checkHdrSupport(const DXGI_OUTPUT_DESC& desc);
        static std::string convertWStringToString(const wchar_t* wstr);

        mutable std::mutex m_mtx;

        ID3D11Device* m_device;
        ID3D11DeviceContext* m_context;
        IDXGIAdapter* m_adapter;
        IDXGIOutput* m_output;
        IDXGIOutputDuplication* m_duplication;
        ID3D11Texture2D* m_stagingTexture;  

        int m_width;
        int m_height;
        bool m_hdr;
        bool m_ownsDevice;    
        bool m_initialized;
    };

}