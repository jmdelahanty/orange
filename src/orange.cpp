// src/orange.cpp

#include "video_capture.h"
#include <iostream>
#include "camera.h"
#include "imgui.h"
#include "implot.h"
#include <ImGuiFileDialog.h>
#include "project.h"
#include "gui.h"
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <cuda.h>
#include <unordered_map>
#include <cuda_runtime.h>
#include "NvEncoder/NvCodecUtils.h"
#include "network_base.h"
#include "enet_thread.h"
#include "yolo_worker.h"
#include "global.h"
#include "encoder_preprocess_worker.h"
#include "encoder_hw_worker.h"
#include "modern_recording_pipeline.h"
#include "opengldisplay.h"
#include "image_writer_worker.h"
#include "yolov8_det.h"
#include "crop_and_encode_worker.h"
#include "crop_preview_worker.h"
#include "crop_producer_worker.h"
#include "pose_worker.h"
#include "frame_ipc_manager.h"
#include "external_recorder_contract_utils.h"
#include "fsuid_guard.h"
#include "recording_ingress.h"
#include "aperture_characterization.h"
#include "camera_preview_utils.h"
#include "gui/camera_properties_panel.h"
#include "gui/frame_ipc_panel.h"
#include "gui/host_ptp_panel.h"
#include "gui/recording_panel.h"
#include "gui_display_frame_rate.h"
#include "image_canvas.h"
#include "recording_output_utils.h"
#include "recording_validation.h"
#include "orange_local_control.h"
#include "session/crop_rolling_sidecars.h"
#include "session/recording_session.h"
#include "spatial_layout_ui.h"
#include "spatial_snapshot_worker.h"
#include "usaf_resolution_ui.h"
#include <opencv2/opencv.hpp>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <fstream>
#include <array>
#include <iomanip>
#include <limits>
#include <map>
#include <optional>
#include <sstream>
#include <cctype>
#include <filesystem>
#include <system_error>
#include <thread>

std::vector<YoloWorker*> yolo_workers; // For managing YOLO workers
ENetPeer* external_data_consumer_peer = nullptr; // Store the peer for YOLO data
std::vector<SpeedTrackingData> speed_tracking_data;

namespace {

using orange::gui::GuiDisplayFrameRateStats;
using orange::gui::GuiFrameTimingSample;
using orange::gui::gui_display_frame_rate_json;
using orange::gui::gui_sample_display_frame_rate;
using orange::gui::gui_sample_frame_timings;

enum class RulerAlignmentOrientation {
    kHorizontal = 0,
    kVertical = 1
};

struct RulerAlignmentMetrics {
    bool has_detected_line = false;
    double line_angle_deg = 0.0;
    double angle_error_deg = 0.0;
    double center_offset_px = 0.0;
    double center_offset_fraction = 0.0;
    int x0 = 0;
    int y0 = 0;
    int x1 = 0;
    int y1 = 0;
};

struct FovCaptureSnapshot {
    bool available = false;
    int width = 0;
    int height = 0;
    std::vector<unsigned char> rgb;
    RulerAlignmentMetrics metrics;
};

double gui_elapsed_ms(const std::chrono::steady_clock::time_point start,
                      const std::chrono::steady_clock::time_point end)
{
    return std::chrono::duration<double, std::milli>(end - start).count();
}

struct LiveFovPreviewState {
    bool available = false;
    int width = 0;
    int height = 0;
    uint64_t frame_serial = 0;
    std::vector<unsigned char> rgba;
    std::vector<unsigned char> raw_rgb;
    RulerAlignmentMetrics metrics;
    std::string status_message = "Idle";
    std::string error_message;
};

struct GuiSessionTimingState {
    bool stream_running = false;
    bool recording_running = false;
    bool recording_finalizing = false;
    std::chrono::steady_clock::time_point stream_started_at{};
    std::chrono::steady_clock::time_point recording_started_at{};
    std::chrono::steady_clock::time_point finalizing_started_at{};
    std::chrono::seconds last_recording_elapsed{0};
};

struct GuiSessionTimingSnapshot {
    bool stream_running = false;
    bool recording_running = false;
    bool recording_finalizing = false;
    bool has_recording_elapsed = false;
    std::string stream_elapsed = "00:00:00";
    std::string recording_elapsed = "00:00:00";
    std::string finalizing_elapsed = "00:00:00";
};

struct GuiRecordingRunState {
    bool active = false;
    bool finalizing = false;
    bool finalized = false;
    std::string recording_folder;
    std::string recording_sink_mode = "real";
    std::string recording_started_at_utc;
    std::string recording_stop_requested_at_utc;
    std::string recording_drained_at_utc;
    std::string stop_reason = "manual_stop";
    nlohmann::json stop_control = nlohmann::json::object();
    std::chrono::steady_clock::time_point recording_started_at{};
    std::chrono::steady_clock::time_point recording_stop_requested_at{};
    std::chrono::steady_clock::time_point recording_drained_at{};
    bool diagnostic_finalize_stall_reported = false;
};

struct GuiLocalControlStopSchedulerState {
    bool enabled = false;
    bool stop_recording_enabled = false;
    bool citrus_completion_enabled = false;
    bool scheduled = false;
    bool stop_triggered = false;
    bool drain_completed = false;
    bool drain_timed_out = false;
    bool drain_timeout_reported = false;
    bool forced_finalize_requested = false;
    bool forced_finalize_stream_stop_requested = false;
    double grace_seconds = 0.0;
    double drain_timeout_seconds = 60.0;
    std::string method;
    std::string request_id;
    std::string operation_id;
    std::string source;
    std::string experiment_id;
    std::string terminal_state;
    std::string reason;
    std::string received_at_utc;
    std::string stop_triggered_at_utc;
    std::string drain_completed_at_utc;
    std::string forced_finalize_requested_at_utc;
    std::string last_event;
    std::string last_event_at_utc;
    std::chrono::steady_clock::time_point deadline{};
    std::chrono::steady_clock::time_point stop_triggered_at{};
};

struct GuiLocalControlStartRequestState {
    bool enabled = false;
    bool pending = false;
    std::string request_id;
    std::string operation_id;
    std::string source;
    std::string reason;
    std::string received_at_utc;
    std::string last_event;
    std::string last_event_at_utc;
};

enum class GuiAutorunStage {
    kDisabled = 0,
    kSelectConfig,
    kOpenCameras,
    kStartStreaming,
    kStreamWarmup,
    kStartRecording,
    kRecording,
    kStopRecording,
    kWaitFinalize,
    kStopStreaming,
    kDone,
    kFailed
};

struct GuiAutorunConfig {
    bool enabled = false;
    int stream_warmup_seconds = 3;
    int record_seconds = 10;
    bool exit_after_finalize = false;
    bool hide_crop_preview = false;
    bool enable_stream = true;
    bool enable_record = true;
    bool enable_yolo = true;
    bool enable_crop = true;
    bool start_recording = true;
    std::string config_dir;
};

struct GuiAutorunState {
    GuiAutorunStage stage = GuiAutorunStage::kDisabled;
    std::chrono::steady_clock::time_point stage_started_at{};
    bool action_requested = false;
    bool close_requested = false;
    std::string error_message;
};

struct GuiAutorunRequests {
    bool open_cameras = false;
    bool toggle_streaming = false;
    bool toggle_recording = false;
    bool close_window = false;
};

std::string gui_lower_ascii(std::string text)
{
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return text;
}

std::string gui_trim_ascii_whitespace(const std::string& input)
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

std::optional<bool> gui_env_flag_value(const char* name)
{
    const char* raw = std::getenv(name);
    if (!raw || !*raw) {
        return std::nullopt;
    }
    const std::string value = gui_lower_ascii(raw);
    if (value == "1" || value == "true" || value == "yes" || value == "on") {
        return true;
    }
    if (value == "0" || value == "false" || value == "no" || value == "off") {
        return false;
    }
    std::cerr << "[GUI][autorun] Ignoring invalid " << name << "='"
              << raw << "'" << std::endl;
    return std::nullopt;
}

bool gui_env_flag_enabled(const char* name, const bool default_value = false)
{
    if (const std::optional<bool> value = gui_env_flag_value(name)) {
        return *value;
    }
    return default_value;
}

bool gui_env_flag_override(
    const char* gui_name,
    const char* generic_name,
    const bool fallback)
{
    if (const std::optional<bool> value = gui_env_flag_value(gui_name)) {
        return *value;
    }
    if (const std::optional<bool> value = gui_env_flag_value(generic_name)) {
        return *value;
    }
    return fallback;
}

std::string gui_env_string_or_empty(const char* name)
{
    const char* raw = std::getenv(name);
    return raw && *raw ? std::string(raw) : std::string();
}

bool gui_local_control_disabled()
{
    return gui_env_flag_override(
        "ORANGE_GUI_LOCAL_CONTROL_DISABLE",
        "ORANGE_LOCAL_CONTROL_DISABLE",
        false);
}

bool gui_local_control_stop_recording_enabled(
    const AppStorageConfig* app_storage_config)
{
    return gui_env_flag_override(
        "ORANGE_GUI_LOCAL_CONTROL_ENABLE_RECORDING_STOP",
        "ORANGE_LOCAL_CONTROL_ENABLE_RECORDING_STOP",
        app_storage_config &&
            app_storage_config->gui_local_control_recording_stop_enabled);
}

bool gui_local_control_citrus_completion_stop_enabled(
    const AppStorageConfig* app_storage_config)
{
    if (gui_local_control_stop_recording_enabled(app_storage_config)) {
        return true;
    }
    return gui_env_flag_override(
        "ORANGE_GUI_LOCAL_CONTROL_ENABLE_CITRUS_STOP",
        "ORANGE_LOCAL_CONTROL_ENABLE_CITRUS_STOP",
        app_storage_config &&
            app_storage_config->gui_local_control_citrus_completion_stop_enabled);
}

bool gui_local_control_recording_start_enabled(
    const AppStorageConfig* app_storage_config)
{
    return gui_env_flag_override(
        "ORANGE_GUI_LOCAL_CONTROL_ENABLE_RECORDING_START",
        "ORANGE_LOCAL_CONTROL_ENABLE_RECORDING_START",
        app_storage_config &&
            app_storage_config->gui_local_control_recording_start_enabled);
}

bool gui_local_control_exit_after_finalize_enabled(
    const AppStorageConfig* app_storage_config)
{
    return gui_env_flag_override(
        "ORANGE_GUI_LOCAL_CONTROL_EXIT_AFTER_FINALIZE",
        "ORANGE_LOCAL_CONTROL_EXIT_AFTER_FINALIZE",
        app_storage_config &&
            app_storage_config->gui_local_control_exit_after_finalize);
}

std::string gui_local_control_socket_path()
{
    std::string path = gui_env_string_or_empty("ORANGE_GUI_LOCAL_CONTROL_SOCKET");
    if (path.empty()) {
        path = gui_env_string_or_empty("ORANGE_LOCAL_CONTROL_SOCKET");
    }
    return path.empty() ? "/tmp/orange_local_control.sock" : path;
}

std::string gui_local_control_log_path(const std::string& socket_path)
{
    std::string path = gui_env_string_or_empty("ORANGE_GUI_LOCAL_CONTROL_LOG");
    if (path.empty()) {
        path = gui_env_string_or_empty("ORANGE_LOCAL_CONTROL_LOG");
    }
    return path.empty() ? (socket_path + ".events.jsonl") : path;
}

void gui_log_local_control_event(
    const std::string& event_log_path,
    nlohmann::json event)
{
    if (event_log_path.empty()) {
        return;
    }
    event["schema_id"] = "orange.local_control.gui_event";
    event["schema_version"] = 1;
    if (!event.contains("event_at_utc")) {
        event["event_at_utc"] = get_current_utc_timestamp();
    }
    std::string error;
    if (!orange::control::AppendLocalControlEventLog(
            event_log_path,
            event,
            &error)) {
        std::cerr << "[GUI][local_control] failed to append event log"
                  << " path=" << event_log_path
                  << " error=" << error
                  << std::endl;
    }
}

void gui_copy_local_control_event_log_to_recording_session(
    const std::string& event_log_path,
    const std::string& recording_folder)
{
    if (event_log_path.empty() || recording_folder.empty()) {
        return;
    }

    const std::filesystem::path source_path(event_log_path);
    const std::filesystem::path recording_dir(recording_folder);
    const std::filesystem::path target_path =
        recording_dir / "orange_local_control.events.jsonl";
    const std::filesystem::path manifest_path =
        recording_dir / "recording_session.json";
    if (!std::filesystem::exists(source_path) ||
        !std::filesystem::exists(manifest_path)) {
        return;
    }

    nlohmann::json manifest;
    {
        std::ifstream in(manifest_path);
        if (!in) {
            return;
        }
        try {
            in >> manifest;
        } catch (const std::exception& ex) {
            std::cerr << "[GUI][local_control] failed to parse recording_session"
                      << " for event-log capture path=" << manifest_path
                      << " error=" << ex.what() << std::endl;
            return;
        }
    }

    nlohmann::json* control = nullptr;
    if (manifest.contains("recording") &&
        manifest["recording"].is_object() &&
        manifest["recording"].contains("control") &&
        manifest["recording"]["control"].is_object()) {
        control = &manifest["recording"]["control"];
    }
    if (!control) {
        return;
    }

    std::error_code copy_error;
    std::filesystem::create_directories(recording_dir, copy_error);
    copy_error.clear();
    std::error_code equivalent_error;
    const bool already_in_place =
        std::filesystem::exists(target_path) &&
        std::filesystem::equivalent(source_path, target_path, equivalent_error);
    if (!already_in_place) {
        std::filesystem::copy_file(
            source_path,
            target_path,
            std::filesystem::copy_options::overwrite_existing,
            copy_error);
    }

    nlohmann::json event_log = {
        {"source_path", source_path.string()},
        {"copied_path", target_path.string()},
        {"relative_path", target_path.filename().string()},
        {"copied", !copy_error},
        {"copied_at_utc", get_current_utc_timestamp()},
        {"copy_error", copy_error ? copy_error.message() : ""}
    };
    if (!copy_error) {
        std::error_code size_error;
        const auto size = std::filesystem::file_size(target_path, size_error);
        if (!size_error) {
            event_log["bytes"] = size;
        }
    }
    (*control)["event_log"] = std::move(event_log);

    std::string manifest_error;
    if (!orange::session::write_recording_session_manifest(
            manifest_path.string(),
            manifest,
            &manifest_error)) {
        std::cerr << "[GUI][local_control] failed to update recording_session"
                  << " with event-log capture path=" << manifest_path
                  << " error=" << manifest_error << std::endl;
    }
}

int gui_env_int(const char* name, const int default_value, const int min_value)
{
    const char* raw = std::getenv(name);
    if (!raw || !*raw) {
        return default_value;
    }
    char* end = nullptr;
    const long parsed = std::strtol(raw, &end, 10);
    if (end == raw || *end != '\0') {
        std::cerr << "[GUI][autorun] Ignoring invalid " << name << "='"
                  << raw << "'" << std::endl;
        return default_value;
    }
    if (parsed < min_value) {
        std::cerr << "[GUI][autorun] Raising " << name << "=" << parsed
                  << " to minimum " << min_value << std::endl;
        return min_value;
    }
    return static_cast<int>(parsed);
}

int gui_local_control_drain_timeout_seconds(
    const AppStorageConfig* app_storage_config)
{
    if (const char* raw = std::getenv("ORANGE_GUI_LOCAL_CONTROL_DRAIN_TIMEOUT_SECONDS");
        raw && *raw) {
        return gui_env_int("ORANGE_GUI_LOCAL_CONTROL_DRAIN_TIMEOUT_SECONDS", 60, 0);
    }
    if (const char* raw = std::getenv("ORANGE_LOCAL_CONTROL_DRAIN_TIMEOUT_SECONDS");
        raw && *raw) {
        return gui_env_int("ORANGE_LOCAL_CONTROL_DRAIN_TIMEOUT_SECONDS", 60, 0);
    }
    if (app_storage_config &&
        app_storage_config->gui_local_control_drain_timeout_seconds >= 0) {
        return app_storage_config->gui_local_control_drain_timeout_seconds;
    }
    return 60;
}

int gui_local_control_diagnostic_finalize_stall_seconds()
{
    if (const char* raw =
            std::getenv("ORANGE_GUI_LOCAL_CONTROL_DIAGNOSTIC_FINALIZE_STALL_SECONDS");
        raw && *raw) {
        return gui_env_int(
            "ORANGE_GUI_LOCAL_CONTROL_DIAGNOSTIC_FINALIZE_STALL_SECONDS",
            0,
            0);
    }
    return gui_env_int(
        "ORANGE_LOCAL_CONTROL_DIAGNOSTIC_FINALIZE_STALL_SECONDS",
        0,
        0);
}

void set_gui_env_from_app_config_if_absent(const char* name,
                                           const std::string& value,
                                           const char* label)
{
    if (!name || !*name || value.empty()) {
        return;
    }
    const char* existing = std::getenv(name);
    if (existing && *existing) {
        return;
    }
    setenv(name, value.c_str(), 1);
    if (label && *label) {
        std::cout << "[GUI][app_config] " << label << ": "
                  << value << std::endl;
    }
}

int resolve_gui_crop_frame_pool_size()
{
    const char* raw = std::getenv("ORANGE_CROP_FRAME_POOL_SIZE");
    if (!raw || !*raw) {
        return CropProducer::kDefaultCropFramePoolSize;
    }
    char* end = nullptr;
    const long parsed = std::strtol(raw, &end, 10);
    if (end == raw || *end != '\0' ||
        parsed < CropProducer::kMinCropFramePoolSize ||
        parsed > CropProducer::kMaxCropFramePoolSize) {
        return CropProducer::kDefaultCropFramePoolSize;
    }
    return static_cast<int>(parsed);
}

GuiAutorunConfig resolve_gui_autorun_config()
{
    GuiAutorunConfig config;
    config.enabled = gui_env_flag_enabled("ORANGE_GUI_AUTORUN", false);
    config.stream_warmup_seconds =
        gui_env_int("ORANGE_GUI_AUTORUN_STREAM_WARMUP_SECONDS", 3, 0);
    config.record_seconds =
        gui_env_int("ORANGE_GUI_AUTORUN_RECORD_SECONDS", 10, 1);
    config.exit_after_finalize =
        gui_env_flag_enabled("ORANGE_GUI_AUTORUN_EXIT_AFTER_FINALIZE", false);
    config.hide_crop_preview =
        gui_env_flag_enabled("ORANGE_GUI_AUTORUN_HIDE_CROP_PREVIEW", false);
    config.enable_stream =
        gui_env_flag_enabled("ORANGE_GUI_AUTORUN_ENABLE_STREAM", true);
    config.enable_record =
        gui_env_flag_enabled("ORANGE_GUI_AUTORUN_ENABLE_RECORD", true);
    config.enable_yolo =
        gui_env_flag_enabled("ORANGE_GUI_AUTORUN_ENABLE_YOLO", true);
    config.enable_crop =
        gui_env_flag_enabled("ORANGE_GUI_AUTORUN_ENABLE_CROP", true);
    config.start_recording =
        gui_env_flag_enabled("ORANGE_GUI_AUTORUN_START_RECORDING", true);
    const char* config_dir = std::getenv("ORANGE_GUI_CONFIG_DIR");
    if (config_dir && *config_dir) {
        config.config_dir = config_dir;
    }
    return config;
}

const char* gui_autorun_stage_name(const GuiAutorunStage stage)
{
    switch (stage) {
        case GuiAutorunStage::kDisabled: return "disabled";
        case GuiAutorunStage::kSelectConfig: return "select_config";
        case GuiAutorunStage::kOpenCameras: return "open_cameras";
        case GuiAutorunStage::kStartStreaming: return "start_streaming";
        case GuiAutorunStage::kStreamWarmup: return "stream_warmup";
        case GuiAutorunStage::kStartRecording: return "start_recording";
        case GuiAutorunStage::kRecording: return "recording";
        case GuiAutorunStage::kStopRecording: return "stop_recording";
        case GuiAutorunStage::kWaitFinalize: return "wait_finalize";
        case GuiAutorunStage::kStopStreaming: return "stop_streaming";
        case GuiAutorunStage::kDone: return "done";
        case GuiAutorunStage::kFailed: return "failed";
    }
    return "unknown";
}

double gui_autorun_stage_elapsed_s(const GuiAutorunState& state)
{
    if (state.stage_started_at.time_since_epoch().count() == 0) {
        return 0.0;
    }
    return std::chrono::duration<double>(
        std::chrono::steady_clock::now() - state.stage_started_at).count();
}

void gui_autorun_enter_stage(GuiAutorunState* state, const GuiAutorunStage stage)
{
    if (!state) {
        return;
    }
    state->stage = stage;
    state->stage_started_at = std::chrono::steady_clock::now();
    state->action_requested = false;
    std::cout << "[GUI][autorun] stage=" << gui_autorun_stage_name(stage)
              << std::endl;
}

void gui_autorun_fail(GuiAutorunState* state, const std::string& message)
{
    if (!state) {
        return;
    }
    state->error_message = message;
    std::cerr << "[GUI][autorun] " << message << std::endl;
    gui_autorun_enter_stage(state, GuiAutorunStage::kFailed);
}

std::string gui_normalized_path_string(const std::string& path)
{
    std::error_code ec;
    std::filesystem::path normalized = std::filesystem::weakly_canonical(path, ec);
    if (ec) {
        normalized = std::filesystem::absolute(path, ec);
    }
    if (ec) {
        normalized = std::filesystem::path(path);
    }
    return normalized.lexically_normal().string();
}

int gui_find_local_config_folder(const std::vector<std::string>& folders,
                                 const std::string& requested_config_dir)
{
    if (requested_config_dir.empty()) {
        return -1;
    }
    const std::string requested = gui_normalized_path_string(requested_config_dir);
    for (std::size_t i = 0; i < folders.size(); ++i) {
        if (gui_normalized_path_string(folders[i]) == requested) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

GuiAutorunRequests gui_autorun_update(
    GuiAutorunState* state,
    const GuiAutorunConfig& config,
    const std::vector<std::string>& local_config_folders,
    int* local_config_select,
    const CameraControl* camera_control,
    const GuiRecordingRunState* recording_run,
    const bool calibration_tool_busy)
{
    GuiAutorunRequests requests;
    if (!state || !config.enabled || !camera_control) {
        return requests;
    }

    switch (state->stage) {
        case GuiAutorunStage::kDisabled:
            gui_autorun_enter_stage(state, GuiAutorunStage::kSelectConfig);
            break;

        case GuiAutorunStage::kSelectConfig: {
            const int folder_index =
                gui_find_local_config_folder(local_config_folders, config.config_dir);
            if (folder_index < 0) {
                gui_autorun_fail(
                    state,
                    "ORANGE_GUI_CONFIG_DIR was not found in local config folders: " +
                    config.config_dir);
                break;
            }
            if (local_config_select) {
                *local_config_select = folder_index;
            }
            std::cout << "[GUI][autorun] selected config folder: "
                      << local_config_folders[folder_index] << std::endl;
            gui_autorun_enter_stage(state, GuiAutorunStage::kOpenCameras);
            break;
        }

        case GuiAutorunStage::kOpenCameras:
            if (camera_control->open) {
                gui_autorun_enter_stage(state, GuiAutorunStage::kStartStreaming);
            } else if (!calibration_tool_busy && !state->action_requested) {
                requests.open_cameras = true;
                state->action_requested = true;
                std::cout << "[GUI][autorun] requesting camera open" << std::endl;
            } else if (gui_autorun_stage_elapsed_s(*state) > 30.0) {
                gui_autorun_fail(state, "timed out opening cameras");
            }
            break;

        case GuiAutorunStage::kStartStreaming:
            if (camera_control->subscribe) {
                gui_autorun_enter_stage(state, GuiAutorunStage::kStreamWarmup);
            } else if (!calibration_tool_busy && !state->action_requested) {
                requests.toggle_streaming = true;
                state->action_requested = true;
                std::cout << "[GUI][autorun] requesting stream start" << std::endl;
            } else if (gui_autorun_stage_elapsed_s(*state) > 45.0) {
                gui_autorun_fail(state, "timed out starting stream");
            }
            break;

        case GuiAutorunStage::kStreamWarmup:
            if (!camera_control->subscribe) {
                gui_autorun_fail(state, "stream stopped during warmup");
            } else if (gui_autorun_stage_elapsed_s(*state) >=
                       static_cast<double>(config.stream_warmup_seconds)) {
                gui_autorun_enter_stage(
                    state,
                    config.start_recording
                        ? GuiAutorunStage::kStartRecording
                        : GuiAutorunStage::kDone);
            }
            break;

        case GuiAutorunStage::kStartRecording:
            if (camera_control->record_video) {
                gui_autorun_enter_stage(state, GuiAutorunStage::kRecording);
            } else if (camera_control->recording_draining) {
                if (gui_autorun_stage_elapsed_s(*state) > 60.0) {
                    gui_autorun_fail(state, "recording was still draining before autorun start");
                }
            } else if (!calibration_tool_busy && !state->action_requested) {
                requests.toggle_recording = true;
                state->action_requested = true;
                std::cout << "[GUI][autorun] requesting recording start" << std::endl;
            } else if (gui_autorun_stage_elapsed_s(*state) > 45.0) {
                gui_autorun_fail(state, "timed out starting recording");
            }
            break;

        case GuiAutorunStage::kRecording:
            if (!camera_control->record_video) {
                gui_autorun_enter_stage(state, GuiAutorunStage::kStopStreaming);
            } else if (gui_autorun_stage_elapsed_s(*state) >=
                       static_cast<double>(config.record_seconds)) {
                gui_autorun_enter_stage(state, GuiAutorunStage::kStopRecording);
            }
            break;

        case GuiAutorunStage::kStopRecording:
            if (!camera_control->record_video) {
                gui_autorun_enter_stage(state, GuiAutorunStage::kStopStreaming);
            } else if (!state->action_requested) {
                requests.toggle_recording = true;
                state->action_requested = true;
                std::cout << "[GUI][autorun] requesting recording stop" << std::endl;
            } else if (gui_autorun_stage_elapsed_s(*state) > 45.0) {
                gui_autorun_fail(state, "timed out stopping recording");
            }
            break;

        case GuiAutorunStage::kWaitFinalize: {
            const bool run_active = recording_run &&
                                    (recording_run->active || recording_run->finalizing);
            if (!camera_control->record_video &&
                !camera_control->recording_draining &&
                !run_active) {
                gui_autorun_enter_stage(state, GuiAutorunStage::kStopStreaming);
            } else if (gui_autorun_stage_elapsed_s(*state) > 300.0) {
                gui_autorun_fail(state, "timed out finalizing recording");
            }
            break;
        }

        case GuiAutorunStage::kStopStreaming:
            if (!camera_control->subscribe) {
                gui_autorun_enter_stage(state, GuiAutorunStage::kDone);
            } else if (!state->action_requested) {
                requests.toggle_streaming = true;
                state->action_requested = true;
                std::cout << "[GUI][autorun] requesting stream stop" << std::endl;
            } else if (gui_autorun_stage_elapsed_s(*state) > 90.0) {
                gui_autorun_fail(state, "timed out stopping stream");
            }
            break;

        case GuiAutorunStage::kDone:
            if (config.exit_after_finalize && !state->close_requested) {
                requests.close_window = true;
                state->close_requested = true;
                std::cout << "[GUI][autorun] requesting GUI exit after finalize" << std::endl;
            }
            break;

        case GuiAutorunStage::kFailed:
            if (config.exit_after_finalize && !state->close_requested) {
                requests.close_window = true;
                state->close_requested = true;
            }
            break;
    }

    return requests;
}

void apply_gui_autorun_camera_selection(const GuiAutorunConfig& config,
                                        CameraEachSelect* cameras_select,
                                        const int num_cameras)
{
    if (!config.enabled || !cameras_select || num_cameras <= 0) {
        return;
    }
    for (int i = 0; i < num_cameras; ++i) {
        cameras_select[i].stream_on = config.enable_stream;
        cameras_select[i].record = config.enable_record;
        cameras_select[i].yolo = config.enable_yolo;
        cameras_select[i].crop_and_encode = config.enable_crop;
        if (cameras_select[i].crop_and_encode) {
            cameras_select[i].record = true;
            cameras_select[i].yolo = true;
        }
    }
    std::cout << "[GUI][autorun] camera selection"
              << " stream=" << (config.enable_stream ? 1 : 0)
              << " record=" << (config.enable_record ? 1 : 0)
              << " yolo=" << (config.enable_yolo ? 1 : 0)
              << " crop=" << (config.enable_crop ? 1 : 0)
              << " cameras=" << num_cameras
              << std::endl;
}

ImVec2 fit_square_image_size(const ImVec2 available, const float fallback_size)
{
    const float usable_width = std::max(1.0f, available.x);
    const float usable_height = std::max(1.0f, available.y);
    const float side = std::min(usable_width, usable_height);
    if (side > 1.0f) {
        return ImVec2(side, side);
    }
    return ImVec2(fallback_size, fallback_size);
}

ImVec2 fit_image_size(const ImVec2 available,
                      const float image_width,
                      const float image_height,
                      const float fallback_width,
                      const float fallback_height)
{
    const float usable_width = std::max(1.0f, available.x);
    const float usable_height = std::max(1.0f, available.y);
    const float source_width = image_width > 0.0f ? image_width : fallback_width;
    const float source_height = image_height > 0.0f ? image_height : fallback_height;
    if (source_width <= 0.0f || source_height <= 0.0f) {
        return ImVec2(usable_width, usable_height);
    }

    const float scale = std::min(usable_width / source_width, usable_height / source_height);
    if (scale <= 0.0f) {
        return ImVec2(fallback_width, fallback_height);
    }
    return ImVec2(std::max(1.0f, source_width * scale),
                  std::max(1.0f, source_height * scale));
}

void render_texture_image_centered(GLuint texture,
                                   const ImVec2 available_size,
                                   const float image_width,
                                   const float image_height)
{
    const ImVec2 image_size =
        fit_image_size(available_size, image_width, image_height, image_width, image_height);
    const float x_offset = std::max(0.0f, (available_size.x - image_size.x) * 0.5f);
    if (x_offset > 0.0f) {
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + x_offset);
    }
    ImGui::Image(
        (ImTextureID)(intptr_t)texture,
        image_size,
        ImVec2(0, 0),
        ImVec2(1, 1));
}

int sanitize_gui_stream_downsample(const int requested)
{
    static constexpr std::array<int, 5> kAllowed = {1, 2, 4, 8, 16};
    for (const int value : kAllowed) {
        if (requested == value) {
            return requested;
        }
    }
    if (requested <= 1) {
        return 1;
    }
    int best = kAllowed.back();
    for (const int value : kAllowed) {
        if (requested <= value) {
            best = value;
            break;
        }
    }
    return best;
}

int gui_stream_downsample_index(const int downsample)
{
    static constexpr std::array<int, 5> kAllowed = {1, 2, 4, 8, 16};
    const int sanitized = sanitize_gui_stream_downsample(downsample);
    for (std::size_t i = 0; i < kAllowed.size(); ++i) {
        if (kAllowed[i] == sanitized) {
            return static_cast<int>(i);
        }
    }
    return 0;
}

int resolve_gui_stream_downsample(const int default_value)
{
    const char* env = std::getenv("ORANGE_GUI_STREAM_DOWNSAMPLE");
    if (!env || !*env) {
        env = std::getenv("ORANGE_DISPLAY_DOWNSAMPLE");
    }
    if (!env || !*env) {
        return sanitize_gui_stream_downsample(default_value);
    }
    char* end = nullptr;
    const long parsed = std::strtol(env, &end, 10);
    if (end == env || *end != '\0') {
        std::cerr << "[GUI][display] Ignoring invalid stream downsample '"
                  << env << "'" << std::endl;
        return sanitize_gui_stream_downsample(default_value);
    }
    const int sanitized = sanitize_gui_stream_downsample(static_cast<int>(parsed));
    if (sanitized != parsed) {
        std::cout << "[GUI][display] Stream downsample " << parsed
                  << " adjusted to " << sanitized << std::endl;
    }
    return sanitized;
}

int sanitize_gui_display_preview_max_fps(const int requested)
{
    if (requested < 0) {
        return 0;
    }
    if (requested > 10000) {
        return 10000;
    }
    return requested;
}

int resolve_gui_display_preview_max_fps_snapshot(const CameraEachSelect* cameras_select,
                                                 const int num_cameras)
{
    int resolved = 60;
    if (cameras_select && num_cameras > 0) {
        resolved = cameras_select[0].display_preview_max_fps;
    }
    const char* env = std::getenv("ORANGE_DISPLAY_PREVIEW_MAX_FPS");
    if (!env || !*env) {
        env = std::getenv("ORANGE_DISPLAY_MAX_FPS");
    }
    if (env && *env) {
        char* end = nullptr;
        const long parsed = std::strtol(env, &end, 10);
        if (end != env && *end == '\0') {
            resolved = static_cast<int>(parsed);
        }
    }
    return sanitize_gui_display_preview_max_fps(resolved);
}

bool resolve_gui_yolo_speed_graphs_enabled(const bool default_value)
{
    return gui_env_flag_enabled("ORANGE_GUI_SHOW_SPEED_GRAPHS", default_value);
}

bool gui_any_crop_recording_enabled(const CameraEachSelect* cameras_select, const int num_cameras)
{
    if (!cameras_select || num_cameras <= 0) {
        return false;
    }
    for (int i = 0; i < num_cameras; ++i) {
        if (cameras_select[i].crop_and_encode) {
            return true;
        }
    }
    return false;
}

std::string gui_crop_stream_camera_serial(const orange::external_recorder::RecorderStreamPlan& stream)
{
    const std::string suffix = "_crop";
    auto strip_suffix = [&](std::string serial) {
        if (serial.size() > suffix.size() &&
            serial.compare(serial.size() - suffix.size(), suffix.size(), suffix) == 0) {
            serial.resize(serial.size() - suffix.size());
        }
        return serial;
    };
    if (stream.output_kind == "crop" && !stream.camera_serial.empty()) {
        return strip_suffix(stream.camera_serial);
    }
    std::string serial = stream.stream_id;
    if (serial.size() > suffix.size() &&
        serial.compare(serial.size() - suffix.size(), suffix.size(), suffix) == 0) {
        serial.resize(serial.size() - suffix.size());
        return serial;
    }
    serial = stream.camera_serial;
    if (serial.size() > suffix.size() &&
        serial.compare(serial.size() - suffix.size(), suffix.size(), suffix) == 0) {
        serial.resize(serial.size() - suffix.size());
    }
    return serial;
}

bool gui_external_stream_is_full(
    const orange::external_recorder::RecorderStreamPlan& stream)
{
    return stream.output_kind.empty() || stream.output_kind == "full";
}

bool gui_external_stream_is_crop(
    const orange::external_recorder::RecorderStreamPlan& stream)
{
    const std::string suffix = "_crop";
    auto has_crop_suffix = [&](const std::string& value) {
        return value.size() > suffix.size() &&
               value.compare(value.size() - suffix.size(), suffix.size(), suffix) == 0;
    };
    return stream.output_kind == "crop" ||
           has_crop_suffix(stream.stream_id) ||
           has_crop_suffix(stream.camera_serial);
}

bool gui_read_json_file(const std::string& path, nlohmann::json* out, std::string* error_out)
{
    if (!out) {
        if (error_out) {
            *error_out = "internal error: null JSON destination";
        }
        return false;
    }
    std::ifstream input(path);
    if (!input) {
        if (error_out) {
            *error_out = "missing JSON file: " + path;
        }
        return false;
    }
    try {
        input >> *out;
        return true;
    } catch (const std::exception& ex) {
        if (error_out) {
            *error_out = "invalid JSON file " + path + ": " + ex.what();
        }
        return false;
    }
}

double gui_elapsed_seconds_between(const std::chrono::steady_clock::time_point& start,
                                   const std::chrono::steady_clock::time_point& finish);

std::string gui_external_recorder_clip_id(const int clip_index)
{
    std::ostringstream out;
    out << "clip_" << std::setw(6) << std::setfill('0') << clip_index;
    return out.str();
}

std::string gui_json_string_or(const nlohmann::json& object,
                               const char* key,
                               const std::string& fallback)
{
    if (object.is_object()) {
        const auto it = object.find(key);
        if (it != object.end() && it->is_string()) {
            const std::string value = it->get<std::string>();
            if (!value.empty()) {
                return value;
            }
        }
    }
    return fallback;
}

uint64_t gui_json_u64_or(const nlohmann::json& object,
                         const char* key,
                         const uint64_t fallback)
{
    if (object.is_object()) {
        const auto it = object.find(key);
        if (it != object.end() && it->is_number_unsigned()) {
            return it->get<uint64_t>();
        }
        if (it != object.end() && it->is_number_integer()) {
            const int64_t value = it->get<int64_t>();
            return value > 0 ? static_cast<uint64_t>(value) : 0;
        }
    }
    return fallback;
}

bool gui_external_recorder_plan_requests_rolling(
    const orange::external_recorder::SupervisorPlan& plan)
{
    for (const auto& stream : plan.streams) {
        if (stream.clip_seconds > 0) {
            return true;
        }
    }
    return false;
}

bool gui_external_recorder_recording_control_from_plan(
    const orange::external_recorder::SupervisorPlan& plan,
    orange::session::RecordingControlConfig* recording_control_out,
    std::string* error_out)
{
    orange::session::RecordingControlConfig resolved;
    bool found = false;
    for (const auto& stream : plan.streams) {
        if (stream.record_for_seconds <= 0 && stream.clip_seconds <= 0) {
            continue;
        }
        if (stream.clip_seconds > 0 && stream.record_for_seconds <= 0) {
            if (error_out) {
                *error_out =
                    "GUI external rolling requires record_for_seconds when clip_seconds is set";
            }
            return false;
        }
        if (!found) {
            resolved.record_for_seconds = stream.record_for_seconds;
            resolved.clip_seconds = stream.clip_seconds;
            found = true;
            continue;
        }
        if (resolved.record_for_seconds != stream.record_for_seconds ||
            resolved.clip_seconds != stream.clip_seconds) {
            if (error_out) {
                *error_out =
                    "GUI external rolling requires consistent recording_control across streams";
            }
            return false;
        }
    }
    if (recording_control_out) {
        *recording_control_out = resolved;
    }
    return true;
}

void gui_append_error_message(std::string& target, const std::string& message)
{
    if (message.empty()) {
        return;
    }
    if (target.find(message) != std::string::npos) {
        return;
    }
    if (!target.empty()) {
        target += "; ";
    }
    target += message;
}

nlohmann::json gui_crop_rollover_json(
    const orange::session::RecordingControlConfig& recording_control,
    const std::string& status)
{
    if (recording_control.clip_seconds > 0) {
        return {
            {"requested", true},
            {"status", status.empty() ? "completed" : status},
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

bool gui_attach_crop_rolling_outputs_to_clips(
    const nlohmann::json& recording_backend,
    std::map<int, orange::session::RollingClipManifestOptions>* clips_by_index,
    std::string* error_out)
{
    if (!clips_by_index) {
        return true;
    }
    const nlohmann::json crop_recording =
        recording_backend.value("crop_recording", nlohmann::json::object());
    if (!crop_recording.is_object()) {
        return true;
    }
    const nlohmann::json rolling_clips =
        crop_recording.value("rolling_clips", nlohmann::json::object());
    if (!rolling_clips.is_object() || rolling_clips.empty()) {
        return true;
    }

    bool all_attached = true;
    for (auto it = rolling_clips.begin(); it != rolling_clips.end(); ++it) {
        const std::string serial = it.key();
        if (serial.empty() || !it.value().is_array()) {
            continue;
        }
        for (const nlohmann::json& clip : it.value()) {
            if (!clip.is_object()) {
                continue;
            }
            const int clip_index = clip.value("clip_index", -1);
            if (clip_index < 0) {
                continue;
            }
            auto clip_it = clips_by_index->find(clip_index);
            if (clip_it == clips_by_index->end()) {
                all_attached = false;
                if (error_out) {
                    gui_append_error_message(
                        *error_out,
                        "external crop rolling clip " +
                            std::to_string(clip_index) +
                            " for camera " + serial +
                            " does not match a full-frame rolling clip");
                }
                continue;
            }

            orange::session::RecordingOutputDescriptor output;
            output.camera_serial = serial;
            output.output_kind = "crop";
            output.role = "sidecar";
            output.backend = "external_ipc";
            output.status = clip.value("status", std::string("completed"));
            output.video_path = clip.value("video", std::string());
            output.metadata_path = clip.value("metadata", std::string());
            output.keyframe_path = clip.value("keyframes", std::string());
            output.perf_path = clip.value("perf", std::string());
            output.summary_path = clip.value("summary", std::string());
            output.frame_count = gui_json_u64_or(clip, "frame_count", 0ULL);
            output.first_recording_frame_id =
                gui_json_u64_or(clip, "first_recording_frame_id", 0ULL);
            output.last_recording_frame_id =
                gui_json_u64_or(clip, "last_recording_frame_id", 0ULL);
            output.recording_frame_id_gaps =
                gui_json_u64_or(clip, "recording_frame_id_gaps", 0ULL);
            output.packet_count = gui_json_u64_or(clip, "packet_count", 0ULL);
            output.packet_count_source =
                clip.value("packet_count_source", std::string());
            output.width = clip.value("width", 0);
            output.height = clip.value("height", 0);
            output.frame_rate = clip.value("frame_rate", 0);
            output.codec = clip.value("codec", std::string("hevc"));
            output.container = clip.value("container", std::string("mp4"));
            output.tuning = clip.value("tuning", std::string("lossless"));
            output.pixel_source_format = "mono8";
            output.encoded_format = "nv12";
            output.coordinate_space = "full_frame_pixels";
            output.details = {
                {"clip_index", clip_index},
                {"clip_id", clip.value("clip_id", std::string())},
                {"stream_id", clip.value("stream_id", std::string())},
                {"video_backend", "external_ipc"},
                {"metadata_backend", "orange_gui_split_crop_csv"},
                {"summary_json", clip.value("summary", std::string())},
                {"selection_policy", "largest_detection_by_confidence"},
                {"blank_frame_policy", "encode_black_frame_when_no_detection"},
                {"recording_control",
                 crop_recording.value("recording_control", nlohmann::json::object())},
                {"rollover",
                 crop_recording.value("rollover", nlohmann::json::object())}
            };
            clip_it->second.recording_outputs.push_back(std::move(output));
        }
    }
    return all_attached;
}

nlohmann::json gui_build_rolling_recording_session_snapshot_update(
    const std::string& recording_folder,
    const nlohmann::json& manifest,
    const orange::session::RecordingSessionIndexArtifacts& index_artifacts,
    const nlohmann::json& gui_display_frame_rate)
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
        {"recording_session_status", manifest.value("status", std::string("incomplete"))},
        {"recording_session_camera_count",
         manifest.value("cameras", nlohmann::json::array()).size()},
        {"recording_session_index", indexes},
        {"rolling_clip_count", manifest.value("clips", nlohmann::json::array()).size()},
        {"rolling_index_row_count", indexes.value("row_count", 0)},
        {"gui_display_frame_rate", gui_display_frame_rate}
    };
    if (manifest.contains("recording_backend")) {
        update["recording_backend"] = manifest["recording_backend"];
    }
    return update;
}

bool gui_write_external_rolling_recording_session_manifest(
    const GuiRecordingRunState& run,
    const orange::external_recorder::SupervisorPlan& plan,
    const nlohmann::json& recording_backend,
    const std::vector<orange::session::RecordingOutputDescriptor>& session_recording_outputs,
    const bool recording_session_ok,
    const nlohmann::json& gui_display_frame_rate,
    nlohmann::json* manifest_out,
    nlohmann::json* bridge_out,
    std::string* error_out)
{
    orange::session::RecordingControlConfig recording_control;
    if (!gui_external_recorder_recording_control_from_plan(
            plan,
            &recording_control,
            error_out)) {
        return false;
    }
    if (recording_control.clip_seconds <= 0) {
        if (error_out) {
            *error_out = "GUI external rolling manifest requested without clip_seconds";
        }
        return false;
    }

    const std::string session_id =
        std::filesystem::path(run.recording_folder).filename().string();
    std::map<int, orange::session::RollingClipManifestOptions> clips_by_index;
    std::vector<std::string> camera_serials;
    nlohmann::json summary_paths = nlohmann::json::object();
    bool full_frame_clips_ok = true;

    for (const auto& stream : plan.streams) {
        if (!gui_external_stream_is_full(stream)) {
            continue;
        }
        const std::string serial = stream.camera_serial.empty()
            ? stream.stream_id
            : stream.camera_serial;
        if (serial.empty() || stream.summary_json.empty()) {
            if (error_out) {
                *error_out =
                    "GUI external rolling manifest bridge missing camera serial or summary_json";
            }
            return false;
        }

        nlohmann::json summary;
        std::string read_error;
        if (!gui_read_json_file(stream.summary_json, &summary, &read_error)) {
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
                *error_out =
                    "external recorder rolling_output has no clips for camera " + serial;
            }
            return false;
        }

        camera_serials.push_back(serial);
        summary_paths[serial] = stream.summary_json;
        const int fps = std::max(
            1,
            summary.value("fps", stream.encode_fps > 0 ? stream.encode_fps : 100));

        for (const nlohmann::json& clip : summary_clips) {
            if (!clip.is_object()) {
                continue;
            }
            const int clip_index = clip.value("clip_index", -1);
            if (clip_index < 0) {
                if (error_out) {
                    *error_out =
                        "external recorder rolling clip missing clip_index for camera " +
                        serial;
                }
                return false;
            }
            orange::session::RollingClipManifestOptions& manifest_clip =
                clips_by_index[clip_index];
            if (manifest_clip.clip_id.empty()) {
                manifest_clip.producer = "orange_gui_external_ipc";
                manifest_clip.session_id = session_id;
                manifest_clip.clip_index = clip_index;
                manifest_clip.clip_id =
                    gui_json_string_or(
                        clip,
                        "clip_id",
                        gui_external_recorder_clip_id(clip_index));
                manifest_clip.directory =
                    clip.value("directory", std::string());
                manifest_clip.recording_folder = manifest_clip.directory;
                manifest_clip.status = "completed";
                manifest_clip.drain_completed = true;
            }

            const bool clip_failed = clip.value("failed", false);
            full_frame_clips_ok = full_frame_clips_ok && !clip_failed;
            if (clip_failed) {
                manifest_clip.status = "incomplete";
                manifest_clip.drain_completed = false;
            }

            const uint64_t frame_count =
                gui_json_u64_or(clip, "frame_count", 0ULL);
            const uint64_t first_frame =
                gui_json_u64_or(clip, "first_recording_frame_id", 0ULL);
            const uint64_t last_frame =
                gui_json_u64_or(clip, "last_recording_frame_id", 0ULL);
            const uint64_t packets_written =
                gui_json_u64_or(clip, "packets_written", 0ULL);
            const std::string mp4 =
                clip.value("mp4", std::string());
            const std::string metadata =
                clip.value("metadata", std::string());
            const std::string keyframes =
                clip.value("keyframes", std::string());
            const bool clip_artifacts_ok =
                frame_count > 0 &&
                packets_written > 0 &&
                !mp4.empty() &&
                std::filesystem::exists(mp4) &&
                !metadata.empty() &&
                std::filesystem::exists(metadata) &&
                !keyframes.empty() &&
                std::filesystem::exists(keyframes);
            if (!clip_artifacts_ok) {
                full_frame_clips_ok = false;
                manifest_clip.status = "incomplete";
                manifest_clip.drain_completed = false;
            }

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
            camera_artifact.video_path = mp4;
            camera_artifact.metadata_path = metadata;
            camera_artifact.keyframe_path = keyframes;
            camera_artifact.frame_count = frame_count;
            camera_artifact.first_recording_frame_id = first_frame;
            camera_artifact.last_recording_frame_id = last_frame;
            camera_artifact.recording_frame_id_gaps = 0;
            camera_artifact.packet_count = packets_written;
            camera_artifact.packet_count_source =
                "external_recorder_summary.packets_written";
            manifest_clip.cameras.push_back(camera_artifact);

            orange::session::RecordingOutputDescriptor output;
            output.camera_serial = serial;
            output.output_kind = "full";
            output.role = "ingest_authoritative";
            output.backend = "external_ipc";
            output.status = clip_artifacts_ok && !clip_failed
                ? "completed"
                : "incomplete";
            output.video_path = mp4;
            output.metadata_path = metadata;
            output.keyframe_path = keyframes;
            output.summary_path = stream.summary_json;
            output.frame_count = frame_count;
            output.first_recording_frame_id = first_frame;
            output.last_recording_frame_id = last_frame;
            output.recording_frame_id_gaps = 0;
            output.packet_count = packets_written;
            output.packet_count_source =
                "external_recorder_summary.packets_written";
            output.frame_rate = fps;
            output.codec = stream.codec;
            output.container = "mp4";
            output.tuning = stream.tuning;
            output.pixel_source_format = "mono8";
            output.encoded_format = "nv12";
            output.coordinate_space = "full_frame_pixels";
            output.details = {
                {"clip_index", clip_index},
                {"clip_id", manifest_clip.clip_id},
                {"stream_id", stream.stream_id},
                {"analytics_gpu_id", stream.analytics_gpu_id},
                {"recorder_gpu_id", stream.recorder_gpu_id},
                {"recording_control",
                 orange::session::build_recording_control_json(recording_control)},
                {"rollover",
                 {
                     {"requested", true},
                     {"status", "completed"},
                     {"implementation",
                      orange::external_recorder::kExternalRecorderRollingImplementation},
                     {"seamless_writer_switch", true},
                     {"records_during_rollover", true},
                     {"boundary", "gop_first_frame_id"}
                 }}
            };
            manifest_clip.recording_outputs.push_back(std::move(output));
        }
    }

    if (clips_by_index.empty()) {
        if (error_out) {
            *error_out = "GUI external rolling manifest bridge found no clips";
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
    std::string crop_output_attachment_error;
    const bool crop_output_attachments_ok =
        gui_attach_crop_rolling_outputs_to_clips(
            recording_backend,
            &clips_by_index,
            &crop_output_attachment_error);
    if (!crop_output_attachments_ok) {
        full_frame_clips_ok = false;
    }
    for (auto it = clips_by_index.begin(); it != clips_by_index.end(); ++it) {
        orange::session::RollingClipManifestOptions clip = std::move(it->second);
        if (clip.recording_folder.empty()) {
            if (error_out) {
                *error_out =
                    "GUI external rolling clip missing output directory for clip " +
                    std::to_string(clip.clip_index);
            }
            return false;
        }
        if (clip.cameras.size() != camera_serials.size()) {
            full_frame_clips_ok = false;
            clip.status = "incomplete";
            clip.drain_completed = false;
        }
        const bool final_clip = std::next(it) == clips_by_index.end();
        clip.start_reason = it == clips_by_index.begin() ? "recording_start" : "rollover";
        clip.stop_reason = final_clip ? run.stop_reason : "clip_seconds_elapsed";
        clip.final_clip = final_clip;
        clip.timed_stop_hit =
            final_clip && clip.stop_reason == "record_for_seconds_elapsed";
        clip.requested_duration_s =
            final_clip
                ? clip.actual_duration_s
                : static_cast<double>(recording_control.clip_seconds);
        clip.rollover_request_id = 0;
        if (!final_clip) {
            clip.rollover_at_recording_frame_id =
                std::next(it)->second.first_recording_frame_id;
        } else if (it != clips_by_index.begin()) {
            clip.rollover_at_recording_frame_id = clip.first_recording_frame_id;
        }
        clip.pending_next_clip = false;
        if (it == clips_by_index.begin()) {
            clip.started_at_utc = run.recording_started_at_utc;
            clip.started_at_elapsed_s = 0.0;
        }
        if (final_clip) {
            clip.stop_requested_at_utc = run.recording_stop_requested_at_utc;
            clip.stop_requested_at_elapsed_s =
                gui_elapsed_seconds_between(
                    run.recording_started_at,
                    run.recording_stop_requested_at);
        }
        clip.finalized_at_utc = run.recording_drained_at_utc;
        clip.finalized_at_elapsed_s =
            gui_elapsed_seconds_between(
                run.recording_started_at,
                run.recording_drained_at);
        clip.drain_duration_s =
            final_clip
                ? gui_elapsed_seconds_between(
                      run.recording_stop_requested_at,
                      run.recording_drained_at)
                : 0.0;
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

    nlohmann::json rolling_recording_backend = recording_backend;
    if (!rolling_recording_backend.is_object()) {
        rolling_recording_backend = nlohmann::json::object();
    }
    if (!full_frame_clips_ok) {
        rolling_recording_backend["status"] = "incomplete";
    }
    if (!crop_output_attachments_ok) {
        rolling_recording_backend["status"] = "incomplete";
        if (rolling_recording_backend.contains("crop_recording") &&
            rolling_recording_backend["crop_recording"].is_object()) {
            rolling_recording_backend["crop_recording"]["status"] = "incomplete";
            if (!crop_output_attachment_error.empty()) {
                std::string combined_error =
                    rolling_recording_backend["crop_recording"].value(
                        "error",
                        std::string());
                gui_append_error_message(combined_error, crop_output_attachment_error);
                rolling_recording_backend["crop_recording"]["error"] = combined_error;
            }
        }
    }
    rolling_recording_backend["recording_control"] =
        orange::session::build_recording_control_json(recording_control);
    rolling_recording_backend["rollover"] = {
        {"requested", true},
        {"status", full_frame_clips_ok ? "completed" : "incomplete"},
        {"implementation",
         orange::external_recorder::kExternalRecorderRollingImplementation},
        {"seamless_writer_switch", true},
        {"records_during_rollover", true},
        {"boundary", "gop_first_frame_id"},
        {"next_writer_preopened", false}
    };
    rolling_recording_backend["summary_json"] = summary_paths;

    orange::session::RollingRecordingSessionManifestOptions manifest_options;
    manifest_options.producer = "orange_gui_external_ipc";
    manifest_options.session_id = session_id;
    manifest_options.created_at_utc = run.recording_started_at_utc;
    manifest_options.updated_at_utc = run.recording_drained_at_utc;
    manifest_options.recording_folder = run.recording_folder;
    manifest_options.status =
        recording_session_ok && full_frame_clips_ok ? "completed" : "incomplete";
    manifest_options.requested_stream_duration_seconds =
        recording_control.record_for_seconds;
    manifest_options.stream_started_at_utc = run.recording_started_at_utc;
    manifest_options.stream_finished_at_utc = run.recording_drained_at_utc;
    manifest_options.stream_actual_elapsed_s =
        gui_elapsed_seconds_between(
            run.recording_started_at,
            run.recording_drained_at);
    manifest_options.recording_control = recording_control;
    manifest_options.recording_started = true;
    manifest_options.recording_started_at_utc = run.recording_started_at_utc;
    manifest_options.recording_started_at_elapsed_s = 0.0;
    manifest_options.recording_stop_requested = true;
    manifest_options.recording_stop_requested_at_utc =
        run.recording_stop_requested_at_utc;
    manifest_options.recording_stop_requested_at_elapsed_s =
        gui_elapsed_seconds_between(
            run.recording_started_at,
            run.recording_stop_requested_at);
    manifest_options.recording_stop_reason = run.stop_reason;
    manifest_options.recording_drain_completed = true;
    manifest_options.recording_drained_at_utc = run.recording_drained_at_utc;
    manifest_options.recording_drained_at_elapsed_s =
        gui_elapsed_seconds_between(
            run.recording_started_at,
            run.recording_drained_at);
    manifest_options.actual_recording_duration_s =
        gui_elapsed_seconds_between(
            run.recording_started_at,
            run.recording_stop_requested_at);
    manifest_options.drain_duration_s =
        gui_elapsed_seconds_between(
            run.recording_stop_requested_at,
            run.recording_drained_at);
    manifest_options.sum_clip_actual_duration_s = sum_clip_actual_duration_s;
    manifest_options.rollover_implementation =
        orange::external_recorder::kExternalRecorderRollingImplementation;
    manifest_options.rollover_next_writer_preopened = false;
    manifest_options.recording_stop_control = run.stop_control;
    manifest_options.recording_backend = std::move(rolling_recording_backend);
    manifest_options.recording_outputs = session_recording_outputs;
    manifest_options.camera_serials = std::move(camera_serials);
    manifest_options.clips = std::move(clip_options);

    const nlohmann::json manifest =
        orange::session::build_rolling_clip_recording_session_manifest(
            manifest_options);
    const std::filesystem::path manifest_path =
        std::filesystem::path(run.recording_folder) / "recording_session.json";
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
        gui_build_rolling_recording_session_snapshot_update(
            run.recording_folder,
            manifest,
            index_artifacts,
            gui_display_frame_rate);
    if (!update_recording_snapshot_session_artifacts(
            run.recording_folder,
            snapshot_update)) {
        if (error_out) {
            *error_out =
                "failed to update recording_snapshot.json with GUI external IPC rolling index";
        }
        return false;
    }
    if (manifest.contains("recording_outputs") &&
        manifest["recording_outputs"].is_object() &&
        !update_recording_snapshot_recording_outputs(
            run.recording_folder,
            manifest["recording_outputs"])) {
        if (error_out) {
            *error_out =
                "failed to update recording_snapshot.json with GUI external IPC recording outputs";
        }
        return false;
    }

    if (manifest_out) {
        *manifest_out = manifest;
    }
    if (bridge_out) {
        *bridge_out = {
            {"pass", recording_session_ok && full_frame_clips_ok},
            {"full_frame_pass", full_frame_clips_ok},
            {"path", manifest_path.string()},
            {"mode", "rolling_clips"},
            {"producer", "orange_gui_external_ipc"},
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
    return true;
}

bool has_gui_timepoint(const std::chrono::steady_clock::time_point& timepoint)
{
    return timepoint.time_since_epoch() != std::chrono::steady_clock::duration::zero();
}

std::chrono::seconds gui_elapsed_since(
    const std::chrono::steady_clock::time_point& started_at,
    const std::chrono::steady_clock::time_point& now)
{
    if (!has_gui_timepoint(started_at) || now < started_at) {
        return std::chrono::seconds{0};
    }
    return std::chrono::duration_cast<std::chrono::seconds>(now - started_at);
}

double gui_elapsed_seconds_between(const std::chrono::steady_clock::time_point& start,
                                   const std::chrono::steady_clock::time_point& finish)
{
    if (!has_gui_timepoint(start) || !has_gui_timepoint(finish) || finish < start) {
        return 0.0;
    }
    return std::chrono::duration<double>(finish - start).count();
}

std::vector<std::string> gui_recording_camera_serials(const CameraParams* cameras_params,
                                                      const CameraEachSelect* cameras_select,
                                                      const int num_cameras)
{
    std::vector<std::string> serials;
    if (!cameras_params || !cameras_select || num_cameras <= 0) {
        return serials;
    }
    for (int i = 0; i < num_cameras; ++i) {
        if (cameras_select[i].record && !cameras_params[i].camera_serial.empty()) {
            serials.push_back(cameras_params[i].camera_serial);
        }
    }
    return serials;
}

void gui_note_recording_started(GuiRecordingRunState* run,
                                CameraControl* camera_control,
                                const std::string& recording_folder,
                                const std::string& recording_sink_mode)
{
    if (!run) {
        return;
    }
    run->active = true;
    run->finalizing = false;
    run->finalized = false;
    run->recording_folder = recording_folder;
    run->recording_sink_mode = recording_sink_mode.empty() ? "real" : recording_sink_mode;
    run->recording_started_at = std::chrono::steady_clock::now();
    run->recording_started_at_utc = get_current_utc_timestamp();
    run->recording_stop_requested_at = {};
    run->recording_stop_requested_at_utc.clear();
    run->recording_drained_at = {};
    run->recording_drained_at_utc.clear();
    run->stop_reason = "manual_stop";
    run->stop_control = nlohmann::json::object();
    run->diagnostic_finalize_stall_reported = false;
    if (camera_control) {
        camera_control->preserve_recording_session_state = true;
    }
}

void gui_note_recording_stop_requested(GuiRecordingRunState* run,
                                       const std::string& stop_reason,
                                       nlohmann::json stop_control = nlohmann::json::object())
{
    if (!run || !run->active) {
        return;
    }
    if (!run->finalizing) {
        run->recording_stop_requested_at = std::chrono::steady_clock::now();
        run->recording_stop_requested_at_utc = get_current_utc_timestamp();
        run->stop_control =
            stop_control.is_object() ? std::move(stop_control) : nlohmann::json::object();
    }
    run->finalizing = true;
    run->stop_reason = stop_reason.empty() ? "manual_stop" : stop_reason;
}

void gui_update_local_control_stop_manifest_for_finalized_drain(
    GuiRecordingRunState* run)
{
    if (!run ||
        !run->stop_control.is_object() ||
        run->stop_control.empty()) {
        return;
    }

    const bool drain_timed_out =
        run->stop_control.value("drain_timed_out", false);
    if (!run->stop_control.contains("forced_finalize_requested")) {
        run->stop_control["forced_finalize_requested"] = false;
    }
    if (!run->stop_control.contains("forced_finalize_stream_stop_requested")) {
        run->stop_control["forced_finalize_stream_stop_requested"] = false;
    }
    if (!run->stop_control.contains("forced_finalize_requested_at_utc")) {
        run->stop_control["forced_finalize_requested_at_utc"] = "";
    }
    run->stop_control["drain_completed"] = true;
    run->stop_control["drain_timed_out"] = drain_timed_out;
    run->stop_control["drain_completed_at_utc"] =
        run->recording_drained_at_utc;
    run->stop_control["health"] = drain_timed_out ? "warning" : "ok";
    run->stop_control["error_code"] =
        drain_timed_out ? "drain_timeout" : "";
    run->stop_control["ack_state"] =
        drain_timed_out ? "failed_timeout" : "executed";
    run->stop_control["last_event"] =
        drain_timed_out ? "finalized_after_drain_timeout" : "finalized";
    run->stop_control["last_event_at_utc"] =
        run->recording_drained_at_utc;
}

bool gui_finalize_recording_session_if_ready(GuiRecordingRunState* run,
                                             orange::session::RecordingSessionState* recording_session,
                                             CameraControl* camera_control,
                                             const CameraParams* cameras_params,
                                             const CameraEachSelect* cameras_select,
                                             const int num_cameras,
                                             const int crop_size_px,
                                             const nlohmann::json& gui_display_frame_rate)
{
    if (!run || !run->active || !run->finalizing || run->recording_folder.empty()) {
        return false;
    }
    const bool external_ipc = run->recording_sink_mode == "external_ipc";
    if (external_ipc) {
        if (camera_control && camera_control->record_video) {
            return false;
        }
        if (camera_control &&
            camera_control->active_recorders.load(std::memory_order_relaxed) > 0) {
            camera_control->recording_draining = true;
            camera_control->stop_record = true;
            return false;
        }
        if (recording_session &&
            !orange::session::recording_pipelines_drained(recording_session)) {
            if (camera_control) {
                camera_control->recording_draining = true;
                camera_control->stop_record = true;
            }
            return false;
        }
        if (camera_control) {
            camera_control->recording_draining = false;
            camera_control->stop_record = false;
        }
    } else if (camera_control &&
               (camera_control->record_video ||
                camera_control->recording_draining ||
                camera_control->active_recorders.load(std::memory_order_relaxed) > 0)) {
        return false;
    }

    const int diagnostic_finalize_stall_seconds =
        gui_local_control_diagnostic_finalize_stall_seconds();
    if (diagnostic_finalize_stall_seconds > 0 &&
        run->stop_control.is_object() &&
        !run->stop_control.empty() &&
        has_gui_timepoint(run->recording_stop_requested_at)) {
        const double elapsed =
            std::chrono::duration<double>(
                std::chrono::steady_clock::now() -
                run->recording_stop_requested_at)
                .count();
        if (elapsed < static_cast<double>(diagnostic_finalize_stall_seconds)) {
            if (!run->diagnostic_finalize_stall_reported) {
                run->diagnostic_finalize_stall_reported = true;
                run->stop_control["diagnostic_finalize_stall_seconds"] =
                    diagnostic_finalize_stall_seconds;
                run->stop_control["diagnostic_finalize_stall_active"] = true;
                std::cerr
                    << "[GUI][local_control] diagnostic finalize stall active"
                    << " seconds=" << diagnostic_finalize_stall_seconds
                    << " request_id="
                    << run->stop_control.value("request_id", std::string())
                    << " operation_id="
                    << run->stop_control.value("operation_id", std::string())
                    << std::endl;
            }
            return false;
        }
        if (run->diagnostic_finalize_stall_reported) {
            run->stop_control["diagnostic_finalize_stall_active"] = false;
        }
    }

    run->recording_drained_at = std::chrono::steady_clock::now();
    run->recording_drained_at_utc = get_current_utc_timestamp();
    if (!has_gui_timepoint(run->recording_stop_requested_at)) {
        run->recording_stop_requested_at = run->recording_drained_at;
        run->recording_stop_requested_at_utc = run->recording_drained_at_utc;
    }
    gui_update_local_control_stop_manifest_for_finalized_drain(run);

    std::vector<std::string> camera_serials =
        gui_recording_camera_serials(cameras_params, cameras_select, num_cameras);
    std::vector<orange::session::RecordingSessionCameraArtifact> camera_artifacts;
    nlohmann::json recording_backend = nlohmann::json::object();
    bool external_recorder_ok = true;
    std::string external_recorder_error;
    std::vector<orange::session::RecordingOutputDescriptor> external_crop_outputs;
    bool crop_external_recorder_active =
        recording_session &&
        recording_session->crop_recording_sink_mode == "external_ipc" &&
        !recording_session->external_crop_recorder_lifecycle.plan.streams.empty();
    bool crop_external_recorder_lifecycle_ok = true;
    bool crop_external_recorder_ok = true;
    std::string crop_external_recorder_error;
    auto json_string_or =
        [](const nlohmann::json& object,
           const char* key,
           const std::string& fallback) -> std::string {
            if (object.is_object()) {
                const auto it = object.find(key);
                if (it != object.end() && it->is_string()) {
                    const std::string value = it->get<std::string>();
                    if (!value.empty()) {
                        return value;
                    }
                }
            }
            return fallback;
        };
    auto json_u64_or =
        [](const nlohmann::json& object,
           const char* key,
           const uint64_t fallback) -> uint64_t {
            if (object.is_object()) {
                const auto it = object.find(key);
                if (it != object.end() && it->is_number_unsigned()) {
                    return it->get<uint64_t>();
                }
                if (it != object.end() && it->is_number_integer()) {
                    const int64_t value = it->get<int64_t>();
                    return value > 0 ? static_cast<uint64_t>(value) : 0;
                }
            }
            return fallback;
        };
    auto json_double_or =
        [](const nlohmann::json& object,
           const char* key,
           const double fallback) -> double {
            if (object.is_object()) {
                const auto it = object.find(key);
                if (it != object.end() && it->is_number()) {
                    return it->get<double>();
                }
            }
            return fallback;
        };
    auto append_error_message =
        [](std::string& target, const std::string& message) {
            if (message.empty()) {
                return;
            }
            if (target.find(message) != std::string::npos) {
                return;
            }
            if (!target.empty()) {
                target += "; ";
            }
            target += message;
        };

    // Crop recorder env overrides are stacked after full-frame recorder
    // overrides. Stop crop first so scoped environment restoration unwinds in
    // the reverse order of startup.
    if (crop_external_recorder_active &&
        recording_session->external_crop_recorder_lifecycle.started) {
        std::string stop_error;
        if (!orange::external_recorder::StopSupervisedRecorderLifecycle(
                &recording_session->external_crop_recorder_lifecycle,
                &stop_error)) {
            crop_external_recorder_ok = false;
            crop_external_recorder_lifecycle_ok = false;
            append_error_message(
                crop_external_recorder_error,
                stop_error.empty()
                    ? "external crop recorder supervisor shutdown failed"
                    : stop_error);
        }
    }

    if (external_ipc) {
        if (!recording_session) {
            external_recorder_ok = false;
            external_recorder_error = "external IPC recording session state is unavailable";
        } else {
            orange::session::reset_external_ipc_connections(recording_session);
            std::string stop_error;
            if (!orange::external_recorder::StopSupervisedRecorderLifecycle(
                    &recording_session->external_recorder_lifecycle,
                    &stop_error)) {
                external_recorder_ok = false;
                append_error_message(
                    external_recorder_error,
                    stop_error.empty()
                        ? "external recorder supervisor shutdown failed"
                        : stop_error);
            }
            if (!recording_session->external_recorder_lifecycle.last_artifact_error.empty()) {
                append_error_message(
                    external_recorder_error,
                    recording_session->external_recorder_lifecycle.last_artifact_error);
                recording_session->external_recorder_lifecycle.last_artifact_error.clear();
                external_recorder_ok = false;
            }
            if (!recording_session->external_recorder_lifecycle.last_runtime_error.empty()) {
                append_error_message(
                    external_recorder_error,
                    recording_session->external_recorder_lifecycle.last_runtime_error);
                recording_session->external_recorder_lifecycle.last_runtime_error.clear();
                external_recorder_ok = false;
            }
            if (!recording_session->external_recorder_last_error.empty()) {
                append_error_message(
                    external_recorder_error,
                    recording_session->external_recorder_last_error);
                external_recorder_ok = false;
            }

            nlohmann::json ingress_stats = nlohmann::json::object();
            uint64_t total_ingress_submitted = 0;
            uint64_t total_ingress_acked = 0;
            uint64_t total_ingress_failures = 0;
            uint64_t total_ingress_ack_timeouts = 0;
            const size_t pipeline_count = std::min(
                recording_session->recording_pipelines.size(),
                static_cast<size_t>(std::max(0, num_cameras)));
            for (size_t i = 0; i < pipeline_count; ++i) {
                const auto& pipeline = recording_session->recording_pipelines[i];
                if (!pipeline || !pipeline->recording_ingress()) {
                    continue;
                }
                if (cameras_select && !cameras_select[i].record) {
                    continue;
                }
                const std::string serial =
                    cameras_params && !cameras_params[i].camera_serial.empty()
                        ? cameras_params[i].camera_serial
                        : std::to_string(i);
                const RecordingIngressStats stats =
                    pipeline->recording_ingress()->GetStats();
                total_ingress_submitted += stats.submitted_frames;
                total_ingress_acked += stats.external_ipc_frames_acked;
                total_ingress_failures += stats.external_ipc_failures;
                total_ingress_ack_timeouts += stats.external_ipc_ack_timeouts;
                ingress_stats[serial] = {
                    {"submitted_frames", stats.submitted_frames},
                    {"external_ipc_frames_acked", stats.external_ipc_frames_acked},
                    {"external_ipc_failures", stats.external_ipc_failures},
                    {"external_ipc_ack_timeouts", stats.external_ipc_ack_timeouts}
                };
            }
            if (total_ingress_failures > 0 ||
                total_ingress_ack_timeouts > 0 ||
                (total_ingress_submitted > 0 &&
                 total_ingress_acked < total_ingress_submitted)) {
                external_recorder_ok = false;
                if (!external_recorder_error.empty()) {
                    external_recorder_error += "; ";
                }
                external_recorder_error +=
                    "external IPC ingress incomplete: submitted=" +
                    std::to_string(total_ingress_submitted) +
                    " acked=" +
                    std::to_string(total_ingress_acked) +
                    " failures=" +
                    std::to_string(total_ingress_failures) +
                    " ack_timeouts=" +
                    std::to_string(total_ingress_ack_timeouts);
            }

            nlohmann::json summary_paths = nlohmann::json::object();
            nlohmann::json merged_mp4s = nlohmann::json::object();
            nlohmann::json keyframe_paths = nlohmann::json::object();
            nlohmann::json gop_routing_paths = nlohmann::json::object();

            for (const auto& stream : recording_session->external_recorder_lifecycle.plan.streams) {
                if (!gui_external_stream_is_full(stream)) {
                    continue;
                }
                const std::string serial = stream.camera_serial.empty()
                    ? stream.stream_id
                    : stream.camera_serial;
                if (serial.empty()) {
                    continue;
                }
                if (std::find(camera_serials.begin(), camera_serials.end(), serial) ==
                    camera_serials.end()) {
                    camera_serials.push_back(serial);
                }

                nlohmann::json summary;
                std::ifstream input(stream.summary_json);
                if (!input) {
                    external_recorder_ok = false;
                    if (!external_recorder_error.empty()) {
                        external_recorder_error += "; ";
                    }
                    external_recorder_error +=
                        "missing external recorder summary for camera " + serial +
                        ": " + stream.summary_json;
                    continue;
                }
                try {
                    input >> summary;
                } catch (const std::exception& ex) {
                    external_recorder_ok = false;
                    if (!external_recorder_error.empty()) {
                        external_recorder_error += "; ";
                    }
                    external_recorder_error +=
                        "invalid external recorder summary for camera " + serial +
                        ": " + ex.what();
                    continue;
                }

                const nlohmann::json merged =
                    summary.value("merged_output", nlohmann::json::object());
                const nlohmann::json outputs =
                    summary.value("outputs", nlohmann::json::object());
                const nlohmann::json external_encode =
                    summary.value("external_encode", nlohmann::json::object());
                const bool worker_failed = summary.value("worker_failed", false);
                const bool merged_enabled =
                    merged.is_object() && merged.value("enabled", false);
                const bool merged_failed =
                    merged_enabled && merged.value("failed", false);
                const uint64_t frames_received =
                    summary.value("frames_received", 0ULL);
                const uint64_t frames_encoded =
                    summary.value("frames_encoded", frames_received);
                const uint64_t external_packets =
                    json_u64_or(external_encode, "mp4_packets", 0ULL);
                const uint64_t packets_written = merged_enabled
                    ? json_u64_or(merged, "packets_written", external_packets)
                    : external_packets;
                const std::string output_mp4 =
                    json_string_or(outputs, "mp4", stream.mp4);
                const std::string output_keyframes =
                    json_string_or(outputs, "mp4_keyframe", stream.mp4_keyframe);
                const std::string mp4 = merged_enabled
                    ? json_string_or(merged, "mp4", output_mp4)
                    : output_mp4;
                const std::string keyframes = merged_enabled
                    ? json_string_or(merged, "mp4_keyframe", output_keyframes)
                    : output_keyframes;

                if (worker_failed || merged_failed || frames_received == 0 ||
                    frames_encoded == 0 || frames_encoded != frames_received ||
                    packets_written == 0 || mp4.empty() ||
                    !std::filesystem::exists(mp4)) {
                    external_recorder_ok = false;
                    if (!external_recorder_error.empty()) {
                        external_recorder_error += "; ";
                    }
                    external_recorder_error +=
                        "external recorder output incomplete for camera " + serial;
                }

                orange::session::RecordingSessionCameraArtifact artifact;
                artifact.camera_serial = serial;
                artifact.video_path = mp4;
                artifact.metadata_path = stream.summary_json;
                artifact.keyframe_path = keyframes;
                artifact.frame_count = frames_encoded;
                artifact.first_recording_frame_id = frames_encoded > 0 ? 1 : 0;
                artifact.last_recording_frame_id = frames_encoded;
                artifact.recording_frame_id_gaps = 0;
                artifact.packet_count = packets_written;
                artifact.packet_count_source = "external_recorder_summary.packets_written";
                camera_artifacts.push_back(std::move(artifact));

                summary_paths[serial] = stream.summary_json;
                merged_mp4s[serial] = mp4;
                keyframe_paths[serial] = keyframes;
                gop_routing_paths[serial] = stream.gop_routing_csv;
            }

            recording_backend = {
                {"mode", "external_ipc"},
                {"status", external_recorder_ok ? "completed" : "incomplete"},
                {"artifact_root", recording_session->external_recorder_lifecycle.plan.artifact_root},
                {"source", "external_recorder_summary"},
                {"ingress_stats", ingress_stats},
                {"ingress_totals",
                 {
                     {"submitted_frames", total_ingress_submitted},
                     {"external_ipc_frames_acked", total_ingress_acked},
                     {"external_ipc_failures", total_ingress_failures},
                     {"external_ipc_ack_timeouts", total_ingress_ack_timeouts}
                 }},
                {"summary_json", summary_paths},
                {"merged_mp4", merged_mp4s},
                {"keyframes", keyframe_paths},
                {"gop_routing_csv", gop_routing_paths},
                {"external_recorder_contract_path", recording_session->external_recorder_contract_path},
                {"external_recorder_supervisor_plan_path",
                 recording_session->external_recorder_supervisor_plan_path},
                {"external_recorder_session_json",
                 (std::filesystem::path(recording_session->external_recorder_lifecycle.plan.artifact_root) /
                  "external_recorder_session.json").string()},
                {"external_recorder_finalization_json",
                 (std::filesystem::path(recording_session->external_recorder_lifecycle.plan.artifact_root) /
                  "external_recorder_finalization.json").string()}
            };
            if (!external_recorder_error.empty()) {
                recording_backend["error"] = external_recorder_error;
            }
        }
    } else {
        camera_artifacts =
            orange::session::build_recording_camera_artifacts(
                camera_serials,
                run->recording_folder,
                true);
    }

    if (crop_external_recorder_active) {
        if (!recording_session->external_crop_recorder_lifecycle.last_artifact_error.empty()) {
            append_error_message(
                crop_external_recorder_error,
                recording_session->external_crop_recorder_lifecycle.last_artifact_error);
            recording_session->external_crop_recorder_lifecycle.last_artifact_error.clear();
            crop_external_recorder_ok = false;
            crop_external_recorder_lifecycle_ok = false;
        }
        if (!recording_session->external_crop_recorder_lifecycle.last_runtime_error.empty()) {
            append_error_message(
                crop_external_recorder_error,
                recording_session->external_crop_recorder_lifecycle.last_runtime_error);
            recording_session->external_crop_recorder_lifecycle.last_runtime_error.clear();
            crop_external_recorder_ok = false;
            crop_external_recorder_lifecycle_ok = false;
        }
        if (!recording_session->external_crop_recorder_last_error.empty()) {
            append_error_message(
                crop_external_recorder_error,
                recording_session->external_crop_recorder_last_error);
            crop_external_recorder_ok = false;
            crop_external_recorder_lifecycle_ok = false;
        }

        nlohmann::json crop_summary_paths = nlohmann::json::object();
        nlohmann::json crop_mp4_paths = nlohmann::json::object();
        nlohmann::json crop_keyframe_paths = nlohmann::json::object();
        nlohmann::json crop_gop_routing_paths = nlohmann::json::object();
        nlohmann::json crop_stream_config = nlohmann::json::object();
        nlohmann::json crop_frames_received = nlohmann::json::object();
        nlohmann::json crop_frames_encoded = nlohmann::json::object();
        nlohmann::json crop_encode_dropped = nlohmann::json::object();
        nlohmann::json crop_external_frames_dropped = nlohmann::json::object();
        nlohmann::json crop_encode_queue_depth = nlohmann::json::object();
        nlohmann::json crop_encode_queue_high_water = nlohmann::json::object();
        nlohmann::json crop_enqueue_age_p95_ms = nlohmann::json::object();
        nlohmann::json crop_rolling_clips = nlohmann::json::object();
        orange::session::RecordingControlConfig crop_recording_control_config;
        std::string crop_recording_control_error;
        if (!gui_external_recorder_recording_control_from_plan(
                recording_session->external_crop_recorder_lifecycle.plan,
                &crop_recording_control_config,
                &crop_recording_control_error)) {
            append_error_message(crop_external_recorder_error, crop_recording_control_error);
            crop_external_recorder_ok = false;
            crop_external_recorder_lifecycle_ok = false;
            crop_recording_control_config = orange::session::RecordingControlConfig{};
        }
        const nlohmann::json crop_recording_control =
            orange::session::build_recording_control_json(crop_recording_control_config);
        const bool crop_rolling_requested =
            crop_recording_control_config.clip_seconds > 0;

        auto append_external_crop_output =
            [&](const auto& stream,
                const std::string& serial,
                const std::string& mp4,
                const std::string& keyframes,
                const uint64_t frames_encoded,
                const uint64_t packets_written,
                const bool stream_ok,
                const std::string& stream_error) {
                orange::session::RecordingOutputDescriptor output;
                output.camera_serial = serial;
                output.output_kind = "crop";
                output.role = "sidecar";
                output.backend = "external_ipc";
                output.status = stream_ok ? "completed" : "incomplete";
                output.video_path = mp4;
                output.metadata_path = "Cam" + serial + "_crop_meta.csv";
                output.keyframe_path = keyframes;
                output.perf_path = "Cam" + serial + "_crop_perf.csv";
                output.sidecar_perf_path = "Cam" + serial + "_crop_sidecar_perf.csv";
                output.summary_path = stream.summary_json;
                output.frame_count = frames_encoded;
                output.first_recording_frame_id = frames_encoded > 0 ? 1 : 0;
                output.last_recording_frame_id = frames_encoded;
                output.recording_frame_id_gaps = 0;
                output.packet_count = packets_written;
                output.packet_count_source = "external_crop_recorder_summary.packets_written";
                const int resolved_crop_size =
                    CropAndEncodeWorker::SanitizeCropSize(crop_size_px);
                const nlohmann::json stream_crop_rollover =
                    gui_crop_rollover_json(
                        crop_recording_control_config,
                        stream_ok ? "completed" : "incomplete");
                output.width = resolved_crop_size;
                output.height = resolved_crop_size;
                output.frame_rate = stream.encode_fps;
                output.codec = stream.codec;
                output.container = "mp4";
                output.tuning = stream.tuning;
                output.pixel_source_format = "mono8";
                output.encoded_format = "nv12";
                output.coordinate_space = "full_frame_pixels";
                output.details = {
                    {"stream_id", stream.stream_id},
                    {"stream_kind", stream.stream_kind},
                    {"output_kind", stream.output_kind},
                    {"camera_serial", stream.camera_serial},
                    {"env_key", stream.env_key},
                    {"scope", crop_rolling_requested ? "session_aggregate" : "single_clip"},
                    {"video_backend", "external_ipc"},
                    {"metadata_backend", "orange_gui"},
                    {"analytics_gpu_id", stream.analytics_gpu_id},
                    {"recorder_gpu_id", stream.recorder_gpu_id},
                    {"encode_queue_depth", stream.encode_queue_depth},
                    {"socket_path", stream.socket_path},
                    {"summary_json", stream.summary_json},
                    {"status_json", stream.status_json},
                    {"recording_control", crop_recording_control},
                    {"rollover", stream_crop_rollover},
                    {"selection_policy", "largest_detection_by_confidence"},
                    {"blank_frame_policy", "encode_black_frame_when_no_detection"}
                };
                if (!stream_ok && !stream_error.empty()) {
                    output.details["status_reason"] = stream_error;
                }
                external_crop_outputs.push_back(std::move(output));

                crop_summary_paths[serial] = stream.summary_json;
                crop_mp4_paths[serial] = mp4;
                crop_keyframe_paths[serial] = keyframes;
                crop_gop_routing_paths[serial] = stream.gop_routing_csv;
                crop_stream_config[serial] = {
                    {"stream_id", stream.stream_id},
                    {"stream_kind", stream.stream_kind},
                    {"output_kind", stream.output_kind},
                    {"camera_serial", stream.camera_serial},
                    {"env_key", stream.env_key},
                    {"analytics_gpu_id", stream.analytics_gpu_id},
                    {"recorder_gpu_id", stream.recorder_gpu_id},
                    {"encode_queue_depth", stream.encode_queue_depth},
                    {"socket_path", stream.socket_path},
                    {"summary_json", stream.summary_json},
                    {"status_json", stream.status_json},
                    {"encode_fps", stream.encode_fps},
                    {"encode_max_fps", stream.encode_max_fps},
                    {"gop", stream.gop},
                    {"terminal_tail_coalesce_frames",
                     stream.terminal_tail_coalesce_frames},
                    {"codec", stream.codec},
                    {"tuning", stream.tuning},
                    {"recording_control", crop_recording_control},
                    {"rollover", stream_crop_rollover}
                };
            };

        for (const auto& stream : recording_session->external_crop_recorder_lifecycle.plan.streams) {
            if (!gui_external_stream_is_crop(stream)) {
                continue;
            }
            const std::string serial = gui_crop_stream_camera_serial(stream);
            if (serial.empty()) {
                continue;
            }

            bool stream_ok = crop_external_recorder_lifecycle_ok;
            std::string stream_error;
            nlohmann::json summary;
            std::string summary_error;
            if (!gui_read_json_file(stream.summary_json, &summary, &summary_error)) {
                stream_ok = false;
                crop_external_recorder_ok = false;
                stream_error =
                    "external crop recorder summary unavailable for camera " +
                    serial + ": " + summary_error;
                if (!crop_external_recorder_error.empty()) {
                    crop_external_recorder_error += "; ";
                }
                crop_external_recorder_error += stream_error;
                append_external_crop_output(
                    stream,
                    serial,
                    stream.mp4,
                    stream.mp4_keyframe,
                    0,
                    0,
                    false,
                    stream_error);
                continue;
            }

            const nlohmann::json merged =
                summary.value("merged_output", nlohmann::json::object());
            const nlohmann::json outputs =
                summary.value("outputs", nlohmann::json::object());
            const nlohmann::json external_encode =
                summary.value("external_encode", nlohmann::json::object());
            const bool worker_failed = summary.value("worker_failed", false);
            const bool merged_enabled =
                merged.is_object() && merged.value("enabled", false);
            const bool merged_failed =
                merged_enabled && merged.value("failed", false);
            const uint64_t frames_received =
                summary.value("frames_received", 0ULL);
            const uint64_t frames_encoded =
                summary.value("frames_encoded", frames_received);
            const uint64_t summary_encode_dropped =
                json_u64_or(summary, "encode_dropped", 0ULL);
            const uint64_t summary_external_frames_dropped =
                json_u64_or(external_encode, "frames_dropped", 0ULL);
            const uint64_t summary_encode_queue_depth =
                json_u64_or(
                    summary,
                    "encode_queue_depth",
                    stream.encode_queue_depth > 0
                        ? static_cast<uint64_t>(stream.encode_queue_depth)
                        : 0ULL);
            const uint64_t summary_encode_queue_high_water =
                json_u64_or(summary, "encode_queue_high_water", 0ULL);
            const double summary_enqueue_age_p95_ms =
                json_double_or(external_encode, "enqueue_age_p95_ms", -1.0);
            const uint64_t external_packets =
                json_u64_or(external_encode, "mp4_packets", 0ULL);
            const uint64_t packets_written = merged_enabled
                ? json_u64_or(merged, "packets_written", external_packets)
                : external_packets;
            const std::string output_mp4 =
                json_string_or(outputs, "mp4", stream.mp4);
            const std::string output_keyframes =
                json_string_or(outputs, "mp4_keyframe", stream.mp4_keyframe);
            const std::string mp4 = merged_enabled
                ? json_string_or(merged, "mp4", output_mp4)
                : output_mp4;
            const std::string keyframes = merged_enabled
                ? json_string_or(merged, "mp4_keyframe", output_keyframes)
                : output_keyframes;
            const nlohmann::json rolling =
                summary.value("rolling_output", nlohmann::json::object());
            const bool summary_rolling_enabled =
                rolling.is_object() && rolling.value("enabled", false);

            if (worker_failed || merged_failed || frames_received == 0 ||
                frames_encoded == 0 || frames_encoded != frames_received ||
                packets_written == 0 || mp4.empty() ||
                !std::filesystem::exists(mp4)) {
                stream_ok = false;
                crop_external_recorder_ok = false;
                stream_error =
                    "external crop recorder output incomplete for camera " + serial;
                if (!crop_external_recorder_error.empty()) {
                    crop_external_recorder_error += "; ";
                }
                crop_external_recorder_error += stream_error;
            }

            if (summary_rolling_enabled) {
                if (!crop_rolling_requested) {
                    stream_ok = false;
                    crop_external_recorder_ok = false;
                    stream_error =
                        "external crop recorder produced rolling output without a crop rolling request";
                    append_error_message(crop_external_recorder_error, stream_error);
                }

                const nlohmann::json summary_clips =
                    rolling.value("clips", nlohmann::json::array());
                if (!summary_clips.is_array() || summary_clips.empty()) {
                    stream_ok = false;
                    crop_external_recorder_ok = false;
                    stream_error =
                        "external crop recorder rolling output has no clips for camera " + serial;
                    append_error_message(crop_external_recorder_error, stream_error);
                } else {
                    std::vector<orange::session::RecordingFrameCsvRange> metadata_ranges;
                    std::vector<orange::session::RecordingFrameCsvRange> perf_ranges;
                    std::vector<nlohmann::json> clip_records;
                    metadata_ranges.reserve(summary_clips.size());
                    perf_ranges.reserve(summary_clips.size());
                    clip_records.reserve(summary_clips.size());
                    const int resolved_crop_size =
                        CropAndEncodeWorker::SanitizeCropSize(crop_size_px);
                    for (const nlohmann::json& clip : summary_clips) {
                        if (!clip.is_object()) {
                            continue;
                        }
                        const int clip_index = clip.value("clip_index", -1);
                        const uint64_t frame_count =
                            json_u64_or(clip, "frame_count", 0ULL);
                        const uint64_t first_frame =
                            json_u64_or(clip, "first_recording_frame_id", 0ULL);
                        const uint64_t last_frame =
                            json_u64_or(clip, "last_recording_frame_id", 0ULL);
                        const uint64_t packets =
                            json_u64_or(clip, "packets_written", 0ULL);
                        const std::string clip_mp4 =
                            json_string_or(clip, "mp4", std::string());
                        const std::string clip_keyframes =
                            json_string_or(clip, "keyframes", std::string());
                        std::filesystem::path clip_dir =
                            clip.value("directory", std::string());
                        if (clip_dir.empty() && !clip_mp4.empty()) {
                            clip_dir = std::filesystem::path(clip_mp4).parent_path();
                        }
                        const std::string clip_metadata =
                            (clip_dir / ("Cam" + serial + "_crop_meta.csv")).string();
                        const std::string clip_perf =
                            (clip_dir / ("Cam" + serial + "_crop_perf.csv")).string();
                        if (clip_index < 0 || frame_count == 0 ||
                            first_frame == 0 || last_frame < first_frame ||
                            clip_mp4.empty() || clip_keyframes.empty() ||
                            clip_dir.empty()) {
                            stream_ok = false;
                            crop_external_recorder_ok = false;
                            stream_error =
                                "external crop recorder rolling clip incomplete for camera " +
                                serial;
                            append_error_message(crop_external_recorder_error, stream_error);
                            continue;
                        }

                        metadata_ranges.push_back({
                            first_frame,
                            last_frame,
                            clip_metadata,
                            0,
                        });
                        perf_ranges.push_back({
                            first_frame,
                            last_frame,
                            clip_perf,
                            0,
                        });
                        clip_records.push_back({
                            {"clip_index", clip_index},
                            {"clip_id",
                             clip.value("clip_id", gui_external_recorder_clip_id(clip_index))},
                            {"status", clip.value("failed", false) ? "incomplete" : "completed"},
                            {"stream_id", stream.stream_id},
                            {"video", clip_mp4},
                            {"metadata", clip_metadata},
                            {"perf", clip_perf},
                            {"keyframes", clip_keyframes},
                            {"summary", stream.summary_json},
                            {"frame_count", frame_count},
                            {"first_recording_frame_id", first_frame},
                            {"last_recording_frame_id", last_frame},
                            {"recording_frame_id_gaps", 0},
                            {"packet_count", packets},
                            {"packet_count_source",
                             "external_crop_recorder_summary.packets_written"},
                            {"width", resolved_crop_size},
                            {"height", resolved_crop_size},
                            {"frame_rate", stream.encode_fps},
                            {"codec", stream.codec},
                            {"container", "mp4"},
                            {"tuning", stream.tuning}
                        });
                    }

                    if (!metadata_ranges.empty()) {
                        std::string split_error;
                        const std::string root_metadata =
                            (std::filesystem::path(run->recording_folder) /
                             ("Cam" + serial + "_crop_meta.csv")).string();
                        if (!orange::session::split_recording_frame_csv_by_ranges(
                                root_metadata,
                                &metadata_ranges,
                                &split_error)) {
                            stream_ok = false;
                            crop_external_recorder_ok = false;
                            append_error_message(
                                crop_external_recorder_error,
                                "failed to split crop metadata for camera " +
                                    serial + ": " + split_error);
                        }
                        split_error.clear();
                        const std::string root_perf =
                            (std::filesystem::path(run->recording_folder) /
                             ("Cam" + serial + "_crop_perf.csv")).string();
                        if (!orange::session::split_recording_frame_csv_by_ranges(
                                root_perf,
                                &perf_ranges,
                                &split_error)) {
                            stream_ok = false;
                            crop_external_recorder_ok = false;
                            append_error_message(
                                crop_external_recorder_error,
                                "failed to split crop perf for camera " +
                                    serial + ": " + split_error);
                        }
                        nlohmann::json stream_clips = nlohmann::json::array();
                        for (size_t i = 0; i < clip_records.size(); ++i) {
                            const uint64_t frame_count =
                                clip_records[i].value("frame_count", 0ULL);
                            clip_records[i]["metadata_rows"] =
                                i < metadata_ranges.size()
                                    ? metadata_ranges[i].rows_written
                                    : 0ULL;
                            clip_records[i]["perf_rows"] =
                                i < perf_ranges.size()
                                    ? perf_ranges[i].rows_written
                                    : 0ULL;
                            if (clip_records[i].value("metadata_rows", 0ULL) != frame_count ||
                                clip_records[i].value("perf_rows", 0ULL) != frame_count) {
                                stream_ok = false;
                                crop_external_recorder_ok = false;
                                append_error_message(
                                    crop_external_recorder_error,
                                    "external crop rolling sidecar row mismatch for camera " +
                                        serial);
                            }
                            stream_clips.push_back(clip_records[i]);
                        }
                        crop_rolling_clips[serial] = std::move(stream_clips);
                    }
                }
            }

            append_external_crop_output(
                stream,
                serial,
                mp4,
                keyframes,
                frames_encoded,
                packets_written,
                stream_ok,
                stream_error);

            crop_frames_received[serial] = frames_received;
            crop_frames_encoded[serial] = frames_encoded;
            crop_encode_dropped[serial] = summary_encode_dropped;
            crop_external_frames_dropped[serial] = summary_external_frames_dropped;
            crop_encode_queue_depth[serial] = summary_encode_queue_depth;
            crop_encode_queue_high_water[serial] = summary_encode_queue_high_water;
            if (summary_enqueue_age_p95_ms >= 0.0) {
                crop_enqueue_age_p95_ms[serial] = summary_enqueue_age_p95_ms;
            }
        }

        if (!recording_backend.is_object() || recording_backend.empty()) {
            recording_backend = {
                {"mode", "real"},
                {"status", "completed"}
            };
        }
        const nlohmann::json crop_rollover_backend =
            gui_crop_rollover_json(
                crop_recording_control_config,
                crop_external_recorder_ok ? "completed" : "incomplete");
        recording_backend["crop_recording"] = {
            {"mode", "external_ipc"},
            {"status", crop_external_recorder_ok ? "completed" : "incomplete"},
            {"artifact_root", recording_session->external_crop_recorder_lifecycle.plan.artifact_root},
            {"source", "external_crop_recorder_summary"},
            {"summary_json", crop_summary_paths},
            {"merged_mp4", crop_mp4_paths},
            {"keyframes", crop_keyframe_paths},
            {"gop_routing_csv", crop_gop_routing_paths},
            {"stream_config", crop_stream_config},
            {"recording_control", crop_recording_control},
            {"rollover", crop_rollover_backend},
            {"frames_received", crop_frames_received},
            {"frames_encoded", crop_frames_encoded},
            {"encode_dropped", crop_encode_dropped},
            {"external_frames_dropped", crop_external_frames_dropped},
            {"encode_queue_depth", crop_encode_queue_depth},
            {"encode_queue_high_water", crop_encode_queue_high_water},
            {"enqueue_age_p95_ms", crop_enqueue_age_p95_ms},
            {"external_crop_recorder_contract_path", recording_session->external_crop_recorder_contract_path},
            {"external_crop_recorder_supervisor_plan_path",
             recording_session->external_crop_recorder_supervisor_plan_path},
            {"external_crop_recorder_session_json",
             (std::filesystem::path(recording_session->external_crop_recorder_lifecycle.plan.artifact_root) /
              "external_recorder_session.json").string()},
            {"external_crop_recorder_finalization_json",
             (std::filesystem::path(recording_session->external_crop_recorder_lifecycle.plan.artifact_root) /
              "external_recorder_finalization.json").string()}
        };
        if (!crop_rolling_clips.empty()) {
            recording_backend["crop_recording"]["rolling_clips"] =
                std::move(crop_rolling_clips);
        }
        if (!crop_external_recorder_error.empty()) {
            recording_backend["crop_recording"]["error"] = crop_external_recorder_error;
        }
    }

    const bool recording_session_ok =
        external_recorder_ok &&
        (!crop_external_recorder_active || crop_external_recorder_ok);

    const std::filesystem::path manifest_path =
        std::filesystem::path(run->recording_folder) / "recording_session.json";
    nlohmann::json manifest = nlohmann::json::object();
    nlohmann::json recording_session_bridge = nlohmann::json::object();
    std::string manifest_mode = "single_clip";
    const std::string manifest_producer =
        external_ipc ? "orange_gui_external_ipc" : "orange_gui";
    const bool external_rolling_requested =
        external_ipc &&
        recording_session &&
        gui_external_recorder_plan_requests_rolling(
            recording_session->external_recorder_lifecycle.plan);

    if (external_rolling_requested) {
        std::string rolling_error;
        if (!gui_write_external_rolling_recording_session_manifest(
                *run,
                recording_session->external_recorder_lifecycle.plan,
                recording_backend,
                external_crop_outputs,
                recording_session_ok,
                gui_display_frame_rate,
                &manifest,
                &recording_session_bridge,
                &rolling_error)) {
            std::cerr << "[GUI][recording] Failed to write GUI external IPC rolling manifest: "
                      << rolling_error << std::endl;
            return false;
        }
        if (!recording_session_bridge.value("full_frame_pass", true)) {
            external_recorder_ok = false;
            append_error_message(
                external_recorder_error,
                "GUI external rolling full-frame clip manifest marked incomplete");
        }
        manifest_mode = "rolling_clips";
    } else {
        orange::session::SingleClipRecordingSessionManifestOptions manifest_options;
        manifest_options.producer = manifest_producer;
        manifest_options.session_id =
            std::filesystem::path(run->recording_folder).filename().string();
        manifest_options.created_at_utc = run->recording_started_at_utc;
        manifest_options.updated_at_utc = run->recording_drained_at_utc;
        manifest_options.recording_folder = run->recording_folder;
        manifest_options.status = recording_session_ok ? "completed" : "incomplete";
        manifest_options.stream_started_at_utc = run->recording_started_at_utc;
        manifest_options.stream_finished_at_utc = run->recording_drained_at_utc;
        manifest_options.stream_actual_elapsed_s =
            gui_elapsed_seconds_between(run->recording_started_at, run->recording_drained_at);
        manifest_options.recording_started = true;
        manifest_options.recording_started_at_utc = run->recording_started_at_utc;
        manifest_options.recording_started_at_elapsed_s = 0.0;
        manifest_options.recording_stop_requested = true;
        manifest_options.recording_stop_requested_at_utc = run->recording_stop_requested_at_utc;
        manifest_options.recording_stop_requested_at_elapsed_s =
            gui_elapsed_seconds_between(run->recording_started_at, run->recording_stop_requested_at);
        manifest_options.recording_stop_reason = run->stop_reason;
        manifest_options.recording_drain_completed = true;
        manifest_options.recording_drained_at_utc = run->recording_drained_at_utc;
        manifest_options.recording_drained_at_elapsed_s =
            gui_elapsed_seconds_between(run->recording_started_at, run->recording_drained_at);
        manifest_options.actual_recording_duration_s =
            gui_elapsed_seconds_between(run->recording_started_at, run->recording_stop_requested_at);
        manifest_options.drain_duration_s =
            gui_elapsed_seconds_between(run->recording_stop_requested_at, run->recording_drained_at);
        manifest_options.timed_stop_hit = false;
        manifest_options.recording_stop_control = run->stop_control;
        manifest_options.recording_backend = recording_backend;
        manifest_options.cameras = std::move(camera_artifacts);
        if (cameras_params && cameras_select) {
            const int resolved_crop_size =
                CropAndEncodeWorker::SanitizeCropSize(crop_size_px);
            for (int i = 0; i < num_cameras; ++i) {
                if (!cameras_select[i].crop_and_encode) {
                    continue;
                }
                std::string camera_serial = cameras_params[i].camera_serial;
                if (camera_serial.empty()) {
                    camera_serial = std::to_string(cameras_params[i].camera_id);
                }
                auto external_crop_it = std::find_if(
                    external_crop_outputs.begin(),
                    external_crop_outputs.end(),
                    [&](const orange::session::RecordingOutputDescriptor& output) {
                        return output.camera_serial == camera_serial;
                    });
                if (external_crop_it != external_crop_outputs.end()) {
                    manifest_options.recording_outputs.push_back(*external_crop_it);
                } else if (crop_external_recorder_active) {
                    const std::filesystem::path external_crop_root =
                        recording_session
                            ? std::filesystem::path(
                                  recording_session->external_crop_recorder_lifecycle.plan.artifact_root)
                            : std::filesystem::path(run->recording_folder) / "external_crop_recorder";
                    const std::string external_crop_prefix =
                        (external_crop_root / ("Cam" + camera_serial + "_crop_external")).string();
                    orange::session::RecordingOutputDescriptor output;
                    output.camera_serial = camera_serial;
                    output.output_kind = "crop";
                    output.role = "sidecar";
                    output.backend = "external_ipc";
                    output.status = "incomplete";
                    output.video_path = external_crop_prefix + ".mp4";
                    output.metadata_path = "Cam" + camera_serial + "_crop_meta.csv";
                    output.keyframe_path = external_crop_prefix + "_keyframe.json";
                    output.perf_path = "Cam" + camera_serial + "_crop_perf.csv";
                    output.sidecar_perf_path =
                        "Cam" + camera_serial + "_crop_sidecar_perf.csv";
                    output.width = resolved_crop_size;
                    output.height = resolved_crop_size;
                    output.frame_rate = cameras_params[i].frame_rate;
                    output.codec = "hevc";
                    output.container = "mp4";
                    output.tuning = "lossless";
                    output.pixel_source_format = "mono8";
                    output.encoded_format = "nv12";
                    output.coordinate_space = "full_frame_pixels";
                    output.details = {
                        {"video_backend", "external_ipc"},
                        {"metadata_backend", "orange_gui"},
                        {"status_reason", "external crop recorder output missing from supervisor plan"},
                        {"recording_control", {
                            {"record_for_seconds", 0},
                            {"clip_seconds", 0}
                        }},
                        {"rollover", {
                            {"requested", false},
                            {"status", "not_requested"},
                            {"implementation", "none"},
                            {"seamless_writer_switch", false},
                            {"records_during_rollover", false},
                            {"output_kind", "crop"},
                            {"supported_mode", "single_clip"},
                            {"rolling_supported", false}
                        }},
                        {"selection_policy", "largest_detection_by_confidence"},
                        {"blank_frame_policy", "encode_black_frame_when_no_detection"}
                    };
                    manifest_options.recording_outputs.push_back(std::move(output));
                } else {
                    manifest_options.recording_outputs.push_back(
                        orange::session::build_crop_recording_output_descriptor(
                            camera_serial,
                            run->recording_folder,
                            true,
                            resolved_crop_size,
                            cameras_params[i].frame_rate,
                            manifest_options.status));
                }
            }
        }

        manifest =
            orange::session::build_single_clip_recording_session_manifest(manifest_options);
        std::string manifest_error;
        if (!orange::session::write_recording_session_manifest(
                manifest_path.string(),
                manifest,
                &manifest_error)) {
            std::cerr << "[GUI][recording] Failed to write recording_session.json: "
                      << manifest_error << std::endl;
            return false;
        }
        recording_session_bridge = {
            {"pass", recording_session_ok},
            {"path", manifest_path.string()},
            {"mode", manifest_mode},
            {"producer", manifest_producer},
            {"camera_count", camera_serials.size()}
        };
    }

    if (external_ipc && recording_session) {
        orange::external_recorder::FinalizationManifestOptions finalization_options;
        finalization_options.experiment_root = run->recording_folder;
        finalization_options.artifact_root =
            recording_session->external_recorder_lifecycle.plan.artifact_root;
        finalization_options.run_id =
            std::filesystem::path(run->recording_folder).filename().string();
        finalization_options.status = external_recorder_ok ? "pass" : "fail";
        finalization_options.started_at_utc = run->recording_started_at_utc;
        finalization_options.finished_at_utc = run->recording_drained_at_utc;
        finalization_options.error = external_recorder_error;
        nlohmann::json finalization =
            orange::external_recorder::BuildExternalRecorderFinalizationManifest(
                finalization_options);
        finalization["recording_session_manifest"] = recording_session_bridge;
        const orange::external_recorder::ArtifactWriteResult finalization_write =
            orange::external_recorder::WriteExternalRecorderFinalizationArtifact(
                recording_session->external_recorder_lifecycle.plan.artifact_root,
                finalization);
        if (!finalization_write.ok) {
            std::cerr << "[GUI][recording] Failed to write external recorder finalization: "
                      << finalization_write.error_message << std::endl;
        }
    }
    if (crop_external_recorder_active && recording_session) {
        orange::external_recorder::FinalizationManifestOptions finalization_options;
        finalization_options.experiment_root = run->recording_folder;
        finalization_options.artifact_root =
            recording_session->external_crop_recorder_lifecycle.plan.artifact_root;
        finalization_options.run_id =
            std::filesystem::path(run->recording_folder).filename().string();
        finalization_options.status = crop_external_recorder_ok ? "pass" : "fail";
        finalization_options.started_at_utc = run->recording_started_at_utc;
        finalization_options.finished_at_utc = run->recording_drained_at_utc;
        finalization_options.error = crop_external_recorder_error;
        nlohmann::json finalization =
            orange::external_recorder::BuildExternalRecorderFinalizationManifest(
                finalization_options);
        finalization["recording_session_manifest"] = {
            {"pass", crop_external_recorder_ok},
            {"path", manifest_path.string()},
            {"mode", manifest_mode},
            {"producer", manifest_producer},
            {"output_kind", "crop"},
            {"crop_mode", "single_clip"},
            {"camera_count", external_crop_outputs.size()}
        };
        const orange::external_recorder::ArtifactWriteResult finalization_write =
            orange::external_recorder::WriteExternalRecorderFinalizationArtifact(
                recording_session->external_crop_recorder_lifecycle.plan.artifact_root,
                finalization);
        if (!finalization_write.ok) {
            std::cerr << "[GUI][recording] Failed to write external crop recorder finalization: "
                      << finalization_write.error_message << std::endl;
        }
    }

    if (!external_rolling_requested) {
        const nlohmann::json snapshot_update = {
            {"recording_mode", "single_clip"},
            {"recording_session_manifest_path", manifest_path.string()},
            {"recording_session_status", recording_session_ok ? "completed" : "incomplete"},
            {"recording_session_camera_count", camera_serials.size()},
            {"gui_display_frame_rate", gui_display_frame_rate}
        };
        if (!update_recording_snapshot_session_artifacts(
                run->recording_folder,
                snapshot_update)) {
            std::cerr << "[GUI][recording] Failed to update recording_snapshot.json session pointers."
                      << std::endl;
            return false;
        }
        if (manifest.contains("recording_outputs") &&
            manifest["recording_outputs"].is_object() &&
            !update_recording_snapshot_recording_outputs(
                run->recording_folder,
                manifest["recording_outputs"])) {
            std::cerr << "[GUI][recording] Failed to update recording_snapshot.json recording outputs."
                      << std::endl;
            return false;
        }
    }

    if (camera_control) {
        camera_control->preserve_recording_session_state = false;
        std::lock_guard<std::mutex> lock(camera_control->recording_folder_mutex);
        if (camera_control->recording_folder == run->recording_folder) {
            camera_control->recording_folder.clear();
        }
        if (camera_control->recording_output_folder == run->recording_folder) {
            camera_control->recording_output_folder.clear();
        }
    }
    std::cout << "[GUI][recording] Wrote recording session manifest: "
              << manifest_path.string() << std::endl;
    if (external_ipc && recording_session) {
        recording_session->active_external_recorder_contract = nlohmann::json::object();
        recording_session->external_recorder_contract_path.clear();
        recording_session->external_recorder_supervisor_plan_path.clear();
        recording_session->external_recorder_last_error = external_recorder_error;
    }
    if (crop_external_recorder_active && recording_session) {
        recording_session->active_external_crop_recorder_contract = nlohmann::json::object();
        recording_session->external_crop_recorder_contract_path.clear();
        recording_session->external_crop_recorder_supervisor_plan_path.clear();
        recording_session->external_crop_recorder_last_error = crop_external_recorder_error;
    }
    run->active = false;
    run->finalizing = false;
    run->finalized = true;
    return true;
}

void gui_mark_stream_started(GuiSessionTimingState* timing)
{
    if (!timing) {
        return;
    }
    const auto now = std::chrono::steady_clock::now();
    timing->stream_running = true;
    timing->stream_started_at = now;
    timing->recording_running = false;
    timing->recording_finalizing = false;
    timing->recording_started_at = {};
    timing->finalizing_started_at = {};
    timing->last_recording_elapsed = std::chrono::seconds{0};
}

void gui_mark_stream_stopped(GuiSessionTimingState* timing)
{
    if (!timing) {
        return;
    }
    *timing = GuiSessionTimingState{};
}

void gui_mark_recording_started(GuiSessionTimingState* timing)
{
    if (!timing) {
        return;
    }
    timing->recording_running = true;
    timing->recording_finalizing = false;
    timing->recording_started_at = std::chrono::steady_clock::now();
    timing->finalizing_started_at = {};
    timing->last_recording_elapsed = std::chrono::seconds{0};
}

void gui_mark_recording_finalizing(GuiSessionTimingState* timing)
{
    if (!timing) {
        return;
    }
    const auto now = std::chrono::steady_clock::now();
    if (timing->recording_running) {
        timing->last_recording_elapsed =
            gui_elapsed_since(timing->recording_started_at, now);
    }
    timing->recording_running = false;
    if (!timing->recording_finalizing) {
        timing->finalizing_started_at = now;
    }
    timing->recording_finalizing = true;
}

void gui_mark_recording_finished(GuiSessionTimingState* timing)
{
    if (!timing) {
        return;
    }
    timing->recording_running = false;
    timing->recording_finalizing = false;
    timing->recording_started_at = {};
    timing->finalizing_started_at = {};
}

void gui_request_recording_stop_through_operator_path(
    orange::session::RecordingSessionState* recording_session,
    CameraControl* camera_control,
    GuiRecordingRunState* recording_run,
    GuiSessionTimingState* timing,
    const std::string& stop_reason,
    nlohmann::json stop_control = nlohmann::json::object())
{
    gui_note_recording_stop_requested(
        recording_run,
        stop_reason,
        std::move(stop_control));
    orange::session::request_drain_recording_run(recording_session, camera_control);
    gui_mark_recording_finalizing(timing);
    try_stop_timer();
    if (camera_control && !camera_control->recording_draining) {
        gui_mark_recording_finished(timing);
    }
}

GuiSessionTimingSnapshot gui_session_timing_snapshot(
    GuiSessionTimingState* timing,
    const CameraControl* camera_control)
{
    GuiSessionTimingSnapshot snapshot;
    if (!timing) {
        return snapshot;
    }

    const auto now = std::chrono::steady_clock::now();
    const bool stream_active = camera_control && camera_control->subscribe;
    const bool recording_active = camera_control && camera_control->record_video;
    const bool recording_draining = camera_control && camera_control->recording_draining;

    if (stream_active && !timing->stream_running) {
        timing->stream_running = true;
        timing->stream_started_at = now;
    } else if (!stream_active && timing->stream_running) {
        gui_mark_stream_stopped(timing);
    }

    if (recording_active && !timing->recording_running) {
        gui_mark_recording_started(timing);
    } else if (!recording_active && recording_draining && !timing->recording_finalizing) {
        gui_mark_recording_finalizing(timing);
    } else if (!recording_active && !recording_draining &&
               (timing->recording_running || timing->recording_finalizing)) {
        gui_mark_recording_finished(timing);
    }

    snapshot.stream_running = timing->stream_running;
    snapshot.recording_running = timing->recording_running;
    snapshot.recording_finalizing = timing->recording_finalizing;
    snapshot.has_recording_elapsed =
        timing->recording_running ||
        timing->recording_finalizing ||
        timing->last_recording_elapsed.count() > 0;

    if (timing->stream_running) {
        snapshot.stream_elapsed =
            format_elapsed_time(gui_elapsed_since(timing->stream_started_at, now));
    }
    if (timing->recording_running) {
        snapshot.recording_elapsed =
            format_elapsed_time(gui_elapsed_since(timing->recording_started_at, now));
    } else if (snapshot.has_recording_elapsed) {
        snapshot.recording_elapsed = format_elapsed_time(timing->last_recording_elapsed);
    }
    if (timing->recording_finalizing) {
        snapshot.finalizing_elapsed =
            format_elapsed_time(gui_elapsed_since(timing->finalizing_started_at, now));
    }
    return snapshot;
}

void render_gui_session_timing_status(
    const GuiSessionTimingSnapshot& timing,
    double streaming_fps_value,
    YoloWorker* yolo_worker)
{
    if (timing.stream_running) {
        ImGui::Text("Stream: %s", timing.stream_elapsed.c_str());
    } else {
        ImGui::TextDisabled("Stream idle");
    }

    ImGui::SameLine();
    if (timing.recording_running) {
        ImGui::TextColored(
            ImVec4{0.0f, 1.0f, 0.0f, 1.0f},
            "Recording: %s",
            timing.recording_elapsed.c_str());
    } else if (timing.recording_finalizing) {
        ImGui::TextColored(
            ImVec4{1.0f, 1.0f, 0.0f, 1.0f},
            "Finalizing: %s",
            timing.finalizing_elapsed.c_str());
        ImGui::TextDisabled("Recorded: %s", timing.recording_elapsed.c_str());
    } else if (timing.has_recording_elapsed) {
        ImGui::TextDisabled("Last recording: %s", timing.recording_elapsed.c_str());
    } else {
        ImGui::TextDisabled("Recording idle");
    }

    ImGui::Text("Streaming FPS: %.1f", streaming_fps_value);
    if (yolo_worker) {
        ImGui::SameLine();
        ImGui::TextColored(
            ImVec4(1.0f, 0.55f, 0.0f, 1.0f),
            "YOLO FPS: %.1f",
            yolo_worker->get_fps());
    }
}

struct GuiExternalRecorderStatusLine {
    bool visible = false;
    std::string label;
    std::string status = "idle";
    std::string error;
    int process_count = 0;
    int active_count = 0;
    int socket_ready_count = 0;
    int error_count = 0;
    int recorder_status_present_count = 0;
    int recorder_status_valid_count = 0;
    uint64_t frames_received = 0;
    uint64_t frames_encoded = 0;
    int storage_checked_count = 0;
    int storage_healthy_count = 0;
    int storage_low_space_count = 0;
    uint64_t storage_path_count = 0;
    uint64_t storage_paths_ok_count = 0;
    bool storage_has_min_available_bytes = false;
    uint64_t storage_min_available_bytes = 0;
    int rolling_process_count = 0;
    int rolling_current_clip_index = -1;
    int rolling_clip_seconds = 0;
    uint64_t rolling_frames_until_next_rollover = 0;
    uint64_t rolling_next_rollover_at_recording_frame_id = 0;
    std::string recorder_status_detail;
    std::string storage_status_detail;
    std::string rolling_status_detail;
    std::string rolling_last_rollover_detail;
};

std::string gui_format_storage_bytes(uint64_t bytes)
{
    static constexpr const char* kUnits[] = {"B", "KiB", "MiB", "GiB", "TiB"};
    double value = static_cast<double>(bytes);
    size_t unit_index = 0;
    constexpr size_t kUnitCount = sizeof(kUnits) / sizeof(kUnits[0]);
    while (value >= 1024.0 && unit_index + 1 < kUnitCount) {
        value /= 1024.0;
        ++unit_index;
    }

    std::ostringstream out;
    if (unit_index == 0 || value >= 100.0) {
        out << std::fixed << std::setprecision(0);
    } else if (value >= 10.0) {
        out << std::fixed << std::setprecision(1);
    } else {
        out << std::fixed << std::setprecision(2);
    }
    out << value << ' ' << kUnits[unit_index];
    return out.str();
}

GuiExternalRecorderStatusLine gui_external_recorder_status_line(
    const char* label,
    const orange::external_recorder::SupervisedRecorderLifecycleState& state,
    const std::string& last_error)
{
    GuiExternalRecorderStatusLine line;
    line.label = label ? label : "External recorder";

    for (const orange::external_recorder::RecorderProcessState& process :
         state.runtime.processes) {
        ++line.process_count;
        if (process.active) {
            ++line.active_count;
        }
        if (process.socket_ready) {
            ++line.socket_ready_count;
        }
        const auto& recorder_status = process.recorder_status;
        if (recorder_status.present) {
            ++line.recorder_status_present_count;
            if (recorder_status.valid) {
                ++line.recorder_status_valid_count;
                line.frames_received += recorder_status.frames_received;
                line.frames_encoded += recorder_status.frames_encoded;
                if (recorder_status.storage_checked) {
                    ++line.storage_checked_count;
                    const bool storage_healthy =
                        recorder_status.storage_ok &&
                        !recorder_status.storage_low_space;
                    if (storage_healthy) {
                        ++line.storage_healthy_count;
                    }
                    if (recorder_status.storage_low_space) {
                        ++line.storage_low_space_count;
                    }
                    line.storage_path_count += recorder_status.storage_path_count;
                    line.storage_paths_ok_count +=
                        recorder_status.storage_paths_ok_count;
                    if (recorder_status.storage_has_min_available_bytes &&
                        (!line.storage_has_min_available_bytes ||
                         recorder_status.storage_min_available_bytes <
                             line.storage_min_available_bytes)) {
                        line.storage_min_available_bytes =
                            recorder_status.storage_min_available_bytes;
                        line.storage_has_min_available_bytes = true;
                    }
                    if (line.storage_status_detail.empty()) {
                        const std::string camera =
                            process.camera_serial.empty()
                                ? process.stream_id
                                : ("Cam" + process.camera_serial);
                        std::ostringstream detail;
                        detail
                            << camera << " storage="
                            << (storage_healthy
                                    ? "ok"
                                    : (recorder_status.storage_low_space
                                           ? "low_space"
                                           : "failed"))
                            << " paths=" << recorder_status.storage_paths_ok_count
                            << "/" << recorder_status.storage_path_count;
                        if (recorder_status.storage_has_min_available_bytes) {
                            detail << " min_avail="
                                   << gui_format_storage_bytes(
                                          recorder_status
                                              .storage_min_available_bytes);
                        }
                        if (recorder_status.storage_min_free_bytes > 0) {
                            detail << " min_required="
                                   << gui_format_storage_bytes(
                                          recorder_status.storage_min_free_bytes);
                        }
                        if (recorder_status.storage_low_space_warning_bytes > 0) {
                            detail << " warn_below="
                                   << gui_format_storage_bytes(
                                          recorder_status
                                              .storage_low_space_warning_bytes);
                        }
                        line.storage_status_detail = detail.str();
                    }
                    if (!storage_healthy) {
                        ++line.error_count;
                        if (line.error.empty()) {
                            const std::string camera =
                                process.camera_serial.empty()
                                    ? process.stream_id
                                    : ("Cam" + process.camera_serial);
                            line.error = camera + ": recorder storage ";
                            line.error += recorder_status.storage_low_space
                                ? "below low-space threshold"
                                : "preflight failed";
                        }
                    }
                }
                if (recorder_status.rolling_enabled) {
                    ++line.rolling_process_count;
                    if (line.rolling_status_detail.empty()) {
                        line.rolling_current_clip_index =
                            recorder_status.rolling_current_clip_index;
                        line.rolling_clip_seconds =
                            recorder_status.rolling_clip_seconds;
                        line.rolling_frames_until_next_rollover =
                            recorder_status.rolling_frames_until_next_rollover;
                        line.rolling_next_rollover_at_recording_frame_id =
                            recorder_status.rolling_next_rollover_at_recording_frame_id;
                        const std::string camera =
                            process.camera_serial.empty()
                                ? process.stream_id
                                : ("Cam" + process.camera_serial);
                        std::ostringstream detail;
                        detail << camera
                               << " clip=" << recorder_status.rolling_current_clip_index
                               << " clip_s=" << recorder_status.rolling_clip_seconds
                               << " next_frame="
                               << recorder_status.rolling_next_rollover_at_recording_frame_id
                               << " frames_left="
                               << recorder_status.rolling_frames_until_next_rollover;
                        line.rolling_status_detail = detail.str();
                        if (recorder_status.rolling_completed_clip_count > 0) {
                            std::ostringstream last_detail;
                            last_detail
                                << camera
                                << " completed_clip="
                                << recorder_status.rolling_last_completed_clip_index
                                << " status="
                                << recorder_status.rolling_last_rollover_status
                                << " last_frame="
                                << recorder_status
                                       .rolling_last_completed_clip_last_recording_frame_id
                                << " frames="
                                << recorder_status.rolling_last_completed_clip_frame_count;
                            line.rolling_last_rollover_detail = last_detail.str();
                        }
                    }
                }
                if (line.recorder_status_detail.empty()) {
                    const std::string camera =
                        process.camera_serial.empty()
                            ? process.stream_id
                            : ("Cam" + process.camera_serial);
                    std::ostringstream detail;
                    detail << camera
                           << " recorder=" << recorder_status.status
                           << " seq=" << recorder_status.heartbeat_sequence
                           << " rx=" << recorder_status.frames_received
                           << " enc=" << recorder_status.frames_encoded;
                    line.recorder_status_detail = detail.str();
                }
                if (recorder_status.status == "failed" ||
                    recorder_status.worker_failed ||
                    !recorder_status.error.empty()) {
                    ++line.error_count;
                    if (line.error.empty()) {
                        const std::string camera =
                            process.camera_serial.empty()
                                ? process.stream_id
                                : ("Cam" + process.camera_serial);
                        line.error =
                            camera + ": recorder status " + recorder_status.status;
                        if (!recorder_status.error.empty()) {
                            line.error += ": " + recorder_status.error;
                        }
                    }
                }
            } else {
                ++line.error_count;
                if (line.error.empty()) {
                    const std::string camera =
                        process.camera_serial.empty()
                            ? process.stream_id
                            : ("Cam" + process.camera_serial);
                    line.error =
                        camera + ": invalid recorder status sidecar";
                    if (!recorder_status.error.empty()) {
                        line.error += ": " + recorder_status.error;
                    }
                }
            }
        }
        if (!process.error.empty()) {
            ++line.error_count;
            if (line.error.empty()) {
                line.error = process.camera_serial.empty()
                    ? process.error
                    : ("Cam" + process.camera_serial + ": " + process.error);
            }
        }
    }

    if (line.error.empty() && !state.last_artifact_error.empty()) {
        line.error = state.last_artifact_error;
        line.error_count = std::max(line.error_count, 1);
    }
    if (line.error.empty() && !state.last_runtime_error.empty()) {
        line.error = state.last_runtime_error;
        line.error_count = std::max(line.error_count, 1);
    }
    if (line.error.empty() && !last_error.empty()) {
        line.error = last_error;
        line.error_count = std::max(line.error_count, 1);
    }

    line.visible = state.started || !line.error.empty();
    if (!line.visible) {
        return line;
    }

    if (!line.error.empty() || line.error_count > 0) {
        line.status = "error";
    } else if (state.started &&
               line.process_count > 0 &&
               line.active_count == line.process_count &&
               line.socket_ready_count == line.process_count) {
        line.status = "running";
    } else if (state.started) {
        line.status = "degraded";
    } else {
        line.status = "stopped";
    }
    return line;
}

void render_gui_external_recorder_status_line(
    const GuiExternalRecorderStatusLine& line)
{
    if (!line.visible) {
        return;
    }

    ImVec4 color{0.7f, 0.7f, 0.7f, 1.0f};
    if (line.status == "running") {
        color = ImVec4{0.25f, 0.85f, 0.35f, 1.0f};
    } else if (line.status == "degraded") {
        color = ImVec4{1.0f, 0.78f, 0.15f, 1.0f};
    } else if (line.status == "error") {
        color = ImVec4{1.0f, 0.25f, 0.20f, 1.0f};
    }

    ImGui::TextColored(
        color,
        "%s: %s (%d/%d running, %d/%d sockets, %d/%d status)",
        line.label.c_str(),
        line.status.c_str(),
        line.active_count,
        line.process_count,
        line.socket_ready_count,
        line.process_count,
        line.recorder_status_valid_count,
        line.process_count);
    if (!line.recorder_status_detail.empty()) {
        ImGui::TextDisabled(
            "%s, total rx=%llu enc=%llu",
            line.recorder_status_detail.c_str(),
            static_cast<unsigned long long>(line.frames_received),
            static_cast<unsigned long long>(line.frames_encoded));
    }
    if (line.storage_checked_count > 0) {
        const bool storage_healthy =
            line.storage_healthy_count == line.storage_checked_count &&
            line.storage_low_space_count == 0;
        ImVec4 storage_color = storage_healthy
            ? ImVec4{0.25f, 0.85f, 0.35f, 1.0f}
            : ImVec4{1.0f, 0.25f, 0.20f, 1.0f};
        const std::string min_available =
            line.storage_has_min_available_bytes
                ? gui_format_storage_bytes(line.storage_min_available_bytes)
                : "unknown";
        ImGui::TextColored(
            storage_color,
            "Storage: %d/%d healthy, low=%d",
            line.storage_healthy_count,
            line.storage_checked_count,
            line.storage_low_space_count);
        ImGui::TextDisabled(
            "Storage paths: %llu/%llu ok, min available=%s",
            static_cast<unsigned long long>(line.storage_paths_ok_count),
            static_cast<unsigned long long>(line.storage_path_count),
            min_available.c_str());
        if (!line.storage_status_detail.empty()) {
            ImGui::TextDisabled("%s", line.storage_status_detail.c_str());
        }
    }
    if (!line.rolling_status_detail.empty()) {
        ImGui::TextDisabled(
            "Rolling: %s (%d/%d streams)",
            line.rolling_status_detail.c_str(),
            line.rolling_process_count,
            line.process_count);
    }
    if (!line.rolling_last_rollover_detail.empty()) {
        ImGui::TextDisabled(
            "Last rollover: %s",
            line.rolling_last_rollover_detail.c_str());
    }
    if (!line.error.empty()) {
        ImGui::TextWrapped("%s", line.error.c_str());
    }
}

void render_gui_external_recorder_status(
    const orange::session::RecordingSessionState& recording_session)
{
    render_gui_external_recorder_status_line(
        gui_external_recorder_status_line(
            "External recorder",
            recording_session.external_recorder_lifecycle,
            recording_session.external_recorder_last_error));
    render_gui_external_recorder_status_line(
        gui_external_recorder_status_line(
            "Crop recorder",
            recording_session.external_crop_recorder_lifecycle,
            recording_session.external_crop_recorder_last_error));
}

void gui_refresh_external_recorder_lifecycle(
    orange::external_recorder::SupervisedRecorderLifecycleState* lifecycle,
    std::string* last_error)
{
    if (!lifecycle || !lifecycle->started) {
        return;
    }

    std::string error;
    if (!orange::external_recorder::RefreshSupervisedRecorderLifecycle(
            lifecycle,
            &error)) {
        if (last_error && last_error->empty()) {
            *last_error = error.empty()
                ? "external recorder supervisor process health check failed"
                : error;
        }
    }
}

void gui_refresh_external_recorder_lifecycles(
    orange::session::RecordingSessionState* recording_session,
    const CameraControl* camera_control)
{
    if (!recording_session || !camera_control) {
        return;
    }
    if (!camera_control->record_video && !camera_control->recording_draining) {
        return;
    }

    gui_refresh_external_recorder_lifecycle(
        &recording_session->external_recorder_lifecycle,
        &recording_session->external_recorder_last_error);
    gui_refresh_external_recorder_lifecycle(
        &recording_session->external_crop_recorder_lifecycle,
        &recording_session->external_crop_recorder_last_error);
}

std::vector<std::string> gui_split_expected_camera_serials()
{
    const char* raw = std::getenv("ORANGE_GUI_EXPECT_CAMERAS");
    if (!raw || !*raw) {
        return {};
    }
    std::vector<std::string> out;
    for (std::string token : string_split(raw, ",")) {
        token = gui_trim_ascii_whitespace(token);
        if (!token.empty()) {
            out.push_back(token);
        }
    }
    return out;
}

std::vector<std::string> gui_camera_serials_for_selection(
    const CameraParams* cameras_params,
    const CameraEachSelect* cameras_select,
    const int num_cameras,
    const char* selection)
{
    std::vector<std::string> serials;
    if (!cameras_params || num_cameras <= 0) {
        return serials;
    }
    serials.reserve(static_cast<std::size_t>(num_cameras));
    for (int i = 0; i < num_cameras; ++i) {
        bool selected = true;
        if (cameras_select) {
            if (std::strcmp(selection, "stream") == 0) {
                selected = cameras_select[i].stream_on;
            } else if (std::strcmp(selection, "record") == 0) {
                selected = cameras_select[i].record;
            } else if (std::strcmp(selection, "yolo") == 0) {
                selected = cameras_select[i].yolo;
            } else if (std::strcmp(selection, "crop") == 0) {
                selected = cameras_select[i].crop_and_encode;
            }
        }
        if (selected && !cameras_params[i].camera_serial.empty()) {
            serials.push_back(cameras_params[i].camera_serial);
        }
    }
    return serials;
}

orange::control::RecorderReadinessSnapshot gui_control_recorder_readiness(
    const orange::external_recorder::SupervisedRecorderLifecycleState& lifecycle,
    const bool external_ipc_enabled)
{
    orange::control::RecorderReadinessSnapshot snapshot;
    snapshot.external_ipc_enabled = external_ipc_enabled;
    snapshot.lifecycle_started = lifecycle.started;
    snapshot.process_count = static_cast<int>(lifecycle.runtime.processes.size());
    for (const auto& process : lifecycle.runtime.processes) {
        if (process.active) {
            ++snapshot.running_process_count;
        }
        if (process.socket_ready) {
            ++snapshot.socket_ready_count;
        }
    }
    snapshot.supervisors_ready =
        !external_ipc_enabled ||
        (lifecycle.started &&
         snapshot.process_count > 0 &&
         snapshot.running_process_count == snapshot.process_count &&
         snapshot.socket_ready_count == snapshot.process_count);
    return snapshot;
}

std::string gui_json_string_or_empty(const nlohmann::json& object, const char* key)
{
    const auto it = object.find(key);
    if (it == object.end() || !it->is_string()) {
        return {};
    }
    return it->get<std::string>();
}

double gui_json_nonnegative_seconds_or_default(
    const nlohmann::json& object,
    const char* key,
    const double default_value)
{
    const auto it = object.find(key);
    if (it == object.end()) {
        return default_value;
    }
    if (!it->is_number()) {
        return default_value;
    }
    const double parsed = it->get<double>();
    if (!std::isfinite(parsed) || parsed < 0.0) {
        return default_value;
    }
    return parsed;
}

orange::control::LocalControlRecordingStopSnapshot gui_control_stop_snapshot(
    const GuiLocalControlStopSchedulerState& scheduler)
{
    orange::control::LocalControlRecordingStopSnapshot snapshot;
    snapshot.enabled = scheduler.stop_recording_enabled;
    snapshot.citrus_completion_enabled = scheduler.citrus_completion_enabled;
    snapshot.scheduled = scheduler.scheduled;
    snapshot.stop_triggered = scheduler.stop_triggered;
    snapshot.grace_seconds = scheduler.grace_seconds;
    snapshot.drain_timed_out = scheduler.drain_timed_out;
    snapshot.forced_finalize_requested = scheduler.forced_finalize_requested;
    snapshot.forced_finalize_stream_stop_requested =
        scheduler.forced_finalize_stream_stop_requested;
    snapshot.drain_timeout_seconds = scheduler.drain_timeout_seconds;
    snapshot.method = scheduler.method;
    snapshot.request_id = scheduler.request_id;
    snapshot.operation_id = scheduler.operation_id;
    snapshot.source = scheduler.source;
    snapshot.experiment_id = scheduler.experiment_id;
    snapshot.terminal_state = scheduler.terminal_state;
    snapshot.reason = scheduler.reason;
    snapshot.received_at_utc = scheduler.received_at_utc;
    snapshot.stop_triggered_at_utc = scheduler.stop_triggered_at_utc;
    snapshot.drain_completed_at_utc = scheduler.drain_completed_at_utc;
    snapshot.forced_finalize_requested_at_utc =
        scheduler.forced_finalize_requested_at_utc;
    snapshot.last_event = scheduler.last_event;
    snapshot.last_event_at_utc = scheduler.last_event_at_utc;
    if (scheduler.scheduled && has_gui_timepoint(scheduler.deadline)) {
        const double remaining =
            std::chrono::duration<double>(
                scheduler.deadline - std::chrono::steady_clock::now())
                .count();
        snapshot.seconds_until_deadline = std::max(0.0, remaining);
    }
    if (scheduler.stop_triggered &&
        !scheduler.drain_completed &&
        has_gui_timepoint(scheduler.stop_triggered_at)) {
        const double elapsed =
            std::chrono::duration<double>(
                std::chrono::steady_clock::now() - scheduler.stop_triggered_at)
                .count();
        snapshot.drain_active = true;
        snapshot.drain_elapsed_seconds = std::max(0.0, elapsed);
    }
    return snapshot;
}

orange::control::LocalControlRecordingStartSnapshot gui_control_start_snapshot(
    const GuiLocalControlStartRequestState& start_request)
{
    orange::control::LocalControlRecordingStartSnapshot snapshot;
    snapshot.enabled = start_request.enabled;
    snapshot.pending = start_request.pending;
    snapshot.request_id = start_request.request_id;
    snapshot.operation_id = start_request.operation_id;
    snapshot.source = start_request.source;
    snapshot.reason = start_request.reason;
    snapshot.received_at_utc = start_request.received_at_utc;
    snapshot.last_event = start_request.last_event;
    snapshot.last_event_at_utc = start_request.last_event_at_utc;
    return snapshot;
}

void gui_note_local_control_stop_event(
    GuiLocalControlStopSchedulerState* scheduler,
    const std::string& event)
{
    if (!scheduler) {
        return;
    }
    scheduler->last_event = event;
    scheduler->last_event_at_utc = get_current_utc_timestamp();
}

nlohmann::json gui_local_control_stop_manifest_control(
    const GuiLocalControlStopSchedulerState& scheduler)
{
    nlohmann::json control = {
        {"source", "orange_gui_local_control"},
        {"method", scheduler.method},
        {"request_id", scheduler.request_id},
        {"operation_id", scheduler.operation_id},
        {"command_source", scheduler.source},
        {"experiment_id", scheduler.experiment_id},
        {"terminal_state", scheduler.terminal_state},
        {"reason", scheduler.reason},
        {"received_at_utc", scheduler.received_at_utc},
        {"grace_seconds", scheduler.grace_seconds},
        {"stop_triggered_at_utc", scheduler.stop_triggered_at_utc},
        {"drain_timeout_seconds", scheduler.drain_timeout_seconds},
        {"drain_completed", false},
        {"drain_timed_out", scheduler.drain_timed_out},
        {"forced_finalize_requested", scheduler.forced_finalize_requested},
        {"forced_finalize_stream_stop_requested",
         scheduler.forced_finalize_stream_stop_requested},
        {"forced_finalize_requested_at_utc",
         scheduler.forced_finalize_requested_at_utc},
        {"ack_state", "executing"},
        {"health", "ok"},
        {"error_code", ""}
    };
    return control;
}

void gui_clear_local_control_stop_schedule(
    GuiLocalControlStopSchedulerState* scheduler)
{
    if (!scheduler) {
        return;
    }
    scheduler->scheduled = false;
    scheduler->deadline = {};
}

void gui_reset_local_control_stop_scheduler_for_recording_start(
    GuiLocalControlStopSchedulerState* scheduler)
{
    if (!scheduler) {
        return;
    }
    const bool enabled = scheduler->enabled;
    const bool stop_recording_enabled = scheduler->stop_recording_enabled;
    const bool citrus_completion_enabled = scheduler->citrus_completion_enabled;
    const double drain_timeout_seconds = scheduler->drain_timeout_seconds;
    *scheduler = GuiLocalControlStopSchedulerState{};
    scheduler->enabled = enabled;
    scheduler->stop_recording_enabled = stop_recording_enabled;
    scheduler->citrus_completion_enabled = citrus_completion_enabled;
    scheduler->drain_timeout_seconds = drain_timeout_seconds;
    gui_note_local_control_stop_event(scheduler, "recording_started");
}

void gui_note_local_control_start_event(
    GuiLocalControlStartRequestState* start_request,
    const std::string& event)
{
    if (!start_request) {
        return;
    }
    start_request->last_event = event;
    start_request->last_event_at_utc = get_current_utc_timestamp();
}

orange::control::LocalControlStatusSnapshot gui_build_local_control_status(
    CameraControl* camera_control,
    const CameraParams* cameras_params,
    const CameraEachSelect* cameras_select,
    const int num_cameras,
    const orange::session::RecordingSessionState& recording_session,
    const GuiRecordingRunState& recording_run,
    const GuiAutorunState& autorun_state,
    const GuiLocalControlStartRequestState& local_control_start_request,
    const GuiLocalControlStopSchedulerState& local_control_stop_scheduler)
{
    orange::control::LocalControlStatusSnapshot snapshot;
    snapshot.updated_at_utc = get_current_utc_timestamp();
    snapshot.autorun_stage = gui_autorun_stage_name(autorun_state.stage);
    if (camera_control) {
        snapshot.cameras_open = camera_control->open;
        snapshot.streaming_active = camera_control->subscribe;
        snapshot.recording_active = camera_control->record_video;
        snapshot.recording_finalizing =
            camera_control->recording_draining || recording_run.finalizing;
        snapshot.active_recorders =
            camera_control->active_recorders.load(std::memory_order_relaxed);
        snapshot.recording_folder =
            orange::session::current_recording_folder(camera_control);
        if (snapshot.recording_folder.empty()) {
            snapshot.recording_folder = recording_run.recording_folder;
        }
    }
    snapshot.recording_finalized = recording_run.finalized;
    snapshot.recording_sink_mode = recording_session.recording_sink_mode;
    snapshot.expected_camera_serials = gui_split_expected_camera_serials();
    snapshot.open_camera_serials =
        snapshot.cameras_open
            ? gui_camera_serials_for_selection(
                  cameras_params,
                  nullptr,
                  num_cameras,
                  "open")
            : std::vector<std::string>{};
    snapshot.stream_selected_camera_serials =
        gui_camera_serials_for_selection(cameras_params, cameras_select, num_cameras, "stream");
    snapshot.record_selected_camera_serials =
        gui_camera_serials_for_selection(cameras_params, cameras_select, num_cameras, "record");
    snapshot.yolo_selected_camera_serials =
        gui_camera_serials_for_selection(cameras_params, cameras_select, num_cameras, "yolo");
    snapshot.crop_selected_camera_serials =
        gui_camera_serials_for_selection(cameras_params, cameras_select, num_cameras, "crop");
    snapshot.full_frame_recorder = gui_control_recorder_readiness(
        recording_session.external_recorder_lifecycle,
        recording_session.recording_sink_mode == "external_ipc");
    snapshot.crop_recorder = gui_control_recorder_readiness(
        recording_session.external_crop_recorder_lifecycle,
        recording_session.crop_recording_sink_mode == "external_ipc");
    snapshot.local_control_recording_start =
        gui_control_start_snapshot(local_control_start_request);
    snapshot.local_control_recording_stop =
        gui_control_stop_snapshot(local_control_stop_scheduler);
    return snapshot;
}

void gui_drain_local_control_commands(
    orange::control::LocalControlServer* local_control_server,
    GuiLocalControlStartRequestState* start_request,
    GuiLocalControlStopSchedulerState* stop_scheduler,
    CameraControl* camera_control,
    const std::string& event_log_path)
{
    if (!local_control_server || !local_control_server->running()) {
        return;
    }
    for (const orange::control::PendingLocalControlCommand& command :
         local_control_server->DrainPendingCommands()) {
        std::cout << "[GUI][local_control] accepted command on GUI thread"
                  << " method=" << command.method
                  << " request_id=" << command.request_id
                  << " operation_id=" << command.operation_id;
        const auto experiment_it = command.params.find("experiment_id");
        if (experiment_it != command.params.end() && experiment_it->is_string()) {
            std::cout << " experiment_id=" << experiment_it->get<std::string>();
        }
        std::cout << " stop_enabled="
                  << (stop_scheduler && stop_scheduler->stop_recording_enabled ? 1 : 0)
                  << " stop_recording_enabled="
                  << (stop_scheduler && stop_scheduler->stop_recording_enabled ? 1 : 0)
                  << " citrus_completion_enabled="
                  << (stop_scheduler && stop_scheduler->citrus_completion_enabled ? 1 : 0)
                  << " start_enabled="
                  << (start_request && start_request->enabled ? 1 : 0)
                  << std::endl;
        nlohmann::json accepted_event = {
            {"event", "gui_command_accepted"},
            {"method", command.method},
            {"request_id", command.request_id},
            {"operation_id", command.operation_id},
            {"command_source", command.source},
            {"received_at_utc", command.received_at_utc},
            {"stop_enabled",
             stop_scheduler && stop_scheduler->stop_recording_enabled},
            {"stop_recording_enabled",
             stop_scheduler && stop_scheduler->stop_recording_enabled},
            {"citrus_completion_enabled",
             stop_scheduler && stop_scheduler->citrus_completion_enabled},
            {"start_enabled", start_request && start_request->enabled},
        };
        const auto experiment_it_for_log = command.params.find("experiment_id");
        if (experiment_it_for_log != command.params.end() &&
            experiment_it_for_log->is_string()) {
            accepted_event["experiment_id"] =
                experiment_it_for_log->get<std::string>();
        }
        gui_log_local_control_event(event_log_path, std::move(accepted_event));

        if (command.method == "start_recording") {
            if (!start_request || !start_request->enabled) {
                if (start_request) {
                    gui_note_local_control_start_event(start_request, "ignored_disabled");
                }
                std::cout << "[GUI][local_control] recording start ignored"
                          << " request_id=" << command.request_id
                          << " reason=start_control_disabled" << std::endl;
                gui_log_local_control_event(
                    event_log_path,
                    {
                        {"event", "recording_start_ignored"},
                        {"method", command.method},
                        {"request_id", command.request_id},
                        {"operation_id", command.operation_id},
                        {"command_source", command.source},
                        {"reason", "start_control_disabled"},
                        {"received_at_utc", command.received_at_utc},
                    });
                continue;
            }
            if (start_request->pending) {
                gui_note_local_control_start_event(start_request, "ignored_pending_start");
                std::cout << "[GUI][local_control] recording start ignored"
                          << " request_id=" << command.request_id
                          << " reason=start_already_pending"
                          << " pending_request_id=" << start_request->request_id
                          << std::endl;
                gui_log_local_control_event(
                    event_log_path,
                    {
                        {"event", "recording_start_ignored"},
                        {"method", command.method},
                        {"request_id", command.request_id},
                        {"operation_id", command.operation_id},
                        {"command_source", command.source},
                        {"reason", "start_already_pending"},
                        {"received_at_utc", command.received_at_utc},
                        {"pending_request_id", start_request->request_id},
                    });
                continue;
            }
            start_request->pending = true;
            start_request->request_id = command.request_id;
            start_request->operation_id = command.operation_id;
            start_request->source = command.source;
            start_request->reason = gui_json_string_or_empty(command.params, "reason");
            if (start_request->reason.empty()) {
                start_request->reason = "local_control_start";
            }
            start_request->received_at_utc = command.received_at_utc;
            gui_note_local_control_start_event(start_request, "queued");
            std::cout << "[GUI][local_control] queued recording start"
                      << " request_id=" << start_request->request_id
                      << " operation_id=" << start_request->operation_id
                      << " reason=" << start_request->reason
                      << std::endl;
            gui_log_local_control_event(
                event_log_path,
                {
                    {"event", "recording_start_queued"},
                    {"method", "start_recording"},
                    {"request_id", start_request->request_id},
                    {"operation_id", start_request->operation_id},
                    {"command_source", start_request->source},
                    {"reason", start_request->reason},
                    {"received_at_utc", start_request->received_at_utc},
                });
            continue;
        }

        const bool stop_command =
            command.method == "citrus_completion" ||
            command.method == "stop_recording";
        if (!stop_scheduler || !stop_command) {
            continue;
        }
        const bool method_stop_enabled =
            command.method == "stop_recording"
                ? stop_scheduler->stop_recording_enabled
                : stop_scheduler->citrus_completion_enabled;
        if (!method_stop_enabled) {
            gui_note_local_control_stop_event(stop_scheduler, "ignored_disabled");
            std::cout << "[GUI][local_control] recording stop ignored"
                      << " method=" << command.method
                      << " request_id=" << command.request_id
                      << " reason=stop_control_disabled" << std::endl;
            gui_log_local_control_event(
                event_log_path,
                {
                    {"event", "recording_stop_ignored"},
                    {"method", command.method},
                    {"request_id", command.request_id},
                    {"operation_id", command.operation_id},
                    {"command_source", command.source},
                    {"reason", "stop_control_disabled"},
                    {"received_at_utc", command.received_at_utc},
                });
            continue;
        }
        if (!camera_control || !camera_control->record_video) {
            gui_note_local_control_stop_event(stop_scheduler, "ignored_not_recording");
            std::cout << "[GUI][local_control] recording stop ignored"
                      << " method=" << command.method
                      << " request_id=" << command.request_id
                      << " reason=orange_not_recording" << std::endl;
            gui_log_local_control_event(
                event_log_path,
                {
                    {"event", "recording_stop_ignored"},
                    {"method", command.method},
                    {"request_id", command.request_id},
                    {"operation_id", command.operation_id},
                    {"command_source", command.source},
                    {"reason", "orange_not_recording"},
                    {"received_at_utc", command.received_at_utc},
                });
            continue;
        }

        const double default_grace_seconds =
            command.method == "stop_recording" ? 0.0 : 10.0;
        const double grace_seconds =
            gui_json_nonnegative_seconds_or_default(
                command.params,
                "grace_seconds",
                default_grace_seconds);
        const auto now = std::chrono::steady_clock::now();
        const auto deadline =
            now +
            std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                std::chrono::duration<double>(grace_seconds));

        const bool keep_existing_deadline =
            stop_scheduler->scheduled &&
            has_gui_timepoint(stop_scheduler->deadline) &&
            stop_scheduler->deadline <= deadline;
        if (keep_existing_deadline) {
            gui_note_local_control_stop_event(
                stop_scheduler,
                "kept_existing_earlier_deadline");
            std::cout << "[GUI][local_control] recording stop schedule kept"
                      << " method=" << command.method
                      << " request_id=" << command.request_id
                      << " existing_request_id=" << stop_scheduler->request_id
                      << " policy=earliest_deadline" << std::endl;
            gui_log_local_control_event(
                event_log_path,
                {
                    {"event", "recording_stop_schedule_kept"},
                    {"method", command.method},
                    {"request_id", command.request_id},
                    {"operation_id", command.operation_id},
                    {"command_source", command.source},
                    {"existing_request_id", stop_scheduler->request_id},
                    {"policy", "earliest_deadline"},
                    {"received_at_utc", command.received_at_utc},
                });
            continue;
        }

        stop_scheduler->scheduled = true;
        stop_scheduler->stop_triggered = false;
        stop_scheduler->drain_completed = false;
        stop_scheduler->drain_timed_out = false;
        stop_scheduler->drain_timeout_reported = false;
        stop_scheduler->forced_finalize_requested = false;
        stop_scheduler->forced_finalize_stream_stop_requested = false;
        stop_scheduler->stop_triggered_at = {};
        stop_scheduler->stop_triggered_at_utc.clear();
        stop_scheduler->drain_completed_at_utc.clear();
        stop_scheduler->forced_finalize_requested_at_utc.clear();
        stop_scheduler->grace_seconds = grace_seconds;
        stop_scheduler->method = command.method;
        stop_scheduler->request_id = command.request_id;
        stop_scheduler->operation_id = command.operation_id;
        stop_scheduler->source = command.source;
        stop_scheduler->experiment_id =
            gui_json_string_or_empty(command.params, "experiment_id");
        stop_scheduler->terminal_state =
            gui_json_string_or_empty(command.params, "terminal_state");
        stop_scheduler->reason =
            gui_json_string_or_empty(command.params, "reason");
        if (stop_scheduler->reason.empty()) {
            stop_scheduler->reason =
                command.method == "stop_recording"
                    ? "local_control_stop"
                    : "citrus_completion";
        }
        stop_scheduler->received_at_utc = command.received_at_utc;
        stop_scheduler->deadline = deadline;
        gui_note_local_control_stop_event(
            stop_scheduler,
            keep_existing_deadline ? "kept_existing_earlier_deadline" : "scheduled");
        std::cout << "[GUI][local_control] scheduled recording stop"
                  << " method=" << stop_scheduler->method
                  << " request_id=" << stop_scheduler->request_id
                  << " operation_id=" << stop_scheduler->operation_id
                  << " experiment_id=" << stop_scheduler->experiment_id
                  << " grace_seconds=" << stop_scheduler->grace_seconds
                  << std::endl;
        gui_log_local_control_event(
            event_log_path,
            {
                {"event", "recording_stop_scheduled"},
                {"method", stop_scheduler->method},
                {"request_id", stop_scheduler->request_id},
                {"operation_id", stop_scheduler->operation_id},
                {"command_source", stop_scheduler->source},
                {"experiment_id", stop_scheduler->experiment_id},
                {"terminal_state", stop_scheduler->terminal_state},
                {"reason", stop_scheduler->reason},
                {"received_at_utc", stop_scheduler->received_at_utc},
                {"grace_seconds", stop_scheduler->grace_seconds},
            });
    }
}

void gui_poll_local_control_stop_scheduler(
    GuiLocalControlStopSchedulerState* stop_scheduler,
    orange::session::RecordingSessionState* recording_session,
    CameraControl* camera_control,
    GuiRecordingRunState* recording_run,
    GuiSessionTimingState* timing,
    const std::string& event_log_path)
{
    if (!stop_scheduler || !stop_scheduler->scheduled) {
        return;
    }
    if (!camera_control || !camera_control->record_video) {
        gui_clear_local_control_stop_schedule(stop_scheduler);
        gui_note_local_control_stop_event(
            stop_scheduler,
            "cancelled_recording_inactive");
        return;
    }
    if (std::chrono::steady_clock::now() < stop_scheduler->deadline) {
        return;
    }

    gui_clear_local_control_stop_schedule(stop_scheduler);
    stop_scheduler->stop_triggered = true;
    stop_scheduler->drain_completed = false;
    stop_scheduler->drain_timed_out = false;
    stop_scheduler->drain_timeout_reported = false;
    stop_scheduler->forced_finalize_requested = false;
    stop_scheduler->forced_finalize_stream_stop_requested = false;
    stop_scheduler->stop_triggered_at = std::chrono::steady_clock::now();
    stop_scheduler->stop_triggered_at_utc = get_current_utc_timestamp();
    stop_scheduler->drain_completed_at_utc.clear();
    stop_scheduler->forced_finalize_requested_at_utc.clear();
    gui_note_local_control_stop_event(stop_scheduler, "stop_triggered");
    const std::string stop_reason =
        stop_scheduler->method == "stop_recording"
            ? "local_control_stop"
            : "citrus_completion";
    gui_request_recording_stop_through_operator_path(
        recording_session,
        camera_control,
        recording_run,
        timing,
        stop_reason,
        gui_local_control_stop_manifest_control(*stop_scheduler));
    std::cout << "[GUI][local_control] triggered recording stop"
              << " method=" << stop_scheduler->method
              << " request_id=" << stop_scheduler->request_id
              << " operation_id=" << stop_scheduler->operation_id
              << " experiment_id=" << stop_scheduler->experiment_id
              << " drain_timeout_seconds="
              << stop_scheduler->drain_timeout_seconds
              << std::endl;
    gui_log_local_control_event(
        event_log_path,
        {
            {"event", "recording_stop_triggered"},
            {"method", stop_scheduler->method},
            {"request_id", stop_scheduler->request_id},
            {"operation_id", stop_scheduler->operation_id},
            {"command_source", stop_scheduler->source},
            {"experiment_id", stop_scheduler->experiment_id},
            {"terminal_state", stop_scheduler->terminal_state},
            {"reason", stop_scheduler->reason},
            {"received_at_utc", stop_scheduler->received_at_utc},
            {"grace_seconds", stop_scheduler->grace_seconds},
            {"stop_triggered_at_utc", stop_scheduler->stop_triggered_at_utc},
            {"drain_timeout_seconds", stop_scheduler->drain_timeout_seconds},
        });
}

void gui_poll_local_control_drain_timeout(
    GuiLocalControlStopSchedulerState* stop_scheduler,
    const CameraControl* camera_control,
    GuiRecordingRunState* recording_run,
    const std::string& event_log_path)
{
    if (!stop_scheduler ||
        !stop_scheduler->stop_triggered ||
        stop_scheduler->drain_completed ||
        stop_scheduler->drain_timeout_seconds <= 0.0 ||
        !has_gui_timepoint(stop_scheduler->stop_triggered_at)) {
        return;
    }
    const bool drain_active =
        (camera_control &&
         (camera_control->recording_draining ||
          camera_control->active_recorders.load(std::memory_order_relaxed) > 0)) ||
        (recording_run && recording_run->finalizing && !recording_run->finalized);
    if (!drain_active) {
        return;
    }

    const double elapsed =
        std::chrono::duration<double>(
            std::chrono::steady_clock::now() - stop_scheduler->stop_triggered_at)
            .count();
    if (elapsed < stop_scheduler->drain_timeout_seconds) {
        return;
    }

    stop_scheduler->drain_timed_out = true;
    if (!stop_scheduler->forced_finalize_requested) {
        stop_scheduler->forced_finalize_requested = true;
        stop_scheduler->forced_finalize_requested_at_utc =
            get_current_utc_timestamp();
        if (recording_run && recording_run->stop_control.is_object()) {
            recording_run->stop_control["drain_timed_out"] = true;
            recording_run->stop_control["forced_finalize_requested"] = true;
            recording_run->stop_control["forced_finalize_stream_stop_requested"] =
                stop_scheduler->forced_finalize_stream_stop_requested;
            recording_run->stop_control["forced_finalize_requested_at_utc"] =
                stop_scheduler->forced_finalize_requested_at_utc;
            recording_run->stop_control["ack_state"] = "failed_timeout";
            recording_run->stop_control["health"] = "critical";
            recording_run->stop_control["error_code"] = "drain_timeout";
        }
    }
    if (stop_scheduler->drain_timeout_reported) {
        return;
    }
    stop_scheduler->drain_timeout_reported = true;
    gui_note_local_control_stop_event(stop_scheduler, "drain_timeout");
    if (recording_run && recording_run->stop_control.is_object()) {
        recording_run->stop_control["last_event"] = "drain_timeout";
        recording_run->stop_control["last_event_at_utc"] =
            stop_scheduler->last_event_at_utc;
    }
    const int active_recorders =
        camera_control
            ? camera_control->active_recorders.load(std::memory_order_relaxed)
            : -1;
    std::cerr << "[GUI][local_control] recording drain timeout"
              << " method=" << stop_scheduler->method
              << " request_id=" << stop_scheduler->request_id
              << " operation_id=" << stop_scheduler->operation_id
              << " elapsed_seconds=" << elapsed
              << " timeout_seconds=" << stop_scheduler->drain_timeout_seconds
              << " active_recorders=" << active_recorders
              << std::endl;
    gui_log_local_control_event(
        event_log_path,
        {
            {"event", "recording_drain_timeout"},
            {"method", stop_scheduler->method},
            {"request_id", stop_scheduler->request_id},
            {"operation_id", stop_scheduler->operation_id},
            {"command_source", stop_scheduler->source},
            {"experiment_id", stop_scheduler->experiment_id},
            {"terminal_state", stop_scheduler->terminal_state},
            {"reason", stop_scheduler->reason},
            {"received_at_utc", stop_scheduler->received_at_utc},
            {"elapsed_seconds", elapsed},
            {"timeout_seconds", stop_scheduler->drain_timeout_seconds},
            {"active_recorders", active_recorders},
            {"forced_finalize_requested", stop_scheduler->forced_finalize_requested},
            {"forced_finalize_requested_at_utc",
             stop_scheduler->forced_finalize_requested_at_utc},
            {"health", "critical"},
            {"error_code", "drain_timeout"},
        });
}

void gui_request_local_control_forced_finalize_if_needed(
    GuiLocalControlStopSchedulerState* stop_scheduler,
    CameraControl* camera_control,
    GuiAutorunRequests* gui_autorun_requests,
    GuiRecordingRunState* recording_run,
    const std::string& event_log_path)
{
    if (!stop_scheduler ||
        !camera_control ||
        !gui_autorun_requests ||
        !stop_scheduler->forced_finalize_requested ||
        stop_scheduler->forced_finalize_stream_stop_requested ||
        stop_scheduler->drain_completed ||
        !camera_control->subscribe) {
        return;
    }

    gui_autorun_requests->toggle_streaming = true;
    stop_scheduler->forced_finalize_stream_stop_requested = true;
    gui_note_local_control_stop_event(
        stop_scheduler,
        "forced_finalize_stream_stop_requested");
    if (recording_run && recording_run->stop_control.is_object()) {
        recording_run->stop_control["forced_finalize_stream_stop_requested"] = true;
        recording_run->stop_control["last_event"] =
            "forced_finalize_stream_stop_requested";
        recording_run->stop_control["last_event_at_utc"] =
            stop_scheduler->last_event_at_utc;
    }
    std::cerr << "[GUI][local_control] requesting stream shutdown for forced finalize"
              << " method=" << stop_scheduler->method
              << " request_id=" << stop_scheduler->request_id
              << " operation_id=" << stop_scheduler->operation_id
              << std::endl;
    gui_log_local_control_event(
        event_log_path,
        {
            {"event", "recording_drain_forced_finalize_requested"},
            {"method", stop_scheduler->method},
            {"request_id", stop_scheduler->request_id},
            {"operation_id", stop_scheduler->operation_id},
            {"command_source", stop_scheduler->source},
            {"experiment_id", stop_scheduler->experiment_id},
            {"terminal_state", stop_scheduler->terminal_state},
            {"reason", stop_scheduler->reason},
            {"received_at_utc", stop_scheduler->received_at_utc},
            {"forced_finalize_requested_at_utc",
             stop_scheduler->forced_finalize_requested_at_utc},
            {"action", "stream_shutdown"},
            {"health", "critical"},
            {"error_code", "drain_timeout"},
        });
}

void gui_mark_local_control_drain_completed(
    GuiLocalControlStopSchedulerState* stop_scheduler,
    const std::string& event_log_path,
    const std::string& recording_folder)
{
    if (!stop_scheduler || !stop_scheduler->stop_triggered) {
        return;
    }
    if (!stop_scheduler->drain_completed) {
        stop_scheduler->drain_completed = true;
        stop_scheduler->drain_completed_at_utc = get_current_utc_timestamp();
        gui_note_local_control_stop_event(
            stop_scheduler,
            stop_scheduler->drain_timed_out
                ? "finalized_after_drain_timeout"
                : "finalized");
        std::cout << "[GUI][local_control] recording drain finalized"
                  << " method=" << stop_scheduler->method
                  << " request_id=" << stop_scheduler->request_id
                  << " operation_id=" << stop_scheduler->operation_id
                  << " drain_timed_out="
                  << (stop_scheduler->drain_timed_out ? 1 : 0)
                  << std::endl;
        gui_log_local_control_event(
            event_log_path,
            {
                {"event", "recording_drain_finalized"},
                {"method", stop_scheduler->method},
                {"request_id", stop_scheduler->request_id},
                {"operation_id", stop_scheduler->operation_id},
                {"command_source", stop_scheduler->source},
                {"experiment_id", stop_scheduler->experiment_id},
                {"terminal_state", stop_scheduler->terminal_state},
                {"reason", stop_scheduler->reason},
                {"received_at_utc", stop_scheduler->received_at_utc},
                {"drain_timed_out", stop_scheduler->drain_timed_out},
                {"drain_completed_at_utc", stop_scheduler->drain_completed_at_utc},
                {"health", stop_scheduler->drain_timed_out ? "warning" : "ok"},
                {"error_code", stop_scheduler->drain_timed_out ? "drain_timeout" : ""},
            });
        gui_copy_local_control_event_log_to_recording_session(
            event_log_path,
            recording_folder);
    }
}

struct ApertureCharacterizationUiState {
    bool show_window = false;
    int selected_camera = 0;
    int configured_camera_index = -1;
    bool use_explicit_iris_values = false;
    char iris_values_csv[256] = "";
    int iris_start = 0;
    int iris_stop = 0;
    int iris_step_multiple = 1;
    int frames_per_step = 3;
    int settle_frames = 30;
    int buffer_count = 4;
    int grab_timeout_ms = 1000;
    int grid_rows = 8;
    int grid_cols = 8;
    bool restore_original_iris = true;
    bool save_representative_frames = true;
    bool use_reference_iris = false;
    int reference_iris = 0;
    bool use_reference_f_number = false;
    float reference_f_number = 2.8f;
    bool enable_fov_calibration = false;
    float working_distance_mm = 700.0f;
    float pixel_pitch_um = 2.74f;
    float field_width_mm = 0.0f;
    float field_height_mm = 0.0f;
    int alignment_preview_fps = 20;
    float saturated_white_fraction = 0.001f;
    float saturated_p99_min = 254.0f;
    float dim_mean_max = 10.0f;
    float dim_p95_max = 20.0f;
    float dim_black_fraction_min = 0.80f;
    char output_dir[512] = "";
    char output_prefix[128] = "aperture_characterization";
    std::thread worker;
    std::atomic<bool> running{false};
    std::atomic<int> progress_completed_steps{0};
    std::atomic<int> progress_total_steps{0};
    std::atomic<int> progress_iris{0};
    std::mutex mutex;
    std::string status_message = "Idle";
    std::string error_message;
    std::string output_artifact_id;
    std::string output_artifact_dir;
    std::string output_manifest_path;
    std::string output_fingerprint;
    std::string output_json_path;
    std::string output_steps_csv_path;
    std::string output_frames_csv_path;
    std::string output_frame_image_dir;
    GLuint preview_texture = 0;
    int preview_texture_width = 0;
    int preview_texture_height = 0;
    std::string preview_texture_path;
    std::string preview_texture_error;
    orange::ui::ImageCanvasViewState representative_canvas_view;
    std::thread alignment_worker;
    std::atomic<bool> alignment_running{false};
    std::atomic<bool> alignment_stop_requested{false};
    std::atomic<int> alignment_orientation{static_cast<int>(RulerAlignmentOrientation::kHorizontal)};
    std::mutex alignment_mutex;
    LiveFovPreviewState live_fov_preview;
    GLuint alignment_texture = 0;
    int alignment_texture_width = 0;
    int alignment_texture_height = 0;
    uint64_t alignment_uploaded_serial = 0;
    FovCaptureSnapshot horizontal_capture;
    FovCaptureSnapshot vertical_capture;
    bool has_result = false;
    int selected_heatmap_step = 0;
    ApertureCharacterizationResult last_result;
    FovCalibrationData last_fov_calibration;
    std::string last_camera_serial;
    unsigned int last_focus = 0;
    unsigned int last_exposure = 0;
};

template <size_t N>
void copy_string_to_buffer(char (&buffer)[N], const std::string& value)
{
    std::snprintf(buffer, N, "%s", value.c_str());
}

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

std::vector<RecordingValidationCameraInput> build_gui_recording_validation_inputs(
    const CameraParams* cameras_params,
    const CameraEachSelect* cameras_select,
    const int num_cameras)
{
    std::vector<RecordingValidationCameraInput> inputs;
    if (!cameras_params || !cameras_select || num_cameras <= 0) {
        return inputs;
    }

    inputs.reserve(static_cast<std::size_t>(num_cameras));
    for (int i = 0; i < num_cameras; ++i) {
        RecordingValidationCameraInput input;
        input.camera_index = i;
        input.camera_serial = cameras_params[i].camera_serial;
        input.record_enabled = cameras_select[i].record;
        input.source_gpu_id = cameras_params[i].gpu_id;
        input.strategy = cameras_params[i].recording.strategy;
        input.constraints = cameras_params[i].recording.constraints;
        inputs.push_back(std::move(input));
    }

    return inputs;
}

RecordingPreflightResult run_gui_recording_preflight(const CameraParams* cameras_params,
                                                     const CameraEachSelect* cameras_select,
                                                     const int num_cameras,
                                                     const std::string& selected_yolo_model,
                                                     const int crop_size_px)
{
    RecordingPreflightResult result = run_recording_preflight(
        build_gui_recording_validation_inputs(cameras_params, cameras_select, num_cameras),
        [](const int source_gpu_id, const int helper_gpu_id) {
            return build_recording_validation_gpu_path_info(source_gpu_id, helper_gpu_id);
        });
    const int resolved_crop_size = CropAndEncodeWorker::SanitizeCropSize(crop_size_px);

    if (!cameras_params || !cameras_select || num_cameras <= 0) {
        return result;
    }

    for (int i = 0; i < num_cameras; ++i) {
        const std::string serial = cameras_params[i].camera_serial.empty()
            ? ("camera_index_" + std::to_string(i))
            : cameras_params[i].camera_serial;

        auto append_camera_error = [&](const std::string& message) {
            result.errors.push_back(serial + ": " + message);
            result.ok = false;
        };

        if (cameras_select[i].yolo) {
            std::string engine_path = selected_yolo_model;
            if (cameras_select[i].yolo_model && cameras_select[i].yolo_model[0] != '\0') {
                engine_path = cameras_select[i].yolo_model;
            }

            if (engine_path.empty()) {
                append_camera_error("YOLO enabled but no detect engine is configured or selected.");
            } else if (!std::filesystem::exists(engine_path)) {
                append_camera_error("YOLO detect engine does not exist: " + engine_path);
            }
        }

        if (!cameras_select[i].crop_and_encode) {
            if (cameras_select[i].pose) {
                append_camera_error("Pose currently requires Crop+Encode enabled.");
            }
            continue;
        }

        if (!cameras_select[i].record) {
            append_camera_error("Crop+Encode currently requires full-frame Record enabled.");
        }
        if (!cameras_select[i].yolo) {
            append_camera_error("Crop+Encode requires YOLO enabled.");
        }
        if (cameras_select[i].pose && !cameras_select[i].yolo) {
            append_camera_error("Pose currently requires YOLO enabled.");
        }
        if (cameras_params[i].width < resolved_crop_size ||
            cameras_params[i].height < resolved_crop_size) {
            std::ostringstream error;
            error << "Crop+Encode requires source frames at least "
                  << resolved_crop_size << "x"
                  << resolved_crop_size
                  << "; configured frame is "
                  << cameras_params[i].width << "x" << cameras_params[i].height
                  << ".";
            append_camera_error(error.str());
        }
    }

    result.ok = result.errors.empty();
    return result;
}

bool gui_camera_has_acquisition_work(const CameraEachSelect& camera_select)
{
    return camera_select.stream_on ||
           camera_select.record ||
           camera_select.yolo ||
           camera_select.crop_and_encode ||
           camera_select.pose ||
           camera_select.frame_save_state == State_Write_New_Frame;
}

int resolve_gui_crop_size_from_camera_configs(const CameraParams* cameras_params,
                                              const int num_cameras,
                                              const int fallback_crop_size,
                                              bool* mixed_values_out)
{
    if (mixed_values_out) {
        *mixed_values_out = false;
    }

    if (!cameras_params || num_cameras <= 0) {
        return CropAndEncodeWorker::SanitizeCropSize(fallback_crop_size);
    }

    const int resolved_crop_size =
        CropAndEncodeWorker::SanitizeCropSize(cameras_params[0].crop_pipeline.crop_size_px);
    bool mixed_values = false;
    for (int i = 1; i < num_cameras; ++i) {
        const int camera_crop_size =
            CropAndEncodeWorker::SanitizeCropSize(cameras_params[i].crop_pipeline.crop_size_px);
        if (camera_crop_size != resolved_crop_size) {
            mixed_values = true;
            break;
        }
    }

    if (mixed_values_out) {
        *mixed_values_out = mixed_values;
    }
    return resolved_crop_size;
}

int resolve_gui_crop_preview_max_fps_from_camera_configs(const CameraParams* cameras_params,
                                                         const int num_cameras,
                                                         const int fallback_preview_max_fps,
                                                         bool* mixed_values_out)
{
    if (mixed_values_out) {
        *mixed_values_out = false;
    }

    if (!cameras_params || num_cameras <= 0) {
        return sanitize_camera_crop_preview_max_fps(fallback_preview_max_fps);
    }

    const int resolved_preview_max_fps =
        sanitize_camera_crop_preview_max_fps(cameras_params[0].crop_pipeline.preview_max_fps);
    bool mixed_values = false;
    for (int i = 1; i < num_cameras; ++i) {
        const int camera_preview_max_fps =
            sanitize_camera_crop_preview_max_fps(cameras_params[i].crop_pipeline.preview_max_fps);
        if (camera_preview_max_fps != resolved_preview_max_fps) {
            mixed_values = true;
            break;
        }
    }

    if (mixed_values_out) {
        *mixed_values_out = mixed_values;
    }
    return resolved_preview_max_fps;
}

void apply_gui_crop_size_to_camera_configs(CameraParams* cameras_params,
                                           const int num_cameras,
                                           const int crop_size_px)
{
    if (!cameras_params || num_cameras <= 0) {
        return;
    }

    const int resolved_crop_size = CropAndEncodeWorker::SanitizeCropSize(crop_size_px);
    for (int i = 0; i < num_cameras; ++i) {
        cameras_params[i].crop_pipeline.crop_size_px = resolved_crop_size;
    }
}

void apply_gui_crop_preview_max_fps_to_camera_configs(CameraParams* cameras_params,
                                                      const int num_cameras,
                                                      const int preview_max_fps)
{
    if (!cameras_params || num_cameras <= 0) {
        return;
    }

    const int resolved_preview_max_fps =
        sanitize_camera_crop_preview_max_fps(preview_max_fps);
    for (int i = 0; i < num_cameras; ++i) {
        cameras_params[i].crop_pipeline.preview_max_fps = resolved_preview_max_fps;
    }
}

int resolve_effective_crop_preview_max_fps(const CameraParams& camera_params)
{
    int resolved = camera_params.crop_pipeline.preview_max_fps;
    const char* env_value = std::getenv("ORANGE_CROP_PREVIEW_MAX_FPS");
    if (env_value && *env_value) {
        char* end = nullptr;
        const long parsed = std::strtol(env_value, &end, 10);
        if (end != env_value && end && *end == '\0') {
            if (parsed > std::numeric_limits<int>::max()) {
                resolved = std::numeric_limits<int>::max();
            } else if (parsed < std::numeric_limits<int>::min()) {
                resolved = std::numeric_limits<int>::min();
            } else {
                resolved = static_cast<int>(parsed);
            }
        }
    }
    return sanitize_camera_crop_preview_max_fps(resolved);
}

YoloWorker* gui_yolo_worker_at(const int camera_index)
{
    if (camera_index < 0 || camera_index >= static_cast<int>(yolo_workers.size())) {
        return nullptr;
    }
    return yolo_workers[static_cast<std::size_t>(camera_index)];
}

void log_recording_preflight_failure(const char* context,
                                     const RecordingPreflightResult& preflight)
{
    if (preflight.ok || preflight.errors.empty()) {
        return;
    }

    std::cerr << "[recording_preflight] " << context << " blocked" << std::endl;
    for (const std::string& error : preflight.errors) {
        std::cerr << "  - " << error << std::endl;
    }
}

bool parse_uint_csv_text(const char* text, std::vector<unsigned int>* out_values, std::string* error_out)
{
    std::stringstream ss(text == nullptr ? "" : text);
    std::string item;
    while (std::getline(ss, item, ',')) {
        std::string trimmed;
        for (char c : item) {
            if (!std::isspace(static_cast<unsigned char>(c))) {
                trimmed.push_back(c);
            }
        }
        if (trimmed.empty()) {
            continue;
        }
        try {
            size_t consumed = 0;
            unsigned long value = std::stoul(trimmed, &consumed, 10);
            if (consumed != trimmed.size() || value > std::numeric_limits<unsigned int>::max()) {
                if (error_out) {
                    *error_out = "Invalid iris value in CSV: " + trimmed;
                }
                return false;
            }
            out_values->push_back(static_cast<unsigned int>(value));
        } catch (...) {
            if (error_out) {
                *error_out = "Invalid iris value in CSV: " + trimmed;
            }
            return false;
        }
    }

    if (out_values->empty()) {
        if (error_out) {
            *error_out = "Explicit iris list is empty.";
        }
        return false;
    }
    return true;
}

const char* ruler_alignment_orientation_label(RulerAlignmentOrientation orientation)
{
    switch (orientation) {
        case RulerAlignmentOrientation::kVertical:
            return "vertical";
        case RulerAlignmentOrientation::kHorizontal:
        default:
            return "horizontal";
    }
}

RulerAlignmentMetrics detect_ruler_alignment(
    const cv::Mat& gray,
    RulerAlignmentOrientation orientation)
{
    RulerAlignmentMetrics best;
    if (gray.empty()) {
        return best;
    }

    auto detect_boundary_anchor = [&](const cv::Mat& source_gray) -> int {
        cv::Mat directionally_smoothed;
        if (orientation == RulerAlignmentOrientation::kHorizontal) {
            cv::GaussianBlur(source_gray, directionally_smoothed, cv::Size(61, 7), 0.0, 0.0, cv::BORDER_REPLICATE);
        } else {
            cv::GaussianBlur(source_gray, directionally_smoothed, cv::Size(7, 61), 0.0, 0.0, cv::BORDER_REPLICATE);
        }

        cv::Mat gradient;
        if (orientation == RulerAlignmentOrientation::kHorizontal) {
            cv::Sobel(directionally_smoothed, gradient, CV_32F, 0, 1, 3);
        } else {
            cv::Sobel(directionally_smoothed, gradient, CV_32F, 1, 0, 3);
        }
        cv::Mat abs_gradient = cv::abs(gradient);

        cv::Mat profile;
        if (orientation == RulerAlignmentOrientation::kHorizontal) {
            cv::reduce(abs_gradient, profile, 1, cv::REDUCE_AVG, CV_32F);
            cv::GaussianBlur(profile, profile, cv::Size(1, 31), 0.0, 0.0, cv::BORDER_REPLICATE);
        } else {
            cv::reduce(abs_gradient, profile, 0, cv::REDUCE_AVG, CV_32F);
            cv::GaussianBlur(profile, profile, cv::Size(31, 1), 0.0, 0.0, cv::BORDER_REPLICATE);
        }

        const int profile_length =
            orientation == RulerAlignmentOrientation::kHorizontal ? profile.rows : profile.cols;
        if (profile_length <= 0) {
            return -1;
        }

        const int margin = std::max(4, profile_length / 40);
        int best_index = -1;
        double best_score = -1.0;
        for (int i = margin; i < profile_length - margin; ++i) {
            const float response =
                orientation == RulerAlignmentOrientation::kHorizontal ? profile.at<float>(i, 0) : profile.at<float>(0, i);
            const double boundary_preference =
                1.0 - std::clamp(static_cast<double>(i) / std::max(1.0, static_cast<double>(profile_length - 1)), 0.0, 1.0);
            const double score = static_cast<double>(response) * (0.35 + 0.65 * boundary_preference);
            if (score > best_score) {
                best_score = score;
                best_index = i;
            }
        }
        return best_index;
    };

    const int boundary_anchor = detect_boundary_anchor(gray);

    cv::Mat blurred;
    cv::GaussianBlur(gray, blurred, cv::Size(5, 5), 0.0);
    cv::Mat edges;
    cv::Canny(blurred, edges, 60.0, 180.0, 3);

    cv::Rect search_roi(0, 0, gray.cols, gray.rows);
    if (boundary_anchor >= 0) {
        const int search_half_band = std::max(16, static_cast<int>(std::round(
            0.04 * static_cast<double>(
                orientation == RulerAlignmentOrientation::kHorizontal ? gray.rows : gray.cols))));
        if (orientation == RulerAlignmentOrientation::kHorizontal) {
            const int y0 = std::max(0, boundary_anchor - search_half_band);
            const int y1 = std::min(gray.rows, boundary_anchor + search_half_band + 1);
            search_roi = cv::Rect(0, y0, gray.cols, std::max(1, y1 - y0));
        } else {
            const int x0 = std::max(0, boundary_anchor - search_half_band);
            const int x1 = std::min(gray.cols, boundary_anchor + search_half_band + 1);
            search_roi = cv::Rect(x0, 0, std::max(1, x1 - x0), gray.rows);
        }
    }

    std::vector<cv::Vec4i> lines;
    cv::HoughLinesP(
        edges(search_roi),
        lines,
        1.0,
        CV_PI / 180.0,
        80,
        std::max(search_roi.width, search_roi.height) / 5.0,
        20.0);
    const double center_x = 0.5 * static_cast<double>(gray.cols);
    const double center_y = 0.5 * static_cast<double>(gray.rows);
    double best_score = -1.0;

    for (const cv::Vec4i& local_line : lines) {
        const cv::Vec4i line(
            local_line[0] + search_roi.x,
            local_line[1] + search_roi.y,
            local_line[2] + search_roi.x,
            local_line[3] + search_roi.y);
        const double dx = static_cast<double>(line[2] - line[0]);
        const double dy = static_cast<double>(line[3] - line[1]);
        const double length = std::hypot(dx, dy);
        if (length < std::max(gray.cols, gray.rows) * 0.15) {
            continue;
        }

        double angle = std::atan2(dy, dx) * 180.0 / CV_PI;
        while (angle > 90.0) angle -= 180.0;
        while (angle <= -90.0) angle += 180.0;

        double angle_error = 0.0;
        double center_offset_px = 0.0;
        double boundary_preference = 0.0;
        double anchor_distance_px = 0.0;
        if (orientation == RulerAlignmentOrientation::kHorizontal) {
            angle_error = std::abs(angle);
            const double line_center_y = (static_cast<double>(line[1]) + static_cast<double>(line[3])) * 0.5;
            center_offset_px = line_center_y - center_y;
            boundary_preference = 1.0 - std::clamp(line_center_y / std::max(1.0, static_cast<double>(gray.rows - 1)), 0.0, 1.0);
            anchor_distance_px = boundary_anchor >= 0 ? std::abs(line_center_y - static_cast<double>(boundary_anchor)) : 0.0;
        } else {
            angle_error = std::abs(90.0 - std::abs(angle));
            const double line_center_x = (static_cast<double>(line[0]) + static_cast<double>(line[2])) * 0.5;
            center_offset_px = line_center_x - center_x;
            boundary_preference = 1.0 - std::clamp(line_center_x / std::max(1.0, static_cast<double>(gray.cols - 1)), 0.0, 1.0);
            anchor_distance_px = boundary_anchor >= 0 ? std::abs(line_center_x - static_cast<double>(boundary_anchor)) : 0.0;
        }
        if (angle_error > 25.0) {
            continue;
        }

        const double center_extent =
            orientation == RulerAlignmentOrientation::kHorizontal ? center_y : center_x;
        const double angle_score = 1.0 - std::min(angle_error / 25.0, 1.0);
        const double anchor_score =
            boundary_anchor >= 0
                ? 1.0 - std::min(anchor_distance_px /
                                     std::max(1.0, 0.5 * static_cast<double>(
                                                       orientation == RulerAlignmentOrientation::kHorizontal
                                                           ? search_roi.height
                                                           : search_roi.width)),
                                 1.0)
                : 1.0;
        const double score =
            length * (0.15 + 0.85 * angle_score) * (0.25 + 0.75 * boundary_preference) * (0.20 + 0.80 * anchor_score);
        if (score > best_score) {
            best_score = score;
            best.has_detected_line = true;
            best.line_angle_deg = angle;
            best.angle_error_deg = angle_error;
            best.center_offset_px = center_offset_px;
            best.center_offset_fraction = center_extent > 0.0 ? center_offset_px / center_extent : 0.0;
            best.x0 = line[0];
            best.y0 = line[1];
            best.x1 = line[2];
            best.y1 = line[3];
        }
    }

    if (!best.has_detected_line && boundary_anchor >= 0) {
        best.has_detected_line = true;
        best.line_angle_deg = orientation == RulerAlignmentOrientation::kHorizontal ? 0.0 : 90.0;
        best.angle_error_deg = 0.0;
        if (orientation == RulerAlignmentOrientation::kHorizontal) {
            best.center_offset_px = static_cast<double>(boundary_anchor) - center_y;
            best.center_offset_fraction = center_y > 0.0 ? best.center_offset_px / center_y : 0.0;
            best.x0 = 0;
            best.x1 = gray.cols - 1;
            best.y0 = boundary_anchor;
            best.y1 = boundary_anchor;
        } else {
            best.center_offset_px = static_cast<double>(boundary_anchor) - center_x;
            best.center_offset_fraction = center_x > 0.0 ? best.center_offset_px / center_x : 0.0;
            best.x0 = boundary_anchor;
            best.x1 = boundary_anchor;
            best.y0 = 0;
            best.y1 = gray.rows - 1;
        }
    }

    return best;
}

void draw_ruler_alignment_overlay(
    cv::Mat* bgr_image,
    const RulerAlignmentMetrics& metrics,
    RulerAlignmentOrientation orientation)
{
    if (bgr_image == nullptr || bgr_image->empty()) {
        return;
    }

    const int width = bgr_image->cols;
    const int height = bgr_image->rows;
    const cv::Scalar guide_color(255, 255, 0);
    const cv::Scalar line_color = metrics.has_detected_line
                                      ? (metrics.angle_error_deg <= 2.0 && std::abs(metrics.center_offset_fraction) <= 0.05
                                             ? cv::Scalar(0, 220, 0)
                                             : cv::Scalar(0, 140, 255))
                                      : cv::Scalar(0, 0, 255);

    if (orientation == RulerAlignmentOrientation::kHorizontal) {
        cv::line(*bgr_image, cv::Point(0, height / 2), cv::Point(width - 1, height / 2), guide_color, 1, cv::LINE_AA);
    } else {
        cv::line(*bgr_image, cv::Point(width / 2, 0), cv::Point(width / 2, height - 1), guide_color, 1, cv::LINE_AA);
    }

    if (metrics.has_detected_line) {
        cv::line(*bgr_image,
                 cv::Point(metrics.x0, metrics.y0),
                 cv::Point(metrics.x1, metrics.y1),
                 line_color,
                 2,
                 cv::LINE_AA);
    }

    std::ostringstream oss;
    if (metrics.has_detected_line) {
        oss << ruler_alignment_orientation_label(orientation)
            << " angle_err=" << std::fixed << std::setprecision(2) << metrics.angle_error_deg
            << "deg offset=" << std::showpos << std::setprecision(1) << metrics.center_offset_px << "px";
    } else {
        oss << "No ruler line detected";
    }
    cv::putText(*bgr_image, oss.str(), cv::Point(20, 32), cv::FONT_HERSHEY_SIMPLEX, 0.75, line_color, 2, cv::LINE_AA);
}

bool write_rgb_image_ppm(
    const std::string& path,
    const std::vector<unsigned char>& rgb,
    int width,
    int height,
    std::string* error_out)
{
    if (width <= 0 || height <= 0 || rgb.size() != static_cast<size_t>(width) * static_cast<size_t>(height) * 3U) {
        if (error_out) {
            *error_out = "RGB preview buffer is invalid for PPM write.";
        }
        return false;
    }

    orange::ScopedFsuid fsuid_guard;
    (void)fsuid_guard;
    std::ofstream out(path, std::ios::binary);
    if (!out) {
        if (error_out) {
            *error_out = "Failed to open FOV capture output path: " + path;
        }
        return false;
    }
    out << "P6\n" << width << " " << height << "\n255\n";
    out.write(reinterpret_cast<const char*>(rgb.data()), static_cast<std::streamsize>(rgb.size()));
    return static_cast<bool>(out);
}

bool read_pnm_token(std::istream& input, std::string* token)
{
    token->clear();
    while (true) {
        int ch = input.peek();
        if (ch == EOF) {
            return false;
        }
        if (std::isspace(static_cast<unsigned char>(ch))) {
            input.get();
            continue;
        }
        if (ch == '#') {
            input.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
            continue;
        }
        break;
    }

    while (true) {
        int ch = input.peek();
        if (ch == EOF || std::isspace(static_cast<unsigned char>(ch)) || ch == '#') {
            break;
        }
        token->push_back(static_cast<char>(input.get()));
    }
    return !token->empty();
}

bool load_representative_frame_rgba(
    const std::string& image_path,
    std::vector<unsigned char>* rgba,
    int* width,
    int* height,
    std::string* error_out)
{
    std::ifstream input(image_path, std::ios::binary);
    if (!input.is_open()) {
        if (error_out) {
            *error_out = "Failed to open representative frame: " + image_path;
        }
        return false;
    }

    std::string magic;
    std::string width_token;
    std::string height_token;
    std::string maxval_token;
    if (!read_pnm_token(input, &magic) ||
        !read_pnm_token(input, &width_token) ||
        !read_pnm_token(input, &height_token) ||
        !read_pnm_token(input, &maxval_token)) {
        if (error_out) {
            *error_out = "Failed to parse representative frame header: " + image_path;
        }
        return false;
    }

    if (magic != "P5" && magic != "P6") {
        if (error_out) {
            *error_out = "Representative frame is not a binary PGM/PPM file: " + image_path;
        }
        return false;
    }

    const int parsed_width = std::stoi(width_token);
    const int parsed_height = std::stoi(height_token);
    const int maxval = std::stoi(maxval_token);
    if (parsed_width <= 0 || parsed_height <= 0 || maxval != 255) {
        if (error_out) {
            *error_out = "Representative frame has unsupported dimensions or max value: " + image_path;
        }
        return false;
    }

    input.get(); // consume the single whitespace byte before raster data

    const int channel_count = magic == "P6" ? 3 : 1;
    const size_t src_size =
        static_cast<size_t>(parsed_width) * static_cast<size_t>(parsed_height) * static_cast<size_t>(channel_count);
    std::vector<unsigned char> src(src_size);
    input.read(reinterpret_cast<char*>(src.data()), static_cast<std::streamsize>(src_size));
    if (input.gcount() != static_cast<std::streamsize>(src_size)) {
        if (error_out) {
            *error_out = "Representative frame raster is truncated: " + image_path;
        }
        return false;
    }

    rgba->resize(static_cast<size_t>(parsed_width) * static_cast<size_t>(parsed_height) * 4U);
    for (int i = 0; i < parsed_width * parsed_height; ++i) {
        const size_t dst_base = static_cast<size_t>(i) * 4U;
        if (channel_count == 1) {
            const unsigned char v = src[static_cast<size_t>(i)];
            (*rgba)[dst_base + 0] = v;
            (*rgba)[dst_base + 1] = v;
            (*rgba)[dst_base + 2] = v;
        } else {
            const size_t src_base = static_cast<size_t>(i) * 3U;
            (*rgba)[dst_base + 0] = src[src_base + 0];
            (*rgba)[dst_base + 1] = src[src_base + 1];
            (*rgba)[dst_base + 2] = src[src_base + 2];
        }
        (*rgba)[dst_base + 3] = 255;
    }

    *width = parsed_width;
    *height = parsed_height;
    return true;
}

void reset_aperture_defaults_for_camera(
    ApertureCharacterizationUiState* ui_state,
    const CameraParams& camera_params,
    int selected_camera,
    const std::string& default_output_dir)
{
    ui_state->selected_camera = selected_camera;
    ui_state->configured_camera_index = selected_camera;
    ui_state->use_explicit_iris_values = false;
    ui_state->iris_values_csv[0] = '\0';
    ui_state->iris_start = static_cast<int>(camera_params.iris_min);
    ui_state->iris_stop = static_cast<int>(camera_params.iris_max);
    ui_state->iris_step_multiple = 1;
    ui_state->reference_iris = static_cast<int>(camera_params.iris_min);
    {
        std::lock_guard<std::mutex> lock(ui_state->alignment_mutex);
        ui_state->horizontal_capture = {};
        ui_state->vertical_capture = {};
        ui_state->live_fov_preview = {};
        ui_state->live_fov_preview.status_message = "Idle";
    }
    if (ui_state->output_dir[0] == '\0') {
        copy_string_to_buffer(ui_state->output_dir, default_output_dir);
    }
}

void join_aperture_worker_if_finished(ApertureCharacterizationUiState* ui_state)
{
    if (!ui_state->running.load(std::memory_order_acquire) && ui_state->worker.joinable()) {
        ui_state->worker.join();
    }
}

void clear_aperture_preview_texture(ApertureCharacterizationUiState* ui_state)
{
    orange::preview::clear_texture(
        &ui_state->preview_texture,
        &ui_state->preview_texture_width,
        &ui_state->preview_texture_height);
    ui_state->preview_texture_path.clear();
    ui_state->preview_texture_error.clear();
    ui_state->representative_canvas_view.fit_requested = true;
    ui_state->representative_canvas_view.last_image_width = 0;
    ui_state->representative_canvas_view.last_image_height = 0;
}

void clear_alignment_preview_texture(ApertureCharacterizationUiState* ui_state)
{
    orange::preview::clear_texture(
        &ui_state->alignment_texture,
        &ui_state->alignment_texture_width,
        &ui_state->alignment_texture_height);
    ui_state->alignment_uploaded_serial = 0;
}

bool ensure_aperture_preview_texture(
    ApertureCharacterizationUiState* ui_state,
    const std::string& image_path)
{
    if (image_path.empty()) {
        clear_aperture_preview_texture(ui_state);
        ui_state->preview_texture_error = "Selected step has no representative frame.";
        return false;
    }
    if (ui_state->preview_texture != 0 && ui_state->preview_texture_path == image_path) {
        return true;
    }

    clear_aperture_preview_texture(ui_state);

    int width = 0;
    int height = 0;
    std::vector<unsigned char> rgba;
    if (!load_representative_frame_rgba(image_path, &rgba, &width, &height, &ui_state->preview_texture_error)) {
        return false;
    }

    orange::preview::update_rgba_texture(
        &ui_state->preview_texture,
        &ui_state->preview_texture_width,
        &ui_state->preview_texture_height,
        rgba,
        width,
        height,
        &ui_state->preview_texture_error);
    ui_state->preview_texture_path = image_path;
    ui_state->preview_texture_error.clear();
    return true;
}

void join_alignment_worker_if_finished(ApertureCharacterizationUiState* ui_state)
{
    if (!ui_state->alignment_running.load(std::memory_order_acquire) && ui_state->alignment_worker.joinable()) {
        ui_state->alignment_worker.join();
    }
}

void stop_fov_alignment_worker(ApertureCharacterizationUiState* ui_state)
{
    if (ui_state->alignment_running.exchange(false, std::memory_order_acq_rel)) {
        ui_state->alignment_stop_requested.store(true, std::memory_order_release);
    }
    if (ui_state->alignment_worker.joinable()) {
        ui_state->alignment_worker.join();
    }
    ui_state->alignment_stop_requested.store(false, std::memory_order_release);
}

bool upload_latest_alignment_texture(ApertureCharacterizationUiState* ui_state)
{
    std::lock_guard<std::mutex> lock(ui_state->alignment_mutex);
    const LiveFovPreviewState& preview = ui_state->live_fov_preview;
    if (!preview.available || preview.rgba.empty() || preview.frame_serial == ui_state->alignment_uploaded_serial) {
        return preview.available;
    }

    orange::preview::update_rgba_texture(
        &ui_state->alignment_texture,
        &ui_state->alignment_texture_width,
        &ui_state->alignment_texture_height,
        preview.rgba,
        preview.width,
        preview.height);
    ui_state->alignment_uploaded_serial = preview.frame_serial;
    return true;
}

bool capture_current_fov_snapshot(
    ApertureCharacterizationUiState* ui_state,
    bool horizontal_capture,
    std::string* error_out)
{
    std::lock_guard<std::mutex> lock(ui_state->alignment_mutex);
    if (!ui_state->live_fov_preview.available ||
        ui_state->live_fov_preview.width <= 0 ||
        ui_state->live_fov_preview.height <= 0 ||
        ui_state->live_fov_preview.raw_rgb.empty()) {
        if (error_out) {
            *error_out = "No live FOV preview frame is available to capture.";
        }
        return false;
    }

    FovCaptureSnapshot snapshot;
    snapshot.available = true;
    snapshot.width = ui_state->live_fov_preview.width;
    snapshot.height = ui_state->live_fov_preview.height;
    snapshot.rgb = ui_state->live_fov_preview.raw_rgb;
    snapshot.metrics = ui_state->live_fov_preview.metrics;
    if (horizontal_capture) {
        ui_state->horizontal_capture = std::move(snapshot);
    } else {
        ui_state->vertical_capture = std::move(snapshot);
    }
    return true;
}

void start_fov_alignment_worker(
    ApertureCharacterizationUiState* ui_state,
    CameraEmergent* ecams,
    CameraParams* cameras_params)
{
    join_alignment_worker_if_finished(ui_state);
    if (ui_state->alignment_running.exchange(true, std::memory_order_acq_rel)) {
        return;
    }
    ui_state->alignment_stop_requested.store(false, std::memory_order_release);

    const int selected_camera = ui_state->selected_camera;
    CameraEmergent* ecam = &ecams[selected_camera];
    CameraParams* camera_params = &cameras_params[selected_camera];
    const int preview_target_fps = std::max(1, ui_state->alignment_preview_fps);

    {
        std::lock_guard<std::mutex> lock(ui_state->alignment_mutex);
        ui_state->live_fov_preview = {};
        ui_state->live_fov_preview.status_message = "Starting live ruler alignment...";
    }

    ui_state->alignment_worker = std::thread([=]() {
        constexpr int kAlignmentPreviewBufferCount = 2;
        Emergent::CEmergentFrame* frames = nullptr;
        bool stream_opened = false;
        bool buffers_allocated = false;
        bool acquisition_started = false;
        bool frame_rate_changed = false;
        unsigned int original_frame_rate = camera_params->frame_rate;
        unsigned int active_preview_fps = static_cast<unsigned int>(preview_target_fps);

        auto publish_error = [&](const std::string& message) {
            std::lock_guard<std::mutex> lock(ui_state->alignment_mutex);
            ui_state->live_fov_preview.error_message = message;
            ui_state->live_fov_preview.status_message = "Live ruler alignment failed.";
        };

        try {
            unsigned int frame_rate_min = camera_params->frame_rate_min;
            unsigned int frame_rate_max = camera_params->frame_rate_max;
            if (get_camera_uint32_param_range(&ecam->camera, "FrameRate", &frame_rate_min, &frame_rate_max)) {
                camera_params->frame_rate_min = frame_rate_min;
                camera_params->frame_rate_max = frame_rate_max;
            }
            const unsigned int clamped_preview_fps =
                std::clamp(static_cast<unsigned int>(preview_target_fps), frame_rate_min, frame_rate_max);
            active_preview_fps = clamped_preview_fps;
            if (clamped_preview_fps != original_frame_rate) {
                check_camera_errors(
                    EVT_CameraSetUInt32Param(&ecam->camera, "FrameRate", clamped_preview_fps),
                    camera_params->camera_serial.c_str());
                camera_params->frame_rate = clamped_preview_fps;
                frame_rate_changed = true;
            }

            camera_open_stream(&ecam->camera, camera_params, "gui_alignment_preview");
            stream_opened = true;

            frames = new Emergent::CEmergentFrame[kAlignmentPreviewBufferCount]();
            allocate_frame_buffer(&ecam->camera, frames, camera_params, kAlignmentPreviewBufferCount);
            buffers_allocated = true;

            check_camera_errors(
                EVT_CameraExecuteCommand(&ecam->camera, "AcquisitionStart"),
                camera_params->camera_serial.c_str());
            acquisition_started = true;

            while (!ui_state->alignment_stop_requested.load(std::memory_order_acquire)) {
                Emergent::CEmergentFrame frame{};
                int dropped_frames = 0;
                if (!orange::preview::grab_latest_frame(&ecam->camera, camera_params, 250, &frame, &dropped_frames)) {
                    continue;
                }

                cv::Mat bgr;
                std::string convert_error;
                if (!orange::preview::frame_to_bgr(frame, &bgr, &convert_error)) {
                    EVT_CameraQueueFrame(&ecam->camera, &frame);
                    throw std::runtime_error(convert_error);
                }

                const int max_preview_dimension = 1920;
                double scale = 1.0;
                if (std::max(bgr.cols, bgr.rows) > max_preview_dimension) {
                    scale = static_cast<double>(max_preview_dimension) /
                            static_cast<double>(std::max(bgr.cols, bgr.rows));
                }
                cv::Mat preview_bgr;
                if (scale < 1.0) {
                    cv::resize(bgr, preview_bgr, cv::Size(), scale, scale, cv::INTER_AREA);
                } else {
                    preview_bgr = bgr;
                }

                cv::Mat gray;
                cv::cvtColor(preview_bgr, gray, cv::COLOR_BGR2GRAY);
                const RulerAlignmentOrientation orientation =
                    ui_state->alignment_orientation.load(std::memory_order_acquire) == static_cast<int>(RulerAlignmentOrientation::kVertical)
                        ? RulerAlignmentOrientation::kVertical
                        : RulerAlignmentOrientation::kHorizontal;
                const RulerAlignmentMetrics metrics = detect_ruler_alignment(gray, orientation);

                cv::Mat overlay_bgr = preview_bgr.clone();
                draw_ruler_alignment_overlay(&overlay_bgr, metrics, orientation);
                cv::Mat overlay_rgba;
                cv::cvtColor(overlay_bgr, overlay_rgba, cv::COLOR_BGR2RGBA);
                cv::Mat preview_rgb;
                cv::cvtColor(preview_bgr, preview_rgb, cv::COLOR_BGR2RGB);

                {
                    std::lock_guard<std::mutex> lock(ui_state->alignment_mutex);
                    ui_state->live_fov_preview.available = true;
                    ui_state->live_fov_preview.width = overlay_rgba.cols;
                    ui_state->live_fov_preview.height = overlay_rgba.rows;
                    ui_state->live_fov_preview.frame_serial += 1;
                    ui_state->live_fov_preview.rgba.assign(
                        overlay_rgba.data,
                        overlay_rgba.data + overlay_rgba.total() * overlay_rgba.elemSize());
                    ui_state->live_fov_preview.raw_rgb.assign(
                        preview_rgb.data,
                        preview_rgb.data + preview_rgb.total() * preview_rgb.elemSize());
                    ui_state->live_fov_preview.metrics = metrics;
                    ui_state->live_fov_preview.error_message.clear();
                    ui_state->live_fov_preview.status_message =
                        std::string("Live ") + ruler_alignment_orientation_label(orientation) +
                        " ruler alignment @ " + std::to_string(active_preview_fps) +
                        " FPS" + (dropped_frames > 0 ? " (dropped " + std::to_string(dropped_frames) + ")" : "");
                }

                check_camera_errors(EVT_CameraQueueFrame(&ecam->camera, &frame), camera_params->camera_serial.c_str());
            }
        } catch (const std::exception& ex) {
            publish_error(ex.what());
        }

        if (acquisition_started) {
            EVT_CameraExecuteCommand(&ecam->camera, "AcquisitionStop");
        }
        if (buffers_allocated && frames != nullptr) {
            try {
                destroy_frame_buffer(&ecam->camera, frames, kAlignmentPreviewBufferCount, camera_params);
            } catch (...) {
            }
        }
        delete[] frames;
        if (stream_opened) {
            EVT_CameraCloseStream(&ecam->camera);
        }
        if (frame_rate_changed) {
            EVT_CameraSetUInt32Param(&ecam->camera, "FrameRate", original_frame_rate);
            camera_params->frame_rate = original_frame_rate;
        }
        ui_state->alignment_running.store(false, std::memory_order_release);
    });
}

void start_aperture_characterization_worker(
    ApertureCharacterizationUiState* ui_state,
    CameraEmergent* ecams,
    CameraParams* cameras_params)
{
    join_aperture_worker_if_finished(ui_state);
    stop_fov_alignment_worker(ui_state);

    if (ui_state->running.exchange(true, std::memory_order_acq_rel)) {
        return;
    }

    const int selected_camera = ui_state->selected_camera;
    CameraEmergent* ecam = &ecams[selected_camera];
    CameraParams* camera_params = &cameras_params[selected_camera];

    const bool use_explicit_iris_values = ui_state->use_explicit_iris_values;
    const std::string iris_values_csv = ui_state->iris_values_csv;
    const int iris_start = ui_state->iris_start;
    const int iris_stop = ui_state->iris_stop;
    const int iris_step_multiple = ui_state->iris_step_multiple;
    const int frames_per_step = ui_state->frames_per_step;
    const int settle_frames = ui_state->settle_frames;
    const int buffer_count = ui_state->buffer_count;
    const int grab_timeout_ms = ui_state->grab_timeout_ms;
    const int grid_rows = ui_state->grid_rows;
    const int grid_cols = ui_state->grid_cols;
    const bool restore_original_iris = ui_state->restore_original_iris;
    const bool save_representative_frames = ui_state->save_representative_frames;
    const bool use_reference_iris = ui_state->use_reference_iris;
    const int reference_iris = ui_state->reference_iris;
    const bool use_reference_f_number = ui_state->use_reference_f_number;
    const float reference_f_number = ui_state->reference_f_number;
    const bool enable_fov_calibration = ui_state->enable_fov_calibration;
    const float working_distance_mm = ui_state->working_distance_mm;
    const float pixel_pitch_um = ui_state->pixel_pitch_um;
    const float field_width_mm = ui_state->field_width_mm;
    const float field_height_mm = ui_state->field_height_mm;
    const float saturated_white_fraction = ui_state->saturated_white_fraction;
    const float saturated_p99_min = ui_state->saturated_p99_min;
    const float dim_mean_max = ui_state->dim_mean_max;
    const float dim_p95_max = ui_state->dim_p95_max;
    const float dim_black_fraction_min = ui_state->dim_black_fraction_min;
    const std::string output_dir = ui_state->output_dir;
    const std::string output_prefix_base = ui_state->output_prefix;
    FovCaptureSnapshot horizontal_capture;
    FovCaptureSnapshot vertical_capture;
    {
        std::lock_guard<std::mutex> lock(ui_state->alignment_mutex);
        horizontal_capture = ui_state->horizontal_capture;
        vertical_capture = ui_state->vertical_capture;
    }

    ui_state->progress_completed_steps.store(0, std::memory_order_release);
    ui_state->progress_total_steps.store(0, std::memory_order_release);
    ui_state->progress_iris.store(0, std::memory_order_release);
    {
        std::lock_guard<std::mutex> lock(ui_state->mutex);
        ui_state->status_message = "Preparing aperture characterization...";
        ui_state->error_message.clear();
        ui_state->output_artifact_id.clear();
        ui_state->output_artifact_dir.clear();
        ui_state->output_manifest_path.clear();
        ui_state->output_fingerprint.clear();
        ui_state->output_json_path.clear();
        ui_state->output_steps_csv_path.clear();
        ui_state->output_frames_csv_path.clear();
        ui_state->output_frame_image_dir.clear();
        ui_state->has_result = false;
    }

    ui_state->worker = std::thread([=]() {
        auto finish = [&]() {
            ui_state->running.store(false, std::memory_order_release);
        };

        try {
            std::vector<unsigned int> iris_values;
            if (use_explicit_iris_values) {
                std::string error;
                if (!parse_uint_csv_text(iris_values_csv.c_str(), &iris_values, &error)) {
                    throw std::runtime_error(error);
                }
            } else {
                if (iris_start < static_cast<int>(camera_params->iris_min) ||
                    iris_stop > static_cast<int>(camera_params->iris_max) ||
                    iris_start > iris_stop) {
                    throw std::runtime_error("Generated iris sweep range is invalid for the selected camera.");
                }
                iris_values = build_iris_sweep(
                    static_cast<unsigned int>(iris_start),
                    static_cast<unsigned int>(iris_stop),
                    camera_params->iris_inc,
                    static_cast<unsigned int>(std::max(1, iris_step_multiple)));
            }

            if (iris_values.empty()) {
                throw std::runtime_error("No iris values were generated for this run.");
            }

            for (unsigned int iris_value : iris_values) {
                if (iris_value < camera_params->iris_min || iris_value > camera_params->iris_max) {
                    throw std::runtime_error("One or more requested iris values are outside the camera range.");
                }
            }

            ui_state->progress_total_steps.store(static_cast<int>(iris_values.size()), std::memory_order_release);

            const std::filesystem::path artifact_root_dir(output_dir);
            {
                orange::ScopedFsuid fsuid_guard;
                (void)fsuid_guard;
                std::filesystem::create_directories(artifact_root_dir);
            }
            const std::string timestamp = get_current_date_time();
            const std::string created_utc = get_current_utc_timestamp();
            const std::string artifact_id =
                build_aperture_characterization_artifact_id(output_prefix_base, *camera_params, timestamp);
            const ApertureCharacterizationArtifactPaths artifact_paths =
                make_aperture_characterization_artifact_paths(artifact_root_dir.string(), artifact_id);
            {
                orange::ScopedFsuid fsuid_guard;
                (void)fsuid_guard;
                std::filesystem::create_directories(artifact_paths.artifact_dir);
            }
            std::string write_error;
            CameraConfigSnapshotProvenance camera_config_snapshot;
            if (!camera_params->config_path.empty()) {
                camera_config_snapshot.has_source_path = true;
                camera_config_snapshot.source_path = camera_params->config_path;
            } else {
                camera_config_snapshot.error = "config path missing";
            }

            if (camera_config_snapshot.has_source_path) {
                std::string config_snapshot_contents;
                std::string config_snapshot_error;
                if (!read_camera_config_snapshot(*camera_params, &config_snapshot_contents, &config_snapshot_error)) {
                    camera_config_snapshot.error = config_snapshot_error;
                } else {
                    orange::ScopedFsuid fsuid_guard;
                    (void)fsuid_guard;
                    std::ofstream config_out(
                        artifact_paths.camera_config_snapshot_path,
                        std::ios::out | std::ios::binary | std::ios::trunc);
                    if (!config_out.is_open()) {
                        camera_config_snapshot.error =
                            "Failed to open camera config snapshot output path: " +
                            artifact_paths.camera_config_snapshot_path;
                    } else {
                        config_out.write(
                            config_snapshot_contents.data(),
                            static_cast<std::streamsize>(config_snapshot_contents.size()));
                        config_out.close();
                        if (!config_out) {
                            camera_config_snapshot.error =
                                "Failed to write camera config snapshot output path: " +
                                artifact_paths.camera_config_snapshot_path;
                        } else {
                            camera_config_snapshot.has_snapshot = true;
                            camera_config_snapshot.snapshot_path = artifact_paths.camera_config_snapshot_path;
                            camera_config_snapshot.error.clear();
                        }
                    }
                }
            }

            FovCalibrationData fov_calibration;
            fov_calibration.enabled = enable_fov_calibration;
            if (enable_fov_calibration) {
                fov_calibration.working_distance_mm = std::max(0.0f, working_distance_mm);
                fov_calibration.pixel_pitch_um = std::max(0.0f, pixel_pitch_um);
                fov_calibration.sensor_width_mm =
                    static_cast<double>(camera_params->width) * static_cast<double>(fov_calibration.pixel_pitch_um) / 1000.0;
                fov_calibration.sensor_height_mm =
                    static_cast<double>(camera_params->height) * static_cast<double>(fov_calibration.pixel_pitch_um) / 1000.0;
                if (field_width_mm > 0.0f) {
                    fov_calibration.has_field_width_mm = true;
                    fov_calibration.field_width_mm = field_width_mm;
                }
                if (field_height_mm > 0.0f) {
                    fov_calibration.has_field_height_mm = true;
                    fov_calibration.field_height_mm = field_height_mm;
                }
                if (fov_calibration.has_field_width_mm && fov_calibration.field_width_mm > 0.0) {
                    fov_calibration.has_magnification_x = true;
                    fov_calibration.magnification_x =
                        fov_calibration.sensor_width_mm / fov_calibration.field_width_mm;
                }
                if (fov_calibration.has_field_height_mm && fov_calibration.field_height_mm > 0.0) {
                    fov_calibration.has_magnification_y = true;
                    fov_calibration.magnification_y =
                        fov_calibration.sensor_height_mm / fov_calibration.field_height_mm;
                }
                if (fov_calibration.has_magnification_x || fov_calibration.has_magnification_y) {
                    const double mag_sum =
                        (fov_calibration.has_magnification_x ? fov_calibration.magnification_x : 0.0) +
                        (fov_calibration.has_magnification_y ? fov_calibration.magnification_y : 0.0);
                    const double mag_count =
                        (fov_calibration.has_magnification_x ? 1.0 : 0.0) +
                        (fov_calibration.has_magnification_y ? 1.0 : 0.0);
                    fov_calibration.has_mean_magnification = mag_count > 0.0;
                    fov_calibration.mean_magnification = mag_count > 0.0 ? (mag_sum / mag_count) : 0.0;
                }
                if (use_reference_f_number && fov_calibration.has_mean_magnification) {
                    fov_calibration.has_effective_reference_f_number = true;
                    fov_calibration.effective_reference_f_number =
                        static_cast<double>(reference_f_number) * (1.0 + fov_calibration.mean_magnification);
                }

                const bool has_any_fov_capture = horizontal_capture.available || vertical_capture.available;
                if (has_any_fov_capture) {
                    orange::ScopedFsuid fsuid_guard;
                    (void)fsuid_guard;
                    std::filesystem::create_directories(artifact_paths.fov_reference_frames_dir);
                }
                if (horizontal_capture.available) {
                    if (!write_rgb_image_ppm(
                            artifact_paths.fov_horizontal_capture_path,
                            horizontal_capture.rgb,
                            horizontal_capture.width,
                            horizontal_capture.height,
                            &write_error)) {
                        throw std::runtime_error(write_error);
                    }
                    fov_calibration.horizontal_capture.has_capture = true;
                    fov_calibration.horizontal_capture.capture_path = artifact_paths.fov_horizontal_capture_path;
                    fov_calibration.horizontal_capture.has_detected_line = horizontal_capture.metrics.has_detected_line;
                    fov_calibration.horizontal_capture.line_angle_deg = horizontal_capture.metrics.line_angle_deg;
                    fov_calibration.horizontal_capture.angle_error_deg = horizontal_capture.metrics.angle_error_deg;
                    fov_calibration.horizontal_capture.center_offset_px = horizontal_capture.metrics.center_offset_px;
                    fov_calibration.horizontal_capture.center_offset_fraction = horizontal_capture.metrics.center_offset_fraction;
                }
                if (vertical_capture.available) {
                    if (!write_rgb_image_ppm(
                            artifact_paths.fov_vertical_capture_path,
                            vertical_capture.rgb,
                            vertical_capture.width,
                            vertical_capture.height,
                            &write_error)) {
                        throw std::runtime_error(write_error);
                    }
                    fov_calibration.vertical_capture.has_capture = true;
                    fov_calibration.vertical_capture.capture_path = artifact_paths.fov_vertical_capture_path;
                    fov_calibration.vertical_capture.has_detected_line = vertical_capture.metrics.has_detected_line;
                    fov_calibration.vertical_capture.line_angle_deg = vertical_capture.metrics.line_angle_deg;
                    fov_calibration.vertical_capture.angle_error_deg = vertical_capture.metrics.angle_error_deg;
                    fov_calibration.vertical_capture.center_offset_px = vertical_capture.metrics.center_offset_px;
                    fov_calibration.vertical_capture.center_offset_fraction = vertical_capture.metrics.center_offset_fraction;
                }
            }

            ApertureCharacterizationRequest request;
            request.iris_values = iris_values;
            request.frames_per_step = static_cast<unsigned int>(std::max(1, frames_per_step));
            request.settle_frames = static_cast<unsigned int>(std::max(0, settle_frames));
            request.grab_timeout_ms = static_cast<unsigned int>(std::max(1, grab_timeout_ms));
            request.grid_rows = (grid_rows > 0 && grid_cols > 0) ? static_cast<unsigned int>(grid_rows) : 0U;
            request.grid_cols = (grid_rows > 0 && grid_cols > 0) ? static_cast<unsigned int>(grid_cols) : 0U;
            request.manage_acquisition = true;
            request.restore_original_iris = restore_original_iris;
            request.has_reference_iris = use_reference_iris;
            request.reference_iris = static_cast<unsigned int>(std::max(0, reference_iris));
            request.has_reference_f_number = use_reference_f_number;
            request.reference_f_number = reference_f_number;
            request.camera_config_snapshot = camera_config_snapshot;
            request.fov_calibration = fov_calibration;
            request.thresholds.saturated_white_fraction = saturated_white_fraction;
            request.thresholds.saturated_p99_min = saturated_p99_min;
            request.thresholds.dim_mean_max = dim_mean_max;
            request.thresholds.dim_p95_max = dim_p95_max;
            request.thresholds.dim_black_fraction_min = dim_black_fraction_min;
            request.save_representative_frames = save_representative_frames;
            request.representative_frame_dir = artifact_paths.representative_frames_dir;
            request.representative_frame_prefix = artifact_id;
            request.progress_callback = [ui_state](size_t completed_steps, size_t total_steps, unsigned int iris_value) {
                ui_state->progress_completed_steps.store(static_cast<int>(completed_steps), std::memory_order_release);
                ui_state->progress_total_steps.store(static_cast<int>(total_steps), std::memory_order_release);
                ui_state->progress_iris.store(static_cast<int>(iris_value), std::memory_order_release);
            };

            std::string lens_name;
            get_camera_string_param(&ecam->camera, "LensName", &lens_name);

            ApertureCharacterizationResult result = characterize_aperture_with_stream(
                &ecam->camera,
                camera_params,
                request,
                static_cast<unsigned int>(std::max(1, buffer_count)));

            const nlohmann::json measurement_json =
                aperture_characterization_to_json(
                    result,
                    request,
                    *camera_params,
                    lens_name,
                    artifact_id,
                    created_utc,
                    "",
                    artifact_paths);
            const std::string fingerprint =
                compute_aperture_characterization_fingerprint(measurement_json, artifact_paths, &write_error);
            if (fingerprint.empty()) {
                throw std::runtime_error(write_error.empty()
                                             ? "Failed to compute aperture artifact fingerprint."
                                             : write_error);
            }
            const nlohmann::json measurement_json_with_fingerprint =
                aperture_characterization_to_json(
                    result,
                    request,
                    *camera_params,
                    lens_name,
                    artifact_id,
                    created_utc,
                    fingerprint,
                    artifact_paths);
            const nlohmann::json manifest_json =
                aperture_characterization_manifest_to_json(
                    result,
                    request,
                    *camera_params,
                    lens_name,
                    artifact_id,
                    created_utc,
                    fingerprint,
                    artifact_paths);
            if (!write_aperture_characterization_json(artifact_paths.manifest_path, manifest_json, &write_error) ||
                !write_aperture_characterization_json(artifact_paths.measurement_json_path, measurement_json_with_fingerprint, &write_error) ||
                !write_aperture_characterization_step_csv(artifact_paths.steps_csv_path, result, artifact_paths, &write_error) ||
                !write_aperture_characterization_frame_csv(artifact_paths.frames_csv_path, result, &write_error)) {
                throw std::runtime_error(write_error);
            }
            if (!update_calibration_artifact_registry(artifact_root_dir.string(), manifest_json, &write_error)) {
                throw std::runtime_error(write_error);
            }

            {
                std::lock_guard<std::mutex> lock(ui_state->mutex);
                ui_state->status_message = "Aperture characterization completed.";
                ui_state->error_message.clear();
                ui_state->output_artifact_id = artifact_id;
                ui_state->output_artifact_dir = artifact_paths.artifact_dir;
                ui_state->output_manifest_path = artifact_paths.manifest_path;
                ui_state->output_fingerprint = fingerprint;
                ui_state->output_json_path = artifact_paths.measurement_json_path;
                ui_state->output_steps_csv_path = artifact_paths.steps_csv_path;
                ui_state->output_frames_csv_path = artifact_paths.frames_csv_path;
                ui_state->output_frame_image_dir = artifact_paths.representative_frames_dir;
                ui_state->has_result = true;
                ui_state->selected_heatmap_step = 0;
                ui_state->last_result = std::move(result);
                ui_state->last_fov_calibration = request.fov_calibration;
                ui_state->last_camera_serial = camera_params->camera_serial;
                ui_state->last_focus = camera_params->focus;
                ui_state->last_exposure = camera_params->exposure;
            }
        } catch (const std::exception& ex) {
            std::cerr << camera_params->camera_serial
                      << " [aperture_characterization] Run failed: "
                      << ex.what() << std::endl;
            std::lock_guard<std::mutex> lock(ui_state->mutex);
            ui_state->status_message = "Aperture characterization failed.";
            ui_state->error_message = ex.what();
        } catch (...) {
            std::cerr << camera_params->camera_serial
                      << " [aperture_characterization] Run failed: unknown error"
                      << std::endl;
            std::lock_guard<std::mutex> lock(ui_state->mutex);
            ui_state->status_message = "Aperture characterization failed.";
            ui_state->error_message = "Unknown error";
        }

        finish();
    });
}

void render_aperture_characterization_window(
    ApertureCharacterizationUiState* ui_state,
    CameraControl* camera_control,
    CameraEmergent* ecams,
    CameraParams* cameras_params,
    int num_cameras,
    const std::string& default_output_dir)
{
    if (!ui_state->show_window) {
        stop_fov_alignment_worker(ui_state);
        clear_aperture_preview_texture(ui_state);
        clear_alignment_preview_texture(ui_state);
        return;
    }

    join_aperture_worker_if_finished(ui_state);
    join_alignment_worker_if_finished(ui_state);

    if (!ImGui::Begin("Aperture Characterization", &ui_state->show_window)) {
        ImGui::End();
        return;
    }

    if (!camera_control->open || num_cameras <= 0 || cameras_params == nullptr || ecams == nullptr) {
        stop_fov_alignment_worker(ui_state);
        clear_alignment_preview_texture(ui_state);
        ImGui::TextDisabled("Open one or more cameras to run aperture characterization.");
        ImGui::End();
        return;
    }

    ui_state->selected_camera = std::clamp(ui_state->selected_camera, 0, num_cameras - 1);
    if (ui_state->configured_camera_index != ui_state->selected_camera) {
        reset_aperture_defaults_for_camera(
            ui_state, cameras_params[ui_state->selected_camera], ui_state->selected_camera, default_output_dir);
    }

    const bool running = ui_state->running.load(std::memory_order_acquire);
    const bool alignment_running = ui_state->alignment_running.load(std::memory_order_acquire);
    if (running) {
        ImGui::BeginDisabled();
    }

    std::vector<const char*> camera_labels;
    camera_labels.reserve(num_cameras);
    for (int i = 0; i < num_cameras; ++i) {
        camera_labels.push_back(cameras_params[i].camera_name.c_str());
    }
    if (ImGui::Combo("Camera", &ui_state->selected_camera, camera_labels.data(), num_cameras)) {
        stop_fov_alignment_worker(ui_state);
        clear_alignment_preview_texture(ui_state);
        reset_aperture_defaults_for_camera(
            ui_state, cameras_params[ui_state->selected_camera], ui_state->selected_camera, default_output_dir);
    }

    CameraParams& selected_camera = cameras_params[ui_state->selected_camera];
    ImGui::Text("Serial: %s", selected_camera.camera_serial.c_str());
    ImGui::Text("Focus: %u  Exposure: %u  Gain: %u  PixelFormat: %s",
                selected_camera.focus,
                selected_camera.exposure,
                selected_camera.gain,
                selected_camera.pixel_format.c_str());
    ImGui::Text("Iris range: [%u, %u] inc=%u",
                selected_camera.iris_min,
                selected_camera.iris_max,
                selected_camera.iris_inc);
    ImGui::TextWrapped(
        "Focus changes the measured transmission on a macro setup. Treat focus as part of the calibration key and rerun if focus changes materially.");

    ImGui::Separator();
    ImGui::Checkbox("Use explicit iris CSV", &ui_state->use_explicit_iris_values);
    if (ui_state->use_explicit_iris_values) {
        ImGui::InputText("Iris CSV", ui_state->iris_values_csv, IM_ARRAYSIZE(ui_state->iris_values_csv));
    } else {
        ImGui::InputInt("Iris start", &ui_state->iris_start);
        ImGui::InputInt("Iris stop", &ui_state->iris_stop);
        ImGui::InputInt("Iris step multiple", &ui_state->iris_step_multiple);
        if (ui_state->iris_step_multiple < 1) {
            ui_state->iris_step_multiple = 1;
        }
    }

    ImGui::InputInt("Frames per step", &ui_state->frames_per_step);
    ImGui::InputInt("Settle frames", &ui_state->settle_frames);
    ImGui::InputInt("Frame buffer count", &ui_state->buffer_count);
    ImGui::InputInt("Grab timeout (ms)", &ui_state->grab_timeout_ms);
    ImGui::InputInt("Grid rows (0=off)", &ui_state->grid_rows);
    ImGui::InputInt("Grid cols (0=off)", &ui_state->grid_cols);
    if (ui_state->frames_per_step < 1) ui_state->frames_per_step = 1;
    if (ui_state->settle_frames < 0) ui_state->settle_frames = 0;
    if (ui_state->buffer_count < 1) ui_state->buffer_count = 1;
    if (ui_state->grab_timeout_ms < 1) ui_state->grab_timeout_ms = 1;
    if (ui_state->grid_rows < 0) ui_state->grid_rows = 0;
    if (ui_state->grid_cols < 0) ui_state->grid_cols = 0;

    ImGui::Checkbox("Restore original iris", &ui_state->restore_original_iris);
    ImGui::Checkbox("Save representative frame per iris", &ui_state->save_representative_frames);
    ImGui::Checkbox("Use reference iris", &ui_state->use_reference_iris);
    if (ui_state->use_reference_iris) {
        ImGui::InputInt("Reference iris", &ui_state->reference_iris);
    }
    ImGui::Checkbox("Use reference f-number", &ui_state->use_reference_f_number);
    if (ui_state->use_reference_f_number) {
        ImGui::InputFloat("Reference f-number", &ui_state->reference_f_number, 0.1f, 1.0f, "%.2f");
        if (ui_state->reference_f_number <= 0.0f) {
            ui_state->reference_f_number = 2.8f;
        }
    }

    ImGui::Separator();
    ImGui::Checkbox("Include FOV calibration metadata", &ui_state->enable_fov_calibration);
    if (ui_state->enable_fov_calibration) {
        ImGui::InputFloat("Working distance (mm)", &ui_state->working_distance_mm, 5.0f, 25.0f, "%.1f");
        ImGui::InputFloat("Pixel pitch (um)", &ui_state->pixel_pitch_um, 0.01f, 0.1f, "%.3f");
        ImGui::InputFloat("Field width (mm)", &ui_state->field_width_mm, 1.0f, 10.0f, "%.2f");
        ImGui::InputFloat("Field height (mm)", &ui_state->field_height_mm, 1.0f, 10.0f, "%.2f");
        ImGui::InputInt("Preview FPS", &ui_state->alignment_preview_fps);
        ui_state->working_distance_mm = std::max(0.0f, ui_state->working_distance_mm);
        ui_state->pixel_pitch_um = std::max(0.0f, ui_state->pixel_pitch_um);
        ui_state->field_width_mm = std::max(0.0f, ui_state->field_width_mm);
        ui_state->field_height_mm = std::max(0.0f, ui_state->field_height_mm);
        ui_state->alignment_preview_fps = std::clamp(ui_state->alignment_preview_fps, 1, 120);

        const double sensor_width_mm =
            static_cast<double>(selected_camera.width) * static_cast<double>(ui_state->pixel_pitch_um) / 1000.0;
        const double sensor_height_mm =
            static_cast<double>(selected_camera.height) * static_cast<double>(ui_state->pixel_pitch_um) / 1000.0;
        ImGui::Text("Derived sensor size: %.3f mm x %.3f mm", sensor_width_mm, sensor_height_mm);
        if (ui_state->field_width_mm > 0.0f) {
            ImGui::Text("Magnification X: %.4f", sensor_width_mm / static_cast<double>(ui_state->field_width_mm));
        }
        if (ui_state->field_height_mm > 0.0f) {
            ImGui::Text("Magnification Y: %.4f", sensor_height_mm / static_cast<double>(ui_state->field_height_mm));
        }
        if (ui_state->use_reference_f_number && (ui_state->field_width_mm > 0.0f || ui_state->field_height_mm > 0.0f)) {
            double mag_sum = 0.0;
            double mag_count = 0.0;
            if (ui_state->field_width_mm > 0.0f) {
                mag_sum += sensor_width_mm / static_cast<double>(ui_state->field_width_mm);
                mag_count += 1.0;
            }
            if (ui_state->field_height_mm > 0.0f) {
                mag_sum += sensor_height_mm / static_cast<double>(ui_state->field_height_mm);
                mag_count += 1.0;
            }
            if (mag_count > 0.0) {
                const double effective_reference_f = static_cast<double>(ui_state->reference_f_number) * (1.0 + (mag_sum / mag_count));
                ImGui::Text("Approx effective reference f-number: %.3f", effective_reference_f);
            }
        }

        ImGui::SeparatorText("Live Ruler Alignment");
        const bool can_preview = !running && !camera_control->subscribe && !camera_control->record_video;
        int alignment_orientation = ui_state->alignment_orientation.load(std::memory_order_acquire);
        const char* orientation_items[] = {"Horizontal ruler", "Vertical ruler"};
        if (ImGui::Combo("Alignment target", &alignment_orientation, orientation_items, IM_ARRAYSIZE(orientation_items))) {
            ui_state->alignment_orientation.store(alignment_orientation, std::memory_order_release);
        }

        if (!can_preview) {
            ImGui::TextDisabled("Stop streaming and recording before using live ruler alignment.");
        }

        if (!alignment_running) {
            if (ImGui::Button("Start live alignment")) {
                if (can_preview) {
                    start_fov_alignment_worker(ui_state, ecams, cameras_params);
                }
            }
        } else {
            if (ImGui::Button("Stop live alignment")) {
                stop_fov_alignment_worker(ui_state);
            }
        }
        ImGui::SameLine();
        ImGui::Text("%s", alignment_running ? "Live" : "Stopped");

        std::string fov_status;
        std::string fov_error;
        bool preview_available = false;
        RulerAlignmentMetrics live_metrics;
        {
            std::lock_guard<std::mutex> lock(ui_state->alignment_mutex);
            fov_status = ui_state->live_fov_preview.status_message;
            fov_error = ui_state->live_fov_preview.error_message;
            preview_available = ui_state->live_fov_preview.available;
            live_metrics = ui_state->live_fov_preview.metrics;
        }
        if (!fov_status.empty()) {
            ImGui::TextWrapped("%s", fov_status.c_str());
        }
        if (!fov_error.empty()) {
            ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.35f, 1.0f), "%s", fov_error.c_str());
        }
        if (live_metrics.has_detected_line) {
            ImGui::Text("Line angle %.2f deg, angle error %.2f deg, center offset %.1f px (%.3f)",
                        live_metrics.line_angle_deg,
                        live_metrics.angle_error_deg,
                        live_metrics.center_offset_px,
                        live_metrics.center_offset_fraction);
        }

        if (preview_available && upload_latest_alignment_texture(ui_state) && ui_state->alignment_texture != 0) {
            const float width = static_cast<float>(ui_state->alignment_texture_width);
            const float height = static_cast<float>(ui_state->alignment_texture_height);
            const ImVec2 available = ImGui::GetContentRegionAvail();
            const float width_scale = available.x > 0.0f ? available.x / width : 1.0f;
            const float height_limit = std::clamp(ImGui::GetIO().DisplaySize.y * 0.55f, 480.0f, 1100.0f);
            const float height_scale = height_limit / std::max(1.0f, height);
            const float scale = std::min(1.0f, std::min(width_scale, height_scale));
            ImGui::Image(
                (ImTextureID)(intptr_t)ui_state->alignment_texture,
                ImVec2(width * scale, height * scale));
        }

        std::string capture_error;
        if (ImGui::Button("Capture horizontal ruler")) {
            if (!capture_current_fov_snapshot(ui_state, true, &capture_error)) {
                std::lock_guard<std::mutex> lock(ui_state->alignment_mutex);
                ui_state->live_fov_preview.error_message = capture_error;
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("Capture vertical ruler")) {
            if (!capture_current_fov_snapshot(ui_state, false, &capture_error)) {
                std::lock_guard<std::mutex> lock(ui_state->alignment_mutex);
                ui_state->live_fov_preview.error_message = capture_error;
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("Clear ruler captures")) {
            std::lock_guard<std::mutex> lock(ui_state->alignment_mutex);
            ui_state->horizontal_capture = {};
            ui_state->vertical_capture = {};
        }

        {
            std::lock_guard<std::mutex> lock(ui_state->alignment_mutex);
            if (ui_state->horizontal_capture.available) {
                ImGui::Text("Horizontal capture: %dx%d, angle error %.2f deg, offset %.3f",
                            ui_state->horizontal_capture.width,
                            ui_state->horizontal_capture.height,
                            ui_state->horizontal_capture.metrics.angle_error_deg,
                            ui_state->horizontal_capture.metrics.center_offset_fraction);
            }
            if (ui_state->vertical_capture.available) {
                ImGui::Text("Vertical capture: %dx%d, angle error %.2f deg, offset %.3f",
                            ui_state->vertical_capture.width,
                            ui_state->vertical_capture.height,
                            ui_state->vertical_capture.metrics.angle_error_deg,
                            ui_state->vertical_capture.metrics.center_offset_fraction);
            }
        }
    }

    ImGui::Separator();
    ImGui::InputFloat("Saturated white fraction", &ui_state->saturated_white_fraction, 0.0001f, 0.001f, "%.4f");
    ImGui::InputFloat("Saturated p99 min", &ui_state->saturated_p99_min, 1.0f, 5.0f, "%.1f");
    ImGui::InputFloat("Dim mean max", &ui_state->dim_mean_max, 1.0f, 5.0f, "%.1f");
    ImGui::InputFloat("Dim p95 max", &ui_state->dim_p95_max, 1.0f, 5.0f, "%.1f");
    ImGui::InputFloat("Dim black fraction min", &ui_state->dim_black_fraction_min, 0.01f, 0.1f, "%.2f");

    ImGui::Separator();
    ImGui::InputText("Artifact root", ui_state->output_dir, IM_ARRAYSIZE(ui_state->output_dir));
    ImGui::InputText("Artifact label", ui_state->output_prefix, IM_ARRAYSIZE(ui_state->output_prefix));

    if (running) {
        ImGui::EndDisabled();
    }

    const bool can_run = !running && !camera_control->subscribe && !camera_control->record_video;
    if (!can_run) {
        ImGui::TextDisabled("Stop streaming and recording before starting characterization.");
    }

    if (ImGui::Button("Run Characterization")) {
        if (can_run) {
            start_aperture_characterization_worker(ui_state, ecams, cameras_params);
        }
    }

    if (running) {
        const int completed_steps = ui_state->progress_completed_steps.load(std::memory_order_acquire);
        const int total_steps = std::max(1, ui_state->progress_total_steps.load(std::memory_order_acquire));
        const int current_iris = ui_state->progress_iris.load(std::memory_order_acquire);
        ImGui::SameLine();
        ImGui::Text("Running...");
        ImGui::ProgressBar(static_cast<float>(completed_steps) / static_cast<float>(total_steps), ImVec2(-1.0f, 0.0f));
        ImGui::Text("Completed %d / %d steps. Last iris=%d", completed_steps, total_steps, current_iris);
    }

    std::string status_message;
    std::string error_message;
    std::string output_artifact_id;
    std::string output_artifact_dir;
    std::string output_manifest_path;
    std::string output_fingerprint;
    std::string output_json_path;
    std::string output_steps_csv_path;
    std::string output_frames_csv_path;
    std::string output_frame_image_dir;
    bool has_result = false;
    ApertureCharacterizationResult last_result;
    FovCalibrationData last_fov_calibration;
    std::string last_camera_serial;
    unsigned int last_focus = 0;
    unsigned int last_exposure = 0;
    {
        std::lock_guard<std::mutex> lock(ui_state->mutex);
        status_message = ui_state->status_message;
        error_message = ui_state->error_message;
        output_artifact_id = ui_state->output_artifact_id;
        output_artifact_dir = ui_state->output_artifact_dir;
        output_manifest_path = ui_state->output_manifest_path;
        output_fingerprint = ui_state->output_fingerprint;
        output_json_path = ui_state->output_json_path;
        output_steps_csv_path = ui_state->output_steps_csv_path;
        output_frames_csv_path = ui_state->output_frames_csv_path;
        output_frame_image_dir = ui_state->output_frame_image_dir;
        has_result = ui_state->has_result;
        last_result = ui_state->last_result;
        last_fov_calibration = ui_state->last_fov_calibration;
        last_camera_serial = ui_state->last_camera_serial;
        last_focus = ui_state->last_focus;
        last_exposure = ui_state->last_exposure;
    }

    ImGui::Separator();
    ImGui::TextWrapped("%s", status_message.c_str());
    if (!error_message.empty()) {
        ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.35f, 1.0f), "%s", error_message.c_str());
    }

    if (has_result) {
        ImGui::Text("Last run: camera=%s focus=%u exposure=%u", last_camera_serial.c_str(), last_focus, last_exposure);
        ImGui::TextWrapped("Artifact ID: %s", output_artifact_id.c_str());
        if (ImGui::SmallButton("Copy Artifact ID")) {
            ImGui::SetClipboardText(output_artifact_id.c_str());
        }
        ImGui::TextWrapped("Fingerprint: %s", output_fingerprint.c_str());
        if (ImGui::SmallButton("Copy Fingerprint")) {
            ImGui::SetClipboardText(output_fingerprint.c_str());
        }
        ImGui::TextWrapped("Artifact dir: %s", output_artifact_dir.c_str());
        ImGui::TextWrapped("Manifest: %s", output_manifest_path.c_str());
        if (ImGui::SmallButton("Copy Manifest Path")) {
            ImGui::SetClipboardText(output_manifest_path.c_str());
        }
        ImGui::TextWrapped("Measurement JSON: %s", output_json_path.c_str());
        ImGui::TextWrapped("Steps CSV: %s", output_steps_csv_path.c_str());
        ImGui::TextWrapped("Frames CSV: %s", output_frames_csv_path.c_str());
        if (!output_frame_image_dir.empty()) {
            ImGui::TextWrapped("Representative frames: %s", output_frame_image_dir.c_str());
        }
        if (last_fov_calibration.enabled) {
            ImGui::Text("FOV metadata: working_distance=%.1f mm pixel_pitch=%.3f um",
                        last_fov_calibration.working_distance_mm,
                        last_fov_calibration.pixel_pitch_um);
            if (last_fov_calibration.has_field_width_mm || last_fov_calibration.has_field_height_mm) {
                if (last_fov_calibration.has_field_width_mm && last_fov_calibration.has_field_height_mm) {
                    ImGui::Text("Field size: %.3f mm x %.3f mm",
                                last_fov_calibration.field_width_mm,
                                last_fov_calibration.field_height_mm);
                } else if (last_fov_calibration.has_field_width_mm) {
                    ImGui::Text("Field width: %.3f mm", last_fov_calibration.field_width_mm);
                } else {
                    ImGui::Text("Field height: %.3f mm", last_fov_calibration.field_height_mm);
                }
            }
            if (last_fov_calibration.has_mean_magnification) {
                ImGui::Text("Mean magnification: %.4f", last_fov_calibration.mean_magnification);
            }
            if (last_fov_calibration.has_effective_reference_f_number) {
                ImGui::Text("Approx effective reference f-number: %.3f",
                            last_fov_calibration.effective_reference_f_number);
            }
        }
        if (last_result.has_saturation_boundary) {
            ImGui::Text("Saturation-limited through iris %u", last_result.saturation_limited_through_iris);
        }
        if (last_result.has_usable_window) {
            ImGui::Text("Usable iris range: [%u, %u]", last_result.usable_iris_min, last_result.usable_iris_max);
        }
        if (last_result.has_dim_boundary) {
            ImGui::Text("Too dim from iris %u onward", last_result.dim_limited_from_iris);
        }
        for (const std::string& warning : last_result.warnings) {
            ImGui::TextWrapped("Warning: %s", warning.c_str());
        }

        std::vector<double> plot_iris;
        std::vector<double> plot_step_mean;
        std::vector<double> plot_frame_iris;
        std::vector<double> plot_frame_mean;
        plot_iris.reserve(last_result.steps.size());
        plot_step_mean.reserve(last_result.steps.size());
        size_t total_samples = 0;
        for (const ApertureStepResult& step : last_result.steps) {
            total_samples += step.samples.size();
        }
        plot_frame_iris.reserve(total_samples);
        plot_frame_mean.reserve(total_samples);
        for (const ApertureStepResult& step : last_result.steps) {
            plot_iris.push_back(static_cast<double>(step.iris));
            plot_step_mean.push_back(step.summary.mean);
            for (const ApertureFrameSample& sample : step.samples) {
                plot_frame_iris.push_back(static_cast<double>(step.iris));
                plot_frame_mean.push_back(sample.stats.mean);
            }
        }

        if (!plot_iris.empty() && ImPlot::BeginPlot("Aperture Mean Intensity", ImVec2(-1.0f, 260.0f))) {
            ImPlot::SetupAxes("Iris", "Mean Intensity");
            ImPlot::SetupAxisLimits(ImAxis_X1,
                                    plot_iris.front() - 0.5,
                                    plot_iris.back() + 0.5,
                                    ImGuiCond_Always);
            ImPlot::SetupAxisLimits(ImAxis_Y1, 0.0, 255.0, ImGuiCond_Always);

            if (!plot_frame_iris.empty()) {
                ImPlot::SetNextMarkerStyle(ImPlotMarker_Circle, 3.0f, ImVec4(0.75f, 0.75f, 0.75f, 0.55f));
                ImPlot::PlotScatter("Frame mean", plot_frame_iris.data(), plot_frame_mean.data(),
                                    static_cast<int>(plot_frame_iris.size()));
            }

            ImPlot::SetNextLineStyle(ImVec4(0.15f, 0.7f, 0.3f, 1.0f), 2.0f);
            ImPlot::SetNextMarkerStyle(ImPlotMarker_Square, 4.0f, ImVec4(0.15f, 0.7f, 0.3f, 1.0f));
            ImPlot::PlotLine("Step mean", plot_iris.data(), plot_step_mean.data(),
                             static_cast<int>(plot_iris.size()));
            ImPlot::EndPlot();
        }

        if (!last_result.steps.empty()) {
            ui_state->selected_heatmap_step =
                std::clamp(ui_state->selected_heatmap_step, 0, static_cast<int>(last_result.steps.size()) - 1);

            std::vector<std::string> heatmap_labels_storage;
            std::vector<const char*> heatmap_labels;
            heatmap_labels_storage.reserve(last_result.steps.size());
            heatmap_labels.reserve(last_result.steps.size());
            for (const ApertureStepResult& step : last_result.steps) {
                std::ostringstream label;
                label << "iris " << step.iris << " (" << aperture_classification_to_string(step.classification) << ")";
                heatmap_labels_storage.push_back(label.str());
            }
            for (const std::string& label : heatmap_labels_storage) {
                heatmap_labels.push_back(label.c_str());
            }

            ImGui::Separator();
            ImGui::Combo("Heatmap step", &ui_state->selected_heatmap_step, heatmap_labels.data(),
                         static_cast<int>(heatmap_labels.size()));

            const ApertureStepResult& heatmap_step =
                last_result.steps[static_cast<size_t>(ui_state->selected_heatmap_step)];
            if (heatmap_step.has_grid &&
                !heatmap_step.grid.tile_relative_mean.empty() &&
                heatmap_step.grid.rows > 0 &&
                heatmap_step.grid.cols > 0) {
                ImGui::Text("Grid uniformity at iris %u: min=%.3fx max=%.3fx cv=%.4f",
                            heatmap_step.iris,
                            heatmap_step.grid.min_relative_mean,
                            heatmap_step.grid.max_relative_mean,
                            heatmap_step.grid.cv_relative_mean);
                if (ensure_aperture_preview_texture(ui_state, heatmap_step.representative_frame_path)) {
                    double scale_min = heatmap_step.grid.min_relative_mean;
                    double scale_max = heatmap_step.grid.max_relative_mean;
                    if (scale_max <= scale_min) {
                        scale_max = scale_min + 1e-6;
                    }
                    const double image_width = static_cast<double>(ui_state->preview_texture_width);
                    const double image_height = static_cast<double>(ui_state->preview_texture_height);
                    if (ImGui::Button("Fit representative view")) {
                        ui_state->representative_canvas_view.fit_requested = true;
                    }
                    if (orange::ui::begin_image_canvas("Representative Frame Overlay",
                                                       ui_state->preview_texture,
                                                       ui_state->preview_texture_width,
                                                       ui_state->preview_texture_height,
                                                       &ui_state->representative_canvas_view,
                                                       0.60f,
                                                       "##aperture_preview")) {
                        ImPlot::PushPlotClipRect();
                        ImDrawList* draw_list = ImPlot::GetPlotDrawList();
                        for (unsigned int row = 0; row < heatmap_step.grid.rows; ++row) {
                            const double y0 = (static_cast<double>(row) * image_height) / heatmap_step.grid.rows;
                            const double y1 = (static_cast<double>(row + 1) * image_height) / heatmap_step.grid.rows;
                            for (unsigned int col = 0; col < heatmap_step.grid.cols; ++col) {
                                const double x0 = (static_cast<double>(col) * image_width) / heatmap_step.grid.cols;
                                const double x1 = (static_cast<double>(col + 1) * image_width) / heatmap_step.grid.cols;
                                const size_t tile_index = static_cast<size_t>(row) * heatmap_step.grid.cols + col;
                                const double value = heatmap_step.grid.tile_relative_mean[tile_index];
                                const float t = static_cast<float>(std::clamp((value - scale_min) / (scale_max - scale_min), 0.0, 1.0));
                                ImVec4 color = ImPlot::SampleColormap(t, ImPlotColormap_Viridis);
                                color.w = 0.35f;
                                const ImVec2 p0 = ImPlot::PlotToPixels(x0, y0);
                                const ImVec2 p1 = ImPlot::PlotToPixels(x1, y1);
                                draw_list->AddRectFilled(p0, p1, ImGui::ColorConvertFloat4ToU32(color));
                                draw_list->AddRect(p0, p1, IM_COL32(255, 255, 255, 70));
                            }
                        }
                        ImPlot::PopPlotClipRect();
                        ImPlot::EndPlot();
                    }
                    ImGui::TextDisabled("Wheel zooms. Drag to pan. Use Fit representative view to reset.");
                    ImPlot::ColormapScale("Relative mean scale",
                                          scale_min,
                                          scale_max,
                                          ImVec2(60.0f, 200.0f),
                                          "%.3f",
                                          ImPlotColormapScaleFlags_None,
                                          ImPlotColormap_Viridis);
                } else if (!ui_state->preview_texture_error.empty()) {
                    ImGui::TextDisabled("%s", ui_state->preview_texture_error.c_str());
                }
            } else {
                ImGui::TextDisabled("Grid heatmap unavailable for the selected step.");
            }
        }

        if (ImGui::BeginTable("ApertureCharacterizationResults", 10,
                              ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchSame)) {
            ImGui::TableSetupColumn("Iris");
            ImGui::TableSetupColumn("RB Set");
            ImGui::TableSetupColumn("RB End");
            ImGui::TableSetupColumn("Class");
            ImGui::TableSetupColumn("Mean");
            ImGui::TableSetupColumn("P95");
            ImGui::TableSetupColumn("White%");
            ImGui::TableSetupColumn("dEV");
            ImGui::TableSetupColumn("f-num");
            ImGui::TableSetupColumn("eff f-num");
            ImGui::TableHeadersRow();
            for (size_t step_index = 0; step_index < last_result.steps.size(); ++step_index) {
                const ApertureStepResult& step = last_result.steps[step_index];
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                const std::string iris_label = std::to_string(step.iris) + "##aperture_step_" + std::to_string(step_index);
                if (ImGui::Selectable(iris_label.c_str(), ui_state->selected_heatmap_step == static_cast<int>(step_index))) {
                    ui_state->selected_heatmap_step = static_cast<int>(step_index);
                }
                ImGui::TableNextColumn();
                if (step.has_iris_readback_after_set) {
                    ImGui::Text("%u%s",
                                step.iris_readback_after_set,
                                step.iris_verified_after_set ? "" : " !");
                } else {
                    ImGui::TextUnformatted("-");
                }
                ImGui::TableNextColumn();
                if (step.has_iris_readback_after_capture) {
                    ImGui::Text("%u%s",
                                step.iris_readback_after_capture,
                                step.iris_verified_after_capture ? "" : " !");
                } else {
                    ImGui::TextUnformatted("-");
                }
                ImGui::TableNextColumn(); ImGui::Text("%s", aperture_classification_to_string(step.classification));
                ImGui::TableNextColumn(); ImGui::Text("%.2f", step.summary.mean);
                ImGui::TableNextColumn(); ImGui::Text("%.1f", step.summary.p95);
                ImGui::TableNextColumn(); ImGui::Text("%.3f", step.summary.white_fraction);
                ImGui::TableNextColumn(); ImGui::Text("%.2f", step.delta_ev);
                ImGui::TableNextColumn();
                if (step.has_estimated_f_number) {
                    ImGui::Text("%.2f", step.estimated_f_number);
                } else {
                    ImGui::TextUnformatted("-");
                }
                ImGui::TableNextColumn();
                if (step.has_estimated_effective_f_number) {
                    ImGui::Text("%.2f", step.estimated_effective_f_number);
                } else {
                    ImGui::TextUnformatted("-");
                }
            }
            ImGui::EndTable();
        }
    }

    ImGui::End();
}

}  // namespace


nlohmann::json build_gui_detect_model_snapshot(const CameraParams& camera_params,
                                               const CameraEachSelect& camera_select,
                                               const std::string& selected_yolo_model)
{
    const bool enabled = camera_select.yolo;
    std::string selected_engine_path = selected_yolo_model;
    if (enabled && camera_select.yolo_model && camera_select.yolo_model[0] != '\0') {
        selected_engine_path = camera_select.yolo_model;
    }
    const std::string engine_path = enabled ? selected_engine_path : "";
    return {
        {"enabled", enabled},
        {"source", {
            {"ui_selected", camera_select.yolo},
            {"camera_config_path", camera_params.config_path}
        }},
        {"runtime", {
            {"worker", "YoloWorker"},
            {"backend", enabled ? "tensorrt" : "none"},
            {"engine_path", engine_path},
            {"model_id", enabled ? build_model_id_from_path(engine_path) : "none"},
            {"gpu_id", camera_params.gpu_id}
        }}
    };
}

void update_gui_detect_model_snapshots(const std::string& recording_folder,
                                       const CameraParams* cameras_params,
                                       const CameraEachSelect* cameras_select,
                                       const int num_cameras,
                                       const std::string& selected_yolo_model)
{
    if (recording_folder.empty() || !cameras_params || !cameras_select || num_cameras <= 0) {
        return;
    }

    for (int i = 0; i < num_cameras; ++i) {
        std::string camera_key = cameras_params[i].camera_serial;
        if (camera_key.empty()) {
            camera_key = std::to_string(cameras_params[i].camera_id);
        }
        if (!update_recording_snapshot_model(
                recording_folder,
                camera_key,
                "detect",
                build_gui_detect_model_snapshot(
                    cameras_params[i],
                    cameras_select[i],
                    selected_yolo_model))) {
            std::cerr << "Failed to update recording snapshot detect model metadata for camera "
                      << camera_key << std::endl;
        }
    }
}

nlohmann::json build_gui_crop_output_snapshot(const CameraParams& camera_params,
                                              const CameraEachSelect& camera_select,
                                              const int crop_size_px)
{
    const bool enabled = camera_select.crop_and_encode;
    const int resolved_crop_size = CropAndEncodeWorker::SanitizeCropSize(crop_size_px);
    const std::string camera_serial = camera_params.camera_serial;

    nlohmann::json files = nlohmann::json::object();
    if (enabled && !camera_serial.empty()) {
        const std::string prefix = "Cam" + camera_serial + "_crop";
        files = {
            {"video", prefix + ".mp4"},
            {"metadata", prefix + "_meta.csv"},
            {"keyframes", prefix + "_keyframe.json"},
            {"perf", prefix + "_perf.csv"},
            {"sidecar_perf", prefix + "_sidecar_perf.csv"}
        };
    }

    return {
        {"schema_version", 1},
        {"enabled", enabled},
        {"mode", enabled ? "yolo_centered_square" : "disabled"},
        {"source", {
            {"ui_selected", enabled},
            {"requires_yolo", true},
            {"requires_recording", true},
            {"camera_config_path", camera_params.config_path}
        }},
        {"runtime", {
            {"worker", "CropAndEncodeWorker"},
            {"source_gpu_id", camera_params.gpu_id},
            {"crop_size_px", resolved_crop_size},
            {"crop_frame_pool_size", resolve_gui_crop_frame_pool_size()},
            {"preview_max_fps", resolve_effective_crop_preview_max_fps(camera_params)},
            {"width", enabled ? resolved_crop_size : 0},
            {"height", enabled ? resolved_crop_size : 0},
            {"coordinate_space", "full_frame_pixels"},
            {"selection_policy", "largest_detection_by_confidence"},
            {"blank_frame_policy", "encode_black_frame_when_no_detection"},
            {"codec", enabled ? "hevc" : "none"},
            {"container", enabled ? "mp4" : "none"},
            {"tuning", enabled ? "lossless" : "none"},
            {"frame_rate", camera_params.frame_rate},
            {"files", files}
        }}
    };
}

void update_gui_crop_output_snapshots(const std::string& recording_folder,
                                      const CameraParams* cameras_params,
                                      const CameraEachSelect* cameras_select,
                                      const int num_cameras,
                                      const int crop_size_px)
{
    if (recording_folder.empty() || !cameras_params || !cameras_select || num_cameras <= 0) {
        return;
    }

    for (int i = 0; i < num_cameras; ++i) {
        std::string camera_key = cameras_params[i].camera_serial;
        if (camera_key.empty()) {
            camera_key = std::to_string(cameras_params[i].camera_id);
        }
        if (!update_recording_snapshot_crop_output(
                recording_folder,
                camera_key,
                build_gui_crop_output_snapshot(
                    cameras_params[i],
                    cameras_select[i],
                    crop_size_px))) {
            std::cerr << "Failed to update recording snapshot crop output metadata for camera "
                      << camera_key << std::endl;
        }
    }
}

nlohmann::json build_gui_pose_model_snapshot(const CameraParams& camera_params,
                                             const CameraEachSelect& camera_select)
{
    const bool enabled = camera_select.pose;
    const std::string camera_serial = camera_params.camera_serial;

    nlohmann::json files = nlohmann::json::object();
    if (enabled && !camera_serial.empty()) {
        files = {
            {"perf", "Cam" + camera_serial + "_pose_perf.csv"},
            {"events", "Cam" + camera_serial + "_pose_events.jsonl"}
        };
    }
    std::string pose_engine_path;
    if (const char* env_pose_engine_path = std::getenv("ORANGE_POSE_ENGINE_PATH")) {
        pose_engine_path = env_pose_engine_path;
    }
    std::string pose_skeleton_id = "unknown";
    if (const char* env_pose_skeleton_id = std::getenv("ORANGE_POSE_SKELETON_ID")) {
        pose_skeleton_id = env_pose_skeleton_id;
    }
    std::string pose_skeleton_path;
    if (const char* env_pose_skeleton_path = std::getenv("ORANGE_POSE_SKELETON_PATH")) {
        pose_skeleton_path = env_pose_skeleton_path;
    }

    return {
        {"enabled", enabled},
        {"source", {
            {"ui_selected", enabled},
            {"requires_yolo", true},
            {"requires_crop_output", true},
            {"camera_config_path", camera_params.config_path}
        }},
        {"runtime", {
            {"worker", "PoseWorker"},
            {"backend", enabled ? "noop" : "none"},
            {"mode", enabled ? "noop" : "disabled"},
            {"engine_path", enabled ? pose_engine_path : ""},
            {"model_id", enabled && !pose_engine_path.empty() ? build_model_id_from_path(pose_engine_path) : "none"},
            {"skeleton_id", enabled ? pose_skeleton_id : "none"},
            {"skeleton_path", enabled ? pose_skeleton_path : ""},
            {"gpu_id", camera_params.gpu_id},
            {"queue_size", enabled ? 32 : 0},
            {"files", files}
        }}
    };
}

void update_gui_pose_model_snapshots(const std::string& recording_folder,
                                     const CameraParams* cameras_params,
                                     const CameraEachSelect* cameras_select,
                                     const int num_cameras)
{
    if (recording_folder.empty() || !cameras_params || !cameras_select || num_cameras <= 0) {
        return;
    }

    for (int i = 0; i < num_cameras; ++i) {
        std::string camera_key = cameras_params[i].camera_serial;
        if (camera_key.empty()) {
            camera_key = std::to_string(cameras_params[i].camera_id);
        }
        if (!update_recording_snapshot_model(
                recording_folder,
                camera_key,
                "pose",
                build_gui_pose_model_snapshot(cameras_params[i], cameras_select[i]))) {
            std::cerr << "Failed to update recording snapshot pose model metadata for camera "
                      << camera_key << std::endl;
        }
    }
}

std::string spatial_calibration_artifact_env_name(const std::string& camera_serial)
{
    return "ORANGE_SPATIAL_CALIBRATION_ARTIFACT_" + camera_serial;
}

std::string resolve_gui_spatial_calibration_artifact_path(const std::string& camera_serial)
{
    if (camera_serial.empty()) {
        return {};
    }
    const std::string env_name = spatial_calibration_artifact_env_name(camera_serial);
    const char* value = std::getenv(env_name.c_str());
    if (!value || value[0] == '\0') {
        return {};
    }
    return value;
}

void update_gui_spatial_calibration_snapshots(const std::string& recording_folder,
                                              const CameraParams* cameras_params,
                                              const CameraEachSelect* cameras_select,
                                              const int num_cameras)
{
    if (recording_folder.empty() || !cameras_params || !cameras_select || num_cameras <= 0) {
        return;
    }

    for (int i = 0; i < num_cameras; ++i) {
        if (!gui_camera_has_acquisition_work(cameras_select[i])) {
            continue;
        }

        std::string camera_key = cameras_params[i].camera_serial;
        if (camera_key.empty()) {
            camera_key = std::to_string(cameras_params[i].camera_id);
        }

        const std::string artifact_path = resolve_gui_spatial_calibration_artifact_path(camera_key);
        if (artifact_path.empty()) {
            continue;
        }

        std::string error;
        if (!update_recording_snapshot_spatial_calibration_from_artifact(
                recording_folder,
                camera_key,
                artifact_path,
                &error)) {
            std::cerr << "Failed to update recording snapshot spatial calibration for camera "
                      << camera_key << " from " << artifact_path;
            if (!error.empty()) {
                std::cerr << ": " << error;
            }
            std::cerr << std::endl;
            continue;
        }

        std::cout << "Recording snapshot spatial calibration for camera "
                  << camera_key << " loaded from " << artifact_path << std::endl;
    }
}

bool gui_request_recording_start_through_operator_path(
    orange::session::RecordingSessionState* recording_session,
    CameraControl* camera_control,
    GuiRecordingRunState* recording_run,
    GuiSessionTimingState* timing,
    orange::gui::GuiDisplayFrameRateStats* display_frame_rate_stats,
    GuiLocalControlStopSchedulerState* stop_scheduler,
    CameraParams* cameras_params,
    CameraEachSelect* cameras_select,
    const int num_cameras,
    const std::string& yolo_model,
    const int crop_size_px,
    EncoderConfig* encoder_config,
    const std::string& input_folder,
    PTPParams* ptp_params,
    CropProducerWorker** crop_producer_workers,
    CropPreviewWorker** crop_preview_workers,
    CropAndEncodeWorker** crop_and_encode_workers,
    PoseWorker** pose_workers,
    std::vector<std::string>* recording_preflight_errors,
    const std::string& context)
{
    if (!recording_session || !camera_control || !recording_run || !timing) {
        if (recording_preflight_errors) {
            *recording_preflight_errors = {
                "Orange GUI recording start request is missing runtime state."};
        }
        std::cerr << "[GUI][recording] Start rejected: missing runtime state"
                  << " context=" << context << std::endl;
        return false;
    }

    const RecordingPreflightResult preflight =
        run_gui_recording_preflight(
            cameras_params,
            cameras_select,
            num_cameras,
            yolo_model,
            crop_size_px);
    if (!preflight.ok) {
        if (recording_preflight_errors) {
            *recording_preflight_errors = preflight.errors;
        }
        log_recording_preflight_failure(context.c_str(), preflight);
        return false;
    }

    if (recording_preflight_errors) {
        recording_preflight_errors->clear();
    }
    const std::string output_root =
        encoder_config && !encoder_config->folder_name.empty()
            ? encoder_config->folder_name
            : input_folder;
    const orange::session::RecordingRunStartResult start_result =
        orange::session::begin_recording_run(
            recording_session,
            camera_control,
            cameras_params,
            cameras_select,
            num_cameras,
            output_root,
            ptp_params,
            recording_session->recording_sink_mode,
            &recording_session->external_recorder_contract_config);

    if (!start_result.ok) {
        if (recording_preflight_errors) {
            *recording_preflight_errors = {
                start_result.error_message.empty()
                    ? "Failed to start recording run."
                    : start_result.error_message};
        }
        if (!start_result.error_message.empty()) {
            std::cerr << "[GUI][recording] " << start_result.error_message << std::endl;
        }
        if (!start_result.external_recorder_contract_path.empty()) {
            std::cerr << "[GUI][recording] external recorder contract: "
                      << start_result.external_recorder_contract_path
                      << std::endl;
        }
        return false;
    }

    const std::string& resolved_recording_folder = start_result.recording_folder;
    const std::string& resolved_recording_sink_mode = start_result.recording_sink_mode;
    for (int i = 0; i < num_cameras; ++i) {
        if (crop_producer_workers && crop_producer_workers[i]) {
            crop_producer_workers[i]->RotateRecordingFolder(
                resolved_recording_folder);
        }
        if (crop_preview_workers && crop_preview_workers[i]) {
            crop_preview_workers[i]->ResetRunCounters();
        }
    }
    update_gui_detect_model_snapshots(
        resolved_recording_folder,
        cameras_params,
        cameras_select,
        num_cameras,
        yolo_model);
    update_gui_crop_output_snapshots(
        resolved_recording_folder,
        cameras_params,
        cameras_select,
        num_cameras,
        crop_size_px);
    update_gui_pose_model_snapshots(
        resolved_recording_folder,
        cameras_params,
        cameras_select,
        num_cameras);
    update_gui_spatial_calibration_snapshots(
        resolved_recording_folder,
        cameras_params,
        cameras_select,
        num_cameras);
    for (int i = 0; i < num_cameras; ++i) {
        if (crop_and_encode_workers && crop_and_encode_workers[i]) {
            crop_and_encode_workers[i]->RotateRecordingFolder(
                resolved_recording_folder);
        }
        if (pose_workers && pose_workers[i]) {
            pose_workers[i]->RotateRecordingFolder(
                resolved_recording_folder);
        }
    }

    try_start_timer();
    gui_mark_recording_started(timing);
    if (display_frame_rate_stats) {
        display_frame_rate_stats->Reset();
    }
    orange_imgui_glfw_reset_size_cache_stats();
    gui_note_recording_started(
        recording_run,
        camera_control,
        resolved_recording_folder,
        resolved_recording_sink_mode);
    gui_reset_local_control_stop_scheduler_for_recording_start(stop_scheduler);
    std::cout << "[GUI][recording] Recording started"
              << " context=" << context
              << " sink_mode=" << resolved_recording_sink_mode
              << std::endl;
    if (resolved_recording_sink_mode != "real") {
        std::cout << "Full-frame video disabled by GUI recording sink mode: "
                  << resolved_recording_sink_mode << std::endl;
    }
    if (!resolved_recording_folder.empty()) {
        std::cout << "Recording folder: " << resolved_recording_folder << std::endl;
    }
    return true;
}

bool gui_poll_local_control_start_request(
    GuiLocalControlStartRequestState* start_request,
    orange::session::RecordingSessionState* recording_session,
    CameraControl* camera_control,
    GuiRecordingRunState* recording_run,
    GuiSessionTimingState* timing,
    orange::gui::GuiDisplayFrameRateStats* display_frame_rate_stats,
    GuiLocalControlStopSchedulerState* stop_scheduler,
    CameraParams* cameras_params,
    CameraEachSelect* cameras_select,
    const int num_cameras,
    const std::string& yolo_model,
    const int crop_size_px,
    EncoderConfig* encoder_config,
    const std::string& input_folder,
    PTPParams* ptp_params,
    CropProducerWorker** crop_producer_workers,
    CropPreviewWorker** crop_preview_workers,
    CropAndEncodeWorker** crop_and_encode_workers,
    PoseWorker** pose_workers,
    std::vector<std::string>* recording_preflight_errors,
    const std::string& event_log_path)
{
    if (!start_request || !start_request->pending) {
        return false;
    }

    const std::string request_id = start_request->request_id;
    const std::string operation_id = start_request->operation_id;
    start_request->pending = false;

    auto reject_start = [&](const std::string& event, const std::string& reason) {
        gui_note_local_control_start_event(start_request, event);
        std::cout << "[GUI][local_control] recording start ignored"
                  << " request_id=" << request_id
                  << " operation_id=" << operation_id
                  << " reason=" << reason
                  << std::endl;
        gui_log_local_control_event(
            event_log_path,
            {
                {"event", "recording_start_ignored"},
                {"method", "start_recording"},
                {"request_id", request_id},
                {"operation_id", operation_id},
                {"command_source", start_request->source},
                {"reason", reason},
                {"received_at_utc", start_request->received_at_utc},
            });
    };

    if (!start_request->enabled) {
        reject_start("ignored_disabled", "start_control_disabled");
        return false;
    }
    if (!camera_control || !camera_control->open) {
        reject_start("ignored_cameras_not_open", "cameras_not_open");
        return false;
    }
    if (!camera_control->subscribe) {
        reject_start("ignored_not_streaming", "streaming_inactive");
        return false;
    }
    if (camera_control->record_video) {
        reject_start("ignored_already_recording", "already_recording");
        return false;
    }
    if (camera_control->recording_draining ||
        (recording_run && recording_run->finalizing)) {
        reject_start("ignored_recording_finalizing", "recording_finalizing");
        return false;
    }

    const bool started = gui_request_recording_start_through_operator_path(
        recording_session,
        camera_control,
        recording_run,
        timing,
        display_frame_rate_stats,
        stop_scheduler,
        cameras_params,
        cameras_select,
        num_cameras,
        yolo_model,
        crop_size_px,
        encoder_config,
        input_folder,
        ptp_params,
        crop_producer_workers,
        crop_preview_workers,
        crop_and_encode_workers,
        pose_workers,
        recording_preflight_errors,
        "local_control_start_recording");
    gui_note_local_control_start_event(
        start_request,
        started ? "start_triggered" : "start_failed");
    std::cout << "[GUI][local_control] recording start "
              << (started ? "triggered" : "failed")
              << " request_id=" << request_id
              << " operation_id=" << operation_id
              << std::endl;
    gui_log_local_control_event(
        event_log_path,
        {
            {"event", started ? "recording_start_triggered" : "recording_start_failed"},
            {"method", "start_recording"},
            {"request_id", request_id},
            {"operation_id", operation_id},
            {"command_source", start_request->source},
            {"reason", start_request->reason},
            {"received_at_utc", start_request->received_at_utc},
        });
    return started;
}

void RenderSpeedGraph(int camera_id, YoloWorker* yolo_worker, SpeedTrackingData& speed_data) {
    if (!yolo_worker) return;
    
    // Get current tracked objects from YOLO worker
    auto tracked_objects = yolo_worker->getTrackedObjects();
    
    // Update speed data WITH CM/S SPEEDS
    static float app_time = 0.0f;
    app_time += ImGui::GetIO().DeltaTime;
    
    for (const auto& obj : tracked_objects) {
        // USE CALIBRATED SPEED IN CM/S
        speed_data.AddSpeedData(obj.track_id, obj.current_speed_physical_units, app_time);
    }
    
    // Clean up old tracks
    speed_data.ClearOldTracks(app_time);
    
    // Render the speed graph
    ImGui::Separator();
    ImGui::Text("Object Speed Tracking");
    
    if (ImGui::CollapsingHeader("Speed Graph", ImGuiTreeNodeFlags_DefaultOpen)) {
        static float history = 10.0f;
        ImGui::SliderFloat("Time Window", &history, 2.0f, 30.0f, "%.1f s");
        
        // Make graph size responsive
        ImVec2 available = ImGui::GetContentRegionAvail();
        available.y -= 80;
        ImVec2 graph_size = ImVec2(-1, std::max(250.0f, available.y));
        
        // CHANGE PLOT TITLE AND Y-AXIS LABEL
        if (ImPlot::BeginPlot("Speed (cm/sec)", graph_size)) {
            // Setup axes
            ImPlot::SetupAxes("Time (s)", "Speed (cm/s)");
            ImPlot::SetupAxisLimits(ImAxis_X1, app_time - history, app_time, ImGuiCond_Always);
            ImPlot::SetupAxisLimits(ImAxis_Y1, 0, speed_data.max_speed_seen);
            
            // Plot each track
            for (const auto& [track_id, buffer] : speed_data.track_buffers) {
                if (buffer.Data.size() > 1) {
                    ImPlot::SetNextLineStyle(speed_data.track_colors.at(track_id), 2.0f);
                    std::string label = "Track " + std::to_string(track_id);
                    ImPlot::PlotLine(label.c_str(), 
                                   &buffer.Data[0].x, &buffer.Data[0].y, 
                                   buffer.Data.size(), 0, buffer.Offset, 
                                   2 * sizeof(float));
                }
            }
            
            ImPlot::EndPlot();
        }
        
        // CHANGE CURRENT SPEEDS DISPLAY TO CM/S
        if (!tracked_objects.empty()) {
            ImGui::Text("Current Speeds:");
            for (const auto& obj : tracked_objects) {
                ImVec4 color = speed_data.track_colors[obj.track_id];
                ImGui::TextColored(color, "Track %d: %.2f cm/s", 
                                 obj.track_id, obj.current_speed_physical_units);
            }
        } else {
            ImGui::TextDisabled("No objects being tracked");
        }
    }
}


simplelogger::Logger *logger = simplelogger::LoggerFactory::CreateConsoleLogger();

#define display_gpu_id 0

int main(int argc, char **args) {

    // Initialize the YOLOv8 plugins
    YOLOv8::initialize_plugins();

    std::filesystem::path cwd = std::filesystem::current_path();
    std::string delimiter = "/";
    std::vector<std::string> tokenized_path = string_split(cwd, delimiter);
    std::string orange_root_dir_str = "/home/" + tokenized_path[2] + "/orange_data";
    prepare_application_folders(orange_root_dir_str);
    AppStorageConfig app_storage_config;
    std::string app_storage_config_error;
    if (!load_app_storage_config(orange_root_dir_str, &app_storage_config, &app_storage_config_error)) {
        std::cerr << "App storage config warning: " << app_storage_config_error << std::endl;
    }
    if (app_storage_config.gui_ptp_register_read_decimate > 1 &&
        std::getenv("ORANGE_PTP_REGISTER_READ_DECIMATE") == nullptr) {
        const std::string decimate_value =
            std::to_string(app_storage_config.gui_ptp_register_read_decimate);
        setenv("ORANGE_PTP_REGISTER_READ_DECIMATE", decimate_value.c_str(), 1);
        std::cout << "[GUI][PTP] PTP register-read decimation from app config: 1/"
                  << app_storage_config.gui_ptp_register_read_decimate
                  << std::endl;
    }
    set_gui_env_from_app_config_if_absent(
        "ORANGE_CROP_RECORDING_SINK_MODE",
        app_storage_config.gui_crop_recording_sink_mode,
        app_storage_config.gui_crop_recording_sink_mode == "external_ipc"
            ? "crop recording sink mode"
            : "");
    if (app_storage_config.gui_crop_external_encode_queue_depth > 0) {
        set_gui_env_from_app_config_if_absent(
            "ORANGE_CROP_EXTERNAL_ENCODE_QUEUE_DEPTH",
            std::to_string(app_storage_config.gui_crop_external_encode_queue_depth),
            "crop external encode queue depth");
    }
    if (app_storage_config.gui_crop_frame_pool_size > 0) {
        set_gui_env_from_app_config_if_absent(
            "ORANGE_CROP_FRAME_POOL_SIZE",
            std::to_string(app_storage_config.gui_crop_frame_pool_size),
            "crop frame pool size");
    }
    if (app_storage_config.gui_crop_external_recorder_gpu_id >= 0) {
        set_gui_env_from_app_config_if_absent(
            "ORANGE_CROP_EXTERNAL_RECORDER_GPU_ID",
            std::to_string(app_storage_config.gui_crop_external_recorder_gpu_id),
            "crop external recorder GPU");
    }
    for (const auto& [serial, gpu_id] :
         app_storage_config.gui_crop_external_recorder_gpu_ids_by_serial) {
        const std::string env_name =
            "ORANGE_CROP_EXTERNAL_RECORDER_GPU_ID_CAM_" + serial;
        const std::string label =
            "crop external recorder GPU for Cam" + serial;
        set_gui_env_from_app_config_if_absent(
            env_name.c_str(),
            std::to_string(gpu_id),
            label.c_str());
    }

    const u32 gui_swap_interval_default =
        app_storage_config.gui_swap_interval >= 0
            ? static_cast<u32>(app_storage_config.gui_swap_interval)
            : 1;
    const u32 gui_frame_max_fps_default =
        app_storage_config.gui_frame_max_fps >= 0
            ? static_cast<u32>(app_storage_config.gui_frame_max_fps)
            : 0;

    gx_context *window = (gx_context *) malloc(sizeof(gx_context));
    *window = (gx_context){
        .swap_interval = gx_resolve_swap_interval(gui_swap_interval_default),
        .frame_max_fps = gx_resolve_frame_max_fps(gui_frame_max_fps_default),
        .width = 1920,
        .height = 1080,
        .window_width = 0,
        .window_height = 0,
        .framebuffer_width = 0,
        .framebuffer_height = 0,
        .render_target_title = (char *) "Orange",
        .glsl_version = (char *) malloc(100)
    };

    render_initialize_target(window);

    int max_cameras = 20;
    int cam_count;
    GigEVisionDeviceInfo unsorted_device_info[max_cameras];
    cam_count = scan_cameras(max_cameras, unsorted_device_info);
    GigEVisionDeviceInfo device_info[max_cameras];
    sort_cameras_ip(unsorted_device_info, device_info, cam_count);

    std::string app_storage_warning;
    std::string input_folder = resolve_default_recording_root(orange_root_dir_str, &app_storage_warning);
    if (!app_storage_warning.empty()) {
        std::cerr << "App storage config warning: " << app_storage_warning << std::endl;
    }

    std::string yolo_model_folder = orange_root_dir_str + "/detect";
    std::string app_model_warning;
    std::string yolo_model = resolve_default_detect_engine(orange_root_dir_str, &app_model_warning);
    if (!app_model_warning.empty()) {
        std::cerr << "App model config warning: " << app_model_warning << std::endl;
    }
    const GuiAutorunConfig gui_autorun_config = resolve_gui_autorun_config();
    if (gui_autorun_config.enabled) {
        std::cout << "[GUI][autorun] enabled"
                  << " config_dir=" << gui_autorun_config.config_dir
                  << " stream_warmup_seconds=" << gui_autorun_config.stream_warmup_seconds
                  << " record_seconds=" << gui_autorun_config.record_seconds
                  << " exit_after_finalize=" << gui_autorun_config.exit_after_finalize
                  << " hide_crop_preview=" << gui_autorun_config.hide_crop_preview
                  << " enable_stream=" << gui_autorun_config.enable_stream
                  << " enable_record=" << gui_autorun_config.enable_record
                  << " enable_yolo=" << gui_autorun_config.enable_yolo
                  << " enable_crop=" << gui_autorun_config.enable_crop
                  << " start_recording=" << gui_autorun_config.start_recording
                  << std::endl;
    }
    
    bool camera_is_selected[cam_count]{0};
    CameraParams *cameras_params = nullptr;
    CameraEachSelect *cameras_select = nullptr;
    CameraEmergent *ecams = nullptr;
    std::vector<std::thread> camera_threads;
    GL_Texture *tex = nullptr;
    GL_Texture* crop_tex = nullptr;
    int num_cameras = 0;
    const int gui_stream_downsample_default =
        app_storage_config.gui_stream_downsample > 0
            ? app_storage_config.gui_stream_downsample
            : 4;
    int stream_downsample = resolve_gui_stream_downsample(gui_stream_downsample_default);
    const int gui_display_preview_max_fps_default =
        app_storage_config.gui_display_preview_max_fps >= 0
            ? app_storage_config.gui_display_preview_max_fps
            : 60;
    int crop_size_px = CropAndEncodeWorker::kDefaultCropSize;
    std::string crop_size_config_status;
    bool crop_size_config_status_warning = false;
    int crop_preview_max_fps = CameraCropPipelineConfig::kDefaultPreviewMaxFps;
    bool show_crop_preview_windows = !gui_autorun_config.hide_crop_preview;
    bool show_yolo_speed_graphs =
        resolve_gui_yolo_speed_graphs_enabled(app_storage_config.gui_show_speed_graphs);
    std::string crop_preview_config_status;
    bool crop_preview_config_status_warning = false;
    CameraControl *camera_control = new CameraControl();

    int evt_buffer_size{100};
    PTPParams *ptp_params = new PTPParams{0, 0, 0, 0, false, false, false, false};
    COpenGLDisplay** openGLDisplayWorkers = nullptr;
    CropProducerWorker** cropProducerWorkers = nullptr;
    CropAndEncodeWorker** cropAndEncodeWorkers = nullptr;
    CropPreviewWorker** cropPreviewWorkers = nullptr;
    SpatialSnapshotWorker** spatialSnapshotWorkers = nullptr;
    std::vector<uint64_t> display_uploaded_serials;
    std::vector<uint64_t> crop_preview_uploaded_serials;
    std::vector<GLuint> live_preview_texture_ids;
    PoseWorker** poseWorkers = nullptr;
    ImageWriterWorker* image_writer = new ImageWriterWorker("ImageSaverThread");
    image_writer->StartThread();

    std::vector<CameraResources> camera_resources;
    orange::session::RecordingSessionState recording_session;
    GuiSessionTimingState gui_session_timing;
    GuiRecordingRunState gui_recording_run;
    GuiLocalControlStartRequestState gui_local_control_start_request;
    gui_local_control_start_request.enabled =
        gui_local_control_recording_start_enabled(&app_storage_config);
    GuiLocalControlStopSchedulerState gui_local_control_stop_scheduler;
    gui_local_control_stop_scheduler.stop_recording_enabled =
        gui_local_control_stop_recording_enabled(&app_storage_config);
    gui_local_control_stop_scheduler.citrus_completion_enabled =
        gui_local_control_citrus_completion_stop_enabled(&app_storage_config);
    gui_local_control_stop_scheduler.enabled =
        gui_local_control_stop_scheduler.stop_recording_enabled ||
        gui_local_control_stop_scheduler.citrus_completion_enabled;
    gui_local_control_stop_scheduler.drain_timeout_seconds =
        gui_local_control_drain_timeout_seconds(&app_storage_config);
    const bool gui_local_control_exit_after_finalize =
        gui_local_control_exit_after_finalize_enabled(&app_storage_config);
    bool gui_local_control_exit_pending_after_finalize = false;
    bool gui_local_control_exit_stream_stop_requested = false;
    GuiDisplayFrameRateStats gui_display_frame_rate_stats;
    orange::control::LocalControlServer gui_local_control_server;
    std::string gui_local_control_event_log_path;
    if (!gui_local_control_disabled()) {
        orange::control::LocalControlServerOptions control_options;
        control_options.socket_path = gui_local_control_socket_path();
        control_options.event_log_path =
            gui_local_control_log_path(control_options.socket_path);
        gui_local_control_event_log_path = control_options.event_log_path;
        control_options.allow_gui_lifecycle_commands = false;
        control_options.allow_gui_start_recording_commands =
            gui_local_control_start_request.enabled;
        control_options.allow_gui_stop_recording_commands =
            gui_local_control_stop_scheduler.stop_recording_enabled;
        control_options.allow_gui_citrus_completion_commands =
            gui_local_control_stop_scheduler.citrus_completion_enabled;
        std::string control_error;
        if (!gui_local_control_server.Start(control_options, &control_error)) {
            std::cerr << "[GUI][local_control] failed to start: "
                      << control_error << std::endl;
        }
    } else {
        std::cout << "[GUI][local_control] disabled by environment" << std::endl;
    }
    std::vector<std::unique_ptr<FrameIPCManager>> frame_ipc_managers;
    std::vector<std::string> frame_ipc_init_errors;
    std::vector<std::string> recording_preflight_errors;
    std::string recording_config_defaults_status;
    bool recording_config_defaults_status_warning = false;

    EncoderConfig *encoder_config = new EncoderConfig{
        "h264",
        "p1",
        "ll",
        "vbr",
        20,
        0,
        -1,
        -1,
        "factor",
        1,
        1024,
        1024,
        ""
    };
    std::vector<std::string> camera_config_files;

    ScrollingBuffer *realtime_plot_data = nullptr;
    bool show_realtime_plot = false;
    bool ptp_stream_sync = false;

    flatbuffers::FlatBufferBuilder *fb_builder = new flatbuffers::FlatBufferBuilder(1024);

    bool enet_runtime_initialized = false;
    bool enet_server_initialized = false;
    EnetContext server;
    if (::enet_initialize() != 0) {
        std::cerr << "[ENet] Global initialization failed; networking disabled." << std::endl;
    } else {
        enet_runtime_initialized = true;
        if (enet_initialize(&server, 3333, 5)) {
            enet_server_initialized = true;
            printf("Server Initiated\n");
        } else {
            std::cerr << "[ENet] Host initialization failed; ENet thread not started." << std::endl;
        }
    }
    ConnectedServer my_servers[2];
    intialize_servers(my_servers);

    INDIGOSignalBuilder indigo_signal_builder{};
    indigo_signal_builder = {
        .builder = fb_builder,
        .server = &server,
        .indigo_connection = nullptr
    };

    std::vector<std::string> network_config_folders;
    std::string network_start_folder_name = orange_root_dir_str + "/config/network";
    for (const auto &entry: std::filesystem::directory_iterator(network_start_folder_name)) {
        network_config_folders.push_back(entry.path().string());
    }
    int network_config_select = 0;

    std::vector<std::string> local_config_folders;
    std::string local_start_folder_name = orange_root_dir_str + "/config/local";
    list_child_directories(local_start_folder_name, local_config_folders);
    std::string picture_save_folder = orange_root_dir_str + "/pictures/" + get_current_date();
    std::string calib_save_folder = orange_root_dir_str + "/exp/calibration/" + get_current_date();
    std::string standalone_calibration_artifact_folder =
        orange_root_dir_str + "/calibrations/standalone_artifacts";
    std::string spatial_calibration_sessions_folder =
        orange_root_dir_str + "/calibrations/sessions";
    std::string aperture_char_output_folder = standalone_calibration_artifact_folder;
    std::string usaf_output_folder = standalone_calibration_artifact_folder;
    orange::gui::HostPtpStackUiState host_ptp_stack_ui;
    ApertureCharacterizationUiState aperture_ui_state;
    copy_string_to_buffer(aperture_ui_state.output_dir, aperture_char_output_folder);
    UsafResolutionUiState usaf_ui_state;
    std::snprintf(usaf_ui_state.output_dir, sizeof(usaf_ui_state.output_dir), "%s", usaf_output_folder.c_str());
    SpatialLayoutUiState spatial_layout_ui_state;

    int local_config_select = 0;
    char new_local_config_folder_name[128] = "";
    std::string local_config_status;
    bool local_config_status_error = false;
    bool select_all_cameras = false;
    char *temp_string = (char *) malloc(64);
    *temp_string = '\0';
    bool save_image_all_ready = true;
    bool quite_enet = false;

    std::thread enet_thread;
    if (enet_server_initialized) {
        enet_thread = std::thread(&create_enet_thread, &server, my_servers, &indigo_signal_builder,
                                  &quite_enet);
    }
    std::vector<std::string> color_temps = { "CT_Off", "CT_2800K", "CT_3000K", "CT_4000K", "CT_5000K", "CT_6500K", "CT_Custom"};
    auto refresh_local_config_folders = [&]() {
        list_child_directories(local_start_folder_name, local_config_folders);
        if (local_config_select > static_cast<int>(local_config_folders.size())) {
            local_config_select = static_cast<int>(local_config_folders.size());
        }
    };
    auto validate_local_config_folder_name = [](const std::string& folder_name,
                                                std::string* error_out) {
        if (error_out) {
            error_out->clear();
        }
        if (folder_name.empty()) {
            if (error_out) {
                *error_out = "Folder name is empty.";
            }
            return false;
        }
        if (folder_name == "." || folder_name == ".." ||
            folder_name.find('/') != std::string::npos ||
            folder_name.find('\\') != std::string::npos) {
            if (error_out) {
                *error_out = "Folder name must not contain path separators or relative path tokens.";
            }
            return false;
        }
        return true;
    };
    auto select_local_config_folder = [&](const std::string& folder_path) {
        refresh_local_config_folders();
        auto it = std::find(local_config_folders.begin(), local_config_folders.end(), folder_path);
        if (it != local_config_folders.end()) {
            local_config_select = static_cast<int>(std::distance(local_config_folders.begin(), it));
        }
    };
    GuiAutorunState gui_autorun_state;
    if (gui_autorun_config.enabled) {
        gui_autorun_enter_stage(&gui_autorun_state, GuiAutorunStage::kSelectConfig);
    }

    auto previous_gui_frame_start = std::chrono::steady_clock::now();
    const auto gui_frame_min_interval =
        window->frame_max_fps > 0
            ? std::chrono::microseconds(1000000 / window->frame_max_fps)
            : std::chrono::microseconds(0);

    while (!glfwWindowShouldClose(window->render_target)) {
        if (gui_frame_min_interval.count() > 0) {
            std::this_thread::sleep_until(previous_gui_frame_start + gui_frame_min_interval);
        }
        const auto gui_frame_start = std::chrono::steady_clock::now();
        previous_gui_frame_start = gui_frame_start;
        GuiFrameTimingSample gui_frame_timing;
        orange::gui::reap_host_ptp_stack_worker(&host_ptp_stack_ui);
        gui_refresh_external_recorder_lifecycles(&recording_session, camera_control);
        gui_poll_local_control_drain_timeout(
            &gui_local_control_stop_scheduler,
            camera_control,
            &gui_recording_run,
            gui_local_control_event_log_path);
        join_aperture_worker_if_finished(&aperture_ui_state);
        join_alignment_worker_if_finished(&aperture_ui_state);
        join_usaf_worker_if_finished(&usaf_ui_state);
        join_usaf_preview_worker_if_finished(&usaf_ui_state);
        const bool aperture_job_running = aperture_ui_state.running.load(std::memory_order_acquire);
        const bool aperture_alignment_running = aperture_ui_state.alignment_running.load(std::memory_order_acquire);
        const bool aperture_tool_busy = aperture_job_running || aperture_alignment_running;
        const bool usaf_job_running = usaf_ui_state.running.load(std::memory_order_acquire);
        const bool usaf_preview_running = usaf_ui_state.preview_running.load(std::memory_order_acquire);
        const bool usaf_tool_busy = usaf_job_running || usaf_preview_running;
        const bool calibration_tool_busy = aperture_tool_busy || usaf_tool_busy;
        GuiAutorunRequests gui_autorun_requests = gui_autorun_update(
            &gui_autorun_state,
            gui_autorun_config,
            local_config_folders,
            &local_config_select,
            camera_control,
            &gui_recording_run,
            calibration_tool_busy);
        gui_request_local_control_forced_finalize_if_needed(
            &gui_local_control_stop_scheduler,
            camera_control,
            &gui_autorun_requests,
            &gui_recording_run,
            gui_local_control_event_log_path);
        if (gui_local_control_exit_pending_after_finalize &&
            !camera_control->record_video &&
            !camera_control->recording_draining) {
            if (camera_control->subscribe) {
                if (!gui_local_control_exit_stream_stop_requested) {
                    gui_autorun_requests.toggle_streaming = true;
                    gui_local_control_exit_stream_stop_requested = true;
                    gui_note_local_control_stop_event(
                        &gui_local_control_stop_scheduler,
                        "finalized_stream_stop_requested");
                    std::cout << "[GUI][local_control] requesting stream stop before GUI exit"
                              << " request_id="
                              << gui_local_control_stop_scheduler.request_id
                              << " operation_id="
                              << gui_local_control_stop_scheduler.operation_id
                              << std::endl;
                    gui_log_local_control_event(
                        gui_local_control_event_log_path,
                        {
                            {"event", "finalized_stream_stop_requested"},
                            {"method", gui_local_control_stop_scheduler.method},
                            {"request_id", gui_local_control_stop_scheduler.request_id},
                            {"operation_id", gui_local_control_stop_scheduler.operation_id},
                            {"command_source", gui_local_control_stop_scheduler.source},
                        });
                }
            } else {
                gui_note_local_control_stop_event(
                    &gui_local_control_stop_scheduler,
                    "finalized_exit_requested");
                std::cout << "[GUI][local_control] requesting GUI exit after stream stop"
                          << " request_id="
                          << gui_local_control_stop_scheduler.request_id
                          << " operation_id="
                          << gui_local_control_stop_scheduler.operation_id
                          << std::endl;
                gui_log_local_control_event(
                    gui_local_control_event_log_path,
                    {
                        {"event", "finalized_exit_requested"},
                        {"method", gui_local_control_stop_scheduler.method},
                        {"request_id", gui_local_control_stop_scheduler.request_id},
                        {"operation_id", gui_local_control_stop_scheduler.operation_id},
                        {"command_source", gui_local_control_stop_scheduler.source},
                    });
                glfwSetWindowShouldClose(window->render_target, GLFW_TRUE);
                gui_local_control_exit_pending_after_finalize = false;
            }
        }
        if (gui_local_control_server.running()) {
            gui_local_control_server.UpdateStatus(
                gui_build_local_control_status(
                    camera_control,
                    cameras_params,
                    cameras_select,
                    num_cameras,
                    recording_session,
                    gui_recording_run,
                    gui_autorun_state,
                    gui_local_control_start_request,
                    gui_local_control_stop_scheduler));
            gui_drain_local_control_commands(
                &gui_local_control_server,
                &gui_local_control_start_request,
                &gui_local_control_stop_scheduler,
                camera_control,
                gui_local_control_event_log_path);
            const bool local_control_start_triggered =
                gui_poll_local_control_start_request(
                    &gui_local_control_start_request,
                    &recording_session,
                    camera_control,
                    &gui_recording_run,
                    &gui_session_timing,
                    &gui_display_frame_rate_stats,
                    &gui_local_control_stop_scheduler,
                    cameras_params,
                    cameras_select,
                    num_cameras,
                    yolo_model,
                    crop_size_px,
                    encoder_config,
                    input_folder,
                    ptp_params,
                    cropProducerWorkers,
                    cropPreviewWorkers,
                    cropAndEncodeWorkers,
                    poseWorkers,
                    &recording_preflight_errors,
                    gui_local_control_event_log_path);
            if (local_control_start_triggered) {
                gui_autorun_requests.toggle_recording = false;
            }
            gui_poll_local_control_stop_scheduler(
                &gui_local_control_stop_scheduler,
                &recording_session,
                camera_control,
                &gui_recording_run,
                &gui_session_timing,
                gui_local_control_event_log_path);
        }
        if (gui_autorun_requests.close_window) {
            glfwSetWindowShouldClose(window->render_target, GLFW_TRUE);
        }
        const auto pre_frame_done = std::chrono::steady_clock::now();
        gui_frame_timing.pre_frame_maintenance_ms = gui_elapsed_ms(
            gui_frame_start,
            pre_frame_done);
        const auto new_frame_start = std::chrono::steady_clock::now();
        create_new_frame();
        gui_frame_timing.imgui_new_frame_ms = gui_elapsed_ms(
            new_frame_start,
            std::chrono::steady_clock::now());
        
        const auto orange_window_draw_start = std::chrono::steady_clock::now();
        if (ImGui::Begin("Orange", nullptr, ImGuiWindowFlags_MenuBar)) {
            ImGui::Text("Application average %.3f ms/frame (%.1f FPS)", 1000.0f / ImGui::GetIO().Framerate,
                       ImGui::GetIO().Framerate);

            if (camera_control->open) {
                // ImGui::BeginDisabled();
            }

            if (ImGui::BeginTable("Cameras", 3,
                                  ImGuiTableFlags_Resizable | ImGuiTableFlags_NoSavedSettings |
                                  ImGuiTableFlags_Borders)) {
                for (int i = 0; i < cam_count; i++) {
                    sprintf(temp_string, "%d", i);
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    ImGui::Selectable(temp_string, &camera_is_selected[i], ImGuiSelectableFlags_SpanAllColumns);
                    ImGui::TableNextColumn();
                    ImGui::Text("%s", device_info[i].serialNumber);
                    ImGui::TableNextColumn();
                    ImGui::Text("%s", device_info[i].currentIp);
                }
                ImGui::EndTable();
            }

            if (ImGui::Button(select_all_cameras ? "Clear all" : "Select all")) {
                select_all_cameras = !select_all_cameras;
                if (select_all_cameras) {
                    for (int i = 0; i < cam_count; i++) {
                        camera_is_selected[i] = true;
                    }
                } else {
                    for (int i = 0; i < cam_count; i++) {
                        camera_is_selected[i] = false;
                    }
                }
            }

            if (camera_control->open) {
                // ImGui::EndDisabled();
            }

            if (camera_control->subscribe) {
                // ImGui::BeginDisabled();
            }

            ImGui::Separator();
            ImGui::Spacing();

            // selection for yolo model
            if (ImGui::Button("Select YOLO")) {
                IGFD::FileDialogConfig config;
                config.countSelectionMax = 1;
                config.path = yolo_model_folder;
                ImGuiFileDialog::Instance()->OpenDialog("ChooseYOLOFile", "Choose File", ".engine", config);
            }
            ImGui::SameLine();
            if (!yolo_model.empty()) {
                ImGui::Text("%s", yolo_model.c_str());
                ImGui::SameLine();
                if (ImGui::Button("Clear YOLO")) {
                    yolo_model.clear();
                }
            } else {
                ImGui::TextDisabled("No YOLO model selected");
            }

            if (camera_control->subscribe) {
                // ImGui::EndDisabled();
            }

            if (camera_control->record_video) {
                // ImGui::BeginDisabled();
            }

            const auto recording_panel_draw_start = std::chrono::steady_clock::now();
            const orange::gui::RecordingPanelActions recording_panel_actions =
                orange::gui::render_recording_config_panel(
                    &input_folder,
                    encoder_config,
                    camera_control->open,
                    camera_control->subscribe,
                    cameras_params,
                    cameras_select,
                    num_cameras,
                    &app_storage_config,
                    &recording_config_defaults_status,
                    recording_config_defaults_status_warning,
                    &recording_preflight_errors);
            gui_frame_timing.recording_panel_draw_ms += gui_elapsed_ms(
                recording_panel_draw_start,
                std::chrono::steady_clock::now());
            if (recording_panel_actions.choose_recording_dir_requested) {
                IGFD::FileDialogConfig config;
                config.countSelectionMax = 1;
                config.path = input_folder;
                ImGuiFileDialog::Instance()->OpenDialog("ChooseRecordingDir", "Choose a Directory", nullptr, config);
            }
            {
                const char *items[] = {"1", "2", "4", "8", "16"};
                static const int item_numbers[] = {1, 2, 4, 8, 16};
                int downsample_current = gui_stream_downsample_index(stream_downsample);
                ImGui::BeginDisabled(camera_control->subscribe);
                if(ImGui::Combo("downsample streaming", &downsample_current, items, IM_ARRAYSIZE(items))) {
                    stream_downsample = item_numbers[downsample_current];
                    for (int i = 0; i < num_cameras; i++) {
                        cameras_select[i].downsample = stream_downsample;
                    }
                }
                ImGui::EndDisabled();
                ImGui::SameLine();
                ImGui::TextDisabled("display only; fixed while streaming");
            }
            int fps_temp = streaming_target_fps.load(); // get the current atomic value

            if (ImGui::InputInt("streaming fps", &fps_temp)) {
                // Clamp if necessary
                if (fps_temp < 1) fps_temp = 1;
                if (fps_temp > 240) fps_temp = 240;
                streaming_target_fps.store(fps_temp); // write it back safely
            }

            ImGui::BeginDisabled(camera_control->subscribe);
            int crop_size_input = crop_size_px;
            if (ImGui::InputInt("crop size px", &crop_size_input, 16, 64)) {
                crop_size_px = CropAndEncodeWorker::SanitizeCropSize(crop_size_input);
                apply_gui_crop_size_to_camera_configs(cameras_params, num_cameras, crop_size_px);
                if (camera_control->open) {
                    crop_size_config_status =
                        "Crop size updated in open camera configs; use Save to config to persist.";
                    crop_size_config_status_warning = false;
                }
            }
            ImGui::EndDisabled();
            ImGui::SameLine();
            ImGui::TextDisabled(
                "square, even, %d-%d px; fixed at stream start",
                CropAndEncodeWorker::kMinCropSize,
                CropAndEncodeWorker::kMaxCropSize);
            if (!crop_size_config_status.empty()) {
                ImGui::TextColored(
                    crop_size_config_status_warning
                        ? ImVec4(0.95f, 0.75f, 0.2f, 1.0f)
                        : ImVec4(0.35f, 0.85f, 0.45f, 1.0f),
                    "%s",
                    crop_size_config_status.c_str());
            }

            ImGui::BeginDisabled(camera_control->subscribe);
            int crop_preview_max_fps_input = crop_preview_max_fps;
            if (ImGui::InputInt("crop preview max fps", &crop_preview_max_fps_input, 1, 15)) {
                crop_preview_max_fps =
                    sanitize_camera_crop_preview_max_fps(crop_preview_max_fps_input);
                apply_gui_crop_preview_max_fps_to_camera_configs(
                    cameras_params,
                    num_cameras,
                    crop_preview_max_fps);
                if (camera_control->open) {
                    crop_preview_config_status =
                        "Crop preview max FPS updated in open camera configs; use Save to config to persist.";
                    crop_preview_config_status_warning = false;
                }
            }
            ImGui::EndDisabled();
            ImGui::SameLine();
            ImGui::TextDisabled(
                "0=unlimited, default %d; fixed at stream start",
                CameraCropPipelineConfig::kDefaultPreviewMaxFps);
            if (!crop_preview_config_status.empty()) {
                ImGui::TextColored(
                    crop_preview_config_status_warning
                        ? ImVec4(0.95f, 0.75f, 0.2f, 1.0f)
                        : ImVec4(0.35f, 0.85f, 0.45f, 1.0f),
                    "%s",
                    crop_preview_config_status.c_str());
            }
            if (ImGui::Checkbox("show crop previews", &show_crop_preview_windows)) {
                if (cropPreviewWorkers) {
                    for (int i = 0; i < num_cameras; ++i) {
                        if (cropPreviewWorkers[i]) {
                            cropPreviewWorkers[i]->SetPreviewDisplayEnabled(
                                show_crop_preview_windows);
                        }
                    }
                }
            }
            ImGui::Checkbox("show YOLO speed graphs", &show_yolo_speed_graphs);
            
   
            if (camera_control->record_video) {
                // ImGui::EndDisabled();
            }

            if (camera_control->open) {
                if (camera_control->record_video) {
                    // ImGui::BeginDisabled();
                }

                ImGui::Checkbox("Show camera temperature", &show_realtime_plot);
                ImGui::SameLine();
                if (ImGui::Button("Aperture Characterization")) {
                    aperture_ui_state.show_window = true;
                    aperture_ui_state.selected_camera = std::clamp(aperture_ui_state.selected_camera, 0, std::max(0, num_cameras - 1));
                }
                ImGui::SameLine();
                if (ImGui::Button("USAF Resolution Calibration")) {
                    usaf_ui_state.show_window = true;
                    usaf_ui_state.selected_camera = std::clamp(usaf_ui_state.selected_camera, 0, std::max(0, num_cameras - 1));
                }
                ImGui::SameLine();
                if (ImGui::Button("Spatial Layout Registration")) {
                    spatial_layout_ui_state.show_window = true;
                    spatial_layout_ui_state.selected_camera =
                        std::clamp(spatial_layout_ui_state.selected_camera, 0, std::max(0, num_cameras - 1));
                }

                if (calibration_tool_busy) {
                    ImGui::BeginDisabled();
                }

                const std::string selected_local_config_folder =
                    (local_config_select >= 0 &&
                     local_config_select < static_cast<int>(local_config_folders.size()))
                        ? local_config_folders[local_config_select]
                        : std::string();
                const auto camera_properties_draw_start = std::chrono::steady_clock::now();
                orange::gui::render_camera_properties_panel(
                    ecams,
                    cameras_params,
                    num_cameras,
                    color_temps,
                    selected_local_config_folder,
                    camera_control->record_video ||
                        camera_control->recording_draining ||
                        gui_recording_run.finalizing);
                gui_frame_timing.camera_properties_draw_ms += gui_elapsed_ms(
                    camera_properties_draw_start,
                    std::chrono::steady_clock::now());

                if (camera_control->record_video) {
                    // ImGui::EndDisabled();
                }

                if (camera_control->subscribe) {
                    // ImGui::BeginDisabled();
                }

                bool stream_all_cameras = true;
                for (int i = 0; i < num_cameras; i++) {
                    if (!cameras_select[i].stream_on) {
                        stream_all_cameras = false;
                        break;
                    }
                }

                bool record_all_cameras = true;
                for (int i = 0; i < num_cameras; i++) {
                    if (!cameras_select[i].record) {
                        record_all_cameras = false;
                        break;
                    }
                }

                bool crop_all_cameras = true;
                for (int i = 0; i < num_cameras; i++) {
                    if (!cameras_select[i].crop_and_encode) {
                        crop_all_cameras = false;
                        break;
                    }
                }

                bool pose_all_cameras = true;
                for (int i = 0; i < num_cameras; i++) {
                    if (!cameras_select[i].pose) {
                        pose_all_cameras = false;
                        break;
                    }
                }

                if (ImGui::BeginTable("Camera Control Setting", 9,
                                      ImGuiTableFlags_Resizable | ImGuiTableFlags_NoSavedSettings |
                                      ImGuiTableFlags_Borders)) {
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    ImGui::Text("Name");
                    ImGui::TableNextColumn();
                    ImGui::Text("Serial");
                    ImGui::TableNextColumn();
                    ImGui::Text("Stream "); ImGui::SameLine();
                    if(ImGui::Checkbox("All##stream", &stream_all_cameras))
                    {
                        if (stream_all_cameras) {
                            for (int i = 0; i < num_cameras; i++) {
                                cameras_select[i].stream_on = true;
                            }
                        } else {
                            for (int i = 0; i < num_cameras; i++) {
                                cameras_select[i].stream_on = false;
                            }
                        }
                    }

                    ImGui::TableNextColumn();
                    ImGui::Text("Record "); ImGui::SameLine();
                    if(ImGui::Checkbox("All##record", &record_all_cameras))
                    {
                        if (record_all_cameras) {
                            for (int i = 0; i < num_cameras; i++) {
                                cameras_select[i].record = true;
                            }
                        } else {
                            for (int i = 0; i < num_cameras; i++) {
                                cameras_select[i].record = false;
                            }
                        }
                    }

                    ImGui::TableNextColumn();
                    ImGui::Text("YOLO "); ImGui::SameLine();

                    ImGui::TableNextColumn();
                    ImGui::Text("Crop "); ImGui::SameLine();
                    ImGui::BeginDisabled(camera_control->subscribe);
                    if(ImGui::Checkbox("All##crop", &crop_all_cameras))
                    {
                        for (int i = 0; i < num_cameras; i++) {
                            cameras_select[i].crop_and_encode = crop_all_cameras;
                        }
                    }
                    ImGui::EndDisabled();

                    ImGui::TableNextColumn();
                    ImGui::Text("Pose "); ImGui::SameLine();
                    ImGui::BeginDisabled(camera_control->subscribe);
                    if(ImGui::Checkbox("All##pose", &pose_all_cameras))
                    {
                        for (int i = 0; i < num_cameras; i++) {
                            cameras_select[i].pose = pose_all_cameras;
                        }
                    }
                    ImGui::EndDisabled();

                    ImGui::TableNextColumn();
                    ImGui::Text("YOLO Debug");

                    ImGui::TableNextColumn();
                    ImGui::Text("YOLO ENet");


                    for (int i = 0; i < num_cameras; i++) {
                        ImGui::TableNextRow();
                        ImGui::TableNextColumn();
                        ImGui::Text("%s", cameras_params[i].camera_name.c_str());
                        ImGui::TableNextColumn();
                        ImGui::Text("%s", cameras_params[i].camera_serial.c_str());
                        ImGui::TableNextColumn();
                        sprintf(temp_string, "##checkbox_stream%d", i);
                        ImGui::Checkbox(temp_string, &cameras_select[i].stream_on);
                        ImGui::TableNextColumn();
                        sprintf(temp_string, "##checkbox_record%d", i);
                        ImGui::Checkbox(temp_string, &cameras_select[i].record);
                        ImGui::TableNextColumn();
                        sprintf(temp_string, "##checkbox_yolo%d", i);
                        ImGui::Checkbox(temp_string, &cameras_select[i].yolo);

                        ImGui::TableNextColumn();
                        sprintf(temp_string, "##checkbox_crop%d", i);
                        ImGui::BeginDisabled(camera_control->subscribe);
                        ImGui::Checkbox(temp_string, &cameras_select[i].crop_and_encode);
                        ImGui::EndDisabled();
                        if (cameras_select[i].crop_and_encode && (!cameras_select[i].yolo || !cameras_select[i].record)) {
                            ImGui::SameLine();
                            ImGui::TextColored(ImVec4(0.95f, 0.35f, 0.25f, 1.0f), "needs rec+yolo");
                        }

                        ImGui::TableNextColumn();
                        sprintf(temp_string, "##checkbox_pose%d", i);
                        ImGui::BeginDisabled(camera_control->subscribe);
                        ImGui::Checkbox(temp_string, &cameras_select[i].pose);
                        ImGui::EndDisabled();
                        if (cameras_select[i].pose && !cameras_select[i].crop_and_encode) {
                            ImGui::SameLine();
                            ImGui::TextColored(ImVec4(0.95f, 0.35f, 0.25f, 1.0f), "needs crop");
                        }

                        ImGui::TableNextColumn();
                        if (cameras_select[i].yolo)
                        {
                            // Button is always visible and clickable if YOLO is selected.
                            sprintf(temp_string, "Dump Input##yolo_debug%d", i);
                            if (ImGui::Button(temp_string))
                            {
                                // We still check if the worker is ready before calling the function
                                // to prevent a crash, but the button is never grayed out.
                                if (camera_control->subscribe && i < yolo_workers.size() && yolo_workers[i])
                                {
                                    yolo_workers[i]->DumpNextFrame();
                                }
                                else
                                {
                                    std::cout << "Warning: Cannot dump frame. YOLO worker is not ready for camera "
                                              << cameras_params[i].camera_serial << std::endl;
                                }
                            }
                        }

                        ImGui::TableNextColumn();
                        sprintf(temp_string, "##yolo_enet%d", i);
                        ImGui::Checkbox(temp_string, &cameras_select[i].send_yolo_via_enet);
                    }
                    ImGui::EndTable();
                }

                orange::gui::render_frame_ipc_status_panel(
                    camera_control->subscribe,
                    cameras_select,
                    cameras_params,
                    num_cameras,
                    frame_ipc_managers,
                    frame_ipc_init_errors);

                if (camera_control->subscribe) {
                    // ImGui::EndDisabled();
                }

                if (calibration_tool_busy) {
                    ImGui::EndDisabled();
                }

                if (camera_control->subscribe == true) {
                    ImGui::Separator();
                    ImGui::Spacing();
                    if (ImGui::Button("Picture save to")) {
                        make_folder(picture_save_folder);
                        for (int i = 0; i < num_cameras; i++) {
                            cameras_select[i].pictures_counter = 0;
                        }
                        IGFD::FileDialogConfig config;
                        config.countSelectionMax = 1;
                        config.path = picture_save_folder;
                        ImGuiFileDialog::Instance()->OpenDialog("ChoosePictureDir", "Choose a Directory", nullptr,
                                                                config);
                    }
                    ImGui::SameLine();
                    ImGui::Text("%s", picture_save_folder.c_str());
                    static int current_picture_format = 0;
                    const char* picture_format_items[] = { "jpg", "tiff", "png"};
                    ImGui::Combo("Picture format", &current_picture_format, picture_format_items, IM_ARRAYSIZE(picture_format_items));
                    for (int i = 0; i < num_cameras; i++) {
                        cameras_select[i].frame_save_format = std::string(picture_format_items[current_picture_format]);
                    }

                    if (ImGui::TreeNode("Save pictures from capturing")) {
                        const int cols = 5;
                        for (int i = 0; i < num_cameras; ++i) {     
                            std::string label = cameras_params[i].camera_name + ": " + std::to_string(cameras_select[i].pictures_counter) + "##calibration_save";
                            if (ImGui::Selectable(label.c_str(), &cameras_select[i].selected_to_save,
                                                    ImGuiSelectableFlags_None,
                                                    ImVec2(150, 50))) {
                            }
    
                            // Keep items on the same line until end of row
                            if ((i + 1) % cols != 0)
                                ImGui::SameLine();
                        }
    
                        ImGui::NewLine();
    
                        ImGui::SeparatorText("Debug Info");
                        ImGui::Text("Window Focused: %d, Window Hovered: %d", ImGui::IsWindowFocused(ImGuiFocusedFlags_AnyWindow), ImGui::IsWindowHovered(ImGuiHoveredFlags_AnyWindow));
                        ImGui::Text("camera_control->subscribe = %s", camera_control->subscribe ? "true" : "false");
                        ImGui::Text("save_image_all_ready = %s", save_image_all_ready ? "true" : "false");
                        ImGui::Separator();
    
                        if (ImGui::Button("Save selected")) {
                            std::cout << "[GUI] 'Save selected' button clicked. Formatting save name." << std::endl;
                            make_folder(picture_save_folder);
                            std::string frame_save_name = get_current_time_milliseconds();
                            for (int i = 0; i < num_cameras; i++) {
                                cameras_select[i].frame_save_name = frame_save_name;
                                cameras_select[i].picture_save_folder = picture_save_folder;
                                if (cameras_select[i].selected_to_save) {
                                    cameras_select[i].frame_save_state = State_Write_New_Frame;
                                    std::cout << "[GUI]   - Flagging camera " << cameras_params[i].camera_serial << " to save frame." << std::endl;
                                }
                            }
                        }
                        ImGui::SameLine();
    
                        if (ImGui::Button("Save pictures all")) {
                            std::cout << "[GUI] 'Save pictures all' button clicked. Formatting save name." << std::endl;
                            make_folder(picture_save_folder);
                            std::string frame_save_name = get_current_time_milliseconds();
                            for (int i = 0; i < num_cameras; i++) {
                                cameras_select[i].frame_save_name = frame_save_name;
                                cameras_select[i].picture_save_folder = picture_save_folder;
                                cameras_select[i].frame_save_state = State_Write_New_Frame;
                            }
                            std::cout << "[GUI]   - Flagging ALL cameras to save frame." << std::endl;
                        }
    

                        if (calib_state == CalibSavePictures) {
                            std::cout << "[GUI] Calibration state is 'CalibSavePictures', triggering next pose." << std::endl;
                            send_indigo_message(indigo_signal_builder.server, indigo_signal_builder.builder, indigo_signal_builder.indigo_connection, FetchGame::SignalType_CalibrationNextPose);
                            calib_state = CalibNextPose;
                        }
                        
                        if (calib_state == CalibPoseReached) {
                            std::cout << "[GUI] Calibration state is 'CalibPoseReached', triggering frame save for all cameras." << std::endl;
                            make_folder(calib_save_folder);
                            for (int i = 0; i < num_cameras; i++) {
                                cameras_select[i].frame_save_name = std::to_string(cameras_select[i].pictures_counter);
                                cameras_select[i].picture_save_folder = calib_save_folder;
                                cameras_select[i].frame_save_state = State_Write_New_Frame;
                            }
                            calib_state = CalibSavePictures;
                        }

                        if (ImGui::Button("Calib save images with counter")) {
                            std::cout << "[GUI] 'Calib save images with counter' button clicked." << std::endl;
                            make_folder(calib_save_folder);
                            for (int i = 0; i < num_cameras; i++) {
                                cameras_select[i].frame_save_name = std::to_string(cameras_select[i].pictures_counter);
                                cameras_select[i].picture_save_folder = calib_save_folder;
                                cameras_select[i].frame_save_state = State_Write_New_Frame;
                            }
                        } 
                        
                        ImGui::TreePop();
                    }
                }
            }
        }
        ImGui::End();

        render_aperture_characterization_window(
            &aperture_ui_state,
            camera_control,
            ecams,
            cameras_params,
            num_cameras,
            aperture_char_output_folder);

        render_usaf_resolution_window(
            &usaf_ui_state,
            camera_control,
            ecams,
            cameras_params,
            num_cameras,
            usaf_output_folder);

        if (tex && num_cameras > 0) {
            if (static_cast<int>(live_preview_texture_ids.size()) < num_cameras) {
                live_preview_texture_ids.resize(num_cameras, 0);
            }
            for (int i = 0; i < num_cameras; ++i) {
                live_preview_texture_ids[i] = tex[i].texture;
            }
        }

        render_spatial_layout_window(
            &spatial_layout_ui_state,
            camera_control,
            ecams,
            cameras_params,
            cameras_select,
            num_cameras,
            calibration_tool_busy,
            spatial_calibration_sessions_folder,
            live_preview_texture_ids.empty() ? nullptr : live_preview_texture_ids.data(),
            display_uploaded_serials.empty() ? nullptr : display_uploaded_serials.data(),
            spatialSnapshotWorkers);

        // file explorer display
        if (ImGuiFileDialog::Instance()->Display("ChooseYOLOFile")) {
            // => will show a dialog
            if (ImGuiFileDialog::Instance()->IsOk()) {
                // action if OK
                yolo_model = ImGuiFileDialog::Instance()->GetFilePathName();
            }
            // close
            ImGuiFileDialog::Instance()->Close();
        }

        if (ImGuiFileDialog::Instance()->Display("ChooseRecordingDir")) {
            // => will show a dialog
            if (ImGuiFileDialog::Instance()->IsOk()) {
                // action if OK
                auto selected_folder = ImGuiFileDialog::Instance()->GetSelection();
                input_folder = ImGuiFileDialog::Instance()->GetCurrentPath();
            }
            // close
            ImGuiFileDialog::Instance()->Close();
        }


        if (ImGuiFileDialog::Instance()->Display("ChoosePictureDir")) {
            // => will show a dialog
            if (ImGuiFileDialog::Instance()->IsOk()) {
                // action if OK
                auto selected_folder = ImGuiFileDialog::Instance()->GetSelection();
                picture_save_folder = ImGuiFileDialog::Instance()->GetCurrentPath();
            }
            // close
            ImGuiFileDialog::Instance()->Close();
        }


        if (ImGui::Begin("Local")) {
            if (camera_control->open) {
                // ImGui::BeginDisabled();
            }

            for (size_t i = 0; i < local_config_folders.size(); i++) {
                std::vector<std::string> folder_token = string_split(local_config_folders[i], "/");
                sprintf(temp_string, "%s", folder_token.back().c_str());
                ImGui::RadioButton(temp_string, &local_config_select, i);
                ImGui::SameLine();
            }
            ImGui::RadioButton("Null", &local_config_select, local_config_folders.size());
            const bool has_selected_local_folder =
                local_config_select >= 0 && local_config_select < static_cast<int>(local_config_folders.size());
            const std::string selected_local_config_folder =
                has_selected_local_folder ? local_config_folders[local_config_select] : std::string();

            ImGui::Separator();
            ImGui::TextWrapped("Selected local folder: %s",
                               has_selected_local_folder ? selected_local_config_folder.c_str() : "Null");
            ImGui::InputText("New local folder", new_local_config_folder_name, IM_ARRAYSIZE(new_local_config_folder_name));
            ImGui::SameLine();
            if (ImGui::Button("Create folder")) {
                const std::string folder_name = trim_ascii_whitespace(new_local_config_folder_name);
                std::string validation_error;
                if (!validate_local_config_folder_name(folder_name, &validation_error)) {
                    local_config_status = validation_error;
                    local_config_status_error = true;
                } else {
                    const std::string new_folder_path =
                        (std::filesystem::path(local_start_folder_name) / folder_name).string();
                    std::string ensure_error;
                    if (ensure_directory_exists(new_folder_path, &ensure_error)) {
                        select_local_config_folder(new_folder_path);
                        new_local_config_folder_name[0] = '\0';
                        local_config_status = std::string("Ready: ") + new_folder_path;
                        local_config_status_error = false;
                    } else {
                        local_config_status = ensure_error.empty() ? "Failed to create local config folder." : ensure_error;
                        local_config_status_error = true;
                    }
                }
            }
            ImGui::SameLine();
            if (!camera_control->open || num_cameras <= 0) {
                ImGui::BeginDisabled();
            }
            if (ImGui::Button("Create config from open cameras")) {
                const std::string requested_folder_name =
                    trim_ascii_whitespace(new_local_config_folder_name);
                std::string folder_name = requested_folder_name.empty()
                    ? std::string("local_") + get_current_date_time()
                    : requested_folder_name;
                std::string validation_error;
                if (!validate_local_config_folder_name(folder_name, &validation_error)) {
                    local_config_status = validation_error;
                    local_config_status_error = true;
                } else {
                    std::filesystem::path new_folder_path =
                        std::filesystem::path(local_start_folder_name) / folder_name;
                    if (requested_folder_name.empty()) {
                        for (int suffix = 2; suffix < 100; ++suffix) {
                            std::error_code suffix_exists_error;
                            const bool suffix_path_exists =
                                std::filesystem::exists(new_folder_path, suffix_exists_error);
                            if (suffix_exists_error || !suffix_path_exists) {
                                break;
                            }
                            new_folder_path = std::filesystem::path(local_start_folder_name) /
                                (folder_name + "_" + std::to_string(suffix));
                        }
                    }
                    std::error_code exists_error;
                    const bool path_exists = std::filesystem::exists(new_folder_path, exists_error);
                    if (exists_error || path_exists) {
                        local_config_status =
                            exists_error
                                ? std::string("Failed to check local config folder: ") + exists_error.message()
                                : std::string("Local config folder already exists: ") + new_folder_path.string();
                        local_config_status_error = true;
                    } else {
                        std::string save_error;
                        if (save_camera_json_configs_to_folder(
                                cameras_params,
                                num_cameras,
                                new_folder_path.string(),
                                &save_error)) {
                            select_local_config_folder(new_folder_path.string());
                            update_camera_configs(camera_config_files, new_folder_path.string());
                            new_local_config_folder_name[0] = '\0';
                            local_config_status =
                                "Created " + std::to_string(num_cameras) +
                                " camera config file" + (num_cameras == 1 ? std::string() : "s") +
                                ": " + new_folder_path.string();
                            local_config_status_error = false;
                        } else {
                            local_config_status =
                                save_error.empty() ? "Failed to create camera config files." : save_error;
                            local_config_status_error = true;
                        }
                    }
                }
            }
            if (!camera_control->open || num_cameras <= 0) {
                ImGui::EndDisabled();
            }

            if (camera_control->open && has_selected_local_folder) {
                if (ImGui::Button("Use selected folder for open cameras")) {
                    assign_camera_config_paths(cameras_params, num_cameras, selected_local_config_folder);
                    local_config_status = std::string("Open cameras now save into ") + selected_local_config_folder;
                    local_config_status_error = false;
                }
            }

            if (!local_config_status.empty()) {
                if (local_config_status_error) {
                    ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.35f, 1.0f), "%s", local_config_status.c_str());
                } else {
                    ImGui::TextColored(ImVec4(0.35f, 0.85f, 0.45f, 1.0f), "%s", local_config_status.c_str());
                }
            }

            if (camera_control->open) {
                // ImGui::EndDisabled();
            }

            if (camera_control->subscribe || calibration_tool_busy) {
                ImGui::BeginDisabled();
            }

            if (ImGui::Button(camera_control->open ? "Close Camera" : "Open camera") ||
                gui_autorun_requests.open_cameras) {
                if (!camera_control->open) {
                    if (local_config_select < local_config_folders.size()) {
                        update_camera_configs(camera_config_files, local_config_folders[local_config_select]);
                        if (!camera_config_files.empty()) {
                            select_cameras_have_configs(camera_config_files, device_info, camera_is_selected, cam_count);
                        } else {
                            std::cout << "[GUI] Selected local config folder is empty; preserving current camera selection."
                                      << std::endl;
                        }
                    }

                    num_cameras = 0;
                    for (int i = 0; i < cam_count; i++) {
                        if (camera_is_selected[i]) {
                            num_cameras++;
                        }
                    }
                    if (num_cameras > 0) {
                        camera_control->open = true;
                        cameras_params = new CameraParams[num_cameras];
                        cameras_select = new CameraEachSelect[num_cameras];
                        for (int i = 0; i < num_cameras; ++i) {
                            cameras_select[i].downsample = stream_downsample;
                            cameras_select[i].display_preview_max_fps =
                                gui_display_preview_max_fps_default;
                        }

                        std::vector<int> selected_cameras;
                        for (int i = 0; i < cam_count; i++) {
                            if (camera_is_selected[i]) {
                                selected_cameras.push_back(i);
                            }
                        }

                        std::vector<bool> skip_setting_params;
                        skip_setting_params.resize(num_cameras);
                        for (int i = 0; i < num_cameras; i++) {
                            if (!set_camera_params(&cameras_params[i], &device_info[selected_cameras[i]],
                                                   camera_config_files, selected_cameras[i], num_cameras)) {
                                skip_setting_params[i] = true;
                                cameras_params[i].camera_id = selected_cameras[i];
                                cameras_params[i].num_cameras = num_cameras;
                            } else {
                                skip_setting_params[i] = false;

                            }
                        }

                        {
                            const orange::recording::RecordingConfigSyncResult sync_result =
                                orange::recording::sync_encoder_config_from_camera_defaults(
                                    cameras_params,
                                    num_cameras,
                                    encoder_config);
                            recording_config_defaults_status = sync_result.message;
                            recording_config_defaults_status_warning = sync_result.warning;
                            if (!recording_config_defaults_status.empty()) {
                                std::cout << "[GUI][recording-defaults] "
                                          << recording_config_defaults_status << std::endl;
                            }
                        }


                        for (int i = 0; i < num_cameras; i++) {
                            cameras_select[i].stream_on = false;
                            if (cameras_params[i].camera_name == "ceiling_center") {
                                cameras_select[i].stream_on = true;
                                cameras_select[i].yolo = false;
                            }

                            if (cameras_params[i].camera_name == "shelter") {
                                cameras_select[i].stream_on = true;
                            }

                        }

                        ecams = new CameraEmergent[num_cameras];
                        for (int i = 0; i < num_cameras; i++) {
                            if (!skip_setting_params[i]) {
                                open_camera_with_params(&ecams[i].camera, &device_info[cameras_params[i].camera_id],
                                                    &cameras_params[i], "gui_open_selected_cameras");
                            } else {
                                update_camera_params(&ecams[i].camera, &device_info[cameras_params[i].camera_id],
                                                    &cameras_params[i]);
                            }

                        }
                        int ptp_config_count = 0;
                        for (int i = 0; i < num_cameras; i++) {
                            if (camera_sync_mode_uses_ptp(&cameras_params[i])) {
                                ptp_config_count++;
                            }
                        }
                        if (ptp_config_count == num_cameras && num_cameras > 0) {
                            ptp_stream_sync = true;
                        } else {
                            if (ptp_config_count > 0) {
                                std::cout << "[GUI] Mixed ptp_gate/non-PTP camera configs loaded; leaving PTP Stream Sync unchecked."
                                          << std::endl;
                            }
                            ptp_stream_sync = false;
                        }
                        {
                            bool mixed_crop_sizes = false;
                            crop_size_px = resolve_gui_crop_size_from_camera_configs(
                                cameras_params,
                                num_cameras,
                                crop_size_px,
                                &mixed_crop_sizes);
                            if (mixed_crop_sizes) {
                                crop_size_config_status =
                                    "Mixed crop sizes loaded; using first open camera value for this GUI session.";
                                crop_size_config_status_warning = true;
                                apply_gui_crop_size_to_camera_configs(cameras_params, num_cameras, crop_size_px);
                            } else {
                                crop_size_config_status =
                                    "Loaded crop size from camera config: " + std::to_string(crop_size_px) + " px.";
                                crop_size_config_status_warning = false;
                            }
                        }
                        {
                            bool mixed_preview_max_fps = false;
                            crop_preview_max_fps =
                                resolve_gui_crop_preview_max_fps_from_camera_configs(
                                    cameras_params,
                                    num_cameras,
                                    crop_preview_max_fps,
                                    &mixed_preview_max_fps);
                            if (mixed_preview_max_fps) {
                                crop_preview_config_status =
                                    "Mixed crop preview FPS caps loaded; using first open camera value for this GUI session.";
                                crop_preview_config_status_warning = true;
                                apply_gui_crop_preview_max_fps_to_camera_configs(
                                    cameras_params,
                                    num_cameras,
                                    crop_preview_max_fps);
                            } else {
                                crop_preview_config_status =
                                    "Loaded crop preview max FPS from camera config: " +
                                    std::to_string(crop_preview_max_fps) + ".";
                                crop_preview_config_status_warning = false;
                            }
                        }
                        apply_gui_autorun_camera_selection(
                            gui_autorun_config,
                            cameras_select,
                            num_cameras);
                        realtime_plot_data = new ScrollingBuffer[num_cameras];

                    }
                } else {
                    camera_control->open = false;
                    recording_config_defaults_status.clear();
                    recording_config_defaults_status_warning = false;
                    crop_size_config_status.clear();
                    crop_size_config_status_warning = false;
                    crop_preview_config_status.clear();
                    crop_preview_config_status_warning = false;
                    ptp_stream_sync = false;
                    for (int i = 0; i < num_cameras; i++) {
                        close_camera(&ecams[i].camera, &cameras_params[i]);
                    }
                    delete[] cameras_params;
                    cameras_params = nullptr;
                    delete[] cameras_select;
                    cameras_select = nullptr;
                    delete[] ecams;
                    ecams = nullptr;
                    delete[] realtime_plot_data;
                    realtime_plot_data = nullptr;
                }
            }
            if (camera_control->subscribe || calibration_tool_busy) {
                ImGui::EndDisabled();
            }

            orange::gui::render_host_ptp_stack_panel(
                &host_ptp_stack_ui,
                camera_control->subscribe,
                ptp_stream_sync);

            if (!camera_control->record_video && camera_control->open) {
                if (camera_control->subscribe) {
                    // ImGui::BeginDisabled();
                }
                ImGui::Checkbox("PTP Stream Sync", &ptp_stream_sync);
                ImGui::SameLine();
                if (camera_control->subscribe) {
                    // ImGui::EndDisabled();
                }
                if (calibration_tool_busy) {
                    ImGui::BeginDisabled();
                }
                if (ImGui::Button(camera_control->subscribe ? "Stop streaming" : "Start streaming") ||
                    gui_autorun_requests.toggle_streaming) {
                    const bool start_streaming = !camera_control->subscribe;
                    if (start_streaming) {
                        const RecordingPreflightResult preflight =
                            run_gui_recording_preflight(
                                cameras_params,
                                cameras_select,
                                num_cameras,
                                yolo_model,
                                crop_size_px);
                        if (!preflight.ok) {
                            recording_preflight_errors = preflight.errors;
                            log_recording_preflight_failure("gui_start_streaming", preflight);
                        } else {
                            recording_preflight_errors.clear();
                            camera_control->subscribe = true;
                            gui_mark_stream_started(&gui_session_timing);
                        // START STREAMING
                            std::cout << "STARTING STREAMING SESSION..." << std::endl;

                            if (std::any_of(cameras_select, cameras_select + num_cameras, [](const CameraEachSelect& cs){ return cs.record || cs.crop_and_encode; })) {
                                // Store the base folder for recordings, not the final timestamped one.
                                encoder_config->folder_name = input_folder;
                            }

                            // This part remains the same
                            camera_resources.resize(num_cameras);
                            frame_ipc_managers.clear();
                            frame_ipc_managers.resize(num_cameras);
                            frame_ipc_init_errors.clear();
                            frame_ipc_init_errors.resize(num_cameras);
                            size_t max_frame_size_bytes = 0;
                            for (int i = 0; i < num_cameras; ++i) {
                                size_t current_size = (size_t)cameras_params[i].width * (size_t)cameras_params[i].height;
                                if (current_size > max_frame_size_bytes) {
                                    max_frame_size_bytes = current_size;
                                }
                            }
                            for (int i = 0; i < num_cameras; ++i) {
                                if (!gui_camera_has_acquisition_work(cameras_select[i])) {
                                    continue;
                                }
                                std::cout << "Initializing resources for camera " << i << " on GPU " << cameras_params[i].gpu_id << std::endl;
                                camera_resources[i].initialize(
                                    cameras_params[i].gpu_id,
                                    max_frame_size_bytes,
                                    cameras_select[i].yolo,
                                    cameras_params[i].recording.resources.acquire_work_entries);
                                if (cameras_select[i].send_frame_ipc) {
                                    frame_ipc_managers[i] = std::make_unique<FrameIPCManager>(&cameras_params[i]);
                                    if (!frame_ipc_managers[i]->isEnabled()) {
                                        frame_ipc_init_errors[i] = frame_ipc_managers[i]->getInitError();
                                        frame_ipc_managers[i].reset();
                                    }
                                }
                            }
                            // Create worker thread objects and GPU textures
                            openGLDisplayWorkers = new COpenGLDisplay*[num_cameras]();
                            cropProducerWorkers = new CropProducerWorker*[num_cameras]();
                            cropAndEncodeWorkers = new CropAndEncodeWorker*[num_cameras]();
                            cropPreviewWorkers = new CropPreviewWorker*[num_cameras]();
                            spatialSnapshotWorkers = new SpatialSnapshotWorker*[num_cameras]();
                            poseWorkers = new PoseWorker*[num_cameras]();
                            tex = new GL_Texture[num_cameras];
                            crop_tex = new GL_Texture[num_cameras];
                            display_uploaded_serials.assign(
                                num_cameras,
                                std::numeric_limits<uint64_t>::max());
                            crop_preview_uploaded_serials.assign(
                                num_cameras,
                                std::numeric_limits<uint64_t>::max());
                            live_preview_texture_ids.assign(
                                num_cameras,
                                0);
                            yolo_workers.assign(num_cameras, nullptr);
                            // Initialize all worker pointers to nullptr
                            for(int i = 0; i < num_cameras; ++i) {
                                openGLDisplayWorkers[i] = nullptr;
                                cropProducerWorkers[i] = nullptr;
                                cropAndEncodeWorkers[i] = nullptr;
                                cropPreviewWorkers[i] = nullptr;
                                spatialSnapshotWorkers[i] = nullptr;
                                poseWorkers[i] = nullptr;
                            }
                            cudaSetDevice(display_gpu_id);

                            // Allocate main textures for each camera's OpenGL display
                            for (int i = 0; i < num_cameras; i++) {
                                if (cameras_select[i].stream_on) {
                                    int w = (int)(cameras_params[i].width / cameras_select[i].downsample);
                                    int h = (int)(cameras_params[i].height / cameras_select[i].downsample);
                                    setup_texture(tex[i], w, h);
                                    if (static_cast<int>(live_preview_texture_ids.size()) < num_cameras) {
                                        live_preview_texture_ids.resize(num_cameras, 0);
                                    }
                                    live_preview_texture_ids[i] = tex[i].texture;
                                }
                            }

                            // Setup cropped textures for each crop/encode worker
                            for (int i = 0; i < num_cameras; i++) {
                                if (cameras_select[i].crop_and_encode) {
                                    setup_texture(
                                        crop_tex[i],
                                        crop_size_px,
                                        crop_size_px);
                                }
                            }

                            // CREATE AND LINK ALL WORKER THREADS
                            for (int i = 0; i < num_cameras; i++) {
                                if (cameras_select[i].stream_on) {
                                    std::string name = "OpenGLDisplay_Cam_" + cameras_params[i].camera_serial;
                                    openGLDisplayWorkers[i] = new COpenGLDisplay(name.c_str(), &cameras_params[i], &cameras_select[i], tex[i].cuda_buffer, &indigo_signal_builder, *camera_resources[i].recycle_queue);
                                }
                                if (cameras_select[i].yolo) {
                                    std::string name = "YoloWorker_Cam_" + cameras_params[i].camera_serial;
                                    cameras_select[i].yolo_model = yolo_model.c_str();
                                    yolo_workers[i] = new YoloWorker(
                                        name.c_str(),
                                        &cameras_params[i],
                                        &cameras_select[i],
                                        camera_control,
                                        *camera_resources[i].recycle_queue
                                    );
                                    if (openGLDisplayWorkers[i]) {
                                        yolo_workers[i]->SetDisplayWorker(openGLDisplayWorkers[i]);
                                    }
                                }
                                if (cameras_select[i].crop_and_encode || cameras_select[i].pose) {
                                    std::string name = "CropProducer_Cam_" + cameras_params[i].camera_serial;
                                    cropProducerWorkers[i] = new CropProducerWorker(
                                        name.c_str(),
                                        &cameras_params[i],
                                        *camera_resources[i].recycle_queue,
                                        camera_control,
                                        crop_size_px
                                    );
                                    if (yolo_workers[i]) {
                                        yolo_workers[i]->SetCropProducerWorker(cropProducerWorkers[i]);
                                    }
                                }
                                if (cameras_select[i].crop_and_encode) {
                                    std::string name = "CropEncode_Cam_" + cameras_params[i].camera_serial;
                                    cropAndEncodeWorkers[i] = new CropAndEncodeWorker(
                                        name.c_str(),
                                        &cameras_params[i],
                                        encoder_config->folder_name,
                                        *camera_resources[i].recycle_queue,
                                        crop_tex[i].cuda_buffer,
                                        camera_control,
                                        crop_size_px
                                    );
                                    if (cropProducerWorkers[i]) {
                                        std::string preview_name = "CropPreview_Cam_" + cameras_params[i].camera_serial;
                                        cropPreviewWorkers[i] = new CropPreviewWorker(
                                            preview_name.c_str(),
                                            &cameras_params[i],
                                            crop_tex[i].cuda_buffer,
                                            cropProducerWorkers[i]->GetCropProducer(),
                                            crop_size_px);
                                        cropPreviewWorkers[i]->SetPreviewDisplayEnabled(
                                            show_crop_preview_windows);
                                        cropAndEncodeWorkers[i]->SetCropProducer(
                                            cropProducerWorkers[i]->GetCropProducer());
                                        cropAndEncodeWorkers[i]->SetCropProducerWorker(
                                            cropProducerWorkers[i]);
                                        cropAndEncodeWorkers[i]->SetCropPreviewWorker(
                                            cropPreviewWorkers[i]);
                                        cropProducerWorkers[i]->SetCropPreviewWorker(
                                            cropPreviewWorkers[i]);
                                        cropProducerWorkers[i]->SetCropAndEncodeWorker(cropAndEncodeWorkers[i]);
                                    }
                                }
                                if (cameras_select[i].pose && cropProducerWorkers[i]) {
                                    std::string name = "PoseWorker_Cam_" + cameras_params[i].camera_serial;
                                    poseWorkers[i] = new PoseWorker(
                                        name.c_str(),
                                        &cameras_params[i],
                                        cropProducerWorkers[i]->GetCropProducer(),
                                        frame_ipc_managers[i].get());
                                    cropProducerWorkers[i]->SetPoseWorker(poseWorkers[i]);
                                }
                                if (gui_camera_has_acquisition_work(cameras_select[i])) {
                                    std::string name =
                                        "SpatialSnapshot_Cam_" + cameras_params[i].camera_serial;
                                    spatialSnapshotWorkers[i] = new SpatialSnapshotWorker(
                                        name.c_str(),
                                        &cameras_params[i],
                                        *camera_resources[i].recycle_queue);
                                }
                            }

                            orange::session::create_recording_pipelines_for_stream(
                                &recording_session,
                                cameras_params,
                                cameras_select,
                                num_cameras,
                                *encoder_config,
                                camera_resources.data(),
                                camera_control,
                                &app_storage_config);

                            // START ALL WORKER THREADS
                            for (int i = 0; i < num_cameras; i++) {
                                if (openGLDisplayWorkers[i]) {
                                    openGLDisplayWorkers[i]->SetMaxQueueSize(240); 
                                    openGLDisplayWorkers[i]->StartThread();
                                }
                                if (cropAndEncodeWorkers[i]) {
                                    cropAndEncodeWorkers[i]->SetMaxQueueSize(240);
                                    cropAndEncodeWorkers[i]->StartThread();
                                }
                                if (cropPreviewWorkers[i]) {
                                    cropPreviewWorkers[i]->SetMaxQueueSize(
                                        CropPreviewWorker::kDefaultQueueSize);
                                    cropPreviewWorkers[i]->StartThread();
                                }
                                if (poseWorkers[i]) {
                                    poseWorkers[i]->SetMaxQueueSize(32);
                                    poseWorkers[i]->StartThread();
                                }
                                if (spatialSnapshotWorkers[i]) {
                                    spatialSnapshotWorkers[i]->SetMaxQueueSize(2);
                                    spatialSnapshotWorkers[i]->StartThread();
                                }
                                if (cropProducerWorkers[i]) {
                                    cropProducerWorkers[i]->SetMaxQueueSize(240);
                                    cropProducerWorkers[i]->StartThread();
                                }
                                if (yolo_workers[i]) {
                                    yolo_workers[i]->SetMaxQueueSize(240);
                                    yolo_workers[i]->StartThread();
                                }
                                orange::session::start_recording_pipeline_for_camera(&recording_session, i);
                            }

                            // PREPARE CAMERAS
                            for (int i = 0; i < num_cameras; i++) {
                                if (!gui_camera_has_acquisition_work(cameras_select[i])) {
                                    continue;
                                }
                                camera_open_stream(&ecams[i].camera, &cameras_params[i], "gui_start_streaming");
                                ecams[i].evt_frame = new Emergent::CEmergentFrame[evt_buffer_size];
                                ecams[i].evt_frame_count = evt_buffer_size;
                                allocate_frame_buffer(&ecams[i].camera, ecams[i].evt_frame, &cameras_params[i], evt_buffer_size);
                            }
                            if (ptp_stream_sync) {
                                for (int i = 0; i < num_cameras; i++) {
                                    if (!gui_camera_has_acquisition_work(cameras_select[i])) {
                                        continue;
                                    }
                                    ptp_camera_sync(&ecams[i].camera, &cameras_params[i]);
                                }
                                camera_control->sync_camera = true;
                            }

                            // Start acquisition threads
                            for (int i = 0; i < num_cameras; i++) {
                                if (!gui_camera_has_acquisition_work(cameras_select[i])) {
                                    continue;
                                }
                                camera_threads.emplace_back(
                                    &acquire_frames,
                                    &ecams[i],
                                    &cameras_params[i],
                                    &cameras_select[i],
                                    camera_control,
                                    ptp_params,
                                    &indigo_signal_builder,
                                    openGLDisplayWorkers[i],
                                    orange::session::recording_ingress_for_camera(recording_session, i),
                                    yolo_workers[i],
                                    image_writer,
                                    &camera_resources[i],
                                    frame_ipc_managers[i].get(),
                                    nullptr,
                                    spatialSnapshotWorkers ? spatialSnapshotWorkers[i] : nullptr
                                );
                            }
                        }
                    } else {
                        camera_control->subscribe = false;
                        // STOP STREAMING
                        std::cout << "STOPPING STREAMING SESSION..." << std::endl;
                        if (camera_control->record_video) {
                            gui_note_recording_stop_requested(
                                &gui_recording_run,
                                "stream_shutdown");
                            orange::session::request_drain_recording_run(&recording_session, camera_control);
                            gui_mark_recording_finalizing(&gui_session_timing);
                            std::cout << "Recording toggled OFF by stream shutdown. Encoders will drain queued frames." << std::endl;
                        } else if (camera_control->recording_draining) {
                            gui_note_recording_stop_requested(
                                &gui_recording_run,
                                "stream_shutdown");
                            gui_mark_recording_finalizing(&gui_session_timing);
                        }

                        // 1. Stop the acquisition threads first.
                        // This prevents new frames from entering the pipeline.
                        for (auto &t : camera_threads) {
                            if (t.joinable()) t.join();
                        }
                        camera_threads.clear();
                        std::cout << "Acquisition threads joined." << std::endl;

                        // RESET PTP STATE
                        if (ptp_stream_sync) {
                            ptp_params->ptp_global_time = 0;
                            ptp_params->ptp_stop_time = 0;
                            ptp_params->ptp_counter = 0;
                            ptp_params->ptp_stop_counter = 0;
                            ptp_params->network_sync = false;
                            ptp_params->network_set_start_ptp = false;
                            ptp_params->ptp_stop_reached = false;
                            ptp_params->ptp_start_reached = false;
                            camera_control->sync_camera = false; // Also reset this flag
                            std::cout << "PTPParams state has been reset for the next run." << std::endl;
                        }

                        // 2. Signal all worker threads to stop processing NEW data from their queues.
                        // They will finish processing whatever is currently in their queue.
                        for (int i = 0; i < num_cameras; i++) {
                            if (yolo_workers[i]) yolo_workers[i]->StopThread();
                            if (openGLDisplayWorkers[i]) openGLDisplayWorkers[i]->StopThread();
                            if (cropProducerWorkers[i]) cropProducerWorkers[i]->StopThread();
                            if (cropPreviewWorkers[i]) cropPreviewWorkers[i]->StopThread();
                            if (cropAndEncodeWorkers[i]) cropAndEncodeWorkers[i]->StopThread();
                            if (poseWorkers[i]) poseWorkers[i]->StopThread();
                            if (spatialSnapshotWorkers && spatialSnapshotWorkers[i]) {
                                spatialSnapshotWorkers[i]->StopThread();
                            }
                            orange::session::request_stop_recording_pipeline_for_camera(&recording_session, i);
                        }
                        std::cout << "All worker threads signaled to stop. Waiting for queues to drain..." << std::endl;

                        // 3. Join and delete workers in REVERSE pipeline order to ensure the pipeline is drained.
                        for (int i = 0; i < num_cameras; i++) {
                            // Endpoints are first.
                            if (openGLDisplayWorkers[i]) {
                                delete openGLDisplayWorkers[i];
                                openGLDisplayWorkers[i] = nullptr;
                            }

                            if (cropAndEncodeWorkers[i]) {
                                std::cout << "Flushing final packets for crop encoder " << cameras_params[i].camera_serial << "..." << std::endl;
                                cropAndEncodeWorkers[i]->finalize_recording();
                                delete cropAndEncodeWorkers[i];
                                cropAndEncodeWorkers[i] = nullptr;
                            }

                            if (cropPreviewWorkers[i]) {
                                delete cropPreviewWorkers[i];
                                cropPreviewWorkers[i] = nullptr;
                            }

                            if (poseWorkers[i]) {
                                poseWorkers[i]->CloseRecording();
                                delete poseWorkers[i];
                                poseWorkers[i] = nullptr;
                            }

                            if (spatialSnapshotWorkers && spatialSnapshotWorkers[i]) {
                                delete spatialSnapshotWorkers[i];
                                spatialSnapshotWorkers[i] = nullptr;
                            }

                            if (cropProducerWorkers[i]) {
                                cropProducerWorkers[i]->CloseRecording();
                                delete cropProducerWorkers[i];
                                cropProducerWorkers[i] = nullptr;
                            }
                            
                            // Now the hardware encoder, which is fed by the preprocessor.
                            // The YOLO worker can be deleted now.
                            if (yolo_workers[i]) {
                                delete yolo_workers[i];
                                yolo_workers[i] = nullptr;
                            }

                            orange::session::shutdown_recording_pipeline_for_camera(&recording_session, i);
                        }
                        
                        // Clear the worker pointer vectors
                        yolo_workers.clear();
                        orange::session::clear_recording_pipelines(&recording_session);
                        if(openGLDisplayWorkers) { delete[] openGLDisplayWorkers; openGLDisplayWorkers = nullptr; }
                        if(cropProducerWorkers) { delete[] cropProducerWorkers; cropProducerWorkers = nullptr; }
                        if(cropAndEncodeWorkers) { delete[] cropAndEncodeWorkers; cropAndEncodeWorkers = nullptr; }
                        if(cropPreviewWorkers) { delete[] cropPreviewWorkers; cropPreviewWorkers = nullptr; }
                        if(spatialSnapshotWorkers) { delete[] spatialSnapshotWorkers; spatialSnapshotWorkers = nullptr; }
                        if(poseWorkers) { delete[] poseWorkers; poseWorkers = nullptr; }
                        std::cout << "Worker threads all cleaned up." << std::endl;
                        frame_ipc_managers.clear();
                        frame_ipc_init_errors.clear();

                        // 4. Final resource cleanup
                        for (int i = 0; i < num_cameras; i++) {
                            if (!gui_camera_has_acquisition_work(cameras_select[i])) {
                                continue;
                            }
                            destroy_frame_buffer(&ecams[i].camera, ecams[i].evt_frame, evt_buffer_size, &cameras_params[i]);
                            delete[] ecams[i].evt_frame;
                            ecams[i].evt_frame = nullptr;
                            ecams[i].evt_frame_count = 0;
                            check_camera_errors(EVT_CameraCloseStream(&ecams[i].camera), cameras_params[i].camera_serial.c_str());
                        }

                        for (int i = 0; i < num_cameras; i++) {
                            if (cameras_select[i].stream_on) {
                                int w = int(cameras_params[i].width / cameras_select[i].downsample);
                                int h = int(cameras_params[i].height / cameras_select[i].downsample);
                                clear_upload_and_cleanup(tex[i], w, h);
                            }
                            // Add this block to clean up the crop textures
                            if (cameras_select[i].crop_and_encode) {
                                clear_upload_and_cleanup(
                                    crop_tex[i],
                                    crop_size_px,
                                    crop_size_px);
                            }
                        }

                        if(tex) delete[] tex;
                        tex = nullptr;
                        if(crop_tex) delete[] crop_tex;
                        crop_tex = nullptr;
                        display_uploaded_serials.clear();
                        crop_preview_uploaded_serials.clear();
                        live_preview_texture_ids.clear();

                        for(int i = 0; i < num_cameras; ++i) {
                            camera_resources[i].cleanup();
                        }
                        camera_resources.clear();
                        std::cout << "Cleaned up all per-camera resources." << std::endl;
                        if (gui_finalize_recording_session_if_ready(
                                &gui_recording_run,
                                &recording_session,
                                camera_control,
                                cameras_params,
                                cameras_select,
                                num_cameras,
                                crop_size_px,
                                gui_display_frame_rate_json(
                                    gui_display_frame_rate_stats,
                                    stream_downsample,
                                    resolve_gui_display_preview_max_fps_snapshot(
                                        cameras_select,
                                        num_cameras),
                                    static_cast<int>(window->swap_interval),
                                    static_cast<int>(window->frame_max_fps),
                                    show_yolo_speed_graphs,
                                    orange_imgui_glfw_size_cache_stats()))) {
                            gui_display_frame_rate_stats.Finish();
                            gui_mark_local_control_drain_completed(
                                &gui_local_control_stop_scheduler,
                                gui_local_control_event_log_path,
                                gui_recording_run.recording_folder);
                            std::cout << "[GUI][recording] Finalized recording session during stream shutdown."
                                      << std::endl;
                        }
                        gui_mark_recording_finished(&gui_session_timing);
                        gui_mark_stream_stopped(&gui_session_timing);
                    }
                }
                if (calibration_tool_busy) {
                    ImGui::EndDisabled();
                }
            }

            if (camera_control->stop_record) {
                ImGui::PushStyleColor(ImGuiCol_Button, ImVec4{0.5f, 0, 0, 1.0f});
            } else {
                ImGui::PushStyleColor(ImGuiCol_Button, ImVec4{0, 0.5f, 0, 1.0f});
            }

            if (camera_control->open) {

                if (!camera_control->subscribe) {
                    // ImGui::BeginDisabled();
                }

                if (calibration_tool_busy) {
                    ImGui::BeginDisabled();
                }
                if (ImGui::Button(camera_control->record_video ? ICON_FK_PAUSE : ICON_FK_PLAY) ||
                    gui_autorun_requests.toggle_recording) {
                    if (!camera_control->record_video && camera_control->recording_draining) {
                        std::cout << "Recording is still draining. Please wait..." << std::endl;
                    } else {
                        const bool next_record_state = !camera_control->record_video;
                        if (next_record_state) {
                            if (gui_request_recording_start_through_operator_path(
                                    &recording_session,
                                    camera_control,
                                    &gui_recording_run,
                                    &gui_session_timing,
                                    &gui_display_frame_rate_stats,
                                    &gui_local_control_stop_scheduler,
                                    cameras_params,
                                    cameras_select,
                                    num_cameras,
                                    yolo_model,
                                    crop_size_px,
                                    encoder_config,
                                    input_folder,
                                    ptp_params,
                                    cropProducerWorkers,
                                    cropPreviewWorkers,
                                    cropAndEncodeWorkers,
                                    poseWorkers,
                                    &recording_preflight_errors,
                                    gui_autorun_requests.toggle_recording
                                        ? "autorun_start_recording"
                                        : "gui_start_recording")) {
                                std::cout << "Recording toggled ON." << std::endl;
                            }
                        } else {
                            // STOP RECORDING
                            gui_request_recording_stop_through_operator_path(
                                &recording_session,
                                camera_control,
                                &gui_recording_run,
                                &gui_session_timing,
                                gui_autorun_requests.toggle_recording
                                    ? "autorun_stop"
                                    : "manual_stop");
                            std::cout << "Recording toggled OFF. Encoders will drain queued frames." << std::endl;
                        }
                    }
                }
                if (calibration_tool_busy) {
                    ImGui::EndDisabled();
                }
                
                if (!camera_control->subscribe) {
                    // ImGui::EndDisabled(); // This can be uncommented if you want to disable the button
                }
            }

            const std::string active_recording_folder =
                orange::session::current_recording_folder(camera_control);
            if (!active_recording_folder.empty()) {
                ImGui::SameLine();
                if (ImGui::Button("Copy recording path")) {
                    ImGui::SetClipboardText(active_recording_folder.c_str());
                }
                ImGui::TextWrapped("Recording path: %s", active_recording_folder.c_str());
            }

            if (gui_finalize_recording_session_if_ready(
                    &gui_recording_run,
                    &recording_session,
                    camera_control,
                    cameras_params,
                    cameras_select,
                    num_cameras,
                    crop_size_px,
                    gui_display_frame_rate_json(
                        gui_display_frame_rate_stats,
                        stream_downsample,
                        resolve_gui_display_preview_max_fps_snapshot(
                            cameras_select,
                            num_cameras),
                        static_cast<int>(window->swap_interval),
                        static_cast<int>(window->frame_max_fps),
                        show_yolo_speed_graphs,
                        orange_imgui_glfw_size_cache_stats()))) {
                gui_display_frame_rate_stats.Finish();
                gui_mark_recording_finished(&gui_session_timing);
                gui_mark_local_control_drain_completed(
                    &gui_local_control_stop_scheduler,
                    gui_local_control_event_log_path,
                    gui_recording_run.recording_folder);
                if (gui_local_control_exit_after_finalize &&
                    gui_local_control_stop_scheduler.stop_triggered) {
                    gui_local_control_exit_pending_after_finalize = true;
                    gui_local_control_exit_stream_stop_requested = false;
                    gui_note_local_control_stop_event(
                        &gui_local_control_stop_scheduler,
                        camera_control->subscribe
                            ? "finalized_exit_pending_stream_stop"
                            : "finalized_exit_pending");
                    std::cout << "[GUI][local_control] recording finalized; GUI exit pending"
                              << " request_id="
                              << gui_local_control_stop_scheduler.request_id
                              << " operation_id="
                              << gui_local_control_stop_scheduler.operation_id
                              << std::endl;
                    gui_log_local_control_event(
                        gui_local_control_event_log_path,
                        {
                            {"event", camera_control->subscribe
                                          ? "finalized_exit_pending_stream_stop"
                                          : "finalized_exit_pending"},
                            {"method", gui_local_control_stop_scheduler.method},
                            {"request_id", gui_local_control_stop_scheduler.request_id},
                            {"operation_id", gui_local_control_stop_scheduler.operation_id},
                            {"command_source", gui_local_control_stop_scheduler.source},
                        });
                }
            }

            if (camera_control->open) {
                const GuiSessionTimingSnapshot timing =
                    gui_session_timing_snapshot(&gui_session_timing, camera_control);
                ImGui::Separator();
                render_gui_session_timing_status(
                    timing,
                    streaming_fps.load(),
                    nullptr);
                render_gui_external_recorder_status(recording_session);
            }

            ImGui::PopStyleColor(1);
        }
        ImGui::End();
        gui_frame_timing.orange_window_draw_ms = gui_elapsed_ms(
            orange_window_draw_start,
            std::chrono::steady_clock::now());


        if (camera_control->subscribe) {
            // Upload the texture data from the PBOs to the GPU textures
            for (int i = 0; i < num_cameras; i++) {
                if (cameras_select[i].stream_on) {
                    if (static_cast<int>(display_uploaded_serials.size()) < num_cameras) {
                        display_uploaded_serials.resize(
                            num_cameras,
                            std::numeric_limits<uint64_t>::max());
                    }
                    bool should_upload_display = true;
                    if (openGLDisplayWorkers && openGLDisplayWorkers[i]) {
                        const uint64_t preview_serial =
                            openGLDisplayWorkers[i]->PreviewSerial();
                        should_upload_display =
                            display_uploaded_serials[i] != preview_serial;
                        if (should_upload_display) {
                            display_uploaded_serials[i] = preview_serial;
                        }
                    }
                    if (should_upload_display) {
                        int camera_width = int(cameras_params[i].width / cameras_select[i].downsample);
                        int camera_height = int(cameras_params[i].height / cameras_select[i].downsample);
                        const auto upload_start = std::chrono::steady_clock::now();
                        upload_texture_from_pbo(tex[i], camera_width, camera_height);
                        gui_frame_timing.main_texture_upload_ms += gui_elapsed_ms(
                            upload_start,
                            std::chrono::steady_clock::now());
                        gui_frame_timing.main_texture_upload_count++;
                    }
                }
                if (show_crop_preview_windows && cameras_select[i].crop_and_encode) {
                    if (static_cast<int>(crop_preview_uploaded_serials.size()) < num_cameras) {
                        crop_preview_uploaded_serials.resize(
                            num_cameras,
                            std::numeric_limits<uint64_t>::max());
                    }
                    bool should_upload_crop_preview = true;
                    if (cropPreviewWorkers && cropPreviewWorkers[i]) {
                        const uint64_t preview_serial =
                            cropPreviewWorkers[i]->PreviewSerial();
                        should_upload_crop_preview =
                            crop_preview_uploaded_serials[i] != preview_serial;
                        if (should_upload_crop_preview) {
                            crop_preview_uploaded_serials[i] = preview_serial;
                        }
                    }
                    if (should_upload_crop_preview) {
                        const auto upload_start = std::chrono::steady_clock::now();
                        upload_texture_from_pbo(
                            crop_tex[i],
                            crop_size_px,
                            crop_size_px);
                        gui_frame_timing.crop_texture_upload_ms += gui_elapsed_ms(
                            upload_start,
                            std::chrono::steady_clock::now());
                        gui_frame_timing.crop_texture_upload_count++;
                    }
                }
            }
            // Draw main camera views
            const auto camera_draw_start = std::chrono::steady_clock::now();
            const GuiSessionTimingSnapshot timing =
                gui_session_timing_snapshot(&gui_session_timing, camera_control);
            if (camera_control->record_video) {
                // Resize speed tracking data
                if (speed_tracking_data.size() != num_cameras) {
                        speed_tracking_data.resize(num_cameras);
                }

                for (int i = 0; i < num_cameras; i++) {
                    
                    if (cameras_select[i].stream_on) {
                        std::string window_name = cameras_params[i].camera_name;
                        ImGui::Begin(window_name.c_str());

                        YoloWorker* yolo_worker = gui_yolo_worker_at(i);
                        render_gui_session_timing_status(
                            timing,
                            streaming_fps.load(),
                            cameras_select[i].yolo ? yolo_worker : nullptr);
            
                        ImVec2 avail_size = ImGui::GetContentRegionAvail();
                        avail_size.y *= 0.5f;
                        const float display_width =
                            static_cast<float>(cameras_params[i].width / cameras_select[i].downsample);
                        const float display_height =
                            static_cast<float>(cameras_params[i].height / cameras_select[i].downsample);
                        render_texture_image_centered(
                            tex[i].texture,
                            avail_size,
                            display_width,
                            display_height);

                        if (show_yolo_speed_graphs && cameras_select[i].yolo && yolo_worker) {
                            const auto speed_graph_start = std::chrono::steady_clock::now();
                            RenderSpeedGraph(i, yolo_worker, speed_tracking_data[i]);
                            gui_frame_timing.speed_graph_draw_ms += gui_elapsed_ms(
                                speed_graph_start,
                                std::chrono::steady_clock::now());
                        }
                        ImGui::End();
                    }
                }
            } else {
                for (int i = 0; i < num_cameras; i++) {
                    if (cameras_select[i].stream_on) {
                        std::string window_name = cameras_params[i].camera_name;
                        ImGui::Begin(window_name.c_str());

                        YoloWorker* yolo_worker = gui_yolo_worker_at(i);
                        render_gui_session_timing_status(
                            timing,
                            streaming_fps.load(),
                            cameras_select[i].yolo ? yolo_worker : nullptr);
                        const ImVec2 avail_size = ImGui::GetContentRegionAvail();
                        const float display_width =
                            static_cast<float>(cameras_params[i].width / cameras_select[i].downsample);
                        const float display_height =
                            static_cast<float>(cameras_params[i].height / cameras_select[i].downsample);
                        render_texture_image_centered(
                            tex[i].texture,
                            avail_size,
                            display_width,
                            display_height);
                        ImGui::End();
                    }
                }
            }
            gui_frame_timing.camera_window_draw_ms = gui_elapsed_ms(
                camera_draw_start,
                std::chrono::steady_clock::now());

            const auto crop_draw_start = std::chrono::steady_clock::now();
            for (int i = 0; i < num_cameras; i++) {
                    // Check if the crop and encode feature is enabled for this camera
                    if (show_crop_preview_windows && cameras_select[i].crop_and_encode) {
                        // Create a unique name for the new window
                        std::string window_name = cameras_params[i].camera_name + " Crop";
                        ImGui::Begin(window_name.c_str());
                        const ImVec2 available_size = ImGui::GetContentRegionAvail();
                        const ImVec2 image_size =
                            fit_square_image_size(available_size, static_cast<float>(crop_size_px));
                        const float x_offset = std::max(0.0f, (available_size.x - image_size.x) * 0.5f);
                        if (x_offset > 0.0f) {
                            ImGui::SetCursorPosX(ImGui::GetCursorPosX() + x_offset);
                        }
                        ImGui::Image(
                            (ImTextureID)(intptr_t)crop_tex[i].texture,
                            image_size,
                            ImVec2(0, 0),
                            ImVec2(1, 1));
                        ImGui::End();
	                    }
	                }
            gui_frame_timing.crop_window_draw_ms = gui_elapsed_ms(
                crop_draw_start,
                std::chrono::steady_clock::now());
        }

        if (camera_control->open && show_realtime_plot) {
            ImGui::Begin("Realtime Plots"); {
                static float t = 0;
                t += ImGui::GetIO().DeltaTime;
                for (int i = 0; i < num_cameras; i++) {
                    get_senstemp_value(&ecams[i].camera, &cameras_params[i]);
                    realtime_plot_data[i].AddPoint(t, cameras_params[i].sens_temp);
                }

                static float history = 10.0f;
                ImGui::SliderFloat("History", &history, 1, 30, "%.1f s");

                static ImPlotAxisFlags flags = ImPlotAxisFlags_NoTickMarks;
                ImVec2 avail_size = ImGui::GetContentRegionAvail();

                if (ImPlot::BeginPlot("Camera Sensor Temperature", avail_size)) {
                    ImPlot::SetupAxes(nullptr, nullptr, flags, flags);
                    ImPlot::SetupAxisLimits(ImAxis_X1, t - history, t, ImGuiCond_Always);
                    ImPlot::SetupAxisLimits(ImAxis_Y1, 30, 90);
                    ImPlot::SetNextFillStyle(IMPLOT_AUTO_COL, 0.5f);

                    for (int i = 0; i < num_cameras; i++) {
                        std::string line_name = std::string(cameras_params[i].camera_serial);
                        ImPlot::PlotLine(line_name.c_str(), &realtime_plot_data[i].Data[0].x,
                                         &realtime_plot_data[i].Data[0].y, realtime_plot_data[i].Data.size(), 0,
                                         realtime_plot_data[i].Offset, 2 * sizeof(float));
                    }
                    ImPlot::EndPlot();
                }
                ImGui::End();
            }
        }

        const auto render_start = std::chrono::steady_clock::now();
        render_a_frame(window);
        const auto render_end = std::chrono::steady_clock::now();
        gui_frame_timing.render_present_ms = gui_elapsed_ms(render_start, render_end);
        gui_frame_timing.frame_total_ms = gui_elapsed_ms(gui_frame_start, render_end);

        gui_sample_display_frame_rate(
            &gui_display_frame_rate_stats,
            ImGui::GetIO().DeltaTime,
            camera_control->record_video,
            camera_control->record_video &&
                gui_any_crop_recording_enabled(cameras_select, num_cameras),
            show_crop_preview_windows);
        gui_sample_frame_timings(
            &gui_display_frame_rate_stats,
            camera_control->record_video,
            gui_frame_timing);
    }

    if (aperture_ui_state.worker.joinable()) {
        aperture_ui_state.worker.join();
    }
    stop_fov_alignment_worker(&aperture_ui_state);
    clear_aperture_preview_texture(&aperture_ui_state);
    clear_alignment_preview_texture(&aperture_ui_state);

    if (usaf_ui_state.worker.joinable()) {
        usaf_ui_state.worker.join();
    }
    stop_usaf_preview_worker(&usaf_ui_state);
    clear_usaf_preview_texture(&usaf_ui_state);
    clear_usaf_captured_texture(&usaf_ui_state);
    clear_spatial_layout_texture(&spatial_layout_ui_state);
    gui_local_control_server.Stop();

    if (camera_control->open) {
        for (int i = 0; i < num_cameras; i++) {
            close_camera(&ecams[i].camera, &cameras_params[i]);
        }
        delete[] cameras_params;
        cameras_params = nullptr;
        delete[] cameras_select;
        cameras_select = nullptr;
        delete[] ecams;
        ecams = nullptr;
        delete[] realtime_plot_data;
        realtime_plot_data = nullptr;
    }

    std::cout << "GUI closed, initiating cleanup..." << std::endl;

    orange::gui::reap_host_ptp_stack_worker(&host_ptp_stack_ui);
    if (host_ptp_stack_ui.worker.joinable()) {
        std::cout << "Waiting for PTP stack command to finish..." << std::endl;
        host_ptp_stack_ui.worker.join();
    }

    // 1. Signal the ENet thread to stop
    quite_enet = true;

    // 2. Join the ENet thread before exiting
    if (enet_thread.joinable()) {
        std::cout << "Waiting for ENet thread to finish..." << std::endl;
        enet_thread.join();
        std::cout << "ENet thread joined successfully." << std::endl;
    }

    if (enet_server_initialized) {
        enet_release(&server);
    }
    if (enet_runtime_initialized) {
        enet_deinitialize();
    }

    // 3. Cleanup any remaining resources
    gx_cleanup(window);

    // 4. Free allocated memory
    free(window->glsl_version);
    free(window);

    std::cout << "Cleanup completed, exiting..." << std::endl;
}
