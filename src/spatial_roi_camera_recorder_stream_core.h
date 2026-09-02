#pragma once

#include "session/spatial_roi_recorder_contract_parser.h"
#include "spatial_roi_camera_recorder.h"
#include "spatial_roi_recorder_cuda_detach.h"
#include "spatial_roi_recorder_evidence.h"
#include "spatial_roi_recorder_frame_journal.h"
#include "spatial_roi_recorder_ipc_session.h"
#include "spatial_roi_lossless_encoder.h"
#include "spatial_roi_socket_runtime_directory.h"
#include "spatial_roi_unix_socket_listener.h"

#include <chrono>
#include <cstdint>
#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <sys/types.h>
#include <thread>

namespace orange::spatial_roi::recording {

// Configuration for one concrete, fixed-region recorder stream.  The camera
// owner supplies the already-authenticated stream projection and shares its
// descriptor-bound artifact/runtime authorities with all four cores.  No
// pathname is accepted as a new authority here: stream.socket_path and the
// twelve binding artifact paths must be the exact authenticated values.
struct SpatialRoiCameraRecorderStreamCoreConfig final {
    session::spatial_roi::SpatialRoiRecorderStreamView stream;
    SpatialRoiRecorderEvidenceBinding evidence_binding;
    std::shared_ptr<SpatialRoiRecorderArtifactRoot> artifact_root;
    std::shared_ptr<ipc::SpatialRoiSocketRuntimeDirectory> runtime_directory;

    // The camera supervisor must pass credentials for the already-spawned
    // Orange producer.  Empty/unset credentials are rejected before binding.
    pid_t expected_producer_pid = -1;
    uid_t expected_producer_uid = static_cast<uid_t>(-1);

    // Every wait is bounded.  A timeout during readiness is nonterminal;
    // timeout during the media probe is terminal for this stream.
    std::chrono::milliseconds accept_timeout{1000};
    std::chrono::milliseconds ipc_response_timeout{1000};
    std::chrono::milliseconds video_probe_timeout{60000};
};

// The concrete implementation composes the already-tested Gate2 seams for
// exactly one logical stream.  It is deliberately separate from the camera
// aggregate: the aggregate can construct four of these in authenticated plan
// order and then advance them independently.
//
// Construction and Create() perform only value/authority checks.  They do not
// bind sockets, create artifact files, initialize CUDA, or start threads.
// Start() binds one exact endpoint. PollReadiness() accepts/authenticates one
// producer, arms recorder resources, negotiates IPC-v2, and starts the session
// owner thread. The session thread drains FRAME records until producer EOF or
// a fatal protocol/ownership error.
class SpatialRoiConcreteCameraRecorderStreamCore final
    : public SpatialRoiCameraRecorderStreamCore {
public:
    static std::unique_ptr<SpatialRoiConcreteCameraRecorderStreamCore> Create(
        SpatialRoiCameraRecorderStreamCoreConfig config,
        std::string* error_out = nullptr);

    ~SpatialRoiConcreteCameraRecorderStreamCore() override;

    SpatialRoiConcreteCameraRecorderStreamCore(
        const SpatialRoiConcreteCameraRecorderStreamCore&) = delete;
    SpatialRoiConcreteCameraRecorderStreamCore& operator=(
        const SpatialRoiConcreteCameraRecorderStreamCore&) = delete;

    bool Start(std::string* error_out = nullptr) override;
    SpatialRoiCameraRecorderReadinessStatus PollReadiness(
        std::string* error_out = nullptr) override;
    SpatialRoiCameraRecorderEofStatus PollEof(
        std::string* error_out = nullptr) override;
    bool StopAdmission(std::string* error_out = nullptr) override;
    bool Drain(std::string* error_out = nullptr) override;
    bool Finalize(std::string* error_out = nullptr) override;

    bool started() const noexcept;
    bool negotiated() const noexcept;
    bool clean_eof() const noexcept;
    bool completed() const noexcept;
    bool failed() const noexcept;
    std::string first_failure() const;

private:
    enum class State {
        kConstructed,
        kBound,
        kNegotiated,
        kEofObserved,
        kDrained,
        kFinalized,
        kFailed,
    };

    explicit SpatialRoiConcreteCameraRecorderStreamCore(
        SpatialRoiCameraRecorderStreamCoreConfig config);

    static bool validate_config(
        const SpatialRoiCameraRecorderStreamCoreConfig& config,
        std::string* error_out);

    bool arm_resources(std::string* error_out);
    bool on_encoder_result(
        const encoder::SpatialRoiLosslessFrameResult& result,
        std::string* error_out);
    bool on_transport_outcome(
        const ipc::SpatialRoiRecorderIpcFrameOutcome& outcome,
        std::string* error_out);
    ipc::SpatialRoiRecorderIpcDispatchResult dispatch_frame(
        const ipc::SpatialRoiIpcFrame& frame);
    void run_session() noexcept;

    bool latch_failure(const std::string& reason,
                       std::string* error_out = nullptr);
    bool require_not_failed(const char* operation,
                            std::string* error_out);
    static std::string bounded_reason(const std::string& value,
                                      const char* fallback);
    static ipc::SpatialRoiIpcStreamIdentity stream_identity(
        const session::spatial_roi::SpatialRoiRecorderStreamView& stream);
    static ipc::SpatialRoiRecorderCudaDetachGeometry stream_geometry(
        const session::spatial_roi::SpatialRoiRecorderStreamView& stream);

    SpatialRoiCameraRecorderStreamCoreConfig config_;
    SpatialRoiRecorderEvidenceBinding evidence_binding_;

    // StopAdmission linearizes against dispatch at this mutex. A dispatch
    // already holding it may finish its bounded detach/enqueue operation;
    // after StopAdmission returns, no later frame can enter those resources.
    mutable std::mutex admission_mutex_;
    mutable std::mutex mutex_;
    State state_ = State::kConstructed;
    std::atomic<bool> admission_stopped_{false};
    std::atomic<bool> cancellation_requested_{false};
    std::atomic<bool> session_done_{false};
    bool drained_ = false;
    bool finalized_ = false;
    bool session_result_valid_ = false;
    ipc::SpatialRoiRecorderIpcSessionResult session_result_;
    std::string first_failure_;

    std::unique_ptr<ipc::SpatialRoiUnixSocketListener> listener_;
    std::unique_ptr<ipc::SpatialRoiUnixSocketLineTransport> transport_;
    std::unique_ptr<ipc::SpatialRoiRecorderIpcSession> session_;
    std::thread session_thread_;

    std::unique_ptr<SpatialRoiRecorderEvidenceWriter> evidence_writer_;
    std::unique_ptr<SpatialRoiRecorderFrameJournal> journal_;
    std::unique_ptr<ipc::SpatialRoiRecorderCudaDetachPool> detach_pool_;
    std::unique_ptr<encoder::SpatialRoiLosslessEncoder> encoder_;
    std::shared_ptr<const SpatialRoiRecorderVideoSanityResult>
        video_sanity_result_;
};

}  // namespace orange::spatial_roi::recording
