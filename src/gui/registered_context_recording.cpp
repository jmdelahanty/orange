#include "gui/registered_context_recording.h"
#include "imgui.h"

namespace orange::gui {
namespace {
using json = nlohmann::json;
json options = {{"schema_version", 1}, {"enabled", false}};
std::string config_path, error, status;
bool confirmed = false;
void save() {
    recording::SaveGuiRecordingEvidenceConfig(config_path,
        recording::GuiRecordingEvidenceConfig::Parse(options));
    status = "Saved GUI recording context settings (scene confirmation is not restored on launch).";
}
}
void LoadRegisteredContextRecordingSettings(const std::string& path) {
    config_path = path; confirmed = false; error.clear();
    options = {{"schema_version", 1}, {"enabled", false}};
    try {
        options = recording::ReadGuiRecordingEvidenceConfig(config_path).ToJson();
    } catch (const std::exception& ex) { error = ex.what(); }
}
void SelectDailyContextForGuiRecording(const json& context) {
    auto c = recording::RegisteredContextConfig::Parse(context);
    recording::MasterAcquisitionConfig master;
    master.enabled = true;
    master.writer_cpu_ids = c.worker_cpu_ids;
    options = recording::GuiRecordingEvidenceConfig::Parse({{"schema_version", 1}, {"enabled", true},
        {"master_frame_journal", master.ToJson()}, {"registered_scene_context", context}}).ToJson();
    error.clear(); confirmed = true;
    status = "Daily context selected for the next GUI or Citrus-triggered recording. Save settings to retain the selection.";
}
recording::GuiRecordingEvidenceConfig RegisteredContextRecordingConfigForArm() {
    if (!error.empty()) throw std::runtime_error("GUI registered-context configuration: " + error);
    const auto config = recording::GuiRecordingEvidenceConfig::Parse(options);
    if (config.enabled && !confirmed)
        throw std::runtime_error("Confirm the scene is unchanged in Registered recording context before starting this recording");
    return config;
}
void ConsumeRegisteredContextRecordingConfirmation() { confirmed = false; }
void RenderRegisteredContextRecordingSettings(bool locked) {
    if (!ImGui::CollapsingHeader("Registered recording context (optional)")) return;
    ImGui::BeginDisabled(locked);
    bool enabled = options.value("enabled", false);
    if (ImGui::Checkbox("Bind daily native context and write master frame journal", &enabled)) {
        options["enabled"] = enabled; confirmed = false;
    }
    if (options.contains("registered_scene_context")) {
        ImGui::TextWrapped("Context: %s", options.at("registered_scene_context").at("source").at("descriptor_path").get<std::string>().c_str());
        ImGui::TextWrapped("Housekeeping CPUs: %s", options.at("master_frame_journal").at("writer_cpu_ids").dump().c_str());
    } else ImGui::TextWrapped("Capture and select a context in Daily Registration, or paste a saved configuration below.");
    ImGui::Checkbox("Scene unchanged for the next recording (required each run)", &confirmed);
    if (ImGui::Button("Paste context recording configuration")) {
        try {
            const char* clipboard = ImGui::GetClipboardText();
            const auto j = json::parse(clipboard ? clipboard : "");
            if (j.contains("registered_scene_context") && !j.contains("master_frame_journal"))
                SelectDailyContextForGuiRecording(j.at("registered_scene_context"));
            else options = recording::GuiRecordingEvidenceConfig::Parse(j).ToJson();
            confirmed = false; error.clear();
        } catch (const std::exception& ex) { error = ex.what(); }
    }
    if (ImGui::Button("Save context recording settings")) {
        try { save(); error.clear(); } catch (const std::exception& ex) { error = ex.what(); }
    }
    ImGui::SameLine();
    if (ImGui::Button("Reload saved context settings")) LoadRegisteredContextRecordingSettings(config_path);
    ImGui::EndDisabled();
    if (!error.empty()) ImGui::TextWrapped("Recording blocked: %s", error.c_str());
    if (!status.empty()) ImGui::TextWrapped("%s", status.c_str());
    ImGui::TextWrapped("Full-frame and moving-crop media choices are unchanged. Context import and digest checks run before arm. "
        "Select housekeeping CPUs from the rig isolation plan; never acquisition/render cores. "
        "CPU lists and journal queue capacity can be edited in the versioned configuration.");
}
} // namespace orange::gui
