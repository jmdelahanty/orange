#include <algorithm>
#include <atomic>
#include <array>
#include <chrono>
#include <cctype>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <map>
#include <memory>
#include <limits>
#include <mutex>
#include <numeric>
#include <sstream>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include "network_base.h"
#include "thread.h"
#include "types.h"
#include <cstring>
#include "video_capture.h"
#include "NvEncoder/NvCodecUtils.h"
#include "project.h"
#include "video_capture.h"
#include "fetch_generated.h"
#include "acquire_frames.h"
#include "frame_ipc_manager.h"
#include "shaman_v2.h"
#include "modern_recording_pipeline.h"
#include "recording_ingress.h"
#include "session/recording_session.h"
#include "external_recorder_contract_utils.h"
#include "external_recorder_lifecycle.h"
#include "external_recorder_supervisor.h"
#include "fsuid_guard.h"
#include "yolov8_det.h"
#include "yolo_worker.h"
#include "yolo_event_log.h"
#include "yolo_event_log_validation.h"
#include "crop_producer_worker.h"
#include "pose_worker.h"
#include "pose_event_log_validation.h"
#include <signal.h>

#define evt_buffer_size 100
#define max_cameras 20

simplelogger::Logger *logger = simplelogger::LoggerFactory::CreateConsoleLogger();

namespace {

enum class HeadlessMode {
    Remote,
    Local,
};

struct HeadlessEncoderSettings {
    std::string codec = "h264";
    std::string preset = "p1";
    std::string tuning = "ll";
    std::string rate_control_mode = "vbr";
    ImportanceMapConfig importance_map;
    int quality_value = 20;
    int gop_length = 0;
    EncoderControlOverrides control_overrides;
    bool select_all_cameras = true;
    std::vector<std::string> camera_serials;
};

enum class HeadlessFrameIpcMode {
    Off,
    ProducerOnly,
    VerifyDrain,
    VerifyDrainV2,
};

struct HeadlessFrameIpcConfig {
    HeadlessFrameIpcMode mode = HeadlessFrameIpcMode::Off;
    bool unlink_existing_queues = false;
    bool require_base_frames = true;
    bool allow_push_failures = false;

    bool enabled() const {
        return mode != HeadlessFrameIpcMode::Off;
    }
};

struct HeadlessYoloWorkerConfig {
    std::string mode = "off";
    std::string engine_path;
    int decimate = 1;
    bool publish_live_ipc = false;
    int timeout_ms = 500;
    int prewarm_iterations = 0;
    bool fail_on_init_error = true;

    bool enabled() const {
        return mode == "real";
    }
};

struct HeadlessPoseWorkerConfig {
    std::string mode = "off";
    std::string engine_path;
    std::string skeleton_id = "unknown";
    std::string skeleton_path;
    int input_width = 256;
    int input_height = 256;
    std::string input_layout = "nchw";
    std::string input_dtype = "fp16";
    std::string normalization = "model_default";
    std::string roi_source = "yolo_top_detection";
    int synthetic_detection_every_n_frames = 1;
    int synthetic_detection_box_width_px = 64;
    int synthetic_detection_box_height_px = 64;
    int synthetic_detection_label = 0;
    double synthetic_detection_confidence = 0.99;
    int queue_depth = 32;
    int crop_frame_pool_size = 0;
    int timeout_ms = 500;
    int prewarm_iterations = 0;
    bool fail_on_init_error = true;
    bool write_events_jsonl = true;

    bool enabled() const {
        return mode == "noop" || mode == "real";
    }

    bool synthetic_runtime_detection_enabled() const {
        return roi_source == "synthetic_center_box";
    }
};

struct HeadlessRecordingControlConfig {
    int record_for_seconds = 0;
    int clip_seconds = 0;

    bool enabled() const {
        return record_for_seconds > 0 || clip_seconds > 0;
    }
};

struct HeadlessExternalRecorderContractConfig {
    std::string mode = "off";
    std::string schema_id = "orange.external_recorder.contract";
    int schema_version = 1;
    std::string artifact_root;
    std::string session_id;
    std::string recorder_tool_path;
    bool supervise_processes = false;
    bool require_summary = true;
    bool require_video_sanity = true;
    bool require_merged_mp4 = true;
    bool require_gop_routing = true;
    nlohmann::json streams = nlohmann::json::object();

    bool enabled() const {
        return mode != "off";
    }
};

struct HeadlessCliOptions {
    HeadlessMode mode = HeadlessMode::Remote;
    bool show_help = false;
    bool list_cameras = false;
    bool stream_only = false;
    bool nvenc_direct_input = false;
    std::string config_folder;
    std::string record_folder;
    std::string experiment_spec_path;
    std::string sync_mode_override;
    std::string ptp_gate_acquisition_mode_override;
    std::string acquisition_buffer_mode_override;
    int ptp_gate_stagger_ns = 0;
    std::string recording_sink_mode = "real";
    int duration_seconds = 0;
    int stream_start_delay_seconds = 0;
    int record_start_delay_seconds = 0;
    std::vector<int> required_gpu_ids;
    HeadlessEncoderSettings encoder_settings;
    PreEncoderReferenceCaptureConfig pre_encoder_reference_capture;
    HeadlessFrameIpcConfig frame_ipc;
    yolo_event_log::SyntheticYoloEventConfig yolo_event_log;
    HeadlessYoloWorkerConfig yolo_worker;
    HeadlessPoseWorkerConfig pose_worker;
    HeadlessRecordingControlConfig recording_control;
    HeadlessExternalRecorderContractConfig external_recorder_contract;
    bool has_recording_override = false;
    nlohmann::json recording_override = nlohmann::json::object();
    std::unordered_map<std::string, nlohmann::json> recording_overrides_by_camera;
};

struct ExperimentSpec {
    std::string source_path;
    nlohmann::json source_json = nlohmann::json::object();
    std::string experiment_id;
    std::string notes;
    std::string output_root;
    std::string config_folder;
    std::string sync_mode = "free_run";
    bool stream_only = false;
    std::string ptp_gate_acquisition_mode;
    std::string acquisition_buffer_mode = "auto";
    int ptp_gate_stagger_ns = 0;
    int ptp_register_read_decimate = 1;
    std::string recording_sink_mode = "real";
    bool helper_noop_source_read = false;
    int64_t helper_copy_bytes = -1;
    int64_t helper_copy_delay_ns = 0;
    int duration_s = 0;
    int warmup_s = 0;
    int stream_start_delay_s = 0;
    bool nvenc_direct_input = false;
    double target_fps_tolerance_pct = 1.0;
    bool require_zero_acq_starve = true;
    bool require_zero_pre_drops = true;
    bool require_zero_enc_fail = true;
    bool require_zero_camera_drops = true;
    bool require_valid_video_content = true;
    std::vector<int> gpu_ids;
    HeadlessEncoderSettings selection;
    PreEncoderReferenceCaptureConfig pre_encoder_reference_capture;
    HeadlessFrameIpcConfig frame_ipc;
    yolo_event_log::SyntheticYoloEventConfig yolo_event_log;
    HeadlessYoloWorkerConfig yolo_worker;
    HeadlessPoseWorkerConfig pose_worker;
    HeadlessRecordingControlConfig recording_control;
    HeadlessExternalRecorderContractConfig external_recorder_contract;
    bool has_recording_override = false;
    nlohmann::json recording_override = nlohmann::json::object();
    std::unordered_map<std::string, nlohmann::json> recording_overrides_by_camera;
    std::vector<std::string> codecs;
    std::vector<std::string> presets;
    std::vector<std::string> tunings;
    std::vector<std::string> rate_control_modes;
    std::vector<int> quality_values;
    std::vector<int> gop_lengths;
    std::vector<int> aq_values;
    std::vector<int> temporal_aq_values;
    std::vector<int> lookahead_values;
    std::vector<int> lookahead_depth_values;
    std::vector<int> target_bitrate_bps_values;
    std::vector<int> max_bitrate_bps_values;
    std::vector<int> vbv_buffer_size_values;
    std::vector<std::string> importance_map_modes;
    std::vector<int> importance_map_roi_size_px_values;
};

struct ExperimentRunPlan {
    int run_index = 0;
    std::string run_id;
    std::string recording_folder;
    int duration_s = 0;
    int warmup_s = 0;
    HeadlessCliOptions options;
    nlohmann::json config_json = nlohmann::json::object();
};

struct HeadlessHostPtpStackGuard;
void teardown_headless_host_ptp_stack(HeadlessHostPtpStackGuard* guard);

struct HeadlessHostPtpStatusSummary {
    bool ptp4l_running = false;
    bool phc2sys_running = false;
    bool socket_present = false;
};

struct HeadlessHostPtpStackGuard {
    std::string script_path;
    bool stop_on_exit = false;

    ~HeadlessHostPtpStackGuard() {
        teardown_headless_host_ptp_stack(this);
    }
};

struct HeadlessHostPtpCommandResult {
    int exit_code = -1;
    std::string output;
    std::string error_message;
};

std::string trim_ascii_whitespace(const std::string& input)
{
    size_t start = 0;
    while (start < input.size() && std::isspace(static_cast<unsigned char>(input[start]))) {
        ++start;
    }
    size_t end = input.size();
    while (end > start && std::isspace(static_cast<unsigned char>(input[end - 1]))) {
        --end;
    }
    return input.substr(start, end - start);
}

std::string shell_quote_single(const std::string& input)
{
    std::string quoted;
    quoted.reserve(input.size() + 2);
    quoted.push_back('\'');
    for (char c : input) {
        if (c == '\'') {
            quoted += "'\\''";
        } else {
            quoted.push_back(c);
        }
    }
    quoted.push_back('\'');
    return quoted;
}

std::filesystem::path resolve_ptp_stack_script_path()
{
    std::vector<std::filesystem::path> candidates;
    candidates.emplace_back(std::filesystem::current_path() / "scripts" / "ptp_stack.sh");

    std::array<char, 4096> exe_path_buffer{};
    const ssize_t exe_path_len =
        readlink("/proc/self/exe", exe_path_buffer.data(), exe_path_buffer.size() - 1);
    if (exe_path_len > 0) {
        const std::filesystem::path exe_path(
            std::string(exe_path_buffer.data(), static_cast<size_t>(exe_path_len)));
        candidates.emplace_back(
            exe_path.parent_path().parent_path().parent_path() / "scripts" / "ptp_stack.sh");
    }

    for (const auto& candidate : candidates) {
        std::error_code ec;
        if (std::filesystem::exists(candidate, ec) && !ec) {
            return std::filesystem::absolute(candidate, ec);
        }
    }
    return {};
}

HeadlessHostPtpCommandResult run_ptp_stack_command(const std::string& script_path,
                                                   const std::string& command)
{
    HeadlessHostPtpCommandResult result;
    if (script_path.empty()) {
        result.error_message = "PTP stack script path is empty.";
        return result;
    }

    const std::string full_command =
        shell_quote_single(script_path) + " " + command + " 2>&1";
    FILE* pipe = popen(full_command.c_str(), "r");
    if (!pipe) {
        result.error_message = "Failed to run PTP stack command.";
        return result;
    }

    std::array<char, 512> buffer{};
    while (fgets(buffer.data(), static_cast<int>(buffer.size()), pipe) != nullptr) {
        result.output += buffer.data();
    }

    const int status = pclose(pipe);
    if (status == -1) {
        result.error_message = "Failed to close PTP stack command pipe.";
        return result;
    }

    if (WIFEXITED(status)) {
        result.exit_code = WEXITSTATUS(status);
    } else {
        result.exit_code = status;
    }
    return result;
}

HeadlessHostPtpStatusSummary parse_ptp_stack_status_output(const std::string& output)
{
    HeadlessHostPtpStatusSummary summary;
    std::istringstream stream(output);
    std::string line;
    bool in_process_state = false;
    bool in_time_status = false;
    while (std::getline(stream, line)) {
        const std::string trimmed = trim_ascii_whitespace(line);
        if (trimmed == "Process state:") {
            in_process_state = true;
            in_time_status = false;
            continue;
        }
        if (trimmed == "PTP TIME_STATUS_NP:") {
            in_process_state = false;
            in_time_status = true;
            continue;
        }
        if (trimmed.empty()) {
            if (in_process_state) {
                in_process_state = false;
            }
            continue;
        }

        if (in_process_state) {
            if (trimmed == "(no ptp4l/phc2sys process)") {
                summary.ptp4l_running = false;
                summary.phc2sys_running = false;
                continue;
            }
            if (trimmed.find("ptp4l") != std::string::npos) {
                summary.ptp4l_running = true;
            }
            if (trimmed.find("phc2sys") != std::string::npos) {
                summary.phc2sys_running = true;
            }
            continue;
        }

        if (in_time_status) {
            if (trimmed.find("(socket ") != std::string::npos &&
                trimmed.find("not found") != std::string::npos) {
                summary.socket_present = false;
                continue;
            }
            if (trimmed.rfind("sending: GET TIME_STATUS_NP", 0) == 0) {
                summary.socket_present = true;
                continue;
            }
        }
    }

    return summary;
}

bool is_host_ptp_status_healthy(const HeadlessHostPtpStatusSummary& summary)
{
    return summary.ptp4l_running && summary.phc2sys_running && summary.socket_present;
}

bool ensure_headless_host_ptp_stack(HeadlessHostPtpStackGuard* guard, std::string* error_out)
{
    if (!guard) {
        if (error_out) {
            *error_out = "PTP stack guard is null.";
        }
        return false;
    }

    const std::filesystem::path script_path = resolve_ptp_stack_script_path();
    if (script_path.empty()) {
        if (error_out) {
            *error_out = "Could not find scripts/ptp_stack.sh relative to the repo or binary.";
        }
        return false;
    }
    guard->script_path = script_path.string();
    guard->stop_on_exit = false;

    const HeadlessHostPtpCommandResult status_before =
        run_ptp_stack_command(guard->script_path, "status");
    if (!status_before.error_message.empty()) {
        if (error_out) {
            *error_out = status_before.error_message;
        }
        return false;
    }
    const HeadlessHostPtpStatusSummary before_summary =
        parse_ptp_stack_status_output(status_before.output);
    if (is_host_ptp_status_healthy(before_summary)) {
        std::cout << "[HEADLESS][PTP] Host PTP stack already healthy." << std::endl;
        return true;
    }

    std::cout << "[HEADLESS][PTP] Host PTP stack not ready; starting it now." << std::endl;
    const bool stack_absent_before =
        !before_summary.ptp4l_running && !before_summary.phc2sys_running && !before_summary.socket_present;
    const HeadlessHostPtpCommandResult start_result =
        run_ptp_stack_command(guard->script_path, "start");
    if (!start_result.error_message.empty() || start_result.exit_code != 0) {
        if (error_out) {
            *error_out = "Failed to start host PTP stack."
                + (start_result.error_message.empty() ? std::string() : (" " + start_result.error_message))
                + (start_result.output.empty() ? std::string() : ("\n" + start_result.output));
        }
        return false;
    }

    const HeadlessHostPtpCommandResult status_after =
        run_ptp_stack_command(guard->script_path, "status");
    if (!status_after.error_message.empty()) {
        if (error_out) {
            *error_out = status_after.error_message;
        }
        return false;
    }
    const HeadlessHostPtpStatusSummary after_summary =
        parse_ptp_stack_status_output(status_after.output);
    if (!is_host_ptp_status_healthy(after_summary)) {
        if (error_out) {
            *error_out =
                "Host PTP stack is still not healthy after start.\n" + status_after.output;
        }
        return false;
    }

    guard->stop_on_exit = stack_absent_before;
    if (guard->stop_on_exit) {
        std::cout << "[HEADLESS][PTP] Host PTP stack was started by this run and will be stopped on exit."
                  << std::endl;
    } else {
        std::cout << "[HEADLESS][PTP] Host PTP stack was repaired for this run and will be left running on exit."
                  << std::endl;
    }
    return true;
}

void teardown_headless_host_ptp_stack(HeadlessHostPtpStackGuard* guard)
{
    if (!guard || !guard->stop_on_exit || guard->script_path.empty()) {
        return;
    }
    const HeadlessHostPtpCommandResult stop_result =
        run_ptp_stack_command(guard->script_path, "stop");
    if (!stop_result.error_message.empty() || stop_result.exit_code != 0) {
        std::cerr << "[HEADLESS][PTP] Failed to stop host PTP stack after run.";
        if (!stop_result.error_message.empty()) {
            std::cerr << " " << stop_result.error_message;
        }
        if (!stop_result.output.empty()) {
            std::cerr << "\n" << stop_result.output;
        }
        std::cerr << std::endl;
        return;
    }
    std::cout << "[HEADLESS][PTP] Stopped host PTP stack started by this run." << std::endl;
    guard->stop_on_exit = false;
}

struct ExperimentCsvWindowStats {
    bool ok = false;
    std::size_t total_rows = 0;
    std::size_t included_rows = 0;
    double acq_fps_mean = 0.0;
    double acq_fps_p95 = 0.0;
    double enc_fps_mean = 0.0;
    double enc_fps_p95 = 0.0;
    double enc_fps_primary_mean = 0.0;
    double enc_fps_primary_p95 = 0.0;
    double enc_fps_helpers_mean = 0.0;
    double enc_fps_helpers_p95 = 0.0;
    int acq_free_entries_min = -1;
    int acq_free_events_min = -1;
    int yolo_events_min = -1;
    uint64_t acq_starve_delta = 0;
    uint64_t pre_waits_delta = 0;
    uint64_t pre_drops_delta = 0;
    uint64_t enc_fail_delta = 0;
    uint64_t enc_slow_delta = 0;
    uint64_t external_ipc_frames_acked_delta = 0;
    uint64_t external_ipc_failures_delta = 0;
    uint64_t external_ipc_ack_timeouts_delta = 0;
    uint64_t submitted_frames_delta = 0;
    uint64_t primary_routed_frames_delta = 0;
    uint64_t helper_requested_frames_delta = 0;
    uint64_t helper_fallback_frames_delta = 0;
    uint64_t helper_dispatched_frames_delta = 0;
    uint64_t camera_dropped_frames_delta = 0;
    uint64_t get_frame_errors_delta = 0;
    int last_get_frame_error_code = 0;
    int pre_buffers_min = -1;
    int pre_events_min = -1;
    std::string error;
};

struct HeadlessGpuDmonMonitor {
    bool active = false;
    pid_t pid = -1;
    int sample_period_seconds = 1;
    std::vector<int> gpu_ids;
    std::string recording_folder;
    std::string artifact_path;
    std::string stderr_path;
    std::string started_at_utc;
    std::string stopped_at_utc;
    std::string status = "not_started";
    int exit_code = -1;
    int term_signal = 0;
    std::string error;
};

struct HeadlessThreadFailureState {
    mutable std::mutex mutex;
    bool failed = false;
    std::string first_error;

    void reset() {
        std::lock_guard<std::mutex> lock(mutex);
        failed = false;
        first_error.clear();
    }

    void record_failure(const std::string& error) {
        std::lock_guard<std::mutex> lock(mutex);
        if (!failed) {
            first_error = error;
        }
        failed = true;
    }

    bool has_failure() const {
        std::lock_guard<std::mutex> lock(mutex);
        return failed;
    }

    std::string get_first_error() const {
        std::lock_guard<std::mutex> lock(mutex);
        return first_error;
    }
};

struct HeadlessFrameIpcReaderStats {
    std::string camera_serial;
    uint32_t camera_id = 0;
    std::string queue_name;
    bool v2 = false;
    bool reader_started = false;
    std::string reader_error;
    uint64_t messages_popped = 0;
    uint64_t base_messages = 0;
    uint64_t detection_update_messages = 0;
    uint64_t yolo_enabled_messages = 0;
    uint64_t v2_latest_state_messages = 0;
    uint64_t v2_detection_pending_messages = 0;
    uint64_t v2_detection_result_messages = 0;
    uint64_t v2_pose_result_messages = 0;
    uint64_t camera_id_mismatches = 0;
    uint64_t frame_id_gaps = 0;
    uint64_t non_monotonic_frame_ids = 0;
    uint64_t sequence_id_gaps = 0;
    uint64_t non_monotonic_sequence_ids = 0;
    uint64_t first_frame_id = 0;
    uint64_t last_frame_id = 0;
    uint64_t first_sequence_id = 0;
    uint64_t last_sequence_id = 0;
};

struct HeadlessFrameIpcRuntime {
    HeadlessFrameIpcConfig config;
    std::atomic<bool> stop_requested{false};
    std::vector<std::thread> reader_threads;
    std::vector<HeadlessFrameIpcReaderStats> reader_stats;

    void reset(const HeadlessFrameIpcConfig& next_config) {
        config = next_config;
        stop_requested.store(false, std::memory_order_release);
        reader_threads.clear();
        reader_stats.clear();
    }
};

using CameraGpuOverrideMap = std::unordered_map<std::string, int>;
using RecordingOverrideMap = std::unordered_map<std::string, nlohmann::json>;

constexpr int kGpuDmonStartupPollMs = 200;
constexpr int kGpuDmonShutdownWaitMs = 2000;
constexpr const char* kBundledFfprobePath = "/opt/orange/lib/ffmpeg-nvidia/bin/ffprobe";
constexpr const char* kBundledFfmpegPath = "/opt/orange/lib/ffmpeg-nvidia/bin/ffmpeg";
constexpr double kVideoSanityMinLumaStddev = 1.0;
constexpr double kVideoSanityMaxBlackFraction = 0.995;
constexpr unsigned char kVideoSanityBlackThreshold = 2;
constexpr size_t kVideoSanityMaxDecodeBytes = 256ULL * 1024ULL * 1024ULL;

std::string headless_frame_ipc_mode_to_string(HeadlessFrameIpcMode mode)
{
    switch (mode) {
        case HeadlessFrameIpcMode::Off:
            return "off";
        case HeadlessFrameIpcMode::ProducerOnly:
            return "producer_only";
        case HeadlessFrameIpcMode::VerifyDrain:
            return "verify_drain";
        case HeadlessFrameIpcMode::VerifyDrainV2:
            return "verify_drain_v2";
    }
    return "off";
}

bool headless_frame_ipc_mode_is_verify_drain(HeadlessFrameIpcMode mode)
{
    return mode == HeadlessFrameIpcMode::VerifyDrain ||
           mode == HeadlessFrameIpcMode::VerifyDrainV2;
}

bool headless_frame_ipc_mode_uses_v2(HeadlessFrameIpcMode mode)
{
    return mode == HeadlessFrameIpcMode::VerifyDrainV2;
}

bool parse_headless_frame_ipc_mode(const std::string& value, HeadlessFrameIpcMode* out)
{
    if (!out) {
        return false;
    }
    std::string normalized;
    normalized.reserve(value.size());
    for (char c : value) {
        normalized.push_back(c == '-' ? '_' : static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    }
    if (normalized == "off" || normalized == "false" || normalized == "disabled" || normalized == "none") {
        *out = HeadlessFrameIpcMode::Off;
        return true;
    }
    if (normalized == "producer_only" || normalized == "producer" || normalized == "on" || normalized == "true") {
        *out = HeadlessFrameIpcMode::ProducerOnly;
        return true;
    }
    if (normalized == "verify_drain" || normalized == "verify" || normalized == "drain") {
        *out = HeadlessFrameIpcMode::VerifyDrain;
        return true;
    }
    if (normalized == "verify_drain_v2" || normalized == "verify_v2" ||
        normalized == "drain_v2" || normalized == "v2") {
        *out = HeadlessFrameIpcMode::VerifyDrainV2;
        return true;
    }
    return false;
}

std::string build_frame_ipc_queue_name_for_serial(const std::string& camera_serial)
{
    return "/shm_cam_" + camera_serial;
}

std::string build_frame_ipc_v2_queue_name_for_serial(const std::string& camera_serial)
{
    return shaman_v2::queue_name_for_camera_serial(camera_serial);
}

nlohmann::json build_headless_yolo_event_log_config_json(
    const yolo_event_log::SyntheticYoloEventConfig& config)
{
    return {
        {"mode", config.mode},
        {"every_n_frames", config.every_n_frames},
        {"pattern", config.pattern},
        {"emit_zero_detections", config.emit_zero_detections},
        {"label", config.label},
        {"confidence", config.confidence}
    };
}

std::string normalize_headless_token(std::string value)
{
    std::transform(
        value.begin(),
        value.end(),
        value.begin(),
        [](unsigned char c) {
            return c == '-' ? '_' : static_cast<char>(std::tolower(c));
        });
    return value;
}

nlohmann::json build_headless_yolo_worker_config_json(
    const HeadlessYoloWorkerConfig& config)
{
    return {
        {"mode", config.mode},
        {"engine_path", config.engine_path},
        {"decimate", config.decimate},
        {"publish_live_ipc", config.publish_live_ipc},
        {"timeout_ms", config.timeout_ms},
        {"prewarm_iterations", config.prewarm_iterations},
        {"fail_on_init_error", config.fail_on_init_error}
    };
}

nlohmann::json build_headless_pose_worker_config_json(
    const HeadlessPoseWorkerConfig& config)
{
    return {
        {"mode", config.mode},
        {"engine_path", config.engine_path},
        {"skeleton_id", config.skeleton_id},
        {"skeleton_path", config.skeleton_path},
        {"input_width", config.input_width},
        {"input_height", config.input_height},
        {"input_layout", config.input_layout},
        {"input_dtype", config.input_dtype},
        {"normalization", config.normalization},
        {"roi_source", config.roi_source},
        {"synthetic_detection", {
            {"enabled", config.synthetic_runtime_detection_enabled()},
            {"every_n_frames", config.synthetic_detection_every_n_frames},
            {"box_width_px", config.synthetic_detection_box_width_px},
            {"box_height_px", config.synthetic_detection_box_height_px},
            {"label", config.synthetic_detection_label},
            {"confidence", config.synthetic_detection_confidence},
            {"production_detection_valid", !config.synthetic_runtime_detection_enabled()},
            {"validation_scope",
             config.synthetic_runtime_detection_enabled()
                 ? "pose_plumbing_only"
                 : "production_detection"}
        }},
        {"queue_depth", config.queue_depth},
        {"crop_frame_pool_size", config.crop_frame_pool_size},
        {"timeout_ms", config.timeout_ms},
        {"prewarm_iterations", config.prewarm_iterations},
        {"fail_on_init_error", config.fail_on_init_error},
        {"write_events_jsonl", config.write_events_jsonl}
    };
}

nlohmann::json build_headless_recording_control_config_json(
    const HeadlessRecordingControlConfig& config)
{
    return orange::session::build_recording_control_json(
        orange::session::RecordingControlConfig{
            config.record_for_seconds,
            config.clip_seconds
        });
}

nlohmann::json build_headless_external_recorder_contract_config_json(
    const HeadlessExternalRecorderContractConfig& config,
    const HeadlessRecordingControlConfig* recording_control = nullptr)
{
    nlohmann::json contract = {
        {"schema_id", config.schema_id},
        {"schema_version", config.schema_version},
        {"mode", config.mode},
        {"artifact_root", config.artifact_root},
        {"session_id", config.session_id},
        {"recorder_tool_path", config.recorder_tool_path},
        {"supervise_processes", config.supervise_processes},
        {"require_summary", config.require_summary},
        {"require_video_sanity", config.require_video_sanity},
        {"require_merged_mp4", config.require_merged_mp4},
        {"require_gop_routing", config.require_gop_routing},
        {"streams", config.streams.is_object() ? config.streams : nlohmann::json::object()}
    };
    if (recording_control) {
        orange::external_recorder::RecordingControlIntent intent;
        intent.record_for_seconds = recording_control->record_for_seconds;
        intent.clip_seconds = recording_control->clip_seconds;
        orange::external_recorder::ApplyExternalRecorderRecordingControlToContract(
            &contract,
            intent);
    }
    return contract;
}

struct ExperimentVideoArtifactStats {
    std::string video_path;
    bool video_present = false;
    uint64_t file_size_bytes = 0;
    double duration_s = 0.0;
    uint64_t achieved_bitrate_bps = 0;
    bool content_checked = false;
    bool content_valid = false;
    std::string content_status = "not_checked";
    double first_frame_luma_mean = 0.0;
    double first_frame_luma_stddev = 0.0;
    double first_frame_black_fraction = 0.0;
    uint64_t first_frame_decoded_bytes = 0;
};

class ScopedEnvVarOverride {
public:
    ScopedEnvVarOverride(const char* name, const char* value)
        : name_(name ? name : "")
    {
        if (name_.empty()) {
            return;
        }
        const char* existing = std::getenv(name_.c_str());
        if (existing) {
            had_original_ = true;
            original_value_ = existing;
        }
        if (value) {
            setenv(name_.c_str(), value, 1);
            active_ = true;
        }
    }

    ~ScopedEnvVarOverride()
    {
        if (!active_ || name_.empty()) {
            return;
        }
        if (had_original_) {
            setenv(name_.c_str(), original_value_.c_str(), 1);
        } else {
            unsetenv(name_.c_str());
        }
    }

private:
    std::string name_;
    std::string original_value_;
    bool had_original_ = false;
    bool active_ = false;
};

std::vector<int> collect_unique_gpu_ids(const CameraParams* cameras_params,
                                        const std::vector<int>& selected_indices);
void start_headless_gpu_dmon_monitor(HeadlessGpuDmonMonitor* monitor,
                                     const std::string& recording_folder,
                                     const std::vector<int>& gpu_ids);
void stop_headless_gpu_dmon_monitor(HeadlessGpuDmonMonitor* monitor);

std::string shell_single_quote(const std::string& value)
{
    std::string out;
    out.reserve(value.size() + 2);
    out.push_back('\'');
    for (char c : value) {
        if (c == '\'') {
            out += "'\\''";
        } else {
            out.push_back(c);
        }
    }
    out.push_back('\'');
    return out;
}

bool read_command_stdout(const std::string& command, std::string* stdout_out)
{
    if (stdout_out) {
        stdout_out->clear();
    }

    FILE* pipe = popen(command.c_str(), "r");
    if (!pipe) {
        return false;
    }

    std::array<char, 4096> buffer{};
    while (fgets(buffer.data(), static_cast<int>(buffer.size()), pipe) != nullptr) {
        if (stdout_out) {
            *stdout_out += buffer.data();
        }
    }

    const int status = pclose(pipe);
    return status == 0;
}

bool read_command_binary(const std::string& command,
                         std::vector<unsigned char>* stdout_out,
                         size_t max_bytes)
{
    if (stdout_out) {
        stdout_out->clear();
    }

    FILE* pipe = popen(command.c_str(), "r");
    if (!pipe) {
        return false;
    }

    bool read_error = false;
    std::array<unsigned char, 65536> buffer{};
    while (true) {
        const size_t read = fread(buffer.data(), 1, buffer.size(), pipe);
        if (read > 0 && stdout_out) {
            if (stdout_out->size() + read > max_bytes) {
                read_error = true;
                break;
            }
            stdout_out->insert(stdout_out->end(), buffer.data(), buffer.data() + read);
        }
        if (read < buffer.size()) {
            if (feof(pipe)) {
                break;
            }
            if (ferror(pipe)) {
                read_error = true;
                break;
            }
        }
    }

    const int status = pclose(pipe);
    return !read_error && status == 0;
}

double json_number_or_default(const nlohmann::json& value, double fallback)
{
    if (value.is_number()) {
        return value.get<double>();
    }
    if (value.is_string()) {
        try {
            return std::stod(value.get<std::string>());
        } catch (...) {
            return fallback;
        }
    }
    return fallback;
}

uint64_t json_u64_or_default(const nlohmann::json& value, uint64_t fallback)
{
    if (value.is_number_unsigned()) {
        return value.get<uint64_t>();
    }
    if (value.is_number_integer()) {
        const auto signed_value = value.get<int64_t>();
        return signed_value >= 0 ? static_cast<uint64_t>(signed_value) : fallback;
    }
    if (value.is_number_float()) {
        const double floating_value = value.get<double>();
        return floating_value >= 0.0 ? static_cast<uint64_t>(floating_value) : fallback;
    }
    if (value.is_string()) {
        try {
            return static_cast<uint64_t>(std::stoull(value.get<std::string>()));
        } catch (...) {
            return fallback;
        }
    }
    return fallback;
}

ExperimentVideoArtifactStats summarize_video_artifact_path(const std::filesystem::path& video_path)
{
    ExperimentVideoArtifactStats stats;
    if (video_path.empty()) {
        return stats;
    }
    stats.video_path = video_path.string();

    std::error_code fs_error;
    if (!std::filesystem::exists(video_path, fs_error) || fs_error) {
        return stats;
    }

    stats.video_present = true;
    stats.file_size_bytes = std::filesystem::file_size(video_path, fs_error);
    if (fs_error) {
        stats.file_size_bytes = 0;
    }

    const std::filesystem::path ffprobe_path =
        std::filesystem::exists(kBundledFfprobePath)
            ? std::filesystem::path(kBundledFfprobePath)
            : std::filesystem::path("ffprobe");
    const std::string command =
        shell_single_quote(ffprobe_path.string()) +
        " -v error -show_entries format=duration,size,bit_rate -of json " +
        shell_single_quote(video_path.string()) + " 2>/dev/null";

    std::string ffprobe_stdout;
    if (!read_command_stdout(command, &ffprobe_stdout) || ffprobe_stdout.empty()) {
        return stats;
    }

    const nlohmann::json ffprobe_json = nlohmann::json::parse(ffprobe_stdout, nullptr, false);
    if (!ffprobe_json.is_object()) {
        return stats;
    }
    const nlohmann::json format = ffprobe_json.value("format", nlohmann::json::object());
    if (!format.is_object()) {
        return stats;
    }

    const double duration_s = json_number_or_default(format.value("duration", nlohmann::json()), 0.0);
    if (duration_s > 0.0) {
        stats.duration_s = duration_s;
    }

    const uint64_t size_bytes = json_u64_or_default(format.value("size", nlohmann::json()), 0ULL);
    if (size_bytes > 0) {
        stats.file_size_bytes = size_bytes;
    }

    uint64_t achieved_bitrate_bps =
        json_u64_or_default(format.value("bit_rate", nlohmann::json()), 0ULL);
    if (achieved_bitrate_bps == 0 && stats.file_size_bytes > 0 && stats.duration_s > 0.0) {
        achieved_bitrate_bps = static_cast<uint64_t>(
            std::llround((static_cast<long double>(stats.file_size_bytes) * 8.0L) / stats.duration_s));
    }
    stats.achieved_bitrate_bps = achieved_bitrate_bps;

    const std::filesystem::path ffmpeg_path =
        std::filesystem::exists(kBundledFfmpegPath)
            ? std::filesystem::path(kBundledFfmpegPath)
            : std::filesystem::path("ffmpeg");
    const std::string decode_command =
        shell_single_quote(ffmpeg_path.string()) +
        " -v error -i " +
        shell_single_quote(video_path.string()) +
        " -frames:v 1 -vf format=gray -f rawvideo pipe:1 2>/dev/null";

    std::vector<unsigned char> frame_bytes;
    if (!read_command_binary(decode_command, &frame_bytes, kVideoSanityMaxDecodeBytes) ||
        frame_bytes.empty()) {
        stats.content_checked = true;
        stats.content_status = "decode_failed";
        stats.content_valid = false;
        return stats;
    }

    long double sum = 0.0L;
    long double sum_sq = 0.0L;
    uint64_t black_count = 0;
    for (const unsigned char value : frame_bytes) {
        const long double v = static_cast<long double>(value);
        sum += v;
        sum_sq += v * v;
        if (value <= kVideoSanityBlackThreshold) {
            ++black_count;
        }
    }

    const long double count = static_cast<long double>(frame_bytes.size());
    const long double mean = sum / count;
    long double variance = (sum_sq / count) - (mean * mean);
    if (variance < 0.0L) {
        variance = 0.0L;
    }

    stats.content_checked = true;
    stats.first_frame_decoded_bytes = static_cast<uint64_t>(frame_bytes.size());
    stats.first_frame_luma_mean = static_cast<double>(mean);
    stats.first_frame_luma_stddev = static_cast<double>(std::sqrt(variance));
    stats.first_frame_black_fraction =
        static_cast<double>(static_cast<long double>(black_count) / count);

    if (stats.first_frame_black_fraction >= kVideoSanityMaxBlackFraction) {
        stats.content_status = "black_frame";
        stats.content_valid = false;
    } else if (stats.first_frame_luma_stddev < kVideoSanityMinLumaStddev) {
        stats.content_status = "low_luma_stddev";
        stats.content_valid = false;
    } else {
        stats.content_status = "pass";
        stats.content_valid = true;
    }

    return stats;
}

ExperimentVideoArtifactStats summarize_video_artifact(const std::string& recording_folder,
                                                      const std::string& camera_serial)
{
    if (recording_folder.empty() || camera_serial.empty()) {
        return ExperimentVideoArtifactStats{};
    }
    return summarize_video_artifact_path(
        std::filesystem::path(recording_folder) / ("Cam" + camera_serial + ".mp4"));
}

nlohmann::json read_json_file_best_effort(const std::filesystem::path& path)
{
    std::ifstream input(path);
    if (!input) {
        return nlohmann::json::object();
    }
    return nlohmann::json::parse(input, nullptr, false);
}

std::filesystem::path resolve_clip_video_path(const std::filesystem::path& run_folder,
                                              const nlohmann::json& clip,
                                              const std::string& camera_serial)
{
    const nlohmann::json artifacts = clip.value("artifacts", nlohmann::json::object());
    const nlohmann::json videos = artifacts.value("videos", nlohmann::json::object());
    if (videos.is_object()) {
        const std::string configured = videos.value(camera_serial, "");
        if (!configured.empty()) {
            std::filesystem::path path(configured);
            if (!path.is_absolute()) {
                path = run_folder / path;
            }
            return path;
        }
    }

    const std::string clip_folder = clip.value("recording_folder", "");
    if (!clip_folder.empty()) {
        return std::filesystem::path(clip_folder) / ("Cam" + camera_serial + ".mp4");
    }
    const std::string directory = clip.value("directory", "");
    if (!directory.empty()) {
        return run_folder / directory / ("Cam" + camera_serial + ".mp4");
    }
    return {};
}

ExperimentVideoArtifactStats summarize_rolling_video_artifacts(
    const std::string& recording_folder,
    const std::string& camera_serial)
{
    ExperimentVideoArtifactStats aggregate;
    if (recording_folder.empty() || camera_serial.empty()) {
        return aggregate;
    }

    const std::filesystem::path run_folder(recording_folder);
    const nlohmann::json manifest =
        read_json_file_best_effort(run_folder / "recording_session.json");
    if (!manifest.is_object() || manifest.value("mode", "") != "rolling_clips") {
        return aggregate;
    }
    const nlohmann::json clips = manifest.value("clips", nlohmann::json::array());
    if (!clips.is_array() || clips.empty()) {
        return aggregate;
    }

    bool saw_content_status = false;
    std::size_t expected_videos = 0;
    std::size_t present_videos = 0;
    uint64_t file_size_bytes = 0;
    for (const nlohmann::json& clip : clips) {
        if (!clip.is_object()) {
            continue;
        }
        const std::filesystem::path path =
            resolve_clip_video_path(run_folder, clip, camera_serial);
        if (path.empty()) {
            continue;
        }
        ++expected_videos;
        ExperimentVideoArtifactStats clip_stats = summarize_video_artifact_path(path);
        if (aggregate.video_path.empty()) {
            aggregate.video_path = path.string();
        }
        if (!clip_stats.video_present) {
            aggregate.content_status = "missing_clip_video";
            saw_content_status = true;
            continue;
        }
        ++present_videos;
        file_size_bytes += clip_stats.file_size_bytes;
        aggregate.duration_s += clip_stats.duration_s;
        if (!aggregate.content_checked && clip_stats.content_checked) {
            aggregate.first_frame_luma_mean = clip_stats.first_frame_luma_mean;
            aggregate.first_frame_luma_stddev = clip_stats.first_frame_luma_stddev;
            aggregate.first_frame_black_fraction = clip_stats.first_frame_black_fraction;
            aggregate.first_frame_decoded_bytes = clip_stats.first_frame_decoded_bytes;
        }
        aggregate.content_checked = aggregate.content_checked || clip_stats.content_checked;
        if (clip_stats.content_checked && !clip_stats.content_valid && !saw_content_status) {
            aggregate.content_status = "clip_" +
                std::to_string(clip.value("clip_index", 0)) + "_" + clip_stats.content_status;
            saw_content_status = true;
        }
    }

    aggregate.video_present = expected_videos > 0 && present_videos == expected_videos;
    aggregate.file_size_bytes = file_size_bytes;
    if (aggregate.file_size_bytes > 0 && aggregate.duration_s > 0.0) {
        aggregate.achieved_bitrate_bps = static_cast<uint64_t>(
            std::llround((static_cast<long double>(aggregate.file_size_bytes) * 8.0L) /
                         aggregate.duration_s));
    }
    aggregate.content_valid =
        aggregate.video_present && aggregate.content_checked && !saw_content_status;
    if (aggregate.content_valid) {
        aggregate.content_status = "pass";
    } else if (!saw_content_status && expected_videos == 0) {
        aggregate.content_status = "missing_clip_artifacts";
    } else if (!saw_content_status && !aggregate.content_checked) {
        aggregate.content_status = "not_checked";
    }
    return aggregate;
}

nlohmann::json build_rolling_recording_session_snapshot_update(
    const std::string& recording_folder,
    const nlohmann::json& manifest,
    const orange::session::RecordingSessionIndexArtifacts& index_artifacts)
{
    nlohmann::json indexes =
        manifest.value("indexes", nlohmann::json::object());
    if (indexes.is_object()) {
        if (!index_artifacts.clip_index_json_path.empty()) {
            indexes["clip_index_json_path"] = index_artifacts.clip_index_json_path;
        }
        if (!index_artifacts.clip_index_csv_path.empty()) {
            indexes["clip_index_csv_path"] = index_artifacts.clip_index_csv_path;
        }
    }

    nlohmann::json update = {
        {"recording_mode", manifest.value("mode", std::string())},
        {"recording_session_manifest_path",
         (std::filesystem::path(recording_folder) / "recording_session.json").string()},
        {"recording_session_index", indexes},
        {"rolling_clip_count", manifest.value("clips", nlohmann::json::array()).size()},
        {"rolling_index_row_count", indexes.value("row_count", 0)}
    };
    if (manifest.contains("recording_backend")) {
        update["recording_backend"] = manifest["recording_backend"];
    }
    return update;
}

std::string canonicalize_headless_camera_serial(std::string value)
{
    value.erase(value.begin(), std::find_if(value.begin(), value.end(), [](unsigned char c) {
        return !std::isspace(c);
    }));
    value.erase(std::find_if(value.rbegin(), value.rend(), [](unsigned char c) {
        return !std::isspace(c);
    }).base(), value.end());

    if (value.empty()) {
        return value;
    }

    const bool is_numeric = std::all_of(value.begin(), value.end(), [](unsigned char c) {
        return std::isdigit(c) != 0;
    });
    if (!is_numeric) {
        return value;
    }

    const std::size_t first_non_zero = value.find_first_not_of('0');
    if (first_non_zero == std::string::npos) {
        return "0";
    }
    return value.substr(first_non_zero);
}

nlohmann::json build_recording_override_map_json(
    const RecordingOverrideMap& overrides)
{
    nlohmann::json out = nlohmann::json::object();
    for (const auto& [camera_serial, recording] : overrides) {
        out[camera_serial] = recording;
    }
    return out;
}

std::string format_recording_strategy_summary(const RecordingStrategyConfig& recording_strategy)
{
    std::ostringstream summary;
    summary << "mode=" << recording_strategy.mode;
    if (recording_strategy.mode == "split_gop") {
        summary << " placement=" << recording_strategy.split_gop.placement
                << " source_encoder_policy=" << recording_strategy.split_gop.source_encoder_policy
                << " transfer_mode=" << recording_strategy.split_gop.transfer_mode
                << " encoder_gpu_ids=";
        if (recording_strategy.split_gop.encoder_gpu_ids.empty()) {
            summary << "<none>";
        } else {
            for (std::size_t i = 0; i < recording_strategy.split_gop.encoder_gpu_ids.size(); ++i) {
                if (i > 0) {
                    summary << ",";
                }
                summary << recording_strategy.split_gop.encoder_gpu_ids[i];
            }
        }
    }
    return summary.str();
}

bool parse_experiment_recording_overrides(const nlohmann::json& fixed,
                                          ExperimentSpec* spec,
                                          std::string* error_out)
{
    if (error_out) {
        error_out->clear();
    }
    if (!spec) {
        if (error_out) {
            *error_out = "Internal error: null experiment spec while parsing recording overrides";
        }
        return false;
    }

    spec->has_recording_override = false;
    spec->recording_override = nlohmann::json::object();
    spec->recording_overrides_by_camera.clear();

    if (fixed.contains("recording")) {
        if (!fixed["recording"].is_object()) {
            if (error_out) {
                *error_out = "Experiment spec fixed.recording must be a JSON object";
            }
            return false;
        }
        CameraRecordingConfig recording_config;
        if (!parse_camera_recording_json(
                fixed["recording"],
                &recording_config,
                error_out)) {
            if (error_out && !error_out->empty()) {
                *error_out = "Experiment spec fixed.recording invalid: " + *error_out;
            }
            return false;
        }
        spec->recording_override = fixed["recording"];
        spec->has_recording_override = true;
    }

    if (!fixed.contains("recording_by_camera")) {
        return true;
    }
    if (!fixed["recording_by_camera"].is_object()) {
        if (error_out) {
            *error_out = "Experiment spec fixed.recording_by_camera must be a JSON object keyed by camera serial";
        }
        return false;
    }

    for (const auto& item : fixed["recording_by_camera"].items()) {
        const std::string canonical_serial = canonicalize_headless_camera_serial(item.key());
        if (canonical_serial.empty()) {
            if (error_out) {
                *error_out = "Experiment spec fixed.recording_by_camera contains an empty camera serial key";
            }
            return false;
        }
        if (!item.value().is_object()) {
            if (error_out) {
                *error_out = "Experiment spec fixed.recording_by_camera." + item.key() +
                             " must be a JSON object";
            }
            return false;
        }
        CameraRecordingConfig recording_config;
        if (!parse_camera_recording_json(item.value(), &recording_config, error_out)) {
            if (error_out && !error_out->empty()) {
                *error_out = "Experiment spec fixed.recording_by_camera." + item.key() +
                             " invalid: " + *error_out;
            }
            return false;
        }
        spec->recording_overrides_by_camera[canonical_serial] = item.value();
    }

    return true;
}

bool apply_recording_overrides_to_selected_cameras(
    const HeadlessCliOptions& options,
    CameraParams* cameras_params,
    const std::vector<int>& selected_inventory_indices,
    std::string* error_out)
{
    if (error_out) {
        error_out->clear();
    }
    if (!cameras_params) {
        if (error_out) {
            *error_out = "Internal error: null camera params while applying recording overrides";
        }
        return false;
    }

    std::unordered_set<std::string> applied_per_camera_overrides;
    for (int inventory_index : selected_inventory_indices) {
        CameraParams& camera_params = cameras_params[inventory_index];
        const std::string camera_serial = canonicalize_headless_camera_serial(camera_params.camera_serial);

        bool applied_override = false;
        nlohmann::json effective_recording_json =
            build_camera_recording_json(camera_params.recording);
        if (options.has_recording_override) {
            effective_recording_json.merge_patch(options.recording_override);
            applied_override = true;
        }

        const auto per_camera_it =
            options.recording_overrides_by_camera.find(camera_serial);
        if (per_camera_it != options.recording_overrides_by_camera.end()) {
            effective_recording_json.merge_patch(per_camera_it->second);
            applied_override = true;
            applied_per_camera_overrides.insert(camera_serial);
        }

        if (applied_override) {
            CameraRecordingConfig merged_recording;
            if (!parse_camera_recording_json(effective_recording_json, &merged_recording, error_out)) {
                if (error_out && !error_out->empty()) {
                    *error_out = "Failed to apply recording override for camera " +
                                 camera_serial + ": " + *error_out;
                }
                return false;
            }
            camera_params.recording = std::move(merged_recording);
        }

        if (applied_override) {
            std::cout << "Applied headless recording override."
                      << " camera=" << camera_serial
                      << " " << format_recording_strategy_summary(camera_params.recording.strategy)
                      << std::endl;
        }
    }

    for (const auto& [camera_serial, _] : options.recording_overrides_by_camera) {
        if (applied_per_camera_overrides.count(camera_serial) == 0) {
            if (error_out) {
                *error_out =
                    "Experiment spec fixed.recording_by_camera contains a camera serial that was not opened: " +
                    camera_serial;
            }
            return false;
        }
    }

    return true;
}

bool parse_headless_sync_mode_override(const std::string& value, std::string* out);
bool parse_headless_ptp_gate_acquisition_mode_override(const std::string& value, std::string* out);
bool parse_headless_acquisition_buffer_mode_override(const std::string& value, std::string* out);
bool parse_headless_recording_sink_mode_override(const std::string& value, std::string* out);

bool apply_sync_mode_override_to_selected_cameras(
    const HeadlessCliOptions& options,
    CameraParams* cameras_params,
    const std::vector<int>& selected_inventory_indices,
    std::string* error_out)
{
    if (error_out) {
        error_out->clear();
    }
    if (!cameras_params) {
        if (error_out) {
            *error_out = "Internal error: null camera params while applying sync mode override";
        }
        return false;
    }
    if (options.sync_mode_override.empty()) {
        return true;
    }

    std::string normalized_sync_mode;
    if (!parse_headless_sync_mode_override(options.sync_mode_override, &normalized_sync_mode)) {
        if (error_out) {
            *error_out = "Unsupported headless sync mode override: " + options.sync_mode_override;
        }
        return false;
    }

    for (int inventory_index : selected_inventory_indices) {
        CameraParams& camera_params = cameras_params[inventory_index];
        camera_params.sync_mode = normalized_sync_mode;
        if (normalized_sync_mode == "ptp_gate" && camera_params.ptp_mode.empty()) {
            camera_params.ptp_mode = "TwoStep";
        }
    }

    return true;
}

bool apply_ptp_gate_stagger_to_selected_cameras(
    const HeadlessCliOptions& options,
    CameraParams* cameras_params,
    const std::vector<int>& selected_inventory_indices,
    std::string* error_out)
{
    if (error_out) {
        error_out->clear();
    }
    if (!cameras_params) {
        if (error_out) {
            *error_out = "Internal error: null camera params while applying PTP gate stagger";
        }
        return false;
    }

    for (int inventory_index : selected_inventory_indices) {
        cameras_params[inventory_index].ptp_gate_offset_ns = 0;
    }

    if (options.ptp_gate_stagger_ns <= 0) {
        return true;
    }

    int ptp_camera_ordinal = 0;
    for (int inventory_index : selected_inventory_indices) {
        CameraParams& camera_params = cameras_params[inventory_index];
        if (!camera_sync_mode_uses_ptp(&camera_params)) {
            continue;
        }
        camera_params.ptp_gate_offset_ns =
            static_cast<unsigned long long>(ptp_camera_ordinal) *
            static_cast<unsigned long long>(options.ptp_gate_stagger_ns);
        ++ptp_camera_ordinal;
    }

    return true;
}

bool apply_ptp_gate_acquisition_mode_to_selected_cameras(
    const HeadlessCliOptions& options,
    CameraParams* cameras_params,
    const std::vector<int>& selected_inventory_indices,
    std::string* error_out)
{
    if (error_out) {
        error_out->clear();
    }
    if (!cameras_params) {
        if (error_out) {
            *error_out = "Internal error: null camera params while applying PTP gate acquisition mode";
        }
        return false;
    }

    std::string normalized_mode = "multiframe";
    if (!options.ptp_gate_acquisition_mode_override.empty()) {
        if (!parse_headless_ptp_gate_acquisition_mode_override(
                options.ptp_gate_acquisition_mode_override, &normalized_mode)) {
            if (error_out) {
                *error_out =
                    "Unsupported PTP gate acquisition mode override: " +
                    options.ptp_gate_acquisition_mode_override;
            }
            return false;
        }
    }

    for (int inventory_index : selected_inventory_indices) {
        CameraParams& camera_params = cameras_params[inventory_index];
        camera_params.ptp_gate_acquisition_mode = "multiframe";
        if (!camera_sync_mode_uses_ptp(&camera_params)) {
            continue;
        }
        camera_params.ptp_gate_acquisition_mode = normalized_mode;
    }

    return true;
}

bool apply_acquisition_buffer_mode_to_selected_cameras(
    const HeadlessCliOptions& options,
    CameraParams* cameras_params,
    const std::vector<int>& selected_inventory_indices,
    std::string* error_out)
{
    if (error_out) {
        error_out->clear();
    }
    if (!cameras_params) {
        if (error_out) {
            *error_out =
                "Internal error: null camera params while applying acquisition buffer mode";
        }
        return false;
    }

    std::string normalized_mode = "auto";
    if (!options.acquisition_buffer_mode_override.empty()) {
        if (!parse_headless_acquisition_buffer_mode_override(
                options.acquisition_buffer_mode_override, &normalized_mode)) {
            if (error_out) {
                *error_out =
                    "Unsupported acquisition buffer mode override: " +
                    options.acquisition_buffer_mode_override;
            }
            return false;
        }
    }

    for (int inventory_index : selected_inventory_indices) {
        CameraParams& camera_params = cameras_params[inventory_index];
        camera_params.acquisition_buffer_mode = normalized_mode;
    }

    return true;
}

std::vector<std::string> split_headless_encoder_setup(const std::string& setup)
{
    std::vector<std::string> tokens;
    std::string current;
    for (char ch : setup) {
        if (std::isspace(static_cast<unsigned char>(ch)) || ch == ',' || ch == ';' || ch == '|') {
            if (!current.empty()) {
                tokens.push_back(current);
                current.clear();
            }
            continue;
        }
        current.push_back(ch);
    }
    if (!current.empty()) {
        tokens.push_back(current);
    }
    return tokens;
}

std::string normalize_headless_sync_mode_label(const CameraParams* camera_params)
{
    if (!camera_params) {
        return "free_run";
    }

    std::string mode = camera_params->sync_mode;
    std::transform(mode.begin(), mode.end(), mode.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });

    if (mode == "ptp_gate") {
        return "ptp_gate";
    }
    return "free_run";
}

bool parse_headless_sync_mode_override(const std::string& value, std::string* out)
{
    if (!out) {
        return false;
    }

    std::string normalized = value;
    std::transform(normalized.begin(), normalized.end(), normalized.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });

    if (normalized.empty() || normalized == "free_run") {
        *out = "free_run";
        return true;
    }
    if (normalized == "ptp_gate") {
        *out = "ptp_gate";
        return true;
    }
    return false;
}

bool parse_headless_ptp_gate_acquisition_mode_override(const std::string& value, std::string* out)
{
    if (!out) {
        return false;
    }

    std::string normalized = value;
    std::transform(normalized.begin(), normalized.end(), normalized.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });

    if (normalized.empty() || normalized == "multiframe") {
        *out = "multiframe";
        return true;
    }
    if (normalized == "continuous") {
        *out = "continuous";
        return true;
    }
    return false;
}

bool parse_headless_acquisition_buffer_mode_override(const std::string& value, std::string* out)
{
    if (!out) {
        return false;
    }

    std::string normalized = value;
    std::transform(normalized.begin(), normalized.end(), normalized.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });

    if (normalized.empty() || normalized == "auto") {
        *out = "auto";
        return true;
    }
    if (normalized == "force_ring_copy") {
        *out = "force_ring_copy";
        return true;
    }
    return false;
}

bool parse_headless_recording_sink_mode_override(const std::string& value, std::string* out)
{
    if (!out) {
        return false;
    }
    const std::string normalized = normalize_recording_sink_mode(value);
    if (normalized.empty()) {
        return false;
    }
    *out = normalized;
    return true;
}

bool parse_headless_toggle_override(const std::string& value, int* out)
{
    if (!out) {
        return false;
    }
    std::string normalized = value;
    std::transform(normalized.begin(), normalized.end(), normalized.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    if (normalized == "auto" || normalized.empty()) {
        *out = -1;
        return true;
    }
    if (normalized == "1" || normalized == "true" || normalized == "on" || normalized == "yes") {
        *out = 1;
        return true;
    }
    if (normalized == "0" || normalized == "false" || normalized == "off" || normalized == "no") {
        *out = 0;
        return true;
    }
    return false;
}

std::string format_headless_toggle_override(int value)
{
    if (value > 0) {
        return "on";
    }
    if (value == 0) {
        return "off";
    }
    return "auto";
}

std::string normalize_importance_map_mode(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    if (value.empty() || value == "off" || value == "none" || value == "disabled") {
        return "off";
    }
    if (value == "static_roi" || value == "static-roi" ||
        value == "static_prior" || value == "static-prior" || value == "static") {
        return "static_roi";
    }
    return value;
}

int normalize_importance_map_roi_size_px(int value)
{
    return value > 0 ? value : ImportanceMapConfig::kDefaultRoiSizePx;
}

bool parse_importance_map_mode(const std::string& value, std::string* out)
{
    if (!out) {
        return false;
    }
    const std::string normalized = normalize_importance_map_mode(value);
    if (normalized == "off" || normalized == "static_roi") {
        *out = normalized;
        return true;
    }
    return false;
}

std::string resolved_headless_ptp_mode_label(const CameraParams* camera_params)
{
    if (!camera_params || camera_params->ptp_mode.empty()) {
        return "TwoStep";
    }
    return camera_params->ptp_mode;
}

void append_camera_selection(HeadlessEncoderSettings* settings, const std::string& value)
{
    if (!settings || value.empty()) {
        return;
    }
    if (value == "all" || value == "*") {
        settings->select_all_cameras = true;
        settings->camera_serials.clear();
        return;
    }
    if (settings->select_all_cameras) {
        settings->select_all_cameras = false;
        settings->camera_serials.clear();
    }
    std::size_t start = 0;
    while (start <= value.size()) {
        const std::size_t plus = value.find('+', start);
        const std::string serial = canonicalize_headless_camera_serial(
            value.substr(start, plus == std::string::npos ? std::string::npos : plus - start));
        if (!serial.empty()) {
            settings->camera_serials.push_back(serial);
        }
        if (plus == std::string::npos) {
            break;
        }
        start = plus + 1;
    }
}

HeadlessEncoderSettings parse_headless_encoder_setup(const std::string& setup)
{
    HeadlessEncoderSettings settings;
    const std::vector<std::string> tokens = split_headless_encoder_setup(setup);

    int positional_index = 0;
    auto assign_positional = [&](const std::string& token) {
        switch (positional_index++) {
            case 0: settings.codec = token; break;
            case 1: settings.preset = token; break;
            case 2: settings.tuning = token; break;
            case 3: settings.rate_control_mode = token; break;
            case 4: settings.quality_value = std::atoi(token.c_str()); break;
            case 5: settings.gop_length = std::atoi(token.c_str()); break;
            default: break;
        }
    };

    for (const std::string& token : tokens) {
        const std::size_t equals = token.find('=');
        if (equals == std::string::npos) {
            assign_positional(token);
            continue;
        }

        const std::string key = token.substr(0, equals);
        const std::string value = token.substr(equals + 1);
        if (value.empty()) {
            continue;
        }

        if (key == "codec") {
            settings.codec = value;
        } else if (key == "preset") {
            settings.preset = value;
        } else if (key == "tuning" || key == "tune") {
            settings.tuning = value;
        } else if (key == "camera" || key == "cameras" ||
                   key == "camera_serial" || key == "camera_serials") {
            append_camera_selection(&settings, value);
        } else if (key == "rc" || key == "rate_control" || key == "rate_control_mode") {
            settings.rate_control_mode = value;
        } else if (key == "importance_map" || key == "importance_map_mode" ||
                   key == "importance-map" || key == "importance-map-mode") {
            std::string parsed_mode;
            if (parse_importance_map_mode(value, &parsed_mode)) {
                settings.importance_map.mode = parsed_mode;
            }
        } else if (key == "importance_map_roi_size_px" ||
                   key == "importance-map-roi-size-px" ||
                   key == "imap_size" ||
                   key == "importance_map_size_px") {
            settings.importance_map.roi_size_px = std::atoi(value.c_str());
        } else if (key == "quality" || key == "cq" || key == "qp") {
            settings.quality_value = std::atoi(value.c_str());
        } else if (key == "gop" || key == "gop_length") {
            settings.gop_length = std::atoi(value.c_str());
        } else if (key == "aq") {
            int parsed = -1;
            if (parse_headless_toggle_override(value, &parsed)) {
                settings.control_overrides.aq = parsed;
            }
        } else if (key == "temporal_aq" || key == "temporal-aq") {
            int parsed = -1;
            if (parse_headless_toggle_override(value, &parsed)) {
                settings.control_overrides.temporal_aq = parsed;
            }
        } else if (key == "lookahead") {
            int parsed = -1;
            if (parse_headless_toggle_override(value, &parsed)) {
                settings.control_overrides.lookahead = parsed;
            }
        } else if (key == "lookahead_depth" || key == "lookahead-depth") {
            settings.control_overrides.lookahead_depth = std::atoi(value.c_str());
        } else if (key == "target_bitrate_bps" || key == "target-bitrate-bps" || key == "bitrate") {
            settings.control_overrides.target_bitrate_bps = std::atoi(value.c_str());
        } else if (key == "max_bitrate_bps" || key == "max-bitrate-bps") {
            settings.control_overrides.max_bitrate_bps = std::atoi(value.c_str());
        } else if (key == "vbv_buffer_size" || key == "vbv-buffer-size" || key == "vbv") {
            settings.control_overrides.vbv_buffer_size = std::atoi(value.c_str());
        }
    }

    if (settings.codec.empty()) {
        settings.codec = "h264";
    }
    if (settings.preset.empty()) {
        settings.preset = "p1";
    }
    if (settings.tuning.empty()) {
        settings.tuning = "ll";
    }
    if (settings.rate_control_mode.empty()) {
        settings.rate_control_mode = "vbr";
    }
    settings.importance_map.mode = normalize_importance_map_mode(settings.importance_map.mode);
    settings.importance_map.roi_size_px =
        normalize_importance_map_roi_size_px(settings.importance_map.roi_size_px);
    if (settings.quality_value < 1) {
        settings.quality_value = 20;
    }
    if (settings.gop_length < 0) {
        settings.gop_length = 0;
    }
    if (settings.control_overrides.lookahead_depth < -1) {
        settings.control_overrides.lookahead_depth = -1;
    }
    if (settings.control_overrides.target_bitrate_bps == 0) {
        settings.control_overrides.target_bitrate_bps = -1;
    }
    if (settings.control_overrides.max_bitrate_bps == 0) {
        settings.control_overrides.max_bitrate_bps = -1;
    }
    if (settings.control_overrides.vbv_buffer_size == 0) {
        settings.control_overrides.vbv_buffer_size = -1;
    }

    return settings;
}

std::string format_selected_camera_serials(const HeadlessEncoderSettings& settings)
{
    if (settings.select_all_cameras || settings.camera_serials.empty()) {
        return "all";
    }

    std::ostringstream out;
    for (std::size_t i = 0; i < settings.camera_serials.size(); ++i) {
        if (i != 0) {
            out << "+";
        }
        out << settings.camera_serials[i];
    }
    return out.str();
}

std::string build_headless_encoder_setup_string(const HeadlessEncoderSettings& settings)
{
    std::ostringstream out;
    out << "codec=" << settings.codec
        << " preset=" << settings.preset
        << " tuning=" << settings.tuning
        << " rc=" << settings.rate_control_mode
        << " importance_map_mode=" << settings.importance_map.mode
        << " importance_map_roi_size_px=" << settings.importance_map.roi_size_px
        << " quality=" << settings.quality_value
        << " gop=" << settings.gop_length
        << " camera=" << format_selected_camera_serials(settings);
    if (settings.control_overrides.aq >= 0) {
        out << " aq=" << format_headless_toggle_override(settings.control_overrides.aq);
    }
    if (settings.control_overrides.temporal_aq >= 0) {
        out << " temporal_aq=" << format_headless_toggle_override(settings.control_overrides.temporal_aq);
    }
    if (settings.control_overrides.lookahead >= 0) {
        out << " lookahead=" << format_headless_toggle_override(settings.control_overrides.lookahead);
    }
    if (settings.control_overrides.lookahead_depth >= 0) {
        out << " lookahead_depth=" << settings.control_overrides.lookahead_depth;
    }
    if (settings.control_overrides.target_bitrate_bps > 0) {
        out << " target_bitrate_bps=" << settings.control_overrides.target_bitrate_bps;
    }
    if (settings.control_overrides.max_bitrate_bps > 0) {
        out << " max_bitrate_bps=" << settings.control_overrides.max_bitrate_bps;
    }
    if (settings.control_overrides.vbv_buffer_size > 0) {
        out << " vbv_buffer_size=" << settings.control_overrides.vbv_buffer_size;
    }
    return out.str();
}

void print_headless_usage(const char* argv0)
{
    std::cout
        << "Usage:\n"
        << "  " << argv0 << " --mode remote\n"
        << "  " << argv0 << " --mode local --record-folder <path> [options]\n"
        << "  " << argv0 << " --mode local --stream-only [options]\n"
        << "  " << argv0 << " --mode local --list-cameras\n\n"
        << "Local mode options:\n"
        << "  --camera <serial|all>        Repeatable. Defaults to all.\n"
        << "  --stream-only                Open stream and run acquisition without recording.\n"
        << "  --nvenc-direct-input         Enable direct registered NVENC input path.\n"
        << "  --gpu-id <int>               Optional runtime GPU placement input.\n"
        << "  --config-folder <path>       Optional camera config folder.\n"
        << "  --record-folder <path>       Required for local recording runs.\n"
        << "  --codec <h264|hevc>\n"
        << "  --preset <p1..p7>\n"
        << "  --tuning <ull|ll|hq>\n"
        << "  --rate-control <vbr|vbr_cq|cbr|cqp>\n"
        << "  --importance-map-mode <off|static_roi>\n"
        << "  --importance-map-roi-size-px <int>  Default 512. Used by static_roi.\n"
        << "  --quality <int>\n"
        << "  --gop <int>\n"
        << "  --aq <auto|on|off>\n"
        << "  --temporal-aq <auto|on|off>\n"
        << "  --lookahead <auto|on|off>\n"
        << "  --lookahead-depth <int>\n"
        << "  --target-bitrate-bps <int>\n"
        << "  --max-bitrate-bps <int>\n"
        << "  --vbv-buffer-size <int>\n"
        << "  --preenc-ref-max-frames <int>\n"
        << "  --preenc-ref-max-seconds <int>\n"
        << "  --duration <seconds>         Optional. Otherwise runs until Ctrl+C.\n"
        << "  --stream-start-delay <sec>   Optional. Wait after camera open before stream open.\n"
        << "  --record-delay <seconds>     Optional. Stream first, then arm recording.\n"
        << "  --ptp-gate-acquisition-mode <multiframe|continuous>\n"
        << "                              Optional. Experimental override for camera-side ptp_gate acquisition mode.\n"
        << "  --acquisition-buffer-mode <auto|force_ring_copy>\n"
        << "                              Optional. Experimental override for acquisition buffer ownership.\n"
        << "  --ptp-gate-stagger-ns <int>  Optional. Apply a per-camera gate-time offset for ptp_gate runs.\n"
        << "  --frame-ipc <off|producer_only|verify_drain|verify_drain_v2>\n"
        << "                              Optional. Publish serial-named frame IPC queues.\n"
        << "  --frame-ipc-unlink-existing Optional. Remove stale serial-named IPC queues before creating writers.\n"
        << "  --frame-ipc-allow-push-failures\n"
        << "                              Optional. Do not fail verification on full/undrained IPC rings.\n"
        << "  --yolo-engine <path>        Optional. Enable real audit-only YOLO with this TensorRT engine.\n"
        << "  --yolo-decimate <int>       Optional. Process 1/N frames when real YOLO is enabled.\n"
        << "  --yolo-publish-live-ipc     Optional. Let real YOLO publish detections to frame IPC.\n"
        << "  --experiment-spec <path>     Run a local single-host experiment matrix.\n"
        << "  --list-cameras               List local cameras and exit.\n"
        << "  --help\n";
}

bool parse_non_negative_int(const std::string& value, int* out)
{
    if (!out || value.empty()) {
        return false;
    }
    char* end = nullptr;
    const long parsed = std::strtol(value.c_str(), &end, 10);
    if (end == value.c_str() || *end != '\0' || parsed < 0 || parsed > std::numeric_limits<int>::max()) {
        return false;
    }
    *out = static_cast<int>(parsed);
    return true;
}

bool pre_encoder_reference_capture_requested(const PreEncoderReferenceCaptureConfig& config)
{
    return config.enabled ||
           config.max_frames > 0 ||
           config.max_seconds > 0 ||
           !config.output_dir.empty();
}

bool headless_frame_ipc_requested(const HeadlessFrameIpcConfig& config)
{
    return config.enabled() ||
           config.unlink_existing_queues ||
           config.allow_push_failures ||
           !config.require_base_frames;
}

bool headless_yolo_worker_requested(const HeadlessYoloWorkerConfig& config)
{
    return config.enabled() ||
           config.mode != "off" ||
           !config.engine_path.empty() ||
           config.decimate != 1 ||
           config.publish_live_ipc ||
           config.timeout_ms != 500 ||
           config.prewarm_iterations != 0 ||
           !config.fail_on_init_error;
}

bool headless_pose_worker_requested(const HeadlessPoseWorkerConfig& config)
{
    return config.enabled() ||
           config.mode != "off" ||
           !config.engine_path.empty() ||
           config.skeleton_id != "unknown" ||
           !config.skeleton_path.empty() ||
           config.input_width != 256 ||
           config.input_height != 256 ||
           config.input_layout != "nchw" ||
           config.input_dtype != "fp16" ||
           config.normalization != "model_default" ||
           config.roi_source != "yolo_top_detection" ||
           config.queue_depth != 32 ||
           config.timeout_ms != 500 ||
           config.prewarm_iterations != 0 ||
           !config.fail_on_init_error ||
           !config.write_events_jsonl;
}

bool validate_headless_yolo_worker_config(const HeadlessYoloWorkerConfig& config,
                                          std::string* error_out,
                                          const std::string& context)
{
    const std::string prefix = context.empty() ? "" : context + ": ";
    if (config.mode != "off" && config.mode != "real") {
        if (error_out) {
            *error_out = prefix + "yolo_worker.mode must be off|real";
        }
        return false;
    }
    if (config.mode == "off") {
        if (!config.engine_path.empty() ||
            config.decimate != 1 ||
            config.publish_live_ipc ||
            config.timeout_ms != 500 ||
            config.prewarm_iterations != 0 ||
            !config.fail_on_init_error) {
            if (error_out) {
                *error_out = prefix + "yolo_worker options require mode=real";
            }
            return false;
        }
        return true;
    }
    if (config.engine_path.empty()) {
        if (error_out) {
            *error_out = prefix + "yolo_worker.engine_path is required when mode=real";
        }
        return false;
    }
    if (config.decimate <= 0) {
        if (error_out) {
            *error_out = prefix + "yolo_worker.decimate must be > 0";
        }
        return false;
    }
    if (config.timeout_ms <= 0) {
        if (error_out) {
            *error_out = prefix + "yolo_worker.timeout_ms must be > 0";
        }
        return false;
    }
    if (config.prewarm_iterations < 0) {
        if (error_out) {
            *error_out = prefix + "yolo_worker.prewarm_iterations must be >= 0";
        }
        return false;
    }
    return true;
}

bool validate_headless_pose_worker_config(const HeadlessPoseWorkerConfig& config,
                                          std::string* error_out,
                                          const std::string& context)
{
    const std::string prefix = context.empty() ? "" : context + ": ";
    if (config.mode != "off" && config.mode != "noop" && config.mode != "real") {
        if (error_out) {
            *error_out = prefix + "pose_worker.mode must be off|noop|real";
        }
        return false;
    }
    if (config.mode == "off") {
        if (!config.engine_path.empty() ||
            config.skeleton_id != "unknown" ||
            !config.skeleton_path.empty() ||
            config.input_width != 256 ||
            config.input_height != 256 ||
            config.input_layout != "nchw" ||
            config.input_dtype != "fp16" ||
            config.normalization != "model_default" ||
            config.roi_source != "yolo_top_detection" ||
            config.synthetic_detection_every_n_frames != 1 ||
            config.synthetic_detection_box_width_px != 64 ||
            config.synthetic_detection_box_height_px != 64 ||
            config.synthetic_detection_label != 0 ||
            config.synthetic_detection_confidence != 0.99 ||
            config.queue_depth != 32 ||
            config.crop_frame_pool_size != 0 ||
            config.timeout_ms != 500 ||
            config.prewarm_iterations != 0 ||
            !config.fail_on_init_error ||
            !config.write_events_jsonl) {
            if (error_out) {
                *error_out = prefix + "pose_worker options require mode=noop|real";
            }
            return false;
        }
        return true;
    }
    if (config.mode == "real" && config.engine_path.empty()) {
        if (error_out) {
            *error_out = prefix + "pose_worker.engine_path is required when mode=real";
        }
        return false;
    }
    if (config.mode == "noop" && !config.engine_path.empty()) {
        if (error_out) {
            *error_out = prefix + "pose_worker.engine_path requires mode=real";
        }
        return false;
    }
    if (config.input_width <= 0 || config.input_height <= 0) {
        if (error_out) {
            *error_out = prefix + "pose_worker input_width/input_height must be > 0";
        }
        return false;
    }
    if (config.input_layout != "nchw" && config.input_layout != "nhwc") {
        if (error_out) {
            *error_out = prefix + "pose_worker.input_layout must be nchw|nhwc";
        }
        return false;
    }
    if (config.input_dtype != "fp16" &&
        config.input_dtype != "fp32" &&
        config.input_dtype != "uint8") {
        if (error_out) {
            *error_out = prefix + "pose_worker.input_dtype must be fp16|fp32|uint8";
        }
        return false;
    }
    if (config.roi_source != "yolo_top_detection" &&
        config.roi_source != "synthetic_center_box") {
        if (error_out) {
            *error_out =
                prefix + "pose_worker.roi_source must be yolo_top_detection|synthetic_center_box";
        }
        return false;
    }
    if (config.synthetic_runtime_detection_enabled()) {
        if (config.synthetic_detection_every_n_frames <= 0) {
            if (error_out) {
                *error_out =
                    prefix + "pose_worker.synthetic_detection.every_n_frames must be > 0";
            }
            return false;
        }
        if (config.synthetic_detection_box_width_px <= 0 ||
            config.synthetic_detection_box_height_px <= 0) {
            if (error_out) {
                *error_out =
                    prefix + "pose_worker.synthetic_detection box dimensions must be > 0";
            }
            return false;
        }
        if (config.synthetic_detection_label < 0) {
            if (error_out) {
                *error_out = prefix + "pose_worker.synthetic_detection.label must be >= 0";
            }
            return false;
        }
        if (config.synthetic_detection_confidence < 0.0 ||
            config.synthetic_detection_confidence > 1.0) {
            if (error_out) {
                *error_out =
                    prefix + "pose_worker.synthetic_detection.confidence must be in [0,1]";
            }
            return false;
        }
    }
    if (config.queue_depth <= 0) {
        if (error_out) {
            *error_out = prefix + "pose_worker.queue_depth must be > 0";
        }
        return false;
    }
    if (config.crop_frame_pool_size < 0 || config.crop_frame_pool_size > 512) {
        if (error_out) {
            *error_out = prefix + "pose_worker.crop_frame_pool_size must be in [0,512]";
        }
        return false;
    }
    if (config.timeout_ms <= 0) {
        if (error_out) {
            *error_out = prefix + "pose_worker.timeout_ms must be > 0";
        }
        return false;
    }
    if (config.prewarm_iterations < 0) {
        if (error_out) {
            *error_out = prefix + "pose_worker.prewarm_iterations must be >= 0";
        }
        return false;
    }
    if (!config.write_events_jsonl) {
        if (error_out) {
            *error_out = prefix + "pose_worker.write_events_jsonl must be true for headless validation";
        }
        return false;
    }
    return true;
}

bool validate_headless_recording_control_config(
    const HeadlessRecordingControlConfig& config,
    std::string* error_out,
    const std::string& context)
{
    return orange::session::validate_recording_control_config(
        orange::session::RecordingControlConfig{
            config.record_for_seconds,
            config.clip_seconds
        },
        error_out,
        context);
}

bool validate_headless_external_recorder_contract_config(
    const HeadlessExternalRecorderContractConfig& config,
    std::string* error_out,
    const std::string& context)
{
    const std::string prefix = context.empty() ? "" : context + ": ";
    if (config.mode != "off" && config.mode != "diagnostic_ipc_v1") {
        if (error_out) {
            *error_out = prefix + "external_recorder_contract.mode must be off|diagnostic_ipc_v1";
        }
        return false;
    }
    if (config.schema_id != "orange.external_recorder.contract") {
        if (error_out) {
            *error_out =
                prefix + "external_recorder_contract.schema_id must be orange.external_recorder.contract";
        }
        return false;
    }
    if (config.schema_version != 1) {
        if (error_out) {
            *error_out = prefix + "external_recorder_contract.schema_version must be 1";
        }
        return false;
    }
    if (!config.streams.is_object()) {
        if (error_out) {
            *error_out = prefix + "external_recorder_contract.streams must be an object";
        }
        return false;
    }
    if (!config.enabled()) {
        if (config.supervise_processes) {
            if (error_out) {
                *error_out =
                    prefix + "external_recorder_contract.supervise_processes requires enabled mode";
            }
            return false;
        }
        return true;
    }
    if (config.artifact_root.empty()) {
        if (error_out) {
            *error_out = prefix + "external_recorder_contract.artifact_root is required";
        }
        return false;
    }
    if (config.streams.empty()) {
        if (error_out) {
            *error_out = prefix + "external_recorder_contract.streams must not be empty";
        }
        return false;
    }
    for (auto it = config.streams.begin(); it != config.streams.end(); ++it) {
        if (!it.value().is_object()) {
            if (error_out) {
                *error_out = prefix + "external_recorder_contract.streams entries must be objects";
            }
            return false;
        }
        const nlohmann::json& stream = it.value();
        const std::string stream_id = stream.value("stream_id", it.key());
        if (stream_id.empty()) {
            if (error_out) {
                *error_out = prefix + "external_recorder_contract stream_id must not be empty";
            }
            return false;
        }
        if (config.require_summary && stream.value("summary_json", "").empty()) {
            if (error_out) {
                *error_out = prefix + "external_recorder_contract summary_json is required";
            }
            return false;
        }
        if (config.require_video_sanity && stream.value("video_sanity_json", "").empty()) {
            if (error_out) {
                *error_out = prefix + "external_recorder_contract video_sanity_json is required";
            }
            return false;
        }
        if (stream.value("mp4", "").empty()) {
            if (error_out) {
                *error_out = prefix + "external_recorder_contract mp4 is required";
            }
            return false;
        }
        if (config.require_gop_routing && stream.value("gop_routing_csv", "").empty()) {
            if (error_out) {
                *error_out = prefix + "external_recorder_contract gop_routing_csv is required";
            }
            return false;
        }
        const std::string routing_policy =
            normalize_headless_token(stream.value("routing_policy", "single_shard"));
        if (routing_policy != "single_shard" && routing_policy != "gop_modulo") {
            if (error_out) {
                *error_out =
                    prefix + "external_recorder_contract.routing_policy must be single_shard|gop_modulo";
            }
            return false;
        }
        if (stream.contains("expected_shard_gpu_ids") &&
            !stream["expected_shard_gpu_ids"].is_array()) {
            if (error_out) {
                *error_out =
                    prefix + "external_recorder_contract.expected_shard_gpu_ids must be an array";
            }
            return false;
        }
    }
    return true;
}

bool validate_pre_encoder_reference_capture_config(const PreEncoderReferenceCaptureConfig& config,
                                                   std::string* error_out,
                                                   const std::string& context)
{
    if (!pre_encoder_reference_capture_requested(config)) {
        return true;
    }

    const std::string prefix = context.empty() ? "" : context + ": ";
    if (!config.enabled) {
        if (error_out) {
            *error_out = prefix +
                "pre_encoder_reference_capture requires enabled=true when any capture fields are provided";
        }
        return false;
    }
    if (!config.has_valid_bound()) {
        if (error_out) {
            *error_out = prefix +
                "pre_encoder_reference_capture requires exactly one positive bound: max_frames or max_seconds";
        }
        return false;
    }
    return true;
}

nlohmann::json build_pre_encoder_reference_capture_json(const PreEncoderReferenceCaptureConfig& config)
{
    nlohmann::json out = {
        {"enabled", config.enabled},
        {"max_frames", config.max_frames},
        {"max_seconds", config.max_seconds}
    };
    if (!config.output_dir.empty()) {
        out["output_dir"] = config.output_dir;
    }
    return out;
}

bool parse_pre_encoder_reference_capture_json(const nlohmann::json& node,
                                              PreEncoderReferenceCaptureConfig* config_out,
                                              std::string* error_out,
                                              const std::string& context)
{
    if (!config_out) {
        if (error_out) {
            *error_out = context + ": internal error: null pre-encoder reference config destination";
        }
        return false;
    }
    if (!node.is_object()) {
        if (error_out) {
            *error_out = context + ": pre_encoder_reference_capture must be a JSON object";
        }
        return false;
    }

    PreEncoderReferenceCaptureConfig config;
    config.enabled = node.value("enabled", true);
    config.max_frames = node.value("max_frames", 0);
    config.max_seconds = node.value("max_seconds", 0);
    config.output_dir = node.value("output_dir", "");
    if (!validate_pre_encoder_reference_capture_config(config, error_out, context)) {
        return false;
    }

    *config_out = config;
    return true;
}

nlohmann::json build_headless_frame_ipc_config_json(const HeadlessFrameIpcConfig& config)
{
    return {
        {"enabled", config.enabled()},
        {"mode", headless_frame_ipc_mode_to_string(config.mode)},
        {"queue_naming", "serial"},
        {"unlink_existing_queues", config.unlink_existing_queues},
        {"require_base_frames", config.require_base_frames},
        {"allow_push_failures", config.allow_push_failures}
    };
}

bool parse_headless_frame_ipc_json(const nlohmann::json& node,
                                   HeadlessFrameIpcConfig* config_out,
                                   std::string* error_out,
                                   const std::string& context)
{
    if (!config_out) {
        if (error_out) {
            *error_out = context + ": internal error: null frame_ipc destination";
        }
        return false;
    }

    HeadlessFrameIpcConfig config;
    if (node.is_boolean()) {
        config.mode = node.get<bool>() ? HeadlessFrameIpcMode::ProducerOnly : HeadlessFrameIpcMode::Off;
        *config_out = config;
        return true;
    }
    if (!node.is_object()) {
        if (error_out) {
            *error_out = context + ": frame_ipc must be a boolean or JSON object";
        }
        return false;
    }

    const bool has_enabled_field = node.contains("enabled");
    const bool has_mode_field = node.contains("mode");
    const bool enabled = node.value("enabled", false);
    const std::string mode_string = node.value("mode", enabled ? "producer_only" : "off");
    if (!parse_headless_frame_ipc_mode(mode_string, &config.mode)) {
        if (error_out) {
            *error_out =
                context + ": frame_ipc.mode must be off|producer_only|verify_drain|verify_drain_v2";
        }
        return false;
    }
    if (!has_enabled_field && !has_mode_field) {
        config.mode = HeadlessFrameIpcMode::Off;
    } else if (has_enabled_field && !enabled) {
        config.mode = HeadlessFrameIpcMode::Off;
    } else if (config.mode == HeadlessFrameIpcMode::Off) {
        if (enabled) {
            if (error_out) {
                *error_out =
                    context + ": frame_ipc.enabled=true requires mode producer_only, verify_drain, or verify_drain_v2";
            }
            return false;
        }
    }
    config.unlink_existing_queues = node.value("unlink_existing_queues", false);
    config.require_base_frames = node.value("require_base_frames", true);
    config.allow_push_failures = node.value("allow_push_failures", false);

    if (!config.enabled() &&
        (config.unlink_existing_queues || config.allow_push_failures || !config.require_base_frames)) {
        if (error_out) {
            *error_out =
                context + ": frame_ipc options require enabled=true";
        }
        return false;
    }

    *config_out = config;
    return true;
}

bool parse_headless_yolo_event_log_json(
    const nlohmann::json& node,
    yolo_event_log::SyntheticYoloEventConfig* config_out,
    std::string* error_out,
    const std::string& context)
{
    if (!config_out) {
        if (error_out) {
            *error_out = context + ": internal error: null yolo_event_log destination";
        }
        return false;
    }

    yolo_event_log::SyntheticYoloEventConfig config;
    if (node.is_boolean()) {
        config.mode = node.get<bool>() ? "synthetic" : "off";
        *config_out = config;
        return true;
    }
    if (!node.is_object()) {
        if (error_out) {
            *error_out = context + ": yolo_event_log must be a boolean or JSON object";
        }
        return false;
    }

    config.mode = node.value("mode", "off");
    std::string normalized_mode;
    normalized_mode.reserve(config.mode.size());
    for (char c : config.mode) {
        normalized_mode.push_back(
            c == '-' ? '_' : static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    }
    config.mode = normalized_mode;
    if (config.mode == "false" || config.mode == "disabled" || config.mode == "none") {
        config.mode = "off";
    } else if (config.mode == "true" || config.mode == "on") {
        config.mode = "synthetic";
    }
    if (config.mode != "off" && config.mode != "synthetic") {
        if (error_out) {
            *error_out = context + ": yolo_event_log.mode must be off|synthetic";
        }
        return false;
    }

    config.every_n_frames = node.value("every_n_frames", config.every_n_frames);
    config.pattern = node.value("pattern", config.pattern);
    config.emit_zero_detections =
        node.value("emit_zero_detections", config.emit_zero_detections);
    config.label = node.value("label", config.label);
    config.confidence = node.value("confidence", config.confidence);

    std::string normalized_pattern;
    normalized_pattern.reserve(config.pattern.size());
    for (char c : config.pattern) {
        normalized_pattern.push_back(
            c == '-' ? '_' : static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    }
    config.pattern = normalized_pattern;

    if (config.enabled()) {
        if (config.every_n_frames <= 0) {
            if (error_out) {
                *error_out = context + ": yolo_event_log.every_n_frames must be > 0";
            }
            return false;
        }
        if (config.pattern != "alternating") {
            if (error_out) {
                *error_out = context + ": yolo_event_log.pattern must be alternating";
            }
            return false;
        }
        if (config.confidence < 0.0 || config.confidence > 1.0) {
            if (error_out) {
                *error_out = context + ": yolo_event_log.confidence must be in [0, 1]";
            }
            return false;
        }
    }

    *config_out = config;
    return true;
}

bool parse_headless_yolo_worker_json(
    const nlohmann::json& node,
    HeadlessYoloWorkerConfig* config_out,
    std::string* error_out,
    const std::string& context)
{
    if (!config_out) {
        if (error_out) {
            *error_out = context + ": internal error: null yolo_worker destination";
        }
        return false;
    }

    HeadlessYoloWorkerConfig config;
    if (node.is_boolean()) {
        config.mode = node.get<bool>() ? "real" : "off";
    } else if (node.is_object()) {
        config.mode = normalize_headless_token(node.value("mode", "off"));
        if (config.mode == "true" || config.mode == "on") {
            config.mode = "real";
        } else if (config.mode == "false" || config.mode == "disabled" ||
                   config.mode == "none") {
            config.mode = "off";
        }
        config.engine_path = node.value("engine_path", "");
        config.decimate = node.value("decimate", config.decimate);
        config.publish_live_ipc =
            node.value("publish_live_ipc", config.publish_live_ipc);
        config.timeout_ms = node.value("timeout_ms", config.timeout_ms);
        config.prewarm_iterations =
            node.value("prewarm_iterations", config.prewarm_iterations);
        config.fail_on_init_error =
            node.value("fail_on_init_error", config.fail_on_init_error);
    } else {
        if (error_out) {
            *error_out = context + ": yolo_worker must be a boolean or JSON object";
        }
        return false;
    }

    if (!validate_headless_yolo_worker_config(config, error_out, context)) {
        return false;
    }

    *config_out = config;
    return true;
}

bool parse_headless_pose_worker_json(
    const nlohmann::json& node,
    HeadlessPoseWorkerConfig* config_out,
    std::string* error_out,
    const std::string& context)
{
    if (!config_out) {
        if (error_out) {
            *error_out = context + ": internal error: null pose_worker destination";
        }
        return false;
    }

    HeadlessPoseWorkerConfig config;
    if (node.is_boolean()) {
        config.mode = node.get<bool>() ? "noop" : "off";
    } else if (node.is_object()) {
        config.mode = normalize_headless_token(node.value("mode", "off"));
        if (config.mode == "true" || config.mode == "on") {
            config.mode = "noop";
        } else if (config.mode == "false" || config.mode == "disabled" ||
                   config.mode == "none") {
            config.mode = "off";
        }
        config.engine_path = node.value("engine_path", "");
        config.skeleton_id = node.value("skeleton_id", config.skeleton_id);
        config.skeleton_path = node.value("skeleton_path", "");
        config.input_width = node.value("input_width", config.input_width);
        config.input_height = node.value("input_height", config.input_height);
        config.input_layout =
            normalize_headless_token(node.value("input_layout", config.input_layout));
        config.input_dtype =
            normalize_headless_token(node.value("input_dtype", config.input_dtype));
        config.normalization = node.value("normalization", config.normalization);
        config.roi_source =
            normalize_headless_token(node.value("roi_source", config.roi_source));
        if (node.contains("synthetic_detection")) {
            const nlohmann::json& synthetic = node["synthetic_detection"];
            if (!synthetic.is_object()) {
                if (error_out) {
                    *error_out = context + ": pose_worker.synthetic_detection must be an object";
                }
                return false;
            }
            config.synthetic_detection_every_n_frames =
                synthetic.value("every_n_frames",
                                config.synthetic_detection_every_n_frames);
            config.synthetic_detection_box_width_px =
                synthetic.value("box_width_px",
                                config.synthetic_detection_box_width_px);
            config.synthetic_detection_box_height_px =
                synthetic.value("box_height_px",
                                config.synthetic_detection_box_height_px);
            config.synthetic_detection_label =
                synthetic.value("label", config.synthetic_detection_label);
            config.synthetic_detection_confidence =
                synthetic.value("confidence",
                                config.synthetic_detection_confidence);
        }
        config.queue_depth = node.value("queue_depth", config.queue_depth);
        config.crop_frame_pool_size =
            node.value("crop_frame_pool_size", config.crop_frame_pool_size);
        config.timeout_ms = node.value("timeout_ms", config.timeout_ms);
        config.prewarm_iterations =
            node.value("prewarm_iterations", config.prewarm_iterations);
        config.fail_on_init_error =
            node.value("fail_on_init_error", config.fail_on_init_error);
        config.write_events_jsonl =
            node.value("write_events_jsonl", config.write_events_jsonl);
    } else {
        if (error_out) {
            *error_out = context + ": pose_worker must be a boolean or JSON object";
        }
        return false;
    }

    if (!validate_headless_pose_worker_config(config, error_out, context)) {
        return false;
    }

    *config_out = config;
    return true;
}

bool parse_headless_recording_control_json(
    const nlohmann::json& node,
    HeadlessRecordingControlConfig* config_out,
    std::string* error_out,
    const std::string& context)
{
    if (!config_out) {
        if (error_out) {
            *error_out = context + ": internal error: null recording_control destination";
        }
        return false;
    }
    if (!node.is_object()) {
        if (error_out) {
            *error_out = context + ": recording_control must be a JSON object";
        }
        return false;
    }

    HeadlessRecordingControlConfig config;
    config.record_for_seconds =
        node.value("record_for_seconds", config.record_for_seconds);
    config.clip_seconds = node.value("clip_seconds", config.clip_seconds);

    if (!validate_headless_recording_control_config(config, error_out, context)) {
        return false;
    }

    *config_out = config;
    return true;
}

bool parse_headless_external_recorder_contract_json(
    const nlohmann::json& node,
    HeadlessExternalRecorderContractConfig* config_out,
    std::string* error_out,
    const std::string& context)
{
    if (!config_out) {
        if (error_out) {
            *error_out =
                context + ": internal error: null external_recorder_contract destination";
        }
        return false;
    }

    HeadlessExternalRecorderContractConfig config;
    if (node.is_boolean()) {
        config.mode = node.get<bool>() ? "diagnostic_ipc_v1" : "off";
    } else if (node.is_object()) {
        const nlohmann::json contract_node =
            orange::external_recorder::ExtractExternalRecorderContractObject(node);
        config.schema_id = contract_node.value("schema_id", config.schema_id);
        config.schema_version =
            contract_node.value("schema_version", config.schema_version);
        config.mode = normalize_headless_token(
            contract_node.value("mode", config.mode));
        if (config.mode == "true" || config.mode == "on") {
            config.mode = "diagnostic_ipc_v1";
        } else if (config.mode == "false" || config.mode == "disabled" ||
                   config.mode == "none") {
            config.mode = "off";
        }
        config.artifact_root =
            contract_node.value("artifact_root", config.artifact_root);
        config.session_id = contract_node.value("session_id", config.session_id);
        config.recorder_tool_path =
            contract_node.value("recorder_tool_path", config.recorder_tool_path);
        config.supervise_processes =
            contract_node.value("supervise_processes", config.supervise_processes);
        config.require_summary =
            contract_node.value("require_summary", config.require_summary);
        config.require_video_sanity =
            contract_node.value("require_video_sanity", config.require_video_sanity);
        config.require_merged_mp4 =
            contract_node.value("require_merged_mp4", config.require_merged_mp4);
        config.require_gop_routing =
            contract_node.value("require_gop_routing", config.require_gop_routing);
        config.streams = contract_node.value("streams", nlohmann::json::object());
    } else {
        if (error_out) {
            *error_out =
                context + ": external_recorder_contract must be a boolean or JSON object";
        }
        return false;
    }

    if (!validate_headless_external_recorder_contract_config(config, error_out, context)) {
        return false;
    }

    *config_out = config;
    return true;
}

bool headless_encoder_settings_is_default(const HeadlessEncoderSettings& settings)
{
    return settings.codec == "h264" &&
           settings.preset == "p1" &&
           settings.tuning == "ll" &&
           settings.rate_control_mode == "vbr" &&
           settings.importance_map.mode == "off" &&
           settings.importance_map.roi_size_px == ImportanceMapConfig::kDefaultRoiSizePx &&
           settings.quality_value == 20 &&
           settings.gop_length == 0 &&
           settings.control_overrides.aq < 0 &&
           settings.control_overrides.temporal_aq < 0 &&
           settings.control_overrides.lookahead < 0 &&
           settings.control_overrides.lookahead_depth < 0 &&
           settings.control_overrides.target_bitrate_bps < 0 &&
           settings.control_overrides.max_bitrate_bps < 0 &&
           settings.control_overrides.vbv_buffer_size < 0 &&
           settings.select_all_cameras &&
           settings.camera_serials.empty();
}

bool parse_headless_cli_options(int argc, char* argv[], HeadlessCliOptions* options, std::string* error_out)
{
    if (!options) {
        if (error_out) {
            *error_out = "Internal error: null CLI options";
        }
        return false;
    }

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        std::string inline_value;
        const std::size_t equals = arg.find('=');
        if (arg.rfind("--", 0) == 0 && equals != std::string::npos) {
            inline_value = arg.substr(equals + 1);
            arg = arg.substr(0, equals);
        }

        auto consume_value = [&](const char* flag_name) -> std::string {
            if (!inline_value.empty()) {
                return inline_value;
            }
            if (i + 1 >= argc) {
                if (error_out) {
                    *error_out = std::string("Missing value for ") + flag_name;
                }
                return {};
            }
            return argv[++i];
        };

        if (arg == "--help" || arg == "-h") {
            options->show_help = true;
            continue;
        }
        if (arg == "--mode") {
            const std::string value = consume_value("--mode");
            if (value.empty() && options->show_help == false && error_out && !error_out->empty()) {
                return false;
            }
            if (value == "remote") {
                options->mode = HeadlessMode::Remote;
            } else if (value == "local") {
                options->mode = HeadlessMode::Local;
            } else {
                if (error_out) {
                    *error_out = "Unsupported --mode value: " + value;
                }
                return false;
            }
            continue;
        }
        if (arg == "--camera") {
            const std::string value = consume_value("--camera");
            if (value.empty() && error_out && !error_out->empty()) {
                return false;
            }
            append_camera_selection(&options->encoder_settings, value);
            continue;
        }
        if (arg == "--nvenc-direct-input") {
            options->nvenc_direct_input = true;
            continue;
        }
        if (arg == "--gpu-id") {
            const std::string value = consume_value("--gpu-id");
            int gpu_id = -1;
            if (value.empty() && error_out && !error_out->empty()) {
                return false;
            }
            if (!parse_non_negative_int(value, &gpu_id)) {
                if (error_out) {
                    *error_out = "Invalid --gpu-id value: " + value;
                }
                return false;
            }
            options->required_gpu_ids.push_back(gpu_id);
            continue;
        }
        if (arg == "--config-folder") {
            options->config_folder = consume_value("--config-folder");
            if (options->config_folder.empty() && error_out && !error_out->empty()) {
                return false;
            }
            continue;
        }
        if (arg == "--record-folder") {
            options->record_folder = consume_value("--record-folder");
            if (options->record_folder.empty() && error_out && !error_out->empty()) {
                return false;
            }
            continue;
        }
        if (arg == "--codec") {
            options->encoder_settings.codec = consume_value("--codec");
            if (options->encoder_settings.codec.empty() && error_out && !error_out->empty()) {
                return false;
            }
            continue;
        }
        if (arg == "--preset") {
            options->encoder_settings.preset = consume_value("--preset");
            if (options->encoder_settings.preset.empty() && error_out && !error_out->empty()) {
                return false;
            }
            continue;
        }
        if (arg == "--tuning") {
            options->encoder_settings.tuning = consume_value("--tuning");
            if (options->encoder_settings.tuning.empty() && error_out && !error_out->empty()) {
                return false;
            }
            continue;
        }
        if (arg == "--rate-control" || arg == "--rate-control-mode" || arg == "--rc") {
            options->encoder_settings.rate_control_mode = consume_value("--rate-control");
            if (options->encoder_settings.rate_control_mode.empty() && error_out && !error_out->empty()) {
                return false;
            }
            continue;
        }
        if (arg == "--importance-map-mode") {
            const std::string value = consume_value("--importance-map-mode");
            if (value.empty() && error_out && !error_out->empty()) {
                return false;
            }
            if (!parse_importance_map_mode(value, &options->encoder_settings.importance_map.mode)) {
                if (error_out) {
                    *error_out = "Invalid --importance-map-mode value: " + value;
                }
                return false;
            }
            continue;
        }
        if (arg == "--importance-map-roi-size-px") {
            const std::string value = consume_value("--importance-map-roi-size-px");
            if (value.empty() && error_out && !error_out->empty()) {
                return false;
            }
            if (!parse_non_negative_int(value, &options->encoder_settings.importance_map.roi_size_px) ||
                options->encoder_settings.importance_map.roi_size_px <= 0) {
                if (error_out) {
                    *error_out = "Invalid --importance-map-roi-size-px value: " + value;
                }
                return false;
            }
            continue;
        }
        if (arg == "--quality" || arg == "--cq" || arg == "--qp") {
            const std::string value = consume_value("--quality");
            if (value.empty() && error_out && !error_out->empty()) {
                return false;
            }
            if (!parse_non_negative_int(value, &options->encoder_settings.quality_value) ||
                options->encoder_settings.quality_value < 1) {
                if (error_out) {
                    *error_out = "Invalid --quality value: " + value;
                }
                return false;
            }
            continue;
        }
        if (arg == "--gop") {
            const std::string value = consume_value("--gop");
            if (value.empty() && error_out && !error_out->empty()) {
                return false;
            }
            if (!parse_non_negative_int(value, &options->encoder_settings.gop_length)) {
                if (error_out) {
                    *error_out = "Invalid --gop value: " + value;
                }
                return false;
            }
            continue;
        }
        if (arg == "--aq") {
            const std::string value = consume_value("--aq");
            if (value.empty() && error_out && !error_out->empty()) {
                return false;
            }
            if (!parse_headless_toggle_override(value, &options->encoder_settings.control_overrides.aq)) {
                if (error_out) {
                    *error_out = "Invalid --aq value: " + value;
                }
                return false;
            }
            continue;
        }
        if (arg == "--temporal-aq") {
            const std::string value = consume_value("--temporal-aq");
            if (value.empty() && error_out && !error_out->empty()) {
                return false;
            }
            if (!parse_headless_toggle_override(
                    value, &options->encoder_settings.control_overrides.temporal_aq)) {
                if (error_out) {
                    *error_out = "Invalid --temporal-aq value: " + value;
                }
                return false;
            }
            continue;
        }
        if (arg == "--lookahead") {
            const std::string value = consume_value("--lookahead");
            if (value.empty() && error_out && !error_out->empty()) {
                return false;
            }
            if (!parse_headless_toggle_override(
                    value, &options->encoder_settings.control_overrides.lookahead)) {
                if (error_out) {
                    *error_out = "Invalid --lookahead value: " + value;
                }
                return false;
            }
            continue;
        }
        if (arg == "--lookahead-depth") {
            const std::string value = consume_value("--lookahead-depth");
            if (value.empty() && error_out && !error_out->empty()) {
                return false;
            }
            if (!parse_non_negative_int(value, &options->encoder_settings.control_overrides.lookahead_depth)) {
                if (error_out) {
                    *error_out = "Invalid --lookahead-depth value: " + value;
                }
                return false;
            }
            continue;
        }
        if (arg == "--target-bitrate-bps") {
            const std::string value = consume_value("--target-bitrate-bps");
            if (value.empty() && error_out && !error_out->empty()) {
                return false;
            }
            if (!parse_non_negative_int(value, &options->encoder_settings.control_overrides.target_bitrate_bps) ||
                options->encoder_settings.control_overrides.target_bitrate_bps <= 0) {
                if (error_out) {
                    *error_out = "Invalid --target-bitrate-bps value: " + value;
                }
                return false;
            }
            continue;
        }
        if (arg == "--max-bitrate-bps") {
            const std::string value = consume_value("--max-bitrate-bps");
            if (value.empty() && error_out && !error_out->empty()) {
                return false;
            }
            if (!parse_non_negative_int(value, &options->encoder_settings.control_overrides.max_bitrate_bps) ||
                options->encoder_settings.control_overrides.max_bitrate_bps <= 0) {
                if (error_out) {
                    *error_out = "Invalid --max-bitrate-bps value: " + value;
                }
                return false;
            }
            continue;
        }
        if (arg == "--vbv-buffer-size") {
            const std::string value = consume_value("--vbv-buffer-size");
            if (value.empty() && error_out && !error_out->empty()) {
                return false;
            }
            if (!parse_non_negative_int(value, &options->encoder_settings.control_overrides.vbv_buffer_size) ||
                options->encoder_settings.control_overrides.vbv_buffer_size <= 0) {
                if (error_out) {
                    *error_out = "Invalid --vbv-buffer-size value: " + value;
                }
                return false;
            }
            continue;
        }
        if (arg == "--preenc-ref-max-frames") {
            const std::string value = consume_value("--preenc-ref-max-frames");
            if (value.empty() && error_out && !error_out->empty()) {
                return false;
            }
            if (!parse_non_negative_int(value, &options->pre_encoder_reference_capture.max_frames) ||
                options->pre_encoder_reference_capture.max_frames <= 0) {
                if (error_out) {
                    *error_out = "Invalid --preenc-ref-max-frames value: " + value;
                }
                return false;
            }
            options->pre_encoder_reference_capture.enabled = true;
            continue;
        }
        if (arg == "--preenc-ref-max-seconds") {
            const std::string value = consume_value("--preenc-ref-max-seconds");
            if (value.empty() && error_out && !error_out->empty()) {
                return false;
            }
            if (!parse_non_negative_int(value, &options->pre_encoder_reference_capture.max_seconds) ||
                options->pre_encoder_reference_capture.max_seconds <= 0) {
                if (error_out) {
                    *error_out = "Invalid --preenc-ref-max-seconds value: " + value;
                }
                return false;
            }
            options->pre_encoder_reference_capture.enabled = true;
            continue;
        }
        if (arg == "--duration") {
            const std::string value = consume_value("--duration");
            if (value.empty() && error_out && !error_out->empty()) {
                return false;
            }
            if (!parse_non_negative_int(value, &options->duration_seconds)) {
                if (error_out) {
                    *error_out = "Invalid --duration value: " + value;
                }
                return false;
            }
            continue;
        }
        if (arg == "--stream-start-delay") {
            const std::string value = consume_value("--stream-start-delay");
            if (value.empty() && error_out && !error_out->empty()) {
                return false;
            }
            if (!parse_non_negative_int(value, &options->stream_start_delay_seconds)) {
                if (error_out) {
                    *error_out = "Invalid --stream-start-delay value: " + value;
                }
                return false;
            }
            continue;
        }
        if (arg == "--record-delay") {
            const std::string value = consume_value("--record-delay");
            if (value.empty() && error_out && !error_out->empty()) {
                return false;
            }
            if (!parse_non_negative_int(value, &options->record_start_delay_seconds)) {
                if (error_out) {
                    *error_out = "Invalid --record-delay value: " + value;
                }
                return false;
            }
            continue;
        }
        if (arg == "--ptp-gate-acquisition-mode") {
            options->ptp_gate_acquisition_mode_override =
                consume_value("--ptp-gate-acquisition-mode");
            if (options->ptp_gate_acquisition_mode_override.empty() && error_out && !error_out->empty()) {
                return false;
            }
            std::string normalized_mode;
            if (!parse_headless_ptp_gate_acquisition_mode_override(
                    options->ptp_gate_acquisition_mode_override, &normalized_mode)) {
                if (error_out) {
                    *error_out =
                        "Invalid --ptp-gate-acquisition-mode value: " +
                        options->ptp_gate_acquisition_mode_override;
                }
                return false;
            }
            options->ptp_gate_acquisition_mode_override = normalized_mode;
            continue;
        }
        if (arg == "--acquisition-buffer-mode") {
            options->acquisition_buffer_mode_override =
                consume_value("--acquisition-buffer-mode");
            if (options->acquisition_buffer_mode_override.empty() && error_out && !error_out->empty()) {
                return false;
            }
            std::string normalized_mode;
            if (!parse_headless_acquisition_buffer_mode_override(
                    options->acquisition_buffer_mode_override, &normalized_mode)) {
                if (error_out) {
                    *error_out =
                        "Invalid --acquisition-buffer-mode value: " +
                        options->acquisition_buffer_mode_override;
                }
                return false;
            }
            options->acquisition_buffer_mode_override = normalized_mode;
            continue;
        }
        if (arg == "--ptp-gate-stagger-ns") {
            const std::string value = consume_value("--ptp-gate-stagger-ns");
            if (value.empty() && error_out && !error_out->empty()) {
                return false;
            }
            if (!parse_non_negative_int(value, &options->ptp_gate_stagger_ns)) {
                if (error_out) {
                    *error_out = "Invalid --ptp-gate-stagger-ns value: " + value;
                }
                return false;
            }
            continue;
        }
        if (arg == "--frame-ipc") {
            const std::string value = consume_value("--frame-ipc");
            if (value.empty() && error_out && !error_out->empty()) {
                return false;
            }
            HeadlessFrameIpcMode mode = HeadlessFrameIpcMode::Off;
            if (!parse_headless_frame_ipc_mode(value, &mode)) {
                if (error_out) {
                    *error_out = "Invalid --frame-ipc value: " + value;
                }
                return false;
            }
            options->frame_ipc.mode = mode;
            continue;
        }
        if (arg == "--frame-ipc-unlink-existing") {
            options->frame_ipc.unlink_existing_queues = true;
            continue;
        }
        if (arg == "--frame-ipc-allow-push-failures") {
            options->frame_ipc.allow_push_failures = true;
            continue;
        }
        if (arg == "--yolo-engine") {
            options->yolo_worker.mode = "real";
            options->yolo_worker.engine_path = consume_value("--yolo-engine");
            if (options->yolo_worker.engine_path.empty() && error_out && !error_out->empty()) {
                return false;
            }
            continue;
        }
        if (arg == "--yolo-decimate") {
            const std::string value = consume_value("--yolo-decimate");
            if (value.empty() && error_out && !error_out->empty()) {
                return false;
            }
            if (!parse_non_negative_int(value, &options->yolo_worker.decimate) ||
                options->yolo_worker.decimate <= 0) {
                if (error_out) {
                    *error_out = "Invalid --yolo-decimate value: " + value;
                }
                return false;
            }
            continue;
        }
        if (arg == "--yolo-publish-live-ipc") {
            options->yolo_worker.publish_live_ipc = true;
            continue;
        }
        if (arg == "--recording-sink-mode") {
            options->recording_sink_mode = consume_value("--recording-sink-mode");
            if (options->recording_sink_mode.empty() && error_out && !error_out->empty()) {
                return false;
            }
            std::string normalized_mode;
            if (!parse_headless_recording_sink_mode_override(
                    options->recording_sink_mode, &normalized_mode)) {
                if (error_out) {
                    *error_out =
                        "Invalid --recording-sink-mode value: " +
                        options->recording_sink_mode;
                }
                return false;
            }
            options->recording_sink_mode = normalized_mode;
            continue;
        }
        if (arg == "--list-cameras") {
            options->list_cameras = true;
            continue;
        }
        if (arg == "--stream-only") {
            options->stream_only = true;
            continue;
        }
        if (arg == "--experiment-spec") {
            options->experiment_spec_path = consume_value("--experiment-spec");
            if (options->experiment_spec_path.empty() && error_out && !error_out->empty()) {
                return false;
            }
            continue;
        }

        if (error_out) {
            *error_out = "Unknown argument: " + arg;
        }
        return false;
    }

    return true;
}

std::vector<int> resolve_selected_camera_indices(const CameraParams* cameras_params,
                                                 int num_cameras,
                                                 const HeadlessEncoderSettings& settings,
                                                 const std::vector<int>& allowed_gpu_ids = {})
{
    std::vector<int> indices;
    if (!cameras_params || num_cameras <= 0) {
        return indices;
    }

    if (settings.select_all_cameras || settings.camera_serials.empty()) {
        indices.reserve(num_cameras);
        for (int i = 0; i < num_cameras; ++i) {
            indices.push_back(i);
        }
    } else {
        std::unordered_map<std::string, int> index_by_serial;
        index_by_serial.reserve(static_cast<std::size_t>(num_cameras));
        for (int i = 0; i < num_cameras; ++i) {
            index_by_serial.emplace(cameras_params[i].camera_serial, i);
        }

        std::unordered_set<int> seen_indices;
        for (const std::string& serial : settings.camera_serials) {
            auto it = index_by_serial.find(serial);
            if (it == index_by_serial.end()) {
                std::ostringstream available;
                for (int i = 0; i < num_cameras; ++i) {
                    if (i != 0) {
                        available << ",";
                    }
                    available << cameras_params[i].camera_serial;
                }
                throw std::runtime_error(
                    "Requested camera serial " + serial +
                    " was not found on this host. Available serials: " + available.str());
            }
            if (seen_indices.insert(it->second).second) {
                indices.push_back(it->second);
            }
        }
    }

    if (!allowed_gpu_ids.empty()) {
        std::unordered_set<int> allowed(allowed_gpu_ids.begin(), allowed_gpu_ids.end());
        std::vector<int> filtered;
        filtered.reserve(indices.size());
        for (int idx : indices) {
            if (allowed.count(cameras_params[idx].gpu_id) > 0) {
                filtered.push_back(idx);
                continue;
            }
            if (!settings.select_all_cameras && !settings.camera_serials.empty()) {
                throw std::runtime_error(
                    "Requested camera serial " + cameras_params[idx].camera_serial +
                    " resolved to gpu_id=" + std::to_string(cameras_params[idx].gpu_id) +
                    ", which is not in the allowed gpu_ids set.");
            }
        }
        indices.swap(filtered);
        if (indices.empty()) {
            throw std::runtime_error("No selected cameras matched the requested gpu_ids filter.");
        }
    }

    return indices;
}

bool build_camera_gpu_override_map(const HeadlessEncoderSettings& settings,
                                   const std::vector<int>& requested_gpu_ids,
                                   CameraGpuOverrideMap* overrides_out,
                                   std::string* error_out)
{
    if (!overrides_out) {
        if (error_out) {
            *error_out = "Internal error: null GPU override destination";
        }
        return false;
    }

    overrides_out->clear();
    if (requested_gpu_ids.empty() || settings.select_all_cameras || settings.camera_serials.empty()) {
        return true;
    }

    if (requested_gpu_ids.size() != 1 &&
        requested_gpu_ids.size() != settings.camera_serials.size()) {
        if (error_out) {
            *error_out =
                "For explicit camera selection, gpu_ids must contain either one shared GPU id "
                "or exactly one GPU id per selected camera.";
        }
        return false;
    }

    if (requested_gpu_ids.size() == 1) {
        for (const std::string& serial : settings.camera_serials) {
            (*overrides_out)[serial] = requested_gpu_ids.front();
        }
        return true;
    }

    for (std::size_t i = 0; i < settings.camera_serials.size(); ++i) {
        (*overrides_out)[settings.camera_serials[i]] = requested_gpu_ids[i];
    }
    return true;
}

void apply_camera_gpu_overrides(CameraParams* cameras_params,
                                int num_cameras,
                                const CameraGpuOverrideMap& overrides)
{
    if (!cameras_params || num_cameras <= 0 || overrides.empty()) {
        return;
    }

    for (int i = 0; i < num_cameras; ++i) {
        const auto it = overrides.find(cameras_params[i].camera_serial);
        if (it == overrides.end()) {
            continue;
        }

        cameras_params[i].gpu_id = it->second;
        cameras_params[i].gpu_id_runtime_overridden =
            (cameras_params[i].configured_gpu_id >= 0 &&
             cameras_params[i].configured_gpu_id != cameras_params[i].gpu_id);
    }
}

void allocate_selected_camera_frame_buffers(CameraEmergent* ecams,
                                            CameraParams* cameras_params,
                                            const std::vector<int>& selected_indices)
{
    for (int idx : selected_indices) {
        camera_open_stream(&ecams[idx].camera, &cameras_params[idx], "headless_start_camera_thread");
        ecams[idx].evt_frame = new Emergent::CEmergentFrame[evt_buffer_size];
        ecams[idx].evt_frame_count = evt_buffer_size;
        allocate_frame_buffer(&ecams[idx].camera, ecams[idx].evt_frame, &cameras_params[idx], evt_buffer_size);
        if (cameras_params[idx].need_reorder && cameras_params[idx].gpu_direct) {
            allocate_frame_reorder_buffer(&ecams[idx].camera, &ecams[idx].frame_reorder, &cameras_params[idx]);
        }
    }
}

void print_available_cameras(GigEVisionDeviceInfo* device_info, int cam_count)
{
    std::cout << "Available cameras: " << cam_count << std::endl;
    for (int i = 0; i < cam_count; ++i) {
        std::cout << "  [" << i << "] serial=" << device_info[i].serialNumber
                  << " ip=" << device_info[i].currentIp
                  << " nic_ip=" << device_info[i].nic.ip4Address
                  << " model=" << device_info[i].modelName
                  << std::endl;
    }
}

std::vector<int> resolve_selected_device_inventory_indices(const GigEVisionDeviceInfo* device_info,
                                                           int cam_count,
                                                           const HeadlessEncoderSettings& settings)
{
    std::vector<int> indices;
    if (!device_info || cam_count <= 0) {
        return indices;
    }

    if (settings.select_all_cameras || settings.camera_serials.empty()) {
        indices.reserve(cam_count);
        for (int i = 0; i < cam_count; ++i) {
            indices.push_back(i);
        }
        return indices;
    }

    std::unordered_map<std::string, int> index_by_serial;
    index_by_serial.reserve(static_cast<std::size_t>(cam_count));
    for (int i = 0; i < cam_count; ++i) {
        index_by_serial.emplace(
            canonicalize_headless_camera_serial(device_info[i].serialNumber),
            i);
    }

    std::unordered_set<int> seen_indices;
    for (const std::string& serial : settings.camera_serials) {
        auto it = index_by_serial.find(serial);
        if (it == index_by_serial.end()) {
            std::ostringstream available;
            for (int i = 0; i < cam_count; ++i) {
                if (i != 0) {
                    available << ",";
                }
                available << canonicalize_headless_camera_serial(device_info[i].serialNumber);
            }
            throw std::runtime_error(
                "Requested camera serial " + serial +
                " was not found on this host. Available serials: " + available.str());
        }
        if (seen_indices.insert(it->second).second) {
            indices.push_back(it->second);
        }
    }

    return indices;
}

void cleanup_selected_camera_buffers(const std::vector<int>& active_camera_indices,
                                     CameraEmergent* ecams,
                                     CameraParams* cameras_params,
                                     std::vector<CameraResources>& camera_resources)
{
    for (int idx : active_camera_indices) {
        if (ecams[idx].evt_frame) {
            destroy_frame_buffer(&ecams[idx].camera, ecams[idx].evt_frame, evt_buffer_size, &cameras_params[idx]);
            delete[] ecams[idx].evt_frame;
            ecams[idx].evt_frame = nullptr;
            ecams[idx].evt_frame_count = 0;
            check_camera_errors(EVT_CameraCloseStream(&ecams[idx].camera), cameras_params[idx].camera_serial.c_str());
        }
        if (idx >= 0 && idx < static_cast<int>(camera_resources.size())) {
            camera_resources[idx].cleanup();
        }
    }
}

void stop_headless_yolo_workers(std::vector<std::unique_ptr<YoloWorker>>& yolo_workers)
{
    for (auto& worker : yolo_workers) {
        if (worker) {
            worker->StopThread();
        }
    }
    yolo_workers.clear();
}

void stop_headless_pose_pipeline(
    std::vector<std::unique_ptr<CropProducerWorker>>& crop_producer_workers,
    std::vector<std::unique_ptr<PoseWorker>>& pose_workers)
{
    for (auto& worker : crop_producer_workers) {
        if (worker) {
            worker->StopThread();
            worker->CloseRecording();
        }
    }

    for (auto& worker : pose_workers) {
        if (worker) {
            worker->StopThread();
            worker->CloseRecording();
        }
    }

    crop_producer_workers.clear();
    pose_workers.clear();
}

void close_all_cameras(CameraEmergent* ecams,
                       CameraParams* cameras_params,
                       int num_cameras)
{
    for (int i = 0; i < num_cameras; ++i) {
        close_camera(&ecams[i].camera, &cameras_params[i]);
    }
}

void close_selected_cameras(const std::vector<int>& selected_indices,
                            CameraEmergent* ecams,
                            CameraParams* cameras_params)
{
    for (int idx : selected_indices) {
        close_camera(&ecams[idx].camera, &cameras_params[idx]);
    }
}

void reset_ptp_params(PTPParams* ptp_params)
{
    if (!ptp_params) {
        return;
    }
    ptp_params->ptp_global_time = 0;
    ptp_params->ptp_stop_time = 0;
    ptp_params->ptp_counter = 0;
    ptp_params->ptp_stop_counter = 0;
    ptp_params->network_sync = true;
    ptp_params->network_set_start_ptp = false;
    ptp_params->ptp_stop_reached = false;
    ptp_params->ptp_start_reached = false;
    ptp_params->network_set_stop_ptp = false;
}

void drain_and_shutdown_recording(std::vector<std::unique_ptr<ModernRecordingPipeline>>& recording_pipelines,
                                  CameraControl* camera_control);
void request_recording_drain(std::vector<std::unique_ptr<ModernRecordingPipeline>>& recording_pipelines,
                             CameraControl* camera_control);
bool wait_for_recording_drain(CameraControl* camera_control,
                              std::chrono::steady_clock::duration timeout,
                              const char* timeout_label);

void shutdown_headless_run(std::vector<std::thread>& camera_threads,
                           std::vector<CameraResources>& camera_resources,
                           std::vector<int>& active_camera_indices,
                           std::vector<std::unique_ptr<ModernRecordingPipeline>>& recording_pipelines,
                           std::vector<std::unique_ptr<YoloWorker>>& yolo_workers,
                           std::vector<std::unique_ptr<CropProducerWorker>>& crop_producer_workers,
                           std::vector<std::unique_ptr<PoseWorker>>& pose_workers,
                           HeadlessGpuDmonMonitor* gpu_dmon_monitor,
                           CameraEmergent* ecams,
                           CameraParams* cameras_params,
                           int num_cameras,
                           const std::vector<int>* opened_camera_indices,
                           CameraControl* camera_control,
                           PTPParams* ptp_params,
                           bool reset_ptp_state)
{
    if (camera_control) {
        if (camera_control->record_video ||
            camera_control->active_recorders.load(std::memory_order_relaxed) > 0) {
            request_recording_drain(recording_pipelines, camera_control);
            (void)wait_for_recording_drain(
                camera_control,
                std::chrono::seconds(10),
                "Headless live recording drain");
        }
        camera_control->subscribe = false;
    }

    for (auto& t : camera_threads) {
        if (t.joinable()) {
            t.join();
        }
    }
    camera_threads.clear();

    stop_headless_yolo_workers(yolo_workers);
    stop_headless_pose_pipeline(crop_producer_workers, pose_workers);

    if (camera_control) {
        drain_and_shutdown_recording(recording_pipelines, camera_control);
        stop_headless_gpu_dmon_monitor(gpu_dmon_monitor);
        if (camera_control->sync_camera) {
            for (int idx : active_camera_indices) {
                ptp_sync_off(&ecams[idx].camera, &cameras_params[idx]);
            }
        }
        camera_control->sync_camera = false;
    } else {
        stop_headless_gpu_dmon_monitor(gpu_dmon_monitor);
    }
    recording_pipelines.clear();

    cleanup_selected_camera_buffers(active_camera_indices, ecams, cameras_params, camera_resources);
    active_camera_indices.clear();
    camera_resources.clear();

    if (reset_ptp_state) {
        reset_ptp_params(ptp_params);
    }

    if (opened_camera_indices && !opened_camera_indices->empty()) {
        close_selected_cameras(*opened_camera_indices, ecams, cameras_params);
    } else {
        close_all_cameras(ecams, cameras_params, num_cameras);
    }
}

CameraRecordingOutputConfig build_native_recording_output_preferences(const CameraParams& camera_params)
{
    CameraRecordingOutputConfig output;
    output.mode = "factor";
    output.downsample_factor = 1;
    output.requested_width = static_cast<int>(camera_params.width);
    output.requested_height = static_cast<int>(camera_params.height);
    return output;
}

bool prepare_headless_recording_artifacts(const std::string& record_folder,
                                          CameraControl* camera_control,
                                          CameraParams* cameras_params,
                                          int num_cameras,
                                          PTPParams* ptp_params,
                                          bool update_latest_pointer,
                                          const std::string& recording_sink_mode)
{
    if (record_folder.empty()) {
        std::cerr << "Headless recording folder is empty." << std::endl;
        return false;
    }

    const std::filesystem::path recording_path(record_folder);
    std::error_code create_error;
    {
        orange::ScopedFsuid fsuid_guard;
        (void)fsuid_guard;
        std::filesystem::create_directories(recording_path, create_error);
    }
    if (create_error && !std::filesystem::exists(recording_path)) {
        std::cerr << "Failed to create recording folder " << record_folder
                  << ": " << create_error.message() << std::endl;
        return false;
    }

    const std::string recording_id = recording_path.filename().string();
    const std::filesystem::path base_path = recording_path.parent_path().empty()
        ? recording_path
        : recording_path.parent_path();

    {
        std::lock_guard<std::mutex> lock(camera_control->recording_folder_mutex);
        camera_control->recording_folder = record_folder;
    }

    if (!write_recording_snapshot(
            record_folder,
            recording_id,
            cameras_params,
            num_cameras,
            base_path.string(),
            update_latest_pointer,
            camera_control->sync_camera,
            ptp_params,
            recording_sink_mode)) {
        std::cerr << "Failed to write headless recording snapshot for " << record_folder << std::endl;
        return false;
    }

    if (!initialize_ptp_sync_summary(
            record_folder,
            recording_id,
            num_cameras,
            camera_control->sync_camera,
            ptp_params)) {
        std::cerr << "Failed to initialize headless PTP summary for " << record_folder << std::endl;
        return false;
    }

    std::cout << "Headless run artifacts save to : " << record_folder << std::endl;
    return true;
}

void drain_and_shutdown_recording(std::vector<std::unique_ptr<ModernRecordingPipeline>>& recording_pipelines,
                                  CameraControl* camera_control)
{
    request_recording_drain(recording_pipelines, camera_control);

    (void)wait_for_recording_drain(
        camera_control,
        std::chrono::seconds(10),
        "Headless recording drain");

    for (auto& pipeline : recording_pipelines) {
        if (!pipeline) {
            continue;
        }
        pipeline->request_stop();
    }

    for (auto& pipeline : recording_pipelines) {
        if (!pipeline) {
            continue;
        }
        pipeline->shutdown();
        pipeline.reset();
    }

    camera_control->recording_draining = false;
    camera_control->stop_record = false;
    std::lock_guard<std::mutex> lock(camera_control->recording_folder_mutex);
    camera_control->recording_folder.clear();
    camera_control->recording_output_folder.clear();
    camera_control->pending_recording_output_folder.clear();
    camera_control->recording_rollover_at_frame_id = 0;
    camera_control->recording_rollover_request_id = 0;
    camera_control->recording_rollover_completed_request_id = 0;
    camera_control->recording_rollover_completed_frame_id = 0;
    camera_control->recording_rollover_completed_folder.clear();
    camera_control->preserve_recording_session_state = false;
    camera_control->latest_recording_frame_id.store(0, std::memory_order_relaxed);
}

void request_recording_drain(std::vector<std::unique_ptr<ModernRecordingPipeline>>& recording_pipelines,
                             CameraControl* camera_control)
{
    if (!camera_control) {
        return;
    }
    camera_control->record_video = false;
    camera_control->recording_draining = true;
    camera_control->stop_record = true;
    {
        std::lock_guard<std::mutex> lock(camera_control->recording_folder_mutex);
        camera_control->pending_recording_output_folder.clear();
        camera_control->recording_rollover_at_frame_id = 0;
        camera_control->recording_rollover_request_id = 0;
    }

    for (auto& pipeline : recording_pipelines) {
        if (pipeline) {
            pipeline->request_recording_drain();
        }
    }
}

bool wait_for_recording_drain(CameraControl* camera_control,
                              std::chrono::steady_clock::duration timeout,
                              const char* timeout_label)
{
    if (!camera_control) {
        return true;
    }
    const auto drain_deadline = std::chrono::steady_clock::now() + timeout;
    while (camera_control->active_recorders.load(std::memory_order_relaxed) > 0 &&
           std::chrono::steady_clock::now() < drain_deadline) {
        usleep(1000);
    }
    const bool drained =
        camera_control->active_recorders.load(std::memory_order_relaxed) == 0;
    if (!drained && timeout_label) {
        std::cerr << timeout_label << " timed out with "
                  << camera_control->active_recorders.load(std::memory_order_relaxed)
                  << " active recorder(s)." << std::endl;
    }
    return drained;
}

void stop_headless_frame_ipc_managers(std::vector<std::unique_ptr<FrameIPCManager>>& frame_ipc_managers)
{
    for (auto& manager : frame_ipc_managers) {
        if (manager) {
            manager->stop();
        }
    }
}

void clear_headless_frame_ipc_managers(std::vector<std::unique_ptr<FrameIPCManager>>& frame_ipc_managers)
{
    stop_headless_frame_ipc_managers(frame_ipc_managers);
    frame_ipc_managers.clear();
}

void stop_headless_frame_ipc_runtime(HeadlessFrameIpcRuntime* runtime)
{
    if (!runtime) {
        return;
    }
    runtime->stop_requested.store(true, std::memory_order_release);
    for (auto& thread : runtime->reader_threads) {
        if (thread.joinable()) {
            thread.join();
        }
    }
    runtime->reader_threads.clear();
}

bool start_headless_frame_ipc_runtime(HeadlessFrameIpcRuntime* runtime,
                                      const HeadlessFrameIpcConfig& config,
                                      const CameraParams* cameras_params,
                                      const std::vector<int>& selected_indices,
                                      std::string* error_out)
{
    if (!runtime) {
        return true;
    }
    runtime->reset(config);
    if (!headless_frame_ipc_mode_is_verify_drain(config.mode)) {
        return true;
    }

    const bool use_v2 = headless_frame_ipc_mode_uses_v2(config.mode);
    runtime->reader_stats.resize(selected_indices.size());
    for (std::size_t stats_index = 0; stats_index < selected_indices.size(); ++stats_index) {
        const int camera_index = selected_indices[stats_index];
        HeadlessFrameIpcReaderStats& stats = runtime->reader_stats[stats_index];
        stats.camera_serial = cameras_params[camera_index].camera_serial;
        stats.camera_id = static_cast<uint32_t>(cameras_params[camera_index].camera_id);
        stats.v2 = use_v2;
        stats.queue_name = use_v2
            ? build_frame_ipc_v2_queue_name_for_serial(stats.camera_serial)
            : build_frame_ipc_queue_name_for_serial(stats.camera_serial);
        HeadlessFrameIpcReaderStats* stats_ptr = &stats;

        if (use_v2) {
            runtime->reader_threads.emplace_back([runtime, stats_ptr]() {
                HeadlessFrameIpcReaderStats& stats = *stats_ptr;
                try {
                    shaman_v2::SharedLiveStateQueue reader(stats.queue_name, false /* writer */);
                    stats.reader_started = true;
                    while (true) {
                        bool popped_any = false;
                        shaman_v2::Slot slot;
                        while (reader.pop(slot)) {
                            popped_any = true;
                            stats.messages_popped++;
                            stats.v2_latest_state_messages++;
                            const uint64_t frame_id = slot.state_frame_id;
                            if (slot.camera_id != stats.camera_id) {
                                stats.camera_id_mismatches++;
                            }

                            const auto detection_status =
                                static_cast<shaman_v2::DetectionStatus>(slot.detection_status);
                            if (detection_status == shaman_v2::DetectionStatus::kPending ||
                                detection_status == shaman_v2::DetectionStatus::kNotScheduled) {
                                stats.base_messages++;
                            }
                            if (detection_status == shaman_v2::DetectionStatus::kPending) {
                                stats.v2_detection_pending_messages++;
                            }
                            if (detection_status == shaman_v2::DetectionStatus::kDetections ||
                                detection_status == shaman_v2::DetectionStatus::kZeroDetections ||
                                detection_status == shaman_v2::DetectionStatus::kFailed) {
                                stats.detection_update_messages++;
                                stats.v2_detection_result_messages++;
                                stats.yolo_enabled_messages++;
                            }

                            const auto pose_status =
                                static_cast<shaman_v2::PoseStatus>(slot.pose_status);
                            if (pose_status == shaman_v2::PoseStatus::kPoses ||
                                pose_status == shaman_v2::PoseStatus::kNoResult ||
                                pose_status == shaman_v2::PoseStatus::kFailed) {
                                stats.v2_pose_result_messages++;
                            }

                            if (stats.messages_popped == 1) {
                                stats.first_frame_id = frame_id;
                                stats.first_sequence_id = slot.sequence_id;
                            } else {
                                if (frame_id == stats.last_frame_id) {
                                    // Base, detection, and pose state updates may share a frame id.
                                } else if (frame_id == stats.last_frame_id + 1) {
                                    // Expected next source frame.
                                } else if (frame_id > stats.last_frame_id + 1) {
                                    stats.frame_id_gaps += frame_id - stats.last_frame_id - 1;
                                } else if (frame_id < stats.last_frame_id) {
                                    stats.non_monotonic_frame_ids++;
                                }

                                if (slot.sequence_id == stats.last_sequence_id + 1) {
                                    // Expected next queue slot.
                                } else if (slot.sequence_id > stats.last_sequence_id + 1) {
                                    stats.sequence_id_gaps +=
                                        slot.sequence_id - stats.last_sequence_id - 1;
                                } else if (slot.sequence_id <= stats.last_sequence_id) {
                                    stats.non_monotonic_sequence_ids++;
                                }
                            }
                            stats.last_frame_id = frame_id;
                            stats.last_sequence_id = slot.sequence_id;
                        }

                        if (runtime->stop_requested.load(std::memory_order_acquire)) {
                            if (!popped_any) {
                                break;
                            }
                            continue;
                        }
                        usleep(1000);
                    }
                } catch (const std::exception& ex) {
                    stats.reader_error = ex.what();
                } catch (...) {
                    stats.reader_error = "unknown Shaman v2 frame IPC reader exception";
                }
            });
            continue;
        }

        runtime->reader_threads.emplace_back([runtime, stats_ptr]() {
            HeadlessFrameIpcReaderStats& stats = *stats_ptr;
            try {
                shaman::SharedBoxQueue reader(stats.queue_name.c_str(), false /* is_writer */);
                stats.reader_started = true;
                while (true) {
                    bool popped_any = false;
                    std::vector<shaman::Object> objects;
                    uint64_t timestamp_epoch = 0;
                    uint64_t timestamp_monotonic = 0;
                    uint64_t frame_id = 0;
                    uint32_t camera_id = 0;
                    bool yolo_enabled = false;
                    while (reader.pop(objects,
                                      timestamp_epoch,
                                      timestamp_monotonic,
                                      frame_id,
                                      camera_id,
                                      yolo_enabled)) {
                        (void)timestamp_epoch;
                        (void)timestamp_monotonic;
                        popped_any = true;
                        stats.messages_popped++;
                        if (objects.empty()) {
                            stats.base_messages++;
                        } else {
                            stats.detection_update_messages++;
                        }
                        if (yolo_enabled) {
                            stats.yolo_enabled_messages++;
                        }
                        if (camera_id != stats.camera_id) {
                            stats.camera_id_mismatches++;
                        }
                        if (stats.messages_popped == 1) {
                            stats.first_frame_id = frame_id;
                        } else if (frame_id == stats.last_frame_id) {
                            // Base and update messages may legitimately share a frame id.
                        } else if (frame_id == stats.last_frame_id + 1) {
                            // Expected next base frame.
                        } else if (frame_id > stats.last_frame_id + 1) {
                            stats.frame_id_gaps += frame_id - stats.last_frame_id - 1;
                        } else if (frame_id < stats.last_frame_id) {
                            stats.non_monotonic_frame_ids++;
                        }
                        stats.last_frame_id = frame_id;
                    }

                    if (runtime->stop_requested.load(std::memory_order_acquire)) {
                        if (!popped_any) {
                            break;
                        }
                        continue;
                    }
                    usleep(1000);
                }
            } catch (const std::exception& ex) {
                stats.reader_error = ex.what();
            } catch (...) {
                stats.reader_error = "unknown frame IPC reader exception";
            }
        });
    }

    if (runtime->reader_threads.size() != selected_indices.size()) {
        if (error_out) {
            *error_out = "Failed to start all headless frame IPC verifier readers";
        }
        stop_headless_frame_ipc_runtime(runtime);
        return false;
    }
    return true;
}

} // namespace

void quit_process(bool error = false, const std::string &reason = "")
{
    enet_deinitialize();
    // Show console reason before exit
    if (error)
    {
        std::cout << reason << std::endl;
        std::exit(EXIT_FAILURE);
    }
}

extern bool quit_server;

bool open_cameras(CameraParams *cameras_params,
                  CameraEmergent *ecams,
                  CameraEachSelect *cameras_select,
                  GigEVisionDeviceInfo *device_info,
                  int num_cameras,
                  std::string config_folder,
                  const CameraGpuOverrideMap& camera_gpu_overrides = {},
                  const std::vector<int>* selected_indices = nullptr)
{
    std::vector<std::string> camera_config_files;
    if (!config_folder.empty()) {
        if (!std::filesystem::exists(config_folder)) {
            std::cerr << "Config folder does not exist: " << config_folder << std::endl;
            return false;
        }
        update_camera_configs(camera_config_files, config_folder);
    }

    std::vector<bool> should_open(static_cast<std::size_t>(num_cameras), true);
    if (selected_indices) {
        std::fill(should_open.begin(), should_open.end(), false);
        for (int idx : *selected_indices) {
            if (idx >= 0 && idx < num_cameras) {
                should_open[static_cast<std::size_t>(idx)] = true;
            }
        }
    }

    for (int i = 0; i < num_cameras; i++)
    {
        ecams[i].evt_frame = nullptr;
        ecams[i].evt_frame_count = 0;
        set_camera_params(&cameras_params[i], &device_info[i], camera_config_files, i, num_cameras);
        apply_camera_gpu_overrides(&cameras_params[i], 1, camera_gpu_overrides);
        if (!should_open[static_cast<std::size_t>(i)]) {
            continue;
        }
        open_camera_with_params(&ecams[i].camera,
                                &device_info[i],
                                &cameras_params[i],
                                "headless_open_cameras");
    }
    return true;
}


bool start_camera_thread(std::vector<std::thread> &camera_threads,
    std::vector<CameraResources>& camera_resources,
    std::vector<int>& active_camera_indices,
    std::vector<std::unique_ptr<ModernRecordingPipeline>>& recording_pipelines,
    std::vector<std::unique_ptr<YoloWorker>>& yolo_workers,
    std::vector<std::unique_ptr<CropProducerWorker>>& crop_producer_workers,
    std::vector<std::unique_ptr<PoseWorker>>& pose_workers,
    std::vector<std::unique_ptr<FrameIPCManager>>& frame_ipc_managers,
    HeadlessFrameIpcRuntime* frame_ipc_runtime,
    const HeadlessFrameIpcConfig& frame_ipc_config,
    HeadlessThreadFailureState* thread_failure_state,
    HeadlessGpuDmonMonitor* gpu_dmon_monitor,
    CameraParams *cameras_params, CameraEmergent *ecams, CameraControl *camera_control, CameraEachSelect *cameras_select,
    GigEVisionDeviceInfo *device_info, int num_cameras, PTPParams *ptp_params,
    const std::vector<int>& required_gpu_ids,
    std::string record_folder, std::string encoder_basic_setup,
    bool nvenc_direct_input,
    const std::string& recording_sink_mode,
    const PreEncoderReferenceCaptureConfig& pre_encoder_reference_capture,
    int record_start_delay_seconds = 0,
    bool enable_recording = true,
    const std::string& initial_recording_output_folder = "",
    const yolo_event_log::SyntheticYoloEventConfig& yolo_event_log_config =
        yolo_event_log::SyntheticYoloEventConfig{},
    const HeadlessYoloWorkerConfig& yolo_worker_config = HeadlessYoloWorkerConfig{},
    const HeadlessPoseWorkerConfig& pose_worker_config = HeadlessPoseWorkerConfig{})
{
    std::cout << "start camera sthread..." << std::endl;
    if (thread_failure_state) {
        thread_failure_state->reset();
    }
    const HeadlessEncoderSettings encoder_settings = parse_headless_encoder_setup(encoder_basic_setup);
    const bool enable_artifacts = !record_folder.empty();
    std::cout << "Headless encoder config: codec=" << encoder_settings.codec
              << " preset=" << encoder_settings.preset
              << " tuning=" << encoder_settings.tuning
              << " rc=" << encoder_settings.rate_control_mode
              << " importance_map_mode=" << encoder_settings.importance_map.mode
              << " importance_map_roi_size_px=" << encoder_settings.importance_map.roi_size_px
              << " quality=" << encoder_settings.quality_value
              << " gop=" << encoder_settings.gop_length
              << " recording_sink_mode=" << recording_sink_mode
              << " aq=" << format_headless_toggle_override(encoder_settings.control_overrides.aq)
              << " temporal_aq="
              << format_headless_toggle_override(encoder_settings.control_overrides.temporal_aq)
              << " lookahead="
              << format_headless_toggle_override(encoder_settings.control_overrides.lookahead)
              << " lookahead_depth=" << encoder_settings.control_overrides.lookahead_depth
              << " target_bitrate_bps=" << encoder_settings.control_overrides.target_bitrate_bps
              << " max_bitrate_bps=" << encoder_settings.control_overrides.max_bitrate_bps
              << " vbv_buffer_size=" << encoder_settings.control_overrides.vbv_buffer_size
              << " cameras=" << format_selected_camera_serials(encoder_settings)
              << std::endl;

    if (pose_worker_config.enabled() && !yolo_worker_config.enabled()) {
        std::cerr << "Headless pose_worker requires fixed.yolo_worker.mode=real "
                  << "because the current pose crop producer is driven by "
                  << "the YOLO worker result path."
                  << std::endl;
        return false;
    }

    std::vector<int> selected_indices;
    size_t max_frame_size_bytes = 0;
    try {
        selected_indices = resolve_selected_camera_indices(
            cameras_params,
            num_cameras,
            encoder_settings,
            required_gpu_ids);
        if (selected_indices.empty()) {
            throw std::runtime_error("No cameras selected for headless run.");
        }
        for (int idx : selected_indices) {
            const size_t current_size =
                static_cast<size_t>(cameras_params[idx].width) * static_cast<size_t>(cameras_params[idx].height);
            if (current_size > max_frame_size_bytes) {
                max_frame_size_bytes = current_size;
            }
        }

        std::vector<RecordingValidationCameraInput> validation_inputs;
        validation_inputs.reserve(selected_indices.size());
        for (int idx : selected_indices) {
            RecordingValidationCameraInput input;
            input.camera_index = idx;
            input.camera_serial = cameras_params[idx].camera_serial;
            input.record_enabled = enable_recording;
            input.source_gpu_id = cameras_params[idx].gpu_id;
            input.strategy = cameras_params[idx].recording.strategy;
            input.constraints = cameras_params[idx].recording.constraints;
            validation_inputs.push_back(std::move(input));
        }
        const RecordingPreflightResult preflight = run_recording_preflight(
            validation_inputs,
            [](const int source_gpu_id, const int helper_gpu_id) {
                return build_recording_validation_gpu_path_info(source_gpu_id, helper_gpu_id);
            });
        if (!preflight.ok) {
            std::cerr << "Headless recording preflight failed." << std::endl;
            for (const std::string& error : preflight.errors) {
                std::cerr << "  - " << error << std::endl;
            }
            return false;
        }

        frame_ipc_managers.clear();
        frame_ipc_managers.resize(num_cameras);
        if (frame_ipc_config.enabled()) {
            const bool force_v2_live_state =
                headless_frame_ipc_mode_uses_v2(frame_ipc_config.mode);
            for (int idx : selected_indices) {
                const std::string queue_name =
                    build_frame_ipc_queue_name_for_serial(cameras_params[idx].camera_serial);
                const std::string v2_queue_name =
                    build_frame_ipc_v2_queue_name_for_serial(cameras_params[idx].camera_serial);
                if (frame_ipc_config.unlink_existing_queues) {
                    shaman::unlinkQueue(queue_name.c_str());
                    if (force_v2_live_state) {
                        shaman_v2::unlink_queue(v2_queue_name);
                    }
                }
                frame_ipc_managers[idx] =
                    std::make_unique<FrameIPCManager>(&cameras_params[idx],
                                                      force_v2_live_state);
                if (!frame_ipc_managers[idx]->isEnabled()) {
                    std::cerr << "Headless frame IPC initialization failed for camera "
                              << cameras_params[idx].camera_serial
                              << " queue=" << queue_name
                              << ": " << frame_ipc_managers[idx]->getInitError()
                              << std::endl;
                    clear_headless_frame_ipc_managers(frame_ipc_managers);
                    cleanup_selected_camera_buffers(selected_indices, ecams, cameras_params, camera_resources);
                    return false;
                }
                if (force_v2_live_state &&
                    !frame_ipc_managers[idx]->isV2Enabled()) {
                    std::cerr << "Headless Shaman v2 frame IPC initialization failed for camera "
                              << cameras_params[idx].camera_serial
                              << " queue=" << v2_queue_name
                              << ": " << frame_ipc_managers[idx]->getV2InitError()
                              << std::endl;
                    clear_headless_frame_ipc_managers(frame_ipc_managers);
                    cleanup_selected_camera_buffers(selected_indices, ecams, cameras_params, camera_resources);
                    return false;
                }
                if (frame_ipc_managers[idx]->getQueueName() != queue_name) {
                    std::cerr << "Headless frame IPC queue-name mismatch for camera "
                              << cameras_params[idx].camera_serial
                              << " expected=" << queue_name
                              << " actual=" << frame_ipc_managers[idx]->getQueueName()
                              << std::endl;
                    clear_headless_frame_ipc_managers(frame_ipc_managers);
                    cleanup_selected_camera_buffers(selected_indices, ecams, cameras_params, camera_resources);
                    return false;
                }
                if (force_v2_live_state &&
                    frame_ipc_managers[idx]->getV2QueueName() != v2_queue_name) {
                    std::cerr << "Headless Shaman v2 frame IPC queue-name mismatch for camera "
                              << cameras_params[idx].camera_serial
                              << " expected=" << v2_queue_name
                              << " actual=" << frame_ipc_managers[idx]->getV2QueueName()
                              << std::endl;
                    clear_headless_frame_ipc_managers(frame_ipc_managers);
                    cleanup_selected_camera_buffers(selected_indices, ecams, cameras_params, camera_resources);
                    return false;
                }
            }

            std::string frame_ipc_error;
            if (!start_headless_frame_ipc_runtime(
                    frame_ipc_runtime,
                    frame_ipc_config,
                    cameras_params,
                    selected_indices,
                    &frame_ipc_error)) {
                std::cerr << frame_ipc_error << std::endl;
                clear_headless_frame_ipc_managers(frame_ipc_managers);
                cleanup_selected_camera_buffers(selected_indices, ecams, cameras_params, camera_resources);
                return false;
            }

            std::cout << "Headless frame IPC enabled."
                      << " mode=" << headless_frame_ipc_mode_to_string(frame_ipc_config.mode)
                      << " queue_naming=serial"
                      << std::endl;
        }

        camera_resources.clear();
        camera_resources.resize(num_cameras);
        yolo_workers.clear();
        yolo_workers.resize(num_cameras);
        crop_producer_workers.clear();
        crop_producer_workers.resize(num_cameras);
        pose_workers.clear();
        pose_workers.resize(num_cameras);
        if (yolo_worker_config.enabled()) {
            YOLOv8::initialize_plugins();
        }
        for (int idx : selected_indices) {
            const bool enable_real_yolo = yolo_worker_config.enabled();
            const bool enable_pose = pose_worker_config.enabled();
            cameras_select[idx].stream_on = false;
            cameras_select[idx].record = enable_recording;
            cameras_select[idx].yolo = enable_real_yolo;
            cameras_select[idx].yolo_model =
                enable_real_yolo ? yolo_worker_config.engine_path.c_str() : nullptr;
            cameras_select[idx].crop_and_encode = false;
            cameras_select[idx].pose = enable_pose;
            cameras_select[idx].send_frame_ipc = frame_ipc_config.enabled();
            cameras_select[idx].send_yolo_via_frame_ipc =
                frame_ipc_config.enabled() && yolo_worker_config.publish_live_ipc;
            camera_resources[idx].initialize(
                cameras_params[idx].gpu_id,
                max_frame_size_bytes,
                enable_real_yolo,
                cameras_params[idx].recording.resources.acquire_work_entries);
        }

    } catch (const std::exception& ex) {
        std::cerr << "Failed to start thread: " << ex.what() << std::endl;
        stop_headless_yolo_workers(yolo_workers);
        stop_headless_pose_pipeline(crop_producer_workers, pose_workers);
        stop_headless_frame_ipc_runtime(frame_ipc_runtime);
        clear_headless_frame_ipc_managers(frame_ipc_managers);
        cleanup_selected_camera_buffers(selected_indices, ecams, cameras_params, camera_resources);
        camera_resources.clear();
        return false;
    }

    camera_control->record_video = enable_recording && (record_start_delay_seconds <= 0);
    camera_control->subscribe = true;
    int ptp_camera_count = 0;
    for (int idx : selected_indices) {
        if (camera_sync_mode_uses_ptp(&cameras_params[idx])) {
            ptp_camera_count++;
        }
    }
    if (ptp_camera_count != 0 && ptp_camera_count != static_cast<int>(selected_indices.size())) {
        std::cerr << "Headless mode does not support mixed ptp_gate and non-PTP camera sync modes in one run." << std::endl;
        cleanup_selected_camera_buffers(selected_indices, ecams, cameras_params, camera_resources);
        return false;
    }
    const bool use_ptp_sync = (ptp_camera_count == static_cast<int>(selected_indices.size()) &&
                               !selected_indices.empty());
    camera_control->sync_camera = use_ptp_sync;
    camera_control->recording_draining = false;
    camera_control->stop_record = false;
    camera_control->latest_recording_frame_id.store(0, std::memory_order_relaxed);
    {
        std::lock_guard<std::mutex> lock(camera_control->recording_folder_mutex);
        camera_control->pending_recording_output_folder.clear();
        camera_control->recording_rollover_at_frame_id = 0;
        camera_control->recording_rollover_request_id = 0;
        camera_control->recording_rollover_completed_request_id = 0;
        camera_control->recording_rollover_completed_frame_id = 0;
        camera_control->recording_rollover_completed_folder.clear();
    }

    std::vector<CameraParams> selected_camera_params;
    selected_camera_params.reserve(selected_indices.size());
    for (int idx : selected_indices) {
        selected_camera_params.push_back(cameras_params[idx]);
    }

    recording_pipelines.clear();
    recording_pipelines.resize(num_cameras);

    std::vector<std::shared_ptr<yolo_event_log::SyntheticYoloEventEmitter>>
        synthetic_yolo_emitters(num_cameras);
    if (yolo_event_log_config.enabled()) {
        for (int idx : selected_indices) {
            synthetic_yolo_emitters[idx] =
                std::make_shared<yolo_event_log::SyntheticYoloEventEmitter>(
                    cameras_params[idx].camera_serial,
                    cameras_params[idx].camera_id,
                    cameras_params[idx].gpu_id,
                    build_frame_ipc_queue_name_for_serial(cameras_params[idx].camera_serial),
                    frame_ipc_config.enabled(),
                    yolo_event_log_config);
        }
        std::cout << "Headless synthetic YOLO event log enabled."
                  << " source=acquisition_metadata"
                  << " audit_only=true"
                  << " real_yolo_worker=false"
                  << " live_detection_ipc=false"
                  << " every_n_frames=" << yolo_event_log_config.every_n_frames
                  << " emit_zero_detections="
                  << (yolo_event_log_config.emit_zero_detections ? "true" : "false")
                  << std::endl;
    }

    try {
        if (enable_artifacts) {
            const bool update_latest_pointer =
                enable_recording && is_real_recording_sink_mode(recording_sink_mode);
            if (!prepare_headless_recording_artifacts(
                    record_folder,
                    camera_control,
                    selected_camera_params.data(),
                    static_cast<int>(selected_camera_params.size()),
                    ptp_params,
                    update_latest_pointer,
                    recording_sink_mode)) {
                cleanup_selected_camera_buffers(selected_indices, ecams, cameras_params, camera_resources);
                return false;
            }
            if (!initial_recording_output_folder.empty()) {
                orange::ScopedFsuid fsuid_guard;
                (void)fsuid_guard;
                make_folder(initial_recording_output_folder);
                std::lock_guard<std::mutex> lock(camera_control->recording_folder_mutex);
                camera_control->recording_output_folder = initial_recording_output_folder;
            }

            start_headless_gpu_dmon_monitor(
                gpu_dmon_monitor,
                record_folder,
                collect_unique_gpu_ids(cameras_params, selected_indices));
        }

        if (enable_recording) {
            for (int idx : selected_indices) {
                ResolvedRecordingConfigOverrides recording_overrides;
                recording_overrides.recording_gpu_id = cameras_params[idx].gpu_id;
                recording_overrides.has_output_preferences_override = true;
                recording_overrides.output_preferences =
                    build_native_recording_output_preferences(cameras_params[idx]);
                recording_overrides.codec = encoder_settings.codec;
                recording_overrides.preset = encoder_settings.preset;
                recording_overrides.tuning = encoder_settings.tuning;
                recording_overrides.rate_control_mode = encoder_settings.rate_control_mode;
                recording_overrides.quality_value = encoder_settings.quality_value;
                recording_overrides.gop_length = encoder_settings.gop_length;
                recording_overrides.has_nvenc_direct_input_override = true;
                recording_overrides.nvenc_direct_input = nvenc_direct_input;
                recording_overrides.encoder_control_overrides = encoder_settings.control_overrides;
                recording_overrides.importance_map = encoder_settings.importance_map;
                recording_overrides.base_folder_name = record_folder;
                recording_overrides.pre_encoder_reference_capture = pre_encoder_reference_capture;
                const ResolvedRecordingConfig resolved_recording_config =
                    build_resolved_recording_config(
                        cameras_params[idx],
                        recording_overrides);
                recording_pipelines[idx] = std::make_unique<ModernRecordingPipeline>(
                    &cameras_params[idx],
                    resolved_recording_config,
                    *camera_resources[idx].recycle_queue,
                    camera_control,
                    recording_sink_mode);
                recording_pipelines[idx]->start();
            }
        }

        if (pose_worker_config.enabled()) {
            std::cout << "Headless pose worker enabled."
                      << " mode=" << pose_worker_config.mode
                      << " runtime=CropProducerWorker->PoseWorker"
                      << " roi_source=" << pose_worker_config.roi_source
                      << " queue_depth=" << pose_worker_config.queue_depth
                      << " write_events_jsonl="
                      << (pose_worker_config.write_events_jsonl ? "true" : "false")
                      << std::endl;
            if (pose_worker_config.synthetic_runtime_detection_enabled()) {
                std::cout << "Headless pose synthetic_center_box is enabled for "
                          << "pose plumbing validation only; it is not a "
                          << "production detection workflow validation."
                          << " every_n_frames="
                          << pose_worker_config.synthetic_detection_every_n_frames
                          << " box="
                          << pose_worker_config.synthetic_detection_box_width_px
                          << "x"
                          << pose_worker_config.synthetic_detection_box_height_px
                          << std::endl;
            }
            for (int idx : selected_indices) {
                const int crop_size_px =
                    CropProducerWorker::SanitizeCropSize(
                        cameras_params[idx].crop_pipeline.crop_size_px);
                std::string crop_name = "HeadlessCropProducer_Cam_" +
                    cameras_params[idx].camera_serial;
                crop_producer_workers[idx] = std::make_unique<CropProducerWorker>(
                    crop_name.c_str(),
                    &cameras_params[idx],
                    *camera_resources[idx].recycle_queue,
                    camera_control,
                    crop_size_px);
                crop_producer_workers[idx]->RotateRecordingFolder(record_folder);

                std::string pose_name = "HeadlessPoseWorker_Cam_" +
                    cameras_params[idx].camera_serial;
                pose_workers[idx] = std::make_unique<PoseWorker>(
                    pose_name.c_str(),
                    &cameras_params[idx],
                    crop_producer_workers[idx]->GetCropProducer());
                pose_workers[idx]->SetMaxQueueSize(pose_worker_config.queue_depth);
                pose_workers[idx]->RotateRecordingFolder(record_folder);
                crop_producer_workers[idx]->SetPoseWorker(pose_workers[idx].get());
            }
        }

        if (yolo_worker_config.enabled()) {
            std::cout << "Headless real YOLO enabled."
                      << " source=TensorRT"
                      << " audit_only="
                      << (yolo_worker_config.publish_live_ipc ? "false" : "true")
                      << " live_detection_ipc="
                      << (yolo_worker_config.publish_live_ipc ? "true" : "false")
                      << " decimate=1/" << yolo_worker_config.decimate
                      << " prewarm_iterations=" << yolo_worker_config.prewarm_iterations
                      << " engine_path=" << yolo_worker_config.engine_path
                      << std::endl;
            for (int idx : selected_indices) {
                std::string name = "HeadlessYoloWorker_Cam_" +
                    cameras_params[idx].camera_serial;
                try {
                    yolo_workers[idx] = std::make_unique<YoloWorker>(
                        name.c_str(),
                        &cameras_params[idx],
                        &cameras_select[idx],
                        camera_control,
                        *camera_resources[idx].recycle_queue);
                    if (crop_producer_workers[idx]) {
                        yolo_workers[idx]->SetCropProducerWorker(
                            crop_producer_workers[idx].get());
                    }
                    yolo_workers[idx]->SetMaxQueueSize(240);
                    yolo_workers[idx]->Warmup(yolo_worker_config.prewarm_iterations);
                    yolo_workers[idx]->StartThread();
                } catch (const std::exception& ex) {
                    if (yolo_worker_config.fail_on_init_error ||
                        pose_worker_config.enabled()) {
                        throw;
                    }
                    std::cerr << "Headless real YOLO disabled for camera "
                              << cameras_params[idx].camera_serial
                              << " after init failure: " << ex.what()
                              << std::endl;
                    cameras_select[idx].yolo = false;
                    cameras_select[idx].yolo_model = nullptr;
                }
            }
        }

        for (int idx : selected_indices) {
            if (pose_workers[idx]) {
                pose_workers[idx]->StartThread();
            }
            if (crop_producer_workers[idx]) {
                crop_producer_workers[idx]->SetMaxQueueSize(240);
                crop_producer_workers[idx]->StartThread();
            }
        }

        // Match the GUI path: build the recording pipeline first, then open the
        // camera stream and allocate EVT frame buffers against the active GPU.
        allocate_selected_camera_frame_buffers(ecams, cameras_params, selected_indices);
    } catch (const std::exception& ex) {
        std::cerr << "Failed to initialize headless recording pipelines: " << ex.what() << std::endl;
        stop_headless_yolo_workers(yolo_workers);
        stop_headless_pose_pipeline(crop_producer_workers, pose_workers);
        stop_headless_frame_ipc_runtime(frame_ipc_runtime);
        clear_headless_frame_ipc_managers(frame_ipc_managers);
        if (enable_artifacts) {
            drain_and_shutdown_recording(recording_pipelines, camera_control);
            stop_headless_gpu_dmon_monitor(gpu_dmon_monitor);
        }
        cleanup_selected_camera_buffers(selected_indices, ecams, cameras_params, camera_resources);
        camera_resources.clear();
        return false;
    }

    if (use_ptp_sync) {
        for (int idx : selected_indices)
        {
            ptp_camera_sync(&ecams[idx].camera, &cameras_params[idx]);
        }
        const CameraParams* representative_camera =
            selected_indices.empty() ? nullptr : &cameras_params[selected_indices.front()];
        std::cout << "Headless sync mode: ptp_gate"
                  << " (PTP " << resolved_headless_ptp_mode_label(representative_camera) << ")"
                  << std::endl;
    } else {
        const CameraParams* representative_camera =
            selected_indices.empty() ? nullptr : &cameras_params[selected_indices.front()];
        std::cout << "Headless sync mode: "
                  << normalize_headless_sync_mode_label(representative_camera)
                  << std::endl;
    }

    for (int idx : selected_indices)
    {
        auto synthetic_yolo_emitter = synthetic_yolo_emitters[idx];
        CameraEmergent* ecams_ptr = ecams;
        CameraParams* cameras_params_ptr = cameras_params;
        CameraEachSelect* cameras_select_ptr = cameras_select;
        CameraControl* camera_control_ptr = camera_control;
        PTPParams* ptp_params_ptr = ptp_params;
        auto* recording_pipelines_ptr = &recording_pipelines;
        auto* yolo_workers_ptr = &yolo_workers;
        auto* frame_ipc_managers_ptr = &frame_ipc_managers;
        auto* camera_resources_ptr = &camera_resources;
        camera_threads.push_back(std::thread(
            [idx,
             thread_failure_state,
             synthetic_yolo_emitter,
             ecams_ptr,
             cameras_params_ptr,
             cameras_select_ptr,
             camera_control_ptr,
             ptp_params_ptr,
             recording_pipelines_ptr,
             yolo_workers_ptr,
             frame_ipc_managers_ptr,
             camera_resources_ptr]() {
                try {
                    acquire_frames(
                        &ecams_ptr[idx],
                        &cameras_params_ptr[idx],
                        &cameras_select_ptr[idx],
                        camera_control_ptr,
                        ptp_params_ptr,
                        nullptr,
                        nullptr,
                        (*recording_pipelines_ptr)[idx]
                            ? (*recording_pipelines_ptr)[idx]->recording_ingress()
                            : nullptr,
                        (*yolo_workers_ptr)[idx].get(),
                        nullptr,
                        &(*camera_resources_ptr)[idx],
                        (*frame_ipc_managers_ptr)[idx].get(),
                        synthetic_yolo_emitter.get());
                } catch (const std::exception& ex) {
                    std::ostringstream message;
                    message << "Headless camera thread failed for camera "
                            << cameras_params_ptr[idx].camera_serial
                            << ": " << ex.what();
                    std::cerr << message.str() << std::endl;
                    if (thread_failure_state) {
                        thread_failure_state->record_failure(message.str());
                    }
                    if (camera_control_ptr) {
                        camera_control_ptr->subscribe = false;
                        camera_control_ptr->record_video = false;
                        camera_control_ptr->recording_draining = true;
                        camera_control_ptr->stop_record = true;
                    }
                    quit_server = true;
                } catch (...) {
                    const std::string message =
                        "Headless camera thread failed with an unknown exception for camera " +
                        cameras_params_ptr[idx].camera_serial;
                    std::cerr << message << std::endl;
                    if (thread_failure_state) {
                        thread_failure_state->record_failure(message);
                    }
                    if (camera_control_ptr) {
                        camera_control_ptr->subscribe = false;
                        camera_control_ptr->record_video = false;
                        camera_control_ptr->recording_draining = true;
                        camera_control_ptr->stop_record = true;
                    }
                    quit_server = true;
                }
            }));
    }

    // wait for all camera ready
    if (use_ptp_sync) {
        while(ptp_params->ptp_counter != static_cast<int>(selected_indices.size())) {
            usleep(10);
        }
    }

    active_camera_indices = std::move(selected_indices);
    return true;
}

bool quit_server = false;


static void interruptHandler(const int signal)
{
    (void)signal;
    printf("\nQuit Orange.\n");
    quit_server = true;
}

struct ManagerContext
{
    FetchGame::ManagerState state;
    bool quit;
};

struct RecordingContext {
    std::string record_folder;
    std::string encoder_basic_setup;
};

void create_camera_manager(int* cam_count, ManagerContext* manager_context, GigEVisionDeviceInfo* unsorted_device_info, GigEVisionDeviceInfo* device_info, std::string* config_folder, RecordingContext* recording_setup, PTPParams *ptp_params) 
{
    CameraEmergent *ecams = nullptr;
    CameraParams *cameras_params = nullptr;
    std::vector<std::thread> camera_threads;
    std::vector<CameraResources> camera_resources;
    std::vector<int> active_camera_indices;
    std::vector<std::unique_ptr<ModernRecordingPipeline>> recording_pipelines;
    std::vector<std::unique_ptr<YoloWorker>> yolo_workers;
    std::vector<std::unique_ptr<CropProducerWorker>> crop_producer_workers;
    std::vector<std::unique_ptr<PoseWorker>> pose_workers;
    std::vector<std::unique_ptr<FrameIPCManager>> frame_ipc_managers;
    HeadlessFrameIpcRuntime frame_ipc_runtime;
    HeadlessThreadFailureState thread_failure_state;
    HeadlessGpuDmonMonitor gpu_dmon_monitor;
    CameraEachSelect *cameras_select = nullptr;
    CameraControl *camera_control = new CameraControl;

    manager_context->state = FetchGame::ManagerState_IDLE;
    while(!manager_context->quit) {
        switch (manager_context->state) {
            case FetchGame::ManagerState_CONNECT:
                *cam_count = scan_cameras(max_cameras, unsorted_device_info);
                std::cout << *cam_count << std::endl;
                sort_cameras_ip(unsorted_device_info, device_info, *cam_count);
                manager_context->state = FetchGame::ManagerState_CONNECTED;
                break;
            case FetchGame::ManagerState_OPENCAMERA:
                ecams = new CameraEmergent[*cam_count];
                cameras_params = new CameraParams[*cam_count];
                cameras_select = new CameraEachSelect[*cam_count];
                if (open_cameras(cameras_params, ecams, cameras_select, device_info, *cam_count, *config_folder)) 
                {
                    manager_context->state = FetchGame::ManagerState_CAMERAOPENED;
                } else {
                    manager_context->state = FetchGame::ManagerState_ERROR;
                }
                break;
            case FetchGame::ManagerState_STARTCAMTHREAD:
                if (start_camera_thread(
                        camera_threads,
                        camera_resources,
                        active_camera_indices,
                        recording_pipelines,
                        yolo_workers,
                        crop_producer_workers,
                        pose_workers,
                        frame_ipc_managers,
                        &frame_ipc_runtime,
                        HeadlessFrameIpcConfig{},
                        &thread_failure_state,
                        &gpu_dmon_monitor,
                        cameras_params,
                        ecams,
                        camera_control,
                        cameras_select,
                        device_info,
                        *cam_count,
                        ptp_params,
                        {},
                        recording_setup->record_folder,
                        recording_setup->encoder_basic_setup,
                        false,
                        "real",
                        PreEncoderReferenceCaptureConfig{},
                        0))
                {
                    manager_context->state = FetchGame::ManagerState_THREADREADY;
                } else {
                    manager_context->state = FetchGame::ManagerState_ERROR;
                }
                break;
            case FetchGame::ManagerState_ERROR:
                if (ecams && cameras_params) {
                    shutdown_headless_run(
                        camera_threads,
                        camera_resources,
                        active_camera_indices,
                        recording_pipelines,
                        yolo_workers,
                        crop_producer_workers,
                        pose_workers,
                        &gpu_dmon_monitor,
                        ecams,
                        cameras_params,
                        *cam_count,
                        nullptr,
                        camera_control,
                        ptp_params,
                        true);
                    stop_headless_frame_ipc_managers(frame_ipc_managers);
                    stop_headless_frame_ipc_runtime(&frame_ipc_runtime);
                    clear_headless_frame_ipc_managers(frame_ipc_managers);
                    delete[] ecams;
                    ecams = nullptr;
                    delete[] cameras_params;
                    cameras_params = nullptr;
                    delete[] cameras_select;
                    cameras_select = nullptr;
                }
                quit_server = true;
                break;
        }

        if (thread_failure_state.has_failure()) {
            if (gpu_dmon_monitor.error.empty()) {
                gpu_dmon_monitor.error = thread_failure_state.get_first_error();
            }
            stop_headless_gpu_dmon_monitor(&gpu_dmon_monitor);
            manager_context->state = FetchGame::ManagerState_ERROR;
        }

        if (ptp_params->network_set_stop_ptp && ptp_params->ptp_stop_reached) {
            ptp_params->network_set_stop_ptp = false;
            shutdown_headless_run(
                camera_threads,
                camera_resources,
                active_camera_indices,
                recording_pipelines,
                yolo_workers,
                crop_producer_workers,
                pose_workers,
                &gpu_dmon_monitor,
                ecams,
                cameras_params,
                *cam_count,
                nullptr,
                camera_control,
                ptp_params,
                true);
            stop_headless_frame_ipc_managers(frame_ipc_managers);
            stop_headless_frame_ipc_runtime(&frame_ipc_runtime);
            clear_headless_frame_ipc_managers(frame_ipc_managers);
            delete[] ecams;
            ecams = nullptr;
            delete[] cameras_params;
            cameras_params = nullptr;
            delete[] cameras_select;
            cameras_select = nullptr;
            manager_context->state = FetchGame::ManagerState_RECORDSTOPPED;
        }
        usleep(1000);
    }

    delete camera_control;
}

bool validate_headless_cli_options(const HeadlessCliOptions& options, std::string* error_out)
{
    if (options.mode == HeadlessMode::Remote) {
        if (!options.record_folder.empty() || !options.config_folder.empty() ||
            options.list_cameras || options.stream_only || !options.experiment_spec_path.empty() ||
            options.duration_seconds > 0 || options.stream_start_delay_seconds > 0 ||
            options.record_start_delay_seconds > 0 ||
            options.nvenc_direct_input ||
            options.recording_sink_mode != "real" ||
            !options.ptp_gate_acquisition_mode_override.empty() ||
            !options.acquisition_buffer_mode_override.empty() ||
            !options.required_gpu_ids.empty() ||
            pre_encoder_reference_capture_requested(options.pre_encoder_reference_capture) ||
            headless_frame_ipc_requested(options.frame_ipc) ||
            headless_yolo_worker_requested(options.yolo_worker) ||
            headless_pose_worker_requested(options.pose_worker) ||
            !headless_encoder_settings_is_default(options.encoder_settings) ||
            !options.encoder_settings.select_all_cameras ||
            !options.encoder_settings.camera_serials.empty()) {
            if (error_out) {
                *error_out =
                    "Direct run flags are only supported with --mode local. "
                    "Remote mode still uses the network control path.";
            }
            return false;
        }
        return true;
    }

    if (!options.experiment_spec_path.empty()) {
        if (options.list_cameras || options.stream_only ||
            !options.record_folder.empty() ||
            options.duration_seconds > 0 ||
            options.stream_start_delay_seconds > 0 ||
            options.record_start_delay_seconds > 0 ||
            options.nvenc_direct_input ||
            options.recording_sink_mode != "real" ||
            !options.ptp_gate_acquisition_mode_override.empty() ||
            !options.acquisition_buffer_mode_override.empty() ||
            !options.required_gpu_ids.empty() ||
            pre_encoder_reference_capture_requested(options.pre_encoder_reference_capture) ||
            headless_frame_ipc_requested(options.frame_ipc) ||
            headless_yolo_worker_requested(options.yolo_worker) ||
            headless_pose_worker_requested(options.pose_worker) ||
            !headless_encoder_settings_is_default(options.encoder_settings)) {
            if (error_out) {
                *error_out =
                    "When --experiment-spec is provided, per-run flags like "
                    "--list-cameras, --stream-only, --record-folder, --camera, --codec, --preset, --tuning, "
                    "--rate-control, --quality, --gop, --preenc-ref-max-frames, "
                    "--preenc-ref-max-seconds, --duration, --stream-start-delay, --nvenc-direct-input, "
                    "--ptp-gate-acquisition-mode, --acquisition-buffer-mode, --recording-sink-mode, "
                    "--record-delay, --frame-ipc, --yolo-engine, pose worker config, and --gpu-id "
                    "must be omitted. Use the spec file instead.";
            }
            return false;
        }
        return true;
    }

    if (!validate_pre_encoder_reference_capture_config(
            options.pre_encoder_reference_capture,
            error_out,
            "Local headless CLI")) {
        return false;
    }
    if (!validate_headless_yolo_worker_config(
            options.yolo_worker,
            error_out,
            "Local headless CLI")) {
        return false;
    }
    if (!validate_headless_pose_worker_config(
            options.pose_worker,
            error_out,
            "Local headless CLI")) {
        return false;
    }

    if (options.yolo_worker.enabled() && options.stream_only) {
        if (error_out) {
            *error_out =
                "real headless YOLO currently requires recording artifacts and is not supported with --stream-only.";
        }
        return false;
    }
    if (options.yolo_worker.enabled() && options.yolo_event_log.enabled()) {
        if (error_out) {
            *error_out =
                "real headless YOLO writes its own event log; do not combine --yolo-engine with synthetic yolo_event_log.";
        }
        return false;
    }
    if (options.pose_worker.enabled()) {
        if (error_out) {
            *error_out =
                "headless pose_worker is currently experiment-spec only; no local CLI pose flags are exposed yet.";
        }
        return false;
    }

    if (options.stream_only && options.pre_encoder_reference_capture.enabled) {
        if (error_out) {
            *error_out =
                "pre_encoder_reference_capture requires recording output and is not supported with --stream-only.";
        }
        return false;
    }
    if (options.recording_sink_mode != "real" &&
        options.pre_encoder_reference_capture.enabled) {
        if (error_out) {
            *error_out =
                "pre_encoder_reference_capture requires the real recording pipeline and is not supported with --recording-sink-mode != real.";
        }
        return false;
    }

    if (options.stream_only && options.recording_sink_mode != "real") {
        if (error_out) {
            *error_out =
                "--recording-sink-mode is only supported when recording is enabled and cannot be used with --stream-only.";
        }
        return false;
    }

    if (!options.frame_ipc.enabled() &&
        (options.frame_ipc.unlink_existing_queues || options.frame_ipc.allow_push_failures)) {
        if (error_out) {
            *error_out =
                "--frame-ipc-unlink-existing and --frame-ipc-allow-push-failures require --frame-ipc != off.";
        }
        return false;
    }

    if (options.frame_ipc.enabled() && options.record_folder.empty()) {
        if (error_out) {
            *error_out = "--record-folder is required when --frame-ipc is enabled so frame_ipc_summary.json can be written.";
        }
        return false;
    }

    if (!options.list_cameras && !options.stream_only && options.record_folder.empty()) {
        if (error_out) {
            *error_out = "--record-folder is required in --mode local unless using --list-cameras or --stream-only.";
        }
        return false;
    }

    return true;
}

bool read_json_file(const std::filesystem::path& path, nlohmann::json* json_out, std::string* error_out)
{
    if (!json_out) {
        if (error_out) {
            *error_out = "Internal error: null JSON destination";
        }
        return false;
    }

    std::ifstream input(path);
    if (!input) {
        if (error_out) {
            *error_out = "Failed to open " + path.string();
        }
        return false;
    }

    try {
        input >> *json_out;
    } catch (const std::exception& ex) {
        if (error_out) {
            *error_out = "Failed to parse " + path.string() + ": " + ex.what();
        }
        return false;
    }
    return true;
}

bool write_json_file(const std::filesystem::path& path, const nlohmann::json& value, std::string* error_out)
{
    orange::ScopedFsuid fsuid_guard;
    (void)fsuid_guard;
    std::error_code create_error;
    std::filesystem::create_directories(path.parent_path(), create_error);
    if (create_error && !std::filesystem::exists(path.parent_path())) {
        if (error_out) {
            *error_out = "Failed to create parent directory for " + path.string() + ": " + create_error.message();
        }
        return false;
    }

    std::ofstream output(path);
    if (!output) {
        if (error_out) {
            *error_out = "Failed to open " + path.string() + " for writing";
        }
        return false;
    }
    output << value.dump(2) << "\n";
    return true;
}

bool write_headless_frame_ipc_summary(
    const std::string& record_folder,
    const HeadlessFrameIpcConfig& config,
    const CameraParams* cameras_params,
    const std::vector<int>& selected_indices,
    const std::vector<std::unique_ptr<FrameIPCManager>>& frame_ipc_managers,
    const HeadlessFrameIpcRuntime& runtime,
    std::string* error_out)
{
    if (!config.enabled() || record_folder.empty()) {
        return true;
    }

    std::unordered_map<std::string, const HeadlessFrameIpcReaderStats*> reader_stats_by_serial;
    for (const HeadlessFrameIpcReaderStats& stats : runtime.reader_stats) {
        reader_stats_by_serial[stats.camera_serial] = &stats;
    }

    bool overall_ok = true;
    const bool use_v2 = headless_frame_ipc_mode_uses_v2(config.mode);
    nlohmann::json summary = {
        {"schema_id", "orange.headless.frame_ipc_summary"},
        {"schema_version", 1},
        {"created_at_utc", get_current_utc_timestamp()},
        {"enabled", config.enabled()},
        {"mode", headless_frame_ipc_mode_to_string(config.mode)},
        {"queue_version", use_v2 ? 2 : 1},
        {"queue_naming", "serial"},
        {"unlink_existing_queues", config.unlink_existing_queues},
        {"require_base_frames", config.require_base_frames},
        {"allow_push_failures", config.allow_push_failures},
        {"cameras", nlohmann::json::object()}
    };

    for (int idx : selected_indices) {
        const CameraParams& camera_params = cameras_params[idx];
        const std::string camera_serial = camera_params.camera_serial;
        const std::string v1_queue_name = build_frame_ipc_queue_name_for_serial(camera_serial);
        const std::string v2_queue_name = build_frame_ipc_v2_queue_name_for_serial(camera_serial);
        const std::string queue_name = use_v2 ? v2_queue_name : v1_queue_name;
        nlohmann::json camera_json = {
            {"camera_serial", camera_serial},
            {"camera_id", camera_params.camera_id},
            {"queue_name", queue_name},
            {"v1_queue_name", v1_queue_name},
            {"v2_queue_name", v2_queue_name},
            {"manager_enabled", false},
            {"manager_init_error", ""},
            {"v2_manager_enabled", false},
            {"v2_manager_init_error", ""},
            {"frames_sent", 0ULL},
            {"updates_sent", 0ULL},
            {"v1_frames_sent", 0ULL},
            {"v1_updates_sent", 0ULL},
            {"base_queue_drops", 0ULL},
            {"update_queue_drops", 0ULL},
            {"update_stale_drops", 0ULL},
            {"ipc_push_failures", 0ULL},
            {"v1_ipc_push_failures", 0ULL},
            {"v2_ipc_push_failures", 0ULL},
            {"v2_frames_published", 0ULL},
            {"v2_yolo_updates_published", 0ULL},
            {"v2_pose_updates_published", 0ULL},
            {"v2_yolo_stale_suppressed", 0ULL},
            {"v2_pose_stale_suppressed", 0ULL},
            {"v2_pending_drops", 0ULL},
            {"v2_queue_drops", 0ULL},
            {"reader_started", false},
            {"reader_error", ""},
            {"reader_messages_popped", 0ULL},
            {"reader_base_messages", 0ULL},
            {"reader_detection_update_messages", 0ULL},
            {"reader_yolo_enabled_messages", 0ULL},
            {"reader_v2_latest_state_messages", 0ULL},
            {"reader_v2_detection_pending_messages", 0ULL},
            {"reader_v2_detection_result_messages", 0ULL},
            {"reader_v2_pose_result_messages", 0ULL},
            {"reader_camera_id_mismatches", 0ULL},
            {"reader_frame_id_gaps", 0ULL},
            {"reader_non_monotonic_frame_ids", 0ULL},
            {"reader_sequence_id_gaps", 0ULL},
            {"reader_non_monotonic_sequence_ids", 0ULL},
            {"reader_first_frame_id", 0ULL},
            {"reader_last_frame_id", 0ULL},
            {"reader_first_sequence_id", 0ULL},
            {"reader_last_sequence_id", 0ULL},
            {"status", "pass"},
            {"failures", nlohmann::json::array()}
        };

        auto add_failure = [&](const std::string& reason) {
            overall_ok = false;
            camera_json["status"] = "fail";
            camera_json["failures"].push_back(reason);
        };

        if (idx >= 0 &&
            static_cast<std::size_t>(idx) < frame_ipc_managers.size() &&
            frame_ipc_managers[idx]) {
            const FrameIPCManager& manager = *frame_ipc_managers[idx];
            camera_json["manager_enabled"] = manager.isEnabled();
            camera_json["manager_init_error"] = manager.getInitError();
            camera_json["v2_manager_enabled"] = manager.isV2Enabled();
            camera_json["v2_manager_init_error"] = manager.getV2InitError();
            camera_json["v1_frames_sent"] = manager.getFramesSent();
            camera_json["v1_updates_sent"] = manager.getUpdatesSent();
            camera_json["base_queue_drops"] = manager.getBaseQueueDrops();
            camera_json["update_queue_drops"] = manager.getUpdateQueueDrops();
            camera_json["update_stale_drops"] = manager.getUpdateStaleDrops();
            camera_json["v1_ipc_push_failures"] = manager.getIpcPushFailures();
            const shaman_v2::LiveStateCounters v2_counters = manager.getV2Counters();
            camera_json["v2_ipc_push_failures"] = v2_counters.push_failures;
            camera_json["v2_frames_published"] = v2_counters.frames_published;
            camera_json["v2_yolo_updates_published"] = v2_counters.yolo_updates_published;
            camera_json["v2_pose_updates_published"] = v2_counters.pose_updates_published;
            camera_json["v2_yolo_stale_suppressed"] = v2_counters.yolo_stale_suppressed;
            camera_json["v2_pose_stale_suppressed"] = v2_counters.pose_stale_suppressed;
            camera_json["v2_pending_drops"] = v2_counters.pending_drops;
            camera_json["v2_queue_drops"] = v2_counters.queue_drops;
            camera_json["frames_sent"] = use_v2
                ? v2_counters.frames_published
                : manager.getFramesSent();
            camera_json["updates_sent"] = use_v2
                ? (v2_counters.yolo_updates_published +
                   v2_counters.pose_updates_published)
                : manager.getUpdatesSent();
            camera_json["ipc_push_failures"] = use_v2
                ? v2_counters.push_failures
                : manager.getIpcPushFailures();
            if (!manager.isEnabled()) {
                add_failure("manager_not_enabled");
            }
            if (use_v2 && !manager.isV2Enabled()) {
                add_failure("v2_manager_not_enabled");
            }
            if (!config.allow_push_failures &&
                camera_json.value("ipc_push_failures", 0ULL) > 0) {
                add_failure("ipc_push_failures");
            }
        } else {
            add_failure("missing_frame_ipc_manager");
        }

        const auto stats_it = reader_stats_by_serial.find(camera_serial);
        if (stats_it != reader_stats_by_serial.end() && stats_it->second) {
            const HeadlessFrameIpcReaderStats& stats = *stats_it->second;
            camera_json["reader_started"] = stats.reader_started;
            camera_json["reader_error"] = stats.reader_error;
            camera_json["reader_messages_popped"] = stats.messages_popped;
            camera_json["reader_base_messages"] = stats.base_messages;
            camera_json["reader_detection_update_messages"] = stats.detection_update_messages;
            camera_json["reader_yolo_enabled_messages"] = stats.yolo_enabled_messages;
            camera_json["reader_v2_latest_state_messages"] = stats.v2_latest_state_messages;
            camera_json["reader_v2_detection_pending_messages"] =
                stats.v2_detection_pending_messages;
            camera_json["reader_v2_detection_result_messages"] =
                stats.v2_detection_result_messages;
            camera_json["reader_v2_pose_result_messages"] = stats.v2_pose_result_messages;
            camera_json["reader_camera_id_mismatches"] = stats.camera_id_mismatches;
            camera_json["reader_frame_id_gaps"] = stats.frame_id_gaps;
            camera_json["reader_non_monotonic_frame_ids"] = stats.non_monotonic_frame_ids;
            camera_json["reader_sequence_id_gaps"] = stats.sequence_id_gaps;
            camera_json["reader_non_monotonic_sequence_ids"] =
                stats.non_monotonic_sequence_ids;
            camera_json["reader_first_frame_id"] = stats.first_frame_id;
            camera_json["reader_last_frame_id"] = stats.last_frame_id;
            camera_json["reader_first_sequence_id"] = stats.first_sequence_id;
            camera_json["reader_last_sequence_id"] = stats.last_sequence_id;

            if (headless_frame_ipc_mode_is_verify_drain(config.mode)) {
                if (!stats.reader_started) {
                    add_failure("reader_not_started");
                }
                if (!stats.reader_error.empty()) {
                    add_failure("reader_error");
                }
                const uint64_t base_read_count =
                    use_v2 ? stats.v2_latest_state_messages : stats.base_messages;
                if (config.require_base_frames && base_read_count == 0) {
                    add_failure("no_base_messages_read");
                }
                if (stats.camera_id_mismatches > 0) {
                    add_failure("reader_camera_id_mismatches");
                }
                if (stats.frame_id_gaps > 0) {
                    add_failure("reader_frame_id_gaps");
                }
                if (stats.non_monotonic_frame_ids > 0) {
                    add_failure("reader_non_monotonic_frame_ids");
                }
                if (use_v2 && stats.sequence_id_gaps > 0) {
                    add_failure("reader_sequence_id_gaps");
                }
                if (use_v2 && stats.non_monotonic_sequence_ids > 0) {
                    add_failure("reader_non_monotonic_sequence_ids");
                }
            }
        } else if (headless_frame_ipc_mode_is_verify_drain(config.mode)) {
            add_failure("missing_reader_stats");
        }

        summary["cameras"][camera_serial] = std::move(camera_json);
    }

    summary["status"] = overall_ok ? "pass" : "fail";
    return write_json_file(
        std::filesystem::path(record_folder) / "frame_ipc_summary.json",
        summary,
        error_out);
}

std::vector<std::string> parse_string_list_field(const nlohmann::json& node,
                                                 const char* key,
                                                 bool* found = nullptr)
{
    if (found) {
        *found = false;
    }
    std::vector<std::string> values;
    if (!node.is_object() || !node.contains(key)) {
        return values;
    }
    if (found) {
        *found = true;
    }
    const nlohmann::json& field = node.at(key);
    if (field.is_string()) {
        values.push_back(field.get<std::string>());
        return values;
    }
    if (field.is_array()) {
        for (const auto& item : field) {
            if (item.is_string()) {
                values.push_back(item.get<std::string>());
            }
        }
    }
    return values;
}

std::vector<int> parse_int_list_field(const nlohmann::json& node,
                                      const char* key,
                                      bool* found = nullptr)
{
    if (found) {
        *found = false;
    }
    std::vector<int> values;
    if (!node.is_object() || !node.contains(key)) {
        return values;
    }
    if (found) {
        *found = true;
    }
    const nlohmann::json& field = node.at(key);
    if (field.is_number_integer()) {
        values.push_back(field.get<int>());
        return values;
    }
    if (field.is_array()) {
        for (const auto& item : field) {
            if (item.is_number_integer()) {
                values.push_back(item.get<int>());
            }
        }
    }
    return values;
}

std::vector<int> parse_toggle_override_list_field(const nlohmann::json& node,
                                                  const char* key,
                                                  bool* found = nullptr)
{
    if (found) {
        *found = false;
    }
    std::vector<int> values;
    if (!node.is_object() || !node.contains(key)) {
        return values;
    }
    if (found) {
        *found = true;
    }
    auto parse_item = [](const nlohmann::json& item, int* out) -> bool {
        if (!out) {
            return false;
        }
        if (item.is_boolean()) {
            *out = item.get<bool>() ? 1 : 0;
            return true;
        }
        if (item.is_number_integer()) {
            const int value = item.get<int>();
            if (value == -1 || value == 0 || value == 1) {
                *out = value;
                return true;
            }
            return false;
        }
        if (item.is_string()) {
            return parse_headless_toggle_override(item.get<std::string>(), out);
        }
        return false;
    };

    const nlohmann::json& field = node.at(key);
    int parsed = -1;
    if (parse_item(field, &parsed)) {
        values.push_back(parsed);
        return values;
    }
    if (field.is_array()) {
        for (const auto& item : field) {
            if (parse_item(item, &parsed)) {
                values.push_back(parsed);
            }
        }
    }
    return values;
}

std::vector<std::string> split_csv_line_simple(const std::string& line)
{
    std::vector<std::string> cells;
    std::stringstream stream(line);
    std::string cell;
    while (std::getline(stream, cell, ',')) {
        cells.push_back(cell);
    }
    return cells;
}

std::string sanitize_run_component(const std::string& value)
{
    std::string out;
    out.reserve(value.size());
    for (char ch : value) {
        if (std::isalnum(static_cast<unsigned char>(ch)) || ch == '-' || ch == '_') {
            out.push_back(ch);
        } else {
            out.push_back('_');
        }
    }
    while (!out.empty() && out.back() == '_') {
        out.pop_back();
    }
    if (out.empty()) {
        out = "value";
    }
    return out;
}

namespace {

std::string join_ints_csv(const std::vector<int>& values)
{
    std::ostringstream out;
    for (std::size_t i = 0; i < values.size(); ++i) {
        if (i != 0) {
            out << ",";
        }
        out << values[i];
    }
    return out.str();
}

std::vector<int> collect_unique_gpu_ids(const CameraParams* cameras_params,
                                        const std::vector<int>& selected_indices)
{
    std::vector<int> gpu_ids;
    std::unordered_set<int> seen;
    auto append_gpu_id = [&](int gpu_id) {
        if (gpu_id < 0) {
            return;
        }
        if (seen.insert(gpu_id).second) {
            gpu_ids.push_back(gpu_id);
        }
    };

    for (int idx : selected_indices) {
        if (!cameras_params) {
            continue;
        }
        const CameraParams& camera = cameras_params[idx];
        append_gpu_id(camera.gpu_id);
        if (camera.recording.strategy.split_gop_enabled()) {
            for (int helper_gpu_id : camera.recording.strategy.split_gop.encoder_gpu_ids) {
                append_gpu_id(helper_gpu_id);
            }
        }
    }
    std::sort(gpu_ids.begin(), gpu_ids.end());
    return gpu_ids;
}

std::filesystem::path resolve_headless_repo_relative_path(const std::string& relative_path,
                                                          const char* env_name)
{
    if (env_name && *env_name) {
        if (const char* env = std::getenv(env_name); env && *env) {
            return std::filesystem::path(env);
        }
    }

    std::error_code ec;
    const std::filesystem::path cwd_candidate =
        std::filesystem::current_path(ec) / relative_path;
    if (!ec && std::filesystem::exists(cwd_candidate)) {
        return cwd_candidate;
    }

    std::array<char, 4096> exe_path{};
    const ssize_t size = readlink("/proc/self/exe", exe_path.data(), exe_path.size() - 1);
    if (size > 0) {
        exe_path[static_cast<std::size_t>(size)] = '\0';
        const std::filesystem::path repo_candidate =
            std::filesystem::path(exe_path.data()).parent_path().parent_path().parent_path() /
            relative_path;
        if (std::filesystem::exists(repo_candidate)) {
            return repo_candidate;
        }
    }
    return std::filesystem::path(relative_path);
}

bool run_headless_shell_command(const std::string& command,
                                const std::string& label,
                                std::string* output_out,
                                std::string* error_out)
{
    std::string output;
    const bool ok = read_command_stdout(command + " 2>&1", &output);
    if (output_out) {
        *output_out = output;
    }
    if (!output.empty()) {
        std::cout << output;
        if (output.back() != '\n') {
            std::cout << std::endl;
        }
    }
    if (!ok) {
        if (error_out) {
            *error_out = label + " failed";
            if (!output.empty()) {
                *error_out += ": " + output;
            }
        }
        return false;
    }
    return true;
}

bool run_supervised_external_recorder_video_sanity(
    const HeadlessExternalRecorderContractConfig& config,
    nlohmann::json* finalization,
    std::string* error_out)
{
    if (!config.enabled() || !config.supervise_processes || !config.require_video_sanity) {
        return true;
    }
    if (!config.streams.is_object()) {
        return true;
    }

    const std::filesystem::path script =
        resolve_headless_repo_relative_path(
            "scripts/external_video_sanity.py",
            "ORANGE_EXTERNAL_RECORDER_VIDEO_SANITY_SCRIPT");
    nlohmann::json results = nlohmann::json::object();
    for (auto it = config.streams.begin(); it != config.streams.end(); ++it) {
        if (!it.value().is_object()) {
            continue;
        }
        const std::string stream_id = it.value().value("stream_id", it.key());
        const std::string mp4_path = it.value().value("mp4", std::string());
        const std::string video_sanity_path =
            it.value().value("video_sanity_json", std::string());
        if (mp4_path.empty() || video_sanity_path.empty()) {
            if (error_out) {
                *error_out = "external recorder video sanity missing mp4 or video_sanity_json for stream " +
                             stream_id;
            }
            return false;
        }

        const std::string command =
            "python3 " + shell_single_quote(script.string()) + " " +
            shell_single_quote(mp4_path) + " " +
            shell_single_quote(video_sanity_path);
        std::string output;
        const bool ok = run_headless_shell_command(
            command,
            "external recorder video sanity for stream " + stream_id,
            &output,
            error_out);
        results[stream_id] = {
            {"mp4", mp4_path},
            {"video_sanity_json", video_sanity_path},
            {"command", command},
            {"output", output},
            {"pass", ok},
        };
        if (!ok) {
            if (finalization) {
                (*finalization)["video_sanity"] = results;
            }
            return false;
        }
    }
    if (finalization) {
        (*finalization)["video_sanity"] = results;
    }
    return true;
}

bool run_supervised_external_recorder_verifier(
    const HeadlessExternalRecorderContractConfig& config,
    const std::filesystem::path& experiment_root,
    nlohmann::json* finalization,
    std::string* error_out)
{
    if (!config.enabled() || !config.supervise_processes) {
        return true;
    }

    const std::filesystem::path script =
        resolve_headless_repo_relative_path(
            "scripts/verify_external_recorder_session.py",
            "ORANGE_EXTERNAL_RECORDER_VERIFY_SCRIPT");
    const std::string command =
        "python3 " + shell_single_quote(script.string()) + " " +
        shell_single_quote(config.artifact_root) + " --analytics-root " +
        shell_single_quote(experiment_root.string());

    std::string output;
    const bool ok = run_headless_shell_command(
        command,
        "external recorder session verifier",
        &output,
        error_out);
    if (finalization) {
        (*finalization)["verifier"] = {
            {"command", command},
            {"output", output},
            {"pass", ok},
        };
    }
    return ok;
}

std::string format_external_recorder_clip_id(const int clip_index)
{
    std::ostringstream out;
    out << "clip_" << std::setw(6) << std::setfill('0') << clip_index;
    return out.str();
}

bool write_supervised_external_recorder_recording_session_manifest(
    const ExperimentRunPlan& run,
    nlohmann::json* bridge_out,
    std::string* error_out)
{
    const HeadlessExternalRecorderContractConfig& config =
        run.options.external_recorder_contract;
    if (!config.enabled() ||
        !config.supervise_processes ||
        run.options.recording_sink_mode != "external_ipc" ||
        run.options.recording_control.record_for_seconds <= 0 ||
        run.options.recording_control.clip_seconds <= 0) {
        return true;
    }
    if (!config.streams.is_object()) {
        if (error_out) {
            *error_out = "external recorder rolling manifest bridge requires stream contracts";
        }
        return false;
    }

    const std::filesystem::path run_folder(run.recording_folder);
    const nlohmann::json local_manifest =
        read_json_file_best_effort(run_folder / "recording_session.json");
    const nlohmann::json local_stream =
        local_manifest.value("stream", nlohmann::json::object());
    const nlohmann::json local_recording =
        local_manifest.value("recording", nlohmann::json::object());

    std::map<int, orange::session::RollingClipManifestOptions> clips_by_index;
    std::vector<std::string> camera_serials;
    nlohmann::json summary_paths = nlohmann::json::object();
    nlohmann::json merged_mp4s = nlohmann::json::object();
    bool all_clips_ok = true;

    for (auto it = config.streams.begin(); it != config.streams.end(); ++it) {
        if (!it.value().is_object()) {
            continue;
        }
        const nlohmann::json& stream = it.value();
        const std::string serial =
            stream.value("camera_serial", stream.value("stream_id", it.key()));
        const std::string summary_path = stream.value("summary_json", std::string());
        if (serial.empty() || summary_path.empty()) {
            if (error_out) {
                *error_out = "external recorder rolling manifest bridge missing serial or summary_json";
            }
            return false;
        }

        nlohmann::json summary;
        std::string read_error;
        if (!read_json_file(summary_path, &summary, &read_error)) {
            if (error_out) {
                *error_out = read_error;
            }
            return false;
        }
        const nlohmann::json rolling =
            summary.value("rolling_output", nlohmann::json::object());
        if (!rolling.is_object() || !rolling.value("enabled", false)) {
            if (error_out) {
                *error_out =
                    "external recorder summary missing enabled rolling_output for camera " +
                    serial;
            }
            return false;
        }
        const nlohmann::json summary_clips =
            rolling.value("clips", nlohmann::json::array());
        if (!summary_clips.is_array() || summary_clips.empty()) {
            if (error_out) {
                *error_out = "external recorder rolling_output has no clips for camera " + serial;
            }
            return false;
        }

        camera_serials.push_back(serial);
        summary_paths[serial] = summary_path;
        merged_mp4s[serial] = stream.value("mp4", std::string());
        const int fps = std::max(1, summary.value("fps", stream.value("encode_fps", 100)));

        for (const nlohmann::json& clip : summary_clips) {
            if (!clip.is_object()) {
                continue;
            }
            const int clip_index = clip.value("clip_index", -1);
            if (clip_index < 0) {
                if (error_out) {
                    *error_out = "external recorder rolling clip missing clip_index for camera " + serial;
                }
                return false;
            }
            orange::session::RollingClipManifestOptions& manifest_clip =
                clips_by_index[clip_index];
            if (manifest_clip.clip_id.empty()) {
                manifest_clip.producer = "orange_headless_external_ipc";
                manifest_clip.session_id = run.run_id;
                manifest_clip.clip_index = clip_index;
                manifest_clip.clip_id =
                    clip.value("clip_id", format_external_recorder_clip_id(clip_index));
                manifest_clip.directory = clip.value("directory", std::string());
                manifest_clip.recording_folder = manifest_clip.directory;
                manifest_clip.status = "completed";
                manifest_clip.drain_completed = true;
            }

            const bool clip_failed = clip.value("failed", false);
            all_clips_ok = all_clips_ok && !clip_failed;
            if (clip_failed) {
                manifest_clip.status = "failed";
                manifest_clip.drain_completed = false;
            }

            const uint64_t frame_count = clip.value("frame_count", 0ULL);
            const uint64_t first_frame = clip.value("first_recording_frame_id", 0ULL);
            const uint64_t last_frame = clip.value("last_recording_frame_id", 0ULL);
            if (first_frame > 0 &&
                (manifest_clip.first_recording_frame_id == 0 ||
                 first_frame < manifest_clip.first_recording_frame_id)) {
                manifest_clip.first_recording_frame_id = first_frame;
            }
            if (last_frame > manifest_clip.last_recording_frame_id) {
                manifest_clip.last_recording_frame_id = last_frame;
            }
            manifest_clip.actual_duration_s =
                std::max(
                    manifest_clip.actual_duration_s,
                    static_cast<double>(frame_count) / static_cast<double>(fps));

            orange::session::RecordingSessionCameraArtifact camera_artifact;
            camera_artifact.camera_serial = serial;
            camera_artifact.video_path = clip.value("mp4", std::string());
            camera_artifact.metadata_path = clip.value("metadata", std::string());
            camera_artifact.keyframe_path = clip.value("keyframes", std::string());
            camera_artifact.frame_count = frame_count;
            camera_artifact.first_recording_frame_id = first_frame;
            camera_artifact.last_recording_frame_id = last_frame;
            camera_artifact.recording_frame_id_gaps = 0;
            camera_artifact.packet_count = clip.value("packets_written", 0ULL);
            camera_artifact.packet_count_source = "external_recorder_summary.packets_written";
            manifest_clip.cameras.push_back(std::move(camera_artifact));
        }
    }

    if (clips_by_index.empty()) {
        if (error_out) {
            *error_out = "external recorder rolling manifest bridge found no clips";
        }
        return false;
    }
    std::sort(camera_serials.begin(), camera_serials.end());
    camera_serials.erase(
        std::unique(camera_serials.begin(), camera_serials.end()),
        camera_serials.end());

    std::vector<orange::session::RollingClipManifestOptions> clip_options;
    clip_options.reserve(clips_by_index.size());
    double sum_clip_actual_duration_s = 0.0;
    for (auto it = clips_by_index.begin(); it != clips_by_index.end(); ++it) {
        orange::session::RollingClipManifestOptions clip = std::move(it->second);
        const bool final_clip = std::next(it) == clips_by_index.end();
        clip.start_reason = it == clips_by_index.begin() ? "recording_start" : "rollover";
        clip.stop_reason =
            final_clip
                ? local_recording.value("stop_reason", std::string("record_for_seconds_elapsed"))
                : "clip_seconds_elapsed";
        clip.final_clip = final_clip;
        clip.timed_stop_hit = final_clip && clip.stop_reason == "record_for_seconds_elapsed";
        clip.requested_duration_s =
            final_clip
                ? clip.actual_duration_s
                : static_cast<double>(run.options.recording_control.clip_seconds);
        clip.rollover_request_id = 0;
        if (!final_clip) {
            clip.rollover_at_recording_frame_id =
                std::next(it)->second.first_recording_frame_id;
        } else if (it != clips_by_index.begin()) {
            clip.rollover_at_recording_frame_id = clip.first_recording_frame_id;
        }
        clip.pending_next_clip = false;
        sum_clip_actual_duration_s += clip.actual_duration_s;

        std::string clip_manifest_error;
        if (!orange::session::write_recording_session_manifest(
                (std::filesystem::path(clip.recording_folder) /
                 "clip_manifest.json").string(),
                orange::session::build_recording_clip_manifest(clip),
                &clip_manifest_error)) {
            if (error_out) {
                *error_out = clip_manifest_error;
            }
            return false;
        }
        clip_options.push_back(std::move(clip));
    }

    orange::session::RollingRecordingSessionManifestOptions manifest_options;
    manifest_options.producer = "orange_headless_external_ipc";
    manifest_options.session_id = run.run_id;
    manifest_options.created_at_utc =
        local_manifest.value("created_at_utc", std::string());
    manifest_options.updated_at_utc = get_current_utc_timestamp();
    manifest_options.recording_folder = run.recording_folder;
    manifest_options.status = all_clips_ok ? "completed" : "incomplete";
    manifest_options.requested_stream_duration_seconds = run.options.duration_seconds;
    manifest_options.stream_start_delay_seconds =
        local_stream.value("stream_start_delay_seconds", run.options.stream_start_delay_seconds);
    manifest_options.stream_started_at_utc =
        local_stream.value("started_at_utc", std::string());
    manifest_options.stream_finished_at_utc =
        local_stream.value("finished_at_utc", std::string());
    manifest_options.stream_actual_elapsed_s =
        local_stream.value("actual_elapsed_s", 0.0);
    manifest_options.stream_interrupted =
        local_stream.value("interrupted", false);
    manifest_options.recording_control = {
        run.options.recording_control.record_for_seconds,
        run.options.recording_control.clip_seconds
    };
    manifest_options.recording_started =
        local_recording.value("started", true);
    manifest_options.recording_started_at_utc =
        local_recording.value("started_at_utc", std::string());
    manifest_options.recording_started_at_elapsed_s =
        local_recording.value("started_at_elapsed_s", 0.0);
    manifest_options.recording_stop_requested =
        local_recording.value("stop_requested", true);
    manifest_options.recording_stop_requested_at_utc =
        local_recording.value("stop_requested_at_utc", std::string());
    manifest_options.recording_stop_requested_at_elapsed_s =
        local_recording.value("stop_requested_at_elapsed_s", 0.0);
    manifest_options.recording_stop_reason =
        local_recording.value("stop_reason", std::string("external_recorder_finalized"));
    manifest_options.recording_drain_completed =
        local_recording.value("drain_completed", true);
    manifest_options.recording_drained_at_utc =
        local_recording.value("drained_at_utc", std::string());
    manifest_options.recording_drained_at_elapsed_s =
        local_recording.value("drained_at_elapsed_s", 0.0);
    manifest_options.actual_recording_duration_s = sum_clip_actual_duration_s;
    manifest_options.drain_duration_s =
        local_recording.value("drain_duration_s", 0.0);
    manifest_options.sum_clip_actual_duration_s = sum_clip_actual_duration_s;
    manifest_options.rollover_implementation =
        orange::external_recorder::kExternalRecorderRollingImplementation;
    manifest_options.rollover_next_writer_preopened = false;
    manifest_options.recording_backend = {
        {"mode", "external_ipc"},
        {"status", all_clips_ok ? "completed" : "incomplete"},
        {"artifact_root", config.artifact_root},
        {"source", "external_recorder_summary"},
        {"summary_json", summary_paths},
        {"merged_mp4", merged_mp4s},
        {"external_recorder_session_json",
         (std::filesystem::path(config.artifact_root) /
          "external_recorder_session.json").string()},
        {"external_recorder_finalization_json",
         (std::filesystem::path(config.artifact_root) /
          "external_recorder_finalization.json").string()}
    };
    manifest_options.camera_serials = std::move(camera_serials);
    manifest_options.clips = std::move(clip_options);

    const nlohmann::json manifest =
        orange::session::build_rolling_clip_recording_session_manifest(manifest_options);
    const std::filesystem::path manifest_path = run_folder / "recording_session.json";
    orange::session::RecordingSessionIndexArtifacts index_artifacts;
    if (!orange::session::write_rolling_clip_index_artifacts(
            run.recording_folder,
            manifest,
            &index_artifacts,
            error_out)) {
        return false;
    }
    std::string manifest_error;
    if (!orange::session::write_recording_session_manifest(
            manifest_path.string(),
            manifest,
            &manifest_error)) {
        if (error_out) {
            *error_out = manifest_error;
        }
        return false;
    }
    const nlohmann::json snapshot_update =
        build_rolling_recording_session_snapshot_update(
            run.recording_folder,
            manifest,
            index_artifacts);
    if (!update_recording_snapshot_session_artifacts(run.recording_folder, snapshot_update)) {
        if (error_out) {
            *error_out = "failed to update recording_snapshot.json with external IPC rolling index";
        }
        return false;
    }

    if (bridge_out) {
        *bridge_out = {
            {"pass", all_clips_ok},
            {"path", manifest_path.string()},
            {"mode", "rolling_clips"},
            {"producer", "orange_headless_external_ipc"},
            {"clip_count", manifest_options.clips.size()},
            {"camera_count", manifest_options.camera_serials.size()},
            {"indexes",
             {
                 {"clip_index_json", index_artifacts.clip_index_json_path},
                 {"clip_index_csv", index_artifacts.clip_index_csv_path}
             }},
            {"summary_json", summary_paths}
        };
    }
    return all_clips_ok;
}

bool finalize_supervised_external_recorder_run(
    const ExperimentRunPlan& run,
    const std::filesystem::path& experiment_root,
    std::string* error_out)
{
    const HeadlessExternalRecorderContractConfig& config =
        run.options.external_recorder_contract;
    if (!config.enabled() || !config.supervise_processes) {
        return true;
    }

    orange::external_recorder::FinalizationManifestOptions finalization_options;
    finalization_options.experiment_root = experiment_root.string();
    finalization_options.artifact_root = config.artifact_root;
    finalization_options.run_id = run.run_id;
    finalization_options.status = "running";
    finalization_options.started_at_utc = get_current_utc_timestamp();
    nlohmann::json finalization =
        orange::external_recorder::BuildExternalRecorderFinalizationManifest(
            finalization_options);
    nlohmann::json recording_session_bridge;

    bool ok = run_supervised_external_recorder_video_sanity(
        config,
        &finalization,
        error_out);
    if (ok) {
        ok = write_supervised_external_recorder_recording_session_manifest(
            run,
            &recording_session_bridge,
            error_out);
    }
    if (ok) {
        ok = run_supervised_external_recorder_verifier(
            config,
            experiment_root,
            &finalization,
            error_out);
    }

    finalization_options.status = ok ? "pass" : "fail";
    finalization_options.finished_at_utc = get_current_utc_timestamp();
    if (finalization.contains("video_sanity")) {
        finalization_options.video_sanity = &finalization["video_sanity"];
    }
    if (finalization.contains("verifier")) {
        finalization_options.verifier = &finalization["verifier"];
    }
    if (!ok && error_out && !error_out->empty()) {
        finalization_options.error = *error_out;
    }
    finalization =
        orange::external_recorder::BuildExternalRecorderFinalizationManifest(
            finalization_options);
    if (!recording_session_bridge.is_null()) {
        finalization["recording_session_manifest"] = recording_session_bridge;
    }

    const orange::external_recorder::ArtifactWriteResult write_result =
        orange::external_recorder::WriteExternalRecorderFinalizationArtifact(
            config.artifact_root,
            finalization);
    if (!write_result.ok) {
        if (error_out) {
            *error_out = write_result.error_message;
        }
        return false;
    }
    return ok;
}

nlohmann::json build_headless_gpu_dmon_snapshot_json(const HeadlessGpuDmonMonitor& monitor)
{
    nlohmann::json info;
    info["schema_version"] = 1;
    info["tool"] = "nvidia-smi dmon";
    info["status"] = monitor.status;
    info["sample_period_seconds"] = monitor.sample_period_seconds;
    info["gpu_ids"] = monitor.gpu_ids;
    info["artifact_path"] = monitor.artifact_path;
    info["stderr_path"] = monitor.stderr_path;
    if (!monitor.started_at_utc.empty()) {
        info["started_at_utc"] = monitor.started_at_utc;
    }
    if (!monitor.stopped_at_utc.empty()) {
        info["stopped_at_utc"] = monitor.stopped_at_utc;
    }
    if (monitor.pid > 0 && monitor.active) {
        info["pid"] = static_cast<int>(monitor.pid);
    }
    if (monitor.exit_code >= 0) {
        info["exit_code"] = monitor.exit_code;
    }
    if (monitor.term_signal > 0) {
        info["signal"] = monitor.term_signal;
    }
    if (!monitor.error.empty()) {
        info["error"] = monitor.error;
    }

    info["command"] = nlohmann::json::array({
        "nvidia-smi",
        "dmon",
        "-i",
        join_ints_csv(monitor.gpu_ids),
        "-s",
        "putcm",
        "-d",
        std::to_string(monitor.sample_period_seconds),
        "-o",
        "DT"
    });
    return info;
}

void update_headless_gpu_dmon_snapshot(const HeadlessGpuDmonMonitor& monitor)
{
    if (monitor.recording_folder.empty()) {
        return;
    }
    if (!update_recording_snapshot_gpu_monitoring(
            monitor.recording_folder,
            "nvidia_smi_dmon",
            build_headless_gpu_dmon_snapshot_json(monitor))) {
        std::cerr << "Failed to update recording snapshot with nvidia-smi dmon metadata for "
                  << monitor.recording_folder << std::endl;
    }
}

void stop_headless_gpu_dmon_monitor(HeadlessGpuDmonMonitor* monitor)
{
    if (!monitor) {
        return;
    }

    if (!monitor->active || monitor->pid <= 0) {
        if (monitor->status != "not_started") {
            monitor->stopped_at_utc = get_current_utc_timestamp();
            update_headless_gpu_dmon_snapshot(*monitor);
        }
        return;
    }

    kill(monitor->pid, SIGTERM);

    int wait_status = 0;
    int waited_ms = 0;
    while (waited_ms < kGpuDmonShutdownWaitMs) {
        const pid_t wait_result = waitpid(monitor->pid, &wait_status, WNOHANG);
        if (wait_result == monitor->pid) {
            break;
        }
        if (wait_result < 0) {
            monitor->error = "waitpid failed while stopping nvidia-smi dmon";
            wait_status = 0;
            break;
        }
        usleep(10000);
        waited_ms += 10;
    }

    if (waited_ms >= kGpuDmonShutdownWaitMs) {
        kill(monitor->pid, SIGKILL);
        if (waitpid(monitor->pid, &wait_status, 0) < 0) {
            monitor->error = "waitpid failed after SIGKILL while stopping nvidia-smi dmon";
        }
    }

    monitor->active = false;
    monitor->stopped_at_utc = get_current_utc_timestamp();
    if (WIFEXITED(wait_status)) {
        monitor->exit_code = WEXITSTATUS(wait_status);
        monitor->status = (monitor->exit_code == 0) ? "completed" : "exited_with_error";
    } else if (WIFSIGNALED(wait_status)) {
        monitor->term_signal = WTERMSIG(wait_status);
        monitor->status = (monitor->term_signal == SIGKILL) ? "killed" : "stopped_with_signal";
    } else if (monitor->status == "running") {
        monitor->status = "stopped";
    }
    update_headless_gpu_dmon_snapshot(*monitor);
}

void start_headless_gpu_dmon_monitor(HeadlessGpuDmonMonitor* monitor,
                                     const std::string& recording_folder,
                                     const std::vector<int>& gpu_ids)
{
    if (!monitor) {
        return;
    }

    monitor->recording_folder = recording_folder;
    monitor->gpu_ids = gpu_ids;
    monitor->artifact_path = (std::filesystem::path(recording_folder) / "nvidia_smi_dmon.csv").string();
    monitor->stderr_path = (std::filesystem::path(recording_folder) / "nvidia_smi_dmon.stderr.log").string();
    monitor->sample_period_seconds = 1;
    monitor->started_at_utc = get_current_utc_timestamp();
    monitor->stopped_at_utc.clear();
    monitor->exit_code = -1;
    monitor->term_signal = 0;
    monitor->error.clear();
    monitor->status = "not_started";
    monitor->active = false;
    monitor->pid = -1;

    if (recording_folder.empty()) {
        monitor->status = "failed_to_start";
        monitor->error = "recording folder is empty";
        update_headless_gpu_dmon_snapshot(*monitor);
        return;
    }
    if (gpu_ids.empty()) {
        monitor->status = "failed_to_start";
        monitor->error = "no gpu ids resolved for dmon monitoring";
        update_headless_gpu_dmon_snapshot(*monitor);
        return;
    }

    const int stdout_fd = ::open(monitor->artifact_path.c_str(), O_CREAT | O_WRONLY | O_TRUNC, 0644);
    if (stdout_fd < 0) {
        monitor->status = "failed_to_start";
        monitor->error = "failed to open dmon artifact for writing";
        update_headless_gpu_dmon_snapshot(*monitor);
        return;
    }
    const int stderr_fd = ::open(monitor->stderr_path.c_str(), O_CREAT | O_WRONLY | O_TRUNC, 0644);
    if (stderr_fd < 0) {
        ::close(stdout_fd);
        monitor->status = "failed_to_start";
        monitor->error = "failed to open dmon stderr log for writing";
        update_headless_gpu_dmon_snapshot(*monitor);
        return;
    }

    const pid_t pid = fork();
    if (pid < 0) {
        ::close(stdout_fd);
        ::close(stderr_fd);
        monitor->status = "failed_to_start";
        monitor->error = "fork failed for nvidia-smi dmon";
        update_headless_gpu_dmon_snapshot(*monitor);
        return;
    }

    if (pid == 0) {
        dup2(stdout_fd, STDOUT_FILENO);
        dup2(stderr_fd, STDERR_FILENO);
        ::close(stdout_fd);
        ::close(stderr_fd);

        const std::string gpu_ids_arg = join_ints_csv(gpu_ids);
        const std::string sample_arg = std::to_string(monitor->sample_period_seconds);
        execlp("nvidia-smi",
               "nvidia-smi",
               "dmon",
               "-i", gpu_ids_arg.c_str(),
               "-s", "putcm",
               "-d", sample_arg.c_str(),
               "-o", "DT",
               static_cast<char*>(nullptr));
        std::fprintf(stderr, "Failed to exec nvidia-smi dmon: %s\n", std::strerror(errno));
        _exit(127);
    }

    ::close(stdout_fd);
    ::close(stderr_fd);

    monitor->pid = pid;
    monitor->active = true;
    monitor->status = "running";
    update_headless_gpu_dmon_snapshot(*monitor);

    usleep(kGpuDmonStartupPollMs * 1000);
    int wait_status = 0;
    const pid_t wait_result = waitpid(pid, &wait_status, WNOHANG);
    if (wait_result == pid) {
        monitor->active = false;
        monitor->stopped_at_utc = get_current_utc_timestamp();
        if (WIFEXITED(wait_status)) {
            monitor->exit_code = WEXITSTATUS(wait_status);
            monitor->status = "failed_to_start";
        } else if (WIFSIGNALED(wait_status)) {
            monitor->term_signal = WTERMSIG(wait_status);
            monitor->status = "failed_to_start";
        } else {
            monitor->status = "failed_to_start";
        }
        monitor->error = "nvidia-smi dmon exited immediately after launch";
        update_headless_gpu_dmon_snapshot(*monitor);
    } else if (wait_result < 0) {
        monitor->active = false;
        monitor->stopped_at_utc = get_current_utc_timestamp();
        monitor->status = "failed_to_start";
        monitor->error = "waitpid failed while polling nvidia-smi dmon startup";
        update_headless_gpu_dmon_snapshot(*monitor);
    }
}

} // namespace

double compute_percentile(std::vector<double> values, double percentile)
{
    if (values.empty()) {
        return 0.0;
    }
    std::sort(values.begin(), values.end());
    if (values.size() == 1) {
        return values.front();
    }
    const double clamped = std::min(100.0, std::max(0.0, percentile));
    const double rank = (clamped / 100.0) * static_cast<double>(values.size() - 1);
    const std::size_t lower = static_cast<std::size_t>(rank);
    const std::size_t upper = std::min(values.size() - 1, lower + 1);
    const double fraction = rank - static_cast<double>(lower);
    return values[lower] * (1.0 - fraction) + values[upper] * fraction;
}

double compute_csv_field_p95(const std::filesystem::path& csv_path, const std::string& field_name)
{
    std::ifstream input(csv_path);
    if (!input) {
        return 0.0;
    }

    std::string header_line;
    if (!std::getline(input, header_line)) {
        return 0.0;
    }

    std::vector<std::string> headers;
    {
        std::stringstream header_stream(header_line);
        std::string cell;
        while (std::getline(header_stream, cell, ',')) {
            headers.push_back(cell);
        }
    }

    int field_index = -1;
    for (std::size_t i = 0; i < headers.size(); ++i) {
        if (headers[i] == field_name) {
            field_index = static_cast<int>(i);
            break;
        }
    }
    if (field_index < 0) {
        return 0.0;
    }

    std::vector<double> values;
    std::string line;
    while (std::getline(input, line)) {
        if (line.empty()) {
            continue;
        }
        std::stringstream line_stream(line);
        std::string cell;
        int index = 0;
        while (std::getline(line_stream, cell, ',')) {
            if (index == field_index) {
                try {
                    values.push_back(std::stod(cell));
                } catch (...) {
                }
                break;
            }
            ++index;
        }
    }
    return compute_percentile(values, 95.0);
}

ExperimentCsvWindowStats compute_csv_window_stats(const std::filesystem::path& csv_path, int warmup_rows)
{
    ExperimentCsvWindowStats stats;
    std::ifstream input(csv_path);
    if (!input) {
        stats.error = "Failed to open " + csv_path.string();
        return stats;
    }

    std::string header_line;
    if (!std::getline(input, header_line)) {
        stats.error = "Missing CSV header in " + csv_path.string();
        return stats;
    }

    const std::vector<std::string> headers = split_csv_line_simple(header_line);
    std::unordered_map<std::string, int> header_index;
    for (std::size_t i = 0; i < headers.size(); ++i) {
        header_index.emplace(headers[i], static_cast<int>(i));
    }

    auto required_index = [&](const char* field_name) -> int {
        auto it = header_index.find(field_name);
        return it == header_index.end() ? -1 : it->second;
    };

    const int acq_fps_index = required_index("acq_fps");
    const int enc_fps_index = required_index("enc_fps");
    const int enc_fps_primary_index = required_index("enc_fps_primary");
    const int enc_fps_helpers_index = required_index("enc_fps_helpers");
    const int acq_free_entries_index = required_index("acq_free_entries");
    const int acq_free_events_index = required_index("acq_free_events");
    const int yolo_events_index = required_index("yolo_events");
    const int acq_starve_index = required_index("acq_starve");
    const int pre_buffers_index = required_index("pre_buffers");
    const int pre_events_index = required_index("pre_events");
    const int pre_waits_index = required_index("pre_waits");
    const int pre_drops_index = required_index("pre_drops");
    const int enc_fail_index = required_index("enc_fail");
    const int enc_slow_index = required_index("enc_slow");
    const int external_ipc_frames_acked_index = required_index("external_ipc_frames_acked");
    const int external_ipc_failures_index = required_index("external_ipc_failures");
    const int external_ipc_ack_timeouts_index = required_index("external_ipc_ack_timeouts");
    const int submitted_frames_index = required_index("submitted_frames");
    const int primary_routed_frames_index = required_index("primary_routed_frames");
    const int helper_requested_frames_index = required_index("helper_requested_frames");
    const int helper_fallback_frames_index = required_index("helper_fallback_frames");
    const int helper_dispatched_frames_index = required_index("helper_dispatched_frames");
    const int camera_dropped_frames_index = required_index("camera_dropped_frames");
    const int get_frame_errors_index = required_index("get_frame_errors");
    const int last_get_frame_error_code_index = required_index("last_get_frame_error_code");
    if (acq_fps_index < 0 || enc_fps_index < 0 || acq_starve_index < 0 ||
        pre_drops_index < 0 || enc_fail_index < 0) {
        stats.error = "CSV is missing one or more required fields in " + csv_path.string();
        return stats;
    }

    auto parse_double_cell = [](const std::vector<std::string>& cells, int index, double* out) -> bool {
        if (!out || index < 0 || index >= static_cast<int>(cells.size())) {
            return false;
        }
        try {
            *out = std::stod(cells[static_cast<std::size_t>(index)]);
            return true;
        } catch (...) {
            return false;
        }
    };

    auto parse_u64_cell = [](const std::vector<std::string>& cells, int index, uint64_t* out) -> bool {
        if (!out || index < 0 || index >= static_cast<int>(cells.size())) {
            return false;
        }
        try {
            *out = static_cast<uint64_t>(std::stoull(cells[static_cast<std::size_t>(index)]));
            return true;
        } catch (...) {
            return false;
        }
    };

    std::vector<double> acq_fps_values;
    std::vector<double> enc_fps_values;
    std::vector<double> enc_fps_primary_values;
    std::vector<double> enc_fps_helpers_values;
    int post_warmup_acq_free_entries_min = std::numeric_limits<int>::max();
    int post_warmup_acq_free_events_min = std::numeric_limits<int>::max();
    int post_warmup_yolo_events_min = std::numeric_limits<int>::max();
    int post_warmup_pre_buffers_min = std::numeric_limits<int>::max();
    int post_warmup_pre_events_min = std::numeric_limits<int>::max();
    uint64_t baseline_acq_starve = 0;
    uint64_t baseline_pre_waits = 0;
    uint64_t baseline_pre_drops = 0;
    uint64_t baseline_enc_fail = 0;
    uint64_t baseline_enc_slow = 0;
    uint64_t baseline_external_ipc_frames_acked = 0;
    uint64_t baseline_external_ipc_failures = 0;
    uint64_t baseline_external_ipc_ack_timeouts = 0;
    uint64_t baseline_submitted_frames = 0;
    uint64_t baseline_primary_routed_frames = 0;
    uint64_t baseline_helper_requested_frames = 0;
    uint64_t baseline_helper_fallback_frames = 0;
    uint64_t baseline_helper_dispatched_frames = 0;
    uint64_t baseline_camera_dropped_frames = 0;
    uint64_t baseline_get_frame_errors = 0;
    uint64_t last_acq_starve = 0;
    uint64_t last_pre_waits = 0;
    uint64_t last_pre_drops = 0;
    uint64_t last_enc_fail = 0;
    uint64_t last_enc_slow = 0;
    uint64_t last_external_ipc_frames_acked = 0;
    uint64_t last_external_ipc_failures = 0;
    uint64_t last_external_ipc_ack_timeouts = 0;
    uint64_t last_submitted_frames = 0;
    uint64_t last_primary_routed_frames = 0;
    uint64_t last_helper_requested_frames = 0;
    uint64_t last_helper_fallback_frames = 0;
    uint64_t last_helper_dispatched_frames = 0;
    uint64_t last_camera_dropped_frames = 0;
    uint64_t last_get_frame_errors = 0;
    int last_get_frame_error_code = 0;

    std::string line;
    std::size_t row_index = 0;
    const std::size_t warmup_row_count = static_cast<std::size_t>(std::max(0, warmup_rows));
    while (std::getline(input, line)) {
        if (line.empty()) {
            continue;
        }

        const std::vector<std::string> cells = split_csv_line_simple(line);
        double acq_fps = 0.0;
        double enc_fps = 0.0;
        double enc_fps_primary = 0.0;
        double enc_fps_helpers = 0.0;
        int acq_free_entries = -1;
        int acq_free_events = -1;
        int yolo_events = -1;
        int pre_buffers = -1;
        int pre_events = -1;
        uint64_t acq_starve = 0;
        uint64_t pre_waits = 0;
        uint64_t pre_drops = 0;
        uint64_t enc_fail = 0;
        uint64_t enc_slow = 0;
        uint64_t external_ipc_frames_acked = 0;
        uint64_t external_ipc_failures = 0;
        uint64_t external_ipc_ack_timeouts = 0;
        uint64_t submitted_frames = 0;
        uint64_t primary_routed_frames = 0;
        uint64_t helper_requested_frames = 0;
        uint64_t helper_fallback_frames = 0;
        uint64_t helper_dispatched_frames = 0;
        uint64_t camera_dropped_frames = 0;
        uint64_t get_frame_errors = 0;
        uint64_t last_get_frame_error_code_u64 = 0;
        if (!parse_double_cell(cells, acq_fps_index, &acq_fps) ||
            !parse_double_cell(cells, enc_fps_index, &enc_fps) ||
            !parse_u64_cell(cells, acq_starve_index, &acq_starve) ||
            !parse_u64_cell(cells, pre_drops_index, &pre_drops) ||
            !parse_u64_cell(cells, enc_fail_index, &enc_fail)) {
            continue;
        }
        if (enc_fps_primary_index >= 0) {
            parse_double_cell(cells, enc_fps_primary_index, &enc_fps_primary);
        }
        if (enc_fps_helpers_index >= 0) {
            parse_double_cell(cells, enc_fps_helpers_index, &enc_fps_helpers);
        }
        if (acq_free_entries_index >= 0) {
            uint64_t acq_free_entries_u64 = 0;
            if (parse_u64_cell(cells, acq_free_entries_index, &acq_free_entries_u64)) {
                acq_free_entries = static_cast<int>(acq_free_entries_u64);
            }
        }
        if (acq_free_events_index >= 0) {
            uint64_t acq_free_events_u64 = 0;
            if (parse_u64_cell(cells, acq_free_events_index, &acq_free_events_u64)) {
                acq_free_events = static_cast<int>(acq_free_events_u64);
            }
        }
        if (yolo_events_index >= 0) {
            uint64_t yolo_events_u64 = 0;
            if (parse_u64_cell(cells, yolo_events_index, &yolo_events_u64)) {
                yolo_events = static_cast<int>(yolo_events_u64);
            }
        }
        if (pre_buffers_index >= 0) {
            uint64_t pre_buffers_u64 = 0;
            if (parse_u64_cell(cells, pre_buffers_index, &pre_buffers_u64)) {
                pre_buffers = static_cast<int>(pre_buffers_u64);
            }
        }
        if (pre_events_index >= 0) {
            uint64_t pre_events_u64 = 0;
            if (parse_u64_cell(cells, pre_events_index, &pre_events_u64)) {
                pre_events = static_cast<int>(pre_events_u64);
            }
        }
        if (pre_waits_index >= 0) {
            parse_u64_cell(cells, pre_waits_index, &pre_waits);
        }
        if (enc_slow_index >= 0) {
            parse_u64_cell(cells, enc_slow_index, &enc_slow);
        }
        if (external_ipc_frames_acked_index >= 0) {
            parse_u64_cell(cells, external_ipc_frames_acked_index, &external_ipc_frames_acked);
        }
        if (external_ipc_failures_index >= 0) {
            parse_u64_cell(cells, external_ipc_failures_index, &external_ipc_failures);
        }
        if (external_ipc_ack_timeouts_index >= 0) {
            parse_u64_cell(cells, external_ipc_ack_timeouts_index, &external_ipc_ack_timeouts);
        }
        if (submitted_frames_index >= 0) {
            parse_u64_cell(cells, submitted_frames_index, &submitted_frames);
        }
        if (primary_routed_frames_index >= 0) {
            parse_u64_cell(cells, primary_routed_frames_index, &primary_routed_frames);
        }
        if (helper_requested_frames_index >= 0) {
            parse_u64_cell(cells, helper_requested_frames_index, &helper_requested_frames);
        }
        if (helper_fallback_frames_index >= 0) {
            parse_u64_cell(cells, helper_fallback_frames_index, &helper_fallback_frames);
        }
        if (helper_dispatched_frames_index >= 0) {
            parse_u64_cell(cells, helper_dispatched_frames_index, &helper_dispatched_frames);
        }
        if (camera_dropped_frames_index >= 0) {
            parse_u64_cell(cells, camera_dropped_frames_index, &camera_dropped_frames);
        }
        if (get_frame_errors_index >= 0) {
            parse_u64_cell(cells, get_frame_errors_index, &get_frame_errors);
        }
        if (last_get_frame_error_code_index >= 0 &&
            parse_u64_cell(cells, last_get_frame_error_code_index, &last_get_frame_error_code_u64)) {
            last_get_frame_error_code = static_cast<int>(last_get_frame_error_code_u64);
        }

        ++stats.total_rows;
        last_acq_starve = acq_starve;
        last_pre_waits = pre_waits;
        last_pre_drops = pre_drops;
        last_enc_fail = enc_fail;
        last_enc_slow = enc_slow;
        last_external_ipc_frames_acked = external_ipc_frames_acked;
        last_external_ipc_failures = external_ipc_failures;
        last_external_ipc_ack_timeouts = external_ipc_ack_timeouts;
        last_submitted_frames = submitted_frames;
        last_primary_routed_frames = primary_routed_frames;
        last_helper_requested_frames = helper_requested_frames;
        last_helper_fallback_frames = helper_fallback_frames;
        last_helper_dispatched_frames = helper_dispatched_frames;
        last_camera_dropped_frames = camera_dropped_frames;
        last_get_frame_errors = get_frame_errors;

        if (row_index < warmup_row_count) {
            baseline_acq_starve = acq_starve;
            baseline_pre_waits = pre_waits;
            baseline_pre_drops = pre_drops;
            baseline_enc_fail = enc_fail;
            baseline_enc_slow = enc_slow;
            baseline_external_ipc_frames_acked = external_ipc_frames_acked;
            baseline_external_ipc_failures = external_ipc_failures;
            baseline_external_ipc_ack_timeouts = external_ipc_ack_timeouts;
            baseline_submitted_frames = submitted_frames;
            baseline_primary_routed_frames = primary_routed_frames;
            baseline_helper_requested_frames = helper_requested_frames;
            baseline_helper_fallback_frames = helper_fallback_frames;
            baseline_helper_dispatched_frames = helper_dispatched_frames;
            baseline_camera_dropped_frames = camera_dropped_frames;
            baseline_get_frame_errors = get_frame_errors;
        } else {
            acq_fps_values.push_back(acq_fps);
            enc_fps_values.push_back(enc_fps);
            if (enc_fps_primary_index >= 0) {
                enc_fps_primary_values.push_back(enc_fps_primary);
            }
            if (enc_fps_helpers_index >= 0) {
                enc_fps_helpers_values.push_back(enc_fps_helpers);
            }
            ++stats.included_rows;
            if (acq_free_entries >= 0) {
                post_warmup_acq_free_entries_min = std::min(post_warmup_acq_free_entries_min, acq_free_entries);
            }
            if (acq_free_events >= 0) {
                post_warmup_acq_free_events_min = std::min(post_warmup_acq_free_events_min, acq_free_events);
            }
            if (yolo_events >= 0) {
                post_warmup_yolo_events_min = std::min(post_warmup_yolo_events_min, yolo_events);
            }
            if (pre_buffers >= 0) {
                post_warmup_pre_buffers_min = std::min(post_warmup_pre_buffers_min, pre_buffers);
            }
            if (pre_events >= 0) {
                post_warmup_pre_events_min = std::min(post_warmup_pre_events_min, pre_events);
            }
        }
        ++row_index;
    }

    if (stats.included_rows == 0) {
        stats.error = "No post-warmup samples found in " + csv_path.string();
        return stats;
    }

    const double acq_fps_sum =
        std::accumulate(acq_fps_values.begin(), acq_fps_values.end(), 0.0);
    stats.acq_fps_mean = acq_fps_sum / static_cast<double>(acq_fps_values.size());
    stats.acq_fps_p95 = compute_percentile(acq_fps_values, 95.0);
    const double enc_fps_sum =
        std::accumulate(enc_fps_values.begin(), enc_fps_values.end(), 0.0);
    stats.enc_fps_mean = enc_fps_sum / static_cast<double>(enc_fps_values.size());
    stats.enc_fps_p95 = compute_percentile(enc_fps_values, 95.0);
    if (!enc_fps_primary_values.empty()) {
        const double enc_fps_primary_sum =
            std::accumulate(enc_fps_primary_values.begin(), enc_fps_primary_values.end(), 0.0);
        stats.enc_fps_primary_mean =
            enc_fps_primary_sum / static_cast<double>(enc_fps_primary_values.size());
        stats.enc_fps_primary_p95 = compute_percentile(enc_fps_primary_values, 95.0);
    }
    if (!enc_fps_helpers_values.empty()) {
        const double enc_fps_helpers_sum =
            std::accumulate(enc_fps_helpers_values.begin(), enc_fps_helpers_values.end(), 0.0);
        stats.enc_fps_helpers_mean =
            enc_fps_helpers_sum / static_cast<double>(enc_fps_helpers_values.size());
        stats.enc_fps_helpers_p95 = compute_percentile(enc_fps_helpers_values, 95.0);
    }
    stats.acq_starve_delta =
        (last_acq_starve >= baseline_acq_starve) ? (last_acq_starve - baseline_acq_starve) : last_acq_starve;
    stats.pre_waits_delta =
        (last_pre_waits >= baseline_pre_waits) ? (last_pre_waits - baseline_pre_waits) : last_pre_waits;
    stats.pre_drops_delta =
        (last_pre_drops >= baseline_pre_drops) ? (last_pre_drops - baseline_pre_drops) : last_pre_drops;
    stats.enc_fail_delta =
        (last_enc_fail >= baseline_enc_fail) ? (last_enc_fail - baseline_enc_fail) : last_enc_fail;
    stats.enc_slow_delta =
        (last_enc_slow >= baseline_enc_slow) ? (last_enc_slow - baseline_enc_slow) : last_enc_slow;
    stats.external_ipc_frames_acked_delta =
        (last_external_ipc_frames_acked >= baseline_external_ipc_frames_acked)
            ? (last_external_ipc_frames_acked - baseline_external_ipc_frames_acked)
            : last_external_ipc_frames_acked;
    stats.external_ipc_failures_delta =
        (last_external_ipc_failures >= baseline_external_ipc_failures)
            ? (last_external_ipc_failures - baseline_external_ipc_failures)
            : last_external_ipc_failures;
    stats.external_ipc_ack_timeouts_delta =
        (last_external_ipc_ack_timeouts >= baseline_external_ipc_ack_timeouts)
            ? (last_external_ipc_ack_timeouts - baseline_external_ipc_ack_timeouts)
            : last_external_ipc_ack_timeouts;
    stats.submitted_frames_delta =
        (last_submitted_frames >= baseline_submitted_frames)
            ? (last_submitted_frames - baseline_submitted_frames)
            : last_submitted_frames;
    stats.primary_routed_frames_delta =
        (last_primary_routed_frames >= baseline_primary_routed_frames)
            ? (last_primary_routed_frames - baseline_primary_routed_frames)
            : last_primary_routed_frames;
    stats.helper_requested_frames_delta =
        (last_helper_requested_frames >= baseline_helper_requested_frames)
            ? (last_helper_requested_frames - baseline_helper_requested_frames)
            : last_helper_requested_frames;
    stats.helper_fallback_frames_delta =
        (last_helper_fallback_frames >= baseline_helper_fallback_frames)
            ? (last_helper_fallback_frames - baseline_helper_fallback_frames)
            : last_helper_fallback_frames;
    stats.helper_dispatched_frames_delta =
        (last_helper_dispatched_frames >= baseline_helper_dispatched_frames)
            ? (last_helper_dispatched_frames - baseline_helper_dispatched_frames)
            : last_helper_dispatched_frames;
    stats.camera_dropped_frames_delta =
        (last_camera_dropped_frames >= baseline_camera_dropped_frames)
            ? (last_camera_dropped_frames - baseline_camera_dropped_frames)
            : last_camera_dropped_frames;
    stats.get_frame_errors_delta =
        (last_get_frame_errors >= baseline_get_frame_errors)
            ? (last_get_frame_errors - baseline_get_frame_errors)
            : last_get_frame_errors;
    stats.last_get_frame_error_code = last_get_frame_error_code;
    if (post_warmup_acq_free_entries_min != std::numeric_limits<int>::max()) {
        stats.acq_free_entries_min = post_warmup_acq_free_entries_min;
    }
    if (post_warmup_acq_free_events_min != std::numeric_limits<int>::max()) {
        stats.acq_free_events_min = post_warmup_acq_free_events_min;
    }
    if (post_warmup_yolo_events_min != std::numeric_limits<int>::max()) {
        stats.yolo_events_min = post_warmup_yolo_events_min;
    }
    if (post_warmup_pre_buffers_min != std::numeric_limits<int>::max()) {
        stats.pre_buffers_min = post_warmup_pre_buffers_min;
    }
    if (post_warmup_pre_events_min != std::numeric_limits<int>::max()) {
        stats.pre_events_min = post_warmup_pre_events_min;
    }
    stats.ok = true;
    return stats;
}

bool load_experiment_spec(const HeadlessCliOptions& cli_options,
                          ExperimentSpec* spec,
                          std::string* error_out)
{
    if (!spec) {
        if (error_out) {
            *error_out = "Internal error: null experiment spec destination";
        }
        return false;
    }

    nlohmann::json root;
    if (!read_json_file(cli_options.experiment_spec_path, &root, error_out)) {
        return false;
    }
    if (!root.is_object()) {
        if (error_out) {
            *error_out = "Experiment spec root must be a JSON object";
        }
        return false;
    }

    spec->source_path = cli_options.experiment_spec_path;
    spec->source_json = root;
    spec->experiment_id = root.value("experiment_id", "");
    spec->notes = root.value("notes", "");
    if (spec->experiment_id.empty()) {
        if (error_out) {
            *error_out = "Experiment spec requires a non-empty experiment_id";
        }
        return false;
    }

    const nlohmann::json selection = root.value("selection", nlohmann::json::object());
    const nlohmann::json fixed = root.value("fixed", nlohmann::json::object());
    const nlohmann::json matrix = root.value("matrix", nlohmann::json::object());
    const nlohmann::json policy = root.value("policy", nlohmann::json::object());

    bool found_camera_serials = false;
    const std::vector<std::string> camera_serials =
        parse_string_list_field(selection, "camera_serials", &found_camera_serials);
    if (found_camera_serials) {
        spec->selection.select_all_cameras = false;
        spec->selection.camera_serials.clear();
        for (const std::string& serial : camera_serials) {
            append_camera_selection(&spec->selection, serial);
        }
    }

    spec->gpu_ids = parse_int_list_field(selection, "gpu_ids");
    spec->output_root = fixed.value("output_root", "");
    spec->config_folder = cli_options.config_folder.empty()
        ? fixed.value("config_folder", "")
        : cli_options.config_folder;
    spec->stream_only = fixed.value("stream_only", false);
    spec->ptp_gate_acquisition_mode = fixed.value("ptp_gate_acquisition_mode", "");
    spec->acquisition_buffer_mode = fixed.value("acquisition_buffer_mode", "auto");
    spec->ptp_gate_stagger_ns = fixed.value("ptp_gate_stagger_ns", 0);
    spec->ptp_register_read_decimate =
        fixed.value("ptp_register_read_decimate", 1);
    spec->recording_sink_mode = fixed.value("recording_sink_mode", "real");
    spec->helper_noop_source_read = fixed.value("helper_noop_source_read", false);
    spec->helper_copy_bytes = fixed.value("helper_copy_bytes", static_cast<int64_t>(-1));
    spec->helper_copy_delay_ns = fixed.value("helper_copy_delay_ns", static_cast<int64_t>(0));
    spec->duration_s = fixed.value("duration_s", 0);
    spec->warmup_s = fixed.value("warmup_s", 0);
    spec->stream_start_delay_s = fixed.value("stream_start_delay_s", 0);
    spec->nvenc_direct_input = fixed.value("nvenc_direct_input", false);
    if (fixed.contains("pre_encoder_reference_capture")) {
        if (!parse_pre_encoder_reference_capture_json(
                fixed["pre_encoder_reference_capture"],
                &spec->pre_encoder_reference_capture,
                error_out,
                "Experiment spec fixed.pre_encoder_reference_capture")) {
            return false;
        }
    }
    if (fixed.contains("frame_ipc")) {
        if (!parse_headless_frame_ipc_json(
                fixed["frame_ipc"],
                &spec->frame_ipc,
                error_out,
                "Experiment spec fixed.frame_ipc")) {
            return false;
        }
    }
    if (fixed.contains("yolo_event_log")) {
        if (!parse_headless_yolo_event_log_json(
                fixed["yolo_event_log"],
                &spec->yolo_event_log,
                error_out,
                "Experiment spec fixed.yolo_event_log")) {
            return false;
        }
    }
    if (fixed.contains("yolo_worker")) {
        if (!parse_headless_yolo_worker_json(
                fixed["yolo_worker"],
                &spec->yolo_worker,
                error_out,
                "Experiment spec fixed.yolo_worker")) {
            return false;
        }
    }
    if (fixed.contains("pose_worker")) {
        if (!parse_headless_pose_worker_json(
                fixed["pose_worker"],
                &spec->pose_worker,
                error_out,
                "Experiment spec fixed.pose_worker")) {
            return false;
        }
    }
    if (fixed.contains("recording_control")) {
        if (!parse_headless_recording_control_json(
                fixed["recording_control"],
                &spec->recording_control,
                error_out,
                "Experiment spec fixed.recording_control")) {
            return false;
        }
    }
    if (fixed.contains("external_recorder_contract")) {
        if (!parse_headless_external_recorder_contract_json(
                fixed["external_recorder_contract"],
                &spec->external_recorder_contract,
                error_out,
                "Experiment spec fixed.external_recorder_contract")) {
            return false;
        }
    }
    if (!parse_experiment_recording_overrides(fixed, spec, error_out)) {
        return false;
    }
    if (spec->output_root.empty()) {
        if (error_out) {
            *error_out = "Experiment spec requires fixed.output_root";
        }
        return false;
    }
    if (spec->duration_s <= 0) {
        if (error_out) {
            *error_out = "Experiment spec requires fixed.duration_s > 0";
        }
        return false;
    }
    if (spec->warmup_s < 0) {
        if (error_out) {
            *error_out = "Experiment spec fixed.warmup_s must be >= 0";
        }
        return false;
    }
    if (spec->stream_only && spec->yolo_event_log.enabled()) {
        if (error_out) {
            *error_out =
                "Experiment spec fixed.yolo_event_log requires recording; stream_only must be false.";
        }
        return false;
    }
    if (spec->stream_only && spec->yolo_worker.enabled()) {
        if (error_out) {
            *error_out =
                "Experiment spec fixed.yolo_worker requires recording artifacts; stream_only must be false.";
        }
        return false;
    }
    if (spec->stream_only && spec->pose_worker.enabled()) {
        if (error_out) {
            *error_out =
                "Experiment spec fixed.pose_worker requires recording artifacts; stream_only must be false.";
        }
        return false;
    }
    if (spec->stream_only && spec->recording_control.enabled()) {
        if (error_out) {
            *error_out =
                "Experiment spec fixed.recording_control requires recording; stream_only must be false.";
        }
        return false;
    }
    if (spec->recording_control.record_for_seconds > 0 &&
        spec->recording_control.record_for_seconds >= spec->duration_s + spec->warmup_s) {
        if (error_out) {
            *error_out =
                "Experiment spec fixed.recording_control.record_for_seconds must be shorter than "
                "fixed.duration_s + fixed.warmup_s for this experimental slice";
        }
        return false;
    }
    if (spec->yolo_event_log.enabled() && spec->yolo_worker.enabled()) {
        if (error_out) {
            *error_out =
                "Experiment spec cannot combine fixed.yolo_event_log synthetic mode with fixed.yolo_worker real mode.";
        }
        return false;
    }
    if (spec->pose_worker.enabled() && !spec->yolo_worker.enabled()) {
        if (error_out) {
            *error_out =
                "Experiment spec fixed.pose_worker currently requires fixed.yolo_worker mode=real because pose crops are driven by the YOLO worker result path.";
        }
        return false;
    }
    if (spec->stream_start_delay_s < 0) {
        if (error_out) {
            *error_out = "Experiment spec fixed.stream_start_delay_s must be >= 0";
        }
        return false;
    }
    if (spec->ptp_gate_stagger_ns < 0) {
        if (error_out) {
            *error_out = "Experiment spec fixed.ptp_gate_stagger_ns must be >= 0";
        }
        return false;
    }
    if (spec->ptp_register_read_decimate < 1) {
        if (error_out) {
            *error_out = "Experiment spec fixed.ptp_register_read_decimate must be >= 1";
        }
        return false;
    }
    if (spec->helper_copy_bytes < -1) {
        if (error_out) {
            *error_out = "Experiment spec fixed.helper_copy_bytes must be -1 or >= 0";
        }
        return false;
    }
    if (spec->helper_copy_delay_ns < 0) {
        if (error_out) {
            *error_out = "Experiment spec fixed.helper_copy_delay_ns must be >= 0";
        }
        return false;
    }

    if (fixed.contains("display") && fixed["display"].is_boolean() && fixed["display"].get<bool>()) {
        if (error_out) {
            *error_out = "Local experiment runner only supports display=false for now";
        }
        return false;
    }
    if (fixed.contains("yolo") && fixed["yolo"].is_boolean() && fixed["yolo"].get<bool>()) {
        if (error_out) {
            *error_out = "Local experiment runner only supports yolo=false for now";
        }
        return false;
    }
    if (spec->stream_only && spec->pre_encoder_reference_capture.enabled) {
        if (error_out) {
            *error_out =
                "Local experiment runner does not support fixed.stream_only=true "
                "with fixed.pre_encoder_reference_capture.";
        }
        return false;
    }
    if (!parse_headless_recording_sink_mode_override(
            spec->recording_sink_mode, &spec->recording_sink_mode)) {
        if (error_out) {
            *error_out =
                "Local experiment runner only supports "
                "fixed.recording_sink_mode=real|preprocess_only|immediate_recycle|threaded_handoff_only|external_ipc";
        }
        return false;
    }
    if (spec->external_recorder_contract.enabled() &&
        spec->recording_sink_mode != "external_ipc") {
        if (error_out) {
            *error_out =
                "Experiment spec fixed.external_recorder_contract requires "
                "fixed.recording_sink_mode=external_ipc.";
        }
        return false;
    }
    if (spec->recording_sink_mode == "external_ipc" &&
        spec->recording_control.clip_seconds > 0 &&
        (!spec->external_recorder_contract.enabled() ||
         !spec->external_recorder_contract.supervise_processes)) {
        if (error_out) {
            *error_out =
                "Experiment spec fixed.recording_control.clip_seconds > 0 with "
                "fixed.recording_sink_mode=external_ipc requires supervised "
                "fixed.external_recorder_contract.";
        }
        return false;
    }
    if (spec->external_recorder_contract.enabled() &&
        !spec->selection.select_all_cameras) {
        for (const std::string& serial : spec->selection.camera_serials) {
            if (!spec->external_recorder_contract.streams.contains(serial)) {
                if (error_out) {
                    *error_out =
                        "Experiment spec fixed.external_recorder_contract.streams missing camera " +
                        serial;
                }
                return false;
            }
        }
    }
    if (spec->stream_only && spec->recording_sink_mode != "real") {
        if (error_out) {
            *error_out =
                "Local experiment runner does not support fixed.stream_only=true "
                "with fixed.recording_sink_mode != real.";
        }
        return false;
    }
    if (spec->recording_sink_mode != "real" &&
        spec->pre_encoder_reference_capture.enabled) {
        if (error_out) {
            *error_out =
                "Local experiment runner does not support "
                "fixed.recording_sink_mode != real with fixed.pre_encoder_reference_capture.";
        }
        return false;
    }
    const std::string raw_sync_mode = fixed.value("sync_mode", "free_run");
    if (!parse_headless_sync_mode_override(raw_sync_mode, &spec->sync_mode)) {
        if (error_out) {
            *error_out =
                "Local experiment runner currently only supports fixed.sync_mode=free_run or ptp_gate";
        }
        return false;
    }
    if (!spec->ptp_gate_acquisition_mode.empty()) {
        std::string normalized_acquisition_mode;
        if (!parse_headless_ptp_gate_acquisition_mode_override(
                spec->ptp_gate_acquisition_mode, &normalized_acquisition_mode)) {
            if (error_out) {
                *error_out =
                    "Local experiment runner only supports "
                    "fixed.ptp_gate_acquisition_mode=multiframe or continuous";
            }
            return false;
        }
        spec->ptp_gate_acquisition_mode = normalized_acquisition_mode;
    }
    if (!parse_headless_acquisition_buffer_mode_override(
            spec->acquisition_buffer_mode, &spec->acquisition_buffer_mode)) {
        if (error_out) {
            *error_out =
                "Local experiment runner only supports "
                "fixed.acquisition_buffer_mode=auto or force_ring_copy";
        }
        return false;
    }

    spec->target_fps_tolerance_pct = policy.value("target_fps_tolerance_pct", 1.0);
    spec->require_zero_acq_starve = policy.value("require_zero_acq_starve", true);
    spec->require_zero_pre_drops = policy.value("require_zero_pre_drops", true);
    spec->require_zero_enc_fail = policy.value("require_zero_enc_fail", true);
    spec->require_zero_camera_drops = policy.value("require_zero_camera_drops", true);
    spec->require_valid_video_content = policy.value("require_valid_video_content", true);

    spec->codecs = parse_string_list_field(matrix, "codec");
    spec->presets = parse_string_list_field(matrix, "preset");
    spec->tunings = parse_string_list_field(matrix, "tuning");
    spec->rate_control_modes = parse_string_list_field(matrix, "rate_control_mode");
    spec->quality_values = parse_int_list_field(matrix, "quality_value");
    spec->gop_lengths = parse_int_list_field(matrix, "gop_length");
    spec->aq_values = parse_toggle_override_list_field(matrix, "aq");
    spec->temporal_aq_values = parse_toggle_override_list_field(matrix, "temporal_aq");
    spec->lookahead_values = parse_toggle_override_list_field(matrix, "lookahead");
    spec->lookahead_depth_values = parse_int_list_field(matrix, "lookahead_depth");
    spec->target_bitrate_bps_values = parse_int_list_field(matrix, "target_bitrate_bps");
    spec->max_bitrate_bps_values = parse_int_list_field(matrix, "max_bitrate_bps");
    spec->vbv_buffer_size_values = parse_int_list_field(matrix, "vbv_buffer_size");
    spec->importance_map_modes = parse_string_list_field(matrix, "importance_map_mode");
    spec->importance_map_roi_size_px_values = parse_int_list_field(matrix, "importance_map_roi_size_px");

    if (spec->codecs.empty()) spec->codecs.push_back("h264");
    if (spec->presets.empty()) spec->presets.push_back("p1");
    if (spec->tunings.empty()) spec->tunings.push_back("ll");
    if (spec->rate_control_modes.empty()) spec->rate_control_modes.push_back("vbr");
    if (spec->quality_values.empty()) spec->quality_values.push_back(20);
    if (spec->gop_lengths.empty()) spec->gop_lengths.push_back(0);
    if (spec->aq_values.empty()) spec->aq_values.push_back(-1);
    if (spec->temporal_aq_values.empty()) spec->temporal_aq_values.push_back(-1);
    if (spec->lookahead_values.empty()) spec->lookahead_values.push_back(-1);
    if (spec->lookahead_depth_values.empty()) spec->lookahead_depth_values.push_back(-1);
    if (spec->target_bitrate_bps_values.empty()) spec->target_bitrate_bps_values.push_back(-1);
    if (spec->max_bitrate_bps_values.empty()) spec->max_bitrate_bps_values.push_back(-1);
    if (spec->vbv_buffer_size_values.empty()) spec->vbv_buffer_size_values.push_back(-1);
    if (spec->importance_map_modes.empty()) spec->importance_map_modes.push_back("off");
    if (spec->importance_map_roi_size_px_values.empty()) {
        spec->importance_map_roi_size_px_values.push_back(ImportanceMapConfig::kDefaultRoiSizePx);
    }

    for (std::string& importance_map_mode : spec->importance_map_modes) {
        if (!parse_importance_map_mode(importance_map_mode, &importance_map_mode)) {
            if (error_out) {
                *error_out = "Experiment spec matrix.importance_map_mode contains unsupported value: " +
                             importance_map_mode;
            }
            return false;
        }
    }
    for (int& roi_size_px : spec->importance_map_roi_size_px_values) {
        if (roi_size_px <= 0) {
            if (error_out) {
                *error_out = "Experiment spec matrix.importance_map_roi_size_px must contain positive integers";
            }
            return false;
        }
        roi_size_px = normalize_importance_map_roi_size_px(roi_size_px);
    }

    return true;
}

std::vector<ExperimentRunPlan> build_experiment_run_plans(const ExperimentSpec& spec)
{
    std::vector<ExperimentRunPlan> runs;
    const std::filesystem::path experiment_root =
        std::filesystem::path(spec.output_root) / spec.experiment_id;
    const bool include_aq_in_run_id =
        spec.aq_values.size() > 1 || spec.aq_values.front() >= 0;
    const bool include_temporal_aq_in_run_id =
        spec.temporal_aq_values.size() > 1 || spec.temporal_aq_values.front() >= 0;
    const bool include_lookahead_in_run_id =
        spec.lookahead_values.size() > 1 || spec.lookahead_values.front() >= 0;
    const bool include_lookahead_depth_in_run_id =
        spec.lookahead_depth_values.size() > 1 || spec.lookahead_depth_values.front() >= 0;
    const bool include_target_bitrate_in_run_id =
        spec.target_bitrate_bps_values.size() > 1 || spec.target_bitrate_bps_values.front() > 0;
    const bool include_max_bitrate_in_run_id =
        spec.max_bitrate_bps_values.size() > 1 || spec.max_bitrate_bps_values.front() > 0;
    const bool include_vbv_in_run_id =
        spec.vbv_buffer_size_values.size() > 1 || spec.vbv_buffer_size_values.front() > 0;
    const bool include_importance_map_in_run_id =
        spec.importance_map_modes.size() > 1 || spec.importance_map_modes.front() != "off";
    const bool include_importance_map_roi_size_in_run_id =
        spec.importance_map_roi_size_px_values.size() > 1 ||
        spec.importance_map_roi_size_px_values.front() != ImportanceMapConfig::kDefaultRoiSizePx;

    int run_index = 0;
    for (const std::string& codec : spec.codecs) {
        for (const std::string& preset : spec.presets) {
            for (const std::string& tuning : spec.tunings) {
                for (const std::string& rc_mode : spec.rate_control_modes) {
                    for (int quality_value : spec.quality_values) {
                        for (int gop_length : spec.gop_lengths) {
                            for (int aq_value : spec.aq_values) {
                                for (int temporal_aq_value : spec.temporal_aq_values) {
                                    for (int lookahead_value : spec.lookahead_values) {
                                        for (int lookahead_depth_value : spec.lookahead_depth_values) {
                                            for (int target_bitrate_bps : spec.target_bitrate_bps_values) {
                                                for (int max_bitrate_bps : spec.max_bitrate_bps_values) {
                                                    for (int vbv_buffer_size : spec.vbv_buffer_size_values) {
                                                        for (const std::string& importance_map_mode : spec.importance_map_modes) {
                                                            for (int importance_map_roi_size_px : spec.importance_map_roi_size_px_values) {
                                                            ++run_index;
                                                            ExperimentRunPlan run;
                                                            run.run_index = run_index;
                                                            std::ostringstream run_id;
                                                            run_id << "run_" << std::setw(4) << std::setfill('0') << run_index
                                                                   << "__codec_" << sanitize_run_component(codec)
                                                                   << "__preset_" << sanitize_run_component(preset)
                                                                   << "__tuning_" << sanitize_run_component(tuning)
                                                                   << "__rc_" << sanitize_run_component(rc_mode)
                                                                   << "__q_" << quality_value
                                                                   << "__gop_" << gop_length;
                                                            if (include_aq_in_run_id) {
                                                                run_id << "__aq_" << sanitize_run_component(
                                                                    format_headless_toggle_override(aq_value));
                                                            }
                                                            if (include_temporal_aq_in_run_id) {
                                                                run_id << "__tempaq_" << sanitize_run_component(
                                                                    format_headless_toggle_override(temporal_aq_value));
                                                            }
                                                            if (include_lookahead_in_run_id) {
                                                                run_id << "__lookahead_" << sanitize_run_component(
                                                                    format_headless_toggle_override(lookahead_value));
                                                            }
                                                            if (include_lookahead_depth_in_run_id) {
                                                                run_id << "__lookdepth_" << lookahead_depth_value;
                                                            }
                                                            if (include_target_bitrate_in_run_id) {
                                                                run_id << "__bitrate_" << target_bitrate_bps;
                                                            }
                                                            if (include_max_bitrate_in_run_id) {
                                                                run_id << "__maxbps_" << max_bitrate_bps;
                                                            }
                                                            if (include_vbv_in_run_id) {
                                                                run_id << "__vbv_" << vbv_buffer_size;
                                                            }
                                                            if (include_importance_map_in_run_id) {
                                                                run_id << "__imap_" << sanitize_run_component(importance_map_mode);
                                                            }
                                                            if (include_importance_map_roi_size_in_run_id) {
                                                                run_id << "__imappx_" << importance_map_roi_size_px;
                                                            }
                                                            run.run_id = run_id.str();
                                                            run.recording_folder = (experiment_root / run.run_id).string();
                                                            run.duration_s = spec.duration_s;
                                                            run.warmup_s = spec.warmup_s;
                                                            run.options.mode = HeadlessMode::Local;
                                                            run.options.config_folder = spec.config_folder;
                                                            run.options.record_folder = run.recording_folder;
                                                            run.options.stream_only = spec.stream_only;
                                                            run.options.recording_sink_mode =
                                                                spec.recording_sink_mode;
                                                            run.options.sync_mode_override = spec.sync_mode;
                                                            run.options.ptp_gate_acquisition_mode_override =
                                                                spec.ptp_gate_acquisition_mode;
                                                            run.options.acquisition_buffer_mode_override =
                                                                spec.acquisition_buffer_mode;
                                                            run.options.ptp_gate_stagger_ns =
                                                                spec.ptp_gate_stagger_ns;
                                                            run.options.duration_seconds = spec.duration_s + spec.warmup_s;
                                                            run.options.stream_start_delay_seconds = spec.stream_start_delay_s;
                                                            run.options.record_start_delay_seconds = 0;
                                                            run.options.nvenc_direct_input = spec.nvenc_direct_input;
                                                            run.options.required_gpu_ids = spec.gpu_ids;
                                                            run.options.encoder_settings = spec.selection;
                                                            run.options.pre_encoder_reference_capture = spec.pre_encoder_reference_capture;
                                                            run.options.frame_ipc = spec.frame_ipc;
                                                            run.options.yolo_event_log = spec.yolo_event_log;
                                                            run.options.yolo_worker = spec.yolo_worker;
                                                            run.options.pose_worker = spec.pose_worker;
                                                            run.options.recording_control =
                                                                spec.recording_control;
                                                            run.options.external_recorder_contract =
                                                                spec.external_recorder_contract;
                                                            run.options.has_recording_override =
                                                                spec.has_recording_override;
                                                            run.options.recording_override =
                                                                spec.recording_override;
                                                            run.options.recording_overrides_by_camera =
                                                                spec.recording_overrides_by_camera;
                                                            if (run.options.pre_encoder_reference_capture.enabled &&
                                                                !run.options.pre_encoder_reference_capture.output_dir.empty()) {
                                                                const std::filesystem::path configured_output_dir(
                                                                    run.options.pre_encoder_reference_capture.output_dir);
                                                                if (!configured_output_dir.is_absolute()) {
                                                                    run.options.pre_encoder_reference_capture.output_dir =
                                                                        (std::filesystem::path(run.recording_folder) /
                                                                         configured_output_dir)
                                                                            .string();
                                                                }
                                                            }
                                                            run.options.encoder_settings.codec = codec;
                                                            run.options.encoder_settings.preset = preset;
                                                            run.options.encoder_settings.tuning = tuning;
                                                            run.options.encoder_settings.rate_control_mode = rc_mode;
                                                            run.options.encoder_settings.importance_map.mode =
                                                                importance_map_mode;
                                                            run.options.encoder_settings.importance_map.roi_size_px =
                                                                importance_map_roi_size_px;
                                                            run.options.encoder_settings.quality_value = quality_value;
                                                            run.options.encoder_settings.gop_length = gop_length;
                                                            run.options.encoder_settings.control_overrides.aq = aq_value;
                                                            run.options.encoder_settings.control_overrides.temporal_aq = temporal_aq_value;
                                                            run.options.encoder_settings.control_overrides.lookahead = lookahead_value;
                                                            run.options.encoder_settings.control_overrides.lookahead_depth =
                                                                lookahead_depth_value;
                                                            run.options.encoder_settings.control_overrides.target_bitrate_bps =
                                                                target_bitrate_bps;
                                                            run.options.encoder_settings.control_overrides.max_bitrate_bps =
                                                                max_bitrate_bps;
                                                            run.options.encoder_settings.control_overrides.vbv_buffer_size =
                                                                vbv_buffer_size;
                                                            run.config_json = {
                                                                {"codec", codec},
                                                                {"preset", preset},
                                                                {"tuning", tuning},
                                                                {"rate_control_mode", rc_mode},
                                                                {"importance_map_mode", importance_map_mode},
                                                                {"importance_map_roi_size_px", importance_map_roi_size_px},
                                                                {"quality_value", quality_value},
                                                                {"gop_length", gop_length},
                                                                {"aq", format_headless_toggle_override(aq_value)},
                                                                {"temporal_aq", format_headless_toggle_override(temporal_aq_value)},
                                                                {"lookahead", format_headless_toggle_override(lookahead_value)},
                                                                {"lookahead_depth", lookahead_depth_value},
                                                                {"target_bitrate_bps", target_bitrate_bps},
                                                                {"max_bitrate_bps", max_bitrate_bps},
                                                                {"vbv_buffer_size", vbv_buffer_size},
                                                                {"camera_serials", run.options.encoder_settings.select_all_cameras
                                                                                       ? nlohmann::json::array({"all"})
                                                                                       : nlohmann::json(run.options.encoder_settings.camera_serials)},
                                                                {"gpu_ids", spec.gpu_ids},
                                                                {"duration_s", spec.duration_s},
                                                                {"warmup_s", spec.warmup_s},
                                                                {"stream_start_delay_s", spec.stream_start_delay_s},
                                                                {"record_start_delay_s", 0},
                                                                {"stream_only", spec.stream_only},
                                                                {"acquisition_buffer_mode",
                                                                 spec.acquisition_buffer_mode},
                                                                {"ptp_register_read_decimate",
                                                                 spec.ptp_register_read_decimate},
                                                                {"recording_sink_mode", spec.recording_sink_mode},
                                                                {"helper_noop_source_read",
                                                                 spec.helper_noop_source_read},
                                                                {"helper_copy_bytes",
                                                                 spec.helper_copy_bytes},
                                                                {"helper_copy_delay_ns",
                                                                 spec.helper_copy_delay_ns},
                                                                {"nvenc_direct_input", spec.nvenc_direct_input},
                                                                {"recording_folder", run.recording_folder},
                                                                {"pre_encoder_reference_capture",
                                                                 build_pre_encoder_reference_capture_json(
                                                                     run.options.pre_encoder_reference_capture)},
                                                                {"frame_ipc",
                                                                 build_headless_frame_ipc_config_json(
                                                                     run.options.frame_ipc)},
                                                                {"yolo_event_log",
                                                                 build_headless_yolo_event_log_config_json(
                                                                     run.options.yolo_event_log)},
                                                                {"yolo_worker",
                                                                 build_headless_yolo_worker_config_json(
                                                                     run.options.yolo_worker)},
                                                                {"pose_worker",
                                                                 build_headless_pose_worker_config_json(
                                                                     run.options.pose_worker)},
                                                                {"recording_control",
                                                                 build_headless_recording_control_config_json(
                                                                     run.options.recording_control)},
                                                                {"external_recorder_contract",
                                                                 build_headless_external_recorder_contract_config_json(
                                                                     run.options.external_recorder_contract,
                                                                     &run.options.recording_control)},
                                                            };
                                                            if (spec.has_recording_override) {
                                                                run.config_json["recording"] =
                                                                    spec.recording_override;
                                                            }
                                                            if (!spec.recording_overrides_by_camera.empty()) {
                                                                run.config_json["recording_by_camera"] =
                                                                    build_recording_override_map_json(
                                                                        spec.recording_overrides_by_camera);
                                                            }
                                                            runs.push_back(std::move(run));
                                                            }
                                                        }
                                                    }
                                                }
                                            }
                                        }
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
    }
    return runs;
}

struct HeadlessRollingClipRuntime {
    int clip_index = 0;
    std::string clip_id;
    std::string directory;
    std::string recording_folder;
    std::string start_reason;
    std::string stop_reason;
    std::chrono::steady_clock::time_point started_time{};
    std::chrono::steady_clock::time_point stop_requested_time{};
    std::chrono::steady_clock::time_point finalized_time{};
    std::string started_at_utc;
    std::string stop_requested_at_utc;
    std::string finalized_at_utc;
    double requested_duration_s = 0.0;
    uint64_t rollover_request_id = 0;
    uint64_t rollover_at_recording_frame_id = 0;
    uint64_t first_recording_frame_id = 0;
    uint64_t last_recording_frame_id = 0;
    bool pending_next_clip = false;
    bool timed_stop_hit = false;
    bool final_clip = false;
    bool drain_completed = false;
    bool active = false;
    bool finalized = false;
};

struct MetadataFrameStats {
    uint64_t frame_count = 0;
    uint64_t first_recording_frame_id = 0;
    uint64_t last_recording_frame_id = 0;
    uint64_t recording_frame_id_gaps = 0;
};

std::string format_headless_clip_id(const int clip_index)
{
    std::ostringstream out;
    out << "clip_" << std::setw(6) << std::setfill('0') << clip_index;
    return out.str();
}

std::string headless_clip_directory(const int clip_index)
{
    return (std::filesystem::path("clips") / format_headless_clip_id(clip_index)).string();
}

std::string headless_clip_folder(const std::string& recording_folder, const int clip_index)
{
    return (std::filesystem::path(recording_folder) / headless_clip_directory(clip_index)).string();
}

void set_headless_recording_output_folder(CameraControl* camera_control,
                                          const std::string& output_folder)
{
    if (!camera_control) {
        return;
    }
    if (!output_folder.empty()) {
        orange::ScopedFsuid fsuid_guard;
        (void)fsuid_guard;
        make_folder(output_folder);
    }
    std::lock_guard<std::mutex> lock(camera_control->recording_folder_mutex);
    camera_control->recording_output_folder = output_folder;
}

uint32_t resolve_headless_rollover_gop_length(const HeadlessCliOptions& options,
                                              const CameraParams* cameras_params,
                                              const std::vector<int>& selected_inventory_indices)
{
    if (options.encoder_settings.gop_length > 0) {
        return static_cast<uint32_t>(options.encoder_settings.gop_length);
    }
    if (cameras_params && !selected_inventory_indices.empty()) {
        const int frame_rate = cameras_params[selected_inventory_indices.front()].frame_rate;
        if (frame_rate > 0) {
            return static_cast<uint32_t>(frame_rate);
        }
    }
    return 1;
}

uint32_t resolve_headless_rollover_frame_rate(const CameraParams* cameras_params,
                                              const std::vector<int>& selected_inventory_indices)
{
    if (cameras_params && !selected_inventory_indices.empty()) {
        const int frame_rate = cameras_params[selected_inventory_indices.front()].frame_rate;
        if (frame_rate > 0) {
            return static_cast<uint32_t>(frame_rate);
        }
    }
    return 1;
}

uint64_t next_recording_gop_boundary_frame(uint64_t latest_frame_id, uint32_t gop_length)
{
    const uint64_t gop = std::max<uint32_t>(1u, gop_length);
    uint64_t candidate = (latest_frame_id / gop) * gop + 1;
    if (candidate <= latest_frame_id) {
        candidate += gop;
    }
    return candidate;
}

uint64_t request_headless_recording_rollover(CameraControl* camera_control,
                                             const std::string& next_output_folder,
                                             uint64_t rollover_at_frame_id)
{
    if (!camera_control || next_output_folder.empty() || rollover_at_frame_id == 0) {
        return 0;
    }
    {
        orange::ScopedFsuid fsuid_guard;
        (void)fsuid_guard;
        make_folder(next_output_folder);
    }

    std::lock_guard<std::mutex> lock(camera_control->recording_folder_mutex);
    if (camera_control->recording_rollover_request_id > 0 &&
        camera_control->pending_recording_output_folder == next_output_folder &&
        camera_control->recording_rollover_at_frame_id == rollover_at_frame_id) {
        return camera_control->recording_rollover_request_id;
    }
    const uint64_t next_request_id = std::max(
        camera_control->recording_rollover_request_id,
        camera_control->recording_rollover_completed_request_id) + 1;
    camera_control->pending_recording_output_folder = next_output_folder;
    camera_control->recording_rollover_at_frame_id = rollover_at_frame_id;
    camera_control->recording_rollover_request_id = next_request_id;
    return next_request_id;
}

MetadataFrameStats read_metadata_frame_stats(const std::filesystem::path& metadata_path)
{
    MetadataFrameStats stats;
    std::ifstream input(metadata_path);
    if (!input) {
        return stats;
    }

    std::string header;
    if (!std::getline(input, header)) {
        return stats;
    }

    std::string line;
    uint64_t previous_frame_id = 0;
    while (std::getline(input, line)) {
        if (line.empty()) {
            continue;
        }
        const std::size_t comma = line.find(',');
        const std::string frame_text =
            comma == std::string::npos ? line : line.substr(0, comma);
        try {
            const uint64_t frame_id = static_cast<uint64_t>(std::stoull(frame_text));
            if (stats.frame_count == 0) {
                stats.first_recording_frame_id = frame_id;
            } else if (frame_id > previous_frame_id + 1) {
                stats.recording_frame_id_gaps += frame_id - previous_frame_id - 1;
            }
            previous_frame_id = frame_id;
            stats.last_recording_frame_id = frame_id;
            ++stats.frame_count;
        } catch (...) {
        }
    }
    return stats;
}

std::vector<orange::session::RecordingSessionCameraArtifact>
build_headless_clip_camera_artifacts(const std::vector<int>& selected_inventory_indices,
                                     const CameraParams* cameras_params,
                                     const std::string& clip_folder)
{
    std::vector<std::string> camera_serials;
    if (!cameras_params) {
        return {};
    }
    for (int idx : selected_inventory_indices) {
        camera_serials.push_back(cameras_params[idx].camera_serial);
    }
    return orange::session::build_recording_camera_artifacts(
        camera_serials,
        clip_folder,
        false);
}

int run_local_recording_session(const HeadlessCliOptions& options, bool print_inventory)
{
    GigEVisionDeviceInfo unsorted_device_info[max_cameras];
    const int discovered_cam_count = scan_cameras(max_cameras, unsorted_device_info);
    if (discovered_cam_count <= 0) {
        std::cerr << "No cameras found." << std::endl;
        return 1;
    }

    GigEVisionDeviceInfo device_info[max_cameras];
    sort_cameras_ip(unsorted_device_info, device_info, discovered_cam_count);
    if (print_inventory) {
        print_available_cameras(device_info, discovered_cam_count);
    }

    if (options.list_cameras) {
        return 0;
    }

    CameraGpuOverrideMap camera_gpu_overrides;
    std::string override_error;
    if (!build_camera_gpu_override_map(
            options.encoder_settings,
            options.required_gpu_ids,
            &camera_gpu_overrides,
            &override_error)) {
        std::cerr << override_error << std::endl;
        return 2;
    }

    std::vector<int> selected_inventory_indices;
    try {
        selected_inventory_indices = resolve_selected_device_inventory_indices(
            device_info,
            discovered_cam_count,
            options.encoder_settings);
    } catch (const std::exception& ex) {
        std::cerr << ex.what() << std::endl;
        return 1;
    }
    if (selected_inventory_indices.empty()) {
        std::cerr << "No cameras selected for local headless run." << std::endl;
        return 1;
    }

    HeadlessHostPtpStackGuard host_ptp_stack_guard;
    std::string normalized_sync_mode_override;
    if (parse_headless_sync_mode_override(
            options.sync_mode_override,
            &normalized_sync_mode_override) &&
        normalized_sync_mode_override == "ptp_gate") {
        std::string host_ptp_error;
        if (!ensure_headless_host_ptp_stack(&host_ptp_stack_guard, &host_ptp_error)) {
            std::cerr << host_ptp_error << std::endl;
            return 1;
        }
    }

    std::unique_ptr<CameraEmergent[]> ecams(new CameraEmergent[discovered_cam_count]);
    std::unique_ptr<CameraParams[]> cameras_params(new CameraParams[discovered_cam_count]);
    std::unique_ptr<CameraEachSelect[]> cameras_select(new CameraEachSelect[discovered_cam_count]);

    if (!open_cameras(
            cameras_params.get(),
            ecams.get(),
            cameras_select.get(),
            device_info,
            discovered_cam_count,
            options.config_folder,
            camera_gpu_overrides,
            &selected_inventory_indices)) {
        std::cerr << "Failed to open cameras." << std::endl;
        return 1;
    }

    std::string sync_mode_override_error;
    if (!apply_sync_mode_override_to_selected_cameras(
            options,
            cameras_params.get(),
            selected_inventory_indices,
            &sync_mode_override_error)) {
        std::cerr << sync_mode_override_error << std::endl;
        close_selected_cameras(selected_inventory_indices, ecams.get(), cameras_params.get());
        return 1;
    }

    std::string ptp_gate_acquisition_mode_error;
    if (!apply_ptp_gate_acquisition_mode_to_selected_cameras(
            options,
            cameras_params.get(),
            selected_inventory_indices,
            &ptp_gate_acquisition_mode_error)) {
        std::cerr << ptp_gate_acquisition_mode_error << std::endl;
        close_selected_cameras(selected_inventory_indices, ecams.get(), cameras_params.get());
        return 1;
    }

    std::string acquisition_buffer_mode_error;
    if (!apply_acquisition_buffer_mode_to_selected_cameras(
            options,
            cameras_params.get(),
            selected_inventory_indices,
            &acquisition_buffer_mode_error)) {
        std::cerr << acquisition_buffer_mode_error << std::endl;
        close_selected_cameras(selected_inventory_indices, ecams.get(), cameras_params.get());
        return 1;
    }

    std::string ptp_gate_stagger_error;
    if (!apply_ptp_gate_stagger_to_selected_cameras(
            options,
            cameras_params.get(),
            selected_inventory_indices,
            &ptp_gate_stagger_error)) {
        std::cerr << ptp_gate_stagger_error << std::endl;
        close_selected_cameras(selected_inventory_indices, ecams.get(), cameras_params.get());
        return 1;
    }

    std::string recording_override_error;
    if (!apply_recording_overrides_to_selected_cameras(
            options,
            cameras_params.get(),
            selected_inventory_indices,
            &recording_override_error)) {
        std::cerr << recording_override_error << std::endl;
        close_selected_cameras(selected_inventory_indices, ecams.get(), cameras_params.get());
        return 1;
    }

    const int selected_camera_count = static_cast<int>(selected_inventory_indices.size());
    for (int inventory_index : selected_inventory_indices) {
        cameras_params[inventory_index].num_cameras = selected_camera_count;
    }

    bool selected_run_uses_ptp = false;
    for (int inventory_index : selected_inventory_indices) {
        if (camera_sync_mode_uses_ptp(&cameras_params[inventory_index])) {
            selected_run_uses_ptp = true;
            break;
        }
    }

    if (selected_run_uses_ptp) {
        std::string host_ptp_error;
        if (!ensure_headless_host_ptp_stack(&host_ptp_stack_guard, &host_ptp_error)) {
            std::cerr << host_ptp_error << std::endl;
            close_selected_cameras(selected_inventory_indices, ecams.get(), cameras_params.get());
            return 1;
        }
    }

    if (selected_run_uses_ptp && options.ptp_gate_stagger_ns > 0) {
        std::cout << "Headless PTP gate stagger enabled."
                  << " per_camera_offset_ns=" << options.ptp_gate_stagger_ns
                  << std::endl;
        for (int inventory_index : selected_inventory_indices) {
            if (!camera_sync_mode_uses_ptp(&cameras_params[inventory_index])) {
                continue;
            }
            std::cout << "  Camera " << cameras_params[inventory_index].camera_serial
                      << " gate_offset_ns=" << cameras_params[inventory_index].ptp_gate_offset_ns
                      << std::endl;
        }
    }
    if (selected_run_uses_ptp && !options.ptp_gate_acquisition_mode_override.empty()) {
        std::cout << "Headless PTP gate acquisition mode override: "
                  << options.ptp_gate_acquisition_mode_override
                  << std::endl;
        for (int inventory_index : selected_inventory_indices) {
            if (!camera_sync_mode_uses_ptp(&cameras_params[inventory_index])) {
                continue;
            }
            std::cout << "  Camera " << cameras_params[inventory_index].camera_serial
                      << " acquisition_mode=" << cameras_params[inventory_index].ptp_gate_acquisition_mode
                      << std::endl;
        }
    }

    if (!options.acquisition_buffer_mode_override.empty()) {
        std::cout << "Headless acquisition buffer mode override: "
                  << options.acquisition_buffer_mode_override
                  << std::endl;
        for (int inventory_index : selected_inventory_indices) {
            std::cout << "  Camera " << cameras_params[inventory_index].camera_serial
                      << " acquisition_buffer_mode="
                      << cameras_params[inventory_index].acquisition_buffer_mode
                      << std::endl;
        }
    }

    if (options.stream_start_delay_seconds > 0) {
        std::cout << "Local headless camera-open settle delay started."
                  << " stream_start_delay_s=" << options.stream_start_delay_seconds
                  << std::endl;
        const auto stream_start_deadline =
            std::chrono::steady_clock::now() + std::chrono::seconds(options.stream_start_delay_seconds);
        while (!quit_server && std::chrono::steady_clock::now() < stream_start_deadline) {
            usleep(100000);
        }
        if (quit_server) {
            close_selected_cameras(selected_inventory_indices, ecams.get(), cameras_params.get());
            return 1;
        }
    }

    CameraControl camera_control;
    PTPParams ptp_params{};
    std::vector<std::thread> camera_threads;
    std::vector<CameraResources> camera_resources;
    std::vector<int> active_camera_indices;
    std::vector<std::unique_ptr<ModernRecordingPipeline>> recording_pipelines;
    std::vector<std::unique_ptr<YoloWorker>> yolo_workers;
    std::vector<std::unique_ptr<CropProducerWorker>> crop_producer_workers;
    std::vector<std::unique_ptr<PoseWorker>> pose_workers;
    std::vector<std::unique_ptr<FrameIPCManager>> frame_ipc_managers;
    HeadlessFrameIpcRuntime frame_ipc_runtime;
    HeadlessThreadFailureState thread_failure_state;
    HeadlessGpuDmonMonitor gpu_dmon_monitor;
    const bool enable_recording = !options.stream_only;
    const std::string active_record_folder = options.record_folder;
    const bool supervise_external_recorder =
        enable_recording &&
        options.recording_sink_mode == "external_ipc" &&
        options.external_recorder_contract.enabled() &&
        options.external_recorder_contract.supervise_processes;
    orange::external_recorder::SupervisorProcessOptions external_recorder_process_options;
    orange::external_recorder::SupervisedRecorderLifecycleState external_recorder_lifecycle;
    auto stop_supervised_external_recorder = [&]() {
        std::string stop_error;
        const bool stopped = orange::external_recorder::StopSupervisedRecorderLifecycle(
            &external_recorder_lifecycle,
            &stop_error);
        if (!external_recorder_lifecycle.last_artifact_error.empty()) {
            std::cerr << external_recorder_lifecycle.last_artifact_error << std::endl;
            external_recorder_lifecycle.last_artifact_error.clear();
        }
        if (!stopped) {
            std::cerr << "External recorder supervisor shutdown failed: "
                      << stop_error << std::endl;
        }
        return stopped;
    };
    if (supervise_external_recorder) {
        const char* verifier_env = std::getenv("ORANGE_EXTERNAL_RECORDER_VERIFY_SCRIPT");
        const std::string verifier_path =
            (verifier_env && *verifier_env)
                ? verifier_env
                : "scripts/verify_external_recorder_session.py";
        const std::filesystem::path analytics_root =
            std::filesystem::path(active_record_folder).parent_path();
        orange::external_recorder::SupervisedRecorderLifecycleOptions lifecycle_options;
        lifecycle_options.contract =
            build_headless_external_recorder_contract_config_json(
                options.external_recorder_contract,
                &options.recording_control);
        lifecycle_options.recorder_tool_path =
            options.external_recorder_contract.recorder_tool_path;
        lifecycle_options.default_session_id =
            options.external_recorder_contract.session_id;
        lifecycle_options.analytics_root = analytics_root.string();
        lifecycle_options.verifier_path = verifier_path;
        lifecycle_options.process_options = external_recorder_process_options;
        std::string supervisor_error;
        if (!orange::external_recorder::StartSupervisedRecorderLifecycle(
                lifecycle_options,
                &external_recorder_lifecycle,
                &supervisor_error)) {
            std::cerr << supervisor_error << std::endl;
            if (!external_recorder_lifecycle.last_artifact_error.empty()) {
                std::cerr << external_recorder_lifecycle.last_artifact_error << std::endl;
                external_recorder_lifecycle.last_artifact_error.clear();
            }
            close_selected_cameras(selected_inventory_indices, ecams.get(), cameras_params.get());
            return 1;
        }
        std::cout << "External recorder supervisor started."
                  << " streams=" << external_recorder_lifecycle.plan.streams.size()
                  << " artifact_root=" << external_recorder_lifecycle.plan.artifact_root
                  << std::endl;
    }
    const std::string yolo_decimate_value =
        options.yolo_worker.enabled() ? std::to_string(options.yolo_worker.decimate) : "";
    std::unique_ptr<ScopedEnvVarOverride> yolo_decimate_override;
    if (options.yolo_worker.enabled()) {
        yolo_decimate_override = std::make_unique<ScopedEnvVarOverride>(
            "ORANGE_YOLO_DECIMATE",
            yolo_decimate_value.c_str());
    }
    std::unique_ptr<ScopedEnvVarOverride> pose_engine_path_override;
    std::unique_ptr<ScopedEnvVarOverride> pose_mode_override;
    std::unique_ptr<ScopedEnvVarOverride> pose_skeleton_id_override;
    std::unique_ptr<ScopedEnvVarOverride> pose_skeleton_path_override;
    std::unique_ptr<ScopedEnvVarOverride> pose_synthetic_detection_override;
    std::unique_ptr<ScopedEnvVarOverride> pose_synthetic_every_override;
    std::unique_ptr<ScopedEnvVarOverride> pose_synthetic_box_width_override;
    std::unique_ptr<ScopedEnvVarOverride> pose_synthetic_box_height_override;
    std::unique_ptr<ScopedEnvVarOverride> pose_synthetic_label_override;
    std::unique_ptr<ScopedEnvVarOverride> pose_synthetic_confidence_override;
    std::unique_ptr<ScopedEnvVarOverride> pose_prewarm_iterations_override;
    std::unique_ptr<ScopedEnvVarOverride> pose_crop_frame_pool_size_override;
    if (options.pose_worker.enabled()) {
        pose_mode_override = std::make_unique<ScopedEnvVarOverride>(
            "ORANGE_POSE_MODE",
            options.pose_worker.mode.c_str());
        pose_engine_path_override = std::make_unique<ScopedEnvVarOverride>(
            "ORANGE_POSE_ENGINE_PATH",
            options.pose_worker.engine_path.c_str());
        pose_skeleton_id_override = std::make_unique<ScopedEnvVarOverride>(
            "ORANGE_POSE_SKELETON_ID",
            options.pose_worker.skeleton_id.c_str());
        pose_skeleton_path_override = std::make_unique<ScopedEnvVarOverride>(
            "ORANGE_POSE_SKELETON_PATH",
            options.pose_worker.skeleton_path.c_str());
        pose_synthetic_detection_override = std::make_unique<ScopedEnvVarOverride>(
            "ORANGE_HEADLESS_POSE_SYNTHETIC_RUNTIME_DETECTIONS",
            options.pose_worker.synthetic_runtime_detection_enabled() ? "1" : "0");
        pose_synthetic_every_override = std::make_unique<ScopedEnvVarOverride>(
            "ORANGE_HEADLESS_POSE_SYNTHETIC_EVERY_N_FRAMES",
            std::to_string(options.pose_worker.synthetic_detection_every_n_frames).c_str());
        pose_synthetic_box_width_override = std::make_unique<ScopedEnvVarOverride>(
            "ORANGE_HEADLESS_POSE_SYNTHETIC_BOX_WIDTH_PX",
            std::to_string(options.pose_worker.synthetic_detection_box_width_px).c_str());
        pose_synthetic_box_height_override = std::make_unique<ScopedEnvVarOverride>(
            "ORANGE_HEADLESS_POSE_SYNTHETIC_BOX_HEIGHT_PX",
            std::to_string(options.pose_worker.synthetic_detection_box_height_px).c_str());
        pose_synthetic_label_override = std::make_unique<ScopedEnvVarOverride>(
            "ORANGE_HEADLESS_POSE_SYNTHETIC_LABEL",
            std::to_string(options.pose_worker.synthetic_detection_label).c_str());
        pose_synthetic_confidence_override = std::make_unique<ScopedEnvVarOverride>(
            "ORANGE_HEADLESS_POSE_SYNTHETIC_CONFIDENCE",
            std::to_string(options.pose_worker.synthetic_detection_confidence).c_str());
        pose_prewarm_iterations_override = std::make_unique<ScopedEnvVarOverride>(
            "ORANGE_POSE_PREWARM_ITERATIONS",
            std::to_string(options.pose_worker.prewarm_iterations).c_str());
        if (options.pose_worker.crop_frame_pool_size > 0) {
            pose_crop_frame_pool_size_override = std::make_unique<ScopedEnvVarOverride>(
                "ORANGE_CROP_FRAME_POOL_SIZE",
                std::to_string(options.pose_worker.crop_frame_pool_size).c_str());
        }
    }

    const std::string encoder_setup = build_headless_encoder_setup_string(options.encoder_settings);
    const bool rolling_clip_recording =
        enable_recording &&
        options.recording_sink_mode != "external_ipc" &&
        options.recording_control.record_for_seconds > 0 &&
        options.recording_control.clip_seconds > 0;
    const std::string initial_recording_output_folder =
        rolling_clip_recording
            ? headless_clip_folder(active_record_folder, 0)
            : std::string();
    if (rolling_clip_recording) {
        camera_control.preserve_recording_session_state = true;
    }
    const bool started = start_camera_thread(
        camera_threads,
        camera_resources,
        active_camera_indices,
        recording_pipelines,
        yolo_workers,
        crop_producer_workers,
        pose_workers,
        frame_ipc_managers,
        &frame_ipc_runtime,
        options.frame_ipc,
        &thread_failure_state,
        &gpu_dmon_monitor,
        cameras_params.get(),
        ecams.get(),
        &camera_control,
        cameras_select.get(),
        device_info,
        discovered_cam_count,
        &ptp_params,
        options.required_gpu_ids,
        active_record_folder,
        encoder_setup,
        options.nvenc_direct_input,
        options.recording_sink_mode,
        options.pre_encoder_reference_capture,
        options.record_start_delay_seconds,
        enable_recording,
        initial_recording_output_folder,
        options.yolo_event_log,
        options.yolo_worker,
        options.pose_worker);

    if (!started) {
        stop_supervised_external_recorder();
        stop_headless_frame_ipc_runtime(&frame_ipc_runtime);
        clear_headless_frame_ipc_managers(frame_ipc_managers);
        close_selected_cameras(selected_inventory_indices, ecams.get(), cameras_params.get());
        return 1;
    }

    std::cout << "Local headless " << (enable_recording ? "recording" : "stream-only")
              << " run started."
              << " folder=" << (active_record_folder.empty() ? "<none>" : active_record_folder)
              << " acquisition_buffer_mode=" << options.acquisition_buffer_mode_override
              << " recording_sink_mode=" << options.recording_sink_mode
              << " frame_ipc=" << headless_frame_ipc_mode_to_string(options.frame_ipc.mode)
              << " yolo_event_log=" << options.yolo_event_log.mode
              << " yolo_worker=" << options.yolo_worker.mode
              << " cameras=" << format_selected_camera_serials(options.encoder_settings)
              << std::endl;

    const std::string session_started_at_utc = get_current_utc_timestamp();
    const auto run_start_time = std::chrono::steady_clock::now();
    const auto record_arm_time = run_start_time + std::chrono::seconds(options.record_start_delay_seconds);
    bool recording_armed = !enable_recording || camera_control.record_video;
    std::chrono::steady_clock::time_point recording_start_time{};
    std::chrono::steady_clock::time_point recording_stop_request_time{};
    std::chrono::steady_clock::time_point recording_drain_done_time{};
    std::string recording_started_at_utc;
    std::string recording_stop_requested_at_utc;
    std::string recording_drained_at_utc;
    std::string recording_stop_reason;
    bool recording_clock_anchored = !options.recording_control.enabled();
    bool recording_auto_stop_requested = false;
    bool recording_drain_completed = false;
    std::vector<HeadlessRollingClipRuntime> rolling_clips;
    HeadlessRollingClipRuntime active_rolling_clip;
    HeadlessRollingClipRuntime pending_next_rolling_clip;
    const uint32_t rolling_gop_length =
        resolve_headless_rollover_gop_length(
            options, cameras_params.get(), selected_inventory_indices);
    const uint32_t rolling_frame_rate =
        resolve_headless_rollover_frame_rate(
            cameras_params.get(), selected_inventory_indices);
    const auto rolling_prepare_margin =
        std::chrono::duration_cast<std::chrono::steady_clock::duration>(
            std::chrono::duration<double>(
                std::min(
                    1.0,
                    std::max(
                        0.05,
                        2.0 * static_cast<double>(rolling_gop_length) /
                            static_cast<double>(std::max<uint32_t>(1u, rolling_frame_rate))))));
    const uint64_t rolling_target_frame_count =
        static_cast<uint64_t>(std::max(0, options.recording_control.record_for_seconds)) *
        static_cast<uint64_t>(std::max<uint32_t>(1u, rolling_frame_rate));
    const uint64_t rolling_terminal_tail_coalesce_frames =
        static_cast<uint64_t>(std::max<uint32_t>(1u, rolling_gop_length));
    bool terminal_tail_rollover_suppressed = false;
    auto rollover_would_create_terminal_tail = [&](const uint64_t rollover_at_frame_id) {
        if (!rolling_clip_recording ||
            rolling_target_frame_count == 0 ||
            rollover_at_frame_id <= rolling_target_frame_count) {
            return false;
        }
        return rollover_at_frame_id - rolling_target_frame_count <=
            rolling_terminal_tail_coalesce_frames;
    };

    auto elapsed_since_run_start = [&](std::chrono::steady_clock::time_point time) {
        if (time.time_since_epoch().count() == 0) {
            return 0.0;
        }
        return std::chrono::duration<double>(time - run_start_time).count();
    };
    auto elapsed_between = [](std::chrono::steady_clock::time_point start,
                              std::chrono::steady_clock::time_point finish) {
        if (start.time_since_epoch().count() == 0 ||
            finish.time_since_epoch().count() == 0 ||
            finish < start) {
            return 0.0;
        }
        return std::chrono::duration<double>(finish - start).count();
    };
    auto anchor_recording_clock_if_ready = [&]() {
        if (!enable_recording ||
            !recording_armed ||
            recording_clock_anchored ||
            !options.recording_control.enabled()) {
            return false;
        }
        const uint64_t first_recording_frame_id =
            camera_control.latest_recording_frame_id.load(std::memory_order_relaxed);
        if (first_recording_frame_id == 0) {
            return false;
        }

        const auto now = std::chrono::steady_clock::now();
        const std::string now_utc = get_current_utc_timestamp();
        recording_start_time = now;
        recording_started_at_utc = now_utc;
        recording_clock_anchored = true;
        if (rolling_clip_recording &&
            active_rolling_clip.active &&
            !active_rolling_clip.finalized &&
            active_rolling_clip.clip_index == 0) {
            active_rolling_clip.started_time = now;
            active_rolling_clip.started_at_utc = now_utc;
        }
        std::cout << "Local headless timed recording clock started."
                  << " first_recording_frame_id=" << first_recording_frame_id
                  << " folder=" << options.record_folder
                  << std::endl;
        return true;
    };
    auto begin_rolling_clip = [&](const int clip_index,
                                  const std::string& start_reason,
                                  const std::chrono::steady_clock::time_point start_time,
                                  const std::string& start_utc,
                                  const uint64_t first_recording_frame_id) {
        active_rolling_clip = HeadlessRollingClipRuntime{};
        active_rolling_clip.clip_index = clip_index;
        active_rolling_clip.clip_id = format_headless_clip_id(clip_index);
        active_rolling_clip.directory = headless_clip_directory(clip_index);
        active_rolling_clip.recording_folder = headless_clip_folder(active_record_folder, clip_index);
        active_rolling_clip.start_reason = start_reason;
        active_rolling_clip.started_time = start_time;
        active_rolling_clip.started_at_utc = start_utc.empty() ? get_current_utc_timestamp() : start_utc;
        active_rolling_clip.requested_duration_s =
            static_cast<double>(options.recording_control.clip_seconds);
        active_rolling_clip.first_recording_frame_id = first_recording_frame_id;
        active_rolling_clip.active = true;
        set_headless_recording_output_folder(&camera_control, active_rolling_clip.recording_folder);
        {
            std::lock_guard<std::mutex> lock(camera_control.recording_folder_mutex);
            camera_control.recording_folder = active_record_folder;
            camera_control.preserve_recording_session_state = true;
        }
        std::cout << "Local headless rolling clip armed."
                  << " clip_id=" << active_rolling_clip.clip_id
                  << " folder=" << active_rolling_clip.recording_folder
                  << std::endl;
    };
    auto finish_rolling_clip = [&](const std::string& stop_reason,
                                   const bool final_clip,
                                   const std::chrono::steady_clock::time_point stop_time,
                                   const std::string& stop_utc,
                                   const bool drain_completed,
                                   const std::chrono::steady_clock::time_point drain_done_time,
                                   const std::string& drain_done_utc,
                                   const uint64_t last_recording_frame_id) {
        if (!active_rolling_clip.active || active_rolling_clip.finalized) {
            return;
        }
        active_rolling_clip.stop_reason = stop_reason;
        active_rolling_clip.stop_requested_time = stop_time;
        active_rolling_clip.stop_requested_at_utc = stop_utc;
        active_rolling_clip.finalized_time = drain_done_time;
        active_rolling_clip.finalized_at_utc = drain_done_utc;
        active_rolling_clip.last_recording_frame_id = last_recording_frame_id;
        active_rolling_clip.timed_stop_hit = stop_reason == "record_for_seconds_elapsed";
        active_rolling_clip.final_clip = final_clip;
        active_rolling_clip.drain_completed = drain_completed;
        active_rolling_clip.finalized = true;
        active_rolling_clip.active = false;
        rolling_clips.push_back(active_rolling_clip);
        std::cout << "Local headless rolling clip finalized."
                  << " clip_id=" << active_rolling_clip.clip_id
                  << " stop_reason=" << stop_reason
                  << " rollover_at_frame=" << active_rolling_clip.rollover_at_recording_frame_id
                  << " drain=" << (drain_completed ? "completed" : "timed_out")
                  << std::endl;
    };
    auto complete_pending_rollover_if_ready = [&]() {
        if (!pending_next_rolling_clip.pending_next_clip ||
            pending_next_rolling_clip.rollover_request_id == 0) {
            return false;
        }

        uint64_t completed_request_id = 0;
        uint64_t completed_frame_id = 0;
        std::string completed_folder;
        {
            std::lock_guard<std::mutex> lock(camera_control.recording_folder_mutex);
            completed_request_id = camera_control.recording_rollover_completed_request_id;
            completed_frame_id = camera_control.recording_rollover_completed_frame_id;
            completed_folder = camera_control.recording_rollover_completed_folder;
        }
        if (completed_request_id < pending_next_rolling_clip.rollover_request_id) {
            return false;
        }

        const auto now = std::chrono::steady_clock::now();
        const std::string now_utc = get_current_utc_timestamp();
        finish_rolling_clip(
            active_rolling_clip.stop_reason.empty()
                ? "clip_seconds_elapsed"
                : active_rolling_clip.stop_reason,
            false,
            active_rolling_clip.stop_requested_time.time_since_epoch().count() == 0
                ? now
                : active_rolling_clip.stop_requested_time,
            active_rolling_clip.stop_requested_at_utc.empty()
                ? now_utc
                : active_rolling_clip.stop_requested_at_utc,
            true,
            now,
            now_utc,
            completed_frame_id > 0 ? completed_frame_id - 1 : 0);
        begin_rolling_clip(
            pending_next_rolling_clip.clip_index,
            pending_next_rolling_clip.start_reason,
            now,
            now_utc,
            completed_frame_id);
        active_rolling_clip.rollover_request_id =
            pending_next_rolling_clip.rollover_request_id;
        active_rolling_clip.rollover_at_recording_frame_id = completed_frame_id;
        if (!completed_folder.empty()) {
            active_rolling_clip.recording_folder = completed_folder;
        }
        pending_next_rolling_clip = HeadlessRollingClipRuntime{};
        return true;
    };

    if (enable_recording && recording_armed) {
        if (options.recording_control.enabled()) {
            recording_start_time = {};
            recording_started_at_utc.clear();
        } else {
            recording_start_time = run_start_time;
            recording_started_at_utc = get_current_utc_timestamp();
        }
        if (rolling_clip_recording) {
            begin_rolling_clip(
                0,
                "recording_start",
                recording_start_time,
                recording_started_at_utc,
                1);
        }
    }
    if (enable_recording && !recording_armed && options.record_start_delay_seconds > 0) {
        std::cout << "Local headless stream warmup started."
                  << " record_arm_delay_s=" << options.record_start_delay_seconds
                  << std::endl;
    }

    const auto deadline = (options.duration_seconds > 0)
        ? run_start_time + std::chrono::seconds(options.duration_seconds)
        : std::chrono::steady_clock::time_point::max();

    while (!quit_server && std::chrono::steady_clock::now() < deadline) {
        if (enable_recording &&
            !recording_armed &&
            options.record_start_delay_seconds > 0 &&
            std::chrono::steady_clock::now() >= record_arm_time) {
            const auto now = std::chrono::steady_clock::now();
            const std::string now_utc = get_current_utc_timestamp();
            if (rolling_clip_recording) {
                begin_rolling_clip(0, "recording_start", now, now_utc, 1);
            }
            camera_control.recording_draining = false;
            camera_control.stop_record = false;
            camera_control.record_video = true;
            recording_armed = true;
            if (options.recording_control.enabled()) {
                recording_start_time = {};
                recording_started_at_utc.clear();
                recording_clock_anchored = false;
            } else {
                recording_start_time = now;
                recording_started_at_utc = now_utc;
            }
            std::cout << "Local headless recording armed after warmup."
                      << " folder=" << options.record_folder
                      << std::endl;
        }
        anchor_recording_clock_if_ready();
        if (enable_recording && rolling_clip_recording) {
            complete_pending_rollover_if_ready();
        }
        if (enable_recording &&
            recording_armed &&
            !recording_auto_stop_requested &&
            options.recording_control.record_for_seconds > 0 &&
            recording_start_time.time_since_epoch().count() > 0 &&
            std::chrono::steady_clock::now() >=
                recording_start_time + std::chrono::seconds(
                    options.recording_control.record_for_seconds)) {
            const auto now = std::chrono::steady_clock::now();
            const std::string now_utc = get_current_utc_timestamp();
            recording_stop_request_time = now;
            recording_stop_requested_at_utc = now_utc;
            recording_stop_reason = "record_for_seconds_elapsed";
            std::cout << "Local headless timed recording stop requested."
                      << " record_for_seconds="
                      << options.recording_control.record_for_seconds
                      << " folder=" << options.record_folder
                      << std::endl;
            if (rolling_clip_recording &&
                pending_next_rolling_clip.pending_next_clip) {
                const auto wait_deadline =
                    std::chrono::steady_clock::now() + std::chrono::seconds(2);
                while (std::chrono::steady_clock::now() < wait_deadline &&
                       pending_next_rolling_clip.pending_next_clip) {
                    if (complete_pending_rollover_if_ready()) {
                        break;
                    }
                    usleep(10000);
                }
            }
            if (rolling_clip_recording) {
                camera_control.preserve_recording_session_state = false;
            }
            request_recording_drain(recording_pipelines, &camera_control);
            recording_drain_completed = wait_for_recording_drain(
                &camera_control,
                std::chrono::seconds(10),
                "Headless timed recording drain");
            recording_drain_done_time = std::chrono::steady_clock::now();
            recording_drained_at_utc = get_current_utc_timestamp();
            if (rolling_clip_recording) {
                finish_rolling_clip(
                    recording_stop_reason,
                    true,
                    recording_stop_request_time,
                    recording_stop_requested_at_utc,
                    recording_drain_completed,
                    recording_drain_done_time,
                    recording_drained_at_utc,
                    camera_control.latest_recording_frame_id.load(std::memory_order_relaxed));
            }
            recording_auto_stop_requested = true;
            std::cout << "Local headless timed recording drain "
                      << (recording_drain_completed ? "completed." : "timed out.")
                      << " folder=" << options.record_folder
                      << std::endl;
        } else if (enable_recording &&
                   rolling_clip_recording &&
                   recording_armed &&
                   recording_start_time.time_since_epoch().count() > 0 &&
                   !recording_auto_stop_requested &&
                   active_rolling_clip.active &&
                   !pending_next_rolling_clip.pending_next_clip &&
                   !terminal_tail_rollover_suppressed &&
                   std::chrono::steady_clock::now() + rolling_prepare_margin >=
                       active_rolling_clip.started_time +
                           std::chrono::seconds(options.recording_control.clip_seconds)) {
            const auto clip_stop_time = std::chrono::steady_clock::now();
            const std::string clip_stop_utc = get_current_utc_timestamp();
            const std::string clip_stop_reason = "clip_seconds_elapsed";
            const uint64_t requested_clip_frames =
                static_cast<uint64_t>(options.recording_control.clip_seconds) *
                static_cast<uint64_t>(std::max<uint32_t>(1u, rolling_frame_rate));
            uint64_t rollover_at_frame_id = next_recording_gop_boundary_frame(
                active_rolling_clip.first_recording_frame_id + requested_clip_frames - 1,
                rolling_gop_length);
            const uint64_t latest_recording_frame_id =
                camera_control.latest_recording_frame_id.load(std::memory_order_relaxed);
            const uint64_t minimum_prepared_frame_id =
                latest_recording_frame_id + rolling_gop_length;
            if (rollover_at_frame_id <= minimum_prepared_frame_id) {
                rollover_at_frame_id = next_recording_gop_boundary_frame(
                    minimum_prepared_frame_id,
                    rolling_gop_length);
            }
            const int next_clip_index = active_rolling_clip.clip_index + 1;
            if (rollover_would_create_terminal_tail(rollover_at_frame_id)) {
                terminal_tail_rollover_suppressed = true;
                std::cout << "Local headless rolling terminal tail rollover suppressed."
                          << " clip_id=" << active_rolling_clip.clip_id
                          << " rollover_at_frame=" << rollover_at_frame_id
                          << " target_frame=" << rolling_target_frame_count
                          << " coalesce_frames=" << rolling_terminal_tail_coalesce_frames
                          << std::endl;
                usleep(20000);
                continue;
            }
            const std::string next_clip_folder =
                headless_clip_folder(active_record_folder, next_clip_index);
            const uint64_t rollover_request_id = request_headless_recording_rollover(
                &camera_control,
                next_clip_folder,
                rollover_at_frame_id);
            if (rollover_request_id == 0) {
                std::cerr << "Local headless rolling clip rollover request failed."
                          << " clip_id=" << active_rolling_clip.clip_id
                          << " next_folder=" << next_clip_folder
                          << " rollover_at_frame=" << rollover_at_frame_id
                          << std::endl;
            } else {
                active_rolling_clip.stop_reason = clip_stop_reason;
                active_rolling_clip.stop_requested_time = clip_stop_time;
                active_rolling_clip.stop_requested_at_utc = clip_stop_utc;
                active_rolling_clip.rollover_request_id = rollover_request_id;
                active_rolling_clip.rollover_at_recording_frame_id = rollover_at_frame_id;
                pending_next_rolling_clip = HeadlessRollingClipRuntime{};
                pending_next_rolling_clip.clip_index = next_clip_index;
                pending_next_rolling_clip.clip_id = format_headless_clip_id(next_clip_index);
                pending_next_rolling_clip.directory = headless_clip_directory(next_clip_index);
                pending_next_rolling_clip.recording_folder = next_clip_folder;
                pending_next_rolling_clip.start_reason = "rollover";
                pending_next_rolling_clip.rollover_request_id = rollover_request_id;
                pending_next_rolling_clip.rollover_at_recording_frame_id = rollover_at_frame_id;
                pending_next_rolling_clip.first_recording_frame_id = rollover_at_frame_id;
                pending_next_rolling_clip.pending_next_clip = true;
            }
            std::cout << "Local headless rolling clip stop requested."
                      << " clip_id=" << active_rolling_clip.clip_id
                      << " folder=" << active_rolling_clip.recording_folder
                      << " next_clip=" << format_headless_clip_id(next_clip_index)
                      << " rollover_at_frame=" << rollover_at_frame_id
                      << " request_id=" << rollover_request_id
                      << std::endl;
        }
        usleep(20000);
    }

    if (thread_failure_state.has_failure() && gpu_dmon_monitor.error.empty()) {
        gpu_dmon_monitor.error = thread_failure_state.get_first_error();
    }
    if (thread_failure_state.has_failure()) {
        stop_headless_gpu_dmon_monitor(&gpu_dmon_monitor);
    }

    shutdown_headless_run(
        camera_threads,
        camera_resources,
        active_camera_indices,
        recording_pipelines,
        yolo_workers,
        crop_producer_workers,
        pose_workers,
        &gpu_dmon_monitor,
        ecams.get(),
        cameras_params.get(),
        discovered_cam_count,
        &selected_inventory_indices,
        &camera_control,
        &ptp_params,
        false);

    const bool external_recorder_stop_ok = stop_supervised_external_recorder();

    if (enable_recording && options.recording_control.enabled()) {
        const auto session_finished_time = std::chrono::steady_clock::now();
        const std::string session_finished_at_utc = get_current_utc_timestamp();
        if (recording_start_time.time_since_epoch().count() > 0 &&
            recording_stop_request_time.time_since_epoch().count() == 0) {
            recording_stop_request_time = session_finished_time;
            recording_stop_requested_at_utc = session_finished_at_utc;
            recording_stop_reason = quit_server ? "interrupted" : "run_shutdown";
        }
        if (recording_drain_done_time.time_since_epoch().count() == 0) {
            recording_drain_done_time = session_finished_time;
            recording_drained_at_utc = session_finished_at_utc;
            recording_drain_completed =
                camera_control.active_recorders.load(std::memory_order_relaxed) == 0;
        }
        if (rolling_clip_recording && active_rolling_clip.active && !active_rolling_clip.finalized) {
            finish_rolling_clip(
                recording_stop_reason,
                true,
                recording_stop_request_time,
                recording_stop_requested_at_utc,
                recording_drain_completed,
                recording_drain_done_time,
                recording_drained_at_utc,
                camera_control.latest_recording_frame_id.load(std::memory_order_relaxed));
        }

        std::vector<std::string> selected_camera_serials;
        selected_camera_serials.reserve(selected_inventory_indices.size());
        for (int idx : selected_inventory_indices) {
            selected_camera_serials.push_back(cameras_params[idx].camera_serial);
        }
        std::vector<orange::session::RecordingSessionCameraArtifact> camera_artifacts =
            orange::session::build_recording_camera_artifacts(
                selected_camera_serials,
                active_record_folder,
                true);

        const double actual_recording_duration_s =
            elapsed_between(recording_start_time, recording_stop_request_time);
        const double drain_duration_s =
            elapsed_between(recording_stop_request_time, recording_drain_done_time);
        const bool recording_started =
            recording_start_time.time_since_epoch().count() > 0;
        const bool timed_stop_hit =
            recording_stop_reason == "record_for_seconds_elapsed";

        nlohmann::json manifest;
        orange::session::RecordingSessionIndexArtifacts rolling_index_artifacts;
        if (rolling_clip_recording) {
            std::vector<std::string> camera_serials;
            camera_serials.reserve(selected_inventory_indices.size());
            for (int idx : selected_inventory_indices) {
                camera_serials.push_back(cameras_params[idx].camera_serial);
            }

            std::vector<orange::session::RollingClipManifestOptions> clip_options;
            clip_options.reserve(rolling_clips.size());
            double sum_clip_actual_duration_s = 0.0;
            bool all_clips_drained = !rolling_clips.empty();
            for (const HeadlessRollingClipRuntime& clip : rolling_clips) {
                orange::session::RollingClipManifestOptions clip_manifest;
                clip_manifest.producer = "orange_headless";
                clip_manifest.session_id =
                    std::filesystem::path(active_record_folder).filename().string();
                clip_manifest.clip_index = clip.clip_index;
                clip_manifest.clip_id = clip.clip_id;
                clip_manifest.recording_folder = clip.recording_folder;
                clip_manifest.directory = clip.directory;
                clip_manifest.status = clip.drain_completed ? "completed" : "incomplete";
                clip_manifest.start_reason = clip.start_reason;
                clip_manifest.stop_reason = clip.stop_reason;
                clip_manifest.started_at_utc = clip.started_at_utc;
                clip_manifest.started_at_elapsed_s =
                    elapsed_since_run_start(clip.started_time);
                clip_manifest.stop_requested_at_utc = clip.stop_requested_at_utc;
                clip_manifest.stop_requested_at_elapsed_s =
                    elapsed_since_run_start(clip.stop_requested_time);
                clip_manifest.finalized_at_utc = clip.finalized_at_utc;
                clip_manifest.finalized_at_elapsed_s =
                    elapsed_since_run_start(clip.finalized_time);
                const double clip_actual_duration_s =
                    elapsed_between(clip.started_time, clip.stop_requested_time);
                clip_manifest.actual_duration_s = clip_actual_duration_s;
                clip_manifest.drain_duration_s =
                    elapsed_between(clip.stop_requested_time, clip.finalized_time);
                clip_manifest.rollover_request_id = clip.rollover_request_id;
                clip_manifest.rollover_at_recording_frame_id =
                    clip.rollover_at_recording_frame_id;
                clip_manifest.first_recording_frame_id = clip.first_recording_frame_id;
                clip_manifest.last_recording_frame_id = clip.last_recording_frame_id;
                clip_manifest.pending_next_clip = clip.pending_next_clip;
                clip_manifest.requested_duration_s = clip.final_clip
                    ? std::max(
                          0.0,
                          static_cast<double>(options.recording_control.record_for_seconds) -
                              elapsed_between(recording_start_time, clip.started_time))
                    : clip.requested_duration_s;
                clip_manifest.timed_stop_hit = clip.timed_stop_hit;
                clip_manifest.final_clip = clip.final_clip;
                clip_manifest.drain_completed = clip.drain_completed;
                clip_manifest.cameras = build_headless_clip_camera_artifacts(
                    selected_inventory_indices,
                    cameras_params.get(),
                    clip.recording_folder);
                for (const auto& camera_artifact : clip_manifest.cameras) {
                    if (camera_artifact.first_recording_frame_id > 0 &&
                        (clip_manifest.first_recording_frame_id == 0 ||
                         camera_artifact.first_recording_frame_id <
                             clip_manifest.first_recording_frame_id)) {
                        clip_manifest.first_recording_frame_id =
                            camera_artifact.first_recording_frame_id;
                    }
                    if (camera_artifact.last_recording_frame_id >
                        clip_manifest.last_recording_frame_id) {
                        clip_manifest.last_recording_frame_id =
                            camera_artifact.last_recording_frame_id;
                    }
                }

                sum_clip_actual_duration_s += clip_actual_duration_s;
                all_clips_drained = all_clips_drained && clip.drain_completed;

                std::string clip_manifest_error;
                if (!orange::session::write_recording_session_manifest(
                        (std::filesystem::path(clip.recording_folder) /
                         "clip_manifest.json").string(),
                        orange::session::build_recording_clip_manifest(clip_manifest),
                        &clip_manifest_error)) {
                    std::cerr << clip_manifest_error << std::endl;
                    stop_headless_frame_ipc_managers(frame_ipc_managers);
                    stop_headless_frame_ipc_runtime(&frame_ipc_runtime);
                    clear_headless_frame_ipc_managers(frame_ipc_managers);
                    return 1;
                }
                clip_options.push_back(std::move(clip_manifest));
            }

            orange::session::RollingRecordingSessionManifestOptions manifest_options;
            manifest_options.producer = "orange_headless";
            manifest_options.session_id =
                std::filesystem::path(active_record_folder).filename().string();
            manifest_options.created_at_utc = session_started_at_utc;
            manifest_options.updated_at_utc = session_finished_at_utc;
            manifest_options.recording_folder = active_record_folder;
            manifest_options.status =
                (recording_started && recording_drain_completed && all_clips_drained)
                    ? "completed"
                    : "incomplete";
            manifest_options.requested_stream_duration_seconds = options.duration_seconds;
            manifest_options.stream_start_delay_seconds = options.stream_start_delay_seconds;
            manifest_options.stream_started_at_utc = session_started_at_utc;
            manifest_options.stream_finished_at_utc = session_finished_at_utc;
            manifest_options.stream_actual_elapsed_s =
                std::chrono::duration<double>(session_finished_time - run_start_time).count();
            manifest_options.stream_interrupted = quit_server;
            manifest_options.recording_control = {
                options.recording_control.record_for_seconds,
                options.recording_control.clip_seconds
            };
            manifest_options.recording_started = recording_started;
            manifest_options.recording_started_at_utc = recording_started_at_utc;
            manifest_options.recording_started_at_elapsed_s =
                elapsed_since_run_start(recording_start_time);
            manifest_options.recording_stop_requested =
                recording_stop_request_time.time_since_epoch().count() > 0;
            manifest_options.recording_stop_requested_at_utc = recording_stop_requested_at_utc;
            manifest_options.recording_stop_requested_at_elapsed_s =
                elapsed_since_run_start(recording_stop_request_time);
            manifest_options.recording_stop_reason = recording_stop_reason;
            manifest_options.recording_drain_completed = recording_drain_completed;
            manifest_options.recording_drained_at_utc = recording_drained_at_utc;
            manifest_options.recording_drained_at_elapsed_s =
                elapsed_since_run_start(recording_drain_done_time);
            manifest_options.actual_recording_duration_s = actual_recording_duration_s;
            manifest_options.drain_duration_s = drain_duration_s;
            manifest_options.sum_clip_actual_duration_s = sum_clip_actual_duration_s;
            manifest_options.camera_serials = std::move(camera_serials);
            manifest_options.clips = std::move(clip_options);
            manifest =
                orange::session::build_rolling_clip_recording_session_manifest(manifest_options);
            std::string index_error;
            if (!orange::session::write_rolling_clip_index_artifacts(
                    active_record_folder,
                    manifest,
                    &rolling_index_artifacts,
                    &index_error)) {
                std::cerr << index_error << std::endl;
                stop_headless_frame_ipc_managers(frame_ipc_managers);
                stop_headless_frame_ipc_runtime(&frame_ipc_runtime);
                clear_headless_frame_ipc_managers(frame_ipc_managers);
                return 1;
            }
        } else {
            orange::session::SingleClipRecordingSessionManifestOptions manifest_options;
            manifest_options.producer = "orange_headless";
            manifest_options.session_id = std::filesystem::path(active_record_folder).filename().string();
            manifest_options.created_at_utc = session_started_at_utc;
            manifest_options.updated_at_utc = session_finished_at_utc;
            manifest_options.recording_folder = active_record_folder;
            manifest_options.status =
                (recording_started && recording_drain_completed) ? "completed" : "incomplete";
            manifest_options.requested_stream_duration_seconds = options.duration_seconds;
            manifest_options.stream_start_delay_seconds = options.stream_start_delay_seconds;
            manifest_options.stream_started_at_utc = session_started_at_utc;
            manifest_options.stream_finished_at_utc = session_finished_at_utc;
            manifest_options.stream_actual_elapsed_s =
                std::chrono::duration<double>(session_finished_time - run_start_time).count();
            manifest_options.stream_interrupted = quit_server;
            manifest_options.recording_control = {
                options.recording_control.record_for_seconds,
                options.recording_control.clip_seconds
            };
            manifest_options.recording_started = recording_started;
            manifest_options.recording_started_at_utc = recording_started_at_utc;
            manifest_options.recording_started_at_elapsed_s =
                elapsed_since_run_start(recording_start_time);
            manifest_options.recording_stop_requested =
                recording_stop_request_time.time_since_epoch().count() > 0;
            manifest_options.recording_stop_requested_at_utc = recording_stop_requested_at_utc;
            manifest_options.recording_stop_requested_at_elapsed_s =
                elapsed_since_run_start(recording_stop_request_time);
            manifest_options.recording_stop_reason = recording_stop_reason;
            manifest_options.recording_drain_completed = recording_drain_completed;
            manifest_options.recording_drained_at_utc = recording_drained_at_utc;
            manifest_options.recording_drained_at_elapsed_s =
                elapsed_since_run_start(recording_drain_done_time);
            manifest_options.actual_recording_duration_s = actual_recording_duration_s;
            manifest_options.drain_duration_s = drain_duration_s;
            manifest_options.timed_stop_hit = timed_stop_hit;
            manifest_options.cameras = std::move(camera_artifacts);
            manifest =
                orange::session::build_single_clip_recording_session_manifest(manifest_options);
        }

        std::string manifest_error;
        if (!orange::session::write_recording_session_manifest(
                (std::filesystem::path(active_record_folder) / "recording_session.json").string(),
                manifest,
                &manifest_error)) {
            std::cerr << manifest_error << std::endl;
            stop_headless_frame_ipc_managers(frame_ipc_managers);
            stop_headless_frame_ipc_runtime(&frame_ipc_runtime);
            clear_headless_frame_ipc_managers(frame_ipc_managers);
            return 1;
        }
        if (rolling_clip_recording) {
            const nlohmann::json snapshot_update =
                build_rolling_recording_session_snapshot_update(
                    active_record_folder,
                    manifest,
                    rolling_index_artifacts);
            if (!update_recording_snapshot_session_artifacts(
                    active_record_folder,
                    snapshot_update)) {
                std::cerr << "Failed to update recording snapshot with rolling clip index."
                          << std::endl;
                stop_headless_frame_ipc_managers(frame_ipc_managers);
                stop_headless_frame_ipc_runtime(&frame_ipc_runtime);
                clear_headless_frame_ipc_managers(frame_ipc_managers);
                return 1;
            }
        }
    }

    stop_headless_frame_ipc_managers(frame_ipc_managers);
    stop_headless_frame_ipc_runtime(&frame_ipc_runtime);
    std::string frame_ipc_summary_error;
    if (!write_headless_frame_ipc_summary(
            active_record_folder,
            options.frame_ipc,
            cameras_params.get(),
            selected_inventory_indices,
            frame_ipc_managers,
            frame_ipc_runtime,
            &frame_ipc_summary_error)) {
        std::cerr << frame_ipc_summary_error << std::endl;
        clear_headless_frame_ipc_managers(frame_ipc_managers);
        return 1;
    }
    clear_headless_frame_ipc_managers(frame_ipc_managers);

    if (!external_recorder_stop_ok) {
        return 1;
    }

    if (thread_failure_state.has_failure()) {
        std::cerr << thread_failure_state.get_first_error() << std::endl;
        return 1;
    }

    return 0;
}

nlohmann::json build_experiment_camera_result(const ExperimentSpec& spec,
                                             const ExperimentRunPlan& run,
                                             const nlohmann::json& snapshot,
                                             const std::string& camera_serial)
{
    nlohmann::json row = nlohmann::json::object();
    row["experiment_id"] = spec.experiment_id;
    row["run_id"] = run.run_id;
    row["camera_serial"] = camera_serial;
    row["codec"] = run.options.encoder_settings.codec;
    row["preset"] = run.options.encoder_settings.preset;
    row["tuning"] = run.options.encoder_settings.tuning;
    row["rate_control_mode"] = run.options.encoder_settings.rate_control_mode;
    row["importance_map_mode"] = run.options.encoder_settings.importance_map.mode;
    row["importance_map_roi_size_px"] = run.options.encoder_settings.importance_map.roi_size_px;
    row["quality_value"] = run.options.encoder_settings.quality_value;
    row["gop_length"] = run.options.encoder_settings.gop_length;
    row["aq_override"] =
        format_headless_toggle_override(run.options.encoder_settings.control_overrides.aq);
    row["temporal_aq_override"] =
        format_headless_toggle_override(run.options.encoder_settings.control_overrides.temporal_aq);
    row["lookahead_override"] =
        format_headless_toggle_override(run.options.encoder_settings.control_overrides.lookahead);
    row["lookahead_depth_override"] =
        run.options.encoder_settings.control_overrides.lookahead_depth;
    row["target_bitrate_bps_override"] =
        run.options.encoder_settings.control_overrides.target_bitrate_bps;
    row["max_bitrate_bps_override"] =
        run.options.encoder_settings.control_overrides.max_bitrate_bps;
    row["vbv_buffer_size_override"] =
        run.options.encoder_settings.control_overrides.vbv_buffer_size;
    row["importance_map_active_mode"] = "off";
    row["importance_map_enabled"] = run.options.encoder_settings.importance_map.mode != "off";
    row["importance_map_block_size"] = 0;
    row["importance_map_grid_width"] = 0;
    row["importance_map_grid_height"] = 0;
    row["nvenc_direct_input"] = run.options.nvenc_direct_input;
    row["duration_s"] = run.duration_s;
    row["warmup_s"] = run.warmup_s;
    row["recording_control_record_for_seconds"] =
        run.options.recording_control.record_for_seconds;
    row["recording_control_clip_seconds"] =
        run.options.recording_control.clip_seconds;
    row["recording_session_manifest_path"] =
        (std::filesystem::path(run.recording_folder) / "recording_session.json").string();
    row["recording_control_video_duration_error_s"] = 0.0;
    row["stream_only"] = run.options.stream_only;
    row["acquisition_buffer_mode"] = run.options.acquisition_buffer_mode_override.empty()
        ? "auto"
        : run.options.acquisition_buffer_mode_override;
    row["recording_sink_mode"] = run.options.recording_sink_mode;
    const nlohmann::json external_recorder_stream =
        run.options.external_recorder_contract.streams.is_object()
            ? run.options.external_recorder_contract.streams.value(
                  camera_serial,
                  nlohmann::json::object())
            : nlohmann::json::object();
    const bool external_recorder_stream_configured =
        run.options.external_recorder_contract.enabled() &&
        external_recorder_stream.is_object() &&
        !external_recorder_stream.empty();
    row["external_recorder_contract_mode"] =
        run.options.external_recorder_contract.mode;
    row["external_recorder_contract_artifact_root"] =
        run.options.external_recorder_contract.artifact_root;
    row["external_recorder_summary_json_path"] =
        external_recorder_stream_configured
            ? external_recorder_stream.value("summary_json", "")
            : "";
    row["external_recorder_video_sanity_json_path"] =
        external_recorder_stream_configured
            ? external_recorder_stream.value("video_sanity_json", "")
            : "";
    row["external_recorder_mp4_path"] =
        external_recorder_stream_configured
            ? external_recorder_stream.value("mp4", "")
            : "";
    row["external_recorder_gop_routing_csv_path"] =
        external_recorder_stream_configured
            ? external_recorder_stream.value("gop_routing_csv", "")
            : "";
    row["external_recorder_routing_policy"] =
        external_recorder_stream_configured
            ? external_recorder_stream.value("routing_policy", "")
            : "";
    row["external_recorder_expected_shard_count"] =
        external_recorder_stream_configured &&
                external_recorder_stream.contains("expected_shard_gpu_ids") &&
                external_recorder_stream["expected_shard_gpu_ids"].is_array()
            ? static_cast<int>(external_recorder_stream["expected_shard_gpu_ids"].size())
            : 0;
    row["frame_ipc_mode"] = headless_frame_ipc_mode_to_string(run.options.frame_ipc.mode);
    row["frame_ipc_status"] = run.options.frame_ipc.enabled() ? "not_reported" : "disabled";
    row["frame_ipc_frames_sent"] = 0ULL;
    row["frame_ipc_reader_popped"] = 0ULL;
    row["frame_ipc_reader_gaps"] = 0ULL;
    row["frame_ipc_push_failures"] = 0ULL;
    row["display"] = false;
    row["yolo"] = run.options.yolo_worker.enabled();
    row["yolo_worker_mode"] = run.options.yolo_worker.mode;
    row["yolo_worker_status"] =
        run.options.yolo_worker.enabled() ? "enabled" : "disabled";
    row["yolo_worker_engine_path"] = run.options.yolo_worker.engine_path;
    row["yolo_worker_decimate"] = run.options.yolo_worker.decimate;
    row["yolo_worker_publish_live_ipc"] =
        run.options.yolo_worker.publish_live_ipc;
    row["yolo_event_log_mode"] = run.options.yolo_event_log.mode;
    row["yolo_event_log_status"] =
        (run.options.yolo_event_log.enabled() || run.options.yolo_worker.enabled())
            ? "not_reported"
            : "disabled";
    row["yolo_event_log_present"] = false;
    row["yolo_event_log_path"] = "";
    row["yolo_event_log_rows"] = 0ULL;
    row["yolo_event_log_detection_rows"] = 0ULL;
    row["yolo_event_log_zero_rows"] = 0ULL;
    row["yolo_event_log_timeout_rows"] = 0ULL;
    row["yolo_event_log_failed_rows"] = 0ULL;
    row["yolo_event_log_parse_errors"] = 0ULL;
    row["yolo_event_log_schema_errors"] = 0ULL;
    row["yolo_event_log_sequence_errors"] = 0ULL;
    row["yolo_event_log_cadence_errors"] = 0ULL;
    row["yolo_event_log_metadata_join_misses"] = 0ULL;
    row["pose"] = run.options.pose_worker.enabled();
    row["pose_worker_mode"] = run.options.pose_worker.mode;
    row["pose_worker_status"] =
        run.options.pose_worker.enabled() ? "configured" : "disabled";
    row["pose_worker_engine_path"] = run.options.pose_worker.engine_path;
    row["pose_worker_skeleton_id"] = run.options.pose_worker.skeleton_id;
    row["pose_worker_skeleton_path"] = run.options.pose_worker.skeleton_path;
    row["pose_worker_input_width"] = run.options.pose_worker.input_width;
    row["pose_worker_input_height"] = run.options.pose_worker.input_height;
    row["pose_worker_input_layout"] = run.options.pose_worker.input_layout;
    row["pose_worker_input_dtype"] = run.options.pose_worker.input_dtype;
    row["pose_worker_normalization"] = run.options.pose_worker.normalization;
    row["pose_worker_roi_source"] = run.options.pose_worker.roi_source;
    row["pose_worker_queue_depth"] = run.options.pose_worker.queue_depth;
    row["pose_worker_timeout_ms"] = run.options.pose_worker.timeout_ms;
    row["pose_worker_prewarm_iterations"] = run.options.pose_worker.prewarm_iterations;
    row["pose_worker_fail_on_init_error"] = run.options.pose_worker.fail_on_init_error;
    row["pose_worker_write_events_jsonl"] = run.options.pose_worker.write_events_jsonl;
    row["pose_event_log_status"] =
        run.options.pose_worker.enabled() ? "not_reported" : "disabled";
    row["pose_event_log_present"] = false;
    row["pose_event_log_path"] = "";
    row["pose_event_log_rows"] = 0ULL;
    row["pose_event_log_no_result_rows"] = 0ULL;
    row["pose_event_log_result_rows"] = 0ULL;
    row["pose_event_log_failed_rows"] = 0ULL;
    row["pose_event_log_parse_errors"] = 0ULL;
    row["pose_event_log_schema_errors"] = 0ULL;
    row["pose_event_log_sequence_errors"] = 0ULL;
    row["pose_event_log_noop_errors"] = 0ULL;
    row["pose_event_log_metadata_join_misses"] = 0ULL;
    row["recording_folder"] = run.recording_folder;
    row["video_present"] = false;
    row["video_path"] = "";
    row["video_file_size_bytes"] = 0ULL;
    row["video_duration_s"] = 0.0;
    row["video_achieved_bitrate_bps"] = 0ULL;
    row["video_content_checked"] = false;
    row["video_content_valid"] = false;
    row["video_content_status"] = "not_checked";
    row["video_first_frame_luma_mean"] = 0.0;
    row["video_first_frame_luma_stddev"] = 0.0;
    row["video_first_frame_black_fraction"] = 0.0;
    row["video_first_frame_decoded_bytes"] = 0ULL;
    row["status"] = "completed";
    row["pass_fail"] = "marginal";
    row["reason"] = "not_evaluated";
    row["gpu_id"] = -1;
    row["gpu_name"] = "";
    row["gpu_pci_bus_id"] = "";
    row["acq_fps_mean"] = 0.0;
    row["acq_fps_p95"] = 0.0;
    row["enc_fps_mean"] = 0.0;
    row["enc_fps_p95"] = 0.0;
    row["enc_fps_primary_mean"] = 0.0;
    row["enc_fps_primary_p95"] = 0.0;
    row["enc_fps_helpers_mean"] = 0.0;
    row["enc_fps_helpers_p95"] = 0.0;
    row["acq_free_entries_min"] = -1;
    row["acq_free_events_min"] = -1;
    row["yolo_events_min"] = -1;
    row["pre_buffers_min"] = -1;
    row["pre_events_min"] = -1;
    row["acq_starve_final"] = 0;
    row["pre_waits_final"] = 0;
    row["pre_drops_final"] = 0;
    row["enc_fail_final"] = 0;
    row["enc_slow_final"] = 0;
    row["external_ipc_frames_acked_final"] = 0ULL;
    row["external_ipc_failures_final"] = 0ULL;
    row["external_ipc_ack_timeouts_final"] = 0ULL;
    row["submitted_frames_final"] = 0ULL;
    row["primary_routed_frames_final"] = 0ULL;
    row["helper_requested_frames_final"] = 0ULL;
    row["helper_fallback_frames_final"] = 0ULL;
    row["helper_dispatched_frames_final"] = 0ULL;
    row["routing_last_target_gpu_id"] = -1;
    row["routing_last_route_mode"] = "";
    row["dropped_frames_camera"] = -1;
    row["camera_frame_id_gaps"] = -1;
    row["get_frame_errors_final"] = 0ULL;
    row["get_frame_error_code_last"] = 0;
    row["get_frame_errors_by_code"] = nlohmann::json::object();
    row["pre_encoder_reference_capture_enabled"] = run.options.pre_encoder_reference_capture.enabled;
    row["pre_encoder_reference_capture_max_frames"] = run.options.pre_encoder_reference_capture.max_frames;
    row["pre_encoder_reference_capture_max_seconds"] = run.options.pre_encoder_reference_capture.max_seconds;
    row["pre_encoder_reference_capture_status"] =
        run.options.pre_encoder_reference_capture.enabled ? "not_reported" : "disabled";
    row["pre_encoder_reference_frames_captured"] = 0ULL;
    row["pre_encoder_reference_bytes_written"] = 0ULL;
    row["pre_encoder_reference_raw_dump_path"] = "";
    row["pre_encoder_reference_index_path"] = "";
    row["pre_encoder_reference_metadata_path"] = "";
    row["pre_encoder_reference_raw_dump_present"] = false;
    row["pre_encoder_reference_index_present"] = false;
    row["pre_encoder_reference_metadata_present"] = false;

    const nlohmann::json pipeline_info = snapshot.value("pipeline_metrics", nlohmann::json::object())
                                          .value(camera_serial, nlohmann::json::object());
    const nlohmann::json encoder_info = snapshot.value("encoders", nlohmann::json::object())
                                          .value(camera_serial, nlohmann::json::object());
    const nlohmann::json camera_runtime_info = snapshot.value("camera_runtime", nlohmann::json::object())
                                                 .value(camera_serial, nlohmann::json::object());
    const nlohmann::json ptp_summary = [&]() {
        nlohmann::json out = nlohmann::json::object();
        std::string error;
        read_json_file(std::filesystem::path(run.recording_folder) / "ptp_sync_summary.json", &out, &error);
        return out;
    }();
    const bool rolling_clip_recording =
        run.options.recording_control.clip_seconds > 0 &&
        run.options.recording_control.record_for_seconds > 0;
    const ExperimentVideoArtifactStats video_stats =
        rolling_clip_recording
            ? summarize_rolling_video_artifacts(run.recording_folder, camera_serial)
            : summarize_video_artifact(run.recording_folder, camera_serial);
    yolo_event_log::SyntheticYoloEventConfig yolo_summary_config =
        run.options.yolo_event_log;
    if (run.options.yolo_worker.enabled() && !yolo_summary_config.enabled()) {
        yolo_summary_config.mode = "real";
    }
    const yolo_event_log::YoloEventLogValidationStats yolo_event_stats =
        yolo_event_log::summarize_yolo_event_log(
            run.recording_folder,
            camera_serial,
            yolo_summary_config);
    if (run.options.yolo_event_log.enabled() || run.options.yolo_worker.enabled()) {
        row["yolo_event_log_status"] = yolo_event_stats.status;
        row["yolo_event_log_present"] = yolo_event_stats.present;
        row["yolo_event_log_path"] = yolo_event_stats.path;
        row["yolo_event_log_rows"] = yolo_event_stats.rows;
        row["yolo_event_log_detection_rows"] = yolo_event_stats.detection_rows;
        row["yolo_event_log_zero_rows"] = yolo_event_stats.zero_rows;
        row["yolo_event_log_timeout_rows"] = yolo_event_stats.timeout_rows;
        row["yolo_event_log_failed_rows"] = yolo_event_stats.failed_rows;
        row["yolo_event_log_parse_errors"] = yolo_event_stats.parse_errors;
        row["yolo_event_log_schema_errors"] = yolo_event_stats.schema_errors;
        row["yolo_event_log_sequence_errors"] = yolo_event_stats.sequence_errors;
        row["yolo_event_log_cadence_errors"] = yolo_event_stats.cadence_errors;
        row["yolo_event_log_metadata_join_misses"] =
            yolo_event_stats.metadata_join_misses;
    }
    pose_event_log::PoseEventLogValidationConfig pose_validation_config;
    pose_validation_config.mode = run.options.pose_worker.mode;
    const pose_event_log::PoseEventLogValidationStats pose_event_stats =
        pose_event_log::summarize_pose_event_log(
            run.recording_folder,
            camera_serial,
            pose_validation_config);
    if (run.options.pose_worker.enabled()) {
        row["pose_event_log_status"] = pose_event_stats.status;
        row["pose_event_log_present"] = pose_event_stats.present;
        row["pose_event_log_path"] = pose_event_stats.path;
        row["pose_event_log_rows"] = pose_event_stats.rows;
        row["pose_event_log_no_result_rows"] = pose_event_stats.no_result_rows;
        row["pose_event_log_result_rows"] = pose_event_stats.result_rows;
        row["pose_event_log_failed_rows"] = pose_event_stats.failed_rows;
        row["pose_event_log_parse_errors"] = pose_event_stats.parse_errors;
        row["pose_event_log_schema_errors"] = pose_event_stats.schema_errors;
        row["pose_event_log_sequence_errors"] = pose_event_stats.sequence_errors;
        row["pose_event_log_noop_errors"] = pose_event_stats.noop_errors;
        row["pose_event_log_metadata_join_misses"] =
            pose_event_stats.metadata_join_misses;
    }
    if (run.options.frame_ipc.enabled()) {
        nlohmann::json frame_ipc_summary;
        std::string frame_ipc_error;
        if (read_json_file(
                std::filesystem::path(run.recording_folder) / "frame_ipc_summary.json",
                &frame_ipc_summary,
                &frame_ipc_error)) {
            const nlohmann::json frame_ipc_camera =
                frame_ipc_summary.value("cameras", nlohmann::json::object())
                                 .value(camera_serial, nlohmann::json::object());
            if (frame_ipc_camera.is_object()) {
                row["frame_ipc_status"] = frame_ipc_camera.value("status", "not_reported");
                row["frame_ipc_frames_sent"] = frame_ipc_camera.value("frames_sent", 0ULL);
                row["frame_ipc_reader_popped"] =
                    frame_ipc_camera.value("reader_messages_popped", 0ULL);
                row["frame_ipc_reader_gaps"] =
                    frame_ipc_camera.value("reader_frame_id_gaps", 0ULL);
                row["frame_ipc_push_failures"] =
                    frame_ipc_camera.value("ipc_push_failures", 0ULL);
            }
        } else {
            row["frame_ipc_status"] = "missing_summary";
        }
    }

    const bool metrics_only_run =
        run.options.stream_only || !is_real_recording_sink_mode(run.options.recording_sink_mode);

    row["video_present"] = metrics_only_run ? false : video_stats.video_present;
    row["video_path"] = metrics_only_run ? "" : video_stats.video_path;
    row["video_file_size_bytes"] = video_stats.file_size_bytes;
    row["video_duration_s"] = video_stats.duration_s;
    row["video_achieved_bitrate_bps"] = video_stats.achieved_bitrate_bps;
    row["video_content_checked"] = metrics_only_run ? false : video_stats.content_checked;
    row["video_content_valid"] = metrics_only_run ? false : video_stats.content_valid;
    row["video_content_status"] = metrics_only_run ? "not_checked" : video_stats.content_status;
    row["video_first_frame_luma_mean"] =
        metrics_only_run ? 0.0 : video_stats.first_frame_luma_mean;
    row["video_first_frame_luma_stddev"] =
        metrics_only_run ? 0.0 : video_stats.first_frame_luma_stddev;
    row["video_first_frame_black_fraction"] =
        metrics_only_run ? 0.0 : video_stats.first_frame_black_fraction;
    row["video_first_frame_decoded_bytes"] =
        metrics_only_run ? 0ULL : video_stats.first_frame_decoded_bytes;

    if (!pipeline_info.is_object()) {
        row["status"] = "failed";
        row["pass_fail"] = "fail";
        row["reason"] = "missing pipeline summary";
        return row;
    }

    row["gpu_id"] = pipeline_info.value("gpu_id", row["gpu_id"]);
    const nlohmann::json gpu = pipeline_info.value("gpu", nlohmann::json::object());
    if (gpu.is_object()) {
        row["gpu_name"] = gpu.value("name", "");
        row["gpu_pci_bus_id"] = gpu.value("pci_bus_id", "");
    }

    const std::string artifact_path = pipeline_info.value("artifact_path", "");
    if (artifact_path.empty()) {
        row["status"] = "failed";
        row["pass_fail"] = "fail";
        row["reason"] = "missing pipeline perf artifact";
        return row;
    }

    const ExperimentCsvWindowStats csv_stats = compute_csv_window_stats(artifact_path, run.warmup_s);
    if (!csv_stats.ok) {
        row["status"] = "failed";
        row["pass_fail"] = "fail";
        row["reason"] = csv_stats.error;
        return row;
    }

    row["acq_fps_mean"] = csv_stats.acq_fps_mean;
    row["acq_fps_p95"] = csv_stats.acq_fps_p95;
    row["enc_fps_mean"] = csv_stats.enc_fps_mean;
    row["enc_fps_p95"] = csv_stats.enc_fps_p95;
    row["enc_fps_primary_mean"] = csv_stats.enc_fps_primary_mean;
    row["enc_fps_primary_p95"] = csv_stats.enc_fps_primary_p95;
    row["enc_fps_helpers_mean"] = csv_stats.enc_fps_helpers_mean;
    row["enc_fps_helpers_p95"] = csv_stats.enc_fps_helpers_p95;
    row["acq_free_entries_min"] = csv_stats.acq_free_entries_min;
    row["acq_free_events_min"] = csv_stats.acq_free_events_min;
    row["yolo_events_min"] = csv_stats.yolo_events_min;
    row["pre_buffers_min"] = csv_stats.pre_buffers_min;
    row["pre_events_min"] = csv_stats.pre_events_min;
    row["acq_starve_final"] = csv_stats.acq_starve_delta;
    row["pre_waits_final"] = csv_stats.pre_waits_delta;
    row["pre_drops_final"] = csv_stats.pre_drops_delta;
    row["enc_fail_final"] = csv_stats.enc_fail_delta;
    row["enc_slow_final"] = csv_stats.enc_slow_delta;
    row["external_ipc_frames_acked_final"] = csv_stats.external_ipc_frames_acked_delta;
    row["external_ipc_failures_final"] = csv_stats.external_ipc_failures_delta;
    row["external_ipc_ack_timeouts_final"] = csv_stats.external_ipc_ack_timeouts_delta;
    row["submitted_frames_final"] = csv_stats.submitted_frames_delta;
    row["primary_routed_frames_final"] = csv_stats.primary_routed_frames_delta;
    row["helper_requested_frames_final"] = csv_stats.helper_requested_frames_delta;
    row["helper_fallback_frames_final"] = csv_stats.helper_fallback_frames_delta;
    row["helper_dispatched_frames_final"] = csv_stats.helper_dispatched_frames_delta;
    row["dropped_frames_camera"] = static_cast<int64_t>(csv_stats.camera_dropped_frames_delta);
    row["camera_frame_id_gaps"] = static_cast<int64_t>(csv_stats.camera_dropped_frames_delta);
    row["get_frame_errors_final"] = csv_stats.get_frame_errors_delta;
    row["get_frame_error_code_last"] = csv_stats.last_get_frame_error_code;
    const nlohmann::json routing_info = pipeline_info.value("routing", nlohmann::json::object());
    if (routing_info.is_object()) {
        row["routing_last_target_gpu_id"] = routing_info.value("last_target_gpu_id", -1);
        row["routing_last_route_mode"] = routing_info.value("last_route_mode", "");
    }

    auto merge_camera_dropped_frames = [&](const nlohmann::json& value) {
        if (!value.is_number_integer() && !value.is_number_unsigned()) {
            return;
        }
        const int64_t candidate = value.get<int64_t>();
        const int64_t current = row["dropped_frames_camera"].get<int64_t>();
        if (candidate >= 0 && (current < 0 || candidate > current)) {
            row["dropped_frames_camera"] = candidate;
            row["camera_frame_id_gaps"] = candidate;
        }
    };

    const nlohmann::json pipeline_totals = pipeline_info.value("totals", nlohmann::json::object());
    if (pipeline_totals.is_object()) {
        if (pipeline_totals.contains("camera_frame_id_gaps")) {
            merge_camera_dropped_frames(pipeline_totals["camera_frame_id_gaps"]);
        } else if (pipeline_totals.contains("camera_dropped_frames")) {
            merge_camera_dropped_frames(pipeline_totals["camera_dropped_frames"]);
        }
        if (pipeline_totals.contains("get_frame_errors") &&
            (pipeline_totals["get_frame_errors"].is_number_integer() ||
             pipeline_totals["get_frame_errors"].is_number_unsigned())) {
            row["get_frame_errors_final"] = pipeline_totals["get_frame_errors"].get<uint64_t>();
        }
        if (pipeline_totals.contains("last_get_frame_error_code") &&
            pipeline_totals["last_get_frame_error_code"].is_number_integer()) {
            row["get_frame_error_code_last"] = pipeline_totals["last_get_frame_error_code"].get<int>();
        }
        if (pipeline_totals.contains("get_frame_errors_by_code") &&
            pipeline_totals["get_frame_errors_by_code"].is_object()) {
            row["get_frame_errors_by_code"] = pipeline_totals["get_frame_errors_by_code"];
        }
    }

    if (ptp_summary.is_object()) {
        const nlohmann::json cameras = ptp_summary.value("cameras", nlohmann::json::object());
        if (cameras.is_object()) {
            const nlohmann::json camera_summary = cameras.value(camera_serial, nlohmann::json::object());
            if (camera_summary.is_object()) {
                if (camera_summary.contains("camera_frame_id_gaps")) {
                    merge_camera_dropped_frames(camera_summary["camera_frame_id_gaps"]);
                } else if (camera_summary.contains("dropped_frames")) {
                    merge_camera_dropped_frames(camera_summary["dropped_frames"]);
                }
            }
        }
    }

    double target_fps = 0.0;
    if (camera_runtime_info.is_object()) {
        const nlohmann::json runtime_info = camera_runtime_info.value("runtime", nlohmann::json::object());
        if (runtime_info.is_object()) {
            target_fps = runtime_info.value("frame_rate", 0.0);
        }
    }
    const int timed_record_for_seconds =
        run.options.recording_control.record_for_seconds;
    const bool timed_recording =
        timed_record_for_seconds > 0 && !run.options.stream_only;
    if (timed_recording) {
        row["recording_control_video_duration_error_s"] =
            video_stats.duration_s - static_cast<double>(timed_record_for_seconds);
    }

    if (metrics_only_run) {
        const uint64_t acq_starve = row["acq_starve_final"].get<uint64_t>();
        const uint64_t pre_drops = row["pre_drops_final"].get<uint64_t>();
        const uint64_t external_ipc_frames_acked =
            row["external_ipc_frames_acked_final"].get<uint64_t>();
        const uint64_t external_ipc_failures =
            row["external_ipc_failures_final"].get<uint64_t>();
        const uint64_t external_ipc_ack_timeouts =
            row["external_ipc_ack_timeouts_final"].get<uint64_t>();
        const uint64_t submitted_frames = row["submitted_frames_final"].get<uint64_t>();
        const int64_t camera_drops = row["dropped_frames_camera"].get<int64_t>();
        const double acq_fps_mean = row["acq_fps_mean"].get<double>();
        const double tolerance = target_fps * (spec.target_fps_tolerance_pct / 100.0);
        const bool fps_ok = (target_fps <= 0.0) || (acq_fps_mean + tolerance >= target_fps);
        const bool external_ipc_ok = run.options.recording_sink_mode != "external_ipc" ||
            (external_ipc_failures == 0 &&
             external_ipc_ack_timeouts == 0 &&
             (submitted_frames == 0 || external_ipc_frames_acked >= submitted_frames));

        if ((spec.require_zero_camera_drops && camera_drops > 0) ||
            (spec.require_zero_acq_starve && acq_starve > 0) ||
            (spec.require_zero_pre_drops && pre_drops > 0) ||
            !external_ipc_ok) {
            row["pass_fail"] = "fail";
            if (spec.require_zero_camera_drops && camera_drops > 0) {
                row["reason"] = "nonzero camera dropped frames";
            } else if (spec.require_zero_acq_starve && acq_starve > 0) {
                row["reason"] = "nonzero acquisition starvation";
            } else if (spec.require_zero_pre_drops && pre_drops > 0) {
                row["reason"] = "nonzero preprocess drops";
            } else if (external_ipc_failures > 0) {
                row["reason"] = "nonzero external ipc failures";
            } else if (external_ipc_ack_timeouts > 0) {
                row["reason"] = "nonzero external ipc ack timeouts";
            } else {
                row["reason"] = "external ipc acked fewer frames than submitted";
            }
        } else if (!fps_ok) {
            row["pass_fail"] = "marginal";
            row["reason"] = "acquisition fps below target tolerance";
        } else {
            row["pass_fail"] = "pass";
            row["reason"] = run.options.stream_only
                ? "meets current stream-only policy"
                : "meets current recording sink diagnostic policy";
        }
    } else if (encoder_info.is_object()) {
        row["nvenc_direct_input"] = encoder_info.value("path", "") == "hw_direct_input";
        if (row["gpu_id"].get<int>() < 0) {
            row["gpu_id"] = encoder_info.value("gpu_id", -1);
        }
        if (row["gpu_name"].get<std::string>().empty()) {
            const nlohmann::json gpu = encoder_info.value("gpu", nlohmann::json::object());
            if (gpu.is_object()) {
                row["gpu_name"] = gpu.value("name", "");
                row["gpu_pci_bus_id"] = gpu.value("pci_bus_id", "");
            }
        }
        const nlohmann::json preenc_capture =
            encoder_info.value("pre_encoder_reference_capture", nlohmann::json::object());
        const nlohmann::json importance_map =
            encoder_info.value("importance_map", nlohmann::json::object());
        if (importance_map.is_object()) {
            row["importance_map_active_mode"] =
                importance_map.value("active_mode", row["importance_map_active_mode"]);
            row["importance_map_enabled"] =
                importance_map.value("enabled", row["importance_map_enabled"]);
            row["importance_map_block_size"] =
                importance_map.value("block_size", row["importance_map_block_size"]);
            row["importance_map_grid_width"] =
                importance_map.value("grid_width", row["importance_map_grid_width"]);
            row["importance_map_grid_height"] =
                importance_map.value("grid_height", row["importance_map_grid_height"]);
            row["importance_map_roi_size_px"] =
                importance_map.value("roi_size_px", row["importance_map_roi_size_px"]);
        }
        if (preenc_capture.is_object()) {
            row["pre_encoder_reference_capture_enabled"] =
                preenc_capture.value("enabled", false);
            row["pre_encoder_reference_capture_max_frames"] =
                preenc_capture.value("max_frames", 0);
            row["pre_encoder_reference_capture_max_seconds"] =
                preenc_capture.value("max_seconds", 0);
            row["pre_encoder_reference_capture_status"] =
                preenc_capture.value("status", "disabled");
            row["pre_encoder_reference_frames_captured"] =
                preenc_capture.value("frames_captured", 0ULL);
            row["pre_encoder_reference_bytes_written"] =
                preenc_capture.value("bytes_written", 0ULL);
            const nlohmann::json artifacts =
                preenc_capture.value("artifacts", nlohmann::json::object());
            if (artifacts.is_object()) {
                const std::string raw_dump_path = artifacts.value("raw_dump", "");
                const std::string index_path = artifacts.value("index", "");
                const std::string metadata_path = artifacts.value("metadata", "");
                row["pre_encoder_reference_raw_dump_path"] = raw_dump_path;
                row["pre_encoder_reference_index_path"] = index_path;
                row["pre_encoder_reference_metadata_path"] = metadata_path;
                row["pre_encoder_reference_raw_dump_present"] =
                    !raw_dump_path.empty() && std::filesystem::exists(raw_dump_path);
                row["pre_encoder_reference_index_present"] =
                    !index_path.empty() && std::filesystem::exists(index_path);
                row["pre_encoder_reference_metadata_present"] =
                    !metadata_path.empty() && std::filesystem::exists(metadata_path);
            }
        }
        if (target_fps <= 0.0) {
            target_fps = static_cast<double>(encoder_info.value("fps", 0));
        }
        const uint64_t acq_starve = row["acq_starve_final"].get<uint64_t>();
        const uint64_t pre_drops = row["pre_drops_final"].get<uint64_t>();
        const uint64_t enc_fail = row["enc_fail_final"].get<uint64_t>();
        const int64_t camera_drops = row["dropped_frames_camera"].get<int64_t>();
        const double acq_fps_mean = row["acq_fps_mean"].get<double>();
        const double enc_fps_mean = row["enc_fps_mean"].get<double>();
        const double tolerance = target_fps * (spec.target_fps_tolerance_pct / 100.0);
        const bool acq_fps_ok = (target_fps <= 0.0) || (acq_fps_mean + tolerance >= target_fps);
        const bool fps_ok = (target_fps <= 0.0) || (enc_fps_mean + tolerance >= target_fps);
        const bool importance_map_requested = row["importance_map_mode"].get<std::string>() != "off";
        const bool importance_map_active =
            row["importance_map_active_mode"].get<std::string>() == row["importance_map_mode"].get<std::string>();
        const bool preenc_enabled = row["pre_encoder_reference_capture_enabled"].get<bool>();
        const std::string preenc_status = row["pre_encoder_reference_capture_status"].get<std::string>();
        const uint64_t preenc_frames = row["pre_encoder_reference_frames_captured"].get<uint64_t>();
        const bool preenc_artifacts_present =
            row["pre_encoder_reference_raw_dump_present"].get<bool>() &&
            row["pre_encoder_reference_index_present"].get<bool>() &&
            row["pre_encoder_reference_metadata_present"].get<bool>();
        const bool video_content_policy_failed =
            spec.require_valid_video_content &&
            (!row["video_present"].get<bool>() || !row["video_content_valid"].get<bool>());
        const double video_duration_tolerance_s =
            timed_recording
                ? std::max(1.0, static_cast<double>(timed_record_for_seconds) * 0.25)
                : 0.0;
        const bool video_duration_policy_failed =
            timed_recording &&
            (video_stats.duration_s <= 0.0 ||
             std::abs(video_stats.duration_s - static_cast<double>(timed_record_for_seconds)) >
                 video_duration_tolerance_s);
        const bool counter_policy_failed =
            (spec.require_zero_camera_drops && camera_drops > 0) ||
            (spec.require_zero_acq_starve && acq_starve > 0) ||
            (spec.require_zero_pre_drops && pre_drops > 0) ||
            (spec.require_zero_enc_fail && enc_fail > 0);
        auto set_counter_policy_failure = [&]() {
            row["pass_fail"] = "fail";
            if (spec.require_zero_camera_drops && camera_drops > 0) {
                row["reason"] = "nonzero camera dropped frames";
            } else if (spec.require_zero_acq_starve && acq_starve > 0) {
                row["reason"] = "nonzero acquisition starvation";
            } else if (spec.require_zero_pre_drops && pre_drops > 0) {
                row["reason"] = "nonzero preprocess drops";
            } else {
                row["reason"] = "nonzero encode failures";
            }
        };

        if (importance_map_requested && !importance_map_active) {
            row["pass_fail"] = "fail";
            row["reason"] = "importance map inactive";
        } else if (video_content_policy_failed) {
            row["pass_fail"] = "fail";
            if (!row["video_present"].get<bool>()) {
                row["reason"] = "missing full-frame video";
            } else {
                row["reason"] = std::string("invalid video content: ") +
                    row["video_content_status"].get<std::string>();
            }
        } else if (video_duration_policy_failed) {
            row["pass_fail"] = "fail";
            row["reason"] = "timed recording video duration outside tolerance";
        } else if (preenc_enabled) {
            if (preenc_status == "error") {
                row["pass_fail"] = "fail";
                row["reason"] = "pre-encoder reference capture error";
            } else if (!preenc_artifacts_present) {
                row["pass_fail"] = "fail";
                row["reason"] = "missing pre-encoder reference artifacts";
            } else if (preenc_frames == 0) {
                row["pass_fail"] = "fail";
                row["reason"] = "pre-encoder reference captured zero frames";
            } else if (preenc_status != "completed" && preenc_status != "budget_reached") {
                row["pass_fail"] = "fail";
                row["reason"] = "pre-encoder reference capture incomplete";
            } else if (counter_policy_failed) {
                set_counter_policy_failure();
            } else if (timed_recording && !acq_fps_ok) {
                row["pass_fail"] = "marginal";
                row["reason"] = "acquisition fps below target tolerance";
            } else if (!timed_recording && !fps_ok) {
                row["pass_fail"] = "marginal";
                row["reason"] = "encode fps below target tolerance";
            } else {
                row["pass_fail"] = "pass";
                row["reason"] = timed_recording
                    ? "meets current timed-recording policy"
                    : "meets current policy";
            }
        } else {
            if (counter_policy_failed) {
                set_counter_policy_failure();
            } else if (timed_recording && !acq_fps_ok) {
                row["pass_fail"] = "marginal";
                row["reason"] = "acquisition fps below target tolerance";
            } else if (!timed_recording && !fps_ok) {
                row["pass_fail"] = "marginal";
                row["reason"] = "encode fps below target tolerance";
            } else {
                row["pass_fail"] = "pass";
                row["reason"] = timed_recording
                    ? "meets current timed-recording policy"
                    : "meets current policy";
            }
        }
    } else {
        row["status"] = "failed";
        row["pass_fail"] = "fail";
        row["reason"] = "missing encoder snapshot";
    }

    if (run.options.frame_ipc.enabled()) {
        const std::string frame_ipc_status = row.value("frame_ipc_status", "not_reported");
        if (frame_ipc_status != "pass" && frame_ipc_status != "disabled") {
            row["pass_fail"] = "fail";
            row["reason"] = "frame IPC verification failed";
        }
    }
    if (run.options.yolo_event_log.enabled() || run.options.yolo_worker.enabled()) {
        const std::string yolo_event_status =
            row.value("yolo_event_log_status", "not_reported");
        if (yolo_event_status != "pass") {
            row["pass_fail"] = "fail";
            row["reason"] = "YOLO event log validation failed";
        }
    }
    if (run.options.pose_worker.enabled()) {
        const std::string pose_event_status =
            row.value("pose_event_log_status", "not_reported");
        if (pose_event_status != "pass") {
            row["pass_fail"] = "fail";
            row["reason"] = "pose event log validation failed";
        }
    }

    return row;
}

bool write_experiment_manifests(const ExperimentSpec& spec,
                                const std::filesystem::path& experiment_root,
                                const nlohmann::json& runs_json,
                                const nlohmann::json& summary_json,
                                std::string* error_out)
{
    if (!write_json_file(experiment_root / "runs.json", runs_json, error_out)) {
        return false;
    }
    if (!write_json_file(experiment_root / "summary.json", summary_json, error_out)) {
        return false;
    }

    orange::ScopedFsuid fsuid_guard;
    (void)fsuid_guard;
    std::ofstream csv(experiment_root / "runs.csv");
    if (!csv) {
        if (error_out) {
            *error_out = "Failed to open runs.csv for writing";
        }
        return false;
    }
    csv << "experiment_id,run_id,camera_serial,gpu_id,gpu_name,gpu_pci_bus_id,codec,preset,tuning,rate_control_mode,importance_map_mode,importance_map_roi_size_px,quality_value,gop_length,aq_override,temporal_aq_override,lookahead_override,lookahead_depth_override,target_bitrate_bps_override,max_bitrate_bps_override,vbv_buffer_size_override,importance_map_enabled,importance_map_active_mode,importance_map_block_size,importance_map_grid_width,importance_map_grid_height,stream_only,acquisition_buffer_mode,recording_sink_mode,external_recorder_contract_mode,external_recorder_contract_artifact_root,external_recorder_summary_json_path,external_recorder_video_sanity_json_path,external_recorder_mp4_path,external_recorder_gop_routing_csv_path,external_recorder_routing_policy,external_recorder_expected_shard_count,frame_ipc_mode,frame_ipc_status,frame_ipc_frames_sent,frame_ipc_reader_popped,frame_ipc_reader_gaps,frame_ipc_push_failures,nvenc_direct_input,duration_s,warmup_s,recording_control_record_for_seconds,recording_control_clip_seconds,recording_session_manifest_path,recording_control_video_duration_error_s,display,yolo,yolo_worker_mode,yolo_worker_status,yolo_worker_engine_path,yolo_worker_decimate,yolo_worker_publish_live_ipc,yolo_event_log_mode,yolo_event_log_status,yolo_event_log_present,yolo_event_log_rows,yolo_event_log_detection_rows,yolo_event_log_zero_rows,yolo_event_log_timeout_rows,yolo_event_log_failed_rows,yolo_event_log_parse_errors,yolo_event_log_schema_errors,yolo_event_log_sequence_errors,yolo_event_log_cadence_errors,yolo_event_log_metadata_join_misses,yolo_event_log_path,pose,pose_worker_mode,pose_worker_status,pose_worker_engine_path,pose_worker_skeleton_id,pose_worker_skeleton_path,pose_worker_input_width,pose_worker_input_height,pose_worker_input_layout,pose_worker_input_dtype,pose_worker_normalization,pose_worker_roi_source,pose_worker_queue_depth,pose_worker_timeout_ms,pose_worker_prewarm_iterations,pose_worker_fail_on_init_error,pose_worker_write_events_jsonl,pose_event_log_mode,pose_event_log_status,pose_event_log_present,pose_event_log_rows,pose_event_log_no_result_rows,pose_event_log_result_rows,pose_event_log_failed_rows,pose_event_log_parse_errors,pose_event_log_schema_errors,pose_event_log_sequence_errors,pose_event_log_noop_errors,pose_event_log_metadata_join_misses,pose_event_log_path,recording_folder,video_present,video_path,video_file_size_bytes,video_duration_s,video_achieved_bitrate_bps,video_content_checked,video_content_valid,video_content_status,video_first_frame_luma_mean,video_first_frame_luma_stddev,video_first_frame_black_fraction,video_first_frame_decoded_bytes,status,pass_fail,reason,acq_fps_mean,acq_fps_p95,enc_fps_mean,enc_fps_p95,enc_fps_primary_mean,enc_fps_primary_p95,enc_fps_helpers_mean,enc_fps_helpers_p95,acq_free_entries_min,acq_free_events_min,yolo_events_min,pre_buffers_min,pre_events_min,acq_starve_final,pre_waits_final,pre_drops_final,enc_fail_final,enc_slow_final,external_ipc_frames_acked_final,external_ipc_failures_final,external_ipc_ack_timeouts_final,submitted_frames_final,primary_routed_frames_final,helper_requested_frames_final,helper_fallback_frames_final,helper_dispatched_frames_final,routing_last_target_gpu_id,routing_last_route_mode,dropped_frames_camera,camera_frame_id_gaps,get_frame_errors_final,get_frame_error_code_last,pre_encoder_reference_capture_enabled,pre_encoder_reference_capture_max_frames,pre_encoder_reference_capture_max_seconds,pre_encoder_reference_capture_status,pre_encoder_reference_frames_captured,pre_encoder_reference_bytes_written,pre_encoder_reference_raw_dump_present,pre_encoder_reference_index_present,pre_encoder_reference_metadata_present,pre_encoder_reference_raw_dump_path,pre_encoder_reference_index_path,pre_encoder_reference_metadata_path\n";
    for (const auto& run_entry : runs_json.value("runs", nlohmann::json::array())) {
        const nlohmann::json cameras = run_entry.value("camera_results", nlohmann::json::array());
        for (const auto& row : cameras) {
            csv << row.value("experiment_id", "") << ","
                << row.value("run_id", "") << ","
                << row.value("camera_serial", "") << ","
                << row.value("gpu_id", -1) << ","
                << "\"" << row.value("gpu_name", "") << "\","
                << "\"" << row.value("gpu_pci_bus_id", "") << "\","
                << row.value("codec", "") << ","
                << row.value("preset", "") << ","
                << row.value("tuning", "") << ","
                << row.value("rate_control_mode", "") << ","
                << row.value("importance_map_mode", "") << ","
                << row.value("importance_map_roi_size_px", ImportanceMapConfig::kDefaultRoiSizePx) << ","
                << row.value("quality_value", 0) << ","
                << row.value("gop_length", 0) << ","
                << row.value("aq_override", "") << ","
                << row.value("temporal_aq_override", "") << ","
                << row.value("lookahead_override", "") << ","
                << row.value("lookahead_depth_override", -1) << ","
                << row.value("target_bitrate_bps_override", -1) << ","
                << row.value("max_bitrate_bps_override", -1) << ","
                << row.value("vbv_buffer_size_override", -1) << ","
                << (row.value("importance_map_enabled", false) ? "true" : "false") << ","
                << row.value("importance_map_active_mode", "") << ","
                << row.value("importance_map_block_size", 0) << ","
                << row.value("importance_map_grid_width", 0) << ","
                << row.value("importance_map_grid_height", 0) << ","
                << (row.value("stream_only", false) ? "true" : "false") << ","
                << row.value("acquisition_buffer_mode", "auto") << ","
                << row.value("recording_sink_mode", "real") << ","
                << row.value("external_recorder_contract_mode", "off") << ","
                << "\"" << row.value("external_recorder_contract_artifact_root", "") << "\","
                << "\"" << row.value("external_recorder_summary_json_path", "") << "\","
                << "\"" << row.value("external_recorder_video_sanity_json_path", "") << "\","
                << "\"" << row.value("external_recorder_mp4_path", "") << "\","
                << "\"" << row.value("external_recorder_gop_routing_csv_path", "") << "\","
                << row.value("external_recorder_routing_policy", "") << ","
                << row.value("external_recorder_expected_shard_count", 0) << ","
                << row.value("frame_ipc_mode", "off") << ","
                << row.value("frame_ipc_status", "disabled") << ","
                << row.value("frame_ipc_frames_sent", 0ULL) << ","
                << row.value("frame_ipc_reader_popped", 0ULL) << ","
                << row.value("frame_ipc_reader_gaps", 0ULL) << ","
                << row.value("frame_ipc_push_failures", 0ULL) << ","
                << (row.value("nvenc_direct_input", false) ? "true" : "false") << ","
                << row.value("duration_s", 0) << ","
                << row.value("warmup_s", 0) << ","
                << row.value("recording_control_record_for_seconds", 0) << ","
                << row.value("recording_control_clip_seconds", 0) << ","
                << "\"" << row.value("recording_session_manifest_path", "") << "\","
                << row.value("recording_control_video_duration_error_s", 0.0) << ","
                << (row.value("display", false) ? "true" : "false") << ","
                << (row.value("yolo", false) ? "true" : "false") << ","
                << row.value("yolo_worker_mode", "off") << ","
                << row.value("yolo_worker_status", "disabled") << ","
                << "\"" << row.value("yolo_worker_engine_path", "") << "\","
                << row.value("yolo_worker_decimate", 1) << ","
                << (row.value("yolo_worker_publish_live_ipc", false) ? "true" : "false") << ","
                << row.value("yolo_event_log_mode", "off") << ","
                << row.value("yolo_event_log_status", "disabled") << ","
                << (row.value("yolo_event_log_present", false) ? "true" : "false") << ","
                << row.value("yolo_event_log_rows", 0ULL) << ","
                << row.value("yolo_event_log_detection_rows", 0ULL) << ","
                << row.value("yolo_event_log_zero_rows", 0ULL) << ","
                << row.value("yolo_event_log_timeout_rows", 0ULL) << ","
                << row.value("yolo_event_log_failed_rows", 0ULL) << ","
                << row.value("yolo_event_log_parse_errors", 0ULL) << ","
                << row.value("yolo_event_log_schema_errors", 0ULL) << ","
                << row.value("yolo_event_log_sequence_errors", 0ULL) << ","
                << row.value("yolo_event_log_cadence_errors", 0ULL) << ","
                << row.value("yolo_event_log_metadata_join_misses", 0ULL) << ","
                << "\"" << row.value("yolo_event_log_path", "") << "\","
                << (row.value("pose", false) ? "true" : "false") << ","
                << row.value("pose_worker_mode", "off") << ","
                << row.value("pose_worker_status", "disabled") << ","
                << "\"" << row.value("pose_worker_engine_path", "") << "\","
                << row.value("pose_worker_skeleton_id", "unknown") << ","
                << "\"" << row.value("pose_worker_skeleton_path", "") << "\","
                << row.value("pose_worker_input_width", 256) << ","
                << row.value("pose_worker_input_height", 256) << ","
                << row.value("pose_worker_input_layout", "nchw") << ","
                << row.value("pose_worker_input_dtype", "fp16") << ","
                << row.value("pose_worker_normalization", "model_default") << ","
                << row.value("pose_worker_roi_source", "yolo_top_detection") << ","
                << row.value("pose_worker_queue_depth", 32) << ","
                << row.value("pose_worker_timeout_ms", 500) << ","
                << row.value("pose_worker_prewarm_iterations", 0) << ","
                << (row.value("pose_worker_fail_on_init_error", true) ? "true" : "false") << ","
                << (row.value("pose_worker_write_events_jsonl", true) ? "true" : "false") << ","
                << row.value("pose_worker_mode", "off") << ","
                << row.value("pose_event_log_status", "disabled") << ","
                << (row.value("pose_event_log_present", false) ? "true" : "false") << ","
                << row.value("pose_event_log_rows", 0ULL) << ","
                << row.value("pose_event_log_no_result_rows", 0ULL) << ","
                << row.value("pose_event_log_result_rows", 0ULL) << ","
                << row.value("pose_event_log_failed_rows", 0ULL) << ","
                << row.value("pose_event_log_parse_errors", 0ULL) << ","
                << row.value("pose_event_log_schema_errors", 0ULL) << ","
                << row.value("pose_event_log_sequence_errors", 0ULL) << ","
                << row.value("pose_event_log_noop_errors", 0ULL) << ","
                << row.value("pose_event_log_metadata_join_misses", 0ULL) << ","
                << "\"" << row.value("pose_event_log_path", "") << "\","
                << "\"" << row.value("recording_folder", "") << "\","
                << (row.value("video_present", false) ? "true" : "false") << ","
                << "\"" << row.value("video_path", "") << "\","
                << row.value("video_file_size_bytes", 0ULL) << ","
                << row.value("video_duration_s", 0.0) << ","
                << row.value("video_achieved_bitrate_bps", 0ULL) << ","
                << (row.value("video_content_checked", false) ? "true" : "false") << ","
                << (row.value("video_content_valid", false) ? "true" : "false") << ","
                << row.value("video_content_status", "not_checked") << ","
                << row.value("video_first_frame_luma_mean", 0.0) << ","
                << row.value("video_first_frame_luma_stddev", 0.0) << ","
                << row.value("video_first_frame_black_fraction", 0.0) << ","
                << row.value("video_first_frame_decoded_bytes", 0ULL) << ","
                << row.value("status", "") << ","
                << row.value("pass_fail", "") << ","
                << "\"" << row.value("reason", "") << "\","
                << row.value("acq_fps_mean", 0.0) << ","
                << row.value("acq_fps_p95", 0.0) << ","
                << row.value("enc_fps_mean", 0.0) << ","
                << row.value("enc_fps_p95", 0.0) << ","
                << row.value("enc_fps_primary_mean", 0.0) << ","
                << row.value("enc_fps_primary_p95", 0.0) << ","
                << row.value("enc_fps_helpers_mean", 0.0) << ","
                << row.value("enc_fps_helpers_p95", 0.0) << ","
                << row.value("acq_free_entries_min", -1) << ","
                << row.value("acq_free_events_min", -1) << ","
                << row.value("yolo_events_min", -1) << ","
                << row.value("pre_buffers_min", -1) << ","
                << row.value("pre_events_min", -1) << ","
                << row.value("acq_starve_final", 0ULL) << ","
                << row.value("pre_waits_final", 0ULL) << ","
                << row.value("pre_drops_final", 0ULL) << ","
                << row.value("enc_fail_final", 0ULL) << ","
                << row.value("enc_slow_final", 0ULL) << ","
                << row.value("external_ipc_frames_acked_final", 0ULL) << ","
                << row.value("external_ipc_failures_final", 0ULL) << ","
                << row.value("external_ipc_ack_timeouts_final", 0ULL) << ","
                << row.value("submitted_frames_final", 0ULL) << ","
                << row.value("primary_routed_frames_final", 0ULL) << ","
                << row.value("helper_requested_frames_final", 0ULL) << ","
                << row.value("helper_fallback_frames_final", 0ULL) << ","
                << row.value("helper_dispatched_frames_final", 0ULL) << ","
                << row.value("routing_last_target_gpu_id", -1) << ","
                << "\"" << row.value("routing_last_route_mode", "") << "\","
                << row.value("dropped_frames_camera", -1) << ","
                << row.value("camera_frame_id_gaps", -1) << ","
                << row.value("get_frame_errors_final", 0ULL) << ","
                << row.value("get_frame_error_code_last", 0) << ","
                << (row.value("pre_encoder_reference_capture_enabled", false) ? "true" : "false") << ","
                << row.value("pre_encoder_reference_capture_max_frames", 0) << ","
                << row.value("pre_encoder_reference_capture_max_seconds", 0) << ","
                << row.value("pre_encoder_reference_capture_status", "") << ","
                << row.value("pre_encoder_reference_frames_captured", 0ULL) << ","
                << row.value("pre_encoder_reference_bytes_written", 0ULL) << ","
                << (row.value("pre_encoder_reference_raw_dump_present", false) ? "true" : "false") << ","
                << (row.value("pre_encoder_reference_index_present", false) ? "true" : "false") << ","
                << (row.value("pre_encoder_reference_metadata_present", false) ? "true" : "false") << ","
                << "\"" << row.value("pre_encoder_reference_raw_dump_path", "") << "\","
                << "\"" << row.value("pre_encoder_reference_index_path", "") << "\","
                << "\"" << row.value("pre_encoder_reference_metadata_path", "") << "\"\n";
        }
    }
    return true;
}

int run_local_experiment(const HeadlessCliOptions& options)
{
    ExperimentSpec spec;
    std::string error;
    if (!load_experiment_spec(options, &spec, &error)) {
        std::cerr << error << std::endl;
        return 2;
    }

    const std::filesystem::path experiment_root =
        std::filesystem::path(spec.output_root) / spec.experiment_id;
    std::error_code create_error;
    {
        orange::ScopedFsuid fsuid_guard;
        (void)fsuid_guard;
        std::filesystem::create_directories(experiment_root, create_error);
    }
    if (create_error && !std::filesystem::exists(experiment_root)) {
        std::cerr << "Failed to create experiment root " << experiment_root
                  << ": " << create_error.message() << std::endl;
        return 1;
    }

    if (!write_json_file(experiment_root / "experiment_spec.json", spec.source_json, &error)) {
        std::cerr << error << std::endl;
        return 1;
    }

    if (spec.helper_noop_source_read) {
        setenv("ORANGE_PREPROCESS_HELPER_NOOP_SOURCE_READ", "1", 1);
        std::cout << "[EXPERIMENT] helper source-read noop enabled via spec"
                  << std::endl;
    }
    if (spec.helper_copy_bytes >= 0) {
        const std::string helper_copy_bytes =
            std::to_string(spec.helper_copy_bytes);
        setenv("ORANGE_PREPROCESS_HELPER_COPY_BYTES",
               helper_copy_bytes.c_str(),
               1);
        std::cout << "[EXPERIMENT] helper peer-copy byte limit enabled via spec"
                  << " bytes=" << spec.helper_copy_bytes << std::endl;
    }
    if (spec.helper_copy_delay_ns > 0) {
        const std::string helper_copy_delay_ns =
            std::to_string(spec.helper_copy_delay_ns);
        setenv("ORANGE_PREPROCESS_HELPER_COPY_DELAY_NS",
               helper_copy_delay_ns.c_str(),
               1);
        std::cout << "[EXPERIMENT] helper peer-copy delay enabled via spec"
                  << " delay_ns=" << spec.helper_copy_delay_ns << std::endl;
    }
    if (spec.ptp_register_read_decimate > 1) {
        const std::string ptp_decimate =
            std::to_string(spec.ptp_register_read_decimate);
        setenv("ORANGE_PTP_REGISTER_READ_DECIMATE",
               ptp_decimate.c_str(),
               1);
        std::cout << "[EXPERIMENT] PTP register reads decimated via spec"
                  << " decimate=" << spec.ptp_register_read_decimate
                  << std::endl;
    }

    const std::vector<ExperimentRunPlan> runs = build_experiment_run_plans(spec);
    if (runs.empty()) {
        std::cerr << "Experiment spec produced zero runs." << std::endl;
        return 1;
    }

    nlohmann::json runs_json = {
        {"experiment_id", spec.experiment_id},
        {"notes", spec.notes},
        {"created_at_utc", get_current_utc_timestamp()},
        {"runs", nlohmann::json::array()}
    };

    std::cout << "[EXPERIMENT] " << spec.experiment_id
              << " runs=" << runs.size()
              << " output_root=" << experiment_root.string()
              << std::endl;

    int run_failures = 0;
    for (const ExperimentRunPlan& run : runs) {
        if (quit_server) {
            break;
        }
        const std::filesystem::path run_path(run.recording_folder);
        if (std::filesystem::exists(run_path) && !std::filesystem::is_empty(run_path)) {
            std::cerr << "[EXPERIMENT] Refusing to reuse non-empty run folder: "
                      << run.recording_folder << std::endl;
            return 1;
        }
        {
            orange::ScopedFsuid fsuid_guard;
            (void)fsuid_guard;
            std::filesystem::create_directories(run_path, create_error);
        }
        if (create_error && !std::filesystem::exists(run_path)) {
            std::cerr << "[EXPERIMENT] Failed to create run folder "
                      << run.recording_folder << ": " << create_error.message() << std::endl;
            return 1;
        }
        if (!write_json_file(run_path / "run_config.json", run.config_json, &error)) {
            std::cerr << error << std::endl;
            return 1;
        }

        std::cout << "[EXPERIMENT] starting " << run.run_id
                  << " codec=" << run.options.encoder_settings.codec
                  << " preset=" << run.options.encoder_settings.preset
                  << " tuning=" << run.options.encoder_settings.tuning
                  << " rc=" << run.options.encoder_settings.rate_control_mode
                  << " q=" << run.options.encoder_settings.quality_value
                  << " gop=" << run.options.encoder_settings.gop_length
                  << " aq="
                  << format_headless_toggle_override(run.options.encoder_settings.control_overrides.aq)
                  << " temporal_aq="
                  << format_headless_toggle_override(
                         run.options.encoder_settings.control_overrides.temporal_aq)
                  << " lookahead="
                  << format_headless_toggle_override(
                         run.options.encoder_settings.control_overrides.lookahead)
                  << " lookahead_depth="
                  << run.options.encoder_settings.control_overrides.lookahead_depth
                  << " target_bitrate_bps="
                  << run.options.encoder_settings.control_overrides.target_bitrate_bps
                  << " max_bitrate_bps="
                  << run.options.encoder_settings.control_overrides.max_bitrate_bps
                  << " vbv_buffer_size="
                  << run.options.encoder_settings.control_overrides.vbv_buffer_size
                  << " direct_input=" << (run.options.nvenc_direct_input ? "true" : "false")
                  << " warmup=" << run.warmup_s
                  << " duration=" << run.duration_s
                  << std::endl;

        const std::string started_at_utc = get_current_utc_timestamp();
        const int rc = run_local_recording_session(run.options, false);
        bool run_failed = (rc != 0);
        nlohmann::json run_entry = {
            {"run_id", run.run_id},
            {"run_index", run.run_index},
            {"recording_folder", run.recording_folder},
            {"config", run.config_json},
            {"started_at_utc", started_at_utc},
            {"status", rc == 0 ? "completed" : "failed"},
            {"camera_results", nlohmann::json::array()}
        };
        if (rc != 0) {
            run_entry["reason"] = "recording session returned nonzero exit code";
        }

        nlohmann::json snapshot;
        std::string snapshot_error;
        const std::filesystem::path snapshot_path =
            std::filesystem::path(run.recording_folder) / "recording_snapshot.json";
        if (!read_json_file(snapshot_path, &snapshot, &snapshot_error)) {
            run_entry["status"] = "failed";
            if (!run_entry.contains("reason")) {
                run_entry["reason"] = snapshot_error;
            }
            run_failed = true;
        } else {
            std::vector<std::string> camera_serials;
            if (run.options.encoder_settings.select_all_cameras ||
                run.options.encoder_settings.camera_serials.empty()) {
                const nlohmann::json pipeline_metrics =
                    snapshot.value("pipeline_metrics", nlohmann::json::object());
                if (pipeline_metrics.is_object()) {
                    for (auto it = pipeline_metrics.begin(); it != pipeline_metrics.end(); ++it) {
                        camera_serials.push_back(it.key());
                    }
                }
                if (camera_serials.empty()) {
                    const nlohmann::json encoders =
                        snapshot.value("encoders", nlohmann::json::object());
                    if (encoders.is_object()) {
                        for (auto it = encoders.begin(); it != encoders.end(); ++it) {
                            camera_serials.push_back(it.key());
                        }
                    }
                }
            } else {
                camera_serials = run.options.encoder_settings.camera_serials;
            }
            std::sort(camera_serials.begin(), camera_serials.end());
            camera_serials.erase(std::unique(camera_serials.begin(), camera_serials.end()),
                                 camera_serials.end());

            if (camera_serials.empty()) {
                run_entry["status"] = "failed";
                run_entry["reason"] = "No camera results found in snapshot";
                run_failed = true;
            } else {
                bool any_fail = false;
                bool any_marginal = false;
                for (const std::string& camera_serial : camera_serials) {
                    const nlohmann::json row =
                        build_experiment_camera_result(spec, run, snapshot, camera_serial);
                    if (row.value("pass_fail", "") == "fail") {
                        any_fail = true;
                    } else if (row.value("pass_fail", "") == "marginal") {
                        any_marginal = true;
                    }
                    run_entry["camera_results"].push_back(row);
                }
                run_entry["pass_fail"] = any_fail ? "fail" : (any_marginal ? "marginal" : "pass");
                run_failed = run_failed || any_fail;
            }
        }

        run_entry["finished_at_utc"] = get_current_utc_timestamp();
        if (run_entry.value("status", "") == "failed" && !run_entry.contains("pass_fail")) {
            run_entry["pass_fail"] = "fail";
        }
        runs_json["runs"].push_back(run_entry);

        auto build_summary_json = [&]() {
            int pass_count = 0;
            int marginal_count = 0;
            int fail_count = 0;
            for (const auto& completed_run : runs_json["runs"]) {
                const std::string pass_fail = completed_run.value("pass_fail", "");
                if (pass_fail == "pass") {
                    ++pass_count;
                } else if (pass_fail == "marginal") {
                    ++marginal_count;
                } else if (pass_fail == "fail") {
                    ++fail_count;
                }
            }
            return nlohmann::json{
                {"experiment_id", spec.experiment_id},
                {"notes", spec.notes},
                {"updated_at_utc", get_current_utc_timestamp()},
                {"total_runs", runs.size()},
                {"completed_runs", runs_json["runs"].size()},
                {"pass_runs", pass_count},
                {"marginal_runs", marginal_count},
                {"fail_runs", fail_count},
                {"interrupted", quit_server},
            };
        };

        nlohmann::json summary_json = build_summary_json();
        if (!write_experiment_manifests(spec, experiment_root, runs_json, summary_json, &error)) {
            std::cerr << error << std::endl;
            return 1;
        }

        if (!run_failed &&
            run.options.external_recorder_contract.enabled() &&
            run.options.external_recorder_contract.supervise_processes) {
            std::string finalization_error;
            if (!finalize_supervised_external_recorder_run(
                    run,
                    experiment_root,
                    &finalization_error)) {
                run_failed = true;
                run_entry["status"] = "failed";
                run_entry["pass_fail"] = "fail";
                run_entry["reason"] =
                    finalization_error.empty()
                        ? "external recorder supervised finalization failed"
                        : finalization_error;
                runs_json["runs"][runs_json["runs"].size() - 1] = run_entry;
                summary_json = build_summary_json();
                if (!write_experiment_manifests(
                        spec,
                        experiment_root,
                        runs_json,
                        summary_json,
                        &error)) {
                    std::cerr << error << std::endl;
                    return 1;
                }
            }
        }

        if (run_failed) {
            ++run_failures;
        }
    }

    return run_failures == 0 ? 0 : 1;
}

int run_local_mode(const HeadlessCliOptions& options)
{
    return run_local_recording_session(options, true);
}

int run_remote_mode()
{
    if (enet_initialize() != 0)
    {
        quit_process(true, "ENET failed to initialize!");
    }

    EnetContext client;
    if (enet_initialize(&client, 3333, 1))
    {
        printf("Network Initialized!\n");
    }

    f32 last_time = tick();
    f32 current_time = tick();

    int cam_count;
    GigEVisionDeviceInfo unsorted_device_info[max_cameras];
    cam_count = scan_cameras(max_cameras, unsorted_device_info);
    GigEVisionDeviceInfo device_info[max_cameras];
    sort_cameras_ip(unsorted_device_info, device_info, cam_count);
    std::cout << "available no of cameras: " << cam_count << std::endl;

    flatbuffers::FlatBufferBuilder* fb_builder = new flatbuffers::FlatBufferBuilder(1024);
    std::string config_folder;
    RecordingContext recording_setup;
    ManagerContext manager_context{FetchGame::ManagerState_IDLE, false};
    PTPParams *ptp_params = new PTPParams{0, 0, 0, 0, true, false, false, false};

    std::thread* manager_thread = new std::thread(&create_camera_manager, &cam_count, &manager_context, unsorted_device_info, device_info, &config_folder, &recording_setup, ptp_params);
    
    while (!quit_server)
    {
        current_time = tick();
        // Handle All Incoming Packets and Send any enqued packets, does this need to be on another thread?
        service_network(&client, current_time - last_time, [&](const ENetEvent &evnt)
        {
            switch (evnt.type) 
            {
                //New connection request or an existing peer accepted our connection request
                case ENET_EVENT_TYPE_CONNECT:
                    {
                        if (manager_context.state == FetchGame::ManagerState_IDLE) {
                            printf("Network: Successfully connected! Rescaning cameras. \n");
                            manager_context.state = FetchGame::ManagerState_CONNECT; // rescan number of cams
                        } else {
                            printf("Network: Successfully connected! \n");
                            client_send_bringup_message(&client, fb_builder, evnt.peer, cam_count, manager_context.state);
                        }
                    }
                    break;
                //Server has sent us a new packet
                case ENET_EVENT_TYPE_RECEIVE:
                    {
                        std::cout << "\nA packet of length "
                                  << evnt.packet->dataLength
                                  << " was received on channel "
                                  << static_cast<unsigned int>(evnt.channelID)
                                  << ".\n";

                        uint8_t* buffer_pointer = evnt.packet->data;
                        auto server_control = FetchGame::GetServer(buffer_pointer);
                        auto server_signal = server_control->control();

                        if (server_signal == FetchGame::ServerControl_OPENCAMERA) {
                            config_folder = server_control->config_folder()->c_str();
                            manager_context.state = FetchGame::ManagerState_OPENCAMERA;
                        }
                        else if (server_signal == FetchGame::ServerControl_STARTTHREAD)
                        {
                            recording_setup.record_folder = server_control->record_folder()->c_str();
                            recording_setup.encoder_basic_setup = server_control->encoder_setup()->c_str();
                            manager_context.state = FetchGame::ManagerState_STARTCAMTHREAD;
                        } else if (server_signal == FetchGame::ServerControl_QUIT) {
                            printf("Exit \n");
                            quit_server = true;
                        } else if (server_signal == FetchGame::ServerControl_STARTRECORDING) {
                            ptp_params->ptp_global_time = server_control->ptp_global_time();
                            std::cout << ptp_params->ptp_global_time << std::endl;
                            ptp_params->network_set_start_ptp = true;
                            manager_context.state = FetchGame::ManagerState_WAITSTOP;
                            client_send_state_update_message(&client, fb_builder, evnt.peer, manager_context.state);
                        } else if (server_signal == FetchGame::ServerControl_STOPRECORDING) {
                            // stop recording
                            printf("stop signal\n");
                            std::cout << server_control->ptp_global_time() << std::endl;
                            ptp_params->ptp_stop_time = server_control->ptp_global_time();
                            std::cout << ptp_params->ptp_stop_time << std::endl;
                            ptp_params->network_set_stop_ptp = true;
                        }
                        enet_packet_destroy(evnt.packet);
                    }
                    break;

                //Server has disconnected
                case ENET_EVENT_TYPE_DISCONNECT:
                    printf("Network: Server has disconnected!\n");
                    break;
            } });

        // coordinate with other thread
        if (manager_context.state == FetchGame::ManagerState_CONNECTED)
        {
            manager_context.state = FetchGame::ManagerState_IDLE;
            client_send_bringup_message(&client, fb_builder, &client.m_pNetwork->peers[0], cam_count, manager_context.state);
        }
        if (manager_context.state == FetchGame::ManagerState_CAMERAOPENED) {
            manager_context.state = FetchGame::ManagerState_WAITTHREAD;
            client_send_state_update_message(&client, fb_builder, &client.m_pNetwork->peers[0], manager_context.state);
        } else if (manager_context.state == FetchGame::ManagerState_THREADREADY)
        {
            manager_context.state = FetchGame::ManagerState_WAITSTART;
            client_send_state_update_message(&client, fb_builder, &client.m_pNetwork->peers[0], manager_context.state);
        } else if (manager_context.state == FetchGame::ManagerState_RECORDSTOPPED)
        {
            manager_context.state = FetchGame::ManagerState_IDLE;
            client_send_state_update_message(&client, fb_builder, &client.m_pNetwork->peers[0], manager_context.state);
        }
    
        usleep(1000);
        last_time = current_time;
    }

    manager_context.quit = true;
    manager_thread->join();

    // Disconnect
    enet_peer_disconnect(&client.m_pNetwork->peers[0], 0);
    uint8_t disconnected = false;
    /* Allow up to 3 seconds for the disconnect to succeed
     * and drop any packets received packets.
     */
    ENetEvent evnt;
    while (enet_host_service(client.m_pNetwork, &evnt, 3000) > 0)
    {
        switch (evnt.type)
        {
        case ENET_EVENT_TYPE_RECEIVE:
            enet_packet_destroy(evnt.packet);
            break;
        case ENET_EVENT_TYPE_DISCONNECT:
            puts("Disconnection succeeded.");
            disconnected = true;
            break;
        }
    }
    

    // Drop connection, since disconnection didn't successed
    if (!disconnected)
    {
        enet_peer_reset(&client.m_pNetwork->peers[0]);
    }
    enet_host_destroy(client.m_pNetwork);
    delete manager_thread;
    delete ptp_params;
    delete fb_builder;
    enet_deinitialize();
    return 0;
}


int main(int argc, char *argv[])
{
    signal(SIGINT, interruptHandler);
    HeadlessCliOptions options;
    std::string parse_error;
    if (!parse_headless_cli_options(argc, argv, &options, &parse_error)) {
        std::cerr << parse_error << std::endl;
        print_headless_usage(argv[0]);
        return 2;
    }

    if (options.show_help) {
        print_headless_usage(argv[0]);
        return 0;
    }

    std::string validation_error;
    if (!validate_headless_cli_options(options, &validation_error)) {
        std::cerr << validation_error << std::endl;
        if (options.mode == HeadlessMode::Local) {
            print_headless_usage(argv[0]);
        }
        return 2;
    }

    if (options.mode == HeadlessMode::Local) {
        if (!options.experiment_spec_path.empty()) {
            return run_local_experiment(options);
        }
        return run_local_mode(options);
    }

    return run_remote_mode();
}
