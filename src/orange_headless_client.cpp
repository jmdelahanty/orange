#include <algorithm>
#include <array>
#include <chrono>
#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <limits>
#include <numeric>
#include <sstream>
#include <thread>
#include <unordered_map>
#include <unordered_set>
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
#include "modern_recording_pipeline.h"
#include "fsuid_guard.h"
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

struct HeadlessCliOptions {
    HeadlessMode mode = HeadlessMode::Remote;
    bool show_help = false;
    bool list_cameras = false;
    bool stream_only = false;
    bool nvenc_direct_input = false;
    std::string config_folder;
    std::string record_folder;
    std::string experiment_spec_path;
    int duration_seconds = 0;
    int stream_start_delay_seconds = 0;
    int record_start_delay_seconds = 0;
    std::vector<int> required_gpu_ids;
    HeadlessEncoderSettings encoder_settings;
    PreEncoderReferenceCaptureConfig pre_encoder_reference_capture;
    bool has_recording_strategy_override = false;
    RecordingStrategyConfig recording_strategy_override;
    std::unordered_map<std::string, RecordingStrategyConfig> recording_strategy_overrides_by_camera;
};

struct ExperimentSpec {
    std::string source_path;
    nlohmann::json source_json = nlohmann::json::object();
    std::string experiment_id;
    std::string notes;
    std::string output_root;
    std::string config_folder;
    int duration_s = 0;
    int warmup_s = 0;
    int stream_start_delay_s = 0;
    bool nvenc_direct_input = false;
    double target_fps_tolerance_pct = 1.0;
    bool require_zero_acq_starve = true;
    bool require_zero_pre_drops = true;
    bool require_zero_enc_fail = true;
    std::vector<int> gpu_ids;
    HeadlessEncoderSettings selection;
    PreEncoderReferenceCaptureConfig pre_encoder_reference_capture;
    bool has_recording_strategy_override = false;
    RecordingStrategyConfig recording_strategy_override;
    std::unordered_map<std::string, RecordingStrategyConfig> recording_strategy_overrides_by_camera;
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

struct ExperimentCsvWindowStats {
    bool ok = false;
    std::size_t total_rows = 0;
    std::size_t included_rows = 0;
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
    uint64_t submitted_frames_delta = 0;
    uint64_t primary_routed_frames_delta = 0;
    uint64_t helper_requested_frames_delta = 0;
    uint64_t helper_fallback_frames_delta = 0;
    uint64_t helper_dispatched_frames_delta = 0;
    uint64_t camera_dropped_frames_delta = 0;
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

using CameraGpuOverrideMap = std::unordered_map<std::string, int>;
using RecordingStrategyOverrideMap = std::unordered_map<std::string, RecordingStrategyConfig>;

constexpr int kGpuDmonStartupPollMs = 200;
constexpr int kGpuDmonShutdownWaitMs = 2000;
constexpr const char* kBundledFfprobePath = "/opt/orange/lib/ffmpeg-nvidia/bin/ffprobe";

struct ExperimentVideoArtifactStats {
    std::string video_path;
    bool video_present = false;
    uint64_t file_size_bytes = 0;
    double duration_s = 0.0;
    uint64_t achieved_bitrate_bps = 0;
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

ExperimentVideoArtifactStats summarize_video_artifact(const std::string& recording_folder,
                                                      const std::string& camera_serial)
{
    ExperimentVideoArtifactStats stats;
    if (recording_folder.empty() || camera_serial.empty()) {
        return stats;
    }

    const std::filesystem::path video_path =
        std::filesystem::path(recording_folder) / ("Cam" + camera_serial + ".mp4");
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

    return stats;
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

nlohmann::json build_recording_strategy_override_map_json(
    const RecordingStrategyOverrideMap& overrides)
{
    nlohmann::json out = nlohmann::json::object();
    for (const auto& [camera_serial, recording_strategy] : overrides) {
        out[camera_serial] = build_recording_strategy_json(recording_strategy);
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

    spec->has_recording_strategy_override = false;
    spec->recording_strategy_override = RecordingStrategyConfig();
    spec->recording_strategy_overrides_by_camera.clear();

    if (fixed.contains("recording")) {
        if (!fixed["recording"].is_object()) {
            if (error_out) {
                *error_out = "Experiment spec fixed.recording must be a JSON object";
            }
            return false;
        }
        if (!parse_recording_strategy_json(
                fixed["recording"],
                &spec->recording_strategy_override,
                error_out)) {
            if (error_out && !error_out->empty()) {
                *error_out = "Experiment spec fixed.recording invalid: " + *error_out;
            }
            return false;
        }
        spec->has_recording_strategy_override = true;
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
        RecordingStrategyConfig recording_strategy;
        if (!parse_recording_strategy_json(item.value(), &recording_strategy, error_out)) {
            if (error_out && !error_out->empty()) {
                *error_out = "Experiment spec fixed.recording_by_camera." + item.key() +
                             " invalid: " + *error_out;
            }
            return false;
        }
        spec->recording_strategy_overrides_by_camera[canonical_serial] = std::move(recording_strategy);
    }

    return true;
}

bool apply_recording_strategy_overrides_to_selected_cameras(
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
        if (options.has_recording_strategy_override) {
            camera_params.recording_strategy = options.recording_strategy_override;
            camera_params.recording.strategy = camera_params.recording_strategy;
            applied_override = true;
        }

        const auto per_camera_it =
            options.recording_strategy_overrides_by_camera.find(camera_serial);
        if (per_camera_it != options.recording_strategy_overrides_by_camera.end()) {
            camera_params.recording_strategy = per_camera_it->second;
            camera_params.recording.strategy = camera_params.recording_strategy;
            applied_override = true;
            applied_per_camera_overrides.insert(camera_serial);
        }

        if (applied_override) {
            std::cout << "Applied headless recording override."
                      << " camera=" << camera_serial
                      << " " << format_recording_strategy_summary(camera_params.recording_strategy)
                      << std::endl;
        }
    }

    for (const auto& [camera_serial, _] : options.recording_strategy_overrides_by_camera) {
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
            out << ",";
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
            check_camera_errors(EVT_CameraCloseStream(&ecams[idx].camera), cameras_params[idx].camera_serial.c_str());
        }
        if (idx >= 0 && idx < static_cast<int>(camera_resources.size())) {
            camera_resources[idx].cleanup();
        }
    }
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

void shutdown_headless_run(std::vector<std::thread>& camera_threads,
                           std::vector<CameraResources>& camera_resources,
                           std::vector<int>& active_camera_indices,
                           std::vector<std::unique_ptr<ModernRecordingPipeline>>& recording_pipelines,
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
        camera_control->subscribe = false;
    }

    for (auto& t : camera_threads) {
        if (t.joinable()) {
            t.join();
        }
    }
    camera_threads.clear();

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

RecordingOutputConfig build_native_recording_output_config(const CameraParams& camera_params)
{
    RecordingOutputConfig output;
    output.mode = "factor";
    output.downsample_factor = 1;
    output.requested_width = static_cast<int>(camera_params.width);
    output.requested_height = static_cast<int>(camera_params.height);
    output.resolved_width = static_cast<int>(camera_params.width);
    output.resolved_height = static_cast<int>(camera_params.height);
    output.resize_enabled = false;
    return output;
}

bool prepare_headless_recording_artifacts(const std::string& record_folder,
                                          CameraControl* camera_control,
                                          CameraParams* cameras_params,
                                          int num_cameras,
                                          PTPParams* ptp_params)
{
    if (record_folder.empty()) {
        std::cerr << "Headless recording folder is empty." << std::endl;
        return false;
    }

    orange::ScopedFsuid fsuid_guard;
    (void)fsuid_guard;
    const std::filesystem::path recording_path(record_folder);
    std::error_code create_error;
    std::filesystem::create_directories(recording_path, create_error);
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
            camera_control->sync_camera,
            ptp_params)) {
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

    std::cout << "Recorded video saves to : " << record_folder << std::endl;
    return true;
}

void drain_and_shutdown_recording(std::vector<std::unique_ptr<ModernRecordingPipeline>>& recording_pipelines,
                                  CameraControl* camera_control)
{
    camera_control->record_video = false;
    camera_control->recording_draining = true;
    camera_control->stop_record = true;

    const auto drain_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (camera_control->active_recorders.load(std::memory_order_relaxed) > 0 &&
           std::chrono::steady_clock::now() < drain_deadline) {
        usleep(1000);
    }

    if (camera_control->active_recorders.load(std::memory_order_relaxed) > 0) {
        std::cerr << "Headless recording drain timed out with "
                  << camera_control->active_recorders.load(std::memory_order_relaxed)
                  << " active recorder(s)." << std::endl;
    }

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
    HeadlessThreadFailureState* thread_failure_state,
    HeadlessGpuDmonMonitor* gpu_dmon_monitor,
    CameraParams *cameras_params, CameraEmergent *ecams, CameraControl *camera_control, CameraEachSelect *cameras_select,
    GigEVisionDeviceInfo *device_info, int num_cameras, PTPParams *ptp_params,
    const std::vector<int>& required_gpu_ids,
    std::string record_folder, std::string encoder_basic_setup,
    const PreEncoderReferenceCaptureConfig& pre_encoder_reference_capture,
    int record_start_delay_seconds = 0,
    bool enable_recording = true)
{
    std::cout << "start camera sthread..." << std::endl;
    if (thread_failure_state) {
        thread_failure_state->reset();
    }
    const HeadlessEncoderSettings encoder_settings = parse_headless_encoder_setup(encoder_basic_setup);
    std::cout << "Headless encoder config: codec=" << encoder_settings.codec
              << " preset=" << encoder_settings.preset
              << " tuning=" << encoder_settings.tuning
              << " rc=" << encoder_settings.rate_control_mode
              << " importance_map_mode=" << encoder_settings.importance_map.mode
              << " importance_map_roi_size_px=" << encoder_settings.importance_map.roi_size_px
              << " quality=" << encoder_settings.quality_value
              << " gop=" << encoder_settings.gop_length
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

        camera_resources.clear();
        camera_resources.resize(num_cameras);
        for (int idx : selected_indices) {
            cameras_select[idx].stream_on = false;
            cameras_select[idx].record = enable_recording;
            cameras_select[idx].yolo = false;
            cameras_select[idx].crop_and_encode = false;
            cameras_select[idx].send_frame_ipc = false;
            camera_resources[idx].initialize(
                cameras_params[idx].gpu_id,
                max_frame_size_bytes,
                false);
        }

    } catch (const std::exception& ex) {
        std::cerr << "Failed to start thread: " << ex.what() << std::endl;
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

    std::vector<CameraParams> selected_camera_params;
    selected_camera_params.reserve(selected_indices.size());
    for (int idx : selected_indices) {
        selected_camera_params.push_back(cameras_params[idx]);
    }

    recording_pipelines.clear();
    recording_pipelines.resize(num_cameras);

    try {
        if (enable_recording) {
            if (!prepare_headless_recording_artifacts(
                    record_folder,
                    camera_control,
                    selected_camera_params.data(),
                    static_cast<int>(selected_camera_params.size()),
                    ptp_params)) {
                cleanup_selected_camera_buffers(selected_indices, ecams, cameras_params, camera_resources);
                return false;
            }

            start_headless_gpu_dmon_monitor(
                gpu_dmon_monitor,
                record_folder,
                collect_unique_gpu_ids(cameras_params, selected_indices));

            for (int idx : selected_indices) {
                const ResolvedRecordingConfig resolved_recording_config =
                    build_resolved_recording_config(
                        cameras_params[idx],
                        cameras_params[idx].gpu_id,
                        build_native_recording_output_config(cameras_params[idx]),
                        encoder_settings.codec,
                        encoder_settings.preset,
                        encoder_settings.tuning,
                        encoder_settings.rate_control_mode,
                        encoder_settings.quality_value,
                        encoder_settings.gop_length,
                        encoder_settings.control_overrides,
                        encoder_settings.importance_map,
                        record_folder,
                        pre_encoder_reference_capture);
                recording_pipelines[idx] = std::make_unique<ModernRecordingPipeline>(
                    &cameras_params[idx],
                    resolved_recording_config,
                    *camera_resources[idx].recycle_queue,
                    camera_control);
                recording_pipelines[idx]->start();
            }
        }

        // Match the GUI path: build the recording pipeline first, then open the
        // camera stream and allocate EVT frame buffers against the active GPU.
        allocate_selected_camera_frame_buffers(ecams, cameras_params, selected_indices);
    } catch (const std::exception& ex) {
        std::cerr << "Failed to initialize headless recording pipelines: " << ex.what() << std::endl;
        if (enable_recording) {
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
        camera_threads.push_back(std::thread(
            [&, idx, thread_failure_state]() {
                try {
                    acquire_frames(
                        &ecams[idx],
                        &cameras_params[idx],
                        &cameras_select[idx],
                        camera_control,
                        ptp_params,
                        nullptr,
                        nullptr,
                        recording_pipelines[idx] ? recording_pipelines[idx]->recording_ingress() : nullptr,
                        nullptr,
                        nullptr,
                        &camera_resources[idx],
                        nullptr);
                } catch (const std::exception& ex) {
                    std::ostringstream message;
                    message << "Headless camera thread failed for camera "
                            << cameras_params[idx].camera_serial
                            << ": " << ex.what();
                    std::cerr << message.str() << std::endl;
                    if (thread_failure_state) {
                        thread_failure_state->record_failure(message.str());
                    }
                    if (camera_control) {
                        camera_control->subscribe = false;
                        camera_control->record_video = false;
                        camera_control->recording_draining = true;
                        camera_control->stop_record = true;
                    }
                    quit_server = true;
                } catch (...) {
                    const std::string message =
                        "Headless camera thread failed with an unknown exception for camera " +
                        cameras_params[idx].camera_serial;
                    std::cerr << message << std::endl;
                    if (thread_failure_state) {
                        thread_failure_state->record_failure(message);
                    }
                    if (camera_control) {
                        camera_control->subscribe = false;
                        camera_control->record_video = false;
                        camera_control->recording_draining = true;
                        camera_control->stop_record = true;
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
                        &gpu_dmon_monitor,
                        ecams,
                        cameras_params,
                        *cam_count,
                        nullptr,
                        camera_control,
                        ptp_params,
                        true);
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
                &gpu_dmon_monitor,
                ecams,
                cameras_params,
                *cam_count,
                nullptr,
                camera_control,
                ptp_params,
                true);
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
            !options.required_gpu_ids.empty() ||
            pre_encoder_reference_capture_requested(options.pre_encoder_reference_capture) ||
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
            !options.required_gpu_ids.empty() ||
            pre_encoder_reference_capture_requested(options.pre_encoder_reference_capture) ||
            !headless_encoder_settings_is_default(options.encoder_settings)) {
            if (error_out) {
                *error_out =
                    "When --experiment-spec is provided, per-run flags like "
                    "--list-cameras, --stream-only, --record-folder, --camera, --codec, --preset, --tuning, "
                    "--rate-control, --quality, --gop, --preenc-ref-max-frames, "
                    "--preenc-ref-max-seconds, --duration, --stream-start-delay, --nvenc-direct-input, "
                    "--record-delay, and --gpu-id "
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

    if (options.stream_only && options.pre_encoder_reference_capture.enabled) {
        if (error_out) {
            *error_out =
                "pre_encoder_reference_capture requires recording output and is not supported with --stream-only.";
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
        if (camera.recording_strategy.split_gop_enabled()) {
            for (int helper_gpu_id : camera.recording_strategy.split_gop.encoder_gpu_ids) {
                append_gpu_id(helper_gpu_id);
            }
        }
    }
    std::sort(gpu_ids.begin(), gpu_ids.end());
    return gpu_ids;
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
    const int submitted_frames_index = required_index("submitted_frames");
    const int primary_routed_frames_index = required_index("primary_routed_frames");
    const int helper_requested_frames_index = required_index("helper_requested_frames");
    const int helper_fallback_frames_index = required_index("helper_fallback_frames");
    const int helper_dispatched_frames_index = required_index("helper_dispatched_frames");
    const int camera_dropped_frames_index = required_index("camera_dropped_frames");
    if (enc_fps_index < 0 || acq_starve_index < 0 || pre_drops_index < 0 || enc_fail_index < 0) {
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
    uint64_t baseline_submitted_frames = 0;
    uint64_t baseline_primary_routed_frames = 0;
    uint64_t baseline_helper_requested_frames = 0;
    uint64_t baseline_helper_fallback_frames = 0;
    uint64_t baseline_helper_dispatched_frames = 0;
    uint64_t baseline_camera_dropped_frames = 0;
    uint64_t last_acq_starve = 0;
    uint64_t last_pre_waits = 0;
    uint64_t last_pre_drops = 0;
    uint64_t last_enc_fail = 0;
    uint64_t last_enc_slow = 0;
    uint64_t last_submitted_frames = 0;
    uint64_t last_primary_routed_frames = 0;
    uint64_t last_helper_requested_frames = 0;
    uint64_t last_helper_fallback_frames = 0;
    uint64_t last_helper_dispatched_frames = 0;
    uint64_t last_camera_dropped_frames = 0;

    std::string line;
    std::size_t row_index = 0;
    const std::size_t warmup_row_count = static_cast<std::size_t>(std::max(0, warmup_rows));
    while (std::getline(input, line)) {
        if (line.empty()) {
            continue;
        }

        const std::vector<std::string> cells = split_csv_line_simple(line);
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
        uint64_t submitted_frames = 0;
        uint64_t primary_routed_frames = 0;
        uint64_t helper_requested_frames = 0;
        uint64_t helper_fallback_frames = 0;
        uint64_t helper_dispatched_frames = 0;
        uint64_t camera_dropped_frames = 0;
        if (!parse_double_cell(cells, enc_fps_index, &enc_fps) ||
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

        ++stats.total_rows;
        last_acq_starve = acq_starve;
        last_pre_waits = pre_waits;
        last_pre_drops = pre_drops;
        last_enc_fail = enc_fail;
        last_enc_slow = enc_slow;
        last_submitted_frames = submitted_frames;
        last_primary_routed_frames = primary_routed_frames;
        last_helper_requested_frames = helper_requested_frames;
        last_helper_fallback_frames = helper_fallback_frames;
        last_helper_dispatched_frames = helper_dispatched_frames;
        last_camera_dropped_frames = camera_dropped_frames;

        if (row_index < warmup_row_count) {
            baseline_acq_starve = acq_starve;
            baseline_pre_waits = pre_waits;
            baseline_pre_drops = pre_drops;
            baseline_enc_fail = enc_fail;
            baseline_enc_slow = enc_slow;
            baseline_submitted_frames = submitted_frames;
            baseline_primary_routed_frames = primary_routed_frames;
            baseline_helper_requested_frames = helper_requested_frames;
            baseline_helper_fallback_frames = helper_fallback_frames;
            baseline_helper_dispatched_frames = helper_dispatched_frames;
            baseline_camera_dropped_frames = camera_dropped_frames;
        } else {
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
    if (spec->stream_start_delay_s < 0) {
        if (error_out) {
            *error_out = "Experiment spec fixed.stream_start_delay_s must be >= 0";
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
    const std::string sync_mode = fixed.value("sync_mode", "free_run");
    if (sync_mode != "free_run") {
        if (error_out) {
            *error_out = "Local experiment runner currently only supports fixed.sync_mode=free_run";
        }
        return false;
    }

    spec->target_fps_tolerance_pct = policy.value("target_fps_tolerance_pct", 1.0);
    spec->require_zero_acq_starve = policy.value("require_zero_acq_starve", true);
    spec->require_zero_pre_drops = policy.value("require_zero_pre_drops", true);
    spec->require_zero_enc_fail = policy.value("require_zero_enc_fail", true);

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
                                                            run.options.duration_seconds = spec.duration_s + spec.warmup_s;
                                                            run.options.stream_start_delay_seconds = spec.stream_start_delay_s;
                                                            run.options.record_start_delay_seconds = 0;
                                                            run.options.nvenc_direct_input = spec.nvenc_direct_input;
                                                            run.options.required_gpu_ids = spec.gpu_ids;
                                                            run.options.encoder_settings = spec.selection;
                                                            run.options.pre_encoder_reference_capture = spec.pre_encoder_reference_capture;
                                                            run.options.has_recording_strategy_override =
                                                                spec.has_recording_strategy_override;
                                                            run.options.recording_strategy_override =
                                                                spec.recording_strategy_override;
                                                            run.options.recording_strategy_overrides_by_camera =
                                                                spec.recording_strategy_overrides_by_camera;
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
                                                                {"nvenc_direct_input", spec.nvenc_direct_input},
                                                                {"recording_folder", run.recording_folder},
                                                                {"pre_encoder_reference_capture",
                                                                 build_pre_encoder_reference_capture_json(
                                                                     run.options.pre_encoder_reference_capture)},
                                                            };
                                                            if (spec.has_recording_strategy_override) {
                                                                run.config_json["recording"] =
                                                                    build_recording_strategy_json(
                                                                        spec.recording_strategy_override);
                                                            }
                                                            if (!spec.recording_strategy_overrides_by_camera.empty()) {
                                                                run.config_json["recording_by_camera"] =
                                                                    build_recording_strategy_override_map_json(
                                                                        spec.recording_strategy_overrides_by_camera);
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

    std::string recording_override_error;
    if (!apply_recording_strategy_overrides_to_selected_cameras(
            options,
            cameras_params.get(),
            selected_inventory_indices,
            &recording_override_error)) {
        std::cerr << recording_override_error << std::endl;
        close_selected_cameras(selected_inventory_indices, ecams.get(), cameras_params.get());
        return 1;
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
    HeadlessThreadFailureState thread_failure_state;
    HeadlessGpuDmonMonitor gpu_dmon_monitor;
    const bool enable_recording = !options.stream_only;
    const std::string active_record_folder = enable_recording ? options.record_folder : std::string();

    const std::string encoder_setup = build_headless_encoder_setup_string(options.encoder_settings);
    const bool started = start_camera_thread(
        camera_threads,
        camera_resources,
        active_camera_indices,
        recording_pipelines,
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
        options.pre_encoder_reference_capture,
        options.record_start_delay_seconds,
        enable_recording);

    if (!started) {
        close_selected_cameras(selected_inventory_indices, ecams.get(), cameras_params.get());
        return 1;
    }

    std::cout << "Local headless " << (enable_recording ? "recording" : "stream-only")
              << " run started."
              << " folder=" << (enable_recording ? options.record_folder : "<none>")
              << " cameras=" << format_selected_camera_serials(options.encoder_settings)
              << std::endl;

    const auto run_start_time = std::chrono::steady_clock::now();
    const auto record_arm_time = run_start_time + std::chrono::seconds(options.record_start_delay_seconds);
    bool recording_armed = !enable_recording || camera_control.record_video;
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
            camera_control.recording_draining = false;
            camera_control.stop_record = false;
            camera_control.record_video = true;
            recording_armed = true;
            std::cout << "Local headless recording armed after warmup."
                      << " folder=" << options.record_folder
                      << std::endl;
        }
        usleep(100000);
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
        &gpu_dmon_monitor,
        ecams.get(),
        cameras_params.get(),
        discovered_cam_count,
        &selected_inventory_indices,
        &camera_control,
        &ptp_params,
        false);

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
    row["display"] = false;
    row["yolo"] = false;
    row["recording_folder"] = run.recording_folder;
    row["video_present"] = false;
    row["video_path"] = "";
    row["video_file_size_bytes"] = 0ULL;
    row["video_duration_s"] = 0.0;
    row["video_achieved_bitrate_bps"] = 0ULL;
    row["status"] = "completed";
    row["pass_fail"] = "marginal";
    row["reason"] = "not_evaluated";
    row["gpu_id"] = -1;
    row["gpu_name"] = "";
    row["gpu_pci_bus_id"] = "";
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
    row["submitted_frames_final"] = 0ULL;
    row["primary_routed_frames_final"] = 0ULL;
    row["helper_requested_frames_final"] = 0ULL;
    row["helper_fallback_frames_final"] = 0ULL;
    row["helper_dispatched_frames_final"] = 0ULL;
    row["routing_last_target_gpu_id"] = -1;
    row["routing_last_route_mode"] = "";
    row["dropped_frames_camera"] = -1;
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
    const nlohmann::json ptp_summary = [&]() {
        nlohmann::json out = nlohmann::json::object();
        std::string error;
        read_json_file(std::filesystem::path(run.recording_folder) / "ptp_sync_summary.json", &out, &error);
        return out;
    }();
    const ExperimentVideoArtifactStats video_stats =
        summarize_video_artifact(run.recording_folder, camera_serial);

    row["video_present"] = video_stats.video_present;
    row["video_path"] = video_stats.video_path;
    row["video_file_size_bytes"] = video_stats.file_size_bytes;
    row["video_duration_s"] = video_stats.duration_s;
    row["video_achieved_bitrate_bps"] = video_stats.achieved_bitrate_bps;

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
    row["submitted_frames_final"] = csv_stats.submitted_frames_delta;
    row["primary_routed_frames_final"] = csv_stats.primary_routed_frames_delta;
    row["helper_requested_frames_final"] = csv_stats.helper_requested_frames_delta;
    row["helper_fallback_frames_final"] = csv_stats.helper_fallback_frames_delta;
    row["helper_dispatched_frames_final"] = csv_stats.helper_dispatched_frames_delta;
    row["dropped_frames_camera"] = static_cast<int64_t>(csv_stats.camera_dropped_frames_delta);
    const nlohmann::json routing_info = pipeline_info.value("routing", nlohmann::json::object());
    if (routing_info.is_object()) {
        row["routing_last_target_gpu_id"] = routing_info.value("last_target_gpu_id", -1);
        row["routing_last_route_mode"] = routing_info.value("last_route_mode", "");
    }

    if (encoder_info.is_object()) {
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
        const double target_fps = static_cast<double>(encoder_info.value("fps", 0));
        const uint64_t acq_starve = row["acq_starve_final"].get<uint64_t>();
        const uint64_t pre_drops = row["pre_drops_final"].get<uint64_t>();
        const uint64_t enc_fail = row["enc_fail_final"].get<uint64_t>();
        const double enc_fps_mean = row["enc_fps_mean"].get<double>();
        const double tolerance = target_fps * (spec.target_fps_tolerance_pct / 100.0);
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

        if (importance_map_requested && !importance_map_active) {
            row["pass_fail"] = "fail";
            row["reason"] = "importance map inactive";
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
            } else if ((spec.require_zero_acq_starve && acq_starve > 0) ||
                       (spec.require_zero_pre_drops && pre_drops > 0) ||
                       (spec.require_zero_enc_fail && enc_fail > 0)) {
                row["pass_fail"] = "fail";
                if (spec.require_zero_acq_starve && acq_starve > 0) {
                    row["reason"] = "nonzero acquisition starvation";
                } else if (spec.require_zero_pre_drops && pre_drops > 0) {
                    row["reason"] = "nonzero preprocess drops";
                } else {
                    row["reason"] = "nonzero encode failures";
                }
            } else if (!fps_ok) {
                row["pass_fail"] = "marginal";
                row["reason"] = "encode fps below target tolerance";
            } else {
                row["pass_fail"] = "pass";
                row["reason"] = "meets current policy";
            }
        } else {
            if ((spec.require_zero_acq_starve && acq_starve > 0) ||
                (spec.require_zero_pre_drops && pre_drops > 0) ||
                (spec.require_zero_enc_fail && enc_fail > 0)) {
                row["pass_fail"] = "fail";
                if (spec.require_zero_acq_starve && acq_starve > 0) {
                    row["reason"] = "nonzero acquisition starvation";
                } else if (spec.require_zero_pre_drops && pre_drops > 0) {
                    row["reason"] = "nonzero preprocess drops";
                } else {
                    row["reason"] = "nonzero encode failures";
                }
            } else if (!fps_ok) {
                row["pass_fail"] = "marginal";
                row["reason"] = "encode fps below target tolerance";
            } else {
                row["pass_fail"] = "pass";
                row["reason"] = "meets current policy";
            }
        }
    } else {
        row["status"] = "failed";
        row["pass_fail"] = "fail";
        row["reason"] = "missing encoder snapshot";
    }

    if (row["dropped_frames_camera"].get<int64_t>() < 0 && pipeline_info.is_object()) {
        const nlohmann::json totals = pipeline_info.value("totals", nlohmann::json::object());
        if (totals.is_object() && totals.contains("camera_dropped_frames")) {
            row["dropped_frames_camera"] = totals.value("camera_dropped_frames", -1LL);
        }
    }

    if (row["dropped_frames_camera"].get<int64_t>() < 0 && ptp_summary.is_object()) {
        const nlohmann::json cameras = ptp_summary.value("cameras", nlohmann::json::object());
        if (cameras.is_object()) {
            const nlohmann::json camera_summary = cameras.value(camera_serial, nlohmann::json::object());
            if (camera_summary.is_object()) {
                row["dropped_frames_camera"] = camera_summary.value("dropped_frames", -1LL);
            }
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
    csv << "experiment_id,run_id,camera_serial,gpu_id,gpu_name,gpu_pci_bus_id,codec,preset,tuning,rate_control_mode,importance_map_mode,importance_map_roi_size_px,quality_value,gop_length,aq_override,temporal_aq_override,lookahead_override,lookahead_depth_override,target_bitrate_bps_override,max_bitrate_bps_override,vbv_buffer_size_override,importance_map_enabled,importance_map_active_mode,importance_map_block_size,importance_map_grid_width,importance_map_grid_height,nvenc_direct_input,duration_s,warmup_s,display,yolo,recording_folder,video_present,video_path,video_file_size_bytes,video_duration_s,video_achieved_bitrate_bps,status,pass_fail,reason,enc_fps_mean,enc_fps_p95,enc_fps_primary_mean,enc_fps_primary_p95,enc_fps_helpers_mean,enc_fps_helpers_p95,acq_free_entries_min,acq_free_events_min,yolo_events_min,pre_buffers_min,pre_events_min,acq_starve_final,pre_waits_final,pre_drops_final,enc_fail_final,enc_slow_final,submitted_frames_final,primary_routed_frames_final,helper_requested_frames_final,helper_fallback_frames_final,helper_dispatched_frames_final,routing_last_target_gpu_id,routing_last_route_mode,dropped_frames_camera,pre_encoder_reference_capture_enabled,pre_encoder_reference_capture_max_frames,pre_encoder_reference_capture_max_seconds,pre_encoder_reference_capture_status,pre_encoder_reference_frames_captured,pre_encoder_reference_bytes_written,pre_encoder_reference_raw_dump_present,pre_encoder_reference_index_present,pre_encoder_reference_metadata_present,pre_encoder_reference_raw_dump_path,pre_encoder_reference_index_path,pre_encoder_reference_metadata_path\n";
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
                << (row.value("nvenc_direct_input", false) ? "true" : "false") << ","
                << row.value("duration_s", 0) << ","
                << row.value("warmup_s", 0) << ","
                << (row.value("display", false) ? "true" : "false") << ","
                << (row.value("yolo", false) ? "true" : "false") << ","
                << "\"" << row.value("recording_folder", "") << "\","
                << (row.value("video_present", false) ? "true" : "false") << ","
                << "\"" << row.value("video_path", "") << "\","
                << row.value("video_file_size_bytes", 0ULL) << ","
                << row.value("video_duration_s", 0.0) << ","
                << row.value("video_achieved_bitrate_bps", 0ULL) << ","
                << row.value("status", "") << ","
                << row.value("pass_fail", "") << ","
                << "\"" << row.value("reason", "") << "\","
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
                << row.value("submitted_frames_final", 0ULL) << ","
                << row.value("primary_routed_frames_final", 0ULL) << ","
                << row.value("helper_requested_frames_final", 0ULL) << ","
                << row.value("helper_fallback_frames_final", 0ULL) << ","
                << row.value("helper_dispatched_frames_final", 0ULL) << ","
                << row.value("routing_last_target_gpu_id", -1) << ","
                << "\"" << row.value("routing_last_route_mode", "") << "\","
                << row.value("dropped_frames_camera", -1) << ","
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
    ScopedEnvVarOverride direct_input_override(
        "ORANGE_NVENC_DIRECT_INPUT",
        options.nvenc_direct_input ? "1" : "0");
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
        ScopedEnvVarOverride per_run_direct_input_override(
            "ORANGE_NVENC_DIRECT_INPUT",
            run.options.nvenc_direct_input ? "1" : "0");
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
        if (run_failed) {
            ++run_failures;
        }

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
        const nlohmann::json summary_json = {
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
        if (!write_experiment_manifests(spec, experiment_root, runs_json, summary_json, &error)) {
            std::cerr << error << std::endl;
            return 1;
        }
    }

    return run_failures == 0 ? 0 : 1;
}

int run_local_mode(const HeadlessCliOptions& options)
{
    ScopedEnvVarOverride direct_input_override(
        "ORANGE_NVENC_DIRECT_INPUT",
        options.nvenc_direct_input ? "1" : "0");
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
