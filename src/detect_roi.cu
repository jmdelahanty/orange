// src/detect_roi.cu
#include "detect_roi.h"

namespace {

// Shared body for device and host. The device instantiation uses the
// round-to-nearest intrinsics so nvcc cannot contract the arithmetic; the
// host instantiation uses plain float operations, which is exactly what
// YOLOv8::postprocess and CropProducerWorker::ProcessEntryImpl compile to.
struct HostOps {
    // Marked __host__ __device__ only so the shared template body compiles
    // for both instantiations; the host instantiation is the one that runs.
    __host__ __device__ static inline float add(float a, float b) { return a + b; }
    __host__ __device__ static inline float sub(float a, float b) { return a - b; }
    __host__ __device__ static inline float mul(float a, float b) { return a * b; }
    __host__ __device__ static inline float sqrt(float a) { return sqrtf(a); }
    __host__ __device__ static inline float clampf(float v, float lo, float hi) { return (v < lo) ? lo : ((v > hi) ? hi : v); }
    __host__ __device__ static inline int clampi(int v, int lo, int hi) { return (v < lo) ? lo : ((v > hi) ? hi : v); }
};

#ifdef __CUDACC__
struct DeviceOps {
    __device__ static inline float add(float a, float b) { return __fadd_rn(a, b); }
    __device__ static inline float sub(float a, float b) { return __fsub_rn(a, b); }
    __device__ static inline float mul(float a, float b) { return __fmul_rn(a, b); }
    __device__ static inline float sqrt(float a) { return __fsqrt_rn(a); }
    __device__ static inline float clampf(float v, float lo, float hi) { return fminf(fmaxf(v, lo), hi); }
    __device__ static inline int clampi(int v, int lo, int hi) { return min(max(v, lo), hi); }
};
#endif

template <typename Ops>
__host__ __device__ inline void select_detect_roi(
    const int* num_dets,
    const float* boxes,
    const float* scores,
    const int* labels,
    const DetectRoiParams& p,
    DetectRoi* out)
{
    DetectRoi r;
    r.crop_w = p.crop_w;
    r.crop_h = p.crop_h;
    const int pw = p.pose_crop_w > 0 ? p.pose_crop_w : p.crop_w;
    const int ph = p.pose_crop_h > 0 ? p.pose_crop_h : p.crop_h;
    r.pose_crop_w = pw;
    r.pose_crop_h = ph;

    // YOLOv8::postprocess: clamp an unreasonable count; then never read past
    // the binding (postprocess trusts the count up to 1000; the bindings are
    // smaller, so the extra bound here only matters for a corrupt count).
    int n = num_dets[0];
    if (n < 0 || n > kDetectRoiMaxReasonableDets) {
        n = kDetectRoiMaxReasonableDets;
    }
    if (n > p.max_dets) {
        n = p.max_dets;
    }
    r.num_dets = n;

    // CropProducerWorker: std::max_element with a.prob < b.prob, which keeps
    // the first of equal maxima.
    int best = -1;
    float best_prob = 0.0f;
    float bx = 0.0f, by = 0.0f, bw = 0.0f, bh = 0.0f;
    int blabel = 0;
    for (int i = 0; i < n; ++i) {
        const float* b = boxes + i * 4;
        const float x0 = Ops::clampf(Ops::mul(Ops::sub(b[0], p.dw), p.inv_ratio), 0.0f, p.src_w);
        const float y0 = Ops::clampf(Ops::mul(Ops::sub(b[1], p.dh), p.inv_ratio), 0.0f, p.src_h);
        const float x1 = Ops::clampf(Ops::mul(Ops::sub(b[2], p.dw), p.inv_ratio), 0.0f, p.src_w);
        const float y1 = Ops::clampf(Ops::mul(Ops::sub(b[3], p.dh), p.inv_ratio), 0.0f, p.src_h);
        if (p.centroid_gate) {
            // orange::analytics_mask::evaluate_box_centroid on the
            // un-letterboxed, clamped rect, same operation order.
            const float w = Ops::sub(x1, x0);
            const float h = Ops::sub(y1, y0);
            const float gcx = Ops::add(x0, Ops::mul(w, 0.5f));
            const float gcy = Ops::add(y0, Ops::mul(h, 0.5f));
            const float dx = Ops::sub(gcx, p.gate_cx);
            const float dy = Ops::sub(gcy, p.gate_cy);
            const float distance = Ops::sqrt(Ops::add(Ops::mul(dx, dx), Ops::mul(dy, dy)));
            const float signed_boundary_distance = Ops::sub(p.gate_radius, distance);
            if (!(signed_boundary_distance >= 0.0f)) {
                ++r.num_gated;
                continue;
            }
        }
        const float prob = scores[i];
        if (best < 0 || prob > best_prob) {
            best = i;
            best_prob = prob;
            bx = x0;
            by = y0;
            bw = Ops::sub(x1, x0);
            bh = Ops::sub(y1, y0);
            blabel = labels[i];
        }
    }

    if (best < 0) {
        r.valid = 0;
        *out = r;
        return;
    }

    // CropProducerWorker::ProcessEntryImpl: centre minus half the crop,
    // clamped so the crop stays inside the frame.
    const float cx = Ops::add(bx, Ops::mul(bw, 0.5f));
    const float cy = Ops::add(by, Ops::mul(bh, 0.5f));
    r.valid = 1;
    r.best_index = best;
    r.score = best_prob;
    r.label = blabel;
    r.box_x = bx;
    r.box_y = by;
    r.box_w = bw;
    r.box_h = bh;
    // A crop larger than the frame has no valid origin range; the CPU path
    // drops such frames before the clamp. Pin the origin at 0 instead of
    // clamping to a negative bound (the crop kernel bounds-checks its reads).
    const int hi_x = p.src_w_int - p.crop_w > 0 ? p.src_w_int - p.crop_w : 0;
    const int hi_y = p.src_h_int - p.crop_h > 0 ? p.src_h_int - p.crop_h : 0;
    const int hi_px = p.src_w_int - pw > 0 ? p.src_w_int - pw : 0;
    const int hi_py = p.src_h_int - ph > 0 ? p.src_h_int - ph : 0;
    r.crop_x = Ops::clampi(static_cast<int>(cx) - p.crop_w / 2, 0, hi_x);
    r.crop_y = Ops::clampi(static_cast<int>(cy) - p.crop_h / 2, 0, hi_y);
    r.pose_crop_x = Ops::clampi(static_cast<int>(cx) - pw / 2, 0, hi_px);
    r.pose_crop_y = Ops::clampi(static_cast<int>(cy) - ph / 2, 0, hi_py);
    *out = r;
}

__global__ void detect_roi_kernel(
    const int* num_dets,
    const float* boxes,
    const float* scores,
    const int* labels,
    DetectRoiParams p,
    DetectRoi* out)
{
    if (threadIdx.x == 0 && blockIdx.x == 0) {
        select_detect_roi<DeviceOps>(num_dets, boxes, scores, labels, p, out);
    }
}

}  // namespace

void launch_detect_roi_kernel(
    const int* d_num_dets,
    const float* d_boxes,
    const float* d_scores,
    const int* d_labels,
    DetectRoi* d_out,
    const DetectRoiParams& params,
    cudaStream_t stream)
{
    detect_roi_kernel<<<1, 1, 0, stream>>>(d_num_dets, d_boxes, d_scores, d_labels, params, d_out);
}

DetectRoi compute_detect_roi_host(
    const int* num_dets,
    const float* boxes,
    const float* scores,
    const int* labels,
    const DetectRoiParams& params)
{
    DetectRoi out;
    select_detect_roi<HostOps>(num_dets, boxes, scores, labels, params, &out);
    return out;
}
