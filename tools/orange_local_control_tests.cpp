#include "orange_local_control.h"

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <array>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

using orange::control::LocalControlServer;
using orange::control::LocalControlServerOptions;
using orange::control::LocalControlStatusSnapshot;
using orange::control::ParseLocalControlRequest;
using orange::control::PendingLocalControlCommand;
using orange::control::RecorderReadinessSnapshot;

void require(const bool condition, const std::string& message)
{
    if (!condition) {
        throw std::runtime_error(message);
    }
}

std::filesystem::path temp_path(const std::string& name)
{
    return std::filesystem::path("/tmp") /
           ("orange_local_control_test_" + std::to_string(getpid()) + "_" + name);
}

nlohmann::json request_json(const std::string& method,
                            const std::string& request_id,
                            const std::string& operation_id = {},
                            nlohmann::json params = nlohmann::json::object())
{
    nlohmann::json request = {
        {"schema_id", orange::control::kLocalControlRequestSchemaId},
        {"schema_version", orange::control::kLocalControlSchemaVersion},
        {"method", method},
        {"request_id", request_id},
        {"source", "test"},
        {"params", std::move(params)},
    };
    if (!operation_id.empty()) {
        request["operation_id"] = operation_id;
    }
    return request;
}

nlohmann::json send_request(const std::filesystem::path& socket_path,
                            const nlohmann::json& request)
{
    const int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    require(fd >= 0, "socket() failed");

    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    const std::string path = socket_path.string();
    require(path.size() < sizeof(addr.sun_path), "test socket path too long");
    std::strncpy(addr.sun_path, path.c_str(), sizeof(addr.sun_path) - 1);

    const int deadline_ms = 2000;
    const auto start = std::chrono::steady_clock::now();
    while (connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                    std::chrono::steady_clock::now() - start)
                                    .count();
        if (elapsed_ms > deadline_ms) {
            close(fd);
            throw std::runtime_error("connect() timed out");
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    const std::string rendered = request.dump() + "\n";
    require(write(fd, rendered.data(), rendered.size()) == static_cast<ssize_t>(rendered.size()),
            "write() failed");
    shutdown(fd, SHUT_WR);

    std::string response_text;
    std::array<char, 4096> buffer{};
    for (;;) {
        const ssize_t bytes = read(fd, buffer.data(), buffer.size());
        if (bytes < 0) {
            close(fd);
            throw std::runtime_error("read() failed");
        }
        if (bytes == 0) {
            break;
        }
        response_text.append(buffer.data(), static_cast<std::size_t>(bytes));
    }
    close(fd);
    return nlohmann::json::parse(response_text);
}

LocalControlStatusSnapshot healthy_status()
{
    LocalControlStatusSnapshot status;
    status.updated_at_utc = "2026-05-28T00:00:00Z";
    status.autorun_stage = "recording";
    status.cameras_open = true;
    status.streaming_active = true;
    status.recording_active = true;
    status.recording_finalizing = false;
    status.active_recorders = 4;
    status.recording_folder = "/tmp/orange_control_test_recording";
    status.recording_sink_mode = "external_ipc";
    status.expected_camera_serials = {"2010093", "2010094"};
    status.open_camera_serials = {"2010093", "2010094"};
    status.stream_selected_camera_serials = {"2010093", "2010094"};
    status.record_selected_camera_serials = {"2010093", "2010094"};
    status.yolo_selected_camera_serials = {"2010093", "2010094"};
    status.crop_selected_camera_serials = {"2010093", "2010094"};
    status.full_frame_recorder = RecorderReadinessSnapshot{
        true,
        true,
        true,
        2,
        2,
        2,
    };
    status.crop_recorder = RecorderReadinessSnapshot{
        true,
        true,
        true,
        2,
        2,
        2,
    };
    return status;
}

void wait_until_running(LocalControlServer* server)
{
    const auto start = std::chrono::steady_clock::now();
    while (!server->running()) {
        const std::string error = server->last_error();
        require(error.empty(), "server failed before listening: " + error);
        const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                    std::chrono::steady_clock::now() - start)
                                    .count();
        require(elapsed_ms < 2000, "server did not report running");
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}

void require_start_server(LocalControlServer* server,
                          const std::filesystem::path& socket_path,
                          const std::filesystem::path& log_path = {})
{
    std::string error;
    const bool started = server->Start({socket_path.string(), log_path.string()}, &error);
    if (!started && error.empty()) {
        error = server->last_error();
    }
    require(
        started,
        "server start failed for socket " + socket_path.string() + ": " + error);
    wait_until_running(server);
}

void test_parse_requires_operation_for_mutating_methods()
{
    std::string error;
    require(!ParseLocalControlRequest(
                request_json("citrus_completion", "req-1", "", {{"experiment_id", "exp-1"}}),
                nullptr,
                &error),
            "citrus_completion without operation_id should fail");
    require(error.find("operation_id") != std::string::npos,
            "failure should mention operation_id");

    require(ParseLocalControlRequest(
                request_json("status", "req-status"),
                nullptr,
                &error),
            "status without operation_id should parse");
}

void test_status_request_returns_readiness_snapshot()
{
    const auto socket_path = temp_path("status.sock");
    const auto log_path = temp_path("status.jsonl");
    std::filesystem::remove(socket_path);
    std::filesystem::remove(log_path);

    LocalControlServer server;
    require_start_server(&server, socket_path, log_path);
    server.UpdateStatus(healthy_status());

    const nlohmann::json response =
        send_request(socket_path, request_json("status", "status-req-1"));
    server.Stop();

    require(response["ok"].get<bool>(), "status response should be ok");
    require(response["accepted"].get<bool>(), "status response should be accepted");
    require(response["status"]["readiness"]["cameras_open"].get<bool>(),
            "status should report cameras open");
    require(response["status"]["readiness"]["selected_cameras_match_expected"].get<bool>(),
            "status should report selected cameras match expected");
    require(response["status"]["phase"].get<std::string>() == "recording",
            "status should expose derived recording phase");
    require(!response["status"]["readiness"]["ready_for_recording_request"].get<bool>(),
            "active recording should not be ready for another start request");
    require(response["status"]["readiness"]["ready_for_citrus_experiment"].get<bool>(),
            "active recording should be ready for Citrus experiment start");
    require(response["status"]["external_recorders"]["full_frame"]["supervisors_ready"].get<bool>(),
            "status should report full-frame recorder ready");
    require(!response["status"]["local_control"]["citrus_completion_stop"]["enabled"].get<bool>(),
            "status should report Citrus completion stop scheduler disabled by default");
    require(!response["status"]["local_control"]["recording_start"]["enabled"].get<bool>(),
            "status should report recording start request disabled by default");
    require(!response["status"]["local_control"]["recording_stop"]["enabled"].get<bool>(),
            "status should report recording stop scheduler disabled by default");
}

void test_status_readiness_matches_expected_camera_sets_without_order_sensitivity()
{
    LocalControlStatusSnapshot status = healthy_status();
    status.recording_active = false;
    status.active_recorders = 0;
    status.open_camera_serials = {"2010094", "2010093"};
    status.stream_selected_camera_serials = {"2010094", "2010093"};
    status.record_selected_camera_serials = {"2010094", "2010093"};
    status.yolo_selected_camera_serials = {"2010094", "2010093"};
    status.crop_selected_camera_serials.clear();
    const nlohmann::json json = orange::control::LocalControlStatusSnapshotToJson(status);

    require(json["phase"].get<std::string>() == "streaming", "phase should be streaming");
    require(json["readiness"]["open_cameras_match_expected"].get<bool>(),
            "open camera match should ignore order");
    require(json["readiness"]["selected_cameras_match_expected"].get<bool>(),
            "selection match should ignore order and allow unrequested crop");
    require(json["readiness"]["ready_for_recording_request"].get<bool>(),
            "streaming configured state should be ready for recording request");
    require(!json["readiness"]["ready_for_citrus_experiment"].get<bool>(),
            "Citrus should not start before Orange recording is active");

    status.record_selected_camera_serials = {"2010093"};
    const nlohmann::json mismatch =
        orange::control::LocalControlStatusSnapshotToJson(status);
    require(!mismatch["readiness"]["record_selection_matches_expected"].get<bool>(),
            "record selection mismatch should be explicit");
    require(!mismatch["readiness"]["ready_for_recording_request"].get<bool>(),
            "recording request should not be ready when record selection mismatches expected cameras");
}

void test_status_reports_completed_recording_after_streaming_stop_path()
{
    LocalControlStatusSnapshot status = healthy_status();
    status.autorun_stage = "done";
    status.recording_active = false;
    status.recording_finalizing = false;
    status.recording_finalized = true;
    status.active_recorders = 0;
    status.recording_folder = "/tmp/orange_control_test_finalized";

    const nlohmann::json json = orange::control::LocalControlStatusSnapshotToJson(status);
    require(json["phase"].get<std::string>() == "streaming",
            "completed local-control recording should not force stream shutdown");
    require(json["readiness"]["recording_finalized"].get<bool>(),
            "completed recording should remain visible after finalization");
    require(json["recording"]["folder"].get<std::string>() == status.recording_folder,
            "completed recording folder should remain visible for orchestrator validation");
}

void test_citrus_completion_is_diagnostic_ack_and_logged()
{
    const auto socket_path = temp_path("completion.sock");
    const auto log_path = temp_path("completion.jsonl");
    std::filesystem::remove(socket_path);
    std::filesystem::remove(log_path);

    LocalControlServer server;
    require_start_server(&server, socket_path, log_path);
    server.UpdateStatus(healthy_status());

    const nlohmann::json request = request_json(
        "citrus_completion",
        "completion-req-1",
        "experiment-42",
        {{"experiment_id", "citrus-exp-42"},
         {"terminal_state", "completed"},
         {"grace_seconds", 10}});
    const nlohmann::json first = send_request(socket_path, request);
    const nlohmann::json duplicate = send_request(socket_path, request);
    const nlohmann::json same_operation_duplicate = send_request(
        socket_path,
        request_json(
            "citrus_completion",
            "completion-req-2",
            "experiment-42",
            {{"experiment_id", "citrus-exp-42"},
             {"terminal_state", "completed"},
             {"grace_seconds", 5}}));
    const std::vector<PendingLocalControlCommand> pending =
        server.DrainPendingCommands();
    const std::vector<PendingLocalControlCommand> second_drain =
        server.DrainPendingCommands();
    server.Stop();

    require(first["ok"].get<bool>(), "completion response should be ok");
    require(first["accepted"].get<bool>(), "completion response should be accepted");
    require(first["diagnostic_only"].get<bool>(), "completion should be diagnostic-only");
    require(first["queued_for_gui_thread"].get<bool>(),
            "first completion request should queue for GUI-thread handling");
    require(!first["effect"]["recording_lifecycle_mutated"].get<bool>(),
            "completion must not mutate recording lifecycle");
    require(duplicate["duplicate"].get<bool>(), "second completion request should be duplicate");
    require(!duplicate["queued_for_gui_thread"].get<bool>(),
            "duplicate completion request should not queue again");
    require(same_operation_duplicate["duplicate"].get<bool>(),
            "same method and operation_id should be duplicate with a new request_id");
    require(!same_operation_duplicate["queued_for_gui_thread"].get<bool>(),
            "same-operation duplicate should not queue again");
    require(pending.size() == 1, "completion should queue exactly one pending command");
    require(pending[0].method == "citrus_completion",
            "pending command should preserve method");
    require(pending[0].request_id == "completion-req-1",
            "pending command should preserve request_id");
    require(pending[0].operation_id == "experiment-42",
            "pending command should preserve operation_id");
    require(pending[0].params["experiment_id"].get<std::string>() == "citrus-exp-42",
            "pending command should preserve params");
    require(!pending[0].received_at_utc.empty(),
            "pending command should capture receive timestamp");
    require(second_drain.empty(), "pending command drain should be empty after first drain");

    const std::string log_text = std::filesystem::exists(log_path)
                                     ? std::filesystem::path(log_path).string()
                                     : "";
    require(!log_text.empty(), "event log path should exist");
    std::ifstream log_stream(log_path);
    const std::string contents{
        std::istreambuf_iterator<char>(log_stream),
        std::istreambuf_iterator<char>()};
    require(contents.find("citrus_completion") != std::string::npos,
            "event log should contain completion method");
}

void test_citrus_completion_reports_deferred_lifecycle_mode_when_enabled()
{
    const auto socket_path = temp_path("completion_enabled.sock");
    std::filesystem::remove(socket_path);

    LocalControlServer server;
    LocalControlServerOptions options;
    options.socket_path = socket_path.string();
    options.allow_gui_lifecycle_commands = true;
    std::string error;
    require(server.Start(options, &error), "server start should allow lifecycle command mode");
    wait_until_running(&server);
    server.UpdateStatus(healthy_status());

    const nlohmann::json response = send_request(
        socket_path,
        request_json(
            "citrus_completion",
            "completion-enabled-req-1",
            "experiment-enabled-42",
            {{"experiment_id", "citrus-exp-42"},
             {"terminal_state", "completed"},
             {"grace_seconds", 0}}));
    server.Stop();

    require(response["ok"].get<bool>(), "completion response should be ok");
    require(!response["diagnostic_only"].get<bool>(),
            "completion response should report non-diagnostic mode when lifecycle commands are enabled");
    require(response["queued_for_gui_thread"].get<bool>(),
            "completion response should still report GUI-thread queueing");
    require(response["effect"]["gui_lifecycle_command_deferred"].get<bool>(),
            "completion response should report deferred GUI lifecycle action");
    require(!response["effect"]["recording_lifecycle_mutated"].get<bool>(),
            "socket thread must still not mutate recording lifecycle");
}

void test_stop_recording_queues_when_lifecycle_mode_enabled()
{
    const auto socket_path = temp_path("stop_enabled.sock");
    std::filesystem::remove(socket_path);

    LocalControlServer server;
    LocalControlServerOptions options;
    options.socket_path = socket_path.string();
    options.allow_gui_lifecycle_commands = true;
    std::string error;
    require(server.Start(options, &error), "server start should allow stop_recording mode");
    wait_until_running(&server);
    server.UpdateStatus(healthy_status());

    const nlohmann::json request =
        request_json("stop_recording", "stop-enabled-req-1", "stop-op-1",
                     {{"reason", "orchestrator_stop"},
                      {"grace_seconds", 0}});
    const nlohmann::json first = send_request(socket_path, request);
    const nlohmann::json duplicate = send_request(socket_path, request);
    const std::vector<PendingLocalControlCommand> pending =
        server.DrainPendingCommands();
    server.Stop();

    require(first["ok"].get<bool>(), "stop_recording response should be ok when enabled");
    require(first["accepted"].get<bool>(),
            "stop_recording response should be accepted when enabled");
    require(!first["diagnostic_only"].get<bool>(),
            "stop_recording response should report non-diagnostic mode when enabled");
    require(first["queued_for_gui_thread"].get<bool>(),
            "stop_recording should queue for GUI-thread handling when enabled");
    require(!first["effect"]["recording_lifecycle_mutated"].get<bool>(),
            "socket thread must not mutate recording lifecycle for stop_recording");
    require(duplicate["duplicate"].get<bool>(),
            "duplicate stop_recording request should be duplicate");
    require(!duplicate["queued_for_gui_thread"].get<bool>(),
            "duplicate stop_recording request should not queue again");
    require(pending.size() == 1, "stop_recording should queue exactly one pending command");
    require(pending[0].method == "stop_recording",
            "pending stop command should preserve method");
    require(pending[0].request_id == "stop-enabled-req-1",
            "pending stop command should preserve request_id");
    require(pending[0].operation_id == "stop-op-1",
            "pending stop command should preserve operation_id");
    require(pending[0].params["reason"].get<std::string>() == "orchestrator_stop",
            "pending stop command should preserve params");
}

void test_start_recording_queues_when_start_mode_enabled()
{
    const auto socket_path = temp_path("start_enabled.sock");
    std::filesystem::remove(socket_path);

    LocalControlServer server;
    LocalControlServerOptions options;
    options.socket_path = socket_path.string();
    options.allow_gui_start_recording_commands = true;
    std::string error;
    require(server.Start(options, &error), "server start should allow start_recording mode");
    wait_until_running(&server);
    LocalControlStatusSnapshot status = healthy_status();
    status.recording_active = false;
    status.active_recorders = 0;
    status.local_control_recording_start.enabled = true;
    server.UpdateStatus(status);

    const nlohmann::json request =
        request_json("start_recording", "start-enabled-req-1", "start-op-1",
                     {{"reason", "orchestrator_start"}});
    const nlohmann::json first = send_request(socket_path, request);
    const nlohmann::json duplicate = send_request(socket_path, request);
    const std::vector<PendingLocalControlCommand> pending =
        server.DrainPendingCommands();
    server.Stop();

    require(first["ok"].get<bool>(), "start_recording response should be ok when enabled");
    require(first["accepted"].get<bool>(),
            "start_recording response should be accepted when enabled");
    require(!first["diagnostic_only"].get<bool>(),
            "start_recording response should report non-diagnostic mode when enabled");
    require(first["queued_for_gui_thread"].get<bool>(),
            "start_recording should queue for GUI-thread handling when enabled");
    require(!first["effect"]["recording_lifecycle_mutated"].get<bool>(),
            "socket thread must not mutate recording lifecycle for start_recording");
    require(!first["effect"]["recording_start_requested"].get<bool>(),
            "socket thread should not directly request recording start");
    require(first["status"]["local_control"]["recording_start"]["enabled"].get<bool>(),
            "status should expose recording start control enabled");
    require(duplicate["duplicate"].get<bool>(),
            "duplicate start_recording request should be duplicate");
    require(!duplicate["queued_for_gui_thread"].get<bool>(),
            "duplicate start_recording request should not queue again");
    require(pending.size() == 1, "start_recording should queue exactly one pending command");
    require(pending[0].method == "start_recording",
            "pending start command should preserve method");
    require(pending[0].request_id == "start-enabled-req-1",
            "pending start command should preserve request_id");
    require(pending[0].operation_id == "start-op-1",
            "pending start command should preserve operation_id");
    require(pending[0].params["reason"].get<std::string>() == "orchestrator_start",
            "pending start command should preserve params");
}

void test_start_stop_are_rejected_in_diagnostic_mode()
{
    const auto socket_path = temp_path("stop.sock");
    std::filesystem::remove(socket_path);

    LocalControlServer server;
    require_start_server(&server, socket_path);
    server.UpdateStatus(healthy_status());

    const nlohmann::json response = send_request(
        socket_path,
        request_json("stop_recording", "stop-req-1", "stop-op-1"));
    server.Stop();

    require(!response["ok"].get<bool>(), "stop_recording should not be ok yet");
    require(!response["accepted"].get<bool>(), "stop_recording should not be accepted yet");
    require(
        response["error"]["code"].get<std::string>() == "unsupported_in_diagnostic_mode",
        "stop_recording should report diagnostic-mode unsupported");

    LocalControlServer enabled_server;
    LocalControlServerOptions options;
    const auto start_socket_path = temp_path("start_enabled.sock");
    std::filesystem::remove(start_socket_path);
    options.socket_path = start_socket_path.string();
    options.allow_gui_lifecycle_commands = true;
    std::string error;
    require(enabled_server.Start(options, &error), "server start should allow lifecycle mode");
    wait_until_running(&enabled_server);
    enabled_server.UpdateStatus(healthy_status());

    const nlohmann::json start_response = send_request(
        start_socket_path,
        request_json("start_recording", "start-req-1", "start-op-1"));
    enabled_server.Stop();

    require(!start_response["ok"].get<bool>(),
            "start_recording should not be ok without the start-specific gate");
    require(!start_response["accepted"].get<bool>(),
            "start_recording should not be accepted without the start-specific gate");
}

}  // namespace

int main()
{
    const std::vector<std::pair<const char*, void (*)()>> tests = {
        {"parse_requires_operation_for_mutating_methods",
         test_parse_requires_operation_for_mutating_methods},
        {"status_request_returns_readiness_snapshot",
         test_status_request_returns_readiness_snapshot},
        {"status_readiness_matches_expected_camera_sets_without_order_sensitivity",
         test_status_readiness_matches_expected_camera_sets_without_order_sensitivity},
        {"status_reports_completed_recording_after_streaming_stop_path",
         test_status_reports_completed_recording_after_streaming_stop_path},
        {"citrus_completion_is_diagnostic_ack_and_logged",
         test_citrus_completion_is_diagnostic_ack_and_logged},
        {"citrus_completion_reports_deferred_lifecycle_mode_when_enabled",
         test_citrus_completion_reports_deferred_lifecycle_mode_when_enabled},
        {"stop_recording_queues_when_lifecycle_mode_enabled",
         test_stop_recording_queues_when_lifecycle_mode_enabled},
        {"start_recording_queues_when_start_mode_enabled",
         test_start_recording_queues_when_start_mode_enabled},
        {"start_stop_are_rejected_in_diagnostic_mode",
         test_start_stop_are_rejected_in_diagnostic_mode},
    };
    for (const auto& test : tests) {
        test.second();
        std::cout << "[PASS] " << test.first << std::endl;
    }
    std::cout << "orange_local_control_tests passed" << std::endl;
    return 0;
}
