#ifndef ORANGE_SHARED_RECORDING_OUTPUT_H
#define ORANGE_SHARED_RECORDING_OUTPUT_H

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "FFmpegWriter.h"
#include "encoder_pipeline.h"
#include "gpu_video_encoder.h"

struct CameraParams;

struct RecordingMetadataRow {
    uint64_t frame_id = 0;
    uint64_t timestamp = 0;
    uint64_t timestamp_sys = 0;
};

struct SharedRecordingOutputStats {
    bool is_open = false;
    bool writer_queue_overflowed = false;
    uint64_t writer_queue_overflow_events = 0;
    size_t writer_queue_peak_packets = 0;
    size_t writer_queue_peak_bytes = 0;
    uint64_t next_gop_to_flush = 0;
    size_t pending_gop_count = 0;
    size_t pending_gop_bytes = 0;
    size_t pending_gop_peak_count = 0;
    size_t pending_gop_peak_bytes = 0;
    bool pending_gop_overflowed = false;
    uint64_t pending_gop_overflow_events = 0;
    int64_t oldest_pending_gop_age_ms = 0;
};

struct SharedRecordingOutputOpenParams {
    CameraParams* camera_params = nullptr;
    RecordingOutputConfig recording_output_config;
    std::string folder_name;
    std::string codec;
    std::vector<std::pair<std::string, std::string>> metadata_tags;
    FFmpegWriterQueueConfig queue_config;
    SplitGopConfig split_gop_config;
    uint32_t recording_gop_length = 1;
};

class SharedRecordingOutput {
public:
    SharedRecordingOutput() = default;
    ~SharedRecordingOutput();

    void open_if_needed(const SharedRecordingOutputOpenParams& params);
    void submit_frame_output(const std::vector<std::vector<uint8_t>>& packets,
                             const std::vector<uint64_t>& output_timestamps,
                             int64_t fallback_sample_index,
                             uint64_t completion_gop_index,
                             bool mark_complete,
                             const std::optional<RecordingMetadataRow>& metadata_row);
    bool close_worker_session(bool request_close_when_idle);
    void close();
    SharedRecordingOutputStats stats() const;
    bool is_open() const;
    std::string active_folder() const;
    size_t active_worker_sessions() const;

private:
    struct BufferedEncodedPacket {
        std::vector<uint8_t> bytes;
        int64_t sample_index = -1;
    };

    struct PendingGop {
        uint64_t gop_index = 0;
        bool complete = false;
        size_t total_bytes = 0;
        std::chrono::steady_clock::time_point created_at = std::chrono::steady_clock::now();
        std::vector<BufferedEncodedPacket> packets;
        std::vector<RecordingMetadataRow> metadata_rows;
    };

    void open_locked(const SharedRecordingOutputOpenParams& params);
    void close_locked();
    void buffer_packets_locked(const std::vector<std::vector<uint8_t>>& packets,
                               const std::vector<uint64_t>& output_timestamps,
                               int64_t fallback_sample_index,
                               uint64_t completion_gop_index,
                               bool mark_complete,
                               const std::optional<RecordingMetadataRow>& metadata_row);
    void flush_pending_gops_locked(bool flush_all);
    void refresh_writer_queue_metrics_locked();
    void write_metadata_row_locked(const RecordingMetadataRow& metadata_row);
    int64_t oldest_pending_gop_age_ms_locked() const;
    void reset_pending_state_locked();

    mutable std::mutex mutex_;
    Writer writer_;
    bool is_open_ = false;
    size_t active_worker_sessions_ = 0;
    bool close_requested_ = false;
    std::string active_folder_;
    SplitGopConfig split_gop_config_;
    uint32_t recording_gop_length_ = 1;
    std::map<uint64_t, PendingGop> pending_gops_;
    uint64_t next_gop_to_flush_ = 0;
    size_t pending_gop_buffered_bytes_ = 0;
    size_t pending_gop_peak_count_ = 0;
    size_t pending_gop_peak_bytes_ = 0;
    bool pending_gop_overflowed_ = false;
    uint64_t pending_gop_overflow_events_ = 0;
    bool writer_queue_overflowed_ = false;
    uint64_t writer_queue_overflow_events_ = 0;
    size_t writer_queue_peak_packets_ = 0;
    size_t writer_queue_peak_bytes_ = 0;
};

#endif // ORANGE_SHARED_RECORDING_OUTPUT_H
