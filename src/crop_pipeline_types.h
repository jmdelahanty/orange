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

    void ResetForReuse()
    {
        event_wait_cpu_ms = 0.0;
        crop_pool_wait_ms = 0.0;
        crop_producer_cpu_ms = 0.0;
        crop_source_wait_enqueue_cpu_ms = 0.0;
        analytics_owned_wait_cpu_ms = 0.0;
        source_stage_enqueue_cpu_ms = 0.0;
        crop_copy_start_event_record_cpu_ms = 0.0;
        crop_roi_copy_enqueue_cpu_ms = 0.0;
        crop_ready_event_record_cpu_ms = 0.0;
        source_release_event_record_cpu_ms = 0.0;
        crop_copy_gpu_ms = -1.0;
        worker_start_steady_ns = 0;
        queue_depth_start = 0;
        encode_active = false;
        has_detection = false;
        blank_frame = false;
        dropped = false;
        drop_reason.clear();
        crop_x = 0;
        crop_y = 0;
        crop_w = 0;
        crop_h = 0;
        packet_count = 0;
        encoded_bytes = 0;
        crop_preview_cpu_ms = 0.0;
        encode_submit_cpu_ms = 0.0;
        metadata_cpu_ms = 0.0;
        stream_sync_ms = 0.0;
        display_sync_ms = 0.0;
        total_ms = 0.0;
    }
};

struct CropEncodeJob {
    CropFrameSnapshot frame;
    CropEncodePerfSample perf;
    CropFrame* crop_frame = nullptr;

    void ResetForReuse()
    {
        frame.ResetForReuse();
        perf.ResetForReuse();
        crop_frame = nullptr;
    }
};

struct CropPreviewJob {
    CropFrameSnapshot frame;
    CropFrame* crop_frame = nullptr;
    bool blank_preview = false;

    void ResetForReuse()
    {
        frame.ResetForReuse();
        crop_frame = nullptr;
        blank_preview = false;
    }
};

#endif  // ORANGE_CROP_PIPELINE_TYPES_H
