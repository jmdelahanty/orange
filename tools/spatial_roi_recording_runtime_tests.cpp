#include "spatial_roi_recording_runtime.h"

#include "session/spatial_roi_recording_config.h"
#include "shaman_v2_recording_identity.h"

#include <cuda_runtime.h>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <iostream>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

using orange::spatial_roi::SpatialRoiBatchCompletionStatus;
using orange::spatial_roi::SpatialRoiBatchSubmission;
using orange::spatial_roi::SpatialRoiFrameIdentity;
using orange::spatial_roi::SpatialRoiLaneDelivery;
using orange::spatial_roi::SpatialRoiLaneSinkResult;
using orange::spatial_roi::SpatialRoiLaneTerminalReason;
using orange::spatial_roi::SpatialRoiRecordingRuntime;
using orange::spatial_roi::SpatialRoiRuntimeSubmitStatus;
using orange::spatial_roi::SpatialRoiSourcePixelFormat;
using orange::spatial_roi::SpatialRoiSourceView;

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

orange::session::spatial_roi::Config make_config()
{
    namespace api = orange::session::spatial_roi;
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
    namespace api = orange::session::spatial_roi;
    api::PlanContext context;
    context.recording_id = "runtime-test-recording";
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

SpatialRoiFrameIdentity make_identity()
{
    SpatialRoiFrameIdentity identity;
    identity.recording_id = "runtime-test-recording";
    identity.recording_identity_token =
        orange::shaman_v2_recording_identity::token_for_recording_id(
            identity.recording_id);
    identity.producer_generation = "generation_1";
    identity.camera_id = 3;
    identity.camera_serial = "10000";
    identity.local_frame_id = 17;
    identity.camera_frame_id = 71;
    identity.recording_frame_id = 9;
    identity.camera_timestamp_ns = 123456789;
    identity.timestamp_sys_ns = 987654321;
    return identity;
}

struct GpuSource {
    unsigned char* device_data = nullptr;
    std::size_t pitch_bytes = 0;
    cudaStream_t stream = nullptr;
    cudaEvent_t ready_event = nullptr;

    GpuSource() = default;
    GpuSource(const GpuSource&) = delete;
    GpuSource& operator=(const GpuSource&) = delete;
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
    GpuSource& operator=(GpuSource&& other) noexcept
    {
        if (this == &other) {
            return *this;
        }
        std::swap(device_data, other.device_data);
        std::swap(pitch_bytes, other.pitch_bytes);
        std::swap(stream, other.stream);
        std::swap(ready_event, other.ready_event);
        return *this;
    }

    ~GpuSource()
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

GpuSource make_source()
{
    GpuSource source;
    require_cuda(cudaMallocPitch(
                     reinterpret_cast<void**>(&source.device_data),
                     &source.pitch_bytes,
                     8,
                     6),
                 "cudaMallocPitch(source)");
    require_cuda(cudaStreamCreateWithFlags(
                     &source.stream, cudaStreamNonBlocking),
                 "cudaStreamCreateWithFlags(source)");
    require_cuda(cudaEventCreateWithFlags(
                     &source.ready_event, cudaEventDisableTiming),
                 "cudaEventCreateWithFlags(source ready)");
    require_cuda(cudaMemset2DAsync(
                     source.device_data,
                     source.pitch_bytes,
                     0x2a,
                     8,
                     6,
                     source.stream),
                 "cudaMemset2DAsync(source)");
    require_cuda(
        cudaEventRecord(source.ready_event, source.stream),
        "cudaEventRecord(source ready)");
    return source;
}

SpatialRoiSourceView make_source_view(const GpuSource& gpu_source)
{
    SpatialRoiSourceView source;
    source.device_data = gpu_source.device_data;
    source.pitch_bytes = gpu_source.pitch_bytes;
    source.allocation_bytes = gpu_source.pitch_bytes * 6;
    source.width = 8;
    source.height = 6;
    source.gpu_id = 0;
    source.pixel_format = SpatialRoiSourcePixelFormat::kMono8;
    source.source_lease = std::make_shared<int>(0);
    source.ready_event = gpu_source.ready_event;
    source.identity = make_identity();
    return source;
}

void test_status_names_and_verified_constructor_rejection()
{
    require(std::string(orange::spatial_roi::
                            spatial_roi_runtime_submit_status_name(
                                SpatialRoiRuntimeSubmitStatus::kIncomplete)) ==
                "incomplete",
            "runtime status name changed");
    require(std::string(orange::spatial_roi::
                            spatial_roi_lane_terminal_reason_name(
                                SpatialRoiLaneTerminalReason::kQueueFull)) ==
                "queue_full",
            "lane terminal reason name changed");
    require(std::string(orange::spatial_roi::
                            spatial_roi_batch_completion_status_name(
                                SpatialRoiBatchCompletionStatus::kComplete)) ==
                "complete",
            "completion status name changed");
    require(std::string(orange::spatial_roi::
                            spatial_roi_runtime_submit_status_name(
                                SpatialRoiRuntimeSubmitStatus::kBusy)) ==
                "busy",
            "busy admission status name changed");
    require(std::string(orange::spatial_roi::
                            spatial_roi_runtime_submit_status_name(
                                SpatialRoiRuntimeSubmitStatus::
                                    kDuplicateOrOutOfOrder)) ==
                "duplicate_or_out_of_order",
            "nonmonotonic source status name changed");

    nlohmann::json plan = make_plan();
    plan["plan_sha256"] = "sha256:" + std::string(64, 'f');
    bool rejected = false;
    try {
        SpatialRoiRecordingRuntime runtime(plan, "10000", 0);
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    require(rejected, "runtime accepted an unverified plan");
}

void test_runtime_cuda_fanout_and_terminal_failures()
{
    GpuSource gpu_source = make_source();
    const SpatialRoiSourceView source = make_source_view(gpu_source);
    const nlohmann::json plan = make_plan();

    std::mutex sink_mutex;
    std::vector<std::size_t> completed_lanes;
    std::vector<std::uint64_t> completed_indexes;
    {
        SpatialRoiRecordingRuntime runtime(
            plan,
            "10000",
            0,
            [&](const SpatialRoiLaneDelivery& delivery) {
                const std::size_t lane_index = delivery.lane_index;
                const auto& envelope = delivery.envelope;
                require(delivery.roi_stream_frame_index > 0,
                        "lane delivery index was not positive");
                require(envelope->work_items().size() == 4,
                        "batch envelope lost one or more ROI work items");
                require(envelope->work_items()[lane_index].roi_id ==
                            "roi_" + std::to_string(lane_index + 1),
                        "ROI lane order was not preserved from the plan");
                require(envelope->result().outputs()[lane_index]
                            .work_item.roi_id == envelope->work_items()[lane_index]
                                                          .roi_id,
                        "lane output identity does not match work item identity");
                std::lock_guard<std::mutex> lock(sink_mutex);
                completed_lanes.push_back(lane_index);
                completed_indexes.push_back(delivery.roi_stream_frame_index);
                return SpatialRoiLaneSinkResult::kCompleted;
            });

        SpatialRoiBatchSubmission submission = runtime.TrySubmit(source);
        require(submission.status == SpatialRoiRuntimeSubmitStatus::kAccepted,
                "valid source was not admitted to every ROI lane");
        require(submission.admitted_lane_count == 4,
                "not every ROI lane was admitted");
        const SpatialRoiBatchSubmission duplicate = runtime.TrySubmit(source);
        require(duplicate.status ==
                    SpatialRoiRuntimeSubmitStatus::kDuplicateOrOutOfOrder &&
                    !duplicate.envelope,
                "duplicate source recording frame was admitted twice");
        runtime.StopAcceptingAndDrain();

        const auto snapshot = submission.envelope->terminal_snapshot();
        require(snapshot.status == SpatialRoiBatchCompletionStatus::kComplete,
                "all successful sinks did not complete the batch");
        require(snapshot.terminal_lane_count == 4,
                "batch did not record every terminal lane");
        require(completed_lanes.size() == 4,
                "sink did not receive one callback per ROI lane");
        require(completed_indexes.size() == 4,
                "sink did not receive one stream index per ROI lane");
        for (const std::uint64_t index : completed_indexes) {
            require(index == 1,
                    "first admitted batch did not receive index one on every lane");
        }
        const auto counters = runtime.counters();
        require(counters.producer_accepted == 1 && counters.lane_admitted == 4,
                "accepted producer/lane counters are not exact");
        require(counters.duplicate_or_out_of_order == 1,
                "duplicate source counter is not exact");
        require(counters.lane_completed == 4 && counters.batches_complete == 1,
                "successful terminal counters are not exact");
    }

    SpatialRoiBatchSubmission failed_submission;
    {
        SpatialRoiRecordingRuntime runtime(
            plan,
            "10000",
            0,
            [](const SpatialRoiLaneDelivery& delivery) {
                const std::size_t lane_index = delivery.lane_index;
                if (lane_index == 1) {
                    return SpatialRoiLaneSinkResult::kRejected;
                }
                if (lane_index == 2) {
                    return SpatialRoiLaneSinkResult::kFailed;
                }
                if (lane_index == 3) {
                    return static_cast<SpatialRoiLaneSinkResult>(999);
                }
                return SpatialRoiLaneSinkResult::kCompleted;
            });
        failed_submission = runtime.TrySubmit(source);
        require(failed_submission.status == SpatialRoiRuntimeSubmitStatus::kAccepted,
                "sink outcome was incorrectly reported as an admission failure");
        runtime.StopAcceptingAndDrain();
        const auto snapshot = failed_submission.envelope->terminal_snapshot();
        require(snapshot.status == SpatialRoiBatchCompletionStatus::kIncomplete,
                "required sink rejection/failure did not make batch incomplete");
        require(snapshot.lane_reasons[1] ==
                    SpatialRoiLaneTerminalReason::kSinkRejected &&
                    snapshot.lane_reasons[2] ==
                        SpatialRoiLaneTerminalReason::kSinkFailed,
                "sink terminal rejection reasons were not retained");
        const auto counters = runtime.counters();
        require(counters.lane_sink_rejected == 1 &&
                    counters.lane_sink_failed == 2 &&
                    counters.batches_incomplete == 1 &&
                    counters.strict_incomplete_batches == 1,
                "incomplete batch counters are not exact");
    }
    failed_submission.envelope.reset();

    SpatialRoiBatchSubmission missing_sink_submission;
    {
        SpatialRoiRecordingRuntime runtime(plan, "10000", 0);
        missing_sink_submission = runtime.TrySubmit(source);
        require(missing_sink_submission.status ==
                    SpatialRoiRuntimeSubmitStatus::kAccepted,
                "missing sink was incorrectly reported as an admission failure");
        runtime.StopAcceptingAndDrain();
        const auto snapshot =
            missing_sink_submission.envelope->terminal_snapshot();
        require(snapshot.status == SpatialRoiBatchCompletionStatus::kIncomplete,
                "missing recorder sink did not fail the batch closed");
        const auto counters = runtime.counters();
        require(counters.lane_sink_failed == 4 &&
                    counters.batches_incomplete == 1,
                "missing recorder sink failure counters are not exact");
    }
    missing_sink_submission.envelope.reset();
}

void test_exact_lane_capacity_and_reentrant_stop()
{
    GpuSource gpu_source = make_source();
    const nlohmann::json plan = make_plan();

    std::mutex gate_mutex;
    std::condition_variable gate_condition;
    bool release_sinks = false;
    std::condition_variable completed_condition;
    std::size_t completed_delivery_count = 0;
    std::vector<std::vector<std::uint64_t>> delivered_indexes(4);
    SpatialRoiBatchSubmission first;
    SpatialRoiBatchSubmission second;
    SpatialRoiBatchSubmission overflow;
    SpatialRoiBatchSubmission fourth;
    {
        SpatialRoiRecordingRuntime runtime(
            plan,
            "10000",
            0,
            [&](const SpatialRoiLaneDelivery& delivery) {
                std::unique_lock<std::mutex> lock(gate_mutex);
                gate_condition.wait(lock, [&]() { return release_sinks; });
                delivered_indexes[delivery.lane_index].push_back(
                    delivery.roi_stream_frame_index);
                ++completed_delivery_count;
                completed_condition.notify_all();
                return SpatialRoiLaneSinkResult::kCompleted;
            });

        SpatialRoiSourceView source = make_source_view(gpu_source);
        first = runtime.TrySubmit(source);
        source.identity.recording_frame_id++;
        second = runtime.TrySubmit(source);
        source.identity.recording_frame_id++;
        overflow = runtime.TrySubmit(source);
        require(first.status == SpatialRoiRuntimeSubmitStatus::kAccepted &&
                    second.status == SpatialRoiRuntimeSubmitStatus::kAccepted,
                "two-frame lane capacity did not admit exactly two batches");
        require(overflow.status == SpatialRoiRuntimeSubmitStatus::kIncomplete &&
                    overflow.admitted_lane_count == 0,
                "third outstanding batch did not fail strict lane admission");
        const auto overflow_snapshot = overflow.envelope->terminal_snapshot();
        require(overflow_snapshot.status ==
                    SpatialRoiBatchCompletionStatus::kIncomplete,
                "queue-full batch was not terminally incomplete");
        for (const auto reason : overflow_snapshot.lane_reasons) {
            require(reason == SpatialRoiLaneTerminalReason::kQueueFull,
                    "queue-full batch lost an exact lane reason");
        }

        {
            std::lock_guard<std::mutex> lock(gate_mutex);
            release_sinks = true;
        }
        gate_condition.notify_all();
        {
            std::unique_lock<std::mutex> lock(gate_mutex);
            completed_condition.wait(lock, [&]() {
                return completed_delivery_count == 8;
            });
        }

        // Release the producer results from the first three submissions so
        // the test can admit another batch without changing the bounded pool
        // configuration.  The callbacks above have already retained the
        // immutable per-lane indices for verification.
        first.envelope.reset();
        second.envelope.reset();
        overflow.envelope.reset();
        source.identity.recording_frame_id++;
        fourth = runtime.TrySubmit(source);
        require(fourth.status == SpatialRoiRuntimeSubmitStatus::kAccepted &&
                    fourth.admitted_lane_count == 4,
                "lane indices could not continue after queue rejection");
        runtime.StopAcceptingAndDrain();
        for (std::size_t lane_index = 0; lane_index < 4; ++lane_index) {
            require(delivered_indexes[lane_index].size() == 3,
                    "one lane did not receive all admitted deliveries");
            require(delivered_indexes[lane_index][0] == 1 &&
                        delivered_indexes[lane_index][1] == 2 &&
                        delivered_indexes[lane_index][2] == 3,
                    "queue rejection introduced a per-lane index gap");
        }
        const auto counters = runtime.counters();
        require(counters.lane_queue_full == 4 &&
                    counters.batches_incomplete == 1 &&
                    counters.strict_incomplete_batches == 1,
                "strict queue-full counters are not exact");
    }
    first.envelope.reset();
    second.envelope.reset();
    overflow.envelope.reset();
    fourth.envelope.reset();

    // A newly constructed runtime represents a new ROI stream generation and
    // therefore starts its independent dense index at one.
    std::uint64_t restarted_index = 0;
    {
        SpatialRoiRecordingRuntime runtime(
            plan,
            "10000",
            0,
            [&](const SpatialRoiLaneDelivery& delivery) {
                if (delivery.lane_index == 0) {
                    restarted_index = delivery.roi_stream_frame_index;
                }
                return SpatialRoiLaneSinkResult::kCompleted;
            });
        SpatialRoiSourceView restarted_source = make_source_view(gpu_source);
        restarted_source.identity.recording_frame_id = 101;
        const SpatialRoiBatchSubmission submission =
            runtime.TrySubmit(restarted_source);
        require(submission.status == SpatialRoiRuntimeSubmitStatus::kAccepted,
                "fresh runtime did not admit its first batch");
        runtime.StopAcceptingAndDrain();
    }
    require(restarted_index == 1,
            "fresh runtime did not restart ROI stream index at one");

    SpatialRoiBatchSubmission reentrant;
    {
        SpatialRoiRecordingRuntime* runtime_ptr = nullptr;
        SpatialRoiRecordingRuntime runtime(
            plan,
            "10000",
            0,
            [&](const SpatialRoiLaneDelivery& delivery) {
                const std::size_t lane_index = delivery.lane_index;
                if (lane_index == 0) {
                    runtime_ptr->StopAcceptingAndDrain();
                }
                return SpatialRoiLaneSinkResult::kCompleted;
            });
        runtime_ptr = &runtime;
        reentrant = runtime.TrySubmit(make_source_view(gpu_source));
        require(reentrant.status == SpatialRoiRuntimeSubmitStatus::kAccepted,
                "reentrant-stop fixture was not admitted");
        runtime.StopAcceptingAndDrain();
        require(reentrant.envelope->terminal_snapshot().status ==
                    SpatialRoiBatchCompletionStatus::kComplete,
                "reentrant stop prevented admitted lanes from draining");
    }
    reentrant.envelope.reset();
}

}  // namespace

int main()
{
    try {
        test_status_names_and_verified_constructor_rejection();

        int device_count = 0;
        const cudaError_t device_status = cudaGetDeviceCount(&device_count);
        if (device_status != cudaSuccess || device_count <= 0) {
            std::cout << "[PASS] spatial ROI runtime verified-plan rejection and status contracts\n";
            std::cout << "[SKIP] spatial ROI runtime CUDA lane fanout: "
                      << (device_status == cudaSuccess
                              ? "no CUDA device"
                              : cudaGetErrorString(device_status))
                      << "\n";
            (void)cudaGetLastError();
            return 0;
        }

        test_runtime_cuda_fanout_and_terminal_failures();
        test_exact_lane_capacity_and_reentrant_stop();
        std::cout << "[PASS] spatial ROI runtime verified-plan rejection and status contracts\n";
        std::cout << "[PASS] spatial ROI runtime CUDA fanout, shared envelope, drain, and terminal reasons\n";
        std::cout << "[PASS] spatial ROI runtime exact lane capacity and reentrant stop\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "[FAIL] " << error.what() << "\n";
        return 1;
    }
}
