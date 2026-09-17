// src/pose_crop_from_roi.h
//
// Cuts the pose crop from a mono source frame using the origin the device
// ROI kernel (src/detect_roi.h) wrote, so the crop can be queued on the
// YOLO stream right behind the detect graph without a host round trip
// (step 1 of the plan in docs/handoff_pose_analytics_2026_09_16.md).
//
// The extent (crop_w x crop_h) is a host launch parameter because it is
// fixed per configuration; only the origin is read from the device. Pixels
// outside the source, and every pixel when roi->valid is 0, are written as
// 0: a blank crop the pose model scores below threshold, which is what
// run-and-mask needs (pose runs on every frame; the CPU discards results
// whose ROI was invalid).
#pragma once

#include <cuda_runtime_api.h>

#include "detect_roi.h"

// d_src is tightly packed mono unless src_pitch says otherwise (pitch in
// bytes). d_dst_mono is tightly packed crop_w x crop_h.
void launch_pose_crop_from_roi(
    const unsigned char* d_src,
    int src_pitch,
    int src_w,
    int src_h,
    const DetectRoi* d_roi,
    unsigned char* d_dst_mono,
    int crop_w,
    int crop_h,
    cudaStream_t stream);

// CPU reference with the same semantics, for the unit test.
void pose_crop_from_roi_host(
    const unsigned char* src,
    int src_pitch,
    int src_w,
    int src_h,
    const DetectRoi& roi,
    unsigned char* dst_mono,
    int crop_w,
    int crop_h);
