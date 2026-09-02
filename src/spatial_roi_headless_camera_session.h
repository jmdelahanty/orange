#pragma once

#include "spatial_roi_camera_producer_coordinator.h"
#include "spatial_roi_camera_recorder_process.h"
#include "spatial_roi_acquisition_controller.h"

#include <functional>
#include <memory>
#include <string>

namespace orange::spatial_roi::headless {

// These two narrow interfaces keep lifecycle ordering testable without
// launching a recorder executable or opening four Unix sockets.  Production
// uses the adapters supplied by Create(); a test may inject deterministic
// implementations that still expose the supervisor/coordinator snapshots.
class CameraRecorderProcessHandle {
public:
    virtual ~CameraRecorderProcessHandle() = default;

    CameraRecorderProcessHandle(const CameraRecorderProcessHandle&) = delete;
    CameraRecorderProcessHandle& operator=(
        const CameraRecorderProcessHandle&) = delete;

    virtual bool Start(std::string* error_out) = 0;
    virtual bool WaitForFourSockets(std::string* error_out) = 0;
    virtual bool WaitUntilReady(std::string* error_out) = 0;
    virtual bool WaitForCleanExit(std::string* error_out) = 0;
    virtual bool Stop(std::string* error_out) = 0;
    virtual const recording::SpatialRoiCameraRecorderProcessStatus& status()
        const noexcept = 0;

protected:
    CameraRecorderProcessHandle() = default;
};

class CameraProducerCoordinatorHandle {
public:
    virtual ~CameraProducerCoordinatorHandle() = default;

    CameraProducerCoordinatorHandle(const CameraProducerCoordinatorHandle&) =
        delete;
    CameraProducerCoordinatorHandle& operator=(
        const CameraProducerCoordinatorHandle&) = delete;

    virtual bool Start(std::string* error_out) = 0;
    virtual bool StopAndDrain(std::string* error_out = nullptr) noexcept = 0;
    virtual bool MakeAcquisitionSession(
        SpatialRoiAcquisitionSession* session_out) const noexcept = 0;
    virtual SpatialRoiCameraProducerSnapshot snapshot() const = 0;

protected:
    CameraProducerCoordinatorHandle() = default;
};

using CameraRecorderProcessFactory = std::function<
    std::unique_ptr<CameraRecorderProcessHandle>(
        recording::SpatialRoiCameraRecorderProcessConfig config,
        std::string* error_out)>;

using CameraProducerCoordinatorFactory = std::function<
    std::unique_ptr<CameraProducerCoordinatorHandle>(
        SpatialRoiCameraProducerConfig config,
        std::string* error_out)>;

struct SpatialRoiHeadlessCameraSessionConfig final {
    // The process config is authenticated and launched exactly once by the
    // session.  producer.expected_recorder_pid is replaced with the exact PID
    // returned by that process before the coordinator is created; every other
    // producer authority remains caller supplied and authenticated by the
    // coordinator.
    recording::SpatialRoiCameraRecorderProcessConfig process;
    SpatialRoiCameraProducerConfig producer;

    // Empty factories select the concrete current process/coordinator APIs.
    // Factories are invoked once at their lifecycle phase and never retried.
    CameraRecorderProcessFactory process_factory;
    CameraProducerCoordinatorFactory producer_factory;
};

enum class SpatialRoiHeadlessCameraSessionState {
    kConstructed,
    kStartingProcess,
    kWaitingForSockets,
    kCreatingCoordinator,
    kStartingCoordinator,
    kWaitingForRecorderReady,
    kArmingAcquisition,
    kArmed,
    kFinishing,
    kFinished,
    kAborting,
    kAborted,
    kFailed,
};

const char* spatial_roi_headless_camera_session_state_name(
    SpatialRoiHeadlessCameraSessionState state) noexcept;

struct SpatialRoiHeadlessCameraSessionSnapshot final {
    SpatialRoiHeadlessCameraSessionState state =
        SpatialRoiHeadlessCameraSessionState::kConstructed;
    bool process_started = false;
    bool sockets_ready = false;
    bool coordinator_created = false;
    bool coordinator_started = false;
    bool recorder_ready = false;
    bool acquisition_armed = false;
    bool cleanup_complete = false;
    bool cleanup_succeeded = false;
    recording::SpatialRoiCameraRecorderProcessStatus process;
    SpatialRoiCameraProducerSnapshot producer;
    std::string first_failure;
};

// Owns one recorder process, one camera-level producer coordinator, and one
// acquisition controller.  The controller pointer is intentionally exposed
// only during the kArmed interval; callers must stop submitting before
// Finish()/Abort() is entered.
class SpatialRoiHeadlessCameraSession final {
public:
    static std::unique_ptr<SpatialRoiHeadlessCameraSession> Create(
        SpatialRoiHeadlessCameraSessionConfig config,
        std::string* error_out = nullptr);

    ~SpatialRoiHeadlessCameraSession();

    SpatialRoiHeadlessCameraSession(
        const SpatialRoiHeadlessCameraSession&) = delete;
    SpatialRoiHeadlessCameraSession& operator=(
        const SpatialRoiHeadlessCameraSession&) = delete;
    SpatialRoiHeadlessCameraSession(
        SpatialRoiHeadlessCameraSession&&) = delete;
    SpatialRoiHeadlessCameraSession& operator=(
        SpatialRoiHeadlessCameraSession&&) = delete;

    // Exact start order:
    // process Start -> four-socket wait -> coordinator create/Start ->
    // process ready wait -> coordinator acquisition session -> controller Arm.
    bool Start(std::string* error_out = nullptr);

    // Normal finish order:
    // controller Disarm -> controller Drain -> coordinator StopAndDrain
    // (which closes producer transports) -> process WaitForCleanExit.
    bool Finish(std::string* error_out = nullptr);

    // Best-effort abort/destructor order.  Admission is stopped and drained
    // before producer transports are closed; the process supervisor then
    // performs its bounded SIGTERM/SIGKILL/reap path.
    bool Abort(std::string* error_out = nullptr) noexcept;

    SpatialRoiHeadlessCameraSessionState state() const noexcept
    {
        return state_;
    }

    // Returns non-null only after a successful Start() and before Finish() or
    // Abort() begins.  No caller can retain a valid public controller pointer
    // across the disarm boundary.
    SpatialRoiAcquisitionController* acquisition_controller() noexcept;
    const SpatialRoiAcquisitionController* acquisition_controller() const
        noexcept;

    SpatialRoiHeadlessCameraSessionSnapshot snapshot() const;
    const std::string& first_failure() const noexcept { return first_failure_; }

private:
    SpatialRoiHeadlessCameraSession(
        SpatialRoiHeadlessCameraSessionConfig config,
        std::unique_ptr<CameraRecorderProcessHandle> process);

    static bool fail(std::string* error_out, std::string message);
    static std::string bounded_reason(const std::string& value,
                                      const char* fallback);

    void latch_failure(std::string reason) noexcept;
    bool abort_impl(std::string* error_out) noexcept;

    SpatialRoiHeadlessCameraSessionConfig config_;
    std::unique_ptr<CameraRecorderProcessHandle> process_;
    std::unique_ptr<CameraProducerCoordinatorHandle> coordinator_;
    SpatialRoiAcquisitionController acquisition_controller_;
    SpatialRoiHeadlessCameraSessionState state_ =
        SpatialRoiHeadlessCameraSessionState::kConstructed;
    std::string first_failure_;
    bool cleanup_complete_ = false;
    bool cleanup_succeeded_ = false;
    bool process_started_ = false;
    bool sockets_ready_ = false;
    bool coordinator_created_ = false;
    bool coordinator_started_ = false;
    bool recorder_ready_ = false;
};

}  // namespace orange::spatial_roi::headless
