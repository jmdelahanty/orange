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
    p.max_dets = c.capacity;
    *dw_out = p.dw; *dh_out = p.dh; *inv_out = p.inv_ratio;
    return p;
}

bool same_float(float a, float b) { return std::memcmp(&a, &b, sizeof(float)) == 0 || a == b; }

void check_case(const Case& c, cudaStream_t stream)
{
    float dw, dh, inv;
    const DetectRoiParams p = params_for(c, &dw, &dh, &inv);

    // Oracle.
    const std::vector<Object> objs = oracle_postprocess(&c.num_dets, c.boxes.data(), c.scores.data(),
                                                        c.labels.data(), dw, dh, inv, p.src_w, p.src_h, c.capacity);
    const OracleCrop oc = oracle_crop(objs, c.src_w, c.src_h, c.crop, c.crop);

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
        if (!oc.has_detection) continue;
        EXPECT_TRUE(r.crop_x == oc.ix && r.crop_y == oc.iy, report(what, r).c_str());
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

    CUDA_OK(cudaStreamDestroy(stream));
    if (g_failures == 0) {
        std::printf("detect_roi_tests: %d cases passed\n", cases);
        return 0;
    }
    std::fprintf(stderr, "detect_roi_tests: %d failures in %d cases\n", g_failures, cases);
    return 1;
}
