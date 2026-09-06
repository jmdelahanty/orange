#pragma once
#include "recording_master_acquisition.h"
#include "recording_registered_context.h"

namespace orange::recording {
// GUI v1 binds a saved Daily Registration context. Fresh native capture remains
// a separate daily workflow; this does not change the selected media products.
struct GuiRecordingEvidenceConfig {
    bool enabled = false;
    MasterAcquisitionConfig master;
    RegisteredContextConfig context;
    static GuiRecordingEvidenceConfig Parse(const nlohmann::json&);
    nlohmann::json ToJson() const;
};
GuiRecordingEvidenceConfig ReadGuiRecordingEvidenceConfig(const std::filesystem::path& app_config);
void SaveGuiRecordingEvidenceConfig(const std::filesystem::path& app_config,
                                    const GuiRecordingEvidenceConfig&);

// CPU-only prearm service. Inputs are frozen value snapshots, never live camera
// objects. Called on the existing GUI start worker before sealing start evidence.
struct GuiRecordingEvidence {
    std::shared_ptr<MasterAcquisitionSet> journals;
    nlohmann::json artifacts = nlohmann::json::object();
    void Prepare(const GuiRecordingEvidenceConfig&, const std::filesystem::path&,
                 std::vector<RegisteredContextCamera>, const nlohmann::json& geometry);
};
} // namespace orange::recording
