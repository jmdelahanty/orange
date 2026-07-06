#pragma once

#include "modern_recording_pipeline.h"
#include "external_recorder_lifecycle.h"
#include "project.h"
#include "recording_config_state.h"
#include "recording_output_descriptor.h"
#include "video_capture.h"
#include "json.hpp"

#include <chrono>
#include <memory>
#include <string>
#include <vector>

namespace orange::session {

struct RecordingControlConfig {
    int record_for_seconds = 0;
    int clip_seconds = 0;

    bool enabled() const {
        return record_for_seconds > 0 || clip_seconds > 0;
    }
};

struct RecordingSessionState {
    std::vector<std::unique_ptr<ModernRecordingPipeline>> recording_pipelines;
    std::string recording_sink_mode = "real";
    RecordingControlConfig gui_recording_control;
    std::string external_recorder_config_status;
    std::string external_recorder_contract_source;
    nlohmann::json external_recorder_contract_config = nlohmann::json::object();
    nlohmann::json active_external_recorder_contract = nlohmann::json::object();
    orange::external_recorder::SupervisedRecorderLifecycleState external_recorder_lifecycle;
    std::string external_recorder_contract_path;
    std::string external_recorder_supervisor_plan_path;
    std::string external_recorder_last_error;
    std::string crop_recording_sink_mode = "in_process";
    nlohmann::json active_external_crop_recorder_contract = nlohmann::json::object();
    orange::external_recorder::SupervisedRecorderLifecycleState external_crop_recorder_lifecycle;
    std::string external_crop_recorder_contract_path;
    std::string external_crop_recorder_supervisor_plan_path;
    std::string external_crop_recorder_last_error;
};

struct RecordingRunStartResult {
    bool ok = false;
    std::string recording_folder;
    std::string recording_sink_mode = "real";
    std::string error_message;
    std::string external_recorder_contract_path;
    std::string external_recorder_supervisor_plan_path;
};

// --- Phased recording-run start ---------------------------------------------
//
// A recording-run start is composed of the three phases declared below,
// which the GUI operator path drives separately so the ImGui render thread
// never blocks on external recorder startup (NVENC init in the spawned
// recorders takes seconds). (A synchronous begin_recording_run wrapper used
// to compose them; it had no live callers and was removed - callers run the
// phases directly.)
//
//   1. prepare_recording_run — caller thread only. Mutates CameraControl and
//      RecordingSessionState: resets the run flags, resolves the recording
//      folder, writes the snapshot/PTP markers, stops a stale previous
//      lifecycle, resets external IPC connections, and materializes +
//      writes the external recorder contracts. On failure it performs the
//      failed-start cleanup itself and returns valid == false.
//   2. start_prepared_recording_run_supervisors — safe to run on a
//      background thread. fork/execs the supervised recorder processes and
//      waits (bounded) for socket readiness. It touches only the prepared
//      options, its own outcome object, and the filesystem — never
//      CameraControl, RecordingSessionState, or worker objects. On failure
//      it stops any lifecycle it already started (zero graceful timeout: no
//      frames were ever sent during startup) so a failed start never leaks
//      recorder children.
//   3. complete_recording_run — caller thread only. Installs the outcome
//      into the session state, writes the supervisor plan artifacts,
//      refreshes the latest-recording snapshot, and only then flips
//      record_video true. On a failed outcome it runs exactly the
//      synchronous failed-start cleanup and reports the error.
//
// abort_prepared_recording_run cancels a prepared (and possibly already
// supervisor-started) run without ever flipping record_video, for teardown
// or shutdown while a background start is still pending.

struct PreparedRecordingRunStart {
    bool valid = false;
    std::string error_message;  // set when valid == false
    std::string recording_folder;
    std::string recording_id;
    std::string resolved_base_folder;
    // Effective sink mode for RecordingRunStartResult reporting
    // (normalized when recognized, the raw request otherwise).
    std::string recording_sink_mode = "real";
    // Normalized sink mode used for the latest-recording snapshot refresh.
    std::string normalized_sink_mode;
    bool external_recorder_requested = false;
    bool external_crop_recorder_requested = false;
    orange::external_recorder::SupervisedRecorderLifecycleOptions
        external_recorder_lifecycle_options;
    orange::external_recorder::SupervisedRecorderLifecycleOptions
        external_crop_recorder_lifecycle_options;
    std::string external_recorder_contract_path;
    std::string external_crop_recorder_contract_path;

    bool requires_supervisor_start() const {
        return external_recorder_requested || external_crop_recorder_requested;
    }
};

struct RecordingRunSupervisorStartOutcome {
    bool ok = false;
    std::string error_message;
    bool external_recorder_attempted = false;
    bool external_crop_recorder_attempted = false;
    std::string external_recorder_last_error;
    std::string external_crop_recorder_last_error;
    orange::external_recorder::SupervisedRecorderLifecycleState
        external_recorder_lifecycle;
    orange::external_recorder::SupervisedRecorderLifecycleState
        external_crop_recorder_lifecycle;
};

PreparedRecordingRunStart prepare_recording_run(
    RecordingSessionState* state,
    CameraControl* camera_control,
    CameraParams* cameras_params,
    const CameraEachSelect* cameras_select,
    int num_cameras,
    const std::string& base_folder,
    PTPParams* ptp_params,
    const std::string& recording_sink_mode = "real",
    const nlohmann::json* external_recorder_contract_config = nullptr);

RecordingRunSupervisorStartOutcome start_prepared_recording_run_supervisors(
    const PreparedRecordingRunStart& prepared);

RecordingRunStartResult complete_recording_run(
    RecordingSessionState* state,
    CameraControl* camera_control,
    CameraParams* cameras_params,
    int num_cameras,
    PTPParams* ptp_params,
    const PreparedRecordingRunStart& prepared,
    RecordingRunSupervisorStartOutcome&& outcome);

void abort_prepared_recording_run(
    RecordingSessionState* state,
    CameraControl* camera_control,
    const PreparedRecordingRunStart& prepared,
    RecordingRunSupervisorStartOutcome&& outcome,
    const std::string& abort_reason);

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
    nlohmann::json recording_stop_control = nlohmann::json::object();
    nlohmann::json recording_backend = nlohmann::json::object();
    std::vector<RecordingSessionCameraArtifact> cameras;
    std::vector<RecordingOutputDescriptor> recording_outputs;
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
    uint64_t rollover_request_id = 0;
    uint64_t rollover_at_recording_frame_id = 0;
    uint64_t first_recording_frame_id = 0;
    uint64_t last_recording_frame_id = 0;
    bool pending_next_clip = false;
    bool timed_stop_hit = false;
    bool final_clip = false;
    bool drain_completed = false;
    std::vector<RecordingSessionCameraArtifact> cameras;
    std::vector<RecordingOutputDescriptor> recording_outputs;
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
    std::string rollover_implementation = "headless_gop_boundary_writer_switch";
    bool rollover_next_writer_preopened = true;
    nlohmann::json recording_stop_control = nlohmann::json::object();
    nlohmann::json recording_backend = nlohmann::json::object();
    std::vector<RecordingOutputDescriptor> recording_outputs;
    std::vector<std::string> camera_serials;
    std::vector<RollingClipManifestOptions> clips;
};

struct RecordingSessionIndexArtifacts {
    std::string clip_index_json_path;
    std::string clip_index_csv_path;
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
RecordingSessionCameraArtifact build_recording_camera_artifact(
    const std::string& camera_serial,
    const std::string& recording_folder,
    bool relative_paths);
std::vector<RecordingSessionCameraArtifact> build_recording_camera_artifacts(
    const std::vector<std::string>& camera_serials,
    const std::string& recording_folder,
    bool relative_paths);
RecordingOutputDescriptor build_crop_recording_output_descriptor(
    const std::string& camera_serial,
    const std::string& recording_folder,
    bool relative_paths,
    int crop_size_px,
    int frame_rate,
    const std::string& status);
bool write_recording_session_manifest(const std::string& path,
                                      const nlohmann::json& manifest,
                                      std::string* error_out = nullptr);
bool write_rolling_clip_index_artifacts(const std::string& recording_folder,
                                        const nlohmann::json& manifest,
                                        RecordingSessionIndexArtifacts* artifacts_out = nullptr,
                                        std::string* error_out = nullptr);

std::string resolve_gui_recording_sink_mode(const AppStorageConfig* app_storage_config,
                                            const CameraParams* cameras_params = nullptr,
                                            const CameraEachSelect* cameras_select = nullptr,
                                            int num_cameras = 0);
void create_recording_pipelines_for_stream(RecordingSessionState* state,
                                           CameraParams* cameras_params,
                                           CameraEachSelect* cameras_select,
                                           int num_cameras,
                                           const EncoderConfig& encoder_config,
                                           CameraResources* camera_resources,
                                           CameraControl* camera_control,
                                           const AppStorageConfig* app_storage_config = nullptr);
// --- Unified recording stop/drain core -------------------------------------
//
// Both binaries (GUI and headless client) MUST route recording stop/drain
// through these functions. The GUI keeps its RecordingSessionState overloads
// below, which delegate to the pipeline-list core; the headless client, which
// owns its ModernRecordingPipeline vector directly, calls the core functions.
//
// Semantics locked down by tools/recording_session_drain_tests.cpp:
// - request_stop_recording_run: clears record_video and pending rollover
//   state, latches recording_draining/stop_record, and takes the early-drain
//   shortcut (flags cleared, recording_folder cleared unless preserved) when
//   active_recorders == 0 so an idle stop does not leave drain latches set.
// - request_drain_recording_run: request_stop + per-pipeline drain request,
//   then re-asserts recording_draining/stop_record for external_ipc sinks
//   while the IPC handoff workers still hold undelivered frames (the encoder
//   finalize paths never see those frames, so active_recorders stays 0).
// - A recording run is drained only when BOTH active_recorders == 0 (in-
//   process encoder + crop encoder finalize paths) AND every recording
//   pipeline reports is_drained() (ingress/handoff workers, incl. external
//   IPC ACK/RELEASE completion). Neither predicate alone is sufficient.
void request_stop_recording_run(CameraControl* camera_control);
void request_drain_recording_run(
    std::vector<std::unique_ptr<ModernRecordingPipeline>>* recording_pipelines,
    const std::string& recording_sink_mode,
    CameraControl* camera_control);
bool recording_pipelines_drained(
    const std::vector<std::unique_ptr<ModernRecordingPipeline>>* recording_pipelines);
// True when drain flags must stay latched after a stop request because an
// external IPC sink still holds undrained frames. Pure decision helper so the
// re-assert rule is unit-testable without live pipelines.
bool should_reassert_recording_drain_flags(const std::string& recording_sink_mode,
                                           bool pipelines_drained);
bool recording_run_drained(
    const std::vector<std::unique_ptr<ModernRecordingPipeline>>* recording_pipelines,
    const CameraControl* camera_control);
// Polls recording_run_drained() until it holds or timeout expires. Re-nudges
// pipeline drain requests periodically so passive drain states (pending GPU
// source releases, IPC protocol polling) keep making progress. Returns true
// when the run fully drained within the timeout.
bool wait_for_recording_run_drain(
    std::vector<std::unique_ptr<ModernRecordingPipeline>>* recording_pipelines,
    CameraControl* camera_control,
    std::chrono::steady_clock::duration timeout,
    const char* timeout_label);
// Post-run CameraControl hygiene: clears every recording-run field (folders,
// rollover bookkeeping, preserve flag, latest frame id). Call only after the
// run's pipelines have drained and shut down.
void clear_recording_run_state(CameraControl* camera_control);
// Full stop sequence: request drain, wait (bounded), stop + shutdown + free
// the pipelines, then clear residual CameraControl state.
void drain_and_shutdown_recording_run(
    std::vector<std::unique_ptr<ModernRecordingPipeline>>* recording_pipelines,
    const std::string& recording_sink_mode,
    CameraControl* camera_control,
    std::chrono::steady_clock::duration drain_timeout = std::chrono::seconds(10),
    const char* drain_timeout_label = "Recording drain");
// RecordingSessionState overloads (GUI callers); thin delegates to the core.
void request_drain_recording_run(RecordingSessionState* state, CameraControl* camera_control);
bool recording_pipelines_drained(const RecordingSessionState* state);
void reset_external_ipc_connections(RecordingSessionState* state);
std::string current_recording_folder(CameraControl* camera_control);
void start_recording_pipeline_for_camera(RecordingSessionState* state, int camera_index);
void request_stop_recording_pipeline_for_camera(RecordingSessionState* state, int camera_index);
void shutdown_recording_pipeline_for_camera(RecordingSessionState* state, int camera_index);
void clear_recording_pipelines(RecordingSessionState* state);
RecordingIngress* recording_ingress_for_camera(const RecordingSessionState& state, int camera_index);

}  // namespace orange::session
