// FFmpegWriter.h
#pragma once
extern "C"
{
#include <libavformat/avformat.h>
#include <libavutil/opt.h>
#include <libswresample/swresample.h>
};
#include <iostream>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <mutex>
#include <thread>
#include <string>
#include <utility>
#include <vector>
#include "latency_stats.h"
#include "thread.h"

struct FFmpegWriterQueueConfig {
    size_t max_queued_packets = 0;
    size_t max_queued_bytes = 0;
};

struct FFmpegWriterLatencyStats {
    LatencyAggregateStats push_packet_total;
    LatencyAggregateStats packet_alloc_copy;
    LatencyAggregateStats queue_push;
    LatencyAggregateStats queue_wait;
    LatencyAggregateStats packet_write;
    LatencyAggregateStats gop_release_to_last_write;
};

class FFmpegWriter
{
public:
    FFmpegWriter(AVCodecID eCodecId, int nWidth, int nHeight, int nFps, const char *szOutFilePath, const char *metadata_file,
                 const std::vector<std::pair<std::string, std::string>>& metadata_tags = {},
                 FFmpegWriterQueueConfig queue_config = {});
    ~FFmpegWriter();
    bool write_packet(uint8_t *pData, int nBytes, int64_t nPts);
    void push_packet(uint8_t* pData,
                     int nBytes,
                     int64_t nPts,
                     uint64_t gop_index = 0,
                     bool is_last_packet_in_gop = false,
                     uint64_t gop_release_started_ns = 0);
    void create_thread();
    void quit_thread();
    void join_thread();
    void write_one_pkt(AVPacket* pkt); 
    bool has_queue_overflowed() const { return queue_overflowed_.load(std::memory_order_relaxed); }
    uint64_t queue_overflow_events() const { return queue_overflow_events_.load(std::memory_order_relaxed); }
    size_t queued_packets() const { return queued_packets_.load(std::memory_order_relaxed); }
    size_t queued_bytes() const { return queued_bytes_.load(std::memory_order_relaxed); }
    size_t peak_queued_packets() const { return peak_queued_packets_.load(std::memory_order_relaxed); }
    size_t peak_queued_bytes() const { return peak_queued_bytes_.load(std::memory_order_relaxed); }
    const FFmpegWriterQueueConfig& queue_config() const { return queue_config_; }
    FFmpegWriterLatencyStats latency_stats() const;
private:
    struct QueuedPacket {
        AVPacket* packet = nullptr;
        uint64_t enqueued_at_ns = 0;
        uint64_t gop_index = 0;
        bool is_last_packet_in_gop = false;
        uint64_t gop_release_started_ns = 0;
    };

    AVFormatContext *oc = NULL;
    AVStream *vs = NULL;
    int nFps = 0;
    int nPts = 0;
    std::ofstream *metadata;
    SafeQueue<QueuedPacket> m_queue; // Queue for packets to be written
    std::thread m_thread;
    int64_t sequential_frame_counter_ = 0; // Counter for sequential frame numbers
    AVCodecID codec_id_ = AV_CODEC_ID_NONE;
    std::string output_label_;
    std::string keyframe_file_;
    std::vector<int64_t> keyframe_frames_;
    FFmpegWriterQueueConfig queue_config_;
    std::atomic<size_t> queued_packets_{0};
    std::atomic<size_t> queued_bytes_{0};
    std::atomic<size_t> peak_queued_packets_{0};
    std::atomic<size_t> peak_queued_bytes_{0};
    std::atomic<bool> queue_overflowed_{false};
    std::atomic<uint64_t> queue_overflow_events_{0};
    mutable std::mutex latency_mutex_;
    FFmpegWriterLatencyStats latency_stats_;
    void write_thread();
    void write_keyframe_sidecar();
    bool packet_has_idr(const uint8_t* data, size_t size) const;
    bool packet_has_idr_h264(const uint8_t* data, size_t size) const;
    bool packet_has_idr_hevc(const uint8_t* data, size_t size) const;
    std::string keyframe_sidecar_path() const;
};
