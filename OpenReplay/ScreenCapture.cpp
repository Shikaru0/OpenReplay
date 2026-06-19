#include "ScreenCapture.h"
#include <iostream>
#include <cstring>
#include <dxgi1_2.h>

namespace OpenReplay {

    ScreenCapture::ScreenCapture()
        : m_device(nullptr)
        , m_context(nullptr)
        , m_adapter(nullptr)
        , m_output(nullptr)
        , m_duplication(nullptr)
        , m_width(0)
        , m_height(0)
        , m_hdr(false)
        , m_ownsDevice(true)
        , m_initialized(false)
    {
    }

    ScreenCapture::~ScreenCapture() {
        shutdown();
    }

    void ScreenCapture::shutdown() {
        std::lock_guard<std::mutex> lock(m_mtx);

        if (!m_initialized) return;

        std::cout << "[ScreenCapture] Shutdown initiated\n";

        if (m_duplication) {
            m_duplication->ReleaseFrame();
            m_duplication->Release();
            m_duplication = nullptr;
            std::cout << "[ScreenCapture] Released IDXGIOutputDuplication\n";
        }

        if (m_output) {
            m_output->Release();
            m_output = nullptr;
            std::cout << "[ScreenCapture] Released IDXGIOutput\n";
        }

        if (m_adapter) {
            m_adapter->Release();
            m_adapter = nullptr;
            std::cout << "[ScreenCapture] Released IDXGIAdapter\n";
        }

        if (m_device && m_ownsDevice) {
            if (m_context) {
                m_context->Release();
                m_context = nullptr;
                std::cout << "[ScreenCapture] Released ID3D11DeviceContext\n";
            }

            m_device->Release();
            m_device = nullptr;
            std::cout << "[ScreenCapture] Released ID3D11Device\n";
        }
        else if (m_device && !m_ownsDevice) {
            m_device->Release();
            m_device = nullptr;
            m_context = nullptr;  
            std::cout << "[ScreenCapture] Released reference to external device\n";
        }

        if (m_stagingTexture) {
            m_stagingTexture->Release();
            m_stagingTexture = nullptr;
        }

        m_width = 0;
        m_height = 0;
        m_hdr = false;
        m_initialized = false;

        std::cout << "[ScreenCapture] Shutdown complete\n";
    }

    bool ScreenCapture::init(int monitorIndex,
        ID3D11Device* existingDevice,
        ID3D11DeviceContext* existingContext)
    {
        std::lock_guard<std::mutex> lock(m_mtx);

        if (m_initialized) {
            shutdown();
        }

        HRESULT hr;

        std::cout << "[ScreenCapture] Initializing for monitor index " << monitorIndex << "\n";

        if (existingDevice && existingContext) {
            std::cout << "[ScreenCapture] Using shared D3D11 device\n";
            m_device = existingDevice;
            m_context = existingContext;
            m_ownsDevice = false;

            m_device->AddRef();
            m_context->AddRef();
        }
        else {
            std::cout << "[ScreenCapture] Creating new D3D11 device\n";

            D3D_FEATURE_LEVEL featureLevel;
            UINT createFlags = D3D11_CREATE_DEVICE_BGRA_SUPPORT; 

            hr = D3D11CreateDevice(
                nullptr,                    // Use default adapter 
                D3D_DRIVER_TYPE_HARDWARE,
                nullptr,                    // No software rasterizer
                createFlags,
                nullptr, 0,                 // Default feature levels
                D3D11_SDK_VERSION,
                &m_device,
                &featureLevel,
                &m_context
            );

            if (FAILED(hr)) {
                std::cerr << "[ScreenCapture] D3D11CreateDevice failed: 0x"
                    << std::hex << hr << std::dec << "\n";

                hr = D3D11CreateDevice(
                    nullptr,
                    D3D_DRIVER_TYPE_WARP,
                    nullptr,
                    createFlags,
                    nullptr, 0,
                    D3D11_SDK_VERSION,
                    &m_device,
                    &featureLevel,
                    &m_context
                );

                if (FAILED(hr)) {
                    std::cerr << "[ScreenCapture] WARP fallback also failed: 0x"
                        << std::hex << hr << std::dec << "\n";
                    return false;
                }
                std::cout << "[ScreenCapture] Using WARP (software) device\n";
            }

            m_ownsDevice = true;
            std::cout << "[ScreenCapture] D3D11 device created (Feature Level "
                << featureLevel << ")\n";
        }

        IDXGIDevice* dxgiDevice = nullptr;
        hr = m_device->QueryInterface(IID_PPV_ARGS(&dxgiDevice));
        if (FAILED(hr)) {
            std::cerr << "[ScreenCapture] Failed to get IDXGIDevice: 0x"
                << std::hex << hr << std::dec << "\n";
            if (m_ownsDevice) cleanupDeviceOnly();
            return false;
        }

        IDXGIAdapter* adapterFromDevice = nullptr;
        hr = dxgiDevice->GetAdapter(&adapterFromDevice);
        dxgiDevice->Release();

        if (FAILED(hr) || !adapterFromDevice) {
            std::cerr << "[ScreenCapture] Failed to get adapter from device\n";
            if (m_ownsDevice) cleanupDeviceOnly();
            return false;
        }

        DXGI_OUTPUT_DESC outputDesc = {};
        bool foundOutput = false;

        std::cout << "[ScreenCapture] Enumerating outputs for monitor " << monitorIndex << "...\n";

        UINT outputCount = 0;
        for (UINT i = 0; ; ++i) {
            IDXGIOutput* tempOutput = nullptr;
            hr = adapterFromDevice->EnumOutputs(i, &tempOutput);

            if (hr == DXGI_ERROR_NOT_FOUND) {
                std::cout << "[ScreenCapture] Found " << i << " total outputs\n";
                break;
            }

            if (FAILED(hr)) {
                std::cerr << "[ScreenCapture] EnumOutputs[" << i << "] failed: 0x"
                    << std::hex << hr << std::dec << "\n";
                continue;
            }

            if ((int)i == monitorIndex) {
                std::cout << "[ScreenCapture] Found target output at index " << i << "\n";

                IDXGIOutput1* output1 = nullptr;
                hr = tempOutput->QueryInterface(IID_PPV_ARGS(&output1));

                if (SUCCEEDED(hr) && output1) {
                    output1->GetDesc(&outputDesc);
                    output1->Release();

                    m_output = tempOutput;  
                    foundOutput = true;

                    std::cout << "[ScreenCapture] Output name: "
                        << convertWStringToString(outputDesc.DeviceName)
                        << " | Attached: " << (outputDesc.AttachedToDesktop ? "Yes" : "No")
                        << " | Rotation: " << outputDesc.Rotation
                        << " | Rect: L=" << outputDesc.DesktopCoordinates.left
                        << " T=" << outputDesc.DesktopCoordinates.top
                        << " R=" << outputDesc.DesktopCoordinates.right
                        << " B=" << outputDesc.DesktopCoordinates.bottom
                        << "\n";
                }
                else {
                    tempOutput->Release();
                    std::cerr << "[ScreenCapture] Failed to query IDXGIOutput1\n";
                }
            }
            else {
                tempOutput->Release(); 
            }
        }

        adapterFromDevice->Release();

        if (!foundOutput || !m_output) {
            std::cerr << "[ScreenCapture] Monitor index " << monitorIndex
                << " not found! Available range: 0-" << (outputCount - 1) << "\n";
            if (m_ownsDevice) cleanupDeviceOnly();
            return false;
        }

        IDXGIDevice* dxgiDevForAdapter = nullptr;
        hr = m_device->QueryInterface(IID_PPV_ARGS(&dxgiDevForAdapter));
        if (SUCCEEDED(hr)) {
            dxgiDevForAdapter->GetAdapter(&m_adapter);
            dxgiDevForAdapter->Release();
        }

        if (!m_adapter) {
            std::cerr << "[ScreenCapture] Failed to retain adapter reference\n";
            m_output->Release();
            m_output = nullptr;
            if (m_ownsDevice) cleanupDeviceOnly();
            return false;
        }

        std::cout << "[ScreenCapture] Attempting DuplicateOutput...\n";

        IDXGIOutput1* output1 = nullptr;
        hr = m_output->QueryInterface(IID_PPV_ARGS(&output1));
        if (FAILED(hr)) {
            std::cerr << "[ScreenCapture] Failed to query IDXGIOutput1: 0x"
                << std::hex << hr << std::dec << "\n";
            m_output->Release();
            m_output = nullptr;
            m_adapter->Release();
            m_adapter = nullptr;
            if (m_ownsDevice) cleanupDeviceOnly();
            return false;
        }

        hr = output1->DuplicateOutput(m_device, &m_duplication);
        output1->Release();

        if (FAILED(hr)) {
            switch (hr) {
            case DXGI_ERROR_ACCESS_LOST:
                std::cerr << "[ScreenCapture] DXGI_ERROR_ACCESS_LOST: "
                    << "Desktop duplication access lost (another app? mode change?)\n";
                break;
            case E_ACCESSDENIED:
                std::cerr << "[ScreenCapture] E_ACCESSDENIED: "
                    << "Permission denied (UAC? secure desktop?)\n";
                break;
            case E_INVALIDARG:
                std::cerr << "[ScreenCapture] E_INVALIDARG: "
                    << "Invalid arguments to DuplicateOutput\n";
                break;
            case DXGI_ERROR_UNSUPPORTED:
                std::cerr << "[ScreenCapture] DXGI_ERROR_UNSUPPORTED: "
                    << "Desktop duplication not supported on this OS/hardware\n";
                break;
            case DXGI_ERROR_SESSION_DISCONNECTED:
                std::cerr << "[ScreenCapture] DXGI_ERROR_SESSION_DISCONNECTED: "
                    << "Session disconnected (RDP? user switch?)\n";
                break;
            default:
                std::cerr << "[ScreenCapture] DuplicateOutput failed: 0x"
                    << std::hex << hr << " (" << hr << ")" << std::dec << "\n";
                break;
            }

            m_output->Release();
            m_output = nullptr;
            m_adapter->Release();
            m_adapter = nullptr;
            if (m_ownsDevice) cleanupDeviceOnly();
            return false;
        }

        std::cout << "[ScreenCapture] Desktop Duplication acquired successfully!\n";

        m_width = outputDesc.DesktopCoordinates.right - outputDesc.DesktopCoordinates.left;
        m_height = outputDesc.DesktopCoordinates.bottom - outputDesc.DesktopCoordinates.top;

        if (outputDesc.Rotation == DXGI_MODE_ROTATION_ROTATE90 ||
            outputDesc.Rotation == DXGI_MODE_ROTATION_ROTATE270) {
            std::swap(m_width, m_height);
            std::cout << "[ScreenCapture] Display is rotated, swapped dimensions to "
                << m_width << "x" << m_height << "\n";
        }

        m_hdr = checkHdrSupport(outputDesc);

        std::cout << "[ScreenCapture] Resolution: " << m_width << "x" << m_height
            << " | HDR: " << (m_hdr ? "Yes" : "No") << "\n";

        D3D11_TEXTURE2D_DESC texDesc = {};
        texDesc.Width = m_width;
        texDesc.Height = m_height;
        texDesc.MipLevels = 1;
        texDesc.ArraySize = 1;
        texDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;  
        texDesc.SampleDesc.Count = 1;
        texDesc.Usage = D3D11_USAGE_STAGING;
        texDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        texDesc.BindFlags = 0;
        texDesc.MiscFlags = 0;

        hr = m_device->CreateTexture2D(&texDesc, nullptr, &m_stagingTexture);
        if (FAILED(hr)) {
            std::cerr << "[ScreenCapture] Failed to create staging texture: 0x"
                << std::hex << hr << std::dec << "\n";
            m_stagingTexture = nullptr;
        }
        else {
            std::cout << "[ScreenCapture] Staging texture created\n";
        }

        m_initialized = true;
        std::cout << "[ScreenCapture] Initialization COMPLETE for monitor "
            << monitorIndex << "\n";

        return true;
    }

    bool ScreenCapture::captureFrame(std::vector<uint8_t>& framePixels, int& outWidth, int& outHeight) {
        std::lock_guard<std::mutex> lock(m_mtx);

        if (!m_initialized || !m_duplication || !m_device || !m_context) {
            return false;
        }

        HRESULT hr;
        DXGI_OUTDUPL_FRAME_INFO frameInfo = {};
        IDXGIResource* desktopResource = nullptr;

        const UINT ACQUIRE_TIMEOUT_MS = 50; 

        hr = m_duplication->AcquireNextFrame(ACQUIRE_TIMEOUT_MS, &frameInfo, &desktopResource);

        if (hr == DXGI_ERROR_WAIT_TIMEOUT) {
            return false;
        }

        if (hr == DXGI_ERROR_ACCESS_LOST) {
            std::cerr << "[ScreenCapture] Access lost during capture, need reinit\n";
            m_duplication->Release();
            m_duplication = nullptr;
            m_initialized = false;
            return false;
        }

        if (hr == DXGI_ERROR_INVALID_CALL) {
            m_duplication->ReleaseFrame();

            hr = m_duplication->AcquireNextFrame(ACQUIRE_TIMEOUT_MS, &frameInfo, &desktopResource);
            if (FAILED(hr)) {
                return false;
            }
        }

        if (FAILED(hr)) {
            std::cerr << "[ScreenCapture] AcquireNextFrame failed: 0x"
                << std::hex << hr << std::dec << "\n";
            return false;
        }

        ID3D11Texture2D* desktopTexture = nullptr;
        hr = desktopResource->QueryInterface(IID_PPV_ARGS(&desktopTexture));
        desktopResource->Release();

        if (FAILED(hr)) {
            m_duplication->ReleaseFrame();
            std::cerr << "[ScreenCapture] Failed to query texture interface\n";
            return false;
        }

        if (m_stagingTexture) {
            m_context->CopyResource(m_stagingTexture, desktopTexture);

            D3D11_MAPPED_SUBRESOURCE mapped;
            hr = m_context->Map(m_stagingTexture, 0, D3D11_MAP_READ, 0, &mapped);

            if (SUCCEEDED(hr)) {
                int rowPitch = static_cast<int>(mapped.RowPitch);
                int dataSize = rowPitch * m_height;

                framePixels.resize(dataSize);

                uint8_t* dest = framePixels.data();
                const uint8_t* src = static_cast<const uint8_t*>(mapped.pData);

                for (int y = 0; y < m_height; y++) {
                    memcpy(dest + (y * m_width * 4), src + (y * rowPitch), m_width * 4);
                }

                m_context->Unmap(m_stagingTexture, 0);

                outWidth = m_width;
                outHeight = m_height;

                desktopTexture->Release();
                m_duplication->ReleaseFrame();

                return true;
            }
            else {
                std::cerr << "[ScreenCapture] Failed to map staging texture: 0x"
                    << std::hex << hr << std::dec << "\n";
            }
        }
        else {
            D3D11_MAPPED_SUBRESOURCE mapped;
            hr = m_context->Map(desktopTexture, 0, D3D11_MAP_READ, 0, &mapped);

            if (SUCCEEDED(hr)) {
                int rowPitch = static_cast<int>(mapped.RowPitch);
                int dataSize = rowPitch * m_height;

                framePixels.resize(dataSize);

                uint8_t* dest = framePixels.data();
                const uint8_t* src = static_cast<const uint8_t*>(mapped.pData);

                for (int y = 0; y < m_height; y++) {
                    memcpy(dest + (y * m_width * 4), src + (y * rowPitch), m_width * 4);
                }

                m_context->Unmap(desktopTexture, 0);

                outWidth = m_width;
                outHeight = m_height;

                desktopTexture->Release();
                m_duplication->ReleaseFrame();

                return true;
            }
        }

        desktopTexture->Release();
        m_duplication->ReleaseFrame();

        return false;
    }

    bool ScreenCapture::checkHdrSupport(const DXGI_OUTPUT_DESC& desc) {
        return false;
    }

    std::string ScreenCapture::convertWStringToString(const wchar_t* wstr) {
        if (!wstr) return "";

        int size = WideCharToMultiByte(CP_UTF8, 0, wstr, -1, nullptr, 0, nullptr, nullptr);
        if (size <= 0) return "";

        std::string result(size - 1, '\0');
        WideCharToMultiByte(CP_UTF8, 0, wstr, -1, &result[0], size, nullptr, nullptr);

        return result;
    }

    void ScreenCapture::cleanupDeviceOnly() {
        if (m_context) {
            m_context->Release();
            m_context = nullptr;
        }
        if (m_device) {
            m_device->Release();
            m_device = nullptr;
        }
    }

    int ScreenCapture::enumerateMonitors() {
        IDXGIFactory1* factory = nullptr;
        HRESULT hr = CreateDXGIFactory1(IID_PPV_ARGS(&factory));
        if (FAILED(hr)) return 0;

        int count = 0;
        IDXGIAdapter* adapter = nullptr;

        for (UINT i = 0; factory->EnumAdapters(i, &adapter) != DXGI_ERROR_NOT_FOUND; ++i) {
            for (UINT j = 0; ; ++j) {
                IDXGIOutput* output = nullptr;
                hr = adapter->EnumOutputs(j, &output);

                if (hr == DXGI_ERROR_NOT_FOUND) break;

                if (SUCCEEDED(hr)) {
                    DXGI_OUTPUT_DESC desc;
                    if (SUCCEEDED(output->GetDesc(&desc))) {
                        if (desc.AttachedToDesktop) {
                            count++;
                        }
                    }
                    output->Release();
                }
            }
            adapter->Release();

            break;
        }

        factory->Release();
        return count;
    }

    const char* ScreenCapture::getMonitorName(int monitorIndex) {
        static std::string nameCache[16]; 
        static bool initialized[16] = { false };

        if (monitorIndex < 0 || monitorIndex >= 16) return nullptr;
        if (initialized[monitorIndex]) return nameCache[monitorIndex].c_str();

        IDXGIFactory1* factory = nullptr;
        HRESULT hr = CreateDXGIFactory1(IID_PPV_ARGS(&factory));
        if (FAILED(hr)) return nullptr;

        IDXGIAdapter* adapter = nullptr;
        if (FAILED(factory->EnumAdapters(0, &adapter))) {
            factory->Release();
            return nullptr;
        }

        int currentIndex = 0;
        for (UINT i = 0; ; ++i) {
            IDXGIOutput* output = nullptr;
            hr = adapter->EnumOutputs(i, &output);

            if (hr == DXGI_ERROR_NOT_FOUND) break;

            if (SUCCEEDED(hr)) {
                DXGI_OUTPUT_DESC desc;
                if (SUCCEEDED(output->GetDesc(&desc)) && desc.AttachedToDesktop) {
                    if (currentIndex == monitorIndex) {
                        nameCache[monitorIndex] = convertWStringToString(desc.DeviceName);

                        int w = desc.DesktopCoordinates.right - desc.DesktopCoordinates.left;
                        int h = desc.DesktopCoordinates.bottom - desc.DesktopCoordinates.top;
                        nameCache[monitorIndex] += " (" + std::to_string(w) + "x" + std::to_string(h) + ")";

                        initialized[monitorIndex] = true;
                        output->Release();
                        adapter->Release();
                        factory->Release();
                        return nameCache[monitorIndex].c_str();
                    }
                    currentIndex++;
                }
                output->Release();
            }
        }

        adapter->Release();
        factory->Release();

        nameCache[monitorIndex] = "Monitor " + std::to_string(monitorIndex);
        initialized[monitorIndex] = true;
        return nameCache[monitorIndex].c_str();
    }

} // namespace OpenReplay