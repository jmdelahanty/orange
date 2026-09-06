#include "spatial_snapshot_worker.h"
#include <chrono>
#include <iostream>
#include <sched.h>
#include <stdexcept>
#include <thread>

namespace {
void check(bool ok, const char* reason) { if (!ok) throw std::runtime_error(reason); }
void cuda_ok(cudaError_t status) { if (status != cudaSuccess) throw std::runtime_error(cudaGetErrorString(status)); }
struct Source {
    WORKER_ENTRY entry{};
    std::vector<unsigned char> pixels;
    Source() {
        entry.width = 8; entry.height = 4; entry.pixelFormat = GVSP_PIX_MONO8;
        entry.source_buffer_bytes = 32; entry.image_gpu_id = 0;
        entry.frame_id = 77; entry.camera_frame_id = 500; entry.recording_frame_id = 0;
        entry.timestamp = 1700000000000000001ULL; entry.timestamp_sys = 1700000000000000023ULL;
        entry.ref_count = 1;
        pixels.resize(32); for (std::size_t i = 0; i < pixels.size(); ++i) pixels[i] = static_cast<unsigned char>(i * 7);
        cuda_ok(cudaMalloc(&entry.d_analytics_image, pixels.size()));
        cuda_ok(cudaMemcpy(entry.d_analytics_image, pixels.data(), pixels.size(), cudaMemcpyHostToDevice));
        cuda_ok(cudaEventCreateWithFlags(&entry.analytics_ready_event, cudaEventDisableTiming));
        cuda_ok(cudaEventRecord(entry.analytics_ready_event));
        entry.analytics_owned_frame_valid = true; entry.analytics_ready_event_recorded = true;
    }
    ~Source() { cudaEventDestroy(entry.analytics_ready_event); cudaFree(entry.d_analytics_image); }
};
void run_case(int test, int cpu) {
    Source source;
    CameraParams params{}; params.camera_serial = "02010093";
    SafeQueue<WORKER_ENTRY*> recycle;
    SpatialSnapshotWorker worker("context-test", &params, recycle);
    worker.SetMaxQueueSize(1);
    if (test != 4) worker.SetNativeSnapshotCpu(cpu);
    if (test == 1) source.entry.analytics_owned_frame_valid = false;
    if (test == 2) source.entry.pixelFormat = GVSP_PIX_MONO12;
    if (test == 3) source.entry.source_buffer_bytes = 31;
    if (test == 5) source.entry.analytics_ready_event_recorded = false;
    if (test == 6) source.entry.image_gpu_id = -1;
    uint64_t request = 0; std::string error;
    check(test == 7 ? worker.RequestSnapshot("test", &request, &error) :
        worker.RequestNativeSnapshot("test", &request, &error), "request failed");
    check(!worker.RequestNativeSnapshot("duplicate", nullptr, &error), "overlapping request accepted");
    check(worker.StartThread() == 0, "worker start failed");
    struct Stop { SpatialSnapshotWorker& worker; ~Stop() { worker.StopThread(); } } stop{worker};
    check(worker.HasPendingRequest() && worker.TryClaimNextFrame(), "claim failed");
    check(!worker.HasPendingRequest() && !worker.TryClaimNextFrame(), "duplicate source claim");
    check(worker.PutObjectToQueueIn(&source.entry), "enqueue failed");
    SpatialSnapshotResult result;
    bool done = false;
    const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (!(done = worker.PopCompletedSnapshot(&result)) && std::chrono::steady_clock::now() < end) {
        check(!worker.HasFatalError(), "snapshot thread exception");
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    worker.StopThread();
    check(done && result.request_id == request, "missing matching result");
    check(result.ok == (test == 0 || test == 7), "unexpected capture acceptance");
    WORKER_ENTRY* returned = nullptr;
    check(source.entry.ref_count == 0 && recycle.pop(returned) && returned == &source.entry && !recycle.pop(returned),
          "source reference not released exactly once");
    check(worker.GetCountQueueOutSize() == 0, "released source pointer escaped to output queue");
    check(!worker.HasPendingRequest(), "one-shot work remained pending");
    if (test == 0 || test == 7) {
        if (test == 0) {
            check(result.native_bytes == source.pixels && result.rgba.empty(), "source pixels changed");
            check(result.capture_representation == "native_bytes", "wrong native capture representation");
        } else {
            check(result.native_bytes.empty() && result.rgba.size() == source.pixels.size() * 4 &&
                  result.capture_representation == "rgba8", "existing RGBA snapshot representation changed");
            for (std::size_t i = 0; i < source.pixels.size(); ++i)
                check(result.rgba[i * 4] == source.pixels[i] && result.rgba[i * 4 + 1] == source.pixels[i] &&
                      result.rgba[i * 4 + 2] == source.pixels[i] && result.rgba[i * 4 + 3] == 255,
                      "existing Mono8-to-RGBA snapshot changed");
        }
        check(result.camera_serial == params.camera_serial, "wrong snapshot camera");
        check(result.width == 8 && result.height == 4 && result.local_frame_id == 77 && result.camera_frame_id == 500 &&
              result.recording_frame_id == 0 && result.camera_timestamp_ns == source.entry.timestamp &&
              result.timestamp_sys_ns == source.entry.timestamp_sys, "source identity/raster changed");
    } else check(!result.error.empty(), "refusal has no diagnostic");
}
}
int main() {
    try {
        int count = 0;
        const auto status = cudaGetDeviceCount(&count);
        if (status != cudaSuccess || count < 1) {
            std::cerr << "CUDA device unavailable: " << cudaGetErrorString(status) << '\n'; return 77;
        }
        cuda_ok(cudaSetDevice(0));
        cpu_set_t allowed; CPU_ZERO(&allowed); check(sched_getaffinity(0, sizeof(allowed), &allowed) == 0, "CPU affinity query failed");
        int cpu = 0; while (cpu < CPU_SETSIZE && !CPU_ISSET(cpu, &allowed)) ++cpu;
        check(cpu < CPU_SETSIZE, "no allowed CPU");
        for (int test = 0; test < 8; ++test) run_case(test, cpu);
        std::cout << "8 context/compatible RGBA snapshot cases passed (CUDA memory only; no camera).\n";
        return 0;
    } catch (const std::exception& ex) { std::cerr << ex.what() << '\n'; return 1; }
}
