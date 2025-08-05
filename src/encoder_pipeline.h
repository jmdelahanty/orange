// src/encoder_pipeline.h

#ifndef ENCODER_PIPELINE_H
#define ENCODER_PIPELINE_H

#include <cstdint> // For uint64_t

// The lightweight struct to pass data from the preprocess worker to the hardware encoder.
// This is the common language between the two stages.
struct ENCODER_WORKER_ENTRY {
    unsigned char* d_prepared_frame; // Pointer to the NV12/YUV frame, ready for the encoder
    uint64_t recording_frame_id;
    uint64_t timestamp;
    uint64_t timestamp_sys;
};

#endif // ENCODER_PIPELINE_H