#include "Config.h"
#include <iostream>
#include <filesystem>

namespace OpenReplay {

OutputFormat formatFromIndex(int idx) {
    switch (idx) {
        case 0:  return OutputFormat::MP4;
        case 1:  return OutputFormat::MKV;
        case 2:  return OutputFormat::WEBM;
        case 3:  return OutputFormat::MOV;
        case 4:  return OutputFormat::AVI;
        case 5:  return OutputFormat::WAV;
        case 6:  return OutputFormat::FLAC;
        case 7:  return OutputFormat::MP3;
        case 8:  return OutputFormat::OGG;
        default: return OutputFormat::MP4;
    }
}

OutputFormat audioFormatFromIndex(int idx) {
    switch (idx) {
        case 0:  return OutputFormat::WAV;
        case 1:  return OutputFormat::FLAC;
        case 2:  return OutputFormat::MP3;
        case 3:  return OutputFormat::OGG;
        default: return OutputFormat::WAV;
    }
}

int formatToIndex(OutputFormat fmt) {
    switch (fmt) {
        case OutputFormat::MP4:  return 0;
        case OutputFormat::MKV:  return 1;
        case OutputFormat::WEBM: return 2;
        case OutputFormat::MOV:  return 3;
        case OutputFormat::AVI:  return 4;
        case OutputFormat::WAV:  return 5;
        case OutputFormat::FLAC: return 6;
        case OutputFormat::MP3:  return 7;
        case OutputFormat::OGG:  return 8;
    }
    return 0;
}

void applyQualityPreset(RecorderConfig& config, const char* name) {
    if (strcmp(name, "lossless") == 0) {
        config.videoBitrate = 0;
        config.encoderPreset = "highquality";
        config.keyframeIntervalSec = 1;
    } else if (strcmp(name, "high") == 0) {
        config.videoBitrate = 50'000'000;
        config.encoderPreset = "highquality";
        config.keyframeIntervalSec = 2;
    } else if (strcmp(name, "standard") == 0) {
        config.videoBitrate = 25'000'000;
        config.encoderPreset = "ultralowlatency";
        config.keyframeIntervalSec = 2;
    } else if (strcmp(name, "stream") == 0) {
        config.videoBitrate = 8'000'000;
        config.encoderPreset = "ultralowlatency";
        config.keyframeIntervalSec = 2;
    }
}

void configToJson(const RecorderConfig& config, json& j) {
    j["fps"] = config.maxFPS;
    j["capture_width"] = config.captureWidth;
    j["capture_height"] = config.captureHeight;
    j["video_bitrate"] = config.videoBitrate;
    j["audio_bitrate"] = config.audioBitrate;
    j["audio_codec"] = config.audioCodec;
    j["keyframe_interval_sec"] = config.keyframeIntervalSec;
    j["vbv_buffer_ms"] = config.vbvBufferMs;
    j["encoder_preset"] = config.encoderPreset;
    j["enable_audio"] = config.enableAudio;
    j["enable_video"] = config.enableVideo;
    j["show_recording_dot"] = config.showRecordingDot;
    j["auto_save_on_stop"] = config.autoSaveOnStop;
    j["minimize_to_tray"] = config.minimizeToTray;
    j["enable_pre_analysis"] = config.enablePreAnalysis;
    j["force_cfr"] = config.forceCfr;
    j["enable_mic"] = config.enableMic;
    j["capture_cursor"] = config.captureCursor;
    j["buffer_file"] = config.bufferFilePath;
    j["buffer_size_mb"] = (int64_t)config.bufferSizeMB;
    j["capture_monitor"] = config.captureMonitor;
    j["output_format"] = formatToIndex(config.outputFormat);
    j["rtmp_url"] = config.rtmpUrl;
    j["audio_device_id"] = config.audioDeviceId;
    j["mic_device_id"] = config.micDeviceId;
}

void configFromJson(const json& j, RecorderConfig& config) {
    config.maxFPS = j.value("fps", 60);
    config.captureWidth = j.value("capture_width", 1920);
    config.captureHeight = j.value("capture_height", 1080);
    config.videoBitrate = j.value("video_bitrate", 8'000'000);
    config.audioBitrate = j.value("audio_bitrate", 192'000);
    config.audioCodec = j.value("audio_codec", 0);
    config.keyframeIntervalSec = j.value("keyframe_interval_sec", 2);
    config.vbvBufferMs = j.value("vbv_buffer_ms", 1000);
    config.encoderPreset = j.value("encoder_preset", "ultralowlatency");
    config.enableAudio = j.value("enable_audio", true);
    config.enableVideo = j.value("enable_video", true);
    config.showRecordingDot = j.value("show_recording_dot", true);
    config.autoSaveOnStop = j.value("auto_save_on_stop", false);
    config.minimizeToTray = j.value("minimize_to_tray", true);
    config.enablePreAnalysis = j.value("enable_pre_analysis", true);
    config.forceCfr = j.value("force_cfr", true);
    config.enableMic = j.value("enable_mic", false);
    config.captureCursor = j.value("capture_cursor", true);
    config.bufferFilePath = j.value("buffer_file", "OpenReplay_Buffer.dat");
    config.bufferSizeMB = j.value("buffer_size_mb", (int64_t)1024);
    config.captureMonitor = j.value("capture_monitor", 0);
    int fmtIdx = j.value("output_format", 0);
    config.outputFormat = formatFromIndex(fmtIdx);
    config.rtmpUrl = j.value("rtmp_url", "");
    config.audioDeviceId = j.value("audio_device_id", "");
    config.micDeviceId = j.value("mic_device_id", "");
}

void ProfileManager::load(const char* path) {
    std::ifstream pf(path);
    if (pf.is_open()) {
        try { profiles = json::parse(pf); } catch (const std::exception& e) {
            std::cerr << "[Config] Failed to parse profiles: " << e.what() << "\n";
        }
    }
    if (!profiles.is_object()) profiles = json::object();
}

void ProfileManager::save(const char* path) {
    std::ofstream pf(path);
    pf << profiles.dump(2);
}

void ProfileManager::apply(const std::string& name, RecorderConfig& config) {
    if (!profiles.contains(name)) return;
    auto& p = profiles[name];
    if (p.contains("maxFPS")) config.maxFPS = p["maxFPS"];
    if (p.contains("captureWidth")) config.captureWidth = p["captureWidth"];
    if (p.contains("captureHeight")) config.captureHeight = p["captureHeight"];
    if (p.contains("videoBitrate")) config.videoBitrate = p["videoBitrate"];
    if (p.contains("audioBitrate")) config.audioBitrate = p["audioBitrate"];
    if (p.contains("encoderPreset")) config.encoderPreset = p["encoderPreset"];
    if (p.contains("enableAudio")) config.enableAudio = p["enableAudio"];
    if (p.contains("keyframeIntervalSec")) config.keyframeIntervalSec = p["keyframeIntervalSec"];
    if (p.contains("vbvBufferMs")) config.vbvBufferMs = p["vbvBufferMs"];
    if (p.contains("captureMonitor")) config.captureMonitor = p["captureMonitor"];
    if (p.contains("enableMic")) config.enableMic = p["enableMic"];
    if (p.contains("captureCursor")) config.captureCursor = p["captureCursor"];
    if (p.contains("audioDeviceId")) config.audioDeviceId = p["audioDeviceId"];
    if (p.contains("micDeviceId")) config.micDeviceId = p["micDeviceId"];
}

void ProfileManager::saveCurrent(const std::string& name, const RecorderConfig& config) {
    json p;
    p["maxFPS"] = config.maxFPS;
    p["captureWidth"] = config.captureWidth;
    p["captureHeight"] = config.captureHeight;
    p["videoBitrate"] = config.videoBitrate;
    p["audioBitrate"] = config.audioBitrate;
    p["encoderPreset"] = config.encoderPreset;
    p["enableAudio"] = config.enableAudio;
    p["keyframeIntervalSec"] = config.keyframeIntervalSec;
    p["vbvBufferMs"] = config.vbvBufferMs;
    p["captureMonitor"] = config.captureMonitor;
    p["enableMic"] = config.enableMic;
    p["captureCursor"] = config.captureCursor;
    p["audioDeviceId"] = config.audioDeviceId;
    p["micDeviceId"] = config.micDeviceId;
    profiles[name] = std::move(p);
}

bool ProfileManager::exportProfile(const std::string& name, const char* exportPath) {
    if (!profiles.contains(name)) return false;
    std::ofstream pf(exportPath);
    if (!pf.is_open()) return false;
    pf << profiles[name].dump(2);
    return true;
}

bool ProfileManager::importProfile(const char* importPath) {
    std::ifstream pf(importPath);
    if (!pf.is_open()) return false;
    try {
        json j = json::parse(pf);
        if (!j.is_object()) return false;
        std::string name = std::filesystem::path(importPath).stem().string();
        profiles[name] = std::move(j);
        return true;
    } catch (const std::exception& e) {
        std::cerr << "[Config] Import failed: " << e.what() << "\n";
        return false;
    }
}

std::vector<std::string> ProfileManager::list() const {
    std::vector<std::string> names;
    for (auto& [key, _] : profiles.items())
        names.push_back(key);
    return names;
}

}
