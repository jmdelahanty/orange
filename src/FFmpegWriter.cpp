// src/FFmpegWriter.cpp

#include "FFmpegWriter.h"
#include <unistd.h>

FFmpegWriter::FFmpegWriter(
    AVCodecID eCodecId,
    int nWidth,
    int nHeight,
    int nFps,
    const char *szOutFilePath,
    const char *metadata_file,
    const std::vector<std::pair<std::string, std::string>>& metadata_tags) : nFps(nFps)
{
    oc = avformat_alloc_context();
    if (!oc) {
        printf("FFMPEG: avformat_alloc_context error");
        return;
    }

    AVOutputFormat *fmt = (AVOutputFormat *)av_guess_format("mp4", NULL, NULL);
    if (!fmt) {
        printf("Invalid format");
        return;
    }
    fmt->video_codec = eCodecId;
    oc->oformat = fmt;

    for (const auto& tag : metadata_tags) {
        if (!tag.first.empty() && !tag.second.empty()) {
            av_dict_set(&oc->metadata, tag.first.c_str(), tag.second.c_str(), 0);
        }
    }

    vs = avformat_new_stream(oc, NULL);
    if (!vs) {
        printf("FFMPEG: Could not alloc video stream");
        return;
    }
    vs->id = 0;
    vs->time_base = AVRational{1, 90000};
    vs->r_frame_rate = AVRational{nFps, 1};

    AVCodecParameters *vpar = vs->codecpar;
    vpar->codec_id = fmt->video_codec;
    vpar->codec_type = AVMEDIA_TYPE_VIDEO;
    vpar->width = nWidth;
    vpar->height = nHeight;

    if (vpar->codec_id == AV_CODEC_ID_H264) {
        vpar->codec_tag = MKTAG('a', 'v', 'c', '1');
    } else if (vpar->codec_id == AV_CODEC_ID_HEVC) {
        vpar->codec_tag = MKTAG('h', 'v', 'c', '1');
    }

    if (avio_open(&oc->pb, szOutFilePath, AVIO_FLAG_WRITE) < 0) {
        printf("FFMPEG: Could not open %s", szOutFilePath);
        return;
    }

    if (avformat_write_header(oc, NULL)) {
        printf("FFMPEG: avformat_write_header error!");
        return;
    }
}

FFmpegWriter::~FFmpegWriter()
{
    if (oc) {
        // Send a NULL packet to muxer for flushing any internally buffered frames
        av_interleaved_write_frame(oc, NULL);
        av_write_trailer(oc);
        avio_close(oc->pb);
        avformat_free_context(oc);
    }
}

void FFmpegWriter::push_packet(uint8_t* pData, int nBytes, int nPts)
{
    AVPacket *pkt = av_packet_alloc();
    if (av_new_packet(pkt, nBytes) < 0) {
        std::cout << "Error, av_new_packet..." << std::endl;
        return;
    }
    memcpy(pkt->data, pData, nBytes);
    
    pkt->pts = av_rescale_q(sequential_frame_counter_++, AVRational{1, nFps}, vs->time_base);
    pkt->dts = pkt->pts;
    pkt->stream_index = vs->index;
    pkt->duration = av_rescale_q(1, AVRational{1, nFps}, vs->time_base);
    
    // A simple way to check for an H.264 IDR frame (a type of keyframe)
    if ((pData[4] & 0x1F) == 5) {
        pkt->flags |= AV_PKT_FLAG_KEY;
    }
    m_queue.push(pkt);
}

void FFmpegWriter::create_thread()
{
    m_thread = std::thread(&FFmpegWriter::write_thread, this);
}

void FFmpegWriter::quit_thread()
{
    m_queue.push(nullptr);
}

void FFmpegWriter::join_thread()
{
    if (m_thread.joinable()) {
        m_thread.join();
    }
}

void FFmpegWriter::write_one_pkt(AVPacket* pkt)
{
    int ret = av_interleaved_write_frame(oc, pkt);
    if (ret < 0) {
        std::cout << "FFMPEG: Error while writing video frame" << std::endl;
    }
}

void FFmpegWriter::write_thread()
{
    while (true) {
        AVPacket* pkt = nullptr;
        if (m_queue.pop(pkt)) {
            if (pkt) {
                write_one_pkt(pkt);
                av_packet_free(&pkt);
            } else {
                break;
            }
        }
        else {
            usleep(100);
        }
    }
}
