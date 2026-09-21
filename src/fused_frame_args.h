// src/fused_frame_args.h
//
// Per-frame arguments of the fused analytics graph (step 3 of
// docs/handoff_pose_analytics_2026_09_16.md). A captured CUDA graph bakes
// every kernel argument in, so anything that changes per frame (the source
// frame address, the pool copy addresses, the mask policy, the ROI
// parameters) lives in this block instead: one per pose slot, in pinned
// mapped host memory the YOLO thread writes before the launch and the
// kernels read through the device mapping. Nothing per frame touches the
// driver except the graph launch and the entry's event records.
#pragma once

#include <cuda_runtime_api.h>

#include "detect_roi.h"
#include "optimized_yolo_preprocess.h"

struct FusedFrameArgs {
    // Source mono frame the detect preprocess and the pose crop read
    // (camera buffer, pool copy or owned copy, chosen per the late-owned-copy
    // contract), tightly packed, src_pitch bytes per row.
    const unsigned char* src_frame = nullptr;
    int src_pitch = 0;
    int src_width = 0;
    int src_height = 0;
    // Late owned pool copy (lever 2d): copy_bytes 0 = nothing to copy.
    const unsigned char* copy_src = nullptr;
    unsigned char* copy_dst = nullptr;
    unsigned long long copy_bytes = 0;
    // Input circle mask for the detect preprocess (gate_and_input_mask).
    int mask_enabled = 0;
    YoloPreprocessCircleMask mask{};
    // Device ROI selection parameters, including the centroid gate.
    DetectRoiParams roi_params{};
};

// Detect preprocess (mono only) reading the source pointer and the mask
// from d_args; bit-identical to launch_optimized_yolo_preprocess /
// launch_optimized_yolo_preprocess_circle_masked for the same inputs.
void launch_optimized_yolo_preprocess_indirect(
    const FusedFrameArgs* d_args,
    float* d_dst_planar,
    int src_width, int src_height,
    int dst_width, int dst_height,
    cudaStream_t stream);

// Pose crop reading the source pointer from d_args and the origin from d_roi.
void launch_pose_crop_from_roi_indirect(
    const FusedFrameArgs* d_args,
    const DetectRoi* d_roi,
    unsigned char* d_dst_mono,
    int crop_w,
    int crop_h,
    cudaStream_t stream);

// Device-to-device copy of copy_bytes from copy_src to copy_dst (no-op when
// copy_bytes is 0), as a kernel so it can be captured with varying addresses.
void launch_indirect_copy(const FusedFrameArgs* d_args, cudaStream_t stream);
