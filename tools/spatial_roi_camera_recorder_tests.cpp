#include "spatial_roi_camera_recorder.h"

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

using json = nlohmann::json;
namespace camera_recording = orange::spatial_roi::recording;
namespace spatial_roi = orange::session::spatial_roi;

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

spatial_roi::RoiConfig make_roi(const std::string& camera_serial,
                                std::size_t index)
{
    spatial_roi::RoiConfig roi;
    roi.roi_id = "roi_" + std::to_string(index + 1);
    roi.region_id = "region_" + std::to_string(index + 1);
    roi.has_arena_id = true;
    roi.arena_id = "arena_" + std::to_string(index + 1);
    roi.required = true;
    roi.content_rect = {
        static_cast<std::uint32_t>((index % 2) * 20),
        static_cast<std::uint32_t>((index / 2) * 20),
        16,
        16};
    roi.logical_stream_id =
        spatial_roi::expected_logical_stream_id(camera_serial, roi.roi_id);
    roi.artifact_stem =
        spatial_roi::expected_artifact_stem(camera_serial, roi.roi_id);
    return roi;
}

struct Fixture {
    json plan;
    spatial_roi::SpatialRoiRecorderRuntimeGpuMapping mapping;
    json contract;
    std::string recording_root = "/tmp/orange_camera_recorder_test";
};

Fixture make_fixture()
{
    spatial_roi::Config config = spatial_roi::default_config();
    config.enabled = true;
    config.buffering.pool_frames_per_stream = 4;
    config.buffering.queue_frames_per_stream = 8;
    config.recording_limits.max_frames_per_stream = 2000;
    config.recording_limits.max_media_bytes_per_stream = 2000000;
    config.recording_limits.max_evidence_bytes_per_stream = 200000;
    config.admission.max_rois_per_camera = 4;
    config.admission.max_total_rois = 4;
    config.admission.max_total_encoder_streams = 4;
    config.admission.max_total_pixel_rate = 100000000ULL;
    config.admission.max_total_pool_bytes = 100000000ULL;
    config.admission.max_total_queue_frames = 32;
    config.admission.max_total_media_bytes = 8000000;
    config.admission.max_total_evidence_bytes = 800000;

    spatial_roi::CameraConfig camera;
    camera.camera_id = 3;
    camera.camera_serial = "2010096";
    camera.native_raster = {100, 100};
    camera.source_frame_rate = 100;
    camera.arena_group_id = "group_1";
    camera.layout = {"layout_1", digest('1')};
    camera.materialization = {"materialization_1", digest('a')};
    camera.registration = {"registration_1", digest('c')};
    for (std::size_t index = 0; index < 4; ++index) {
        camera.rois.push_back(make_roi(camera.camera_serial, index));
    }
    config.cameras.emplace(camera.camera_serial, std::move(camera));

    spatial_roi::PlanContext context;
    context.recording_id = "camera-recorder-test";
    context.recording_identity_token =
        orange::shaman_v2_recording_identity::token_for_recording_id(
            context.recording_id);
    context.generated_at_utc = "2026-09-01T00:00:00Z";
    context.producer_generation = "generation_camera_recorder";

    Fixture fixture;
    std::string error;
    require(spatial_roi::build_plan(
                config, context, &fixture.plan, nullptr, &error),
            error);
    fixture.mapping.analytics_gpu_by_camera_serial.emplace("2010096", 5);
    for (std::size_t index = 0; index < 4; ++index) {
        fixture.mapping.recorder_gpu_by_logical_stream_id.emplace(
            spatial_roi::expected_logical_stream_id(
                "2010096", "roi_" + std::to_string(index + 1)),
            static_cast<int>(6 + index));
    }
    require(spatial_roi::build_spatial_roi_recorder_contract(
                fixture.plan,
                fixture.recording_root,
                fixture.mapping,
                &fixture.contract,
                &error),
            error);
    return fixture;
}

struct FakeCoreControl {
    std::string logical_stream_id;
    std::vector<std::string>* calls = nullptr;
    camera_recording::SpatialRoiCameraRecorderReadinessStatus readiness =
        camera_recording::SpatialRoiCameraRecorderReadinessStatus::kPending;
    camera_recording::SpatialRoiCameraRecorderEofStatus eof =
        camera_recording::SpatialRoiCameraRecorderEofStatus::kPending;
    bool start_ok = true;
    bool stop_ok = true;
    bool drain_ok = true;
    bool finalize_ok = true;
    bool throw_on_readiness = false;
    std::size_t stop_calls = 0;
    std::string readiness_error;
    std::string eof_error;
};

class FakeCore final
    : public camera_recording::SpatialRoiCameraRecorderStreamCore {
public:
    explicit FakeCore(std::shared_ptr<FakeCoreControl> control)
        : control_(std::move(control))
    {
    }

    bool Start(std::string* error_out) override
    {
        note("start");
        if (!control_->start_ok && error_out) {
            *error_out = "injected start failure";
        }
        return control_->start_ok;
    }

    camera_recording::SpatialRoiCameraRecorderReadinessStatus PollReadiness(
        std::string* error_out) override
    {
        note("ready");
        if (control_->throw_on_readiness) {
            throw std::runtime_error("injected readiness exception");
        }
        if (control_->readiness ==
                camera_recording::SpatialRoiCameraRecorderReadinessStatus::
                    kFailed &&
            error_out) {
            *error_out = control_->readiness_error;
        }
        return control_->readiness;
    }

    camera_recording::SpatialRoiCameraRecorderEofStatus PollEof(
        std::string* error_out) override
    {
        note("eof");
        if (control_->eof ==
                camera_recording::SpatialRoiCameraRecorderEofStatus::kFailed &&
            error_out) {
            *error_out = control_->eof_error;
        }
        return control_->eof;
    }

    bool StopAdmission(std::string* error_out) override
    {
        note("stop");
        ++control_->stop_calls;
        if (!control_->stop_ok && error_out) {
            *error_out = "injected stop failure";
        }
        return control_->stop_ok;
    }

    bool Drain(std::string* error_out) override
    {
        note("drain");
        if (!control_->drain_ok && error_out) {
            *error_out = "injected drain failure";
        }
        return control_->drain_ok;
    }

    bool Finalize(std::string* error_out) override
    {
        note("finalize");
        if (!control_->finalize_ok && error_out) {
            *error_out = "injected finalize failure";
        }
        return control_->finalize_ok;
    }

private:
    void note(const char* operation)
    {
        if (control_->calls) {
            control_->calls->push_back(std::string(operation) + ":" +
                                       control_->logical_stream_id);
        }
    }

    std::shared_ptr<FakeCoreControl> control_;
};

struct OwnerFixture {
    Fixture authority = make_fixture();
    std::vector<std::string> calls;
    std::vector<std::string> factory_order;
    std::vector<std::shared_ptr<FakeCoreControl>> controls;
};

std::unique_ptr<camera_recording::SpatialRoiCameraRecorder> make_owner(
    OwnerFixture* fixture,
    std::string* error_out = nullptr)
{
    auto owner = camera_recording::SpatialRoiCameraRecorder::Create(
        fixture->authority.contract,
        fixture->authority.plan,
        fixture->authority.recording_root,
        fixture->authority.mapping,
        [fixture](const spatial_roi::SpatialRoiRecorderStreamView& stream,
                  std::size_t plan_index,
                  std::string*) {
            require(plan_index == fixture->controls.size(),
                    "factory index was not dense plan order");
            fixture->factory_order.push_back(stream.logical_stream_id);
            auto control = std::make_shared<FakeCoreControl>();
            control->logical_stream_id = stream.logical_stream_id;
            control->calls = &fixture->calls;
            fixture->controls.push_back(control);
            return std::make_unique<FakeCore>(std::move(control));
        },
        error_out);
    if (owner) {
        camera_recording::SpatialRoiRecorderStoragePreflightResult preflight;
        preflight.checked = true;
        preflight.passed = true;
        preflight.status = "passed";
        const auto& aggregate = fixture->authority.contract.at("aggregate_bounds");
        preflight.max_media_bytes_total =
            aggregate.at("max_media_bytes_total").get<std::uint64_t>();
        preflight.max_evidence_bytes_total =
            aggregate.at("max_evidence_bytes_total").get<std::uint64_t>();
        preflight.required_bytes =
            preflight.max_media_bytes_total +
            preflight.max_evidence_bytes_total +
            preflight.policy.reserved_free_bytes;
        preflight.filesystem.block_size_bytes = 1;
        preflight.filesystem.total_blocks = preflight.required_bytes;
        preflight.filesystem.available_blocks = preflight.required_bytes;
        preflight.capacity_bytes = preflight.required_bytes;
        preflight.available_bytes = preflight.required_bytes;
        owner->set_storage_preflight_result(std::move(preflight));
    }
    return owner;
}

std::vector<std::string> expected_calls(const char* operation)
{
    std::vector<std::string> result;
    for (std::size_t index = 0; index < 4; ++index) {
        result.push_back(
            std::string(operation) + ":2010096_spatial_roi_roi_" +
            std::to_string(index + 1));
    }
    return result;
}

void accepts_plan_order_and_completes_four_stream_lifecycle()
{
    OwnerFixture fixture;
    std::string error;
    auto owner = make_owner(&fixture, &error);
    require(owner != nullptr, error);
    require(fixture.factory_order ==
                std::vector<std::string>{
                    "2010096_spatial_roi_roi_1",
                    "2010096_spatial_roi_roi_2",
                    "2010096_spatial_roi_roi_3",
                    "2010096_spatial_roi_roi_4"},
            "factory was not invoked in authenticated plan order");
    require(owner->contract().product_kind == "fixed_region" &&
                owner->contract().stream_count == 4,
            "owner did not retain the fixed-region camera contract");

    require(owner->Start(&error), error);
    require(fixture.calls == expected_calls("start"),
            "streams did not start in plan order");

    fixture.controls[3]->readiness =
        camera_recording::SpatialRoiCameraRecorderReadinessStatus::kReady;
    fixture.controls[1]->readiness =
        camera_recording::SpatialRoiCameraRecorderReadinessStatus::kReady;
    require(owner->PollReadiness(&error), error);
    require(owner->state() ==
                camera_recording::SpatialRoiCameraRecorderState::kStarting &&
                !owner->snapshot().ready,
            "partial readiness armed the aggregate");

    fixture.controls[0]->readiness =
        camera_recording::SpatialRoiCameraRecorderReadinessStatus::kReady;
    fixture.controls[2]->readiness =
        camera_recording::SpatialRoiCameraRecorderReadinessStatus::kReady;
    require(owner->PollReadiness(&error), error);
    require(owner->state() ==
                camera_recording::SpatialRoiCameraRecorderState::kReady &&
                owner->snapshot().ready,
            "four ready streams did not arm the aggregate");

    fixture.controls[2]->eof =
        camera_recording::SpatialRoiCameraRecorderEofStatus::kClean;
    fixture.controls[0]->eof =
        camera_recording::SpatialRoiCameraRecorderEofStatus::kClean;
    require(owner->PollEof(&error), error);
    require(owner->state() ==
                camera_recording::SpatialRoiCameraRecorderState::kAwaitingEof &&
                !owner->snapshot().clean_eof,
            "partial EOF closed the aggregate");

    fixture.controls[1]->eof =
        camera_recording::SpatialRoiCameraRecorderEofStatus::kClean;
    fixture.controls[3]->eof =
        camera_recording::SpatialRoiCameraRecorderEofStatus::kClean;
    require(owner->PollEof(&error), error);
    require(owner->state() ==
                camera_recording::SpatialRoiCameraRecorderState::kEofObserved &&
                owner->snapshot().clean_eof,
            "four clean EOF reports did not close intake");

    fixture.calls.clear();
    require(owner->DrainAndFinalize(&error), error);
    std::vector<std::string> expected = expected_calls("stop");
    const auto drains = expected_calls("drain");
    const auto finalizes = expected_calls("finalize");
    expected.insert(expected.end(), drains.begin(), drains.end());
    expected.insert(expected.end(), finalizes.begin(), finalizes.end());
    require(fixture.calls == expected,
            "stop/drain/finalize was not complete plan-ordered orchestration");

    const auto snapshot = owner->snapshot();
    require(snapshot.completed && snapshot.ready && snapshot.clean_eof &&
                snapshot.first_failure.empty() && snapshot.streams.size() == 4,
            "completed camera snapshot is incomplete");
    for (std::size_t index = 0; index < snapshot.streams.size(); ++index) {
        const auto& stream = snapshot.streams[index];
        require(stream.plan_index == index && stream.started && stream.ready &&
                    stream.clean_eof && stream.admission_stopped &&
                    stream.drained && stream.finalized && !stream.failed,
                "completed stream lifecycle snapshot is incomplete");
        require(stream.recording_id == snapshot.recording_id &&
                    stream.session_id == snapshot.session_id &&
                    stream.camera_serial == snapshot.camera_serial,
                "stream snapshot lost shared camera/session identity");
    }
    const std::size_t call_count = fixture.calls.size();
    require(owner->DrainAndFinalize(&error) &&
                fixture.calls.size() == call_count,
            "completed finalization was not idempotent");
}

void rejects_mutated_contract_before_factory()
{
    OwnerFixture fixture;
    fixture.authority.contract["recording_id"] = "untrusted-recording";
    std::size_t factory_calls = 0;
    std::string error;
    auto owner = camera_recording::SpatialRoiCameraRecorder::Create(
        fixture.authority.contract,
        fixture.authority.plan,
        fixture.authority.recording_root,
        fixture.authority.mapping,
        [&factory_calls](const spatial_roi::SpatialRoiRecorderStreamView&,
                         std::size_t,
                         std::string*) {
            ++factory_calls;
            return std::unique_ptr<
                camera_recording::SpatialRoiCameraRecorderStreamCore>{};
        },
        &error);
    require(owner == nullptr && factory_calls == 0 && !error.empty(),
            "untrusted contract reached the output-core factory");
}

void rejects_partial_factory_construction()
{
    Fixture fixture = make_fixture();
    std::vector<std::size_t> calls;
    std::string error;
    auto owner = camera_recording::SpatialRoiCameraRecorder::Create(
        fixture.contract,
        fixture.plan,
        fixture.recording_root,
        fixture.mapping,
        [&calls](const spatial_roi::SpatialRoiRecorderStreamView& stream,
                 std::size_t index,
                 std::string* factory_error) {
            calls.push_back(index);
            if (index == 2) {
                *factory_error = "injected factory rejection";
                return std::unique_ptr<
                    camera_recording::SpatialRoiCameraRecorderStreamCore>{};
            }
            auto control = std::make_shared<FakeCoreControl>();
            control->logical_stream_id = stream.logical_stream_id;
            return std::unique_ptr<
                camera_recording::SpatialRoiCameraRecorderStreamCore>(
                new FakeCore(std::move(control)));
        },
        &error);
    require(owner == nullptr && calls == std::vector<std::size_t>{0, 1, 2} &&
                error.find("injected factory rejection") != std::string::npos,
            "partial factory construction did not fail closed in plan order");
}

void partial_start_failure_stops_every_attempted_core()
{
    OwnerFixture fixture;
    std::string error;
    auto owner = make_owner(&fixture, &error);
    require(owner != nullptr, error);
    fixture.controls[2]->start_ok = false;
    require(!owner->Start(&error) && owner->failed(),
            "injected start failure did not fail the aggregate");
    const auto snapshot = owner->snapshot();
    require(snapshot.streams[0].admission_stopped &&
                snapshot.streams[1].admission_stopped &&
                snapshot.streams[2].stop_admission_attempted &&
                !snapshot.streams[3].start_attempted &&
                !snapshot.streams[3].stop_admission_attempted,
            "partial startup did not stop every attempted core exactly once");
    require(snapshot.first_failure_stream_id ==
                "2010096_spatial_roi_roi_3" &&
                error.find("start failed") != std::string::npos,
            "partial startup lost first-failure identity");
}

void readiness_failure_stops_all_started_streams()
{
    OwnerFixture fixture;
    std::string error;
    auto owner = make_owner(&fixture, &error);
    require(owner != nullptr && owner->Start(&error), error);
    for (auto& control : fixture.controls) {
        control->readiness =
            camera_recording::SpatialRoiCameraRecorderReadinessStatus::kReady;
    }
    fixture.controls[1]->readiness =
        camera_recording::SpatialRoiCameraRecorderReadinessStatus::kFailed;
    fixture.controls[1]->readiness_error = "listener peer mismatch";
    require(!owner->PollReadiness(&error) && owner->failed(),
            "readiness failure did not fail the aggregate");
    const auto snapshot = owner->snapshot();
    require(snapshot.first_failure_stream_id ==
                "2010096_spatial_roi_roi_2" &&
                error.find("listener peer mismatch") != std::string::npos,
            "readiness failure lost stream identity or reason");
    for (const auto& stream : snapshot.streams) {
        require(stream.stop_admission_attempted && stream.admission_stopped,
                "readiness failure did not stop all started streams");
    }
}

void eof_failure_is_terminal_and_stops_admission()
{
    OwnerFixture fixture;
    std::string error;
    auto owner = make_owner(&fixture, &error);
    require(owner != nullptr && owner->Start(&error), error);
    for (auto& control : fixture.controls) {
        control->readiness =
            camera_recording::SpatialRoiCameraRecorderReadinessStatus::kReady;
        control->eof = camera_recording::SpatialRoiCameraRecorderEofStatus::kClean;
    }
    require(owner->PollReadiness(&error), error);
    fixture.controls[3]->eof =
        camera_recording::SpatialRoiCameraRecorderEofStatus::kFailed;
    fixture.controls[3]->eof_error = "transport ownership uncertain";
    require(!owner->PollEof(&error) && owner->failed(),
            "fatal EOF did not fail the aggregate");
    const auto snapshot = owner->snapshot();
    require(snapshot.first_failure_stream_id ==
                "2010096_spatial_roi_roi_4" &&
                error.find("ownership uncertain") != std::string::npos,
            "fatal EOF lost exact stream failure");
    for (const auto& stream : snapshot.streams) {
        require(stream.admission_stopped,
                "fatal EOF did not stop admission on every stream");
    }
}

void finalization_failure_preserves_other_stream_results()
{
    OwnerFixture fixture;
    std::string error;
    auto owner = make_owner(&fixture, &error);
    require(owner != nullptr && owner->Start(&error), error);
    for (auto& control : fixture.controls) {
        control->readiness =
            camera_recording::SpatialRoiCameraRecorderReadinessStatus::kReady;
        control->eof = camera_recording::SpatialRoiCameraRecorderEofStatus::kClean;
    }
    require(owner->PollReadiness(&error) && owner->PollEof(&error), error);
    fixture.controls[1]->drain_ok = false;
    fixture.controls[2]->finalize_ok = false;
    require(!owner->DrainAndFinalize(&error) && owner->failed(),
            "per-stream finalization failure completed the aggregate");
    const auto snapshot = owner->snapshot();
    require(snapshot.first_failure_stream_id ==
                "2010096_spatial_roi_roi_2" &&
                error.find("drain failed") != std::string::npos,
            "finalization did not preserve the first failed stream");
    for (const auto& stream : snapshot.streams) {
        require(stream.finalize_attempted,
                "one stream failure skipped another stream finalization");
    }
    require(snapshot.streams[0].drained && snapshot.streams[0].finalized &&
                !snapshot.streams[0].failed &&
                !snapshot.streams[1].drained && snapshot.streams[1].finalized &&
                snapshot.streams[1].failed && snapshot.streams[2].drained &&
                !snapshot.streams[2].finalized && snapshot.streams[2].failed &&
                snapshot.streams[3].drained && snapshot.streams[3].finalized &&
                !snapshot.streams[3].failed,
            "independent stream finalization identities were collapsed");
}

void stop_admission_failure_keeps_aggregate_incomplete()
{
    OwnerFixture fixture;
    std::string error;
    auto owner = make_owner(&fixture, &error);
    require(owner != nullptr && owner->Start(&error), error);
    for (auto& control : fixture.controls) {
        control->readiness =
            camera_recording::SpatialRoiCameraRecorderReadinessStatus::kReady;
        control->eof = camera_recording::SpatialRoiCameraRecorderEofStatus::kClean;
    }
    require(owner->PollReadiness(&error) && owner->PollEof(&error), error);

    fixture.controls[1]->stop_ok = false;
    require(!owner->DrainAndFinalize(&error) && owner->failed(),
            "stop-admission failure completed the aggregate");
    const auto snapshot = owner->snapshot();
    require(snapshot.first_failure_stream_id ==
                "2010096_spatial_roi_roi_2" &&
                error.find("stop-admission failed") != std::string::npos,
            "stop-admission failure lost its stream identity or reason");
    require(snapshot.streams[0].admission_stopped &&
                !snapshot.streams[1].admission_stopped &&
                snapshot.streams[1].failed &&
                snapshot.streams[2].admission_stopped &&
                snapshot.streams[3].admission_stopped,
            "stop-admission failure did not preserve per-stream results");
    for (const auto& control : fixture.controls) {
        require(control->stop_calls == 1,
                "stop admission was not attempted exactly once");
    }
}

void thrown_readiness_operation_fails_closed()
{
    OwnerFixture fixture;
    std::string error;
    auto owner = make_owner(&fixture, &error);
    require(owner != nullptr && owner->Start(&error), error);
    for (auto& control : fixture.controls) {
        control->readiness =
            camera_recording::SpatialRoiCameraRecorderReadinessStatus::kReady;
    }
    fixture.controls[2]->throw_on_readiness = true;

    require(!owner->PollReadiness(&error) && owner->failed(),
            "thrown readiness operation did not fail the aggregate");
    const auto snapshot = owner->snapshot();
    require(snapshot.first_failure_stream_id ==
                "2010096_spatial_roi_roi_3" &&
                error.find("readiness poll threw") != std::string::npos,
            "thrown readiness operation lost its stream identity or reason");
    for (const auto& stream : snapshot.streams) {
        require(stream.stop_admission_attempted && stream.admission_stopped,
                "thrown readiness operation did not stop every started stream");
    }
}

void destructor_stops_attempted_cores_after_partial_start()
{
    OwnerFixture fixture;
    std::string error;
    auto owner = make_owner(&fixture, &error);
    require(owner != nullptr, error);
    fixture.controls[2]->start_ok = false;
    require(!owner->Start(&error) && owner->failed(),
            "partial start did not fail before destruction");
    owner.reset();

    require(fixture.controls[0]->stop_calls == 1 &&
                fixture.controls[1]->stop_calls == 1 &&
                fixture.controls[2]->stop_calls == 1 &&
                fixture.controls[3]->stop_calls == 0,
            "destructor duplicated or omitted partial-start cleanup");
}

void repeated_readiness_and_eof_polls_are_idempotent()
{
    OwnerFixture fixture;
    std::string error;
    auto owner = make_owner(&fixture, &error);
    require(owner != nullptr && owner->Start(&error), error);
    for (auto& control : fixture.controls) {
        control->readiness =
            camera_recording::SpatialRoiCameraRecorderReadinessStatus::kReady;
        control->eof = camera_recording::SpatialRoiCameraRecorderEofStatus::kClean;
    }

    require(owner->PollReadiness(&error), error);
    fixture.calls.clear();
    require(owner->PollReadiness(&error), error);
    require(fixture.calls.empty(),
            "repeated readiness poll called already-ready cores");

    require(owner->PollEof(&error), error);
    fixture.calls.clear();
    require(owner->PollEof(&error), error);
    require(fixture.calls.empty(), "repeated EOF poll called clean cores");
}

void out_of_order_lifecycle_call_fails_closed()
{
    OwnerFixture fixture;
    std::string error;
    auto owner = make_owner(&fixture, &error);
    require(owner != nullptr, error);
    require(!owner->PollEof(&error) && owner->failed() &&
                error.find("PollEof is invalid from state constructed") !=
                    std::string::npos,
            "out-of-order EOF poll did not fail the one-shot lifecycle");
}

void readiness_requires_successful_storage_preflight()
{
    OwnerFixture fixture;
    std::string error;
    auto owner = make_owner(&fixture, &error);
    require(owner != nullptr && owner->Start(&error), error);
    owner->set_storage_preflight_result(
        camera_recording::SpatialRoiRecorderStoragePreflightResult{});
    for (auto& control : fixture.controls) {
        control->readiness =
            camera_recording::SpatialRoiCameraRecorderReadinessStatus::kReady;
    }
    require(!owner->PollReadiness(&error) && owner->failed() &&
                error.find("storage preflight") != std::string::npos,
            "camera owner reported readiness without a successful storage preflight");
}

}  // namespace

int main()
{
    try {
        accepts_plan_order_and_completes_four_stream_lifecycle();
        rejects_mutated_contract_before_factory();
        rejects_partial_factory_construction();
        partial_start_failure_stops_every_attempted_core();
        readiness_failure_stops_all_started_streams();
        eof_failure_is_terminal_and_stops_admission();
        finalization_failure_preserves_other_stream_results();
        stop_admission_failure_keeps_aggregate_incomplete();
        thrown_readiness_operation_fails_closed();
        destructor_stops_attempted_cores_after_partial_start();
        repeated_readiness_and_eof_polls_are_idempotent();
        out_of_order_lifecycle_call_fails_closed();
        readiness_requires_successful_storage_preflight();
        std::cout << "spatial ROI camera recorder tests passed\n";
        return 0;
    } catch (const std::exception& ex) {
        std::cerr << "spatial ROI camera recorder tests failed: " << ex.what()
                  << '\n';
        return 1;
    }
}
