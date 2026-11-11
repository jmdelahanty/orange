#include "session/session_control_host.h"

#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <utility>

namespace orange::session {
namespace {

std::string EnvOrEmpty(const char* name)
{
    const char* value = std::getenv(name);
    return value != nullptr ? std::string(value) : std::string();
}

std::string UtcNow()
{
    const auto now = std::chrono::system_clock::now();
    const std::time_t seconds = std::chrono::system_clock::to_time_t(now);
    std::tm utc{};
    gmtime_r(&seconds, &utc);
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &utc);
    return buf;
}

std::string NormalFolder(const std::string& folder)
{
    std::filesystem::path path = std::filesystem::path(folder).lexically_normal();
    if (path.has_parent_path() && path.filename().empty()) path = path.parent_path();  // "/a/b/" -> "/a/b"
    return path.string();
}

double GraceSeconds(const nlohmann::json& params, double fallback)
{
    const auto it = params.find("grace_seconds");
    if (it == params.end() || !it->is_number()) return fallback;
    const double value = it->get<double>();
    return std::isfinite(value) && value >= 0.0 ? value : fallback;
}

std::string ParamString(const nlohmann::json& params, const char* key)
{
    const auto it = params.find(key);
    return it != params.end() && it->is_string() ? it->get<std::string>() : std::string();
}

}  // namespace

std::string LocalControlSocketPathFromEnvironment()
{
    std::string path = EnvOrEmpty("ORANGE_GUI_LOCAL_CONTROL_SOCKET");
    if (path.empty()) path = EnvOrEmpty("ORANGE_LOCAL_CONTROL_SOCKET");
    return path.empty() ? "/tmp/orange_local_control.sock" : path;
}

std::string LocalControlEventLogPathFromEnvironment(const std::string& socket_path)
{
    std::string path = EnvOrEmpty("ORANGE_GUI_LOCAL_CONTROL_LOG");
    if (path.empty()) path = EnvOrEmpty("ORANGE_LOCAL_CONTROL_LOG");
    return path.empty() ? socket_path + ".events.jsonl" : path;
}

const char* StopDecisionName(StopDecision decision)
{
    switch (decision) {
    case StopDecision::kNotAStop: return "not_a_stop";
    case StopDecision::kScheduled: return "scheduled";
    case StopDecision::kKeptEarlierDeadline: return "kept_existing_earlier_deadline";
    case StopDecision::kIgnoredDisabled: return "ignored_disabled";
    case StopDecision::kIgnoredNotRecording: return "ignored_not_recording";
    case StopDecision::kIgnoredNotCitrusBound: return "ignored_not_citrus_bound";
    }
    return "unknown";
}

StopDecision DecideStop(
    const orange::control::PendingLocalControlCommand& command,
    const StopInputs& inputs,
    std::chrono::steady_clock::time_point now,
    StopSchedule* schedule)
{
    const bool completion = command.method == "citrus_completion";
    if (!completion && command.method != "stop_recording") return StopDecision::kNotAStop;
    if (completion ? !inputs.citrus_completion_enabled : !inputs.stop_recording_enabled) {
        return StopDecision::kIgnoredDisabled;
    }
    if (!inputs.recording_active) return StopDecision::kIgnoredNotRecording;
    // Citrus can only end a recording it is bound to.
    if (completion && inputs.binding_mode != "required") return StopDecision::kIgnoredNotCitrusBound;

    const double grace = GraceSeconds(command.params, completion ? 10.0 : 0.0);
    const auto deadline =
        now + std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                  std::chrono::duration<double>(grace));
    if (schedule == nullptr) return StopDecision::kScheduled;
    if (schedule->scheduled && schedule->deadline <= deadline) {
        return StopDecision::kKeptEarlierDeadline;
    }
    schedule->scheduled = true;
    schedule->deadline = deadline;
    schedule->grace_seconds = grace;
    schedule->method = command.method;
    schedule->request_id = command.request_id;
    schedule->operation_id = command.operation_id;
    schedule->source = command.source;
    schedule->experiment_id = ParamString(command.params, "experiment_id");
    schedule->terminal_state = ParamString(command.params, "terminal_state");
    schedule->reason = ParamString(command.params, "reason");
    schedule->received_at_utc = command.received_at_utc;
    return StopDecision::kScheduled;
}

SessionControlHost::SessionControlHost(SessionControlHostOptions options)
    : options_(std::move(options))
{
    RecordingObservationFinalizationJobs::Options jobs_options;
    if (options_.finalizer) jobs_options.finalizer = options_.finalizer;
    jobs_options.after_finalized =
        [this](const std::string& folder,
               const RecordingObservationFinalizationResult& result,
               std::string* error_out) { return AfterFinalized(folder, result, error_out); };
    finalization_jobs_ = std::make_unique<RecordingObservationFinalizationJobs>(std::move(jobs_options));

    server_options_.socket_path = options_.socket_path.empty()
        ? LocalControlSocketPathFromEnvironment()
        : options_.socket_path;
    server_options_.event_log_path = options_.event_log_path.empty()
        ? LocalControlEventLogPathFromEnvironment(server_options_.socket_path)
        : options_.event_log_path;
    options_.socket_path = server_options_.socket_path;
    options_.event_log_path = server_options_.event_log_path;
    server_options_.allow_gui_lifecycle_commands = false;
    server_options_.allow_gui_start_recording_commands = options_.allow_start_recording;
    server_options_.allow_gui_stop_recording_commands = options_.allow_stop_recording;
    server_options_.allow_gui_citrus_completion_commands = options_.allow_citrus_completion;
    server_options_.recording_observation_finalization_handler =
        [this](const nlohmann::json& params,
               const orange::control::LocalControlStatusSnapshot& status,
               std::string* error_out,
               std::string* error_code_out) {
            return HandleFinalization(params, status, error_out, error_code_out);
        };
}

SessionControlHost::~SessionControlHost()
{
    server_.Stop();
}

bool SessionControlHost::Start(std::string* error_out)
{
    if (server_.running()) return true;
    return server_.Start(server_options_, error_out);
}

void SessionControlHost::Stop()
{
    server_.Stop();
}

void SessionControlHost::LogEvent(nlohmann::json event) const
{
    if (options_.event_log_path.empty()) return;
    // The GUI's events keep their schema id (orange.local_control.gui_event).
    event["schema_id"] = options_.process == "orange_gui"
        ? std::string("orange.local_control.gui_event")
        : "orange.local_control." + options_.process + "_event";
    event["schema_version"] = 1;
    if (!event.contains("event_at_utc")) event["event_at_utc"] = UtcNow();
    std::string error;
    if (!orange::control::AppendLocalControlEventLog(options_.event_log_path, event, &error)) {
        std::cerr << "[" << options_.process << "][local_control] event log write failed: " << error << std::endl;
    }
}

nlohmann::json SessionControlHost::HandleFinalization(
    const nlohmann::json& params,
    const orange::control::LocalControlStatusSnapshot& status,
    std::string* error_out,
    std::string* error_code_out)
{
    const std::string requested_folder = params.value("recording_folder", "");
    if (!status.recording_folder.empty() &&
        NormalFolder(status.recording_folder) != NormalFolder(requested_folder)) {
        if (error_out != nullptr) {
            *error_out = "Citrus receipt folder conflicts with Orange's current recording folder";
        }
        return nlohmann::json::object();
    }
    if (!status.recording_folder.empty() && !status.recording_active &&
        !status.recording_finalizing && !status.recording_finalized) {
        if (error_out != nullptr) {
            *error_out = "Orange's current recording is not in a receipt-eligible lifecycle state";
        }
        return nlohmann::json::object();
    }
    const std::string folder = status.recording_folder.empty() ? requested_folder : status.recording_folder;
    const auto answer = finalization_jobs_->Submit(folder, params);
    if (!answer.accepted) {
        if (error_out != nullptr) *error_out = answer.error;
        if (error_code_out != nullptr) *error_code_out = answer.error_code;
        return nlohmann::json::object();
    }
    return answer.effect;
}

bool SessionControlHost::AfterFinalized(
    const std::string& recording_folder,
    const RecordingObservationFinalizationResult& result,
    std::string* error_out)
{
    // Subject identity v4: a Citrus bug if it happens; recorded, never refused.
    for (const auto& line : result.subject_declaration_disagreements) {
        std::cerr << "[" << options_.process << "][observation_binding] subject declaration and receipt "
                     "disagree (recorded, not refused): " << line << std::endl;
    }
    std::string refresh_error;
    if (!refresh_recording_session_observation_bindings(recording_folder, &refresh_error)) {
        if (error_out != nullptr) {
            *error_out = "receipts were persisted, but the finalized recording_session.json refresh failed: " +
                         refresh_error;
        }
        return false;
    }
    if (options_.after_finalized && !options_.after_finalized(recording_folder, result, error_out)) {
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(receipts_mutex_);
        finalized_folders_.insert(NormalFolder(recording_folder));
    }
    receipts_changed_.notify_all();
    return true;
}

bool SessionControlHost::ReceiptsFinalized(const std::string& recording_folder) const
{
    std::lock_guard<std::mutex> lock(receipts_mutex_);
    return finalized_folders_.count(NormalFolder(recording_folder)) != 0;
}

bool SessionControlHost::WaitForReceipts(
    const std::string& recording_folder,
    std::chrono::milliseconds timeout) const
{
    const std::string key = NormalFolder(recording_folder);
    std::unique_lock<std::mutex> lock(receipts_mutex_);
    return receipts_changed_.wait_for(lock, timeout, [&] { return finalized_folders_.count(key) != 0; });
}

}  // namespace orange::session
