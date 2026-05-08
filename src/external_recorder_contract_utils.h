#pragma once

#include "json.hpp"

#include <string>

struct CameraEachSelect;
struct CameraParams;

namespace orange::external_recorder {

struct SupervisorPlan;
struct SupervisorRuntimeState;

inline constexpr const char* kGuiExternalRecorderNotImplementedReason =
    "external recorder GUI supervision is not implemented yet; use headless supervised spec or in-process recording";

struct CameraContractMaterializationInput {
    const nlohmann::json* contract_config = nullptr;
    std::string recording_folder;
    std::string recording_id;
    const CameraParams* cameras_params = nullptr;
    const CameraEachSelect* cameras_select = nullptr;
    int num_cameras = 0;
};

struct FailFastArtifactOptions {
    std::string recording_folder;
    std::string recording_id;
    std::string producer = "orange_gui";
    std::string reason = kGuiExternalRecorderNotImplementedReason;
    nlohmann::json contract = nlohmann::json::object();
};

struct FailFastArtifactResult {
    bool ok = false;
    std::string error_message;
    std::string external_recorder_contract_path;
    std::string external_recorder_supervisor_plan_path;
    std::string recording_session_path;
};

struct SupervisedSessionArtifactOptions {
    std::string artifact_root;
    nlohmann::json contract = nlohmann::json::object();
    const SupervisorPlan* supervisor_plan = nullptr;
};

struct SupervisedSessionArtifactResult {
    bool ok = false;
    std::string error_message;
    std::string external_recorder_session_path;
    std::string external_recorder_supervisor_plan_path;
};

struct ArtifactWriteResult {
    bool ok = false;
    std::string error_message;
    std::string path;
};

struct SupervisorRuntimeArtifactOptions {
    std::string artifact_root;
    const SupervisorRuntimeState* runtime = nullptr;
};

struct VerifierHandoffArtifactOptions {
    std::string artifact_root;
    std::string analytics_root;
    std::string verifier_path = "scripts/verify_external_recorder_session.py";
    std::string status = "pending_runs_json_and_video_sanity";
    bool require_video_sanity = true;
};

struct FinalizationManifestOptions {
    std::string experiment_root;
    std::string artifact_root;
    std::string run_id;
    std::string status = "running";
    std::string started_at_utc;
    std::string finished_at_utc;
    std::string error;
    const nlohmann::json* video_sanity = nullptr;
    const nlohmann::json* verifier = nullptr;
};

nlohmann::json ExtractExternalRecorderContractObject(const nlohmann::json& payload);

bool ReadExternalRecorderContractConfigFile(const std::string& path,
                                            nlohmann::json* contract_out,
                                            std::string* error_out = nullptr);

nlohmann::json MaterializeExternalRecorderContractForCameras(
    const CameraContractMaterializationInput& input);

FailFastArtifactResult WriteExternalRecorderFailFastArtifacts(
    const FailFastArtifactOptions& options);

SupervisedSessionArtifactResult WriteExternalRecorderSupervisedSessionArtifacts(
    const SupervisedSessionArtifactOptions& options);

ArtifactWriteResult WriteExternalRecorderSupervisorRuntimeArtifact(
    const SupervisorRuntimeArtifactOptions& options);

nlohmann::json BuildExternalRecorderVerifierHandoff(
    const VerifierHandoffArtifactOptions& options);

ArtifactWriteResult WriteExternalRecorderVerifierHandoffArtifact(
    const VerifierHandoffArtifactOptions& options);

nlohmann::json BuildExternalRecorderFinalizationManifest(
    const FinalizationManifestOptions& options);

ArtifactWriteResult WriteExternalRecorderFinalizationArtifact(
    const std::string& artifact_root,
    const nlohmann::json& finalization);

}  // namespace orange::external_recorder
