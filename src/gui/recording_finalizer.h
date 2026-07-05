#pragma once

#include "gui/recording_run_state.h"
#include "external_recorder_lifecycle.h"
#include "json.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <map>
#include <string>
#include <thread>
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

// --- Phased recording-session finalize ---------------------------------------
//
// gui_finalize_recording_session_if_ready keeps its synchronous contract (the
// stream-teardown and window-close callers block in it deliberately); it is
// composed of the phases declared below, which the GUI render loop drives
// across a background thread so a finalize (two external recorder stops of up
// to graceful_shutdown_timeout_ms each, per-camera summary reads, per-clip
// CSV splits, and every manifest/snapshot write) never freezes the ImGui
// thread. The phase split mirrors the phased recording-run start
// (orange::session::prepare_recording_run /
// start_prepared_recording_run_supervisors / complete_recording_run):
//
//   gate  - gui_recording_finalize_gate_ready: the per-frame drain gate.
//           GUI thread only; re-asserts/clears CameraControl drain flags
//           exactly as the previous monolithic implementation did.
//   F1    - gui_prepare_recording_finalize: GUI thread only, the frame the
//           gate opens. Stamps the drain timestamps, updates the
//           local-control stop manifest, resets external IPC connections and
//           snapshots the per-pipeline ingress stats (both touch live
//           pipeline objects), snapshots every input the background phase
//           needs, and MOVES ownership of the two external recorder
//           lifecycle states out of the session so nothing else can touch
//           them while the background phase runs.
//   F2    - gui_run_recording_finalize: safe on a background thread. In
//           today's order: stop crop recorder -> stop full-frame recorder ->
//           read recorder summaries -> split crop sidecar CSVs by clip
//           ranges -> write every clip/session manifest, finalization
//           artifact, and snapshot update. THE ORDER IS A DATA-INTEGRITY
//           INVARIANT: summaries exist only after recorder exit; clip ranges
//           come from the summaries; a consumer must never observe a
//           finalized session whose terminal clip manifest is not durable.
//           Touches only its inputs/outcome structs and the filesystem -
//           never ImGui, CameraControl, worker objects, or the live session.
//   F3    - gui_complete_recording_finalize: GUI thread only. Returns
//           lifecycle ownership to the session, installs the recorder error
//           strings, releases the CameraControl recording-folder claim, and
//           flips the run to {active=false, finalizing=false,
//           finalized=true}. ONLY F3 releases the folder or flips finalized.
//           On F2 failure it installs the error and leaves `finalizing`
//           latched and the folder claimed, so the per-frame gate retries
//           the whole finalize idempotently (re-stopping recorders no-ops
//           when the lifecycle is not started; the manifest writes are
//           trunc-overwrites).

// Coarse progress stages published by the background finalize phase.
enum class GuiRecordingFinalizeStage : int {
    kIdle = 0,
    kStoppingRecorders = 1,
    kReadingSummaries = 2,
    kWritingClipManifests = 3,
    kUpdatingSnapshot = 4,
};

// Human-readable label for a GuiRecordingFinalizeStage value (as published
// through the progress atomics). Unknown values map to "preparing".
const char* gui_recording_finalize_stage_label(int stage);

// Progress slots the background phase publishes and the GUI thread reads;
// plain atomics so the reader never blocks on the worker.
struct GuiRecordingFinalizeProgress {
    std::atomic<int> stage{static_cast<int>(GuiRecordingFinalizeStage::kIdle)};
    std::atomic<int> clips_done{0};
    std::atomic<int> clips_total{0};
};

// Value snapshot of the progress atomics for status rendering.
struct GuiRecordingFinalizeProgressView {
    bool pending = false;
    int stage = static_cast<int>(GuiRecordingFinalizeStage::kIdle);
    int clips_done = 0;
    int clips_total = 0;
};

// Per-camera inputs the background phase needs (snapshotted in F1 because
// the CameraParams/CameraEachSelect arrays are owned by the GUI and are
// deleted during teardown).
struct GuiRecordingFinalizeCameraSnapshot {
    std::string camera_serial;
    int camera_id = 0;
    int frame_rate = 0;
    bool record = false;
    bool crop_and_encode = false;
};

// Everything the background finalize phase reads. Owns the two external
// recorder lifecycle states for the duration of the finalize (moved out of
// the session in F1, moved back in F3).
struct GuiRecordingFinalizeInputs {
    bool valid = false;
    GuiRecordingRunState run;
    bool external_ipc = false;
    bool recording_session_available = false;
    bool crop_external_recorder_active = false;
    std::vector<GuiRecordingFinalizeCameraSnapshot> cameras;
    int crop_size_px = 0;
    nlohmann::json gui_display_frame_rate = nlohmann::json::object();

    orange::external_recorder::SupervisedRecorderLifecycleState
        external_recorder_lifecycle;
    orange::external_recorder::SupervisedRecorderLifecycleState
        external_crop_recorder_lifecycle;
    std::string external_recorder_last_error;
    std::string external_crop_recorder_last_error;
    std::string external_recorder_contract_path;
    std::string external_recorder_supervisor_plan_path;
    std::string external_crop_recorder_contract_path;
    std::string external_crop_recorder_supervisor_plan_path;

    // Ingress stats snapshot (reads live pipeline objects; GUI thread only).
    nlohmann::json ingress_stats = nlohmann::json::object();
    uint64_t total_ingress_submitted = 0;
    uint64_t total_ingress_acked = 0;
    uint64_t total_ingress_failures = 0;
    uint64_t total_ingress_ack_timeouts = 0;
};

// Outcome published by the background phase and consumed by F3.
struct GuiRecordingFinalizeOutcome {
    bool ok = false;
    // Fatal error (manifest/snapshot write failure or an escaped exception);
    // set only when ok == false. The finalize is retried by the gate.
    std::string error_message;
    // Recorder error strings F3 installs into the session state (may be
    // non-empty on ok == true finalizes whose manifests are "incomplete").
    std::string external_recorder_error;
    std::string crop_external_recorder_error;
};

// Per-frame drain gate (GUI thread). True when the drained run may finalize
// this frame. Mutates the CameraControl drain flags exactly as the previous
// monolithic implementation did.
bool gui_recording_finalize_gate_ready(
    GuiRecordingRunState* run,
    orange::session::RecordingSessionState* recording_session,
    CameraControl* camera_control);

// F1 (GUI thread; call only after the gate opened this frame).
GuiRecordingFinalizeInputs gui_prepare_recording_finalize(
    GuiRecordingRunState* run,
    orange::session::RecordingSessionState* recording_session,
    const CameraParams* cameras_params,
    const CameraEachSelect* cameras_select,
    int num_cameras,
    int crop_size_px,
    const nlohmann::json& gui_display_frame_rate);

// F2 (safe on a background thread; progress may be null).
GuiRecordingFinalizeOutcome gui_run_recording_finalize(
    GuiRecordingFinalizeInputs* inputs,
    GuiRecordingFinalizeProgress* progress);

// F3 (GUI thread). Returns true exactly when the run finalized.
bool gui_complete_recording_finalize(
    GuiRecordingFinalizeInputs* inputs,
    GuiRecordingFinalizeOutcome&& outcome,
    GuiRecordingRunState* run,
    orange::session::RecordingSessionState* recording_session,
    CameraControl* camera_control);

// Owns the single background thread that runs gui_run_recording_finalize for
// a GUI recording finalize. Owned by main scope; the worker is joined on
// completion (gui_poll_async_recording_finalize) and on teardown/shutdown
// (gui_join_async_recording_finalize) - never detached. The background
// thread must never touch ImGui, CameraControl, worker objects, or any other
// GUI state: it only reads `inputs`, writes `outcome`/`progress`, and then
// publishes `done`; the GUI thread reads `outcome` only after observing
// `done` and joining the worker.
struct GuiAsyncRecordingFinalizeState {
    std::thread worker;
    std::atomic<bool> done{false};
    bool active = false;
    GuiRecordingFinalizeInputs inputs;
    GuiRecordingFinalizeOutcome outcome;
    GuiRecordingFinalizeProgress progress;
    std::chrono::steady_clock::time_point started_at{};
};

// Polled once per GUI frame. Launches the background finalize the frame the
// drain gate opens and completes it (F3) the frame the worker publishes
// done. Returns true exactly when a finalize completed successfully this
// frame (same contract the synchronous
// gui_finalize_recording_session_if_ready gives its callers).
bool gui_poll_async_recording_finalize(
    GuiAsyncRecordingFinalizeState* state,
    GuiRecordingRunState* run,
    orange::session::RecordingSessionState* recording_session,
    CameraControl* camera_control,
    const CameraParams* cameras_params,
    const CameraEachSelect* cameras_select,
    int num_cameras,
    int crop_size_px,
    const nlohmann::json& gui_display_frame_rate);

// Blocking join for teardown/shutdown: waits for an in-flight background
// finalize (bounded by the recorder graceful-shutdown timeouts) and
// completes it (F3) on the calling thread. Must run before any pipeline or
// worker teardown: F1 handed the recorder lifecycle objects to the finalize
// state and the background phase may be mid-write. Returns true when a
// finalize completed successfully during this call; no-op when idle.
bool gui_join_async_recording_finalize(
    GuiAsyncRecordingFinalizeState* state,
    GuiRecordingRunState* run,
    orange::session::RecordingSessionState* recording_session,
    CameraControl* camera_control);

// Value snapshot of the async finalize progress for the session-status line.
GuiRecordingFinalizeProgressView gui_async_recording_finalize_progress_view(
    const GuiAsyncRecordingFinalizeState& state);

bool gui_write_external_rolling_recording_session_manifest(
    const GuiRecordingRunState& run,
    const orange::external_recorder::SupervisorPlan& plan,
    const nlohmann::json& recording_backend,
    const std::vector<orange::session::RecordingOutputDescriptor>& session_recording_outputs,
    bool recording_session_ok,
    const nlohmann::json& gui_display_frame_rate,
    nlohmann::json* manifest_out,
    nlohmann::json* bridge_out,
    std::string* error_out,
    GuiRecordingFinalizeProgress* progress = nullptr);

// Synchronous composition of gate + F1 + F2 + F3, used by the
// stream-teardown and window-close paths (blocking there is deliberate) and
// kept as the single-call form of the phased finalize above.
bool gui_finalize_recording_session_if_ready(GuiRecordingRunState* run,
                                             orange::session::RecordingSessionState* recording_session,
                                             CameraControl* camera_control,
                                             const CameraParams* cameras_params,
                                             const CameraEachSelect* cameras_select,
                                             int num_cameras,
                                             int crop_size_px,
                                             const nlohmann::json& gui_display_frame_rate);
