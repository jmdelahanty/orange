#include "session/recording_session.h"

#include "citrus_recording_geometry.h"
#include "external_recorder_contract_utils.h"
#include "external_recorder_supervisor.h"
#include "fsuid_guard.h"
#include "project.h"
#include "recording_ingress.h"
#include "recording_output_utils.h"
#include "session/external_crop_recorder_config.h"
#include "gui/spatial_layout/sha256.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <map>
#include <set>
#include <sstream>
#include <thread>
#include <utility>

namespace orange::session {

namespace {

bool is_valid_pipeline_index(const RecordingSessionState& state, const int camera_index)
{
    return camera_index >= 0 &&
           camera_index < static_cast<int>(state.recording_pipelines.size());
}

bool env_flag_enabled(const char* name)
{
    const char* value = std::getenv(name);
    if (!value) {
        return false;
    }
    std::string text(value);
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return !(text.empty() || text == "0" || text == "false" || text == "no" || text == "off");
}

std::string trim_ascii_copy(std::string value)
{
    auto is_space = [](unsigned char c) { return std::isspace(c) != 0; };
    value.erase(value.begin(), std::find_if(value.begin(), value.end(), [&](unsigned char c) {
        return !is_space(c);
    }));
    value.erase(std::find_if(value.rbegin(), value.rend(), [&](unsigned char c) {
        return !is_space(c);
    }).base(), value.end());
    return value;
}

void add_recording_geometry_contract_reference(
    nlohmann::json* manifest,
    const std::string& recording_folder)
{
    if (manifest == nullptr || recording_folder.empty()) {
        return;
    }
    const std::filesystem::path path =
        std::filesystem::path(recording_folder) /
        "recording_geometry_contract.json";
    std::string bytes;
    std::string error;
    if (!orange::gui::spatial_layout::checksum::read_file(
            path, &bytes, &error)) {
        return;
    }
    nlohmann::json contract = nlohmann::json::parse(bytes, nullptr, false);
    if (contract.is_discarded() || !contract.is_object() ||
        contract.value("schema_id", "") !=
            orange::recording_geometry::kRecordingGeometryContractSchemaId ||
        contract.value("schema_version", 0) !=
            orange::recording_geometry::kRecordingGeometryContractSchemaVersion) {
        std::cerr << "[recording_session] Ignoring invalid recording geometry contract: "
                  << path << std::endl;
        return;
    }
    std::error_code path_error;
    std::filesystem::path absolute_path = std::filesystem::absolute(path, path_error);
    if (path_error) {
        absolute_path = path;
    }
    if (!manifest->contains("metadata") || !(*manifest)["metadata"].is_object()) {
        (*manifest)["metadata"] = nlohmann::json::object();
    }
    (*manifest)["metadata"]["recording_geometry_contract"] = {
        {"schema_id", contract.value("schema_id", "")},
        {"schema_version", contract.value("schema_version", 0)},
        {"status", contract.value("status", "unknown")},
        {"relative_path", "recording_geometry_contract.json"},
        {"path", absolute_path.string()},
        {"sha256", "sha256:" +
            orange::gui::spatial_layout::checksum::sha256_hex(bytes)},
    };
    const auto assets_it = contract.find("materialized_assets");
    if (assets_it != contract.end() && assets_it->is_object()) {
        (*manifest)["metadata"]["recording_geometry_contract"]
            ["materialized_assets"] = *assets_it;
    }
}

int resolve_positive_int_env(const char* name, const int fallback, const int max_value)
{
    const char* raw = std::getenv(name);
    if (!raw || !*raw) {
        return fallback;
    }
    std::string text = trim_ascii_copy(raw);
    if (text.empty()) {
        return fallback;
    }

    char* end = nullptr;
    const long value = std::strtol(text.c_str(), &end, 10);
    while (end && *end && std::isspace(static_cast<unsigned char>(*end)) != 0) {
        ++end;
    }
    if (end == text.c_str() || (end && *end) || value < 1 || value > max_value) {
        std::cerr << "[recording_session] Ignoring invalid " << name << "='"
                  << raw << "'; using " << fallback << std::endl;
        return fallback;
    }
    return static_cast<int>(value);
}

bool resolve_nonnegative_int_env(const char* name, int* value_out)
{
    const char* raw = std::getenv(name);
    if (!raw || !*raw) {
        return false;
    }
    std::string text = trim_ascii_copy(raw);
    if (text.empty()) {
        return false;
    }

    char* end = nullptr;
    const long value = std::strtol(text.c_str(), &end, 10);
    while (end && *end && std::isspace(static_cast<unsigned char>(*end)) != 0) {
        ++end;
    }
    if (end == text.c_str() || (end && *end) || value < 0 ||
        value > std::numeric_limits<int>::max()) {
        std::cerr << "[recording_session] Ignoring invalid " << name << "='"
                  << raw << "'" << std::endl;
        return false;
    }
    if (value_out) {
        *value_out = static_cast<int>(value);
    }
    return true;
}

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

    std::array<char, 512> buffer{};
    while (fgets(buffer.data(), static_cast<int>(buffer.size()), pipe) != nullptr) {
        if (stdout_out) {
            *stdout_out += buffer.data();
        }
    }

    return pclose(pipe) == 0;
}

std::filesystem::path resolve_ffprobe_path()
{
    const std::filesystem::path bundled("/opt/orange/lib/ffmpeg-nvidia/bin/ffprobe");
    std::error_code ec;
    if (std::filesystem::exists(bundled, ec) && !ec) {
        return bundled;
    }
    return "ffprobe";
}

bool count_video_packets(const std::filesystem::path& video_path,
                         uint64_t* packet_count_out)
{
    if (packet_count_out) {
        *packet_count_out = 0;
    }
    if (video_path.empty()) {
        return false;
    }
    std::error_code ec;
    if (!std::filesystem::exists(video_path, ec) || ec) {
        return false;
    }

    const std::string command =
        shell_single_quote(resolve_ffprobe_path().string()) +
        " -v error -select_streams v:0 -count_packets "
        "-show_entries stream=nb_read_packets "
        "-of default=noprint_wrappers=1:nokey=1 " +
        shell_single_quote(video_path.string()) + " 2>/dev/null";

    std::string output;
    if (!read_command_stdout(command, &output)) {
        return false;
    }
    output = trim_ascii_copy(output);
    if (output.empty() || output == "N/A") {
        return false;
    }
    try {
        if (packet_count_out) {
            *packet_count_out = static_cast<uint64_t>(std::stoull(output));
        }
        return true;
    } catch (...) {
        return false;
    }
}

bool parse_strict_positive_recording_frame_id(
    const std::string& text,
    uint64_t* frame_id_out)
{
    if (text.empty()) {
        return false;
    }
    for (const unsigned char c : text) {
        if (std::isdigit(c) == 0) {
            return false;
        }
    }
    try {
        std::size_t parsed = 0;
        const uint64_t frame_id = std::stoull(text, &parsed, 10);
        if (parsed != text.size() || frame_id == 0) {
            return false;
        }
        if (frame_id_out) {
            *frame_id_out = frame_id;
        }
        return true;
    } catch (...) {
        return false;
    }
}

struct MetadataFrameStats {
    uint64_t frame_count = 0;
    uint64_t metadata_row_count = 0;
    uint64_t malformed_metadata_row_count = 0;
    uint64_t first_recording_frame_id = 0;
    uint64_t last_recording_frame_id = 0;
    uint64_t recording_frame_id_gaps = 0;
};

MetadataFrameStats read_metadata_frame_stats_from_bytes(const std::string& metadata_bytes)
{
    MetadataFrameStats stats;
    std::istringstream input(metadata_bytes);

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
        ++stats.metadata_row_count;
        const std::size_t comma = line.find(',');
        const std::string frame_text =
            comma == std::string::npos ? line : line.substr(0, comma);
        uint64_t frame_id = 0;
        if (parse_strict_positive_recording_frame_id(frame_text, &frame_id)) {
            if (stats.frame_count == 0) {
                stats.first_recording_frame_id = frame_id;
            } else if (frame_id > previous_frame_id + 1) {
                stats.recording_frame_id_gaps += frame_id - previous_frame_id - 1;
            }
            previous_frame_id = frame_id;
            stats.last_recording_frame_id = frame_id;
            ++stats.frame_count;
        } else {
            ++stats.malformed_metadata_row_count;
        }
    }
    return stats;
}

MetadataFrameStats read_metadata_frame_stats(const std::filesystem::path& metadata_path)
{
    std::ifstream input(metadata_path, std::ios::binary);
    if (!input) {
        return {};
    }
    std::ostringstream bytes;
    bytes << input.rdbuf();
    return read_metadata_frame_stats_from_bytes(bytes.str());
}

std::string artifact_path_string(const std::filesystem::path& folder,
                                 const std::string& file_name,
                                 const bool relative_paths)
{
    return relative_paths ? file_name : (folder / file_name).string();
}

std::string resolve_camera_preferred_recording_sink_mode(
    const CameraParams* cameras_params,
    const CameraEachSelect* cameras_select,
    const int num_cameras,
    std::vector<std::string>* preferred_serials_out)
{
    if (preferred_serials_out) {
        preferred_serials_out->clear();
    }
    if (!cameras_params || !cameras_select || num_cameras <= 0) {
        return {};
    }

    bool wants_external_ipc = false;
    bool wants_real = false;
    for (int i = 0; i < num_cameras; ++i) {
        if (!cameras_select[i].record) {
            continue;
        }
        const std::string raw_preference =
            cameras_params[i].recording.preferred_sink_mode;
        if (raw_preference.empty()) {
            continue;
        }
        const std::string preference = normalize_recording_sink_mode(raw_preference);
        if (preference == "external_ipc") {
            wants_external_ipc = true;
        } else if (preference == "real") {
            wants_real = true;
        } else {
            continue;
        }
        if (preferred_serials_out) {
            preferred_serials_out->push_back(cameras_params[i].camera_serial);
        }
    }

    if (wants_external_ipc) {
        return "external_ipc";
    }
    if (wants_real) {
        return "real";
    }
    return {};
}

std::string resolve_gui_recording_sink_mode_impl(const AppStorageConfig* app_storage_config,
                                                 const CameraParams* cameras_params,
                                                 const CameraEachSelect* cameras_select,
                                                 const int num_cameras)
{
    std::string requested;
    std::string source = "built_in_default";
    if (app_storage_config &&
        app_storage_config->gui_recording_sink_mode_configured &&
        !app_storage_config->gui_recording_sink_mode.empty()) {
        requested = app_storage_config->gui_recording_sink_mode;
        source = "app_config";
    } else {
        std::vector<std::string> preferred_serials;
        const std::string camera_preference =
            resolve_camera_preferred_recording_sink_mode(
                cameras_params,
                cameras_select,
                num_cameras,
                &preferred_serials);
        if (!camera_preference.empty()) {
            requested = camera_preference;
            source = "camera_preference";
            std::ostringstream serials;
            for (size_t i = 0; i < preferred_serials.size(); ++i) {
                if (i > 0) {
                    serials << ",";
                }
                serials << preferred_serials[i];
            }
            std::cout << "[recording_session] GUI recording sink mode from camera preference: "
                      << requested;
            if (!preferred_serials.empty()) {
                std::cout << " serials=" << serials.str();
            }
            std::cout << std::endl;
        } else if (app_storage_config && !app_storage_config->gui_recording_sink_mode.empty()) {
            requested = app_storage_config->gui_recording_sink_mode;
        }
    }
    if (const char* env = std::getenv("ORANGE_GUI_RECORDING_SINK_MODE")) {
        requested = env;
        source = "environment";
    }
    if (env_flag_enabled("ORANGE_GUI_DIAGNOSTIC_NO_FULL_FRAME")) {
        requested = "immediate_recycle";
        source = "diagnostic_no_full_frame";
    }

    std::string normalized = normalize_recording_sink_mode(requested);
    if (normalized.empty()) {
        std::cerr << "[recording_session] Unsupported GUI recording sink mode '"
                  << requested << "'; using real full-frame recording." << std::endl;
        normalized = "real";
    }
    if (normalized != "real") {
        std::cout << "[recording_session] GUI recording sink mode: " << normalized
                  << " source=" << source
                  << " (full-frame video disabled for non-real sink modes)" << std::endl;
    }
    return normalized;
}

RecordingControlConfig resolve_gui_recording_control(
    const AppStorageConfig* app_storage_config)
{
    RecordingControlConfig config;
    if (app_storage_config) {
        config.record_for_seconds =
            std::max(0, app_storage_config->gui_recording_record_for_seconds);
        config.clip_seconds =
            std::max(0, app_storage_config->gui_recording_clip_seconds);
    }

    int env_value = 0;
    if (resolve_nonnegative_int_env("ORANGE_GUI_RECORD_FOR_SECONDS", &env_value)) {
        config.record_for_seconds = env_value;
    }
    if (resolve_nonnegative_int_env("ORANGE_GUI_CLIP_SECONDS", &env_value)) {
        config.clip_seconds = env_value;
    }
    if (config.clip_seconds > 0 &&
        config.record_for_seconds <= 0 &&
        env_flag_enabled("ORANGE_GUI_AUTORUN") &&
        resolve_nonnegative_int_env("ORANGE_GUI_AUTORUN_RECORD_SECONDS", &env_value) &&
        env_value > 0) {
        config.record_for_seconds = env_value;
        std::cout
            << "[recording_session] Using ORANGE_GUI_AUTORUN_RECORD_SECONDS="
            << env_value
            << " as GUI external recorder record_for_seconds." << std::endl;
    }

    std::string validation_error;
    if (!validate_recording_control_config(
            config,
            &validation_error,
            "GUI ")) {
        std::cerr << "[recording_session] " << validation_error
                  << "; disabling GUI recording_control." << std::endl;
        config = RecordingControlConfig{};
    }

    if (config.enabled()) {
        std::cout << "[recording_session] GUI recording_control: record_for_seconds="
                  << config.record_for_seconds
                  << " clip_seconds=" << config.clip_seconds << std::endl;
    }
    return config;
}

std::string normalize_crop_recording_sink_mode(std::string requested)
{
    requested = trim_ascii_copy(std::move(requested));
    std::transform(requested.begin(), requested.end(), requested.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    if (requested.empty() || requested == "real" || requested == "in_process" ||
        requested == "inprocess") {
        return "in_process";
    }
    if (requested == "external_ipc") {
        return requested;
    }
    return {};
}

std::string resolve_gui_crop_recording_sink_mode()
{
    const char* env = std::getenv("ORANGE_CROP_RECORDING_SINK_MODE");
    const std::string normalized =
        normalize_crop_recording_sink_mode(env ? env : "");
    if (normalized.empty()) {
        std::cerr << "[recording_session] Unsupported crop recording sink mode '"
                  << (env ? env : "")
                  << "'; using in_process crop recording." << std::endl;
        return "in_process";
    }
    if (normalized != "in_process") {
        std::cout << "[recording_session] Crop recording sink mode: "
                  << normalized << std::endl;
    }
    return normalized;
}

int resolve_external_crop_encode_queue_depth()
{
    constexpr int kDefaultQueueDepth = 64;
    constexpr int kMaxQueueDepth = 4096;
    const int depth = resolve_positive_int_env(
        "ORANGE_CROP_EXTERNAL_ENCODE_QUEUE_DEPTH",
        kDefaultQueueDepth,
        kMaxQueueDepth);
    if (depth != kDefaultQueueDepth || std::getenv("ORANGE_CROP_EXTERNAL_ENCODE_QUEUE_DEPTH")) {
        std::cout << "[recording_session] External crop encode queue depth: "
                  << depth << std::endl;
    }
    return depth;
}

std::string external_crop_contract_validation_error(const nlohmann::json& contract)
{
    if (!contract.contains("validation_errors") ||
        !contract["validation_errors"].is_array() ||
        contract["validation_errors"].empty()) {
        if (contract.contains("recording_control") &&
            contract["recording_control"].is_object()) {
            const nlohmann::json& control = contract["recording_control"];
            const int record_for_seconds =
                control.contains("record_for_seconds") &&
                        control["record_for_seconds"].is_number_integer()
                    ? control["record_for_seconds"].get<int>()
                    : 0;
            const int clip_seconds =
                control.contains("clip_seconds") && control["clip_seconds"].is_number_integer()
                    ? control["clip_seconds"].get<int>()
                    : 0;
            RecordingControlConfig config;
            config.record_for_seconds = record_for_seconds;
            config.clip_seconds = clip_seconds;
            std::string validation_error;
            if (!validate_recording_control_config(
                    config,
                    &validation_error,
                    "external crop recorder ")) {
                return "external crop recorder contract invalid; " + validation_error;
            }
        }
        if (contract.contains("rollover") &&
            contract["rollover"].is_object()) {
            const bool rollover_requested =
                contract["rollover"].value("requested", false);
            const nlohmann::json control =
                contract.value("recording_control", nlohmann::json::object());
            const int clip_seconds =
                control.is_object() && control.contains("clip_seconds") &&
                        control["clip_seconds"].is_number_integer()
                    ? control["clip_seconds"].get<int>()
                    : 0;
            if (rollover_requested != (clip_seconds > 0)) {
                return "external crop recorder contract invalid; rollover request does not match recording_control";
            }
        }
        if (contract.contains("streams") && contract["streams"].is_object()) {
            for (auto it = contract["streams"].begin(); it != contract["streams"].end(); ++it) {
                const nlohmann::json& stream = it.value();
                if (!stream.is_object()) {
                    continue;
                }
                if (stream.contains("recording_control") &&
                    stream["recording_control"].is_object()) {
                    const nlohmann::json& control = stream["recording_control"];
                    const int record_for_seconds =
                        control.contains("record_for_seconds") &&
                                control["record_for_seconds"].is_number_integer()
                            ? control["record_for_seconds"].get<int>()
                            : 0;
                    const int clip_seconds =
                        control.contains("clip_seconds") && control["clip_seconds"].is_number_integer()
                            ? control["clip_seconds"].get<int>()
                            : 0;
                    RecordingControlConfig config;
                    config.record_for_seconds = record_for_seconds;
                    config.clip_seconds = clip_seconds;
                    std::string validation_error;
                    if (!validate_recording_control_config(
                            config,
                            &validation_error,
                            "external crop recorder " + it.key() + " ")) {
                        return "external crop recorder contract invalid; " + validation_error;
                    }
                }
                if (stream.contains("rollover") &&
                    stream["rollover"].is_object()) {
                    const bool rollover_requested =
                        stream["rollover"].value("requested", false);
                    const nlohmann::json control =
                        stream.value("recording_control", nlohmann::json::object());
                    const int clip_seconds =
                        control.is_object() && control.contains("clip_seconds") &&
                                control["clip_seconds"].is_number_integer()
                            ? control["clip_seconds"].get<int>()
                            : 0;
                    if (rollover_requested != (clip_seconds > 0)) {
                        return "external crop recorder contract invalid; " + it.key() +
                            " rollover request does not match recording_control";
                    }
                }
            }
        }
        return {};
    }
    std::ostringstream message;
    message << "external crop recorder contract invalid";
    for (const auto& error : contract["validation_errors"]) {
        if (error.is_string()) {
            message << "; " << error.get<std::string>();
        }
    }
    return message.str();
}

nlohmann::json external_crop_recording_control_json(
    const RecordingControlConfig& recording_control)
{
    return {
        {"record_for_seconds", recording_control.record_for_seconds},
        {"clip_seconds", recording_control.clip_seconds}
    };
}

nlohmann::json external_crop_rollover_json(const RecordingControlConfig& recording_control)
{
    if (recording_control.clip_seconds > 0) {
        return {
            {"requested", true},
            {"status", "supported"},
            {"implementation",
             orange::external_recorder::kExternalRecorderRollingImplementation},
            {"seamless_writer_switch", true},
            {"records_during_rollover", true},
            {"boundary", "recording_frame_id"},
            {"output_kind", "crop"},
            {"supported_mode", "rolling_clips"},
            {"rolling_supported", true},
            {"next_writer_preopened", false}
        };
    }
    return {
        {"requested", false},
        {"status", "not_requested"},
        {"implementation", "none"},
        {"seamless_writer_switch", false},
        {"records_during_rollover", false},
        {"output_kind", "crop"},
        {"supported_mode", "single_clip"},
        {"rolling_supported", true}
    };
}

nlohmann::json materialize_external_crop_recorder_contract_for_cameras(
    const std::string& recording_folder,
    const std::string& recording_id,
    const CameraParams* cameras_params,
    const CameraEachSelect* cameras_select,
    const int num_cameras,
    const RecordingControlConfig& recording_control)
{
    const nlohmann::json crop_recording_control =
        external_crop_recording_control_json(recording_control);
    const nlohmann::json crop_rollover =
        external_crop_rollover_json(recording_control);
    nlohmann::json contract = {
        {"schema_id", "orange.external_recorder.contract"},
        {"schema_version", 1},
        {"mode", "diagnostic_ipc_v1"},
        {"supervise_processes", true},
        {"session_id", recording_id},
        {"artifact_root", (std::filesystem::path(recording_folder) / "external_crop_recorder").string()},
        {"require_summary", true},
        {"require_video_sanity", false},
        {"require_merged_mp4", recording_control.clip_seconds <= 0},
        {"require_gop_routing", true},
        {"require_status", true},
        {"require_status_runtime", true},
        {"require_storage_preflight", true},
        {"require_protocol_hello", true},
        {"preserve_shard_mp4s", false},
        {"storage_budget", {
            {"enabled", true},
            {"safety_headroom_ratio", 0.10},
            {"reserved_free_bytes", 500000000000ULL},
            {"metadata_bytes_per_frame", 1024},
            {"raw_nv12_expansion_ratio", 1.10},
            {"require_finite_duration", false},
            {"planned_duration_seconds", 0},
        }},
        {"recording_control", crop_recording_control},
        {"rollover", crop_rollover},
        {"require_recorder_gpu_separate_from_analytics",
         external_crop_recorder_require_separate_gpu_from_env()},
        {"validation_errors", nlohmann::json::array()},
        {"streams", nlohmann::json::object()}
    };

    if (!cameras_params || !cameras_select || num_cameras <= 0) {
        return contract;
    }

    const std::string artifact_root = contract["artifact_root"].get<std::string>();
    const int external_crop_encode_queue_depth = resolve_external_crop_encode_queue_depth();
    for (int i = 0; i < num_cameras; ++i) {
        if (!cameras_select[i].crop_and_encode) {
            continue;
        }
        const CameraParams& camera = cameras_params[i];
        const std::string serial =
            !camera.camera_serial.empty()
                ? camera.camera_serial
                : std::to_string(camera.camera_id);
        if (serial.empty()) {
            continue;
        }
        const std::string stream_id = serial + "_crop";
        const std::string stream_key = stream_id;
        const int crop_size =
            sanitize_camera_crop_size_px(camera.crop_pipeline.crop_size_px);
        const int frame_rate =
            std::max(1, static_cast<int>(camera.frame_rate));
        const int full_frame_gop =
            camera.recording.encode.gop_length > 0
                ? camera.recording.encode.gop_length
                : frame_rate;
        const int analytics_gpu_id =
            camera.gpu_id >= 0 ? camera.gpu_id : 0;
        const int recorder_gpu_id =
            resolve_external_crop_recorder_gpu_id_from_env(serial, analytics_gpu_id);
        const bool same_gpu_as_analytics =
            recorder_gpu_id == analytics_gpu_id;
        const std::string prefix =
            (std::filesystem::path(artifact_root) /
             ("Cam" + serial + "_crop_external")).string();

        contract["streams"][stream_key] = {
            {"stream_id", stream_id},
            {"stream_kind", "crop"},
            {"output_kind", "crop"},
            {"camera_serial", serial},
            // Use a crop-suffixed process/env key so the supervisor's
            // environment variables cannot overwrite full-frame sockets.
            {"env_key", stream_id},
            {"analytics_gpu_id", analytics_gpu_id},
            {"recorder_gpu_id", recorder_gpu_id},
            {"same_gpu_as_analytics", same_gpu_as_analytics},
            {"expected_shard_gpu_ids", nlohmann::json::array({recorder_gpu_id})},
            {"routing_policy", "single_shard"},
            {"summary_json", prefix + "_summary.json"},
            {"status_json", prefix + "_status.json"},
            {"video_sanity_json", prefix + "_video_sanity.json"},
            {"mp4", prefix + ".mp4"},
            {"mp4_keyframe", prefix + "_keyframe.json"},
            {"detach_csv", prefix + "_detach.csv"},
            {"encode_csv", prefix + "_encode.csv"},
            {"gop_routing_csv", prefix + "_gop_routing.csv"},
            {"recorder_log", prefix + "_recorder.log"},
            {"socket_path", "/tmp/orange_external_recorder_" + serial + "_crop.sock"},
            {"encode_fps", frame_rate},
            {"encode_max_fps", 0},
            {"encode_queue_depth", external_crop_encode_queue_depth},
            {"prewarm_slots", 4},
            {"prewarm_bytes", static_cast<uint64_t>(crop_size) * static_cast<uint64_t>(crop_size)},
            {"prewarm_peer_copy", true},
            {"recording_control", crop_recording_control},
            {"rollover", crop_rollover},
            {"codec", "hevc"},
            {"preset", "p7"},
            {"tuning", "lossless"},
            {"gop", 1},
            {"max_pending_gops", 8},
            {"max_pending_bytes", 268435456},
            {"max_pending_frontier_age_ms", 2000},
            {"max_writer_queue_packets", 512},
            {"max_writer_queue_bytes", 134217728},
            {"terminal_tail_coalesce_frames",
             recording_control.clip_seconds > 0
                 ? static_cast<uint64_t>(std::max(1, full_frame_gop))
                 : 0ULL},
            {"bitrate_bps", 150000000},
            {"max_bitrate_bps", 150000000},
            {"vbv_buffer_size", 150000000}
        };
        if (external_crop_recorder_same_gpu_disallowed(
                analytics_gpu_id,
                recorder_gpu_id)) {
            contract["validation_errors"].push_back(
                "Cam" + serial +
                " external crop recorder GPU " + std::to_string(recorder_gpu_id) +
                " matches analytics GPU " + std::to_string(analytics_gpu_id) +
                "; set ORANGE_CROP_EXTERNAL_RECORDER_GPU_ID or " +
                "ORANGE_CROP_EXTERNAL_RECORDER_GPU_ID_CAM_" + serial +
                " to a different GPU");
        }
    }
    return contract;
}

nlohmann::json resolve_gui_external_recorder_contract_config(
    const AppStorageConfig* app_storage_config,
    std::string* source_out,
    std::string* status_out)
{
    if (source_out) {
        source_out->clear();
    }
    if (status_out) {
        status_out->clear();
    }

    nlohmann::json contract = nlohmann::json::object();
    if (app_storage_config &&
        app_storage_config->gui_external_recorder_contract.is_object() &&
        !app_storage_config->gui_external_recorder_contract.empty()) {
        contract = orange::external_recorder::ExtractExternalRecorderContractObject(
            app_storage_config->gui_external_recorder_contract);
        if (source_out) {
            *source_out = "app_config_inline";
        }
    }

    std::string contract_path =
        app_storage_config ? app_storage_config->gui_external_recorder_contract_path : std::string();
    if (const char* env = std::getenv("ORANGE_GUI_EXTERNAL_RECORDER_CONTRACT"); env && *env) {
        contract_path = env;
    }
    if (const char* env = std::getenv("ORANGE_GUI_EXTERNAL_RECORDER_CONTRACT_PATH"); env && *env) {
        contract_path = env;
    }

    contract_path = trim_ascii_copy(contract_path);
    if (!contract_path.empty()) {
        std::string error;
        if (orange::external_recorder::ReadExternalRecorderContractConfigFile(
                contract_path,
                &contract,
                &error)) {
            if (source_out) {
                *source_out = contract_path;
            }
        } else if (status_out) {
            *status_out = "Failed to load GUI external recorder contract " +
                          contract_path + ": " + error;
        }
    }
    return contract.is_object() ? contract : nlohmann::json::object();
}

bool write_json_file(const std::filesystem::path& path,
                     const nlohmann::json& payload,
                     std::string* error_out)
{
    orange::ScopedFsuid fsuid_guard;
    (void)fsuid_guard;

    std::error_code create_error;
    std::filesystem::create_directories(path.parent_path(), create_error);
    if (create_error && !std::filesystem::exists(path.parent_path())) {
        if (error_out) {
            *error_out = "Failed to create parent directory for " +
                         path.string() + ": " + create_error.message();
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
    output << payload.dump(2) << "\n";
    if (!output.good()) {
        if (error_out) {
            *error_out = "Failed to write " + path.string();
        }
        return false;
    }
    return true;
}

std::string csv_escape_value(const nlohmann::json& value)
{
    std::string text;
    if (value.is_null()) {
        text.clear();
    } else if (value.is_string()) {
        text = value.get<std::string>();
    } else if (value.is_boolean()) {
        text = value.get<bool>() ? "true" : "false";
    } else {
        text = value.dump();
    }

    const bool needs_quotes =
        text.find_first_of(",\"\n\r") != std::string::npos;
    if (!needs_quotes) {
        return text;
    }

    std::string escaped;
    escaped.reserve(text.size() + 2);
    escaped.push_back('"');
    for (char c : text) {
        if (c == '"') {
            escaped.push_back('"');
        }
        escaped.push_back(c);
    }
    escaped.push_back('"');
    return escaped;
}

bool write_csv_file(const std::filesystem::path& path,
                    const std::vector<std::string>& columns,
                    const nlohmann::json& rows,
                    std::string* error_out)
{
    orange::ScopedFsuid fsuid_guard;
    (void)fsuid_guard;

    std::error_code create_error;
    std::filesystem::create_directories(path.parent_path(), create_error);
    if (create_error && !std::filesystem::exists(path.parent_path())) {
        if (error_out) {
            *error_out = "Failed to create parent directory for " +
                         path.string() + ": " + create_error.message();
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

    for (std::size_t i = 0; i < columns.size(); ++i) {
        if (i > 0) {
            output << ',';
        }
        output << columns[i];
    }
    output << '\n';

    if (rows.is_array()) {
        for (const nlohmann::json& row : rows) {
            for (std::size_t i = 0; i < columns.size(); ++i) {
                if (i > 0) {
                    output << ',';
                }
                if (row.is_object()) {
                    const auto it = row.find(columns[i]);
                    if (it != row.end()) {
                        output << csv_escape_value(*it);
                    }
                }
            }
            output << '\n';
        }
    }

    if (!output.good()) {
        if (error_out) {
            *error_out = "Failed to write " + path.string();
        }
        return false;
    }
    return true;
}

nlohmann::json default_rolling_index_manifest_json(const std::size_t clip_count,
                                                   const std::size_t row_count)
{
    return {
        {"schema_id", "orange.recording_session.indexes"},
        {"schema_version", 1},
        {"clip_index_json", "recording_clip_index.json"},
        {"clip_index_csv", "recording_clip_index.csv"},
        {"row_granularity", "clip_camera"},
        {"path_style", "relative_to_recording_folder"},
        {"clip_count", clip_count},
        {"row_count", row_count}
    };
}

nlohmann::json make_clip_index_row(const nlohmann::json& manifest,
                                   const nlohmann::json& clip,
                                   const std::string& camera_serial,
                                   const nlohmann::json& camera_artifact)
{
    const nlohmann::json rollover =
        clip.value("rollover", nlohmann::json::object());
    const std::string clip_folder =
        clip.value("recording_folder", std::string());
    const std::filesystem::path clip_manifest_path =
        clip_folder.empty()
            ? std::filesystem::path()
            : std::filesystem::path(clip_folder) / "clip_manifest.json";
    const bool has_packet_count =
        camera_artifact.contains("packet_count") &&
        !camera_artifact["packet_count"].is_null();
    const std::string packet_count_source =
        camera_artifact.value(
            "packet_count_source",
            has_packet_count ? std::string("unknown") : std::string("not_collected"));

    nlohmann::json row = {
        {"session_id", manifest.value("session_id", std::string())},
        {"producer", manifest.value("producer", std::string())},
        {"clip_index", clip.value("clip_index", 0)},
        {"clip_id", clip.value("clip_id", std::string())},
        {"camera_serial", camera_serial},
        {"status", clip.value("status", std::string())},
        {"start_reason", clip.value("start_reason", std::string())},
        {"stop_reason", clip.value("stop_reason", std::string())},
        {"final_clip", clip.value("final_clip", false)},
        {"timed_stop_hit", clip.value("timed_stop_hit", false)},
        {"drain_completed", clip.value("drain_completed", false)},
        {"started_at_utc", clip.value("started_at_utc", std::string())},
        {"stop_requested_at_utc", clip.value("stop_requested_at_utc", std::string())},
        {"finalized_at_utc", clip.value("finalized_at_utc", std::string())},
        {"started_at_elapsed_s", clip.value("started_at_elapsed_s", 0.0)},
        {"stop_requested_at_elapsed_s", clip.value("stop_requested_at_elapsed_s", 0.0)},
        {"finalized_at_elapsed_s", clip.value("finalized_at_elapsed_s", 0.0)},
        {"requested_duration_s", clip.value("requested_duration_s", 0.0)},
        {"actual_duration_s", clip.value("actual_duration_s", 0.0)},
        {"drain_duration_s", clip.value("drain_duration_s", 0.0)},
        {"first_recording_frame_id", camera_artifact.value("first_recording_frame_id", 0ULL)},
        {"last_recording_frame_id", camera_artifact.value("last_recording_frame_id", 0ULL)},
        {"frame_count", camera_artifact.value("frame_count", 0ULL)},
        {"recording_frame_id_gaps", camera_artifact.value("recording_frame_id_gaps", 0ULL)},
        {"packet_count", has_packet_count ? camera_artifact["packet_count"] : nlohmann::json(nullptr)},
        {"packet_count_source", packet_count_source},
        {"rollover_request_id", rollover.value("request_id", 0ULL)},
        {"rollover_at_recording_frame_id", rollover.value("rollover_at_recording_frame_id", 0ULL)},
        {"clip_directory", clip.value("directory", std::string())},
        {"clip_recording_folder", clip_folder},
        {"clip_manifest_path", clip_manifest_path.string()},
        {"video", camera_artifact.value("video", std::string())},
        {"metadata", camera_artifact.value("metadata", std::string())},
        {"keyframes", camera_artifact.value("keyframes", std::string())}
    };
    return row;
}

}  // namespace

std::string resolve_gui_recording_sink_mode(const AppStorageConfig* app_storage_config,
                                            const CameraParams* cameras_params,
                                            const CameraEachSelect* cameras_select,
                                            const int num_cameras)
{
    return resolve_gui_recording_sink_mode_impl(
        app_storage_config,
        cameras_params,
        cameras_select,
        num_cameras);
}

nlohmann::json build_recording_control_json(const RecordingControlConfig& config)
{
    return {
        {"record_for_seconds", config.record_for_seconds},
        {"clip_seconds", config.clip_seconds}
    };
}

bool validate_recording_control_config(const RecordingControlConfig& config,
                                       std::string* error_out,
                                       const std::string& context)
{
    const std::string prefix = context.empty() ? "" : context + ": ";
    if (config.record_for_seconds < 0) {
        if (error_out) {
            *error_out = prefix + "recording_control.record_for_seconds must be >= 0";
        }
        return false;
    }
    if (config.clip_seconds < 0) {
        if (error_out) {
            *error_out = prefix + "recording_control.clip_seconds must be >= 0";
        }
        return false;
    }
    if (config.clip_seconds > 0 && config.record_for_seconds <= 0) {
        if (error_out) {
            *error_out = prefix +
                "recording_control.clip_seconds requires record_for_seconds > 0";
        }
        return false;
    }
    return true;
}

namespace {

nlohmann::json build_camera_artifact_json(
    const RecordingSessionCameraArtifact& camera)
{
    nlohmann::json out = {
        {"video", camera.video_path},
        {"metadata", camera.metadata_path},
        {"keyframes", camera.keyframe_path}
    };
    if (camera.frame_count > 0 ||
        camera.first_recording_frame_id > 0 ||
        camera.last_recording_frame_id > 0) {
        out["frame_count"] = camera.frame_count;
        out["first_recording_frame_id"] = camera.first_recording_frame_id;
        out["last_recording_frame_id"] = camera.last_recording_frame_id;
        out["recording_frame_id_gaps"] = camera.recording_frame_id_gaps;
    }
    if (!camera.packet_count_source.empty()) {
        out["packet_count"] = camera.packet_count;
        out["packet_count_source"] = camera.packet_count_source;
    }
    return out;
}

constexpr int64_t kTaiMinusUtcSecondsForClockRuleV1 = 37;
constexpr int64_t kNanosecondsPerSecond = 1000000000LL;
constexpr int64_t kCameraRealtimeOffsetToleranceNs = 1000000000LL;
constexpr int64_t kCameraRealtimeOffsetMaxSpanNs = 1000000000LL;
constexpr int64_t kPtpOffsetMaxAbsNs = 10000000LL;
constexpr int64_t kLatchMinusFrameMinNs = -10000000LL;
constexpr int64_t kLatchMinusFrameMaxNs = 1000000000LL;

bool json_integer(const nlohmann::json& object,
                  const char* key,
                  int64_t* value_out)
{
    if (!object.is_object() || !key || !value_out || !object.contains(key) ||
        (!object[key].is_number_integer() && !object[key].is_number_unsigned())) {
        return false;
    }
    try {
        *value_out = object[key].get<int64_t>();
        return true;
    } catch (...) {
        return false;
    }
}

bool json_number(const nlohmann::json& object,
                 const char* key,
                 long double* value_out)
{
    if (!object.is_object() || !key || !value_out || !object.contains(key) ||
        !object[key].is_number()) {
        return false;
    }
    try {
        *value_out = object[key].get<long double>();
        return true;
    } catch (...) {
        return false;
    }
}

uint64_t stats_samples(const nlohmann::json& stats)
{
    if (!stats.is_object() || !stats.contains("samples") ||
        (!stats["samples"].is_number_unsigned() &&
         !stats["samples"].is_number_integer())) {
        return 0;
    }
    try {
        return stats["samples"].get<uint64_t>();
    } catch (...) {
        return 0;
    }
}

std::string lower_ascii_copy(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return value;
}

bool readback_is_stable_and_enabled(const nlohmann::json& readback,
                                    const bool mode_readback)
{
    if (!readback.is_object() || readback.value("samples", 0ULL) == 0) {
        return false;
    }
    if (readback.value("changes", 0ULL) != 0 ||
        !readback.contains("last") || !readback["last"].is_string()) {
        return false;
    }
    const std::string value = lower_ascii_copy(readback["last"].get<std::string>());
    if (mode_readback) {
        return !value.empty() && value != "off" && value != "disabled";
    }
    return value == "slave" || value == "master";
}

std::pair<uint64_t, uint64_t> recording_frame_range_for_camera(
    const nlohmann::json& manifest,
    const std::string& camera_serial,
    const nlohmann::json& camera_evidence)
{
    uint64_t first = 0;
    uint64_t last = 0;
    const auto include_artifact = [&](const nlohmann::json& artifact) {
        if (!artifact.is_object()) {
            return;
        }
        const uint64_t artifact_first = artifact.value(
            "first_recording_frame_id", 0ULL);
        const uint64_t artifact_last = artifact.value(
            "last_recording_frame_id", 0ULL);
        if (artifact_first > 0 && (first == 0 || artifact_first < first)) {
            first = artifact_first;
        }
        if (artifact_last > last) {
            last = artifact_last;
        }
    };

    const nlohmann::json camera_artifacts = manifest.value(
        "camera_artifacts", nlohmann::json::object());
    if (camera_artifacts.is_object() && camera_artifacts.contains(camera_serial)) {
        include_artifact(camera_artifacts[camera_serial]);
    }

    const nlohmann::json clips = manifest.value("clips", nlohmann::json::array());
    if (clips.is_array()) {
        for (const nlohmann::json& clip : clips) {
            if (!clip.is_object()) {
                continue;
            }
            const nlohmann::json clip_camera_artifacts = clip.value(
                "camera_artifacts", nlohmann::json::object());
            if (clip_camera_artifacts.is_object() &&
                clip_camera_artifacts.contains(camera_serial)) {
                include_artifact(clip_camera_artifacts[camera_serial]);
                continue;
            }
            const nlohmann::json artifacts = clip.value(
                "artifacts", nlohmann::json::object());
            const nlohmann::json metadata = artifacts.value(
                "metadata", nlohmann::json::object());
            if (metadata.is_object() && metadata.contains(camera_serial)) {
                const nlohmann::json rollover = clip.value(
                    "rollover", nlohmann::json::object());
                include_artifact(rollover);
            }
        }
    }

    if (last == 0 && camera_evidence.is_object()) {
        last = camera_evidence.value("last_recording_frame_id", 0ULL);
        if (last > 0) {
            first = 1;
        }
    }
    return {first, last};
}

nlohmann::json build_camera_clock_descriptor(
    const std::string& camera_serial,
    const nlohmann::json& session_sync,
    const nlohmann::json& camera_evidence,
    const nlohmann::json& host_ptp_evidence)
{
    const nlohmann::json empty_object = nlohmann::json::object();
    const nlohmann::json& ptp_offset = camera_evidence.is_object()
        ? camera_evidence.value("ptp_offset_ns", empty_object) : empty_object;
    const nlohmann::json& latch_minus_frame = camera_evidence.is_object()
        ? camera_evidence.value("latch_minus_frame_ns", empty_object) : empty_object;
    const nlohmann::json& camera_minus_realtime = camera_evidence.is_object()
        ? camera_evidence.value("recording_camera_minus_realtime_ns", empty_object)
        : empty_object;
    const nlohmann::json& ptp_mode_readback = camera_evidence.is_object()
        ? camera_evidence.value("ptp_mode_readback", empty_object) : empty_object;
    const nlohmann::json& ptp_status_readback = camera_evidence.is_object()
        ? camera_evidence.value("ptp_status_readback", empty_object) : empty_object;

    const bool session_ptp_enabled =
        session_sync.is_object() && session_sync.value("camera_sync_enabled", false) &&
        session_sync.value("mode", std::string("none")) != "none";
    const bool camera_ptp_enabled =
        camera_evidence.is_object() &&
        camera_evidence.value("sync_camera_enabled", false);
    const bool camera_evidence_available = camera_evidence.is_object() &&
        !camera_evidence.empty();
    const bool source_summary_finalized =
        camera_evidence_available && camera_evidence.value("finalized", false);

    int64_t ptp_offset_min = 0;
    int64_t ptp_offset_max = 0;
    const bool ptp_offset_bounded =
        stats_samples(ptp_offset) > 0 &&
        json_integer(ptp_offset, "min", &ptp_offset_min) &&
        json_integer(ptp_offset, "max", &ptp_offset_max) &&
        std::fabs(static_cast<long double>(ptp_offset_min)) <= kPtpOffsetMaxAbsNs &&
        std::fabs(static_cast<long double>(ptp_offset_max)) <= kPtpOffsetMaxAbsNs;

    int64_t latch_min = 0;
    int64_t latch_max = 0;
    const bool latch_agreement =
        stats_samples(latch_minus_frame) > 0 &&
        json_integer(latch_minus_frame, "min", &latch_min) &&
        json_integer(latch_minus_frame, "max", &latch_max) &&
        latch_min >= kLatchMinusFrameMinNs && latch_max <= kLatchMinusFrameMaxNs;

    long double camera_realtime_mean = 0.0;
    int64_t camera_realtime_min = 0;
    int64_t camera_realtime_max = 0;
    const nlohmann::json host_time_properties = host_ptp_evidence.is_object()
        ? host_ptp_evidence.value(
            "time_properties", nlohmann::json::object())
        : nlohmann::json::object();
    const bool host_management_available =
        host_ptp_evidence.is_object() &&
        host_ptp_evidence.value("available", false);
    const bool host_ptp_timescale =
        host_management_available &&
        host_time_properties.value("ptpTimescale", false);
    const bool host_utc_offset_valid =
        host_ptp_timescale &&
        host_time_properties.value("currentUtcOffsetValid", false);
    const int64_t advertised_utc_offset_seconds =
        host_time_properties.value(
            "currentUtcOffset", kTaiMinusUtcSecondsForClockRuleV1);
    nlohmann::json host_ptp_evidence_snapshot = host_ptp_evidence;
    if (host_ptp_evidence_snapshot.is_object()) {
        host_ptp_evidence_snapshot.erase("raw_output");
    }
    const int64_t expected_offset_seconds = host_utc_offset_valid
        ? advertised_utc_offset_seconds : kTaiMinusUtcSecondsForClockRuleV1;
    const int64_t expected_offset_ns =
        expected_offset_seconds * kNanosecondsPerSecond;
    const bool camera_realtime_offset_matches =
        stats_samples(camera_minus_realtime) > 0 &&
        json_number(camera_minus_realtime, "mean", &camera_realtime_mean) &&
        json_integer(camera_minus_realtime, "min", &camera_realtime_min) &&
        json_integer(camera_minus_realtime, "max", &camera_realtime_max) &&
        std::fabs(camera_realtime_mean - expected_offset_ns) <=
            kCameraRealtimeOffsetToleranceNs &&
        camera_realtime_max >= camera_realtime_min &&
        camera_realtime_max - camera_realtime_min <=
            kCameraRealtimeOffsetMaxSpanNs;

    const bool ptp_readbacks_stable =
        readback_is_stable_and_enabled(ptp_mode_readback, true) &&
        readback_is_stable_and_enabled(ptp_status_readback, false);
    const bool base_camera_ptp_evidence =
        session_ptp_enabled && camera_ptp_enabled && camera_evidence_available &&
        ptp_offset_bounded && latch_agreement && camera_realtime_offset_matches &&
        ptp_readbacks_stable;
    const bool direct_tai =
        base_camera_ptp_evidence && host_ptp_timescale && host_utc_offset_valid;
    const bool ptp_timescale_untraceable =
        base_camera_ptp_evidence && host_ptp_timescale && !host_utc_offset_valid;
    const bool inferred_tai =
        base_camera_ptp_evidence && !host_management_available;
    const bool classified_tai = direct_tai || inferred_tai;
    const bool classified_ptp = classified_tai || ptp_timescale_untraceable;
    const std::string classification = classified_tai
        ? "ieee1588_tai"
        : (ptp_timescale_untraceable
            ? "ieee1588_ptp_timescale_untraceable" : "device_defined");
    const std::string semantic_authority = direct_tai
        ? "host_ptp_management_dataset_plus_camera_lock_evidence"
        : (inferred_tai
            ? "inferred_from_recording_evidence"
            : (ptp_timescale_untraceable
                ? "ptp_timescale_advertised_utc_relationship_unvalidated"
                : "epoch_unspecified"));

    nlohmann::json descriptor = {
        {"clock_id", "camera_" + camera_serial},
        {"unit", "nanosecond"},
        {"clock_domain", classified_ptp ? "camera_hardware_ptp" : "camera_hardware"},
        {"source_field", "Emergent::CEmergentFrame.timestamp"},
        {"producer", "camera_firmware_via_emergent_esdk"},
        {"orange_transformation", "none"},
        {"classification", classification},
        {"origin", classified_tai
            ? nlohmann::json("1970-01-01T00:00:00 TAI")
            : (ptp_timescale_untraceable
                ? nlohmann::json("1970-01-01T00:00:00 PTP epoch")
                : nlohmann::json(nullptr))},
        {"timescale", classified_tai
            ? "TAI" : (ptp_timescale_untraceable ? "PTP" : "device_defined")},
        {"semantic_authority", semantic_authority},
        {"semantic_authority_by_property", {
            {"unit", "orange_deployment_contract"},
            {"source_field", "orange_producer_declared"},
            {"clock_domain", classified_ptp
                ? semantic_authority : "device_declared"},
            {"origin", classified_ptp ? semantic_authority : "unspecified"},
            {"timescale", classified_ptp ? semantic_authority : "unspecified"}
        }},
        {"inference", {
            {"rule_id", "orange_camera_clock_classification_v2"},
            {"result", direct_tai
                ? "direct_ptp_time_properties_pass"
                : (inferred_tai
                    ? "fallback_inference_pass"
                    : (ptp_timescale_untraceable
                        ? "ptp_timescale_present_utc_offset_invalid"
                        : "insufficient_evidence"))},
            {"expected_tai_minus_utc_seconds", expected_offset_seconds},
            {"checks", {
                {"session_ptp_enabled", session_ptp_enabled},
                {"camera_ptp_enabled", camera_ptp_enabled},
                {"camera_evidence_available", camera_evidence_available},
                {"source_summary_finalized", source_summary_finalized},
                {"ptp_offset_bounded", ptp_offset_bounded},
                {"latch_agreement", latch_agreement},
                {"camera_minus_realtime_matches", camera_realtime_offset_matches},
                {"ptp_readbacks_stable", ptp_readbacks_stable},
                {"host_management_evidence_available", host_management_available},
                {"host_ptp_timescale", host_ptp_timescale},
                {"host_current_utc_offset_valid", host_utc_offset_valid}
            }},
            {"thresholds_ns", {
                {"camera_minus_realtime_tolerance", kCameraRealtimeOffsetToleranceNs},
                {"camera_minus_realtime_max_span", kCameraRealtimeOffsetMaxSpanNs},
                {"ptp_offset_max_abs", kPtpOffsetMaxAbsNs},
                {"latch_minus_frame_min", kLatchMinusFrameMinNs},
                {"latch_minus_frame_max", kLatchMinusFrameMaxNs}
            }}
        }},
        {"evidence_snapshot", {
            {"sync_camera_enabled", camera_evidence.value("sync_camera_enabled", false)},
            {"finalized", camera_evidence.value("finalized", false)},
            {"ptp_offset_ns", ptp_offset},
            {"latch_minus_frame_ns", latch_minus_frame},
            {"recording_camera_minus_realtime_ns", camera_minus_realtime},
            {"ptp_mode_readback", ptp_mode_readback},
            {"ptp_status_readback", ptp_status_readback},
            {"host_ptp_management_evidence", host_ptp_evidence_snapshot},
            {"ptp_readback_observations", camera_evidence.value(
                "ptp_readback_observations", nlohmann::json::array())}
        }}
    };
    return descriptor;
}

std::filesystem::path resolve_manifest_artifact_path(
    const std::filesystem::path& manifest_path,
    const std::string& value)
{
    if (value.empty()) {
        return {};
    }
    const std::filesystem::path path(value);
    return path.is_absolute() ? path : manifest_path.parent_path() / path;
}

bool add_finalized_frame_identity_contract(
    nlohmann::json* manifest,
    const std::filesystem::path& manifest_path,
    std::string* error_out)
{
    if (!manifest || !manifest->is_object()) {
        return true;
    }
    const auto existing = manifest->find("frame_identity_contract");
    if (existing != manifest->end() && existing->is_object() &&
        existing->value("schema_id", std::string()) ==
            "orange.recording.frame_identity" &&
        existing->value("schema_version", 0) == 1 &&
        existing->value("status", std::string()) == "finalized") {
        return true;
    }

    const bool recording_finalized =
        manifest->value("status", std::string()) == "completed";
    const nlohmann::json backend = manifest->value(
        "recording_backend", nlohmann::json::object());
    const bool external_ipc =
        backend.is_object() && backend.value("mode", std::string()) == "external_ipc";

    bool proof_required = false;
    const std::filesystem::path external_contract_path =
        manifest_path.parent_path() / "external_recorder_contract.json";
    std::string external_contract_bytes;
    std::string external_contract_error;
    if (orange::gui::spatial_layout::checksum::read_file(
            external_contract_path,
            &external_contract_bytes,
            &external_contract_error)) {
        const nlohmann::json external_contract = nlohmann::json::parse(
            external_contract_bytes, nullptr, false);
        if (external_contract.is_object()) {
            proof_required = external_contract.value(
                "require_frame_identity_proof", false);
        }
    }

    nlohmann::json contract = {
        {"schema_id", "orange.recording.frame_identity"},
        {"schema_version", 1},
        {"status", recording_finalized ? "finalized" : "pending"},
        {"canonical_field", "recording_frame_id"},
        {"scope", "recording_session_and_camera_stream"},
        {"assignment_event", "orange_acquisition_recording_frame_sequence"},
        {"legacy_aliases", {{"frame_id", "recording_frame_id"}}},
        {"timestamp_fields", {"timestamp", "timestamp_sys"}},
        {"continuity_policy", "encoded_subset"},
        {"recording_frame_id_gaps_allowed", true},
        {"camera_streams", nlohmann::json::object()},
        {"verification", {
            {"required", proof_required},
            {"result", recording_finalized ? "not_evaluated" : "pending"},
        }},
    };

    const nlohmann::json summary_paths =
        backend.is_object()
            ? backend.value("summary_json", nlohmann::json::object())
            : nlohmann::json::object();
    const nlohmann::json cameras = manifest->value(
        "cameras", nlohmann::json::array());
    bool all_verified = true;
    bool any_proof = false;
    if (cameras.is_array()) {
        for (const nlohmann::json& camera_value : cameras) {
            if (!camera_value.is_string()) {
                continue;
            }
            const std::string serial = camera_value.get<std::string>();
            nlohmann::json stream = {
                {"camera_serial", serial},
                {"backend", external_ipc ? "external_ipc" : "in_process"},
                {"verification_status", external_ipc
                    ? "summary_unavailable" : "producer_declared"},
            };
            if (external_ipc && summary_paths.is_object() &&
                summary_paths.contains(serial) && summary_paths[serial].is_string()) {
                const std::filesystem::path summary_path =
                    resolve_manifest_artifact_path(
                        manifest_path, summary_paths[serial].get<std::string>());
                std::string summary_bytes;
                std::string summary_error;
                if (orange::gui::spatial_layout::checksum::read_file(
                        summary_path, &summary_bytes, &summary_error)) {
                    const nlohmann::json summary = nlohmann::json::parse(
                        summary_bytes, nullptr, false);
                    const nlohmann::json proof = summary.is_object()
                        ? summary.value(
                            "frame_identity_proof", nlohmann::json::object())
                        : nlohmann::json::object();
                    const nlohmann::json binding = proof.is_object()
                        ? proof.value("video_binding", nlohmann::json::object())
                        : nlohmann::json::object();
                    const uint64_t frames_encoded = summary.is_object()
                        ? summary.value("frames_encoded", 0ULL) : 0ULL;
                    const bool proof_present =
                        proof.is_object() && !proof.empty();
                    const bool proof_valid =
                        proof_present &&
                        proof.value("schema_id", std::string()) ==
                            "orange.external_recorder.frame_identity_proof" &&
                        proof.value("schema_version", 0) == 1 &&
                        proof.value("status", std::string()) == "passed" &&
                        proof.value("canonical_field", std::string()) ==
                            "recording_frame_id" &&
                        binding.is_object() &&
                        binding.value("verified", false) &&
                        binding.value("encoded_video_frames", 0ULL) == frames_encoded &&
                        binding.value("submitted_frame_identities", 0ULL) == frames_encoded &&
                        binding.value("returned_identity_matches", 0ULL) == frames_encoded &&
                        binding.value("identity_mismatches", 1ULL) == 0 &&
                        binding.value("outstanding_submitted_identities", 1ULL) == 0 &&
                        binding.value("metadata_rows", 0ULL) == frames_encoded &&
                        binding.value("packets_written", 0ULL) == frames_encoded;
                    any_proof = any_proof || proof_present;
                    all_verified = all_verified && proof_valid;
                    stream["summary"] = {
                        {"path", summary_path.string()},
                        {"sha256", "sha256:" +
                            orange::gui::spatial_layout::checksum::sha256_hex(
                                summary_bytes)},
                    };
                    stream["verification_status"] = proof_valid
                        ? "passed"
                        : (proof_present ? "invalid" : "legacy_unproven");
                    if (proof_present) {
                        stream["proof"] = proof;
                    }
                } else {
                    all_verified = false;
                    stream["summary"] = {
                        {"path", summary_path.string()},
                        {"available", false},
                    };
                }
            } else if (external_ipc) {
                all_verified = false;
            }
            contract["camera_streams"][serial] = std::move(stream);
        }
    }

    if (external_ipc && recording_finalized) {
        const bool accepted = all_verified && any_proof;
        contract["verification"]["result"] = accepted
            ? "passed"
            : (any_proof ? "failed" : "legacy_unproven");
        if (!accepted && (proof_required || any_proof)) {
            if (error_out) {
                *error_out =
                    "finalized external recording lacks a complete returned-NVENC "
                    "frame identity proof";
            }
            return false;
        }
    } else if (recording_finalized) {
        contract["verification"]["result"] = "producer_declared";
    }

    (*manifest)["frame_identity_contract"] = std::move(contract);
    return true;
}

// This is deliberately a sibling of (rather than an amendment to)
// orange.recording.frame_identity v1.  The latter is already an emitted
// artifact contract.  The mapping below is the narrower, dense-recording
// authority Citrus needs to turn a current recording_frame_id into a
// zero-based source_acquisition_frame_index.
std::string canonical_json_sha256(const nlohmann::json& value)
{
    // nlohmann::json objects use ordered keys.  This is therefore compact,
    // sorted-key UTF-8 JSON with no trailing newline.  Do not hash pretty
    // printed manifest bytes here: this is a semantic-record digest.
    return "sha256:" + orange::gui::spatial_layout::checksum::sha256_hex(
        value.dump(-1, ' ', false, nlohmann::json::error_handler_t::strict));
}

bool recording_relative_artifact_path(
    const std::filesystem::path& manifest_path,
    const std::string& artifact_value,
    std::filesystem::path* resolved_out,
    std::string* relative_out)
{
    if (artifact_value.empty()) {
        return false;
    }
    std::error_code ec;
    const std::filesystem::path manifest_folder = std::filesystem::weakly_canonical(
        manifest_path.parent_path(), ec);
    if (ec) {
        return false;
    }
    const std::filesystem::path artifact = resolve_manifest_artifact_path(
        manifest_path, artifact_value);
    const std::filesystem::path resolved = std::filesystem::weakly_canonical(
        artifact, ec);
    if (ec) {
        return false;
    }
    const std::filesystem::path relative = resolved.lexically_relative(manifest_folder);
    if (relative.empty() || relative.is_absolute()) {
        return false;
    }
    for (const std::filesystem::path& component : relative) {
        if (component == "..") {
            return false;
        }
    }
    if (resolved_out) {
        *resolved_out = resolved;
    }
    if (relative_out) {
        *relative_out = relative.generic_string();
    }
    return true;
}

nlohmann::json acquisition_mapping_coverage_json(const MetadataFrameStats& stats)
{
    return {
        {"first_recording_frame_id", stats.first_recording_frame_id},
        {"last_recording_frame_id", stats.last_recording_frame_id},
        {"total_acquisitions", stats.frame_count},
        {"metadata_row_count", stats.metadata_row_count},
        {"gap_count", stats.recording_frame_id_gaps},
        {"gap_policy", stats.recording_frame_id_gaps == 0 ? "none" : "unsealed"},
    };
}

bool has_duplicate_camera_serials(const nlohmann::json& cameras)
{
    if (!cameras.is_array()) {
        return false;
    }
    std::set<std::string> seen;
    for (const nlohmann::json& camera_value : cameras) {
        if (!camera_value.is_string()) {
            continue;
        }
        const std::string serial = camera_value.get<std::string>();
        if (!serial.empty() && !seen.insert(serial).second) {
            return true;
        }
    }
    return false;
}

bool add_acquisition_index_mapping_contract(
    nlohmann::json* manifest,
    const std::filesystem::path& manifest_path,
    std::string* error_out)
{
    if (!manifest || !manifest->is_object()) {
        return true;
    }
    const auto existing = manifest->find("acquisition_index_mapping");
    if (existing != manifest->end() && existing->is_object() &&
        existing->value("schema_id", std::string()) ==
            "orange.recording.acquisition_index_mapping" &&
        existing->value("schema_version", 0) == 1 &&
        existing->value("status", std::string()) == "finalized") {
        const auto digest = manifest->find("acquisition_index_mapping_sha256");
        if (digest != manifest->end() && digest->is_string() &&
            digest->get<std::string>() == canonical_json_sha256(*existing)) {
            // The source artifact could have changed after finalization.
            // Preserve a verified sealed historical record exactly.
            return true;
        }
        if (error_out) {
            *error_out =
                "finalized acquisition index mapping is missing or has an invalid "
                "semantic-record digest";
        }
        return false;
    }

    const nlohmann::json frame_identity = manifest->value(
        "frame_identity_contract", nlohmann::json::object());
    const std::string recording_id = manifest->value("session_id", std::string());
    const nlohmann::json cameras = manifest->value("cameras", nlohmann::json::array());
    const nlohmann::json camera_artifacts = manifest->value(
        "camera_artifacts", nlohmann::json::object());
    const bool finalized_frame_identity =
        frame_identity.is_object() &&
        frame_identity.value("schema_id", std::string()) ==
            "orange.recording.frame_identity" &&
        frame_identity.value("schema_version", 0) == 1 &&
        frame_identity.value("status", std::string()) == "finalized";

    nlohmann::json mapping = {
        {"schema_id", "orange.recording.acquisition_index_mapping"},
        {"schema_version", 1},
        {"status", "unsealed"},
        {"recording_id", recording_id},
        {"canonicalization", "canonical_json_utf8_sort_keys_compact_v1"},
        {"camera_streams", nlohmann::json::object()},
    };
    if (finalized_frame_identity) {
        mapping["frame_identity_contract_ref"] = "#/frame_identity_contract";
        mapping["frame_identity_contract_sha256"] = canonical_json_sha256(frame_identity);
    }

    std::string root_reason;
    if (manifest->value("status", std::string()) != "completed") {
        root_reason = "recording_not_completed";
    } else if (recording_id.empty()) {
        root_reason = "missing_recording_id";
    } else if (!finalized_frame_identity) {
        root_reason = "frame_identity_contract_unavailable";
    } else if (!cameras.is_array() || cameras.empty() || !camera_artifacts.is_object()) {
        root_reason = "missing_camera_streams";
    } else if (has_duplicate_camera_serials(cameras)) {
        root_reason = "duplicate_camera_serial";
    }

    bool all_streams_sealed = root_reason.empty();
    if (cameras.is_array()) {
        for (const nlohmann::json& camera_value : cameras) {
            if (!camera_value.is_string() || camera_value.get<std::string>().empty()) {
                all_streams_sealed = false;
                if (root_reason.empty()) {
                    root_reason = "invalid_camera_serial";
                }
                continue;
            }
            const std::string serial = camera_value.get<std::string>();
            const nlohmann::json artifact =
                camera_artifacts.value(serial, nlohmann::json::object());
            const std::string metadata_value = artifact.is_object()
                ? artifact.value("metadata", std::string())
                : std::string();
            std::filesystem::path metadata_path;
            std::string relative_metadata_path;
            std::string metadata_bytes;
            std::string metadata_error;
            bool metadata_readable = false;
            std::string stream_reason = root_reason;
            if (stream_reason.empty() && finalized_frame_identity) {
                const nlohmann::json frame_streams = frame_identity.value(
                    "camera_streams", nlohmann::json::object());
                if (!frame_streams.is_object()) {
                    stream_reason = "frame_identity_camera_binding_mismatch";
                } else {
                    const auto identity_stream = frame_streams.find(serial);
                    if (identity_stream == frame_streams.end() ||
                        !identity_stream->is_object() ||
                        identity_stream->value("camera_serial", std::string()) != serial) {
                        stream_reason = "frame_identity_camera_binding_mismatch";
                    }
                }
            }
            if (!recording_relative_artifact_path(
                    manifest_path, metadata_value, &metadata_path, &relative_metadata_path)) {
                if (stream_reason.empty()) {
                    stream_reason = metadata_value.empty()
                        ? "missing_metadata_artifact"
                        : "metadata_artifact_outside_recording_bundle";
                }
            } else if (!orange::gui::spatial_layout::checksum::read_file(
                           metadata_path, &metadata_bytes, &metadata_error)) {
                if (stream_reason.empty()) {
                    stream_reason = "metadata_artifact_unreadable";
                }
            } else {
                metadata_readable = true;
            }

            MetadataFrameStats stats;
            if (metadata_readable) {
                // Parse the exact bytes that are checksum-bound below.  Do not
                // reopen the path after hashing: the mapping's range evidence
                // and its artifact digest must describe one captured payload.
                stats = read_metadata_frame_stats_from_bytes(metadata_bytes);
                if (stream_reason.empty() &&
                    (stats.frame_count == 0 ||
                    stats.malformed_metadata_row_count != 0 ||
                    stats.first_recording_frame_id != 1 ||
                    stats.recording_frame_id_gaps != 0 ||
                    stats.metadata_row_count != stats.frame_count ||
                    stats.last_recording_frame_id != stats.frame_count ||
                    stats.last_recording_frame_id >
                        static_cast<uint64_t>(std::numeric_limits<int64_t>::max()))) {
                    stream_reason = "noncontiguous_recording_frame_ids";
                }
            }

            nlohmann::json stream = {
                {"camera_serial", serial},
                {"coverage", acquisition_mapping_coverage_json(stats)},
            };
            if (metadata_readable) {
                nlohmann::json source_artifact = {
                    {"relative_path", relative_metadata_path},
                    {"media_type", "text/csv"},
                    {"sha256", "sha256:" +
                        orange::gui::spatial_layout::checksum::sha256_hex(metadata_bytes)},
                };
                stream["source_metadata_artifact"] = std::move(source_artifact);
            }
            if (stream_reason.empty()) {
                stream["producer_identity"] = {
                    {"field", "recording_frame_id"},
                    {"dtype", "uint64"},
                    {"index_base", 1},
                    {"scope", "recording_session_and_camera_stream"},
                    {"assignment_event", "orange_acquisition_recording_frame_sequence"},
                };
                stream["destination_identity"] = {
                    {"field", "source_acquisition_frame_index"},
                    {"dtype", "int64"},
                    {"index_base", 0},
                };
                stream["conversion"] = {
                    {"method", "subtract_constant_v1"},
                    {"expression",
                     "source_acquisition_frame_index = recording_frame_id - 1"},
                    {"constant", 1},
                };
            } else {
                stream["status"] = "unsealed";
                stream["reason_code"] = stream_reason;
                all_streams_sealed = false;
                if (root_reason.empty()) {
                    root_reason = "camera_stream_not_sealable_v1";
                }
            }
            mapping["camera_streams"][serial] = std::move(stream);
        }
    }

    if (all_streams_sealed && !mapping["camera_streams"].empty()) {
        mapping["status"] = "finalized";
        (*manifest)["acquisition_index_mapping"] = mapping;
        (*manifest)["acquisition_index_mapping_sha256"] = canonical_json_sha256(mapping);
    } else {
        mapping["reason_code"] = root_reason.empty()
            ? "camera_stream_not_sealable_v1"
            : root_reason;
        (*manifest)["acquisition_index_mapping"] = std::move(mapping);
        // Deliberately remove any old seal if a caller supplied an unsealed
        // replacement record.  v1 must never leave a dense-mapping digest next
        // to evidence that no longer satisfies the dense-sequence predicate.
        manifest->erase("acquisition_index_mapping_sha256");
    }
    return true;
}

void add_finalized_timestamp_clock_contract(
    nlohmann::json* manifest,
    const std::filesystem::path& manifest_path)
{
    if (!manifest || !manifest->is_object()) {
        return;
    }
    const auto existing_contract = manifest->find("timestamp_clock_contract");
    if (existing_contract != manifest->end() && existing_contract->is_object() &&
        existing_contract->value("schema_id", std::string()) ==
            "orange.recording.timestamp_clocks" &&
        existing_contract->value("schema_version", 0) == 1 &&
        existing_contract->value("status", std::string()) == "finalized") {
        return;
    }

    const std::filesystem::path summary_path =
        manifest_path.parent_path() / "ptp_sync_summary.json";
    std::string summary_bytes;
    std::string summary_read_error;
    nlohmann::json ptp_summary = nlohmann::json::object();
    bool summary_valid = false;
    if (orange::gui::spatial_layout::checksum::read_file(
            summary_path, &summary_bytes, &summary_read_error)) {
        ptp_summary = nlohmann::json::parse(summary_bytes, nullptr, false);
        summary_valid = ptp_summary.is_object();
    }

    nlohmann::json contract = {
        {"schema_id", "orange.recording.timestamp_clocks"},
        {"schema_version", 1},
        {"status", "finalized"},
        {"timestamp_fields", {
            {"timestamp", {
                {"semantic_name", "camera_timestamp_ns"},
                {"unit", "nanosecond"},
                {"clock_assignment", "camera_serial"},
                {"csv_columns", {"timestamp"}}
            }},
            {"timestamp_sys", {
                {"semantic_name", "host_receive_timestamp_posix_ns"},
                {"unit", "nanosecond"},
                {"clock_id", "host_realtime"},
                {"csv_columns", {"timestamp_sys"}}
            }}
        }},
        {"clocks", {
            {"host_realtime", {
                {"clock_id", "host_realtime"},
                {"unit", "nanosecond"},
                {"origin", "1970-01-01T00:00:00 UTC"},
                {"timescale", "POSIX"},
                {"clock_domain", "host_system_realtime"},
                {"source_field", "clock_gettime(CLOCK_REALTIME)"},
                {"producer", "orange_acquisition"},
                {"sample_event", "immediately_after_EVT_CameraGetFrame_returns"},
                {"semantic_authority", "producer_declared"},
                {"monotonic", false},
                {"recording_relative", false}
            }}
        }},
        {"camera_assignments", nlohmann::json::object()},
        {"clock_state_intervals", nlohmann::json::object()},
        {"evidence", {
            {"ptp_sync_summary", {
                {"relative_path", "ptp_sync_summary.json"},
                {"available", summary_valid},
                {"sha256", summary_valid
                    ? nlohmann::json("sha256:" +
                        orange::gui::spatial_layout::checksum::sha256_hex(summary_bytes))
                    : nlohmann::json(nullptr)}
            }}
        }}
    };

    const nlohmann::json session_sync = summary_valid
        ? ptp_summary.value("sync", nlohmann::json::object())
        : nlohmann::json::object();
    const nlohmann::json summary_cameras = summary_valid
        ? ptp_summary.value("cameras", nlohmann::json::object())
        : nlohmann::json::object();
    const nlohmann::json host_ptp_evidence = summary_valid
        ? ptp_summary.value(
            "host_ptp_management_evidence", nlohmann::json::object())
        : nlohmann::json::object();
    const nlohmann::json cameras = manifest->value("cameras", nlohmann::json::array());
    if (cameras.is_array()) {
        for (const nlohmann::json& camera_value : cameras) {
            if (!camera_value.is_string()) {
                continue;
            }
            const std::string serial = camera_value.get<std::string>();
            if (serial.empty()) {
                continue;
            }
            const nlohmann::json camera_evidence =
                summary_cameras.is_object() && summary_cameras.contains(serial) &&
                summary_cameras[serial].is_object()
                    ? summary_cameras[serial] : nlohmann::json::object();
            nlohmann::json descriptor = build_camera_clock_descriptor(
                serial, session_sync, camera_evidence, host_ptp_evidence);
            const std::string clock_id = descriptor.value(
                "clock_id", "camera_" + serial);
            contract["clocks"][clock_id] = descriptor;
            contract["camera_assignments"][serial] = {
                {"timestamp", clock_id},
                {"timestamp_sys", "host_realtime"}
            };

            const auto [first_recording_frame_id, last_recording_frame_id] =
                recording_frame_range_for_camera(
                    *manifest, serial, camera_evidence);
            if (first_recording_frame_id > 0 && last_recording_frame_id > 0) {
                contract["clock_state_intervals"][serial] = nlohmann::json::array({{
                    {"first_recording_frame_id", first_recording_frame_id},
                    {"last_recording_frame_id", last_recording_frame_id},
                    {"camera_clock_id", clock_id},
                    {"classification", descriptor.value(
                        "classification", std::string("device_defined"))},
                    {"semantic_authority", descriptor.value(
                        "semantic_authority", std::string("epoch_unspecified"))}
                }});
            }
        }
    }

    (*manifest)["timestamp_clock_contract"] = std::move(contract);
}

nlohmann::json build_clip_artifacts_json(
    const std::vector<RecordingSessionCameraArtifact>& cameras,
    nlohmann::json* camera_artifacts_out = nullptr,
    nlohmann::json* camera_serials_out = nullptr)
{
    nlohmann::json video_artifacts = nlohmann::json::object();
    nlohmann::json metadata_artifacts = nlohmann::json::object();
    nlohmann::json keyframe_artifacts = nlohmann::json::object();
    nlohmann::json camera_artifacts = nlohmann::json::object();
    nlohmann::json camera_serials = nlohmann::json::array();

    for (const RecordingSessionCameraArtifact& camera : cameras) {
        if (camera.camera_serial.empty()) {
            continue;
        }
        camera_serials.push_back(camera.camera_serial);
        video_artifacts[camera.camera_serial] = camera.video_path;
        metadata_artifacts[camera.camera_serial] = camera.metadata_path;
        keyframe_artifacts[camera.camera_serial] = camera.keyframe_path;
        camera_artifacts[camera.camera_serial] = build_camera_artifact_json(camera);
    }

    if (camera_artifacts_out) {
        *camera_artifacts_out = camera_artifacts;
    }
    if (camera_serials_out) {
        *camera_serials_out = camera_serials;
    }
    return {
        {"videos", video_artifacts},
        {"metadata", metadata_artifacts},
        {"keyframes", keyframe_artifacts}
    };
}

nlohmann::json build_rolling_clip_entry_json(
    const RollingClipManifestOptions& clip,
    const bool include_schema_fields)
{
    nlohmann::json camera_artifacts;
    nlohmann::json camera_serials;
    nlohmann::json artifacts =
        build_clip_artifacts_json(clip.cameras, &camera_artifacts, &camera_serials);
    std::vector<RecordingOutputDescriptor> recording_outputs =
        build_full_recording_output_descriptors(
            clip.cameras,
            clip.output_backend.empty() ? "in_process" : clip.output_backend,
            clip.status);
    recording_outputs.insert(
        recording_outputs.end(),
        clip.recording_outputs.begin(),
        clip.recording_outputs.end());
    const nlohmann::json recording_outputs_json =
        build_recording_outputs_json(recording_outputs);

    nlohmann::json out = {
        {"producer", clip.producer},
        {"session_id", clip.session_id},
        {"clip_index", clip.clip_index},
        {"clip_id", clip.clip_id},
        {"recording_folder", clip.recording_folder},
        {"directory", clip.directory},
        {"status", clip.status},
        {"start_reason", clip.start_reason},
        {"stop_reason", clip.stop_reason},
        {"started_at_utc", clip.started_at_utc},
        {"started_at_elapsed_s", clip.started_at_elapsed_s},
        {"stop_requested_at_utc", clip.stop_requested_at_utc},
        {"stop_requested_at_elapsed_s", clip.stop_requested_at_elapsed_s},
        {"finalized_at_utc", clip.finalized_at_utc},
        {"finalized_at_elapsed_s", clip.finalized_at_elapsed_s},
        {"requested_duration_s", clip.requested_duration_s},
        {"actual_duration_s", clip.actual_duration_s},
        {"drain_duration_s", clip.drain_duration_s},
        {"rollover",
         {
             {"request_id", clip.rollover_request_id},
             {"rollover_at_recording_frame_id", clip.rollover_at_recording_frame_id},
             {"first_recording_frame_id", clip.first_recording_frame_id},
             {"last_recording_frame_id", clip.last_recording_frame_id},
             {"pending_next_clip", clip.pending_next_clip}
         }},
        {"timed_stop_hit", clip.timed_stop_hit},
        {"final_clip", clip.final_clip},
        {"drain_completed", clip.drain_completed},
        {"cameras", camera_serials},
        {"camera_artifacts", camera_artifacts},
        {"recording_outputs", recording_outputs_json},
        {"artifacts", artifacts}
    };
    if (include_schema_fields) {
        out["schema_id"] = "orange.recording_clip";
        out["schema_version"] = 1;
    }
    return out;
}

}  // namespace

RecordingOutputDescriptor build_crop_recording_output_descriptor(
    const std::string& camera_serial,
    const std::string& recording_folder,
    const bool relative_paths,
    const int crop_size_px,
    const int frame_rate,
    const std::string& status)
{
    RecordingOutputDescriptor output;
    output.camera_serial = camera_serial;
    output.output_kind = "crop";
    output.role = "sidecar";
    output.backend = "in_process";
    output.status = status.empty() ? "finalized" : status;
    output.width = crop_size_px;
    output.height = crop_size_px;
    output.frame_rate = frame_rate;
    output.codec = "hevc";
    output.container = "mp4";
    output.tuning = "lossless";
    output.pixel_source_format = "mono8";
    output.encoded_format = "nv12";
    output.coordinate_space = "full_frame_pixels";
    output.details = {
        {"selection_policy", "largest_detection_by_confidence"},
        {"blank_frame_policy", "encode_black_frame_when_no_detection"}
    };

    if (camera_serial.empty() || recording_folder.empty()) {
        return output;
    }

    const std::filesystem::path folder(recording_folder);
    const std::string prefix = "Cam" + camera_serial + "_crop";
    const std::string video_name = prefix + ".mp4";
    const std::string metadata_name = prefix + "_meta.csv";
    const std::string keyframe_name = prefix + "_keyframe.json";
    const std::string perf_name = prefix + "_perf.csv";
    const std::string sidecar_perf_name = prefix + "_sidecar_perf.csv";
    const std::filesystem::path video_path = folder / video_name;
    const std::filesystem::path metadata_path = folder / metadata_name;

    output.video_path = artifact_path_string(folder, video_name, relative_paths);
    output.metadata_path = artifact_path_string(folder, metadata_name, relative_paths);
    output.keyframe_path = artifact_path_string(folder, keyframe_name, relative_paths);
    output.perf_path = artifact_path_string(folder, perf_name, relative_paths);
    output.sidecar_perf_path = artifact_path_string(folder, sidecar_perf_name, relative_paths);

    const MetadataFrameStats frame_stats = read_metadata_frame_stats(metadata_path);
    output.frame_count = frame_stats.frame_count;
    output.first_recording_frame_id = frame_stats.first_recording_frame_id;
    output.last_recording_frame_id = frame_stats.last_recording_frame_id;
    output.recording_frame_id_gaps = frame_stats.recording_frame_id_gaps;

    uint64_t packet_count = 0;
    if (count_video_packets(video_path, &packet_count)) {
        output.packet_count = packet_count;
        output.packet_count_source = "ffprobe_nb_read_packets";
    } else {
        output.packet_count = 0;
        output.packet_count_source = "unavailable";
    }
    return output;
}

nlohmann::json build_single_clip_recording_session_manifest(
    const SingleClipRecordingSessionManifestOptions& options)
{
    nlohmann::json cameras = nlohmann::json::array();
    nlohmann::json video_artifacts = nlohmann::json::object();
    nlohmann::json metadata_artifacts = nlohmann::json::object();
    nlohmann::json keyframe_artifacts = nlohmann::json::object();
    nlohmann::json camera_artifacts = nlohmann::json::object();

    for (const RecordingSessionCameraArtifact& camera : options.cameras) {
        if (camera.camera_serial.empty()) {
            continue;
        }
        cameras.push_back(camera.camera_serial);
        video_artifacts[camera.camera_serial] = camera.video_path;
        metadata_artifacts[camera.camera_serial] = camera.metadata_path;
        keyframe_artifacts[camera.camera_serial] = camera.keyframe_path;
        camera_artifacts[camera.camera_serial] = build_camera_artifact_json(camera);
    }

    std::string output_backend = "in_process";
    if (options.recording_backend.is_object() &&
        options.recording_backend.contains("mode") &&
        options.recording_backend["mode"].is_string() &&
        !options.recording_backend["mode"].get<std::string>().empty()) {
        output_backend = options.recording_backend["mode"].get<std::string>();
    }
    std::vector<RecordingOutputDescriptor> recording_outputs =
        build_full_recording_output_descriptors(
            options.cameras,
            output_backend,
            options.status);
    recording_outputs.insert(
        recording_outputs.end(),
        options.recording_outputs.begin(),
        options.recording_outputs.end());
    const nlohmann::json recording_outputs_json =
        build_recording_outputs_json(recording_outputs);

    nlohmann::json manifest = {
        {"schema_id", "orange.recording_session"},
        {"schema_version", 1},
        {"producer", options.producer},
        {"session_id", options.session_id},
        {"created_at_utc", options.created_at_utc},
        {"updated_at_utc", options.updated_at_utc},
        {"recording_folder", options.recording_folder},
        {"mode", "single_clip"},
        {"status", options.status},
        {"cameras", cameras},
        {"camera_artifacts", camera_artifacts},
        {"recording_outputs", recording_outputs_json},
        {"stream",
         {
             {"requested_duration_seconds", options.requested_stream_duration_seconds},
             {"stream_start_delay_seconds", options.stream_start_delay_seconds},
             {"started_at_utc", options.stream_started_at_utc},
             {"finished_at_utc", options.stream_finished_at_utc},
             {"actual_elapsed_s", options.stream_actual_elapsed_s},
             {"interrupted", options.stream_interrupted}
         }},
        {"recording_control", build_recording_control_json(options.recording_control)},
        {"recording",
         {
             {"started", options.recording_started},
             {"started_at_utc", options.recording_started_at_utc},
             {"started_at_elapsed_s", options.recording_started_at_elapsed_s},
             {"stop_requested", options.recording_stop_requested},
             {"stop_requested_at_utc", options.recording_stop_requested_at_utc},
             {"stop_requested_at_elapsed_s", options.recording_stop_requested_at_elapsed_s},
             {"stop_reason", options.recording_stop_reason},
             {"drain_completed", options.recording_drain_completed},
             {"drained_at_utc", options.recording_drained_at_utc},
             {"drained_at_elapsed_s", options.recording_drained_at_elapsed_s},
             {"actual_recording_duration_s", options.actual_recording_duration_s},
             {"drain_duration_s", options.drain_duration_s}
         }},
        {"clips",
         nlohmann::json::array(
             {{
                 {"clip_index", 0},
                 {"clip_id", "clip_0000"},
                 {"recording_folder", options.recording_folder},
                 {"directory", "."},
                 {"started_at_utc", options.recording_started_at_utc},
                 {"stop_requested_at_utc", options.recording_stop_requested_at_utc},
                 {"finalized_at_utc", options.recording_drained_at_utc},
                 {"stop_reason", options.recording_stop_reason},
                 {"requested_duration_s", options.recording_control.record_for_seconds},
                 {"actual_duration_s", options.actual_recording_duration_s},
                 {"timed_stop_hit", options.timed_stop_hit},
                 {"drain_completed", options.recording_drain_completed},
                 {"artifacts",
                  {
                      {"videos", video_artifacts},
                      {"metadata", metadata_artifacts},
                      {"keyframes", keyframe_artifacts}
                  }},
                 {"recording_outputs", recording_outputs_json}
             }})}
    };
    if (options.recording_backend.is_object() && !options.recording_backend.empty()) {
        manifest["recording_backend"] = options.recording_backend;
    }
    if (options.recording_stop_control.is_object() &&
        !options.recording_stop_control.empty()) {
        manifest["recording"]["control"] = options.recording_stop_control;
    }
    add_recording_geometry_contract_reference(
        &manifest, options.recording_folder);
    return manifest;
}

nlohmann::json build_recording_clip_manifest(
    const RollingClipManifestOptions& options)
{
    return build_rolling_clip_entry_json(options, true);
}

nlohmann::json build_rolling_clip_recording_session_manifest(
    const RollingRecordingSessionManifestOptions& options)
{
    nlohmann::json cameras = nlohmann::json::array();
    for (const std::string& camera_serial : options.camera_serials) {
        if (!camera_serial.empty()) {
            cameras.push_back(camera_serial);
        }
    }

    nlohmann::json clips = nlohmann::json::array();
    std::size_t index_row_count = 0;
    double sum_clip_actual_duration_s = options.sum_clip_actual_duration_s;
    if (sum_clip_actual_duration_s <= 0.0) {
        for (const RollingClipManifestOptions& clip : options.clips) {
            sum_clip_actual_duration_s += clip.actual_duration_s;
        }
    }
    for (const RollingClipManifestOptions& clip : options.clips) {
        clips.push_back(build_rolling_clip_entry_json(clip, false));
        index_row_count += clip.cameras.size();
    }

    nlohmann::json manifest = {
        {"schema_id", "orange.recording_session"},
        {"schema_version", 1},
        {"producer", options.producer},
        {"session_id", options.session_id},
        {"created_at_utc", options.created_at_utc},
        {"updated_at_utc", options.updated_at_utc},
        {"recording_folder", options.recording_folder},
        {"mode", "rolling_clips"},
        {"status", options.status},
        {"cameras", cameras},
        {"stream",
         {
             {"requested_duration_seconds", options.requested_stream_duration_seconds},
             {"stream_start_delay_seconds", options.stream_start_delay_seconds},
             {"started_at_utc", options.stream_started_at_utc},
             {"finished_at_utc", options.stream_finished_at_utc},
             {"actual_elapsed_s", options.stream_actual_elapsed_s},
             {"interrupted", options.stream_interrupted}
         }},
        {"recording_control", build_recording_control_json(options.recording_control)},
        {"recording",
         {
             {"started", options.recording_started},
             {"started_at_utc", options.recording_started_at_utc},
             {"started_at_elapsed_s", options.recording_started_at_elapsed_s},
             {"stop_requested", options.recording_stop_requested},
             {"stop_requested_at_utc", options.recording_stop_requested_at_utc},
             {"stop_requested_at_elapsed_s", options.recording_stop_requested_at_elapsed_s},
             {"stop_reason", options.recording_stop_reason},
             {"drain_completed", options.recording_drain_completed},
             {"drained_at_utc", options.recording_drained_at_utc},
             {"drained_at_elapsed_s", options.recording_drained_at_elapsed_s},
             {"actual_recording_duration_s", options.actual_recording_duration_s},
             {"sum_clip_actual_duration_s", sum_clip_actual_duration_s},
             {"drain_duration_s", options.drain_duration_s}
        }},
        {"rollover",
         {
             {"implementation", options.rollover_implementation},
             {"seamless_writer_switch", true},
             {"records_during_rollover", true},
             {"boundary", "gop_first_frame_id"},
             {"next_writer_preopened", options.rollover_next_writer_preopened}
         }},
        {"indexes", default_rolling_index_manifest_json(options.clips.size(), index_row_count)},
        {"clips", clips}
    };
    if (options.recording_backend.is_object() && !options.recording_backend.empty()) {
        manifest["recording_backend"] = options.recording_backend;
    }
    if (options.recording_stop_control.is_object() &&
        !options.recording_stop_control.empty()) {
        manifest["recording"]["control"] = options.recording_stop_control;
    }
    if (!options.recording_outputs.empty()) {
        manifest["recording_outputs"] =
            build_recording_outputs_json(options.recording_outputs);
    }
    add_recording_geometry_contract_reference(
        &manifest, options.recording_folder);
    return manifest;
}

RecordingSessionCameraArtifact build_recording_camera_artifact(
    const std::string& camera_serial,
    const std::string& recording_folder,
    const bool relative_paths)
{
    RecordingSessionCameraArtifact artifact;
    artifact.camera_serial = camera_serial;
    if (camera_serial.empty() || recording_folder.empty()) {
        return artifact;
    }

    const std::filesystem::path folder(recording_folder);
    const std::string video_name = "Cam" + camera_serial + ".mp4";
    const std::string metadata_name = "Cam" + camera_serial + "_meta.csv";
    const std::string keyframe_name = "Cam" + camera_serial + "_keyframe.json";
    const std::filesystem::path video_path = folder / video_name;
    const std::filesystem::path metadata_path = folder / metadata_name;

    artifact.video_path = artifact_path_string(folder, video_name, relative_paths);
    artifact.metadata_path = artifact_path_string(folder, metadata_name, relative_paths);
    artifact.keyframe_path = artifact_path_string(folder, keyframe_name, relative_paths);

    const MetadataFrameStats frame_stats = read_metadata_frame_stats(metadata_path);
    artifact.frame_count = frame_stats.frame_count;
    artifact.first_recording_frame_id = frame_stats.first_recording_frame_id;
    artifact.last_recording_frame_id = frame_stats.last_recording_frame_id;
    artifact.recording_frame_id_gaps = frame_stats.recording_frame_id_gaps;

    uint64_t packet_count = 0;
    if (count_video_packets(video_path, &packet_count)) {
        artifact.packet_count = packet_count;
        artifact.packet_count_source = "ffprobe_nb_read_packets";
    } else {
        artifact.packet_count = 0;
        artifact.packet_count_source = "unavailable";
    }
    return artifact;
}

std::vector<RecordingSessionCameraArtifact> build_recording_camera_artifacts(
    const std::vector<std::string>& camera_serials,
    const std::string& recording_folder,
    const bool relative_paths)
{
    std::vector<RecordingSessionCameraArtifact> artifacts;
    artifacts.reserve(camera_serials.size());
    for (const std::string& camera_serial : camera_serials) {
        if (camera_serial.empty()) {
            continue;
        }
        artifacts.push_back(build_recording_camera_artifact(
            camera_serial,
            recording_folder,
            relative_paths));
    }
    return artifacts;
}

bool write_recording_session_manifest(const std::string& path,
                                      const nlohmann::json& manifest,
                                      std::string* error_out)
{
    if (path.empty()) {
        if (error_out) {
            *error_out = "recording session manifest path is empty";
        }
        return false;
    }
    const std::filesystem::path manifest_path(path);
    nlohmann::json finalized_manifest = manifest;
    if (!add_finalized_frame_identity_contract(
            &finalized_manifest, manifest_path, error_out)) {
        return false;
    }
    if (!add_acquisition_index_mapping_contract(
            &finalized_manifest, manifest_path, error_out)) {
        return false;
    }
    add_finalized_timestamp_clock_contract(&finalized_manifest, manifest_path);
    return write_json_file(manifest_path, finalized_manifest, error_out);
}

bool write_rolling_clip_index_artifacts(const std::string& recording_folder,
                                        const nlohmann::json& manifest,
                                        RecordingSessionIndexArtifacts* artifacts_out,
                                        std::string* error_out)
{
    if (artifacts_out) {
        *artifacts_out = RecordingSessionIndexArtifacts{};
    }
    if (recording_folder.empty()) {
        if (error_out) {
            *error_out = "recording folder is empty for rolling clip index artifacts";
        }
        return false;
    }
    if (!manifest.is_object() || manifest.value("mode", std::string()) != "rolling_clips") {
        if (error_out) {
            *error_out = "rolling clip index artifacts require a rolling_clips manifest";
        }
        return false;
    }

    const std::filesystem::path folder(recording_folder);
    const nlohmann::json indexes =
        manifest.value("indexes", default_rolling_index_manifest_json(0, 0));
    const std::filesystem::path json_path =
        folder / indexes.value("clip_index_json", std::string("recording_clip_index.json"));
    const std::filesystem::path csv_path =
        folder / indexes.value("clip_index_csv", std::string("recording_clip_index.csv"));
    const nlohmann::json clips = manifest.value("clips", nlohmann::json::array());
    if (!clips.is_array() || clips.empty()) {
        if (error_out) {
            *error_out = "rolling clip index artifacts require at least one clip";
        }
        return false;
    }

    nlohmann::json rows = nlohmann::json::array();
    nlohmann::json camera_ranges = nlohmann::json::object();
    for (const nlohmann::json& clip : clips) {
        if (!clip.is_object()) {
            continue;
        }
        const nlohmann::json camera_artifacts =
            clip.value("camera_artifacts", nlohmann::json::object());
        if (!camera_artifacts.is_object()) {
            continue;
        }
        for (auto it = camera_artifacts.begin(); it != camera_artifacts.end(); ++it) {
            if (!it.value().is_object()) {
                continue;
            }
            const std::string camera_serial = it.key();
            nlohmann::json row =
                make_clip_index_row(manifest, clip, camera_serial, it.value());
            rows.push_back(row);

            nlohmann::json& camera_range = camera_ranges[camera_serial];
            if (!camera_range.is_object()) {
                camera_range = {
                    {"clip_count", 0},
                    {"total_frame_count", 0ULL},
                    {"total_packet_count", 0ULL},
                    {"packet_count_source", "not_collected"},
                    {"first_recording_frame_id", 0ULL},
                    {"last_recording_frame_id", 0ULL},
                    {"recording_frame_id_gaps", 0ULL},
                    {"total_actual_duration_s", 0.0}
                };
            }
            const uint64_t first = row.value("first_recording_frame_id", 0ULL);
            const uint64_t last = row.value("last_recording_frame_id", 0ULL);
            const uint64_t frame_count = row.value("frame_count", 0ULL);
            const uint64_t gaps = row.value("recording_frame_id_gaps", 0ULL);
            const bool has_packet_count =
                row.contains("packet_count") && !row["packet_count"].is_null();
            camera_range["clip_count"] =
                camera_range.value("clip_count", 0) + 1;
            camera_range["total_frame_count"] =
                camera_range.value("total_frame_count", 0ULL) + frame_count;
            if (has_packet_count) {
                camera_range["total_packet_count"] =
                    camera_range.value("total_packet_count", 0ULL) +
                    row.value("packet_count", 0ULL);
                const std::string existing_source =
                    camera_range.value("packet_count_source", std::string("not_collected"));
                const std::string row_source =
                    row.value("packet_count_source", std::string("unknown"));
                camera_range["packet_count_source"] =
                    (existing_source == "not_collected" || existing_source == row_source)
                        ? row_source
                        : "mixed";
            }
            if (first > 0 &&
                (camera_range.value("first_recording_frame_id", 0ULL) == 0 ||
                 first < camera_range.value("first_recording_frame_id", 0ULL))) {
                camera_range["first_recording_frame_id"] = first;
            }
            if (last > camera_range.value("last_recording_frame_id", 0ULL)) {
                camera_range["last_recording_frame_id"] = last;
            }
            camera_range["recording_frame_id_gaps"] =
                camera_range.value("recording_frame_id_gaps", 0ULL) + gaps;
            camera_range["total_actual_duration_s"] =
                camera_range.value("total_actual_duration_s", 0.0) +
                row.value("actual_duration_s", 0.0);
        }
    }

    const std::vector<std::string> columns = {
        "session_id",
        "producer",
        "clip_index",
        "clip_id",
        "camera_serial",
        "status",
        "start_reason",
        "stop_reason",
        "final_clip",
        "timed_stop_hit",
        "drain_completed",
        "started_at_utc",
        "stop_requested_at_utc",
        "finalized_at_utc",
        "started_at_elapsed_s",
        "stop_requested_at_elapsed_s",
        "finalized_at_elapsed_s",
        "requested_duration_s",
        "actual_duration_s",
        "drain_duration_s",
        "first_recording_frame_id",
        "last_recording_frame_id",
        "frame_count",
        "recording_frame_id_gaps",
        "packet_count",
        "packet_count_source",
        "rollover_request_id",
        "rollover_at_recording_frame_id",
        "clip_directory",
        "clip_recording_folder",
        "clip_manifest_path",
        "video",
        "metadata",
        "keyframes"
    };

    nlohmann::json index = {
        {"schema_id", "orange.recording_clip_index"},
        {"schema_version", 1},
        {"producer", manifest.value("producer", std::string())},
        {"session_id", manifest.value("session_id", std::string())},
        {"mode", "rolling_clips"},
        {"generated_at_utc", manifest.value("updated_at_utc", std::string())},
        {"recording_folder", recording_folder},
        {"row_granularity", "clip_camera"},
        {"columns", columns},
        {"cameras", manifest.value("cameras", nlohmann::json::array())},
        {"clip_count", clips.size()},
        {"row_count", rows.size()},
        {"camera_ranges", camera_ranges},
        {"rows", rows}
    };

    if (!write_json_file(json_path, index, error_out)) {
        return false;
    }
    if (!write_csv_file(csv_path, columns, rows, error_out)) {
        return false;
    }

    if (artifacts_out) {
        artifacts_out->clip_index_json_path = json_path.string();
        artifacts_out->clip_index_csv_path = csv_path.string();
    }
    return true;
}

void create_recording_pipelines_for_stream(RecordingSessionState* state,
                                           CameraParams* cameras_params,
                                           CameraEachSelect* cameras_select,
                                           const int num_cameras,
                                           const EncoderConfig& encoder_config,
                                           CameraResources* const* camera_resources,
                                           CameraControl* camera_control,
                                           const AppStorageConfig* app_storage_config)
{
    if (!state || !cameras_params || !cameras_select || !camera_resources || !camera_control || num_cameras <= 0) {
        return;
    }

    state->recording_pipelines.clear();
    state->recording_pipelines.resize(num_cameras);
    state->resolved_recording_configs.clear();
    state->resolved_recording_configs.resize(num_cameras);
    state->recording_sink_mode =
        resolve_gui_recording_sink_mode(
            app_storage_config,
            cameras_params,
            cameras_select,
            num_cameras);
    state->gui_recording_control = resolve_gui_recording_control(app_storage_config);
    state->crop_recording_sink_mode = resolve_gui_crop_recording_sink_mode();
    state->external_recorder_contract_config =
        resolve_gui_external_recorder_contract_config(
            app_storage_config,
            &state->external_recorder_contract_source,
            &state->external_recorder_config_status);
    if (!state->external_recorder_config_status.empty()) {
        std::cerr << "[recording_session] "
                  << state->external_recorder_config_status << std::endl;
    }

    for (int i = 0; i < num_cameras; ++i) {
        if (!cameras_select[i].record) {
            continue;
        }

        std::string recording_output_warning;
        const RecordingOutputConfig recording_output_config =
            orange::recording::resolve_recording_output_config(
                cameras_params[i],
                encoder_config,
                cameras_select[i],
                &recording_output_warning);
        if (!recording_output_warning.empty()) {
            std::cerr << "[record_output] Cam " << cameras_params[i].camera_serial
                      << ": " << recording_output_warning
                      << ". Falling back to native "
                      << cameras_params[i].width << "x" << cameras_params[i].height
                      << "." << std::endl;
        }

        ResolvedRecordingConfigOverrides recording_overrides;
        recording_overrides.recording_gpu_id = cameras_params[i].gpu_id;
        recording_overrides.has_output_preferences_override = true;
        recording_overrides.output_preferences.mode =
            recording_output_config.mode == "resolution" ? "resolution"
            : (recording_output_config.mode == "exact_size" ? "exact_size" : "factor");
        recording_overrides.output_preferences.downsample_factor =
            recording_output_config.downsample_factor;
        recording_overrides.output_preferences.requested_width =
            recording_output_config.requested_width;
        recording_overrides.output_preferences.requested_height =
            recording_output_config.requested_height;
        recording_overrides.codec = encoder_config.encoder_codec;
        recording_overrides.preset = encoder_config.encoder_preset;
        recording_overrides.tuning = encoder_config.tuning_info;
        recording_overrides.rate_control_mode = encoder_config.rate_control_mode;
        recording_overrides.quality_value = encoder_config.quality_value;
        recording_overrides.gop_length = encoder_config.gop_length;
        recording_overrides.encoder_control_overrides.aq = encoder_config.aq;
        recording_overrides.encoder_control_overrides.temporal_aq = encoder_config.temporal_aq;
        recording_overrides.base_folder_name = encoder_config.folder_name;

        const ResolvedRecordingConfig resolved_recording_config =
            build_resolved_recording_config(cameras_params[i], recording_overrides);
        state->resolved_recording_configs[i] = resolved_recording_config;
        state->recording_pipelines[i] = std::make_unique<ModernRecordingPipeline>(
            &cameras_params[i],
            resolved_recording_config,
            *camera_resources[i]->recycle_queue,
            camera_control,
            state->recording_sink_mode);
    }
}

namespace {

// Failed-start cleanup shared by every phase: release the run's claim on
// the CameraControl folders/flags and stop any session lifecycle still
// marked started.
void cleanup_failed_recording_run_start(RecordingSessionState* state,
                                        CameraControl* camera_control,
                                        const std::string& recording_folder)
{
    if (!camera_control) {
        return;
    }
    {
        std::lock_guard<std::mutex> lock(camera_control->recording_folder_mutex);
        if (camera_control->recording_folder == recording_folder) {
            camera_control->recording_folder.clear();
        }
        if (camera_control->recording_output_folder == recording_folder) {
            camera_control->recording_output_folder.clear();
        }
    }
    camera_control->record_video = false;
    camera_control->recording_draining = false;
    camera_control->stop_record = false;
    if (state) {
        std::string ignored_error;
        if (state->external_crop_recorder_lifecycle.started) {
            (void)orange::external_recorder::StopSupervisedRecorderLifecycle(
                &state->external_crop_recorder_lifecycle,
                &ignored_error);
        }
        if (state->external_recorder_lifecycle.started) {
            (void)orange::external_recorder::StopSupervisedRecorderLifecycle(
                &state->external_recorder_lifecycle,
                &ignored_error);
        }
        (void)orange::monitoring::StopNicThermalMonitorProcess(
            &state->nic_thermal_monitor,
            &ignored_error);
    }
}

// Stops a lifecycle that was started during the (still pending) supervisor
// phase. The recorders never received a frame during startup, so skip the
// graceful drain wait; SIGTERM promptly and keep the stop bounded.
void stop_pending_recording_run_lifecycle(
    orange::external_recorder::SupervisedRecorderLifecycleState* lifecycle)
{
    if (!lifecycle || !lifecycle->started) {
        return;
    }
    lifecycle->process_options.graceful_shutdown_timeout_ms = 0;
    std::string ignored_error;
    (void)orange::external_recorder::StopSupervisedRecorderLifecycle(
        lifecycle,
        &ignored_error);
}

RecordingRunStartResult failed_recording_run_start_result(
    const PreparedRecordingRunStart& prepared,
    const std::string& error_message)
{
    RecordingRunStartResult result;
    result.recording_folder = prepared.recording_folder;
    result.recording_sink_mode = prepared.recording_sink_mode;
    result.error_message = error_message;
    result.external_recorder_contract_path = prepared.external_recorder_contract_path;
    return result;
}

}  // namespace

PreparedRecordingRunStart prepare_recording_run(
    RecordingSessionState* state,
    CameraControl* camera_control,
    CameraParams* cameras_params,
    const CameraEachSelect* cameras_select,
    const int num_cameras,
    const std::string& base_folder,
    PTPParams* ptp_params,
    const std::string& recording_sink_mode,
    const nlohmann::json* external_recorder_contract_config)
{
    PreparedRecordingRunStart prepared;
    if (!camera_control || !cameras_params || num_cameras <= 0) {
        return prepared;
    }

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
        camera_control->worker_stop_reason.clear();
    }

    const std::string recording_id_base = get_current_date_time();
    std::string recording_id;
    std::string recording_folder;
    std::string resolved_base_folder = base_folder;
    {
        std::lock_guard<std::mutex> lock(camera_control->recording_folder_mutex);
        if (!camera_control->recording_folder.empty()) {
            // A non-empty folder claim at prepare time is a leak: the
            // finalize path releases the claim when a run completes, so a
            // surviving claim means the previous run never finalized.
            // Reusing it would send this run into the previous run's
            // directory, truncate-overwriting its snapshot/PTP/session
            // artifacts and reopening the same Cam* output files. Mint a
            // fresh timestamped folder unconditionally.
            const std::string stale_folder = camera_control->recording_folder;
            std::cerr << "[recording] WARNING: prepare_recording_run found a"
                         " stale recording folder claim from a previous run"
                         " that never finalized; discarding it and minting a"
                         " fresh folder."
                      << " stale_folder=" << stale_folder
                      << std::endl;
            if (resolved_base_folder.empty()) {
                const std::filesystem::path stale_parent =
                    std::filesystem::path(stale_folder).parent_path();
                if (!stale_parent.empty() && stale_parent != "/") {
                    resolved_base_folder = stale_parent.string();
                }
            }
            camera_control->recording_folder.clear();
        }
        std::string folder_error;
        if (!create_unique_timestamped_folder(
                resolved_base_folder,
                recording_id_base,
                &recording_folder,
                &recording_id,
                &folder_error)) {
            prepared.error_message = folder_error.empty()
                ? "failed to allocate a unique recording folder"
                : folder_error;
            std::cerr << "[recording] Failed to allocate recording folder: "
                      << prepared.error_message << std::endl;
            return prepared;
        }
        camera_control->recording_folder = recording_folder;
        recording_folder = camera_control->recording_folder;
    }

    if (resolved_base_folder.empty() && !recording_folder.empty()) {
        const std::filesystem::path parent = std::filesystem::path(recording_folder).parent_path();
        if (parent.empty() || parent == "/") {
            resolved_base_folder = recording_folder;
        } else {
            resolved_base_folder = parent.string();
        }
    }

    const std::string normalized_sink_mode = normalize_recording_sink_mode(recording_sink_mode);
    const bool external_recorder_requested = normalized_sink_mode == "external_ipc";

    prepared.recording_folder = recording_folder;
    prepared.recording_id = recording_id;
    prepared.resolved_base_folder = resolved_base_folder;
    prepared.normalized_sink_mode = normalized_sink_mode;
    prepared.recording_sink_mode =
        normalized_sink_mode.empty() ? recording_sink_mode : normalized_sink_mode;
    prepared.external_recorder_requested = external_recorder_requested;

    write_recording_snapshot(
        recording_folder,
        recording_id,
        cameras_params,
        num_cameras,
        resolved_base_folder,
        !external_recorder_requested,
        camera_control->sync_camera,
        ptp_params,
        prepared.recording_sink_mode,
        state && state->resolved_recording_configs.size() ==
                         static_cast<size_t>(num_cameras)
            ? state->resolved_recording_configs.data()
            : nullptr,
        state ? static_cast<int>(state->resolved_recording_configs.size()) : 0,
        cameras_select);
    initialize_ptp_sync_summary(
        recording_folder,
        recording_id,
        num_cameras,
        camera_control->sync_camera,
        ptp_params);

    if (external_recorder_requested) {
        if (!state) {
            prepared.error_message = "external_ipc recording requires a recording session state";
            cleanup_failed_recording_run_start(state, camera_control, recording_folder);
            return prepared;
        }
        if (state->external_recorder_lifecycle.started) {
            std::string stop_error;
            (void)orange::external_recorder::StopSupervisedRecorderLifecycle(
                &state->external_recorder_lifecycle,
                &stop_error);
        }
        reset_external_ipc_connections(state);

        orange::external_recorder::CameraContractMaterializationInput contract_input;
        contract_input.contract_config = external_recorder_contract_config;
        contract_input.recording_folder = recording_folder;
        contract_input.recording_id = recording_id;
        contract_input.cameras_params = cameras_params;
        contract_input.cameras_select = cameras_select;
        contract_input.num_cameras = num_cameras;
        if (state->resolved_recording_configs.size() ==
            static_cast<size_t>(num_cameras)) {
            contract_input.resolved_recording_configs =
                state->resolved_recording_configs.data();
            contract_input.num_resolved_recording_configs = num_cameras;
        }
        if (state->gui_recording_control.enabled()) {
            contract_input.recording_control.record_for_seconds =
                state->gui_recording_control.record_for_seconds;
            contract_input.recording_control.clip_seconds =
                state->gui_recording_control.clip_seconds;
        }
        const nlohmann::json contract =
            orange::external_recorder::MaterializeExternalRecorderContractForCameras(
                contract_input);

        state->active_external_recorder_contract = contract;
        state->external_recorder_last_error.clear();

        const std::filesystem::path contract_path =
            std::filesystem::path(recording_folder) / "external_recorder_contract.json";
        std::string artifact_error;
        if (!write_json_file(contract_path, contract, &artifact_error)) {
            prepared.error_message = artifact_error;
            state->external_recorder_last_error = artifact_error;
            cleanup_failed_recording_run_start(state, camera_control, recording_folder);
            return prepared;
        }
        state->external_recorder_contract_path = contract_path.string();
        prepared.external_recorder_contract_path = state->external_recorder_contract_path;

        prepared.external_recorder_lifecycle_options.contract = contract;
        prepared.external_recorder_lifecycle_options.default_session_id = recording_id;
        prepared.external_recorder_lifecycle_options.analytics_root = recording_folder;
        prepared.external_recorder_lifecycle_options.verifier_path =
            "scripts/verify_external_recorder_session.py";
    }

    if (state &&
        state->crop_recording_sink_mode == "external_ipc" &&
        cameras_select &&
        std::any_of(
            cameras_select,
            cameras_select + num_cameras,
            [](const CameraEachSelect& selected) {
                return selected.crop_and_encode;
            })) {
        if (state->external_crop_recorder_lifecycle.started) {
            std::string stop_error;
            (void)orange::external_recorder::StopSupervisedRecorderLifecycle(
                &state->external_crop_recorder_lifecycle,
                &stop_error);
        }

        const nlohmann::json crop_contract =
            materialize_external_crop_recorder_contract_for_cameras(
                recording_folder,
                recording_id,
                cameras_params,
                cameras_select,
                num_cameras,
                state->recording_sink_mode == "external_ipc"
                    ? state->gui_recording_control
                    : RecordingControlConfig{});
        state->active_external_crop_recorder_contract = crop_contract;
        state->external_crop_recorder_last_error.clear();

        const std::filesystem::path crop_contract_path =
            std::filesystem::path(recording_folder) / "external_crop_recorder_contract.json";
        std::string artifact_error;
        if (!write_json_file(crop_contract_path, crop_contract, &artifact_error)) {
            prepared.error_message = artifact_error;
            state->external_crop_recorder_last_error = artifact_error;
            cleanup_failed_recording_run_start(state, camera_control, recording_folder);
            return prepared;
        }
        state->external_crop_recorder_contract_path = crop_contract_path.string();
        prepared.external_crop_recorder_contract_path =
            state->external_crop_recorder_contract_path;

        const std::string crop_contract_error =
            external_crop_contract_validation_error(crop_contract);
        if (!crop_contract_error.empty()) {
            prepared.error_message = crop_contract_error;
            state->external_crop_recorder_last_error = crop_contract_error;
            std::cerr << "[recording_session] " << crop_contract_error << std::endl;
            cleanup_failed_recording_run_start(state, camera_control, recording_folder);
            return prepared;
        }

        prepared.external_crop_recorder_requested = true;
        prepared.external_crop_recorder_lifecycle_options.contract = crop_contract;
        prepared.external_crop_recorder_lifecycle_options.default_session_id = recording_id;
        prepared.external_crop_recorder_lifecycle_options.analytics_root = recording_folder;
        prepared.external_crop_recorder_lifecycle_options.verifier_path =
            "scripts/verify_external_recorder_session.py";
    }

    prepared.valid = true;
    return prepared;
}

RecordingRunSupervisorStartOutcome start_prepared_recording_run_supervisors(
    const PreparedRecordingRunStart& prepared)
{
    RecordingRunSupervisorStartOutcome outcome;
    if (!prepared.valid) {
        outcome.error_message = prepared.error_message.empty()
            ? "recording run start was not prepared"
            : prepared.error_message;
        return outcome;
    }

    // Perform one aggregate capacity calculation before starting either the
    // full-frame or crop recorder family.  Starting them independently and
    // relying on each child process's free-space query is unsafe: every child
    // observes the same available bytes but cannot see the other streams'
    // planned consumption.
    if (prepared.requires_supervisor_start()) {
        std::vector<orange::external_recorder::SupervisorPlan> storage_plan_storage;
        storage_plan_storage.reserve(2);
        auto append_storage_plan = [&](
                                       const orange::external_recorder::SupervisedRecorderLifecycleOptions& options,
                                       const char* label) -> bool {
            orange::external_recorder::SupervisorPlanOptions plan_options;
            plan_options.recorder_tool_path =
                orange::external_recorder::ResolveExternalRecorderToolPath(
                    options.recorder_tool_path);
            plan_options.default_session_id = options.default_session_id;
            orange::external_recorder::SupervisorPlan plan;
            std::string plan_error;
            if (!orange::external_recorder::BuildSupervisorPlanFromContract(
                    options.contract,
                    plan_options,
                    &plan,
                    &plan_error)) {
                outcome.error_message = std::string("failed to build ") + label +
                                        " storage plan: " + plan_error;
                return false;
            }
            storage_plan_storage.push_back(std::move(plan));
            return true;
        };
        if (prepared.external_recorder_requested &&
            !append_storage_plan(
                prepared.external_recorder_lifecycle_options,
                "full-frame recorder")) {
            return outcome;
        }
        if (prepared.external_crop_recorder_requested &&
            !append_storage_plan(
                prepared.external_crop_recorder_lifecycle_options,
                "crop recorder")) {
            return outcome;
        }
        std::vector<orange::external_recorder::SupervisorPlan*> storage_plans;
        storage_plans.reserve(storage_plan_storage.size());
        for (auto& plan : storage_plan_storage) {
            storage_plans.push_back(&plan);
        }
        orange::external_recorder::DurationAwareStoragePreflight storage_preflight;
        std::string storage_error;
        const bool storage_ok =
            orange::external_recorder::RunDurationAwareStoragePreflight(
                storage_plans,
                &storage_preflight,
                &storage_error);
        const std::filesystem::path storage_artifact =
            std::filesystem::path(prepared.recording_folder) /
            "duration_aware_storage_preflight.json";
        std::string artifact_error;
        if (!orange::external_recorder::WriteDurationAwareStoragePreflightArtifact(
                storage_artifact.string(),
                storage_preflight,
                &artifact_error)) {
            outcome.error_message = artifact_error;
            return outcome;
        }
        if (!storage_ok) {
            outcome.error_message = storage_error.empty()
                ? "duration-aware storage preflight failed"
                : storage_error;
            return outcome;
        }
    }

    if (prepared.external_recorder_requested) {
        outcome.external_recorder_attempted = true;
        std::string supervisor_error;
        if (!orange::external_recorder::StartSupervisedRecorderLifecycle(
                prepared.external_recorder_lifecycle_options,
                &outcome.external_recorder_lifecycle,
                &supervisor_error)) {
            outcome.error_message = supervisor_error.empty()
                ? "failed to start GUI external recorder supervisor"
                : supervisor_error;
            outcome.external_recorder_last_error = outcome.error_message;
            if (!outcome.external_recorder_lifecycle.last_artifact_error.empty()) {
                outcome.external_recorder_last_error += "; " +
                    outcome.external_recorder_lifecycle.last_artifact_error;
            }
            return outcome;
        }
    }

    if (prepared.external_crop_recorder_requested) {
        outcome.external_crop_recorder_attempted = true;
        std::string crop_supervisor_error;
        if (!orange::external_recorder::StartSupervisedRecorderLifecycle(
                prepared.external_crop_recorder_lifecycle_options,
                &outcome.external_crop_recorder_lifecycle,
                &crop_supervisor_error)) {
            outcome.error_message = crop_supervisor_error.empty()
                ? "failed to start GUI external crop recorder supervisor"
                : crop_supervisor_error;
            outcome.external_crop_recorder_last_error = outcome.error_message;
            if (!outcome.external_crop_recorder_lifecycle.last_artifact_error.empty()) {
                outcome.external_crop_recorder_last_error += "; " +
                    outcome.external_crop_recorder_lifecycle.last_artifact_error;
            }
            // The full-frame lifecycle (if any) is still private to this
            // outcome; stop it here so a failed start never leaks recorder
            // children, no matter what the caller does next.
            stop_pending_recording_run_lifecycle(&outcome.external_recorder_lifecycle);
            return outcome;
        }
    }

    outcome.ok = true;
    return outcome;
}

RecordingRunStartResult complete_recording_run(
    RecordingSessionState* state,
    CameraControl* camera_control,
    CameraParams* cameras_params,
    const int num_cameras,
    PTPParams* ptp_params,
    const PreparedRecordingRunStart& prepared,
    RecordingRunSupervisorStartOutcome&& outcome)
{
    // Retained in the phased-start API for parity with in-process completion
    // and existing callers. External supervisor completion no longer rebuilds
    // the base recording snapshot from these values.
    (void)cameras_params;
    (void)num_cameras;
    (void)ptp_params;

    RecordingRunStartResult result;
    if (!camera_control) {
        result.error_message = "recording run completion is missing CameraControl";
        return result;
    }
    if (!prepared.valid) {
        return failed_recording_run_start_result(
            prepared,
            prepared.error_message.empty()
                ? "recording run start was not prepared"
                : prepared.error_message);
    }

    if (!outcome.ok) {
        if (state) {
            if (outcome.external_recorder_attempted) {
                state->external_recorder_lifecycle =
                    std::move(outcome.external_recorder_lifecycle);
                if (!outcome.external_recorder_last_error.empty()) {
                    state->external_recorder_last_error =
                        outcome.external_recorder_last_error;
                    std::cerr << "[recording_session] "
                              << state->external_recorder_last_error << std::endl;
                }
            }
            if (outcome.external_crop_recorder_attempted) {
                state->external_crop_recorder_lifecycle =
                    std::move(outcome.external_crop_recorder_lifecycle);
                if (!outcome.external_crop_recorder_last_error.empty()) {
                    state->external_crop_recorder_last_error =
                        outcome.external_crop_recorder_last_error;
                    std::cerr << "[recording_session] "
                              << state->external_crop_recorder_last_error << std::endl;
                }
            }
        }
        cleanup_failed_recording_run_start(
            state, camera_control, prepared.recording_folder);
        return failed_recording_run_start_result(
            prepared,
            outcome.error_message.empty()
                ? "failed to start external recorder supervisors"
                : outcome.error_message);
    }

    result.external_recorder_contract_path = prepared.external_recorder_contract_path;

    if (outcome.external_recorder_attempted && state) {
        state->external_recorder_lifecycle =
            std::move(outcome.external_recorder_lifecycle);
        state->external_recorder_last_error.clear();

        const std::filesystem::path plan_path =
            std::filesystem::path(prepared.recording_folder) /
            "external_recorder_supervisor_plan.json";
        std::string artifact_error;
        if (write_json_file(
                plan_path,
                orange::external_recorder::SupervisorPlanToJson(
                    state->external_recorder_lifecycle.plan),
                &artifact_error)) {
            state->external_recorder_supervisor_plan_path = plan_path.string();
            result.external_recorder_supervisor_plan_path =
                state->external_recorder_supervisor_plan_path;
        } else {
            std::cerr << "[recording_session] Failed to write GUI external recorder supervisor plan: "
                      << artifact_error << std::endl;
        }

        std::cout << "[recording_session] GUI external recorder supervisor started."
                  << " streams=" << state->external_recorder_lifecycle.plan.streams.size()
                  << " artifact_root=" << state->external_recorder_lifecycle.plan.artifact_root
                  << std::endl;

        // The snapshot was created during prepare and may already contain
        // recording-start extensions such as the immutable geometry-contract
        // reference and direct daily rim-mask entries. Successful supervisor
        // startup only needs to publish the latest-recording pointer; rewriting
        // the base snapshot here would erase those extensions.
        if (!publish_latest_recording_pointer(
                prepared.resolved_base_folder,
                prepared.recording_folder,
                prepared.recording_id)) {
            std::cerr << "[recording_session] Failed to refresh latest-recording pointer for GUI external recorder run."
                      << std::endl;
        }
    }

    if (outcome.external_crop_recorder_attempted && state) {
        state->external_crop_recorder_lifecycle =
            std::move(outcome.external_crop_recorder_lifecycle);
        state->external_crop_recorder_last_error.clear();

        const std::filesystem::path crop_plan_path =
            std::filesystem::path(prepared.recording_folder) /
            "external_crop_recorder_supervisor_plan.json";
        std::string artifact_error;
        if (write_json_file(
                crop_plan_path,
                orange::external_recorder::SupervisorPlanToJson(
                    state->external_crop_recorder_lifecycle.plan),
                &artifact_error)) {
            state->external_crop_recorder_supervisor_plan_path = crop_plan_path.string();
        } else {
            std::cerr << "[recording_session] Failed to write GUI external crop recorder supervisor plan: "
                      << artifact_error << std::endl;
        }

        std::cout << "[recording_session] GUI external crop recorder supervisor started."
                  << " streams=" << state->external_crop_recorder_lifecycle.plan.streams.size()
                  << " artifact_root=" << state->external_crop_recorder_lifecycle.plan.artifact_root
                  << std::endl;
    }

    if (state) {
        std::string thermal_error;
        if (!orange::monitoring::StartNicThermalMonitorProcess(
                prepared.recording_folder,
                &state->nic_thermal_monitor,
                &thermal_error)) {
            std::cerr << "[recording_session] NIC thermal monitoring is unavailable: "
                      << thermal_error << std::endl;
        }
        if (!update_recording_snapshot_system_monitoring(
                prepared.recording_folder,
                "nic_thermal",
                orange::monitoring::NicThermalMonitorProcessToJson(
                    state->nic_thermal_monitor))) {
            std::cerr << "[recording_session] Failed to publish NIC thermal monitor metadata."
                      << std::endl;
        }
    }

    camera_control->record_video = true;
    result.ok = true;
    result.recording_folder = prepared.recording_folder;
    result.recording_sink_mode = prepared.recording_sink_mode;
    return result;
}

void abort_prepared_recording_run(
    RecordingSessionState* state,
    CameraControl* camera_control,
    const PreparedRecordingRunStart& prepared,
    RecordingRunSupervisorStartOutcome&& outcome,
    const std::string& abort_reason)
{
    // Stop lifecycles the supervisor phase started before they were ever
    // installed into the session; they received no frames, so skip the
    // graceful drain wait. Stopping also writes their runtime/verifier
    // artifacts into the abandoned run folder for forensics.
    stop_pending_recording_run_lifecycle(&outcome.external_crop_recorder_lifecycle);
    stop_pending_recording_run_lifecycle(&outcome.external_recorder_lifecycle);

    if (state) {
        if (outcome.external_recorder_attempted) {
            state->external_recorder_lifecycle =
                std::move(outcome.external_recorder_lifecycle);
            if (!outcome.external_recorder_last_error.empty()) {
                state->external_recorder_last_error =
                    outcome.external_recorder_last_error;
            }
        }
        if (outcome.external_crop_recorder_attempted) {
            state->external_crop_recorder_lifecycle =
                std::move(outcome.external_crop_recorder_lifecycle);
            if (!outcome.external_crop_recorder_last_error.empty()) {
                state->external_crop_recorder_last_error =
                    outcome.external_crop_recorder_last_error;
            }
        }
    }
    cleanup_failed_recording_run_start(
        state, camera_control, prepared.recording_folder);
    std::cout << "[recording_session] Recording run start aborted (" << abort_reason
              << ")." << (prepared.recording_folder.empty()
                              ? std::string()
                              : " folder=" + prepared.recording_folder)
              << std::endl;
}

void request_stop_recording_run(CameraControl* camera_control)
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
    if (camera_control->active_recorders.load(std::memory_order_relaxed) == 0) {
        camera_control->recording_draining = false;
        camera_control->stop_record = false;
        if (!camera_control->preserve_recording_session_state) {
            std::lock_guard<std::mutex> lock(camera_control->recording_folder_mutex);
            camera_control->recording_folder.clear();
        }
    }
}

void request_drain_recording_run(
    std::vector<std::unique_ptr<ModernRecordingPipeline>>* recording_pipelines,
    const std::string& recording_sink_mode,
    CameraControl* camera_control)
{
    request_stop_recording_run(camera_control);
    if (!recording_pipelines) {
        return;
    }
    for (auto& pipeline : *recording_pipelines) {
        if (pipeline) {
            pipeline->request_recording_drain();
        }
    }
    // External IPC sinks never increment active_recorders (no in-process
    // encoder owns the output), so the early-drain shortcut in
    // request_stop_recording_run may have just cleared the drain latches even
    // though the IPC handoff workers still hold frames awaiting ACK/RELEASE.
    // Re-assert the latches until every pipeline reports drained; the drain
    // completion path (workers or wait_for_recording_run_drain callers)
    // clears them once the handoff is truly finished.
    if (camera_control &&
        should_reassert_recording_drain_flags(
            recording_sink_mode,
            recording_pipelines_drained(recording_pipelines))) {
        camera_control->recording_draining = true;
        camera_control->stop_record = true;
    }
}

void request_drain_recording_run(RecordingSessionState* state, CameraControl* camera_control)
{
    request_drain_recording_run(
        state ? &state->recording_pipelines : nullptr,
        state ? state->recording_sink_mode : std::string(),
        camera_control);
}

bool should_reassert_recording_drain_flags(const std::string& recording_sink_mode,
                                           const bool pipelines_drained)
{
    return recording_sink_mode == "external_ipc" && !pipelines_drained;
}

bool recording_pipelines_drained(
    const std::vector<std::unique_ptr<ModernRecordingPipeline>>* recording_pipelines)
{
    if (!recording_pipelines) {
        return true;
    }
    for (const auto& pipeline : *recording_pipelines) {
        if (pipeline && !pipeline->is_drained()) {
            return false;
        }
    }
    return true;
}

bool recording_pipelines_drained(const RecordingSessionState* state)
{
    return recording_pipelines_drained(state ? &state->recording_pipelines : nullptr);
}

bool recording_run_drained(
    const std::vector<std::unique_ptr<ModernRecordingPipeline>>* recording_pipelines,
    const CameraControl* camera_control)
{
    if (camera_control &&
        camera_control->active_recorders.load(std::memory_order_relaxed) > 0) {
        return false;
    }
    return recording_pipelines_drained(recording_pipelines);
}

bool wait_for_recording_run_drain(
    std::vector<std::unique_ptr<ModernRecordingPipeline>>* recording_pipelines,
    CameraControl* camera_control,
    const std::chrono::steady_clock::duration timeout,
    const char* timeout_label)
{
    constexpr auto kPollInterval = std::chrono::milliseconds(1);
    constexpr auto kDrainNudgeInterval = std::chrono::milliseconds(250);
    const auto drain_deadline = std::chrono::steady_clock::now() + timeout;
    auto next_drain_nudge = std::chrono::steady_clock::now() + kDrainNudgeInterval;
    while (!recording_run_drained(recording_pipelines, camera_control) &&
           std::chrono::steady_clock::now() < drain_deadline) {
        const auto now = std::chrono::steady_clock::now();
        if (recording_pipelines && now >= next_drain_nudge) {
            // Passive drain states (pending GPU source releases, external IPC
            // protocol replies) only make progress on worker flush ticks;
            // re-request the drain so those ticks keep flowing.
            for (auto& pipeline : *recording_pipelines) {
                if (pipeline && !pipeline->is_drained()) {
                    pipeline->request_recording_drain();
                }
            }
            next_drain_nudge = now + kDrainNudgeInterval;
        }
        std::this_thread::sleep_for(kPollInterval);
    }
    const bool drained = recording_run_drained(recording_pipelines, camera_control);
    if (!drained && timeout_label) {
        std::cerr << timeout_label << " timed out with "
                  << (camera_control
                          ? camera_control->active_recorders.load(std::memory_order_relaxed)
                          : 0)
                  << " active recorder(s), pipelines_drained="
                  << (recording_pipelines_drained(recording_pipelines) ? "true" : "false")
                  << "." << std::endl;
    }
    return drained;
}

void clear_recording_run_state(CameraControl* camera_control)
{
    if (!camera_control) {
        return;
    }
    camera_control->record_video = false;
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
    camera_control->worker_stop_reason.clear();
    camera_control->latest_recording_frame_id.store(0, std::memory_order_relaxed);
}

void drain_and_shutdown_recording_run(
    std::vector<std::unique_ptr<ModernRecordingPipeline>>* recording_pipelines,
    const std::string& recording_sink_mode,
    CameraControl* camera_control,
    const std::chrono::steady_clock::duration drain_timeout,
    const char* drain_timeout_label)
{
    request_drain_recording_run(recording_pipelines, recording_sink_mode, camera_control);
    (void)wait_for_recording_run_drain(
        recording_pipelines,
        camera_control,
        drain_timeout,
        drain_timeout_label);

    if (recording_pipelines) {
        for (auto& pipeline : *recording_pipelines) {
            if (pipeline) {
                pipeline->request_stop();
            }
        }
        for (auto& pipeline : *recording_pipelines) {
            if (pipeline) {
                pipeline->shutdown();
                pipeline.reset();
            }
        }
    }

    clear_recording_run_state(camera_control);
}

void reset_external_ipc_connections(RecordingSessionState* state)
{
    if (!state) {
        return;
    }
    for (auto& pipeline : state->recording_pipelines) {
        if (pipeline) {
            pipeline->reset_external_ipc_connection();
        }
    }
}

std::string current_recording_folder(CameraControl* camera_control)
{
    if (!camera_control) {
        return {};
    }

    std::lock_guard<std::mutex> lock(camera_control->recording_folder_mutex);
    return camera_control->recording_folder;
}

void start_recording_pipeline_for_camera(RecordingSessionState* state, const int camera_index)
{
    if (!state || !is_valid_pipeline_index(*state, camera_index)) {
        return;
    }
    if (state->recording_pipelines[camera_index]) {
        state->recording_pipelines[camera_index]->start();
    }
}

void request_stop_recording_pipeline_for_camera(RecordingSessionState* state, const int camera_index)
{
    if (!state || !is_valid_pipeline_index(*state, camera_index)) {
        return;
    }
    if (state->recording_pipelines[camera_index]) {
        state->recording_pipelines[camera_index]->request_stop();
    }
}

void shutdown_recording_pipeline_for_camera(RecordingSessionState* state, const int camera_index)
{
    if (!state || !is_valid_pipeline_index(*state, camera_index)) {
        return;
    }
    if (state->recording_pipelines[camera_index]) {
        state->recording_pipelines[camera_index]->shutdown();
        state->recording_pipelines[camera_index].reset();
    }
}

void clear_recording_pipelines(RecordingSessionState* state)
{
    if (!state) {
        return;
    }
    state->recording_pipelines.clear();
}

RecordingIngress* recording_ingress_for_camera(const RecordingSessionState& state, const int camera_index)
{
    if (!is_valid_pipeline_index(state, camera_index)) {
        return nullptr;
    }
    return state.recording_pipelines[camera_index]
        ? state.recording_pipelines[camera_index]->recording_ingress()
        : nullptr;
}

}  // namespace orange::session
