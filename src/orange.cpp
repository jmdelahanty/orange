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
#include "gui/autorun.h"
#include "gui/arena_centering_autorun.h"
#include "gui/env_util.h"
#include "gui/camera_properties_panel.h"
#include "gui/frame_ipc_panel.h"
#include "gui/guided_capture_autorun.h"
#include "gui/host_ptp_panel.h"
#include "gui/incremental_clip_shadow.h"
#include "gui/recording_finalizer.h"
#include "gui/recording_panel.h"
#include "gui/recording_snapshots.h"
#include "gui/session_status.h"
#include "gui_display_frame_rate.h"
#include "image_canvas.h"
#include "recording_output_utils.h"
#include "recording_validation.h"
#include "ruler_alignment.h"
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
#include <atomic>
#include <chrono>
#include <cmath>
#include <fstream>
#include <array>
#include <iomanip>
#include <limits>
#include <map>
#include <optional>
#include <set>
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
    uint64_t epoch = 0;  // 0 = command carried no epoch/seq (legacy)
    uint64_t seq = 0;
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
    uint64_t epoch = 0;  // 0 = command carried no epoch/seq (legacy)
    uint64_t seq = 0;
};

// Owns the single background thread that runs the external-recorder
// supervisor spawn + socket wait for a GUI recording start
// (orange::session::start_prepared_recording_run_supervisors). Owned by
// main scope; the worker is joined on completion
// (gui_poll_async_recording_start), on cancellation, and on shutdown
// (gui_cancel_async_recording_start) - never detached. The background
// thread must never touch ImGui, CameraControl, worker objects, or any
// other GUI state: it only reads `prepared`, writes `outcome`, and then
// publishes `done`; the GUI thread reads `outcome` only after observing
// `done` and joining the worker.
struct GuiAsyncRecordingStartState {
    std::thread worker;
    std::atomic<bool> done{false};
    bool active = false;
    orange::session::PreparedRecordingRunStart prepared;
    orange::session::RecordingRunSupervisorStartOutcome outcome;
    std::string context;
    std::chrono::steady_clock::time_point started_at{};
    // Deferred local-control ack bookkeeping, copied from the start request
    // at launch time (the live request state may be overwritten by a newer
    // command while this start is still pending).
    bool from_local_control = false;
    uint64_t local_control_epoch = 0;
    uint64_t local_control_seq = 0;
    std::string local_control_request_id;
    std::string local_control_operation_id;
    std::string local_control_source;
    std::string local_control_reason;
    std::string local_control_received_at_utc;
};

// Outcome of a GUI-operator recording-start request. kPending means the
// external recorder supervisors are starting on the background thread and
// the run will start (or fail loudly) at a later
// gui_poll_async_recording_start; record_video stays false until then.
enum class GuiRecordingStartDispatch {
    kFailed,
    kStarted,
    kPending,
};

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

void render_primary_video_texture(GLuint texture,
                                  const ImVec2 available_size,
                                  const float image_width,
                                  const float image_height,
                                  orange::ui::ImageCanvasViewState* canvas_view,
                                  const bool canvas_enabled,
                                  const int camera_index)
{
    if (canvas_enabled &&
        canvas_view != nullptr &&
        texture != 0 &&
        available_size.x > 1.0f &&
        available_size.y > 1.0f &&
        image_width > 0.0f &&
        image_height > 0.0f) {
        const std::string plot_id =
            "##primary_video_canvas_plot_" + std::to_string(camera_index);
        const std::string image_id =
            "##primary_video_canvas_image_" + std::to_string(camera_index);
        if (orange::ui::begin_image_canvas_sized(
                plot_id.c_str(),
                texture,
                static_cast<int>(image_width),
                static_cast<int>(image_height),
                canvas_view,
                available_size,
                image_id.c_str())) {
            ImPlot::EndPlot();
        }
        return;
    }

    render_texture_image_centered(texture, available_size, image_width, image_height);
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

bool resolve_gui_primary_video_canvas_enabled(const bool default_value)
{
    return gui_env_flag_enabled("ORANGE_GUI_PRIMARY_VIDEO_CANVAS", default_value);
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

// --- Worker fatal-error observability -------------------------------------
// Every pipeline worker derives from COffThreadMachine, whose fatal-error
// latch (HasFatalError()/GetFatalErrorMessage()) records the first exception
// caught at the worker-thread boundary. These helpers sweep the GUI-owned
// worker fleet each frame while streaming and surface any latched worker as
// a prominent red banner in the "Local" window, plus a once-per-worker
// stderr log line.

struct GuiWorkerFatalErrorEntry {
    std::string worker_name;
    std::string message;
};

void gui_collect_worker_fatal_error(
    std::vector<GuiWorkerFatalErrorEntry>* entries,
    const char* label,
    int camera_index,
    const COffThreadMachine* worker)
{
    if (!entries || !worker || !worker->HasFatalError()) {
        return;
    }
    std::string name = label;
    if (camera_index >= 0) {
        name += "[";
        name += std::to_string(camera_index);
        name += "]";
    }
    const char* thread_name = worker->GetThreadName();
    if (thread_name && thread_name[0] != '\0') {
        name += " (";
        name += thread_name;
        name += ")";
    }
    std::string message = worker->GetFatalErrorMessage();
    if (message.empty()) {
        message = "worker thread exception (no message recorded)";
    }
    if (worker->GetFatalErrorCount() > 1) {
        message += " [+";
        message += std::to_string(worker->GetFatalErrorCount() - 1);
        message += " more]";
    }
    entries->push_back({std::move(name), std::move(message)});
}

// Sweeps every live worker the GUI owns. Only call while
// camera_control->subscribe is true: the pointer arrays exist solely for the
// duration of a streaming session (allocated at stream start, deleted and
// nulled by the stop-streaming teardown before this runs in the same frame).
std::vector<GuiWorkerFatalErrorEntry> gui_collect_worker_fatal_errors(
    int num_cameras,
    COpenGLDisplay** display_workers,
    CropProducerWorker** crop_producer_workers,
    CropAndEncodeWorker** crop_and_encode_workers,
    CropPreviewWorker** crop_preview_workers,
    SpatialSnapshotWorker** spatial_snapshot_workers,
    PoseWorker** pose_workers,
    ImageWriterWorker* image_writer_worker,
    const orange::session::RecordingSessionState& recording_session)
{
    std::vector<GuiWorkerFatalErrorEntry> entries;
    for (int i = 0; i < num_cameras; ++i) {
        if (display_workers) {
            gui_collect_worker_fatal_error(
                &entries, "display", i, display_workers[i]);
        }
        if (crop_producer_workers) {
            gui_collect_worker_fatal_error(
                &entries, "crop producer", i, crop_producer_workers[i]);
        }
        if (crop_and_encode_workers) {
            gui_collect_worker_fatal_error(
                &entries, "crop encoder", i, crop_and_encode_workers[i]);
        }
        if (crop_preview_workers) {
            gui_collect_worker_fatal_error(
                &entries, "crop preview", i, crop_preview_workers[i]);
        }
        if (spatial_snapshot_workers) {
            gui_collect_worker_fatal_error(
                &entries, "spatial snapshot", i, spatial_snapshot_workers[i]);
        }
        if (pose_workers) {
            gui_collect_worker_fatal_error(
                &entries, "pose", i, pose_workers[i]);
        }
        if (static_cast<size_t>(i) < yolo_workers.size()) {
            gui_collect_worker_fatal_error(
                &entries, "yolo", i, yolo_workers[i]);
        }
    }
    gui_collect_worker_fatal_error(
        &entries, "image writer", -1, image_writer_worker);
    for (size_t i = 0; i < recording_session.recording_pipelines.size(); ++i) {
        const auto& pipeline = recording_session.recording_pipelines[i];
        if (!pipeline) {
            continue;
        }
        const int camera_index = static_cast<int>(i);
        gui_collect_worker_fatal_error(
            &entries, "encoder preprocess", camera_index,
            pipeline->preprocess_worker());
        gui_collect_worker_fatal_error(
            &entries, "encoder hw", camera_index, pipeline->hw_worker());
        for (const auto& helper : pipeline->helper_encode_targets_) {
            gui_collect_worker_fatal_error(
                &entries, "helper encoder preprocess", camera_index,
                helper.preprocess_worker.get());
            gui_collect_worker_fatal_error(
                &entries, "helper encoder hw", camera_index,
                helper.hw_worker.get());
        }
    }
    return entries;
}

// stderr log, once per newly-latched worker (not per frame). The caller
// clears `already_logged` when streaming stops so a fresh session logs anew.
void gui_note_new_worker_fatal_errors(
    std::set<std::string>* already_logged,
    const std::vector<GuiWorkerFatalErrorEntry>& entries)
{
    if (!already_logged) {
        return;
    }
    for (const auto& entry : entries) {
        if (already_logged->insert(entry.worker_name).second) {
            std::cerr << "[GUI][worker_fatal] " << entry.worker_name << ": "
                      << entry.message << std::endl;
        }
    }
}

void render_gui_worker_fatal_errors(
    const std::vector<GuiWorkerFatalErrorEntry>& entries)
{
    if (entries.empty()) {
        return;
    }
    const ImVec4 error_color{1.0f, 0.25f, 0.20f, 1.0f};
    ImGui::Separator();
    ImGui::TextColored(
        error_color,
        "WORKER FATAL ERROR: %d worker thread%s stopped processing",
        static_cast<int>(entries.size()),
        entries.size() == 1 ? "" : "s");
    ImGui::PushStyleColor(ImGuiCol_Text, error_color);
    for (const auto& entry : entries) {
        ImGui::TextWrapped(
            "  %s: %s", entry.worker_name.c_str(), entry.message.c_str());
    }
    ImGui::PopStyleColor();
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

// Set once in main(); lets the recording-start and drain-completion choke
// points advance the local-control epoch fence without threading the server
// pointer through every recording-path call signature.
orange::control::LocalControlServer* g_gui_local_control_server = nullptr;

void gui_reset_local_control_stop_scheduler_for_recording_start(
    GuiLocalControlStopSchedulerState* scheduler)
{
    if (g_gui_local_control_server) {
        // A new recording generation begins: commands stamped for the
        // previous generation must no longer be able to stop this one.
        g_gui_local_control_server->AdvanceSessionEpoch();
    }
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
            start_request->epoch = command.epoch;
            start_request->seq = command.seq;
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
        stop_scheduler->epoch = command.epoch;
        stop_scheduler->seq = command.seq;
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
        if (g_gui_local_control_server && stop_scheduler->epoch != 0) {
            g_gui_local_control_server->MarkCommandDone(
                stop_scheduler->epoch,
                stop_scheduler->seq);
        }
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
        if (static_cast<int>(cameras_params[i].width) < resolved_crop_size ||
            static_cast<int>(cameras_params[i].height) < resolved_crop_size) {
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

YoloWorker* gui_yolo_worker_at(const int camera_index)
{
    if (camera_index < 0 || camera_index >= static_cast<int>(yolo_workers.size())) {
        return nullptr;
    }
    return yolo_workers[static_cast<std::size_t>(camera_index)];
}

using GuiYoloSpatialMaskConfig = orange::analytics_mask::RuntimeConfig;

struct GuiYoloSpatialMaskConfigState {
    GuiYoloSpatialMaskConfig config;
    std::string error;
    bool initialized = false;
};

GuiYoloSpatialMaskConfigState& gui_yolo_spatial_mask_config_state()
{
    static GuiYoloSpatialMaskConfigState state;
    if (!state.initialized) {
        const auto resolved =
            orange::analytics_mask::resolve_runtime_config_from_environment();
        if (resolved.ok) {
            state.config = resolved.config;
        } else {
            state.error = resolved.error;
        }
        state.initialized = true;
    }
    return state;
}

struct GuiYoloSpatialMaskArmResult {
    bool ok = false;
    bool explicitly_enabled = false;
    std::string error;
};

bool resolve_gui_yolo_spatial_mask_config(
    GuiYoloSpatialMaskConfig* config_out,
    std::string* error_out)
{
    if (!config_out) {
        if (error_out) {
            *error_out = "internal error: null spatial mask config destination";
        }
        return false;
    }
    const GuiYoloSpatialMaskConfigState& state =
        gui_yolo_spatial_mask_config_state();
    if (!state.error.empty()) {
        if (error_out) {
            *error_out = state.error;
        }
        return false;
    }
    *config_out = state.config;
    if (error_out) {
        error_out->clear();
    }
    return true;
}

GuiYoloSpatialMaskArmResult arm_gui_yolo_spatial_masks(
    nlohmann::json* recording_geometry_contract,
    const CameraParams* cameras_params,
    const CameraEachSelect* cameras_select,
    const int num_cameras)
{
    GuiYoloSpatialMaskArmResult result;
    GuiYoloSpatialMaskConfig config;
    if (!resolve_gui_yolo_spatial_mask_config(&config, &result.error)) {
        result.explicitly_enabled =
            std::getenv("ORANGE_YOLO_SPATIAL_MASK_MODE") != nullptr;
        return result;
    }
    result.explicitly_enabled =
        config.mode != orange::analytics_mask::Mode::kOff;
    if (!recording_geometry_contract || !recording_geometry_contract->is_object() ||
        !cameras_params || !cameras_select || num_cameras <= 0) {
        result.error = "recording geometry contract or camera state is unavailable";
        return result;
    }

    nlohmann::json runtime = {
        {"schema_id", "orange.analytics.spatial_mask_recording_arm"},
        {"schema_version", 1},
        {"status", "resolving"},
        {"mode", orange::analytics_mask::mode_to_string(config.mode)},
        {"configuration_source", config.source},
        {"input_context_outset_px", config.input_context_outset_px},
        {"outside_tensor_value", 0.0},
        {"apply_timeout_ms", config.apply_timeout_ms},
        {"immutable_for_recording", true},
        {"cameras", nlohmann::json::object()},
    };

    struct PendingArm {
        int camera_index = -1;
        std::string camera_serial;
        YoloWorker* worker = nullptr;
        orange::analytics_mask::Policy policy;
        std::uint64_t generation = 0;
    };
    std::vector<PendingArm> pending;
    for (int index = 0; index < num_cameras; ++index) {
        if (!cameras_select[index].yolo) {
            continue;
        }
        PendingArm arm;
        arm.camera_index = index;
        arm.camera_serial = cameras_params[index].camera_serial.empty()
            ? std::to_string(cameras_params[index].camera_id)
            : cameras_params[index].camera_serial;
        arm.worker = gui_yolo_worker_at(index);
        if (!arm.worker) {
            result.error = "YOLO spatial mask arm has no worker for camera " +
                arm.camera_serial;
            runtime["status"] = "failed";
            runtime["error"] = result.error;
            (*recording_geometry_contract)["analytics_runtime"]
                ["yolo_spatial_mask"] = std::move(runtime);
            return result;
        }

        if (config.mode == orange::analytics_mask::Mode::kOff) {
            arm.policy.mode = config.mode;
            arm.policy.camera_serial = arm.camera_serial;
            arm.policy.source_width = cameras_params[index].width;
            arm.policy.source_height = cameras_params[index].height;
        } else {
            auto resolved =
                orange::analytics_mask::resolve_policy_from_recording_geometry_contract(
                    *recording_geometry_contract,
                    arm.camera_serial,
                    cameras_params[index].width,
                    cameras_params[index].height,
                    config.mode,
                    config.input_context_outset_px);
            if (!resolved.ok) {
                result.error = "camera " + arm.camera_serial + ": " +
                    resolved.error;
                runtime["status"] = "failed";
                runtime["error"] = result.error;
                runtime["cameras"][arm.camera_serial] = {
                    {"status", "resolution_failed"},
                    {"error", resolved.error},
                };
                (*recording_geometry_contract)["analytics_runtime"]
                    ["yolo_spatial_mask"] = std::move(runtime);
                return result;
            }
            arm.policy = std::move(resolved.policy);
        }
        runtime["cameras"][arm.camera_serial] =
            orange::analytics_mask::policy_to_json(arm.policy);
        runtime["cameras"][arm.camera_serial]["status"] = "resolved";
        pending.push_back(std::move(arm));
    }

    if (pending.empty()) {
        runtime["status"] = "not_applicable_no_yolo_workers";
        (*recording_geometry_contract)["analytics_runtime"]
            ["yolo_spatial_mask"] = std::move(runtime);
        result.ok = true;
        return result;
    }

    const auto request_off_rollback = [&]() {
        for (PendingArm& arm : pending) {
            orange::analytics_mask::Policy off;
            off.mode = orange::analytics_mask::Mode::kOff;
            off.camera_serial = arm.camera_serial;
            off.source_width = cameras_params[arm.camera_index].width;
            off.source_height = cameras_params[arm.camera_index].height;
            std::uint64_t ignored_generation = 0;
            std::string ignored_error;
            arm.worker->RequestSpatialMaskPolicy(
                off, &ignored_generation, &ignored_error);
        }
    };

    for (PendingArm& arm : pending) {
        std::string request_error;
        if (!arm.worker->RequestSpatialMaskPolicy(
                arm.policy, &arm.generation, &request_error)) {
            result.error = "camera " + arm.camera_serial +
                " rejected spatial mask policy: " + request_error;
            runtime["status"] = "failed";
            runtime["error"] = result.error;
            request_off_rollback();
            (*recording_geometry_contract)["analytics_runtime"]
                ["yolo_spatial_mask"] = std::move(runtime);
            return result;
        }
    }
    for (PendingArm& arm : pending) {
        std::string wait_error;
        if (!arm.worker->WaitForSpatialMaskPolicy(
                arm.generation,
                std::chrono::milliseconds(config.apply_timeout_ms),
                &wait_error)) {
            result.error = "camera " + arm.camera_serial +
                " did not apply spatial mask policy: " + wait_error;
            runtime["status"] = "failed";
            runtime["error"] = result.error;
            request_off_rollback();
            (*recording_geometry_contract)["analytics_runtime"]
                ["yolo_spatial_mask"] = std::move(runtime);
            return result;
        }
        runtime["cameras"][arm.camera_serial]["status"] = "armed";
        runtime["cameras"][arm.camera_serial]["policy_generation"] =
            arm.generation;
        (*recording_geometry_contract)["cameras"][arm.camera_serial]
            ["yolo_spatial_mask_runtime"] =
                runtime["cameras"][arm.camera_serial];
        if (arm.policy.mode != orange::analytics_mask::Mode::kOff) {
            nlohmann::json& daily_entry =
                (*recording_geometry_contract)["cameras"][arm.camera_serial]
                    ["daily_registration_geometry"]["recording_snapshot_entry"];
            daily_entry["active_in_orange_live_detection_pipeline"] =
                orange::analytics_mask::enforces_centroid(arm.policy.mode);
            daily_entry["orange_live_detection_pipeline_mode"] =
                orange::analytics_mask::mode_to_string(arm.policy.mode);
            daily_entry["active_in_orange_neural_input_mask"] =
                orange::analytics_mask::masks_input(arm.policy.mode);
        }
    }
    runtime["status"] = "armed";
    runtime["armed_camera_count"] = pending.size();
    (*recording_geometry_contract)["analytics_runtime"]
        ["yolo_spatial_mask"] = std::move(runtime);
    result.ok = true;
    return result;
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

namespace {

// Shared failure reporting for the synchronous and asynchronous recording
// start paths; mirrors the reporting the pre-async operator path did inline.
void gui_log_recording_start_failure(
    const orange::session::RecordingRunStartResult& start_result,
    std::vector<std::string>* recording_preflight_errors)
{
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
}

}  // namespace

// GUI-side completion of a successful recording-run start: worker folder
// rotation, per-run snapshots, timers, and run-state bookkeeping. Runs on
// the GUI thread only (touches worker objects and GUI state), either
// directly after a synchronous start or from gui_poll_async_recording_start
// once the background supervisor start completed.
void gui_finish_recording_start_through_operator_path(
    const orange::session::RecordingRunStartResult& start_result,
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
    CropProducerWorker** crop_producer_workers,
    CropPreviewWorker** crop_preview_workers,
    CropAndEncodeWorker** crop_and_encode_workers,
    PoseWorker** pose_workers,
    const std::string& context)
{
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
    update_gui_citrus_runtime_geometry_snapshot(resolved_recording_folder);
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
}

GuiRecordingStartDispatch gui_request_recording_start_through_operator_path(
    GuiAsyncRecordingStartState* async_start,
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
    const std::string& citrus_canvas_config_path,
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
        return GuiRecordingStartDispatch::kFailed;
    }
    if (async_start && async_start->active) {
        // Debounce: a background recorder start is already in flight; the
        // pending window resolves through gui_poll_async_recording_start.
        if (recording_preflight_errors) {
            *recording_preflight_errors = {
                "A recording start is already in progress"
                " (external recorder starting)."};
        }
        std::cerr << "[GUI][recording] Start ignored: a recording start is already pending"
                  << " context=" << context << std::endl;
        return GuiRecordingStartDispatch::kFailed;
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
        return GuiRecordingStartDispatch::kFailed;
    }

    if (recording_preflight_errors) {
        recording_preflight_errors->clear();
    }
    const std::string output_root =
        encoder_config && !encoder_config->folder_name.empty()
            ? encoder_config->folder_name
            : input_folder;
    orange::session::PreparedRecordingRunStart prepared =
        orange::session::prepare_recording_run(
            recording_session,
            camera_control,
            cameras_params,
            cameras_select,
            num_cameras,
            output_root,
            ptp_params,
            recording_session->recording_sink_mode,
            &recording_session->external_recorder_contract_config);
    if (!prepared.valid) {
        orange::session::RecordingRunStartResult failed_result;
        failed_result.recording_folder = prepared.recording_folder;
        failed_result.recording_sink_mode = prepared.recording_sink_mode;
        failed_result.error_message = prepared.error_message;
        failed_result.external_recorder_contract_path =
            prepared.external_recorder_contract_path;
        gui_log_recording_start_failure(failed_result, recording_preflight_errors);
        return GuiRecordingStartDispatch::kFailed;
    }

    nlohmann::json recording_geometry_contract =
        build_gui_recording_geometry_contract(
        citrus_canvas_config_path,
        cameras_params,
        cameras_select,
        num_cameras);
    GuiYoloSpatialMaskArmResult spatial_mask_arm =
        arm_gui_yolo_spatial_masks(
            &recording_geometry_contract,
            cameras_params,
            cameras_select,
            num_cameras);
    if (!spatial_mask_arm.ok &&
        !recording_geometry_contract.contains("analytics_runtime")) {
        recording_geometry_contract["analytics_runtime"]
            ["yolo_spatial_mask"] = {
                {"schema_id", "orange.analytics.spatial_mask_recording_arm"},
                {"schema_version", 1},
                {"status", "failed"},
                {"error", spatial_mask_arm.error},
            };
    }
    std::string geometry_write_error;
    const bool geometry_written = write_gui_recording_geometry_contract(
        prepared.recording_folder,
        recording_geometry_contract,
        &geometry_write_error);
    if (!geometry_written) {
        std::cerr << "[GUI][recording] Failed to write recording geometry contract: "
                  << geometry_write_error << std::endl;
    }
    if (!spatial_mask_arm.ok ||
        (spatial_mask_arm.explicitly_enabled && !geometry_written)) {
        std::string error = spatial_mask_arm.ok
            ? "Spatial masking was armed, but its recording geometry contract "
              "could not be persisted: " + geometry_write_error
            : "YOLO spatial mask arm failed: " + spatial_mask_arm.error;
        orange::session::abort_prepared_recording_run(
            recording_session,
            camera_control,
            prepared,
            orange::session::RecordingRunSupervisorStartOutcome{},
            error);
        if (recording_preflight_errors) {
            *recording_preflight_errors = {error};
        }
        std::cerr << "[GUI][recording] Start rejected: " << error << std::endl;
        return GuiRecordingStartDispatch::kFailed;
    }

    if (!async_start || !prepared.requires_supervisor_start()) {
        // No external recorder processes to wait for (or no async runner):
        // keep the synchronous behavior, which is fast for in-process sinks.
        orange::session::RecordingRunSupervisorStartOutcome outcome =
            orange::session::start_prepared_recording_run_supervisors(prepared);
        const orange::session::RecordingRunStartResult start_result =
            orange::session::complete_recording_run(
                recording_session,
                camera_control,
                cameras_params,
                num_cameras,
                ptp_params,
                prepared,
                std::move(outcome));
        if (!start_result.ok) {
            gui_log_recording_start_failure(start_result, recording_preflight_errors);
            return GuiRecordingStartDispatch::kFailed;
        }
        gui_finish_recording_start_through_operator_path(
            start_result,
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
            crop_producer_workers,
            crop_preview_workers,
            crop_and_encode_workers,
            pose_workers,
            context);
        return GuiRecordingStartDispatch::kStarted;
    }

    async_start->done.store(false, std::memory_order_release);
    async_start->active = true;
    async_start->prepared = std::move(prepared);
    async_start->outcome = orange::session::RecordingRunSupervisorStartOutcome{};
    async_start->context = context;
    async_start->started_at = std::chrono::steady_clock::now();
    async_start->from_local_control = false;
    GuiAsyncRecordingStartState* worker_state = async_start;
    async_start->worker = std::thread([worker_state]() {
        // Thread boundary (docs/error_handling_convention.md): an exception
        // escaping a raw std::thread would std::terminate the whole process.
        // Catch, report through the outcome slot, and let the GUI poll
        // surface the failure loudly.
        try {
            worker_state->outcome =
                orange::session::start_prepared_recording_run_supervisors(
                    worker_state->prepared);
        } catch (const std::exception& ex) {
            worker_state->outcome =
                orange::session::RecordingRunSupervisorStartOutcome{};
            worker_state->outcome.error_message =
                std::string("external recorder start threw: ") + ex.what();
        } catch (...) {
            worker_state->outcome =
                orange::session::RecordingRunSupervisorStartOutcome{};
            worker_state->outcome.error_message =
                "external recorder start threw a non-std exception";
        }
        worker_state->done.store(true, std::memory_order_release);
    });
    std::cout << "[GUI][recording] External recorder start pending"
              << " context=" << context
              << " sink_mode=" << async_start->prepared.recording_sink_mode
              << " folder=" << async_start->prepared.recording_folder
              << std::endl;
    return GuiRecordingStartDispatch::kPending;
}

// Polled once per GUI frame. When the background supervisor start finishes,
// joins the worker and completes (or fails) the run on the GUI thread:
// record_video flips true here, workers rotate folders here, and any
// deferred local-control ack fires here. Returns true when a pending start
// completed successfully this frame.
bool gui_poll_async_recording_start(
    GuiAsyncRecordingStartState* async_start,
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
    PTPParams* ptp_params,
    CropProducerWorker** crop_producer_workers,
    CropPreviewWorker** crop_preview_workers,
    CropAndEncodeWorker** crop_and_encode_workers,
    PoseWorker** pose_workers,
    std::vector<std::string>* recording_preflight_errors,
    const std::string& event_log_path)
{
    if (!async_start || !async_start->active ||
        !async_start->done.load(std::memory_order_acquire)) {
        return false;
    }
    if (async_start->worker.joinable()) {
        async_start->worker.join();
    }
    async_start->active = false;

    const orange::session::RecordingRunStartResult start_result =
        orange::session::complete_recording_run(
            recording_session,
            camera_control,
            cameras_params,
            num_cameras,
            ptp_params,
            async_start->prepared,
            std::move(async_start->outcome));
    const bool started = start_result.ok;
    if (started) {
        gui_finish_recording_start_through_operator_path(
            start_result,
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
            crop_producer_workers,
            crop_preview_workers,
            crop_and_encode_workers,
            pose_workers,
            async_start->context);
    } else {
        gui_log_recording_start_failure(start_result, recording_preflight_errors);
        std::cerr << "[GUI][recording] Recording start failed"
                  << " context=" << async_start->context << std::endl;
    }

    if (async_start->from_local_control) {
        // Deferred local-control ack: the epoch fence (MarkCommandDone) and
        // the start_triggered event advance only now that the start actually
        // completed; a failed start reports start_failed instead, exactly
        // like the synchronous failure path.
        if (started && g_gui_local_control_server &&
            async_start->local_control_epoch != 0) {
            g_gui_local_control_server->MarkCommandDone(
                async_start->local_control_epoch,
                async_start->local_control_seq);
        }
        gui_note_local_control_start_event(
            start_request,
            started ? "start_triggered" : "start_failed");
        std::cout << "[GUI][local_control] recording start "
                  << (started ? "triggered" : "failed")
                  << " request_id=" << async_start->local_control_request_id
                  << " operation_id=" << async_start->local_control_operation_id
                  << std::endl;
        gui_log_local_control_event(
            event_log_path,
            {
                {"event", started ? "recording_start_triggered"
                                  : "recording_start_failed"},
                {"method", "start_recording"},
                {"request_id", async_start->local_control_request_id},
                {"operation_id", async_start->local_control_operation_id},
                {"command_source", async_start->local_control_source},
                {"reason", async_start->local_control_reason},
                {"received_at_utc", async_start->local_control_received_at_utc},
            });
    }

    async_start->prepared = orange::session::PreparedRecordingRunStart{};
    async_start->outcome = orange::session::RecordingRunSupervisorStartOutcome{};
    async_start->from_local_control = false;
    return started;
}

// Cancels a pending background recording start: joins the worker (bounded
// by the supervisor's socket-ready timeout plus its fast-fail cleanup),
// stops any recorder processes it spawned, and restores the pre-start
// CameraControl/session state without ever flipping record_video. Must run
// before worker teardown in stop_streaming_and_teardown and before shutdown.
void gui_cancel_async_recording_start(
    GuiAsyncRecordingStartState* async_start,
    GuiLocalControlStartRequestState* start_request,
    orange::session::RecordingSessionState* recording_session,
    CameraControl* camera_control,
    const std::string& reason,
    const std::string& event_log_path)
{
    if (!async_start) {
        return;
    }
    if (!async_start->active) {
        if (async_start->worker.joinable()) {
            async_start->worker.join();
        }
        return;
    }
    std::cout << "[GUI][recording] Canceling pending recording start"
              << " reason=" << reason << std::endl;
    if (async_start->worker.joinable()) {
        async_start->worker.join();
    }
    async_start->active = false;
    orange::session::abort_prepared_recording_run(
        recording_session,
        camera_control,
        async_start->prepared,
        std::move(async_start->outcome),
        reason);
    if (async_start->from_local_control) {
        gui_note_local_control_start_event(start_request, "start_failed");
        std::cout << "[GUI][local_control] recording start canceled"
                  << " request_id=" << async_start->local_control_request_id
                  << " operation_id=" << async_start->local_control_operation_id
                  << " reason=" << reason
                  << std::endl;
        gui_log_local_control_event(
            event_log_path,
            {
                {"event", "recording_start_failed"},
                {"method", "start_recording"},
                {"request_id", async_start->local_control_request_id},
                {"operation_id", async_start->local_control_operation_id},
                {"command_source", async_start->local_control_source},
                {"reason", reason},
                {"received_at_utc", async_start->local_control_received_at_utc},
            });
    }
    async_start->prepared = orange::session::PreparedRecordingRunStart{};
    async_start->outcome = orange::session::RecordingRunSupervisorStartOutcome{};
    async_start->from_local_control = false;
}

bool gui_poll_local_control_start_request(
    GuiLocalControlStartRequestState* start_request,
    GuiAsyncRecordingStartState* async_start,
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
    const std::string& citrus_canvas_config_path,
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
    if (async_start && async_start->active) {
        reject_start("ignored_start_pending", "recording_start_pending");
        return false;
    }
    if (camera_control->recording_draining ||
        (recording_run && recording_run->finalizing)) {
        reject_start("ignored_recording_finalizing", "recording_finalizing");
        return false;
    }
    if (recording_run && recording_run->active) {
        // Defense-in-depth: an active run that never finalized
        // (record_video already false, finalizing never latched). The
        // worker-stop reconciler normally routes this into finalization
        // within a frame; starting here would reuse the previous run's
        // still-claimed recording folder.
        reject_start("ignored_previous_run_active", "previous_recording_run_active");
        return false;
    }

    const GuiRecordingStartDispatch dispatch =
        gui_request_recording_start_through_operator_path(
            async_start,
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
            citrus_canvas_config_path,
            ptp_params,
            crop_producer_workers,
            crop_preview_workers,
            crop_and_encode_workers,
            pose_workers,
            recording_preflight_errors,
            "local_control_start_recording");

    if (dispatch == GuiRecordingStartDispatch::kPending) {
        // The start is running on the background thread. Defer the epoch
        // fence (MarkCommandDone) and the start_triggered/start_failed
        // resolution to gui_poll_async_recording_start /
        // gui_cancel_async_recording_start; copy the ack identity now
        // because the live request state may be reused by a newer command.
        async_start->from_local_control = true;
        async_start->local_control_epoch = start_request->epoch;
        async_start->local_control_seq = start_request->seq;
        async_start->local_control_request_id = request_id;
        async_start->local_control_operation_id = operation_id;
        async_start->local_control_source = start_request->source;
        async_start->local_control_reason = start_request->reason;
        async_start->local_control_received_at_utc = start_request->received_at_utc;
        gui_note_local_control_start_event(start_request, "start_pending");
        std::cout << "[GUI][local_control] recording start pending"
                  << " request_id=" << request_id
                  << " operation_id=" << operation_id
                  << std::endl;
        gui_log_local_control_event(
            event_log_path,
            {
                {"event", "recording_start_pending"},
                {"method", "start_recording"},
                {"request_id", request_id},
                {"operation_id", operation_id},
                {"command_source", start_request->source},
                {"reason", start_request->reason},
                {"received_at_utc", start_request->received_at_utc},
            });
        // The start was accepted; report true so the caller suppresses a
        // same-frame autorun toggle, exactly as for a synchronous start.
        return true;
    }

    const bool started = dispatch == GuiRecordingStartDispatch::kStarted;
    if (started && g_gui_local_control_server && start_request->epoch != 0) {
        g_gui_local_control_server->MarkCommandDone(
            start_request->epoch,
            start_request->seq);
    }
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

void RenderSpeedGraph(int /*camera_id*/, YoloWorker* yolo_worker, SpeedTrackingData& speed_data) {
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

int main(int /*argc*/, char ** /*args*/) {

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
    if (app_storage_config.gui_incremental_clip_shadow) {
        set_gui_env_from_app_config_if_absent(
            "ORANGE_GUI_INCREMENTAL_CLIP_SHADOW",
            "1",
            "incremental clip shadow mode");
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

    // GLFW/window helpers throw on failure (library code never exits);
    // main() owns the process-exit decision.
    try {
        render_initialize_target(window);
    } catch (const std::exception& ex) {
        std::cerr << "FATAL: display initialization failed: " << ex.what() << std::endl;
        return EXIT_FAILURE;
    }

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
    const orange::gui::GuidedCaptureAutorunConfig guided_capture_autorun_config =
        orange::gui::resolve_guided_capture_autorun_config();
    if (guided_capture_autorun_config.enabled) {
        std::cout << "[GUI][guided_capture_autorun] enabled"
                  << " citrus_config="
                  << guided_capture_autorun_config.citrus_config_path
                  << " profile="
                  << guided_capture_autorun_config.workflow_profile_id
                  << " recipe=" << guided_capture_autorun_config.recipe
                  << " purpose=" << guided_capture_autorun_config.purpose
                  << " cameras="
                  << guided_capture_autorun_config.camera_serials.size()
                  << " frame_count="
                  << guided_capture_autorun_config.frame_count
                  << " sweep_levels="
                  << guided_capture_autorun_config.sweep_foreground_grays_u8.size()
                  << " sweep_repeats="
                  << guided_capture_autorun_config.sweep_repeats
                  << " save="
                  << guided_capture_autorun_config.save_captures
                  << " result="
                  << guided_capture_autorun_config.result_json_path
                  << std::endl;
    }
    const orange::gui::ArenaCenteringAutorunConfig arena_centering_autorun_config =
        orange::gui::resolve_arena_centering_autorun_config();
    if (guided_capture_autorun_config.enabled &&
        arena_centering_autorun_config.enabled) {
        std::cerr << "[GUI] guided capture sweep and arena-centering autorun "
                     "cannot own the calibration scene simultaneously."
                  << std::endl;
        return EXIT_FAILURE;
    }
    if (arena_centering_autorun_config.enabled) {
        std::cout << "[GUI][arena_centering] enabled"
                  << " citrus_config="
                  << arena_centering_autorun_config.citrus_config_path
                  << " cameras="
                  << arena_centering_autorun_config.camera_serials.size()
                  << " probe_canvas_px="
                  << arena_centering_autorun_config.symmetric_probe_canvas_px
                  << " tolerance_camera_px="
                  << arena_centering_autorun_config.verification_tolerance_camera_px
                  << " save_verified_centers_armed="
                  << arena_centering_autorun_config.save_verified_centers_armed
                  << " fit_homographies="
                  << arena_centering_autorun_config.fit_homographies_after_centering
                  << " accept_homographies_armed="
                  << arena_centering_autorun_config.accept_homographies_armed
                  << " result="
                  << arena_centering_autorun_config.result_json_path
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
    bool primary_video_canvas_enabled =
        resolve_gui_primary_video_canvas_enabled(false);
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
    std::vector<orange::ui::ImageCanvasViewState> primary_video_canvas_views;
    PoseWorker** poseWorkers = nullptr;
    ImageWriterWorker* image_writer = new ImageWriterWorker("ImageSaverThread");
    image_writer->StartThread();
    std::set<std::string> gui_worker_fatal_errors_logged; // once-per-worker stderr log

    std::vector<CameraResources> camera_resources;
    orange::session::RecordingSessionState recording_session;
    GuiSessionTimingState gui_session_timing;
    GuiRecordingRunState gui_recording_run;
    // Background external-recorder start runner (joined on completion,
    // cancellation, and shutdown; see GuiAsyncRecordingStartState).
    GuiAsyncRecordingStartState gui_async_recording_start;
    // Background recording-finalize runner (recorder stops + summary reads +
    // manifest/snapshot writes off the render thread). Joined on completion
    // by the per-frame gui_poll_async_recording_finalize, and on
    // teardown/shutdown by gui_join_async_recording_finalize - never
    // detached (see GuiAsyncRecordingFinalizeState in gui/recording_finalizer.h).
    GuiAsyncRecordingFinalizeState gui_async_recording_finalize;
    // Shadow-mode incremental clip splitter (stage 4, gated by
    // ORANGE_GUI_INCREMENTAL_CLIP_SHADOW; default OFF = zero new behavior,
    // zero new threads). Started with an external-IPC rolling recording,
    // fed per-frame completion watermarks after the recorder lifecycle
    // refresh, and joined the frame the run enters finalizing and on
    // teardown/shutdown - never detached (see
    // GuiIncrementalClipShadowState in gui/incremental_clip_shadow.h).
    GuiIncrementalClipShadowState gui_incremental_clip_shadow;
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
    g_gui_local_control_server = &gui_local_control_server;
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
    // Orderly stop-streaming teardown, shared by the "Stop streaming"
    // button and the window-close shutdown path. Must be defined after
    // every local it captures by reference and called synchronously from
    // this scope only.
    auto stop_streaming_and_teardown = [&]() {
        // A recording start may still be pending on the background thread;
        // join and abort it BEFORE any worker teardown so the thread never
        // outlives the objects the completed run would have touched, and so
        // no recorder children leak from a half-started run.
        gui_cancel_async_recording_start(
            &gui_async_recording_start,
            &gui_local_control_start_request,
            &recording_session,
            camera_control,
            "stream_shutdown",
            gui_local_control_event_log_path);
        // The shadow-mode incremental clip splitter (if running) must stop
        // BEFORE the finalize sequence below: the finalize prepare phase
        // moves the recorder lifecycle states and the authoritative split
        // writes into the same clip folders the shadow worker appends to.
        // Prompt no-op when idle or the flag is off.
        gui_join_incremental_clip_shadow(&gui_incremental_clip_shadow);
        // A recording finalize may still be running on the background
        // thread; join it and complete it BEFORE any pipeline/worker
        // teardown (blocking here is deliberate - this path already waits):
        // the prepare phase handed the recorder lifecycle objects to the
        // finalize state and the background phase may be mid-write.
        if (gui_join_async_recording_finalize(
                &gui_async_recording_finalize,
                &gui_recording_run,
                &recording_session,
                camera_control)) {
            gui_display_frame_rate_stats.Finish();
            gui_mark_local_control_drain_completed(
                &gui_local_control_stop_scheduler,
                gui_local_control_event_log_path,
                gui_recording_run.recording_folder);
            std::cout << "[GUI][recording] Finalized recording session before"
                         " stream teardown." << std::endl;
        }
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
        primary_video_canvas_views.clear();

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
    };
    GuiAutorunState gui_autorun_state;
    if (gui_autorun_config.enabled) {
        gui_autorun_enter_stage(&gui_autorun_state, GuiAutorunStage::kSelectConfig);
    }
    orange::gui::GuidedCaptureAutorunState guided_capture_autorun_state;
    orange::gui::guided_capture_autorun_start(
        &guided_capture_autorun_state,
        guided_capture_autorun_config);
    orange::gui::ArenaCenteringAutorunState arena_centering_autorun_state;
    orange::gui::arena_centering_autorun_start(
        &arena_centering_autorun_state,
        arena_centering_autorun_config);

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
        // Shadow-mode incremental clip splitter
        // (ORANGE_GUI_INCREMENTAL_CLIP_SHADOW): starts with an external-IPC
        // rolling recording and is fed the crop recorder status snapshots
        // refreshed just above. The join lives directly before
        // gui_poll_async_recording_finalize below (the only place the
        // finalize prepare phase can launch this frame). With the flag off
        // every call below is a cheap no-op.
        if (!gui_recording_run.finalizing) {
            gui_maybe_start_incremental_clip_shadow(
                &gui_incremental_clip_shadow,
                recording_session.external_crop_recorder_lifecycle,
                gui_recording_run.recording_folder,
                camera_control->record_video &&
                    !camera_control->recording_draining);
            gui_push_incremental_clip_shadow_watermarks(
                &gui_incremental_clip_shadow,
                recording_session.external_crop_recorder_lifecycle);
        }
        gui_poll_async_recording_start(
            &gui_async_recording_start,
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
            ptp_params,
            cropProducerWorkers,
            cropPreviewWorkers,
            cropAndEncodeWorkers,
            poseWorkers,
            &recording_preflight_errors,
            gui_local_control_event_log_path);
        // Worker-initiated stop reconciliation. Pipeline workers may stop a
        // recording themselves (recording.fail_on_drop in acquire_frames /
        // EncoderPreprocessWorker flips record_video false and latches the
        // drain flags) but must stay GUI-agnostic, so they never mark the
        // GUI run finalizing. Without this the finalize gate (whose first
        // term is run->finalizing) never opens, the CameraControl folder
        // claim stays latched, and the next start would reuse the previous
        // run's folder. Detect that state here - after the async-start poll
        // so a start completing this frame is never misread as a stop, and
        // before the ImGui draw so the finalize gate poll later this same
        // frame can launch the finalize - and route it through the exact
        // operator stop path.
        if (gui_detect_externally_requested_stop(
                camera_control,
                &gui_recording_run,
                gui_async_recording_start.active)) {
            std::string worker_stop_reason;
            {
                std::lock_guard<std::mutex> stop_reason_lock(
                    camera_control->recording_folder_mutex);
                worker_stop_reason.swap(camera_control->worker_stop_reason);
            }
            const std::string external_stop_reason =
                worker_stop_reason.empty() ? "external_stop" : worker_stop_reason;
            std::cerr << "[GUI][recording] Worker-initiated recording stop"
                         " detected; routing the run into finalization."
                      << " stop_reason=" << external_stop_reason
                      << " folder=" << gui_recording_run.recording_folder
                      << std::endl;
            gui_request_recording_stop_through_operator_path(
                &recording_session,
                camera_control,
                &gui_recording_run,
                &gui_session_timing,
                external_stop_reason);
        }
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
        const orange::gui::GuidedCaptureAutorunRequests guided_capture_requests =
            orange::gui::guided_capture_autorun_update(
                &guided_capture_autorun_state,
                guided_capture_autorun_config,
                &spatial_layout_ui_state,
                camera_control,
                ecams,
                cameras_params,
                cameras_select,
                num_cameras,
                spatialSnapshotWorkers,
                spatial_calibration_sessions_folder);
        const orange::gui::ArenaCenteringAutorunRequests arena_centering_requests =
            orange::gui::arena_centering_autorun_update(
                &arena_centering_autorun_state,
                arena_centering_autorun_config,
                &spatial_layout_ui_state,
                camera_control,
                ecams,
                cameras_params,
                cameras_select,
                num_cameras,
                spatialSnapshotWorkers,
                spatial_calibration_sessions_folder);
        gui_autorun_requests.toggle_streaming =
            gui_autorun_requests.toggle_streaming ||
            guided_capture_requests.toggle_streaming ||
            arena_centering_requests.toggle_streaming;
        gui_autorun_requests.close_window =
            gui_autorun_requests.close_window ||
            guided_capture_requests.close_window ||
            arena_centering_requests.close_window;
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
                    &gui_async_recording_start,
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
                    spatial_layout_ui_state.citrus_canvas_config_path,
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

            {
                GuiYoloSpatialMaskConfigState& spatial_mask_state =
                    gui_yolo_spatial_mask_config_state();
                static constexpr const char* spatial_mask_modes[] = {
                    "Off",
                    "Audit only",
                    "Centroid gate",
                    "Centroid gate + fused input mask",
                };
                int mode_index = static_cast<int>(
                    spatial_mask_state.config.mode);
                ImGui::BeginDisabled(
                    camera_control->record_video ||
                    gui_async_recording_start.active);
                if (ImGui::Combo(
                        "YOLO dish spatial policy",
                        &mode_index,
                        spatial_mask_modes,
                        IM_ARRAYSIZE(spatial_mask_modes))) {
                    spatial_mask_state.config.mode =
                        static_cast<orange::analytics_mask::Mode>(mode_index);
                    spatial_mask_state.config.source = "gui";
                    spatial_mask_state.error.clear();
                }
                if (orange::analytics_mask::masks_input(
                        spatial_mask_state.config.mode)) {
                    float outset =
                        spatial_mask_state.config.input_context_outset_px;
                    if (ImGui::InputFloat(
                            "Neural input context outset (camera px)",
                            &outset,
                            1.0f,
                            10.0f,
                            "%.1f")) {
                        spatial_mask_state.config.input_context_outset_px =
                            std::clamp(outset, 0.0f, 10000.0f);
                        spatial_mask_state.config.source = "gui";
                        spatial_mask_state.error.clear();
                    }
                }
                ImGui::EndDisabled();
                ImGui::TextDisabled(
                    "Resolved from the exact selected daily rim at recording arm; "
                    "full-frame recording is unchanged.");
                if (!spatial_mask_state.error.empty()) {
                    ImGui::TextColored(
                        ImVec4(0.95f, 0.35f, 0.30f, 1.0f),
                        "%s",
                        spatial_mask_state.error.c_str());
                }
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
            ImGui::Checkbox("zoom/pan primary video", &primary_video_canvas_enabled);
            
   
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
                                if (camera_control->subscribe && static_cast<size_t>(i) < yolo_workers.size() && yolo_workers[i])
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
            if (static_cast<int>(primary_video_canvas_views.size()) < num_cameras) {
                primary_video_canvas_views.resize(num_cameras);
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
                    if (static_cast<size_t>(local_config_select) < local_config_folders.size()) {
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
                            // CameraResources::initialize and the worker
                            // constructors below throw on CUDA failures;
                            // covered by the pipeline-construction try/catch.
                            try {
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
                            // Create worker thread objects and GPU textures.
                            // Worker constructors (YoloWorker, CropAndEncodeWorker, ...)
                            // and CHECK() throw on CUDA/TensorRT failures; library code
                            // never exits. Catch construction-time throws here at the
                            // pipeline construction boundary: print and exit nonzero
                            // (exiting from main is fine).
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
                            primary_video_canvas_views.assign(
                                num_cameras,
                                {});
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
                            } catch (const std::exception& ex) {
                                std::cerr << "FATAL: pipeline construction failed: "
                                          << ex.what() << std::endl;
                                return 1;
                            }

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
                                // Thread boundary (docs/error_handling_convention.md):
                                // an exception escaping a raw std::thread would
                                // std::terminate the whole process. Catch, log
                                // loudly, and let the thread exit cleanly so
                                // other cameras keep streaming and recordings
                                // can still be finalized.
                                CameraEmergent* acquire_ecam = &ecams[i];
                                CameraParams* acquire_params = &cameras_params[i];
                                CameraEachSelect* acquire_select = &cameras_select[i];
                                INDIGOSignalBuilder* acquire_indigo = &indigo_signal_builder;
                                COpenGLDisplay* acquire_display = openGLDisplayWorkers[i];
                                RecordingIngress* acquire_ingress =
                                    orange::session::recording_ingress_for_camera(recording_session, i);
                                YoloWorker* acquire_yolo = yolo_workers[i];
                                CameraResources* acquire_resources = &camera_resources[i];
                                FrameIPCManager* acquire_ipc = frame_ipc_managers[i].get();
                                SpatialSnapshotWorker* acquire_snapshot =
                                    spatialSnapshotWorkers ? spatialSnapshotWorkers[i] : nullptr;
                                camera_threads.emplace_back(
                                    [=]() {
                                        try {
                                            acquire_frames(
                                                acquire_ecam,
                                                acquire_params,
                                                acquire_select,
                                                camera_control,
                                                ptp_params,
                                                acquire_indigo,
                                                acquire_display,
                                                acquire_ingress,
                                                acquire_yolo,
                                                image_writer,
                                                acquire_resources,
                                                acquire_ipc,
                                                nullptr,
                                                acquire_snapshot);
                                        } catch (const std::exception& ex) {
                                            std::cerr << "[FATAL] acquisition thread for camera "
                                                      << acquire_params->camera_serial
                                                      << " failed: " << ex.what() << std::endl;
                                        } catch (...) {
                                            std::cerr << "[FATAL] acquisition thread for camera "
                                                      << acquire_params->camera_serial
                                                      << " failed with a non-std exception" << std::endl;
                                        }
                                    });
                            }
                        }
                    } else {
                        stop_streaming_and_teardown();
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
                    if (gui_async_recording_start.active) {
                        // Debounce: a background external-recorder start is
                        // still pending; ignore the press until it resolves.
                        std::cout << "Recording start already in progress"
                                     " (external recorder starting). Please wait..."
                                  << std::endl;
                    } else if (!camera_control->record_video &&
                               (camera_control->recording_draining ||
                                gui_recording_run.finalizing ||
                                gui_async_recording_finalize.active)) {
                        // The finalize gate clears recording_draining before
                        // the background finalize phase runs, so the drain
                        // flag alone no longer covers the whole window: also
                        // reject while the run is still finalizing (latched
                        // until the completion phase, including across
                        // finalize-failure retries) or the background
                        // finalize worker is in flight. Without this a press
                        // here could start a new run into the still-claimed
                        // recording folder.
                        std::cout << "Recording is still draining/finalizing."
                                     " Please wait..." << std::endl;
                        // Surface the rejection in the GUI: stdout alone made the
                        // play button look dead while a drain was latched.
                        recording_preflight_errors = {
                            "Recording start rejected: the previous recording is still "
                            "draining or finalizing. Wait for finalization to complete "
                            "(see session status below); if this state persists, the "
                            "external recorder may not be draining."};
                    } else if (!camera_control->record_video &&
                               gui_recording_run.active) {
                        // Defense-in-depth: an active run that never
                        // finalized (record_video already false, but the run
                        // was never routed into finalization). The
                        // worker-stop reconciler earlier this frame normally
                        // makes this state transient (it marks the run
                        // finalizing, caught by the branch above), so this
                        // guards the reconciler's one-frame window and any
                        // future leak. Starting here would reuse the
                        // previous run's still-claimed recording folder.
                        std::cout << "Recording start rejected: previous"
                                     " recording run never finalized."
                                  << std::endl;
                        recording_preflight_errors = {
                            "Recording start rejected: the previous recording run is "
                            "still active and has not finalized. Wait one frame for "
                            "the stop reconciler to route it into finalization; if "
                            "this state persists, report it as a bug."};
                    } else {
                        const bool next_record_state = !camera_control->record_video;
                        if (next_record_state) {
                            const GuiRecordingStartDispatch record_start_dispatch =
                                gui_request_recording_start_through_operator_path(
                                    &gui_async_recording_start,
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
                                    spatial_layout_ui_state.citrus_canvas_config_path,
                                    ptp_params,
                                    cropProducerWorkers,
                                    cropPreviewWorkers,
                                    cropAndEncodeWorkers,
                                    poseWorkers,
                                    &recording_preflight_errors,
                                    gui_autorun_requests.toggle_recording
                                        ? "autorun_start_recording"
                                        : "gui_start_recording");
                            if (record_start_dispatch ==
                                GuiRecordingStartDispatch::kStarted) {
                                std::cout << "Recording toggled ON." << std::endl;
                            } else if (record_start_dispatch ==
                                       GuiRecordingStartDispatch::kPending) {
                                std::cout << "Recording start pending:"
                                             " external recorder starting..."
                                          << std::endl;
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

            // The shadow-mode incremental clip splitter (if running) is
            // signaled and JOINED the frame the run enters finalizing -
            // directly before the poll below, the only place the finalize
            // prepare phase (F1) can launch this frame - so the worker is
            // gone before F1 moves the recorder lifecycle states and before
            // the authoritative F2 split touches the same clip folders.
            // Prompt no-op when idle or the flag is off.
            if (gui_recording_run.finalizing) {
                gui_join_incremental_clip_shadow(&gui_incremental_clip_shadow);
            }
            // Phased finalize: launches the background finalize the frame
            // the drain gate opens and completes it (GUI-thread completion
            // phase) the frame the worker finishes; true exactly when a
            // finalize completed successfully this frame, matching the old
            // synchronous gui_finalize_recording_session_if_ready contract.
            if (gui_poll_async_recording_finalize(
                    &gui_async_recording_finalize,
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
                    gui_session_timing_snapshot(
                        &gui_session_timing,
                        camera_control,
                        gui_async_recording_start.active,
                        gui_async_recording_finalize_progress_view(
                            gui_async_recording_finalize));
                ImGui::Separator();
                render_gui_session_timing_status(
                    timing,
                    streaming_fps.load(),
                    nullptr);
                render_gui_external_recorder_status(recording_session);
            }

            if (camera_control->subscribe) {
                // The worker arrays only exist while streaming. The
                // stop-streaming teardown (same GUI thread, earlier in this
                // frame) clears subscribe first and then deletes the workers
                // and nulls the arrays, so when this guard passes the
                // pointers are either live or nullptr - never freed.
                const std::vector<GuiWorkerFatalErrorEntry> worker_fatal_errors =
                    gui_collect_worker_fatal_errors(
                        num_cameras,
                        openGLDisplayWorkers,
                        cropProducerWorkers,
                        cropAndEncodeWorkers,
                        cropPreviewWorkers,
                        spatialSnapshotWorkers,
                        poseWorkers,
                        image_writer,
                        recording_session);
                gui_note_new_worker_fatal_errors(
                    &gui_worker_fatal_errors_logged, worker_fatal_errors);
                render_gui_worker_fatal_errors(worker_fatal_errors);
            } else if (!gui_worker_fatal_errors_logged.empty()) {
                // Workers are gone; a fresh streaming session starts with
                // clean latches, so let it log anew.
                gui_worker_fatal_errors_logged.clear();
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
                gui_session_timing_snapshot(
                    &gui_session_timing,
                    camera_control,
                    gui_async_recording_start.active,
                    gui_async_recording_finalize_progress_view(
                        gui_async_recording_finalize));
            if (camera_control->record_video) {
                // Resize speed tracking data
                if (speed_tracking_data.size() != static_cast<size_t>(num_cameras)) {
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
                        orange::ui::ImageCanvasViewState* primary_canvas_view =
                            (i >= 0 && i < static_cast<int>(primary_video_canvas_views.size()))
                                ? &primary_video_canvas_views[i]
                                : nullptr;
                        if (primary_video_canvas_enabled && primary_canvas_view != nullptr) {
                            const std::string fit_button_id =
                                "Fit##primary_video_canvas_fit_recording_" + std::to_string(i);
                            if (ImGui::SmallButton(fit_button_id.c_str())) {
                                primary_canvas_view->fit_requested = true;
                            }
                        }
            
                        ImVec2 avail_size = ImGui::GetContentRegionAvail();
                        avail_size.y *= 0.5f;
                        const float display_width =
                            static_cast<float>(cameras_params[i].width / cameras_select[i].downsample);
                        const float display_height =
                            static_cast<float>(cameras_params[i].height / cameras_select[i].downsample);
                        render_primary_video_texture(
                            tex[i].texture,
                            avail_size,
                            display_width,
                            display_height,
                            primary_canvas_view,
                            primary_video_canvas_enabled,
                            i);

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
                        orange::ui::ImageCanvasViewState* primary_canvas_view =
                            (i >= 0 && i < static_cast<int>(primary_video_canvas_views.size()))
                                ? &primary_video_canvas_views[i]
                                : nullptr;
                        if (primary_video_canvas_enabled && primary_canvas_view != nullptr) {
                            const std::string fit_button_id =
                                "Fit##primary_video_canvas_fit_streaming_" + std::to_string(i);
                            if (ImGui::SmallButton(fit_button_id.c_str())) {
                                primary_canvas_view->fit_requested = true;
                            }
                        }
                        const ImVec2 avail_size = ImGui::GetContentRegionAvail();
                        const float display_width =
                            static_cast<float>(cameras_params[i].width / cameras_select[i].downsample);
                        const float display_height =
                            static_cast<float>(cameras_params[i].height / cameras_select[i].downsample);
                        render_primary_video_texture(
                            tex[i].texture,
                            avail_size,
                            display_width,
                            display_height,
                            primary_canvas_view,
                            primary_video_canvas_enabled,
                            i);
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

    // Window-close shutdown: a recording start may still be pending on the
    // background thread even when streaming already stopped (defensive; the
    // streaming teardown below also cancels). Join and abort it before any
    // further cleanup. Idempotent when nothing is pending.
    gui_cancel_async_recording_start(
        &gui_async_recording_start,
        &gui_local_control_start_request,
        &recording_session,
        camera_control,
        "gui_shutdown",
        gui_local_control_event_log_path);

    // Window-close shutdown: stop the shadow-mode incremental clip splitter
    // (if running) before the finalize join below, mirroring the streaming
    // teardown ordering. Prompt no-op when idle or the flag is off.
    gui_join_incremental_clip_shadow(&gui_incremental_clip_shadow);

    // Window-close shutdown: a recording finalize may still be running on
    // the background thread. JOIN it (a single recorder stop can exceed the
    // bounded 15s finalize wait below, so racing a fresh synchronous
    // finalize against that cap would abandon the in-flight one) and
    // complete it before any further cleanup. Idempotent when nothing is in
    // flight; the streaming teardown below also joins first.
    if (gui_join_async_recording_finalize(
            &gui_async_recording_finalize,
            &gui_recording_run,
            &recording_session,
            camera_control)) {
        gui_display_frame_rate_stats.Finish();
        gui_mark_local_control_drain_completed(
            &gui_local_control_stop_scheduler,
            gui_local_control_event_log_path,
            gui_recording_run.recording_folder);
        std::cout << "[GUI][recording] Finalized recording session during"
                     " window-close shutdown." << std::endl;
    }

    if (camera_control->subscribe) {
        std::cout << "Window closed while streaming; running stream shutdown..." << std::endl;
        stop_streaming_and_teardown();
        // At window close there are no further GUI frames to poll
        // gui_finalize_recording_session_if_ready (the render loop does that
        // every frame after a button-initiated stop). Drain completion is
        // synchronous by the end of the teardown, so the finalize call inside
        // it normally succeeds; poll here with a bounded wait to cover the
        // remaining not-ready paths (e.g. an injected diagnostic finalize
        // stall) instead of exiting without a session manifest.
        if (gui_recording_run.active && gui_recording_run.finalizing) {
            const auto finalize_deadline =
                std::chrono::steady_clock::now() + std::chrono::seconds(15);
            bool finalized_at_exit = false;
            while (!finalized_at_exit &&
                   std::chrono::steady_clock::now() < finalize_deadline) {
                finalized_at_exit = gui_finalize_recording_session_if_ready(
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
                        orange_imgui_glfw_size_cache_stats()));
                if (finalized_at_exit) {
                    gui_display_frame_rate_stats.Finish();
                    gui_mark_local_control_drain_completed(
                        &gui_local_control_stop_scheduler,
                        gui_local_control_event_log_path,
                        gui_recording_run.recording_folder);
                    std::cout << "[GUI][recording] Finalized recording session during"
                                 " window-close shutdown." << std::endl;
                } else {
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
                }
            }
            if (!finalized_at_exit) {
                std::cerr << "[GUI][recording] WARNING: recording session did not"
                             " finalize within 15s of window close; the recording"
                             " session manifest may be missing or incomplete."
                          << std::endl;
            }
        }
    }

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
