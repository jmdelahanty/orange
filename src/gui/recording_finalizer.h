#pragma once

#include "gui/recording_run_state.h"
#include "json.hpp"

#include <chrono>
#include <map>
#include <string>
#include <vector>

struct CameraControl;
struct CameraParams;
struct CameraEachSelect;

namespace orange::session {
struct RecordingControlConfig;
struct RecordingOutputDescriptor;
struct RecordingSessionIndexArtifacts;
struct RecordingSessionState;
struct RollingClipManifestOptions;
}  // namespace orange::session

namespace orange::external_recorder {
struct RecorderStreamPlan;
struct SupervisorPlan;
}  // namespace orange::external_recorder

// Extracted verbatim from src/orange.cpp. These are global-namespace free
// functions so the unqualified call sites remaining in orange.cpp compile
// unchanged.

// Shared env helper: orange.cpp's local-control drain timeout resolution
// also calls this, so both translation units use this single copy.

// Shared with orange.cpp's timed-stop scheduler code.
bool has_gui_timepoint(const std::chrono::steady_clock::time_point& timepoint);

bool gui_external_stream_is_full(
    const orange::external_recorder::RecorderStreamPlan& stream);

bool gui_external_stream_is_crop(
    const orange::external_recorder::RecorderStreamPlan& stream);

std::string gui_external_recorder_clip_id(int clip_index);

std::string gui_json_string_or(const nlohmann::json& object,
                               const char* key,
                               const std::string& fallback);

nlohmann::json gui_crop_rollover_json(
    const orange::session::RecordingControlConfig& recording_control,
    const std::string& status);

bool gui_attach_crop_rolling_outputs_to_clips(
    const nlohmann::json& recording_backend,
    std::map<int, orange::session::RollingClipManifestOptions>* clips_by_index,
    std::string* error_out);

nlohmann::json gui_build_rolling_recording_session_snapshot_update(
    const std::string& recording_folder,
    const nlohmann::json& manifest,
    const orange::session::RecordingSessionIndexArtifacts& index_artifacts,
    const nlohmann::json& gui_display_frame_rate);

bool gui_write_external_rolling_recording_session_manifest(
    const GuiRecordingRunState& run,
    const orange::external_recorder::SupervisorPlan& plan,
    const nlohmann::json& recording_backend,
    const std::vector<orange::session::RecordingOutputDescriptor>& session_recording_outputs,
    bool recording_session_ok,
    const nlohmann::json& gui_display_frame_rate,
    nlohmann::json* manifest_out,
    nlohmann::json* bridge_out,
    std::string* error_out);

bool gui_finalize_recording_session_if_ready(GuiRecordingRunState* run,
                                             orange::session::RecordingSessionState* recording_session,
                                             CameraControl* camera_control,
                                             const CameraParams* cameras_params,
                                             const CameraEachSelect* cameras_select,
                                             int num_cameras,
                                             int crop_size_px,
                                             const nlohmann::json& gui_display_frame_rate);
