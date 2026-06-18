#include "DiskBackedBuffer.h"
#include <iostream>
#include <cstring>
#include <algorithm>

namespace OpenReplay {

static size_t roundUpPow2(size_t v) {
    v--;
    v |= v >> 1;
    v |= v >> 2;
    v |= v >> 4;
    v |= v >> 8;
    v |= v >> 16;
#if SIZE_MAX > 0xFFFFFFFF
    v |= v >> 32;
#endif
    return ++v;
}

bool DiskBackedBuffer::init(const char* filepath, uint64_t maxBytes) {
    std::lock_guard<std::mutex> lock(m_mtx);

    m_fileHandle = CreateFileA(
        filepath,
        GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ,
        nullptr,
        CREATE_ALWAYS,
        0,
        nullptr);

    if (m_fileHandle == INVALID_HANDLE_VALUE) {
        std::cerr << "[Buffer] CreateFile failed: " << GetLastError() << "\n";
        return false;
    }

    m_filepath = filepath;
    m_fileSize = maxBytes;

    LARGE_INTEGER li;
    li.QuadPart = maxBytes - 1;
    if (!SetFilePointerEx(m_fileHandle, li, nullptr, FILE_BEGIN)) {
        close(); return false;
    }
    if (!SetEndOfFile(m_fileHandle)) {
        std::cerr << "[Buffer] SetEndOfFile failed: " << GetLastError() << "\n";
        close(); return false;
    }
    li.QuadPart = 0;
    SetFilePointerEx(m_fileHandle, li, nullptr, FILE_BEGIN);

    m_mapHandle = CreateFileMapping(
        m_fileHandle,
        nullptr,
        PAGE_READWRITE,
        (DWORD)(maxBytes >> 32),
        (DWORD)(maxBytes & 0xFFFFFFFF),
        nullptr);

    if (!m_mapHandle) {
        std::cerr << "[Buffer] CreateFileMapping failed: " << GetLastError() << "\n";
        close(); return false;
    }

    m_mappedView = (uint8_t*)MapViewOfFile(
        m_mapHandle,
        FILE_MAP_ALL_ACCESS,
        0, 0,
        (SIZE_T)maxBytes);

    if (!m_mappedView) {
        std::cerr << "[Buffer] MapViewOfFile failed: " << GetLastError() << "\n";
        close(); return false;
    }

    size_t rawCapacity = std::max<size_t>(maxBytes / 4096, size_t(65536));
    m_idxCapacity = roundUpPow2(rawCapacity);
    m_idxMask = m_idxCapacity - 1;
    m_idx.resize(m_idxCapacity);

    m_writeOffset = 0;
    m_idxHead = 0;
    m_idxCount = 0;
    m_generation = 0;
    m_oldestPts = 0;
    m_latestPts = 0;
    m_extradata.clear();
    m_extradataInjected = false;

    return true;
}

void DiskBackedBuffer::close() {
    std::lock_guard<std::mutex> lock(m_mtx);
    if (m_mappedView) {
        UnmapViewOfFile(m_mappedView);
        m_mappedView = nullptr;
    }
    if (m_mapHandle) {
        CloseHandle(m_mapHandle);
        m_mapHandle = nullptr;
    }
    if (m_fileHandle != INVALID_HANDLE_VALUE) {
        CloseHandle(m_fileHandle);
        m_fileHandle = INVALID_HANDLE_VALUE;
    }
    m_idx.clear();
    m_idxCapacity = 0;
}

bool DiskBackedBuffer::writePacket(const uint8_t* data, uint32_t size, int64_t pts, PacketType type) {
    std::lock_guard<std::mutex> lock(m_mtx);
    if (!m_mappedView) return false;

    uint32_t totalSize = sizeof(PacketHeader) + size;
    if (totalSize > m_fileSize) return false;

    if (m_writeOffset + totalSize > m_fileSize) {
        m_writeOffset = 0;
        m_generation++;
    }

    uint64_t thisOffset = m_writeOffset;

    PacketHeader hdr;
    hdr.magic = 0x4B52504F;
    hdr.size = size;
    hdr.pts = pts;
    hdr.type = type;
    std::memset(hdr.reserved, 0, sizeof(hdr.reserved));

    std::memcpy(m_mappedView + thisOffset, &hdr, sizeof(hdr));
    if (size > 0) {
        std::memcpy(m_mappedView + thisOffset + sizeof(hdr), data, size);
    }

    m_writeOffset += totalSize;

    IndexEntry entry;
    entry.fileOffset = thisOffset;
    entry.pts = pts;
    entry.type = type;
    entry.size = totalSize;
    entry.generation = m_generation;

    m_idx[m_idxHead] = entry;
    m_idxHead = (m_idxHead + 1) & m_idxMask;
    if (m_idxCount < m_idxCapacity) {
        m_idxCount++;
        if (m_idxCount == 1) m_oldestPts = pts;
    } else {
        m_oldestPts = m_idx[m_idxHead].pts;
    }
    m_latestPts = pts;

    return true;
}

std::vector<RetrievedPacket> DiskBackedBuffer::readRange(int64_t endPts, int64_t durationUs) const {
    std::lock_guard<std::mutex> lock(m_mtx);
    std::vector<RetrievedPacket> result;
    if (!m_mappedView || m_idxCount == 0) return result;

    int64_t startPts = endPts - durationUs;
    size_t startIdx = (m_idxHead + m_idxCapacity - m_idxCount) & m_idxMask;

    for (size_t i = 0; i < m_idxCount; ++i) {
        size_t idx = (startIdx + i) & m_idxMask;
        const auto& entry = m_idx[idx];
        if (entry.generation == m_generation &&
            entry.pts >= startPts && entry.pts <= endPts &&
            entry.type != PacketType::CodecExtradata) {
            RetrievedPacket rp;
            uint32_t payloadSize = entry.size - sizeof(PacketHeader);
            rp.data.resize(payloadSize);
            rp.pts = entry.pts;
            rp.type = entry.type;

            std::memcpy(rp.data.data(),
                       m_mappedView + entry.fileOffset + sizeof(PacketHeader),
                       payloadSize);

            result.push_back(std::move(rp));
        }
    }

    return result;
}

size_t DiskBackedBuffer::getPacketCount() const {
    std::lock_guard<std::mutex> lock(m_mtx);
    return m_idxCount;
}

bool DiskBackedBuffer::getPacketInfo(size_t index, int64_t& pts, PacketType& type, uint32_t& dataSize) const {
    std::lock_guard<std::mutex> lock(m_mtx);
    if (index >= m_idxCount || !m_mappedView) return false;

    size_t startIdx = (m_idxHead + m_idxCapacity - m_idxCount) & m_idxMask;
    size_t idx = (startIdx + index) & m_idxMask;
    const auto& entry = m_idx[idx];

    if (entry.generation != m_generation) return false;

    pts = entry.pts;
    type = entry.type;
    dataSize = entry.size - sizeof(PacketHeader);
    return true;
}

bool DiskBackedBuffer::readPacketData(size_t index, uint8_t* outData) const {
    std::lock_guard<std::mutex> lock(m_mtx);
    if (index >= m_idxCount || !m_mappedView) return false;

    size_t startIdx = (m_idxHead + m_idxCapacity - m_idxCount) & m_idxMask;
    size_t idx = (startIdx + index) & m_idxMask;
    const auto& entry = m_idx[idx];

    if (entry.generation != m_generation) return false;

    uint32_t payloadSize = entry.size - sizeof(PacketHeader);
    std::memcpy(outData, m_mappedView + entry.fileOffset + sizeof(PacketHeader), payloadSize);
    return true;
}

void DiskBackedBuffer::injectExtradata() {
    std::lock_guard<std::mutex> lock(m_mtx);
    if (m_extradataInjected || m_extradata.empty() || !m_mappedView) return;

    uint32_t totalSize = sizeof(PacketHeader) + (uint32_t)m_extradata.size();
    if (totalSize > m_fileSize) return;

    if (m_writeOffset + totalSize > m_fileSize)
        m_writeOffset = 0;

    uint64_t thisOffset = m_writeOffset;

    PacketHeader hdr;
    hdr.magic = 0x4B52504F;
    hdr.size = (uint32_t)m_extradata.size();
    hdr.pts = 0;
    hdr.type = PacketType::CodecExtradata;
    std::memset(hdr.reserved, 0, sizeof(hdr.reserved));

    std::memcpy(m_mappedView + thisOffset, &hdr, sizeof(hdr));
    std::memcpy(m_mappedView + thisOffset + sizeof(hdr),
                m_extradata.data(), m_extradata.size());
    m_writeOffset += totalSize;

    IndexEntry entry;
    entry.fileOffset = thisOffset;
    entry.pts = 0;
    entry.type = PacketType::CodecExtradata;
    entry.size = totalSize;
    entry.generation = m_generation;

    m_idx[m_idxHead] = entry;
    m_idxHead = (m_idxHead + 1) & m_idxMask;
    if (m_idxCount < m_idxCapacity) m_idxCount++;

    m_extradataInjected = true;
}

void DiskBackedBuffer::setExtradata(const uint8_t* data, uint32_t size) {
    std::lock_guard<std::mutex> lock(m_mtx);
    m_extradata.assign(data, data + size);
    m_extradataInjected = false;
}

void DiskBackedBuffer::reset(uint64_t maxBytes) {
    std::lock_guard<std::mutex> lock(m_mtx);
    if (maxBytes > 0) m_fileSize = maxBytes;

    if (m_mappedView) {
        UnmapViewOfFile(m_mappedView);
        m_mappedView = nullptr;
    }
    if (m_mapHandle) {
        CloseHandle(m_mapHandle);
        m_mapHandle = nullptr;
    }
    if (m_fileHandle != INVALID_HANDLE_VALUE) {
        CloseHandle(m_fileHandle);
        m_fileHandle = INVALID_HANDLE_VALUE;
    }

    m_fileHandle = CreateFileA(
        m_filepath.c_str(),
        GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ,
        nullptr,
        CREATE_ALWAYS,
        0,
        nullptr);

    if (m_fileHandle == INVALID_HANDLE_VALUE) return;

    LARGE_INTEGER li;
    li.QuadPart = m_fileSize - 1;
    SetFilePointerEx(m_fileHandle, li, nullptr, FILE_BEGIN);
    SetEndOfFile(m_fileHandle);
    SetFilePointerEx(m_fileHandle, li, nullptr, FILE_BEGIN);
    if (m_fileSize > 0) {
        DWORD written;
        WriteFile(m_fileHandle, "", 1, &written, nullptr);
    }
    li.QuadPart = 0;
    SetFilePointerEx(m_fileHandle, li, nullptr, FILE_BEGIN);

    m_mapHandle = CreateFileMapping(
        m_fileHandle, nullptr, PAGE_READWRITE,
        (DWORD)(m_fileSize >> 32),
        (DWORD)(m_fileSize & 0xFFFFFFFF),
        nullptr);

    if (m_mapHandle) {
        m_mappedView = (uint8_t*)MapViewOfFile(
            m_mapHandle, FILE_MAP_ALL_ACCESS, 0, 0, (SIZE_T)m_fileSize);
    }

    m_writeOffset = 0;
    m_idxHead = 0;
    m_idxCount = 0;
    m_generation = 0;
    m_oldestPts = 0;
    m_latestPts = 0;
    m_extradata.clear();
    m_extradataInjected = false;
}

}
