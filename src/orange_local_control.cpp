#include "orange_local_control.h"

#include <sys/select.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include <cerrno>
#include <array>
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
           method == "citrus_completion";
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

}  // namespace

nlohmann::json LocalControlStatusSnapshotToJson(
    const LocalControlStatusSnapshot& snapshot)
{
    return {
        {"process", snapshot.process},
        {"updated_at_utc", snapshot.updated_at_utc},
        {"autorun_stage", snapshot.autorun_stage},
        {"readiness",
         {
             {"process_started", true},
             {"cameras_open", snapshot.cameras_open},
             {"streaming_active", snapshot.streaming_active},
             {"recording_active", snapshot.recording_active},
             {"recording_finalizing", snapshot.recording_finalizing},
             {"active_recorders", snapshot.active_recorders},
             {"selected_cameras_match_expected",
              snapshot.expected_camera_serials.empty()
                  ? nlohmann::json(nullptr)
                  : nlohmann::json(snapshot.open_camera_serials ==
                                   snapshot.expected_camera_serials)},
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
        parsed.method != "citrus_completion") {
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
    listen_fd_ = -1;
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        last_error_.clear();
        accepted_request_ids_.clear();
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
    if (!options_.socket_path.empty()) {
        unlink(options_.socket_path.c_str());
    }
    running_.store(false, std::memory_order_release);
}

void LocalControlServer::UpdateStatus(const LocalControlStatusSnapshot& snapshot)
{
    std::lock_guard<std::mutex> lock(status_mutex_);
    status_ = snapshot;
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

    unlink(options_.socket_path.c_str());
    const std::filesystem::path socket_parent =
        std::filesystem::path(options_.socket_path).parent_path();
    if (!socket_parent.empty()) {
        std::error_code ec;
        std::filesystem::create_directories(socket_parent, ec);
    }
    if (bind(listen_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        SetLastError("bind(" + options_.socket_path + ") failed: " +
                     std::string(std::strerror(errno)));
        close(listen_fd_);
        listen_fd_ = -1;
        running_.store(false, std::memory_order_release);
        return;
    }
    chmod(options_.socket_path.c_str(), static_cast<mode_t>(options_.socket_mode));

    if (listen(listen_fd_, 8) != 0) {
        SetLastError("listen(" + options_.socket_path + ") failed: " +
                     std::string(std::strerror(errno)));
        close(listen_fd_);
        listen_fd_ = -1;
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
    unlink(options_.socket_path.c_str());
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
        const ssize_t written = write(client_fd, data, remaining);
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

    bool duplicate = false;
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        const auto inserted = accepted_request_ids_.insert(parsed.request_id);
        duplicate = !inserted.second;
    }

    if (parsed.method == "citrus_completion") {
        nlohmann::json response = {
            {"schema_id", kLocalControlResponseSchemaId},
            {"schema_version", kLocalControlSchemaVersion},
            {"ok", true},
            {"accepted", true},
            {"duplicate", duplicate},
            {"diagnostic_only", true},
            {"request_id", parsed.request_id},
            {"operation_id", parsed.operation_id},
            {"method", parsed.method},
            {"responded_at_utc", utc_now()},
            {"status", LocalControlStatusSnapshotToJson(status_snapshot)},
            {"effect",
             {
                 {"recording_stop_requested", false},
                 {"recording_lifecycle_mutated", false},
             }},
        };
        LogEvent({{"received_at_utc", utc_now()},
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
        {"request_id", parsed.request_id},
        {"operation_id", parsed.operation_id},
        {"method", parsed.method},
        {"responded_at_utc", utc_now()},
        {"error",
         {
             {"code", "unsupported_in_diagnostic_mode"},
             {"message",
              "start_recording and stop_recording are contract-defined but not wired to Orange recording lifecycle yet"},
         }},
        {"status", LocalControlStatusSnapshotToJson(status_snapshot)},
    };
    LogEvent({{"received_at_utc", utc_now()},
              {"request", request},
              {"response", response}});
    return response;
}

void LocalControlServer::LogEvent(const nlohmann::json& event)
{
    if (options_.event_log_path.empty()) {
        return;
    }
    const std::filesystem::path log_path(options_.event_log_path);
    if (!log_path.parent_path().empty()) {
        std::error_code ec;
        std::filesystem::create_directories(log_path.parent_path(), ec);
    }
    std::ofstream out(options_.event_log_path, std::ios::app);
    if (!out) {
        SetLastError("failed to open local control event log: " + options_.event_log_path);
        return;
    }
    out << event.dump() << '\n';
}

}  // namespace orange::control
