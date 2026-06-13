#include "AudioCapture.h"
#include <iostream>
#include <cmath>
#include <vector>
#include <string>
#include <functiondiscoverykeys.h>

static const CLSID CLSID_MMDeviceEnumerator_
    = __uuidof(MMDeviceEnumerator);
static const IID IID_IMMDeviceEnumerator_
    = __uuidof(IMMDeviceEnumerator);
static const IID IID_IAudioClient_
    = __uuidof(IAudioClient);
static const IID IID_IAudioCaptureClient_
    = __uuidof(IAudioCaptureClient);

namespace OpenReplay {

constexpr int64_t kSilenceDurationThresholdUs = 500000;

std::vector<AudioCapture::DeviceInfo> AudioCapture::enumerateDevices(CaptureMode mode) {
    std::vector<DeviceInfo> devices;
    HRESULT hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    bool needUninit = (hr == S_OK);

    IMMDeviceEnumerator* enumerator = nullptr;
    hr = CoCreateInstance(CLSID_MMDeviceEnumerator_, NULL, CLSCTX_ALL,
                          IID_IMMDeviceEnumerator_, (void**)&enumerator);
    if (FAILED(hr)) {
        if (needUninit) CoUninitialize();
        return devices;
    }

    EDataFlow flow = (mode == Microphone) ? eCapture : eRender;
    IMMDeviceCollection* collection = nullptr;
    hr = enumerator->EnumAudioEndpoints(flow, DEVICE_STATE_ACTIVE, &collection);
    if (SUCCEEDED(hr)) {
        UINT count = 0;
        collection->GetCount(&count);
        for (UINT i = 0; i < count; ++i) {
            IMMDevice* dev = nullptr;
            if (FAILED(collection->Item(i, &dev))) continue;

            DeviceInfo info;
            LPWSTR devId = nullptr;
            if (SUCCEEDED(dev->GetId(&devId))) {
                int wlen = WideCharToMultiByte(CP_UTF8, 0, devId, -1, nullptr, 0, nullptr, nullptr);
                info.id.resize(wlen);
                WideCharToMultiByte(CP_UTF8, 0, devId, -1, info.id.data(), wlen, nullptr, nullptr);
                info.id.resize(wlen - 1);
                CoTaskMemFree(devId);
            }

            IPropertyStore* props = nullptr;
            if (SUCCEEDED(dev->OpenPropertyStore(STGM_READ, &props))) {
                PROPVARIANT var;
                PropVariantInit(&var);
                if (SUCCEEDED(props->GetValue(PKEY_Device_FriendlyName, &var))) {
                    int wlen = WideCharToMultiByte(CP_UTF8, 0, var.pwszVal, -1, nullptr, 0, nullptr, nullptr);
                    info.name.resize(wlen);
                    WideCharToMultiByte(CP_UTF8, 0, var.pwszVal, -1, info.name.data(), wlen, nullptr, nullptr);
                    info.name.resize(wlen - 1);
                    PropVariantClear(&var);
                }
                props->Release();
            }

            IAudioClient* client = nullptr;
            if (SUCCEEDED(dev->Activate(IID_IAudioClient_, CLSCTX_ALL, nullptr, (void**)&client))) {
                WAVEFORMATEX* fmt = nullptr;
                if (SUCCEEDED(client->GetMixFormat(&fmt))) {
                    info.channels = fmt->nChannels;
                    info.sampleRate = fmt->nSamplesPerSec;
                    CoTaskMemFree(fmt);
                }
                client->Release();
            }

            if (info.name.empty()) info.name = info.id;
            devices.push_back(std::move(info));
            dev->Release();
        }
        collection->Release();
    }

    enumerator->Release();
    if (needUninit) CoUninitialize();
    return devices;
}

bool AudioCapture::init(CaptureMode mode, const std::string& deviceId) {
    HRESULT hr;

    m_deviceId = deviceId;
    m_comInitialized = false;

    hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    if (FAILED(hr)) {
        std::cerr << "[AudioCapture] CoInitializeEx failed: 0x"
                  << std::hex << hr << std::dec << "\n";
        return false;
    }
    m_comInitialized = (hr == S_OK);

    LARGE_INTEGER freq;
    QueryPerformanceFrequency(&freq);
    m_qpcFreq = freq.QuadPart;

    hr = CoCreateInstance(CLSID_MMDeviceEnumerator_, NULL, CLSCTX_ALL,
                          IID_IMMDeviceEnumerator_, (void**)&m_enumerator);
    if (FAILED(hr)) {
        std::cerr << "[AudioCapture] MMDeviceEnumerator failed: 0x"
                  << std::hex << hr << std::dec << "\n";
        return false;
    }

    if (!deviceId.empty()) {
        int wlen = MultiByteToWideChar(CP_UTF8, 0, deviceId.c_str(), -1, nullptr, 0);
        std::wstring wid(wlen, L'\0');
        MultiByteToWideChar(CP_UTF8, 0, deviceId.c_str(), -1, wid.data(), wlen);
        hr = m_enumerator->GetDevice(wid.c_str(), &m_device);
        if (FAILED(hr)) {
            m_device = nullptr;
        }
    }

    if (!m_device) {
        EDataFlow flow = (mode == Microphone) ? eCapture : eRender;
        ERole role = (mode == Microphone) ? eCommunications : eConsole;
        hr = m_enumerator->GetDefaultAudioEndpoint(flow, role, &m_device);
        if (FAILED(hr)) {
            std::cerr << "[AudioCapture] GetDefaultAudioEndpoint failed: 0x"
                      << std::hex << hr << std::dec << "\n";
            return false;
        }
    }

    hr = m_device->Activate(IID_IAudioClient_, CLSCTX_ALL, NULL,
                            (void**)&m_audioClient);
    if (FAILED(hr)) {
        std::cerr << "[AudioCapture] IAudioClient activation failed: 0x"
                  << std::hex << hr << std::dec << "\n";
        return false;
    }

    hr = m_audioClient->GetMixFormat(&m_waveFormat);
    if (FAILED(hr)) {
        std::cerr << "[AudioCapture] GetMixFormat failed: 0x"
                  << std::hex << hr << std::dec << "\n";
        return false;
    }

    m_sampleRate = m_waveFormat->nSamplesPerSec;
    m_channels = m_waveFormat->nChannels;
    m_bitsPerSample = m_waveFormat->wBitsPerSample;

    m_eventHandle = CreateEventA(nullptr, FALSE, FALSE, nullptr);
    if (!m_eventHandle) {
        std::cerr << "[AudioCapture] CreateEvent failed\n";
        return false;
    }

    m_stopEvent = CreateEventA(nullptr, TRUE, FALSE, nullptr);
    if (!m_stopEvent) {
        std::cerr << "[AudioCapture] CreateEvent(stop) failed\n";
        return false;
    }

    DWORD streamFlags = AUDCLNT_STREAMFLAGS_EVENTCALLBACK |
                        AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM;
    if (mode == Loopback)
        streamFlags |= AUDCLNT_STREAMFLAGS_LOOPBACK;

    hr = m_audioClient->Initialize(AUDCLNT_SHAREMODE_SHARED,
                                   streamFlags,
                                   0, 0, m_waveFormat, NULL);
    if (FAILED(hr)) {
        std::cerr << "[AudioCapture] AudioClient init failed: 0x"
                  << std::hex << hr << std::dec << "\n";
        return false;
    }

    hr = m_audioClient->SetEventHandle(m_eventHandle);
    if (FAILED(hr)) {
        std::cerr << "[AudioCapture] SetEventHandle failed: 0x"
                  << std::hex << hr << std::dec << "\n";
        return false;
    }

    hr = m_audioClient->GetService(IID_IAudioCaptureClient_,
                                   (void**)&m_captureClient);
    if (FAILED(hr)) {
        std::cerr << "[AudioCapture] GetCaptureClient failed: 0x"
                  << std::hex << hr << std::dec << "\n";
        return false;
    }

    return true;
}

void AudioCapture::start() {
    if (m_running.exchange(true)) return;
    ResetEvent(m_stopEvent);
    m_silenceDurationUs = 0;
    m_thread = std::thread(&AudioCapture::captureThread, this);
}

void AudioCapture::stop() {
    if (!m_running.exchange(false)) return;
    SetEvent(m_stopEvent);
    if (m_eventHandle) SetEvent(m_eventHandle);
    if (m_thread.joinable()) m_thread.join();
}

bool AudioCapture::isSilent(const float* samples, size_t count) const {
    if (count == 0) return true;
    double sumSq = 0.0;
    for (size_t i = 0; i < count; ++i) {
        sumSq += (double)samples[i] * (double)samples[i];
    }
    double rms = std::sqrt(sumSq / (double)count);
    return (float)rms < m_silenceThreshold;
}

void AudioCapture::captureThread() {
    int bpf = m_waveFormat->nBlockAlign;
    int64_t qpcOrigin = 0;
    HANDLE waitHandles[2] = { m_eventHandle, m_stopEvent };

    HRESULT hr = m_audioClient->Start();
    if (FAILED(hr)) {
        std::cerr << "[AudioCapture] Start() failed: 0x"
                  << std::hex << hr << std::dec << "\n";
        return;
    }

    while (m_running) {
        DWORD waitResult = WaitForMultipleObjects(2, waitHandles, FALSE, 1000);

        if (!m_running) break;
        if (waitResult == WAIT_TIMEOUT) continue;
        if (waitResult == WAIT_OBJECT_0 + 1) break;
        if (waitResult != WAIT_OBJECT_0) continue;

        while (m_running) {
            UINT32 packetSize = 0;
            hr = m_captureClient->GetNextPacketSize(&packetSize);
            if (FAILED(hr) || packetSize == 0) break;

            BYTE* pData = nullptr;
            UINT32 framesAvailable = 0;
            DWORD flags = 0;
            UINT64 qpcPos = 0;

            hr = m_captureClient->GetBuffer(&pData, &framesAvailable,
                                            &flags, nullptr, &qpcPos);
            if (FAILED(hr)) break;

            if (qpcOrigin == 0) qpcOrigin = qpcPos;

            if (framesAvailable > 0) {
                uint32_t bytes = framesAvailable * bpf;
                int64_t pts = (int64_t)((qpcPos - qpcOrigin) * 1'000'000 / m_qpcFreq);

                bool isSilentFlag = (flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0;
                bool isSilentPcm = false;

                if (!isSilentFlag) {
                    if (m_bitsPerSample == 32 && m_channels > 0) {
                        isSilentPcm = isSilent((const float*)pData, framesAvailable * m_channels);
                    }
                }

                if (isSilentFlag || isSilentPcm) {
                    m_silenceDurationUs += (int64_t)framesAvailable * 1'000'000 / m_sampleRate;
                    if (m_silenceDurationUs > kSilenceDurationThresholdUs) {
                        if (m_silenceBuf.size() < bytes)
                            m_silenceBuf.resize(bytes, 0);
                        if (m_callback)
                            m_callback(m_silenceBuf.data(), bytes, pts);
                    }
                } else {
                    m_silenceDurationUs = 0;
                    if (m_callback)
                        m_callback(pData, bytes, pts);
                }
            }

            m_captureClient->ReleaseBuffer(framesAvailable);
        }
    }

    m_audioClient->Stop();
}

void AudioCapture::cleanup() {
    if (m_captureClient) { m_captureClient->Release(); m_captureClient = nullptr; }
    if (m_audioClient) { m_audioClient->Release(); m_audioClient = nullptr; }
    if (m_waveFormat) { CoTaskMemFree(m_waveFormat); m_waveFormat = nullptr; }
    if (m_device) { m_device->Release(); m_device = nullptr; }
    if (m_enumerator) { m_enumerator->Release(); m_enumerator = nullptr; }
    if (m_eventHandle) { CloseHandle(m_eventHandle); m_eventHandle = nullptr; }
    if (m_stopEvent) { CloseHandle(m_stopEvent); m_stopEvent = nullptr; }
    if (m_comInitialized) { CoUninitialize(); m_comInitialized = false; }
}

}
