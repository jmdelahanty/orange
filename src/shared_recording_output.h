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

struct RecordingOutputTimingSample {
    uint64_t encoder_cuda_set_device_ns = 0;
    uint64_t preprocess_complete_stream_wait_enqueue_ns = 0;
    uint64_t source_to_helper_copy_sync_wait_ns = 0;
    uint64_t source_to_helper_copy_elapsed_query_ns = 0;
    bool has_source_to_helper_copy = false;
    uint64_t source_to_helper_copy_ns = 0;
    uint64_t pre_encoder_reference_capture_enqueue_ns = 0;
    uint64_t nvenc_get_next_input_frame_ns = 0;
    bool has_bitstream_fetch = false;
    uint64_t bitstream_fetch_ns = 0;
    uint64_t nvenc_copy_to_input_ns = 0;
    uint64_t nvenc_encode_frame_total_ns = 0;
    uint64_t nvenc_map_input_resource_ns = 0;
    uint64_t nvenc_map_reference_resource_ns = 0;
    uint64_t nvenc_encode_picture_ns = 0;
    uint64_t nvenc_completion_wait_ns = 0;
    uint64_t nvenc_lock_bitstream_ns = 0;
    uint64_t nvenc_bitstream_copy_ns = 0;
    uint64_t nvenc_unlock_bitstream_ns = 0;
    uint64_t nvenc_unmap_input_resource_ns = 0;
    uint64_t nvenc_unmap_reference_resource_ns = 0;
    uint64_t encoder_output_accounting_ns = 0;
};

struct SharedRecordingOutputStats {
    bool is_open = false;
    bool writer_queue_overflowed = false;
    uint64_t writer_queue_overflow_events = 0;
    size_t writer_queue_peak_packets = 0;
    size_t writer_queue_peak_bytes = 0;
    uint64_t next_gop_to_flush = 0;
    size_t pending_gop_count = 0;
    size_t pending_gop_backlog_count = 0;
    size_t pending_gop_bytes = 0;
    size_t pending_gop_peak_count = 0;
    size_t pending_gop_peak_backlog_count = 0;
    size_t pending_gop_peak_bytes = 0;
    bool pending_gop_overflowed = false;
    uint64_t pending_gop_overflow_events = 0;
    int64_t oldest_pending_gop_age_ms = 0;
    std::string pending_gop_overflow_reason;
    uint64_t pending_gop_overflow_completion_gop_index = 0;
    uint64_t pending_gop_overflow_next_gop_to_flush = 0;
    size_t pending_gop_overflow_limit = 0;
    size_t pending_gop_overflow_pending_count = 0;
    size_t pending_gop_overflow_backlog_count = 0;
    bool pending_gop_overflow_frontier_present = false;
    bool pending_gop_overflow_frontier_complete = false;
    std::vector<uint64_t> pending_gop_overflow_pending_keys;
    LatencyAggregateStats encoder_cuda_set_device;
    LatencyAggregateStats preprocess_complete_stream_wait_enqueue;
    LatencyAggregateStats source_to_helper_copy_sync_wait;
    LatencyAggregateStats source_to_helper_copy_elapsed_query;
    LatencyAggregateStats source_to_helper_copy;
    LatencyAggregateStats pre_encoder_reference_capture_enqueue;
    LatencyAggregateStats nvenc_get_next_input_frame;
    LatencyAggregateStats bitstream_fetch;
    LatencyAggregateStats nvenc_copy_to_input;
    LatencyAggregateStats nvenc_encode_frame_total;
    LatencyAggregateStats nvenc_map_input_resource;
    LatencyAggregateStats nvenc_map_reference_resource;
    LatencyAggregateStats nvenc_encode_picture;
    LatencyAggregateStats nvenc_completion_wait;
    LatencyAggregateStats nvenc_lock_bitstream;
    LatencyAggregateStats nvenc_bitstream_copy;
    LatencyAggregateStats nvenc_unlock_bitstream;
    LatencyAggregateStats nvenc_unmap_input_resource;
    LatencyAggregateStats nvenc_unmap_reference_resource;
    LatencyAggregateStats encoder_output_accounting;
    LatencyAggregateStats shared_submit_total;
    LatencyAggregateStats shared_submit_lock_wait;
    LatencyAggregateStats shared_gop_buffering;
    LatencyAggregateStats gop_hold_before_release;
    FFmpegWriterLatencyStats writer_latency;
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
                             const std::optional<RecordingMetadataRow>& metadata_row,
                             const std::optional<RecordingOutputTimingSample>& timing_sample);
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
        uint64_t completed_at_ns = 0;
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
                               const std::optional<RecordingMetadataRow>& metadata_row,
                               const std::optional<RecordingOutputTimingSample>& timing_sample);
    void flush_pending_gops_locked(bool flush_all);
    void refresh_writer_queue_metrics_locked();
    void write_metadata_row_locked(const RecordingMetadataRow& metadata_row);
    int64_t normalize_writer_sample_index_locked(int64_t sample_index);
    void record_pending_gop_overflow_locked(const char* reason,
                                            uint64_t completion_gop_index,
                                            size_t limit);
    size_t pending_gop_backlog_count_locked() const;
    void update_pending_gop_peaks_locked();
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
    bool next_gop_to_flush_initialized_ = false;
    int64_t writer_sample_index_base_ = 0;
    bool writer_sample_index_base_initialized_ = false;
    size_t pending_gop_buffered_bytes_ = 0;
    size_t pending_gop_peak_count_ = 0;
    size_t pending_gop_peak_backlog_count_ = 0;
    size_t pending_gop_peak_bytes_ = 0;
    bool pending_gop_overflowed_ = false;
    uint64_t pending_gop_overflow_events_ = 0;
    std::string pending_gop_overflow_reason_;
    uint64_t pending_gop_overflow_completion_gop_index_ = 0;
    uint64_t pending_gop_overflow_next_gop_to_flush_ = 0;
    size_t pending_gop_overflow_limit_ = 0;
    size_t pending_gop_overflow_pending_count_ = 0;
    size_t pending_gop_overflow_backlog_count_ = 0;
    bool pending_gop_overflow_frontier_present_ = false;
    bool pending_gop_overflow_frontier_complete_ = false;
    std::vector<uint64_t> pending_gop_overflow_pending_keys_;
    bool writer_queue_overflowed_ = false;
    uint64_t writer_queue_overflow_events_ = 0;
    size_t writer_queue_peak_packets_ = 0;
    size_t writer_queue_peak_bytes_ = 0;
    LatencyAggregateStats encoder_cuda_set_device_latency_;
    LatencyAggregateStats preprocess_complete_stream_wait_enqueue_latency_;
    LatencyAggregateStats source_to_helper_copy_sync_wait_latency_;
    LatencyAggregateStats source_to_helper_copy_elapsed_query_latency_;
    LatencyAggregateStats source_to_helper_copy_latency_;
    LatencyAggregateStats pre_encoder_reference_capture_enqueue_latency_;
    LatencyAggregateStats nvenc_get_next_input_frame_latency_;
    LatencyAggregateStats bitstream_fetch_latency_;
    LatencyAggregateStats nvenc_copy_to_input_latency_;
    LatencyAggregateStats nvenc_encode_frame_total_latency_;
    LatencyAggregateStats nvenc_map_input_resource_latency_;
    LatencyAggregateStats nvenc_map_reference_resource_latency_;
    LatencyAggregateStats nvenc_encode_picture_latency_;
    LatencyAggregateStats nvenc_completion_wait_latency_;
    LatencyAggregateStats nvenc_lock_bitstream_latency_;
    LatencyAggregateStats nvenc_bitstream_copy_latency_;
    LatencyAggregateStats nvenc_unlock_bitstream_latency_;
    LatencyAggregateStats nvenc_unmap_input_resource_latency_;
    LatencyAggregateStats nvenc_unmap_reference_resource_latency_;
    LatencyAggregateStats encoder_output_accounting_latency_;
    LatencyAggregateStats shared_submit_total_latency_;
    LatencyAggregateStats shared_submit_lock_wait_latency_;
    LatencyAggregateStats shared_gop_buffering_latency_;
    LatencyAggregateStats gop_hold_before_release_latency_;
    FFmpegWriterLatencyStats writer_latency_stats_;
};

#endif // ORANGE_SHARED_RECORDING_OUTPUT_H
