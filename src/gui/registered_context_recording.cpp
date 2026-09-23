#include "gui/registered_context_recording.h"
#include "imgui.h"

namespace orange::gui {
namespace {
using json = nlohmann::json;
json options = {{"schema_version", 1}, {"enabled", false}};
std::string config_path, error, status;
recording::RecordingMediaSelection media_selection;
std::string media_error, media_status;
bool confirmed = false;
void save() {
    recording::SaveGuiRecordingEvidenceConfig(config_path,
        recording::GuiRecordingEvidenceConfig::Parse(options));
    status = "Saved GUI recording context settings (scene confirmation is not restored on launch).";
}
}
void LoadRegisteredContextRecordingSettings(const std::string& path, bool load_media) {
    config_path = path; confirmed = false; error.clear();
    options = {{"schema_version", 1}, {"enabled", false}};
    try {
        options = recording::ReadGuiRecordingEvidenceConfig(config_path).ToJson();
    } catch (const std::exception& ex) { error = ex.what(); }
    if (load_media) {
        media_selection = {}; media_error.clear();
        try { media_selection = recording::ReadGuiRecordingMediaSelection(path); }
        catch (const std::exception& ex) { media_error = ex.what(); }
    }
}
recording::RecordingMediaSelection RecordingMediaSelectionForStream() {
    if (!media_error.empty()) throw std::runtime_error("GUI media selection: " + media_error);
    return media_selection;
}
void RenderRecordingMediaSelection(bool stream_locked) {
    if (!ImGui::CollapsingHeader("Recording media products")) return;
    ImGui::BeginDisabled(stream_locked);
    int selected = !media_selection.mode ? 0 :
        (*media_selection.mode == recording::RecordingMediaMode::FullFrame ? 1 :
         *media_selection.mode == recording::RecordingMediaMode::FullFrameAndMovingCrops ? 2 : 3);
    const char* choices[] = {"Existing per-camera choices", "Full-frame video",
        "Full-frame video + moving crops", "Registered context + moving crops"};
    bool changed = false;
    if (ImGui::BeginCombo("Media products", choices[selected])) {
        for (int i = 0; i < 4; ++i) {
            if (ImGui::Selectable(choices[i], selected == i)) { selected = i; changed = true; }
        }
        ImGui::EndCombo();
    }
    if (changed) {
        media_selection.mode.reset();
        if (selected == 1) media_selection.mode = recording::RecordingMediaMode::FullFrame;
        if (selected == 2) media_selection.mode = recording::RecordingMediaMode::FullFrameAndMovingCrops;
        if (selected == 3) media_selection.mode = recording::RecordingMediaMode::RegisteredContextAndMovingCrops;
        media_error.clear(); media_status = "Selection applies at the next stream startup; not saved yet.";
    }
    if (ImGui::Button("Save media product selection")) {
        try {
            recording::SaveGuiRecordingMediaSelection(config_path, media_selection);
            media_error.clear(); media_status = "Saved recording.media_products; encoder settings are unchanged.";
        } catch (const std::exception& ex) { media_error = ex.what(); }
    }
    ImGui::EndDisabled();
    ImGui::TextWrapped("Choose before streaming. Record still controls session membership. "
        "An explicit product choice determines moving-crop recording for each participating camera. "
        "Native and split-GOP full-frame recording remain supported.");
    if (media_selection.RequiresContext())
        ImGui::TextWrapped("Requires a selected daily context, a master frame journal, full-rate YOLO/event logging and external moving crops. "
            "Confirm the scene before each recording. No continuous full-frame video is encoded. Live rig acceptance remains pending.");
    if (!media_error.empty()) ImGui::TextWrapped("Startup blocked: %s", media_error.c_str());
    if (!media_status.empty()) ImGui::TextWrapped("%s", media_status.c_str());
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
    if (media_selection.RequiresContext() && !config.enabled)
        throw std::runtime_error("Crop-only recording requires a selected daily registered context and master frame journal");
    if (config.enabled && !confirmed)
        throw std::runtime_error("Confirm the scene is unchanged in Registered recording context before starting this recording");
    return config;
}
void ConsumeRegisteredContextRecordingConfirmation() { confirmed = false; }
void RenderRegisteredContextRecordingSettings(bool locked) {
    if (!ImGui::CollapsingHeader(media_selection.RequiresContext() ? "Registered recording context (required for crop-only)" :
            "Registered recording context (optional)")) return;
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
    if (ImGui::Button("Reload saved context settings")) LoadRegisteredContextRecordingSettings(config_path, false);
    ImGui::EndDisabled();
    if (!error.empty()) ImGui::TextWrapped("Recording blocked: %s", error.c_str());
    if (!status.empty()) ImGui::TextWrapped("%s", status.c_str());
    ImGui::TextWrapped("Full-frame and moving-crop media choices are unchanged. Context import and digest checks run before arm. "
        "Select housekeeping CPUs from the rig isolation plan; never acquisition/render cores. "
        "CPU lists and journal queue capacity can be edited in the versioned configuration.");
}
} // namespace orange::gui
