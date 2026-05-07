#pragma once

#include "json.hpp"

#include <string>

struct CameraEachSelect;
struct CameraParams;

namespace orange::external_recorder {

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

nlohmann::json ExtractExternalRecorderContractObject(const nlohmann::json& payload);

bool ReadExternalRecorderContractConfigFile(const std::string& path,
                                            nlohmann::json* contract_out,
                                            std::string* error_out = nullptr);

nlohmann::json MaterializeExternalRecorderContractForCameras(
    const CameraContractMaterializationInput& input);

FailFastArtifactResult WriteExternalRecorderFailFastArtifacts(
    const FailFastArtifactOptions& options);

}  // namespace orange::external_recorder
