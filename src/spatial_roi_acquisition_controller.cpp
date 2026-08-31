#include "spatial_roi_acquisition_controller.h"

#include "worker_entry_release.h"

#include <atomic>
#include <exception>
#include <memory>
#include <string>
#include <utility>

namespace orange::spatial_roi {

namespace {

void set_error(std::string* error_out, std::string message)
{
    if (error_out) {
        *error_out = std::move(message);
    }
}

void set_result_error(
    SpatialRoiAcquisitionControllerResult* result,
    const char* message) noexcept
{
    result->status = SpatialRoiAcquisitionControllerSubmitStatus::kException;
    result->bridge_result.bridge_status =
        SpatialRoiAcquisitionBridgeStatus::kRuntimeException;
    try {
        result->bridge_result.error = message ? message : "unknown exception";
    } catch (...) {
        // TrySubmit is a noexcept boundary.  The enum remains useful even if
        // an exceptional allocation failure prevents an explanatory string.
    }
}

}  // namespace

const char* spatial_roi_acquisition_arm_status_name(
    SpatialRoiAcquisitionArmStatus status) noexcept
{
    switch (status) {
    case SpatialRoiAcquisitionArmStatus::kArmed:
        return "armed";
    case SpatialRoiAcquisitionArmStatus::kAlreadyArmed:
        return "already_armed";
    case SpatialRoiAcquisitionArmStatus::kMissingRuntime:
        return "missing_runtime";
    case SpatialRoiAcquisitionArmStatus::kRuntimeStopped:
        return "runtime_stopped";
    case SpatialRoiAcquisitionArmStatus::kInvalidSession:
        return "invalid_session";
    case SpatialRoiAcquisitionArmStatus::kIdentityMismatch:
        return "identity_mismatch";
    case SpatialRoiAcquisitionArmStatus::kGenerationReused:
        return "generation_reused";
    }
    return "unknown";
}

const char* spatial_roi_acquisition_controller_submit_status_name(
    SpatialRoiAcquisitionControllerSubmitStatus status) noexcept
{
    switch (status) {
    case SpatialRoiAcquisitionControllerSubmitStatus::kSubmitted:
        return "submitted";
    case SpatialRoiAcquisitionControllerSubmitStatus::kRuntimeIncomplete:
        return "runtime_incomplete";
    case SpatialRoiAcquisitionControllerSubmitStatus::kDisarmed:
        return "disarmed";
    case SpatialRoiAcquisitionControllerSubmitStatus::kBridgeRejected:
        return "bridge_rejected";
    case SpatialRoiAcquisitionControllerSubmitStatus::kException:
        return "exception";
    }
    return "unknown";
}

SpatialRoiAcquisitionController::SpatialRoiAcquisitionController() = default;

SpatialRoiAcquisitionController::~SpatialRoiAcquisitionController()
{
    // Stop the active session first, then explicitly drain every retained
    // predecessor.  Destruction is the final teardown boundary and must do
    // this even if the controller is accidentally destroyed from a different
    // (but non-lane) thread.
    (void)Disarm();
    drain_retired_runtimes();

    // A well-formed controller has no active session after Disarm.  Keep this
    // fallback explicit in case a future edit changes that invariant: a raw
    // active runtime must never remain accepting while its owner disappears.
    const std::shared_ptr<ActiveSession> active =
        std::atomic_load_explicit(&active_session_, std::memory_order_acquire);
    if (active && active->session.runtime) {
        active->session.runtime->StopAcceptingAndDrain();
    }
}

SpatialRoiAcquisitionArmStatus
SpatialRoiAcquisitionController::validate_session(
    const SpatialRoiAcquisitionSession& session,
    std::string* error_out)
{
    if (!session.runtime) {
        set_error(error_out, "spatial ROI acquisition runtime is null");
        return SpatialRoiAcquisitionArmStatus::kMissingRuntime;
    }
    if (session.recording_id.empty() ||
        session.recording_identity_token.empty() ||
        session.producer_generation.empty() ||
        session.camera_id < 0 ||
        session.camera_serial.empty()) {
        set_error(
            error_out,
            "recording/session identity is incomplete for spatial ROI arming");
        return SpatialRoiAcquisitionArmStatus::kInvalidSession;
    }
    return SpatialRoiAcquisitionArmStatus::kArmed;
}

SpatialRoiAcquisitionArmStatus
SpatialRoiAcquisitionController::validate_runtime_binding(
    const SpatialRoiAcquisitionSession& session,
    std::string* error_out)
{
    const SpatialRoiBatchLimits& limits =
        session.runtime->producer_limits();
    if (session.recording_id != limits.expected_recording_id ||
        session.recording_identity_token !=
            limits.expected_recording_identity_token ||
        session.producer_generation != limits.expected_producer_generation ||
        session.camera_id != limits.expected_camera_id ||
        session.camera_serial != limits.expected_camera_serial) {
        set_error(
            error_out,
            "session identity does not exactly match the verified spatial ROI runtime binding");
        return SpatialRoiAcquisitionArmStatus::kIdentityMismatch;
    }
    return SpatialRoiAcquisitionArmStatus::kArmed;
}

SpatialRoiAcquisitionArmResult SpatialRoiAcquisitionController::Arm(
    SpatialRoiAcquisitionSession session)
{
    SpatialRoiAcquisitionArmResult result;
    result.status = validate_session(session, &result.error);
    if (result.status != SpatialRoiAcquisitionArmStatus::kArmed) {
        return result;
    }
    result.status = validate_runtime_binding(session, &result.error);
    if (result.status != SpatialRoiAcquisitionArmStatus::kArmed) {
        return result;
    }
    std::lock_guard<std::mutex> lock(lifecycle_mutex_);
    if (std::atomic_load_explicit(
            &active_session_,
            std::memory_order_acquire)) {
        result.status = SpatialRoiAcquisitionArmStatus::kAlreadyArmed;
        result.error = "spatial ROI acquisition controller is already armed";
        return result;
    }
    if (armed_generations_.count(session.producer_generation) != 0) {
        result.status = SpatialRoiAcquisitionArmStatus::kGenerationReused;
        result.error =
            "spatial ROI producer_generation was already armed by this controller";
        return result;
    }

    // Check acceptance while holding the same transition mutex as Disarm.
    // Otherwise Disarm could stop this runtime between the check and the
    // atomic publication of a new active session.
    if (!session.runtime->accepting()) {
        result.status = SpatialRoiAcquisitionArmStatus::kRuntimeStopped;
        result.error = "spatial ROI runtime is not accepting submissions";
        return result;
    }

    // Construct before publishing.  If allocation throws, the controller
    // remains disarmed and any retired runtime remains owned by the linked
    // list head.
    auto active = std::make_shared<ActiveSession>(std::move(session));
    const auto inserted =
        armed_generations_.insert(active->session.producer_generation);
    if (!inserted.second) {
        result.status = SpatialRoiAcquisitionArmStatus::kGenerationReused;
        result.error =
            "spatial ROI producer_generation was already armed by this controller";
        return result;
    }
    std::atomic_store_explicit(
        &active_session_, std::move(active), std::memory_order_release);
    result.status = SpatialRoiAcquisitionArmStatus::kArmed;
    result.error.clear();
    return result;
}

bool SpatialRoiAcquisitionController::Disarm() noexcept
{
    std::lock_guard<std::mutex> lock(lifecycle_mutex_);
    if (!std::atomic_load_explicit(
            &active_session_, std::memory_order_acquire)) {
        return false;
    }

    // Remove first while holding the transition mutex.  The node already owns
    // its runtime and its retired_next link, so the remainder of Disarm is
    // allocation-free.  StopAccepting is deliberately before linking: a
    // detached predecessor is never visible as an active accepting runtime.
    std::shared_ptr<ActiveSession> removed =
        std::atomic_exchange_explicit(
            &active_session_,
            std::shared_ptr<ActiveSession>{},
            std::memory_order_acq_rel);
    if (removed && removed->session.runtime) {
        removed->session.runtime->StopAccepting();
    }

    // Link only after the exchange and stop.  No caller can observe a live
    // active session that is absent from either active_session_ or the
    // retired list, and no allocation can fail between those two states.
    removed->retired_next = std::move(retired_head_);
    retired_head_ = std::move(removed);
    return true;
}

bool SpatialRoiAcquisitionController::Drain() noexcept
{
    drain_retired_runtimes();
    return true;
}

void SpatialRoiAcquisitionController::drain_retired_runtimes() noexcept
{
    // Drain in batches so a concurrent Disarm cannot make a runtime vanish
    // between detaching the retired list and joining the previous batch.  A
    // runtime is shared-owned by the local linked-list head for the entire
    // drain.  No allocation is needed to detach or traverse the list.
    while (true) {
        std::shared_ptr<ActiveSession> retired;
        {
            std::lock_guard<std::mutex> lock(lifecycle_mutex_);
            if (!retired_head_) {
                return;
            }
            retired = std::move(retired_head_);
        }
        while (retired) {
            std::shared_ptr<ActiveSession> next =
                std::move(retired->retired_next);
            if (retired->session.runtime) {
                retired->session.runtime->StopAcceptingAndDrain();
            }
            retired = std::move(next);
        }
    }
}

bool SpatialRoiAcquisitionController::armed() const noexcept
{
    return static_cast<bool>(std::atomic_load_explicit(
        &active_session_, std::memory_order_acquire));
}

SpatialRoiAcquisitionControllerResult
SpatialRoiAcquisitionController::TrySubmit(
    WORKER_ENTRY* entry,
    SafeQueue<WORKER_ENTRY*>* recycle_queue,
    WorkerEntryReleaseContext context) noexcept
{
    SpatialRoiAcquisitionControllerResult result;
    try {
        // Keep this one snapshot alive through all identity construction and
        // through submit_spatial_roi_acquisition.  Disarm may stop the raw
        // runtime pointer, but cannot destroy it until this shared owner is
        // released.
        const std::shared_ptr<ActiveSession> active =
            std::atomic_load_explicit(
                &active_session_, std::memory_order_acquire);
        if (!active || !active->session.runtime) {
            result.status =
                SpatialRoiAcquisitionControllerSubmitStatus::kDisarmed;
            result.bridge_result.bridge_status =
                SpatialRoiAcquisitionBridgeStatus::kMissingRuntime;
            result.bridge_result.error =
                "spatial ROI acquisition controller is disarmed";
            return result;
        }

        const SpatialRoiAcquisitionSession& session = active->session;
        SpatialRoiAcquisitionIdentity identity;
        // This is deliberately not inferred from the session or from any
        // other flag.  A zero recording frame is the acquisition loop's only
        // recording-inactive marker at this boundary.
        identity.recording_active =
            entry != nullptr && entry->recording_frame_id != 0;
        identity.recording_id = session.recording_id;
        identity.recording_identity_token = session.recording_identity_token;
        identity.producer_generation = session.producer_generation;
        identity.camera_id = session.camera_id;
        identity.camera_serial = session.camera_serial;
        if (entry) {
            identity.local_frame_id = entry->frame_id;
            identity.camera_frame_id = entry->camera_frame_id;
            identity.recording_frame_id = entry->recording_frame_id;
            identity.camera_timestamp_ns = entry->timestamp;
            identity.timestamp_sys_ns = entry->timestamp_sys;
        }

        result.bridge_result = submit_spatial_roi_acquisition(
            entry,
            recycle_queue,
            context,
            session.runtime.get(),
            identity);
        if (result.bridge_result.bridge_status ==
            SpatialRoiAcquisitionBridgeStatus::kSubmitted) {
            result.status =
                SpatialRoiAcquisitionControllerSubmitStatus::kSubmitted;
        } else if (result.bridge_result.bridge_status ==
                   SpatialRoiAcquisitionBridgeStatus::kRuntimeIncomplete) {
            result.status = SpatialRoiAcquisitionControllerSubmitStatus::
                kRuntimeIncomplete;
        } else {
            result.status =
                SpatialRoiAcquisitionControllerSubmitStatus::kBridgeRejected;
        }
        return result;
    } catch (const std::exception& exception) {
        set_result_error(&result, exception.what());
        return result;
    } catch (...) {
        set_result_error(&result, "spatial ROI controller submission threw");
        return result;
    }
}

}  // namespace orange::spatial_roi
