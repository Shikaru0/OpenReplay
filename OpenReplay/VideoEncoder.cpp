#include "VideoEncoder.h"
#include <iostream>
#include <cstring>

extern "C" {
#include <libavutil/opt.h>
#include <libavutil/pixdesc.h>
#include <libavutil/error.h>
}

namespace OpenReplay {

bool VideoEncoder::init(int width, int height, int fps, int bitrate,
                          int keyframeIntervalSec, const char* preset,
                          int vbvBufferMs, bool enablePreAnalysis) {
    m_width = width;
    m_height = height;

    struct EncoderCandidate {
        const char* name;
        bool isAmd;
    };
    const EncoderCandidate candidates[] = {
        { "h264_amf",    true  },
        { "h264_nvenc",  false },
        { "libx264",     false },
    };

    const AVCodec* codec = nullptr;
    int gopSize = fps * std::max(1, keyframeIntervalSec);

    for (auto& cand : candidates) {
        codec = avcodec_find_encoder_by_name(cand.name);
        if (!codec) continue;

        AVCodecContext* ctx = avcodec_alloc_context3(codec);
        if (!ctx) continue;

        ctx->width = width;
        ctx->height = height;
        ctx->time_base = { 1, 1'000'000 };
        ctx->framerate = { fps, 1 };
        ctx->pix_fmt = AV_PIX_FMT_NV12;
        ctx->bit_rate = bitrate;
        ctx->gop_size = gopSize;
        ctx->max_b_frames = 0;

        if (cand.isAmd) {
            av_opt_set(ctx->priv_data, "usage", preset, 0);
            av_opt_set(ctx->priv_data, "quality", "speed", 0);
            av_opt_set(ctx->priv_data, "rc", "cbr", 0);
            av_opt_set(ctx->priv_data, "enforce_hrd", "1", 0);
            av_opt_set(ctx->priv_data, "filler_data", "1", 0);
            av_opt_set(ctx->priv_data, "preanalysis", enablePreAnalysis ? "1" : "0", 0);
            char vbvBuf[32];
            snprintf(vbvBuf, sizeof(vbvBuf), "%d", (int)((int64_t)bitrate * vbvBufferMs / 1000));
            av_opt_set(ctx->priv_data, "vbv_buf_size", vbvBuf, 0);
            char maxAu[32];
            snprintf(maxAu, sizeof(maxAu), "%d", bitrate / fps);
            av_opt_set(ctx->priv_data, "max_au_size", maxAu, 0);
            if (strstr(cand.name, "h264")) {
                av_opt_set(ctx->priv_data, "profile", "high", 0);
            } else {
                av_opt_set(ctx->priv_data, "profile", "main", 0);
            }
            av_opt_set(ctx->priv_data, "level", "auto", 0);
            ctx->thread_count = 1;
        } else if (strstr(cand.name, "nvenc")) {
            av_opt_set(ctx->priv_data, "preset", "p6", 0);
            av_opt_set(ctx->priv_data, "rc", "vbr_hq", 0);
            av_opt_set(ctx->priv_data, "tune", "hq", 0);
            ctx->thread_count = 4;
        } else {
            av_opt_set(ctx->priv_data, "preset", "ultrafast", 0);
            av_opt_set(ctx->priv_data, "tune", "zerolatency", 0);
            ctx->thread_count = 4;
        }

        if (avcodec_open2(ctx, codec, nullptr) == 0) {
            m_codecCtx = ctx;
            m_codecName = cand.name;
            break;
        }

        avcodec_free_context(&ctx);
        codec = nullptr;
    }

    if (!codec) {
        codec = avcodec_find_encoder(AV_CODEC_ID_H264);
        if (codec) {
            m_codecCtx = avcodec_alloc_context3(codec);
            if (m_codecCtx) {
                m_codecCtx->width = width;
                m_codecCtx->height = height;
                m_codecCtx->time_base = { 1, 1'000'000 };
                m_codecCtx->framerate = { fps, 1 };
                m_codecCtx->pix_fmt = AV_PIX_FMT_NV12;
                m_codecCtx->bit_rate = bitrate;
                m_codecCtx->gop_size = gopSize;
                m_codecCtx->max_b_frames = 0;
                m_codecCtx->thread_count = 4;
                av_opt_set(m_codecCtx->priv_data, "preset", "medium", 0);
                if (avcodec_open2(m_codecCtx, codec, nullptr) < 0) {
                    avcodec_free_context(&m_codecCtx);
                    codec = nullptr;
                } else {
                    m_codecName = codec->name;
                }
            }
        }
    }

    if (!codec) {
        std::cerr << "[VideoEncoder] No H.264/H.265 encoder found\n";
        return false;
    }

    m_codecId = codec->id;

    m_frame = av_frame_alloc();
    if (!m_frame) { avcodec_free_context(&m_codecCtx); return false; }
    m_frame->format = AV_PIX_FMT_NV12;
    m_frame->width = width;
    m_frame->height = height;
    if (av_frame_get_buffer(m_frame, 32) < 0) {
        av_frame_free(&m_frame);
        avcodec_free_context(&m_codecCtx);
        return false;
    }

    m_swsCtx = sws_getContext(width, height, AV_PIX_FMT_BGRA,
                              width, height, AV_PIX_FMT_NV12,
                              SWS_FAST_BILINEAR, nullptr, nullptr, nullptr);
    if (!m_swsCtx) {
        av_frame_free(&m_frame);
        avcodec_free_context(&m_codecCtx);
        return false;
    }

    return true;
}

void VideoEncoder::shutdown() {
    if (m_swsCtx) { sws_freeContext(m_swsCtx); m_swsCtx = nullptr; }
    if (m_frame) { av_frame_free(&m_frame); }
    if (m_codecCtx) { avcodec_free_context(&m_codecCtx); }
}

bool VideoEncoder::encode(const uint8_t* bgraData, int64_t pts) {
    if (!m_codecCtx || !bgraData) return false;

    const uint8_t* srcSlice[1] = { bgraData };
    int srcStride[1] = { m_width * 4 };
    sws_scale(m_swsCtx, srcSlice, srcStride, 0, m_height,
              m_frame->data, m_frame->linesize);

    return sendFrame(m_frame, pts);
}

bool VideoEncoder::sendFrame(AVFrame* frame, int64_t pts) {
    if (frame) frame->pts = pts;

    int ret = avcodec_send_frame(m_codecCtx, frame);
    if (ret < 0) {
        if (ret != AVERROR_EOF) { 
            char errBuf[256];
            av_strerror(ret, errBuf, sizeof(errBuf));
        }
        return false;
    }

    AVPacket* pkt = av_packet_alloc();
    if (!pkt) return false;

    while (avcodec_receive_packet(m_codecCtx, pkt) == 0) {
        if ((pkt->flags & AV_PKT_FLAG_KEY) && m_extradata.empty() &&
            m_codecCtx->extradata && m_codecCtx->extradata_size > 0) {
            m_extradata.assign(m_codecCtx->extradata,
                m_codecCtx->extradata + m_codecCtx->extradata_size);
        }
        if (m_callback)
            m_callback(pkt->data, pkt->size, pkt->pts, pkt->flags & AV_PKT_FLAG_KEY);
        av_packet_unref(pkt);
    }
    av_packet_free(&pkt);
    return true;
}

void VideoEncoder::flush() {
    sendFrame(nullptr, AV_NOPTS_VALUE);
}

}
