#include "external_recorder_supervisor.h"

#include "fsuid_guard.h"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <cstdio>
#include <filesystem>
#include <fcntl.h>
#include <fstream>
#include <limits>
#include <signal.h>
#include <sstream>
#include <sys/stat.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

namespace orange::external_recorder {
namespace {

bool set_error(std::string* error_out, const std::string& message)
{
    if (error_out) {
        *error_out = message;
    }
    return false;
}

std::string normalize_token(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(), [](const unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return value;
}

std::string path_join(const std::string& root, const std::string& leaf)
{
    if (root.empty()) {
        return leaf;
    }
    return (std::filesystem::path(root) / leaf).string();
}

bool ends_with(const std::string& value, const std::string& suffix)
{
    return value.size() >= suffix.size() &&
           value.compare(value.size() - suffix.size(), suffix.size(), suffix) == 0;
}

std::string replace_suffix(const std::string& value,
                           const std::string& suffix,
                           const std::string& replacement)
{
    if (!ends_with(value, suffix)) {
        return value;
    }
    return value.substr(0, value.size() - suffix.size()) + replacement;
}

std::string derive_keyframe_path(const std::string& mp4_path)
{
    if (mp4_path.empty()) {
        return {};
    }
    const std::filesystem::path path(mp4_path);
    return (path.parent_path() / (path.stem().string() + "_keyframes.json")).string();
}

std::string join_gpu_ids(const std::vector<int>& gpu_ids)
{
    std::ostringstream out;
    for (size_t i = 0; i < gpu_ids.size(); ++i) {
        if (i > 0) {
            out << ',';
        }
        out << gpu_ids[i];
    }
    return out.str();
}

bool socket_path_ready(const std::string& socket_path, const bool allow_regular_file)
{
    struct stat st {};
    if (socket_path.empty() || lstat(socket_path.c_str(), &st) != 0) {
        return false;
    }
    return S_ISSOCK(st.st_mode) || (allow_regular_file && S_ISREG(st.st_mode));
}

std::vector<char*> make_exec_argv(const std::vector<std::string>& argv)
{
    std::vector<char*> exec_argv;
    exec_argv.reserve(argv.size() + 1);
    for (const std::string& item : argv) {
        exec_argv.push_back(const_cast<char*>(item.c_str()));
    }
    exec_argv.push_back(nullptr);
    return exec_argv;
}

void capture_wait_status(RecorderProcessState* process, const int wait_status)
{
    if (!process) {
        return;
    }
    process->active = false;
    process->error.clear();
    if (WIFEXITED(wait_status)) {
        process->exit_code = WEXITSTATUS(wait_status);
        process->status = process->exit_code == 0 ? "exited" : "exited_with_error";
        if (process->exit_code != 0 && process->exit_code != 143) {
            process->error =
                "external recorder exited with code " + std::to_string(process->exit_code);
        }
    } else if (WIFSIGNALED(wait_status)) {
        process->term_signal = WTERMSIG(wait_status);
        process->status = process->term_signal == SIGKILL ? "killed" : "stopped_with_signal";
        if (!process->termination_requested || process->term_signal == SIGKILL) {
            process->error =
                "external recorder stopped by signal " + std::to_string(process->term_signal);
        }
    } else {
        process->status = "stopped";
    }
}

bool poll_process_exit(RecorderProcessState* process, std::string* error_out)
{
    if (!process || !process->active || process->pid <= 0) {
        return false;
    }
    int wait_status = 0;
    const pid_t wait_result = waitpid(process->pid, &wait_status, WNOHANG);
    if (wait_result == 0) {
        return false;
    }
    if (wait_result < 0) {
        process->active = false;
        process->status = "wait_failed";
        process->error = std::string("waitpid failed: ") + std::strerror(errno);
        if (error_out && error_out->empty()) {
            *error_out = process->error;
        }
        return true;
    }
    capture_wait_status(process, wait_status);
    return true;
}

uint64_t optional_u64(const nlohmann::json& node, const char* key)
{
    if (!node.contains(key)) {
        return 0;
    }
    const nlohmann::json& value = node[key];
    if (value.is_number_unsigned()) {
        return value.get<uint64_t>();
    }
    if (value.is_number_integer()) {
        const int64_t parsed = value.get<int64_t>();
        return parsed > 0 ? static_cast<uint64_t>(parsed) : 0;
    }
    return 0;
}

int optional_int(const nlohmann::json& node, const char* key)
{
    if (!node.contains(key)) {
        return 0;
    }
    const nlohmann::json& value = node[key];
    if (value.is_number_integer() || value.is_number_unsigned()) {
        return value.get<int>();
    }
    return 0;
}

std::string optional_string(const nlohmann::json& node, const char* key)
{
    if (!node.contains(key) || !node[key].is_string()) {
        return {};
    }
    return node[key].get<std::string>();
}

bool optional_bool(const nlohmann::json& node, const char* key)
{
    return node.contains(key) && node[key].is_boolean() && node[key].get<bool>();
}

void refresh_recorder_status_sidecar(RecorderProcessState* process)
{
    if (!process || process->status_json_path.empty()) {
        return;
    }

    RecorderStatusSnapshot snapshot;
    snapshot.path = process->status_json_path;

    std::error_code exists_error;
    if (!std::filesystem::exists(process->status_json_path, exists_error)) {
        process->recorder_status = std::move(snapshot);
        return;
    }

    snapshot.present = true;
    try {
        orange::ScopedFsuid fsuid_guard;
        (void)fsuid_guard;
        std::ifstream input(process->status_json_path);
        if (!input) {
            snapshot.error = "failed to open recorder status sidecar";
            process->recorder_status = std::move(snapshot);
            return;
        }

        const nlohmann::json parsed = nlohmann::json::parse(input);
        snapshot.schema_id = optional_string(parsed, "schema_id");
        snapshot.schema_version = optional_int(parsed, "schema_version");
        snapshot.status = optional_string(parsed, "status");
        snapshot.steady_clock_ns = optional_u64(parsed, "steady_clock_ns");
        snapshot.heartbeat_sequence = optional_u64(parsed, "heartbeat_sequence");
        snapshot.frames_received = optional_u64(parsed, "frames_received");
        snapshot.acks_sent = optional_u64(parsed, "acks_sent");
        snapshot.detach_copied = optional_u64(parsed, "detach_copied");
        snapshot.encode_enqueued = optional_u64(parsed, "encode_enqueued");
        snapshot.encode_skipped = optional_u64(parsed, "encode_skipped");
        snapshot.encode_dropped = optional_u64(parsed, "encode_dropped");
        snapshot.encode_queue_high_water =
            optional_u64(parsed, "encode_queue_high_water");
        snapshot.frames_encoded = optional_u64(parsed, "frames_encoded");
        snapshot.frames_dropped = optional_u64(parsed, "frames_dropped");
        if (parsed.contains("rolling") && parsed["rolling"].is_object()) {
            const nlohmann::json& rolling = parsed["rolling"];
            snapshot.rolling_enabled = optional_bool(rolling, "enabled");
            snapshot.rolling_record_for_seconds =
                optional_int(rolling, "record_for_seconds");
            snapshot.rolling_clip_seconds =
                optional_int(rolling, "clip_seconds");
            snapshot.rolling_clip_span_frames =
                optional_u64(rolling, "clip_span_frames");
            snapshot.rolling_target_frame_count =
                optional_u64(rolling, "target_frame_count");
            snapshot.rolling_current_clip_index =
                optional_int(rolling, "current_clip_index");
            snapshot.rolling_next_rollover_at_recording_frame_id =
                optional_u64(rolling, "next_rollover_at_recording_frame_id");
            snapshot.rolling_frames_until_next_rollover =
                optional_u64(rolling, "frames_until_next_rollover");
            snapshot.rolling_completed_clip_count =
                optional_u64(rolling, "completed_clip_count");
            snapshot.rolling_last_completed_clip_index =
                optional_int(rolling, "last_completed_clip_index");
            snapshot.rolling_last_completed_clip_last_recording_frame_id =
                optional_u64(rolling, "last_completed_clip_last_recording_frame_id");
            snapshot.rolling_last_completed_clip_frame_count =
                optional_u64(rolling, "last_completed_clip_frame_count");
            snapshot.rolling_last_rollover_status =
                optional_string(rolling, "last_rollover_status");
        }
        snapshot.worker_failed = optional_bool(parsed, "worker_failed");
        if (parsed.contains("storage_preflight") &&
            parsed["storage_preflight"].is_object()) {
            const nlohmann::json& storage = parsed["storage_preflight"];
            snapshot.storage_checked = optional_bool(storage, "checked");
            snapshot.storage_ok =
                !storage.contains("ok") || optional_bool(storage, "ok");
            snapshot.storage_low_space = optional_bool(storage, "low_space");
            snapshot.storage_min_free_bytes =
                optional_u64(storage, "min_free_bytes");
            snapshot.storage_low_space_warning_bytes =
                optional_u64(storage, "low_space_warning_bytes");
            if (storage.contains("paths") && storage["paths"].is_array()) {
                for (const nlohmann::json& path : storage["paths"]) {
                    if (!path.is_object()) {
                        continue;
                    }
                    ++snapshot.storage_path_count;
                    if (optional_bool(path, "ok")) {
                        ++snapshot.storage_paths_ok_count;
                    }
                    if (optional_bool(path, "below_warning")) {
                        ++snapshot.storage_paths_low_space_count;
                    }
                    if (path.contains("available_bytes") &&
                        (path["available_bytes"].is_number_unsigned() ||
                         path["available_bytes"].is_number_integer())) {
                        const uint64_t available_bytes =
                            optional_u64(path, "available_bytes");
                        if (!snapshot.storage_has_min_available_bytes ||
                            available_bytes < snapshot.storage_min_available_bytes) {
                            snapshot.storage_min_available_bytes = available_bytes;
                            snapshot.storage_has_min_available_bytes = true;
                        }
                    }
                }
            }
        }
        snapshot.error = optional_string(parsed, "error");
        snapshot.valid =
            snapshot.schema_id == "orange.external_recorder.status" &&
            snapshot.schema_version == 1 &&
            !snapshot.status.empty();
        if (!snapshot.valid && snapshot.error.empty()) {
            snapshot.error = "recorder status sidecar has unexpected schema";
        }
    } catch (const std::exception& ex) {
        snapshot.valid = false;
        snapshot.error = std::string("failed to parse recorder status sidecar: ") +
                         ex.what();
    }

    process->recorder_status = std::move(snapshot);
}

bool all_processes_inactive(const SupervisorRuntimeState& runtime)
{
    for (const RecorderProcessState& process : runtime.processes) {
        if (process.active) {
            return false;
        }
    }
    return true;
}

bool read_string_field(const nlohmann::json& node,
                       const std::string& key,
                       std::string* out,
                       std::string* error_out,
                       const std::string& context,
                       const bool required = false)
{
    if (!node.contains(key)) {
        if (required) {
            return set_error(error_out, context + "." + key + " is required");
        }
        return true;
    }
    if (!node[key].is_string()) {
        return set_error(error_out, context + "." + key + " must be a string");
    }
    *out = node[key].get<std::string>();
    if (required && out->empty()) {
        return set_error(error_out, context + "." + key + " must not be empty");
    }
    return true;
}

bool read_bool_field(const nlohmann::json& node,
                     const std::string& key,
                     bool* out,
                     std::string* error_out,
                     const std::string& context)
{
    if (!node.contains(key)) {
        return true;
    }
    if (!node[key].is_boolean()) {
        return set_error(error_out, context + "." + key + " must be a boolean");
    }
    *out = node[key].get<bool>();
    return true;
}

bool read_int_field(const nlohmann::json& node,
                    const std::string& key,
                    int* out,
                    std::string* error_out,
                    const std::string& context,
                    const int min_value = 0)
{
    if (!node.contains(key)) {
        return true;
    }
    if (!node[key].is_number_integer() && !node[key].is_number_unsigned()) {
        return set_error(error_out, context + "." + key + " must be an integer");
    }
    const int64_t value = node[key].is_number_unsigned()
                              ? static_cast<int64_t>(node[key].get<uint64_t>())
                              : node[key].get<int64_t>();
    if (value < min_value || value > std::numeric_limits<int>::max()) {
        return set_error(error_out, context + "." + key + " is out of range");
    }
    *out = static_cast<int>(value);
    return true;
}

bool read_u64_field(const nlohmann::json& node,
                    const std::string& key,
                    uint64_t* out,
                    std::string* error_out,
                    const std::string& context)
{
    if (!node.contains(key)) {
        return true;
    }
    if (!node[key].is_number_unsigned() && !node[key].is_number_integer()) {
        return set_error(error_out, context + "." + key + " must be a non-negative integer");
    }
    if (node[key].is_number_integer() && node[key].get<int64_t>() < 0) {
        return set_error(error_out, context + "." + key + " must be a non-negative integer");
    }
    *out = node[key].is_number_unsigned()
               ? node[key].get<uint64_t>()
               : static_cast<uint64_t>(node[key].get<int64_t>());
    return true;
}

bool read_gpu_id_array(const nlohmann::json& node,
                       const std::string& key,
                       std::vector<int>* out,
                       std::string* error_out,
                       const std::string& context)
{
    if (!node.contains(key)) {
        return true;
    }
    if (!node[key].is_array()) {
        return set_error(error_out, context + "." + key + " must be an array");
    }
    std::vector<int> values;
    for (size_t i = 0; i < node[key].size(); ++i) {
        if (!node[key][i].is_number_integer() && !node[key][i].is_number_unsigned()) {
            return set_error(error_out, context + "." + key + " entries must be integers");
        }
        const int64_t value = node[key][i].is_number_unsigned()
                                  ? static_cast<int64_t>(node[key][i].get<uint64_t>())
                                  : node[key][i].get<int64_t>();
        if (value < 0 || value > std::numeric_limits<int>::max()) {
            return set_error(error_out, context + "." + key + " entries must be non-negative");
        }
        values.push_back(static_cast<int>(value));
    }
    *out = std::move(values);
    return true;
}

bool read_recording_control(const nlohmann::json& node,
                            int* record_for_seconds,
                            int* clip_seconds,
                            std::string* error_out,
                            const std::string& context)
{
    if (!node.contains("recording_control")) {
        return true;
    }
    if (!node["recording_control"].is_object()) {
        return set_error(error_out, context + ".recording_control must be an object");
    }
    const nlohmann::json& recording_control = node["recording_control"];
    return read_int_field(recording_control,
                          "record_for_seconds",
                          record_for_seconds,
                          error_out,
                          context + ".recording_control",
                          0) &&
           read_int_field(recording_control,
                          "clip_seconds",
                          clip_seconds,
                          error_out,
                          context + ".recording_control",
                          0);
}

bool append_selection_camera_serials(const nlohmann::json& experiment_spec,
                                     std::vector<std::string>* camera_serials,
                                     std::string* error_out)
{
    if (!experiment_spec.contains("selection")) {
        return true;
    }
    const nlohmann::json& selection = experiment_spec["selection"];
    if (!selection.is_object() || !selection.contains("camera_serials")) {
        return true;
    }
    if (!selection["camera_serials"].is_array()) {
        return set_error(error_out, "selection.camera_serials must be an array");
    }
    for (size_t i = 0; i < selection["camera_serials"].size(); ++i) {
        if (!selection["camera_serials"][i].is_string()) {
            return set_error(error_out, "selection.camera_serials entries must be strings");
        }
        camera_serials->push_back(selection["camera_serials"][i].get<std::string>());
    }
    return true;
}

bool stream_key_exists(const nlohmann::json& streams, const std::string& serial)
{
    if (streams.contains(serial)) {
        return true;
    }
    for (auto it = streams.begin(); it != streams.end(); ++it) {
        if (it.value().is_object() &&
            it.value().value("camera_serial", std::string()) == serial) {
            return true;
        }
    }
    return false;
}

}  // namespace

bool BuildSupervisorPlanFromContract(const nlohmann::json& contract,
                                     const SupervisorPlanOptions& options,
                                     SupervisorPlan* plan_out,
                                     std::string* error_out)
{
    if (!plan_out) {
        return set_error(error_out, "internal error: null supervisor plan destination");
    }
    if (!contract.is_object()) {
        return set_error(error_out, "external_recorder_contract must be a JSON object");
    }

    SupervisorPlan plan;
    plan.recorder_tool_path = options.recorder_tool_path;
    plan.mode = "off";
    if (!read_string_field(contract,
                           "mode",
                           &plan.mode,
                           error_out,
                           "external_recorder_contract")) {
        return false;
    }
    plan.mode = normalize_token(plan.mode);
    if (plan.mode == "true" || plan.mode == "on") {
        plan.mode = "diagnostic_ipc_v1";
    }
    if (plan.mode != "diagnostic_ipc_v1") {
        return set_error(error_out,
                         "external_recorder_contract.mode must be diagnostic_ipc_v1 for a supervisor plan");
    }

    std::string schema_id = "orange.external_recorder.contract";
    if (!read_string_field(contract,
                           "schema_id",
                           &schema_id,
                           error_out,
                           "external_recorder_contract")) {
        return false;
    }
    if (schema_id != "orange.external_recorder.contract") {
        return set_error(error_out,
                         "external_recorder_contract.schema_id must be orange.external_recorder.contract");
    }
    int schema_version = 1;
    if (!read_int_field(contract,
                        "schema_version",
                        &schema_version,
                        error_out,
                        "external_recorder_contract",
                        1)) {
        return false;
    }
    if (schema_version != 1) {
        return set_error(error_out, "external_recorder_contract.schema_version must be 1");
    }

    if (!read_string_field(contract,
                           "artifact_root",
                           &plan.artifact_root,
                           error_out,
                           "external_recorder_contract",
                           true) ||
        !read_string_field(contract,
                           "session_id",
                           &plan.session_id,
                           error_out,
                           "external_recorder_contract")) {
        return false;
    }
    if (plan.session_id.empty()) {
        plan.session_id = options.default_session_id;
    }
    if (plan.session_id.empty()) {
        return set_error(error_out, "external_recorder_contract.session_id is required");
    }
    plan.require_status_runtime = contract.value("supervise_processes", false);
    if (!read_bool_field(contract,
                         "require_summary",
                         &plan.require_summary,
                         error_out,
                         "external_recorder_contract") ||
        !read_bool_field(contract,
                         "require_video_sanity",
                         &plan.require_video_sanity,
                         error_out,
                         "external_recorder_contract") ||
        !read_bool_field(contract,
                         "require_merged_mp4",
                         &plan.require_merged_mp4,
                         error_out,
                         "external_recorder_contract") ||
        !read_bool_field(contract,
                         "require_gop_routing",
                         &plan.require_gop_routing,
                         error_out,
                         "external_recorder_contract") ||
        !read_bool_field(contract,
                         "require_status",
                         &plan.require_status,
                         error_out,
                         "external_recorder_contract") ||
        !read_bool_field(contract,
                         "require_status_runtime",
                         &plan.require_status_runtime,
                         error_out,
                         "external_recorder_contract")) {
        return false;
    }
    int contract_record_for_seconds = 0;
    int contract_clip_seconds = 0;
    if (!read_recording_control(contract,
                                &contract_record_for_seconds,
                                &contract_clip_seconds,
                                error_out,
                                "external_recorder_contract")) {
        return false;
    }

    if (!contract.contains("streams") || !contract["streams"].is_object()) {
        return set_error(error_out, "external_recorder_contract.streams must be an object");
    }
    const nlohmann::json& streams = contract["streams"];
    if (streams.empty()) {
        return set_error(error_out, "external_recorder_contract.streams must not be empty");
    }

    const size_t stream_count = streams.size();
    for (auto it = streams.begin(); it != streams.end(); ++it) {
        const std::string context =
            "external_recorder_contract.streams." + it.key();
        if (!it.value().is_object()) {
            return set_error(error_out, context + " must be an object");
        }
        const nlohmann::json& stream = it.value();

        RecorderStreamPlan stream_plan;
        stream_plan.contract_key = it.key();
        stream_plan.stream_id = it.key();
        if (!read_string_field(stream,
                               "stream_id",
                               &stream_plan.stream_id,
                               error_out,
                               context) ||
            !read_string_field(stream,
                               "camera_serial",
                               &stream_plan.camera_serial,
                               error_out,
                               context)) {
            return false;
        }
        if (stream_plan.camera_serial.empty()) {
            stream_plan.camera_serial = stream_plan.stream_id;
        }
        if (stream_plan.stream_id.empty()) {
            return set_error(error_out, context + ".stream_id must not be empty");
        }
        if (stream_plan.camera_serial.empty()) {
            return set_error(error_out, context + ".camera_serial must not be empty");
        }

        stream_plan.encode_fps = options.default_encode_fps;
        stream_plan.encode_max_fps = options.default_encode_max_fps;
        stream_plan.encode_queue_depth = options.default_encode_queue_depth;
        stream_plan.prewarm_slots = options.default_prewarm_slots;
        stream_plan.prewarm_bytes = options.default_prewarm_bytes;
        stream_plan.prewarm_peer_copy = options.default_prewarm_peer_copy;
        stream_plan.codec = options.default_codec;
        stream_plan.preset = options.default_preset;
        stream_plan.tuning = options.default_tuning;
        stream_plan.gop = options.default_gop;
        stream_plan.bitrate_bps = options.default_bitrate_bps;
        stream_plan.max_bitrate_bps = options.default_max_bitrate_bps;
        stream_plan.vbv_buffer_size = options.default_vbv_buffer_size;
        stream_plan.min_free_bytes = options.default_min_free_bytes;
        stream_plan.low_space_warning_bytes =
            options.default_low_space_warning_bytes;
        stream_plan.record_for_seconds = contract_record_for_seconds;
        stream_plan.clip_seconds = contract_clip_seconds;

        bool saw_recorder_gpu = stream.contains("recorder_gpu_id");
        if (!read_int_field(stream,
                            "analytics_gpu_id",
                            &stream_plan.analytics_gpu_id,
                            error_out,
                            context,
                            0) ||
            !read_int_field(stream,
                            "recorder_gpu_id",
                            &stream_plan.recorder_gpu_id,
                            error_out,
                            context,
                            0) ||
            !read_gpu_id_array(stream,
                               "expected_shard_gpu_ids",
                               &stream_plan.expected_shard_gpu_ids,
                               error_out,
                               context) ||
            !read_string_field(stream,
                               "routing_policy",
                               &stream_plan.routing_policy,
                               error_out,
                               context) ||
            !read_string_field(stream,
                               "summary_json",
                               &stream_plan.summary_json,
                               error_out,
                               context) ||
            !read_string_field(stream,
                               "status_json",
                               &stream_plan.status_json,
                               error_out,
                               context) ||
            !read_string_field(stream,
                               "video_sanity_json",
                               &stream_plan.video_sanity_json,
                               error_out,
                               context) ||
            !read_string_field(stream,
                               "mp4",
                               &stream_plan.mp4,
                               error_out,
                               context) ||
            !read_string_field(stream,
                               "mp4_keyframe",
                               &stream_plan.mp4_keyframe,
                               error_out,
                               context) ||
            !read_string_field(stream,
                               "detach_csv",
                               &stream_plan.detach_csv,
                               error_out,
                               context) ||
            !read_string_field(stream,
                               "encode_csv",
                               &stream_plan.encode_csv,
                               error_out,
                               context) ||
            !read_string_field(stream,
                               "gop_routing_csv",
                               &stream_plan.gop_routing_csv,
                               error_out,
                               context) ||
            !read_string_field(stream,
                               "recorder_log",
                               &stream_plan.recorder_log,
                               error_out,
                               context) ||
            !read_string_field(stream,
                               "socket_path",
                               &stream_plan.socket_path,
                               error_out,
                               context) ||
            !read_recording_control(stream,
                                    &stream_plan.record_for_seconds,
                                    &stream_plan.clip_seconds,
                                    error_out,
                                    context) ||
            !read_int_field(stream,
                            "encode_fps",
                            &stream_plan.encode_fps,
                            error_out,
                            context,
                            1) ||
            !read_int_field(stream,
                            "encode_max_fps",
                            &stream_plan.encode_max_fps,
                            error_out,
                            context,
                            0) ||
            !read_int_field(stream,
                            "encode_queue_depth",
                            &stream_plan.encode_queue_depth,
                            error_out,
                            context,
                            1) ||
            !read_int_field(stream,
                            "prewarm_slots",
                            &stream_plan.prewarm_slots,
                            error_out,
                            context,
                            0) ||
            !read_u64_field(stream,
                            "prewarm_bytes",
                            &stream_plan.prewarm_bytes,
                            error_out,
                            context) ||
            !read_bool_field(stream,
                             "prewarm_peer_copy",
                             &stream_plan.prewarm_peer_copy,
                             error_out,
                             context) ||
            !read_string_field(stream, "codec", &stream_plan.codec, error_out, context) ||
            !read_string_field(stream, "preset", &stream_plan.preset, error_out, context) ||
            !read_string_field(stream, "tuning", &stream_plan.tuning, error_out, context) ||
            !read_int_field(stream, "gop", &stream_plan.gop, error_out, context, 1) ||
            !read_u64_field(stream,
                            "terminal_tail_coalesce_frames",
                            &stream_plan.terminal_tail_coalesce_frames,
                            error_out,
                            context) ||
            !read_u64_field(stream,
                            "bitrate_bps",
                            &stream_plan.bitrate_bps,
                            error_out,
                            context) ||
            !read_u64_field(stream,
                            "max_bitrate_bps",
                            &stream_plan.max_bitrate_bps,
                            error_out,
                            context) ||
            !read_u64_field(stream,
                            "vbv_buffer_size",
                            &stream_plan.vbv_buffer_size,
                            error_out,
                            context) ||
            !read_u64_field(stream,
                            "min_free_bytes",
                            &stream_plan.min_free_bytes,
                            error_out,
                            context) ||
            !read_u64_field(stream,
                            "low_space_warning_bytes",
                            &stream_plan.low_space_warning_bytes,
                            error_out,
                            context)) {
            return false;
        }

        if (stream_plan.analytics_gpu_id < 0) {
            return set_error(error_out, context + ".analytics_gpu_id is required");
        }
        if (stream_plan.expected_shard_gpu_ids.empty()) {
            if (stream_plan.recorder_gpu_id >= 0) {
                stream_plan.expected_shard_gpu_ids.push_back(stream_plan.recorder_gpu_id);
            } else {
                stream_plan.expected_shard_gpu_ids.push_back(stream_plan.analytics_gpu_id);
            }
        }
        if (!saw_recorder_gpu) {
            stream_plan.recorder_gpu_id = stream_plan.expected_shard_gpu_ids.front();
        }
        if (stream_plan.recorder_gpu_id != stream_plan.expected_shard_gpu_ids.front()) {
            return set_error(error_out,
                             context +
                                 ".recorder_gpu_id must match the first expected_shard_gpu_ids entry");
        }

        stream_plan.routing_policy = normalize_token(stream_plan.routing_policy);
        if (stream_plan.routing_policy != "single_shard" &&
            stream_plan.routing_policy != "gop_modulo") {
            return set_error(error_out,
                             context +
                                 ".routing_policy must be single_shard or gop_modulo");
        }
        if (stream_plan.routing_policy == "single_shard" &&
            stream_plan.expected_shard_gpu_ids.size() != 1) {
            return set_error(error_out,
                             context +
                                 ".routing_policy=single_shard requires exactly one shard GPU");
        }
        if (stream_plan.routing_policy == "gop_modulo" &&
            stream_plan.expected_shard_gpu_ids.size() < 2) {
            return set_error(error_out,
                             context +
                                 ".routing_policy=gop_modulo requires at least two shard GPUs");
        }
        if (stream_plan.clip_seconds > 0 && stream_plan.record_for_seconds <= 0) {
            return set_error(error_out,
                             context +
                                 ".recording_control.clip_seconds requires record_for_seconds > 0");
        }
        if (plan.require_summary && stream_plan.summary_json.empty()) {
            return set_error(error_out, context + ".summary_json is required");
        }
        if (plan.require_video_sanity && stream_plan.video_sanity_json.empty()) {
            return set_error(error_out, context + ".video_sanity_json is required");
        }
        if (stream_plan.mp4.empty()) {
            return set_error(error_out, context + ".mp4 is required");
        }
        if (plan.require_gop_routing && stream_plan.gop_routing_csv.empty()) {
            return set_error(error_out, context + ".gop_routing_csv is required");
        }

        if (stream_plan.socket_path.empty()) {
            stream_plan.socket_path =
                "/tmp/orange_external_recorder_" + stream_plan.camera_serial + ".sock";
        }
        if (stream_plan.mp4_keyframe.empty()) {
            stream_plan.mp4_keyframe = derive_keyframe_path(stream_plan.mp4);
        }
        if (stream_plan.status_json.empty()) {
            stream_plan.status_json =
                !stream_plan.summary_json.empty()
                    ? replace_suffix(stream_plan.summary_json, "_summary.json", "_status.json")
                    : path_join(plan.artifact_root,
                                "Cam" + stream_plan.camera_serial + "_external_status.json");
        }
        if (stream_plan.detach_csv.empty()) {
            stream_plan.detach_csv =
                !stream_plan.gop_routing_csv.empty()
                    ? replace_suffix(stream_plan.gop_routing_csv, "_gop_routing.csv", "_detach.csv")
                    : path_join(plan.artifact_root,
                                "Cam" + stream_plan.camera_serial + "_external_detach.csv");
        }
        if (stream_plan.encode_csv.empty()) {
            stream_plan.encode_csv =
                !stream_plan.gop_routing_csv.empty()
                    ? replace_suffix(stream_plan.gop_routing_csv, "_gop_routing.csv", "_encode.csv")
                    : path_join(plan.artifact_root,
                                "Cam" + stream_plan.camera_serial + "_external_encode.csv");
        }
        if (stream_plan.recorder_log.empty()) {
            stream_plan.recorder_log =
                path_join(plan.artifact_root,
                          stream_count == 1
                              ? "external_recorder.log"
                              : "Cam" + stream_plan.camera_serial + "_external_recorder.log");
        }

        plan.streams.push_back(std::move(stream_plan));
    }

    *plan_out = std::move(plan);
    return true;
}

bool BuildSupervisorPlanFromExperimentSpec(const nlohmann::json& experiment_spec,
                                           const SupervisorPlanOptions& options,
                                           SupervisorPlan* plan_out,
                                           std::string* error_out)
{
    if (!experiment_spec.is_object()) {
        return set_error(error_out, "experiment spec must be a JSON object");
    }
    if (!experiment_spec.contains("fixed") || !experiment_spec["fixed"].is_object()) {
        return set_error(error_out, "experiment spec fixed object is required");
    }
    const nlohmann::json& fixed = experiment_spec["fixed"];
    std::string recording_sink_mode = "real";
    if (!read_string_field(fixed,
                           "recording_sink_mode",
                           &recording_sink_mode,
                           error_out,
                           "fixed")) {
        return false;
    }
    recording_sink_mode = normalize_token(recording_sink_mode);
    if (recording_sink_mode != "external_ipc") {
        return set_error(error_out,
                         "fixed.external_recorder_contract requires fixed.recording_sink_mode=external_ipc");
    }
    if (!fixed.contains("external_recorder_contract")) {
        return set_error(error_out, "fixed.external_recorder_contract is required");
    }

    SupervisorPlanOptions options_with_spec = options;
    if (options_with_spec.default_session_id.empty()) {
        if (!read_string_field(experiment_spec,
                               "experiment_id",
                               &options_with_spec.default_session_id,
                               error_out,
                               "experiment_spec")) {
            return false;
        }
    }

    nlohmann::json contract = fixed["external_recorder_contract"];
    if (fixed.contains("recording_control") &&
        fixed["recording_control"].is_object() &&
        contract.is_object() &&
        !contract.contains("recording_control")) {
        contract["recording_control"] = fixed["recording_control"];
    }

    SupervisorPlan plan;
    if (!BuildSupervisorPlanFromContract(contract,
                                         options_with_spec,
                                         &plan,
                                         error_out)) {
        return false;
    }

    std::vector<std::string> selected_serials;
    if (!append_selection_camera_serials(experiment_spec, &selected_serials, error_out)) {
        return false;
    }
    for (const std::string& serial : selected_serials) {
        if (!stream_key_exists(contract["streams"], serial)) {
            return set_error(error_out,
                             "fixed.external_recorder_contract.streams missing selected camera " +
                                 serial);
        }
    }

    *plan_out = std::move(plan);
    return true;
}

std::vector<std::string> BuildRecorderCommand(const SupervisorPlan& plan,
                                              const RecorderStreamPlan& stream)
{
    std::vector<std::string> argv = {
        plan.recorder_tool_path,
        "--socket",
        stream.socket_path,
        "--gpu-id",
        std::to_string(stream.recorder_gpu_id),
        "--csv",
        stream.detach_csv,
        "--encode",
        "--encode-max-fps",
        std::to_string(stream.encode_max_fps),
        "--encode-queue-depth",
        std::to_string(stream.encode_queue_depth),
        "--prewarm-slots",
        std::to_string(stream.prewarm_slots),
        "--fps",
        std::to_string(stream.encode_fps),
        "--codec",
        stream.codec,
        "--preset",
        stream.preset,
        "--tuning",
        stream.tuning,
        "--gop",
        std::to_string(stream.gop),
        "--bitrate-bps",
        std::to_string(stream.bitrate_bps),
        "--max-bitrate-bps",
        std::to_string(stream.max_bitrate_bps),
        "--vbv-buffer-size",
        std::to_string(stream.vbv_buffer_size),
        "--mp4-out",
        stream.mp4,
        "--mp4-keyframe",
        stream.mp4_keyframe,
        "--encode-csv",
        stream.encode_csv,
        "--gop-routing-csv",
        stream.gop_routing_csv,
        "--summary-json",
        stream.summary_json,
        "--status-json",
        stream.status_json,
        "--session-id",
        plan.session_id,
        "--stream-id",
        stream.stream_id,
        "--record-for-seconds",
        std::to_string(stream.record_for_seconds),
        "--clip-seconds",
        std::to_string(stream.clip_seconds),
        "--shard-id",
        std::to_string(stream.shard_id),
        "--routing-policy",
        stream.routing_policy,
    };
    if (stream.terminal_tail_coalesce_frames > 0) {
        argv.push_back("--terminal-tail-coalesce-frames");
        argv.push_back(std::to_string(stream.terminal_tail_coalesce_frames));
    }
    if (stream.min_free_bytes > 0) {
        argv.push_back("--min-free-bytes");
        argv.push_back(std::to_string(stream.min_free_bytes));
    }
    if (stream.low_space_warning_bytes > 0) {
        argv.push_back("--low-space-warning-bytes");
        argv.push_back(std::to_string(stream.low_space_warning_bytes));
    }
    if (stream.prewarm_bytes > 0) {
        argv.push_back("--prewarm-bytes");
        argv.push_back(std::to_string(stream.prewarm_bytes));
    }
    if (stream.prewarm_peer_copy) {
        argv.push_back("--prewarm-peer-copy");
    }
    if (stream.expected_shard_gpu_ids.size() > 1) {
        argv.push_back("--shard-gpu-ids");
        argv.push_back(join_gpu_ids(stream.expected_shard_gpu_ids));
    }
    return argv;
}

nlohmann::json SupervisorPlanToJson(const SupervisorPlan& plan)
{
    nlohmann::json streams = nlohmann::json::array();
    for (const RecorderStreamPlan& stream : plan.streams) {
        streams.push_back({
            {"contract_key", stream.contract_key},
            {"stream_id", stream.stream_id},
            {"camera_serial", stream.camera_serial},
            {"analytics_gpu_id", stream.analytics_gpu_id},
            {"recorder_gpu_id", stream.recorder_gpu_id},
            {"expected_shard_gpu_ids", stream.expected_shard_gpu_ids},
            {"routing_policy", stream.routing_policy},
            {"socket_path", stream.socket_path},
            {"summary_json", stream.summary_json},
            {"status_json", stream.status_json},
            {"video_sanity_json", stream.video_sanity_json},
            {"mp4", stream.mp4},
            {"mp4_keyframe", stream.mp4_keyframe},
            {"detach_csv", stream.detach_csv},
            {"encode_csv", stream.encode_csv},
            {"gop_routing_csv", stream.gop_routing_csv},
            {"recorder_log", stream.recorder_log},
            {"recording_control", {
                {"record_for_seconds", stream.record_for_seconds},
                {"clip_seconds", stream.clip_seconds},
            }},
            {"encode_fps", stream.encode_fps},
            {"encode_max_fps", stream.encode_max_fps},
            {"encode_queue_depth", stream.encode_queue_depth},
            {"prewarm_slots", stream.prewarm_slots},
            {"prewarm_bytes", stream.prewarm_bytes},
            {"prewarm_peer_copy", stream.prewarm_peer_copy},
            {"codec", stream.codec},
            {"preset", stream.preset},
            {"tuning", stream.tuning},
            {"gop", stream.gop},
            {"terminal_tail_coalesce_frames", stream.terminal_tail_coalesce_frames},
            {"bitrate_bps", stream.bitrate_bps},
            {"max_bitrate_bps", stream.max_bitrate_bps},
            {"vbv_buffer_size", stream.vbv_buffer_size},
            {"min_free_bytes", stream.min_free_bytes},
            {"low_space_warning_bytes", stream.low_space_warning_bytes},
            {"command", {
                {"argv", BuildRecorderCommand(plan, stream)},
                {"log_path", stream.recorder_log},
                {"socket_path", stream.socket_path},
            }},
        });
    }

    return {
        {"schema_id", plan.schema_id},
        {"schema_version", plan.schema_version},
        {"recorder_tool_path", plan.recorder_tool_path},
        {"source_path", plan.source_path},
        {"mode", plan.mode},
        {"artifact_root", plan.artifact_root},
        {"session_id", plan.session_id},
        {"require_summary", plan.require_summary},
        {"require_video_sanity", plan.require_video_sanity},
        {"require_merged_mp4", plan.require_merged_mp4},
        {"require_gop_routing", plan.require_gop_routing},
        {"require_status", plan.require_status},
        {"require_status_runtime", plan.require_status_runtime},
        {"streams", streams},
    };
}

bool StartSupervisorProcesses(const SupervisorPlan& plan,
                              const SupervisorProcessOptions& options,
                              SupervisorRuntimeState* runtime_out,
                              std::string* error_out)
{
    if (!runtime_out) {
        return set_error(error_out, "internal error: null supervisor runtime destination");
    }
    if (plan.streams.empty()) {
        return set_error(error_out, "external recorder supervisor plan has no streams");
    }

    SupervisorRuntimeState runtime;
    runtime.artifact_root = plan.artifact_root;
    runtime.session_id = plan.session_id;

    for (const RecorderStreamPlan& stream : plan.streams) {
        RecorderProcessState process;
        process.stream_id = stream.stream_id;
        process.camera_serial = stream.camera_serial;
        process.socket_path = stream.socket_path;
        process.status_json_path = stream.status_json;
        process.recorder_status.path = stream.status_json;
        process.log_path = stream.recorder_log;
        process.argv = BuildRecorderCommand(plan, stream);
        process.status = "not_started";

        if (process.argv.empty()) {
            return set_error(error_out, "recorder command argv is empty for stream " + stream.stream_id);
        }
        if (process.socket_path.empty()) {
            return set_error(error_out, "recorder socket path is empty for stream " + stream.stream_id);
        }
        if (options.unlink_existing_sockets) {
            unlink(process.socket_path.c_str());
        }
        if (!process.status_json_path.empty()) {
            orange::ScopedFsuid fsuid_guard;
            (void)fsuid_guard;
            std::error_code remove_error;
            std::filesystem::remove(process.status_json_path, remove_error);
            std::filesystem::remove(process.status_json_path + ".tmp", remove_error);
        }

        int log_fd = -1;
        int log_open_errno = 0;
        {
            orange::ScopedFsuid fsuid_guard;
            (void)fsuid_guard;
            const std::filesystem::path log_parent =
                std::filesystem::path(process.log_path).parent_path();
            if (!log_parent.empty()) {
                std::error_code create_error;
                std::filesystem::create_directories(log_parent, create_error);
                if (create_error) {
                    return set_error(error_out,
                                     "failed to create recorder log directory for stream " +
                                         stream.stream_id + ": " + create_error.message());
                }
            }
            log_fd = ::open(process.log_path.c_str(), O_CREAT | O_WRONLY | O_TRUNC, 0644);
            log_open_errno = errno;
        }
        if (log_fd < 0) {
            return set_error(error_out,
                             "failed to open recorder log " + process.log_path +
                                 ": " + std::strerror(log_open_errno));
        }

        const pid_t pid = fork();
        if (pid < 0) {
            ::close(log_fd);
            return set_error(error_out,
                             "fork failed for external recorder stream " +
                                 stream.stream_id + ": " + std::strerror(errno));
        }

        if (pid == 0) {
            dup2(log_fd, STDOUT_FILENO);
            dup2(log_fd, STDERR_FILENO);
            ::close(log_fd);
            std::vector<char*> exec_argv = make_exec_argv(process.argv);
            execvp(exec_argv[0], exec_argv.data());
            std::fprintf(stderr,
                         "Failed to exec external recorder '%s': %s\n",
                         exec_argv[0],
                         std::strerror(errno));
            _exit(127);
        }

        ::close(log_fd);
        process.pid = pid;
        process.active = true;
        process.status = "starting";
        runtime.processes.push_back(std::move(process));
    }

    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(options.socket_ready_timeout_ms);
    while (std::chrono::steady_clock::now() < deadline) {
        bool all_ready = true;
        std::string wait_error;
        for (RecorderProcessState& process : runtime.processes) {
            poll_process_exit(&process, &wait_error);
            refresh_recorder_status_sidecar(&process);
            if (!process.active && !process.socket_ready) {
                *runtime_out = std::move(runtime);
                SupervisorProcessOptions stop_options = options;
                stop_options.graceful_shutdown_timeout_ms = 0;
                StopSupervisorProcesses(runtime_out, stop_options, nullptr);
                return set_error(error_out,
                                 "external recorder exited before socket readiness for stream " +
                                     process.stream_id);
            }
            if (!process.socket_ready &&
                socket_path_ready(process.socket_path,
                                  options.allow_regular_file_socket_ready_for_tests)) {
                process.socket_ready = true;
                process.status = "running";
            }
            all_ready = all_ready && process.socket_ready;
        }
        if (all_ready) {
            *runtime_out = std::move(runtime);
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }

    *runtime_out = std::move(runtime);
    SupervisorProcessOptions stop_options = options;
    stop_options.graceful_shutdown_timeout_ms = 0;
    StopSupervisorProcesses(runtime_out, stop_options, nullptr);
    return set_error(error_out, "timed out waiting for external recorder sockets");
}

bool StopSupervisorProcesses(SupervisorRuntimeState* runtime,
                             const SupervisorProcessOptions& options,
                             std::string* error_out)
{
    if (!runtime) {
        return set_error(error_out, "internal error: null supervisor runtime");
    }

    auto wait_until = [&](const int timeout_ms) {
        const auto deadline = std::chrono::steady_clock::now() +
                              std::chrono::milliseconds(timeout_ms);
        while (std::chrono::steady_clock::now() < deadline) {
            std::string wait_error;
            for (RecorderProcessState& process : runtime->processes) {
                poll_process_exit(&process, &wait_error);
                refresh_recorder_status_sidecar(&process);
            }
            if (all_processes_inactive(*runtime)) {
                return true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        return all_processes_inactive(*runtime);
    };

    wait_until(options.graceful_shutdown_timeout_ms);

    for (RecorderProcessState& process : runtime->processes) {
        if (process.active && process.pid > 0) {
            kill(process.pid, SIGTERM);
            process.termination_requested = true;
            process.status = "terminating";
        }
    }

    wait_until(options.terminate_timeout_ms);

    bool ok = true;
    for (RecorderProcessState& process : runtime->processes) {
        if (process.active && process.pid > 0) {
            kill(process.pid, SIGKILL);
            int wait_status = 0;
            if (waitpid(process.pid, &wait_status, 0) < 0) {
                ok = false;
                process.status = "wait_failed";
                process.error = std::string("waitpid after SIGKILL failed: ") +
                                std::strerror(errno);
            } else {
                capture_wait_status(&process, wait_status);
            }
        }
        if (!process.error.empty()) {
            ok = false;
        } else if (process.exit_code > 0 && process.exit_code != 143) {
            ok = false;
            if (process.error.empty()) {
                process.error = "external recorder exited nonzero";
            }
        }
    }

    if (!ok && error_out) {
        for (const RecorderProcessState& process : runtime->processes) {
            if (!process.error.empty()) {
                *error_out = process.error;
                break;
            }
        }
        if (error_out->empty()) {
            *error_out = "external recorder supervisor shutdown reported errors";
        }
    }
    return ok;
}

bool PollSupervisorProcesses(SupervisorRuntimeState* runtime,
                             std::string* error_out)
{
    if (!runtime) {
        return set_error(error_out, "internal error: null supervisor runtime");
    }

    std::string wait_error;
    for (RecorderProcessState& process : runtime->processes) {
        poll_process_exit(&process, &wait_error);
        refresh_recorder_status_sidecar(&process);
    }

    for (const RecorderProcessState& process : runtime->processes) {
        if (!process.error.empty()) {
            return set_error(error_out, process.error);
        }
    }
    if (!wait_error.empty()) {
        return set_error(error_out, wait_error);
    }
    if (error_out) {
        error_out->clear();
    }
    return true;
}

nlohmann::json SupervisorRuntimeStateToJson(const SupervisorRuntimeState& runtime)
{
    nlohmann::json processes = nlohmann::json::array();
    for (const RecorderProcessState& process : runtime.processes) {
        const RecorderStatusSnapshot& recorder_status = process.recorder_status;
        processes.push_back({
            {"stream_id", process.stream_id},
            {"camera_serial", process.camera_serial},
            {"socket_path", process.socket_path},
            {"status_json_path", process.status_json_path},
            {"log_path", process.log_path},
            {"argv", process.argv},
            {"pid", process.pid > 0 ? static_cast<int>(process.pid) : -1},
            {"active", process.active},
            {"socket_ready", process.socket_ready},
            {"termination_requested", process.termination_requested},
            {"exit_code", process.exit_code},
            {"term_signal", process.term_signal},
            {"status", process.status},
            {"error", process.error},
            {"recorder_status", {
                {"path", recorder_status.path},
                {"present", recorder_status.present},
                {"valid", recorder_status.valid},
                {"schema_id", recorder_status.schema_id},
                {"schema_version", recorder_status.schema_version},
                {"status", recorder_status.status},
                {"steady_clock_ns", recorder_status.steady_clock_ns},
                {"heartbeat_sequence", recorder_status.heartbeat_sequence},
                {"frames_received", recorder_status.frames_received},
                {"acks_sent", recorder_status.acks_sent},
                {"detach_copied", recorder_status.detach_copied},
                {"encode_enqueued", recorder_status.encode_enqueued},
                {"encode_skipped", recorder_status.encode_skipped},
                {"encode_dropped", recorder_status.encode_dropped},
                {"encode_queue_high_water", recorder_status.encode_queue_high_water},
                {"frames_encoded", recorder_status.frames_encoded},
                {"frames_dropped", recorder_status.frames_dropped},
                {"rolling_enabled", recorder_status.rolling_enabled},
                {"rolling_record_for_seconds", recorder_status.rolling_record_for_seconds},
                {"rolling_clip_seconds", recorder_status.rolling_clip_seconds},
                {"rolling_clip_span_frames", recorder_status.rolling_clip_span_frames},
                {"rolling_target_frame_count", recorder_status.rolling_target_frame_count},
                {"rolling_current_clip_index", recorder_status.rolling_current_clip_index},
                {"rolling_next_rollover_at_recording_frame_id",
                 recorder_status.rolling_next_rollover_at_recording_frame_id},
                {"rolling_frames_until_next_rollover",
                 recorder_status.rolling_frames_until_next_rollover},
                {"rolling_completed_clip_count", recorder_status.rolling_completed_clip_count},
                {"rolling_last_completed_clip_index",
                 recorder_status.rolling_last_completed_clip_index},
                {"rolling_last_completed_clip_last_recording_frame_id",
                 recorder_status.rolling_last_completed_clip_last_recording_frame_id},
                {"rolling_last_completed_clip_frame_count",
                 recorder_status.rolling_last_completed_clip_frame_count},
                {"rolling_last_rollover_status", recorder_status.rolling_last_rollover_status},
                {"worker_failed", recorder_status.worker_failed},
                {"storage_checked", recorder_status.storage_checked},
                {"storage_ok", recorder_status.storage_ok},
                {"storage_low_space", recorder_status.storage_low_space},
                {"storage_min_free_bytes", recorder_status.storage_min_free_bytes},
                {"storage_low_space_warning_bytes",
                 recorder_status.storage_low_space_warning_bytes},
                {"storage_path_count", recorder_status.storage_path_count},
                {"storage_paths_ok_count", recorder_status.storage_paths_ok_count},
                {"storage_paths_low_space_count",
                 recorder_status.storage_paths_low_space_count},
                {"storage_has_min_available_bytes",
                 recorder_status.storage_has_min_available_bytes},
                {"storage_min_available_bytes",
                 recorder_status.storage_min_available_bytes},
                {"error", recorder_status.error},
            }},
        });
    }
    return {
        {"schema_id", runtime.schema_id},
        {"schema_version", runtime.schema_version},
        {"artifact_root", runtime.artifact_root},
        {"session_id", runtime.session_id},
        {"processes", processes},
    };
}

}  // namespace orange::external_recorder
