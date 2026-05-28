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
    uint64_t default_min_free_bytes = 0;
    uint64_t default_low_space_warning_bytes = 0;
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
    std::string status_json;
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
    uint64_t terminal_tail_coalesce_frames = 0;
    uint64_t bitrate_bps = 150000000;
    uint64_t max_bitrate_bps = 150000000;
    uint64_t vbv_buffer_size = 150000000;
    uint64_t min_free_bytes = 0;
    uint64_t low_space_warning_bytes = 0;
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
    bool require_status = true;
    bool require_status_runtime = false;
    bool require_storage_preflight = true;
    bool require_protocol_hello = true;
    std::vector<RecorderStreamPlan> streams;
};

struct SupervisorProcessOptions {
    int socket_ready_timeout_ms = 5000;
    int graceful_shutdown_timeout_ms = 30000;
    int terminate_timeout_ms = 5000;
    bool unlink_existing_sockets = true;
    bool allow_regular_file_socket_ready_for_tests = false;
};

struct RecorderStatusSnapshot {
    std::string path;
    bool present = false;
    bool valid = false;
    std::string schema_id;
    int schema_version = 0;
    std::string status;
    uint64_t steady_clock_ns = 0;
    uint64_t heartbeat_sequence = 0;
    uint64_t frames_received = 0;
    uint64_t acks_sent = 0;
    uint64_t detach_copied = 0;
    uint64_t encode_enqueued = 0;
    uint64_t encode_skipped = 0;
    uint64_t encode_dropped = 0;
    uint64_t encode_queue_high_water = 0;
    uint64_t frames_encoded = 0;
    uint64_t frames_dropped = 0;
    bool rolling_enabled = false;
    int rolling_record_for_seconds = 0;
    int rolling_clip_seconds = 0;
    uint64_t rolling_clip_span_frames = 0;
    uint64_t rolling_target_frame_count = 0;
    int rolling_current_clip_index = -1;
    uint64_t rolling_next_rollover_at_recording_frame_id = 0;
    uint64_t rolling_frames_until_next_rollover = 0;
    uint64_t rolling_completed_clip_count = 0;
    int rolling_last_completed_clip_index = -1;
    uint64_t rolling_last_completed_clip_last_recording_frame_id = 0;
    uint64_t rolling_last_completed_clip_frame_count = 0;
    std::string rolling_last_rollover_status;
    bool worker_failed = false;
    bool storage_checked = false;
    bool storage_ok = true;
    bool storage_low_space = false;
    uint64_t storage_min_free_bytes = 0;
    uint64_t storage_low_space_warning_bytes = 0;
    uint64_t storage_path_count = 0;
    uint64_t storage_paths_ok_count = 0;
    uint64_t storage_paths_low_space_count = 0;
    bool storage_has_min_available_bytes = false;
    uint64_t storage_min_available_bytes = 0;
    std::string ipc_protocol_name;
    int ipc_protocol_version = 0;
    bool recorder_hello_sent = false;
    bool client_hello_received = false;
    uint64_t recorder_status_messages_sent = 0;
    uint64_t recorder_status_send_failures = 0;
    std::string error;
};

struct RecorderProcessState {
    std::string stream_id;
    std::string camera_serial;
    std::string socket_path;
    std::string status_json_path;
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
    RecorderStatusSnapshot recorder_status;
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

bool PollSupervisorProcesses(SupervisorRuntimeState* runtime,
                             std::string* error_out);

nlohmann::json SupervisorRuntimeStateToJson(const SupervisorRuntimeState& runtime);

}  // namespace orange::external_recorder
