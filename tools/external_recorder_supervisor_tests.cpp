#include "external_recorder_lifecycle.h"
#include "external_recorder_supervisor.h"

#include "json.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <signal.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace {

using orange::external_recorder::BuildRecorderCommand;
using orange::external_recorder::BuildSupervisorPlanFromContract;
using orange::external_recorder::BuildSupervisorPlanFromExperimentSpec;
using orange::external_recorder::PollSupervisorProcesses;
using orange::external_recorder::StartSupervisedRecorderLifecycle;
using orange::external_recorder::StartSupervisorProcesses;
using orange::external_recorder::StopSupervisedRecorderLifecycle;
using orange::external_recorder::SupervisedRecorderLifecycleOptions;
using orange::external_recorder::SupervisedRecorderLifecycleState;
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
        {"require_status", true},
        {"require_status_runtime", true},
        {"require_storage_preflight", true},
        {"require_protocol_hello", true},
        {"streams", {
            {"2010096", {
                {"stream_id", "2010096"},
                {"stream_kind", "full_frame"},
                {"output_kind", "full"},
                {"camera_serial", "2010096"},
                {"env_key", "2010096"},
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
    require(plan.require_status, "plan should require recorder status sidecars");
    require(plan.require_status_runtime, "plan should require runtime recorder status parsing");
    require(plan.require_storage_preflight,
            "plan should require recorder storage preflight telemetry");
    require(plan.require_protocol_hello,
            "plan should require IPC protocol hello telemetry");
    const auto& stream = plan.streams[0];
    require(stream.stream_kind == "full_frame", "default full stream kind should parse");
    require(stream.output_kind == "full", "default full output kind should parse");
    require(stream.env_key == "2010096", "default full env key should parse");
    require(stream.socket_path == "/tmp/orange_external_recorder_2010096.sock",
            "default socket path should use camera serial");
    require(stream.detach_csv.find("Cam2010096_external_detach.csv") != std::string::npos,
            "detach csv should derive from gop routing path");
    require(stream.encode_csv.find("Cam2010096_external_encode.csv") != std::string::npos,
            "encode csv should derive from gop routing path");
    require(stream.status_json.find("Cam2010096_external_status.json") != std::string::npos,
            "status json should derive from summary path");

    const std::vector<std::string> argv = BuildRecorderCommand(plan, stream);
    require(argv.front() == options.recorder_tool_path, "command should start with recorder tool");
    require(has_arg_pair(argv, "--socket", "/tmp/orange_external_recorder_2010096.sock"),
            "command should include socket path");
    require(has_arg_pair(argv, "--gpu-id", "5"), "command should include recorder gpu");
    require(has_arg_pair(argv, "--stream-kind", "full_frame"),
            "command should include full stream kind");
    require(has_arg_pair(argv, "--output-kind", "full"),
            "command should include full output kind");
    require(has_arg_pair(argv, "--routing-policy", "single_shard"),
            "command should include single_shard policy");
    require(has_arg_pair(argv, "--prewarm-bytes", "20358144"),
            "command should include prewarm bytes");
    require(has_arg_pair(argv,
                         "--status-json",
                         "/tmp/orange_external_recorder_supervisor_tests/Cam2010096_external_status.json"),
            "command should include live status sidecar path");
    require(!has_arg(argv, "--shard-gpu-ids"),
            "single shard command should not include --shard-gpu-ids");
    require(has_arg_pair(argv, "--importance-map-mode", "off"),
            "importance maps must be explicitly disabled by default");
}

void test_static_dish_prior_builds_geometry_command()
{
    nlohmann::json contract = make_contract({5, 6}, "gop_modulo");
    contract["streams"]["2010096"]["importance_map"] = {
        {"mode", "static_dish_prior"},
        {"geometry", {
            {"shape", "circle"},
            {"center_x_px", 2243.25},
            {"center_y_px", 2234.75},
            {"radius_px", 2160.5},
        }},
        {"halo_px", 64.0},
        {"inside_delta_qp", -2},
        {"halo_delta_qp", 0},
        {"outside_delta_qp", 2},
        {"source", {
            {"artifact_path", "/recording/geometry/Cam2010096/rim.json"},
            {"artifact_sha256", "sha256:abc"},
            {"artifact_fingerprint", "rim-v1:def"},
        }},
    };

    SupervisorPlan plan;
    std::string error;
    require(BuildSupervisorPlanFromContract(contract, {}, &plan, &error),
            "static dish-prior contract should build: " + error);
    require(plan.streams.size() == 1, "expected one QP-map stream");
    const auto& policy = plan.streams[0].importance_map;
    require(policy.enabled(), "static dish-prior policy should be enabled");
    require(policy.circle.center_x_px == 2243.25,
            "circle center X should survive contract parsing");
    require(policy.circle.radius_px == 2160.5,
            "circle radius should survive contract parsing");
    require(policy.source_artifact_sha256 == "sha256:abc",
            "source checksum should survive contract parsing");

    const std::vector<std::string> argv = BuildRecorderCommand(plan, plan.streams[0]);
    require(has_arg_pair(argv, "--importance-map-mode", "static_dish_prior"),
            "recorder command should enable static dish priority");
    require(has_arg_pair(argv, "--importance-map-center-x-px", "2243.250000"),
            "recorder command should include circle center X");
    require(has_arg_pair(argv, "--importance-map-radius-px", "2160.500000"),
            "recorder command should include circle radius");
    require(has_arg_pair(argv, "--importance-map-inside-delta-qp", "-2"),
            "recorder command should include inside QP delta");
    require(has_arg_pair(
                argv,
                "--importance-map-source-artifact-sha256",
                "sha256:abc"),
            "recorder command should include source checksum");

    const nlohmann::json plan_json = SupervisorPlanToJson(plan);
    require(plan_json["streams"][0]["importance_map"]["mode"] ==
                "static_dish_prior",
            "plan artifact should preserve active policy");
}

void test_static_dish_prior_requires_valid_circle()
{
    nlohmann::json contract = make_contract({5}, "single_shard");
    contract["streams"]["2010096"]["importance_map"] = {
        {"mode", "static_dish_prior"},
        {"geometry", {
            {"shape", "circle"},
            {"center_x_px", 100.0},
            {"center_y_px", 100.0},
            {"radius_px", 0.0},
        }},
    };
    SupervisorPlan plan;
    std::string error;
    require(!BuildSupervisorPlanFromContract(contract, {}, &plan, &error),
            "zero-radius dish prior must be rejected");
    require(error.find("radius") != std::string::npos,
            "invalid circle rejection should name radius");
}

void test_vbr_cq_flows_to_recorder_and_plan()
{
    nlohmann::json contract = make_contract({5, 6}, "gop_modulo");
    auto& stream = contract["streams"]["2010096"];
    stream["rate_control_mode"] = "vbr_cq";
    stream["quality_value"] = 22;
    stream["bitrate_bps"] = 150000000;
    stream["max_bitrate_bps"] = 250000000;
    stream["vbv_buffer_size"] = 250000000;

    SupervisorPlan plan;
    std::string error;
    require(BuildSupervisorPlanFromContract(contract, {}, &plan, &error),
            "VBR-CQ contract should build: " + error);
    require(plan.streams.size() == 1, "expected one VBR-CQ stream");
    const auto& parsed = plan.streams[0];
    require(parsed.rate_control_mode == "vbr_cq",
            "VBR-CQ mode should survive contract parsing");
    require(parsed.quality_value == 22,
            "VBR-CQ target quality should survive contract parsing");
    require(parsed.max_bitrate_bps == 250000000,
            "VBR-CQ bitrate ceiling should survive contract parsing");

    const std::vector<std::string> argv = BuildRecorderCommand(plan, parsed);
    require(has_arg_pair(argv, "--rate-control", "vbr_cq"),
            "recorder command should select VBR-CQ");
    require(has_arg_pair(argv, "--quality", "22"),
            "recorder command should include VBR-CQ target quality");
    require(has_arg_pair(argv, "--max-bitrate-bps", "250000000"),
            "recorder command should include VBR-CQ bitrate ceiling");

    const nlohmann::json plan_json = SupervisorPlanToJson(plan);
    require(plan_json["streams"][0]["rate_control_mode"] == "vbr_cq",
            "supervisor artifact should preserve VBR-CQ mode");
    require(plan_json["streams"][0]["quality_value"] == 22,
            "supervisor artifact should preserve VBR-CQ target quality");
}

void test_invalid_external_rate_control_is_rejected()
{
    nlohmann::json contract = make_contract({5}, "single_shard");
    contract["streams"]["2010096"]["rate_control_mode"] = "mystery";
    SupervisorPlan plan;
    std::string error;
    require(!BuildSupervisorPlanFromContract(contract, {}, &plan, &error),
            "unknown external rate-control mode must be rejected");
    require(error.find("rate_control_mode") != std::string::npos,
            "rate-control rejection should name the invalid field");

    contract = make_contract({5}, "single_shard");
    contract["streams"]["2010096"]["rate_control_mode"] = "vbr_cq";
    contract["streams"]["2010096"]["quality_value"] = 52;
    error.clear();
    require(!BuildSupervisorPlanFromContract(contract, {}, &plan, &error),
            "VBR-CQ target quality above 51 must be rejected");
    require(error.find("quality_value") != std::string::npos,
            "quality rejection should name the invalid field");
}

void test_crop_stream_plan_uses_real_camera_serial_and_env_key()
{
    nlohmann::json contract = make_contract({6}, "single_shard");
    contract["streams"] = nlohmann::json::object({
        {"2010096_crop", {
            {"stream_id", "2010096_crop"},
            {"stream_kind", "crop"},
            {"output_kind", "crop"},
            {"camera_serial", "2010096"},
            {"env_key", "2010096_crop"},
            {"analytics_gpu_id", 5},
            {"recorder_gpu_id", 6},
            {"expected_shard_gpu_ids", nlohmann::json::array({6})},
            {"routing_policy", "single_shard"},
            {"summary_json", "/tmp/orange_external_recorder_supervisor_tests/Cam2010096_crop_external_summary.json"},
            {"video_sanity_json", "/tmp/orange_external_recorder_supervisor_tests/Cam2010096_crop_external_video_sanity.json"},
            {"mp4", "/tmp/orange_external_recorder_supervisor_tests/Cam2010096_crop_external.mp4"},
            {"gop_routing_csv", "/tmp/orange_external_recorder_supervisor_tests/Cam2010096_crop_external_gop_routing.csv"},
            {"encode_fps", 100},
            {"encode_max_fps", 0},
            {"encode_queue_depth", 64},
        }},
    });

    SupervisorPlan plan;
    std::string error;
    require(BuildSupervisorPlanFromContract(contract, {}, &plan, &error),
            "crop stream contract should build: " + error);
    require(plan.streams.size() == 1, "expected one crop stream");
    const auto& stream = plan.streams[0];
    require(stream.stream_id == "2010096_crop", "crop stream id should parse");
    require(stream.stream_kind == "crop", "crop stream kind should parse");
    require(stream.output_kind == "crop", "crop output kind should parse");
    require(stream.camera_serial == "2010096", "crop stream should keep real camera serial");
    require(stream.env_key == "2010096_crop", "crop stream should use crop env key");
    require(stream.socket_path == "/tmp/orange_external_recorder_2010096_crop.sock",
            "crop default socket should derive from env key");
    require(stream.status_json.find("Cam2010096_crop_external_status.json") != std::string::npos,
            "crop status path should derive from crop summary path");

    const std::vector<std::string> argv = BuildRecorderCommand(plan, stream);
    require(has_arg_pair(argv, "--stream-id", "2010096_crop"),
            "crop command should include crop stream id");
    require(has_arg_pair(argv, "--stream-kind", "crop"),
            "crop command should include crop stream kind");
    require(has_arg_pair(argv, "--output-kind", "crop"),
            "crop command should include crop output kind");
    require(has_arg_pair(argv, "--socket", "/tmp/orange_external_recorder_2010096_crop.sock"),
            "crop command should include env-key-derived socket path");

    const nlohmann::json json_plan = SupervisorPlanToJson(plan);
    require(json_plan["streams"][0]["stream_kind"] == "crop",
            "plan json should expose crop stream kind");
    require(json_plan["streams"][0]["output_kind"] == "crop",
            "plan json should expose crop output kind");
    require(json_plan["streams"][0]["camera_serial"] == "2010096",
            "plan json should expose real camera serial");
    require(json_plan["streams"][0]["env_key"] == "2010096_crop",
            "plan json should expose crop env key");
}

void test_two_shard_plan_builds_gop_modulo_command()
{
    SupervisorPlanOptions options;
    SupervisorPlan plan;
    std::string error;
    require(BuildSupervisorPlanFromContract(
                make_contract({5, 6}, "gop_modulo"), options, &plan, &error),
            "two-shard contract should build: " + error);
    require(!plan.preserve_shard_mp4s,
            "two-shard plan should delete shard MP4s by default after merge");

    const std::vector<std::string> argv = BuildRecorderCommand(plan, plan.streams[0]);
    require(has_arg_pair(argv, "--routing-policy", "gop_modulo"),
            "command should include GOP modulo policy");
    require(has_arg_pair(argv, "--shard-gpu-ids", "5,6"),
            "command should include shard GPU csv");
    require(!has_arg(argv, "--preserve-shard-mp4s"),
            "default command should not preserve shard MP4s");

    const nlohmann::json json_plan = SupervisorPlanToJson(plan);
    require(json_plan["schema_id"] == "orange.external_recorder.supervisor_plan",
            "plan json schema id should be present");
    require(json_plan["streams"][0]["command"]["argv"].is_array(),
            "plan json should expose command argv");
    require(json_plan.value("require_status", false),
            "plan json should expose status sidecar requirement");
    require(json_plan.value("require_status_runtime", false),
            "plan json should expose runtime status requirement");
    require(json_plan.value("require_storage_preflight", false),
            "plan json should expose storage preflight requirement");
    require(json_plan.value("require_protocol_hello", false),
            "plan json should expose IPC protocol hello requirement");
    require(json_plan.value("preserve_shard_mp4s", true) == false,
            "plan json should expose default shard MP4 retention policy");
    require(json_plan["streams"][0].value("status_json", "").find(
                "Cam2010096_external_status.json") != std::string::npos,
            "plan json should expose status sidecar path");
}

void test_preserve_shard_mp4s_opt_in()
{
    SupervisorPlanOptions options;
    nlohmann::json contract = make_contract({5, 6}, "gop_modulo");
    contract["preserve_shard_mp4s"] = true;

    SupervisorPlan plan;
    std::string error;
    require(BuildSupervisorPlanFromContract(contract, options, &plan, &error),
            "preserve-shard contract should build: " + error);
    require(plan.preserve_shard_mp4s,
            "plan should carry preserve_shard_mp4s=true");

    const std::vector<std::string> argv = BuildRecorderCommand(plan, plan.streams[0]);
    require(has_arg(argv, "--preserve-shard-mp4s"),
            "command should opt into preserving shard MP4s");
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

void test_spec_selected_stream_ignores_crop_output()
{
    nlohmann::json contract = make_contract({6}, "single_shard");
    contract["streams"] = nlohmann::json::object({
        {"2010096_crop", {
            {"stream_id", "2010096_crop"},
            {"stream_kind", "crop"},
            {"output_kind", "crop"},
            {"camera_serial", "2010096"},
            {"env_key", "2010096_crop"},
            {"analytics_gpu_id", 5},
            {"recorder_gpu_id", 6},
            {"expected_shard_gpu_ids", nlohmann::json::array({6})},
            {"routing_policy", "single_shard"},
            {"summary_json", "/tmp/orange_external_recorder_supervisor_tests/Cam2010096_crop_external_summary.json"},
            {"video_sanity_json", "/tmp/orange_external_recorder_supervisor_tests/Cam2010096_crop_external_video_sanity.json"},
            {"mp4", "/tmp/orange_external_recorder_supervisor_tests/Cam2010096_crop_external.mp4"},
            {"gop_routing_csv", "/tmp/orange_external_recorder_supervisor_tests/Cam2010096_crop_external_gop_routing.csv"},
        }},
    });
    nlohmann::json spec = {
        {"experiment_id", "crop_only_stream"},
        {"selection", {{"camera_serials", {"2010096"}}, {"gpu_ids", {5}}}},
        {"fixed", {
            {"recording_sink_mode", "external_ipc"},
            {"external_recorder_contract", contract},
        }},
    };

    SupervisorPlan plan;
    std::string error;
    require(!BuildSupervisorPlanFromExperimentSpec(spec, {}, &plan, &error),
            "crop-only stream should not satisfy full-frame selected camera");
    require(error.find("2010096") != std::string::npos,
            "failure should identify missing full-frame camera");
}

void test_spec_recording_control_flows_to_command()
{
    nlohmann::json contract = make_contract({5, 6}, "gop_modulo");
    contract["streams"]["2010096"]["terminal_tail_coalesce_frames"] = 25;
    nlohmann::json spec = {
        {"experiment_id", "rolling_control"},
        {"selection", {{"camera_serials", {"2010096"}}, {"gpu_ids", {5}}}},
        {"fixed", {
            {"recording_sink_mode", "external_ipc"},
            {"recording_control", {
                {"record_for_seconds", 6},
                {"clip_seconds", 2},
            }},
            {"external_recorder_contract", contract},
        }},
    };

    SupervisorPlan plan;
    std::string error;
    require(BuildSupervisorPlanFromExperimentSpec(spec, {}, &plan, &error),
            "spec recording_control should build: " + error);
    require(plan.streams.size() == 1, "expected one stream");
    require(plan.streams[0].record_for_seconds == 6,
            "record_for_seconds should flow from fixed.recording_control");
    require(plan.streams[0].clip_seconds == 2,
            "clip_seconds should flow from fixed.recording_control");
    require(plan.streams[0].terminal_tail_coalesce_frames == 25,
            "terminal tail coalesce frame count should flow from stream contract");

    const std::vector<std::string> argv = BuildRecorderCommand(plan, plan.streams[0]);
    require(has_arg_pair(argv, "--record-for-seconds", "6"),
            "command should include record duration");
    require(has_arg_pair(argv, "--clip-seconds", "2"),
            "command should include clip duration");
    require(has_arg_pair(argv, "--terminal-tail-coalesce-frames", "25"),
            "command should include terminal tail coalesce frame count");

    const nlohmann::json json_plan = SupervisorPlanToJson(plan);
    require(json_plan["streams"][0]["recording_control"]["record_for_seconds"] == 6,
            "plan json should include record duration");
    require(json_plan["streams"][0]["recording_control"]["clip_seconds"] == 2,
            "plan json should include clip duration");
    require(json_plan["streams"][0]["terminal_tail_coalesce_frames"] == 25,
            "plan json should include terminal tail coalesce frame count");
}

void test_storage_thresholds_flow_to_command_and_plan()
{
    nlohmann::json contract = make_contract({5}, "single_shard");
    contract["streams"]["2010096"]["min_free_bytes"] = 123456789ULL;
    contract["streams"]["2010096"]["low_space_warning_bytes"] = 234567890ULL;

    SupervisorPlan plan;
    std::string error;
    require(BuildSupervisorPlanFromContract(contract, {}, &plan, &error),
            "storage threshold contract should build: " + error);
    require(plan.streams.size() == 1, "expected one stream");
    require(plan.streams[0].min_free_bytes == 123456789ULL,
            "min_free_bytes should flow to stream plan");
    require(plan.streams[0].low_space_warning_bytes == 234567890ULL,
            "low_space_warning_bytes should flow to stream plan");

    const std::vector<std::string> argv = BuildRecorderCommand(plan, plan.streams[0]);
    require(has_arg_pair(argv, "--min-free-bytes", "123456789"),
            "command should include storage preflight hard minimum");
    require(has_arg_pair(argv, "--low-space-warning-bytes", "234567890"),
            "command should include storage low-space warning threshold");

    const nlohmann::json json_plan = SupervisorPlanToJson(plan);
    require(json_plan["streams"][0]["min_free_bytes"] == 123456789ULL,
            "plan json should expose min free bytes");
    require(json_plan["streams"][0]["low_space_warning_bytes"] == 234567890ULL,
            "plan json should expose low-space warning bytes");
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

void test_process_poll_detects_unexpected_signal_exit()
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
            "contract should build for poll test: " + error);

    const std::string suffix = std::to_string(static_cast<long long>(getpid())) + "_poll";
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
            "supervisor should launch stub for poll test: " + error);
    require(runtime.processes.size() == 1, "runtime should contain one process");
    require(runtime.processes[0].active, "stub should start active");

    require(kill(runtime.processes[0].pid, SIGKILL) == 0,
            "test should be able to kill stub process");

    bool observed_failure = false;
    for (int attempt = 0; attempt < 100; ++attempt) {
        error.clear();
        if (!PollSupervisorProcesses(&runtime, &error)) {
            observed_failure = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    require(observed_failure, "poll should report unexpected recorder death");
    require(!runtime.processes[0].active, "poll should mark killed process inactive");
    require(runtime.processes[0].term_signal == SIGKILL,
            "poll should capture SIGKILL");
    require(runtime.processes[0].status == "killed",
            "poll should report killed status");
    require(error.find("signal") != std::string::npos,
            "poll error should explain signal death");

    error.clear();
    require(!StopSupervisorProcesses(&runtime, process_options, &error),
            "stop should preserve unexpected process death as a failure");
    require(error.find("signal") != std::string::npos,
            "stop error should preserve signal death");

    std::filesystem::remove(plan.streams[0].socket_path);
    std::filesystem::remove(plan.streams[0].recorder_log);
}

void test_process_poll_reads_status_sidecar()
{
    const std::string suffix =
        std::to_string(static_cast<long long>(getpid())) + "_status";
    const std::filesystem::path status_path =
        std::filesystem::temp_directory_path() /
        ("orange_external_recorder_status_" + suffix + ".json");
    std::ostringstream full_status_json_out;
    {
        std::ostringstream& out = full_status_json_out;
        out
            << "{\n"
            << "  \"schema_id\": \"orange.external_recorder.status\",\n"
            << "  \"schema_version\": 1,\n"
            << "  \"status\": \"running\",\n"
            << "  \"stream_kind\": \"crop\",\n"
            << "  \"output_kind\": \"crop\",\n"
            << "  \"steady_clock_ns\": 12345,\n"
            << "  \"heartbeat_sequence\": 7,\n"
            << "  \"frames_received\": 11,\n"
            << "  \"acks_sent\": 10,\n"
            << "  \"detach_copied\": 9,\n"
            << "  \"encode_enqueued\": 8,\n"
            << "  \"encode_skipped\": 1,\n"
            << "  \"encode_dropped\": 0,\n"
            << "  \"encode_queue_high_water\": 3,\n"
            << "  \"frames_encoded\": 8,\n"
            << "  \"frames_dropped\": 0,\n"
            << "  \"rolling\": {\n"
            << "    \"enabled\": true,\n"
            << "    \"record_for_seconds\": 6,\n"
            << "    \"clip_seconds\": 2,\n"
            << "    \"clip_span_frames\": 200,\n"
            << "    \"target_frame_count\": 600,\n"
            << "    \"current_clip_index\": 1,\n"
            << "    \"next_rollover_at_recording_frame_id\": 401,\n"
            << "    \"frames_until_next_rollover\": 37,\n"
            << "    \"completed_clip_count\": 1,\n"
            << "    \"last_completed_clip_index\": 0,\n"
            << "    \"last_completed_clip_first_recording_frame_id\": 1,\n"
            << "    \"last_completed_clip_last_recording_frame_id\": 200,\n"
            << "    \"last_completed_clip_frame_count\": 200,\n"
            << "    \"last_completed_clip_packets_written\": 207,\n"
            << "    \"last_rollover_status\": \"completed\"\n"
            << "  },\n"
            << "  \"storage_preflight\": {\n"
            << "    \"checked\": true,\n"
            << "    \"ok\": true,\n"
            << "    \"low_space\": false,\n"
            << "    \"min_free_bytes\": 123456789,\n"
            << "    \"low_space_warning_bytes\": 234567890,\n"
            << "    \"paths\": [\n"
            << "      {\n"
            << "        \"path\": \"/tmp/orange_external_recorder_status_test\",\n"
            << "        \"ok\": true,\n"
            << "        \"meets_min_free\": true,\n"
            << "        \"below_warning\": false,\n"
            << "        \"capacity_bytes\": 1000000000,\n"
            << "        \"free_bytes\": 900000000,\n"
            << "        \"available_bytes\": 800000000,\n"
            << "        \"error\": \"\"\n"
            << "      }\n"
            << "    ]\n"
            << "  },\n"
            << "  \"ipc_protocol\": {\n"
            << "    \"name\": \"orange.external_recorder.ipc\",\n"
            << "    \"version\": 1,\n"
            << "    \"recorder_hello_sent\": true,\n"
            << "    \"client_hello_received\": true,\n"
            << "    \"recorder_status_messages_sent\": 3,\n"
            << "    \"recorder_status_send_failures\": 0,\n"
            << "    \"client_control_messages_received\": 2,\n"
            << "    \"client_drain_messages_received\": 1,\n"
            << "    \"client_finalize_messages_received\": 1,\n"
            << "    \"client_drain_received\": true,\n"
            << "    \"client_finalize_received\": true,\n"
            << "    \"client_drain_first_frame_count\": 8,\n"
            << "    \"client_finalize_frame_count\": 8,\n"
            << "    \"client_control_state\": \"finalize_requested\",\n"
            << "    \"descriptor_intake_end_reason\": \"client_finalize\",\n"
            << "    \"descriptor_intake_completed_cleanly\": true,\n"
            << "    \"last_client_control_command\": \"finalize\",\n"
            << "    \"last_client_control_reason\": \"worker_drained\"\n"
            << "  },\n"
            << "  \"worker_failed\": false\n"
            << "}\n";
    }
    const std::string full_status_json = full_status_json_out.str();
    {
        std::ofstream out(status_path);
        out << full_status_json;
    }

    SupervisorRuntimeState runtime;
    runtime.artifact_root = "/tmp/orange_external_recorder_supervisor_tests";
    runtime.session_id = "status_session";
    runtime.processes.push_back({});
    runtime.processes[0].stream_id = "2010096";
    runtime.processes[0].camera_serial = "2010096";
    runtime.processes[0].status_json_path = status_path.string();
    runtime.processes[0].recorder_status.path = status_path.string();

    std::string error;
    require(PollSupervisorProcesses(&runtime, &error),
            "status sidecar polling should not fail: " + error);
    require(runtime.processes[0].recorder_status.present,
            "status sidecar should be present");
    require(runtime.processes[0].recorder_status.valid,
            "status sidecar should be valid");
    require(runtime.processes[0].recorder_status.status == "running",
            "status sidecar status should parse");
    require(runtime.processes[0].recorder_status.stream_kind == "crop",
            "status sidecar stream kind should parse");
    require(runtime.processes[0].recorder_status.output_kind == "crop",
            "status sidecar output kind should parse");
    require(runtime.processes[0].recorder_status.heartbeat_sequence == 7,
            "status sidecar heartbeat should parse");
    require(runtime.processes[0].recorder_status.frames_received == 11,
            "status sidecar received count should parse");
    require(runtime.processes[0].recorder_status.frames_encoded == 8,
            "status sidecar encoded count should parse");
    require(runtime.processes[0].recorder_status.rolling_enabled,
            "status sidecar rolling enabled should parse");
    require(runtime.processes[0].recorder_status.rolling_current_clip_index == 1,
            "status sidecar rolling current clip should parse");
    require(runtime.processes[0].recorder_status.rolling_frames_until_next_rollover == 37,
            "status sidecar rolling frame countdown should parse");
    require(runtime.processes[0].recorder_status.rolling_completed_clip_count == 1,
            "status sidecar rolling completed clip count should parse");
    require(runtime.processes[0].recorder_status.rolling_last_completed_clip_index == 0,
            "status sidecar rolling last completed clip should parse");
    require(runtime.processes[0].recorder_status
                    .rolling_last_completed_clip_first_recording_frame_id == 1,
            "status sidecar rolling last completed clip first frame id should parse");
    require(runtime.processes[0].recorder_status
                    .rolling_last_completed_clip_packets_written == 207,
            "status sidecar rolling last completed clip packets written should parse");
    require(runtime.processes[0].recorder_status.rolling_last_rollover_status == "completed",
            "status sidecar rolling last rollover status should parse");
    require(runtime.processes[0].recorder_status.storage_checked,
            "status sidecar storage checked should parse");
    require(runtime.processes[0].recorder_status.storage_ok,
            "status sidecar storage ok should parse");
    require(!runtime.processes[0].recorder_status.storage_low_space,
            "status sidecar storage low-space flag should parse");
    require(runtime.processes[0].recorder_status.storage_min_free_bytes == 123456789ULL,
            "status sidecar min free bytes should parse");
    require(runtime.processes[0].recorder_status.storage_low_space_warning_bytes == 234567890ULL,
            "status sidecar low-space warning bytes should parse");
    require(runtime.processes[0].recorder_status.storage_path_count == 1,
            "status sidecar storage path count should parse");
    require(runtime.processes[0].recorder_status.storage_paths_ok_count == 1,
            "status sidecar storage path ok count should parse");
    require(runtime.processes[0].recorder_status.storage_paths_low_space_count == 0,
            "status sidecar low-space path count should parse");
    require(runtime.processes[0].recorder_status.storage_has_min_available_bytes,
            "status sidecar storage min available flag should parse");
    require(runtime.processes[0].recorder_status.storage_min_available_bytes == 800000000ULL,
            "status sidecar storage min available bytes should parse");
    require(runtime.processes[0].recorder_status.ipc_protocol_name ==
                "orange.external_recorder.ipc",
            "status sidecar IPC protocol name should parse");
    require(runtime.processes[0].recorder_status.ipc_protocol_version == 1,
            "status sidecar IPC protocol version should parse");
    require(runtime.processes[0].recorder_status.recorder_hello_sent,
            "status sidecar recorder hello flag should parse");
    require(runtime.processes[0].recorder_status.client_hello_received,
            "status sidecar client hello flag should parse");
    require(runtime.processes[0].recorder_status.recorder_status_messages_sent == 3,
            "status sidecar recorder status message count should parse");
    require(runtime.processes[0].recorder_status.recorder_status_send_failures == 0,
            "status sidecar recorder status failure count should parse");
    require(runtime.processes[0].recorder_status.client_control_messages_received == 2,
            "status sidecar client control count should parse");
    require(runtime.processes[0].recorder_status.client_drain_messages_received == 1,
            "status sidecar client drain count should parse");
    require(runtime.processes[0].recorder_status.client_finalize_messages_received == 1,
            "status sidecar client finalize count should parse");
    require(runtime.processes[0].recorder_status.client_drain_received,
            "status sidecar client drain flag should parse");
    require(runtime.processes[0].recorder_status.client_finalize_received,
            "status sidecar client finalize flag should parse");
    require(runtime.processes[0].recorder_status.client_drain_first_frame_count == 8,
            "status sidecar client drain frame count should parse");
    require(runtime.processes[0].recorder_status.client_finalize_frame_count == 8,
            "status sidecar client finalize frame count should parse");
    require(runtime.processes[0].recorder_status.client_control_state == "finalize_requested",
            "status sidecar client control state should parse");
    require(runtime.processes[0].recorder_status.descriptor_intake_end_reason == "client_finalize",
            "status sidecar descriptor intake reason should parse");
    require(runtime.processes[0].recorder_status.descriptor_intake_completed_cleanly,
            "status sidecar descriptor intake clean flag should parse");
    require(runtime.processes[0].recorder_status.last_client_control_command == "finalize",
            "status sidecar last client control command should parse");

    // Old recorders that do not emit the per-clip completion fields must
    // still parse cleanly, with the new fields defaulting to zero.
    {
        std::ofstream out(status_path);
        out
            << "{\n"
            << "  \"schema_id\": \"orange.external_recorder.status\",\n"
            << "  \"schema_version\": 1,\n"
            << "  \"status\": \"running\",\n"
            << "  \"rolling\": {\n"
            << "    \"enabled\": true,\n"
            << "    \"completed_clip_count\": 1,\n"
            << "    \"last_completed_clip_index\": 0,\n"
            << "    \"last_completed_clip_last_recording_frame_id\": 200,\n"
            << "    \"last_completed_clip_frame_count\": 200\n"
            << "  }\n"
            << "}\n";
    }
    require(PollSupervisorProcesses(&runtime, &error),
            "old-recorder status sidecar polling should not fail: " + error);
    require(runtime.processes[0].recorder_status.rolling_last_completed_clip_frame_count == 200,
            "old-recorder status sidecar frame count should parse");
    require(runtime.processes[0].recorder_status
                    .rolling_last_completed_clip_first_recording_frame_id == 0,
            "absent first recording frame id should default to zero");
    require(runtime.processes[0].recorder_status
                    .rolling_last_completed_clip_packets_written == 0,
            "absent packets written should default to zero");
    // Restore the full sidecar for the runtime summary assertions below.
    {
        std::ofstream out(status_path);
        out << full_status_json;
    }
    require(PollSupervisorProcesses(&runtime, &error),
            "restored status sidecar polling should not fail: " + error);

    const nlohmann::json summary = SupervisorRuntimeStateToJson(runtime);
    require(summary["processes"][0]["status_json_path"] == status_path.string(),
            "runtime summary should include status sidecar path");
    require(summary["processes"][0]["recorder_status"]["valid"].get<bool>(),
            "runtime summary should include parsed status validity");
    require(summary["processes"][0]["recorder_status"]["stream_kind"] == "crop",
            "runtime summary should include parsed stream kind");
    require(summary["processes"][0]["recorder_status"]["output_kind"] == "crop",
            "runtime summary should include parsed output kind");
    require(summary["processes"][0]["recorder_status"]["heartbeat_sequence"] == 7,
            "runtime summary should include parsed heartbeat");
    require(summary["processes"][0]["recorder_status"]["frames_encoded"] == 8,
            "runtime summary should include parsed encoded count");
    require(summary["processes"][0]["recorder_status"]["recorder_status_messages_sent"] == 3,
            "runtime summary should include parsed recorder status message count");
    require(summary["processes"][0]["recorder_status"]["client_drain_messages_received"] == 1,
            "runtime summary should include parsed client drain count");
    require(summary["processes"][0]["recorder_status"]["client_finalize_messages_received"] == 1,
            "runtime summary should include parsed client finalize count");
    require(summary["processes"][0]["recorder_status"]["client_drain_received"].get<bool>(),
            "runtime summary should include parsed client drain flag");
    require(summary["processes"][0]["recorder_status"]["client_finalize_received"].get<bool>(),
            "runtime summary should include parsed client finalize flag");
    require(summary["processes"][0]["recorder_status"]["client_drain_first_frame_count"] == 8,
            "runtime summary should include parsed client drain frame count");
    require(summary["processes"][0]["recorder_status"]["client_finalize_frame_count"] == 8,
            "runtime summary should include parsed client finalize frame count");
    require(summary["processes"][0]["recorder_status"]["client_control_state"] == "finalize_requested",
            "runtime summary should include parsed client control state");
    require(summary["processes"][0]["recorder_status"]["descriptor_intake_end_reason"] == "client_finalize",
            "runtime summary should include parsed descriptor intake reason");
    require(summary["processes"][0]["recorder_status"]["descriptor_intake_completed_cleanly"].get<bool>(),
            "runtime summary should include parsed descriptor intake clean flag");
    require(summary["processes"][0]["recorder_status"]["rolling_enabled"].get<bool>(),
            "runtime summary should include parsed rolling enabled");
    require(summary["processes"][0]["recorder_status"]["rolling_current_clip_index"] == 1,
            "runtime summary should include parsed rolling clip index");
    require(summary["processes"][0]["recorder_status"]["rolling_frames_until_next_rollover"] == 37,
            "runtime summary should include parsed rolling countdown");
    require(summary["processes"][0]["recorder_status"]["rolling_completed_clip_count"] == 1,
            "runtime summary should include parsed rolling completed clip count");
    require(summary["processes"][0]["recorder_status"]["rolling_last_completed_clip_index"] == 0,
            "runtime summary should include parsed rolling last completed clip");
    require(summary["processes"][0]["recorder_status"]["rolling_last_rollover_status"] == "completed",
            "runtime summary should include parsed rolling last rollover status");
    require(summary["processes"][0]["recorder_status"]["storage_checked"].get<bool>(),
            "runtime summary should include parsed storage checked flag");
    require(summary["processes"][0]["recorder_status"]["storage_ok"].get<bool>(),
            "runtime summary should include parsed storage ok flag");
    require(!summary["processes"][0]["recorder_status"]["storage_low_space"].get<bool>(),
            "runtime summary should include parsed low-space flag");
    require(summary["processes"][0]["recorder_status"]["storage_min_free_bytes"] == 123456789ULL,
            "runtime summary should include parsed min free bytes");
    require(summary["processes"][0]["recorder_status"]["storage_low_space_warning_bytes"] == 234567890ULL,
            "runtime summary should include parsed low-space warning bytes");
    require(summary["processes"][0]["recorder_status"]["storage_path_count"] == 1,
            "runtime summary should include parsed storage path count");
    require(summary["processes"][0]["recorder_status"]["storage_paths_ok_count"] == 1,
            "runtime summary should include parsed storage path ok count");
    require(summary["processes"][0]["recorder_status"]["storage_paths_low_space_count"] == 0,
            "runtime summary should include parsed storage low-space path count");
    require(summary["processes"][0]["recorder_status"]["storage_has_min_available_bytes"].get<bool>(),
            "runtime summary should include parsed storage min available flag");
    require(summary["processes"][0]["recorder_status"]["storage_min_available_bytes"] == 800000000ULL,
            "runtime summary should include parsed storage min available bytes");


    std::filesystem::remove(status_path);
}

void test_supervised_lifecycle_writes_artifacts_and_env()
{
    const std::filesystem::path stub_path =
        g_binary_dir / "external_recorder_supervisor_socket_stub";
    require(std::filesystem::exists(stub_path), "socket stub helper is missing");

    const std::string suffix = std::to_string(static_cast<long long>(getpid()));
    const std::filesystem::path root =
        std::filesystem::temp_directory_path() /
        ("orange_external_recorder_lifecycle_test_" + suffix);
    std::filesystem::remove_all(root);

    nlohmann::json contract = make_contract({5}, "single_shard");
    contract["artifact_root"] = root.string();
    contract["session_id"] = "lifecycle_session";
    contract["streams"]["2010096"]["socket_path"] =
        "/tmp/orange_external_recorder_lifecycle_" + suffix + ".sock";
    contract["streams"]["2010096"]["summary_json"] =
        (root / "Cam2010096_external_summary.json").string();
    contract["streams"]["2010096"]["video_sanity_json"] =
        (root / "Cam2010096_external_video_sanity.json").string();
    contract["streams"]["2010096"]["mp4"] =
        (root / "Cam2010096_external.mp4").string();
    contract["streams"]["2010096"]["gop_routing_csv"] =
        (root / "Cam2010096_external_gop_routing.csv").string();

    SupervisedRecorderLifecycleOptions options;
    options.contract = contract;
    options.recorder_tool_path = stub_path.string();
    options.default_session_id = "lifecycle_session";
    options.analytics_root = "/tmp/orange_analytics_lifecycle";
    options.verifier_path = "/repo/scripts/verify_external_recorder_session.py";
    options.process_options.socket_ready_timeout_ms = 2000;
    options.process_options.graceful_shutdown_timeout_ms = 20;
    options.process_options.terminate_timeout_ms = 2000;
    options.process_options.allow_regular_file_socket_ready_for_tests = true;

    const char* original_session_env = std::getenv("ORANGE_EXTERNAL_RECORDER_SESSION_ID");
    const std::string original_session_value = original_session_env ? original_session_env : "";
    const bool had_original_session = original_session_env != nullptr;
    const char* original_socket_env =
        std::getenv("ORANGE_EXTERNAL_RECORDER_SOCKET_CAM_2010096");
    const std::string original_socket_value = original_socket_env ? original_socket_env : "";
    const bool had_original_socket = original_socket_env != nullptr;

    SupervisedRecorderLifecycleState state;
    std::string error;
    require(StartSupervisedRecorderLifecycle(options, &state, &error),
            "supervised lifecycle should start: " + error);
    require(state.started, "lifecycle state should be started");
    require(state.plan.streams.size() == 1, "lifecycle plan should have one stream");
    require(std::filesystem::exists(root / "external_recorder_session.json"),
            "lifecycle session artifact should exist");
    require(std::filesystem::exists(root / "external_recorder_supervisor_plan.json"),
            "lifecycle plan artifact should exist");

    const char* session_env = std::getenv("ORANGE_EXTERNAL_RECORDER_SESSION_ID");
    require(session_env && std::string(session_env) == "lifecycle_session",
            "global recorder session env should be set");
    const char* socket_env = std::getenv("ORANGE_EXTERNAL_RECORDER_SOCKET_CAM_2010096");
    require(socket_env &&
                std::string(socket_env) ==
                    contract["streams"]["2010096"]["socket_path"].get<std::string>(),
            "per-camera socket env should be set");
    const char* stream_socket_env =
        std::getenv("ORANGE_EXTERNAL_RECORDER_SOCKET_STREAM_2010096");
    require(stream_socket_env &&
                std::string(stream_socket_env) ==
                    contract["streams"]["2010096"]["socket_path"].get<std::string>(),
            "per-stream socket env should be set");

    require(StopSupervisedRecorderLifecycle(&state, &error),
            "supervised lifecycle should stop: " + error);
    require(!state.started, "lifecycle state should be stopped");
    const char* restored_session_env = std::getenv("ORANGE_EXTERNAL_RECORDER_SESSION_ID");
    require((had_original_session &&
             restored_session_env &&
             std::string(restored_session_env) == original_session_value) ||
                (!had_original_session && restored_session_env == nullptr),
            "global recorder session env should be restored");
    const char* restored_socket_env =
        std::getenv("ORANGE_EXTERNAL_RECORDER_SOCKET_CAM_2010096");
    require((had_original_socket &&
             restored_socket_env &&
             std::string(restored_socket_env) == original_socket_value) ||
                (!had_original_socket && restored_socket_env == nullptr),
            "per-camera socket env should be restored");
    require(std::filesystem::exists(root / "external_recorder_supervisor_runtime.json"),
            "runtime artifact should exist");
    require(std::filesystem::exists(root / "external_recorder_verifier_handoff.json"),
            "handoff artifact should exist");

    std::filesystem::remove_all(root);
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
        {"static_dish_prior_builds_geometry_command",
         test_static_dish_prior_builds_geometry_command},
        {"static_dish_prior_requires_valid_circle",
         test_static_dish_prior_requires_valid_circle},
        {"vbr_cq_flows_to_recorder_and_plan",
         test_vbr_cq_flows_to_recorder_and_plan},
        {"invalid_external_rate_control_is_rejected",
         test_invalid_external_rate_control_is_rejected},
        {"crop_stream_plan_uses_real_camera_serial_and_env_key",
         test_crop_stream_plan_uses_real_camera_serial_and_env_key},
        {"two_shard_plan_builds_gop_modulo_command", test_two_shard_plan_builds_gop_modulo_command},
        {"preserve_shard_mp4s_opt_in", test_preserve_shard_mp4s_opt_in},
        {"spec_requires_external_ipc_sink", test_spec_requires_external_ipc_sink},
        {"spec_requires_selected_stream", test_spec_requires_selected_stream},
        {"spec_selected_stream_ignores_crop_output", test_spec_selected_stream_ignores_crop_output},
        {"spec_recording_control_flows_to_command", test_spec_recording_control_flows_to_command},
        {"storage_thresholds_flow_to_command_and_plan", test_storage_thresholds_flow_to_command_and_plan},
        {"invalid_shard_policy_fails", test_invalid_shard_policy_fails},
        {"process_lifecycle_waits_socket_and_stops", test_process_lifecycle_waits_socket_and_stops},
        {"process_poll_detects_unexpected_signal_exit", test_process_poll_detects_unexpected_signal_exit},
        {"process_poll_reads_status_sidecar", test_process_poll_reads_status_sidecar},
        {"supervised_lifecycle_writes_artifacts_and_env", test_supervised_lifecycle_writes_artifacts_and_env},
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
