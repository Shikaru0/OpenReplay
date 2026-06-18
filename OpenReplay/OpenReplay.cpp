#include "OpenReplay.h"
#include <iostream>
#include <chrono>
#include <vector>
#include <cstring>
#include <algorithm>

namespace OpenReplay {

constexpr int64_t kFpsCalcIntervalMs = 2000;

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

    if (config.enableAudio) {
        auto deviceIds = config.audioDeviceIds;
        if (deviceIds.empty()) deviceIds.push_back("");
        for (auto& devId : deviceIds) {
            auto cap = std::make_unique<AudioCapture>();
            cap->setAudioCallback(
                [this](const uint8_t* d, uint32_t s, int64_t p) {
                    onAudioData(d, s, p);
                });
            if (cap->init(AudioCapture::Loopback, devId)) {
                m_audioCaptures.push_back(std::move(cap));
            }
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
        if (!m_screenCapture->init(config.captureMonitor)) {
            std::cerr << "[Engine] ScreenCapture init failed\n";
            return false;
        }
        m_stats.isHdr.store(m_screenCapture->isHdr());
        {
            std::lock_guard<std::mutex> lock(m_configMtx);
            m_config.captureWidth = m_screenCapture->width();
            m_config.captureHeight = m_screenCapture->height();
        }

        m_videoEncoder = std::make_unique<VideoEncoder>();
        m_videoEncoder->setPacketCallback(
            [this](const uint8_t* d, uint32_t s, int64_t p, bool k) {
                onEncodedPacket(d, s, p, k);
            });

        if (!m_videoEncoder->init(m_screenCapture->width(), m_screenCapture->height(),
                                   config.maxFPS, config.videoBitrate,
                                   config.keyframeIntervalSec,
                                   config.encoderPreset.c_str(),
                                   config.vbvBufferMs,
                                   config.enablePreAnalysis)) {
            std::cerr << "[Engine] Failed to init video encoder\n";
            return false;
        }
    }

    m_muxer = std::make_unique<Muxer>();
    m_initialized = true;
    return true;
}

void OpenReplayEngine::startCapture() {
    if (!m_initialized || m_isCapturing.exchange(true)) return;

    m_encoderActive = false;

    SetThreadExecutionState(ES_CONTINUOUS | ES_SYSTEM_REQUIRED | ES_DISPLAY_REQUIRED);

    LARGE_INTEGER qpcNow;
    QueryPerformanceCounter(&qpcNow);
    m_qpcOrigin = qpcNow.QuadPart;

    m_stats.framesCaptured.store(0);
    m_stats.framesEncoded.store(0);
    m_stats.audioPackets.store(0);
    m_stats.currentFps.store(0.0);
    m_stats.durationMs.store(0);

    {
        std::lock_guard<std::mutex> lock(m_configMtx);
        uint64_t bufBytes = m_config.bufferSizeMB * 1024ULL * 1024;
        m_diskBuffer->reset(bufBytes);
    }
    for (auto& cap : m_audioCaptures)
        cap->start();
    if (m_micCapture)
        m_micCapture->start();

    if (m_config.enableVideo) {
        m_captureThread = std::thread(&OpenReplayEngine::captureThreadLoop, this);
    }
}

void OpenReplayEngine::stopCapture() {
    if (!m_isCapturing.exchange(false)) return;

    SetThreadExecutionState(ES_CONTINUOUS);

    for (auto& cap : m_audioCaptures)
        cap->stop();
    if (m_micCapture)
        m_micCapture->stop();

    if (m_captureThread.joinable())
        m_captureThread.join();

    if (m_videoEncoder && m_encoderActive) {
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
            m_encoderActive = true;
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

void OpenReplayEngine::onMicAudioData(const uint8_t* data, uint32_t size,
                                       int64_t pts) {
    m_diskBuffer->writePacket(data, size, pts, PacketType::AudioMic);
    m_stats.audioPackets++;
}

void OpenReplayEngine::onFrameDropped() {
    m_stats.framesDropped++;
}

void OpenReplayEngine::onAudioData(const uint8_t* data, uint32_t size,
                                    int64_t pts) {
    m_diskBuffer->writePacket(data, size, pts, PacketType::AudioLoopback);
    m_stats.audioPackets++;
}

const char* OpenReplayEngine::getCodecName() const {
    return m_videoEncoder ? m_videoEncoder->codecName() : "none";
}

bool OpenReplayEngine::saveLastMoments(const char* outputPath,
                                        int durationSeconds,
                                        Muxer::ProgressCallback progress) {
    if (!m_initialized || !m_diskBuffer || !m_diskBuffer->isInitialized()) {
        return false;
    }

    int64_t endPts = m_diskBuffer->getLatestPts();
    int64_t durationUs = (int64_t)durationSeconds * 1'000'000;

    std::vector<AudioStreamInfo> audioStreams;
    if (!m_audioCaptures.empty()) {
        auto& cap = m_audioCaptures.front();
        audioStreams.push_back({
            .sampleRate = cap->getSampleRate(),
            .channels = cap->getChannels(),
            .bitsPerSample = cap->getBitsPerSample(),
            .bitrate = m_config.audioBitrate,
            .packetType = PacketType::AudioLoopback,
            .title = "Loopback"
        });
    }
    if (m_micCapture) {
        audioStreams.push_back({
            .sampleRate = m_micCapture->getSampleRate(),
            .channels = m_micCapture->getChannels(),
            .bitsPerSample = m_micCapture->getBitsPerSample(),
            .bitrate = m_config.audioBitrate,
            .packetType = PacketType::AudioMic,
            .title = "Microphone"
        });
    }

    AVCodecID codecId = m_videoEncoder ? m_videoEncoder->codecId() : AV_CODEC_ID_H264;

    return m_muxer->muxStreaming(
        outputPath, m_config.outputFormat,
        m_diskBuffer.get(),
        endPts, durationUs,
        m_config.captureWidth, m_config.captureHeight, m_config.maxFPS,
        audioStreams,
        m_config.audioCodec,
        codecId,
        progress);
}

}
