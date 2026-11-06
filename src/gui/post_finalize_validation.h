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
#include <string>
#include <vector>

#include <sys/types.h>

namespace orange::gui {

struct PostFinalizeValidationView {
    std::string recording_name;
    std::string status;  // "", running, pass, fail, error, cancelled
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

// Reaps a finished run and prints its result; call once per frame.
void PollPostFinalizeValidation();

// A recording is starting: stop a run in flight so it cannot load the host.
void CancelPostFinalizeValidation(const char* reason);

PostFinalizeValidationView PostFinalizeValidationStatus();

}  // namespace orange::gui
