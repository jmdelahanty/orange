// src/FFmpegWriter.cpp

#include "FFmpegWriter.h"
#include <unistd.h>

FFmpegWriter::FFmpegWriter(
    AVCodecID eCodecId,
    int nWidth,
    int nHeight,
    int nFps,
    const char *szOutFilePath,
    const char *metadata_file) : nFps(nFps)
{
    oc = avformat_alloc_context();
    if (!oc) {
        printf("FFMPEG: avformat_alloc_context error");
        return;
    }

    // Set format on oc
    AVOutputFormat *fmt = (AVOutputFormat *)av_guess_format("mp4", NULL, NULL);
    if (!fmt) {
        printf("Invalid format");
        return;
    }
    fmt->video_codec = eCodecId;
    oc->oformat = fmt;

    // Add video stream to oc
    vs = avformat_new_stream(oc, NULL);
    if (!vs) {
        printf("FFMPEG: Could not alloc video stream");
        return;
    }
    vs->id = 0;

    // Use standard MPEG time base for better compatibility
    vs->time_base = AVRational{1, 90000};
    vs->r_frame_rate = AVRational{nFps, 1};

    // Set video parameters
    AVCodecParameters *vpar = vs->codecpar;
    vpar->codec_id = fmt->video_codec;
    vpar->codec_type = AVMEDIA_TYPE_VIDEO;
    vpar->width = nWidth;
    vpar->height = nHeight;

    // Set codec tags for Apple compatibility
    if (vpar->codec_id == AV_CODEC_ID_H264) {
        vpar->codec_tag = MKTAG('a', 'v', 'c', '1');
    } else if (vpar->codec_id == AV_CODEC_ID_HEVC) {
        vpar->codec_tag = MKTAG('h', 'v', 'c', '1');
    }

    // Everything is ready. Now open the output stream.
    if (avio_open(&oc->pb, szOutFilePath, AVIO_FLAG_WRITE) < 0) {
        printf("FFMPEG: Could not open %s", szOutFilePath);
        return;
    }

    // Write the container header
    if (avformat_write_header(oc, NULL)) {
        printf("FFMPEG: avformat_write_header error!");
        return;
    }
}

FFmpegWriter::~FFmpegWriter()
{
    // The quit_thread() and join_thread() calls should be done by the owner
    // (GPUVideoEncoder::close_writer) *before* this destructor is called.
    // This ensures all packets are processed before we write the trailer.
    if (oc) {
        // Send a NULL packet to muxer for flushing any internally buffered frames
        // and ensure that last frame is written
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
    
    // Use sequential counter instead of passed-in PTS to ensure proper playback
    pkt->pts = av_rescale_q(sequential_frame_counter_++, AVRational{1, nFps}, vs->time_base);
    pkt->dts = pkt->pts;
    pkt->stream_index = vs->index;
    
    // Add duration for each frame
    pkt->duration = av_rescale_q(1, AVRational{1, nFps}, vs->time_base);
    
    // Check for keyframe
    if(!memcmp(pData, "\x00\x00\x00\x01\x67", 5)) {
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
    m_queue.push(nullptr); // Push a nullptr to signal the thread to quit
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
    // We don't unref the packet here because the caller (write_thread) will free it
}

// off-thread saving
void FFmpegWriter::write_thread()
{
    while (true) {
        AVPacket* pkt = nullptr;
        if (m_queue.pop(pkt)) {
            if (pkt) {
                write_one_pkt(pkt);
                av_packet_free(&pkt);
            } else {
                // nullptr is the sentinel, break the loop
                break;
            }
        }
        // Add a small sleep to prevent busy-waiting if the queue is empty
        else {
            usleep(100);
        }
    }
}

// this function only used for on thread saving
bool FFmpegWriter::write_packet(uint8_t * pData, int nBytes, int nPts)
{
    AVPacket *pkt;
    pkt = av_packet_alloc();

    // Use sequential counter instead of passed-in PTS to ensure proper playback
    pkt->pts = av_rescale_q(sequential_frame_counter_++, AVRational{1, nFps}, vs->time_base);
    pkt->dts = pkt->pts;
    pkt->stream_index = vs->index;
    pkt->data = pData;
    pkt->size = nBytes;
    
    // Add duration for each frame
    pkt->duration = av_rescale_q(1, AVRational{1, nFps}, vs->time_base);

    // Check for keyframe
    if(!memcmp(pData, "\x00\x00\x00\x01\x67", 5)) {
        pkt->flags |= AV_PKT_FLAG_KEY;
    }

    // Write the compressed frame into the output
    int ret = av_write_frame(oc, pkt);
    av_packet_free(&pkt); // Free the packet after writing
    if (ret < 0) {
        std::cout << "FFMPEG: Error while writing video frame" << std::endl;
        return false;
    }
    return true;
}