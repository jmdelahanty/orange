#pragma once

#include "session/crop_rolling_sidecars.h"
#include "json.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace orange::external_recorder {
struct SupervisedRecorderLifecycleState;
}  // namespace orange::external_recorder

// --- Shadow-mode incremental clip finalization (stage 4) --------------------
//
// Gated by ORANGE_GUI_INCREMENTAL_CLIP_SHADOW (default OFF: zero new
// behavior, zero new threads). During an external-IPC rolling recording, a
// single background worker incrementally splits each completed clip's crop
// sidecar CSVs (Cam<serial>_crop_meta.csv / Cam<serial>_crop_perf.csv) into
// SHADOW per-clip files (clips/clip_NNNNNN/Cam<serial>_crop_meta.shadow.csv
// and ..._crop_perf.shadow.csv) as the CROP recorder's status sidecar
// announces clip completions. The authoritative finalize path is untouched:
// it still whole-file-splits into the unsuffixed names at finalize, and a
// cross-check then compares shadow vs. authoritative output byte-for-byte
// and reports agreement without ever affecting the finalize outcome. Shadow
// files are deliberately left on disk as evidence.
//
// Threading model mirrors GuiAsyncRecordingStartState /
// GuiAsyncRecordingFinalizeState in orange.cpp: one std::thread owned by
// main scope, joined (never detached) via gui_join_incremental_clip_shadow
// the frame the run enters finalizing and on teardown/window-close. The
// worker touches only its own state and the filesystem - never ImGui,
// CameraControl, worker objects, or the live session. The GUI thread feeds
// it per-camera completion watermarks (read from the crop recorder status
// snapshots refreshed each frame) through a mutex-guarded slot with a
// condition-variable wake; the worker never busy-spins and checks the stop
// flag between clips and between retries so the join is prompt.
//
// Deferred-clip semantics: the incremental splitter keeps one sequential
// cursor per root CSV, so clips of one camera can only be extracted in
// order. A clip that fails (kFailed) or stays incomplete past the bounded
// retry budget therefore PARKS that camera's whole shadow chain: the parked
// state is recorded in the shadow index, later clips of that camera are not
// attempted (their rows can no longer be reached without violating the
// cursor's gap policy), other cameras keep going, and the recording and the
// authoritative finalize are never affected.

// Environment flag gating the whole feature.
inline constexpr const char kGuiIncrementalClipShadowEnvFlag[] =
    "ORANGE_GUI_INCREMENTAL_CLIP_SHADOW";

// True when ORANGE_GUI_INCREMENTAL_CLIP_SHADOW is enabled (default false).
bool gui_incremental_clip_shadow_enabled();

// One completed clip's identity as announced by the crop recorder status
// sidecar (RecorderStatusSnapshot rolling_last_completed_clip_* fields).
struct GuiShadowClipAnnouncement {
    int clip_index = -1;
    uint64_t first_recording_frame_id = 0;
    uint64_t last_recording_frame_id = 0;

    bool valid() const
    {
        return clip_index >= 0 &&
               first_recording_frame_id > 0 &&
               last_recording_frame_id >= first_recording_frame_id;
    }
};

// How a freshly observed announcement folds into a camera chain whose
// highest accepted clip index is last_accepted_clip_index (-1 before any).
enum class GuiShadowAnnouncementIntake {
    // Invalid, or a repeat/stale announcement (clip_index <= last accepted).
    kIgnored,
    // Exactly the next expected clip index: enqueue for processing.
    kAccepted,
    // The announcement skipped at least one clip index. The skipped clips'
    // ranges are unknown, so the chain must park (see header comment).
    kGap,
};

GuiShadowAnnouncementIntake gui_shadow_classify_announcement(
    int last_accepted_clip_index,
    const GuiShadowClipAnnouncement& announcement);

// Worker-owned per-camera shadow split chain. Built on the GUI thread before
// the worker starts; touched only by the worker thread afterwards.
struct GuiShadowCameraChainState {
    std::string camera_serial;
    std::string root_meta_csv_path;
    std::string root_perf_csv_path;
    // Directory holding the per-clip folders (…/clips); clip N's shadow CSVs
    // land in clips_root/clip_NNNNNN/ next to the recorder's clip outputs.
    std::string clips_root;
    orange::session::IncrementalClipSplitCursor meta_cursor;
    orange::session::IncrementalClipSplitCursor perf_cursor;
    // Highest clip index accepted into `pending` (-1 before any).
    int last_accepted_clip_index = -1;
    // Accepted announcements not yet fully shadow-split, in clip order.
    std::deque<GuiShadowClipAnnouncement> pending;
    // Bounded-retry state for pending.front() when it reports kIncomplete.
    int incomplete_retry_count = 0;
    std::chrono::steady_clock::time_point next_retry_at{};
    // Parked (deferred) chain state; see the header comment.
    bool parked = false;
    int parked_clip_index = -1;
    std::string parked_reason;
    uint64_t clips_completed = 0;
};

// Applies gui_shadow_classify_announcement to the chain: on kAccepted the
// announcement is enqueued and last_accepted_clip_index advances. Returns
// the classification (kGap is reported but does not mutate the chain; the
// caller parks it with a descriptive reason).
GuiShadowAnnouncementIntake gui_shadow_intake_announcement(
    GuiShadowCameraChainState* chain,
    const GuiShadowClipAnnouncement& announcement);

enum class GuiShadowClipAttemptStatus {
    // Both shadow CSVs hold every row of the clip.
    kCompleted,
    // The root CSVs do not contain the clip's full row range yet (or do not
    // exist yet); retry later with the same announcement.
    kIncomplete,
    // Unrecoverable for this chain (orphan/gap/parse/IO failure).
    kFailed,
};

struct GuiShadowClipAttemptResult {
    GuiShadowClipAttemptStatus status = GuiShadowClipAttemptStatus::kFailed;
    uint64_t meta_rows = 0;  // cumulative rows in the clip's shadow meta CSV
    uint64_t perf_rows = 0;  // cumulative rows in the clip's shadow perf CSV
    std::string meta_shadow_path;
    std::string perf_shadow_path;
    std::string error;
};

// One attempt at shadow-splitting the announced clip for this chain. Safe to
// retry with the same announcement after kIncomplete (append resumes) and
// idempotent after kCompleted. Mirrors the authoritative finalize policies:
// crop meta uses the kFail orphan policy, crop perf uses kCountAndSkip.
GuiShadowClipAttemptResult gui_shadow_process_clip_attempt(
    GuiShadowCameraChainState* chain,
    const GuiShadowClipAnnouncement& clip);

// --- Paths -------------------------------------------------------------------

// Durable JSONL partial index written into the recording folder
// (recording_clip_index.shadow.jsonl).
std::string gui_shadow_index_path(const std::string& recording_folder);
std::string gui_shadow_clip_directory(const std::string& clips_root,
                                      int clip_index);
std::string gui_shadow_meta_csv_path(const std::string& clips_root,
                                     int clip_index,
                                     const std::string& camera_serial);
std::string gui_shadow_perf_csv_path(const std::string& clips_root,
                                     int clip_index,
                                     const std::string& camera_serial);
// Shadow sibling of an authoritative per-clip CSV: "…/foo.csv" ->
// "…/foo.shadow.csv". Returns "" when the path does not end in ".csv".
std::string gui_shadow_sibling_csv_path(const std::string& authoritative_csv_path);

// --- Shadow index (JSONL) ------------------------------------------------------

nlohmann::json gui_shadow_clip_completed_index_line(
    const std::string& camera_serial,
    const GuiShadowClipAnnouncement& clip,
    uint64_t meta_rows,
    uint64_t perf_rows,
    const std::string& meta_shadow_path,
    const std::string& perf_shadow_path,
    const std::string& wall_clock_utc);

nlohmann::json gui_shadow_chain_parked_index_line(
    const std::string& camera_serial,
    int clip_index,
    const std::string& reason,
    const std::string& wall_clock_utc);

// Appends one line to the JSONL index and flushes it.
bool gui_shadow_append_index_line(const std::string& index_path,
                                  const nlohmann::json& line,
                                  std::string* error_out = nullptr);

// --- Finalize cross-check ------------------------------------------------------

// Result of comparing the authoritative per-clip CSVs written by the
// finalize split against the shadow CSVs written during the recording.
struct GuiShadowCrossCheckOutcome {
    int total = 0;       // authoritative clip CSVs considered
    int identical = 0;   // shadow sibling present and byte-identical
    int mismatched = 0;  // shadow sibling present but differs
    int missing = 0;     // shadow sibling absent
    std::string summary;
};

// Compares each authoritative clip CSV (that exists on disk) against its
// shadow sibling byte-for-byte, appends a durable summary line to the
// shadow index, and returns the counts plus a one-line summary. Observation
// only: never fails and never deletes shadow files.
GuiShadowCrossCheckOutcome gui_cross_check_incremental_clip_shadow(
    const std::string& shadow_index_path,
    const std::vector<std::string>& authoritative_csv_paths);

// --- State + worker ------------------------------------------------------------

// Mutex-guarded per-camera watermark slot the GUI thread writes and the
// worker polls. Only the latest announcement is kept; announcements arrive
// far slower (one per clip) than the per-frame GUI push, so the worker
// always observes every index in order (a skipped index parks the chain).
struct GuiShadowFeedSlot {
    std::string camera_serial;
    GuiShadowClipAnnouncement latest;
};

// Owns the single background shadow-split worker thread. Owned by main
// scope; joined on run finalize and on teardown/shutdown via
// gui_join_incremental_clip_shadow - never detached. See the file header
// for the threading model.
struct GuiIncrementalClipShadowState {
    std::thread worker;
    std::atomic<bool> done{false};
    bool active = false;  // GUI thread bookkeeping
    std::string recording_folder;
    std::string index_path;
    // Recording folder the last start decision was made for, so the
    // per-frame maybe-start probes each run at most once.
    std::string last_recording_folder;

    std::mutex mutex;
    std::condition_variable cv;
    bool stop_requested = false;          // guarded by mutex
    uint64_t push_sequence = 0;           // guarded by mutex
    std::vector<GuiShadowFeedSlot> slots; // guarded by mutex

    // Worker-owned after gui_start_incremental_clip_shadow (same index
    // order as slots); the GUI thread may read it again after the join.
    std::vector<GuiShadowCameraChainState> chains;

    std::atomic<uint64_t> clips_completed{0};
    std::atomic<uint64_t> chains_parked{0};

    GuiIncrementalClipShadowState() = default;
    GuiIncrementalClipShadowState(const GuiIncrementalClipShadowState&) = delete;
    GuiIncrementalClipShadowState& operator=(const GuiIncrementalClipShadowState&) = delete;
    // Defensive: signals stop and joins if the worker is still running (the
    // explicit join call sites normally run first).
    ~GuiIncrementalClipShadowState();
};

// Starts the worker over the given chains. Returns false (and starts
// nothing) when already active or the inputs are empty.
bool gui_start_incremental_clip_shadow(
    GuiIncrementalClipShadowState* state,
    std::vector<GuiShadowCameraChainState> chains,
    const std::string& recording_folder);

// Low-level watermark push for one camera (also used by tests).
void gui_push_incremental_clip_shadow_announcement(
    GuiIncrementalClipShadowState* state,
    const std::string& camera_serial,
    const GuiShadowClipAnnouncement& announcement);

// Per-frame GUI hook: starts the worker for a new recording when the env
// flag is on, the run is active, and the crop recorder lifecycle is started
// with a rolling (clip_seconds > 0) crop stream. No-op (and no new threads)
// otherwise. Returns true when the worker started this call.
bool gui_maybe_start_incremental_clip_shadow(
    GuiIncrementalClipShadowState* state,
    const orange::external_recorder::SupervisedRecorderLifecycleState& crop_lifecycle,
    const std::string& recording_folder,
    bool recording_active);

// Per-frame GUI hook: pushes the crop recorder status snapshots' completed
// clip watermarks into the worker's slots. No-op when the worker is idle.
void gui_push_incremental_clip_shadow_watermarks(
    GuiIncrementalClipShadowState* state,
    const orange::external_recorder::SupervisedRecorderLifecycleState& crop_lifecycle);

// Signals stop and joins the worker (prompt: the worker checks the flag
// between clips and between retries). Idempotent; no-op when idle.
void gui_join_incremental_clip_shadow(GuiIncrementalClipShadowState* state);
