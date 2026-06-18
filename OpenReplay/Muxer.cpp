#include "Muxer.h"
#include <iostream>
#include <cstring>
#include <algorithm>

extern "C" {
#include <libavutil/avutil.h>
#include <libavutil/mathematics.h>
#include <libavutil/channel_layout.h>
#include <libavcodec/avcodec.h>
}

namespace OpenReplay {

const char* Muxer::extension(OutputFormat fmt) {
    switch (fmt) {
        case OutputFormat::MP4:  return "mp4";
        case OutputFormat::MKV:  return "mkv";
        case OutputFormat::WEBM: return "webm";
        case OutputFormat::MOV:  return "mov";
        case OutputFormat::AVI:  return "avi";
        case OutputFormat::WAV:  return "wav";
        case OutputFormat::FLAC: return "flac";
        case OutputFormat::MP3:  return "mp3";
        case OutputFormat::OGG:  return "ogg";
    }
    return "mp4";
}

bool Muxer::isVideoFormat(OutputFormat fmt) {
    return fmt == OutputFormat::MP4  || fmt == OutputFormat::MKV ||
           fmt == OutputFormat::WEBM || fmt == OutputFormat::MOV ||
           fmt == OutputFormat::AVI;
}

bool Muxer::needsAudioEncode(OutputFormat fmt) {
    return fmt == OutputFormat::MP4  || fmt == OutputFormat::MKV ||
           fmt == OutputFormat::WEBM || fmt == OutputFormat::MOV ||
           fmt == OutputFormat::FLAC ||
           fmt == OutputFormat::MP3  || fmt == OutputFormat::OGG;
}

static AVCodecID pcmCodecId(int bitsPerSample) {
    switch (bitsPerSample) {
        case 16: return AV_CODEC_ID_PCM_S16LE;
        case 32: return AV_CODEC_ID_PCM_F32LE;
        default: return AV_CODEC_ID_PCM_F32LE;
    }
}

static AVSampleFormat pcmSampleFmt(int bitsPerSample) {
    return bitsPerSample == 32 ? AV_SAMPLE_FMT_FLT : AV_SAMPLE_FMT_S16;
}

static const char* containerName(OutputFormat fmt) {
    switch (fmt) {
        case OutputFormat::MP4:  return "mp4";
        case OutputFormat::MKV:  return "matroska";
        case OutputFormat::WEBM: return "webm";
        case OutputFormat::MOV:  return "mov";
        case OutputFormat::AVI:  return "avi";
        case OutputFormat::WAV:  return "wav";
        case OutputFormat::FLAC: return "flac";
        case OutputFormat::MP3:  return "mp3";
        case OutputFormat::OGG:  return "ogg";
    }
    return "mp4";
}

static AVCodecID selectAudioCodec(OutputFormat format, int pref) {
    switch (format) {
        case OutputFormat::MP4:
        case OutputFormat::MOV:
        case OutputFormat::MKV:
        case OutputFormat::AVI: {
            if (pref == 1) {
                if (avcodec_find_encoder(AV_CODEC_ID_AAC))
                    return AV_CODEC_ID_AAC;
                if (avcodec_find_encoder(AV_CODEC_ID_MP3))
                    return AV_CODEC_ID_MP3;
            } else {
                if (avcodec_find_encoder(AV_CODEC_ID_MP3))
                    return AV_CODEC_ID_MP3;
                if (avcodec_find_encoder(AV_CODEC_ID_AAC))
                    return AV_CODEC_ID_AAC;
            }
            if (avcodec_find_encoder(AV_CODEC_ID_AC3))
                return AV_CODEC_ID_AC3;
            std::cerr << "[Muxer] No compatible audio encoder available\n";
            return AV_CODEC_ID_NONE;
        }
        case OutputFormat::WEBM: {
            if (avcodec_find_encoder(AV_CODEC_ID_OPUS))
                return AV_CODEC_ID_OPUS;
            if (avcodec_find_encoder(AV_CODEC_ID_VORBIS))
                return AV_CODEC_ID_VORBIS;
            return AV_CODEC_ID_NONE;
        }
        case OutputFormat::FLAC:
            return AV_CODEC_ID_FLAC;
        case OutputFormat::MP3:
            return AV_CODEC_ID_MP3;
        case OutputFormat::OGG:
            return AV_CODEC_ID_VORBIS;
        default:
            return AV_CODEC_ID_NONE;
    }
}

static AVSampleFormat codecTargetFmt(AVCodecID codecId) {
    switch (codecId) {
        case AV_CODEC_ID_AAC:   return AV_SAMPLE_FMT_FLTP;
        case AV_CODEC_ID_ALAC:  return AV_SAMPLE_FMT_S16P;
        case AV_CODEC_ID_FLAC:  return AV_SAMPLE_FMT_S16;
        case AV_CODEC_ID_MP3:   return AV_SAMPLE_FMT_FLTP;
        case AV_CODEC_ID_VORBIS:return AV_SAMPLE_FMT_FLTP;
        case AV_CODEC_ID_OPUS:  return AV_SAMPLE_FMT_FLTP;
        case AV_CODEC_ID_AC3:   return AV_SAMPLE_FMT_FLTP;
        default:                return AV_SAMPLE_FMT_FLTP;
    }
}

struct MuxStreams {
    AVFormatContext* fmtCtx = nullptr;
    AVStream* vStream = nullptr;
    AVStream* aStream = nullptr;
    AVDictionary* opts = nullptr;
    AVRational vTB{};
    AVRational aTB{};
    bool hasVideo = false;
    bool hasAudio = false;
    bool doAudioEncode = false;
    AVCodecID audioCodecId = AV_CODEC_ID_NONE;
    AVCodecContext* audioEncCtx = nullptr;
    int64_t ptsOffset = 0;
};

static bool openOutput(AVFormatContext* fmtCtx, const char* path) {
    if (!(fmtCtx->oformat->flags & AVFMT_NOFILE)) {
        if (avio_open(&fmtCtx->pb, path, AVIO_FLAG_WRITE) < 0) {
            std::cerr << "[Muxer] Failed to open output file\n";
            return false;
        }
    }
    return true;
}

static bool writeHeader(AVFormatContext* fmtCtx, AVDictionary** opts, OutputFormat format) {
    if (format == OutputFormat::MP4)
        av_dict_set(opts, "movflags", "+faststart", 0);
    if (avformat_write_header(fmtCtx, opts) < 0) {
        std::cerr << "[Muxer] Failed to write header\n";
        return false;
    }
    return true;
}

static void writePacketDirect(AVFormatContext* fmtCtx, uint8_t* data, int size,
                               int streamIndex, int64_t pts, int flags) {
    AVPacket* pkt = av_packet_alloc();
    if (!pkt) { av_free(data); return; }
    av_packet_from_data(pkt, data, size);
    pkt->pts = pts;
    pkt->dts = pts;
    pkt->stream_index = streamIndex;
    pkt->flags = flags;
    pkt->duration = 0;

    av_interleaved_write_frame(fmtCtx, pkt);
    av_packet_free(&pkt);
}

static void freeMuxStreams(MuxStreams& ms) {
    av_dict_free(&ms.opts);
    if (ms.fmtCtx) {
        if (ms.fmtCtx->pb && !(ms.fmtCtx->oformat->flags & AVFMT_NOFILE))
            avio_closep(&ms.fmtCtx->pb);
        avformat_free_context(ms.fmtCtx);
    }
    if (ms.audioEncCtx)
        avcodec_free_context(&ms.audioEncCtx);
}

static bool setupMuxContext(const char* outputPath, OutputFormat format,
                             int width, int height, int fps,
                             int audioSampleRate, int audioChannels, int audioBitsPerSample,
                             int audioBitrate,
                             AVCodecID videoCodecId, int audioCodecPref,
                             const std::vector<uint8_t>* extradata,
                             MuxStreams& ms) {
    const char* fmtName = containerName(format);
    if (strncmp(outputPath, "rtmp://", 7) == 0 || strncmp(outputPath, "rtmps://", 8) == 0)
        fmtName = nullptr;
    avformat_alloc_output_context2(&ms.fmtCtx, nullptr,
        fmtName, outputPath);
    if (!ms.fmtCtx) {
        std::cerr << "[Muxer] Failed to create output context\n";
        return false;
    }

    if (ms.hasVideo) {
        ms.vStream = avformat_new_stream(ms.fmtCtx, nullptr);
        if (!ms.vStream) return false;
        ms.vStream->id = 0;
        ms.vStream->time_base = { 1, fps * 1000 };
        ms.vTB = ms.vStream->time_base;

        AVCodecParameters* vpar = ms.vStream->codecpar;
        vpar->codec_type = AVMEDIA_TYPE_VIDEO;
        vpar->codec_id = videoCodecId;
        vpar->width = width;
        vpar->height = height;
        vpar->format = AV_PIX_FMT_NV12;

        if (extradata && !extradata->empty()) {
            vpar->extradata_size = (int)extradata->size();
            vpar->extradata = (uint8_t*)av_mallocz(
                (size_t)vpar->extradata_size + AV_INPUT_BUFFER_PADDING_SIZE);
            if (vpar->extradata)
                std::memcpy(vpar->extradata, extradata->data(), extradata->size());
        }
    }

    if (ms.hasAudio) {
        if (ms.doAudioEncode) {
            ms.audioCodecId = selectAudioCodec(format, audioCodecPref);
            std::cerr << "[Muxer] selected audio codec: " << ms.audioCodecId << "\n";
            if (ms.audioCodecId == AV_CODEC_ID_NONE) {
                ms.hasAudio = false;
                ms.doAudioEncode = false;
            }
        }
        if (ms.hasAudio) {
            ms.aStream = avformat_new_stream(ms.fmtCtx, nullptr);
            if (!ms.aStream) return false;
            ms.aStream->id = ms.hasVideo ? 1 : 0;
            ms.aStream->time_base = { 1, audioSampleRate };
            ms.aTB = ms.aStream->time_base;

            AVCodecParameters* apar = ms.aStream->codecpar;
            apar->codec_type = AVMEDIA_TYPE_AUDIO;
            apar->sample_rate = audioSampleRate;
            av_channel_layout_default(&apar->ch_layout, audioChannels);

            if (ms.doAudioEncode) {
                apar->codec_id = ms.audioCodecId;
                apar->format = codecTargetFmt(ms.audioCodecId);
            } else {
                apar->codec_id = pcmCodecId(audioBitsPerSample);
                apar->format = pcmSampleFmt(audioBitsPerSample);
                apar->bits_per_coded_sample = audioBitsPerSample;
            }
        }
    }

    if (ms.doAudioEncode && ms.hasAudio && !ms.audioEncCtx) {
        const AVCodec* codec = avcodec_find_encoder(ms.audioCodecId);
        if (codec) {
            AVCodecContext* enc = avcodec_alloc_context3(codec);
            if (enc) {
                AVChannelLayout chLayout;
                av_channel_layout_default(&chLayout, audioChannels);
                enc->sample_rate = audioSampleRate;
                enc->ch_layout = chLayout;
                enc->sample_fmt = codecTargetFmt(ms.audioCodecId);
                enc->bit_rate = audioBitrate;
                enc->time_base = { 1, audioSampleRate };
                if (avcodec_open2(enc, codec, nullptr) >= 0) {
                    ms.audioEncCtx = enc;
                    if (enc->extradata && enc->extradata_size > 0 && ms.aStream) {
                        AVCodecParameters* apar = ms.aStream->codecpar;
                        apar->extradata = (uint8_t*)av_mallocz(
                            (size_t)enc->extradata_size + AV_INPUT_BUFFER_PADDING_SIZE);
                        if (apar->extradata) {
                            std::memcpy(apar->extradata, enc->extradata, enc->extradata_size);
                            apar->extradata_size = enc->extradata_size;
                        }
                    }
                } else {
                    avcodec_free_context(&enc);
                }
            }
        }
    }

    if (!openOutput(ms.fmtCtx, outputPath)) return false;
    if (!writeHeader(ms.fmtCtx, &ms.opts, format)) return false;

    return true;
}

static bool encodeAndWriteAudio(
    AVFormatContext* fmtCtx, AVStream* aStream,
    const std::vector<RetrievedPacket>& audioPackets,
    int sampleRate, int channels, int bitsPerSample,
    AVCodecID targetCodecId, AVSampleFormat targetSampleFmt,
    int64_t bitRate, int64_t ptsOffset,
    AVCodecContext* existingEnc = nullptr)
{
    std::cerr << "[Muxer] encodeAndWriteAudio: packets=" << audioPackets.size()
              << " sr=" << sampleRate << " ch=" << channels << " bps=" << bitsPerSample
              << " codec=" << targetCodecId << " fmt=" << targetSampleFmt << "\n";
    AVCodecContext* actx = existingEnc;
    bool ownCtx = false;
    AVChannelLayout chLayout;
    av_channel_layout_default(&chLayout, channels);

    if (!actx) {
        const AVCodec* codec = avcodec_find_encoder(targetCodecId);
        if (!codec) {
            std::cerr << "[Muxer] Audio encoder not found for codec "
                      << targetCodecId << "\n";
            return false;
        }
        actx = avcodec_alloc_context3(codec);
        if (!actx) return false;
        ownCtx = true;

        actx->sample_rate = sampleRate;
        actx->ch_layout = chLayout;
        actx->sample_fmt = targetSampleFmt;
        actx->bit_rate = (int)bitRate;
        actx->time_base = { 1, sampleRate };

        if (avcodec_open2(actx, codec, nullptr) < 0) {
            std::cerr << "[Muxer] Failed to open audio encoder\n";
            avcodec_free_context(&actx);
            return false;
        }
        std::cerr << "[Muxer] audio encoder opened: frame_size=" << actx->frame_size << "\n";
    }

    AVSampleFormat srcFmt = pcmSampleFmt(bitsPerSample);
    AVChannelLayout srcLayout;
    av_channel_layout_default(&srcLayout, channels);
    int srcBps = av_get_bytes_per_sample(srcFmt);

    SwrContext* swr = nullptr;
    int swrRet = swr_alloc_set_opts2(&swr,
        &chLayout, targetSampleFmt, sampleRate,
        &srcLayout, srcFmt, sampleRate,
        0, nullptr);
    if (!swr || swrRet < 0 || swr_init(swr) < 0) {
        std::cerr << "[Muxer] Failed to init audio resampler\n";
        if (swr) swr_free(&swr);
        if (ownCtx) avcodec_free_context(&actx);
        return false;
    }

    AVFrame* frame = av_frame_alloc();
    if (!frame) { swr_free(&swr); if (ownCtx) avcodec_free_context(&actx); return false; }
    frame->nb_samples = actx->frame_size ? actx->frame_size : 1024;
    frame->format = targetSampleFmt;
    frame->ch_layout = chLayout;
    frame->sample_rate = sampleRate;
    if (av_frame_get_buffer(frame, 0) < 0) {
        av_frame_free(&frame); swr_free(&swr); if (ownCtx) avcodec_free_context(&actx);
        return false;
    }

    std::vector<uint8_t> inputBuf;
    int srcFrameBytes = frame->nb_samples * channels * srcBps;
    int64_t inputPts = 0;
    bool hasInputPts = false;
    bool hadError = false;

    int pktCount = 0;
    for (const auto& pkt : audioPackets) {
        if (!hasInputPts && !pkt.data.empty()) {
            inputPts = pkt.pts - ptsOffset;
            hasInputPts = true;
        }

        inputBuf.insert(inputBuf.end(), pkt.data.begin(), pkt.data.end());

        while ((int)inputBuf.size() >= srcFrameBytes) {
            const uint8_t* inData[1] = { inputBuf.data() };

            int ret = swr_convert(swr,
                frame->data, frame->nb_samples,
                inData, frame->nb_samples);
            if (ret < 0) { hadError = true; break; }

            if (ret < frame->nb_samples) frame->nb_samples = ret;

            frame->pts = av_rescale_q(
                inputPts,
                AVRational{1, 1'000'000},
                AVRational{1, sampleRate});

            inputPts += (int64_t)frame->nb_samples * 1'000'000 / sampleRate;

            int sendRet = avcodec_send_frame(actx, frame);
            if (sendRet < 0 && sendRet != AVERROR(EAGAIN)) { hadError = true; break; }

            while (true) {
                AVPacket* epkt = av_packet_alloc();
                if (!epkt) { hadError = true; break; }
                int recvRet = avcodec_receive_packet(actx, epkt);
                if (recvRet == 0) {
                    epkt->stream_index = aStream->index;
                    av_interleaved_write_frame(fmtCtx, epkt);
                }
                av_packet_free(&epkt);
                if (recvRet < 0) break;
            }
            if (hadError) break;

            inputBuf.erase(inputBuf.begin(), inputBuf.begin() + srcFrameBytes);
            pktCount++;
            if (pktCount % 50 == 0)
                std::cerr << "[Muxer] encode: wrote " << pktCount << " audio frames\n";
        }
        if (hadError) break;
    }
    std::cerr << "[Muxer] encode: finished main loop, hadError=" << hadError << " pktCount=" << pktCount << " inputBuf=" << inputBuf.size() << "\n";

    if (!inputBuf.empty() && !hadError) {
        inputBuf.resize(srcFrameBytes, 0);
        const uint8_t* inData[1] = { inputBuf.data() };
        int inSamples = (int)(srcFrameBytes / (channels * srcBps));
        int ret = swr_convert(swr, frame->data, frame->nb_samples, inData, inSamples);
        if (ret > 0) {
            frame->nb_samples = ret;
            frame->pts = av_rescale_q(inputPts, AVRational{1, 1'000'000}, AVRational{1, sampleRate});
            if (avcodec_send_frame(actx, frame) >= 0) {
                while (true) {
                    AVPacket* epkt = av_packet_alloc();
                    if (!epkt) break;
                    int flushRet = avcodec_receive_packet(actx, epkt);
                    if (flushRet == 0) {
                        epkt->stream_index = aStream->index;
                        av_interleaved_write_frame(fmtCtx, epkt);
                    }
                    av_packet_free(&epkt);
                    if (flushRet < 0) break;
                }
            }
        }
    }

    if (!hadError) {
        avcodec_send_frame(actx, nullptr);
        while (true) {
            AVPacket* epkt = av_packet_alloc();
            if (!epkt) break;
            int flushRet = avcodec_receive_packet(actx, epkt);
            if (flushRet == 0) {
                epkt->stream_index = aStream->index;
                av_interleaved_write_frame(fmtCtx, epkt);
            }
            av_packet_free(&epkt);
            if (flushRet < 0) break;
        }
    }

    av_frame_free(&frame);
    swr_free(&swr);
    if (ownCtx) avcodec_free_context(&actx);
    return !hadError;
}

bool Muxer::muxStreaming(const char* outputPath, OutputFormat format,
                          const DiskBackedBuffer* buffer,
                          int64_t endPts, int64_t durationUs,
                          int width, int height, int fps,
                          int audioSampleRate, int audioChannels,
                          int audioBitsPerSample, int audioBitrate,
                          AVCodecID videoCodecId,
                          int audioCodecPref,
                          ProgressCallback progress)
{
    bool hasVideo = isVideoFormat(format);
    bool hasAudio = audioSampleRate > 0;
    std::cerr << "[Muxer] muxStreaming: path=" << outputPath << " format=" << (int)format
              << " hasVideo=" << hasVideo << " hasAudio=" << hasAudio
              << " endPts=" << endPts << " durationUs=" << durationUs << "\n";

    if (!hasVideo && !hasAudio) return false;

    MuxStreams ms;
    ms.hasVideo = hasVideo;
    ms.hasAudio = hasAudio;
    ms.doAudioEncode = needsAudioEncode(format) && hasAudio;

    size_t totalPkts = buffer->getPacketCount();
    std::cerr << "[Muxer] totalPkts=" << totalPkts << "\n";
    if (totalPkts == 0) { std::cerr << "[Muxer] no packets\n"; freeMuxStreams(ms); return false; }

    size_t totalVideo = 0, totalAudio = 0;
    int64_t ptsOffset = INT64_MAX;

    for (size_t i = 0; i < totalPkts; ++i) {
        int64_t pts;
        PacketType type;
        uint32_t dataSize;
        if (!buffer->getPacketInfo(i, pts, type, dataSize)) continue;

        if (type == PacketType::AudioData) {
            totalAudio++;
        } else if (type == PacketType::VideoKeyFrame || type == PacketType::VideoDeltaFrame) {
            totalVideo++;
        }
        if (pts < ptsOffset) ptsOffset = pts;
    }
    if (ptsOffset == INT64_MAX) ptsOffset = 0;

    if (ms.hasVideo && totalVideo == 0) ms.hasVideo = false;
    if (ms.hasAudio && totalAudio == 0) ms.hasAudio = false;
    if (!ms.hasVideo && !ms.hasAudio) { freeMuxStreams(ms); return false; }

    const auto& extradata = buffer->getExtradata();
    if (!setupMuxContext(outputPath, format, width, height, fps,
                          audioSampleRate, audioChannels, audioBitsPerSample,
                          audioBitrate,
                          videoCodecId, audioCodecPref, &extradata, ms))
    {
        freeMuxStreams(ms);
        return false;
    }

    if (progress) progress(0.05f);

    size_t totalToWrite = totalVideo + totalAudio;
    size_t written = 0;

    struct PktRef {
        size_t idx;
        int64_t pts;
        PacketType type;
        bool isVideo;
    };
    std::vector<PktRef> refs;
    refs.reserve(totalToWrite);

    for (size_t i = 0; i < totalPkts; ++i) {
        int64_t pts;
        PacketType type;
        uint32_t dataSize;
        if (!buffer->getPacketInfo(i, pts, type, dataSize)) continue;
        if (type == PacketType::CodecExtradata) continue;

        bool isV = (type == PacketType::VideoKeyFrame || type == PacketType::VideoDeltaFrame);
        if ((isV && !ms.hasVideo) || (!isV && !ms.hasAudio)) continue;

        refs.push_back({i, pts, type, isV});
    }

    std::sort(refs.begin(), refs.end(),
        [](const PktRef& a, const PktRef& b) { return a.pts < b.pts; });

    int64_t rangeStart = endPts - durationUs;
    bool ok = true;
    bool hadError = false;

    if (ms.doAudioEncode && ms.hasAudio) {
        std::cerr << "[Muxer] preparing audio encode, totalAudio=" << totalAudio << "\n";
        std::vector<RetrievedPacket> audioPkts;
        for (auto& ref : refs) {
            if (ref.isVideo || ref.pts < rangeStart || ref.pts > endPts) continue;
            uint32_t ds = 0;
            PacketType pt;
            int64_t p;
            if (!buffer->getPacketInfo(ref.idx, p, pt, ds)) continue;
            std::vector<uint8_t> d(ds);
            if (buffer->readPacketData(ref.idx, d.data()))
                audioPkts.push_back({std::move(d), p, PacketType::AudioData});
        }
        std::cerr << "[Muxer] collected " << audioPkts.size() << " audio packets for encoding\n";
        if (!audioPkts.empty()) {
            std::cerr << "[Muxer] calling encodeAndWriteAudio...\n";
            if (encodeAndWriteAudio(ms.fmtCtx, ms.aStream, audioPkts,
                audioSampleRate, audioChannels, audioBitsPerSample,
                ms.audioCodecId, codecTargetFmt(ms.audioCodecId),
                audioBitrate, ptsOffset, ms.audioEncCtx))
            {
                written += audioPkts.size();
            }
            if (progress && totalToWrite > 0) {
                float pct = 0.05f + 0.90f * ((float)written / (float)totalToWrite);
                progress(pct);
            }
        }
    }

    for (auto& ref : refs) {
        if (ref.pts < rangeStart || ref.pts > endPts) continue;
        if (ms.doAudioEncode && !ref.isVideo) continue;

        AVRational tb = ref.isVideo ? ms.vTB : ms.aTB;

        uint32_t dataSize = 0;
        PacketType ptype;
        int64_t pts;
        if (!buffer->getPacketInfo(ref.idx, pts, ptype, dataSize)) continue;

        uint8_t* pktData = (uint8_t*)av_malloc(dataSize + AV_INPUT_BUFFER_PADDING_SIZE);
        if (!pktData) { hadError = true; continue; }

        if (!buffer->readPacketData(ref.idx, pktData)) {
            av_free(pktData);
            hadError = true;
            continue;
        }

        int streamIdx = ref.isVideo
            ? (ms.vStream ? ms.vStream->index : 0)
            : (ms.aStream ? ms.aStream->index : 1);
        int64_t scaledPts = av_rescale_q(pts - ptsOffset, AVRational{1, 1'000'000}, tb);
        int flags = (ref.isVideo && ptype == PacketType::VideoKeyFrame) ? AV_PKT_FLAG_KEY : 0;

        writePacketDirect(ms.fmtCtx, pktData, (int)dataSize, streamIdx, scaledPts, flags);

        written++;
        if (progress && totalToWrite > 0) {
            float pct = 0.05f + 0.90f * ((float)written / (float)totalToWrite);
            progress(pct);
        }
    }

    av_write_trailer(ms.fmtCtx);

    if (progress) progress(1.0f);
    ok = !hadError && written > 0;

    freeMuxStreams(ms);
    return ok;
}

}
