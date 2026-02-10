// FFmpegWriter.h
#pragma once
extern "C"
{
#include <libavformat/avformat.h>
#include <libavutil/opt.h>
#include <libswresample/swresample.h>
};
#include <iostream>
#include <fstream>
#include <thread>
#include <string>
#include <utility>
#include <vector>
#include "thread.h"

class FFmpegWriter
{
public:
    FFmpegWriter(AVCodecID eCodecId, int nWidth, int nHeight, int nFps, const char *szOutFilePath, const char *metadata_file,
                 const std::vector<std::pair<std::string, std::string>>& metadata_tags = {});
    ~FFmpegWriter();
    bool write_packet(uint8_t *pData, int nBytes, int nPts);
    void push_packet(uint8_t* pData, int nBytes, int nPts);
    void create_thread();
    void quit_thread();
    void join_thread();
    void write_one_pkt(AVPacket* pkt); 
private:
    AVFormatContext *oc = NULL;
    AVStream *vs = NULL;
    int nFps = 0;
    int nPts = 0;
    std::ofstream *metadata;
    SafeQueue<AVPacket*> m_queue; // Queue for packets to be written
    std::thread m_thread;
    int sequential_frame_counter_ = 0; // Counter for sequential frame numbers
    AVCodecID codec_id_ = AV_CODEC_ID_NONE;
    std::string keyframe_file_;
    std::vector<int64_t> keyframe_frames_;
    void write_thread();
    void write_keyframe_sidecar();
    bool packet_has_idr(const uint8_t* data, size_t size) const;
    bool packet_has_idr_h264(const uint8_t* data, size_t size) const;
    bool packet_has_idr_hevc(const uint8_t* data, size_t size) const;
    std::string keyframe_sidecar_path() const;
};
