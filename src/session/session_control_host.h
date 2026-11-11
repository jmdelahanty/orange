#pragma once

// The Orange local-control endpoint of a recording process, shared by the GUI
// and the headless client (docs/session_engine_design_2026_11_02.md, slice 5).
//
// It owns the control server (orange::control::LocalControlServer), the
// Citrus receipt finalization (RecordingObservationFinalizationJobs) and the
// decision of what a Citrus completion or stop request does to the current
// recording. The owner process feeds it the recording state and acts on the
// stop it reports. Socket path and event-log naming are the GUI's, so Citrus
// reaches either binary the same way (CITRUS_ORANGE_LOCAL_CONTROL_SOCKET).
//
// Receipts are written by this process: a recording bound to Citrus is only
// complete once finalize_recording_observation_bindings has been accepted for
// its folder, so a process that ends after a recording waits for that
// (WaitForReceipts) before it exits.

#include "json.hpp"
#include "orange_local_control.h"
#include "session/recording_observation_finalization.h"

#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <vector>

namespace orange::session {

// ORANGE_GUI_LOCAL_CONTROL_SOCKET, else ORANGE_LOCAL_CONTROL_SOCKET, else
// /tmp/orange_local_control.sock.
std::string LocalControlSocketPathFromEnvironment();
// ORANGE_GUI_LOCAL_CONTROL_LOG, else ORANGE_LOCAL_CONTROL_LOG, else
// <socket>.events.jsonl.
std::string LocalControlEventLogPathFromEnvironment(const std::string& socket_path);

// What a citrus_completion or stop_recording request does to the recording.
enum class StopDecision {
    kNotAStop,               // another method
    kScheduled,              // a stop is now due at the deadline
    kKeptEarlierDeadline,    // a stop was already due no later
    kIgnoredDisabled,        // this process does not accept that method
    kIgnoredNotRecording,    // nothing is recording
    kIgnoredNotCitrusBound,  // citrus_completion for a recording Citrus is not bound to
};
const char* StopDecisionName(StopDecision decision);

struct StopSchedule {
    bool scheduled = false;
    std::chrono::steady_clock::time_point deadline{};
    double grace_seconds = 0.0;
    std::string method;
    std::string request_id;
    std::string operation_id;
    std::string source;
    std::string experiment_id;
    std::string terminal_state;
    std::string reason;
    std::string received_at_utc;
};

struct StopInputs {
    bool stop_recording_enabled = false;
    bool citrus_completion_enabled = false;
    bool recording_active = false;
    // BindingModeFromRecordingIntent of the current recording: "required"
    // (stimulus_experiment), "not_applicable" (recording_only), "" unknown.
    std::string binding_mode;
};

// Citrus completion: default grace 10 s (the GUI's); stop_recording: 0 s.
// params.grace_seconds overrides when it is a finite number >= 0. Earliest
// deadline wins when a stop is already scheduled.
StopDecision DecideStop(
    const orange::control::PendingLocalControlCommand& command,
    const StopInputs& inputs,
    std::chrono::steady_clock::time_point now,
    StopSchedule* schedule);

struct SessionControlHostOptions {
    std::string process = "orange_gui";  // status.process and event schema id
    std::string socket_path;
    std::string event_log_path;
    bool allow_start_recording = false;
    bool allow_stop_recording = false;
    bool allow_citrus_completion = false;
    // Extra work after a receipt verification succeeds. The host already logs
    // declaration/receipt disagreements and refreshes recording_session.json.
    RecordingObservationFinalizationJobs::AfterFinalized after_finalized;
    // Tests replace the receipt verification.
    RecordingObservationFinalizationJobs::Finalizer finalizer;
};

class SessionControlHost {
public:
    explicit SessionControlHost(SessionControlHostOptions options);
    ~SessionControlHost();
    SessionControlHost(const SessionControlHost&) = delete;
    SessionControlHost& operator=(const SessionControlHost&) = delete;

    // Starts (or restarts) the server with the options above.
    bool Start(std::string* error_out = nullptr);
    void Stop();
    bool listening() const { return server_.running(); }

    orange::control::LocalControlServer& server() { return server_; }
    const orange::control::LocalControlServerOptions& server_options() const { return server_options_; }
    const SessionControlHostOptions& options() const { return options_; }

    void UpdateStatus(const orange::control::LocalControlStatusSnapshot& snapshot) {
        server_.UpdateStatus(snapshot);
    }
    std::vector<orange::control::PendingLocalControlCommand> DrainPendingCommands() {
        return server_.DrainPendingCommands();
    }

    // One line in the event log: schema_id orange.local_control.gui_event for
    // the GUI, orange.local_control.<process>_event otherwise.
    void LogEvent(nlohmann::json event) const;

    // finalize_recording_observation_bindings was accepted (receipts verified
    // and persisted) for this recording folder.
    bool ReceiptsFinalized(const std::string& recording_folder) const;
    // Waits until ReceiptsFinalized(folder) or the timeout; true when finalized.
    bool WaitForReceipts(const std::string& recording_folder, std::chrono::milliseconds timeout) const;

private:
    nlohmann::json HandleFinalization(
        const nlohmann::json& params,
        const orange::control::LocalControlStatusSnapshot& status,
        std::string* error_out,
        std::string* error_code_out);
    bool AfterFinalized(
        const std::string& recording_folder,
        const RecordingObservationFinalizationResult& result,
        std::string* error_out);

    SessionControlHostOptions options_;
    mutable std::mutex receipts_mutex_;
    mutable std::condition_variable receipts_changed_;
    std::set<std::string> finalized_folders_;
    // Declared before the server so it outlives it.
    std::unique_ptr<RecordingObservationFinalizationJobs> finalization_jobs_;
    orange::control::LocalControlServerOptions server_options_;
    orange::control::LocalControlServer server_;
};

}  // namespace orange::session
