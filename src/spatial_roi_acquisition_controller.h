#pragma once

#include "spatial_roi_acquisition_bridge.h"

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_set>

namespace orange::spatial_roi {

// The controller is the session-time ownership boundary for one camera.  A
// runtime is immutable with respect to its verified plan, but the recording
// identity is deliberately supplied again when it is armed so that an
// accidentally reused runtime fails closed.
struct SpatialRoiAcquisitionSession {
    std::shared_ptr<SpatialRoiRecordingRuntime> runtime;
    std::string recording_id;
    std::string recording_identity_token;
    std::string producer_generation;
    int camera_id = -1;
    std::string camera_serial;
};

enum class SpatialRoiAcquisitionArmStatus {
    kArmed,
    kAlreadyArmed,
    kMissingRuntime,
    kRuntimeStopped,
    kInvalidSession,
    kIdentityMismatch,
    kGenerationReused,
};

const char* spatial_roi_acquisition_arm_status_name(
    SpatialRoiAcquisitionArmStatus status) noexcept;

struct SpatialRoiAcquisitionArmResult {
    SpatialRoiAcquisitionArmStatus status =
        SpatialRoiAcquisitionArmStatus::kMissingRuntime;
    std::string error;

    bool armed() const noexcept
    {
        return status == SpatialRoiAcquisitionArmStatus::kArmed;
    }
};

enum class SpatialRoiAcquisitionControllerSubmitStatus {
    kSubmitted,
    kRuntimeIncomplete,
    kDisarmed,
    kBridgeRejected,
    kException,
};

const char* spatial_roi_acquisition_controller_submit_status_name(
    SpatialRoiAcquisitionControllerSubmitStatus status) noexcept;

struct SpatialRoiAcquisitionControllerResult {
    SpatialRoiAcquisitionControllerSubmitStatus status =
        SpatialRoiAcquisitionControllerSubmitStatus::kDisarmed;
    // The bridge result is retained so callers can inspect the exact
    // fail-closed validation/runtime status without relying on a lossy
    // controller-level enum.
    SpatialRoiAcquisitionBridgeResult bridge_result;

    bool submitted() const noexcept
    {
        return status == SpatialRoiAcquisitionControllerSubmitStatus::kSubmitted;
    }
};

// TrySubmit may be called by an acquisition thread; it takes an atomic shared
// snapshot and never accesses a session after that snapshot is released.
// Drain/destruction must be invoked from a non-acquisition, non-lane/sink
// owner or teardown thread so runtime lane joins cannot self-join.
class SpatialRoiAcquisitionController final {
public:
    SpatialRoiAcquisitionController();
    ~SpatialRoiAcquisitionController();

    SpatialRoiAcquisitionController(const SpatialRoiAcquisitionController&) =
        delete;
    SpatialRoiAcquisitionController& operator=(
        const SpatialRoiAcquisitionController&) = delete;
    SpatialRoiAcquisitionController(SpatialRoiAcquisitionController&&) = delete;
    SpatialRoiAcquisitionController& operator=(
        SpatialRoiAcquisitionController&&) = delete;

    // Arm validates all plan-bound identity fields against the runtime's
    // verified producer limits.  Replacement is rejected while an active
    // session exists.  A previous disarmed runtime remains retained until
    // Drain(), so Arm cannot silently destroy an undrained lane pipeline.
    SpatialRoiAcquisitionArmResult Arm(
        SpatialRoiAcquisitionSession session);

    // Atomically removes the active session and stops accepting new runtime
    // submissions.  It intentionally does not join lane threads.  Retired
    // ownership is an already-allocated shared_ptr link, so this operation is
    // allocation-free and noexcept.
    bool Disarm() noexcept;

    // Drain all retired runtimes.  This is safe to call repeatedly and from
    // any non-acquisition, non-lane/sink owner or teardown thread.  The
    // runtime's own drain remains responsible for refusing a self-join when a
    // sink callback requests a re-entrant stop.
    bool Drain() noexcept;

    bool armed() const noexcept;

    // Derives recording_active only from entry != nullptr and a nonzero
    // entry->recording_frame_id.  One atomic session snapshot is held through
    // the bridge call, keeping the runtime alive if Disarm races this method.
    SpatialRoiAcquisitionControllerResult TrySubmit(
        WORKER_ENTRY* entry,
        SafeQueue<WORKER_ENTRY*>* recycle_queue,
        WorkerEntryReleaseContext context) noexcept;

private:
    struct ActiveSession {
        explicit ActiveSession(SpatialRoiAcquisitionSession value)
            : session(std::move(value))
        {
        }

        SpatialRoiAcquisitionSession session;
        // Once an active node is atomically removed, this pre-existing
        // shared_ptr member links it into retired_head_ without allocation.
        std::shared_ptr<ActiveSession> retired_next;
    };

    static SpatialRoiAcquisitionArmStatus validate_session(
        const SpatialRoiAcquisitionSession& session,
        std::string* error_out);
    static SpatialRoiAcquisitionArmStatus validate_runtime_binding(
        const SpatialRoiAcquisitionSession& session,
        std::string* error_out);

    // Drains every runtime currently in the retired linked list.  Both the
    // public Drain() and destruction use this explicit stop/drain path; the
    // caller is responsible for invoking it away from acquisition and lane
    // callback threads.
    void drain_retired_runtimes() noexcept;

    // std::shared_ptr's C++17 atomic free functions are used for this field;
    // do not replace this with an atomic<shared_ptr>, which is unavailable on
    // the supported C++17 toolchains.
    std::shared_ptr<ActiveSession> active_session_;

    // Lifecycle transitions are serialized separately from the lock-free
    // acquisition snapshot.  ActiveSession's next link is assigned only
    // after atomic removal and StopAccepting, so Disarm never allocates and
    // cannot lose an undrained predecessor when a later Arm occurs.
    mutable std::mutex lifecycle_mutex_;
    std::shared_ptr<ActiveSession> retired_head_;
    // A dense ROI stream restarts at one for each runtime. Reusing a producer
    // generation in this controller would therefore collide with an earlier
    // epoch even when the previous runtime was drained.
    std::unordered_set<std::string> armed_generations_;
};

}  // namespace orange::spatial_roi
