#pragma once

#include "session/spatial_roi_recorder_camera_contract.h"
#include "spatial_roi_recorder_storage_preflight.h"

#include <cstddef>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace orange::spatial_roi::recording {

// This is the deterministic, single-owner orchestration seam for the first
// camera-level fixed-region recorder.  It deliberately has no full-frame
// recorder dependency: full-frame recording remains an independent,
// first-class product owned by its existing process and protocol.
enum class SpatialRoiCameraRecorderState {
    kConstructed,
    kStarting,
    kReady,
    kAwaitingEof,
    kEofObserved,
    kFinalizing,
    kCompleted,
    kFailed,
};

const char* spatial_roi_camera_recorder_state_name(
    SpatialRoiCameraRecorderState state) noexcept;

enum class SpatialRoiCameraRecorderReadinessStatus {
    kPending,
    kReady,
    kFailed,
};

enum class SpatialRoiCameraRecorderEofStatus {
    kPending,
    kClean,
    kFailed,
};

// One injected implementation owns the concrete listener, IPC session,
// detach pool, encoder, journal, and evidence writer for exactly one stream.
// The camera owner never interprets connection order or CUDA data.  Methods
// are called synchronously by one lifecycle thread and must be idempotent with
// respect to StopAdmission after a partial Start failure.
class SpatialRoiCameraRecorderStreamCore {
public:
    virtual ~SpatialRoiCameraRecorderStreamCore() = default;

    SpatialRoiCameraRecorderStreamCore(
        const SpatialRoiCameraRecorderStreamCore&) = delete;
    SpatialRoiCameraRecorderStreamCore& operator=(
        const SpatialRoiCameraRecorderStreamCore&) = delete;

    virtual bool Start(std::string* error_out) = 0;
    virtual SpatialRoiCameraRecorderReadinessStatus PollReadiness(
        std::string* error_out) = 0;
    virtual SpatialRoiCameraRecorderEofStatus PollEof(
        std::string* error_out) = 0;
    virtual bool StopAdmission(std::string* error_out) = 0;
    virtual bool Drain(std::string* error_out) = 0;
    virtual bool Finalize(std::string* error_out) = 0;

protected:
    SpatialRoiCameraRecorderStreamCore() = default;
};

// The factory is invoked exactly four times and only after complete contract
// authentication. Calls are in verified plan order. Construction must not arm
// intake; Start is the explicit resource-arming boundary.
using SpatialRoiCameraRecorderStreamCoreFactory = std::function<
    std::unique_ptr<SpatialRoiCameraRecorderStreamCore>(
        const session::spatial_roi::SpatialRoiRecorderStreamView& stream,
        std::size_t plan_index,
        std::string* error_out)>;

struct SpatialRoiCameraRecorderStreamSnapshot {
    std::size_t plan_index = 0;
    std::string logical_stream_id;
    std::string roi_id;
    std::string region_id;
    std::string camera_serial;
    std::string recording_id;
    std::string session_id;

    bool start_attempted = false;
    bool started = false;
    bool ready = false;
    bool clean_eof = false;
    bool stop_admission_attempted = false;
    bool admission_stopped = false;
    bool drain_attempted = false;
    bool drained = false;
    bool finalize_attempted = false;
    bool finalized = false;
    bool failed = false;
    std::string first_failure;
};

struct SpatialRoiCameraRecorderSnapshot {
    SpatialRoiCameraRecorderState state =
        SpatialRoiCameraRecorderState::kConstructed;
    std::string product_kind;
    std::string recording_id;
    std::string session_id;
    std::string recording_identity_token;
    std::string producer_generation;
    std::string spatial_roi_plan_sha256;
    int camera_id = -1;
    std::string camera_serial;
    bool ready = false;
    bool clean_eof = false;
    bool completed = false;
    std::string first_failure_stream_id;
    std::string first_failure;
    SpatialRoiRecorderStoragePreflightResult storage_preflight;
    std::vector<SpatialRoiCameraRecorderStreamSnapshot> streams;
};

// Canonical lifecycle JSON used by the production executable and by the
// parent-supervisor integration tests. Keeping this serializer in the shared
// recorder core prevents the live ready boundary from drifting away from
// hand-authored process fixtures.
nlohmann::json spatial_roi_camera_recorder_snapshot_to_json(
    const SpatialRoiCameraRecorderSnapshot& snapshot,
    const std::string& event,
    const std::string& terminal_reason = {});

// Authenticated, bounded orchestration for exactly one camera and four
// required fixed-region products.  The class is intentionally synchronous
// and not thread-safe; a future executable may place it behind one lifecycle
// thread while individual cores own their internal workers.
class SpatialRoiCameraRecorder final {
public:
    // Authentication occurs before the factory can observe any stream.  The
    // returned object owns a closed copy of the authenticated camera view and
    // four plan-ordered cores.  nullptr means no runnable owner was created.
    static std::unique_ptr<SpatialRoiCameraRecorder> Create(
        const nlohmann::json& candidate_contract,
        const nlohmann::json& independently_verified_plan,
        const std::string& expected_recording_root,
        const session::spatial_roi::SpatialRoiRecorderRuntimeGpuMapping&
            expected_gpu_mapping,
        SpatialRoiCameraRecorderStreamCoreFactory factory,
        std::string* error_out = nullptr);

    // Destruction is not successful finalization, but it does make a
    // best-effort stop-admission pass before destroying any active core.
    ~SpatialRoiCameraRecorder();

    SpatialRoiCameraRecorder(const SpatialRoiCameraRecorder&) = delete;
    SpatialRoiCameraRecorder& operator=(const SpatialRoiCameraRecorder&) =
        delete;
    SpatialRoiCameraRecorder(SpatialRoiCameraRecorder&&) = delete;
    SpatialRoiCameraRecorder& operator=(SpatialRoiCameraRecorder&&) = delete;

    bool Start(std::string* error_out = nullptr);

    // The executable performs the live descriptor-bound preflight before
    // arming this owner. Keeping the typed observation here makes every
    // lifecycle snapshot (ready, heartbeat, and terminal) self-describing.
    void set_storage_preflight_result(
        SpatialRoiRecorderStoragePreflightResult result)
    {
        storage_preflight_ = std::move(result);
    }

    // Pending is a successful poll.  The aggregate enters kReady only when
    // all four required cores report ready.  A single failure stops admission
    // on every started core and latches kFailed.
    bool PollReadiness(std::string* error_out = nullptr);

    // May begin only after aggregate readiness.  Pending remains nonterminal;
    // all four clean EOF reports are required for kEofObserved.
    bool PollEof(std::string* error_out = nullptr);

    // Valid only after all four clean EOF reports.  Stop-admission, drain, and
    // finalize are each attempted in plan order for every started core.  A
    // failure in one core does not relabel or skip the other cores, but makes
    // aggregate completion fail closed.
    bool DrainAndFinalize(std::string* error_out = nullptr);

    SpatialRoiCameraRecorderState state() const noexcept { return state_; }
    bool ready() const noexcept
    {
        return state_ == SpatialRoiCameraRecorderState::kReady ||
               state_ == SpatialRoiCameraRecorderState::kAwaitingEof ||
               state_ == SpatialRoiCameraRecorderState::kEofObserved ||
               state_ == SpatialRoiCameraRecorderState::kFinalizing ||
               state_ == SpatialRoiCameraRecorderState::kCompleted;
    }
    bool completed() const noexcept
    {
        return state_ == SpatialRoiCameraRecorderState::kCompleted;
    }
    bool failed() const noexcept
    {
        return state_ == SpatialRoiCameraRecorderState::kFailed;
    }
    const std::string& first_failure() const noexcept
    {
        return first_failure_;
    }
    const session::spatial_roi::SpatialRoiRecorderCameraContractView&
    contract() const noexcept
    {
        return contract_;
    }
    SpatialRoiCameraRecorderSnapshot snapshot() const;

private:
    struct StreamSlot {
        session::spatial_roi::SpatialRoiRecorderStreamView contract;
        std::unique_ptr<SpatialRoiCameraRecorderStreamCore> core;
        SpatialRoiCameraRecorderStreamSnapshot snapshot;
    };

    SpatialRoiCameraRecorder(
        session::spatial_roi::SpatialRoiRecorderCameraContractView contract,
        std::vector<StreamSlot> streams);

    static bool validate_authenticated_view(
        const session::spatial_roi::SpatialRoiRecorderCameraContractView& view,
        std::string* error_out);
    static std::string bounded_reason(const std::string& value,
                                      const char* fallback);

    bool require_state(SpatialRoiCameraRecorderState expected,
                       const char* operation,
                       std::string* error_out);
    void record_stream_failure(std::size_t index,
                               const std::string& reason) noexcept;
    void latch_failure(std::size_t index,
                       const std::string& reason,
                       bool stop_admission) noexcept;
    void stop_admission_best_effort() noexcept;
    void set_error(std::string* error_out) const;

    session::spatial_roi::SpatialRoiRecorderCameraContractView contract_;
    std::vector<StreamSlot> streams_;
    SpatialRoiCameraRecorderState state_ =
        SpatialRoiCameraRecorderState::kConstructed;
    std::string first_failure_stream_id_;
    std::string first_failure_;
    SpatialRoiRecorderStoragePreflightResult storage_preflight_;
    // Separate from the diagnostic string: allocating/copying a diagnostic
    // must never be able to turn a failed required stream into aggregate
    // success under memory pressure.
    bool failure_latched_ = false;
};

}  // namespace orange::spatial_roi::recording
