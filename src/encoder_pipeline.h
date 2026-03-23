// src/encoder_pipeline.h

#ifndef ENCODER_PIPELINE_H
#define ENCODER_PIPELINE_H

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

// The lightweight struct to pass data from the preprocess worker to the hardware encoder.
struct ENCODER_WORKER_ENTRY {
    unsigned char* d_prepared_frame;
    uint64_t recording_frame_id;
    uint64_t timestamp;
    uint64_t timestamp_sys;
    cudaEvent_t* preprocess_complete_event; // Add this event pointer
};

#endif // ENCODER_PIPELINE_H
