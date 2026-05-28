#pragma once

#include "json.hpp"

#include <atomic>
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

struct LocalControlStatusSnapshot {
    std::string process = "orange_gui";
    std::string updated_at_utc;
    std::string autorun_stage;
    bool cameras_open = false;
    bool streaming_active = false;
    bool recording_active = false;
    bool recording_finalizing = false;
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
};

struct LocalControlServerOptions {
    std::string socket_path;
    std::string event_log_path;
    int socket_mode = 0666;
    std::size_t max_request_bytes = 64 * 1024;
};

struct ParsedLocalControlRequest {
    std::string method;
    std::string request_id;
    std::string operation_id;
    std::string source;
    nlohmann::json params = nlohmann::json::object();
};

struct PendingLocalControlCommand {
    std::string method;
    std::string request_id;
    std::string operation_id;
    std::string source;
    std::string received_at_utc;
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

private:
    void ServeLoop();
    void HandleClient(int client_fd);
    nlohmann::json HandleRequest(const nlohmann::json& request);
    void LogEvent(const nlohmann::json& event);
    void SetLastError(const std::string& error);

    LocalControlServerOptions options_;
    mutable std::mutex status_mutex_;
    LocalControlStatusSnapshot status_;
    mutable std::mutex state_mutex_;
    std::unordered_set<std::string> accepted_request_ids_;
    std::vector<PendingLocalControlCommand> pending_commands_;
    std::string last_error_;
    std::thread thread_;
    std::atomic<bool> running_{false};
    std::atomic<bool> stop_requested_{false};
    int listen_fd_ = -1;
};

}  // namespace orange::control
