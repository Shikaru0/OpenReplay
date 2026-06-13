#include "ClipManager.h"
#include <iostream>
#include <algorithm>
#include <cstring>
#include <ctime>
#include <chrono>
#include <sstream>

ClipManager::ClipManager() {}

void ClipManager::refresh(const std::string& directory) {
    m_clips.clear();
    try {
        for (auto& entry : fs::directory_iterator(directory)) {
            auto ext = entry.path().extension().string();
            std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
            if (isMediaExtension(ext)) {
                m_clips.push_back(entry.path());
            }
        }
    } catch (const std::exception& e) {
        std::cerr << "[ClipManager] " << e.what() << "\n";
    }
}

bool ClipManager::remove(size_t index) {
    if (index >= m_clips.size()) return false;
    bool ok = fs::remove(m_clips[index]);
    if (ok) m_clips.erase(m_clips.begin() + index);
    return ok;
}

void ClipManager::clear() {
    m_clips.clear();
}

bool ClipManager::isMediaExtension(const std::string& ext) {
    return ext == ".mp4" || ext == ".mkv" || ext == ".webm" || ext == ".mov" ||
           ext == ".avi" || ext == ".wav" || ext == ".flac" || ext == ".mp3" || ext == ".ogg";
}

std::string ClipManager::formatSize(uint64_t bytes) {
    char buf[32];
    if (bytes < 1024)
        snprintf(buf, sizeof(buf), "%llu B", (unsigned long long)bytes);
    else if (bytes < 1024 * 1024)
        snprintf(buf, sizeof(buf), "%.1f KB", (double)bytes / 1024);
    else
        snprintf(buf, sizeof(buf), "%.1f MB", (double)bytes / (1024 * 1024));
    return buf;
}

std::vector<ClipManager::ClipInfo> ClipManager::enumerate(const std::string& directory) {
    std::vector<ClipInfo> result;
    try {
        for (auto& entry : fs::directory_iterator(directory)) {
            auto ext = entry.path().extension().string();
            std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
            if (!isMediaExtension(ext)) continue;

            ClipInfo info;
            info.path = entry.path();
            info.fileSize = entry.file_size();
            info.sizeStr = formatSize(info.fileSize);

            auto ftEpoch = entry.last_write_time().time_since_epoch();
            auto ftSec = std::chrono::duration_cast<std::chrono::seconds>(ftEpoch).count();
            std::time_t tt = (std::time_t)(ftSec - 11644473600LL);
            std::tm tm;
            localtime_s(&tm, &tt);
            char buf[64];
            strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M", &tm);
            info.dateStr = buf;

            info.displayName = entry.path().filename().string();

            result.push_back(std::move(info));
        }
    } catch (const std::exception& e) {
        std::cerr << "[ClipManager] " << e.what() << "\n";
    }
    return result;
}
