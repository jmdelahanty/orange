#pragma once

#include "gui/recording_run_state.h"
#include "json.hpp"

#include <chrono>
#include <cstdint>
#include <string>

struct CameraControl;
class YoloWorker;

namespace orange::session {
struct RecordingSessionState;
}  // namespace orange::session

namespace orange::external_recorder {
struct SupervisedRecorderLifecycleState;
}  // namespace orange::external_recorder

// Session-timing and external-recorder status extracted verbatim from
// src/orange.cpp. These are global-namespace free functions so the
// unqualified call sites remaining in orange.cpp compile unchanged.
//
// The struct definitions and function bodies are byte-identical to their
// previous definitions in orange.cpp's anonymous namespace, with one
// declared exception: the nlohmann::json stop_control default arguments
// of gui_note_recording_stop_requested and
// gui_request_recording_stop_through_operator_path moved from the
// definitions to the declarations below (C++ requires the default on the
// declaration visible to callers in other translation units).

struct GuiSessionTimingState {
    bool stream_running = false;
    // A recording start is pending: the external recorder supervisors are
    // being spawned on a background thread and record_video has not flipped
    // true yet. Reconciled in gui_session_timing_snapshot from the
    // recording_start_pending argument.
    bool recording_starting = false;
    bool recording_running = false;
    bool recording_finalizing = false;
    std::chrono::steady_clock::time_point stream_started_at{};
    std::chrono::steady_clock::time_point recording_start_pending_at{};
    std::chrono::steady_clock::time_point recording_started_at{};
    std::chrono::steady_clock::time_point finalizing_started_at{};
    std::chrono::seconds last_recording_elapsed{0};
};

struct GuiSessionTimingSnapshot {
    bool stream_running = false;
    bool recording_starting = false;
    bool recording_running = false;
    bool recording_finalizing = false;
    bool has_recording_elapsed = false;
    std::string stream_elapsed = "00:00:00";
    std::string starting_elapsed = "00:00:00";
    std::string recording_elapsed = "00:00:00";
    std::string finalizing_elapsed = "00:00:00";
};

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

// --- Session timing ---------------------------------------------------------

std::chrono::seconds gui_elapsed_since(
    const std::chrono::steady_clock::time_point& started_at,
    const std::chrono::steady_clock::time_point& now);

void gui_note_recording_started(GuiRecordingRunState* run,
                                CameraControl* camera_control,
                                const std::string& recording_folder,
                                const std::string& recording_sink_mode);

void gui_note_recording_stop_requested(GuiRecordingRunState* run,
                                       const std::string& stop_reason,
                                       nlohmann::json stop_control = nlohmann::json::object());

void gui_mark_stream_started(GuiSessionTimingState* timing);

void gui_mark_stream_stopped(GuiSessionTimingState* timing);

void gui_mark_recording_started(GuiSessionTimingState* timing);

void gui_mark_recording_finalizing(GuiSessionTimingState* timing);

void gui_mark_recording_finished(GuiSessionTimingState* timing);

void gui_request_recording_stop_through_operator_path(
    orange::session::RecordingSessionState* recording_session,
    CameraControl* camera_control,
    GuiRecordingRunState* recording_run,
    GuiSessionTimingState* timing,
    const std::string& stop_reason,
    nlohmann::json stop_control = nlohmann::json::object());

// recording_start_pending is true while a background recording start (the
// external recorder supervisor spawn + socket wait) is still in flight;
// CameraControl carries no flag for that window, so the GUI passes it in.
// CameraControl stays authoritative for the other flags: an active
// recording (record_video) always wins over a stale pending marker.
GuiSessionTimingSnapshot gui_session_timing_snapshot(
    GuiSessionTimingState* timing,
    const CameraControl* camera_control,
    bool recording_start_pending = false);

// Text-only ImGui renderer (defined in session_status_render.cpp so the
// session_status_tests link stays free of ImGui and worker objects).
void render_gui_session_timing_status(
    const GuiSessionTimingSnapshot& timing,
    double streaming_fps_value,
    YoloWorker* yolo_worker);

// --- External-recorder status ------------------------------------------------

std::string gui_format_storage_bytes(uint64_t bytes);

GuiExternalRecorderStatusLine gui_external_recorder_status_line(
    const char* label,
    const orange::external_recorder::SupervisedRecorderLifecycleState& state,
    const std::string& last_error);

// Text-only ImGui renderer (defined in session_status_render.cpp).
void render_gui_external_recorder_status(
    const orange::session::RecordingSessionState& recording_session);

// Polls the supervised external-recorder lifecycles while recording is
// active or draining; self-contained lifecycle refresh over
// orange::external_recorder APIs.
void gui_refresh_external_recorder_lifecycles(
    orange::session::RecordingSessionState* recording_session,
    const CameraControl* camera_control);
