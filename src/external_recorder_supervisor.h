#pragma once

#include "json.hpp"

#include <cstdint>
#include <sys/types.h>
#include <string>
#include <vector>

namespace orange::external_recorder {

struct SupervisorPlanOptions {
    std::string recorder_tool_path = "external_recorder_ipc_probe";
    std::string default_session_id;
    int default_encode_fps = 100;
    int default_encode_max_fps = 0;
    int default_encode_queue_depth = 32;
    int default_prewarm_slots = 4;
    uint64_t default_prewarm_bytes = 0;
    bool default_prewarm_peer_copy = true;
    std::string default_codec = "hevc";
    std::string default_preset = "p1";
    std::string default_tuning = "ll";
    int default_gop = 25;
    uint64_t default_bitrate_bps = 150000000;
    uint64_t default_max_bitrate_bps = 150000000;
    uint64_t default_vbv_buffer_size = 150000000;
};

struct RecorderStreamPlan {
    std::string contract_key;
    std::string stream_id;
    std::string camera_serial;
    int analytics_gpu_id = -1;
    int recorder_gpu_id = -1;
    std::vector<int> expected_shard_gpu_ids;
    std::string routing_policy = "single_shard";
    std::string socket_path;
    std::string summary_json;
    std::string video_sanity_json;
    std::string mp4;
    std::string mp4_keyframe;
    std::string detach_csv;
    std::string encode_csv;
    std::string gop_routing_csv;
    std::string recorder_log;
    int record_for_seconds = 0;
    int clip_seconds = 0;
    int encode_fps = 100;
    int encode_max_fps = 0;
    int encode_queue_depth = 32;
    int prewarm_slots = 4;
    uint64_t prewarm_bytes = 0;
    bool prewarm_peer_copy = true;
    std::string codec = "hevc";
    std::string preset = "p1";
    std::string tuning = "ll";
    int gop = 25;
    uint64_t bitrate_bps = 150000000;
    uint64_t max_bitrate_bps = 150000000;
    uint64_t vbv_buffer_size = 150000000;
    int shard_id = 0;
};

struct SupervisorPlan {
    std::string schema_id = "orange.external_recorder.supervisor_plan";
    int schema_version = 1;
    std::string recorder_tool_path;
    std::string source_path;
    std::string mode;
    std::string artifact_root;
    std::string session_id;
    bool require_summary = true;
    bool require_video_sanity = true;
    bool require_merged_mp4 = true;
    bool require_gop_routing = true;
    std::vector<RecorderStreamPlan> streams;
};

struct SupervisorProcessOptions {
    int socket_ready_timeout_ms = 5000;
    int graceful_shutdown_timeout_ms = 30000;
    int terminate_timeout_ms = 5000;
    bool unlink_existing_sockets = true;
    bool allow_regular_file_socket_ready_for_tests = false;
};

struct RecorderProcessState {
    std::string stream_id;
    std::string camera_serial;
    std::string socket_path;
    std::string log_path;
    std::vector<std::string> argv;
    pid_t pid = -1;
    bool active = false;
    bool socket_ready = false;
    bool termination_requested = false;
    int exit_code = -1;
    int term_signal = 0;
    std::string status = "not_started";
    std::string error;
};

struct SupervisorRuntimeState {
    std::string schema_id = "orange.external_recorder.supervisor_runtime";
    int schema_version = 1;
    std::string artifact_root;
    std::string session_id;
    std::vector<RecorderProcessState> processes;
};

bool BuildSupervisorPlanFromContract(const nlohmann::json& contract,
                                     const SupervisorPlanOptions& options,
                                     SupervisorPlan* plan_out,
                                     std::string* error_out);

bool BuildSupervisorPlanFromExperimentSpec(const nlohmann::json& experiment_spec,
                                           const SupervisorPlanOptions& options,
                                           SupervisorPlan* plan_out,
                                           std::string* error_out);

std::vector<std::string> BuildRecorderCommand(const SupervisorPlan& plan,
                                              const RecorderStreamPlan& stream);

nlohmann::json SupervisorPlanToJson(const SupervisorPlan& plan);

bool StartSupervisorProcesses(const SupervisorPlan& plan,
                              const SupervisorProcessOptions& options,
                              SupervisorRuntimeState* runtime_out,
                              std::string* error_out);

bool StopSupervisorProcesses(SupervisorRuntimeState* runtime,
                             const SupervisorProcessOptions& options,
                             std::string* error_out);

nlohmann::json SupervisorRuntimeStateToJson(const SupervisorRuntimeState& runtime);

}  // namespace orange::external_recorder
