#pragma once

#include "json.hpp"

#include <atomic>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

namespace orange::control {

inline constexpr const char* kLocalControlRequestSchemaId =
    "orange.local_control.request";
inline constexpr const char* kLocalControlResponseSchemaId =
    "orange.local_control.response";
inline constexpr int kLocalControlSchemaVersion = 1;

struct RecorderReadinessSnapshot {
    bool external_ipc_enabled = false;
    bool lifecycle_started = false;
    bool supervisors_ready = false;
    int process_count = 0;
    int running_process_count = 0;
    int socket_ready_count = 0;
};

struct LocalControlRecordingStopSnapshot {
    bool enabled = false;
    bool citrus_completion_enabled = false;
    bool scheduled = false;
    bool stop_triggered = false;
    bool drain_active = false;
    bool drain_timed_out = false;
    bool forced_finalize_requested = false;
    bool forced_finalize_stream_stop_requested = false;
    double grace_seconds = 0.0;
    double seconds_until_deadline = 0.0;
    double drain_timeout_seconds = 0.0;
    double drain_elapsed_seconds = 0.0;
    std::string method;
    std::string request_id;
    std::string operation_id;
    std::string source;
    std::string experiment_id;
    std::string terminal_state;
    std::string reason;
    std::string received_at_utc;
    std::string stop_triggered_at_utc;
    std::string drain_completed_at_utc;
    std::string forced_finalize_requested_at_utc;
    std::string last_event;
    std::string last_event_at_utc;
};

struct LocalControlRecordingStartSnapshot {
    bool enabled = false;
    bool pending = false;
    std::string request_id;
    std::string operation_id;
    std::string source;
    std::string reason;
    std::string received_at_utc;
    std::string last_event;
    std::string last_event_at_utc;
};

struct LocalControlStatusSnapshot {
    std::string process = "orange_gui";
    std::string updated_at_utc;
    std::string autorun_stage;
    bool cameras_open = false;
    bool streaming_active = false;
    bool recording_active = false;
    bool recording_finalizing = false;
    bool recording_finalized = false;
    int active_recorders = 0;
    std::string recording_folder;
    std::string recording_sink_mode = "real";
    std::vector<std::string> expected_camera_serials;
    std::vector<std::string> open_camera_serials;
    std::vector<std::string> stream_selected_camera_serials;
    std::vector<std::string> record_selected_camera_serials;
    std::vector<std::string> yolo_selected_camera_serials;
    std::vector<std::string> crop_selected_camera_serials;
    RecorderReadinessSnapshot full_frame_recorder;
    RecorderReadinessSnapshot crop_recorder;
    LocalControlRecordingStartSnapshot local_control_recording_start;
    LocalControlRecordingStopSnapshot local_control_recording_stop;
};

struct LocalControlServerOptions {
    std::string socket_path;
    std::string event_log_path;
    int socket_mode = 0666;
    std::size_t max_request_bytes = 64 * 1024;
    std::size_t max_dedupe_entries = 4096;
    bool allow_gui_lifecycle_commands = false;
    bool allow_gui_start_recording_commands = false;
    bool allow_gui_stop_recording_commands = false;
    bool allow_gui_citrus_completion_commands = false;
};

struct ParsedLocalControlRequest {
    std::string method;
    std::string request_id;
    std::string operation_id;
    std::string source;
    // Optional epoch/seq fence fields. Zero means "not present" (legacy
    // request); when present they must be positive integers.
    std::uint64_t epoch = 0;
    std::uint64_t seq = 0;
    nlohmann::json params = nlohmann::json::object();
};

struct PendingLocalControlCommand {
    std::string method;
    std::string request_id;
    std::string operation_id;
    std::string source;
    std::string received_at_utc;
    // Zero means the request carried no epoch/seq (legacy mode).
    std::uint64_t epoch = 0;
    std::uint64_t seq = 0;
    nlohmann::json params = nlohmann::json::object();
};

nlohmann::json LocalControlStatusSnapshotToJson(
    const LocalControlStatusSnapshot& snapshot);

bool ParseLocalControlRequest(const nlohmann::json& request,
                              ParsedLocalControlRequest* parsed_out,
                              std::string* error_out);

nlohmann::json BuildLocalControlErrorResponse(const nlohmann::json& request,
                                              const std::string& code,
                                              const std::string& message);

bool AppendLocalControlEventLog(const std::string& event_log_path,
                                const nlohmann::json& event,
                                std::string* error_out = nullptr);

class LocalControlServer {
public:
    LocalControlServer() = default;
    ~LocalControlServer();

    LocalControlServer(const LocalControlServer&) = delete;
    LocalControlServer& operator=(const LocalControlServer&) = delete;

    bool Start(const LocalControlServerOptions& options, std::string* error_out = nullptr);
    void Stop();
    bool running() const { return running_.load(std::memory_order_acquire); }

    void UpdateStatus(const LocalControlStatusSnapshot& snapshot);
    std::vector<PendingLocalControlCommand> DrainPendingCommands();
    std::string last_error() const;

    // Recording-generation fence. The GUI thread advances the session epoch
    // whenever a new recording generation starts; commands stamped with an
    // older epoch are rejected before they can be queued.
    void AdvanceSessionEpoch();
    // Marks a fenced command as completed. Ignored for stale epochs and for
    // legacy commands that carried no epoch/seq (epoch == 0 or seq == 0).
    void MarkCommandDone(std::uint64_t epoch, std::uint64_t seq);
    std::uint64_t session_epoch() const;
    std::uint64_t last_done_seq() const;

private:
    void ServeLoop();
    void HandleClient(int client_fd);
    nlohmann::json HandleRequest(const nlohmann::json& request);
    void AppendEpochTelemetry(nlohmann::json* response,
                              const ParsedLocalControlRequest& parsed) const;
    void LogEvent(const nlohmann::json& event);
    void SetLastError(const std::string& error);
    void ReleaseSocketPathLock();

    LocalControlServerOptions options_;
    mutable std::mutex status_mutex_;
    LocalControlStatusSnapshot status_;
    mutable std::mutex state_mutex_;
    std::unordered_set<std::string> accepted_request_ids_;
    std::unordered_set<std::string> accepted_operation_keys_;
    std::deque<std::string> accepted_request_id_order_;
    std::deque<std::string> accepted_operation_key_order_;
    std::uint64_t session_epoch_ = 1;
    std::uint64_t last_done_seq_ = 0;
    std::vector<PendingLocalControlCommand> pending_commands_;
    std::string last_error_;
    std::thread thread_;
    std::atomic<bool> running_{false};
    std::atomic<bool> stop_requested_{false};
    std::atomic<bool> owns_socket_path_{false};
    std::atomic<bool> owns_socket_path_lock_{false};
    int listen_fd_ = -1;
    int socket_lock_fd_ = -1;
    std::string socket_lock_path_;
};

}  // namespace orange::control
