#pragma once
#include <cstdint>
#include <vector>
#include <string>
#include <functional>
#include "DiskBackedBuffer.h"

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswresample/swresample.h>
}

namespace OpenReplay {

enum class OutputFormat {
    MP4, MKV, WEBM, MOV, AVI, WAV, FLAC, MP3, OGG
};

struct AudioStreamInfo {
    int sampleRate = 0;
    int channels = 0;
    int bitsPerSample = 0;
    int bitrate = 192000;
    PacketType packetType = PacketType::AudioLoopback;
    const char* title = nullptr;
};

class Muxer {
public:
    using ProgressCallback = std::function<void(float percent)>;

    bool muxStreaming(const char* outputPath, OutputFormat format,
                      const DiskBackedBuffer* buffer,
                      int64_t endPts, int64_t durationUs,
                      int width, int height, int fps,
                      const std::vector<AudioStreamInfo>& audioStreams,
                      int audioCodecPref,
                      AVCodecID videoCodecId,
                      ProgressCallback progress = nullptr);

    static const char* extension(OutputFormat fmt);
    static bool isVideoFormat(OutputFormat fmt);
    static bool needsAudioEncode(OutputFormat fmt);
};

}
