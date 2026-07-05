// Unit tests for the session-timing and external-recorder status module
// extracted to src/gui/session_status.{h,cpp}.
//
// Derivation rules locked in by these tests (read from the implementation):
//   - gui_session_timing_snapshot reconciles the GUI mirror state against
//     CameraControl in place; CameraControl is authoritative. A stale mirror
//     is rewritten to match the flags (subscribe / record_video /
//     recording_draining) before the snapshot strings are derived.
//   - Operator truth: when record_video is false but recording_draining is
//     still true, the snapshot reports "finalizing", never "idle", and the
//     recording-elapsed string freezes at the value captured when the
//     finalizing transition happened.
//   - gui_mark_recording_finalizing sets finalizing_started_at exactly once;
//     re-marking while already finalizing preserves the original timestamp
//     and does not re-capture last_recording_elapsed.
//   - gui_mark_recording_finished preserves last_recording_elapsed (drives
//     the "Last recording:" line); gui_mark_stream_stopped resets everything.
//   - gui_note_recording_stop_requested latches the stop timestamp and
//     stop_control on the first request only; a re-request while finalizing
//     updates stop_reason but not the latched fields.
//   - gui_format_storage_bytes: binary units B..TiB, precision 0 for the
//     byte unit or values >= 100, 1 decimal for >= 10, else 2 decimals.

#include "gui/session_status.h"

#include "external_recorder_lifecycle.h"
#include "external_recorder_supervisor.h"
#include "video_capture.h"

#include "NvEncoder/Logger.h"

#include <chrono>
#include <iostream>
#include <stdexcept>
#include <string>

// The NvEnc/CUDA sources linked into this target log through this global.
simplelogger::Logger* logger =
    simplelogger::LoggerFactory::CreateConsoleLogger();

using std::chrono::seconds;
using std::chrono::steady_clock;

namespace {

void require(const bool condition, const std::string& message)
{
    if (!condition) {
        throw std::runtime_error(message);
    }
}

void require_eq(const std::string& actual,
                const std::string& expected,
                const std::string& message)
{
    if (actual != expected) {
        throw std::runtime_error(
            message + " (expected \"" + expected + "\", got \"" + actual + "\")");
    }
}

// --- gui_elapsed_since -------------------------------------------------------

void test_elapsed_since()
{
    const auto now = steady_clock::now();
    require(gui_elapsed_since(steady_clock::time_point{}, now) == seconds{0},
            "unset start timepoint yields zero elapsed");
    require(gui_elapsed_since(now + seconds{5}, now) == seconds{0},
            "start after now yields zero elapsed");
    require(gui_elapsed_since(now - seconds{90}, now) == seconds{90},
            "elapsed is the truncated whole-second difference");
    std::cout << "PASS test_elapsed_since" << std::endl;
}

// --- gui_format_storage_bytes ------------------------------------------------

void test_format_storage_bytes()
{
    require_eq(gui_format_storage_bytes(0), "0 B", "zero bytes");
    require_eq(gui_format_storage_bytes(512), "512 B", "sub-KiB stays in bytes");
    require_eq(gui_format_storage_bytes(1023), "1023 B",
               "1023 bytes stays in bytes with no decimals");
    require_eq(gui_format_storage_bytes(1024), "1.00 KiB",
               "KiB boundary uses two decimals below 10");
    require_eq(gui_format_storage_bytes(1536), "1.50 KiB", "fractional KiB");
    require_eq(gui_format_storage_bytes(10 * 1024ULL), "10.0 KiB",
               "one decimal from 10 up to 100");
    require_eq(gui_format_storage_bytes(100 * 1024ULL), "100 KiB",
               "no decimals from 100 up");
    require_eq(gui_format_storage_bytes(1048575), "1024 KiB",
               "1023.999 KiB rounds up to 1024 KiB, not 1.00 MiB");
    require_eq(gui_format_storage_bytes(1ULL << 20), "1.00 MiB", "MiB boundary");
    require_eq(gui_format_storage_bytes(1ULL << 30), "1.00 GiB", "GiB boundary");
    require_eq(gui_format_storage_bytes(1ULL << 40), "1.00 TiB", "TiB boundary");
    require_eq(gui_format_storage_bytes(1ULL << 50), "1024 TiB",
               "units cap at TiB");
    std::cout << "PASS test_format_storage_bytes" << std::endl;
}

// --- gui_mark_* transitions --------------------------------------------------

void test_mark_transitions()
{
    GuiSessionTimingState timing;

    const auto before_stream = steady_clock::now();
    gui_mark_stream_started(&timing);
    const auto after_stream = steady_clock::now();
    require(timing.stream_running, "stream start sets stream_running");
    require(timing.stream_started_at >= before_stream &&
                timing.stream_started_at <= after_stream,
            "stream start stamps stream_started_at with now");
    require(!timing.recording_running && !timing.recording_finalizing,
            "stream start clears recording flags");
    require(timing.last_recording_elapsed == seconds{0},
            "stream start clears last recording elapsed");

    const auto before_rec = steady_clock::now();
    gui_mark_recording_started(&timing);
    const auto after_rec = steady_clock::now();
    require(timing.recording_running && !timing.recording_finalizing,
            "recording start sets running and clears finalizing");
    require(timing.recording_started_at >= before_rec &&
                timing.recording_started_at <= after_rec,
            "recording start stamps recording_started_at with now");
    require(timing.finalizing_started_at == steady_clock::time_point{},
            "recording start clears finalizing_started_at");

    // Backdate the recording start so the finalizing transition captures a
    // non-zero last_recording_elapsed.
    timing.recording_started_at = steady_clock::now() - seconds{42};
    gui_mark_recording_finalizing(&timing);
    require(!timing.recording_running && timing.recording_finalizing,
            "finalizing clears running and sets finalizing");
    require(timing.last_recording_elapsed == seconds{42},
            "finalizing captures elapsed from recording_started_at");
    const auto first_finalizing_at = timing.finalizing_started_at;
    require(first_finalizing_at != steady_clock::time_point{},
            "finalizing stamps finalizing_started_at");

    // Re-marking while already finalizing must set the timestamp exactly
    // once and must not re-capture last_recording_elapsed.
    timing.recording_started_at = steady_clock::now() - seconds{300};
    gui_mark_recording_finalizing(&timing);
    require(timing.finalizing_started_at == first_finalizing_at,
            "re-marking finalizing preserves the original timestamp");
    require(timing.last_recording_elapsed == seconds{42},
            "re-marking finalizing does not re-capture elapsed");

    gui_mark_recording_finished(&timing);
    require(!timing.recording_running && !timing.recording_finalizing,
            "finished clears both recording flags");
    require(timing.recording_started_at == steady_clock::time_point{} &&
                timing.finalizing_started_at == steady_clock::time_point{},
            "finished clears the recording timestamps");
    require(timing.last_recording_elapsed == seconds{42},
            "finished preserves last_recording_elapsed for the Last-recording line");
    require(timing.stream_running, "finished leaves the stream running");

    gui_mark_stream_stopped(&timing);
    require(!timing.stream_running && !timing.recording_running &&
                !timing.recording_finalizing &&
                timing.last_recording_elapsed == seconds{0} &&
                timing.stream_started_at == steady_clock::time_point{},
            "stream stop resets the whole timing state");

    gui_mark_stream_started(nullptr);  // null-safety: must not crash
    gui_mark_recording_finalizing(nullptr);
    std::cout << "PASS test_mark_transitions" << std::endl;
}

// --- gui_note_recording_started / stop_requested -----------------------------

void test_note_recording_run()
{
    GuiRecordingRunState run;
    CameraControl camera_control;

    gui_note_recording_started(&run, &camera_control, "/data/rec_001", "");
    require(run.active && !run.finalizing && !run.finalized,
            "note started marks the run active");
    require_eq(run.recording_folder, "/data/rec_001", "note started stores folder");
    require_eq(run.recording_sink_mode, "real",
               "empty sink mode defaults to real");
    require(!run.recording_started_at_utc.empty(),
            "note started stamps a UTC start time");
    require(run.recording_stop_requested_at == steady_clock::time_point{} &&
                run.recording_stop_requested_at_utc.empty(),
            "note started clears stop-request fields");
    require_eq(run.stop_reason, "manual_stop", "note started resets stop reason");
    require(camera_control.preserve_recording_session_state,
            "note started asks CameraControl to preserve session state");

    nlohmann::json control{{"request_id", "abc"}};
    gui_note_recording_stop_requested(&run, "local_control_stop", control);
    require(run.finalizing, "stop request marks the run finalizing");
    require_eq(run.stop_reason, "local_control_stop", "stop reason recorded");
    require(run.stop_control == control, "stop control latched on first request");
    const auto first_stop_at = run.recording_stop_requested_at;
    const std::string first_stop_utc = run.recording_stop_requested_at_utc;
    require(first_stop_at != steady_clock::time_point{} && !first_stop_utc.empty(),
            "stop request stamps the stop timestamps");

    // Re-request while finalizing: stop_reason updates, latched fields do not.
    gui_note_recording_stop_requested(
        &run, "stream_shutdown", nlohmann::json{{"request_id", "other"}});
    require(run.recording_stop_requested_at == first_stop_at &&
                run.recording_stop_requested_at_utc == first_stop_utc,
            "re-request preserves the first stop timestamp");
    require(run.stop_control == control,
            "re-request preserves the first stop_control");
    require_eq(run.stop_reason, "stream_shutdown",
               "re-request still updates the stop reason");

    GuiRecordingRunState inactive;
    gui_note_recording_stop_requested(&inactive, "manual_stop");
    require(!inactive.finalizing &&
                inactive.recording_stop_requested_at == steady_clock::time_point{},
            "stop request on an inactive run is a no-op");

    GuiRecordingRunState empty_reason_run;
    empty_reason_run.active = true;
    gui_note_recording_stop_requested(&empty_reason_run, "",
                                      nlohmann::json::array());
    require_eq(empty_reason_run.stop_reason, "manual_stop",
               "empty stop reason defaults to manual_stop");
    require(empty_reason_run.stop_control == nlohmann::json::object(),
            "non-object stop_control is replaced with an empty object");
    std::cout << "PASS test_note_recording_run" << std::endl;
}

// --- gui_session_timing_snapshot ---------------------------------------------

void test_snapshot_idle()
{
    GuiSessionTimingState timing;
    CameraControl camera_control;  // subscribe/record_video/draining all false
    const GuiSessionTimingSnapshot snapshot =
        gui_session_timing_snapshot(&timing, &camera_control);
    require(!snapshot.stream_running && !snapshot.recording_running &&
                !snapshot.recording_finalizing && !snapshot.has_recording_elapsed,
            "idle snapshot has all flags false");
    require_eq(snapshot.stream_elapsed, "00:00:00", "idle stream elapsed");
    require_eq(snapshot.recording_elapsed, "00:00:00", "idle recording elapsed");

    require(!gui_session_timing_snapshot(nullptr, &camera_control).stream_running,
            "null timing yields default snapshot");
    std::cout << "PASS test_snapshot_idle" << std::endl;
}

void test_snapshot_streaming_only()
{
    GuiSessionTimingState timing;  // stale mirror: knows nothing of the stream
    CameraControl camera_control;
    camera_control.subscribe = true;
    const GuiSessionTimingSnapshot snapshot =
        gui_session_timing_snapshot(&timing, &camera_control);
    require(snapshot.stream_running,
            "CameraControl.subscribe wins over the stale mirror");
    require(timing.stream_running,
            "reconciliation rewrites the mirror in place");
    require_eq(snapshot.stream_elapsed, "00:00:00",
               "stream elapsed starts at zero on reconciliation");
    require(!snapshot.recording_running && !snapshot.has_recording_elapsed,
            "streaming-only snapshot reports recording idle");

    // Elapsed derives from the mirror's stream_started_at timestamp.
    timing.stream_started_at = steady_clock::now() - seconds{3725};
    const GuiSessionTimingSnapshot later =
        gui_session_timing_snapshot(&timing, &camera_control);
    require_eq(later.stream_elapsed, "01:02:05",
               "stream elapsed formats HH:MM:SS from stream_started_at");
    std::cout << "PASS test_snapshot_streaming_only" << std::endl;
}

void test_snapshot_streaming_and_recording()
{
    GuiSessionTimingState timing;
    CameraControl camera_control;
    camera_control.subscribe = true;
    camera_control.record_video = true;

    // Stale mirror: record_video already true but mirror idle. CameraControl
    // is authoritative, so the snapshot transitions to recording now.
    const GuiSessionTimingSnapshot first =
        gui_session_timing_snapshot(&timing, &camera_control);
    require(first.stream_running && first.recording_running,
            "record_video wins over the stale mirror");
    require(first.has_recording_elapsed,
            "recording in progress reports has_recording_elapsed");
    require(!first.recording_finalizing, "recording is not finalizing");

    // Elapsed derives from the gui_mark_recording_started timestamp.
    timing.recording_started_at = steady_clock::now() - seconds{90};
    const GuiSessionTimingSnapshot later =
        gui_session_timing_snapshot(&timing, &camera_control);
    require_eq(later.recording_elapsed, "00:01:30",
               "recording elapsed formats HH:MM:SS from recording_started_at");
    std::cout << "PASS test_snapshot_streaming_and_recording" << std::endl;
}

void test_snapshot_draining_shows_finalizing()
{
    // Operator truth: the GUI stopped recording (record_video false), but
    // CameraControl still reports the drain in progress. The snapshot must
    // report finalizing, never idle.
    GuiSessionTimingState timing;
    timing.stream_running = true;
    timing.stream_started_at = steady_clock::now() - seconds{600};
    timing.recording_running = true;
    timing.recording_started_at = steady_clock::now() - seconds{120};

    CameraControl camera_control;
    camera_control.subscribe = true;
    camera_control.record_video = false;
    camera_control.recording_draining = true;

    const GuiSessionTimingSnapshot snapshot =
        gui_session_timing_snapshot(&timing, &camera_control);
    require(snapshot.recording_finalizing,
            "draining reports finalizing, not idle");
    require(!snapshot.recording_running,
            "draining is no longer recording");
    require(snapshot.has_recording_elapsed,
            "finalizing keeps the recording elapsed visible");
    require_eq(snapshot.recording_elapsed, "00:02:00",
               "recording elapsed freezes at the finalizing transition");
    require(timing.recording_finalizing,
            "reconciliation marked the mirror finalizing");

    // Still draining a snapshot later: finalizing persists, frozen elapsed
    // stays frozen even though recording_started_at was cleared of meaning.
    const GuiSessionTimingSnapshot again =
        gui_session_timing_snapshot(&timing, &camera_control);
    require(again.recording_finalizing, "finalizing persists while draining");
    require_eq(again.recording_elapsed, "00:02:00",
               "frozen recording elapsed is stable across frames");
    std::cout << "PASS test_snapshot_draining_shows_finalizing" << std::endl;
}

void test_snapshot_drain_complete_and_last_recording()
{
    GuiSessionTimingState timing;
    timing.stream_running = true;
    timing.stream_started_at = steady_clock::now() - seconds{600};
    timing.recording_finalizing = true;
    timing.finalizing_started_at = steady_clock::now() - seconds{4};
    timing.last_recording_elapsed = seconds{120};

    CameraControl camera_control;
    camera_control.subscribe = true;  // stream continues, drain finished

    const GuiSessionTimingSnapshot snapshot =
        gui_session_timing_snapshot(&timing, &camera_control);
    require(!snapshot.recording_running && !snapshot.recording_finalizing,
            "drain completion transitions to finished");
    require(snapshot.has_recording_elapsed,
            "finished run keeps has_recording_elapsed from last elapsed");
    require_eq(snapshot.recording_elapsed, "00:02:00",
               "finished run reports the last recording elapsed");
    require(snapshot.stream_running, "stream stays running after the drain");

    // A stale recording mirror with no draining and nothing captured goes
    // straight to idle: no last elapsed, so no Last-recording line.
    GuiSessionTimingState stale;
    stale.stream_running = true;
    stale.stream_started_at = steady_clock::now();
    stale.recording_running = true;
    stale.recording_started_at = steady_clock::now() - seconds{50};
    const GuiSessionTimingSnapshot stale_snapshot =
        gui_session_timing_snapshot(&stale, &camera_control);
    require(!stale_snapshot.recording_running &&
                !stale_snapshot.recording_finalizing &&
                !stale_snapshot.has_recording_elapsed,
            "stale mirror with no drain and no captured elapsed reads idle");
    std::cout << "PASS test_snapshot_drain_complete_and_last_recording"
              << std::endl;
}

void test_snapshot_stream_stop_resets()
{
    GuiSessionTimingState timing;
    timing.stream_running = true;
    timing.stream_started_at = steady_clock::now() - seconds{600};
    timing.recording_running = true;
    timing.recording_started_at = steady_clock::now() - seconds{120};
    timing.last_recording_elapsed = seconds{99};

    CameraControl camera_control;  // subscribe false: stream gone

    const GuiSessionTimingSnapshot snapshot =
        gui_session_timing_snapshot(&timing, &camera_control);
    require(!snapshot.stream_running && !snapshot.recording_running &&
                !snapshot.recording_finalizing && !snapshot.has_recording_elapsed,
            "stream stop wipes the whole mirror to idle");
    require(timing.last_recording_elapsed == seconds{0},
            "stream stop resets last_recording_elapsed");
    std::cout << "PASS test_snapshot_stream_stop_resets" << std::endl;
}

// --- recording-start pending derivation ---------------------------------------
//
// Rules locked in (read from the implementation):
//   - The pending window is reported by the caller (recording_start_pending);
//     CameraControl carries no flag for it. While pending and record_video is
//     false, the snapshot reports recording_starting with an elapsed derived
//     from the reconciliation timestamp.
//   - CameraControl stays authoritative: record_video == true always clears
//     the starting state, even if the caller still reports pending.
//   - A pending window that ends without record_video flipping (start failed
//     or was canceled) returns the snapshot to recording-idle with no
//     Last-recording line.

void test_snapshot_recording_start_pending()
{
    GuiSessionTimingState timing;
    CameraControl camera_control;
    camera_control.subscribe = true;

    const GuiSessionTimingSnapshot pending =
        gui_session_timing_snapshot(&timing, &camera_control, true);
    require(pending.stream_running, "pending start keeps the stream running");
    require(pending.recording_starting,
            "pending start reports recording_starting");
    require(!pending.recording_running && !pending.recording_finalizing,
            "pending start is neither recording nor finalizing");
    require(!pending.has_recording_elapsed,
            "pending start reports no recording elapsed yet");
    require_eq(pending.starting_elapsed, "00:00:00",
               "starting elapsed begins at zero on reconciliation");
    require(timing.recording_starting,
            "reconciliation marks the mirror starting in place");

    // Elapsed derives from the mirror's pending timestamp and persists
    // across frames while the caller keeps reporting pending.
    timing.recording_start_pending_at = steady_clock::now() - seconds{3};
    const GuiSessionTimingSnapshot later =
        gui_session_timing_snapshot(&timing, &camera_control, true);
    require(later.recording_starting, "pending persists while still reported");
    require_eq(later.starting_elapsed, "00:00:03",
               "starting elapsed formats from the pending timestamp");
    std::cout << "PASS test_snapshot_recording_start_pending" << std::endl;
}

void test_snapshot_pending_completion_transitions_to_recording()
{
    GuiSessionTimingState timing;
    CameraControl camera_control;
    camera_control.subscribe = true;

    (void)gui_session_timing_snapshot(&timing, &camera_control, true);
    require(timing.recording_starting, "pending latched before completion");

    // Completion: record_video flips true. Even if the caller still reports
    // pending for the same frame, CameraControl is authoritative and the
    // snapshot transitions straight to recording.
    camera_control.record_video = true;
    const GuiSessionTimingSnapshot completed =
        gui_session_timing_snapshot(&timing, &camera_control, true);
    require(completed.recording_running,
            "record_video wins over a stale pending marker");
    require(!completed.recording_starting,
            "recording clears the starting state");
    require(!timing.recording_starting &&
                timing.recording_start_pending_at == steady_clock::time_point{},
            "completion resets the mirror's pending fields");
    std::cout << "PASS test_snapshot_pending_completion_transitions_to_recording"
              << std::endl;
}

void test_snapshot_pending_failure_returns_to_idle()
{
    GuiSessionTimingState timing;
    CameraControl camera_control;
    camera_control.subscribe = true;

    (void)gui_session_timing_snapshot(&timing, &camera_control, true);
    require(timing.recording_starting, "pending latched before the failure");

    // The start failed or was canceled: the caller stops reporting pending
    // and record_video never flipped. The snapshot returns to recording-idle
    // with no Last-recording line.
    const GuiSessionTimingSnapshot failed =
        gui_session_timing_snapshot(&timing, &camera_control, false);
    require(!failed.recording_starting && !failed.recording_running &&
                !failed.recording_finalizing && !failed.has_recording_elapsed,
            "a failed/canceled pending start reads recording-idle");
    require(!timing.recording_starting &&
                timing.recording_start_pending_at == steady_clock::time_point{},
            "failure resets the mirror's pending fields");
    require(failed.stream_running, "the stream keeps running after the failure");
    std::cout << "PASS test_snapshot_pending_failure_returns_to_idle" << std::endl;
}

void test_mark_recording_started_clears_pending()
{
    GuiSessionTimingState timing;
    timing.recording_starting = true;
    timing.recording_start_pending_at = steady_clock::now() - seconds{2};

    gui_mark_recording_started(&timing);
    require(!timing.recording_starting &&
                timing.recording_start_pending_at == steady_clock::time_point{},
            "gui_mark_recording_started clears the pending-start fields");
    require(timing.recording_running,
            "gui_mark_recording_started still marks the recording running");

    GuiSessionTimingState stream_reset;
    stream_reset.recording_starting = true;
    stream_reset.recording_start_pending_at = steady_clock::now();
    gui_mark_stream_started(&stream_reset);
    require(!stream_reset.recording_starting &&
                stream_reset.recording_start_pending_at ==
                    steady_clock::time_point{},
            "gui_mark_stream_started clears the pending-start fields");
    std::cout << "PASS test_mark_recording_started_clears_pending" << std::endl;
}

// --- background finalize progress derivation ---------------------------------

void test_finalize_stage_labels()
{
    require_eq(gui_recording_finalize_stage_label(
                   static_cast<int>(GuiRecordingFinalizeStage::kStoppingRecorders)),
               "stopping recorders", "stopping-recorders stage label");
    require_eq(gui_recording_finalize_stage_label(
                   static_cast<int>(GuiRecordingFinalizeStage::kReadingSummaries)),
               "reading recorder summaries", "reading-summaries stage label");
    require_eq(gui_recording_finalize_stage_label(
                   static_cast<int>(GuiRecordingFinalizeStage::kWritingClipManifests)),
               "writing clip manifests", "writing-clip-manifests stage label");
    require_eq(gui_recording_finalize_stage_label(
                   static_cast<int>(GuiRecordingFinalizeStage::kUpdatingSnapshot)),
               "updating session snapshot", "updating-snapshot stage label");
    require_eq(gui_recording_finalize_stage_label(
                   static_cast<int>(GuiRecordingFinalizeStage::kIdle)),
               "preparing", "idle stage label");
    require_eq(gui_recording_finalize_stage_label(999),
               "preparing", "unknown stage values map to preparing");
    std::cout << "PASS test_finalize_stage_labels" << std::endl;
}

void test_snapshot_finalize_pending_keeps_finalizing_and_shows_progress()
{
    // The finalize gate clears recording_draining the frame the background
    // finalize launches; without the pending marker the snapshot would fall
    // through to "finished" mid-finalize. The marker must keep the snapshot
    // finalizing (with the original finalizing-elapsed baseline) and surface
    // the coarse stage plus clip counter.
    GuiSessionTimingState timing;
    timing.stream_running = true;
    timing.stream_started_at = steady_clock::now() - seconds{600};
    timing.recording_running = true;
    timing.recording_started_at = steady_clock::now() - seconds{120};

    CameraControl camera_control;
    camera_control.subscribe = true;
    camera_control.record_video = false;
    camera_control.recording_draining = true;

    // Frame 1: drain in progress, no background finalize yet.
    const GuiSessionTimingSnapshot draining =
        gui_session_timing_snapshot(&timing, &camera_control);
    require(draining.recording_finalizing, "draining reports finalizing");
    require(!draining.finalize_progress_visible,
            "no finalize progress line before the background phase launches");
    const auto finalizing_started_at = timing.finalizing_started_at;

    // Frame 2: gate opened, drain flags cleared, background finalize
    // in flight.
    camera_control.recording_draining = false;
    GuiRecordingFinalizeProgressView progress;
    progress.pending = true;
    progress.stage = static_cast<int>(GuiRecordingFinalizeStage::kStoppingRecorders);
    const GuiSessionTimingSnapshot stopping =
        gui_session_timing_snapshot(&timing, &camera_control, false, progress);
    require(stopping.recording_finalizing,
            "pending finalize keeps the snapshot finalizing after the gate"
            " cleared the drain flags");
    require(timing.finalizing_started_at == finalizing_started_at,
            "pending finalize preserves the finalizing-elapsed baseline");
    require(stopping.finalize_progress_visible,
            "pending finalize surfaces the progress line");
    require_eq(stopping.finalize_stage_label, "stopping recorders",
               "progress line carries the stage label");
    require(stopping.finalize_clips_done == 0 && stopping.finalize_clips_total == 0,
            "no clip counter before the clip-manifest stage");
    require_eq(stopping.recording_elapsed, "00:02:00",
               "frozen recording elapsed survives the pending window");

    // Later frame: clip manifests being written, clip counter published.
    progress.stage = static_cast<int>(GuiRecordingFinalizeStage::kWritingClipManifests);
    progress.clips_done = 2;
    progress.clips_total = 5;
    const GuiSessionTimingSnapshot writing =
        gui_session_timing_snapshot(&timing, &camera_control, false, progress);
    require(writing.recording_finalizing && writing.finalize_progress_visible,
            "clip-manifest stage still reports finalizing with progress");
    require_eq(writing.finalize_stage_label, "writing clip manifests",
               "clip-manifest stage label");
    require(writing.finalize_clips_done == 2 && writing.finalize_clips_total == 5,
            "clip counter passes through to the snapshot");

    // Completion: the background finalize finished, drain flags stay clear.
    const GuiSessionTimingSnapshot finished =
        gui_session_timing_snapshot(&timing, &camera_control);
    require(!finished.recording_finalizing,
            "finalize completion transitions the snapshot out of finalizing");
    require(!finished.finalize_progress_visible,
            "no progress line after completion");
    require(finished.has_recording_elapsed,
            "last-recording elapsed survives finalize completion");
    std::cout << "PASS test_snapshot_finalize_pending_keeps_finalizing_and_shows_progress"
              << std::endl;
}

void test_snapshot_finalize_pending_never_overrides_active_recording()
{
    // CameraControl stays authoritative: a (contradictory) pending finalize
    // marker must not override an active recording.
    GuiSessionTimingState timing;
    CameraControl camera_control;
    camera_control.subscribe = true;
    camera_control.record_video = true;

    GuiRecordingFinalizeProgressView progress;
    progress.pending = true;
    progress.stage = static_cast<int>(GuiRecordingFinalizeStage::kReadingSummaries);
    const GuiSessionTimingSnapshot snapshot =
        gui_session_timing_snapshot(&timing, &camera_control, false, progress);
    require(snapshot.recording_running,
            "an active recording wins over a stale finalize-pending marker");
    require(!snapshot.recording_finalizing && !snapshot.finalize_progress_visible,
            "no finalizing/progress state while the recording is active");
    std::cout << "PASS test_snapshot_finalize_pending_never_overrides_active_recording"
              << std::endl;
}

// --- gui_external_recorder_status_line ----------------------------------------

orange::external_recorder::RecorderProcessState make_process(
    const std::string& serial)
{
    orange::external_recorder::RecorderProcessState process;
    process.stream_id = "stream_" + serial;
    process.camera_serial = serial;
    process.active = true;
    process.socket_ready = true;
    process.recorder_status.present = true;
    process.recorder_status.valid = true;
    process.recorder_status.status = "recording";
    process.recorder_status.heartbeat_sequence = 7;
    return process;
}

void test_status_line_idle_and_error_fallback()
{
    orange::external_recorder::SupervisedRecorderLifecycleState state;
    const GuiExternalRecorderStatusLine idle =
        gui_external_recorder_status_line("External recorder", state, "");
    require(!idle.visible, "not-started lifecycle with no error is invisible");
    require_eq(idle.status, "idle", "invisible line keeps idle status");

    const GuiExternalRecorderStatusLine fallback =
        gui_external_recorder_status_line(
            "External recorder", state, "supervisor exploded");
    require(fallback.visible, "a last_error makes the line visible");
    require_eq(fallback.status, "error", "last_error forces error status");
    require_eq(fallback.error, "supervisor exploded",
               "last_error is surfaced verbatim");
    require(fallback.error_count == 1, "last_error counts as one error");

    require_eq(gui_external_recorder_status_line(nullptr, state, "x").label,
               "External recorder", "null label falls back to default");
    std::cout << "PASS test_status_line_idle_and_error_fallback" << std::endl;
}

void test_status_line_running_and_degraded()
{
    orange::external_recorder::SupervisedRecorderLifecycleState state;
    state.started = true;
    auto first = make_process("111");
    first.recorder_status.frames_received = 100;
    first.recorder_status.frames_encoded = 90;
    auto second = make_process("222");
    second.recorder_status.frames_received = 50;
    second.recorder_status.frames_encoded = 40;
    state.runtime.processes.push_back(first);
    state.runtime.processes.push_back(second);

    const GuiExternalRecorderStatusLine running =
        gui_external_recorder_status_line("External recorder", state, "");
    require(running.visible, "started lifecycle is visible");
    require_eq(running.status, "running",
               "all processes active with sockets and status reads running");
    require(running.process_count == 2 && running.active_count == 2 &&
                running.socket_ready_count == 2 &&
                running.recorder_status_valid_count == 2,
            "process counters aggregate across processes");
    require(running.frames_received == 150 && running.frames_encoded == 130,
            "frame counters sum across valid recorder statuses");
    require(running.recorder_status_detail.find("Cam111") != std::string::npos,
            "detail line reports the first camera");
    require(running.error.empty() && running.error_count == 0,
            "healthy line carries no error");

    state.runtime.processes[1].socket_ready = false;
    const GuiExternalRecorderStatusLine degraded =
        gui_external_recorder_status_line("External recorder", state, "");
    require_eq(degraded.status, "degraded",
               "a missing socket degrades the status");

    orange::external_recorder::SupervisedRecorderLifecycleState empty_started;
    empty_started.started = true;
    require_eq(gui_external_recorder_status_line("X", empty_started, "").status,
               "degraded", "started with zero processes reads degraded");
    std::cout << "PASS test_status_line_running_and_degraded" << std::endl;
}

void test_status_line_error_states()
{
    orange::external_recorder::SupervisedRecorderLifecycleState state;
    state.started = true;
    auto invalid = make_process("333");
    invalid.recorder_status.valid = false;
    invalid.recorder_status.error = "bad json";
    state.runtime.processes.push_back(invalid);

    const GuiExternalRecorderStatusLine line =
        gui_external_recorder_status_line("External recorder", state, "");
    require_eq(line.status, "error", "invalid recorder status is an error");
    require(line.error.find("Cam333") != std::string::npos &&
                line.error.find("invalid recorder status sidecar") !=
                    std::string::npos &&
                line.error.find("bad json") != std::string::npos,
            "invalid sidecar error names the camera and the cause");

    orange::external_recorder::SupervisedRecorderLifecycleState failed;
    failed.started = true;
    auto failing = make_process("444");
    failing.recorder_status.status = "failed";
    failed.runtime.processes.push_back(failing);
    const GuiExternalRecorderStatusLine failed_line =
        gui_external_recorder_status_line("External recorder", failed, "");
    require_eq(failed_line.status, "error", "failed recorder status is an error");
    require(failed_line.error.find("Cam444: recorder status failed") !=
                std::string::npos,
            "failed status error names the camera");

    orange::external_recorder::SupervisedRecorderLifecycleState artifact;
    artifact.started = true;
    artifact.last_artifact_error = "manifest write failed";
    const GuiExternalRecorderStatusLine artifact_line =
        gui_external_recorder_status_line("External recorder", artifact, "");
    require_eq(artifact_line.error, "manifest write failed",
               "lifecycle artifact error is surfaced");
    require_eq(artifact_line.status, "error", "artifact error forces error status");
    std::cout << "PASS test_status_line_error_states" << std::endl;
}

void test_status_line_storage_and_rolling()
{
    orange::external_recorder::SupervisedRecorderLifecycleState state;
    state.started = true;
    auto process = make_process("555");
    auto& rs = process.recorder_status;
    rs.storage_checked = true;
    rs.storage_ok = true;
    rs.storage_low_space = true;  // healthy requires ok && !low_space
    rs.storage_path_count = 2;
    rs.storage_paths_ok_count = 1;
    rs.storage_has_min_available_bytes = true;
    rs.storage_min_available_bytes = 5ULL << 30;
    rs.storage_min_free_bytes = 1ULL << 30;
    rs.rolling_enabled = true;
    rs.rolling_current_clip_index = 3;
    rs.rolling_clip_seconds = 60;
    rs.rolling_frames_until_next_rollover = 500;
    rs.rolling_next_rollover_at_recording_frame_id = 12000;
    rs.rolling_completed_clip_count = 3;
    rs.rolling_last_completed_clip_index = 2;
    rs.rolling_last_rollover_status = "ok";
    state.runtime.processes.push_back(process);

    const GuiExternalRecorderStatusLine line =
        gui_external_recorder_status_line("External recorder", state, "");
    require(line.storage_checked_count == 1 && line.storage_healthy_count == 0 &&
                line.storage_low_space_count == 1,
            "low-space storage is checked but not healthy");
    require(line.storage_has_min_available_bytes &&
                line.storage_min_available_bytes == (5ULL << 30),
            "minimum available bytes propagate to the line");
    require(line.storage_status_detail.find("storage=low_space") !=
                    std::string::npos &&
                line.storage_status_detail.find("paths=1/2") != std::string::npos &&
                line.storage_status_detail.find("min_avail=5.00 GiB") !=
                    std::string::npos,
            "storage detail encodes health, path counts, and formatted bytes");
    require_eq(line.status, "error", "unhealthy storage forces error status");
    require(line.error.find("below low-space threshold") != std::string::npos,
            "low-space storage error names the threshold");

    require(line.rolling_process_count == 1 &&
                line.rolling_current_clip_index == 3 &&
                line.rolling_clip_seconds == 60 &&
                line.rolling_frames_until_next_rollover == 500 &&
                line.rolling_next_rollover_at_recording_frame_id == 12000,
            "rolling fields propagate from the recorder status");
    require(line.rolling_status_detail.find("clip=3") != std::string::npos,
            "rolling detail includes the current clip");
    require(line.rolling_last_rollover_detail.find("completed_clip=2") !=
                std::string::npos,
            "last rollover detail includes the completed clip");
    std::cout << "PASS test_status_line_storage_and_rolling" << std::endl;
}

}  // namespace

int main()
{
    try {
        test_elapsed_since();
        test_format_storage_bytes();
        test_mark_transitions();
        test_note_recording_run();
        test_snapshot_idle();
        test_snapshot_streaming_only();
        test_snapshot_streaming_and_recording();
        test_snapshot_draining_shows_finalizing();
        test_snapshot_drain_complete_and_last_recording();
        test_snapshot_stream_stop_resets();
        test_snapshot_recording_start_pending();
        test_snapshot_pending_completion_transitions_to_recording();
        test_snapshot_pending_failure_returns_to_idle();
        test_mark_recording_started_clears_pending();
        test_finalize_stage_labels();
        test_snapshot_finalize_pending_keeps_finalizing_and_shows_progress();
        test_snapshot_finalize_pending_never_overrides_active_recording();
        test_status_line_idle_and_error_fallback();
        test_status_line_running_and_degraded();
        test_status_line_error_states();
        test_status_line_storage_and_rolling();
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << std::endl;
        return 1;
    }
    std::cout << "All session_status tests passed." << std::endl;
    return 0;
}
