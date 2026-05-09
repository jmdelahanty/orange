#pragma once

#include "modern_recording_pipeline.h"
#include "project.h"
#include "recording_config_state.h"
#include "video_capture.h"
#include "json.hpp"

#include <memory>
#include <string>
#include <vector>

namespace orange::session {

inline constexpr const char* kConservativeRollingClipsNote =
    "headless rolling clips currently rotate by draining the current clip and "
    "arming the next clip; seamless GOP-boundary writer switching is not implemented yet.";

struct RecordingSessionState {
    std::vector<std::unique_ptr<ModernRecordingPipeline>> recording_pipelines;
    std::string recording_sink_mode = "real";
    std::string external_recorder_config_status;
    std::string external_recorder_contract_source;
    nlohmann::json external_recorder_contract_config = nlohmann::json::object();
};

struct RecordingRunStartResult {
    bool ok = false;
    std::string recording_folder;
    std::string recording_sink_mode = "real";
    std::string error_message;
    std::string external_recorder_contract_path;
    std::string external_recorder_supervisor_plan_path;
};

struct RecordingControlConfig {
    int record_for_seconds = 0;
    int clip_seconds = 0;

    bool enabled() const {
        return record_for_seconds > 0 || clip_seconds > 0;
    }
};

struct RecordingSessionCameraArtifact {
    std::string camera_serial;
    std::string video_path;
    std::string metadata_path;
    std::string keyframe_path;
    uint64_t frame_count = 0;
    uint64_t first_recording_frame_id = 0;
    uint64_t last_recording_frame_id = 0;
    uint64_t recording_frame_id_gaps = 0;
};

struct SingleClipRecordingSessionManifestOptions {
    std::string producer = "orange";
    std::string session_id;
    std::string created_at_utc;
    std::string updated_at_utc;
    std::string recording_folder;
    std::string status = "incomplete";
    int requested_stream_duration_seconds = 0;
    int stream_start_delay_seconds = 0;
    std::string stream_started_at_utc;
    std::string stream_finished_at_utc;
    double stream_actual_elapsed_s = 0.0;
    bool stream_interrupted = false;
    RecordingControlConfig recording_control;
    bool recording_started = false;
    std::string recording_started_at_utc;
    double recording_started_at_elapsed_s = 0.0;
    bool recording_stop_requested = false;
    std::string recording_stop_requested_at_utc;
    double recording_stop_requested_at_elapsed_s = 0.0;
    std::string recording_stop_reason;
    bool recording_drain_completed = false;
    std::string recording_drained_at_utc;
    double recording_drained_at_elapsed_s = 0.0;
    double actual_recording_duration_s = 0.0;
    double drain_duration_s = 0.0;
    bool timed_stop_hit = false;
    std::vector<RecordingSessionCameraArtifact> cameras;
};

struct RollingClipManifestOptions {
    std::string producer = "orange";
    std::string session_id;
    int clip_index = 0;
    std::string clip_id;
    std::string recording_folder;
    std::string directory;
    std::string status = "incomplete";
    std::string start_reason;
    std::string stop_reason;
    std::string started_at_utc;
    double started_at_elapsed_s = 0.0;
    std::string stop_requested_at_utc;
    double stop_requested_at_elapsed_s = 0.0;
    std::string finalized_at_utc;
    double finalized_at_elapsed_s = 0.0;
    double requested_duration_s = 0.0;
    double actual_duration_s = 0.0;
    double drain_duration_s = 0.0;
    bool timed_stop_hit = false;
    bool final_clip = false;
    bool drain_completed = false;
    std::vector<RecordingSessionCameraArtifact> cameras;
};

struct RollingRecordingSessionManifestOptions {
    std::string producer = "orange";
    std::string session_id;
    std::string created_at_utc;
    std::string updated_at_utc;
    std::string recording_folder;
    std::string status = "incomplete";
    int requested_stream_duration_seconds = 0;
    int stream_start_delay_seconds = 0;
    std::string stream_started_at_utc;
    std::string stream_finished_at_utc;
    double stream_actual_elapsed_s = 0.0;
    bool stream_interrupted = false;
    RecordingControlConfig recording_control;
    bool recording_started = false;
    std::string recording_started_at_utc;
    double recording_started_at_elapsed_s = 0.0;
    bool recording_stop_requested = false;
    std::string recording_stop_requested_at_utc;
    double recording_stop_requested_at_elapsed_s = 0.0;
    std::string recording_stop_reason;
    bool recording_drain_completed = false;
    std::string recording_drained_at_utc;
    double recording_drained_at_elapsed_s = 0.0;
    double actual_recording_duration_s = 0.0;
    double drain_duration_s = 0.0;
    double sum_clip_actual_duration_s = 0.0;
    std::vector<std::string> camera_serials;
    std::vector<RollingClipManifestOptions> clips;
};

nlohmann::json build_recording_control_json(const RecordingControlConfig& config);
bool validate_recording_control_config(const RecordingControlConfig& config,
                                       std::string* error_out = nullptr,
                                       const std::string& context = {});
nlohmann::json build_single_clip_recording_session_manifest(
    const SingleClipRecordingSessionManifestOptions& options);
nlohmann::json build_recording_clip_manifest(
    const RollingClipManifestOptions& options);
nlohmann::json build_rolling_clip_recording_session_manifest(
    const RollingRecordingSessionManifestOptions& options);
bool write_recording_session_manifest(const std::string& path,
                                      const nlohmann::json& manifest,
                                      std::string* error_out = nullptr);

void create_recording_pipelines_for_stream(RecordingSessionState* state,
                                           CameraParams* cameras_params,
                                           CameraEachSelect* cameras_select,
                                           int num_cameras,
                                           const EncoderConfig& encoder_config,
                                           CameraResources* camera_resources,
                                           CameraControl* camera_control,
                                           const AppStorageConfig* app_storage_config = nullptr);
RecordingRunStartResult begin_recording_run(CameraControl* camera_control,
                                            CameraParams* cameras_params,
                                            const CameraEachSelect* cameras_select,
                                            int num_cameras,
                                            const std::string& base_folder,
                                            PTPParams* ptp_params,
                                            const std::string& recording_sink_mode = "real",
                                            const nlohmann::json* external_recorder_contract_config = nullptr);
void request_stop_recording_run(CameraControl* camera_control);
void request_drain_recording_run(RecordingSessionState* state, CameraControl* camera_control);
std::string current_recording_folder(CameraControl* camera_control);
void start_recording_pipeline_for_camera(RecordingSessionState* state, int camera_index);
void request_stop_recording_pipeline_for_camera(RecordingSessionState* state, int camera_index);
void shutdown_recording_pipeline_for_camera(RecordingSessionState* state, int camera_index);
void clear_recording_pipelines(RecordingSessionState* state);
RecordingIngress* recording_ingress_for_camera(const RecordingSessionState& state, int camera_index);

}  // namespace orange::session
