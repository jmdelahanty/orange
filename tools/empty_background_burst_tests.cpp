// Empty-dish background burst on a GPU (src/empty_background_burst_worker.h,
// src/empty_background_median.cu). Runs on device 0 with small synthetic frames
// (never a camera die on the rig: GPU 0 is the RTX A6000). Exit 77 (skip) without a GPU.
//
//  - the median kernel equals the CPU reference MedianOfBurst exactly (median and
//    saturated count), on random bursts of an odd pixel count;
//  - a burst of 17 analytics-owned frames, and of 17 pool ring copies (detection
//    off): the median, the source frames in stream order, the minimum free pool
//    counts, every lease released exactly once, nothing beyond 17 claimed;
//  - refusals: a stream frame gap, a recording starting, a GPUDirect-only frame,
//    a second request while busy, a camera whose reservation failed, an abort;
//  - WriteCapture with the GPU median writes a descriptor and a PNG that decodes
//    to that median.
#include "empty_background.h"
#include "empty_background_burst_worker.h"
#include "empty_background_median.h"

#include <cuda_runtime.h>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <unistd.h>
#include <iostream>
#include <memory>
#include <random>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace eb = orange::empty_background;
namespace fs = std::filesystem;

namespace {
int failures = 0;
void check(bool ok, const std::string& what)
{
    std::cout << (ok ? "[PASS] " : "[FAIL] ") << what << "\n";
    failures += ok ? 0 : 1;
}
void cuda_ok(cudaError_t status, const char* what)
{
    if (status != cudaSuccess) throw std::runtime_error(std::string(what) + ": " + cudaGetErrorString(status));
}

constexpr int kWidth = 64;
constexpr int kHeight = 9;  // 576 pixels: not a multiple of the 256-thread block
constexpr std::size_t kPixels = static_cast<std::size_t>(kWidth) * kHeight;

std::vector<std::uint8_t> random_frame(std::mt19937& rng, std::size_t pixels)
{
    std::uniform_int_distribution<int> value(0, 255);
    std::uniform_int_distribution<int> roll(0, 99);
    std::vector<std::uint8_t> frame(pixels);
    for (auto& p : frame) p = static_cast<std::uint8_t>(roll(rng) < 3 ? 255 : value(rng));
    return frame;
}

// One acquisition frame: an owned device copy with a recorded readiness event.
struct Source {
    WORKER_ENTRY entry{};
    cudaEvent_t event = nullptr;
    std::vector<std::uint8_t> pixels;
    Source(const std::vector<std::uint8_t>& frame, std::uint64_t stream_frame_id, bool ring_owned)
        : pixels(frame)
    {
        entry.width = kWidth;
        entry.height = kHeight;
        entry.pixelFormat = GVSP_PIX_MONO8;
        entry.source_buffer_bytes = pixels.size();
        entry.image_gpu_id = 0;
        entry.frame_id = stream_frame_id;
        entry.camera_frame_id = 65530 + stream_frame_id;  // wraps inside the burst on purpose
        if (entry.camera_frame_id > 65535) entry.camera_frame_id -= 65535;
        entry.timestamp = 1700000000000000000ULL + stream_frame_id * 10000000ULL;
        entry.timestamp_sys = entry.timestamp + 9100000ULL;
        entry.ref_count = 1;  // the acquisition thread's own reference
        unsigned char* device = nullptr;
        cuda_ok(cudaMalloc(&device, pixels.size()), "cudaMalloc source");
        cuda_ok(cudaMemcpy(device, pixels.data(), pixels.size(), cudaMemcpyHostToDevice), "upload source");
        cuda_ok(cudaEventCreateWithFlags(&event, cudaEventDisableTiming), "event");
        cuda_ok(cudaEventRecord(event), "record");
        if (ring_owned) {
            entry.d_image_pool = device;
            entry.d_image = device;
            entry.event_ptr = &event;
            entry.owns_memory = true;
            entry.gpu_direct_mode = false;
        } else {
            entry.d_analytics_image = device;
            entry.analytics_ready_event = event;
            entry.analytics_owned_frame_valid = true;
            entry.analytics_ready_event_recorded = true;
            entry.d_image = device;  // the ring pointer stands in; it must not be read
        }
    }
    ~Source()
    {
        cudaFree(entry.d_analytics_image ? entry.d_analytics_image : entry.d_image_pool);
        cudaEventDestroy(event);
    }
};

bool wait_result(EmptyBackgroundBurstWorker& worker, EmptyBackgroundBurstResult* out)
{
    const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (std::chrono::steady_clock::now() < end) {
        if (worker.PopResult(out)) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return false;
}

CameraParams camera(const std::string& serial)
{
    CameraParams params{};
    params.camera_serial = serial;
    params.width = kWidth;
    params.height = kHeight;
    params.pixel_format = "Mono8";
    params.gpu_id = 0;
    return params;
}

struct Burst {
    std::vector<std::vector<std::uint8_t>> frames;
    std::vector<std::unique_ptr<Source>> sources;
};

Burst make_burst(std::mt19937& rng, bool ring_owned, std::uint64_t first_id = 100)
{
    Burst b;
    for (std::uint64_t k = 0; k < eb::kFrameCount; ++k) {
        b.frames.push_back(random_frame(rng, kPixels));
        b.sources.push_back(std::make_unique<Source>(b.frames.back(), first_id + k, ring_owned));
    }
    return b;
}

void kernel_case(std::mt19937& rng)
{
    const std::size_t pixels = 100003;
    std::vector<std::vector<std::uint8_t>> frames;
    std::vector<std::uint8_t> packed;
    for (std::size_t k = 0; k < eb::kFrameCount; ++k) {
        frames.push_back(random_frame(rng, pixels));
        packed.insert(packed.end(), frames.back().begin(), frames.back().end());
    }
    const eb::BurstMedian expected = eb::MedianOfBurst(frames, pixels);
    std::uint8_t *d_frames = nullptr, *d_median = nullptr;
    unsigned long long* d_count = nullptr;
    cuda_ok(cudaMalloc(&d_frames, packed.size()), "frames");
    cuda_ok(cudaMalloc(&d_median, pixels), "median");
    cuda_ok(cudaMalloc(&d_count, sizeof(unsigned long long)), "count");
    cuda_ok(cudaMemcpy(d_frames, packed.data(), packed.size(), cudaMemcpyHostToDevice), "upload");
    check(eb::LaunchBurstMedian17(d_frames, pixels, pixels, d_median, d_count, nullptr) == cudaSuccess,
          "median kernel launches");
    std::vector<std::uint8_t> median(pixels);
    unsigned long long count = 0;
    cuda_ok(cudaMemcpy(median.data(), d_median, pixels, cudaMemcpyDeviceToHost), "download");
    cuda_ok(cudaMemcpy(&count, d_count, sizeof(count), cudaMemcpyDeviceToHost), "download count");
    check(median == expected.median, "GPU median equals MedianOfBurst on 100003 pixels");
    check(count == expected.any_frame_saturated_pixels && count > 0,
          "GPU saturated count equals the CPU count (" + std::to_string(count) + ")");
    check(eb::LaunchBurstMedian17(d_frames, pixels - 1, pixels, d_median, d_count, nullptr) == cudaErrorInvalidValue,
          "a stride below the pixel count is refused");
    cudaFree(d_frames);
    cudaFree(d_median);
    cudaFree(d_count);
}

void good_burst_case(std::mt19937& rng, bool ring_owned)
{
    const std::string label = ring_owned ? "pool ring copy (detection off)" : "late owned copy (analytics path)";
    CameraParams params = camera("2010093");
    SafeQueue<WORKER_ENTRY*> recycle;
    EmptyBackgroundBurstWorker worker("burst-test", &params, recycle);
    std::string error;
    check(worker.Reserve(&error), label + ": staging reserved " + error);
    check(worker.reserved_bytes() == 18 * kPixels + sizeof(unsigned long long), label + ": 17 slots + median reserved");
    check(worker.StartThread() == 0, label + ": thread starts");
    struct Stop { EmptyBackgroundBurstWorker& w; ~Stop() { w.StopThread(); } } stop{worker};
    std::uint64_t request = 0;
    check(!worker.WantsFrame(), label + ": idle worker wants no frame");
    check(worker.Request(&request, &error) && request != 0, label + ": request accepted");
    check(!worker.Request(nullptr, &error), label + ": second request refused while busy (" + error + ")");
    Burst burst = make_burst(rng, ring_owned);
    for (std::size_t k = 0; k < burst.sources.size(); ++k) {
        check(worker.WantsFrame(), label + ": wants frame " + std::to_string(k + 1));
        worker.OfferFrame(&burst.sources[k]->entry, false, 60 - static_cast<int>(k % 5), 40 - static_cast<int>(k % 3));
    }
    check(!worker.WantsFrame(), label + ": claims stop at 17");
    Source extra(burst.frames[0], 117, ring_owned);
    worker.OfferFrame(&extra.entry, false, 1, 1);  // ignored: not wanted
    EmptyBackgroundBurstResult result;
    check(wait_result(worker, &result), label + ": result delivered");
    check(result.ok, label + ": burst ok " + result.failure_reason);
    const eb::BurstMedian expected = eb::MedianOfBurst(burst.frames, kPixels);
    check(result.median.median == expected.median, label + ": median equals the CPU reference");
    check(result.median.any_frame_saturated_pixels == expected.any_frame_saturated_pixels,
          label + ": saturated count equals the CPU reference");
    bool ordered = result.source_frames.size() == eb::kFrameCount;
    for (std::size_t k = 0; ordered && k < result.source_frames.size(); ++k)
        ordered = result.source_frames[k].stream_frame_id == 100 + k &&
                  result.source_frames[k].camera_frame_id == burst.sources[k]->entry.camera_frame_id &&
                  result.source_frames[k].camera_timestamp_ns == burst.sources[k]->entry.timestamp;
    check(ordered, label + ": 17 source frames in stream order (camera id wraps inside)");
    check(result.min_free_entries == 56 && result.min_free_events == 38,
          label + ": lowest free pool counts recorded (" + std::to_string(result.min_free_entries) + ", " +
              std::to_string(result.min_free_events) + ")");
    check(result.max_queue_depth >= 1 && result.max_queue_depth <= eb::kFrameCount, label + ": queue depth recorded");
    bool released = extra.entry.ref_count == 1;
    for (const auto& s : burst.sources) released = released && s->entry.ref_count == 1;
    WORKER_ENTRY* leaked = nullptr;
    check(released && !recycle.pop(leaked), label + ": every lease released once, acquisition's own kept");
    check(!worker.Busy(), label + ": idle after the result");
    check(worker.Request(&request, &error), label + ": a new burst can start after the result");
    worker.Abort("test over");
    check(wait_result(worker, &result) && !result.ok && result.failure_reason == "test over",
          label + ": abort before any frame ends the burst as failed");
}

void refusal_case(std::mt19937& rng, const std::string& label, int which)
{
    CameraParams params = camera("2010094");
    SafeQueue<WORKER_ENTRY*> recycle;
    EmptyBackgroundBurstWorker worker("burst-refusal", &params, recycle);
    std::string error;
    worker.Reserve(&error);
    worker.StartThread();
    struct Stop { EmptyBackgroundBurstWorker& w; ~Stop() { w.StopThread(); } } stop{worker};
    std::uint64_t request = 0;
    worker.Request(&request, &error);
    Burst burst = make_burst(rng, false);
    if (which == 0) burst.sources[5]->entry.frame_id += 1;  // stream frame gap
    if (which == 2) {                                         // GPUDirect only, no owned copy
        burst.sources[3]->entry.analytics_owned_frame_valid = false;
        burst.sources[3]->entry.gpu_direct_mode = true;
    }
    if (which == 3) burst.sources[2]->entry.image_gpu_id = 1;  // another die
    for (std::size_t k = 0; k < burst.sources.size() && worker.WantsFrame(); ++k)
        worker.OfferFrame(&burst.sources[k]->entry, which == 1 && k == 7, 60, 40);
    EmptyBackgroundBurstResult result;
    check(wait_result(worker, &result), label + ": result delivered");
    check(!result.ok, label + ": burst refused (" + result.failure_reason + ")");
    bool released = true;
    for (const auto& s : burst.sources) released = released && s->entry.ref_count == 1;
    check(released, label + ": every lease released");
}

void reservation_case()
{
    CameraParams params = camera("2010095");
    params.pixel_format = "Mono12";
    SafeQueue<WORKER_ENTRY*> recycle;
    EmptyBackgroundBurstWorker worker("burst-mono12", &params, recycle);
    std::string error;
    check(!worker.Reserve(&error) && !worker.reserved(), "a Mono12 camera reserves nothing (" + error + ")");
    std::uint64_t request = 0;
    check(!worker.Request(&request, &error) && error.find("Mono12") != std::string::npos,
          "its burst request is refused with the reason");
    int device = -1;
    cudaGetDevice(&device);
    check(device == 0, "Reserve leaves the caller's device unchanged");
}

void write_case(std::mt19937& rng)
{
    CameraParams params = camera("2010096");
    SafeQueue<WORKER_ENTRY*> recycle;
    EmptyBackgroundBurstWorker worker("burst-write", &params, recycle);
    std::string error;
    worker.Reserve(&error);
    worker.StartThread();
    struct Stop { EmptyBackgroundBurstWorker& w; ~Stop() { w.StopThread(); } } stop{worker};
    worker.Request(nullptr, &error);
    Burst burst = make_burst(rng, false);
    for (auto& s : burst.sources) worker.OfferFrame(&s->entry, false, 60, 40);
    EmptyBackgroundBurstResult result;
    wait_result(worker, &result);
    const fs::path store = fs::temp_directory_path() / ("orange_eb_burst_test_" + std::to_string(::getpid()));
    fs::remove_all(store);
    eb::CapturePlan plan;
    plan.schema_version = 1;
    plan.store_root = store;
    plan.capture_id = "burst_test";
    plan.captured_at_utc = "2026-11-15T12:00:00.000Z";
    plan.scene_declaration.note = "synthetic";
    plan.scene_declaration.stimulus_display.state = "off";
    std::vector<eb::CameraBurst> bursts(1);
    bursts[0].camera_serial = params.camera_serial;
    bursts[0].configuration.width = kWidth;
    bursts[0].configuration.height = kHeight;
    bursts[0].configuration.pixel_format = "Mono8";
    bursts[0].source_frames = result.source_frames;
    bursts[0].median = result.median;
    try {
        const eb::CaptureReceipt receipt = eb::WriteCapture(plan, &bursts);
        check(receipt.descriptor.cameras.size() == 1 && receipt.descriptor.cameras[0].capture_status == "captured",
              "WriteCapture takes the GPU median");
        const eb::StoredCapture stored = eb::ReadCapture(receipt.descriptor_path);
        check(stored.descriptor_sha256 == receipt.descriptor_sha256, "the descriptor reads back");
        std::string png, why;
        {
            std::ifstream in(store / "burst_test" / "Cam2010096_empty_background.png", std::ios::binary);
            png.assign(std::istreambuf_iterator<char>(in), {});
        }
        std::vector<std::uint8_t> decoded;
        std::uint32_t w = 0, h = 0;
        check(eb::DecodePngGray8(png, &decoded, &w, &h, &why) && w == kWidth && h == kHeight &&
                  decoded == result.median.median,
              "the PNG decodes to the GPU median");
    } catch (const std::exception& ex) {
        check(false, std::string("WriteCapture with a GPU median: ") + ex.what());
    }
    std::vector<eb::CameraBurst> both(1, bursts[0]);
    both[0].median = result.median;
    both[0].frames = burst.frames;
    plan.capture_id = "burst_test_both";
    bool refused = false;
    try {
        eb::WriteCapture(plan, &both);
    } catch (const std::exception&) {
        refused = true;
    }
    check(refused && !fs::exists(store / "burst_test_both"), "frames and a median together are refused before any write");
    fs::remove_all(store);
}
}  // namespace

int main()
{
    int devices = 0;
    if (cudaGetDeviceCount(&devices) != cudaSuccess || devices == 0) {
        std::cout << "empty_background_burst_tests: no CUDA device, skipped\n";
        return 77;
    }
    try {
        cuda_ok(cudaSetDevice(0), "cudaSetDevice 0");
        std::mt19937 rng(20261115);
        kernel_case(rng);
        good_burst_case(rng, false);
        good_burst_case(rng, true);
        refusal_case(rng, "stream frame gap", 0);
        refusal_case(rng, "recording starts during the burst", 1);
        refusal_case(rng, "GPUDirect-only frame", 2);
        refusal_case(rng, "frame on another die", 3);
        reservation_case();
        write_case(rng);
    } catch (const std::exception& ex) {
        check(false, std::string("exception: ") + ex.what());
    }
    std::cout << (failures == 0 ? "empty_background_burst_tests: PASS\n" : "empty_background_burst_tests: FAIL\n");
    return failures == 0 ? 0 : 1;
}
