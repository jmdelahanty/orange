#include "gui/registered_context_recording.h"
#include <cstdio>
#include "imgui.h"

namespace orange::gui {
namespace {
using json = nlohmann::json;
json options = {{"schema_version", 1}, {"enabled", false}};
std::string config_path, error, status;
recording::RecordingMediaSelection media_selection;
std::string media_error, media_status;
recording::RecordingContextsConfig recording_contexts;
std::string contexts_error;
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
        recording_contexts = {}; contexts_error.clear();
        try { recording_contexts = recording::ReadGuiRecordingContexts(path); }
        catch (const std::exception& ex) { contexts_error = ex.what(); }
    }
}
recording::RecordingMediaSelection RecordingMediaSelectionForStream() {
    if (!media_error.empty()) throw std::runtime_error("GUI media selection: " + media_error);
    return media_selection;
}
recording::RecordingContextsConfig RecordingContextsForStream() {
    if (!contexts_error.empty()) throw std::runtime_error("GUI recording contexts: " + contexts_error);
    return recording_contexts;
}
namespace {
std::string contexts_status;
bool contexts_dirty = false;
// Editable copy of the default entry; per-camera overrides are shown, not edited here.
char ctx_type[256] = "";
char ctx_subtype[256] = "";
int ctx_mode = 0;    // free | embedded | none
int ctx_intent = 1;  // stimulus_experiment | recording_only
int ctx_origin = 0;  // acquired | synthetic
bool ctx_fields_loaded = false;
const char* kModes[] = {"free", "embedded", "none"};
const char* kIntents[] = {"stimulus_experiment", "recording_only"};
const char* kOrigins[] = {"acquired", "synthetic"};
int index_of(const char* const* options, int count, const std::string& value, int fallback) {
    for (int i = 0; i < count; ++i) if (value == options[i]) return i;
    return fallback;
}
void load_fields_from_config() {
    ctx_type[0] = ctx_subtype[0] = '\0';
    if (recording_contexts.default_context) {
        const auto& d = *recording_contexts.default_context;
        std::snprintf(ctx_type, sizeof ctx_type, "%s", d.recording_type.c_str());
        std::snprintf(ctx_subtype, sizeof ctx_subtype, "%s", d.recording_subtype.c_str());
        ctx_mode = index_of(kModes, 3, d.behavior_mode, 0);
        ctx_intent = index_of(kIntents, 2, d.recording_intent, 1);
        ctx_origin = index_of(kOrigins, 2, d.data_origin, 0);
    }
    ctx_fields_loaded = true;
    contexts_dirty = false;
}
bool apply_fields_to_config(std::string* error) {
    try {
        recording::RecordingContext d;
        d.recording_type = ctx_type; d.recording_subtype = ctx_subtype;
        d.behavior_mode = kModes[ctx_mode]; d.recording_intent = kIntents[ctx_intent]; d.data_origin = kOrigins[ctx_origin];
        recording::RecordingContext::Parse(d.ToJson());  // same rules as the config parser
        recording_contexts.default_context = d;
        contexts_error.clear();
        return true;
    } catch (const std::exception& ex) { if (error) *error = ex.what(); return false; }
}
}  // namespace
void RenderRecordingContextSelection(bool locked) {
    if (!ImGui::CollapsingHeader("Recording context (what this recording is)")) return;
    if (!ctx_fields_loaded) load_fields_from_config();
    ImGui::BeginDisabled(locked);
    bool changed = false;
    changed |= ImGui::InputText("Recording type", ctx_type, sizeof ctx_type);
    changed |= ImGui::InputText("Recording subtype", ctx_subtype, sizeof ctx_subtype);
    changed |= ImGui::Combo("Behavior mode", &ctx_mode, kModes, 3);
    changed |= ImGui::Combo("Recording intent", &ctx_intent, kIntents, 2);
    changed |= ImGui::Combo("Data origin", &ctx_origin, kOrigins, 2);
    if (changed) {
        std::string err;
        if (apply_fields_to_config(&err)) {
            contexts_dirty = true;
            contexts_status = "Applies at the next record start; not saved yet.";
        } else {
            contexts_status = "Not applied: " + err;
        }
    }
    if (ImGui::Button("Save recording context")) {
        std::string err;
        if (apply_fields_to_config(&err)) {
            try {
                recording::SaveGuiRecordingContexts(config_path, recording_contexts);
                contexts_dirty = false;
                contexts_status = "Saved recording.contexts in the app config.";
            } catch (const std::exception& ex) { contexts_status = std::string("Save failed: ") + ex.what(); }
        } else contexts_status = "Not saved: " + err;
    }
    ImGui::SameLine();
    if (ImGui::Button("Reload from app config")) {
        recording_contexts = {}; contexts_error.clear();
        try { recording_contexts = recording::ReadGuiRecordingContexts(config_path); }
        catch (const std::exception& ex) { contexts_error = ex.what(); }
        load_fields_from_config();
        contexts_status = contexts_error.empty() ? "Reloaded recording.contexts." : "Reload failed: " + contexts_error;
    }
    ImGui::EndDisabled();
    if (!recording_contexts.cameras.empty()) {
        ImGui::TextWrapped("Per-camera overrides in the app config (edit the file to change):");
        for (const auto& [serial, ctx] : recording_contexts.cameras)
            ImGui::BulletText("%s: %s / %s, %s, %s, %s", serial.c_str(), ctx.recording_type.c_str(), ctx.recording_subtype.c_str(),
                              ctx.behavior_mode.c_str(), ctx.recording_intent.c_str(), ctx.data_origin.c_str());
    }
    if (!recording_contexts.configured())
        ImGui::TextWrapped("No recording context configured: recording_session.json will carry none and Citrus transfer-v2 will refuse the recording.");
    else if (ctx_intent == 1)
        ImGui::TextWrapped("recording_only: no Citrus observation binding for this recording (binding mode not_applicable).");
    else
        ImGui::TextWrapped("stimulus_experiment: Citrus binding mode follows ORANGE_CITRUS_OBSERVATION_BINDING_MODE; the context is frozen in the start snapshot and, once the v2 handshake lands, sent to Citrus before capture.");
    ImGui::TextWrapped("Frozen into the recording start snapshot for exactly the recording cameras; every manifest write carries it unchanged.");
    if (contexts_dirty) ImGui::TextWrapped("Unsaved: the next launch reloads the app config.");
    if (!contexts_error.empty()) ImGui::TextWrapped("Record start blocked: %s", contexts_error.c_str());
    if (!contexts_status.empty()) ImGui::TextWrapped("%s", contexts_status.c_str());
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
