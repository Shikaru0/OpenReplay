#pragma once
#include <cstdint>
#include <string>
#include <functional>
#include <thread>
#include <atomic>
#include <vector>

#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>

namespace OpenReplay {

class AudioCapture {
public:
    enum CaptureMode { Loopback, Microphone };

    AudioCapture() = default;
    ~AudioCapture() { stop(); cleanup(); }

    AudioCapture(const AudioCapture&) = delete;
    AudioCapture& operator=(const AudioCapture&) = delete;

    using AudioCallback = std::function<void(const uint8_t* data, uint32_t size, int64_t pts)>;

    bool init(CaptureMode mode = Loopback, const std::string& deviceId = "");
    void start();
    void stop();

    void setAudioCallback(AudioCallback cb) { m_callback = std::move(cb); }
    void setSilenceThreshold(float threshold) { m_silenceThreshold = threshold; }

    int getSampleRate() const { return m_sampleRate; }
    int getChannels() const { return m_channels; }
    int getBitsPerSample() const { return m_bitsPerSample; }

    struct DeviceInfo {
        std::string id;
        std::string name;
        int channels;
        int sampleRate;
    };

    static std::vector<DeviceInfo> enumerateDevices(CaptureMode mode);

private:
    void captureThread();
    void cleanup();
    bool isSilent(const float* samples, size_t count) const;

    IAudioClient* m_audioClient = nullptr;
    IAudioCaptureClient* m_captureClient = nullptr;
    WAVEFORMATEX* m_waveFormat = nullptr;
    IMMDevice* m_device = nullptr;
    IMMDeviceEnumerator* m_enumerator = nullptr;
    bool m_comInitialized = false;
    std::string m_deviceId;

    int m_sampleRate = 48000;
    int m_channels = 2;
    int m_bitsPerSample = 32;

    float m_silenceThreshold = 0.001f;
    int64_t m_silenceDurationUs = 0;

    std::thread m_thread;
    std::atomic<bool> m_running{false};
    HANDLE m_stopEvent = nullptr;
    AudioCallback m_callback;

    int64_t m_qpcFreq = 0;
    HANDLE m_eventHandle = nullptr;
    std::vector<uint8_t> m_silenceBuf;
};

}
