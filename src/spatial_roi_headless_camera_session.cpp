#include "spatial_roi_headless_camera_session.h"

#include <exception>
#include <utility>

namespace orange::spatial_roi::headless {
namespace {

constexpr std::size_t kMaximumReasonBytes = 1024U;

bool set_error(std::string* error_out, std::string message)
{
    if (error_out) {
        *error_out = std::move(message);
    }
    return false;
}

std::string bounded_reason_text(const std::string& value, const char* fallback)
{
    std::string result = value;
    if (result.empty()) {
        result = fallback ? fallback : "operation failed";
    }
    if (result.size() > kMaximumReasonBytes) {
        result.resize(kMaximumReasonBytes);
    }
    return result;
}

std::string exception_reason(const char* operation,
                             const std::exception& exception)
{
    return bounded_reason_text(
        std::string(operation ? operation : "operation") + " threw: " +
            exception.what(),
        "operation threw");
}

class ConcreteProcessHandle final : public CameraRecorderProcessHandle {
public:
    explicit ConcreteProcessHandle(
        std::unique_ptr<recording::SpatialRoiCameraRecorderProcess> process)
        : process_(std::move(process))
    {
    }

    bool Start(std::string* error_out) override
    {
        return process_ && process_->Start(error_out);
    }

    bool WaitForFourSockets(std::string* error_out) override
    {
        return process_ && process_->WaitForFourSockets(error_out);
    }

    bool WaitUntilReady(std::string* error_out) override
    {
        return process_ && process_->WaitUntilReady(error_out);
    }

    bool WaitForCleanExit(std::string* error_out) override
    {
        return process_ && process_->WaitForCleanExit(error_out);
    }

    bool Stop(std::string* error_out) override
    {
        return process_ && process_->Stop(error_out);
    }

    const recording::SpatialRoiCameraRecorderProcessStatus& status()
        const noexcept override
    {
        return process_->status();
    }

private:
    std::unique_ptr<recording::SpatialRoiCameraRecorderProcess> process_;
};

class ConcreteCoordinatorHandle final
    : public CameraProducerCoordinatorHandle {
public:
    explicit ConcreteCoordinatorHandle(
        std::unique_ptr<SpatialRoiCameraProducerCoordinator> coordinator)
        : coordinator_(std::move(coordinator))
    {
    }

    bool Start(std::string* error_out) override
    {
        return coordinator_ && coordinator_->Start(error_out);
    }

    bool StopAndDrain(std::string* error_out) noexcept override
    {
        if (coordinator_) {
            return coordinator_->StopAndDrain(error_out);
        }
        if (error_out) {
            try {
                *error_out = "headless camera session has no producer coordinator";
            } catch (...) {
            }
        }
        return false;
    }

    bool MakeAcquisitionSession(
        SpatialRoiAcquisitionSession* session_out) const noexcept override
    {
        return coordinator_ && coordinator_->MakeAcquisitionSession(session_out);
    }

    SpatialRoiCameraProducerSnapshot snapshot() const override
    {
        return coordinator_ ? coordinator_->snapshot()
                            : SpatialRoiCameraProducerSnapshot{};
    }

private:
    std::unique_ptr<SpatialRoiCameraProducerCoordinator> coordinator_;
};

std::unique_ptr<CameraRecorderProcessHandle> make_concrete_process(
    recording::SpatialRoiCameraRecorderProcessConfig config,
    std::string* error_out)
{
    auto process = recording::SpatialRoiCameraRecorderProcess::Create(
        std::move(config), error_out);
    if (!process) {
        return nullptr;
    }
    return std::make_unique<ConcreteProcessHandle>(std::move(process));
}

std::unique_ptr<CameraProducerCoordinatorHandle> make_concrete_coordinator(
    SpatialRoiCameraProducerConfig config,
    std::string* error_out)
{
    auto coordinator = SpatialRoiCameraProducerCoordinator::Create(
        std::move(config), error_out);
    if (!coordinator) {
        return nullptr;
    }
    return std::make_unique<ConcreteCoordinatorHandle>(std::move(coordinator));
}

}  // namespace

const char* spatial_roi_headless_camera_session_state_name(
    const SpatialRoiHeadlessCameraSessionState state) noexcept
{
    switch (state) {
    case SpatialRoiHeadlessCameraSessionState::kConstructed:
        return "constructed";
    case SpatialRoiHeadlessCameraSessionState::kStartingProcess:
        return "starting_process";
    case SpatialRoiHeadlessCameraSessionState::kWaitingForSockets:
        return "waiting_for_sockets";
    case SpatialRoiHeadlessCameraSessionState::kCreatingCoordinator:
        return "creating_coordinator";
    case SpatialRoiHeadlessCameraSessionState::kStartingCoordinator:
        return "starting_coordinator";
    case SpatialRoiHeadlessCameraSessionState::kWaitingForRecorderReady:
        return "waiting_for_recorder_ready";
    case SpatialRoiHeadlessCameraSessionState::kArmingAcquisition:
        return "arming_acquisition";
    case SpatialRoiHeadlessCameraSessionState::kArmed:
        return "armed";
    case SpatialRoiHeadlessCameraSessionState::kFinishing:
        return "finishing";
    case SpatialRoiHeadlessCameraSessionState::kFinished:
        return "finished";
    case SpatialRoiHeadlessCameraSessionState::kAborting:
        return "aborting";
    case SpatialRoiHeadlessCameraSessionState::kAborted:
        return "aborted";
    case SpatialRoiHeadlessCameraSessionState::kFailed:
        return "failed";
    }
    return "unknown";
}

SpatialRoiHeadlessCameraSession::SpatialRoiHeadlessCameraSession(
    SpatialRoiHeadlessCameraSessionConfig config,
    std::unique_ptr<CameraRecorderProcessHandle> process)
    : config_(std::move(config)),
      process_(std::move(process))
{
}

SpatialRoiHeadlessCameraSession::~SpatialRoiHeadlessCameraSession()
{
    try {
        (void)Abort(nullptr);
    } catch (...) {
        // Destruction is a best-effort boundary. The process supervisor owns
        // its own bounded escalation if a child does not exit promptly.
    }
}

std::unique_ptr<SpatialRoiHeadlessCameraSession>
SpatialRoiHeadlessCameraSession::Create(
    SpatialRoiHeadlessCameraSessionConfig config,
    std::string* error_out)
{
    if (error_out) {
        error_out->clear();
    }
    try {
        if (!config.process_factory) {
            config.process_factory = make_concrete_process;
        }
        std::string process_error;
        std::unique_ptr<CameraRecorderProcessHandle> process =
            config.process_factory(std::move(config.process), &process_error);
        if (!process) {
            set_error(
                error_out,
                "headless camera session could not construct recorder process: " +
                    bounded_reason_text(process_error, "null process"));
            return nullptr;
        }
        return std::unique_ptr<SpatialRoiHeadlessCameraSession>(
            new SpatialRoiHeadlessCameraSession(std::move(config),
                                                std::move(process)));
    } catch (const std::exception& exception) {
        set_error(error_out, exception_reason("headless camera session creation", exception));
    } catch (...) {
        set_error(error_out, "headless camera session creation threw");
    }
    return nullptr;
}

bool SpatialRoiHeadlessCameraSession::fail(std::string* error_out,
                                           std::string message)
{
    return set_error(error_out, std::move(message));
}

std::string SpatialRoiHeadlessCameraSession::bounded_reason(
    const std::string& value,
    const char* fallback)
{
    return bounded_reason_text(value, fallback);
}

void SpatialRoiHeadlessCameraSession::latch_failure(std::string reason) noexcept
{
    if (!first_failure_.empty()) {
        return;
    }
    try {
        first_failure_ = bounded_reason(reason, "headless camera session failed");
    } catch (...) {
        // Keep the failure state terminal even when explanatory allocation
        // fails at an exception boundary.
    }
}

bool SpatialRoiHeadlessCameraSession::Start(std::string* error_out)
{
    if (error_out) {
        error_out->clear();
    }
    if (state_ == SpatialRoiHeadlessCameraSessionState::kArmed) {
        return true;
    }
    if (state_ != SpatialRoiHeadlessCameraSessionState::kConstructed) {
        return fail(
            error_out,
            "headless camera session Start is invalid from state " +
                std::string(spatial_roi_headless_camera_session_state_name(state_)));
    }
    if (!process_) {
        latch_failure("headless camera session has no recorder process");
        state_ = SpatialRoiHeadlessCameraSessionState::kFailed;
        return fail(error_out, first_failure_);
    }

    const auto phase_failed = [&](const char* phase,
                                  const std::string& phase_error) {
        latch_failure(
            std::string("headless camera session ") + phase + " failed: " +
            bounded_reason(phase_error, phase));
        (void)abort_impl(nullptr);
        state_ = SpatialRoiHeadlessCameraSessionState::kFailed;
        return fail(error_out, first_failure_);
    };

    try {
        std::string phase_error;
        state_ = SpatialRoiHeadlessCameraSessionState::kStartingProcess;
        if (!process_->Start(&phase_error)) {
            return phase_failed("recorder process start", phase_error);
        }
        process_started_ = true;
        if (process_->status().pid <= 0) {
            return phase_failed(
                "recorder process start",
                "recorder process did not return a positive child PID");
        }

        state_ = SpatialRoiHeadlessCameraSessionState::kWaitingForSockets;
        if (!process_->WaitForFourSockets(&phase_error)) {
            return phase_failed("recorder socket binding", phase_error);
        }
        sockets_ready_ = true;

        SpatialRoiCameraProducerConfig producer_config = config_.producer;
        producer_config.expected_recorder_pid = process_->status().pid;
        state_ = SpatialRoiHeadlessCameraSessionState::kCreatingCoordinator;
        if (!config_.producer_factory) {
            config_.producer_factory = make_concrete_coordinator;
        }
        std::string coordinator_error;
        coordinator_ = config_.producer_factory(
            std::move(producer_config), &coordinator_error);
        if (!coordinator_) {
            return phase_failed(
                "producer coordinator creation",
                coordinator_error.empty() ? "null coordinator" : coordinator_error);
        }
        coordinator_created_ = true;

        state_ = SpatialRoiHeadlessCameraSessionState::kStartingCoordinator;
        if (!coordinator_->Start(&phase_error)) {
            return phase_failed("producer coordinator start", phase_error);
        }
        coordinator_started_ = true;

        state_ = SpatialRoiHeadlessCameraSessionState::kWaitingForRecorderReady;
        if (!process_->WaitUntilReady(&phase_error)) {
            return phase_failed("recorder readiness", phase_error);
        }
        recorder_ready_ = true;

        SpatialRoiAcquisitionSession acquisition_session;
        state_ = SpatialRoiHeadlessCameraSessionState::kArmingAcquisition;
        if (!coordinator_->MakeAcquisitionSession(&acquisition_session)) {
            return phase_failed(
                "acquisition session creation",
                "producer coordinator did not expose its production runtime");
        }
        const SpatialRoiAcquisitionArmResult arm =
            acquisition_controller_.Arm(std::move(acquisition_session));
        if (!arm.armed()) {
            return phase_failed(
                "acquisition controller arm",
                arm.error.empty()
                    ? std::string("status=") +
                          spatial_roi_acquisition_arm_status_name(arm.status)
                    : arm.error);
        }
        state_ = SpatialRoiHeadlessCameraSessionState::kArmed;
        return true;
    } catch (const std::exception& exception) {
        return phase_failed("lifecycle", exception_reason("lifecycle", exception));
    } catch (...) {
        return phase_failed("lifecycle", "unknown lifecycle exception");
    }
}

bool SpatialRoiHeadlessCameraSession::Finish(std::string* error_out)
{
    if (error_out) {
        error_out->clear();
    }
    if (state_ == SpatialRoiHeadlessCameraSessionState::kFinished) {
        return true;
    }
    if (state_ != SpatialRoiHeadlessCameraSessionState::kArmed) {
        return fail(
            error_out,
            "headless camera session Finish requires an armed session; state=" +
                std::string(spatial_roi_headless_camera_session_state_name(state_)));
    }

    state_ = SpatialRoiHeadlessCameraSessionState::kFinishing;
    bool success = true;
    if (!acquisition_controller_.Disarm()) {
        success = false;
        latch_failure("headless camera session acquisition disarm found no active session");
    }
    if (!acquisition_controller_.Drain()) {
        success = false;
        latch_failure("headless camera session acquisition drain failed");
    }
    if (coordinator_) {
        try {
            std::string coordinator_error;
            if (!coordinator_->StopAndDrain(&coordinator_error)) {
                success = false;
                latch_failure(
                    "headless camera session ROI lane drain failed: " +
                    bounded_reason(coordinator_error, "ROI lane incomplete"));
            }
        } catch (...) {
            success = false;
            latch_failure("headless camera session ROI lane drain threw");
        }
    } else {
        success = false;
        latch_failure("headless camera session has no producer coordinator");
    }

    std::string process_error;
    if (!process_ || !process_->WaitForCleanExit(&process_error)) {
        success = false;
        latch_failure(
            "headless camera session recorder clean exit failed: " +
            bounded_reason(process_error, "clean exit failed"));
        // WaitForCleanExit already performs bounded escalation on failure;
        // Stop is still called to cover an injected supervisor with the same
        // contract and to make the destructor path idempotent.
        if (process_) {
            std::string stop_error;
            if (!process_->Stop(&stop_error)) {
                latch_failure(
                    "headless camera session recorder stop failed: " +
                    bounded_reason(stop_error, "stop failed"));
            }
        }
    }

    cleanup_complete_ = true;
    if (success) {
        cleanup_succeeded_ = true;
        state_ = SpatialRoiHeadlessCameraSessionState::kFinished;
        return true;
    }
    state_ = SpatialRoiHeadlessCameraSessionState::kFailed;
    return fail(error_out, first_failure_);
}

bool SpatialRoiHeadlessCameraSession::abort_impl(std::string* error_out) noexcept
{
    if (cleanup_complete_) {
        return cleanup_succeeded_;
    }
    state_ = SpatialRoiHeadlessCameraSessionState::kAborting;
    bool success = true;

    try {
        // Disarm linearizes against any acquisition callback. Drain then
        // joins the runtime lanes before coordinator teardown closes streams.
        (void)acquisition_controller_.Disarm();
        (void)acquisition_controller_.Drain();
    } catch (...) {
        success = false;
        latch_failure("headless camera session acquisition abort drain threw");
    }

    try {
        if (coordinator_) {
            std::string coordinator_error;
            if (!coordinator_->StopAndDrain(&coordinator_error)) {
                success = false;
                latch_failure(
                    "headless camera session ROI lane abort drain failed: " +
                    bounded_reason(coordinator_error, "ROI lane incomplete"));
            }
        }
    } catch (...) {
        success = false;
        latch_failure("headless camera session producer abort drain threw");
    }

    try {
        if (process_) {
            std::string process_error;
            if (!process_->Stop(&process_error)) {
                success = false;
                latch_failure(
                    "headless camera session recorder abort failed: " +
                    bounded_reason(process_error, "bounded recorder stop failed"));
            }
        }
    } catch (const std::exception& exception) {
        success = false;
        latch_failure(exception_reason("headless camera session recorder abort", exception));
    } catch (...) {
        success = false;
        latch_failure("headless camera session recorder abort threw");
    }

    cleanup_complete_ = true;
    if (success) {
        cleanup_succeeded_ = true;
        state_ = SpatialRoiHeadlessCameraSessionState::kAborted;
        return true;
    }
    state_ = SpatialRoiHeadlessCameraSessionState::kFailed;
    try {
        if (error_out) {
            *error_out = first_failure_.empty()
                             ? "headless camera session abort failed"
                             : first_failure_;
        }
    } catch (...) {
        // Abort is noexcept; the terminal state remains authoritative when a
        // caller's error string cannot be assigned.
    }
    return false;
}

bool SpatialRoiHeadlessCameraSession::Abort(std::string* error_out) noexcept
{
    if (error_out) {
        error_out->clear();
    }
    return abort_impl(error_out);
}

SpatialRoiAcquisitionController*
SpatialRoiHeadlessCameraSession::acquisition_controller() noexcept
{
    return state_ == SpatialRoiHeadlessCameraSessionState::kArmed &&
                   acquisition_controller_.armed()
               ? &acquisition_controller_
               : nullptr;
}

const SpatialRoiAcquisitionController*
SpatialRoiHeadlessCameraSession::acquisition_controller() const noexcept
{
    return state_ == SpatialRoiHeadlessCameraSessionState::kArmed &&
                   acquisition_controller_.armed()
               ? &acquisition_controller_
               : nullptr;
}

SpatialRoiHeadlessCameraSessionSnapshot
SpatialRoiHeadlessCameraSession::snapshot() const
{
    SpatialRoiHeadlessCameraSessionSnapshot result;
    result.state = state_;
    result.cleanup_complete = cleanup_complete_;
    result.cleanup_succeeded = cleanup_succeeded_;
    result.first_failure = first_failure_;
    if (process_) {
        result.process = process_->status();
    }
    result.process_started = process_started_;
    result.sockets_ready = sockets_ready_;
    result.recorder_ready = recorder_ready_;
    result.coordinator_created = coordinator_created_;
    if (coordinator_) {
        result.producer = coordinator_->snapshot();
    }
    result.coordinator_started = coordinator_started_;
    result.acquisition_armed = acquisition_controller_.armed();
    return result;
}

}  // namespace orange::spatial_roi::headless
