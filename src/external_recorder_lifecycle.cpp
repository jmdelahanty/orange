#include "external_recorder_lifecycle.h"

#include "external_recorder_contract_utils.h"

#include <array>
#include <cstdlib>
#include <filesystem>
#include <unistd.h>

namespace orange::external_recorder {
namespace {

void set_error(std::string* error_out, const std::string& message)
{
    if (error_out) {
        *error_out = message;
    }
}

void write_runtime_artifact(SupervisedRecorderLifecycleState* state)
{
    if (!state || state->artifact_root.empty()) {
        return;
    }
    SupervisorRuntimeArtifactOptions artifact_options;
    artifact_options.artifact_root = state->artifact_root;
    artifact_options.runtime = &state->runtime;
    const ArtifactWriteResult result =
        WriteExternalRecorderSupervisorRuntimeArtifact(artifact_options);
    if (!result.ok) {
        state->last_artifact_error = result.error_message;
    }
}

void write_verifier_handoff_artifact(SupervisedRecorderLifecycleState* state)
{
    if (!state || state->artifact_root.empty()) {
        return;
    }
    VerifierHandoffArtifactOptions handoff_options;
    handoff_options.artifact_root = state->artifact_root;
    handoff_options.analytics_root = state->analytics_root;
    handoff_options.verifier_path = state->verifier_path;
    handoff_options.require_video_sanity = state->plan.require_video_sanity;
    handoff_options.require_status = state->plan.require_status;
    handoff_options.require_status_runtime = state->plan.require_status_runtime;
    handoff_options.require_storage_preflight = state->plan.require_storage_preflight;
    handoff_options.require_protocol_hello = state->plan.require_protocol_hello;
    const ArtifactWriteResult result =
        WriteExternalRecorderVerifierHandoffArtifact(handoff_options);
    if (!result.ok) {
        state->last_artifact_error = result.error_message;
    }
}

}  // namespace

class ScopedExternalRecorderEnvOverride {
public:
    ScopedExternalRecorderEnvOverride(const std::string& name, const std::string& value)
        : name_(name)
    {
        if (name_.empty()) {
            return;
        }
        const char* existing = std::getenv(name_.c_str());
        if (existing) {
            had_original_ = true;
            original_value_ = existing;
        }
        setenv(name_.c_str(), value.c_str(), 1);
        active_ = true;
    }

    ~ScopedExternalRecorderEnvOverride()
    {
        if (!active_ || name_.empty()) {
            return;
        }
        if (had_original_) {
            setenv(name_.c_str(), original_value_.c_str(), 1);
        } else {
            unsetenv(name_.c_str());
        }
    }

private:
    std::string name_;
    std::string original_value_;
    bool had_original_ = false;
    bool active_ = false;
};

SupervisedRecorderLifecycleState::SupervisedRecorderLifecycleState() = default;
SupervisedRecorderLifecycleState::~SupervisedRecorderLifecycleState() = default;
SupervisedRecorderLifecycleState::SupervisedRecorderLifecycleState(
    SupervisedRecorderLifecycleState&&) noexcept = default;
SupervisedRecorderLifecycleState& SupervisedRecorderLifecycleState::operator=(
    SupervisedRecorderLifecycleState&&) noexcept = default;

std::string ResolveExternalRecorderToolPath(const std::string& configured_tool_path)
{
    if (!configured_tool_path.empty()) {
        return configured_tool_path;
    }
    if (const char* env = std::getenv("ORANGE_EXTERNAL_RECORDER_TOOL"); env && *env) {
        return env;
    }

    std::array<char, 4096> exe_path{};
    const ssize_t size = readlink("/proc/self/exe", exe_path.data(), exe_path.size() - 1);
    if (size > 0) {
        exe_path[static_cast<std::size_t>(size)] = '\0';
        const std::filesystem::path sibling =
            std::filesystem::path(exe_path.data()).parent_path() /
            "external_recorder_ipc_probe";
        if (std::filesystem::exists(sibling)) {
            return sibling.string();
        }
    }
    return "external_recorder_ipc_probe";
}

bool StartSupervisedRecorderLifecycle(const SupervisedRecorderLifecycleOptions& options,
                                      SupervisedRecorderLifecycleState* state_out,
                                      std::string* error_out)
{
    if (!state_out) {
        set_error(error_out, "internal error: null external recorder lifecycle state");
        return false;
    }
    if (state_out->started) {
        set_error(error_out, "external recorder lifecycle is already started");
        return false;
    }

    SupervisedRecorderLifecycleState state;
    state.process_options = options.process_options;
    state.analytics_root = options.analytics_root;
    state.verifier_path = options.verifier_path.empty()
                              ? "scripts/verify_external_recorder_session.py"
                              : options.verifier_path;

    SupervisorPlanOptions plan_options;
    plan_options.recorder_tool_path =
        ResolveExternalRecorderToolPath(options.recorder_tool_path);
    plan_options.default_session_id = options.default_session_id;
    if (!BuildSupervisorPlanFromContract(
            options.contract,
            plan_options,
            &state.plan,
            error_out)) {
        return false;
    }
    state.artifact_root = state.plan.artifact_root;

    if (state.plan.require_storage_preflight && state.plan.storage_budget.enabled) {
        DurationAwareStoragePreflight storage_preflight;
        std::vector<SupervisorPlan*> storage_plans = {&state.plan};
        std::string storage_error;
        const bool storage_ok = RunDurationAwareStoragePreflight(
            storage_plans,
            &storage_preflight,
            &storage_error);
        const std::filesystem::path storage_artifact =
            std::filesystem::path(state.artifact_root) /
            "duration_aware_storage_preflight.json";
        std::string artifact_error;
        if (!WriteDurationAwareStoragePreflightArtifact(
                storage_artifact.string(),
                storage_preflight,
                &artifact_error)) {
            set_error(error_out, artifact_error);
            *state_out = std::move(state);
            return false;
        }
        if (!storage_ok) {
            set_error(
                error_out,
                storage_error.empty()
                    ? "duration-aware storage preflight failed"
                    : storage_error);
            *state_out = std::move(state);
            return false;
        }
    }

    SupervisedSessionArtifactOptions artifact_options;
    artifact_options.artifact_root = state.artifact_root;
    artifact_options.contract = options.contract;
    artifact_options.supervisor_plan = &state.plan;
    const SupervisedSessionArtifactResult artifact_result =
        WriteExternalRecorderSupervisedSessionArtifacts(artifact_options);
    if (!artifact_result.ok) {
        set_error(error_out, artifact_result.error_message);
        return false;
    }

    state.env_overrides.push_back(
        std::make_unique<ScopedExternalRecorderEnvOverride>(
            "ORANGE_EXTERNAL_RECORDER_SESSION_ID",
            state.plan.session_id));
    for (const RecorderStreamPlan& stream : state.plan.streams) {
        const std::string env_key =
            stream.env_key.empty() ? stream.stream_id : stream.env_key;
        state.env_overrides.push_back(
            std::make_unique<ScopedExternalRecorderEnvOverride>(
                "ORANGE_EXTERNAL_RECORDER_SESSION_ID_STREAM_" + stream.stream_id,
                state.plan.session_id));
        state.env_overrides.push_back(
            std::make_unique<ScopedExternalRecorderEnvOverride>(
                "ORANGE_EXTERNAL_RECORDER_SOCKET_STREAM_" + stream.stream_id,
                stream.socket_path));
        state.env_overrides.push_back(
            std::make_unique<ScopedExternalRecorderEnvOverride>(
                "ORANGE_EXTERNAL_RECORDER_SESSION_ID_CAM_" + env_key,
                state.plan.session_id));
        state.env_overrides.push_back(
            std::make_unique<ScopedExternalRecorderEnvOverride>(
                "ORANGE_EXTERNAL_RECORDER_SOCKET_CAM_" + env_key,
                stream.socket_path));
    }

    if (!StartSupervisorProcesses(
            state.plan,
            state.process_options,
            &state.runtime,
            error_out)) {
        state.env_overrides.clear();
        write_runtime_artifact(&state);
        *state_out = std::move(state);
        return false;
    }

    state.started = true;
    *state_out = std::move(state);
    return true;
}

bool StopSupervisedRecorderLifecycle(SupervisedRecorderLifecycleState* state,
                                     std::string* error_out)
{
    if (!state || !state->started) {
        return true;
    }

    const bool stopped =
        StopSupervisorProcesses(&state->runtime, state->process_options, error_out);
    state->started = false;
    write_runtime_artifact(state);
    write_verifier_handoff_artifact(state);
    state->env_overrides.clear();
    return stopped;
}

bool RefreshSupervisedRecorderLifecycle(SupervisedRecorderLifecycleState* state,
                                        std::string* error_out)
{
    if (!state || !state->started) {
        if (error_out) {
            error_out->clear();
        }
        return true;
    }

    const bool ok = PollSupervisorProcesses(&state->runtime, error_out);
    if (!ok && error_out && !error_out->empty()) {
        state->last_runtime_error = *error_out;
    }
    return ok;
}

}  // namespace orange::external_recorder
