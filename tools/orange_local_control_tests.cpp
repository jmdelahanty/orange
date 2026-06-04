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
using orange::control::AppendLocalControlEventLog;

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

LocalControlStatusSnapshot preflight_ready_status()
{
    LocalControlStatusSnapshot status = healthy_status();
    status.autorun_stage = "streaming";
    status.recording_active = false;
    status.recording_finalizing = false;
    status.recording_finalized = false;
    status.active_recorders = 0;
    return status;
}

nlohmann::json projected_center_preflight_params(
    const std::string& camera_serial = "2010096")
{
    return {
        {"camera_serial", camera_serial},
        {"capture_mode", "visible_projected_crosshair_verification"},
        {"projector_visible_to_camera", true},
        {"exposure_us_requested", 150000.0},
        {"exposure_us_readback", 150000.0},
        {"frame_rate_hz_requested", 2.0},
        {"frame_rate_hz_readback", 2.0},
        {"filter_state", "operator_confirmed_removed"},
    };
}

const nlohmann::json& preflight_effect(const nlohmann::json& response)
{
    return response["effect"]["preflight_projected_center_verification"];
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

void test_citrus_readiness_requires_external_recorders_when_enabled()
{
    LocalControlStatusSnapshot status = healthy_status();
    status.full_frame_recorder.supervisors_ready = false;
    nlohmann::json json = orange::control::LocalControlStatusSnapshotToJson(status);

    require(!json["readiness"]["ready_for_citrus_experiment"].get<bool>(),
            "Citrus should not start until full-frame external recorders are ready");
    require(!json["readiness"]["full_frame_external_recorders_ready"].get<bool>(),
            "full-frame recorder readiness should remain explicit");

    status.full_frame_recorder.supervisors_ready = true;
    status.crop_recorder.supervisors_ready = false;
    json = orange::control::LocalControlStatusSnapshotToJson(status);
    require(!json["readiness"]["ready_for_citrus_experiment"].get<bool>(),
            "Citrus should not start until crop external recorders are ready");
    require(!json["readiness"]["crop_external_recorders_ready"].get<bool>(),
            "crop recorder readiness should remain explicit");

    status.crop_selected_camera_serials.clear();
    json = orange::control::LocalControlStatusSnapshotToJson(status);
    require(json["readiness"]["ready_for_citrus_experiment"].get<bool>(),
            "unselected crop recording should not block Citrus readiness");
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

void test_status_reports_separate_stop_and_citrus_completion_gates()
{
    LocalControlStatusSnapshot status = healthy_status();
    status.local_control_recording_stop.enabled = false;
    status.local_control_recording_stop.citrus_completion_enabled = true;

    const nlohmann::json json =
        orange::control::LocalControlStatusSnapshotToJson(status);
    require(
        !json["local_control"]["recording_stop"]["enabled"].get<bool>(),
        "generic stop_recording should stay disabled");
    require(
        json["local_control"]["citrus_completion_stop"]["enabled"].get<bool>(),
        "Citrus completion stop should be independently enabled");
}

void test_status_reports_local_control_drain_timeout_telemetry()
{
    LocalControlStatusSnapshot status = healthy_status();
    status.local_control_recording_stop.enabled = true;
    status.local_control_recording_stop.citrus_completion_enabled = true;
    status.local_control_recording_stop.stop_triggered = true;
    status.local_control_recording_stop.drain_active = true;
    status.local_control_recording_stop.drain_timed_out = true;
    status.local_control_recording_stop.forced_finalize_requested = true;
    status.local_control_recording_stop.forced_finalize_stream_stop_requested = true;
    status.local_control_recording_stop.drain_timeout_seconds = 60.0;
    status.local_control_recording_stop.drain_elapsed_seconds = 61.25;
    status.local_control_recording_stop.method = "stop_recording";
    status.local_control_recording_stop.request_id = "stop-req-1";
    status.local_control_recording_stop.operation_id = "stop-op-1";
    status.local_control_recording_stop.reason = "orchestrator_stop";
    status.local_control_recording_stop.stop_triggered_at_utc =
        "2026-05-29T00:00:01Z";
    status.local_control_recording_stop.drain_completed_at_utc.clear();
    status.local_control_recording_stop.forced_finalize_requested_at_utc =
        "2026-05-29T00:01:02Z";
    status.local_control_recording_stop.last_event = "drain_timeout";
    status.local_control_recording_stop.last_event_at_utc =
        "2026-05-29T00:01:02Z";

    const nlohmann::json json =
        orange::control::LocalControlStatusSnapshotToJson(status);
    const auto& stop = json["local_control"]["recording_stop"];
    require(stop["enabled"].get<bool>(), "recording stop control should be enabled");
    require(stop["state"].get<std::string>() == "drain_timeout",
            "active timed-out drain should report drain_timeout state");
    require(stop["health"].get<std::string>() == "critical",
            "active timed-out drain should report critical health");
    require(stop["ack_state"].get<std::string>() == "failed_timeout",
            "active timed-out drain should report failed-timeout ACK state");
    require(stop["error_code"].get<std::string>() == "drain_timeout",
            "timed-out drain should report drain_timeout error code");
    require(stop["stop_triggered"].get<bool>(), "stop should be marked triggered");
    require(stop["drain_active"].get<bool>(), "drain should be marked active");
    require(stop["drain_timed_out"].get<bool>(), "drain timeout should be visible");
    require(stop["forced_finalize_requested"].get<bool>(),
            "timed-out drain should report forced finalize request");
    require(stop["forced_finalize_stream_stop_requested"].get<bool>(),
            "timed-out drain should report forced stream-stop request");
    require(stop["drain_timeout_seconds"].get<double>() == 60.0,
            "drain timeout threshold should be visible");
    require(stop["drain_elapsed_seconds"].get<double>() == 61.25,
            "drain elapsed seconds should be visible");
    require(stop["stop_triggered_at_utc"].get<std::string>() ==
                "2026-05-29T00:00:01Z",
            "stop trigger timestamp should be visible");
    require(stop["drain_completed_at_utc"].get<std::string>().empty(),
            "unfinished drain should have no completion timestamp");
    require(stop["forced_finalize_requested_at_utc"].get<std::string>() ==
                "2026-05-29T00:01:02Z",
            "forced finalize request timestamp should be visible");
    require(stop["last_event"].get<std::string>() == "drain_timeout",
            "last scheduler event should report drain timeout");

    const auto& completion_alias =
        json["local_control"]["citrus_completion_stop"];
    require(completion_alias["drain_timed_out"].get<bool>(),
            "compatibility alias should expose drain timeout");
    require(completion_alias["forced_finalize_requested"].get<bool>(),
            "compatibility alias should expose forced finalize request");
    require(completion_alias["forced_finalize_stream_stop_requested"].get<bool>(),
            "compatibility alias should expose forced stream-stop request");
    require(completion_alias["health"].get<std::string>() == "critical",
            "compatibility alias should expose critical health");
    require(completion_alias["ack_state"].get<std::string>() == "failed_timeout",
            "compatibility alias should expose failed-timeout ACK state");
    require(completion_alias["request_id"].get<std::string>() == "stop-req-1",
            "compatibility alias should mirror stop request id");
}

void test_status_reports_finalized_after_drain_timeout_as_warning()
{
    LocalControlStatusSnapshot status = healthy_status();
    status.local_control_recording_stop.enabled = true;
    status.local_control_recording_stop.citrus_completion_enabled = true;
    status.local_control_recording_stop.stop_triggered = true;
    status.local_control_recording_stop.drain_active = false;
    status.local_control_recording_stop.drain_timed_out = true;
    status.local_control_recording_stop.forced_finalize_requested = true;
    status.local_control_recording_stop.forced_finalize_stream_stop_requested = true;
    status.local_control_recording_stop.drain_timeout_seconds = 60.0;
    status.local_control_recording_stop.drain_elapsed_seconds = 62.0;
    status.local_control_recording_stop.method = "stop_recording";
    status.local_control_recording_stop.request_id = "stop-req-2";
    status.local_control_recording_stop.operation_id = "stop-op-2";
    status.local_control_recording_stop.stop_triggered_at_utc =
        "2026-05-29T00:00:01Z";
    status.local_control_recording_stop.drain_completed_at_utc =
        "2026-05-29T00:01:03Z";
    status.local_control_recording_stop.forced_finalize_requested_at_utc =
        "2026-05-29T00:01:02Z";
    status.local_control_recording_stop.last_event =
        "finalized_after_drain_timeout";

    const nlohmann::json json =
        orange::control::LocalControlStatusSnapshotToJson(status);
    const auto& stop = json["local_control"]["recording_stop"];
    require(stop["state"].get<std::string>() == "finalized_after_drain_timeout",
            "completed timed-out drain should report finalized-after-timeout state");
    require(stop["health"].get<std::string>() == "warning",
            "completed timed-out drain should report warning health");
    require(stop["ack_state"].get<std::string>() == "failed_timeout",
            "completed timed-out drain should report failed-timeout ACK state");
    require(stop["error_code"].get<std::string>() == "drain_timeout",
            "completed timed-out drain should retain drain_timeout error code");
    require(stop["forced_finalize_requested"].get<bool>(),
            "completed timed-out drain should report forced finalize request");
    require(stop["forced_finalize_stream_stop_requested"].get<bool>(),
            "completed timed-out drain should report forced stream-stop request");
    require(stop["forced_finalize_requested_at_utc"].get<std::string>() ==
                "2026-05-29T00:01:02Z",
            "completed timed-out drain should retain forced finalize timestamp");
}

void test_status_reports_recording_stop_ack_states()
{
    LocalControlStatusSnapshot status = healthy_status();
    status.local_control_recording_stop.enabled = true;

    nlohmann::json json = orange::control::LocalControlStatusSnapshotToJson(status);
    require(
        json["local_control"]["recording_stop"]["ack_state"].get<std::string>() == "idle",
        "idle stop control should report idle ACK state");

    status.local_control_recording_stop.scheduled = true;
    json = orange::control::LocalControlStatusSnapshotToJson(status);
    require(
        json["local_control"]["recording_stop"]["ack_state"].get<std::string>() == "accepted",
        "scheduled stop control should report accepted ACK state");

    status.local_control_recording_stop.scheduled = false;
    status.local_control_recording_stop.stop_triggered = true;
    json = orange::control::LocalControlStatusSnapshotToJson(status);
    require(
        json["local_control"]["recording_stop"]["ack_state"].get<std::string>() == "executing",
        "triggered stop control should report executing ACK state");

    status.local_control_recording_stop.drain_completed_at_utc =
        "2026-05-29T00:00:02Z";
    json = orange::control::LocalControlStatusSnapshotToJson(status);
    require(
        json["local_control"]["recording_stop"]["ack_state"].get<std::string>() == "executed",
        "finalized stop control should report executed ACK state");

    status.local_control_recording_stop = {};
    status.local_control_recording_stop.enabled = true;
    status.local_control_recording_stop.last_event = "ignored_not_recording";
    json = orange::control::LocalControlStatusSnapshotToJson(status);
    require(
        json["local_control"]["recording_stop"]["ack_state"].get<std::string>() == "ignored",
        "ignored stop control should report ignored ACK state");
}

void test_append_local_control_event_log_creates_jsonl()
{
    const auto log_path = temp_path("nested/events.jsonl");
    std::filesystem::remove_all(log_path.parent_path());

    std::string error;
    require(
        AppendLocalControlEventLog(
            log_path.string(),
            {
                {"schema_id", "orange.local_control.gui_event"},
                {"schema_version", 1},
                {"event", "recording_stop_triggered"},
                {"event_at_utc", "2026-05-29T00:00:01Z"},
                {"request_id", "stop-req-1"},
            },
            &error),
        "event log append should succeed: " + error);
    require(
        AppendLocalControlEventLog(
            log_path.string(),
            {
                {"schema_id", "orange.local_control.gui_event"},
                {"schema_version", 1},
                {"event", "recording_drain_finalized"},
                {"event_at_utc", "2026-05-29T00:00:02Z"},
                {"request_id", "stop-req-1"},
            },
            &error),
        "second event log append should succeed: " + error);

    std::ifstream in(log_path);
    require(static_cast<bool>(in), "event log should exist");
    std::string line1;
    std::string line2;
    std::getline(in, line1);
    std::getline(in, line2);
    require(!line1.empty() && !line2.empty(), "event log should contain two lines");
    const nlohmann::json first = nlohmann::json::parse(line1);
    const nlohmann::json second = nlohmann::json::parse(line2);
    require(first["event"].get<std::string>() == "recording_stop_triggered",
            "first event should parse");
    require(second["event"].get<std::string>() == "recording_drain_finalized",
            "second event should parse");
    require(second["request_id"].get<std::string>() == "stop-req-1",
            "event log should preserve request id");
}

void test_server_start_truncates_local_control_event_log()
{
    const auto socket_path = temp_path("truncate_log.sock");
    const auto log_path = temp_path("truncate_log/events.jsonl");
    std::filesystem::remove(socket_path);
    std::filesystem::create_directories(log_path.parent_path());
    {
        std::ofstream out(log_path);
        out << "{\"stale\":true}\n";
    }

    LocalControlServer server;
    require_start_server(&server, socket_path, log_path);
    server.Stop();

    std::ifstream in(log_path);
    require(static_cast<bool>(in), "event log should exist after server start");
    const std::string contents(
        (std::istreambuf_iterator<char>(in)),
        std::istreambuf_iterator<char>());
    require(contents.empty(), "server start should truncate stale event log contents");
}

void test_server_start_refuses_live_socket_without_truncating_log()
{
    const auto socket_path = temp_path("live_socket_refusal.sock");
    const auto log_path = temp_path("live_socket_refusal/events.jsonl");
    std::filesystem::remove(socket_path);
    std::filesystem::remove_all(log_path.parent_path());

    LocalControlServer first_server;
    require_start_server(&first_server, socket_path, log_path);
    first_server.UpdateStatus(healthy_status());

    std::string append_error;
    require(
        AppendLocalControlEventLog(
            log_path.string(),
            {
                {"schema_id", "orange.local_control.gui_event"},
                {"schema_version", 1},
                {"event", "first_server_row"},
                {"event_at_utc", "2026-05-29T00:00:01Z"},
                {"request_id", "first-server"},
            },
            &append_error),
        "event log append should succeed before conflicting start: " + append_error);

    LocalControlServer second_server;
    std::string start_error;
    const bool second_started =
        second_server.Start({socket_path.string(), log_path.string()}, &start_error);
    if (second_started) {
        second_server.Stop();
    }
    require(!second_started, "second server start should refuse a live socket");
    require(
        start_error.find("already owned by another process") != std::string::npos,
        "second server error should report socket ownership lock, got: " + start_error);

    std::ifstream in(log_path);
    require(static_cast<bool>(in), "event log should remain readable after failed start");
    std::string line;
    std::getline(in, line);
    require(!line.empty(), "event log row should survive failed conflicting start");
    const nlohmann::json row = nlohmann::json::parse(line);
    require(
        row["event"].get<std::string>() == "first_server_row",
        "failed conflicting start must not truncate the running server log");

    const nlohmann::json status =
        send_request(socket_path, request_json("status", "status-after-conflict"));
    require(status["ok"].get<bool>(), "first server should still answer after conflict");
    first_server.Stop();
}

void test_server_start_refuses_lockless_live_socket_without_truncating_log()
{
    const auto socket_path = temp_path("lockless_live_socket.sock");
    const auto log_path = temp_path("lockless_live_socket/events.jsonl");
    std::filesystem::remove(socket_path);
    std::filesystem::remove_all(log_path.parent_path());
    std::filesystem::create_directories(log_path.parent_path());
    {
        std::ofstream out(log_path);
        out << "{\"event\":\"existing_row\"}\n";
    }

    const int listen_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    require(listen_fd >= 0, "raw socket() should succeed");
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    const std::string socket_text = socket_path.string();
    std::strncpy(addr.sun_path, socket_text.c_str(), sizeof(addr.sun_path) - 1);
    require(
        bind(listen_fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0,
        "raw bind() should succeed");
    require(listen(listen_fd, 1) == 0, "raw listen() should succeed");

    LocalControlServer server;
    std::string error;
    const bool started = server.Start({socket_path.string(), log_path.string()}, &error);
    if (started) {
        server.Stop();
    }
    close(listen_fd);
    std::filesystem::remove(socket_path);

    require(!started, "server start should refuse a lockless live socket");
    require(
        error.find("already registered by another process") != std::string::npos,
        "server error should report registered live socket, got: " + error);

    std::ifstream in(log_path);
    require(static_cast<bool>(in), "event log should remain readable after lockless refusal");
    std::string line;
    std::getline(in, line);
    require(
        line.find("existing_row") != std::string::npos,
        "lockless live socket refusal must not truncate existing event log");
}

void test_server_start_removes_stale_socket_and_truncates_log()
{
    const auto socket_path = temp_path("stale_socket_recovery.sock");
    const auto log_path = temp_path("stale_socket_recovery/events.jsonl");
    std::filesystem::remove(socket_path);
    std::filesystem::remove_all(log_path.parent_path());
    std::filesystem::create_directories(log_path.parent_path());
    {
        std::ofstream out(log_path);
        out << "{\"event\":\"stale_row\"}\n";
    }

    const int stale_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    require(stale_fd >= 0, "raw stale socket() should succeed");
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    const std::string socket_text = socket_path.string();
    std::strncpy(addr.sun_path, socket_text.c_str(), sizeof(addr.sun_path) - 1);
    require(
        bind(stale_fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0,
        "raw stale bind() should succeed");
    close(stale_fd);
    require(
        std::filesystem::exists(socket_path),
        "closed raw socket should leave a filesystem socket path");

    LocalControlServer server;
    require_start_server(&server, socket_path, log_path);
    server.UpdateStatus(healthy_status());
    const nlohmann::json status =
        send_request(socket_path, request_json("status", "status-after-stale-recovery"));
    require(status["ok"].get<bool>(), "server should answer after stale socket recovery");
    server.Stop();

    std::ifstream in(log_path);
    require(static_cast<bool>(in), "event log should exist after stale socket recovery");
    const std::string contents(
        (std::istreambuf_iterator<char>(in)),
        std::istreambuf_iterator<char>());
    require(contents.empty(), "stale socket recovery should truncate stale event log contents");
}

void test_server_start_fails_when_socket_parent_is_not_directory()
{
    const auto parent_path = temp_path("socket_parent_file");
    const auto socket_path = parent_path / "control.sock";
    const auto log_path = temp_path("socket_parent_file_log/events.jsonl");
    std::filesystem::remove_all(parent_path);
    std::filesystem::remove_all(log_path.parent_path());
    {
        std::ofstream out(parent_path);
        out << "not a directory\n";
    }

    LocalControlServer server;
    std::string error;
    const bool started = server.Start({socket_path.string(), log_path.string()}, &error);
    if (started) {
        server.Stop();
    }
    require(!started, "server start should fail when socket parent is not a directory");
    require(
        error.find("failed to create local control socket directory") != std::string::npos,
        "server error should name socket directory creation failure, got: " + error);
    require(
        !std::filesystem::exists(log_path),
        "event log should not be initialized when socket directory creation fails");
}

void test_projected_center_preflight_requires_operation_id()
{
    std::string error;
    require(!ParseLocalControlRequest(
                request_json(
                    "preflight_projected_center_verification",
                    "preflight-missing-operation",
                    "",
                    projected_center_preflight_params()),
                nullptr,
                &error),
            "preflight without operation_id should fail");
    require(
        error.find("operation_id") != std::string::npos,
        "preflight missing operation should mention operation_id");
}

void test_projected_center_preflight_2010096_fails_without_suppression()
{
    const auto socket_path = temp_path("preflight_2010096_missing_suppression.sock");
    std::filesystem::remove(socket_path);

    LocalControlServer server;
    require_start_server(&server, socket_path);
    server.UpdateStatus(preflight_ready_status());

    const nlohmann::json response = send_request(
        socket_path,
        request_json(
            "preflight_projected_center_verification",
            "preflight-req-1",
            "preflight-op-1",
            projected_center_preflight_params("2010096")));
    server.Stop();

    require(response["ok"].get<bool>(), "preflight validation request should be ok");
    require(!response["accepted"].get<bool>(), "missing suppression should fail preflight");
    require(response["diagnostic_only"].get<bool>(), "preflight should be diagnostic-only");
    require(!response["mutation_allowed"].get<bool>(), "preflight should disallow mutation");
    const nlohmann::json& effect = preflight_effect(response);
    require(effect["status"].get<std::string>() == "fail", "preflight effect should fail");
    require(
        effect["light_trigger_suppression"]["required"].get<bool>(),
        "2010096 should require light trigger suppression");
    require(
        effect["blocking_reasons"].dump().find("light_trigger_suppression") != std::string::npos,
        "blocking reasons should mention light trigger suppression");
}

void test_projected_center_preflight_2010096_passes_with_pinout_disabled()
{
    const auto socket_path = temp_path("preflight_2010096_pinout.sock");
    std::filesystem::remove(socket_path);

    nlohmann::json params = projected_center_preflight_params("2010096");
    params["light_trigger_suppression"] = {
        {"method", "pinout_disabled"},
        {"trigger_output_disabled_readback", true},
    };

    LocalControlServer server;
    require_start_server(&server, socket_path);
    server.UpdateStatus(preflight_ready_status());
    const nlohmann::json first = send_request(
        socket_path,
        request_json(
            "preflight_projected_center_verification",
            "preflight-pinout-req-1",
            "preflight-pinout-op",
            params));
    const nlohmann::json duplicate = send_request(
        socket_path,
        request_json(
            "preflight_projected_center_verification",
            "preflight-pinout-req-1",
            "preflight-pinout-op",
            params));
    const nlohmann::json same_operation_duplicate = send_request(
        socket_path,
        request_json(
            "preflight_projected_center_verification",
            "preflight-pinout-req-2",
            "preflight-pinout-op",
            params));
    const std::vector<PendingLocalControlCommand> pending = server.DrainPendingCommands();
    server.Stop();

    require(first["ok"].get<bool>(), "pinout preflight response should be ok");
    require(first["accepted"].get<bool>(), "pinout preflight should pass");
    require(!first["queued_for_gui_thread"].get<bool>(), "preflight should not queue GUI command");
    require(preflight_effect(first)["status"].get<std::string>() == "pass", "effect status pass");
    require(
        preflight_effect(first)["light_trigger_suppression"]["method"].get<std::string>() ==
            "pinout_disabled",
        "effect should preserve pinout method");
    require(duplicate["duplicate"].get<bool>(), "duplicate request_id should be marked duplicate");
    require(
        same_operation_duplicate["duplicate"].get<bool>(),
        "duplicate operation_id should be marked duplicate");
    require(pending.empty(), "preflight must not create pending commands");
}

void test_projected_center_preflight_2010096_passes_with_outlets_off()
{
    const auto socket_path = temp_path("preflight_2010096_outlets.sock");
    std::filesystem::remove(socket_path);

    nlohmann::json params = projected_center_preflight_params("2010096");
    params["light_trigger_suppression"] = {
        {"method", "pancake_batter_outlets_7_8_off"},
        {"outlets", nlohmann::json::array({7, 8})},
        {"outlets_confirmed_off", true},
    };

    LocalControlServer server;
    require_start_server(&server, socket_path);
    server.UpdateStatus(preflight_ready_status());
    const nlohmann::json response = send_request(
        socket_path,
        request_json(
            "preflight_projected_center_verification",
            "preflight-outlets-req",
            "preflight-outlets-op",
            params));
    server.Stop();

    require(response["accepted"].get<bool>(), "outlets preflight should pass");
    require(
        preflight_effect(response)["light_trigger_suppression"]["method"].get<std::string>() ==
            "pancake_batter_outlets_7_8_off",
        "effect should preserve outlet method");
    require(
        preflight_effect(response)["light_trigger_suppression"]["outlets"]["outlets_confirmed_off"].get<bool>(),
        "outlets confirmation should be visible");
}

void test_projected_center_preflight_fails_missing_filter_confirmation()
{
    const auto socket_path = temp_path("preflight_missing_filter.sock");
    std::filesystem::remove(socket_path);
    nlohmann::json params = projected_center_preflight_params("2010095");
    params["filter_state"] = "unknown";

    LocalControlServer server;
    require_start_server(&server, socket_path);
    server.UpdateStatus(preflight_ready_status());
    const nlohmann::json response = send_request(
        socket_path,
        request_json(
            "preflight_projected_center_verification",
            "preflight-filter-req",
            "preflight-filter-op",
            params));
    server.Stop();

    require(!response["accepted"].get<bool>(), "missing filter confirmation should fail");
    require(
        preflight_effect(response)["blocking_reasons"].dump().find("filter_not_operator_confirmed_removed") !=
            std::string::npos,
        "blocking reason should name missing filter confirmation");
    require(
        !preflight_effect(response)["light_trigger_suppression"]["required"].get<bool>(),
        "non-2010096 should not require suppression by default");
}

void test_projected_center_preflight_fails_exposure_and_frame_rate_thresholds()
{
    const auto socket_path = temp_path("preflight_thresholds.sock");
    std::filesystem::remove(socket_path);
    nlohmann::json params = projected_center_preflight_params("2010095");
    params["exposure_us_requested"] = 99999.0;
    params["exposure_us_readback"] = 50000.0;
    params["frame_rate_hz_requested"] = 6.0;
    params["frame_rate_hz_readback"] = 10.0;

    LocalControlServer server;
    require_start_server(&server, socket_path);
    server.UpdateStatus(preflight_ready_status());
    const nlohmann::json response = send_request(
        socket_path,
        request_json(
            "preflight_projected_center_verification",
            "preflight-thresholds-req",
            "preflight-thresholds-op",
            params));
    server.Stop();

    const nlohmann::json& effect = preflight_effect(response);
    require(!response["accepted"].get<bool>(), "threshold failures should fail preflight");
    require(effect["exposure_us_min_required"].get<double>() == 100000.0,
            "default min exposure should be visible");
    require(effect["frame_rate_hz_max_allowed"].get<double>() == 5.0,
            "default max frame rate should be visible");
    const std::string reasons = effect["blocking_reasons"].dump();
    require(reasons.find("exposure_us_requested_below_minimum") != std::string::npos,
            "requested exposure threshold reason");
    require(reasons.find("exposure_us_readback_below_minimum") != std::string::npos,
            "readback exposure threshold reason");
    require(reasons.find("frame_rate_hz_requested_above_maximum") != std::string::npos,
            "requested frame rate threshold reason");
    require(reasons.find("frame_rate_hz_readback_above_maximum") != std::string::npos,
            "readback frame rate threshold reason");
}

void test_projected_center_preflight_rejects_recording_active()
{
    const auto socket_path = temp_path("preflight_recording_active.sock");
    std::filesystem::remove(socket_path);

    nlohmann::json params = projected_center_preflight_params("2010095");
    LocalControlServer server;
    require_start_server(&server, socket_path);
    server.UpdateStatus(healthy_status());
    const nlohmann::json response = send_request(
        socket_path,
        request_json(
            "preflight_projected_center_verification",
            "preflight-recording-req",
            "preflight-recording-op",
            params));
    server.Stop();

    require(!response["accepted"].get<bool>(), "recording-active status should fail preflight");
    require(
        preflight_effect(response)["blocking_reasons"].dump().find("recording_active") != std::string::npos,
        "recording-active reason should be visible");
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
    require(duplicate["ok"].get<bool>(), "duplicate completion request should still be ok");
    require(duplicate["accepted"].get<bool>(),
            "duplicate completion request should still be accepted");
    require(!duplicate["queued_for_gui_thread"].get<bool>(),
            "duplicate completion request should not queue again");
    require(same_operation_duplicate["duplicate"].get<bool>(),
            "same method and operation_id should be duplicate with a new request_id");
    require(same_operation_duplicate["ok"].get<bool>(),
            "same-operation duplicate should still be ok");
    require(same_operation_duplicate["accepted"].get<bool>(),
            "same-operation duplicate should still be accepted");
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

void test_citrus_completion_can_be_enabled_without_stop_recording()
{
    const auto socket_path = temp_path("completion_only_enabled.sock");
    std::filesystem::remove(socket_path);

    LocalControlServer server;
    LocalControlServerOptions options;
    options.socket_path = socket_path.string();
    options.allow_gui_citrus_completion_commands = true;
    std::string error;
    require(server.Start(options, &error),
            "server start should allow citrus_completion-only lifecycle mode");
    wait_until_running(&server);
    server.UpdateStatus(healthy_status());

    const nlohmann::json completion_response = send_request(
        socket_path,
        request_json(
            "citrus_completion",
            "completion-only-req-1",
            "completion-only-op-1",
            {{"experiment_id", "citrus-exp-completion-only"},
             {"terminal_state", "completed"},
             {"grace_seconds", 0}}));
    const nlohmann::json stop_response = send_request(
        socket_path,
        request_json(
            "stop_recording",
            "stop-should-stay-disabled-req-1",
            "stop-should-stay-disabled-op-1",
            {{"reason", "direct_stop_not_allowed"},
             {"grace_seconds", 0}}));
    const std::vector<PendingLocalControlCommand> pending =
        server.DrainPendingCommands();
    server.Stop();

    require(completion_response["ok"].get<bool>(),
            "citrus_completion should be accepted");
    require(!completion_response["diagnostic_only"].get<bool>(),
            "citrus_completion should be non-diagnostic when specifically enabled");
    require(completion_response["queued_for_gui_thread"].get<bool>(),
            "citrus_completion should queue for the GUI thread");
    require(!stop_response["ok"].get<bool>(),
            "stop_recording should remain disabled");
    require(!stop_response["accepted"].get<bool>(),
            "stop_recording should not be accepted");
    require(stop_response["error"]["code"].get<std::string>() ==
                "unsupported_in_diagnostic_mode",
            "stop_recording should report diagnostic-mode unsupported");
    require(pending.size() == 1,
            "only the citrus_completion request should be queued");
    require(pending[0].method == "citrus_completion",
            "queued command should be citrus_completion");
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
    const nlohmann::json same_operation_duplicate = send_request(
        socket_path,
        request_json("stop_recording", "stop-enabled-req-2", "stop-op-1",
                     {{"reason", "orchestrator_stop_retry"},
                      {"grace_seconds", 5}}));
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
    require(same_operation_duplicate["duplicate"].get<bool>(),
            "same method and operation_id stop_recording should be duplicate with a new request_id");
    require(!same_operation_duplicate["queued_for_gui_thread"].get<bool>(),
            "same-operation stop_recording duplicate should not queue again");
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
        {"citrus_readiness_requires_external_recorders_when_enabled",
         test_citrus_readiness_requires_external_recorders_when_enabled},
        {"status_reports_completed_recording_after_streaming_stop_path",
         test_status_reports_completed_recording_after_streaming_stop_path},
        {"status_reports_separate_stop_and_citrus_completion_gates",
         test_status_reports_separate_stop_and_citrus_completion_gates},
        {"status_reports_local_control_drain_timeout_telemetry",
         test_status_reports_local_control_drain_timeout_telemetry},
        {"status_reports_finalized_after_drain_timeout_as_warning",
         test_status_reports_finalized_after_drain_timeout_as_warning},
        {"status_reports_recording_stop_ack_states",
         test_status_reports_recording_stop_ack_states},
        {"append_local_control_event_log_creates_jsonl",
         test_append_local_control_event_log_creates_jsonl},
        {"server_start_truncates_local_control_event_log",
         test_server_start_truncates_local_control_event_log},
        {"server_start_refuses_live_socket_without_truncating_log",
         test_server_start_refuses_live_socket_without_truncating_log},
        {"server_start_refuses_lockless_live_socket_without_truncating_log",
         test_server_start_refuses_lockless_live_socket_without_truncating_log},
        {"server_start_removes_stale_socket_and_truncates_log",
         test_server_start_removes_stale_socket_and_truncates_log},
        {"server_start_fails_when_socket_parent_is_not_directory",
         test_server_start_fails_when_socket_parent_is_not_directory},
        {"projected_center_preflight_requires_operation_id",
         test_projected_center_preflight_requires_operation_id},
        {"projected_center_preflight_2010096_fails_without_suppression",
         test_projected_center_preflight_2010096_fails_without_suppression},
        {"projected_center_preflight_2010096_passes_with_pinout_disabled",
         test_projected_center_preflight_2010096_passes_with_pinout_disabled},
        {"projected_center_preflight_2010096_passes_with_outlets_off",
         test_projected_center_preflight_2010096_passes_with_outlets_off},
        {"projected_center_preflight_fails_missing_filter_confirmation",
         test_projected_center_preflight_fails_missing_filter_confirmation},
        {"projected_center_preflight_fails_exposure_and_frame_rate_thresholds",
         test_projected_center_preflight_fails_exposure_and_frame_rate_thresholds},
        {"projected_center_preflight_rejects_recording_active",
         test_projected_center_preflight_rejects_recording_active},
        {"citrus_completion_is_diagnostic_ack_and_logged",
         test_citrus_completion_is_diagnostic_ack_and_logged},
        {"citrus_completion_reports_deferred_lifecycle_mode_when_enabled",
         test_citrus_completion_reports_deferred_lifecycle_mode_when_enabled},
        {"citrus_completion_can_be_enabled_without_stop_recording",
         test_citrus_completion_can_be_enabled_without_stop_recording},
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
