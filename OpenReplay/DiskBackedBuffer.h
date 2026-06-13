#pragma once
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>
#include <mutex>
#include <windows.h>

namespace OpenReplay {

enum class PacketType : uint8_t {
    VideoKeyFrame = 0,
    VideoDeltaFrame = 1,
    AudioData = 2,
    CodecExtradata = 3
};

#pragma pack(push, 1)
struct PacketHeader {
    uint32_t magic;
    uint32_t size;
    int64_t pts;
    PacketType type;
    uint8_t reserved[7];
};
static_assert(sizeof(PacketHeader) == 24, "PacketHeader must be 24 bytes");
#pragma pack(pop)

struct RetrievedPacket {
    std::vector<uint8_t> data;
    int64_t pts;
    PacketType type;
};

class DiskBackedBuffer {
public:
    DiskBackedBuffer() = default;
    ~DiskBackedBuffer() { close(); }

    DiskBackedBuffer(const DiskBackedBuffer&) = delete;
    DiskBackedBuffer& operator=(const DiskBackedBuffer&) = delete;

    bool init(const char* filepath, uint64_t maxBytes);
    void close();

    bool writePacket(const uint8_t* data, uint32_t size, int64_t pts, PacketType type);
    std::vector<RetrievedPacket> readRange(int64_t endPts, int64_t durationUs) const;

    size_t getPacketCount() const;
    bool getPacketInfo(size_t index, int64_t& pts, PacketType& type, uint32_t& dataSize) const;
    bool readPacketData(size_t index, uint8_t* outData) const;

    void injectExtradata();
    const std::vector<uint8_t>& getExtradata() const { return m_extradata; }
    void setExtradata(const uint8_t* data, uint32_t size);

    int64_t getOldestPts() const { return m_oldestPts; }
    int64_t getLatestPts() const { return m_latestPts; }
    bool isInitialized() const { return m_mappedView != nullptr; }
    void reset();

private:
    struct IndexEntry {
        uint64_t fileOffset;
        int64_t pts;
        PacketType type;
        uint32_t size;
        uint32_t generation;
    };

    mutable std::mutex m_mtx;
    HANDLE m_fileHandle = INVALID_HANDLE_VALUE;
    HANDLE m_mapHandle = nullptr;
    uint8_t* m_mappedView = nullptr;
    std::string m_filepath;
    uint64_t m_fileSize = 0;
    uint64_t m_writeOffset = 0;

    std::vector<IndexEntry> m_idx;
    size_t m_idxHead = 0;
    size_t m_idxCount = 0;
    size_t m_idxCapacity = 0;
    size_t m_idxMask = 0;

    uint32_t m_generation = 0;

    int64_t m_oldestPts = 0;
    int64_t m_latestPts = 0;
    std::vector<uint8_t> m_extradata;
    bool m_extradataInjected = false;
};

}
