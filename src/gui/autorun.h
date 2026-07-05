#pragma once

#include "json.hpp"
#include "video_capture.h"

#include <chrono>
#include <string>
#include <vector>

namespace orange::gui {

// Moved verbatim from src/orange.cpp: gui_autorun_update() reads the
// active/finalizing flags in its kWaitFinalize stage, so the run-state
// struct must be visible to both translation units.
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

GuiAutorunConfig resolve_gui_autorun_config();

const char* gui_autorun_stage_name(GuiAutorunStage stage);

double gui_autorun_stage_elapsed_s(const GuiAutorunState& state);

void gui_autorun_enter_stage(GuiAutorunState* state, GuiAutorunStage stage);

void gui_autorun_fail(GuiAutorunState* state, const std::string& message);

GuiAutorunRequests gui_autorun_update(
    GuiAutorunState* state,
    const GuiAutorunConfig& config,
    const std::vector<std::string>& local_config_folders,
    int* local_config_select,
    const CameraControl* camera_control,
    const GuiRecordingRunState* recording_run,
    bool calibration_tool_busy);

void apply_gui_autorun_camera_selection(const GuiAutorunConfig& config,
                                        CameraEachSelect* cameras_select,
                                        int num_cameras);

}  // namespace orange::gui

// These symbols were defined in orange.cpp's anonymous namespace and are
// referenced there unqualified; keep that spelling compiling unchanged.
using orange::gui::GuiRecordingRunState;      // NOLINT(misc-unused-using-decls)
using orange::gui::GuiAutorunStage;           // NOLINT(misc-unused-using-decls)
using orange::gui::GuiAutorunConfig;          // NOLINT(misc-unused-using-decls)
using orange::gui::GuiAutorunState;           // NOLINT(misc-unused-using-decls)
using orange::gui::GuiAutorunRequests;        // NOLINT(misc-unused-using-decls)
using orange::gui::resolve_gui_autorun_config;         // NOLINT(misc-unused-using-decls)
using orange::gui::gui_autorun_stage_name;             // NOLINT(misc-unused-using-decls)
using orange::gui::gui_autorun_stage_elapsed_s;        // NOLINT(misc-unused-using-decls)
using orange::gui::gui_autorun_enter_stage;            // NOLINT(misc-unused-using-decls)
using orange::gui::gui_autorun_fail;                   // NOLINT(misc-unused-using-decls)
using orange::gui::gui_autorun_update;                 // NOLINT(misc-unused-using-decls)
using orange::gui::apply_gui_autorun_camera_selection; // NOLINT(misc-unused-using-decls)
