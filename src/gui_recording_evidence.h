#pragma once
#include "recording_master_acquisition.h"
#include "recording_registered_context.h"
#include "recording_media_plan.h"
#include "recording_context.h"
#include "recording_subject_reference.h"

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
// Media products moved to the camera configuration (camera JSON
// recording.media_products, 2026-11-15): an app config that still has
// recording.media_products is refused (throws), never read.
void RefuseAppConfigMediaProducts(const std::filesystem::path& app_config);
// recording.contexts: parent recording contexts (Citrus transfer-v2); absent ->
// unconfigured (no block emitted; validators report it).
RecordingContextsConfig ReadGuiRecordingContexts(const std::filesystem::path& app_config);
void SaveGuiRecordingContexts(const std::filesystem::path& app_config, const RecordingContextsConfig&);
// recording.subject_references (dish per camera) and recording.zebrobot
// (lookup base_url/timeout); both optional, unconfigured when absent.
SubjectReferencesConfig ReadGuiSubjectReferences(const std::filesystem::path& app_config);
void SaveGuiSubjectReferences(const std::filesystem::path& app_config, const SubjectReferencesConfig&);
ZebrobotLookupConfig ReadGuiZebrobotLookup(const std::filesystem::path& app_config);

// CPU-only prearm service. Inputs are frozen value snapshots, never live camera
// objects. Called on the existing GUI start worker before sealing start evidence.
struct GuiRecordingEvidence {
    std::shared_ptr<MasterAcquisitionSet> journals;
    nlohmann::json artifacts = nlohmann::json::object();
    void Prepare(const GuiRecordingEvidenceConfig&, const std::filesystem::path&,
                 std::vector<RegisteredContextCamera>, const nlohmann::json& geometry);
};
} // namespace orange::recording
