#pragma once

#include "Config.h"
#include "DiskBackedBuffer.h"
#include "VideoEncoder.h"
#include "AudioCapture.h"
#include "Muxer.h"
#include "ScreenCapture.h"

#include <thread>
#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <mutex>
#include <deque>
#include <vector>
#include <algorithm>
#include <cstdlib>

extern "C" {
#include <libavcodec/avcodec.h>
}

namespace OpenReplay {

struct CaptureStats {
    std::atomic<int64_t> framesCaptured{0};
    std::atomic<int64_t> framesEncoded{0};
    std::atomic<int64_t> framesDropped{0};
    std::atomic<int64_t> audioPackets{0};
    std::atomic<double> currentFps{0.0};
    std::atomic<int64_t> durationMs{0};
    std::atomic<bool> isHdr{false};

    CaptureStats() = default;
    CaptureStats(const CaptureStats& o)
        : framesCaptured(o.framesCaptured.load())
        , framesEncoded(o.framesEncoded.load())
        , framesDropped(o.framesDropped.load())
        , audioPackets(o.audioPackets.load())
        , currentFps(o.currentFps.load())
        , durationMs(o.durationMs.load())
        , isHdr(o.isHdr.load()) {}
    CaptureStats& operator=(const CaptureStats& o) {
        framesCaptured.store(o.framesCaptured.load());
        framesEncoded.store(o.framesEncoded.load());
        framesDropped.store(o.framesDropped.load());
        audioPackets.store(o.audioPackets.load());
        currentFps.store(o.currentFps.load());
        durationMs.store(o.durationMs.load());
        isHdr.store(o.isHdr.load());
        return *this;
    }
};

class OpenReplayEngine {
public:
    OpenReplayEngine();
    ~OpenReplayEngine();

    bool init(const RecorderConfig& config);
    void setConfig(const RecorderConfig& config);
    void startCapture();
    void stopCapture();
    bool saveLastMoments(const char* outputPath, int durationSeconds,
                          Muxer::ProgressCallback progress = nullptr);
    bool isCapturing() const;

    const char* getCodecName() const;
    bool isInitialized() const { return m_initialized; }
    bool hasPendingData() const { return m_diskBuffer && m_diskBuffer->getPacketCount() > 0; }
    CaptureStats getStats() const { return m_stats; }

private:
    void captureThreadLoop();
    void onEncodedPacket(const uint8_t* data, uint32_t size, int64_t pts, bool isKey);
    void onAudioData(const uint8_t* data, uint32_t size, int64_t pts);
    void onFrameDropped();

    RecorderConfig m_config;
    mutable std::mutex m_configMtx;
    bool m_initialized = false;
    CaptureStats m_stats;

    std::unique_ptr<DiskBackedBuffer> m_diskBuffer;
    std::unique_ptr<VideoEncoder> m_videoEncoder;
    std::unique_ptr<AudioCapture> m_audioCapture;
    std::unique_ptr<AudioCapture> m_micCapture;
    std::unique_ptr<ScreenCapture> m_screenCapture;
    std::unique_ptr<Muxer> m_muxer;

    std::thread m_captureThread;
    std::atomic<bool> m_isCapturing{false};
    bool m_encoderActive = false;

    int64_t m_qpcFreq = 0;
    int64_t m_qpcOrigin = 0;

    void onMicAudioData(const uint8_t* data, uint32_t size, int64_t pts);
};

}
