#pragma once

#include "external_recorder_supervisor.h"
#include "json.hpp"

#include <memory>
#include <string>
#include <vector>

namespace orange::external_recorder {

class ScopedExternalRecorderEnvOverride;

struct SupervisedRecorderLifecycleOptions {
    nlohmann::json contract = nlohmann::json::object();
    std::string recorder_tool_path;
    std::string default_session_id;
    std::string analytics_root;
    std::string verifier_path = "scripts/verify_external_recorder_session.py";
    SupervisorProcessOptions process_options;
};

struct SupervisedRecorderLifecycleState {
    SupervisedRecorderLifecycleState();
    ~SupervisedRecorderLifecycleState();
    SupervisedRecorderLifecycleState(SupervisedRecorderLifecycleState&&) noexcept;
    SupervisedRecorderLifecycleState& operator=(SupervisedRecorderLifecycleState&&) noexcept;
    SupervisedRecorderLifecycleState(const SupervisedRecorderLifecycleState&) = delete;
    SupervisedRecorderLifecycleState& operator=(const SupervisedRecorderLifecycleState&) = delete;

    SupervisorPlan plan;
    SupervisorRuntimeState runtime;
    SupervisorProcessOptions process_options;
    std::string artifact_root;
    std::string analytics_root;
    std::string verifier_path;
    std::string last_artifact_error;
    std::string last_runtime_error;
    bool started = false;
    std::vector<std::unique_ptr<ScopedExternalRecorderEnvOverride>> env_overrides;
};

std::string ResolveExternalRecorderToolPath(const std::string& configured_tool_path = {});

bool StartSupervisedRecorderLifecycle(const SupervisedRecorderLifecycleOptions& options,
                                      SupervisedRecorderLifecycleState* state_out,
                                      std::string* error_out);

bool StopSupervisedRecorderLifecycle(SupervisedRecorderLifecycleState* state,
                                     std::string* error_out);

bool RefreshSupervisedRecorderLifecycle(SupervisedRecorderLifecycleState* state,
                                        std::string* error_out);

}  // namespace orange::external_recorder
