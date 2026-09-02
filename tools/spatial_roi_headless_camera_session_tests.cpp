#include "spatial_roi_headless_camera_session.h"

#include "session/spatial_roi_recording_config.h"
#include "shaman_v2_recording_identity.h"

#include <cuda_runtime.h>

#include <algorithm>
#include <chrono>
#include <functional>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <unistd.h>
#include <utility>
#include <vector>

namespace {

namespace headless = orange::spatial_roi::headless;
namespace recording = orange::spatial_roi::recording;
namespace spatial_roi = orange::session::spatial_roi;

void require(const bool condition, const std::string& message)
{
    if (!condition) {
        throw std::runtime_error(message);
    }
}

std::string digest(const char fill)
{
    return "sha256:" + std::string(64, fill);
}

nlohmann::json make_plan()
{
    spatial_roi::Config config = spatial_roi::default_config();
    config.enabled = true;
    config.buffering.pool_frames_per_stream = 2;
    config.buffering.queue_frames_per_stream = 2;
    config.recording_limits.max_frames_per_stream = 1000;
    config.recording_limits.max_media_bytes_per_stream = 1000000;
    config.recording_limits.max_evidence_bytes_per_stream = 100000;
    config.admission.max_rois_per_camera = 4;
    config.admission.max_total_rois = 4;
    config.admission.max_total_encoder_streams = 4;
    config.admission.max_total_pixel_rate = 1000000;
    config.admission.max_total_pool_bytes = 1000000;
    config.admission.max_total_queue_frames = 16;
    config.admission.max_total_media_bytes = 4000000;
    config.admission.max_total_evidence_bytes = 400000;

    spatial_roi::CameraConfig camera;
    camera.camera_id = 3;
    camera.camera_serial = "headless-camera";
    camera.native_raster = {16, 16};
    camera.source_frame_rate = 30;
    camera.arena_group_id = "headless-group";
    camera.layout = {"layout", digest('1')};
    camera.materialization = {"materialization", digest('2')};
    camera.registration = {"registration", digest('3')};
    for (int index = 0; index < 4; ++index) {
        spatial_roi::RoiConfig roi;
        roi.roi_id = "roi_" + std::to_string(index + 1);
        roi.region_id = "region_" + std::to_string(index + 1);
        roi.content_rect = {
            static_cast<std::uint32_t>(index * 4), 0, 4, 4};
        roi.logical_stream_id = spatial_roi::expected_logical_stream_id(
            camera.camera_serial, roi.roi_id);
        roi.artifact_stem = spatial_roi::expected_artifact_stem(
            camera.camera_serial, roi.roi_id);
        camera.rois.push_back(std::move(roi));
    }
    config.cameras.emplace(camera.camera_serial, std::move(camera));

    spatial_roi::PlanContext context;
    context.recording_id = "headless-session-test";
    context.recording_identity_token =
        orange::shaman_v2_recording_identity::token_for_recording_id(
            context.recording_id);
    context.generated_at_utc = "2026-09-01T00:00:00Z";
    context.producer_generation = "headless-generation";

    nlohmann::json plan;
    std::string error;
    require(spatial_roi::build_plan(config, context, &plan, nullptr, &error), error);
    return plan;
}

struct FakeProcess final : public headless::CameraRecorderProcessHandle {
    explicit FakeProcess(std::vector<std::string>* events)
        : events_(events)
    {
    }

    bool Start(std::string*) override
    {
        events_->push_back("process.start");
        status_.started = true;
        status_.pid = 4321;
        status_.state =
            recording::SpatialRoiCameraRecorderProcessState::kStarting;
        return true;
    }

    bool WaitForFourSockets(std::string*) override
    {
        events_->push_back("process.sockets");
        status_.sockets_bound = true;
        status_.state =
            recording::SpatialRoiCameraRecorderProcessState::kSocketsBound;
        return true;
    }

    bool WaitUntilReady(std::string*) override
    {
        events_->push_back("process.ready");
        status_.ready = true;
        status_.state = recording::SpatialRoiCameraRecorderProcessState::kReady;
        return true;
    }

    bool WaitForCleanExit(std::string*) override
    {
        require(events_->size() >= 1 && events_->back() == "coordinator.stop",
                "recorder clean-exit wait ran before producer drain/close");
        events_->push_back("process.clean_exit");
        status_.terminal_seen = true;
        status_.exited = true;
        status_.reaped = true;
        status_.exit_code = 0;
        status_.state = recording::SpatialRoiCameraRecorderProcessState::kExited;
        status_.terminal.completed = true;
        status_.terminal.clean_eof = true;
        return true;
    }

    bool Stop(std::string*) override
    {
        events_->push_back("process.stop");
        status_.state = recording::SpatialRoiCameraRecorderProcessState::kStopped;
        status_.reaped = status_.started;
        return true;
    }

    const recording::SpatialRoiCameraRecorderProcessStatus& status()
        const noexcept override
    {
        return status_;
    }

    std::vector<std::string>* events_ = nullptr;
    recording::SpatialRoiCameraRecorderProcessStatus status_;
};

struct FakeCoordinator final : public headless::CameraProducerCoordinatorHandle {
    FakeCoordinator(std::vector<std::string>* events,
                    std::shared_ptr<orange::spatial_roi::SpatialRoiRecordingRuntime> runtime,
                    bool drain_ok = true,
                    std::string drain_error = {})
        : events_(events),
          runtime_(std::move(runtime)),
          drain_ok_(drain_ok),
          drain_error_(std::move(drain_error))
    {
    }

    bool Start(std::string*) override
    {
        events_->push_back("coordinator.start");
        started_ = true;
        return true;
    }

    bool StopAndDrain(std::string* error_out) noexcept override
    {
        events_->push_back(runtime_ && runtime_->accepting()
                               ? "coordinator.stop_before_disarm"
                               : "coordinator.stop");
        stopped_ = true;
        bool success = drain_ok_;
        if (runtime_) {
            success = runtime_->StopAcceptingAndDrain() && success;
        }
        if (!success && error_out) {
            *error_out = drain_error_.empty() ? "injected ROI lane failure"
                                              : drain_error_;
        }
        return success;
    }

    bool MakeAcquisitionSession(
        orange::spatial_roi::SpatialRoiAcquisitionSession* session_out)
        const noexcept override
    {
        events_->push_back("coordinator.make_session");
        if (!session_out || !runtime_) {
            return false;
        }
        const auto& limits = runtime_->producer_limits();
        session_out->runtime = runtime_;
        session_out->recording_id = limits.expected_recording_id;
        session_out->recording_identity_token =
            limits.expected_recording_identity_token;
        session_out->producer_generation = limits.expected_producer_generation;
        session_out->camera_id = limits.expected_camera_id;
        session_out->camera_serial = limits.expected_camera_serial;
        return true;
    }

    orange::spatial_roi::SpatialRoiCameraProducerSnapshot snapshot()
        const override
    {
        orange::spatial_roi::SpatialRoiCameraProducerSnapshot snapshot;
        snapshot.state = stopped_
                              ? orange::spatial_roi::SpatialRoiCameraProducerState::kStopped
                              : (started_
                                     ? orange::spatial_roi::SpatialRoiCameraProducerState::kReady
                                     : orange::spatial_roi::SpatialRoiCameraProducerState::kConstructed);
        snapshot.stream_count = 4;
        return snapshot;
    }

    std::vector<std::string>* events_ = nullptr;
    std::shared_ptr<orange::spatial_roi::SpatialRoiRecordingRuntime> runtime_;
    bool started_ = false;
    bool stopped_ = false;
    bool drain_ok_ = true;
    std::string drain_error_;
};

headless::SpatialRoiHeadlessCameraSessionConfig make_config(
    const nlohmann::json& plan,
    std::vector<std::string>* events,
    const bool make_runtime = true,
    const bool coordinator_drain_ok = true,
    std::string coordinator_drain_error = {})
{
    headless::SpatialRoiHeadlessCameraSessionConfig config;
    config.process.expected_producer_pid = ::getpid();
    config.process.expected_producer_uid = ::geteuid();
    config.producer.independently_verified_plan = plan;
    config.producer.expected_recording_root = "/tmp/headless-session-test";
    config.producer.producer_gpu_id = 0;
    config.producer.expected_recorder_uid = ::geteuid();
    config.process_factory = [events](auto, std::string*) {
        return std::unique_ptr<headless::CameraRecorderProcessHandle>(
            new FakeProcess(events));
    };
    config.producer_factory = [events,
                               plan,
                               make_runtime,
                               coordinator_drain_ok,
                               coordinator_drain_error =
                                   std::move(coordinator_drain_error)](
                                  auto producer_config, std::string*) {
        events->push_back("coordinator.create");
        require(producer_config.expected_recorder_pid == 4321,
                "coordinator did not receive the exact recorder child PID");
        if (!make_runtime) {
            return std::unique_ptr<headless::CameraProducerCoordinatorHandle>(
                new FakeCoordinator(events,
                                    nullptr,
                                    coordinator_drain_ok,
                                    coordinator_drain_error));
        }
        auto runtime = std::make_shared<orange::spatial_roi::SpatialRoiRecordingRuntime>(
            plan,
            "headless-camera",
            0,
            [](const orange::spatial_roi::SpatialRoiLaneDelivery&) {
                return orange::spatial_roi::SpatialRoiLaneSinkResult::kCompleted;
            });
        return std::unique_ptr<headless::CameraProducerCoordinatorHandle>(
            new FakeCoordinator(events,
                                std::move(runtime),
                                coordinator_drain_ok,
                                coordinator_drain_error));
    };
    return config;
}

void test_finish_order_and_controller_visibility()
{
    int device_count = 0;
    if (cudaGetDeviceCount(&device_count) != cudaSuccess || device_count == 0) {
        std::cout << "[SKIP] finish_order_and_controller_visibility (no CUDA device)\n";
        return;
    }

    const nlohmann::json plan = make_plan();
    std::vector<std::string> events;
    events.reserve(16);
    std::string error;
    auto session = headless::SpatialRoiHeadlessCameraSession::Create(
        make_config(plan, &events), &error);
    require(session != nullptr, error);
    require(session->Start(&error), error);
    require(session->acquisition_controller() != nullptr,
            "armed session did not expose its acquisition controller");
    require((events == std::vector<std::string>{
                 "process.start", "process.sockets", "coordinator.create",
                 "coordinator.start", "process.ready",
                 "coordinator.make_session"}),
            "start lifecycle order was not exact");
    require(session->Finish(&error), error);
    require(session->acquisition_controller() == nullptr,
            "finished session exposed its acquisition controller");
    require((events == std::vector<std::string>{
                 "process.start", "process.sockets", "coordinator.create",
                 "coordinator.start", "process.ready",
                 "coordinator.make_session",
                 "coordinator.stop", "process.clean_exit"}),
            "finish lifecycle order was not exact");
}

void test_start_order_and_abort_after_acquisition_failure()
{
    const nlohmann::json plan = make_plan();
    std::vector<std::string> events;
    events.reserve(16);
    std::string error;
    auto session = headless::SpatialRoiHeadlessCameraSession::Create(
        make_config(plan, &events, false), &error);
    require(session != nullptr, error);
    require(!session->Start(&error),
            "missing coordinator acquisition session unexpectedly armed");
    require(session->acquisition_controller() == nullptr,
            "failed start exposed its acquisition controller");
    require((events == std::vector<std::string>{
                 "process.start", "process.sockets", "coordinator.create",
                 "coordinator.start", "process.ready",
                 "coordinator.make_session", "coordinator.stop",
                 "process.stop"}),
            "start failure/abort lifecycle order was not exact");
    require(session->Abort(&error),
            "abort was not idempotent after bounded failed-start cleanup: " +
                error);
}

void test_abort_order_and_bounded_process_stop()
{
    int device_count = 0;
    if (cudaGetDeviceCount(&device_count) != cudaSuccess || device_count == 0) {
        std::cout << "[SKIP] abort_order_and_bounded_process_stop (no CUDA device)\n";
        return;
    }

    const nlohmann::json plan = make_plan();
    std::vector<std::string> events;
    events.reserve(16);
    std::string error;
    auto session = headless::SpatialRoiHeadlessCameraSession::Create(
        make_config(plan, &events), &error);
    require(session != nullptr, error);
    require(session->Start(&error), error);
    require(session->acquisition_controller() != nullptr,
            "armed session did not expose its acquisition controller");
    require(session->Abort(&error), error);
    require(session->acquisition_controller() == nullptr,
            "aborted session exposed its acquisition controller");
    require((events == std::vector<std::string>{
                 "process.start", "process.sockets", "coordinator.create",
                 "coordinator.start", "process.ready",
                 "coordinator.make_session",
                 "coordinator.stop", "process.stop"}),
            "abort lifecycle order was not exact");
}

void test_finish_rejects_async_lane_failure()
{
    int device_count = 0;
    if (cudaGetDeviceCount(&device_count) != cudaSuccess || device_count == 0) {
        std::cout << "[SKIP] finish_rejects_async_lane_failure (no CUDA device)\n";
        return;
    }

    const nlohmann::json plan = make_plan();
    std::vector<std::string> events;
    std::string error;
    auto session = headless::SpatialRoiHeadlessCameraSession::Create(
        make_config(plan,
                    &events,
                    true,
                    false,
                    "injected asynchronous ROI lane failure"),
        &error);
    require(session != nullptr, error);
    require(session->Start(&error), error);
    require(!session->Finish(&error),
            "Finish reported success after an asynchronous ROI lane failure");
    require(session->state() ==
                headless::SpatialRoiHeadlessCameraSessionState::kFailed,
            "async ROI lane failure did not fail the headless session");
    require(error.find("injected asynchronous ROI lane failure") !=
                std::string::npos,
            "Finish did not propagate the asynchronous ROI lane failure reason");
}

}  // namespace

int main()
{
    const std::vector<std::pair<const char*, std::function<void()>>> tests = {
        {"start_order_and_abort_after_acquisition_failure",
         test_start_order_and_abort_after_acquisition_failure},
        {"finish_order_and_controller_visibility",
         test_finish_order_and_controller_visibility},
        {"abort_order_and_bounded_process_stop",
         test_abort_order_and_bounded_process_stop},
        {"finish_rejects_async_lane_failure",
         test_finish_rejects_async_lane_failure},
    };
    int failures = 0;
    for (const auto& [name, test] : tests) {
        try {
            test();
            std::cout << "[PASS] " << name << '\n';
        } catch (const std::exception& exception) {
            ++failures;
            std::cerr << "[FAIL] " << name << ": " << exception.what() << '\n';
        }
    }
    return failures == 0 ? 0 : 1;
}
