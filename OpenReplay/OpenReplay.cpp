#include "OpenReplay.h"
#include <iostream>
#include <chrono>
#include <vector>
#include <cstring>
#include <algorithm>

namespace OpenReplay {

constexpr int64_t kFpsCalcIntervalMs = 2000;
constexpr int64_t kMicMixWindowUs = 10000;
constexpr int64_t kMicQueueDrainUs = 100000;

OpenReplayEngine::OpenReplayEngine() = default;

OpenReplayEngine::~OpenReplayEngine() {
    stopCapture();
}

bool OpenReplayEngine::init(const RecorderConfig& config) {
    {
        std::lock_guard<std::mutex> lock(m_configMtx);
        m_config = config;
    }

    LARGE_INTEGER qpcFreq;
    QueryPerformanceFrequency(&qpcFreq);
    m_qpcFreq = qpcFreq.QuadPart;
    LARGE_INTEGER qpcNow;
    QueryPerformanceCounter(&qpcNow);
    m_qpcOrigin = qpcNow.QuadPart;

    uint64_t bufferBytes = config.bufferSizeMB * 1024 * 1024;
    m_diskBuffer = std::make_unique<DiskBackedBuffer>();
    if (!m_diskBuffer->init(config.bufferFilePath.c_str(), bufferBytes)) {
        std::cerr << "[Engine] Failed to init disk buffer\n";
        return false;
    }

    if (config.enableVideo) {
        m_videoEncoder = std::make_unique<VideoEncoder>();
        m_videoEncoder->setPacketCallback(
            [this](const uint8_t* d, uint32_t s, int64_t p, bool k) {
                onEncodedPacket(d, s, p, k);
            });

        if (!m_videoEncoder->init(config.captureWidth, config.captureHeight,
                                   config.maxFPS, config.videoBitrate,
                                   config.keyframeIntervalSec,
                                   config.encoderPreset.c_str(),
                                   config.vbvBufferMs,
                                   config.enablePreAnalysis)) {
            std::cerr << "[Engine] Failed to init video encoder\n";
            return false;
        }
    }

    if (config.enableAudio) {
        m_audioCapture = std::make_unique<AudioCapture>();
        m_audioCapture->setAudioCallback(
            [this](const uint8_t* d, uint32_t s, int64_t p) {
                onAudioData(d, s, p);
            });

        if (!m_audioCapture->init(AudioCapture::Loopback, config.audioDeviceId)) {
            m_audioCapture.reset();
        }
    }

    if (config.enableMic) {
        m_micCapture = std::make_unique<AudioCapture>();
        m_micCapture->setAudioCallback(
            [this](const uint8_t* d, uint32_t s, int64_t p) {
                onMicAudioData(d, s, p);
            });

        if (!m_micCapture->init(AudioCapture::Microphone, config.micDeviceId)) {
            m_micCapture.reset();
        }
    }

    if (config.enableVideo) {
        m_screenCapture = std::make_unique<ScreenCapture>();
        if (!m_screenCapture->init()) {
            std::cerr << "[Engine] ScreenCapture init failed\n";
            return false;
        }
        m_screenCapture->setCaptureCursor(config.captureCursor);
        m_stats.isHdr.store(m_screenCapture->isHdr());
    }

    m_muxer = std::make_unique<Muxer>();
    m_initialized = true;
    return true;
}

void OpenReplayEngine::startCapture() {
    if (!m_initialized || m_isCapturing.exchange(true)) return;

    SetThreadExecutionState(ES_CONTINUOUS | ES_SYSTEM_REQUIRED | ES_DISPLAY_REQUIRED);

    LARGE_INTEGER qpcNow;
    QueryPerformanceCounter(&qpcNow);
    m_qpcOrigin = qpcNow.QuadPart;

    m_stats.framesCaptured.store(0);
    m_stats.framesEncoded.store(0);
    m_stats.audioPackets.store(0);
    m_stats.currentFps.store(0.0);
    m_stats.durationMs.store(0);

    m_diskBuffer->reset();
    {
        std::lock_guard<std::mutex> lock(m_micMtx);
        m_micQueue.clear();
    }

    if (m_audioCapture)
        m_audioCapture->start();
    if (m_micCapture)
        m_micCapture->start();

    if (m_config.enableVideo) {
        m_captureThread = std::thread(&OpenReplayEngine::captureThreadLoop, this);
    }
}

void OpenReplayEngine::stopCapture() {
    if (!m_isCapturing.exchange(false)) return;

    SetThreadExecutionState(ES_CONTINUOUS);

    if (m_audioCapture)
        m_audioCapture->stop();
    if (m_micCapture)
        m_micCapture->stop();

    if (m_captureThread.joinable())
        m_captureThread.join();

    if (m_videoEncoder) {
        m_videoEncoder->flush();

        if (!m_videoEncoder->getExtradata().empty()) {
            m_diskBuffer->setExtradata(
                m_videoEncoder->getExtradata().data(),
                (uint32_t)m_videoEncoder->getExtradata().size());
            m_diskBuffer->injectExtradata();
        }
    }

}

void OpenReplayEngine::setConfig(const RecorderConfig& config) {
    std::lock_guard<std::mutex> lock(m_configMtx);
    m_config = config;
}

bool OpenReplayEngine::isCapturing() const {
    return m_isCapturing.load();
}

void OpenReplayEngine::captureThreadLoop() {
    RecorderConfig config;
    {
        std::lock_guard<std::mutex> lock(m_configMtx);
        config = m_config;
    }

    int fps = config.maxFPS;
    int64_t intervalUs = 1'000'000 / fps;

    std::vector<uint8_t> framePixels;
    int capW = 0, capH = 0;
    auto lastFpsCalc = std::chrono::steady_clock::now();
    int64_t framesInInterval = 0;

    while (m_isCapturing) {
        auto frameStart = std::chrono::steady_clock::now();

        LARGE_INTEGER qpcNow;
        QueryPerformanceCounter(&qpcNow);
        int64_t pts = (qpcNow.QuadPart - m_qpcOrigin) * 1'000'000 / m_qpcFreq;

        if (m_screenCapture->captureFrame(framePixels, capW, capH)) {
            m_videoEncoder->encode(framePixels.data(), pts);
            m_stats.framesEncoded++;
        } else {
            onFrameDropped();
        }
        m_stats.framesCaptured++;

        framesInInterval++;

        auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - frameStart).count();
        if (elapsed < intervalUs) {
            std::this_thread::sleep_for(
                std::chrono::microseconds(intervalUs - elapsed));
        }

        auto now = std::chrono::steady_clock::now();
        auto intervalMs = std::chrono::duration_cast<std::chrono::milliseconds>(
            now - lastFpsCalc).count();
        if (intervalMs >= kFpsCalcIntervalMs) {
            m_stats.currentFps.store((double)framesInInterval * 1000.0 / intervalMs);
            framesInInterval = 0;
            lastFpsCalc = now;
        }

        m_stats.durationMs.store(pts / 1000);
    }
}

void OpenReplayEngine::onEncodedPacket(const uint8_t* data, uint32_t size,
                                        int64_t pts, bool isKey) {
    m_diskBuffer->writePacket(data, size, pts,
        isKey ? PacketType::VideoKeyFrame : PacketType::VideoDeltaFrame);
}

void OpenReplayEngine::mixWithMic(uint8_t* data, uint32_t size, int64_t pts) {
    std::lock_guard<std::mutex> lock(m_micMtx);
    int64_t windowUs = kMicMixWindowUs;

    while (!m_micQueue.empty() && m_micQueue.front().pts < pts - kMicQueueDrainUs)
        m_micQueue.pop_front();

    for (size_t i = 0; i < m_micQueue.size(); ++i) {
        auto& mp = m_micQueue[i];
        if (std::abs(mp.pts - pts) > windowUs) continue;
        if (mp.data.size() != size) continue;
        float* dest = reinterpret_cast<float*>(data);
        const float* src = reinterpret_cast<const float*>(mp.data.data());
        size_t samples = size / sizeof(float);
        for (size_t s = 0; s < samples; ++s) {
            float sum = dest[s] + src[s];
            if (sum < -1.0f) sum = -1.0f;
            if (sum > 1.0f) sum = 1.0f;
            dest[s] = sum;
        }
        m_micQueue.erase(m_micQueue.begin() + i);
        break;
    }
}

void OpenReplayEngine::onMicAudioData(const uint8_t* data, uint32_t size,
                                       int64_t pts) {
    MicPacket mp;
    mp.data.assign(data, data + size);
    mp.pts = pts;
    std::lock_guard<std::mutex> lock(m_micMtx);
    m_micQueue.push_back(std::move(mp));
}

void OpenReplayEngine::onFrameDropped() {
    m_stats.framesDropped++;
}

void OpenReplayEngine::onAudioData(const uint8_t* data, uint32_t size,
                                    int64_t pts) {
    if (m_mixBuf.size() < size)
        m_mixBuf.resize(size);
    std::memcpy(m_mixBuf.data(), data, size);
    if (m_micCapture)
        mixWithMic(m_mixBuf.data(), size, pts);
    m_diskBuffer->writePacket(m_mixBuf.data(), size, pts, PacketType::AudioData);
    m_stats.audioPackets++;
}

const char* OpenReplayEngine::getCodecName() const {
    return m_videoEncoder ? m_videoEncoder->codecName() : "none";
}

bool OpenReplayEngine::saveLastMoments(const char* outputPath,
                                        int durationSeconds,
                                        Muxer::ProgressCallback progress) {
    if (!m_initialized || !m_diskBuffer || !m_diskBuffer->isInitialized())
        return false;

    int64_t endPts = m_diskBuffer->getLatestPts();
    int64_t durationUs = (int64_t)durationSeconds * 1'000'000;

    int audioSR = m_audioCapture ? m_audioCapture->getSampleRate() : 0;
    int audioCh = m_audioCapture ? m_audioCapture->getChannels() : 0;
    int audioBPS = m_audioCapture ? m_audioCapture->getBitsPerSample() : 0;
    AVCodecID codecId = m_videoEncoder ? m_videoEncoder->codecId() : AV_CODEC_ID_H264;

    bool result = m_muxer->muxStreaming(
        outputPath, m_config.outputFormat,
        m_diskBuffer.get(),
        endPts, durationUs,
        m_config.captureWidth, m_config.captureHeight, m_config.maxFPS,
        audioSR, audioCh, audioBPS, m_config.audioBitrate,
        codecId, m_config.audioCodec,
        progress);

    return result;
}

}
