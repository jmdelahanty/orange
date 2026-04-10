// src/encoder_pipeline.h

#ifndef ENCODER_PIPELINE_H
#define ENCODER_PIPELINE_H

#include <cstddef>
#include <cstdint>
#include <string>
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
    std::string mode = "off";

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

// The lightweight struct to pass data from the preprocess worker to the hardware encoder.
struct ENCODER_WORKER_ENTRY {
    unsigned char* d_prepared_frame;
    int slot_id = -1;
    size_t surface_pitch = 0;
    bool direct_input = false;
    uint64_t recording_frame_id;
    uint64_t timestamp;
    uint64_t timestamp_sys;
    cudaEvent_t* preprocess_complete_event; // Add this event pointer
};

#endif // ENCODER_PIPELINE_H
