// src/detect_roi.h
//
// Device-side selection of the crop origin from the detect engine's
// EfficientNMS outputs (step 2 of
// docs/analytics_pipeline_configuration_design_2026_09_12.md).
//
// Today the crop origin is computed on the CPU after the detections have
// been copied device-to-host and un-letterboxed by YOLOv8::postprocess, and
// the crop producer picks the highest-score box (std::max_element, first of
// equals) and centres a fixed-size crop on it, clamped to the frame
// (src/crop_producer_worker.cpp). The kernel here reproduces that arithmetic
// bit for bit on the device, reading num_dets / boxes / scores / labels
// straight from the engine's output buffers, so a later stage can crop
// without waiting for the host. compute_detect_roi_host() is the same
// arithmetic on the CPU; the unit test (tools/detect_roi_tests.cpp) checks
// the two agree field for field, and the YOLO worker compares the device
// result against the existing CPU path on every frame when
// ORANGE_ANALYTICS_DEVICE_ROI is on.
//
// Float ordering matters for the integer crop origin: the device code uses
// __fadd_rn / __fsub_rn / __fmul_rn so the compiler cannot contract
// x0 + width * 0.5f into an FMA, which could move (int)cx across an integer
// boundary relative to the host.
#pragma once

#include <cuda_runtime_api.h>

struct DetectRoi {
    int valid = 0;        // 1 when at least one detection was present
    int crop_x = 0;       // crop origin in source pixels, clamped to [0, src_w - crop_w]
    int crop_y = 0;
    int crop_w = 0;
    int crop_h = 0;
    int label = 0;        // label of the selected box
    int num_dets = 0;     // detections considered (after the postprocess clamp)
    int best_index = -1;  // index of the selected box in the NMS output
    float score = 0.0f;   // score of the selected box
    float box_x = 0.0f;   // selected box in source pixels (un-letterboxed, clamped)
    float box_y = 0.0f;
    float box_w = 0.0f;
    float box_h = 0.0f;
};

struct DetectRoiParams {
    // Letterbox parameters of the preprocess that fed the engine
    // (YOLOv8::pparam): inv_ratio = 1 / r, dw/dh = padding in input pixels.
    float inv_ratio = 1.0f;
    float dw = 0.0f;
    float dh = 0.0f;
    float src_w = 0.0f;   // source frame size as floats (postprocess clamps to these)
    float src_h = 0.0f;
    int src_w_int = 0;    // and as ints (the crop clamp uses entry->width/height)
    int src_h_int = 0;
    int crop_w = 0;       // fixed crop size (crop_pipeline.crop_size_px, sanitized)
    int crop_h = 0;
    int max_dets = 0;     // capacity of the boxes/scores/labels bindings
};

// The same clamp YOLOv8::postprocess applies to num_dets.
constexpr int kDetectRoiMaxReasonableDets = 1000;

// Enqueue the selection on `stream`. Reads the four EfficientNMS output
// buffers (device), writes one DetectRoi (device). One thread; the work is
// at most max_dets comparisons.
void launch_detect_roi_kernel(
    const int* d_num_dets,
    const float* d_boxes,     // [max_dets][4] letterboxed x0 y0 x1 y1
    const float* d_scores,    // [max_dets]
    const int* d_labels,      // [max_dets]
    DetectRoi* d_out,
    const DetectRoiParams& params,
    cudaStream_t stream);

// CPU reference with the same arithmetic, on host copies of the same buffers.
DetectRoi compute_detect_roi_host(
    const int* num_dets,
    const float* boxes,
    const float* scores,
    const int* labels,
    const DetectRoiParams& params);
