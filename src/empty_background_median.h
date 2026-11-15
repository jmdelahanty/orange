// Per-pixel median of the 17 burst frames on the landing die
// (docs/empty_background_capture_design.md, "Burst"). The burst stays in device
// memory: only the median (one frame) crosses PCIe, not the 17 frames.
//
// Exact, and equal to orange::empty_background::MedianOfBurst on the CPU (the
// 9th smallest of 17 samples, so the median is always one of the samples). The
// same kernel counts pixels that are 255 in at least one frame
// (any_frame_saturated_fraction's numerator).
#pragma once

#include <cuda_runtime_api.h>

#include <cstddef>
#include <cstdint>

namespace orange::empty_background {

inline constexpr int kMedianFrameCount = 17;

// frames: kMedianFrameCount frames of pixel_count bytes, frame k at frames + k * frame_stride_bytes
// (frame_stride_bytes >= pixel_count). median_out: pixel_count bytes. saturated_count_out: one
// unsigned long long, set to the count (the kernel zeroes it first on `stream`). All three on the
// current device. Asynchronous on `stream`; returns the launch status.
cudaError_t LaunchBurstMedian17(const std::uint8_t* frames, std::size_t frame_stride_bytes, std::size_t pixel_count,
                                std::uint8_t* median_out, unsigned long long* saturated_count_out,
                                cudaStream_t stream);

}  // namespace orange::empty_background
