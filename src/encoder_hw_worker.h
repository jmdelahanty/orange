// src/encoder_hw_worker.h

#ifndef ENCODER_HW_WORKER_H
#define ENCODER_HW_WORKER_H

#include "threadworker.h"
#include "video_capture.h"
#include "gpu_video_encoder.h"
#include <chrono>
#include <deque>
#include <fstream>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include "encoder_pipeline.h"
#include "json.hpp"
#include "pre_encoder_reference_writer.h"
#include "shared_recording_output.h"

class EncoderPreprocessWorker; // Forward declaration

struct RecordingBitrateEstimate {
    uint32_t average_bitrate = 0;
    uint32_t max_bitrate = 0;
    double target_bpp = 0.0;
    bool average_clamped_to_min = false;
    bool average_clamped_to_max = false;
    bool max_clamped_to_max = false;
};

RecordingBitrateEstimate estimate_recording_bitrate(const CameraParams& camera_params,
                                                    const RecordingOutputConfig& recording_output_config);
int sanitize_recording_gop_length(int requested_gop_length);
uint32_t resolve_recording_gop_length(const CameraParams& camera_params,
                                      const std::string& tuning,
                                      int requested_gop_length);

class EncoderHwWorker : public CThreadWorker<ENCODER_WORKER_ENTRY>
{
public:
    EncoderHwWorker(
        const char* name,
        CameraParams* camera_params,
        int encode_gpu_id,
        const RecordingOutputConfig& recording_output_config,
        const std::string& codec,
        const std::string& preset,
        const std::string& tuning,
        const std::string& rate_control_mode,
        int quality_value,
        int gop_length,
        const EncoderControlOverrides& encoder_control_overrides,
        const ImportanceMapConfig& importance_map_config,
        std::string folder_name,
        std::shared_ptr<SharedRecordingOutput> shared_output,
        bool owns_recording_output,
        EncoderPreprocessWorker* prep_worker,
        CameraControl* camera_control,
        const PreEncoderReferenceCaptureConfig& pre_encoder_reference_capture_config
    );
    ~EncoderHwWorker() override;

    void SetPreprocessWorker(EncoderPreprocessWorker* prep_worker);
    void flush_and_close();

    double get_fps() const { return current_fps_.load(std::memory_order_relaxed); }
    uint64_t get_total_packets() const { return total_packets_.load(); }
    uint64_t get_encode_failures() const { return encode_failures_.load(); }
    uint64_t get_slow_frames() const { return slow_frames_.load(); }
    int get_queue_depth() const { return const_cast<EncoderHwWorker*>(this)->GetCountQueueInSize(); }
    int encode_gpu_id() const { return encode_gpu_id_; }
    uint32_t recording_gop_length() const { return recording_gop_length_; }
    const RecordingStrategyConfig& recording_strategy_config() const { return recording_strategy_config_; }
    bool direct_input_enabled() const { return direct_input_enabled_; }
    int encoder_input_pitch() const { return encoder_input_pitch_; }
    int encoder_buffer_count() const { return encoder_buffer_count_; }
    void SetSplitGopTopologyStaticSnapshot(const nlohmann::json& topology_snapshot);
    void SetSplitGopTopologyRuntimeSnapshot(const nlohmann::json& topology_snapshot);

    EncoderContext encoder_;
    EncoderPreprocessWorker* m_prep_worker_;

protected:
    bool WorkerFunction(ENCODER_WORKER_ENTRY* f) override;

private:
    void finalize_recording();
    bool drain_ready();
    nlohmann::json build_encoder_snapshot_json() const;
    void initialize_pre_encoder_reference_capture();
    void finalize_pre_encoder_reference_capture();
    bool begin_pre_encoder_reference_capture(ENCODER_WORKER_ENTRY* entry,
                                             size_t* staging_slot_out,
                                             size_t* frame_size_out);
    void poll_pre_encoder_reference_captures(bool wait_for_all);
    void recycle_encoder_entry(ENCODER_WORKER_ENTRY* entry,
                               const std::vector<uint32_t>& retired_slots,
                               bool slot_submitted);
    void release_pre_encoder_reference_capture_resources();
    bool ensure_pre_encoder_reference_staging_slots(size_t frame_size, std::string* error_out);
    void initialize_importance_map();
    bool importance_map_active() const;
    void refresh_writer_queue_metrics();
    void reset_pending_gop_state();
    void buffer_encoded_packets(const std::vector<std::vector<uint8_t>>& packets,
                                const std::vector<uint64_t>& output_timestamps,
                                int64_t fallback_sample_index,
                                uint64_t completion_gop_index,
                                bool mark_complete,
                                const std::optional<RecordingMetadataRow>& metadata_row,
                                const std::optional<RecordingOutputTimingSample>& timing_sample = std::nullopt);
    std::vector<uint64_t> resolve_output_sample_indices(
        size_t packet_count,
        const std::vector<uint64_t>& output_timestamps,
        std::optional<uint64_t> submitted_sample_index,
        const char* context);
    void note_submitted_frame(uint64_t gop_index, bool is_last_frame_in_gop);
    std::vector<uint64_t> note_emitted_packets_and_collect_completed_gops(
        const std::vector<uint64_t>& packet_sample_indices);
    void flush_pending_gops(bool flush_all);
    size_t pending_gop_backlog_count() const;
    int64_t oldest_pending_gop_age_ms() const;

    struct ReferenceCaptureStagingSlot {
        unsigned char* host_buffer = nullptr;
        cudaEvent_t copy_complete_event = nullptr;
        size_t buffer_size = 0;
        bool in_use = false;
    };

    struct PendingReferenceCapture {
        ENCODER_WORKER_ENTRY* entry = nullptr;
        size_t staging_slot = 0;
        size_t frame_size = 0;
        std::vector<uint32_t> retired_slots;
        bool slot_submitted = false;
    };

    struct BufferedEncodedPacket {
        std::vector<uint8_t> bytes;
        int64_t sample_index = -1;
    };

    struct PendingGop {
        uint64_t gop_index = 0;
        uint32_t frame_count = 0;
        bool complete = false;
        size_t total_bytes = 0;
        std::chrono::steady_clock::time_point created_at = std::chrono::steady_clock::now();
        std::vector<BufferedEncodedPacket> packets;
    };

    struct EncoderSnapshotInfo {
        std::string backend;
        std::string path;
        std::string codec;
        std::string preset;
        std::string tuning;
        std::string rc_strategy;
        std::string output_mode;
        uint32_t width = 0;
        uint32_t height = 0;
        uint32_t source_width = 0;
        uint32_t source_height = 0;
        uint32_t fps = 0;
        uint32_t downsample_factor = 1;
        uint32_t requested_output_width = 0;
        uint32_t requested_output_height = 0;
        uint32_t gop_length = 0;
        uint32_t frame_interval_p = 0;
        uint32_t idr_period = 0;
        uint32_t max_num_ref_frames = 0;
        uint32_t max_num_ref_frames_in_dpb = 0;
        uint32_t rc_mode = 0;
        uint32_t average_bitrate = 0;
        uint32_t max_bitrate = 0;
        uint32_t vbv_buffer_size = 0;
        uint32_t target_quality = 0;
        uint32_t target_quality_lsb = 0;
        uint32_t const_qp_inter_p = 0;
        uint32_t const_qp_inter_b = 0;
        uint32_t const_qp_intra = 0;
        uint32_t enable_aq = 0;
        uint32_t enable_temporal_aq = 0;
        uint32_t enable_lookahead = 0;
        uint32_t lookahead_depth = 0;
        uint32_t multi_pass = 0;
        uint32_t low_delay_keyframe_scale = 0;
        uint32_t strict_gop_target = 0;
        uint32_t enable_non_ref_p = 0;
        uint32_t repeat_sps_pps = 0;
        uint32_t enable_ptd = 0;
        int requested_aq = -1;
        int requested_temporal_aq = -1;
        int requested_lookahead = -1;
        int requested_lookahead_depth = -1;
        int requested_target_bitrate_bps = -1;
        int requested_max_bitrate_bps = -1;
        int requested_vbv_buffer_size = -1;
        std::string requested_importance_map_mode = "off";
        int requested_importance_map_roi_size_px = ImportanceMapConfig::kDefaultRoiSizePx;
        std::string active_importance_map_mode = "off";
        uint32_t qp_map_mode = 0;
        uint32_t importance_map_block_size = 0;
        uint32_t importance_map_grid_width = 0;
        uint32_t importance_map_grid_height = 0;
        uint32_t importance_map_roi_size_px = 0;
        int importance_map_inside_delta = 0;
        int importance_map_outside_delta = 0;
        int source_gpu_id = -1;
        int encode_gpu_id = -1;
        int gpu_id = -1;
        nlohmann::json gpu = nlohmann::json::object();
        nlohmann::json split_gop_topology_static = nlohmann::json::object();
        nlohmann::json split_gop_topology_runtime = nlohmann::json::object();
        nlohmann::json resolved_config = nlohmann::json::object();
        bool color = false;
        RecordingStrategyConfig recording_strategy;
    };

    CameraParams* camera_params_;
    int encode_gpu_id_;
    RecordingOutputConfig recording_output_config_;
    std::string base_folder_name_;
    std::string codec_;
    std::string preset_;
    std::string tuning_;
    std::string rate_control_mode_;
    EncoderControlOverrides encoder_control_overrides_;
    ImportanceMapConfig importance_map_config_;
    PreEncoderReferenceCaptureConfig pre_encoder_reference_capture_config_;
    bool direct_input_enabled_ = false;
    bool direct_input_registered_ = false;
    bool importance_map_enabled_ = false;
    int encoder_input_pitch_ = 0;
    int encoder_buffer_count_ = 0;
    bool pre_encoder_reference_async_enabled_ = false;
    int quality_value_ = 20;
    int gop_length_ = 0;
    uint32_t recording_gop_length_ = 1;
    RecordingStrategyConfig recording_strategy_config_;
    std::shared_ptr<SharedRecordingOutput> shared_output_;
    bool owns_recording_output_ = true;
    Writer writer_;
    PreEncoderReferenceWriter pre_encoder_reference_writer_;
    std::vector<ReferenceCaptureStagingSlot> pre_encoder_reference_staging_slots_;
    std::deque<PendingReferenceCapture> pending_pre_encoder_reference_captures_;
    std::string active_recording_folder_;
    std::vector<int8_t> importance_map_qp_delta_;
    std::size_t importance_map_qp_delta_size_ = 0;
    cudaStream_t m_stream = nullptr;
    cudaStream_t pre_encoder_reference_stream_ = nullptr;
    CameraControl* camera_control_;

    bool is_recording_ = false; // Tracks the local recording state of this worker
    bool encoder_snapshot_valid_ = false;
    EncoderSnapshotInfo encoder_snapshot_;
    bool writer_queue_overflowed_ = false;
    uint64_t writer_queue_overflow_events_ = 0;
    size_t writer_queue_peak_packets_ = 0;
    size_t writer_queue_peak_bytes_ = 0;
    std::map<uint64_t, PendingGop> pending_gops_;
    uint64_t next_gop_to_flush_ = 0;
    size_t pending_gop_buffered_bytes_ = 0;
    size_t pending_gop_peak_count_ = 0;
    size_t pending_gop_peak_backlog_count_ = 0;
    size_t pending_gop_peak_bytes_ = 0;
    bool pending_gop_overflowed_ = false;
    uint64_t pending_gop_overflow_events_ = 0;
    std::deque<uint64_t> pending_output_sample_indices_;
    std::map<uint64_t, uint32_t> submitted_frames_by_gop_;
    std::map<uint64_t, uint32_t> emitted_frames_by_gop_;
    std::set<uint64_t> submitted_complete_gops_;

    uint64_t last_recording_frame_id_ = 0;
    std::chrono::steady_clock::time_point last_fps_update_time_;
    int frame_counter_;
    std::atomic<double> current_fps_{0.0};
    std::atomic<uint64_t> slow_frames_{0};
    std::atomic<uint64_t> total_packets_{0};
    std::atomic<uint64_t> encode_failures_{0};
};

#endif // ENCODER_HW_WORKER_H
