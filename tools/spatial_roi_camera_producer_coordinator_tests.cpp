#include "spatial_roi_camera_producer_coordinator.h"

#include "session/spatial_roi_recording_config.h"
#include "shaman_v2_recording_identity.h"

#include <cstdint>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

namespace roi = orange::session::spatial_roi;
namespace producer = orange::spatial_roi;
using json = nlohmann::json;

void require(bool condition, const std::string& message)
{
    if (!condition) {
        throw std::runtime_error(message);
    }
}

std::string digest(char fill)
{
    return "sha256:" + std::string(64, fill);
}

struct Fixture {
    json plan;
    json contract;
    roi::SpatialRoiRecorderRuntimeGpuMapping mapping;
    std::string root = "/tmp/orange_camera_producer_coordinator_test";
};

Fixture make_fixture()
{
    roi::Config config = roi::default_config();
    config.enabled = true;
    config.buffering.pool_frames_per_stream = 3;
    config.buffering.queue_frames_per_stream = 2;
    config.recording_limits.max_frames_per_stream = 200;
    config.recording_limits.max_media_bytes_per_stream = 2000000;
    config.recording_limits.max_evidence_bytes_per_stream = 200000;
    config.admission.max_rois_per_camera = 4;
    config.admission.max_total_rois = 4;
    config.admission.max_total_encoder_streams = 4;
    config.admission.max_total_pixel_rate = 100000000;
    config.admission.max_total_pool_bytes = 100000000;
    config.admission.max_total_queue_frames = 32;
    config.admission.max_total_media_bytes = 8000000;
    config.admission.max_total_evidence_bytes = 800000;

    roi::CameraConfig camera;
    camera.camera_id = 7;
    camera.camera_serial = "producer-camera";
    camera.native_raster = {64, 48};
    camera.source_frame_rate = 30;
    camera.arena_group_id = "producer-group";
    camera.layout = {"layout", digest('1')};
    camera.materialization = {"materialization", digest('2')};
    camera.registration = {"registration", digest('3')};
    for (std::size_t index = 0; index < 4; ++index) {
        roi::RoiConfig item;
        item.roi_id = "roi_" + std::to_string(index + 1);
        item.region_id = "region_" + std::to_string(index + 1);
        item.required = true;
        item.content_rect = {
            static_cast<std::uint32_t>((index % 2) * 16),
            static_cast<std::uint32_t>((index / 2) * 16),
            16,
            16};
        item.logical_stream_id = roi::expected_logical_stream_id(
            camera.camera_serial, item.roi_id);
        item.artifact_stem = roi::expected_artifact_stem(
            camera.camera_serial, item.roi_id);
        camera.rois.push_back(std::move(item));
    }
    config.cameras.emplace(camera.camera_serial, std::move(camera));

    roi::PlanContext context;
    context.recording_id = "producer-coordinator-recording";
    context.recording_identity_token =
        orange::shaman_v2_recording_identity::token_for_recording_id(
            context.recording_id);
    context.generated_at_utc = "2026-09-01T00:00:00Z";
    context.producer_generation = "producer-coordinator-generation";

    Fixture fixture;
    std::string error;
    require(roi::build_plan(config, context, &fixture.plan, nullptr, &error),
            error);
    require(roi::verify_plan(fixture.plan, &error), error);
    fixture.mapping.analytics_gpu_by_camera_serial.emplace(
        "producer-camera", 2);
    for (std::size_t index = 0; index < 4; ++index) {
        fixture.mapping.recorder_gpu_by_logical_stream_id.emplace(
            roi::expected_logical_stream_id(
                "producer-camera", "roi_" + std::to_string(index + 1)),
            static_cast<int>(3 + index));
    }
    require(roi::build_spatial_roi_recorder_contract(
                fixture.plan,
                fixture.root,
                fixture.mapping,
                &fixture.contract,
                &error),
            error);
    return fixture;
}

struct StreamControl {
    std::vector<std::string>* calls = nullptr;
    bool start_ok = true;
    std::size_t starts = 0;
    std::size_t submits = 0;
    std::size_t stops = 0;
    std::size_t releases = 0;
    bool release_ok = true;
};

class FakeStream final : public producer::SpatialRoiCameraProducerStream {
public:
    FakeStream(std::shared_ptr<StreamControl> control, std::string id)
        : control_(std::move(control)), id_(std::move(id))
    {
    }

    bool Start(std::string* error_out) override
    {
        ++control_->starts;
        control_->calls->push_back("start:" + id_);
        if (!control_->start_ok && error_out) {
            *error_out = "injected stream start failure";
        }
        return control_->start_ok;
    }

    producer::SpatialRoiLaneSinkResult Submit(
        const producer::SpatialRoiLaneDelivery&) override
    {
        ++control_->submits;
        return producer::SpatialRoiLaneSinkResult::kCompleted;
    }

    void Stop() noexcept override
    {
        ++control_->stops;
        control_->calls->push_back("stop:" + id_);
    }

    bool ReleaseProducerCudaResourcesAfterRecorderReaped(
        std::string* error_out) noexcept override
    {
        if (released_) {
            return true;
        }
        ++control_->releases;
        control_->calls->push_back("release:" + id_);
        if (!control_->release_ok && error_out) {
            *error_out = "injected producer resource release failure";
        }
        if (control_->release_ok) {
            released_ = true;
        }
        return control_->release_ok;
    }

private:
    std::shared_ptr<StreamControl> control_;
    std::string id_;
    bool released_ = false;
};

class FakeRuntime final : public producer::SpatialRoiCameraProducerRuntime {
public:
    explicit FakeRuntime(producer::SpatialRoiLaneSink sink,
                         producer::SpatialRoiRuntimeSubmitStatus status,
                         std::size_t* submit_count,
                         std::vector<std::string>* calls,
                         bool drain_ok,
                         std::string drain_error,
                         std::size_t* destroy_count = nullptr)
        : sink_(std::move(sink)),
          status_(status),
          submit_count_(submit_count),
          calls_(calls),
          drain_ok_(drain_ok),
          drain_error_(std::move(drain_error)),
          destroy_count_(destroy_count)
    {
    }

    ~FakeRuntime() override
    {
        if (destroy_count_) {
            ++*destroy_count_;
        }
    }

    producer::SpatialRoiBatchSubmission TrySubmit(
        const producer::SpatialRoiSourceView&) override
    {
        ++*submit_count_;
        // The runtime's real implementation performs the four plan-ordered
        // lane callbacks. This host fake intentionally records only that the
        // coordinator supplied one shared sink and called the runtime once.
        require(static_cast<bool>(sink_),
                "coordinator did not provide a runtime fanout sink");
        producer::SpatialRoiBatchSubmission result;
        result.status = status_;
        result.producer_status =
            status_ == producer::SpatialRoiRuntimeSubmitStatus::kAccepted
                ? producer::SpatialRoiBatchStatus::kAccepted
                : producer::SpatialRoiBatchStatus::kInvalidArgument;
        result.lane_count = 4;
        result.admitted_lane_count =
            status_ == producer::SpatialRoiRuntimeSubmitStatus::kAccepted ? 4 : 0;
        return result;
    }

    void StopAccepting() noexcept override
    {
        ++stop_count_;
        calls_->push_back("runtime_stop");
    }
    bool StopAcceptingAndDrain(std::string* error_out,
                               bool* fully_drained_out) noexcept override
    {
        ++drain_count_;
        calls_->push_back("runtime_drain");
        if (fully_drained_out) {
            *fully_drained_out = true;
        }
        if (error_out) {
            *error_out = drain_ok_ ? std::string{} : drain_error_;
        }
        return drain_ok_;
    }

    std::size_t stop_count_ = 0;
    std::size_t drain_count_ = 0;

private:
    producer::SpatialRoiLaneSink sink_;
    producer::SpatialRoiRuntimeSubmitStatus status_;
    std::size_t* submit_count_;
    std::vector<std::string>* calls_;
    bool drain_ok_ = true;
    std::string drain_error_;
    std::size_t* destroy_count_ = nullptr;
};

producer::SpatialRoiCameraProducerConfig config_for(
    const Fixture& fixture,
    std::vector<std::shared_ptr<StreamControl>>* controls,
    std::vector<std::string>* calls,
    std::size_t* runtime_submit_count,
    producer::SpatialRoiRuntimeSubmitStatus runtime_status =
        producer::SpatialRoiRuntimeSubmitStatus::kAccepted,
    bool runtime_drain_ok = true,
    std::string runtime_drain_error = {},
    std::size_t* runtime_destroy_count = nullptr)
{
    producer::SpatialRoiCameraProducerConfig config;
    config.candidate_contract = fixture.contract;
    config.independently_verified_plan = fixture.plan;
    config.expected_recording_root = fixture.root;
    config.expected_gpu_mapping = fixture.mapping;
    config.producer_gpu_id = 2;
    config.expected_recorder_pid = 1000;
    config.expected_recorder_uid = 1000;
    config.stream_factory = [controls, calls](
                                const roi::SpatialRoiRecorderStreamView& stream,
                                std::size_t index,
                                std::string*) {
        require(index == controls->size(),
                "stream factory was not invoked in plan order");
        auto control = std::make_shared<StreamControl>();
        control->calls = calls;
        controls->push_back(control);
        return std::unique_ptr<producer::SpatialRoiCameraProducerStream>(
            new FakeStream(std::move(control), stream.logical_stream_id));
    };
    config.runtime_factory = [runtime_submit_count,
                              runtime_status,
                              calls,
                              runtime_drain_ok,
                              runtime_drain_error = std::move(runtime_drain_error),
                              runtime_destroy_count](
                                 const json&,
                                 const std::string& camera_serial,
                                 int producer_gpu_id,
                                 producer::SpatialRoiLaneSink sink,
                                 std::string*) {
        require(camera_serial == "producer-camera",
                "runtime factory received the wrong camera");
        require(producer_gpu_id == 2,
                "runtime factory received the wrong producer GPU");
        return std::unique_ptr<producer::SpatialRoiCameraProducerRuntime>(
            new FakeRuntime(std::move(sink), runtime_status,
                            runtime_submit_count, calls, runtime_drain_ok,
                            runtime_drain_error, runtime_destroy_count));
    };
    return config;
}

void test_status_names()
{
    require(std::string(producer::spatial_roi_camera_producer_state_name(
                producer::SpatialRoiCameraProducerState::kReady)) == "ready",
            "producer state name changed");
    require(std::string(producer::spatial_roi_camera_producer_submit_status_name(
                producer::SpatialRoiCameraProducerSubmitStatus::kSubmitted)) ==
                "submitted",
            "producer submit status name changed");
}

void test_one_source_update_and_plan_order()
{
    const Fixture fixture = make_fixture();
    std::vector<std::shared_ptr<StreamControl>> controls;
    std::vector<std::string> calls;
    std::size_t runtime_submit_count = 0;
    auto owner = producer::SpatialRoiCameraProducerCoordinator::Create(
        config_for(fixture, &controls, &calls, &runtime_submit_count), nullptr);
    require(owner != nullptr, "valid camera producer was rejected");
    require(controls.size() == 4, "producer did not construct four streams");
    require(owner->contract().stream_order.size() == 4,
            "producer lost authenticated stream order");
    std::string error;
    require(owner->Start(&error), error);
    require(owner->ready(), "producer did not become ready");
    require(calls.size() == 4 && calls[0].find("start:") == 0 &&
                calls[3].find("start:") == 0,
            "producer did not start all streams");

    producer::SpatialRoiSourceView source;
    const auto first = owner->Submit(source);
    require(first.status ==
                producer::SpatialRoiCameraProducerSubmitStatus::kSubmitted,
            "one accepted source did not produce one accepted update");
    require(runtime_submit_count == 1,
            "one source frame was submitted to the runtime more than once");
    require(owner->snapshot().submit_attempted == 1 &&
                owner->snapshot().submitted == 1,
            "producer counters did not record one source update");

    owner->StopAndDrain();
    require(owner->state() == producer::SpatialRoiCameraProducerState::kStopped,
            "producer did not stop after drain");
    for (const auto& control : controls) {
        require(control->starts == 1 && control->stops == 1,
                "producer stream lifecycle was not bounded");
    }
    require(calls.size() == 9 && calls[4] == "runtime_drain" &&
                calls[5].find("stop:") == 0 && calls[8].find("stop:") == 0,
            "producer closed a transport before runtime drain completed");
}

void test_post_reap_resource_release_is_idempotent()
{
    const Fixture fixture = make_fixture();
    std::vector<std::shared_ptr<StreamControl>> controls;
    std::vector<std::string> calls;
    std::size_t runtime_submit_count = 0;
    std::size_t runtime_destroy_count = 0;
    auto owner = producer::SpatialRoiCameraProducerCoordinator::Create(
        config_for(fixture,
                   &controls,
                   &calls,
                   &runtime_submit_count,
                   producer::SpatialRoiRuntimeSubmitStatus::kAccepted,
                   true,
                   {},
                   &runtime_destroy_count),
        nullptr);
    require(owner != nullptr, "resource release test owner was rejected");
    require(owner->Start(), "resource release test owner did not start");
    require(owner->StopAndDrain(), "resource release test owner did not drain");
    require(runtime_destroy_count == 0,
            "producer batch runtime was destroyed before recorder reap proof");
    for (const auto& control : controls) {
        require(control->releases == 0,
                "producer resources were released before recorder reap proof");
    }

    std::string error;
    require(owner->ReleaseProducerCudaResourcesAfterRecorderReaped(&error),
            error);
    require(runtime_destroy_count == 1,
            "producer batch runtime was not released at the post-reap boundary");
    require(owner->ReleaseProducerCudaResourcesAfterRecorderReaped(&error),
            "second post-reap resource release was not idempotent: " + error);
    for (const auto& control : controls) {
        require(control->releases == 1,
                "post-reap resource release ran more than once per stream");
    }
}

void test_post_reap_resource_release_requires_terminal_admission_state()
{
    const Fixture fixture = make_fixture();
    std::vector<std::shared_ptr<StreamControl>> controls;
    std::vector<std::string> calls;
    std::size_t runtime_submit_count = 0;
    std::size_t runtime_destroy_count = 0;
    auto owner = producer::SpatialRoiCameraProducerCoordinator::Create(
        config_for(fixture,
                   &controls,
                   &calls,
                   &runtime_submit_count,
                   producer::SpatialRoiRuntimeSubmitStatus::kAccepted,
                   true,
                   {},
                   &runtime_destroy_count),
        nullptr);
    require(owner != nullptr, "resource release state test owner was rejected");
    require(owner->Start(), "resource release state test owner did not start");

    std::string error;
    require(!owner->ReleaseProducerCudaResourcesAfterRecorderReaped(&error),
            "ready producer allowed post-reap resource release");
    require(error.find("stopped or failed") != std::string::npos,
            "premature resource release did not explain its lifecycle boundary");
    require(runtime_destroy_count == 0,
            "premature resource release destroyed the producer runtime");
    for (const auto& control : controls) {
        require(control->releases == 0,
                "premature resource release reached a producer stream");
    }

    require(owner->StopAndDrain(), "resource release state test did not drain");
    require(owner->ReleaseProducerCudaResourcesAfterRecorderReaped(&error),
            error);
    require(runtime_destroy_count == 1,
            "terminal producer did not release its runtime");
}

void test_failed_submit_still_requires_explicit_runtime_drain()
{
    const Fixture fixture = make_fixture();
    std::vector<std::shared_ptr<StreamControl>> controls;
    std::vector<std::string> calls;
    std::size_t runtime_submit_count = 0;
    std::size_t runtime_destroy_count = 0;
    auto owner = producer::SpatialRoiCameraProducerCoordinator::Create(
        config_for(fixture,
                   &controls,
                   &calls,
                   &runtime_submit_count,
                   producer::SpatialRoiRuntimeSubmitStatus::kCudaError,
                   true,
                   {},
                   &runtime_destroy_count),
        nullptr);
    require(owner != nullptr, "failed-submit drain test owner was rejected");
    require(owner->Start(), "failed-submit drain test owner did not start");
    const auto submission = owner->Submit(producer::SpatialRoiSourceView{});
    require(submission.status ==
                producer::SpatialRoiCameraProducerSubmitStatus::kCudaError &&
                owner->failed(),
            "injected runtime CUDA error did not fail the producer");

    std::string error;
    require(!owner->ReleaseProducerCudaResourcesAfterRecorderReaped(&error),
            "failed producer released resources before runtime drain");
    require(error.find("completed StopAndDrain") != std::string::npos,
            "failed producer did not report the missing drain boundary");
    require(runtime_destroy_count == 0,
            "failed producer runtime was destroyed before explicit drain");
    for (const auto& control : controls) {
        require(control->releases == 0,
                "failed producer released a stream before explicit drain");
    }

    require(!owner->StopAndDrain(),
            "already-failed producer drain was relabeled successful");
    require(owner->ReleaseProducerCudaResourcesAfterRecorderReaped(&error),
            "drained failed producer could not release after reap: " + error);
    require(runtime_destroy_count == 1,
            "drained failed producer retained its runtime after reap");
}

void test_post_reap_resource_release_failure_is_retained()
{
    const Fixture fixture = make_fixture();
    std::vector<std::shared_ptr<StreamControl>> controls;
    std::vector<std::string> calls;
    std::size_t runtime_submit_count = 0;
    auto owner = producer::SpatialRoiCameraProducerCoordinator::Create(
        config_for(fixture, &controls, &calls, &runtime_submit_count), nullptr);
    require(owner != nullptr, "resource release failure owner was rejected");
    require(owner->Start(), "resource release failure owner did not start");
    require(owner->StopAndDrain(), "resource release failure owner did not drain");
    controls.front()->release_ok = false;

    std::string error;
    require(!owner->ReleaseProducerCudaResourcesAfterRecorderReaped(&error),
            "injected resource release failure was reported as success");
    require(owner->failed() &&
                error.find("injected producer resource release failure") !=
                    std::string::npos,
            "resource release failure was not latched and surfaced");
    controls.front()->release_ok = true;
    require(owner->ReleaseProducerCudaResourcesAfterRecorderReaped(&error),
            "retained resource release could not be retried: " + error);
    require(controls.front()->releases == 2,
            "failed resource release was not retained for a retry");
    for (std::size_t index = 1; index < controls.size(); ++index) {
        require(controls[index]->releases == 1,
                "healthy stream resource release was repeated after failure");
    }
}

void test_runtime_incomplete_is_not_silent_success()
{
    const Fixture fixture = make_fixture();
    std::vector<std::shared_ptr<StreamControl>> controls;
    std::vector<std::string> calls;
    std::size_t runtime_submit_count = 0;
    auto owner = producer::SpatialRoiCameraProducerCoordinator::Create(
        config_for(fixture,
                   &controls,
                   &calls,
                   &runtime_submit_count,
                   producer::SpatialRoiRuntimeSubmitStatus::kIncomplete),
        nullptr);
    require(owner != nullptr, "incomplete test owner was rejected");
    require(owner->Start(), "incomplete test owner did not start");
    const auto result = owner->Submit(producer::SpatialRoiSourceView{});
    require(result.status ==
                producer::SpatialRoiCameraProducerSubmitStatus::kRuntimeIncomplete,
            "strict incomplete runtime result was relabeled success");
    require(owner->snapshot().incomplete == 1 && runtime_submit_count == 1,
            "incomplete runtime update was not counted once");
}

void test_async_lane_failure_blocks_stop_success()
{
    const Fixture fixture = make_fixture();
    std::vector<std::shared_ptr<StreamControl>> controls;
    std::vector<std::string> calls;
    std::size_t runtime_submit_count = 0;
    auto owner = producer::SpatialRoiCameraProducerCoordinator::Create(
        config_for(fixture,
                   &controls,
                   &calls,
                   &runtime_submit_count,
                   producer::SpatialRoiRuntimeSubmitStatus::kAccepted,
                   false,
                   "lane 2 sink failed asynchronously"),
        nullptr);
    require(owner != nullptr, "async lane failure owner was rejected");
    require(owner->Start(), "async lane failure owner did not start");
    require(owner->Submit(producer::SpatialRoiSourceView{}).submitted(),
            "async lane failure source was not admitted");

    std::string error;
    require(!owner->StopAndDrain(&error),
            "async lane failure was reported as successful drain");
    require(owner->failed() && owner->state() ==
                producer::SpatialRoiCameraProducerState::kFailed,
            "async lane failure did not make producer terminally failed");
    require(error.find("lane 2 sink failed asynchronously") !=
                std::string::npos,
            "async lane failure reason was not propagated");
}

void test_authentication_precedes_factories()
{
    const Fixture fixture = make_fixture();
    std::vector<std::shared_ptr<StreamControl>> controls;
    std::vector<std::string> calls;
    std::size_t runtime_submit_count = 0;
    auto config = config_for(fixture, &controls, &calls, &runtime_submit_count);
    config.candidate_contract["recording_id"] = "untrusted-recording";
    std::string error;
    auto owner = producer::SpatialRoiCameraProducerCoordinator::Create(
        std::move(config), &error);
    require(owner == nullptr && !error.empty(),
            "mutated producer contract was accepted");
    require(controls.empty(), "stream factory ran before authentication");
}

}  // namespace

int main()
{
    try {
        test_status_names();
        test_one_source_update_and_plan_order();
        test_post_reap_resource_release_is_idempotent();
        test_post_reap_resource_release_requires_terminal_admission_state();
        test_failed_submit_still_requires_explicit_runtime_drain();
        test_post_reap_resource_release_failure_is_retained();
        test_runtime_incomplete_is_not_silent_success();
        test_async_lane_failure_blocks_stop_success();
        test_authentication_precedes_factories();
        std::cout << "spatial_roi_camera_producer_coordinator_tests: PASS\n";
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "spatial_roi_camera_producer_coordinator_tests: FAIL: "
                  << exception.what() << '\n';
        return 1;
    }
}
