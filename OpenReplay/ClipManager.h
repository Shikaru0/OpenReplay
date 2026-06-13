#pragma once
#include <string>
#include <vector>
#include <filesystem>

namespace fs = std::filesystem;

class ClipManager {
public:
    ClipManager();
    ~ClipManager() = default;

    void refresh(const std::string& directory);
    size_t count() const { return m_clips.size(); }
    const fs::path& get(size_t index) const { return m_clips[index]; }
    bool remove(size_t index);
    void clear();

    struct ClipInfo {
        fs::path path;
        std::string displayName;
        std::string dateStr;
        std::string sizeStr;
        uint64_t fileSize;
    };

    std::vector<ClipInfo> enumerate(const std::string& directory);
    static std::string formatSize(uint64_t bytes);

private:
    std::vector<fs::path> m_clips;

    static bool isMediaExtension(const std::string& ext);
};
