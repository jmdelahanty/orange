// tools/detect_roi_tests.cpp
//
// Checks that the device-side crop-origin selection (src/detect_roi.cu)
// agrees field for field with an independent transcription of the existing
// CPU path: YOLOv8::postprocess (un-letterbox and clamp) followed by
// CropProducerWorker::ProcessEntryImpl (highest score, centre, clamp).
// The oracle below is written from those two functions, not from
// detect_roi.h, so a shared mistake cannot pass.
//
// Needs one CUDA device. Cameras are not touched.

#include "detect_roi.h"
#include "pose_crop_from_roi.h"

#include <cuda_runtime_api.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <string>
#include <vector>

namespace {

int g_failures = 0;

#define EXPECT_TRUE(cond, msg)                                                    \
    do {                                                                          \
        if (!(cond)) {                                                            \
            ++g_failures;                                                         \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, msg);   \
        }                                                                         \
    } while (0)

#define CUDA_OK(call)                                                             \
    do {                                                                          \
        const cudaError_t err__ = (call);                                         \
        if (err__ != cudaSuccess) {                                               \
            std::fprintf(stderr, "CUDA error %s at %s:%d: %s\n", #call, __FILE__, \
                         __LINE__, cudaGetErrorString(err__));                    \
            std::exit(2);                                                         \
        }                                                                         \
    } while (0)

struct Rect { float x, y, width, height; };
struct Object { Rect rect; int label; float prob; };

template <typename T>
T clampv(T v, T lo, T hi) { return std::min(std::max(v, lo), hi); }

// Transcribed from YOLOv8::postprocess (src/yolov8_det.cpp).
std::vector<Object> oracle_postprocess(const int* num_dets, const float* boxes, const float* scores,
                                       const int* labels, float dw, float dh, float inv_ratio,
                                       float original_img_w, float original_img_h, int binding_capacity)
{
    std::vector<Object> objs;
    int detections_to_process = num_dets[0];
    const int MAX_REASONABLE_DETS = 1000;
    if (detections_to_process < 0 || detections_to_process > MAX_REASONABLE_DETS) {
        detections_to_process = MAX_REASONABLE_DETS;
    }
    // postprocess itself would read past the binding here; the test buffers
    // are sized to the binding, so mirror the kernel's bound.
    detections_to_process = std::min(detections_to_process, binding_capacity);
    for (int i = 0; i < detections_to_process; i++) {
        const float* ptr = boxes + i * 4;
        float x0_letterboxed = *ptr++;
        float y0_letterboxed = *ptr++;
        float x1_letterboxed = *ptr++;
        float y1_letterboxed = *ptr;
        float x0_resized = x0_letterboxed - dw;
        float y0_resized = y0_letterboxed - dh;
        float x1_resized = x1_letterboxed - dw;
        float y1_resized = y1_letterboxed - dh;
        float x0_original = clampv(x0_resized * inv_ratio, 0.f, original_img_w);
        float y0_original = clampv(y0_resized * inv_ratio, 0.f, original_img_h);
        float x1_original = clampv(x1_resized * inv_ratio, 0.f, original_img_w);
        float y1_original = clampv(y1_resized * inv_ratio, 0.f, original_img_h);
        Object obj;
        obj.rect.x = x0_original;
        obj.rect.y = y0_original;
        obj.rect.width = x1_original - x0_original;
        obj.rect.height = y1_original - y0_original;
        obj.prob = scores[i];
        obj.label = labels[i];
        objs.push_back(obj);
    }
    return objs;
}

// Transcribed from orange::analytics_mask::evaluate_box_centroid
// (src/yolo_spatial_mask.h) and the in-place compaction in YoloWorker that
// drops detections outside the centroid gate when the mask policy enforces it.
struct OracleGate { bool enabled = false; float cx = 0.f, cy = 0.f, radius = 0.f; };

std::vector<Object> oracle_gate_filter(const std::vector<Object>& detections, const OracleGate& gate)
{
    if (!gate.enabled) return detections;
    std::vector<Object> kept;
    for (const Object& detection : detections) {
        const float centroid_x = detection.rect.x + detection.rect.width * 0.5f;
        const float centroid_y = detection.rect.y + detection.rect.height * 0.5f;
        const float dx = centroid_x - gate.cx;
        const float dy = centroid_y - gate.cy;
        const float distance = std::sqrt(dx * dx + dy * dy);
        const float signed_boundary_distance_px = gate.radius - distance;
        const bool inside = signed_boundary_distance_px >= 0.0f;
        if (inside) kept.push_back(detection);
    }
    return kept;
}

// Transcribed from CropProducerWorker::ProcessEntryImpl.
struct OracleCrop { bool has_detection; int ix, iy; float prob; int label; Rect rect; };

OracleCrop oracle_crop(const std::vector<Object>& detections, int width, int height, int crop_width, int crop_height)
{
    OracleCrop c{};
    c.has_detection = !detections.empty();
    if (!c.has_detection) return c;
    const Object best_detection = *std::max_element(
        detections.begin(), detections.end(),
        [](const Object& a, const Object& b) { return a.prob < b.prob; });
    const float cx = best_detection.rect.x + best_detection.rect.width * 0.5f;
    const float cy = best_detection.rect.y + best_detection.rect.height * 0.5f;
    c.ix = std::clamp(static_cast<int>(cx) - crop_width / 2, 0, width - crop_width);
    c.iy = std::clamp(static_cast<int>(cy) - crop_height / 2, 0, height - crop_height);
    c.prob = best_detection.prob;
    c.label = best_detection.label;
    c.rect = best_detection.rect;
    return c;
}

struct Case {
    std::string name;
    int src_w, src_h, inp_w, inp_h, crop;
    int pose_crop = 0;  // 0 = single crop
    OracleGate gate;    // spatial-mask centroid gate (off by default)
    int capacity;
    int num_dets;
    std::vector<float> boxes;   // capacity * 4
    std::vector<float> scores;  // capacity
    std::vector<int> labels;    // capacity
};

DetectRoiParams params_for(const Case& c, float* dw_out, float* dh_out, float* inv_out)
{
    // YOLOv8::preprocess_gpu letterbox arithmetic.
    const float r = std::min(static_cast<float>(c.inp_h) / c.src_h, static_cast<float>(c.inp_w) / c.src_w);
    DetectRoiParams p;
    p.inv_ratio = 1.0f / r;
    p.dw = (c.inp_w - c.src_w * r) / 2.0f;
    p.dh = (c.inp_h - c.src_h * r) / 2.0f;
    p.src_w = static_cast<float>(c.src_w);
    p.src_h = static_cast<float>(c.src_h);
    p.src_w_int = c.src_w;
    p.src_h_int = c.src_h;
    p.crop_w = c.crop;
    p.crop_h = c.crop;
    p.pose_crop_w = c.pose_crop;
    p.pose_crop_h = c.pose_crop;
    p.max_dets = c.capacity;
    p.centroid_gate = c.gate.enabled ? 1 : 0;
    p.gate_cx = c.gate.cx;
    p.gate_cy = c.gate.cy;
    p.gate_radius = c.gate.radius;
    *dw_out = p.dw; *dh_out = p.dh; *inv_out = p.inv_ratio;
    return p;
}

bool same_float(float a, float b) { return std::memcmp(&a, &b, sizeof(float)) == 0 || a == b; }

void check_case(const Case& c, cudaStream_t stream)
{
    float dw, dh, inv;
    const DetectRoiParams p = params_for(c, &dw, &dh, &inv);

    // Oracle.
    const std::vector<Object> raw_objs = oracle_postprocess(&c.num_dets, c.boxes.data(), c.scores.data(),
                                                            c.labels.data(), dw, dh, inv, p.src_w, p.src_h, c.capacity);
    const std::vector<Object> objs = oracle_gate_filter(raw_objs, c.gate);
    const int oracle_gated = static_cast<int>(raw_objs.size() - objs.size());
    const OracleCrop oc = oracle_crop(objs, c.src_w, c.src_h, c.crop, c.crop);
    const int pose_size = c.pose_crop > 0 ? c.pose_crop : c.crop;
    const OracleCrop op = oracle_crop(objs, c.src_w, c.src_h, pose_size, pose_size);

    // Host reference.
    const DetectRoi host = compute_detect_roi_host(&c.num_dets, c.boxes.data(), c.scores.data(), c.labels.data(), p);

    // Device.
    int* d_num = nullptr; float* d_boxes = nullptr; float* d_scores = nullptr; int* d_labels = nullptr; DetectRoi* d_out = nullptr;
    CUDA_OK(cudaMalloc(reinterpret_cast<void**>(&d_num), sizeof(int)));
    CUDA_OK(cudaMalloc(reinterpret_cast<void**>(&d_boxes), c.boxes.size() * sizeof(float)));
    CUDA_OK(cudaMalloc(reinterpret_cast<void**>(&d_scores), c.scores.size() * sizeof(float)));
    CUDA_OK(cudaMalloc(reinterpret_cast<void**>(&d_labels), c.labels.size() * sizeof(int)));
    CUDA_OK(cudaMalloc(reinterpret_cast<void**>(&d_out), sizeof(DetectRoi)));
    CUDA_OK(cudaMemcpyAsync(d_num, &c.num_dets, sizeof(int), cudaMemcpyHostToDevice, stream));
    CUDA_OK(cudaMemcpyAsync(d_boxes, c.boxes.data(), c.boxes.size() * sizeof(float), cudaMemcpyHostToDevice, stream));
    CUDA_OK(cudaMemcpyAsync(d_scores, c.scores.data(), c.scores.size() * sizeof(float), cudaMemcpyHostToDevice, stream));
    CUDA_OK(cudaMemcpyAsync(d_labels, c.labels.data(), c.labels.size() * sizeof(int), cudaMemcpyHostToDevice, stream));
    CUDA_OK(cudaMemsetAsync(d_out, 0xCD, sizeof(DetectRoi), stream));
    launch_detect_roi_kernel(d_num, d_boxes, d_scores, d_labels, d_out, p, stream);
    CUDA_OK(cudaGetLastError());
    DetectRoi dev;
    CUDA_OK(cudaMemcpyAsync(&dev, d_out, sizeof(DetectRoi), cudaMemcpyDeviceToHost, stream));
    CUDA_OK(cudaStreamSynchronize(stream));
    CUDA_OK(cudaFree(d_num)); CUDA_OK(cudaFree(d_boxes)); CUDA_OK(cudaFree(d_scores)); CUDA_OK(cudaFree(d_labels)); CUDA_OK(cudaFree(d_out));

    auto report = [&](const char* what, const DetectRoi& r) {
        char msg[512];
        std::snprintf(msg, sizeof msg,
                      "%s [%s]: oracle has=%d ix=%d iy=%d prob=%.9g label=%d rect=(%.9g,%.9g,%.9g,%.9g) | got valid=%d crop=(%d,%d) score=%.9g label=%d box=(%.9g,%.9g,%.9g,%.9g) n=%d best=%d",
                      what, c.name.c_str(), oc.has_detection ? 1 : 0, oc.ix, oc.iy, oc.prob, oc.label,
                      oc.rect.x, oc.rect.y, oc.rect.width, oc.rect.height,
                      r.valid, r.crop_x, r.crop_y, r.score, r.label, r.box_x, r.box_y, r.box_w, r.box_h, r.num_dets, r.best_index);
        return std::string(msg);
    };

    const DetectRoi* results[2] = {&host, &dev};
    for (const DetectRoi* pair : results) {
        const DetectRoi& r = *pair;
        const char* what = (pair == &host) ? "host" : "device";
        EXPECT_TRUE((r.valid == 1) == oc.has_detection, report(what, r).c_str());
        EXPECT_TRUE(r.crop_w == c.crop && r.crop_h == c.crop, report(what, r).c_str());
        EXPECT_TRUE(r.num_gated == oracle_gated, report(what, r).c_str());
        if (!oc.has_detection) continue;
        EXPECT_TRUE(r.crop_x == oc.ix && r.crop_y == oc.iy, report(what, r).c_str());
        EXPECT_TRUE(r.pose_crop_w == pose_size && r.pose_crop_h == pose_size, report(what, r).c_str());
        EXPECT_TRUE(r.pose_crop_x == op.ix && r.pose_crop_y == op.iy, report(what, r).c_str());
        EXPECT_TRUE(same_float(r.score, oc.prob), report(what, r).c_str());
        EXPECT_TRUE(r.label == oc.label, report(what, r).c_str());
        EXPECT_TRUE(same_float(r.box_x, oc.rect.x) && same_float(r.box_y, oc.rect.y) &&
                    same_float(r.box_w, oc.rect.width) && same_float(r.box_h, oc.rect.height),
                    report(what, r).c_str());
    }
    // Device and host reference must be bit-identical to each other too.
    EXPECT_TRUE(std::memcmp(&host, &dev, sizeof(DetectRoi)) == 0, report("host-vs-device", dev).c_str());
}

Case make_case(const std::string& name, int src_w, int src_h, int inp, int crop, int capacity, int num_dets, std::mt19937& rng,
               bool edge_boxes = false)
{
    Case c;
    c.name = name; c.src_w = src_w; c.src_h = src_h; c.inp_w = inp; c.inp_h = inp; c.crop = crop;
    c.capacity = capacity; c.num_dets = num_dets;
    c.boxes.assign(capacity * 4, 0.0f);
    c.scores.assign(capacity, 0.0f);
    c.labels.assign(capacity, 0);
    std::uniform_real_distribution<float> coord(edge_boxes ? -40.0f : 0.0f, edge_boxes ? inp + 40.0f : static_cast<float>(inp));
    std::uniform_real_distribution<float> score(0.05f, 0.99f);
    std::uniform_int_distribution<int> label(0, 3);
    for (int i = 0; i < capacity; ++i) {
        float a = coord(rng), b = coord(rng), x = coord(rng), y = coord(rng);
        c.boxes[i * 4 + 0] = std::min(a, x); c.boxes[i * 4 + 2] = std::max(a, x);
        c.boxes[i * 4 + 1] = std::min(b, y); c.boxes[i * 4 + 3] = std::max(b, y);
        c.scores[i] = score(rng);
        c.labels[i] = label(rng);
    }
    return c;
}

// Device crop against the host reference, for a synthetic mono frame.
void check_crop(const char* name, int src_w, int src_h, const DetectRoi& roi, int crop_w, int crop_h,
                std::mt19937& rng, cudaStream_t stream)
{
    std::vector<unsigned char> src(static_cast<size_t>(src_w) * src_h);
    std::uniform_int_distribution<int> byte(0, 255);
    for (unsigned char& v : src) v = static_cast<unsigned char>(byte(rng));
    std::vector<unsigned char> expected(static_cast<size_t>(crop_w) * crop_h, 0xAA);
    pose_crop_from_roi_host(src.data(), src_w, src_w, src_h, roi, expected.data(), crop_w, crop_h);

    unsigned char* d_src = nullptr; unsigned char* d_dst = nullptr; DetectRoi* d_roi = nullptr;
    CUDA_OK(cudaMalloc(reinterpret_cast<void**>(&d_src), src.size()));
    CUDA_OK(cudaMalloc(reinterpret_cast<void**>(&d_dst), expected.size()));
    CUDA_OK(cudaMalloc(reinterpret_cast<void**>(&d_roi), sizeof(DetectRoi)));
    CUDA_OK(cudaMemcpyAsync(d_src, src.data(), src.size(), cudaMemcpyHostToDevice, stream));
    CUDA_OK(cudaMemcpyAsync(d_roi, &roi, sizeof(DetectRoi), cudaMemcpyHostToDevice, stream));
    CUDA_OK(cudaMemsetAsync(d_dst, 0xAA, expected.size(), stream));
    launch_pose_crop_from_roi(d_src, src_w, src_w, src_h, d_roi, d_dst, crop_w, crop_h, stream);
    CUDA_OK(cudaGetLastError());
    std::vector<unsigned char> got(expected.size());
    CUDA_OK(cudaMemcpyAsync(got.data(), d_dst, got.size(), cudaMemcpyDeviceToHost, stream));
    CUDA_OK(cudaStreamSynchronize(stream));
    CUDA_OK(cudaFree(d_src)); CUDA_OK(cudaFree(d_dst)); CUDA_OK(cudaFree(d_roi));

    char msg[256];
    std::snprintf(msg, sizeof msg, "crop %s: device output differs from host reference (valid=%d origin=%d,%d size=%dx%d)",
                  name, roi.valid, roi.pose_crop_x, roi.pose_crop_y, crop_w, crop_h);
    EXPECT_TRUE(got == expected, msg);
    // Sanity on the reference itself: an interior crop is a straight copy.
    if (roi.valid && roi.pose_crop_x >= 0 && roi.pose_crop_y >= 0 &&
        roi.pose_crop_x + crop_w <= src_w && roi.pose_crop_y + crop_h <= src_h) {
        bool straight = true;
        for (int y = 0; y < crop_h && straight; ++y)
            for (int x = 0; x < crop_w; ++x)
                if (expected[static_cast<size_t>(y) * crop_w + x] !=
                    src[static_cast<size_t>(roi.pose_crop_y + y) * src_w + roi.pose_crop_x + x]) { straight = false; break; }
        std::snprintf(msg, sizeof msg, "crop %s: host reference is not a straight copy", name);
        EXPECT_TRUE(straight, msg);
    }
}

}  // namespace

int main()
{
    int device_count = 0;
    if (cudaGetDeviceCount(&device_count) != cudaSuccess || device_count == 0) {
        std::fprintf(stderr, "detect_roi_tests: no CUDA device available\n");
        return 2;
    }
    cudaStream_t stream = nullptr;
    CUDA_OK(cudaStreamCreateWithFlags(&stream, cudaStreamNonBlocking));

    std::mt19937 rng(20260912);
    int cases = 0;

    // Frame geometries: the rig's 4512 square (no padding), a landscape
    // source (vertical padding), a portrait source (horizontal padding), a
    // small sensor, and a 1024 square.
    struct Geo { int w, h; } geos[] = {{4512, 4512}, {4512, 3000}, {3000, 4512}, {1920, 1080}, {1024, 1024}};
    const int crops[] = {96, 128, 256, 384};
    const int counts[] = {0, 1, 2, 3, 7, 100};

    for (const Geo& g : geos) {
        for (int crop : crops) {
            if (crop > std::min(g.w, g.h)) continue;
            for (int n : counts) {
                for (int rep = 0; rep < 8; ++rep) {
                    const bool edge = (rep % 2) == 1;
                    Case c = make_case("geo" + std::to_string(g.w) + "x" + std::to_string(g.h) + "_crop" + std::to_string(crop) +
                                       "_n" + std::to_string(n) + (edge ? "_edge" : ""), g.w, g.h, 640, crop, 100, n, rng, edge);
                    check_case(c, stream); ++cases;
                    // Same inputs with a head-sized pose crop alongside the video crop.
                    Case c2 = c; c2.name += "_pose" ; c2.pose_crop = std::max(32, crop / 2);
                    check_case(c2, stream); ++cases;
                }
            }
        }
    }

    // Ties: equal top scores at indices 0 and 2, the first must win.
    {
        Case c = make_case("ties", 4512, 4512, 640, 256, 100, 5, rng);
        c.scores[0] = 0.7f; c.scores[1] = 0.1f; c.scores[2] = 0.7f; c.scores[3] = 0.69999f; c.scores[4] = 0.2f;
        check_case(c, stream); ++cases;
        Case d = c; d.name = "ties_best_not_first"; d.scores[0] = 0.3f;
        check_case(d, stream); ++cases;
    }
    // Corrupt count: postprocess clamps to 1000, the kernel then to capacity.
    {
        Case c = make_case("corrupt_count", 4512, 4512, 640, 256, 100, 123456789, rng);
        check_case(c, stream); ++cases;
        Case d = make_case("negative_count", 4512, 4512, 640, 256, 100, -3, rng);
        check_case(d, stream); ++cases;
    }
    // Boxes that force each clamp branch of the crop origin.
    {
        Case c = make_case("corner_boxes", 4512, 4512, 640, 256, 100, 4, rng);
        const float b[4][4] = {{0, 0, 5, 5}, {635, 635, 640, 640}, {0, 635, 5, 640}, {635, 0, 640, 5}};
        for (int i = 0; i < 4; ++i) for (int k = 0; k < 4; ++k) c.boxes[i * 4 + k] = b[i][k];
        for (int i = 0; i < 4; ++i) { Case d = c; d.name = "corner_box_" + std::to_string(i); d.scores[i] = 0.99f; check_case(d, stream); ++cases; }
    }
    // Integer-boundary centres: cx exactly on .0 and just below, where an
    // FMA contraction would move the truncation.
    {
        Case c = make_case("integer_boundary", 4512, 4512, 640, 256, 100, 1, rng);
        const float inv = 4512.0f / 640.0f;
        for (int k = 0; k < 64; ++k) {
            const float centre_src = 1000.0f + k;                      // target source centre
            const float half_w_src = 17.0f + 0.25f * k;
            const float x0 = (centre_src - half_w_src) / inv;          // back to letterboxed input
            const float x1 = (centre_src + half_w_src) / inv;
            c.boxes[0] = x0; c.boxes[1] = 100.0f; c.boxes[2] = x1; c.boxes[3] = 120.0f;
            c.scores[0] = 0.9f;
            c.name = "integer_boundary_" + std::to_string(k);
            check_case(c, stream); ++cases;
        }
    }

    // Spatial-mask centroid gate: the device must select the same box the CPU
    // selects after dropping detections whose centroid is outside the circle.
    {
        std::uniform_real_distribution<float> radius_frac(0.05f, 0.7f);
        std::uniform_real_distribution<float> centre_frac(0.2f, 0.8f);
        for (const Geo& g : geos) {
            for (int rep = 0; rep < 24; ++rep) {
                const int n = counts[rep % 6];
                Case c = make_case("gate_geo" + std::to_string(g.w) + "x" + std::to_string(g.h) + "_rep" + std::to_string(rep),
                                   g.w, g.h, 640, 256, 100, n, rng, (rep % 3) == 1);
                c.gate.enabled = true;
                c.gate.cx = g.w * centre_frac(rng);
                c.gate.cy = g.h * centre_frac(rng);
                c.gate.radius = std::min(g.w, g.h) * radius_frac(rng);
                check_case(c, stream); ++cases;
                Case p = c; p.name += "_pose"; p.pose_crop = 128;
                check_case(p, stream); ++cases;
            }
        }
        // Boundary: centroids placed exactly on and just off the circle along
        // the x axis, where the >= 0 test decides.
        Case c = make_case("gate_boundary", 4512, 4512, 640, 256, 100, 1, rng);
        c.gate.enabled = true; c.gate.cx = 2256.0f; c.gate.cy = 2256.0f; c.gate.radius = 900.0f;
        const float inv = 4512.0f / 640.0f;
        for (int k = -8; k <= 8; ++k) {
            const float centre_src = 2256.0f + 900.0f + 0.25f * k;
            const float half_w_src = 12.0f;
            c.boxes[0] = (centre_src - half_w_src) / inv; c.boxes[1] = (2256.0f - 10.0f) / inv;
            c.boxes[2] = (centre_src + half_w_src) / inv; c.boxes[3] = (2256.0f + 10.0f) / inv;
            c.scores[0] = 0.9f;
            c.name = "gate_boundary_" + std::to_string(k + 8);
            check_case(c, stream); ++cases;
        }
        // Best box outside the gate, second best inside: the second must win.
        Case d = make_case("gate_best_outside", 4512, 4512, 640, 256, 100, 3, rng);
        d.gate.enabled = true; d.gate.cx = 2256.0f; d.gate.cy = 2256.0f; d.gate.radius = 500.0f;
        const float b[3][4] = {{10, 10, 30, 30}, {310, 310, 330, 330}, {600, 600, 630, 630}};
        for (int i = 0; i < 3; ++i) for (int k = 0; k < 4; ++k) d.boxes[i * 4 + k] = b[i][k];
        d.scores[0] = 0.95f; d.scores[1] = 0.6f; d.scores[2] = 0.7f;
        check_case(d, stream); ++cases;
        // Everything outside: no valid ROI, all counted as gated.
        Case e = d; e.name = "gate_all_outside"; e.gate.radius = 5.0f;
        check_case(e, stream); ++cases;
    }

    // Pose crop kernel: interior, every edge clamp, invalid ROI, and a crop
    // larger than the source (pinned origin, zero fill outside).
    {
        std::uniform_int_distribution<int> pick(0, 1000000);
        for (int rep = 0; rep < 40; ++rep) {
            const int src_w = 640 + (pick(rng) % 400);
            const int src_h = 480 + (pick(rng) % 400);
            const int crop = (rep % 4 == 0) ? 96 : (rep % 4 == 1) ? 128 : (rep % 4 == 2) ? 192 : 256;
            DetectRoi roi;
            roi.valid = 1;
            roi.pose_crop_w = crop; roi.pose_crop_h = crop;
            roi.pose_crop_x = pick(rng) % std::max(1, src_w - crop + 1);
            roi.pose_crop_y = pick(rng) % std::max(1, src_h - crop + 1);
            if (rep % 5 == 1) { roi.pose_crop_x = 0; roi.pose_crop_y = 0; }
            if (rep % 5 == 2) { roi.pose_crop_x = src_w - crop; roi.pose_crop_y = src_h - crop; }
            if (rep % 5 == 3) { roi.valid = 0; }
            check_crop(("rep" + std::to_string(rep)).c_str(), src_w, src_h, roi, crop, crop, rng, stream); ++cases;
        }
        DetectRoi big; big.valid = 1; big.pose_crop_x = 0; big.pose_crop_y = 0; big.pose_crop_w = 256; big.pose_crop_h = 256;
        check_crop("larger_than_source", 200, 150, big, 256, 256, rng, stream); ++cases;
        DetectRoi neg = big; neg.pose_crop_x = -17; neg.pose_crop_y = -3;
        check_crop("negative_origin", 640, 480, neg, 256, 256, rng, stream); ++cases;
    }

    CUDA_OK(cudaStreamDestroy(stream));
    if (g_failures == 0) {
        std::printf("detect_roi_tests: %d cases passed\n", cases);
        return 0;
    }
    std::fprintf(stderr, "detect_roi_tests: %d failures in %d cases\n", g_failures, cases);
    return 1;
}
