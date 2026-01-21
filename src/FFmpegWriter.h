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
    void write_thread();
};
