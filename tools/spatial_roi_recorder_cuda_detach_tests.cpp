#include "spatial_roi_recorder_cuda_detach.h"

#include "shaman_v2_recording_identity.h"

#include <cuda_runtime.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <csignal>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <sys/types.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <utility>
#include <vector>

namespace {

using orange::spatial_roi::SpatialRoiFrameDescriptor;
using orange::spatial_roi::ipc::SpatialRoiCudaIpcBuffer;
using orange::spatial_roi::ipc::SpatialRoiIpcFrame;
using orange::spatial_roi::ipc::SpatialRoiRecorderCudaDetachConfig;
using orange::spatial_roi::ipc::SpatialRoiRecorderCudaDetachPool;
using orange::spatial_roi::ipc::SpatialRoiRecorderDetachStatus;

// A detach test frame is expected to contain only the descriptor and two
// fixed-size handles.  This bounded transport also prevents a wedged child
// from filling an unbounded parent-side string or pipe.
constexpr std::size_t kTestWireLimit = 256 * 1024;

void require(const bool condition, const std::string& message)
{
    if (!condition) {
        throw std::runtime_error(message);
    }
}

void require_cuda(const cudaError_t status, const char* operation)
{
    if (status != cudaSuccess) {
        throw std::runtime_error(
            std::string(operation) + " failed: " + cudaGetErrorString(status));
    }
}

std::string hex_handle(const void* handle, const std::size_t bytes)
{
    static constexpr char kHex[] = "0123456789abcdef";
    const auto* input = static_cast<const unsigned char*>(handle);
    std::string output;
    output.reserve(bytes * 2);
    for (std::size_t index = 0; index < bytes; ++index) {
        output.push_back(kHex[input[index] >> 4]);
        output.push_back(kHex[input[index] & 0x0f]);
    }
    return output;
}

int parse_gpu_id_arg(const char* text, const char* context)
{
    require(text && *text,
            std::string(context ? context : "GPU id") + " is empty");
    char* end = nullptr;
    errno = 0;
    const long parsed = std::strtol(text, &end, 10);
    require(errno == 0 && end && *end == '\0' && parsed >= 0 &&
                parsed <= std::numeric_limits<int>::max(),
            std::string("invalid ") + (context ? context : "GPU id") +
                ": " + text);
    return static_cast<int>(parsed);
}

bool write_all(const int fd, const void* data, const std::size_t bytes)
{
    const auto* input = static_cast<const unsigned char*>(data);
    std::size_t written = 0;
    while (written < bytes) {
        const ssize_t result = write(fd, input + written, bytes - written);
        if (result > 0) {
            written += static_cast<std::size_t>(result);
            continue;
        }
        if (result < 0 && errno == EINTR) {
            continue;
        }
        return false;
    }
    return true;
}

void write_line(const int fd, const std::string& line)
{
    require(line.find('\n') == std::string::npos,
            "internal test protocol line contains a newline");
    require(line.size() + 1 <= kTestWireLimit,
            "internal test protocol line exceeds bounded limit");
    require(write_all(fd, line.data(), line.size()),
            "write on test IPC pipe failed");
    const char newline = '\n';
    require(write_all(fd, &newline, 1), "write test IPC newline failed");
}

bool read_line(const int fd, std::string* line_out)
{
    if (!line_out) {
        return false;
    }
    line_out->clear();
    while (line_out->size() < kTestWireLimit) {
        char byte = 0;
        const ssize_t result = read(fd, &byte, 1);
        if (result == 1) {
            if (byte == '\n') {
                return true;
            }
            line_out->push_back(byte);
            continue;
        }
        if (result == 0) {
            return !line_out->empty();
        }
        if (errno == EINTR) {
            continue;
        }
        return false;
    }
    return false;
}

struct GpuSource {
    int gpu_id = -1;
    unsigned char* device_data = nullptr;
    cudaStream_t stream = nullptr;
    cudaEvent_t ready_event = nullptr;

    GpuSource() = default;
    GpuSource(const GpuSource&) = delete;
    GpuSource& operator=(const GpuSource&) = delete;
    GpuSource(GpuSource&& other) noexcept
        : gpu_id(other.gpu_id),
          device_data(other.device_data),
          stream(other.stream),
          ready_event(other.ready_event)
    {
        other.gpu_id = -1;
        other.device_data = nullptr;
        other.stream = nullptr;
        other.ready_event = nullptr;
    }

    GpuSource& operator=(GpuSource&& other) noexcept
    {
        if (this != &other) {
            Release();
            gpu_id = other.gpu_id;
            device_data = other.device_data;
            stream = other.stream;
            ready_event = other.ready_event;
            other.gpu_id = -1;
            other.device_data = nullptr;
            other.stream = nullptr;
            other.ready_event = nullptr;
        }
        return *this;
    }

    void Release() noexcept
    {
        if (gpu_id >= 0) {
            (void)cudaSetDevice(gpu_id);
        }
        if (stream) {
            (void)cudaStreamSynchronize(stream);
        }
        if (ready_event) {
            (void)cudaEventDestroy(ready_event);
        }
        if (device_data) {
            (void)cudaFree(device_data);
        }
        if (stream) {
            (void)cudaStreamDestroy(stream);
        }
        gpu_id = -1;
        device_data = nullptr;
        stream = nullptr;
        ready_event = nullptr;
    }

    ~GpuSource()
    {
        Release();
    }
};

GpuSource make_source(const int gpu_id,
                      const std::vector<unsigned char>& bytes)
{
    GpuSource source;
    source.gpu_id = gpu_id;
    require_cuda(cudaSetDevice(gpu_id), "cudaSetDevice(source)");
    require_cuda(cudaMalloc(reinterpret_cast<void**>(&source.device_data),
                            bytes.size()),
                 "cudaMalloc(source)");
    require_cuda(cudaStreamCreateWithFlags(&source.stream, cudaStreamNonBlocking),
                 "cudaStreamCreateWithFlags(source)");
    require_cuda(cudaEventCreateWithFlags(&source.ready_event,
                                          cudaEventDisableTiming |
                                              cudaEventInterprocess),
                 "cudaEventCreateWithFlags(source)");
    require_cuda(cudaMemcpyAsync(source.device_data,
                                 bytes.data(),
                                 bytes.size(),
                                 cudaMemcpyHostToDevice,
                                 source.stream),
                 "cudaMemcpyAsync(source)");
    require_cuda(cudaEventRecord(source.ready_event, source.stream),
                 "cudaEventRecord(source)");
    require_cuda(cudaEventSynchronize(source.ready_event),
                 "cudaEventSynchronize(source)");
    return source;
}

struct HostGate {
    std::atomic<bool> open{false};
};

void CUDART_CB wait_for_host_gate(void* user_data)
{
    auto* gate = static_cast<HostGate*>(user_data);
    while (!gate->open.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }
}

GpuSource make_pending_source(const int gpu_id,
                              const std::vector<unsigned char>& bytes,
                              HostGate* gate)
{
    require(gate != nullptr, "pending source host gate is null");
    GpuSource source;
    source.gpu_id = gpu_id;
    require_cuda(cudaSetDevice(gpu_id), "cudaSetDevice(pending source)");
    require_cuda(cudaMalloc(reinterpret_cast<void**>(&source.device_data),
                            bytes.size()),
                 "cudaMalloc(pending source)");
    require_cuda(cudaStreamCreateWithFlags(&source.stream, cudaStreamNonBlocking),
                 "cudaStreamCreateWithFlags(pending source)");
    require_cuda(cudaEventCreateWithFlags(&source.ready_event,
                                          cudaEventDisableTiming |
                                              cudaEventInterprocess),
                 "cudaEventCreateWithFlags(pending source)");
    require_cuda(cudaMemcpyAsync(source.device_data,
                                 bytes.data(),
                                 bytes.size(),
                                 cudaMemcpyHostToDevice,
                                 source.stream),
                 "cudaMemcpyAsync(pending source)");
    // The exported event is recorded after a host callback whose gate is
    // deliberately held closed.  A separate recorder must therefore exercise
    // its bounded cudaEventQuery deadline rather than wait indefinitely.
    require_cuda(cudaLaunchHostFunc(source.stream, wait_for_host_gate, gate),
                 "cudaLaunchHostFunc(pending source)");
    require_cuda(cudaEventRecord(source.ready_event, source.stream),
                 "cudaEventRecord(pending source)");
    return source;
}

SpatialRoiFrameDescriptor make_descriptor(const int source_gpu_id,
                                          const int recorder_gpu_id,
                                          const std::string& recording_id =
                                              "recorder-detach-test")
{
    SpatialRoiFrameDescriptor descriptor;
    descriptor.recording_id = recording_id;
    descriptor.recording_identity_token =
        orange::shaman_v2_recording_identity::token_for_recording_id(
            recording_id);
    descriptor.producer_generation = "generation_1";
    descriptor.camera_id = 3;
    descriptor.camera_serial = "2010096";
    descriptor.local_frame_id = 91;
    descriptor.camera_frame_id = 7001;
    descriptor.recording_frame_id = 17;
    descriptor.roi_stream_frame_index = 1;
    descriptor.camera_timestamp_ns = 123456789;
    descriptor.timestamp_sys_ns = 987654321;
    descriptor.roi_id = "roi_1";
    descriptor.region_id = "region_1";
    descriptor.arena_group_id = "group_1";
    descriptor.arena_id = "arena_1";
    descriptor.logical_stream_id = "2010096_spatial_roi_roi_1";
    descriptor.spatial_roi_plan_sha256 = "sha256:" + std::string(64, 'a');
    descriptor.native_raster = {8, 8};
    descriptor.content_rect = {0, 0, 4, 4};
    descriptor.encoded_raster = {4, 4};
    descriptor.encoded_content_rect = {0, 0, 4, 4};
    descriptor.padding = {0, 0, 0, 0, 0};
    descriptor.source_pixel_format = orange::spatial_roi::kSpatialRoiMono8PixelFormat;
    descriptor.bytes = 16;
    descriptor.source_gpu_id = source_gpu_id;
    descriptor.assigned_gpu_id = recorder_gpu_id;
    descriptor.assigned_shard_id = 0;
    descriptor.routing_policy = "single_shard";
    return descriptor;
}

SpatialRoiIpcFrame make_frame(const SpatialRoiFrameDescriptor& descriptor,
                              const GpuSource& source)
{
    SpatialRoiIpcFrame frame;
    frame.descriptor = descriptor;
    cudaIpcMemHandle_t memory_handle{};
    cudaIpcEventHandle_t event_handle{};
    require_cuda(cudaIpcGetMemHandle(&memory_handle, source.device_data),
                 "cudaIpcGetMemHandle(source)");
    require_cuda(cudaIpcGetEventHandle(&event_handle, source.ready_event),
                 "cudaIpcGetEventHandle(source event)");
    SpatialRoiCudaIpcBuffer& buffer = frame.cuda_buffer;
    buffer.memory_handle_hex = hex_handle(&memory_handle, sizeof(memory_handle));
    buffer.ready_event_handle_hex = hex_handle(&event_handle, sizeof(event_handle));
    buffer.byte_offset = 0;
    buffer.byte_length = descriptor.bytes;
    buffer.row_pitch_bytes = descriptor.encoded_raster.width;
    buffer.pixel_format = orange::spatial_roi::kSpatialRoiMono8PixelFormat;
    buffer.layout = "packed_row_major";
    return frame;
}

SpatialRoiRecorderCudaDetachConfig make_config(
    const SpatialRoiFrameDescriptor& descriptor,
    const std::size_t slot_count,
    const std::uint32_t operation_timeout_ms = 1000)
{
    SpatialRoiRecorderCudaDetachConfig config;
    config.expected_stream =
        orange::spatial_roi::ipc::spatial_roi_ipc_stream_identity_from_descriptor(
            descriptor);
    config.recorder_gpu_id = descriptor.assigned_gpu_id;
    config.expected_source_gpu_id = descriptor.source_gpu_id;
    config.expected_assigned_shard_id = descriptor.assigned_shard_id;
    config.expected_geometry.native_raster = descriptor.native_raster;
    config.expected_geometry.content_rect = descriptor.content_rect;
    config.expected_geometry.encoded_raster = descriptor.encoded_raster;
    config.expected_geometry.encoded_content_rect = descriptor.encoded_content_rect;
    config.expected_geometry.padding = descriptor.padding;
    config.expected_geometry.routing_policy = descriptor.routing_policy;
    config.slot_count = slot_count;
    const std::uint64_t mono8_bytes =
        static_cast<std::uint64_t>(descriptor.encoded_raster.width) *
        static_cast<std::uint64_t>(descriptor.encoded_raster.height);
    config.max_pool_bytes =
        (mono8_bytes + mono8_bytes * 3ULL / 2ULL) *
        static_cast<std::uint64_t>(slot_count);
    config.operation_timeout_ms = operation_timeout_ms;
    return config;
}

void verify_outputs(
    const orange::spatial_roi::ipc::SpatialRoiRecorderDetachedFrame& frame,
    const std::vector<unsigned char>& source_bytes,
    const char* context)
{
    std::vector<unsigned char> mono(source_bytes.size(), 0);
    std::vector<unsigned char> nv12(source_bytes.size() + source_bytes.size() / 2,
                                    0);
    require_cuda(cudaMemcpy(mono.data(),
                            frame.device_mono8(),
                            mono.size(),
                            cudaMemcpyDeviceToHost),
                 "cudaMemcpy(detached Mono8)");
    require_cuda(cudaMemcpy(nv12.data(),
                            frame.device_nv12(),
                            nv12.size(),
                            cudaMemcpyDeviceToHost),
                 "cudaMemcpy(detached NV12)");
    require(mono == source_bytes,
            std::string(context) + ": detached Mono8 bytes changed");
    require(std::equal(source_bytes.begin(), source_bytes.end(), nv12.begin()),
            std::string(context) + ": NV12 Y plane is not byte-identical Mono8");
    require(std::all_of(nv12.begin() + source_bytes.size(), nv12.end(),
                        [](const unsigned char value) { return value == 128; }),
            std::string(context) + ": NV12 UV plane is not neutral 128");
}

void require_safe(
    const SpatialRoiRecorderDetachStatus status,
    const orange::spatial_roi::ipc::SpatialRoiRecorderCudaDetachResult& result,
    const char* context)
{
    require(result.status == status,
            std::string(context) + ": unexpected status " +
                orange::spatial_roi::ipc::spatial_roi_recorder_detach_status_name(
                    result.status) +
                (result.error.empty() ? std::string{} : ": " + result.error));
    require(result.source_release_safe(),
            std::string(context) + ": rejected frame was not source-release safe");
}

void run_recorder_checks(const SpatialRoiIpcFrame& frame)
{
    const int gpu_id = frame.descriptor.assigned_gpu_id;
    const std::vector<unsigned char> source_bytes = {
        0, 1, 2, 3,
        4, 5, 6, 7,
        8, 9, 10, 11,
        12, 13, 14, 15};

    require(frame.descriptor.bytes == source_bytes.size(),
            "producer FRAME did not carry the exact expected byte length");
    require(frame.cuda_buffer.byte_offset == 0 &&
                frame.cuda_buffer.byte_length == source_bytes.size() &&
                frame.cuda_buffer.row_pitch_bytes == 4,
            "producer FRAME changed the exact packed raster span");

    // Retaining the first result proves nonblocking exhaustion.  Releasing it
    // and detaching again proves that the bounded slot is reusable.
    SpatialRoiRecorderCudaDetachPool pool(make_config(frame.descriptor, 1));
    require(pool.valid(), "single-slot detach pool initialization failed: " +
                              pool.error());
    auto first = pool.TryDetach(frame);
    require(first.detached(), "first separate-process detach did not succeed: " +
                                first.error);
    require(first.source_release_safe(),
            "successful separate-process detach was not source-release safe");
    verify_outputs(first.frame, source_bytes, "separate-process first detach");

    auto exhausted = pool.TryDetach(frame);
    require_safe(SpatialRoiRecorderDetachStatus::kPoolExhausted,
                 exhausted,
                 "retained slot pool exhaustion");
    first.frame.Release();
    require(pool.available_slot_count() == 1,
            "released separate-process detach slot did not become available");

    auto recycled = pool.TryDetach(frame);
    require(recycled.detached(), "released separate-process slot was not reusable: " +
                                  recycled.error);
    verify_outputs(recycled.frame, source_bytes, "separate-process recycled detach");
    recycled.frame.Release();
    recycled.frame.Release();
    const auto reuse_counters = pool.counters();
    require(reuse_counters.pool_exhausted >= 1,
            "separate-process pool exhaustion was not counted");
    require(reuse_counters.slot_releases >= 2,
            "separate-process slot releases were not counted");

    // These rejections happen before a slot is claimed/imported and remain
    // safe for the producer to RELEASE.
    SpatialRoiIpcFrame wrong_identity = frame;
    wrong_identity.descriptor.recording_id = "another-recording";
    wrong_identity.descriptor.recording_identity_token =
        orange::shaman_v2_recording_identity::token_for_recording_id(
            wrong_identity.descriptor.recording_id);
    auto wrong_stream = pool.TryDetach(wrong_identity);
    require_safe(SpatialRoiRecorderDetachStatus::kInvalidArgument,
                 wrong_stream,
                 "recording identity rejection");
    require(pool.available_slot_count() == 1,
            "identity rejection consumed a detach slot");

    SpatialRoiIpcFrame wrong_gpu = frame;
    wrong_gpu.descriptor.assigned_gpu_id = gpu_id + 1;
    auto gpu_result = pool.TryDetach(wrong_gpu);
    require_safe(SpatialRoiRecorderDetachStatus::kWrongDevice,
                 gpu_result,
                 "assigned GPU rejection");
    require(pool.available_slot_count() == 1,
            "GPU rejection consumed a detach slot");

    SpatialRoiIpcFrame bad_handle = frame;
    bad_handle.cuda_buffer.memory_handle_hex.assign(
        orange::spatial_roi::ipc::kSpatialRoiCudaIpcHandleHexBytes, 'g');
    auto invalid_handle = pool.TryDetach(bad_handle);
    require_safe(SpatialRoiRecorderDetachStatus::kInvalidArgument,
                 invalid_handle,
                 "invalid memory handle rejection");
    require(pool.available_slot_count() == 1,
            "invalid handle rejection consumed a detach slot");

    // Keep the encoded dimensions unchanged while moving the native source
    // rectangle. The descriptor remains internally self-consistent, so only
    // the independently bound contract geometry can reject this mutation.
    SpatialRoiIpcFrame wrong_geometry = frame;
    ++wrong_geometry.descriptor.content_rect.x;
    auto geometry_result = pool.TryDetach(wrong_geometry);
    require_safe(SpatialRoiRecorderDetachStatus::kInvalidArgument,
                 geometry_result,
                 "plan geometry rejection");

    SpatialRoiIpcFrame wrong_pitch = frame;
    ++wrong_pitch.cuda_buffer.row_pitch_bytes;
    auto pitch_result = pool.TryDetach(wrong_pitch);
    require_safe(SpatialRoiRecorderDetachStatus::kInvalidArgument,
                 pitch_result,
                 "packed row-pitch rejection");

    auto insufficient_budget = make_config(frame.descriptor, 1);
    --insufficient_budget.max_pool_bytes;
    SpatialRoiRecorderCudaDetachPool budget_pool(
        std::move(insufficient_budget));
    require(!budget_pool.valid(),
            "under-budget recorder-owned pool was accepted");

    pool.Stop();
    auto normally_stopped = pool.TryDetach(frame);
    require_safe(SpatialRoiRecorderDetachStatus::kStopped,
                 normally_stopped,
                 "normally stopped pool");

    // A syntactically valid but unusable event handle fails after memory
    // import.  The implementation must quarantine that slot and report that
    // producer RELEASE is unsafe; it must never recycle the imported source.
    SpatialRoiRecorderCudaDetachPool fault_pool(make_config(frame.descriptor, 1));
    require(fault_pool.valid(), "fault-test detach pool initialization failed: " +
                                  fault_pool.error());
    SpatialRoiIpcFrame bad_event = frame;
    bad_event.cuda_buffer.ready_event_handle_hex.assign(
        orange::spatial_roi::ipc::kSpatialRoiCudaIpcHandleHexBytes, '0');
    auto failed = fault_pool.TryDetach(bad_event);
    require(failed.status == SpatialRoiRecorderDetachStatus::kSourceQuarantined,
            "post-import event failure did not quarantine source ownership: " +
                failed.error);
    require(!failed.source_release_safe(),
            "quarantined source was incorrectly reported RELEASE-safe");
    require(fault_pool.available_slot_count() == 0,
            "quarantined slot became available for reuse");
    const auto fault_counters = fault_pool.counters();
    require(fault_counters.source_quarantines == 1,
            "source quarantine was not counted exactly once");
    SpatialRoiIpcFrame malformed_after_quarantine = frame;
    malformed_after_quarantine.cuda_buffer.memory_handle_hex = "bad";
    auto malformed_sticky = fault_pool.TryDetach(malformed_after_quarantine);
    require(malformed_sticky.status ==
                SpatialRoiRecorderDetachStatus::kSourceQuarantined &&
                !malformed_sticky.source_release_safe(),
            "malformed retry bypassed sticky source quarantine");
    fault_pool.Stop();
    auto stopped = fault_pool.TryDetach(frame);
    require(stopped.status ==
                SpatialRoiRecorderDetachStatus::kSourceQuarantined &&
                !stopped.source_release_safe(),
            "stopped quarantined pool lost its source-unsafe terminal state");
}

void run_timeout_recorder_checks(const SpatialRoiIpcFrame& frame)
{
    // Keep this deadline short enough to make a missing bounded wait obvious,
    // but leave ample room for CUDA context creation and a loaded test host.
    constexpr std::uint32_t kTimeoutMs = 50;
    constexpr auto kWallClockBound = std::chrono::milliseconds(1000);
    SpatialRoiRecorderCudaDetachPool pool(
        make_config(frame.descriptor, 1, kTimeoutMs));
    require(pool.valid(), "timeout detach pool initialization failed: " +
                              pool.error());

    // Run the blocked detach on one recorder thread.  The attempted counter is
    // incremented only after that call has acquired the whole-operation gate,
    // so waiting for it avoids a sleep/race before issuing the concurrent call.
    const auto start = std::chrono::steady_clock::now();
    orange::spatial_roi::ipc::SpatialRoiRecorderCudaDetachResult result;
    std::thread blocked_detach([&]() { result = pool.TryDetach(frame); });
    const auto gate_wait_deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
    while (pool.counters().detach_attempted == 0 &&
           std::chrono::steady_clock::now() < gate_wait_deadline) {
        std::this_thread::yield();
    }
    require(pool.counters().detach_attempted >= 1,
            "timeout detach did not enter the recorder operation gate");

    const auto busy_start = std::chrono::steady_clock::now();
    auto busy = pool.TryDetach(frame);
    const auto busy_elapsed = std::chrono::steady_clock::now() - busy_start;
    require_safe(SpatialRoiRecorderDetachStatus::kBusy,
                 busy,
                 "concurrent whole-operation gate rejection");
    require(busy_elapsed < std::chrono::milliseconds(100),
            "concurrent kBusy rejection was not immediate");

    blocked_detach.join();
    const auto elapsed = std::chrono::steady_clock::now() - start;
    require(result.status == SpatialRoiRecorderDetachStatus::kSourceQuarantined,
            "pending producer event did not return source_quarantined: " +
                result.error);
    require(!result.source_release_safe(),
            "timed-out pending producer event was reported RELEASE-safe");
    require(result.error.find("deadline") != std::string::npos,
            "timeout result did not identify the recorder deadline: " +
                result.error);
    require(elapsed < kWallClockBound,
            "pending-event detach exceeded its bounded wall-clock interval");
    require(pool.available_slot_count() == 0,
            "timed-out source slot became available for reuse");
    const auto counters = pool.counters();
    require(counters.source_quarantines == 1,
            "timed-out source quarantine was not counted exactly once");
    require(counters.memory_imports == 1 && counters.event_imports == 1,
            "timeout happened before both producer IPC handles were imported");

    pool.Stop();
    auto stopped = pool.TryDetach(frame);
    require(stopped.status ==
                SpatialRoiRecorderDetachStatus::kSourceQuarantined &&
                !stopped.source_release_safe(),
            "timeout-quarantined pool lost its source-unsafe terminal state");
}

int run_producer(const int source_gpu_id, const int recorder_gpu_id)
{
    int device_count = 0;
    const cudaError_t device_status = cudaGetDeviceCount(&device_count);
    if (device_status != cudaSuccess || device_count <= 0) {
        write_line(STDOUT_FILENO,
                   std::string("SKIP ") +
                       (device_status == cudaSuccess
                            ? "no CUDA device"
                            : cudaGetErrorString(device_status)));
        (void)cudaGetLastError();
        return 0;
    }

    require(source_gpu_id < device_count && recorder_gpu_id < device_count,
            "configured source/recorder GPU is outside the CUDA device range");
    const std::vector<unsigned char> source_bytes = {
        0, 1, 2, 3,
        4, 5, 6, 7,
        8, 9, 10, 11,
        12, 13, 14, 15};
    GpuSource source = make_source(source_gpu_id, source_bytes);
    const SpatialRoiFrameDescriptor descriptor =
        make_descriptor(source_gpu_id, recorder_gpu_id);
    const SpatialRoiIpcFrame frame = make_frame(descriptor, source);
    std::string serialization_error;
    const std::string wire =
        orange::spatial_roi::ipc::serialize_spatial_roi_ipc_message(
            orange::spatial_roi::ipc::SpatialRoiIpcMessage(frame),
            &serialization_error);
    require(!wire.empty(), "producer FRAME serialization failed: " +
                              serialization_error);
    require(wire.back() == '\n',
            "serialized producer FRAME is missing its wire newline");
    require(wire.size() <= kTestWireLimit,
            "serialized producer FRAME exceeded bounded pipe record size");
    require(write_all(STDOUT_FILENO, wire.data(), wire.size()),
            "producer FRAME pipe write failed");

    // The supervisor sends this only after the recorder has imported, copied,
    // synchronized, closed its imports, and reported all checks.  The source
    // allocation/event therefore remain alive during the complete detach.
    std::string command;
    require(read_line(STDIN_FILENO, &command),
            "producer did not receive recorder result/lifetime command");
    require(command == "RELEASE" || command == "ABORT",
            "producer received unknown lifetime command: " + command);
    if (command == "RELEASE") {
        source.Release();
        write_line(STDOUT_FILENO, "PRODUCER_OK source_released_after_recorder_result");
    } else {
        write_line(STDOUT_FILENO, "PRODUCER_ABORTED");
    }
    return 0;
}

int run_timeout_producer(const int source_gpu_id, const int recorder_gpu_id)
{
    int device_count = 0;
    const cudaError_t device_status = cudaGetDeviceCount(&device_count);
    if (device_status != cudaSuccess || device_count <= 0) {
        write_line(STDOUT_FILENO,
                   std::string("SKIP ") +
                       (device_status == cudaSuccess
                            ? "no CUDA device"
                            : cudaGetErrorString(device_status)));
        (void)cudaGetLastError();
        return 0;
    }

    require(source_gpu_id < device_count && recorder_gpu_id < device_count,
            "configured timeout source/recorder GPU is outside the CUDA device range");
    const std::vector<unsigned char> source_bytes = {
        0, 1, 2, 3,
        4, 5, 6, 7,
        8, 9, 10, 11,
        12, 13, 14, 15};
    HostGate gate;
    GpuSource source = make_pending_source(source_gpu_id, source_bytes, &gate);
    const SpatialRoiFrameDescriptor descriptor =
        make_descriptor(source_gpu_id, recorder_gpu_id);
    const SpatialRoiIpcFrame frame = make_frame(descriptor, source);
    std::string serialization_error;
    const std::string wire =
        orange::spatial_roi::ipc::serialize_spatial_roi_ipc_message(
            orange::spatial_roi::ipc::SpatialRoiIpcMessage(frame),
            &serialization_error);
    require(!wire.empty(), "timeout producer FRAME serialization failed: " +
                              serialization_error);
    require(wire.back() == '\n',
            "serialized timeout FRAME is missing its wire newline");
    require(wire.size() <= kTestWireLimit,
            "serialized timeout FRAME exceeded bounded pipe record size");
    require(write_all(STDOUT_FILENO, wire.data(), wire.size()),
            "timeout producer FRAME pipe write failed");

    // The supervisor sends OPEN only after it has reaped the recorder.  This
    // ordering lets the recorder retain the unresolved imported mapping for
    // process lifetime without racing producer stream completion/free.
    std::string command;
    require(read_line(STDIN_FILENO, &command),
            "timeout producer did not receive gate command");
    require(command == "OPEN" || command == "ABORT",
            "timeout producer received unknown gate command: " + command);
    if (command == "OPEN") {
        gate.open.store(true, std::memory_order_release);
        require_cuda(cudaStreamSynchronize(source.stream),
                     "cudaStreamSynchronize(timeout producer after OPEN)");
        source.Release();
        write_line(STDOUT_FILENO,
                   "TIMEOUT_PRODUCER_OK gate_opened_after_recorder_exit");
    } else {
        // A recorder failure still must not leave the callback spinning at
        // process teardown; open it before releasing the source resources.
        gate.open.store(true, std::memory_order_release);
        (void)cudaStreamSynchronize(source.stream);
        source.Release();
        write_line(STDOUT_FILENO, "TIMEOUT_PRODUCER_ABORTED");
    }
    return 0;
}

int run_recorder(const bool timeout_mode)
{
    int device_count = 0;
    const cudaError_t device_status = cudaGetDeviceCount(&device_count);
    if (device_status != cudaSuccess || device_count <= 0) {
        write_line(STDOUT_FILENO,
                   std::string("SKIP ") +
                       (device_status == cudaSuccess
                            ? "no CUDA device"
                            : cudaGetErrorString(device_status)));
        (void)cudaGetLastError();
        return 0;
    }

    std::string wire;
    require(read_line(STDIN_FILENO, &wire),
            "recorder did not receive producer FRAME");
    orange::spatial_roi::ipc::SpatialRoiIpcMessage message;
    std::string parse_error;
    require(orange::spatial_roi::ipc::parse_spatial_roi_ipc_message(
                wire, &message, &parse_error),
            "recorder FRAME parse failed: " + parse_error);
    const auto* frame = std::get_if<SpatialRoiIpcFrame>(&message);
    require(frame != nullptr, "recorder received a non-FRAME IPC message");

    if (timeout_mode) {
        run_timeout_recorder_checks(*frame);
        write_line(STDOUT_FILENO,
                   "PASS timeout_pending_event quarantine source_release_unsafe "
                   "bounded_wall_clock");
    } else {
        run_recorder_checks(*frame);
        write_line(STDOUT_FILENO,
                   "PASS separate_process mono8_exact nv12_uv_128 pool_reuse "
                   "identity_rejection source_quarantine");
    }
    return 0;
}

std::string self_executable_path()
{
    std::vector<char> path(4096, '\0');
    const ssize_t length = readlink("/proc/self/exe", path.data(), path.size() - 1);
    require(length > 0, "readlink(/proc/self/exe) failed");
    path[static_cast<std::size_t>(length)] = '\0';
    return std::string(path.data(), static_cast<std::size_t>(length));
}

pid_t spawn_mode(const std::string& executable,
                 const char* mode,
                 const int input_read,
                 const int output_write,
                 const int* close_fds,
                 const std::size_t close_fd_count,
                 const char* argument_1 = nullptr,
                 const char* argument_2 = nullptr)
{
    // Resolve the string pointer before fork.  The child then performs only
    // dup2/close/exec calls, which keeps this fork+exec seam safe even if the
    // supervisor later grows other threads.
    const char* executable_path = executable.c_str();
    const pid_t pid = fork();
    require(pid >= 0, "fork failed");
    if (pid == 0) {
        // Child code is async-signal-safe until exec.  CUDA is initialized only
        // by the exec'd mode process.
        if (dup2(input_read, STDIN_FILENO) < 0 ||
            dup2(output_write, STDOUT_FILENO) < 0) {
            _exit(126);
        }
        for (std::size_t index = 0; index < close_fd_count; ++index) {
            const int fd = close_fds[index];
            if (fd != STDIN_FILENO && fd != STDOUT_FILENO) {
                close(fd);
            }
        }
        if (argument_1 && argument_2) {
            execl(executable_path,
                  executable_path,
                  mode,
                  argument_1,
                  argument_2,
                  static_cast<char*>(nullptr));
        } else {
            execl(executable_path,
                  executable_path,
                  mode,
                  static_cast<char*>(nullptr));
        }
        _exit(127);
    }
    return pid;
}

int wait_child(const pid_t pid, const char* name)
{
    int status = 0;
    while (waitpid(pid, &status, 0) < 0) {
        if (errno == EINTR) {
            continue;
        }
        throw std::runtime_error(std::string("waitpid(") + name + ") failed");
    }
    if (WIFEXITED(status)) {
        return WEXITSTATUS(status);
    }
    if (WIFSIGNALED(status)) {
        throw std::runtime_error(std::string(name) + " terminated by signal " +
                                 std::to_string(WTERMSIG(status)));
    }
    throw std::runtime_error(std::string(name) + " did not exit normally");
}

struct ExchangeOutcome {
    bool skipped = false;
    std::string producer_record;
    std::string recorder_record;
    std::string producer_result;
};

ExchangeOutcome run_exchange(const std::string& executable,
                             const char* producer_mode,
                             const char* recorder_mode,
                             const char* release_command,
                             const char* producer_success_prefix,
                             const int source_gpu_id,
                             const int recorder_gpu_id)
{
    const std::string source_gpu_arg = std::to_string(source_gpu_id);
    const std::string recorder_gpu_arg = std::to_string(recorder_gpu_id);
    int producer_frame[2] = {-1, -1};
    int producer_control[2] = {-1, -1};
    require(pipe(producer_frame) == 0 && pipe(producer_control) == 0,
            "producer pipe creation failed");
    const std::array<int, 4> producer_close_fds = {
        producer_frame[0], producer_control[1],
        producer_control[0], producer_frame[1]};
    const pid_t producer_pid = spawn_mode(executable,
                                          producer_mode,
                                          producer_control[0],
                                          producer_frame[1],
                                          producer_close_fds.data(),
                                          producer_close_fds.size(),
                                          source_gpu_arg.c_str(),
                                          recorder_gpu_arg.c_str());
    close(producer_control[0]);
    close(producer_frame[1]);
    producer_control[0] = -1;
    producer_frame[1] = -1;

    ExchangeOutcome outcome;
    require(read_line(producer_frame[0], &outcome.producer_record),
            "producer exited without a bounded FRAME/SKIP record");
    if (outcome.producer_record.rfind("SKIP ", 0) == 0) {
        close(producer_frame[0]);
        close(producer_control[1]);
        const int producer_status = wait_child(producer_pid, "producer");
        require(producer_status == 0, "producer SKIP mode failed");
        outcome.skipped = true;
        return outcome;
    }

    int recorder_input[2] = {-1, -1};
    int recorder_result[2] = {-1, -1};
    require(pipe(recorder_input) == 0 && pipe(recorder_result) == 0,
            "recorder pipe creation failed");
    const std::array<int, 6> recorder_close_fds = {
        recorder_input[0], recorder_result[1],
        recorder_input[1], recorder_result[0],
        producer_control[1], producer_frame[0]};
    const pid_t recorder_pid = spawn_mode(executable,
                                          recorder_mode,
                                          recorder_input[0],
                                          recorder_result[1],
                                          recorder_close_fds.data(),
                                          recorder_close_fds.size());
    close(recorder_input[0]);
    close(recorder_result[1]);
    recorder_input[0] = -1;
    recorder_result[1] = -1;
    write_line(recorder_input[1], outcome.producer_record);
    close(recorder_input[1]);
    recorder_input[1] = -1;

    const bool got_recorder_record =
        read_line(recorder_result[0], &outcome.recorder_record);
    close(recorder_result[0]);
    recorder_result[0] = -1;
    // Reap the recorder before releasing/opening the producer.  The fault and
    // timeout paths intentionally retain imported mappings in a process-level
    // quarantine, so producer cudaFree must wait for recorder process exit.
    const int recorder_status = wait_child(recorder_pid, "recorder");

    const bool recorder_skipped =
        got_recorder_record && outcome.recorder_record.rfind("SKIP ", 0) == 0;
    const char* command = recorder_skipped ? "ABORT\n" : release_command;
    if (!got_recorder_record) {
        // Never release/open the producer as though a missing recorder result
        // represented successful completion.
        command = "ABORT\n";
    }
    require(write_all(producer_control[1], command, std::strlen(command)),
            "producer lifetime command write failed");
    close(producer_control[1]);
    producer_control[1] = -1;

    const bool got_producer_result =
        read_line(producer_frame[0], &outcome.producer_result);
    close(producer_frame[0]);
    producer_frame[0] = -1;
    require(got_producer_result,
            "producer exited without a lifetime result record");
    const int producer_status = wait_child(producer_pid, "producer");
    require(recorder_status == 0,
            "recorder child failed; result: " + outcome.recorder_record);
    require(producer_status == 0,
            "producer child failed after recorder result");
    require(got_recorder_record,
            "recorder exited without a bounded result record");
    if (recorder_skipped) {
        outcome.skipped = true;
        return outcome;
    }
    require(outcome.recorder_record.rfind("PASS ", 0) == 0,
            "recorder checks failed: " + outcome.recorder_record);
    require(outcome.producer_result.rfind(producer_success_prefix, 0) == 0,
            "producer did not complete after recorder result: " +
                outcome.producer_result);
    return outcome;
}

bool cuda_ipc_device_usable(const int gpu_id)
{
    int unified_addressing = 0;
    if (cudaDeviceGetAttribute(&unified_addressing,
                               cudaDevAttrUnifiedAddressing,
                               gpu_id) != cudaSuccess ||
        unified_addressing == 0) {
        return false;
    }
    int ipc_event_support = 0;
    return cudaDeviceGetAttribute(&ipc_event_support,
                                  cudaDevAttrIpcEventSupport,
                                  gpu_id) == cudaSuccess &&
           ipc_event_support != 0;
}

struct TestGpuPair {
    int source_gpu_id = -1;
    int recorder_gpu_id = -1;

    bool valid() const noexcept
    {
        return source_gpu_id >= 0 && recorder_gpu_id >= 0;
    }
};

int find_same_gpu_control(const int device_count)
{
    for (int gpu_id = 0; gpu_id < device_count; ++gpu_id) {
        if (cuda_ipc_device_usable(gpu_id)) {
            return gpu_id;
        }
    }
    return -1;
}

TestGpuPair find_distinct_peer_pair(const int device_count)
{
    for (int source_gpu_id = 0; source_gpu_id < device_count; ++source_gpu_id) {
        if (!cuda_ipc_device_usable(source_gpu_id)) {
            continue;
        }
        for (int recorder_gpu_id = 0;
             recorder_gpu_id < device_count;
             ++recorder_gpu_id) {
            if (recorder_gpu_id == source_gpu_id ||
                !cuda_ipc_device_usable(recorder_gpu_id)) {
                continue;
            }
            int recorder_can_access_source = 0;
            if (cudaDeviceCanAccessPeer(&recorder_can_access_source,
                                        recorder_gpu_id,
                                        source_gpu_id) == cudaSuccess &&
                recorder_can_access_source != 0) {
                return {source_gpu_id, recorder_gpu_id};
            }
        }
    }
    return {};
}

int run_supervisor()
{
    (void)signal(SIGPIPE, SIG_IGN);
    int device_count = 0;
    const cudaError_t device_status = cudaGetDeviceCount(&device_count);
    if (device_status != cudaSuccess || device_count <= 0) {
        std::cout << "[SKIP] spatial ROI recorder CUDA detach tests: "
                  << (device_status == cudaSuccess
                          ? "no CUDA device"
                          : cudaGetErrorString(device_status))
                  << '\n';
        (void)cudaGetLastError();
        return 0;
    }
    const int control_gpu_id = find_same_gpu_control(device_count);
    if (control_gpu_id < 0) {
        std::cout << "[SKIP] spatial ROI recorder CUDA detach tests: "
                     "no CUDA device supports unified addressing and IPC events\n";
        return 0;
    }

    const std::string executable = self_executable_path();
    const ExchangeOutcome success = run_exchange(executable,
                                                 "--producer",
                                                 "--recorder",
                                                 "RELEASE\n",
                                                 "PRODUCER_OK ",
                                                 control_gpu_id,
                                                 control_gpu_id);
    if (success.skipped) {
        std::cout << "[SKIP] spatial ROI recorder CUDA detach tests: "
                  << success.producer_record.substr(5) << "\n";
        return 0;
    }

    const ExchangeOutcome timeout = run_exchange(executable,
                                                 "--timeout-producer",
                                                 "--recorder-timeout",
                                                 "OPEN\n",
                                                 "TIMEOUT_PRODUCER_OK ",
                                                 control_gpu_id,
                                                 control_gpu_id);
    if (timeout.skipped) {
        std::cout << "[SKIP] spatial ROI recorder CUDA detach tests: "
                  << timeout.producer_record.substr(5) << "\n";
        return 0;
    }
    const TestGpuPair cross_gpu = find_distinct_peer_pair(device_count);
    if (cross_gpu.valid()) {
        const ExchangeOutcome cross_gpu_success = run_exchange(
            executable,
            "--producer",
            "--recorder",
            "RELEASE\n",
            "PRODUCER_OK ",
            cross_gpu.source_gpu_id,
            cross_gpu.recorder_gpu_id);
        require(!cross_gpu_success.skipped,
                "selected peer-capable cross-GPU exchange was unexpectedly skipped");
        std::cout << "[PASS] spatial ROI cross-GPU CUDA IPC detach: source GPU "
                  << cross_gpu.source_gpu_id << " -> recorder GPU "
                  << cross_gpu.recorder_gpu_id << '\n';
    } else {
        std::cout << "[SKIP] spatial ROI cross-GPU CUDA IPC detach: "
                     "no distinct recorder-to-source peer-capable GPU pair\n";
    }
    std::cout << "spatial_roi_recorder_cuda_detach_tests: "
                 "same-GPU IPC, bounded-timeout, and available cross-GPU checks passed\n";
    return 0;
}

int run_requested_cross_gpu(const int source_gpu_id,
                            const int recorder_gpu_id)
{
    (void)signal(SIGPIPE, SIG_IGN);
    int device_count = 0;
    require_cuda(cudaGetDeviceCount(&device_count), "cudaGetDeviceCount");
    require(source_gpu_id >= 0 && source_gpu_id < device_count &&
                recorder_gpu_id >= 0 && recorder_gpu_id < device_count,
            "requested cross-GPU pair is outside the CUDA device range");
    require(source_gpu_id != recorder_gpu_id,
            "requested cross-GPU pair must use distinct devices");
    require(cuda_ipc_device_usable(source_gpu_id) &&
                cuda_ipc_device_usable(recorder_gpu_id),
            "requested cross-GPU pair does not support CUDA IPC events and unified addressing");
    int recorder_can_access_source = 0;
    require_cuda(cudaDeviceCanAccessPeer(&recorder_can_access_source,
                                         recorder_gpu_id,
                                         source_gpu_id),
                 "cudaDeviceCanAccessPeer(requested cross-GPU pair)");
    require(recorder_can_access_source != 0,
            "requested recorder GPU cannot access the source GPU as a peer");

    const ExchangeOutcome outcome = run_exchange(self_executable_path(),
                                                  "--producer",
                                                  "--recorder",
                                                  "RELEASE\n",
                                                  "PRODUCER_OK ",
                                                  source_gpu_id,
                                                  recorder_gpu_id);
    require(!outcome.skipped,
            "requested peer-capable cross-GPU exchange was unexpectedly skipped");
    std::cout << "[PASS] spatial ROI requested cross-GPU CUDA IPC detach: source GPU "
              << source_gpu_id << " -> recorder GPU " << recorder_gpu_id
              << '\n';
    return 0;
}

}  // namespace

int main(const int argc, char** argv)
{
    try {
        if (argc == 4 && std::strcmp(argv[1], "--producer") == 0) {
            return run_producer(parse_gpu_id_arg(argv[2], "source GPU id"),
                                parse_gpu_id_arg(argv[3], "recorder GPU id"));
        }
        if (argc == 2 && std::strcmp(argv[1], "--recorder") == 0) {
            return run_recorder(false);
        }
        if (argc == 2 && std::strcmp(argv[1], "--recorder-timeout") == 0) {
            return run_recorder(true);
        }
        if (argc == 4 && std::strcmp(argv[1], "--timeout-producer") == 0) {
            return run_timeout_producer(
                parse_gpu_id_arg(argv[2], "timeout source GPU id"),
                parse_gpu_id_arg(argv[3], "timeout recorder GPU id"));
        }
        if (argc == 4 && std::strcmp(argv[1], "--cross-gpu") == 0) {
            return run_requested_cross_gpu(
                parse_gpu_id_arg(argv[2], "source GPU id"),
                parse_gpu_id_arg(argv[3], "recorder GPU id"));
        }
        if (argc != 1) {
            std::cerr << "usage: spatial_roi_recorder_cuda_detach_tests "
                         "[--cross-gpu <source-gpu> <recorder-gpu>]\n";
            return 2;
        }
        return run_supervisor();
    } catch (const std::exception& exception) {
        std::cerr << "[FAIL] " << exception.what() << "\n";
        return 1;
    }
}
