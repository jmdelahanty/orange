#include "spatial_roi_acquisition_bridge.h"

#include "camera.h"
#include "session/spatial_roi_recording_config.h"
#include "shaman_v2_recording_identity.h"
#include "video_capture.h"

#include "worker_entry_release.h"

#include <cuda_runtime_api.h>

#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using orange::spatial_roi::SpatialRoiAcquisitionBridgeStatus;
using orange::spatial_roi::SpatialRoiAcquisitionIdentity;
using orange::spatial_roi::SpatialRoiAcquisitionBridgeResult;
using orange::spatial_roi::SpatialRoiRecordingRuntime;
using orange::spatial_roi::validate_spatial_roi_acquisition_input;

void require(bool condition, const std::string& message)
{
    if (!condition) {
        throw std::runtime_error(message);
    }
}

SpatialRoiAcquisitionIdentity valid_identity(const WORKER_ENTRY& entry)
{
    SpatialRoiAcquisitionIdentity identity;
    identity.recording_active = true;
    identity.recording_id = "bridge-test-recording";
    identity.recording_identity_token =
        orange::shaman_v2_recording_identity::token_for_recording_id(
            identity.recording_id);
    identity.producer_generation = "bridge-generation";
    identity.camera_id = 3;
    identity.camera_serial = "10000";
    identity.local_frame_id = entry.frame_id;
    identity.camera_frame_id = entry.camera_frame_id;
    identity.recording_frame_id = entry.recording_frame_id;
    identity.camera_timestamp_ns = entry.timestamp;
    identity.timestamp_sys_ns = entry.timestamp_sys;
    return identity;
}

void initialize_valid_entry(WORKER_ENTRY* entry)
{
    entry->width = 8;
    entry->height = 6;
    entry->pixelFormat = GVSP_PIX_MONO8;
    entry->source_buffer_bytes = 48;
    entry->image_gpu_id = 0;
    entry->frame_id = 17;
    entry->camera_frame_id = 71;
    entry->recording_frame_id = 9;
    entry->timestamp = 123456789;
    entry->timestamp_sys = 987654321;
    entry->gpu_direct_mode = false;
    entry->owns_memory = true;
}

void require_status(
    const WORKER_ENTRY& entry,
    const SafeQueue<WORKER_ENTRY*>& queue,
    const SpatialRoiAcquisitionIdentity& identity,
    SpatialRoiAcquisitionBridgeStatus expected,
    const std::string& label)
{
    std::string error;
    const SpatialRoiAcquisitionBridgeStatus actual =
        validate_spatial_roi_acquisition_input(
            &entry,
            &queue,
            reinterpret_cast<const SpatialRoiRecordingRuntime*>(0x1),
            identity,
            &error);
    require(actual == expected, label + ": unexpected status " +
                                  std::string(
                                      orange::spatial_roi::
                                          spatial_roi_acquisition_bridge_status_name(actual)) +
                                  " error=" + error);
}

void test_rejects_without_retaining()
{
    SafeQueue<WORKER_ENTRY*> queue;
    WORKER_ENTRY entry{};
    initialize_valid_entry(&entry);
    entry.ref_count.store(1);
    SpatialRoiAcquisitionIdentity identity = valid_identity(entry);

    cudaEvent_t ready_event = reinterpret_cast<cudaEvent_t>(0x1);
    entry.d_image = reinterpret_cast<unsigned char*>(0x1);
    entry.event_ptr = &ready_event;
    std::string valid_error;
    require(validate_spatial_roi_acquisition_input(
                &entry, &queue,
                reinterpret_cast<const SpatialRoiRecordingRuntime*>(0x1),
                identity, &valid_error) ==
                SpatialRoiAcquisitionBridgeStatus::kSubmitted,
            "valid packed Mono8 source was rejected: " + valid_error);
    require(entry.ref_count.load() == 1,
            "pure validation changed the entry ref count");

    // The pure seam deliberately has no lease side effects.  It is called
    // once for each fail-closed source/identity condition.
    entry.gpu_direct_mode = true;
    require_status(entry, queue, identity,
                   SpatialRoiAcquisitionBridgeStatus::kGpuDirect,
                   "GPU-direct");
    require(entry.ref_count.load() == 1, "GPU-direct rejection retained entry");
    entry.gpu_direct_mode = false;

    entry.owns_memory = false;
    require_status(entry, queue, identity,
                   SpatialRoiAcquisitionBridgeStatus::kUnowned,
                   "unowned");
    require(entry.ref_count.load() == 1, "unowned rejection retained entry");
    entry.owns_memory = true;

    entry.source_buffer_bytes = 49;
    require_status(entry, queue, identity,
                   SpatialRoiAcquisitionBridgeStatus::kStrideUnknown,
                   "stride-unknown");
    require(entry.ref_count.load() == 1,
            "stride-unknown rejection retained entry");
    entry.source_buffer_bytes = 48;

    identity.recording_active = false;
    require_status(entry, queue, identity,
                   SpatialRoiAcquisitionBridgeStatus::kNotRecording,
                   "nonrecording");
    require(entry.ref_count.load() == 1,
            "nonrecording rejection retained entry");
    identity.recording_active = true;

    identity.recording_frame_id = 0;
    require_status(entry, queue, identity,
                   SpatialRoiAcquisitionBridgeStatus::kZeroIdentity,
                   "zero identity");
    require(entry.ref_count.load() == 1,
            "zero identity rejection retained entry");

    identity = valid_identity(entry);
    entry.pixelFormat = 0;
    require_status(entry, queue, identity,
                   SpatialRoiAcquisitionBridgeStatus::kNotMono8,
                   "non-Mono8");
    require(entry.ref_count.load() == 1,
            "non-Mono8 rejection retained entry");
}

void test_rejects_missing_wiring()
{
    WORKER_ENTRY entry{};
    initialize_valid_entry(&entry);
    entry.ref_count.store(1);
    SafeQueue<WORKER_ENTRY*> queue;
    SpatialRoiAcquisitionIdentity identity = valid_identity(entry);
    std::string error;

    require(validate_spatial_roi_acquisition_input(
                &entry, nullptr,
                reinterpret_cast<const SpatialRoiRecordingRuntime*>(0x1),
                identity, &error) ==
                SpatialRoiAcquisitionBridgeStatus::kMissingRecycleQueue,
            "missing recycle queue was not rejected");
    require(validate_spatial_roi_acquisition_input(
                &entry, &queue, nullptr, identity, &error) ==
                SpatialRoiAcquisitionBridgeStatus::kMissingRuntime,
            "missing runtime was not rejected");
    require(entry.ref_count.load() == 1,
            "missing wiring rejection retained entry");
}

void throw_before_runtime_submit()
{
    throw std::runtime_error("injected pre-TrySubmit failure");
}

void test_pre_submit_exception_releases_lease()
{
    WORKER_ENTRY entry{};
    initialize_valid_entry(&entry);
    entry.ref_count.store(1);
    entry.d_image = reinterpret_cast<unsigned char*>(0x1);
    cudaEvent_t ready_event = reinterpret_cast<cudaEvent_t>(0x1);
    entry.event_ptr = &ready_event;
    SafeQueue<WORKER_ENTRY*> queue;
    const SpatialRoiAcquisitionIdentity identity = valid_identity(entry);

    const auto result = orange::spatial_roi::submit_spatial_roi_acquisition(
        &entry,
        &queue,
        WorkerEntryReleaseContext{"bridge-camera", "bridge-test"},
        reinterpret_cast<SpatialRoiRecordingRuntime*>(0x1),
        identity,
        &throw_before_runtime_submit);
    require(result.bridge_status ==
                SpatialRoiAcquisitionBridgeStatus::kRuntimeException,
            "pre-TrySubmit exception was not converted to bridge status");
    require(!result.runtime_submission.envelope,
            "pre-TrySubmit exception returned an envelope");
    require(entry.ref_count.load() == 1,
            "pre-TrySubmit exception leaked the retained entry lease");
}

void test_status_names()
{
    require(std::string(
                orange::spatial_roi::
                    spatial_roi_acquisition_bridge_status_name(
                        SpatialRoiAcquisitionBridgeStatus::kStrideUnknown)) ==
                "stride_unknown",
            "bridge status name changed unexpectedly");
}

nlohmann::json make_verified_plan()
{
    namespace api = orange::session::spatial_roi;
    api::Config config = api::default_config();
    config.enabled = true;
    config.output_alignment_px = 2;
    config.buffering.pool_frames_per_stream = 2;
    config.buffering.queue_frames_per_stream = 1;
    config.admission.max_rois_per_camera = 4;
    config.admission.max_total_rois = 4;
    config.admission.max_total_encoder_streams = 4;
    config.admission.max_total_pixel_rate = 1000000;
    config.admission.max_total_pool_bytes = 1000000;
    config.admission.max_total_queue_frames = 1000000;

    api::CameraConfig camera;
    camera.camera_id = 3;
    camera.camera_serial = "10000";
    camera.native_raster = {8, 6};
    camera.source_frame_rate = 30;
    camera.arena_group_id = "bridge_arena_group";
    camera.layout = {"bridge_layout", "sha256:" + std::string(64, '1')};
    camera.materialization = {
        "bridge_materialization", "sha256:" + std::string(64, '2')};
    camera.registration = {
        "bridge_registration", "sha256:" + std::string(64, '3')};
    for (std::size_t index = 0; index < 4; ++index) {
        api::RoiConfig roi;
        roi.roi_id = "roi_" + std::to_string(index + 1);
        roi.region_id = "region_" + std::to_string(index + 1);
        roi.content_rect = {
            static_cast<std::uint32_t>((index % 2) * 2),
            static_cast<std::uint32_t>((index / 2) * 2),
            2,
            2};
        roi.logical_stream_id =
            api::expected_logical_stream_id(camera.camera_serial, roi.roi_id);
        roi.artifact_stem =
            api::expected_artifact_stem(camera.camera_serial, roi.roi_id);
        camera.rois.push_back(std::move(roi));
    }
    config.cameras.emplace(camera.camera_serial, std::move(camera));

    api::PlanContext context;
    context.recording_id = "bridge-test-recording";
    context.recording_identity_token =
        orange::shaman_v2_recording_identity::token_for_recording_id(
            context.recording_id);
    context.generated_at_utc = "2026-08-31T00:00:00Z";
    context.producer_generation = "bridge-generation";

    nlohmann::json plan;
    std::string error;
    require(api::build_plan(config, context, &plan, nullptr, &error),
            "bridge test plan build failed: " + error);
    require(api::verify_plan(plan, &error),
            "bridge test plan verification failed: " + error);
    return plan;
}

void test_cuda_accepted_envelope_retains_entry()
{
    int device_count = 0;
    const cudaError_t device_status = cudaGetDeviceCount(&device_count);
    if (device_status != cudaSuccess || device_count <= 0) {
        std::cout << "[SKIP] bridge CUDA lease lifetime: "
                  << (device_status == cudaSuccess
                          ? "no CUDA device"
                          : cudaGetErrorString(device_status))
                  << "\n";
        (void)cudaGetLastError();
        return;
    }

    WORKER_ENTRY entry{};
    initialize_valid_entry(&entry);
    entry.ref_count.store(1);
    entry.d_image_pool = nullptr;
    entry.gpu_direct_mode = false;
    entry.owns_memory = true;
    require(cudaMalloc(reinterpret_cast<void**>(&entry.d_image), 48) ==
                cudaSuccess,
            "bridge test source allocation failed");
    cudaEvent_t ready_event = nullptr;
    require(cudaEventCreateWithFlags(&ready_event, cudaEventDisableTiming) ==
                cudaSuccess,
            "bridge test ready event creation failed");
    entry.event_ptr = &ready_event;
    require(cudaEventRecord(ready_event, nullptr) == cudaSuccess,
            "bridge test ready event record failed");

    SafeQueue<WORKER_ENTRY*> recycle_queue;
    SpatialRoiAcquisitionIdentity identity = valid_identity(entry);
    SpatialRoiAcquisitionBridgeResult bridge_result;
    {
        SpatialRoiRecordingRuntime runtime(
            make_verified_plan(),
            "10000",
            0,
            [](std::size_t,
               std::shared_ptr<const orange::spatial_roi::SpatialRoiBatchEnvelope>) {
                return orange::spatial_roi::SpatialRoiLaneSinkResult::kCompleted;
            });
        bridge_result = orange::spatial_roi::submit_spatial_roi_acquisition(
            &entry,
            &recycle_queue,
            WorkerEntryReleaseContext{"bridge-camera", "bridge-test"},
            &runtime,
            identity);
        require(bridge_result.bridge_status ==
                    SpatialRoiAcquisitionBridgeStatus::kSubmitted,
                "valid WORKER_ENTRY was not submitted: " +
                    bridge_result.error);
        require(static_cast<bool>(bridge_result.runtime_submission.envelope),
                "accepted bridge submission lost its envelope");
        require(entry.ref_count.load() == 2,
                "accepted bridge submission did not retain exactly one lease");
        runtime.StopAcceptingAndDrain();
        require(bridge_result.runtime_submission.envelope->strict_complete(),
                "accepted bridge batch did not complete all ROI lanes");
    }

    // Runtime destruction cannot release the source while the returned
    // envelope remains in the caller's result.  Resetting that one reference
    // releases the single holder lease, returning the refcount to acquisition.
    require(entry.ref_count.load() == 2,
            "runtime teardown released the accepted source too early");
    bridge_result.runtime_submission.envelope.reset();
    require(entry.ref_count.load() == 1,
            "accepted envelope did not release its one source lease");

    require(release_worker_entry_to_recycle(
                &recycle_queue,
                &entry,
                WorkerEntryReleaseContext{"bridge-camera", "bridge-test"}),
            "acquisition owner did not recycle the test entry");
    WORKER_ENTRY* recycled_entry = nullptr;
    require(recycle_queue.pop(recycled_entry) && recycled_entry == &entry,
            "accepted bridge entry was not returned to recycle queue");
    require(cudaEventDestroy(ready_event) == cudaSuccess,
            "bridge test ready event destruction failed");
    require(cudaFree(entry.d_image) == cudaSuccess,
            "bridge test source free failed");
    std::cout << "[PASS] bridge CUDA accepted envelope source lease lifetime\n";
}

}  // namespace

int main()
{
    try {
        test_rejects_without_retaining();
        test_rejects_missing_wiring();
        test_pre_submit_exception_releases_lease();
        test_status_names();
        test_cuda_accepted_envelope_retains_entry();
    } catch (const std::exception& exception) {
        std::cerr << "[FAIL] spatial ROI acquisition bridge tests: "
                  << exception.what() << '\n';
        return 1;
    }
    std::cout << "[PASS] spatial ROI acquisition bridge tests\n";
    return 0;
}
