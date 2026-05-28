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
using orange::control::RecorderReadinessSnapshot;

void require(const bool condition, const std::string& message)
{
    if (!condition) {
        throw std::runtime_error(message);
    }
}

std::filesystem::path temp_path(const std::string& name)
{
    return std::filesystem::temp_directory_path() /
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
    std::string error;
    require(server.Start({socket_path.string(), log_path.string()}, &error),
            "server start failed: " + error);
    wait_until_running(&server);
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
    require(response["status"]["external_recorders"]["full_frame"]["supervisors_ready"].get<bool>(),
            "status should report full-frame recorder ready");
}

void test_citrus_completion_is_diagnostic_ack_and_logged()
{
    const auto socket_path = temp_path("completion.sock");
    const auto log_path = temp_path("completion.jsonl");
    std::filesystem::remove(socket_path);
    std::filesystem::remove(log_path);

    LocalControlServer server;
    std::string error;
    require(server.Start({socket_path.string(), log_path.string()}, &error),
            "server start failed: " + error);
    wait_until_running(&server);
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
    server.Stop();

    require(first["ok"].get<bool>(), "completion response should be ok");
    require(first["accepted"].get<bool>(), "completion response should be accepted");
    require(first["diagnostic_only"].get<bool>(), "completion should be diagnostic-only");
    require(!first["effect"]["recording_lifecycle_mutated"].get<bool>(),
            "completion must not mutate recording lifecycle");
    require(duplicate["duplicate"].get<bool>(), "second completion request should be duplicate");

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

void test_start_stop_are_not_implemented_in_diagnostic_mode()
{
    const auto socket_path = temp_path("stop.sock");
    std::filesystem::remove(socket_path);

    LocalControlServer server;
    std::string error;
    require(server.Start({socket_path.string(), ""}, &error),
            "server start failed: " + error);
    wait_until_running(&server);
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
}

}  // namespace

int main()
{
    const std::vector<std::pair<const char*, void (*)()>> tests = {
        {"parse_requires_operation_for_mutating_methods",
         test_parse_requires_operation_for_mutating_methods},
        {"status_request_returns_readiness_snapshot",
         test_status_request_returns_readiness_snapshot},
        {"citrus_completion_is_diagnostic_ack_and_logged",
         test_citrus_completion_is_diagnostic_ack_and_logged},
        {"start_stop_are_not_implemented_in_diagnostic_mode",
         test_start_stop_are_not_implemented_in_diagnostic_mode},
    };
    for (const auto& test : tests) {
        test.second();
        std::cout << "[PASS] " << test.first << std::endl;
    }
    std::cout << "orange_local_control_tests passed" << std::endl;
    return 0;
}
