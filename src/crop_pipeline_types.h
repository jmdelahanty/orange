#ifndef ORANGE_CROP_PIPELINE_TYPES_H
#define ORANGE_CROP_PIPELINE_TYPES_H

#include "crop_producer.h"

#include <cstddef>
#include <cstdint>
#include <string>

struct CropEncodePerfSample : CropProducerPerfSample {
    uint64_t worker_start_steady_ns = 0;
    int queue_depth_start = 0;
    bool encode_active = false;
    bool has_detection = false;
    bool blank_frame = false;
    bool dropped = false;
    std::string drop_reason;
    int crop_x = 0;
    int crop_y = 0;
    int crop_w = 0;
    int crop_h = 0;
    size_t packet_count = 0;
    size_t encoded_bytes = 0;
    double crop_preview_cpu_ms = 0.0;
    double encode_submit_cpu_ms = 0.0;
    double metadata_cpu_ms = 0.0;
    double stream_sync_ms = 0.0;
    double display_sync_ms = 0.0;
    double total_ms = 0.0;
};

struct CropEncodeJob {
    CropFrameSnapshot frame;
    CropEncodePerfSample perf;
    CropFrame* crop_frame = nullptr;
};

#endif  // ORANGE_CROP_PIPELINE_TYPES_H
