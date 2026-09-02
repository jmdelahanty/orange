#pragma once

#include "json.hpp"
#include "session/spatial_roi_recorder_contract.h"
#include "session/spatial_roi_recorder_contract_parser.h"
#include "spatial_roi_recorder_artifact_root.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <sys/types.h>
#include <vector>

namespace orange::spatial_roi::recording {

// This is deliberately a child-process boundary, not another recorder
// implementation.  The supervisor authenticates the two files before fork,
// derives the four expected sockets from that authenticated view, and passes
// only the fixed argument set below to the child executable.
enum class SpatialRoiCameraRecorderProcessState {
    kConstructed,
    kStarting,
    kSocketsBound,
    kReady,
    kExited,
    kFailed,
    kStopped,
};

const char* spatial_roi_camera_recorder_process_state_name(
    SpatialRoiCameraRecorderProcessState state) noexcept;

struct SpatialRoiCameraRecorderProcessSnapshot final {
    std::string event;
    std::string status;
    std::string state;
    bool ready = false;
    bool clean_eof = false;
    bool completed = false;
    bool failed = false;
    std::string first_failure_stream_id;
    std::string first_failure;
    std::string error;

    // The child line is bounded before parsing and this is only the latest
    // snapshot for a given event.  Keeping it makes per-ROI counters and
    // future status fields observable without retaining an unbounded JSONL
    // transcript.
    nlohmann::json payload = nlohmann::json::object();
};

struct SpatialRoiCameraRecorderProcessStatus final {
    SpatialRoiCameraRecorderProcessState state =
        SpatialRoiCameraRecorderProcessState::kConstructed;
    pid_t pid = -1;
    bool started = false;
    bool sockets_bound = false;
    bool ready = false;
    bool terminal_seen = false;
    bool exited = false;
    bool reaped = false;
    int exit_code = -1;
    int term_signal = 0;
    std::uint64_t stdout_bytes_read = 0;
    std::string error;
    SpatialRoiCameraRecorderProcessSnapshot starting;
    SpatialRoiCameraRecorderProcessSnapshot ready_snapshot;
    SpatialRoiCameraRecorderProcessSnapshot heartbeat;
    SpatialRoiCameraRecorderProcessSnapshot terminal;
    SpatialRoiCameraRecorderProcessSnapshot last;
};

// Production uses the built-in fork/fexecve launcher.  Tests may inject a
// child that writes JSONL to stdout and returns its PID; the supervisor still
// owns the pipe, exact-PID wait, timeout, and termination protocol.  The
// callback takes ownership of stdout_fd on success and must arrange for it to
// be closed in the child before returning from the child process.
using SpatialRoiCameraRecorderProcessSpawnOverride = std::function<pid_t(
    const std::vector<std::string>& argv,
    int stdout_fd,
    std::string* error_out)>;

struct SpatialRoiCameraRecorderProcessConfig final {
    // These paths are authenticated as bounded, non-symlink regular files
    // before launch and are passed unchanged to the child.  The child repeats
    // authentication, so a post-authentication replacement is fail-closed.
    std::string contract_path;
    std::string verified_plan_path;
    std::string expected_recording_root;
    std::string recorder_executable;

    // The recorder creates its artifact directory in the child, so this
    // identity is optional at process construction. When a trusted caller
    // already retained the exact artifact-root identity, every successful
    // child preflight must match it.
    bool expected_artifact_root_identity_available = false;
    SpatialRoiRecorderArtifactIdentity expected_artifact_root_identity;

    session::spatial_roi::SpatialRoiRecorderRuntimeGpuMapping gpu_mapping;
    pid_t expected_producer_pid = -1;
    uid_t expected_producer_uid = static_cast<uid_t>(-1);

    // Optional camera-recorder-wide Linux CPU list (for example
    // "28-31,60-63"). The child applies it to its control thread before any
    // workers are created; those workers inherit the multi-core mask. Empty
    // preserves and reports the ambient affinity. The source is mandatory
    // whenever a mask is configured so durable metadata retains authority.
    std::string cpu_affinity;
    std::string cpu_affinity_source;

    // EOF timeout is mandatory and bounded by the child CLI's seven-day
    // ceiling.  Every other timeout is explicit in the generated argv and is
    // also bounded before fork.
    std::chrono::milliseconds eof_timeout{0};
    std::chrono::milliseconds readiness_timeout{60000};
    std::chrono::milliseconds poll_interval{20};
    std::chrono::milliseconds heartbeat_interval{5000};
    std::chrono::milliseconds accept_timeout{1000};
    std::chrono::milliseconds ipc_timeout{30000};
    std::chrono::milliseconds video_probe_timeout{60000};

    std::chrono::milliseconds socket_wait_timeout{60000};
    std::chrono::milliseconds ready_wait_timeout{60000};
    std::chrono::milliseconds clean_exit_timeout{604800000};
    std::chrono::milliseconds term_grace_timeout{5000};
    std::chrono::milliseconds kill_reap_timeout{5000};
    std::chrono::milliseconds supervisor_poll_interval{20};

    // A single JSONL line and each retained latest snapshot are bounded by
    // this value.  The complete child transcript is drained but never kept.
    std::size_t max_stdout_line_bytes = 1024U * 1024U;

    // Optional deterministic host-test seam.  It is not needed by production
    // callers and cannot add arguments: argv is still generated by the
    // supervisor from authenticated values.
    SpatialRoiCameraRecorderProcessSpawnOverride spawn_override;
};

class SpatialRoiCameraRecorderProcess final {
public:
    static std::unique_ptr<SpatialRoiCameraRecorderProcess> Create(
        SpatialRoiCameraRecorderProcessConfig config,
        std::string* error_out = nullptr);

    ~SpatialRoiCameraRecorderProcess();

    SpatialRoiCameraRecorderProcess(
        const SpatialRoiCameraRecorderProcess&) = delete;
    SpatialRoiCameraRecorderProcess& operator=(
        const SpatialRoiCameraRecorderProcess&) = delete;
    SpatialRoiCameraRecorderProcess(
        SpatialRoiCameraRecorderProcess&&) = delete;
    SpatialRoiCameraRecorderProcess& operator=(
        SpatialRoiCameraRecorderProcess&&) = delete;

    // Authenticate the contract/plan/root/GPU mapping, then fork and exec
    // exactly one child.  No socket or full-frame lifecycle is touched here.
    bool Start(std::string* error_out = nullptr);

    // Socket-bound is a namespace observation only.  This method never
    // connects, unlinks, or changes any socket; it returns once all four exact
    // authenticated endpoint leaves are owner-only filesystem sockets.
    bool WaitForFourSockets(std::string* error_out = nullptr);

    // Requires the socket-bound phase.  A child JSON "ready" event is distinct
    // from socket creation because producers may connect during that gap.
    bool WaitUntilReady(std::string* error_out = nullptr);

    // Requires ready, and succeeds only after a terminal complete JSON event,
    // clean child EOF, and exit status zero.  Any timeout performs bounded
    // SIGTERM, SIGKILL escalation, and exact-PID reap.
    bool WaitForCleanExit(std::string* error_out = nullptr);

    // Idempotent bounded teardown.  It never unlinks runtime sockets and does
    // not claim to make a stuck CUDA/destructor call interruptible; the parent
    // process still needs its documented outer supervisor bound.
    bool Stop(std::string* error_out = nullptr);

    SpatialRoiCameraRecorderProcessState state() const noexcept
    {
        return status_.state;
    }
    const SpatialRoiCameraRecorderProcessStatus& status() const noexcept
    {
        return status_;
    }
    const std::vector<std::string>& child_argv() const noexcept
    {
        return child_argv_;
    }
    const std::vector<std::string>& socket_paths() const noexcept
    {
        return socket_paths_;
    }

private:
    explicit SpatialRoiCameraRecorderProcess(
        SpatialRoiCameraRecorderProcessConfig config);

    bool authenticate(std::string* error_out);
    bool build_child_argv(std::string* error_out);
    bool launch(std::string* error_out);
    bool open_runtime_directory(bool* pending, std::string* error_out);
    bool sockets_ready(bool* pending, std::string* error_out);
    bool pump_output(std::string* error_out);
    bool parse_line(const std::string& line, std::string* error_out);
    bool validate_storage_preflight_payload(
        const nlohmann::json& value,
        std::string* error_out) const;
    bool validate_scheduling_payload(
        const nlohmann::json& value,
        std::string* error_out) const;
    bool poll_child(std::string* error_out);
    bool wait_until(std::chrono::steady_clock::time_point deadline,
                    const char* phase,
                    const std::function<bool()>& predicate,
                    std::string* error_out);
    bool terminate_bounded(std::string* error_out);
    bool fail_and_stop(const std::string& reason, std::string* error_out);
    bool set_error(std::string* error_out, const std::string& message) const;

    SpatialRoiCameraRecorderProcessConfig config_;
    SpatialRoiCameraRecorderProcessStatus status_;
    std::vector<std::string> child_argv_;
    std::vector<std::string> socket_paths_;
    std::string runtime_directory_path_;
    std::string stdout_line_buffer_;
    int stdout_fd_ = -1;
    int runtime_directory_fd_ = -1;
    int executable_fd_ = -1;
    session::spatial_roi::SpatialRoiRecorderStoragePreflightPolicyView
        expected_storage_preflight_policy_;
    session::spatial_roi::SpatialRoiRecorderAggregateBoundsView
        expected_aggregate_bounds_;
    bool stdout_closed_ = false;
    bool stop_attempted_ = false;
};

}  // namespace orange::spatial_roi::recording
