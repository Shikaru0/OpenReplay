#pragma once
#include <cstdint>
#include <functional>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/frame.h>
#include <libswscale/swscale.h>
}

namespace OpenReplay {

class VideoEncoder {
public:
    VideoEncoder() = default;
    ~VideoEncoder() { shutdown(); }

    VideoEncoder(const VideoEncoder&) = delete;
    VideoEncoder& operator=(const VideoEncoder&) = delete;

    using PacketCallback = std::function<void(const uint8_t* data, uint32_t size, int64_t pts, bool isKey)>;

    bool init(int width, int height, int fps, int bitrate,
              int keyframeIntervalSec = 2,
              const char* preset = "ultralowlatency",
              int vbvBufferMs = 1000,
              bool enablePreAnalysis = true);
    void shutdown();

    void setPacketCallback(PacketCallback cb) { m_callback = std::move(cb); }

    bool encode(const uint8_t* bgraData, int64_t pts);
    void flush();

    const std::vector<uint8_t>& getExtradata() const { return m_extradata; }
    AVCodecID codecId() const { return m_codecId; }
    const char* codecName() const { return m_codecName; }

private:
    bool sendFrame(AVFrame* frame, int64_t pts);

    AVCodecContext* m_codecCtx = nullptr;
    AVFrame* m_frame = nullptr;
    SwsContext* m_swsCtx = nullptr;
    PacketCallback m_callback;
    std::vector<uint8_t> m_extradata;
    int m_width = 0;
    int m_height = 0;
    AVCodecID m_codecId = AV_CODEC_ID_H264;
    const char* m_codecName = "";
};

}
