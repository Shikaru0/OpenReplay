#pragma once
#include <string>
#include <cstdint>
#include <vector>
#include <fstream>
#include "Muxer.h"
#include "json.hpp"

using json = nlohmann::json;

namespace OpenReplay {

struct RecorderConfig {
    int maxFPS = 60;
    int captureWidth = 1920;
    int captureHeight = 1080;
    int videoBitrate = 8'000'000;
    int audioBitrate = 192'000;
    int audioCodec = 0; // 0 = MP3 (WMP), 1 = AAC (quality)
    int keyframeIntervalSec = 2;
    int vbvBufferMs = 1000;
    bool enableAudio = true;
    bool enableVideo = true;
    bool showRecordingDot = true;
    bool autoSaveOnStop = false;
    bool minimizeToTray = true;
    bool enablePreAnalysis = true;
    bool enableMic = false;
    bool captureCursor = true;
    std::string bufferFilePath = "OpenReplay_Buffer.dat";
    uint64_t bufferSizeMB = 1024;
    OutputFormat outputFormat = OutputFormat::MP4;
    std::string encoderPreset = "ultralowlatency";
    int captureMonitor = 0;
    std::string rtmpUrl;
    std::vector<std::string> audioDeviceIds;
    std::string micDeviceId;
};

OutputFormat formatFromIndex(int idx);
OutputFormat audioFormatFromIndex(int idx);
int formatToIndex(OutputFormat fmt);

void applyQualityPreset(RecorderConfig& config, const char* name);

void configToJson(const RecorderConfig& config, json& j);
void configFromJson(const json& j, RecorderConfig& config);

struct ProfileManager {
    json profiles;
    void load(const char* path);
    void save(const char* path);
    void apply(const std::string& name, RecorderConfig& config);
    void saveCurrent(const std::string& name, const RecorderConfig& config);
    bool exportProfile(const std::string& name, const char* exportPath);
    bool importProfile(const char* importPath);
    std::vector<std::string> list() const;
};

}
