#pragma once

#include "session/spatial_roi_recorder_camera_contract.h"
#include "spatial_roi_acquisition_controller.h"
#include "spatial_roi_recording_runtime.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <sys/types.h>
#include <vector>

namespace orange::spatial_roi {

// One producer-side stream owns the exact exporter, connected AF_UNIX
// transport, and synchronous FRAME/ACK/RELEASE handoff for one plan lane.
// The camera coordinator invokes a stream only from the runtime's matching
// lane callback; a stream must never reconnect after Start or a fatal handoff.
class SpatialRoiCameraProducerStream {
public:
    virtual ~SpatialRoiCameraProducerStream() = default;

    SpatialRoiCameraProducerStream(const SpatialRoiCameraProducerStream&) =
        delete;
    SpatialRoiCameraProducerStream& operator=(
        const SpatialRoiCameraProducerStream&) = delete;

    virtual bool Start(std::string* error_out) = 0;
    virtual SpatialRoiLaneSinkResult Submit(
        const SpatialRoiLaneDelivery& delivery) = 0;
    virtual void Stop() noexcept = 0;

protected:
    SpatialRoiCameraProducerStream() = default;
};

// This indirection keeps the coordinator's host-side lifecycle testable. The
// production factory below returns an adapter around the existing
// SpatialRoiRecordingRuntime; tests can count one TrySubmit call without
// allocating CUDA state or opening sockets.
class SpatialRoiCameraProducerRuntime {
public:
    virtual ~SpatialRoiCameraProducerRuntime() = default;

    SpatialRoiCameraProducerRuntime(const SpatialRoiCameraProducerRuntime&) =
        delete;
    SpatialRoiCameraProducerRuntime& operator=(
        const SpatialRoiCameraProducerRuntime&) = delete;

    virtual SpatialRoiBatchSubmission TrySubmit(
        const SpatialRoiSourceView& source) = 0;
    virtual void StopAccepting() noexcept = 0;
    virtual bool StopAcceptingAndDrain(
        std::string* error_out = nullptr) noexcept = 0;

protected:
    SpatialRoiCameraProducerRuntime() = default;
};

using SpatialRoiCameraProducerStreamFactory = std::function<
    std::unique_ptr<SpatialRoiCameraProducerStream>(
        const session::spatial_roi::SpatialRoiRecorderStreamView& stream,
        std::size_t plan_index,
        std::string* error_out)>;

using SpatialRoiCameraProducerRuntimeFactory = std::function<
    std::unique_ptr<SpatialRoiCameraProducerRuntime>(
        const nlohmann::json& verified_plan,
        const std::string& camera_serial,
        int producer_gpu_id,
        SpatialRoiLaneSink sink,
        std::string* error_out)>;

struct SpatialRoiCameraProducerConfig final {
    // The candidate contract is authenticated against the independently
    // verified plan before either factory is called. Neither factory may use
    // candidate JSON as an alternate source of identity or geometry.
    nlohmann::json candidate_contract;
    nlohmann::json independently_verified_plan;
    std::string expected_recording_root;
    session::spatial_roi::SpatialRoiRecorderRuntimeGpuMapping
        expected_gpu_mapping;

    int producer_gpu_id = -1;

    // The recorder child credentials are required by the concrete connector;
    // the coordinator does not discover, launch, or retry a peer.
    pid_t expected_recorder_pid = -1;
    uid_t expected_recorder_uid = static_cast<uid_t>(-1);
    std::chrono::milliseconds connect_timeout{1000};
    std::chrono::milliseconds write_timeout{1000};
    std::chrono::milliseconds ipc_response_timeout{1000};

    // Empty factories select the concrete existing spatial-ROI components.
    // Supplying both is the supported host-test seam and does not weaken
    // contract authentication.
    SpatialRoiCameraProducerStreamFactory stream_factory;
    SpatialRoiCameraProducerRuntimeFactory runtime_factory;
};

enum class SpatialRoiCameraProducerState {
    kConstructed,
    kStarting,
    kReady,
    kStopped,
    kFailed,
};

const char* spatial_roi_camera_producer_state_name(
    SpatialRoiCameraProducerState state) noexcept;

enum class SpatialRoiCameraProducerSubmitStatus {
    kSubmitted,
    kRuntimeIncomplete,
    kNotReady,
    kStopped,
    kBusy,
    kInvalidArgument,
    kPoolExhausted,
    kCudaError,
    kSourceQuarantined,
    kDuplicateOrOutOfOrder,
    kFailed,
};

const char* spatial_roi_camera_producer_submit_status_name(
    SpatialRoiCameraProducerSubmitStatus status) noexcept;

struct SpatialRoiCameraProducerSubmitResult final {
    SpatialRoiCameraProducerSubmitStatus status =
        SpatialRoiCameraProducerSubmitStatus::kStopped;
    SpatialRoiBatchSubmission runtime_submission;
    std::string error;

    bool submitted() const noexcept
    {
        return status == SpatialRoiCameraProducerSubmitStatus::kSubmitted;
    }
};

struct SpatialRoiCameraProducerSnapshot final {
    SpatialRoiCameraProducerState state =
        SpatialRoiCameraProducerState::kConstructed;
    std::string recording_id;
    std::string session_id;
    std::string recording_identity_token;
    std::string producer_generation;
    std::string spatial_roi_plan_sha256;
    int camera_id = -1;
    std::string camera_serial;
    std::size_t stream_count = 0;
    std::uint64_t submit_attempted = 0;
    std::uint64_t submitted = 0;
    std::uint64_t incomplete = 0;
    std::uint64_t rejected = 0;
    std::string first_failure;
};

// One camera-level producer owner. Every accepted full-resolution source is
// submitted to one immutable runtime exactly once; the runtime performs the
// verified four-ROI extraction and queues one delivery per plan lane. The
// coordinator itself has no acquire_frames/headless dependency and does not
// own the full-frame recorder path.
class SpatialRoiCameraProducerCoordinator final {
public:
    static std::unique_ptr<SpatialRoiCameraProducerCoordinator> Create(
        SpatialRoiCameraProducerConfig config,
        std::string* error_out = nullptr);

    ~SpatialRoiCameraProducerCoordinator();

    SpatialRoiCameraProducerCoordinator(
        const SpatialRoiCameraProducerCoordinator&) = delete;
    SpatialRoiCameraProducerCoordinator& operator=(
        const SpatialRoiCameraProducerCoordinator&) = delete;
    SpatialRoiCameraProducerCoordinator(
        SpatialRoiCameraProducerCoordinator&&) = delete;
    SpatialRoiCameraProducerCoordinator& operator=(
        SpatialRoiCameraProducerCoordinator&&) = delete;

    // Start performs one connector/HELLO attempt per authenticated stream and
    // then creates the one camera runtime. A partial start stops all streams
    // and leaves the owner failed; no endpoint is retried.
    bool Start(std::string* error_out = nullptr);

    // This is the only per-frame producer update path. It does not inspect,
    // copy, or submit the source four times; SpatialRoiRecordingRuntime owns
    // one bounded extraction batch and fans it out to exactly four lanes.
    SpatialRoiCameraProducerSubmitResult Submit(
        const SpatialRoiSourceView& source) noexcept;

    // Stop admission, drain already queued lane work, then stop transports.
    // Destruction invokes the same best-effort boundary. A fatal handoff with
    // indeterminate source ownership remains quarantined by its handoff.
    // Returns false when any admitted ROI lane failed, was rejected/dropped,
    // or otherwise completed incompletely. Transports are still stopped on
    // that path and the first failure is retained in the snapshot.
    bool StopAndDrain(std::string* error_out = nullptr) noexcept;

    SpatialRoiCameraProducerState state() const noexcept { return state_; }
    bool ready() const noexcept
    {
        return state_ == SpatialRoiCameraProducerState::kReady;
    }
    bool failed() const noexcept
    {
        return state_ == SpatialRoiCameraProducerState::kFailed;
    }
    // Production Start() retains the exact runtime that owns this
    // coordinator's four lane sink closures. An acquisition controller may
    // Arm() this shared object so its WORKER_ENTRY adapter submits to the
    // same fanout; it must not create a second runtime. Custom test runtime
    // factories intentionally return nullptr here.
    std::shared_ptr<SpatialRoiRecordingRuntime> acquisition_runtime()
        const noexcept;
    // Convenience adapter for the existing WORKER_ENTRY controller. The
    // returned session points at the same coordinator-owned runtime and
    // carries its authenticated identity; callers should Arm() it once and
    // then submit through that controller rather than constructing another
    // runtime.
    bool MakeAcquisitionSession(
        SpatialRoiAcquisitionSession* session_out) const noexcept;
    const std::string& first_failure() const noexcept
    {
        return first_failure_;
    }
    const session::spatial_roi::SpatialRoiRecorderCameraContractView& contract()
        const noexcept
    {
        return contract_;
    }
    SpatialRoiCameraProducerSnapshot snapshot() const;

private:
    struct StreamSlot {
        session::spatial_roi::SpatialRoiRecorderStreamView contract;
        std::unique_ptr<SpatialRoiCameraProducerStream> stream;
        bool start_attempted = false;
        bool started = false;
        bool stopped = false;
    };

    SpatialRoiCameraProducerCoordinator(
        SpatialRoiCameraProducerConfig config,
        session::spatial_roi::SpatialRoiRecorderCameraContractView contract,
        std::vector<StreamSlot> streams);

    static bool validate_config(const SpatialRoiCameraProducerConfig& config,
                                std::string* error_out);
    static bool validate_contract(
        const session::spatial_roi::SpatialRoiRecorderCameraContractView& view,
        int producer_gpu_id,
        std::string* error_out);
    static SpatialRoiCameraProducerSubmitStatus map_runtime_status(
        SpatialRoiRuntimeSubmitStatus status) noexcept;
    static std::string bounded_reason(const std::string& value,
                                      const char* fallback);
    static bool fail(std::string* error_out, std::string message);

    void latch_failure(std::string reason) noexcept;
    void stop_streams_best_effort() noexcept;

    SpatialRoiCameraProducerConfig config_;
    session::spatial_roi::SpatialRoiRecorderCameraContractView contract_;
    // Shared because acquisition_runtime() may be retained by an
    // acquisition controller after this coordinator has begun teardown. The
    // runtime sink therefore keeps its stream objects alive and sees a
    // stopped (never dangling) stream until the controller drains too.
    std::shared_ptr<std::vector<StreamSlot>> streams_;
    std::unique_ptr<SpatialRoiCameraProducerRuntime> runtime_;
    std::shared_ptr<SpatialRoiRecordingRuntime> recording_runtime_;
    SpatialRoiCameraProducerRuntimeFactory runtime_factory_;
    SpatialRoiCameraProducerState state_ =
        SpatialRoiCameraProducerState::kConstructed;
    std::string first_failure_;
    std::uint64_t submit_attempted_ = 0;
    std::uint64_t submitted_ = 0;
    std::uint64_t incomplete_ = 0;
    std::uint64_t rejected_ = 0;
};

}  // namespace orange::spatial_roi
