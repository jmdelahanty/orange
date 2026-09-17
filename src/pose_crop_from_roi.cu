// src/pose_crop_from_roi.cu
#include "pose_crop_from_roi.h"
#include "fused_frame_args.h"

namespace {

__global__ void pose_crop_from_roi_kernel(
    const unsigned char* __restrict__ d_src,
    int src_pitch,
    int src_w,
    int src_h,
    const DetectRoi* __restrict__ d_roi,
    unsigned char* __restrict__ d_dst_mono,
    int crop_w,
    int crop_h)
{
    const int dst_x = blockIdx.x * blockDim.x + threadIdx.x;
    const int dst_y = blockIdx.y * blockDim.y + threadIdx.y;
    if (dst_x >= crop_w || dst_y >= crop_h) {
        return;
    }
    // The ROI is 60-odd bytes read by every thread; it stays in L1.
    const int valid = d_roi->valid;
    const int origin_x = d_roi->pose_crop_x;
    const int origin_y = d_roi->pose_crop_y;
    unsigned char value = 0;
    if (valid) {
        const int src_x = origin_x + dst_x;
        const int src_y = origin_y + dst_y;
        if (src_x >= 0 && src_x < src_w && src_y >= 0 && src_y < src_h) {
            value = d_src[static_cast<size_t>(src_y) * static_cast<size_t>(src_pitch) + src_x];
        }
    }
    d_dst_mono[static_cast<size_t>(dst_y) * static_cast<size_t>(crop_w) + dst_x] = value;
}

__global__ void pose_crop_from_roi_indirect_kernel(
    const FusedFrameArgs* __restrict__ args,
    const DetectRoi* __restrict__ d_roi,
    unsigned char* __restrict__ d_dst_mono,
    int crop_w,
    int crop_h)
{
    const int dst_x = blockIdx.x * blockDim.x + threadIdx.x;
    const int dst_y = blockIdx.y * blockDim.y + threadIdx.y;
    if (dst_x >= crop_w || dst_y >= crop_h) {
        return;
    }
    const unsigned char* d_src = args->src_frame;
    const int src_pitch = args->src_pitch;
    const int src_w = args->src_width;
    const int src_h = args->src_height;
    const int valid = d_roi->valid;
    const int origin_x = d_roi->pose_crop_x;
    const int origin_y = d_roi->pose_crop_y;
    unsigned char value = 0;
    if (valid && d_src) {
        const int src_x = origin_x + dst_x;
        const int src_y = origin_y + dst_y;
        if (src_x >= 0 && src_x < src_w && src_y >= 0 && src_y < src_h) {
            value = d_src[static_cast<size_t>(src_y) * static_cast<size_t>(src_pitch) + src_x];
        }
    }
    d_dst_mono[static_cast<size_t>(dst_y) * static_cast<size_t>(crop_w) + dst_x] = value;
}

// Grid-stride copy in 16-byte units with a byte tail; the sizes and
// addresses come from the argument block so the launch is capturable.
__global__ void indirect_copy_kernel(const FusedFrameArgs* __restrict__ args)
{
    const unsigned long long bytes = args->copy_bytes;
    if (bytes == 0) {
        return;
    }
    const unsigned char* src = args->copy_src;
    unsigned char* dst = args->copy_dst;
    const unsigned long long stride = static_cast<unsigned long long>(gridDim.x) * blockDim.x;
    const unsigned long long tid = static_cast<unsigned long long>(blockIdx.x) * blockDim.x + threadIdx.x;
    const bool aligned = ((reinterpret_cast<unsigned long long>(src) | reinterpret_cast<unsigned long long>(dst)) & 15ull) == 0;
    if (aligned) {
        const unsigned long long vecs = bytes / 16ull;
        const uint4* s4 = reinterpret_cast<const uint4*>(src);
        uint4* d4 = reinterpret_cast<uint4*>(dst);
        for (unsigned long long i = tid; i < vecs; i += stride) {
            d4[i] = s4[i];
        }
        for (unsigned long long i = vecs * 16ull + tid; i < bytes; i += stride) {
            dst[i] = src[i];
        }
    } else {
        for (unsigned long long i = tid; i < bytes; i += stride) {
            dst[i] = src[i];
        }
    }
}

}  // namespace

void launch_pose_crop_from_roi_indirect(
    const FusedFrameArgs* d_args,
    const DetectRoi* d_roi,
    unsigned char* d_dst_mono,
    int crop_w,
    int crop_h,
    cudaStream_t stream)
{
    if (crop_w <= 0 || crop_h <= 0) {
        return;
    }
    const dim3 block(32, 8);
    const dim3 grid((crop_w + block.x - 1) / block.x, (crop_h + block.y - 1) / block.y);
    pose_crop_from_roi_indirect_kernel<<<grid, block, 0, stream>>>(d_args, d_roi, d_dst_mono, crop_w, crop_h);
}

void launch_indirect_copy(const FusedFrameArgs* d_args, cudaStream_t stream)
{
    // 20 MB at 16 bytes per thread-iteration: 1024 blocks of 256 threads
    // keep every SM of a GA107 die busy with a few iterations each.
    indirect_copy_kernel<<<1024, 256, 0, stream>>>(d_args);
}

void launch_pose_crop_from_roi(
    const unsigned char* d_src,
    int src_pitch,
    int src_w,
    int src_h,
    const DetectRoi* d_roi,
    unsigned char* d_dst_mono,
    int crop_w,
    int crop_h,
    cudaStream_t stream)
{
    if (crop_w <= 0 || crop_h <= 0) {
        return;
    }
    const dim3 block(32, 8);
    const dim3 grid((crop_w + block.x - 1) / block.x, (crop_h + block.y - 1) / block.y);
    pose_crop_from_roi_kernel<<<grid, block, 0, stream>>>(
        d_src, src_pitch, src_w, src_h, d_roi, d_dst_mono, crop_w, crop_h);
}

void pose_crop_from_roi_host(
    const unsigned char* src,
    int src_pitch,
    int src_w,
    int src_h,
    const DetectRoi& roi,
    unsigned char* dst_mono,
    int crop_w,
    int crop_h)
{
    for (int y = 0; y < crop_h; ++y) {
        for (int x = 0; x < crop_w; ++x) {
            unsigned char value = 0;
            if (roi.valid) {
                const int sx = roi.pose_crop_x + x;
                const int sy = roi.pose_crop_y + y;
                if (sx >= 0 && sx < src_w && sy >= 0 && sy < src_h) {
                    value = src[static_cast<size_t>(sy) * static_cast<size_t>(src_pitch) + sx];
                }
            }
            dst_mono[static_cast<size_t>(y) * static_cast<size_t>(crop_w) + x] = value;
        }
    }
}
