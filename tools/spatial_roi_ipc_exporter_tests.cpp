#include "spatial_roi_ipc_exporter.h"

#include "session/spatial_roi_recording_config.h"
#include "shaman_v2_recording_identity.h"

#include <cuda_runtime.h>

#include <algorithm>
#include <cstdint>
#include <iostream>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace {

namespace api = orange::session::spatial_roi;
using orange::spatial_roi::SpatialRoiBatchEnvelope;
using orange::spatial_roi::SpatialRoiFrameIdentity;
using orange::spatial_roi::SpatialRoiLaneDelivery;
using orange::spatial_roi::SpatialRoiLaneSinkResult;
using orange::spatial_roi::SpatialRoiRecordingRuntime;
using orange::spatial_roi::SpatialRoiSourcePixelFormat;
using orange::spatial_roi::SpatialRoiSourceView;
using orange::spatial_roi::ipc::SpatialRoiIpcExport;
using orange::spatial_roi::ipc::SpatialRoiIpcFrameExporter;

static_assert(!std::is_default_constructible_v<SpatialRoiLaneDelivery>,
              "lane deliveries must come only from an admitted runtime lane");
static_assert(
    !std::is_constructible_v<
        SpatialRoiLaneDelivery,
        std::size_t,
        std::uint64_t,
        std::shared_ptr<const SpatialRoiBatchEnvelope>>,
    "callers must not manufacture lane/index/envelope combinations");
static_assert(
    !std::is_assignable_v<decltype(SpatialRoiLaneDelivery::roi_stream_frame_index)&,
                          std::uint64_t>,
    "lane-assigned stream indexes must be immutable");

void require(bool condition, const std::string& message)
{
    if (!condition) {
        throw std::runtime_error(message);
    }
}

void require_cuda(cudaError_t status, const char* operation)
{
    if (status != cudaSuccess) {
        throw std::runtime_error(
            std::string(operation) + " failed: " + cudaGetErrorString(status));
    }
}

api::Config make_config()
{
    api::Config config = api::default_config();
    config.enabled = true;
    config.output_alignment_px = 2;
    config.buffering.pool_frames_per_stream = 4;
    config.buffering.queue_frames_per_stream = 2;
    config.admission.max_rois_per_camera = 8;
    config.admission.max_total_rois = 8;
    config.admission.max_total_encoder_streams = 8;
    config.admission.max_total_pixel_rate = 1000000;
    config.admission.max_total_pool_bytes = 1000000;
    config.admission.max_total_queue_frames = 1000000;

    api::CameraConfig camera;
    camera.camera_id = 3;
    camera.camera_serial = "10000";
    camera.native_raster = {8, 6};
    camera.source_frame_rate = 30;
    camera.arena_group_id = "arena_group_1";
    camera.layout = {"layout_1", "sha256:" + std::string(64, '1')};
    camera.materialization = {
        "materialization_1", "sha256:" + std::string(64, '2')};
    camera.registration = {
        "registration_1", "sha256:" + std::string(64, '3')};

    const std::vector<api::Rect> rectangles = {
        {0, 0, 2, 2},
        {2, 0, 2, 2},
        {0, 2, 2, 2},
        {2, 2, 2, 2},
    };
    for (std::size_t index = 0; index < rectangles.size(); ++index) {
        api::RoiConfig roi;
        roi.roi_id = "roi_" + std::to_string(index + 1);
        roi.region_id = "region_" + std::to_string(index + 1);
        roi.content_rect = rectangles[index];
        roi.logical_stream_id =
            api::expected_logical_stream_id(camera.camera_serial, roi.roi_id);
        roi.artifact_stem =
            api::expected_artifact_stem(camera.camera_serial, roi.roi_id);
        camera.rois.push_back(std::move(roi));
    }
    config.cameras.emplace(camera.camera_serial, std::move(camera));
    return config;
}

nlohmann::json make_plan()
{
    api::PlanContext context;
    context.recording_id = "ipc-exporter-test-recording";
    context.recording_identity_token =
        orange::shaman_v2_recording_identity::token_for_recording_id(
            context.recording_id);
    context.generated_at_utc = "2026-08-31T00:00:00Z";
    context.producer_generation = "generation_1";

    nlohmann::json plan;
    std::string error;
    require(api::build_plan(
                make_config(), context, &plan, nullptr, &error),
            "build_plan failed: " + error);
    require(api::verify_plan(plan, &error), "verify_plan failed: " + error);
    return plan;
}

void test_host_rejections()
{
    SpatialRoiIpcFrameExporter malformed(nlohmann::json::object(), "10000", 0);
    require(!malformed.valid(), "malformed verified plan was accepted");

    const nlohmann::json plan = make_plan();
    SpatialRoiIpcFrameExporter exporter(plan, "10000", 0);
    require(exporter.valid(), "valid verified plan was rejected: " + exporter.error());

    SpatialRoiIpcFrameExporter wrong_camera(plan, "not_the_camera", 0);
    require(!wrong_camera.valid(),
            "exporter accepted a camera not present in the verified plan");

    nlohmann::json tampered = plan;
    tampered["plan"]["cameras"]["10000"]["rois"][0]["region_id"] =
        "unbound_region";
    SpatialRoiIpcFrameExporter tampered_plan(tampered, "10000", 0);
    require(!tampered_plan.valid(),
            "tampered verified-plan identity was accepted by the exporter");
}

struct GpuSource {
    unsigned char* device_data = nullptr;
    std::size_t pitch_bytes = 0;
    cudaStream_t stream = nullptr;
    cudaEvent_t ready_event = nullptr;

    GpuSource() = default;
    GpuSource(GpuSource&& other) noexcept
        : device_data(other.device_data),
          pitch_bytes(other.pitch_bytes),
          stream(other.stream),
          ready_event(other.ready_event)
    {
        other.device_data = nullptr;
        other.pitch_bytes = 0;
        other.stream = nullptr;
        other.ready_event = nullptr;
    }

    ~GpuSource()
    {
        if (ready_event) {
            (void)cudaEventDestroy(ready_event);
        }
        if (stream) {
            (void)cudaStreamDestroy(stream);
        }
        if (device_data) {
            (void)cudaFree(device_data);
        }
    }
    GpuSource(const GpuSource&) = delete;
    GpuSource& operator=(const GpuSource&) = delete;
};

GpuSource make_source()
{
    GpuSource source;
    require_cuda(cudaMallocPitch(reinterpret_cast<void**>(&source.device_data),
                                 &source.pitch_bytes,
                                 8,
                                 6),
                 "cudaMallocPitch(source)");
    require_cuda(cudaStreamCreateWithFlags(&source.stream, cudaStreamNonBlocking),
                 "cudaStreamCreateWithFlags(source)");
    require_cuda(cudaEventCreateWithFlags(&source.ready_event, cudaEventDisableTiming),
                 "cudaEventCreateWithFlags(source)");
    require_cuda(cudaMemset2DAsync(source.device_data,
                                   source.pitch_bytes,
                                   0x2a,
                                   8,
                                   6,
                                   source.stream),
                 "cudaMemset2DAsync(source)");
    require_cuda(cudaEventRecord(source.ready_event, source.stream),
                 "cudaEventRecord(source)");
    return source;
}

SpatialRoiSourceView make_source_view(const GpuSource& source)
{
    SpatialRoiSourceView view;
    view.device_data = source.device_data;
    view.pitch_bytes = source.pitch_bytes;
    view.allocation_bytes = source.pitch_bytes * 6;
    view.width = 8;
    view.height = 6;
    view.gpu_id = 0;
    view.pixel_format = SpatialRoiSourcePixelFormat::kMono8;
    view.source_lease = std::make_shared<int>(0);
    view.ready_event = source.ready_event;
    view.identity.recording_id = "ipc-exporter-test-recording";
    view.identity.recording_identity_token =
        orange::shaman_v2_recording_identity::token_for_recording_id(
            view.identity.recording_id);
    view.identity.producer_generation = "generation_1";
    view.identity.camera_id = 3;
    view.identity.camera_serial = "10000";
    view.identity.local_frame_id = 17;
    view.identity.camera_frame_id = 71;
    view.identity.recording_frame_id = 9;
    view.identity.camera_timestamp_ns = 123456789;
    view.identity.timestamp_sys_ns = 987654321;
    return view;
}

void test_completed_gpu_export()
{
    int device_count = 0;
    const cudaError_t count_status = cudaGetDeviceCount(&device_count);
    if (count_status != cudaSuccess || device_count == 0) {
        std::cout << "[SKIP] spatial ROI IPC exporter CUDA handle test: "
                  << (count_status == cudaSuccess
                          ? "no CUDA device"
                          : cudaGetErrorString(count_status))
                  << '\n';
        (void)cudaGetLastError();
        return;
    }

    GpuSource source = make_source();
    SpatialRoiIpcFrameExporter exporter(make_plan(), "10000", 0);
    require(exporter.valid(), "GPU exporter plan setup failed: " + exporter.error());

    std::mutex mutex;
    std::vector<SpatialRoiIpcExport> exports;
    std::vector<std::string> errors;
    {
        SpatialRoiRecordingRuntime runtime(
            make_plan(),
            "10000",
            0,
            [&](const SpatialRoiLaneDelivery& delivery) {
                SpatialRoiIpcExport exported;
                std::string error;
                const bool success = exporter.Build(delivery,
                                                    0,
                                                    static_cast<int>(delivery.lane_index),
                                                    &exported,
                                                    &error);
                std::lock_guard<std::mutex> lock(mutex);
                if (!success) {
                    errors.push_back(error);
                    return SpatialRoiLaneSinkResult::kFailed;
                }
                exports.push_back(std::move(exported));
                return SpatialRoiLaneSinkResult::kCompleted;
            });

        const auto submission = runtime.TrySubmit(make_source_view(source));
        require(submission.producer_accepted(),
                "GPU source was not admitted for exporter test");
        runtime.StopAcceptingAndDrain();
    }

    require(errors.empty(),
            errors.empty() ? "" : "GPU exporter failed: " + errors.front());
    require(exports.size() == 4,
            "exporter did not produce one FRAME for each verified ROI lane");
    std::sort(
        exports.begin(),
        exports.end(),
        [](const SpatialRoiIpcExport& lhs, const SpatialRoiIpcExport& rhs) {
            return lhs.frame.descriptor.assigned_shard_id <
                   rhs.frame.descriptor.assigned_shard_id;
        });
    for (std::size_t index = 0; index < exports.size(); ++index) {
        const auto& exported = exports[index];
        require(static_cast<bool>(exported.envelope),
                "successful export did not retain its batch envelope");
        require(exported.frame.descriptor.roi_stream_frame_index == 1,
                "exporter changed the lane-assigned stream frame index");
        require(exported.frame.descriptor.assigned_shard_id ==
                    static_cast<int>(index),
                "exporter lost recorder shard assignment");
        require(exported.frame.cuda_buffer.memory_handle_hex.size() == 128,
                "exported memory handle is not fixed-size hex");
        require(exported.frame.cuda_buffer.ready_event_handle_hex.size() == 128,
                "exported event handle is not fixed-size hex");
        require(exported.frame.cuda_buffer.memory_handle_hex.find_first_not_of(
                    "0123456789abcdef") == std::string::npos,
                "exported memory handle is not lower-case canonical hex");
        require(exported.frame.cuda_buffer.ready_event_handle_hex.find_first_not_of(
                    "0123456789abcdef") == std::string::npos,
                "exported event handle is not lower-case canonical hex");
        std::string error;
        require(validate_spatial_roi_ipc_frame(exported.frame, &error),
                "exporter produced an invalid protocol frame: " + error);
    }

    // Dropping every exported envelope is the handoff-side lifetime boundary;
    // this occurs before the source fixture's CUDA allocation is destroyed.
    exports.clear();
}

}  // namespace

int main()
{
    try {
        test_host_rejections();
        test_completed_gpu_export();
    } catch (const std::exception& exception) {
        std::cerr << "[FAIL] spatial ROI IPC exporter tests: "
                  << exception.what() << '\n';
        return 1;
    }
    std::cout << "[PASS] spatial ROI IPC exporter tests\n";
    return 0;
}
