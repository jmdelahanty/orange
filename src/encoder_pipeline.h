// src/encoder_pipeline.h

#ifndef ENCODER_PIPELINE_H
#define ENCODER_PIPELINE_H

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>
#include <cuda_runtime.h> // Add this include for cudaEvent_t

struct RecordingOutputConfig {
    std::string mode = "factor";
    int downsample_factor = 1;
    int requested_width = 0;
    int requested_height = 0;
    int resolved_width = 0;
    int resolved_height = 0;
    bool resize_enabled = false;
};

struct PreEncoderReferenceCaptureConfig {
    bool enabled = false;
    int max_frames = 0;
    int max_seconds = 0;
    std::string output_dir;

    bool has_frame_bound() const { return max_frames > 0; }
    bool has_time_bound() const { return max_seconds > 0; }
    bool has_valid_bound() const { return !enabled || (has_frame_bound() != has_time_bound()); }
};

struct ImportanceMapConfig {
    static constexpr int kDefaultRoiSizePx = 512;

    std::string mode = "off";
    int roi_size_px = kDefaultRoiSizePx;

    bool enabled() const { return mode != "off"; }
};

struct EncoderControlOverrides {
    int aq = -1;
    int temporal_aq = -1;
    int lookahead = -1;
    int lookahead_depth = -1;
    int target_bitrate_bps = -1;
    int max_bitrate_bps = -1;
    int vbv_buffer_size = -1;

    bool has_any_override() const {
        return aq >= 0 || temporal_aq >= 0 || lookahead >= 0 || lookahead_depth >= 0 ||
               target_bitrate_bps >= 0 || max_bitrate_bps >= 0 || vbv_buffer_size >= 0;
    }
};

struct SplitGopWriterQueueConfig {
    uint64_t max_packets = 0;
    uint64_t max_bytes = 0;
    bool fail_on_overflow = true;
};

struct SplitGopConfig {
    bool enabled = false;
    std::string placement = "single_gpu";
    std::vector<int> encoder_gpu_ids;
    std::string source_encoder_policy = "local_only";
    std::string transfer_mode = "auto";
    uint64_t max_inflight_gops = 0;
    uint64_t max_buffered_bytes = 0;
    bool strict = false;
    SplitGopWriterQueueConfig writer_queue;
};

struct RecordingStrategyConfig {
    std::string requested_mode = "single_session";
    std::string mode = "single_session";
    std::string resolution_note;
    SplitGopConfig split_gop;

    bool split_gop_enabled() const { return mode == "split_gop" && split_gop.enabled; }
};

struct CameraRecordingEncodeConfig {
    std::string codec = "h264";
    std::string preset = "p1";
    std::string tuning = "ll";
    std::string rate_control_mode = "vbr";
    int quality_value = 20;
    int gop_length = 0;
    bool nvenc_direct_input = false;
};

struct CameraRecordingOutputConfig {
    std::string mode = "factor";
    int downsample_factor = 1;
    int requested_width = 0;
    int requested_height = 0;
};

struct CameraRecordingConstraintsConfig {
    bool require_peer_access = false;
    std::string preferred_topology_class;
};

struct CameraRecordingResourcesConfig {
    int acquire_work_entries = 0;
    int encoder_entry_pool_size = 0;
};

struct CameraRecordingConfig {
    std::string profile_name;
    CameraRecordingEncodeConfig encode;
    CameraRecordingOutputConfig output;
    RecordingStrategyConfig strategy;
    CameraRecordingConstraintsConfig constraints;
    CameraRecordingResourcesConfig resources;
};

struct ResolvedRecordingConfig {
    int source_gpu_id = -1;
    int recording_gpu_id = -1;
    CameraRecordingEncodeConfig encode;
    RecordingOutputConfig output;
    RecordingStrategyConfig strategy;
    CameraRecordingConstraintsConfig constraints;
    CameraRecordingResourcesConfig resources;
    EncoderControlOverrides encoder_control_overrides;
    ImportanceMapConfig importance_map;
    std::string base_folder_name;
    PreEncoderReferenceCaptureConfig pre_encoder_reference_capture;
};

struct ResolvedRecordingConfigOverrides {
    int recording_gpu_id = -1;
    bool has_output_override = false;
    RecordingOutputConfig output;
    std::string codec;
    std::string preset;
    std::string tuning;
    std::string rate_control_mode;
    int quality_value = -1;
    int gop_length = -1;
    bool has_nvenc_direct_input_override = false;
    bool nvenc_direct_input = false;
    EncoderControlOverrides encoder_control_overrides;
    ImportanceMapConfig importance_map;
    std::string base_folder_name;
    PreEncoderReferenceCaptureConfig pre_encoder_reference_capture;
};

// The lightweight struct to pass data from the preprocess worker to the hardware encoder.
struct ENCODER_WORKER_ENTRY {
    unsigned char* d_prepared_frame;
    int slot_id = -1;
    size_t surface_pitch = 0;
    bool direct_input = false;
    int surface_gpu_id = -1;
    bool cross_gpu_copy_performed = false;
    uint64_t recording_frame_id;
    uint64_t gop_index = 0;
    uint32_t frame_index_within_gop = 0;
    bool is_last_frame_in_gop = false;
    uint64_t timestamp;
    uint64_t timestamp_sys;
    cudaEvent_t* preprocess_complete_event; // Add this event pointer
    cudaEvent_t copy_start_event = nullptr;
    cudaEvent_t copy_end_event = nullptr;
};

#endif // ENCODER_PIPELINE_H
