#include "calibration_transaction.h"
#include "gui/spatial_layout/calibration_transaction_bridge.h"
#include "gui/spatial_layout/state.h"

#include <iostream>
#include <stdexcept>
#include <string>

namespace {

void require(const bool condition, const std::string& message)
{
    if (!condition) {
        throw std::runtime_error(message);
    }
}

orange::calibration::TransactionRequest request(
    const std::string& owner,
    const orange::calibration::WorkflowKind workflow,
    std::vector<std::string> cameras,
    const orange::calibration::MutationSet mutations)
{
    orange::calibration::TransactionRequest value;
    value.owner_id = owner;
    value.workflow = workflow;
    value.camera_serials = std::move(cameras);
    value.allowed_owner_mutations = mutations;
    value.reason = "focused transaction test";
    return value;
}

void test_exclusive_ownership_and_diagnostics()
{
    using namespace orange::calibration;
    TransactionCoordinator coordinator;
    AcquireResult first = coordinator.TryAcquire(request(
        "dailyreg_1",
        WorkflowKind::kDailyRegistration,
        {"2010096", "2010095", "2010095", ""},
        Mutation::kCameraParameters | Mutation::kCitrusScene));
    require(first.ok(), first.error);
    const TransactionSnapshot active = coordinator.snapshot();
    require(active.active, "coordinator must report an active transaction");
    require(active.camera_serials ==
                std::vector<std::string>({"2010095", "2010096"}),
            "camera scope must be normalized");

    AcquireResult conflict = coordinator.TryAcquire(request(
        "usaf_1",
        WorkflowKind::kUsafResolution,
        {"2010094"},
        mutation_set(Mutation::kCameraParameters)));
    require(!conflict.ok(), "a second process transaction must be rejected");
    require(conflict.blocker.owner_id == "dailyreg_1",
            "rejection must identify the owner");
    require(conflict.error.find("dailyreg_1") != std::string::npos,
            "rejection text must identify the owner");
    require(coordinator.rejection_message("Recording start").find(
                "Recording start is blocked") != std::string::npos,
            "action rejection must be operator-readable");
}

void test_permissions_scope_and_explicit_release()
{
    using namespace orange::calibration;
    TransactionCoordinator coordinator;
    AcquireResult acquired = coordinator.TryAcquire(request(
        "guided_1",
        WorkflowKind::kGuidedCommissioning,
        {"2010093", "2010094"},
        Mutation::kCameraParameters |
            Mutation::kCameraStreamLifecycle |
            Mutation::kCitrusScene));
    require(acquired.ok(), acquired.error);
    require(acquired.lease->permits(Mutation::kCameraParameters),
            "declared camera mutation must be allowed to the owner");
    require(!acquired.lease->permits(Mutation::kRecordingStart),
            "recording start must not be implicitly allowed");
    require(acquired.lease->covers_cameras({"2010094"}),
            "a child camera subset must be covered");
    require(!acquired.lease->covers_cameras({"2010095"}),
            "an out-of-scope child camera must be rejected");

    acquired.lease->Release("complete", "restoration verified");
    require(!coordinator.active(), "explicit release must clear ownership");
    const TransactionSnapshot released = coordinator.snapshot();
    require(released.last_terminal_status == "complete",
            "terminal status must survive release");
    require(released.last_terminal_reason == "restoration verified",
            "terminal reason must survive release");

    AcquireResult next = coordinator.TryAcquire(request(
        "manual_group_2",
        WorkflowKind::kSpatialGroupedCapture,
        {"2010095"},
        mutation_set(Mutation::kCitrusScene)));
    require(next.ok(), "a new transaction must start after explicit release");
}

void test_scope_destruction_releases()
{
    using namespace orange::calibration;
    TransactionCoordinator coordinator;
    {
        AcquireResult acquired = coordinator.TryAcquire(request(
            "aperture_1",
            WorkflowKind::kApertureCharacterization,
            {"2010093"},
            mutation_set(Mutation::kCameraParameters)));
        require(acquired.ok(), acquired.error);
        require(coordinator.active(), "lease must remain active in owner scope");
    }
    require(!coordinator.active(), "lease destruction must fail-safe release");
    require(coordinator.snapshot().last_terminal_status == "scope_destroyed",
            "RAII release must remain diagnosable");
}

void test_invalid_requests_do_not_claim_coordinator()
{
    using namespace orange::calibration;
    TransactionCoordinator coordinator;
    TransactionRequest missing_owner;
    missing_owner.reason = "missing owner";
    require(!coordinator.TryAcquire(std::move(missing_owner)).ok(),
            "ownerless request must fail");

    TransactionRequest missing_reason;
    missing_reason.owner_id = "owner";
    require(!coordinator.TryAcquire(std::move(missing_reason)).ok(),
            "reasonless request must fail");
    require(!coordinator.active(), "invalid requests must not claim ownership");
}

void test_recording_start_reservation_excludes_calibration()
{
    using namespace orange::calibration;
    require(std::string(workflow_kind_name(
                WorkflowKind::kRecordingStartReservation)) ==
                "recording_start_reservation",
            "recording-start reservations must have a stable diagnostic name");
    TransactionCoordinator coordinator;
    AcquireResult recording = coordinator.TryAcquire(request(
        "recording_start_1",
        WorkflowKind::kRecordingStartReservation,
        {"2010095"},
        mutation_set(Mutation::kRecordingStart)));
    require(recording.ok(), recording.error);
    require(recording.lease->permits(Mutation::kRecordingStart),
            "the recording owner must hold its declared start permission");
    AcquireResult calibration = coordinator.TryAcquire(request(
        "dailyreg_during_recording_start",
        WorkflowKind::kDailyRegistration,
        {"2010095"},
        Mutation::kCameraParameters | Mutation::kCitrusScene));
    require(!calibration.ok(),
            "calibration must not begin while recording activation is pending");
    require(calibration.blocker.workflow ==
                WorkflowKind::kRecordingStartReservation,
            "the rejection must identify the recording-start reservation");
}

void test_spatial_parent_bridge_enforces_owner_scope_and_permission()
{
    using namespace orange::calibration;
    using namespace orange::gui::spatial_layout;
    SpatialLayoutUiState state;
    std::string error;
    require(acquire_spatial_calibration_transaction(
                &state,
                "daily_registration",
                "dailyreg_bridge_1",
                WorkflowKind::kDailyRegistration,
                {"2010095", "2010096"},
                Mutation::kCameraParameters | Mutation::kCitrusScene,
                "bridge contract test",
                &error),
            error);
    require(spatial_calibration_transaction_owned_by(
                state, "daily_registration"),
            "bridge must preserve the declared parent owner kind");
    require(!spatial_calibration_transaction_owned_by(
                state, "guided_commissioning"),
            "a child must not impersonate another parent workflow");
    require(require_spatial_calibration_transaction(
                state,
                {"2010096"},
                Mutation::kCitrusScene,
                &error),
            error);
    require(!require_spatial_calibration_transaction(
                state,
                {"2010094"},
                Mutation::kCitrusScene,
                &error),
            "bridge must reject child cameras outside the parent scope");
    require(!require_spatial_calibration_transaction(
                state,
                {"2010095"},
                Mutation::kCameraStreamLifecycle,
                &error),
            "bridge must reject child mutations outside the parent permission set");
    release_spatial_calibration_transaction(
        &state, "complete", "bridge contract test complete");
    require(!global_transaction_coordinator().active(),
            "bridge release must clear process-wide ownership");
}

}  // namespace

int main()
{
    try {
        test_exclusive_ownership_and_diagnostics();
        test_permissions_scope_and_explicit_release();
        test_scope_destruction_releases();
        test_invalid_requests_do_not_claim_coordinator();
        test_recording_start_reservation_excludes_calibration();
        test_spatial_parent_bridge_enforces_owner_scope_and_permission();
        std::cout << "calibration_transaction_tests: PASS\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "calibration_transaction_tests: FAIL: "
                  << error.what() << '\n';
        return 1;
    }
}
