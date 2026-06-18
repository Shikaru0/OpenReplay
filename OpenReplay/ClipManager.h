#pragma once
#include <string>
#include <vector>
#include <filesystem>

namespace fs = std::filesystem;

class ClipManager {
public:
    ClipManager();
    ~ClipManager() = default;

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
    static bool isMediaExtension(const std::string& ext);
};
