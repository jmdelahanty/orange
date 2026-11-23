#pragma once

// After every finalized GUI recording, run scripts/validate_gui_ptp_recording.py
// on it (camera frame gaps and drops, recorder counts, NIC port counters, ...)
// in a child process: as the sudo invoker, nice 19, idle I/O class, on the
// host's non-isolated cores. Its result goes to
// ~/orange_data/recording_validation/<recording>.json (and .log), outside the
// recording folder, so a sealed folder never changes after finalize.
//
// The validator takes about a minute of one core and ~2 GB of memory on a
// 30-minute four-camera recording, so it never overlaps a recording: a
// recording start cancels a run still in flight (rerun it by hand with the
// command it prints).
//
// GUI thread only.

#include <chrono>
#include <functional>
#include <string>
#include <vector>

#include <sys/types.h>

namespace orange::gui {

struct PostFinalizeValidationView {
    std::string recording_name;
    std::string status;  // "", waiting_for_receipts, running, pass, fail, error, cancelled
    int failures = 0;
    int warnings = 0;
    double elapsed_seconds = 0.0;
    std::vector<std::string> failure_lines;
    std::string json_path;
    std::string detail;  // error or cancel reason
};

// Starts the validator for a finalized recording folder; a run still in flight
// for an earlier recording is cancelled first. Failing to start is reported
// in the view (status "error"), never to the recording.
void StartPostFinalizeValidation(const std::string& recording_folder);

// Citrus's receipts for a stimulus_experiment recording land a few seconds
// after finalize (finalize_recording_observation_bindings); a validator started
// before them reports "stimulus_experiment recording without a finalized
// Citrus binding" (keep-open runs, 2026-11-22). The GUI therefore schedules
// the validator with the same wait the exit-after-finalize path uses
// (session::CitrusReceiptsExpected / CitrusReceiptWaitLimit).
struct PostFinalizeValidationReceiptWait {
    bool expected = false;            // CitrusReceiptsExpected(folder)
    std::chrono::seconds limit{0};    // CitrusReceiptWaitLimit(folder)
    std::function<bool()> finalized;  // receipts stored for the folder (GUI thread)
};

// Whether a scheduled validator may start now: no receipts are due, they are
// stored, or the limit has passed since finalize.
bool PostFinalizeValidationMayStart(
    const PostFinalizeValidationReceiptWait& wait,
    std::chrono::steady_clock::duration since_finalize);

// Schedules the validator for a finalized folder: it starts at once when no
// receipts are due, else from PollPostFinalizeValidation once
// PostFinalizeValidationMayStart says so. Replaces (cancels) an earlier
// scheduled or running validation.
void SchedulePostFinalizeValidation(
    const std::string& recording_folder,
    PostFinalizeValidationReceiptWait wait);

// The GUI is exiting: start a validator still waiting for receipts now (the
// control server is down, so no receipt can arrive any more). No-op otherwise.
void StartScheduledPostFinalizeValidationNow(const char* reason);

// Reaps a finished run and prints its result, and starts a scheduled one
// whose receipts arrived; call once per frame.
void PollPostFinalizeValidation();

// Tests only: replaces the child-process start (nullptr restores it).
void SetPostFinalizeValidationStarterForTesting(
    std::function<void(const std::string& recording_folder)> starter);

// A recording is starting: stop a run in flight (or drop one still waiting
// for receipts) so it cannot load the host.
void CancelPostFinalizeValidation(const char* reason);

PostFinalizeValidationView PostFinalizeValidationStatus();

}  // namespace orange::gui
