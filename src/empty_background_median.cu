// src/empty_background_median.cu (see empty_background_median.h)
#include "empty_background_median.h"

namespace orange::empty_background {
namespace {

__global__ void burst_median17_kernel(const std::uint8_t* __restrict__ frames, std::size_t stride,
                                      std::size_t pixel_count, std::uint8_t* __restrict__ median_out,
                                      unsigned long long* __restrict__ saturated_count)
{
    const std::size_t pixel = static_cast<std::size_t>(blockIdx.x) * blockDim.x + threadIdx.x;
    bool saturated = false;
    if (pixel < pixel_count) {
        // Insertion sort of 17 bytes in registers; the kernel is bound by the
        // 17 coalesced byte reads per pixel, not by the compares.
        unsigned int v[kMedianFrameCount];
#pragma unroll
        for (int k = 0; k < kMedianFrameCount; ++k) {
            v[k] = frames[static_cast<std::size_t>(k) * stride + pixel];
            saturated |= v[k] == 255u;
        }
#pragma unroll
        for (int i = 1; i < kMedianFrameCount; ++i) {
#pragma unroll
            for (int j = i; j > 0; --j) {
                const unsigned int a = v[j - 1], b = v[j];
                v[j - 1] = min(a, b);
                v[j] = max(a, b);
            }
        }
        median_out[pixel] = static_cast<std::uint8_t>(v[kMedianFrameCount / 2]);  // 9th smallest
    }
    const unsigned int ballot = __ballot_sync(0xffffffffu, saturated);
    if ((threadIdx.x & 31u) == 0u && ballot != 0u) {
        atomicAdd(saturated_count, static_cast<unsigned long long>(__popc(ballot)));
    }
}

}  // namespace

cudaError_t LaunchBurstMedian17(const std::uint8_t* frames, std::size_t frame_stride_bytes, std::size_t pixel_count,
                                std::uint8_t* median_out, unsigned long long* saturated_count_out,
                                cudaStream_t stream)
{
    if (!frames || !median_out || !saturated_count_out || pixel_count == 0 || frame_stride_bytes < pixel_count) {
        return cudaErrorInvalidValue;
    }
    cudaError_t status = cudaMemsetAsync(saturated_count_out, 0, sizeof(unsigned long long), stream);
    if (status != cudaSuccess) return status;
    constexpr unsigned int kThreads = 256;  // a multiple of 32: every warp is full for the ballot
    const std::size_t blocks = (pixel_count + kThreads - 1) / kThreads;
    if (blocks > 0x7fffffffu) return cudaErrorInvalidValue;
    burst_median17_kernel<<<static_cast<unsigned int>(blocks), kThreads, 0, stream>>>(
        frames, frame_stride_bytes, pixel_count, median_out, saturated_count_out);
    return cudaGetLastError();
}

}  // namespace orange::empty_background
