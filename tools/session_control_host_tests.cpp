// tools/session_control_host_tests.cpp
//
// src/session/session_control_host.h: the stop decision both binaries use
// (DecideStop), the socket/log naming from the environment, and a host on a
// temporary socket answering status, queueing citrus_completion and
// finalizing receipts the way Citrus sends them, with a fake receipt
// verification. No camera, Citrus or recording.

#include "session/session_control_host.h"

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <thread>

namespace {

int g_failures = 0;

#define EXPECT(cond, msg)                                                       \
    do {                                                                        \
        if (!(cond)) {                                                          \
            ++g_failures;                                                       \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, msg); \
        }                                                                       \
    } while (0)

using namespace orange::session;
using orange::control::PendingLocalControlCommand;
using nlohmann::json;
using Clock = std::chrono::steady_clock;

PendingLocalControlCommand Command(const std::string& method, json params = json::object())
{
    PendingLocalControlCommand command;
    command.method = method;
    command.request_id = method + "_1";
    command.operation_id = "op_1";
    command.source = "citrus";
    command.received_at_utc = "2026-11-11T02:00:00Z";
    command.params = std::move(params);
    return command;
}

void TestDecideStop()
{
    StopInputs bound;
    bound.stop_recording_enabled = true;
    bound.citrus_completion_enabled = true;
    bound.recording_active = true;
    bound.binding_mode = "required";
    const auto now = Clock::now();

    StopSchedule schedule;
    EXPECT(DecideStop(Command("status"), bound, now, &schedule) == StopDecision::kNotAStop, "status is no stop");

    auto d = DecideStop(Command("citrus_completion", {{"experiment_id", "x"}, {"terminal_state", "completed"}}),
                        bound, now, &schedule);
    EXPECT(d == StopDecision::kScheduled && schedule.scheduled, "completion schedules");
    EXPECT(schedule.grace_seconds == 10.0, "completion default grace 10 s");
    EXPECT(schedule.deadline == now + std::chrono::seconds(10), "deadline = now + grace");
    EXPECT(schedule.experiment_id == "x" && schedule.terminal_state == "completed", "carries Citrus fields");

    // A later completion with a longer grace keeps the earlier deadline.
    d = DecideStop(Command("citrus_completion", {{"experiment_id", "x"}, {"grace_seconds", 30}}), bound,
                   now + std::chrono::seconds(1), &schedule);
    EXPECT(d == StopDecision::kKeptEarlierDeadline, "earliest deadline wins");
    // stop_recording (grace 0) moves it earlier.
    d = DecideStop(Command("stop_recording"), bound, now + std::chrono::seconds(2), &schedule);
    EXPECT(d == StopDecision::kScheduled && schedule.method == "stop_recording" && schedule.grace_seconds == 0.0,
           "an earlier stop replaces it");

    StopSchedule fresh;
    d = DecideStop(Command("citrus_completion", {{"experiment_id", "x"}, {"grace_seconds", -3}}), bound, now, &fresh);
    EXPECT(d == StopDecision::kScheduled && fresh.grace_seconds == 10.0, "negative grace falls back to default");

    StopInputs recording_only = bound;
    recording_only.binding_mode = "not_applicable";
    StopSchedule none;
    EXPECT(DecideStop(Command("citrus_completion", {{"experiment_id", "x"}}), recording_only, now, &none) ==
               StopDecision::kIgnoredNotCitrusBound && !none.scheduled,
           "Citrus cannot stop a recording_only recording");
    EXPECT(DecideStop(Command("stop_recording"), recording_only, now, &none) == StopDecision::kScheduled,
           "a generic stop still stops a recording_only recording");

    StopInputs idle = bound;
    idle.recording_active = false;
    StopSchedule idle_schedule;
    EXPECT(DecideStop(Command("citrus_completion", {{"experiment_id", "x"}}), idle, now, &idle_schedule) ==
               StopDecision::kIgnoredNotRecording,
           "nothing recording");

    StopInputs disabled = bound;
    disabled.citrus_completion_enabled = false;
    EXPECT(DecideStop(Command("citrus_completion", {{"experiment_id", "x"}}), disabled, now, &idle_schedule) ==
               StopDecision::kIgnoredDisabled,
           "completion disabled");
    EXPECT(std::string(StopDecisionName(StopDecision::kIgnoredNotCitrusBound)) == "ignored_not_citrus_bound",
           "decision names match the GUI event names");
}

void TestNaming()
{
    unsetenv("ORANGE_GUI_LOCAL_CONTROL_SOCKET");
    unsetenv("ORANGE_LOCAL_CONTROL_SOCKET");
    unsetenv("ORANGE_GUI_LOCAL_CONTROL_LOG");
    unsetenv("ORANGE_LOCAL_CONTROL_LOG");
    EXPECT(LocalControlSocketPathFromEnvironment() == "/tmp/orange_local_control.sock", "default socket");
    EXPECT(LocalControlEventLogPathFromEnvironment("/tmp/a.sock") == "/tmp/a.sock.events.jsonl", "default log");
    setenv("ORANGE_LOCAL_CONTROL_SOCKET", "/tmp/b.sock", 1);
    EXPECT(LocalControlSocketPathFromEnvironment() == "/tmp/b.sock", "generic socket env");
    setenv("ORANGE_GUI_LOCAL_CONTROL_SOCKET", "/tmp/c.sock", 1);
    EXPECT(LocalControlSocketPathFromEnvironment() == "/tmp/c.sock", "GUI socket env wins");
    setenv("ORANGE_LOCAL_CONTROL_LOG", "/tmp/d.jsonl", 1);
    EXPECT(LocalControlEventLogPathFromEnvironment("/tmp/c.sock") == "/tmp/d.jsonl", "log env");
    unsetenv("ORANGE_GUI_LOCAL_CONTROL_SOCKET");
    unsetenv("ORANGE_LOCAL_CONTROL_SOCKET");
    unsetenv("ORANGE_LOCAL_CONTROL_LOG");
}

json Send(const std::string& socket_path, const json& request)
{
    const int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    std::snprintf(address.sun_path, sizeof(address.sun_path), "%s", socket_path.c_str());
    if (connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
        close(fd);
        return json();
    }
    const std::string line = request.dump() + "\n";
    (void)write(fd, line.data(), line.size());
    shutdown(fd, SHUT_WR);
    std::string reply;
    char buf[4096];
    for (ssize_t n; (n = read(fd, buf, sizeof(buf))) > 0;) reply.append(buf, static_cast<size_t>(n));
    close(fd);
    const auto end = reply.find('\n');
    return json::parse(reply.substr(0, end), nullptr, false);
}

json Request(const std::string& method, const std::string& id, json params = json::object())
{
    json r = {{"schema_id", "orange.local_control.request"}, {"schema_version", 1}, {"method", method},
              {"request_id", id}, {"source", "citrus"}, {"params", std::move(params)}};
    if (method != "status") r["operation_id"] = "op_" + id;
    return r;
}

void TestHostOverSocket()
{
    namespace fs = std::filesystem;
    const fs::path dir = fs::temp_directory_path() / ("orange_session_control_host_tests_" + std::to_string(getpid()));
    fs::create_directories(dir / "recording");
    const std::string socket_path = (dir / "orange.sock").string();
    const std::string folder = (dir / "recording").lexically_normal().string();

    int verifications = 0;
    SessionControlHostOptions options;
    options.process = "orange_client";
    options.socket_path = socket_path;
    options.allow_citrus_completion = true;
    options.finalizer = [&verifications](const std::string&, const json& params, const FinalizationProgress&) {
        ++verifications;
        RecordingObservationFinalizationResult result;
        result.ok = true;
        result.collection_reference = {{"experiment_id", params.value("experiment_id", "")}};
        return result;
    };
    {
        SessionControlHost host(options);
        EXPECT(host.options().event_log_path == socket_path + ".events.jsonl", "GUI log naming");
        std::string error;
        EXPECT(host.Start(&error), error.c_str());
        for (int i = 0; i < 200 && !fs::exists(socket_path); ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        EXPECT(host.listening(), "listening");

        orange::control::LocalControlStatusSnapshot status;
        status.process = "orange_client";
        status.recording_active = true;
        status.recording_folder = folder;
        host.UpdateStatus(status);

        json reply = Send(socket_path, Request("status", "s1"));
        EXPECT(reply.value("ok", false), "status answered");
        EXPECT(reply.contains("status") && reply["status"].value("process", "") == "orange_client" &&
                   reply["status"].value("recording", json::object()).value("folder", "") == folder,
               "status carries the owner's snapshot");

        reply = Send(socket_path, Request("citrus_completion", "c1", {{"experiment_id", "exp1"},
                                                                       {"terminal_state", "completed"}}));
        EXPECT(reply.value("ok", false) && reply.value("accepted", false), "completion acked as Citrus requires");
        const auto pending = host.DrainPendingCommands();
        EXPECT(pending.size() == 1 && pending[0].method == "citrus_completion" &&
                   pending[0].params.value("experiment_id", "") == "exp1",
               "completion queued for the owner");

        // Receipts for another folder are refused.
        reply = Send(socket_path, Request("finalize_recording_observation_bindings", "f0",
                                          {{"experiment_id", "exp1"}, {"recording_folder", (dir / "other").string()},
                                           {"receipts", json::array({{{"sha256", "a"}}})}}));
        EXPECT(!reply.value("accepted", true), "foreign folder refused");
        EXPECT(!host.ReceiptsFinalized(folder), "not finalized yet");

        // Finalizing state, then Citrus's receipts.
        status.recording_active = false;
        status.recording_finalizing = true;
        host.UpdateStatus(status);
        reply = Send(socket_path, Request("finalize_recording_observation_bindings", "f1",
                                          {{"experiment_id", "exp1"}, {"recording_folder", folder},
                                           {"receipts", json::array({{{"sha256", "a"}}})}}));
        EXPECT(reply.value("ok", false) && reply.value("accepted", false), "receipts accepted");
        EXPECT(host.WaitForReceipts(folder, std::chrono::milliseconds(3000)), "receipts finalized for the folder");
        EXPECT(host.ReceiptsFinalized(folder + "/"), "folder compared normalized");
        EXPECT(verifications == 1, "verified once");

        // A resend of the same set (Citrus's retry) is answered without a new verification.
        reply = Send(socket_path, Request("finalize_recording_observation_bindings", "f2",
                                          {{"experiment_id", "exp1"}, {"recording_folder", folder},
                                           {"receipts", json::array({{{"sha256", "a"}}})}}));
        EXPECT(reply.value("accepted", false) && verifications == 1, "retry answered from the stored outcome");

        // An idle process (no recording) does not accept receipts for an old state.
        status.recording_finalizing = false;
        status.recording_finalized = false;
        host.UpdateStatus(status);
        reply = Send(socket_path, Request("finalize_recording_observation_bindings", "f3",
                                          {{"experiment_id", "exp2"}, {"recording_folder", folder},
                                           {"receipts", json::array({{{"sha256", "b"}}})}}));
        EXPECT(!reply.value("accepted", true), "receipts refused when the recording is not eligible");

        EXPECT(!host.WaitForReceipts((dir / "never").string(), std::chrono::milliseconds(50)), "timeout");
        host.Stop();
        EXPECT(!host.listening(), "stopped");
    }
    EXPECT(!fs::exists(socket_path), "socket removed at stop");
    std::error_code ec;
    fs::remove_all(dir, ec);
}

}  // namespace

int main()
{
    TestDecideStop();
    TestNaming();
    TestHostOverSocket();
    if (g_failures) {
        std::fprintf(stderr, "session_control_host_tests: %d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("session_control_host_tests: PASS\n");
    return 0;
}
