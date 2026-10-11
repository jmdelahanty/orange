#include "gui/registered_context_recording.h"
#include <algorithm>
#include <cstdio>
#include <map>
#include <string>
#include <vector>
#include "imgui.h"
#include "timing/clock.h"

namespace orange::gui {
namespace {
using json = nlohmann::json;
json options = {{"schema_version", 1}, {"enabled", false}};
std::string config_path, error, status;
recording::RecordingMediaSelection media_selection;
std::string media_error, media_status;
recording::RecordingContextsConfig recording_contexts;
recording::SubjectReferencesConfig subject_references;
recording::ZebrobotLookupConfig zebrobot_lookup;
std::string subject_error;
std::string contexts_error;
bool confirmed = false;
// Per-start confirmation (Jeremy 2026-10-11): the dish table is app-scoped and
// a saved value silently carried into later sessions (the 2026-10-09 snow
// sessions declared one dish for four arenas). Each GUI record start needs the
// operator to confirm the table as it stands; any edit or reload clears it,
// and a start consumes it.
bool subject_confirmed = false;
json subject_confirmation;  // orange.recording_subject_confirmation v1
std::vector<std::string> subject_cross_check_mismatches;
bool subject_cross_check_ran = false;
void clear_subject_confirmation() {
    subject_confirmed = false;
    subject_confirmation = nullptr;
}
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
        subject_references = {}; zebrobot_lookup = {}; subject_error.clear();
        clear_subject_confirmation();
        try {
            subject_references = recording::ReadGuiSubjectReferences(path);
            zebrobot_lookup = recording::ReadGuiZebrobotLookup(path);
        } catch (const std::exception& ex) { subject_error = ex.what(); }
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
recording::SubjectReferencesConfig SubjectReferencesForStream() {
    if (!subject_error.empty()) throw std::runtime_error("GUI subject references: " + subject_error);
    return subject_references;
}
void SetSubjectCrossCheckResult(const std::vector<std::string>& mismatches, bool citrus_declared) {
    subject_cross_check_mismatches = mismatches;
    subject_cross_check_ran = citrus_declared;
}
recording::SubjectReferencesConfig SubjectReferencesForRecordStart(json* confirmation_out) {
    if (!subject_error.empty()) throw std::runtime_error("GUI subject references: " + subject_error);
    if (!subject_confirmed || subject_confirmation.value("config", json()) != subject_references.ToJson())
        throw std::runtime_error("Confirm each camera's dish in the Subject panel before starting this recording "
                                 "(the dish table is confirmed per recording)");
    if (confirmation_out) *confirmation_out = subject_confirmation;
    clear_subject_confirmation();
    return subject_references;
}
recording::ZebrobotLookupConfig ZebrobotLookupForStream() {
    if (!subject_error.empty()) throw std::runtime_error("GUI subject references: " + subject_error);
    return zebrobot_lookup;
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
        std::snprintf(ctx_subtype, sizeof ctx_subtype, "%s", d.recording_subtype ? d.recording_subtype->c_str() : "");
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
        d.recording_type = ctx_type;
        // Blank subtype = not specified (context version 2, key omitted); never a placeholder.
        if (ctx_subtype[0] != '\0') d.recording_subtype = std::string(ctx_subtype);
        else d.recording_subtype.reset();
        d.behavior_mode = kModes[ctx_mode]; d.recording_intent = kIntents[ctx_intent]; d.data_origin = kOrigins[ctx_origin];
        recording::RecordingContext::Parse(d.ToJson());  // same rules as the config parser
        recording_contexts.default_context = d;
        contexts_error.clear();
        return true;
    } catch (const std::exception& ex) { if (error) *error = ex.what(); return false; }
}
}  // namespace
namespace {
std::string subject_status;
bool subject_dirty = false;
// One editable row: the default (all cameras) or one camera's override.
struct SubjectRow {
    char dish_id[128] = "";
    int subject_count = 0;  // 0 = not declared (camera rows: use the default)
};
SubjectRow subject_default_row;
std::map<std::string, SubjectRow> subject_camera_rows;  // by camera serial
bool subject_fields_loaded = false;
void load_row(SubjectRow* row, const recording::SubjectDeclaration* d) {
    *row = SubjectRow{};
    if (!d) return;
    if (d->dish_id) std::snprintf(row->dish_id, sizeof row->dish_id, "%s", d->dish_id->c_str());
    if (d->subject_count) row->subject_count = *d->subject_count;
}
recording::SubjectDeclaration row_declaration(const SubjectRow& row) {
    recording::SubjectDeclaration d;
    if (row.dish_id[0] != '\0') d.dish_id = std::string(row.dish_id);
    if (row.subject_count > 0) d.subject_count = row.subject_count;
    return d;
}
void load_subject_fields_from_config() {
    load_row(&subject_default_row,
             subject_references.default_declaration ? &*subject_references.default_declaration : nullptr);
    subject_camera_rows.clear();
    for (const auto& [serial, d] : subject_references.cameras) load_row(&subject_camera_rows[serial], &d);
    subject_fields_loaded = true;
    subject_dirty = false;
}
bool apply_subject_fields_to_config(std::string* error) {
    try {
        recording::SubjectReferencesConfig next = subject_references;
        const recording::SubjectDeclaration d = row_declaration(subject_default_row);
        if (d.empty()) next.default_declaration.reset();
        else next.default_declaration = d;
        // Camera rows replace the camera entries; an empty row removes the
        // override (that camera follows the default).
        for (const auto& [serial, row] : subject_camera_rows) {
            const recording::SubjectDeclaration c = row_declaration(row);
            if (c.empty()) next.cameras.erase(serial);
            else next.cameras[serial] = c;
        }
        recording::SubjectReferencesConfig::Parse(next.ToJson());  // same rules as the config parser
        subject_references = next;
        subject_error.clear();
        return true;
    } catch (const std::exception& ex) { if (error) *error = ex.what(); return false; }
}
}  // namespace
void RenderSubjectReferenceSelection(bool locked, const std::vector<std::string>& camera_serials) {
    if (!ImGui::CollapsingHeader("Subject (MetaZebrobot dish per camera)")) return;
    if (!subject_fields_loaded) load_subject_fields_from_config();
    ImGui::BeginDisabled(locked);
    ImGui::TextUnformatted("All cameras (default):");
    bool changed = ImGui::InputText("Dish id (blank = no dish declared)", subject_default_row.dish_id,
                                    sizeof subject_default_row.dish_id);
    changed |= ImGui::InputInt("Subject count per camera (0 = not declared)", &subject_default_row.subject_count);
    if (subject_default_row.subject_count < 0) subject_default_row.subject_count = 0;
    // Each camera images its own dish: a camera row overrides the default
    // field by field (blank dish / count 0 = use the default).
    if (!camera_serials.empty() && ImGui::BeginTable("subject_cameras", 4, ImGuiTableFlags_BordersInnerH)) {
        ImGui::TableSetupColumn("Camera");
        ImGui::TableSetupColumn("Dish id (blank = default)");
        ImGui::TableSetupColumn("Subject count (0 = default)");
        ImGui::TableSetupColumn("Recorded as");
        ImGui::TableHeadersRow();
        for (const std::string& serial : camera_serials) {
            SubjectRow& row = subject_camera_rows[serial];
            ImGui::PushID(serial.c_str());
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::TextUnformatted(serial.c_str());
            ImGui::TableSetColumnIndex(1);
            ImGui::SetNextItemWidth(-1);
            changed |= ImGui::InputText("##dish", row.dish_id, sizeof row.dish_id);
            ImGui::TableSetColumnIndex(2);
            ImGui::SetNextItemWidth(-1);
            changed |= ImGui::InputInt("##count", &row.subject_count);
            if (row.subject_count < 0) row.subject_count = 0;
            ImGui::TableSetColumnIndex(3);
            const char* dish = row.dish_id[0] ? row.dish_id
                : subject_default_row.dish_id[0] ? subject_default_row.dish_id : "(none)";
            const int count = row.subject_count > 0 ? row.subject_count : subject_default_row.subject_count;
            ImGui::Text("%s, count %s", dish, count > 0 ? std::to_string(count).c_str() : "(none)");
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    if (changed) {
        clear_subject_confirmation();
        std::string err;
        if (apply_subject_fields_to_config(&err)) {
            subject_dirty = true;
            subject_status = "Applies at the next record start; not saved yet.";
        } else subject_status = "Not applied: " + err;
    }
    {
        std::string err;
        if (ImGui::Button("Confirm dishes for the next recording") && apply_subject_fields_to_config(&err)) {
            json cameras = json::object();
            for (const auto& [serial, d] : subject_references.Resolve(camera_serials)) {
                cameras[serial] = {{"dish_id", d.dish_id ? json(*d.dish_id) : json(nullptr)},
                                   {"subject_count", d.subject_count ? json(*d.subject_count) : json(nullptr)}};
            }
            subject_confirmation = {
                {"schema_id", "orange.recording_subject_confirmation"},
                {"schema_version", 1},
                {"confirmed_by", "gui_operator"},
                {"confirmed_at_utc", orange::timing::utc_iso8601_now()},
                {"cameras", cameras},
                {"config", subject_references.ToJson()}};
            subject_confirmed = true;
        } else if (!err.empty()) {
            subject_status = "Not confirmed: " + err;
        }
    }
    if (!subject_cross_check_mismatches.empty()) {
        ImGui::TextColored(ImVec4(1.0f, 0.55f, 0.1f, 1.0f),
                           "Last start REFUSED: Citrus declared a different subject than you confirmed:");
        for (const auto& line : subject_cross_check_mismatches) ImGui::BulletText("%s", line.c_str());
        ImGui::TextWrapped("Recorded in subject_cross_check.json of the refused recording. Until Citrus's confirmed "
                           "dish can reach the manifest, a mismatch refuses the start (Palette would refuse the "
                           "import). Fix the table here or in Citrus, confirm, and start again.");
    } else if (subject_cross_check_ran) {
        ImGui::TextWrapped("Current recording: Citrus's dish declarations match your confirmed entries.");
    }
    if (subject_confirmed)
        ImGui::TextWrapped("Confirmed at %s for the next recording.",
                           subject_confirmation.value("confirmed_at_utc", "").c_str());
    else
        ImGui::TextWrapped("Not confirmed: the next GUI or Citrus-triggered record start is refused until you confirm "
                           "the dish of every camera above.");
    if (ImGui::Button("Save subject declaration")) {
        std::string err;
        if (apply_subject_fields_to_config(&err)) {
            try {
                recording::SaveGuiSubjectReferences(config_path, subject_references);
                subject_dirty = false;
                subject_status = "Saved recording.subject_references in the app config.";
            } catch (const std::exception& ex) { subject_status = std::string("Save failed: ") + ex.what(); }
        } else subject_status = "Not saved: " + err;
    }
    ImGui::SameLine();
    if (ImGui::Button("Reload dish id from app config")) {
        subject_references = {}; zebrobot_lookup = {}; subject_error.clear();
        clear_subject_confirmation();
        try {
            subject_references = recording::ReadGuiSubjectReferences(config_path);
            zebrobot_lookup = recording::ReadGuiZebrobotLookup(config_path);
        } catch (const std::exception& ex) { subject_error = ex.what(); }
        load_subject_fields_from_config();
        subject_status = subject_error.empty() ? "Reloaded recording.subject_references." : "Reload failed: " + subject_error;
    }
    ImGui::EndDisabled();
    for (const auto& [serial, d] : subject_references.cameras) {
        if (std::find(camera_serials.begin(), camera_serials.end(), serial) != camera_serials.end()) continue;
        ImGui::BulletText("Camera %s (not in this rig's camera list): dish %s, subject_count %s", serial.c_str(),
                          d.dish_id ? d.dish_id->c_str() : "(default)",
                          d.subject_count ? std::to_string(*d.subject_count).c_str() : "(default)");
    }
    if (zebrobot_lookup.configured())
        ImGui::TextWrapped("Lookup at record start: %s (timeout %d ms); Orange records dish/fish identifiers as served, no biological fields.",
                           zebrobot_lookup.base_url.c_str(), zebrobot_lookup.timeout_ms);
    else
        ImGui::TextWrapped("recording.zebrobot.base_url is not configured: a declared dish will be recorded as lookup_failed (zebrobot_not_configured); recording still starts.");
    if (!subject_references.configured())
        ImGui::TextWrapped("No dish declared: every camera is recorded as not_collected (no_dish_declared). Palette admits declared absence.");
    ImGui::TextWrapped("Subject count is the number of animals this camera records, declared by you; it is never derived from the dish registry. Null (0) means Palette publishes subject metadata without an experiment setup.");
    if (ctx_intent == 1)
        ImGui::TextWrapped("recording_only: Orange is the only source of the subject record, so declare the dish and count here.");
    else
        ImGui::TextWrapped("stimulus_experiment: Citrus supplies the subject record in its H5; declaring the dish here is optional and lets Palette cross-check the two.");
    if (subject_dirty) ImGui::TextWrapped("Unsaved: the next launch reloads the app config.");
    if (!subject_error.empty()) ImGui::TextWrapped("Record start blocked: %s", subject_error.c_str());
    if (!subject_status.empty()) ImGui::TextWrapped("%s", subject_status.c_str());
}
void RenderRecordingContextSelection(bool locked) {
    if (!ImGui::CollapsingHeader("Recording context (what this recording is)")) return;
    if (!ctx_fields_loaded) load_fields_from_config();
    ImGui::BeginDisabled(locked);
    bool changed = false;
    changed |= ImGui::InputText("Recording type", ctx_type, sizeof ctx_type);
    changed |= ImGui::InputText("Recording subtype (blank = not specified)", ctx_subtype, sizeof ctx_subtype);
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
            ImGui::BulletText("%s: %s / %s, %s, %s, %s", serial.c_str(), ctx.recording_type.c_str(),
                              ctx.recording_subtype ? ctx.recording_subtype->c_str() : "(no subtype)",
                              ctx.behavior_mode.c_str(), ctx.recording_intent.c_str(), ctx.data_origin.c_str());
    }
    if (!recording_contexts.configured())
        ImGui::TextWrapped("No recording context configured: recording_session.json will carry none and Citrus transfer-v2 will refuse the recording.");
    else if (ctx_intent == 1)
        ImGui::TextWrapped("recording_only: no Citrus observation binding for this recording (binding mode not_applicable).");
    else
        ImGui::TextWrapped("stimulus_experiment: Citrus binding mode follows ORANGE_CITRUS_OBSERVATION_BINDING_MODE; the context is frozen in the start snapshot and, once the v2 handshake lands, sent to Citrus before capture.");
    if (ctx_subtype[0] == '\0' && recording_contexts.configured())
        ImGui::TextWrapped("No subtype: emitted as citrus.parent_recording_context version 2 (subtype omitted, no default substituted). Citrus and Palette must accept version 2 before a subtype-free recording is transferred.");
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
