#include "external_recorder_supervisor.h"

#include "json.hpp"

#include <algorithm>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <signal.h>
#include <unistd.h>
#include <vector>

namespace {

using orange::external_recorder::BuildRecorderCommand;
using orange::external_recorder::BuildSupervisorPlanFromContract;
using orange::external_recorder::BuildSupervisorPlanFromExperimentSpec;
using orange::external_recorder::StartSupervisorProcesses;
using orange::external_recorder::SupervisorPlan;
using orange::external_recorder::SupervisorProcessOptions;
using orange::external_recorder::SupervisorRuntimeState;
using orange::external_recorder::SupervisorRuntimeStateToJson;
using orange::external_recorder::SupervisorPlanOptions;
using orange::external_recorder::SupervisorPlanToJson;
using orange::external_recorder::StopSupervisorProcesses;

std::filesystem::path g_binary_dir;

void require(const bool condition, const std::string& message)
{
    if (!condition) {
        throw std::runtime_error(message);
    }
}

bool has_arg_pair(const std::vector<std::string>& argv,
                  const std::string& key,
                  const std::string& value)
{
    for (size_t i = 0; i + 1 < argv.size(); ++i) {
        if (argv[i] == key && argv[i + 1] == value) {
            return true;
        }
    }
    return false;
}

bool has_arg(const std::vector<std::string>& argv, const std::string& key)
{
    return std::find(argv.begin(), argv.end(), key) != argv.end();
}

nlohmann::json make_contract(const std::vector<int>& shard_gpu_ids,
                             const std::string& routing_policy)
{
    return {
        {"schema_id", "orange.external_recorder.contract"},
        {"schema_version", 1},
        {"mode", "diagnostic_ipc_v1"},
        {"artifact_root", "/tmp/orange_external_recorder_supervisor_tests"},
        {"session_id", "session_a"},
        {"require_summary", true},
        {"require_video_sanity", true},
        {"require_merged_mp4", shard_gpu_ids.size() > 1},
        {"require_gop_routing", true},
        {"streams", {
            {"2010096", {
                {"stream_id", "2010096"},
                {"camera_serial", "2010096"},
                {"analytics_gpu_id", 5},
                {"recorder_gpu_id", shard_gpu_ids.front()},
                {"expected_shard_gpu_ids", shard_gpu_ids},
                {"routing_policy", routing_policy},
                {"summary_json", "/tmp/orange_external_recorder_supervisor_tests/Cam2010096_external_summary.json"},
                {"video_sanity_json", "/tmp/orange_external_recorder_supervisor_tests/Cam2010096_external_video_sanity.json"},
                {"mp4", "/tmp/orange_external_recorder_supervisor_tests/Cam2010096_external.mp4"},
                {"gop_routing_csv", "/tmp/orange_external_recorder_supervisor_tests/Cam2010096_external_gop_routing.csv"},
                {"encode_fps", 100},
                {"encode_max_fps", 0},
                {"encode_queue_depth", 32},
                {"prewarm_slots", 4},
                {"prewarm_bytes", 20358144},
                {"prewarm_peer_copy", true},
            }},
        }},
    };
}

void test_single_shard_plan_builds_command()
{
    SupervisorPlanOptions options;
    options.recorder_tool_path = "/repo/targets/release/external_recorder_ipc_probe";

    SupervisorPlan plan;
    std::string error;
    require(BuildSupervisorPlanFromContract(
                make_contract({5}, "single_shard"), options, &plan, &error),
            "single-shard contract should build: " + error);
    require(plan.streams.size() == 1, "expected one stream");
    const auto& stream = plan.streams[0];
    require(stream.socket_path == "/tmp/orange_external_recorder_2010096.sock",
            "default socket path should use camera serial");
    require(stream.detach_csv.find("Cam2010096_external_detach.csv") != std::string::npos,
            "detach csv should derive from gop routing path");
    require(stream.encode_csv.find("Cam2010096_external_encode.csv") != std::string::npos,
            "encode csv should derive from gop routing path");

    const std::vector<std::string> argv = BuildRecorderCommand(plan, stream);
    require(argv.front() == options.recorder_tool_path, "command should start with recorder tool");
    require(has_arg_pair(argv, "--socket", "/tmp/orange_external_recorder_2010096.sock"),
            "command should include socket path");
    require(has_arg_pair(argv, "--gpu-id", "5"), "command should include recorder gpu");
    require(has_arg_pair(argv, "--routing-policy", "single_shard"),
            "command should include single_shard policy");
    require(has_arg_pair(argv, "--prewarm-bytes", "20358144"),
            "command should include prewarm bytes");
    require(!has_arg(argv, "--shard-gpu-ids"),
            "single shard command should not include --shard-gpu-ids");
}

void test_two_shard_plan_builds_gop_modulo_command()
{
    SupervisorPlanOptions options;
    SupervisorPlan plan;
    std::string error;
    require(BuildSupervisorPlanFromContract(
                make_contract({5, 6}, "gop_modulo"), options, &plan, &error),
            "two-shard contract should build: " + error);

    const std::vector<std::string> argv = BuildRecorderCommand(plan, plan.streams[0]);
    require(has_arg_pair(argv, "--routing-policy", "gop_modulo"),
            "command should include GOP modulo policy");
    require(has_arg_pair(argv, "--shard-gpu-ids", "5,6"),
            "command should include shard GPU csv");

    const nlohmann::json json_plan = SupervisorPlanToJson(plan);
    require(json_plan["schema_id"] == "orange.external_recorder.supervisor_plan",
            "plan json schema id should be present");
    require(json_plan["streams"][0]["command"]["argv"].is_array(),
            "plan json should expose command argv");
}

void test_spec_requires_external_ipc_sink()
{
    nlohmann::json spec = {
        {"experiment_id", "bad_sink"},
        {"selection", {{"camera_serials", {"2010096"}}, {"gpu_ids", {5}}}},
        {"fixed", {
            {"recording_sink_mode", "real"},
            {"external_recorder_contract", make_contract({5}, "single_shard")},
        }},
    };

    SupervisorPlan plan;
    std::string error;
    require(!BuildSupervisorPlanFromExperimentSpec(spec, {}, &plan, &error),
            "real sink with external contract should fail");
    require(error.find("recording_sink_mode=external_ipc") != std::string::npos,
            "failure should explain required sink mode");
}

void test_spec_requires_selected_stream()
{
    nlohmann::json spec = {
        {"experiment_id", "missing_selected_stream"},
        {"selection", {{"camera_serials", {"2010095"}}, {"gpu_ids", {5}}}},
        {"fixed", {
            {"recording_sink_mode", "external_ipc"},
            {"external_recorder_contract", make_contract({5}, "single_shard")},
        }},
    };

    SupervisorPlan plan;
    std::string error;
    require(!BuildSupervisorPlanFromExperimentSpec(spec, {}, &plan, &error),
            "missing selected stream should fail");
    require(error.find("2010095") != std::string::npos,
            "failure should identify missing camera");
}

void test_invalid_shard_policy_fails()
{
    SupervisorPlan plan;
    std::string error;
    require(!BuildSupervisorPlanFromContract(
                make_contract({5, 6}, "single_shard"), {}, &plan, &error),
            "single_shard with multiple shard GPUs should fail");
    require(error.find("single_shard") != std::string::npos,
            "failure should identify invalid shard policy");
}

void test_process_lifecycle_waits_socket_and_stops()
{
    const std::filesystem::path stub_path =
        g_binary_dir / "external_recorder_supervisor_socket_stub";
    require(std::filesystem::exists(stub_path), "socket stub helper is missing");

    SupervisorPlanOptions options;
    options.recorder_tool_path = stub_path.string();

    SupervisorPlan plan;
    std::string error;
    require(BuildSupervisorPlanFromContract(
                make_contract({5}, "single_shard"), options, &plan, &error),
            "contract should build for lifecycle test: " + error);

    const std::string suffix = std::to_string(static_cast<long long>(getpid()));
    plan.streams[0].socket_path =
        "/tmp/orange_external_recorder_supervisor_lifecycle_" + suffix + ".sock";
    plan.streams[0].recorder_log =
        "/tmp/orange_external_recorder_supervisor_lifecycle_" + suffix + ".log";

    SupervisorRuntimeState runtime;
    SupervisorProcessOptions process_options;
    process_options.socket_ready_timeout_ms = 2000;
    process_options.graceful_shutdown_timeout_ms = 20;
    process_options.terminate_timeout_ms = 2000;
    process_options.allow_regular_file_socket_ready_for_tests = true;

    require(StartSupervisorProcesses(plan, process_options, &runtime, &error),
            "supervisor should launch stub and observe socket readiness: " + error);
    require(runtime.processes.size() == 1, "runtime should contain one process");
    require(runtime.processes[0].socket_ready, "stub socket should be ready");
    require(runtime.processes[0].active, "stub process should still be active");

    require(StopSupervisorProcesses(&runtime, process_options, &error),
            "supervisor should stop stub process: " + error);
    require(!runtime.processes[0].active, "stub process should be inactive after stop");
    require(runtime.processes[0].term_signal == SIGTERM ||
                runtime.processes[0].exit_code == 0,
            "stub should exit cleanly or from SIGTERM");

    const nlohmann::json summary = SupervisorRuntimeStateToJson(runtime);
    require(summary["schema_id"] == "orange.external_recorder.supervisor_runtime",
            "runtime summary schema should be present");
    require(summary["processes"][0]["socket_ready"].get<bool>(),
            "runtime summary should preserve socket readiness");
}

}  // namespace

int main(int argc, char** argv)
{
    if (argc > 0) {
        g_binary_dir = std::filesystem::absolute(argv[0]).parent_path();
    }

    struct TestCase {
        const char* name;
        void (*fn)();
    };

    const TestCase tests[] = {
        {"single_shard_plan_builds_command", test_single_shard_plan_builds_command},
        {"two_shard_plan_builds_gop_modulo_command", test_two_shard_plan_builds_gop_modulo_command},
        {"spec_requires_external_ipc_sink", test_spec_requires_external_ipc_sink},
        {"spec_requires_selected_stream", test_spec_requires_selected_stream},
        {"invalid_shard_policy_fails", test_invalid_shard_policy_fails},
        {"process_lifecycle_waits_socket_and_stops", test_process_lifecycle_waits_socket_and_stops},
    };

    bool ok = true;
    for (const TestCase& test : tests) {
        try {
            test.fn();
            std::cout << "[PASS] " << test.name << "\n";
        } catch (const std::exception& ex) {
            ok = false;
            std::cerr << "[FAIL] " << test.name << ": " << ex.what() << "\n";
        }
    }
    return ok ? 0 : 1;
}
