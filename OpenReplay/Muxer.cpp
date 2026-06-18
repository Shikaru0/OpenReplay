#include "Muxer.h"
#include <iostream>
#include <cstring>
#include <algorithm>

#ifdef _DEBUG
#define LOG_DBG std::cerr
#else
#define LOG_DBG if (false) std::cerr
#endif

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

struct AudioStreamState {
    AVStream* stream = nullptr;
    AVRational tb{};
    bool doEncode = false;
    AVCodecID codecId = AV_CODEC_ID_NONE;
    AVCodecContext* encCtx = nullptr;
    AudioStreamInfo info;
    bool encoded = false;
};

struct MuxStreams {
    AVFormatContext* fmtCtx = nullptr;
    AVStream* vStream = nullptr;
    AVDictionary* opts = nullptr;
    AVRational vTB{};
    bool hasVideo = false;
    std::vector<AudioStreamState> audioStates;
    bool hasAudio = false;
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
    for (auto& as : ms.audioStates) {
        if (as.encCtx)
            avcodec_free_context(&as.encCtx);
    }
}

static bool setupMuxContext(const char* outputPath, OutputFormat format,
                             int width, int height, int fps,
                             const std::vector<AudioStreamInfo>& audioInfos,
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

    bool doAudioEncode = Muxer::needsAudioEncode(format) && !audioInfos.empty();
    AVCodecID audioCodecId = AV_CODEC_ID_NONE;
    if (doAudioEncode) {
        audioCodecId = selectAudioCodec(format, audioCodecPref);
        LOG_DBG << "[Muxer] selected audio codec: " << audioCodecId << "\n";
        if (audioCodecId == AV_CODEC_ID_NONE) {
            doAudioEncode = false;
        }
    }

    for (int i = 0; i < (int)audioInfos.size(); i++) {
        const auto& info = audioInfos[i];
        AudioStreamState as;
        as.info = info;
        as.doEncode = doAudioEncode;

        AVCodecID codecId = doAudioEncode ? audioCodecId : pcmCodecId(info.bitsPerSample);
        as.codecId = codecId;

        as.stream = avformat_new_stream(ms.fmtCtx, nullptr);
        if (!as.stream) return false;
        as.stream->id = (ms.hasVideo ? 1 : 0) + i;
        as.stream->time_base = { 1, info.sampleRate };
        as.tb = as.stream->time_base;

        if (info.title) {
            av_dict_set(&as.stream->metadata, "title", info.title, 0);
        }

        AVCodecParameters* apar = as.stream->codecpar;
        apar->codec_type = AVMEDIA_TYPE_AUDIO;
        apar->codec_id = codecId;
        apar->sample_rate = info.sampleRate;
        av_channel_layout_default(&apar->ch_layout, info.channels);

        if (doAudioEncode) {
            apar->format = codecTargetFmt(codecId);
        } else {
            apar->format = pcmSampleFmt(info.bitsPerSample);
            apar->bits_per_coded_sample = info.bitsPerSample;
        }

        ms.audioStates.push_back(std::move(as));
        ms.hasAudio = true;
    }

    if (doAudioEncode && ms.hasAudio) {
        for (auto& as : ms.audioStates) {
            const AVCodec* codec = avcodec_find_encoder(as.codecId);
            if (!codec) continue;
            AVCodecContext* enc = avcodec_alloc_context3(codec);
            if (!enc) continue;
            AVChannelLayout chLayout;
            av_channel_layout_default(&chLayout, as.info.channels);
            enc->sample_rate = as.info.sampleRate;
            enc->ch_layout = chLayout;
            enc->sample_fmt = codecTargetFmt(as.codecId);
            enc->bit_rate = as.info.bitrate;
            enc->time_base = { 1, as.info.sampleRate };
            if (avcodec_open2(enc, codec, nullptr) >= 0) {
                as.encCtx = enc;
                if (enc->extradata && enc->extradata_size > 0 && as.stream) {
                    AVCodecParameters* apar = as.stream->codecpar;
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
    LOG_DBG << "[Muxer] encodeAndWriteAudio: packets=" << audioPackets.size()
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
        LOG_DBG << "[Muxer] audio encoder opened: frame_size=" << actx->frame_size << "\n";
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
                LOG_DBG << "[Muxer] encode: wrote " << pktCount << " audio frames\n";
        }
        if (hadError) break;
    }
    LOG_DBG << "[Muxer] encode: finished main loop, hadError=" << hadError << " pktCount=" << pktCount << " inputBuf=" << inputBuf.size() << "\n";

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
                          const std::vector<AudioStreamInfo>& audioStreams,
                          int audioCodecPref,
                          AVCodecID videoCodecId,
                          ProgressCallback progress)
{
    bool hasVideo = isVideoFormat(format);
    bool hasAudio = !audioStreams.empty();
    LOG_DBG << "[Muxer] muxStreaming: path=" << outputPath << " format=" << (int)format
            << " hasVideo=" << hasVideo << " hasAudio=" << hasAudio
            << " audioStreams=" << audioStreams.size()
            << " endPts=" << endPts << " durationUs=" << durationUs << "\n";

    if (!hasVideo && !hasAudio) return false;

    MuxStreams ms;
    ms.hasVideo = hasVideo;
    ms.hasAudio = hasAudio;

    size_t totalPkts = buffer->getPacketCount();
    LOG_DBG << "[Muxer] totalPkts=" << totalPkts << "\n";
    if (totalPkts == 0) { LOG_DBG << "[Muxer] no packets\n"; freeMuxStreams(ms); return false; }

    size_t totalVideo = 0, totalAudio = 0;
    int64_t ptsOffset = INT64_MAX;

    for (size_t i = 0; i < totalPkts; ++i) {
        int64_t pts;
        PacketType type;
        uint32_t dataSize;
        if (!buffer->getPacketInfo(i, pts, type, dataSize)) continue;

        if (type == PacketType::AudioData || type == PacketType::AudioLoopback || type == PacketType::AudioMic) {
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
                          audioStreams,
                          192000,
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
        bool isAudioType = (type == PacketType::AudioData || type == PacketType::AudioLoopback || type == PacketType::AudioMic);
        if ((isV && !ms.hasVideo) || (!isV && (!isAudioType || !ms.hasAudio))) continue;

        refs.push_back({i, pts, type, isV});
    }

    std::sort(refs.begin(), refs.end(),
        [](const PktRef& a, const PktRef& b) { return a.pts < b.pts; });

    int64_t rangeStart = endPts - durationUs;
    bool ok = true;
    bool hadError = false;

    for (auto& as : ms.audioStates) {
        if (!as.doEncode) continue;
        LOG_DBG << "[Muxer] preparing audio encode for type=" << (int)as.info.packetType
                << " title=" << (as.info.title ? as.info.title : "?") << "\n";
        std::vector<RetrievedPacket> audioPkts;
        for (auto& ref : refs) {
            if (ref.isVideo || ref.pts < rangeStart || ref.pts > endPts) continue;
            if (ref.type != as.info.packetType) continue;
            uint32_t ds = 0;
            PacketType pt;
            int64_t p;
            if (!buffer->getPacketInfo(ref.idx, p, pt, ds)) continue;
            std::vector<uint8_t> d(ds);
            if (buffer->readPacketData(ref.idx, d.data()))
                audioPkts.push_back({std::move(d), p, pt});
        }
        LOG_DBG << "[Muxer] collected " << audioPkts.size() << " audio packets for encoding\n";
        if (!audioPkts.empty()) {
            LOG_DBG << "[Muxer] calling encodeAndWriteAudio...\n";
            if (encodeAndWriteAudio(ms.fmtCtx, as.stream, audioPkts,
                as.info.sampleRate, as.info.channels, as.info.bitsPerSample,
                as.codecId, codecTargetFmt(as.codecId),
                as.info.bitrate, ptsOffset, as.encCtx))
            {
                written += audioPkts.size();
                as.encoded = true;
            }
            if (progress && totalToWrite > 0) {
                float pct = 0.05f + 0.90f * ((float)written / (float)totalToWrite);
                progress(pct);
            }
        }
    }

    for (auto& ref : refs) {
        if (ref.pts < rangeStart || ref.pts > endPts) continue;

        uint32_t dataSize = 0;
        PacketType ptype;
        int64_t pts;
        if (!buffer->getPacketInfo(ref.idx, pts, ptype, dataSize)) continue;

        int streamIdx;
        AVRational tb;
        if (ref.isVideo) {
            streamIdx = ms.vStream ? ms.vStream->index : 0;
            tb = ms.vTB;
        } else {
            auto it = std::find_if(ms.audioStates.begin(), ms.audioStates.end(),
                [ptype](const AudioStreamState& s) { return s.info.packetType == ptype; });
            if (it == ms.audioStates.end()) continue;
            if (it->encoded) continue;
            streamIdx = it->stream->index;
            tb = it->tb;
        }

        uint8_t* pktData = (uint8_t*)av_malloc(dataSize + AV_INPUT_BUFFER_PADDING_SIZE);
        if (!pktData) { hadError = true; continue; }

        if (!buffer->readPacketData(ref.idx, pktData)) {
            av_free(pktData);
            hadError = true;
            continue;
        }

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
