#include "orange_local_control.h"
#include "projected_center_preflight.h"

#include <fcntl.h>
#include <sys/file.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include <cerrno>
#include <array>
#include <algorithm>
#include <chrono>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <system_error>
#include <thread>

namespace orange::control {
namespace {

std::mutex g_event_log_mutex;

std::string utc_now()
{
    const auto now = std::chrono::system_clock::now();
    const std::time_t now_time = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
    gmtime_r(&now_time, &tm);

    std::ostringstream out;
    out << std::put_time(&tm, "%Y-%m-%dT%H:%M:%SZ");
    return out.str();
}

nlohmann::json recorder_readiness_to_json(const RecorderReadinessSnapshot& snapshot)
{
    return {
        {"external_ipc_enabled", snapshot.external_ipc_enabled},
        {"lifecycle_started", snapshot.lifecycle_started},
        {"supervisors_ready", snapshot.supervisors_ready},
        {"process_count", snapshot.process_count},
        {"running_process_count", snapshot.running_process_count},
        {"socket_ready_count", snapshot.socket_ready_count},
    };
}

bool json_string_field(const nlohmann::json& object,
                       const char* name,
                       std::string* value_out,
                       std::string* error_out,
                       const bool required)
{
    const auto it = object.find(name);
    if (it == object.end()) {
        if (required) {
            if (error_out) {
                *error_out = std::string("missing required string field: ") + name;
            }
            return false;
        }
        if (value_out) {
            value_out->clear();
        }
        return true;
    }
    if (!it->is_string()) {
        if (error_out) {
            *error_out = std::string("field must be a string: ") + name;
        }
        return false;
    }
    const std::string value = it->get<std::string>();
    if (required && value.empty()) {
        if (error_out) {
            *error_out = std::string("field must not be empty: ") + name;
        }
        return false;
    }
    if (value_out) {
        *value_out = value;
    }
    return true;
}

bool is_mutating_method(const std::string& method)
{
    return method == "start_recording" ||
           method == "stop_recording" ||
           method == "citrus_completion" ||
           IsProjectedCenterPreflightMethod(method);
}

std::string request_string_or_empty(const nlohmann::json& request, const char* key)
{
    const auto it = request.find(key);
    if (it == request.end() || !it->is_string()) {
        return {};
    }
    return it->get<std::string>();
}

void set_error(std::string* error_out, const std::string& error)
{
    if (error_out) {
        *error_out = error;
    }
}

nlohmann::json matches_expected_or_null(const std::vector<std::string>& expected,
                                        const std::vector<std::string>& actual)
{
    if (expected.empty()) {
        return nullptr;
    }
    std::vector<std::string> sorted_expected = expected;
    std::vector<std::string> sorted_actual = actual;
    std::sort(sorted_expected.begin(), sorted_expected.end());
    std::sort(sorted_actual.begin(), sorted_actual.end());
    return sorted_expected == sorted_actual;
}

bool matches_expected_or_true_when_unset(const std::vector<std::string>& expected,
                                         const std::vector<std::string>& actual)
{
    if (expected.empty()) {
        return true;
    }
    return matches_expected_or_null(expected, actual).get<bool>();
}

std::string local_control_phase(const LocalControlStatusSnapshot& snapshot)
{
    if (snapshot.recording_finalizing) {
        return "recording_finalizing";
    }
    if (snapshot.recording_active) {
        return "recording";
    }
    if (snapshot.streaming_active) {
        return "streaming";
    }
    if (snapshot.cameras_open) {
        return "cameras_open";
    }
    return "idle";
}

bool recording_stop_has_drain_completed(
    const LocalControlRecordingStopSnapshot& snapshot)
{
    return !snapshot.drain_completed_at_utc.empty();
}

std::string recording_stop_state(
    const LocalControlRecordingStopSnapshot& snapshot)
{
    if (!snapshot.enabled) {
        return "disabled";
    }
    if (snapshot.drain_timed_out &&
        recording_stop_has_drain_completed(snapshot)) {
        return "finalized_after_drain_timeout";
    }
    if (snapshot.drain_timed_out) {
        return "drain_timeout";
    }
    if (recording_stop_has_drain_completed(snapshot)) {
        return "finalized";
    }
    if (snapshot.drain_active) {
        return "draining";
    }
    if (snapshot.stop_triggered) {
        return "stop_triggered";
    }
    if (snapshot.scheduled) {
        return "scheduled";
    }
    return "idle";
}

std::string recording_stop_health(
    const LocalControlRecordingStopSnapshot& snapshot)
{
    const std::string state = recording_stop_state(snapshot);
    if (state == "drain_timeout") {
        return "critical";
    }
    if (state == "finalized_after_drain_timeout") {
        return "warning";
    }
    return "ok";
}

std::string recording_stop_ack_state(
    const LocalControlRecordingStopSnapshot& snapshot)
{
    const std::string state = recording_stop_state(snapshot);
    if (state == "disabled") {
        return "disabled";
    }
    if (snapshot.drain_timed_out) {
        return "failed_timeout";
    }
    if (state == "finalized") {
        return "executed";
    }
    if (state == "draining" || state == "stop_triggered") {
        return "executing";
    }
    if (state == "scheduled") {
        return "accepted";
    }
    if (snapshot.last_event == "ignored_disabled" ||
        snapshot.last_event == "ignored_not_recording" ||
        snapshot.last_event == "cancelled_recording_inactive") {
        return "ignored";
    }
    return "idle";
}

nlohmann::json recording_stop_to_json(
    const LocalControlRecordingStopSnapshot& snapshot,
    const bool enabled)
{
    const std::string state = recording_stop_state(snapshot);
    return {
        {"enabled", enabled},
        {"state", state},
        {"health", recording_stop_health(snapshot)},
        {"ack_state", recording_stop_ack_state(snapshot)},
        {"error_code",
         (state == "drain_timeout" ||
          state == "finalized_after_drain_timeout")
             ? "drain_timeout"
             : ""},
        {"scheduled", snapshot.scheduled},
        {"stop_triggered", snapshot.stop_triggered},
        {"drain_active", snapshot.drain_active},
        {"drain_timed_out", snapshot.drain_timed_out},
        {"forced_finalize_requested", snapshot.forced_finalize_requested},
        {"forced_finalize_stream_stop_requested",
         snapshot.forced_finalize_stream_stop_requested},
        {"grace_seconds", snapshot.grace_seconds},
        {"seconds_until_deadline", snapshot.seconds_until_deadline},
        {"drain_timeout_seconds", snapshot.drain_timeout_seconds},
        {"drain_elapsed_seconds", snapshot.drain_elapsed_seconds},
        {"method", snapshot.method},
        {"request_id", snapshot.request_id},
        {"operation_id", snapshot.operation_id},
        {"source", snapshot.source},
        {"experiment_id", snapshot.experiment_id},
        {"terminal_state", snapshot.terminal_state},
        {"reason", snapshot.reason},
        {"received_at_utc", snapshot.received_at_utc},
        {"stop_triggered_at_utc", snapshot.stop_triggered_at_utc},
        {"drain_completed_at_utc", snapshot.drain_completed_at_utc},
        {"forced_finalize_requested_at_utc", snapshot.forced_finalize_requested_at_utc},
        {"last_event", snapshot.last_event},
        {"last_event_at_utc", snapshot.last_event_at_utc},
    };
}

nlohmann::json recording_start_to_json(
    const LocalControlRecordingStartSnapshot& snapshot)
{
    return {
        {"enabled", snapshot.enabled},
        {"pending", snapshot.pending},
        {"request_id", snapshot.request_id},
        {"operation_id", snapshot.operation_id},
        {"source", snapshot.source},
        {"reason", snapshot.reason},
        {"received_at_utc", snapshot.received_at_utc},
        {"last_event", snapshot.last_event},
        {"last_event_at_utc", snapshot.last_event_at_utc},
    };
}

std::string operation_dedupe_key(const ParsedLocalControlRequest& parsed)
{
    return parsed.method + "\n" + parsed.operation_id;
}

bool allow_gui_start_recording(const LocalControlServerOptions& options)
{
    return options.allow_gui_start_recording_commands;
}

bool allow_gui_stop_recording(const LocalControlServerOptions& options)
{
    return options.allow_gui_stop_recording_commands ||
           options.allow_gui_lifecycle_commands;
}

bool allow_gui_citrus_completion(const LocalControlServerOptions& options)
{
    return options.allow_gui_citrus_completion_commands ||
           options.allow_gui_stop_recording_commands ||
           options.allow_gui_lifecycle_commands;
}

bool prepare_event_log_directory(const std::filesystem::path& log_path,
                                 std::string* error_out)
{
    if (!log_path.parent_path().empty()) {
        std::error_code ec;
        std::filesystem::create_directories(log_path.parent_path(), ec);
        if (ec) {
            set_error(error_out,
                      "failed to create local control event log directory: " +
                          log_path.parent_path().string() + ": " + ec.message());
            return false;
        }
    }
    return true;
}

bool InitializeLocalControlEventLog(const std::string& event_log_path,
                                    std::string* error_out)
{
    if (event_log_path.empty()) {
        return true;
    }
    const std::lock_guard<std::mutex> lock(g_event_log_mutex);
    const std::filesystem::path log_path(event_log_path);
    if (!prepare_event_log_directory(log_path, error_out)) {
        return false;
    }
    std::ofstream out(event_log_path, std::ios::trunc);
    if (!out) {
        set_error(error_out,
                  "failed to initialize local control event log: " + event_log_path);
        return false;
    }
    return true;
}

std::string socket_lock_path_for_socket(const std::string& socket_path)
{
    return socket_path + ".lock";
}

bool acquire_socket_path_lock(const std::string& socket_path,
                              int* lock_fd_out,
                              std::string* lock_path_out,
                              std::string* error_out)
{
    if (!lock_fd_out) {
        set_error(error_out, "lock fd output pointer is null");
        return false;
    }
    *lock_fd_out = -1;
    const std::string lock_path = socket_lock_path_for_socket(socket_path);
    const int fd = open(lock_path.c_str(), O_CREAT | O_RDWR | O_CLOEXEC, 0666);
    if (fd < 0) {
        set_error(error_out,
                  "failed to open local control socket lock " + lock_path + ": " +
                      std::strerror(errno));
        return false;
    }
    if (flock(fd, LOCK_EX | LOCK_NB) != 0) {
        const int lock_errno = errno;
        close(fd);
        if (lock_errno == EWOULDBLOCK || lock_errno == EAGAIN) {
            set_error(error_out,
                      "local control socket path is already owned by another process: " +
                          socket_path);
        } else {
            set_error(error_out,
                      "failed to lock local control socket path " + socket_path + ": " +
                          std::strerror(lock_errno));
        }
        return false;
    }
    *lock_fd_out = fd;
    if (lock_path_out) {
        *lock_path_out = lock_path;
    }
    return true;
}

bool unix_socket_path_is_registered(const std::string& socket_path)
{
    std::ifstream in("/proc/net/unix");
    if (!in) {
        return false;
    }
    std::string line;
    while (std::getline(in, line)) {
        const std::size_t last_space = line.find_last_of(" \t");
        if (last_space == std::string::npos || last_space + 1 >= line.size()) {
            continue;
        }
        if (line.substr(last_space + 1) == socket_path) {
            return true;
        }
    }
    return false;
}

bool prepare_socket_path_for_bind(const std::string& socket_path,
                                  std::string* error_out)
{
    const std::filesystem::path path(socket_path);
    std::error_code exists_error;
    if (!std::filesystem::exists(path, exists_error)) {
        if (exists_error) {
            set_error(error_out,
                      "failed to stat local control socket path " + socket_path + ": " +
                          exists_error.message());
            return false;
        }
        return true;
    }
    std::error_code status_error;
    const std::filesystem::file_status status =
        std::filesystem::symlink_status(path, status_error);
    if (status_error) {
        set_error(error_out,
                  "failed to stat local control socket path " + socket_path + ": " +
                      status_error.message());
        return false;
    }
    if (status.type() != std::filesystem::file_type::socket) {
        set_error(error_out,
                  "local control socket path exists and is not a socket: " + socket_path);
        return false;
    }
    if (unix_socket_path_is_registered(socket_path)) {
        set_error(error_out,
                  "local control socket path is already registered by another process: " +
                      socket_path);
        return false;
    }

    std::error_code unlink_error;
    std::filesystem::remove(path, unlink_error);
    if (unlink_error) {
        set_error(error_out,
                  "failed to remove stale local control socket path " + socket_path + ": " +
                      unlink_error.message());
        return false;
    }
    return true;
}

bool recorder_ready_when_required(const RecorderReadinessSnapshot& snapshot,
                                  const bool recorder_required)
{
    return !recorder_required ||
           !snapshot.external_ipc_enabled ||
           snapshot.supervisors_ready;
}

}  // namespace

bool AppendLocalControlEventLog(const std::string& event_log_path,
                                const nlohmann::json& event,
                                std::string* error_out)
{
    if (event_log_path.empty()) {
        return true;
    }
    const std::lock_guard<std::mutex> lock(g_event_log_mutex);
    const std::filesystem::path log_path(event_log_path);
    if (!prepare_event_log_directory(log_path, error_out)) {
        return false;
    }
    std::ofstream out(event_log_path, std::ios::app);
    if (!out) {
        set_error(error_out,
                  "failed to open local control event log: " + event_log_path);
        return false;
    }
    out << event.dump() << '\n';
    return true;
}

nlohmann::json LocalControlStatusSnapshotToJson(
    const LocalControlStatusSnapshot& snapshot)
{
    const nlohmann::json open_match =
        matches_expected_or_null(snapshot.expected_camera_serials, snapshot.open_camera_serials);
    const nlohmann::json stream_match =
        matches_expected_or_null(
            snapshot.expected_camera_serials,
            snapshot.stream_selected_camera_serials);
    const nlohmann::json record_match =
        matches_expected_or_null(
            snapshot.expected_camera_serials,
            snapshot.record_selected_camera_serials);
    const nlohmann::json yolo_match =
        matches_expected_or_null(
            snapshot.expected_camera_serials,
            snapshot.yolo_selected_camera_serials);
    const nlohmann::json crop_match =
        matches_expected_or_null(
            snapshot.expected_camera_serials,
            snapshot.crop_selected_camera_serials);
    const bool expected_open =
        matches_expected_or_true_when_unset(
            snapshot.expected_camera_serials,
            snapshot.open_camera_serials);
    const bool expected_stream_selected =
        matches_expected_or_true_when_unset(
            snapshot.expected_camera_serials,
            snapshot.stream_selected_camera_serials);
    const bool expected_record_selected =
        matches_expected_or_true_when_unset(
            snapshot.expected_camera_serials,
            snapshot.record_selected_camera_serials);
    const bool expected_yolo_selected =
        matches_expected_or_true_when_unset(
            snapshot.expected_camera_serials,
            snapshot.yolo_selected_camera_serials);
    const bool expected_crop_selected =
        snapshot.crop_selected_camera_serials.empty()
            ? true
            : matches_expected_or_true_when_unset(
                  snapshot.expected_camera_serials,
                  snapshot.crop_selected_camera_serials);
    const bool selections_match_expected =
        expected_stream_selected &&
        expected_record_selected &&
        expected_yolo_selected &&
        expected_crop_selected;
    const bool full_frame_recorder_required =
        !snapshot.record_selected_camera_serials.empty();
    const bool crop_recorder_required =
        !snapshot.crop_selected_camera_serials.empty();
    const bool full_frame_recorders_ready =
        recorder_ready_when_required(
            snapshot.full_frame_recorder,
            full_frame_recorder_required);
    const bool crop_recorders_ready =
        recorder_ready_when_required(
            snapshot.crop_recorder,
            crop_recorder_required);
    const bool ready_for_recording_request =
        snapshot.cameras_open &&
        snapshot.streaming_active &&
        !snapshot.recording_active &&
        !snapshot.recording_finalizing &&
        expected_open &&
        selections_match_expected;
    const bool ready_for_citrus_experiment =
        snapshot.cameras_open &&
        snapshot.streaming_active &&
        snapshot.recording_active &&
        !snapshot.recording_finalizing &&
        expected_open &&
        selections_match_expected &&
        full_frame_recorders_ready &&
        crop_recorders_ready;
    const bool recording_finalized =
        snapshot.recording_finalized ||
        (!snapshot.recording_active &&
         !snapshot.recording_finalizing &&
         !snapshot.recording_folder.empty());

    return {
        {"process", snapshot.process},
        {"updated_at_utc", snapshot.updated_at_utc},
        {"autorun_stage", snapshot.autorun_stage},
        {"phase", local_control_phase(snapshot)},
        {"readiness",
         {
             {"process_started", true},
             {"cameras_open", snapshot.cameras_open},
             {"streaming_active", snapshot.streaming_active},
             {"recording_active", snapshot.recording_active},
             {"recording_finalizing", snapshot.recording_finalizing},
             {"recording_finalized", recording_finalized},
             {"active_recorders", snapshot.active_recorders},
             {"open_cameras_match_expected", open_match},
             {"stream_selection_matches_expected", stream_match},
             {"record_selection_matches_expected", record_match},
             {"yolo_selection_matches_expected", yolo_match},
             {"crop_selection_matches_expected", crop_match},
             {"selected_cameras_match_expected",
              snapshot.expected_camera_serials.empty()
                  ? nlohmann::json(nullptr)
                  : nlohmann::json(
                        expected_open &&
                        selections_match_expected)},
             {"ready_for_recording_request", ready_for_recording_request},
             {"ready_for_citrus_experiment", ready_for_citrus_experiment},
             {"full_frame_external_recorders_ready",
              snapshot.full_frame_recorder.supervisors_ready},
             {"crop_external_recorders_ready",
              snapshot.crop_recorder.supervisors_ready},
         }},
        {"recording",
         {
             {"folder", snapshot.recording_folder},
             {"sink_mode", snapshot.recording_sink_mode},
         }},
        {"cameras",
         {
             {"expected_serials", snapshot.expected_camera_serials},
             {"open_serials", snapshot.open_camera_serials},
             {"stream_selected_serials", snapshot.stream_selected_camera_serials},
             {"record_selected_serials", snapshot.record_selected_camera_serials},
             {"yolo_selected_serials", snapshot.yolo_selected_camera_serials},
             {"crop_selected_serials", snapshot.crop_selected_camera_serials},
         }},
        {"external_recorders",
         {
             {"full_frame", recorder_readiness_to_json(snapshot.full_frame_recorder)},
             {"crop", recorder_readiness_to_json(snapshot.crop_recorder)},
         }},
        {"local_control",
         {
             {"recording_start",
              recording_start_to_json(snapshot.local_control_recording_start)},
             {"recording_stop",
              recording_stop_to_json(
                  snapshot.local_control_recording_stop,
                  snapshot.local_control_recording_stop.enabled)},
             {"citrus_completion_stop",
              recording_stop_to_json(
                  snapshot.local_control_recording_stop,
                  snapshot.local_control_recording_stop.citrus_completion_enabled)},
         }},
    };
}

bool ParseLocalControlRequest(const nlohmann::json& request,
                              ParsedLocalControlRequest* parsed_out,
                              std::string* error_out)
{
    if (!request.is_object()) {
        set_error(error_out, "request must be a JSON object");
        return false;
    }
    const auto schema_it = request.find("schema_id");
    if (schema_it == request.end() ||
        !schema_it->is_string() ||
        schema_it->get<std::string>() != kLocalControlRequestSchemaId) {
        set_error(error_out, "schema_id must be orange.local_control.request");
        return false;
    }
    const auto version_it = request.find("schema_version");
    if (version_it == request.end() ||
        !version_it->is_number_integer() ||
        version_it->get<int>() != kLocalControlSchemaVersion) {
        set_error(error_out, "schema_version must be 1");
        return false;
    }

    ParsedLocalControlRequest parsed;
    if (!json_string_field(request, "method", &parsed.method, error_out, true) ||
        !json_string_field(request, "request_id", &parsed.request_id, error_out, true) ||
        !json_string_field(request, "source", &parsed.source, error_out, false)) {
        return false;
    }

    if (parsed.method != "status" &&
        parsed.method != "start_recording" &&
        parsed.method != "stop_recording" &&
        parsed.method != "citrus_completion" &&
        !IsProjectedCenterPreflightMethod(parsed.method)) {
        set_error(error_out, "unsupported method: " + parsed.method);
        return false;
    }

    const bool operation_required = is_mutating_method(parsed.method);
    if (!json_string_field(
            request,
            "operation_id",
            &parsed.operation_id,
            error_out,
            operation_required)) {
        return false;
    }

    const auto params_it = request.find("params");
    if (params_it == request.end()) {
        parsed.params = nlohmann::json::object();
    } else if (!params_it->is_object()) {
        set_error(error_out, "params must be a JSON object");
        return false;
    } else {
        parsed.params = *params_it;
    }

    if (parsed.method == "citrus_completion") {
        const auto experiment_it = parsed.params.find("experiment_id");
        if (experiment_it == parsed.params.end() ||
            !experiment_it->is_string() ||
            experiment_it->get<std::string>().empty()) {
            set_error(error_out, "citrus_completion params.experiment_id is required");
            return false;
        }
    }

    if (parsed_out) {
        *parsed_out = std::move(parsed);
    }
    return true;
}

nlohmann::json BuildLocalControlErrorResponse(const nlohmann::json& request,
                                              const std::string& code,
                                              const std::string& message)
{
    return {
        {"schema_id", kLocalControlResponseSchemaId},
        {"schema_version", kLocalControlSchemaVersion},
        {"ok", false},
        {"accepted", false},
        {"request_id", request_string_or_empty(request, "request_id")},
        {"operation_id", request_string_or_empty(request, "operation_id")},
        {"method", request_string_or_empty(request, "method")},
        {"responded_at_utc", utc_now()},
        {"error",
         {
             {"code", code},
             {"message", message},
         }},
    };
}

LocalControlServer::~LocalControlServer()
{
    Stop();
}

bool LocalControlServer::Start(const LocalControlServerOptions& options,
                               std::string* error_out)
{
    if (running()) {
        set_error(error_out, "local control server is already running");
        return false;
    }
    if (options.socket_path.empty()) {
        set_error(error_out, "local control socket path is empty");
        return false;
    }
    if (options.socket_path.size() >= sizeof(sockaddr_un{}.sun_path)) {
        set_error(error_out, "local control socket path is too long");
        return false;
    }

    options_ = options;
    stop_requested_.store(false, std::memory_order_release);
    owns_socket_path_.store(false, std::memory_order_release);
    owns_socket_path_lock_.store(false, std::memory_order_release);
    listen_fd_ = -1;
    socket_lock_fd_ = -1;
    socket_lock_path_.clear();
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        last_error_.clear();
        accepted_request_ids_.clear();
        accepted_operation_keys_.clear();
        pending_commands_.clear();
    }
    thread_ = std::thread(&LocalControlServer::ServeLoop, this);
    const auto start = std::chrono::steady_clock::now();
    while (!running()) {
        const std::string error = last_error();
        if (!error.empty()) {
            Stop();
            set_error(error_out, error);
            return false;
        }
        const auto elapsed = std::chrono::steady_clock::now() - start;
        if (elapsed > std::chrono::seconds(1)) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return true;
}

void LocalControlServer::Stop()
{
    stop_requested_.store(true, std::memory_order_release);
    if (thread_.joinable()) {
        thread_.join();
    }
    if (owns_socket_path_.load(std::memory_order_acquire) &&
        !options_.socket_path.empty()) {
        unlink(options_.socket_path.c_str());
        owns_socket_path_.store(false, std::memory_order_release);
    }
    ReleaseSocketPathLock();
    running_.store(false, std::memory_order_release);
}

void LocalControlServer::UpdateStatus(const LocalControlStatusSnapshot& snapshot)
{
    std::lock_guard<std::mutex> lock(status_mutex_);
    status_ = snapshot;
}

std::vector<PendingLocalControlCommand> LocalControlServer::DrainPendingCommands()
{
    std::vector<PendingLocalControlCommand> pending;
    std::lock_guard<std::mutex> lock(state_mutex_);
    pending.swap(pending_commands_);
    return pending;
}

std::string LocalControlServer::last_error() const
{
    std::lock_guard<std::mutex> lock(state_mutex_);
    return last_error_;
}

void LocalControlServer::SetLastError(const std::string& error)
{
    std::lock_guard<std::mutex> lock(state_mutex_);
    last_error_ = error;
}

void LocalControlServer::ReleaseSocketPathLock()
{
    if (socket_lock_fd_ >= 0) {
        flock(socket_lock_fd_, LOCK_UN);
        close(socket_lock_fd_);
        socket_lock_fd_ = -1;
    }
    owns_socket_path_lock_.store(false, std::memory_order_release);
    socket_lock_path_.clear();
}

void LocalControlServer::ServeLoop()
{
    listen_fd_ = socket(AF_UNIX, SOCK_STREAM, 0);
    if (listen_fd_ < 0) {
        SetLastError("socket() failed: " + std::string(std::strerror(errno)));
        running_.store(false, std::memory_order_release);
        return;
    }

    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    std::strncpy(addr.sun_path, options_.socket_path.c_str(), sizeof(addr.sun_path) - 1);

    const std::filesystem::path socket_parent =
        std::filesystem::path(options_.socket_path).parent_path();
    if (!socket_parent.empty()) {
        std::error_code ec;
        std::filesystem::create_directories(socket_parent, ec);
        if (ec) {
            SetLastError(
                "failed to create local control socket directory " +
                socket_parent.string() + ": " + ec.message());
            close(listen_fd_);
            listen_fd_ = -1;
            running_.store(false, std::memory_order_release);
            return;
        }
    }
    std::string lock_path_error;
    if (!acquire_socket_path_lock(
            options_.socket_path,
            &socket_lock_fd_,
            &socket_lock_path_,
            &lock_path_error)) {
        SetLastError(lock_path_error);
        close(listen_fd_);
        listen_fd_ = -1;
        running_.store(false, std::memory_order_release);
        return;
    }
    owns_socket_path_lock_.store(true, std::memory_order_release);

    std::string socket_path_error;
    if (!prepare_socket_path_for_bind(options_.socket_path, &socket_path_error)) {
        SetLastError(socket_path_error);
        close(listen_fd_);
        listen_fd_ = -1;
        ReleaseSocketPathLock();
        running_.store(false, std::memory_order_release);
        return;
    }
    if (bind(listen_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        SetLastError("bind(" + options_.socket_path + ") failed: " +
                     std::string(std::strerror(errno)));
        close(listen_fd_);
        listen_fd_ = -1;
        ReleaseSocketPathLock();
        running_.store(false, std::memory_order_release);
        return;
    }
    owns_socket_path_.store(true, std::memory_order_release);
    chmod(options_.socket_path.c_str(), static_cast<mode_t>(options_.socket_mode));

    if (listen(listen_fd_, 8) != 0) {
        SetLastError("listen(" + options_.socket_path + ") failed: " +
                     std::string(std::strerror(errno)));
        close(listen_fd_);
        listen_fd_ = -1;
        running_.store(false, std::memory_order_release);
        if (owns_socket_path_.load(std::memory_order_acquire)) {
            unlink(options_.socket_path.c_str());
            owns_socket_path_.store(false, std::memory_order_release);
        }
        ReleaseSocketPathLock();
        return;
    }

    std::string event_log_error;
    if (!InitializeLocalControlEventLog(options_.event_log_path, &event_log_error)) {
        SetLastError(event_log_error);
        close(listen_fd_);
        listen_fd_ = -1;
        if (owns_socket_path_.load(std::memory_order_acquire)) {
            unlink(options_.socket_path.c_str());
            owns_socket_path_.store(false, std::memory_order_release);
        }
        ReleaseSocketPathLock();
        running_.store(false, std::memory_order_release);
        return;
    }

    running_.store(true, std::memory_order_release);
    std::cout << "[GUI][local_control] listening socket=" << options_.socket_path;
    if (!options_.event_log_path.empty()) {
        std::cout << " event_log=" << options_.event_log_path;
    }
    std::cout << std::endl;

    while (!stop_requested_.load(std::memory_order_acquire)) {
        fd_set read_fds;
        FD_ZERO(&read_fds);
        FD_SET(listen_fd_, &read_fds);
        timeval timeout{};
        timeout.tv_sec = 0;
        timeout.tv_usec = 100000;
        const int ready = select(listen_fd_ + 1, &read_fds, nullptr, nullptr, &timeout);
        if (ready < 0) {
            if (errno == EINTR) {
                continue;
            }
            if (!stop_requested_.load(std::memory_order_acquire)) {
                SetLastError("select() failed: " + std::string(std::strerror(errno)));
            }
            break;
        }
        if (ready == 0 || !FD_ISSET(listen_fd_, &read_fds)) {
            continue;
        }
        const int client_fd = accept(listen_fd_, nullptr, nullptr);
        if (client_fd < 0) {
            if (errno == EINTR || stop_requested_.load(std::memory_order_acquire)) {
                continue;
            }
            SetLastError("accept() failed: " + std::string(std::strerror(errno)));
            continue;
        }
        HandleClient(client_fd);
        close(client_fd);
    }

    if (listen_fd_ >= 0) {
        close(listen_fd_);
        listen_fd_ = -1;
    }
    if (owns_socket_path_.load(std::memory_order_acquire)) {
        unlink(options_.socket_path.c_str());
        owns_socket_path_.store(false, std::memory_order_release);
    }
    ReleaseSocketPathLock();
    running_.store(false, std::memory_order_release);
}

void LocalControlServer::HandleClient(const int client_fd)
{
    std::string request_text;
    std::array<char, 4096> buffer{};
    while (request_text.size() < options_.max_request_bytes) {
        const ssize_t bytes = read(client_fd, buffer.data(), buffer.size());
        if (bytes < 0) {
            if (errno == EINTR) {
                continue;
            }
            return;
        }
        if (bytes == 0) {
            break;
        }
        request_text.append(buffer.data(), static_cast<std::size_t>(bytes));
        if (request_text.find('\n') != std::string::npos) {
            break;
        }
    }

    nlohmann::json response;
    try {
        nlohmann::json request = nlohmann::json::parse(request_text);
        response = HandleRequest(request);
    } catch (const std::exception& exc) {
        response = {
            {"schema_id", kLocalControlResponseSchemaId},
            {"schema_version", kLocalControlSchemaVersion},
            {"ok", false},
            {"accepted", false},
            {"responded_at_utc", utc_now()},
            {"error",
             {
                 {"code", "invalid_json"},
                 {"message", exc.what()},
             }},
        };
    }

    const std::string rendered = response.dump() + "\n";
    const char* data = rendered.data();
    std::size_t remaining = rendered.size();
    while (remaining > 0) {
        const ssize_t written = send(client_fd, data, remaining, MSG_NOSIGNAL);
        if (written < 0) {
            if (errno == EINTR) {
                continue;
            }
            break;
        }
        data += written;
        remaining -= static_cast<std::size_t>(written);
    }
}

nlohmann::json LocalControlServer::HandleRequest(const nlohmann::json& request)
{
    ParsedLocalControlRequest parsed;
    std::string error;
    if (!ParseLocalControlRequest(request, &parsed, &error)) {
        return BuildLocalControlErrorResponse(request, "invalid_request", error);
    }

    LocalControlStatusSnapshot status_snapshot;
    {
        std::lock_guard<std::mutex> lock(status_mutex_);
        status_snapshot = status_;
    }

    if (parsed.method == "status") {
        return {
            {"schema_id", kLocalControlResponseSchemaId},
            {"schema_version", kLocalControlSchemaVersion},
            {"ok", true},
            {"accepted", true},
            {"request_id", parsed.request_id},
            {"operation_id", parsed.operation_id},
            {"method", parsed.method},
            {"responded_at_utc", utc_now()},
            {"status", LocalControlStatusSnapshotToJson(status_snapshot)},
        };
    }

    const std::string received_at_utc = utc_now();
    bool duplicate = false;
    bool queued_for_gui_thread = false;
    const bool start_recording_allowed = allow_gui_start_recording(options_);
    const bool stop_recording_allowed = allow_gui_stop_recording(options_);
    const bool citrus_completion_allowed =
        allow_gui_citrus_completion(options_);
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        const auto inserted = accepted_request_ids_.insert(parsed.request_id);
        const bool duplicate_request_id = !inserted.second;
        bool duplicate_operation_id = false;
        if (!duplicate_request_id && is_mutating_method(parsed.method)) {
            const auto operation_inserted =
                accepted_operation_keys_.insert(operation_dedupe_key(parsed));
            duplicate_operation_id = !operation_inserted.second;
        }
        duplicate = duplicate_request_id || duplicate_operation_id;
        const bool supports_gui_thread_queue =
            parsed.method == "citrus_completion" ||
            (parsed.method == "start_recording" &&
             start_recording_allowed) ||
            (parsed.method == "stop_recording" &&
             stop_recording_allowed);
        if (supports_gui_thread_queue && !duplicate) {
            PendingLocalControlCommand command;
            command.method = parsed.method;
            command.request_id = parsed.request_id;
            command.operation_id = parsed.operation_id;
            command.source = parsed.source;
            command.received_at_utc = received_at_utc;
            command.params = parsed.params;
            pending_commands_.push_back(std::move(command));
            queued_for_gui_thread = true;
        }
    }

    if (IsProjectedCenterPreflightMethod(parsed.method)) {
        const ProjectedCenterPreflightResult preflight =
            BuildProjectedCenterVerificationPreflight(
                status_snapshot,
                parsed.params,
                received_at_utc);
        nlohmann::json response = {
            {"schema_id", kLocalControlResponseSchemaId},
            {"schema_version", kLocalControlSchemaVersion},
            {"ok", true},
            {"accepted", preflight.passed},
            {"duplicate", duplicate},
            {"diagnostic_only", true},
            {"mutation_allowed", false},
            {"queued_for_gui_thread", false},
            {"request_id", parsed.request_id},
            {"operation_id", parsed.operation_id},
            {"method", parsed.method},
            {"responded_at_utc", utc_now()},
            {"status", LocalControlStatusSnapshotToJson(status_snapshot)},
            {"effect",
             {
                 {"preflight_projected_center_verification", preflight.effect},
             }},
        };
        LogEvent({{"received_at_utc", received_at_utc},
                  {"request", request},
                  {"response", response}});
        return response;
    }

    if (parsed.method == "citrus_completion") {
        nlohmann::json response = {
            {"schema_id", kLocalControlResponseSchemaId},
            {"schema_version", kLocalControlSchemaVersion},
            {"ok", true},
            {"accepted", true},
            {"duplicate", duplicate},
            {"diagnostic_only", !citrus_completion_allowed},
            {"queued_for_gui_thread", queued_for_gui_thread},
            {"request_id", parsed.request_id},
            {"operation_id", parsed.operation_id},
            {"method", parsed.method},
            {"responded_at_utc", utc_now()},
            {"status", LocalControlStatusSnapshotToJson(status_snapshot)},
            {"effect",
             {
                 {"gui_lifecycle_command_deferred", queued_for_gui_thread},
                 {"recording_stop_requested", false},
                 {"recording_lifecycle_mutated", false},
             }},
        };
        LogEvent({{"received_at_utc", received_at_utc},
                  {"request", request},
                  {"response", response}});
        return response;
    }

    if (parsed.method == "stop_recording" &&
        stop_recording_allowed) {
        nlohmann::json response = {
            {"schema_id", kLocalControlResponseSchemaId},
            {"schema_version", kLocalControlSchemaVersion},
            {"ok", true},
            {"accepted", true},
            {"duplicate", duplicate},
            {"diagnostic_only", false},
            {"queued_for_gui_thread", queued_for_gui_thread},
            {"request_id", parsed.request_id},
            {"operation_id", parsed.operation_id},
            {"method", parsed.method},
            {"responded_at_utc", utc_now()},
            {"status", LocalControlStatusSnapshotToJson(status_snapshot)},
            {"effect",
             {
                 {"gui_lifecycle_command_deferred", queued_for_gui_thread},
                 {"recording_stop_requested", false},
                 {"recording_lifecycle_mutated", false},
             }},
        };
        LogEvent({{"received_at_utc", received_at_utc},
                  {"request", request},
                  {"response", response}});
        return response;
    }

    if (parsed.method == "start_recording" &&
        start_recording_allowed) {
        nlohmann::json response = {
            {"schema_id", kLocalControlResponseSchemaId},
            {"schema_version", kLocalControlSchemaVersion},
            {"ok", true},
            {"accepted", true},
            {"duplicate", duplicate},
            {"diagnostic_only", false},
            {"queued_for_gui_thread", queued_for_gui_thread},
            {"request_id", parsed.request_id},
            {"operation_id", parsed.operation_id},
            {"method", parsed.method},
            {"responded_at_utc", utc_now()},
            {"status", LocalControlStatusSnapshotToJson(status_snapshot)},
            {"effect",
             {
                 {"gui_lifecycle_command_deferred", queued_for_gui_thread},
                 {"recording_start_requested", false},
                 {"recording_lifecycle_mutated", false},
             }},
        };
        LogEvent({{"received_at_utc", received_at_utc},
                  {"request", request},
                  {"response", response}});
        return response;
    }

    nlohmann::json response = {
        {"schema_id", kLocalControlResponseSchemaId},
        {"schema_version", kLocalControlSchemaVersion},
        {"ok", false},
        {"accepted", false},
        {"duplicate", duplicate},
        {"diagnostic_only", true},
        {"queued_for_gui_thread", false},
        {"request_id", parsed.request_id},
        {"operation_id", parsed.operation_id},
        {"method", parsed.method},
        {"responded_at_utc", utc_now()},
        {"error",
         {
             {"code", "unsupported_in_diagnostic_mode"},
             {"message",
              parsed.method == "stop_recording"
                  ? "stop_recording requires ORANGE_GUI_LOCAL_CONTROL_ENABLE_RECORDING_STOP=1"
                  : "start_recording requires ORANGE_GUI_LOCAL_CONTROL_ENABLE_RECORDING_START=1"},
         }},
        {"status", LocalControlStatusSnapshotToJson(status_snapshot)},
    };
    LogEvent({{"received_at_utc", received_at_utc},
              {"request", request},
              {"response", response}});
    return response;
}

void LocalControlServer::LogEvent(const nlohmann::json& event)
{
    std::string error;
    if (!AppendLocalControlEventLog(options_.event_log_path, event, &error)) {
        SetLastError(error);
    }
}

}  // namespace orange::control
