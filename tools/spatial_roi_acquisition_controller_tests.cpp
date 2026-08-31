#include "spatial_roi_acquisition_controller.h"

#include "camera.h"
#include "session/spatial_roi_recording_config.h"
#include "shaman_v2_recording_identity.h"
#include "video_capture.h"
#include "worker_entry_release.h"

#include <cuda_runtime.h>

#include <atomic>
#include <cstdint>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>

namespace {

using orange::spatial_roi::SpatialRoiAcquisitionArmStatus;
using orange::spatial_roi::SpatialRoiAcquisitionController;
using orange::spatial_roi::SpatialRoiAcquisitionControllerSubmitStatus;
using orange::spatial_roi::SpatialRoiAcquisitionSession;
using orange::spatial_roi::SpatialRoiAcquisitionBridgeStatus;
using orange::spatial_roi::SpatialRoiRecordingRuntime;

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

nlohmann::json make_plan(
    const std::string& producer_generation = "controller-generation")
{
    namespace api = orange::session::spatial_roi;
    api::Config config = api::default_config();
    config.enabled = true;
    config.buffering.pool_frames_per_stream = 3;
    config.buffering.queue_frames_per_stream = 2;
    config.admission.max_rois_per_camera = 4;
    config.admission.max_total_rois = 4;
    config.admission.max_total_encoder_streams = 4;
    config.admission.max_total_pixel_rate = 1000000;
    config.admission.max_total_pool_bytes = 1000000;
    config.admission.max_total_queue_frames = 1000000;

    api::CameraConfig camera;
    camera.camera_id = 3;
    camera.camera_serial = "controller-camera";
    camera.native_raster = {8, 6};
    camera.source_frame_rate = 30;
    camera.arena_group_id = "controller-group";
    camera.layout = {"layout", "sha256:" + std::string(64, '1')};
    camera.materialization = {
        "materialization", "sha256:" + std::string(64, '2')};
    camera.registration = {
        "registration", "sha256:" + std::string(64, '3')};

    api::RoiConfig roi;
    roi.roi_id = "roi_1";
    roi.region_id = "region_1";
    roi.content_rect = {0, 0, 4, 4};
    roi.logical_stream_id =
        api::expected_logical_stream_id(camera.camera_serial, roi.roi_id);
    roi.artifact_stem =
        api::expected_artifact_stem(camera.camera_serial, roi.roi_id);
    camera.rois.push_back(std::move(roi));
    config.cameras.emplace(camera.camera_serial, std::move(camera));

    api::PlanContext context;
    context.recording_id = "controller-recording";
    context.recording_identity_token =
        orange::shaman_v2_recording_identity::token_for_recording_id(
            context.recording_id);
    context.generated_at_utc = "2026-08-31T00:00:00Z";
    context.producer_generation = producer_generation;

    nlohmann::json plan;
    std::string error;
    require(api::build_plan(config, context, &plan, nullptr, &error), error);
    require(api::verify_plan(plan, &error), error);
    return plan;
}

SpatialRoiAcquisitionSession session_for_runtime(
    const std::shared_ptr<SpatialRoiRecordingRuntime>& runtime)
{
    const auto& limits = runtime->producer_limits();
    SpatialRoiAcquisitionSession session;
    session.runtime = runtime;
    session.recording_id = limits.expected_recording_id;
    session.recording_identity_token = limits.expected_recording_identity_token;
    session.producer_generation = limits.expected_producer_generation;
    session.camera_id = limits.expected_camera_id;
    session.camera_serial = limits.expected_camera_serial;
    return session;
}

struct Source final {
    unsigned char* device_data = nullptr;
    cudaStream_t stream = nullptr;
    cudaEvent_t ready_event = nullptr;

    Source() = default;
    Source(const Source&) = delete;
    Source& operator=(const Source&) = delete;
    Source(Source&& other) noexcept
        : device_data(other.device_data),
          stream(other.stream),
          ready_event(other.ready_event)
    {
        other.device_data = nullptr;
        other.stream = nullptr;
        other.ready_event = nullptr;
    }
    Source& operator=(Source&& other) noexcept
    {
        if (this != &other) {
            std::swap(device_data, other.device_data);
            std::swap(stream, other.stream);
            std::swap(ready_event, other.ready_event);
        }
        return *this;
    }
    ~Source()
    {
        if (ready_event) {
            cudaEventDestroy(ready_event);
        }
        if (stream) {
            cudaStreamDestroy(stream);
        }
        if (device_data) {
            cudaFree(device_data);
        }
    }
};

Source make_source()
{
    Source source;
    require_cuda(cudaMalloc(
                     reinterpret_cast<void**>(&source.device_data), 8 * 6),
                 "cudaMalloc(source)");
    require_cuda(cudaStreamCreateWithFlags(
                     &source.stream, cudaStreamNonBlocking),
                 "cudaStreamCreateWithFlags(source)");
    require_cuda(cudaEventCreateWithFlags(
                     &source.ready_event, cudaEventDisableTiming),
                 "cudaEventCreateWithFlags(source)");
    require_cuda(cudaMemsetAsync(
                     source.device_data, 0x2a, 8 * 6, source.stream),
                 "cudaMemsetAsync(source)");
    require_cuda(cudaEventRecord(source.ready_event, source.stream),
                 "cudaEventRecord(source)");
    return source;
}

void initialize_entry(
    WORKER_ENTRY* entry,
    const Source& source,
    std::uint64_t recording_frame)
{
    entry->d_image = source.device_data;
    entry->event_ptr = const_cast<cudaEvent_t*>(&source.ready_event);
    entry->width = 8;
    entry->height = 6;
    entry->pixelFormat = GVSP_PIX_MONO8;
    entry->timestamp = 123456789 + recording_frame;
    entry->frame_id = 17 + recording_frame;
    entry->camera_frame_id = 71 + recording_frame;
    entry->recording_frame_id = recording_frame;
    entry->source_buffer_bytes = 8 * 6;
    entry->timestamp_sys = 987654321 + recording_frame;
    entry->image_gpu_id = 0;
    entry->gpu_direct_mode = false;
    entry->owns_memory = true;
    entry->ref_count.store(1);
}

void test_default_off()
{
    SpatialRoiAcquisitionController controller;
    static_assert(noexcept(controller.Disarm()),
                  "Disarm must be an allocation-free noexcept operation");
    require(!controller.armed(), "new controller is armed");
    const auto result = controller.TrySubmit(nullptr, nullptr, {});
    require(result.status == SpatialRoiAcquisitionControllerSubmitStatus::kDisarmed,
            "disarmed controller accepted a submission");
    require(result.bridge_result.bridge_status ==
                SpatialRoiAcquisitionBridgeStatus::kMissingRuntime,
            "disarmed result lost its explicit missing-runtime status");
    require(!controller.Disarm(), "disarming an idle controller changed state");
    require(controller.Drain(), "drain of idle controller failed");
}

void test_runtime_cases()
{
    const nlohmann::json plan = make_plan();
    auto runtime = std::make_shared<SpatialRoiRecordingRuntime>(
        plan,
        "controller-camera",
        0,
        [](std::size_t, std::shared_ptr<const orange::spatial_roi::SpatialRoiBatchEnvelope>) {
            return orange::spatial_roi::SpatialRoiLaneSinkResult::kCompleted;
        });

    {
        SpatialRoiAcquisitionController controller;
        SpatialRoiAcquisitionSession mismatch = session_for_runtime(runtime);
        mismatch.camera_id += 1;
        auto arm = controller.Arm(std::move(mismatch));
        require(arm.status == SpatialRoiAcquisitionArmStatus::kIdentityMismatch,
                "controller armed an identity-mismatched runtime");
        require(!controller.armed(), "failed arm left controller armed");
    }

    Source source = make_source();
    WORKER_ENTRY entry{};
    initialize_entry(&entry, source, 1);
    SafeQueue<WORKER_ENTRY*> recycle_queue;
    WorkerEntryReleaseContext context{
        "controller-camera", "spatial_roi_controller_tests"};
    {
        SpatialRoiAcquisitionController controller;
        auto arm = controller.Arm(session_for_runtime(runtime));
        require(arm.armed(), "valid runtime/session did not arm");
        auto replacement = controller.Arm(session_for_runtime(runtime));
        require(replacement.status ==
                    SpatialRoiAcquisitionArmStatus::kAlreadyArmed,
                "controller replaced an already armed session");

        auto submitted =
            controller.TrySubmit(&entry, &recycle_queue, context);
        require(submitted.status ==
                    SpatialRoiAcquisitionControllerSubmitStatus::kSubmitted,
                std::string("valid controller submission was not accepted: controller=") +
                    orange::spatial_roi::
                        spatial_roi_acquisition_controller_submit_status_name(
                            submitted.status) +
                    " bridge=" +
                    orange::spatial_roi::spatial_roi_acquisition_bridge_status_name(
                        submitted.bridge_result.bridge_status) +
                    " error=" + submitted.bridge_result.error);
        require(static_cast<bool>(
                    submitted.bridge_result.runtime_submission.envelope),
                "accepted controller submission lost its envelope");
        const auto& work_item = submitted.bridge_result.runtime_submission
                                    .envelope->work_items().front();
        require(work_item.source.recording_id == "controller-recording" &&
                    work_item.source.camera_id == 3 &&
                    work_item.source.recording_frame_id == 1 &&
                    work_item.source.local_frame_id == 18 &&
                    work_item.source.camera_frame_id == 72 &&
                    work_item.source.camera_timestamp_ns == 123456790 &&
                    work_item.source.timestamp_sys_ns == 987654322,
                "controller did not assemble the exact immutable/frame identity");

        const int ref_count_before_zero_frame = entry.ref_count.load();
        entry.recording_frame_id = 0;
        const auto zero_frame =
            controller.TrySubmit(&entry, &recycle_queue, context);
        require(zero_frame.status ==
                    SpatialRoiAcquisitionControllerSubmitStatus::kBridgeRejected &&
                    zero_frame.bridge_result.bridge_status ==
                        SpatialRoiAcquisitionBridgeStatus::kNotRecording,
                "zero recording frame was not treated as inactive");
        require(entry.ref_count.load() == ref_count_before_zero_frame,
                "inactive zero-frame submission retained WORKER_ENTRY");

        require(controller.armed(), "controller unexpectedly disarmed during submit");
        const bool first_disarm = controller.Disarm();
        require(first_disarm,
                std::string("valid controller did not disarm; armed after call=") +
                    (controller.armed() ? "true" : "false"));
        require(!controller.armed(), "disarm left an active session");
        auto reused_generation_runtime =
            std::make_shared<SpatialRoiRecordingRuntime>(
                plan,
                "controller-camera",
                0,
                [](std::size_t,
                   std::shared_ptr<const orange::spatial_roi::SpatialRoiBatchEnvelope>) {
                    return orange::spatial_roi::SpatialRoiLaneSinkResult::kCompleted;
                });
        const auto reused_generation =
            controller.Arm(session_for_runtime(reused_generation_runtime));
        require(reused_generation.status ==
                    SpatialRoiAcquisitionArmStatus::kGenerationReused,
                "controller accepted a reused producer generation");

        const nlohmann::json successor_plan =
            make_plan("controller-generation-2");
        auto predecessor = runtime;
        auto successor = std::make_shared<SpatialRoiRecordingRuntime>(
            successor_plan,
            "controller-camera",
            0,
            [](std::size_t,
               std::shared_ptr<const orange::spatial_roi::SpatialRoiBatchEnvelope>) {
                return orange::spatial_roi::SpatialRoiLaneSinkResult::kCompleted;
            });
        require(controller.Arm(session_for_runtime(successor)).armed(),
                "controller could not arm while predecessor was retained");
        entry.frame_id = 19;
        entry.camera_frame_id = 73;
        entry.recording_frame_id = 2;
        entry.timestamp = 123456791;
        entry.timestamp_sys = 987654323;
        auto successor_submission =
            controller.TrySubmit(&entry, &recycle_queue, context);
        require(successor_submission.status ==
                    SpatialRoiAcquisitionControllerSubmitStatus::kSubmitted,
                "successor runtime did not accept after predecessor disarm");
        require(static_cast<bool>(
                    successor_submission.bridge_result.runtime_submission.envelope),
                "successor submission lost its envelope");
        require(controller.Disarm(), "successor controller did not disarm");
        require(!controller.Disarm(), "repeated disarm was not idempotent");
        successor_submission.bridge_result.runtime_submission.envelope.reset();
        submitted.bridge_result.runtime_submission.envelope.reset();

        bool non_owner_drain = false;
        std::thread non_owner([&] { non_owner_drain = controller.Drain(); });
        non_owner.join();
        require(non_owner_drain,
                "teardown thread could not drain retired runtimes safely");
        require(controller.Drain(), "repeated runtime drain failed");
        require(controller.Drain(), "repeated runtime drain was not idempotent");
        require(predecessor && successor,
                "retired runtime ownership was not retained through drain");
    }
    // The producer pool also owns the source lease until its runtime is
    // destroyed; drop the test's external runtime owner before inspecting the
    // acquisition entry's final owner reference.
    runtime.reset();
    require(entry.ref_count.load() == 1,
            "controller/runtime drain released acquisition owner reference; ref_count=" +
                std::to_string(entry.ref_count.load()));
}

void test_concurrent_snapshot_and_disarm()
{
    const nlohmann::json plan = make_plan();
    auto runtime = std::make_shared<SpatialRoiRecordingRuntime>(
        plan,
        "controller-camera",
        0,
        [](std::size_t, std::shared_ptr<const orange::spatial_roi::SpatialRoiBatchEnvelope>) {
            return orange::spatial_roi::SpatialRoiLaneSinkResult::kCompleted;
        });
    SpatialRoiAcquisitionController controller;
    require(controller.Arm(session_for_runtime(runtime)).armed(),
            "concurrency test failed to arm");
    Source source = make_source();
    WORKER_ENTRY entry{};
    initialize_entry(&entry, source, 1);
    SafeQueue<WORKER_ENTRY*> recycle_queue;
    WorkerEntryReleaseContext context{
        "controller-camera", "spatial_roi_controller_tests"};
    std::atomic<bool> start{false};
    std::atomic<unsigned int> unexpected{0};
    std::thread submitter([&] {
        while (!start.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
        for (int i = 0; i != 100; ++i) {
            const auto result =
                controller.TrySubmit(&entry, &recycle_queue, context);
            if (result.status !=
                    SpatialRoiAcquisitionControllerSubmitStatus::kSubmitted &&
                result.status !=
                    SpatialRoiAcquisitionControllerSubmitStatus::kBridgeRejected &&
                result.status !=
                    SpatialRoiAcquisitionControllerSubmitStatus::kDisarmed &&
                result.status !=
                    SpatialRoiAcquisitionControllerSubmitStatus::kRuntimeIncomplete) {
                unexpected.fetch_add(1, std::memory_order_relaxed);
            }
        }
    });
    start.store(true, std::memory_order_release);
    require(controller.Disarm(), "concurrent disarm did not remove session");
    submitter.join();
    require(unexpected.load(std::memory_order_relaxed) == 0,
            "concurrent snapshot/disarm returned an invalid status");
    require(controller.Drain(), "concurrent retired runtime did not drain");
    runtime.reset();
    // The original owner reference remains; all accepted submission leases
    // were released by their per-iteration result destructors.
    require(entry.ref_count.load() == 1,
            "concurrent controller use leaked a WORKER_ENTRY lease");
}

}  // namespace

int main()
{
    try {
        test_default_off();
        int device_count = 0;
        const cudaError_t device_status = cudaGetDeviceCount(&device_count);
        if (device_status == cudaErrorNoDevice || device_count == 0) {
            std::cout << "spatial_roi_acquisition_controller_tests: host-only "
                         "checks passed; CUDA checks skipped\n";
            return 0;
        }
        require_cuda(device_status, "cudaGetDeviceCount");
        require_cuda(cudaSetDevice(0), "cudaSetDevice");
        test_runtime_cases();
        test_concurrent_snapshot_and_disarm();
        std::cout << "spatial_roi_acquisition_controller_tests: all checks passed\n";
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "spatial_roi_acquisition_controller_tests: FAIL: "
                  << exception.what() << '\n';
        return 1;
    }
}
