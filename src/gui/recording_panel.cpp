#include "gui/recording_panel.h"

#include "encoder_hw_worker.h"
#include "imgui.h"
#include "project.h"
#include "recording_output_utils.h"
#include "recording_validation.h"

#include <iomanip>
#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <sstream>
#include <limits>

namespace {

bool combo_select_string(const char* label,
                         std::string* value,
                         const char* const* option_values,
                         const char* const* option_labels,
                         const int option_count)
{
    int current_index = -1;
    for (int i = 0; i < option_count; ++i) {
        if (*value == option_values[i]) {
            current_index = i;
            break;
        }
    }

    const char* preview_value =
        current_index >= 0
            ? option_labels[current_index]
            : (value->empty() ? "(none)" : value->c_str());

    bool changed = false;
    if (ImGui::BeginCombo(label, preview_value)) {
        for (int i = 0; i < option_count; ++i) {
            const bool is_selected = (i == current_index);
            if (ImGui::Selectable(option_labels[i], is_selected)) {
                *value = option_values[i];
                changed = true;
            }
            if (is_selected) {
                ImGui::SetItemDefaultFocus();
            }
        }
        ImGui::EndCombo();
    }
    return changed;
}

bool combo_select_encoder_toggle(const char* label, int* value)
{
    static const int kValues[] = {-1, 0, 1};
    static const char* const kLabels[] = {"auto", "off", "on"};

    int current_index = 0;
    for (int i = 0; i < IM_ARRAYSIZE(kValues); ++i) {
        if (*value == kValues[i]) {
            current_index = i;
            break;
        }
    }

    bool changed = false;
    if (ImGui::BeginCombo(label, kLabels[current_index])) {
        for (int i = 0; i < IM_ARRAYSIZE(kValues); ++i) {
            const bool is_selected = (i == current_index);
            if (ImGui::Selectable(kLabels[i], is_selected)) {
                *value = kValues[i];
                changed = true;
            }
            if (is_selected) {
                ImGui::SetItemDefaultFocus();
            }
        }
        ImGui::EndCombo();
    }
    return changed;
}

std::string join_gpu_ids(const std::vector<int>& gpu_ids)
{
    if (gpu_ids.empty()) {
        return "(none)";
    }

    std::ostringstream oss;
    for (std::size_t i = 0; i < gpu_ids.size(); ++i) {
        if (i > 0) {
            oss << ", ";
        }
        oss << gpu_ids[i];
    }
    return oss.str();
}

std::string current_strategy_mode(const RecordingStrategyConfig& strategy)
{
    if (!strategy.requested_mode.empty()) {
        return strategy.requested_mode;
    }
    return strategy.mode.empty() ? "single_session" : strategy.mode;
}

void apply_strategy_mode(RecordingStrategyConfig* strategy, const std::string& mode)
{
    if (!strategy) {
        return;
    }
    const std::string resolved_mode = (mode == "split_gop") ? "split_gop" : "single_session";
    strategy->requested_mode = resolved_mode;
    strategy->mode = resolved_mode;
    strategy->split_gop.enabled = (resolved_mode == "split_gop");
}

int current_helper_gpu_id(const CameraParams& camera_params)
{
    const std::vector<int> helper_gpu_ids = build_recording_helper_gpu_ids(
        camera_params.gpu_id,
        camera_params.recording.strategy.split_gop.encoder_gpu_ids);
    return helper_gpu_ids.empty() ? -1 : helper_gpu_ids.front();
}

void set_single_helper_gpu_id(CameraParams* camera_params, const int helper_gpu_id)
{
    if (!camera_params) {
        return;
    }

    std::vector<int> encoder_gpu_ids;
    encoder_gpu_ids.push_back(camera_params->gpu_id);
    if (helper_gpu_id >= 0 && helper_gpu_id != camera_params->gpu_id) {
        encoder_gpu_ids.push_back(helper_gpu_id);
    }

    camera_params->recording.strategy.split_gop.encoder_gpu_ids = std::move(encoder_gpu_ids);
    camera_params->recording.strategy.split_gop.placement =
        camera_params->recording.strategy.split_gop.encoder_gpu_ids.size() > 1
            ? "multi_gpu"
            : "single_gpu";
}

std::string normalized_ascii(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return value;
}

const char* nonempty_env(const char* name)
{
    const char* raw = std::getenv(name);
    return raw && *raw ? raw : nullptr;
}

bool parse_nonnegative_int_text(const char* raw, int* value_out)
{
    if (!raw || !*raw) {
        return false;
    }
    char* end = nullptr;
    const long value = std::strtol(raw, &end, 10);
    while (end && *end && std::isspace(static_cast<unsigned char>(*end)) != 0) {
        ++end;
    }
    if (end == raw || (end && *end) || value < 0 ||
        value > std::numeric_limits<int>::max()) {
        return false;
    }
    if (value_out) {
        *value_out = static_cast<int>(value);
    }
    return true;
}

std::string effective_gui_recording_sink_mode(const AppStorageConfig* app_storage_config)
{
    if (const char* env = nonempty_env("ORANGE_GUI_RECORDING_SINK_MODE")) {
        return normalized_ascii(env);
    }
    if (app_storage_config &&
        app_storage_config->gui_recording_sink_mode_configured &&
        !app_storage_config->gui_recording_sink_mode.empty()) {
        return normalized_ascii(app_storage_config->gui_recording_sink_mode);
    }
    return "real";
}

int effective_recording_control_value(const AppStorageConfig* app_storage_config,
                                      const char* env_name,
                                      const int AppStorageConfig::*field)
{
    int env_value = 0;
    if (parse_nonnegative_int_text(nonempty_env(env_name), &env_value)) {
        return env_value;
    }
    return app_storage_config ? std::max(0, app_storage_config->*field) : 0;
}

void render_gui_external_rolling_controls(AppStorageConfig* app_storage_config,
                                          const bool streaming_active)
{
    ImGui::SeparatorText("External IPC Rolling");

    if (!app_storage_config) {
        ImGui::TextDisabled("App recording config is unavailable.");
        return;
    }

    const std::string sink_mode = effective_gui_recording_sink_mode(app_storage_config);
    const bool external_ipc = sink_mode == "external_ipc";
    const bool record_for_env = nonempty_env("ORANGE_GUI_RECORD_FOR_SECONDS") != nullptr;
    const bool clip_env = nonempty_env("ORANGE_GUI_CLIP_SECONDS") != nullptr;
    int record_for_seconds = effective_recording_control_value(
        app_storage_config,
        "ORANGE_GUI_RECORD_FOR_SECONDS",
        &AppStorageConfig::gui_recording_record_for_seconds);
    int clip_seconds = effective_recording_control_value(
        app_storage_config,
        "ORANGE_GUI_CLIP_SECONDS",
        &AppStorageConfig::gui_recording_clip_seconds);
    bool rolling_enabled = clip_seconds > 0;

    ImGui::BeginDisabled(streaming_active || !external_ipc || clip_env);
    if (ImGui::Checkbox("Enable full-frame rolling clips", &rolling_enabled)) {
        if (rolling_enabled) {
            if (app_storage_config->gui_recording_clip_seconds <= 0) {
                app_storage_config->gui_recording_clip_seconds = 1800;
            }
            if (!record_for_env && app_storage_config->gui_recording_record_for_seconds <= 0) {
                app_storage_config->gui_recording_record_for_seconds =
                    std::max(3600, app_storage_config->gui_recording_clip_seconds);
            }
        } else {
            if (!record_for_env) {
                app_storage_config->gui_recording_record_for_seconds = 0;
            }
            app_storage_config->gui_recording_clip_seconds = 0;
        }
        record_for_seconds = effective_recording_control_value(
            app_storage_config,
            "ORANGE_GUI_RECORD_FOR_SECONDS",
            &AppStorageConfig::gui_recording_record_for_seconds);
        clip_seconds = effective_recording_control_value(
            app_storage_config,
            "ORANGE_GUI_CLIP_SECONDS",
            &AppStorageConfig::gui_recording_clip_seconds);
    }
    ImGui::EndDisabled();

    ImGui::BeginDisabled(streaming_active || !external_ipc || !rolling_enabled || record_for_env);
    if (ImGui::InputInt("record duration seconds", &record_for_seconds)) {
        app_storage_config->gui_recording_record_for_seconds = std::max(1, record_for_seconds);
        record_for_seconds = app_storage_config->gui_recording_record_for_seconds;
    }
    ImGui::EndDisabled();
    ImGui::BeginDisabled(streaming_active || !external_ipc || !rolling_enabled || clip_env);
    if (ImGui::InputInt("clip seconds", &clip_seconds)) {
        app_storage_config->gui_recording_clip_seconds = std::max(1, clip_seconds);
        clip_seconds = app_storage_config->gui_recording_clip_seconds;
    }
    ImGui::EndDisabled();

    if (!external_ipc) {
        ImGui::TextDisabled("Full-frame rolling uses GUI recording sink mode external_ipc.");
    } else if (record_for_env || clip_env) {
        ImGui::TextDisabled("ORANGE_GUI_RECORD_FOR_SECONDS / ORANGE_GUI_CLIP_SECONDS override these fields.");
    } else if (streaming_active) {
        ImGui::TextDisabled("Rolling settings are locked while streaming is active.");
    }

    if (rolling_enabled) {
        ImGui::TextDisabled(
            "Effective full-frame clips: %d seconds per clip for %d recording seconds.",
            clip_seconds,
            record_for_seconds);
    } else {
        ImGui::TextDisabled("Effective full-frame clips: single clip.");
    }
}

void render_advanced_recording_controls(CameraParams* cameras_params,
                                        CameraEachSelect* cameras_select,
                                        const int num_cameras,
                                        const bool camera_open,
                                        const bool streaming_active)
{
    ImGui::SeparatorText("Advanced Recording");

    if (!camera_open || !cameras_params || !cameras_select || num_cameras <= 0) {
        ImGui::TextDisabled("Open cameras to inspect and edit per-camera advanced recording settings.");
        return;
    }

    ImGui::TextDisabled(
        "These controls edit the in-memory per-camera recording config used for the next stream/record session.");
    if (streaming_active) {
        ImGui::TextDisabled("Advanced recording settings are locked while streaming is active.");
    }

    if (!ImGui::TreeNode("Per-camera advanced controls")) {
        return;
    }

    ImGui::BeginDisabled(streaming_active);

    static const char* const kRecordingModeValues[] = {"single_session", "split_gop"};
    static const char* const kRecordingModeLabels[] = {"single_session", "split_gop"};
    static const char* const kTransferModeValues[] = {"auto", "raw"};
    static const char* const kTransferModeLabels[] = {"auto", "raw"};
    static const char* const kSourcePolicyValues[] = {"hybrid_split", "local_only"};
    static const char* const kSourcePolicyLabels[] = {"hybrid_split", "local_only"};
    static const char* const kTopologyValues[] = {"", "PIX", "PXB", "PHB", "SYS"};
    static const char* const kTopologyLabels[] = {"(none)", "PIX", "PXB", "PHB", "SYS"};

    for (int i = 0; i < num_cameras; ++i) {
        CameraParams& camera_params = cameras_params[i];
        CameraRecordingConfig& recording = camera_params.recording;
        RecordingStrategyConfig& strategy = recording.strategy;

        std::ostringstream header;
        header << camera_params.camera_serial << " (source GPU " << camera_params.gpu_id << ")";
        if (cameras_select[i].record) {
            header << " [record]";
        }

        if (!ImGui::TreeNode(header.str().c_str())) {
            continue;
        }

        ImGui::PushID(i);

        ImGui::TextDisabled("Source GPU: %d", camera_params.gpu_id);
        ImGui::TextDisabled(
            "Config schema: %s v%d",
            camera_params.config_schema_id.empty() ? "(legacy)" : camera_params.config_schema_id.c_str(),
            camera_params.config_schema_version);

        std::string ui_mode = current_strategy_mode(strategy);
        if (combo_select_string(
                "recording mode",
                &ui_mode,
                kRecordingModeValues,
                kRecordingModeLabels,
                IM_ARRAYSIZE(kRecordingModeValues))) {
            apply_strategy_mode(&strategy, ui_mode);
        }

        const bool split_gop_enabled = current_strategy_mode(strategy) == "split_gop";
        ImGui::BeginDisabled(!split_gop_enabled);

        int helper_gpu_id = current_helper_gpu_id(camera_params);
        if (ImGui::InputInt("helper GPU id (-1 disables helper)", &helper_gpu_id)) {
            set_single_helper_gpu_id(&camera_params, helper_gpu_id);
        }

        combo_select_string(
            "transfer mode",
            &strategy.split_gop.transfer_mode,
            kTransferModeValues,
            kTransferModeLabels,
            IM_ARRAYSIZE(kTransferModeValues));

        combo_select_string(
            "source encoder policy",
            &strategy.split_gop.source_encoder_policy,
            kSourcePolicyValues,
            kSourcePolicyLabels,
            IM_ARRAYSIZE(kSourcePolicyValues));

        ImGui::Checkbox("require peer access", &recording.constraints.require_peer_access);
        combo_select_string(
            "preferred topology",
            &recording.constraints.preferred_topology_class,
            kTopologyValues,
            kTopologyLabels,
            IM_ARRAYSIZE(kTopologyValues));

        ImGui::InputInt(
            "acquire work entries (0=default)",
            &recording.resources.acquire_work_entries);
        if (recording.resources.acquire_work_entries < 0) {
            recording.resources.acquire_work_entries = 0;
        }

        ImGui::InputInt(
            "encoder entry pool size (0=default)",
            &recording.resources.encoder_entry_pool_size);
        if (recording.resources.encoder_entry_pool_size < 0) {
            recording.resources.encoder_entry_pool_size = 0;
        }

        ImGui::TextDisabled(
            "Placement: %s | encoder GPU ids: %s",
            strategy.split_gop.placement.c_str(),
            join_gpu_ids(strategy.split_gop.encoder_gpu_ids).c_str());

        if (strategy.split_gop.source_encoder_policy != "hybrid_split") {
            ImGui::TextDisabled(
                "Only hybrid_split is currently validated for the multi-GPU recording path.");
        }

        ImGui::EndDisabled();
        if (!split_gop_enabled) {
            ImGui::TextDisabled("Split-GOP-specific controls are disabled while recording mode is single_session.");
        }

        ImGui::PopID();
        ImGui::TreePop();
    }

    ImGui::EndDisabled();
    ImGui::TreePop();
}

void render_advanced_recording_validation_summary(CameraParams* cameras_params,
                                                  CameraEachSelect* cameras_select,
                                                  const int num_cameras)
{
    if (!cameras_params || !cameras_select || num_cameras <= 0) {
        ImGui::SeparatorText("Advanced Recording Validation");
        ImGui::TextDisabled("Open cameras to inspect advanced recording validation.");
        return;
    }

    int record_enabled_count = 0;
    int record_enabled_split_gop_count = 0;
    for (int i = 0; i < num_cameras; ++i) {
        if (cameras_select[i].record) {
            ++record_enabled_count;
            if (cameras_params[i].recording.strategy.split_gop_enabled()) {
                ++record_enabled_split_gop_count;
            }
        }
    }

    if (record_enabled_count == 0) {
        ImGui::SeparatorText("Advanced Recording Validation");
        ImGui::TextDisabled("No cameras are currently selected for recording.");
        return;
    }

    if (record_enabled_split_gop_count == 0) {
        ImGui::SeparatorText("Advanced Recording Validation");
        ImGui::TextDisabled("No record-enabled cameras are currently using split-GOP recording.");
        return;
    }

    if (!ImGui::TreeNode("Advanced Recording Validation")) {
        ImGui::TextDisabled(
            "Split-GOP validation is available for %d record-enabled camera(s). "
            "Expand to run topology and peer-access checks.",
            record_enabled_split_gop_count);
        return;
    }

    std::vector<RecordingValidationCameraInput> validation_inputs;
    validation_inputs.reserve(static_cast<std::size_t>(num_cameras));
    for (int i = 0; i < num_cameras; ++i) {
        RecordingValidationCameraInput input;
        input.camera_index = i;
        input.camera_serial = cameras_params[i].camera_serial;
        input.record_enabled = cameras_select[i].record;
        input.source_gpu_id = cameras_params[i].gpu_id;
        input.strategy = cameras_params[i].recording.strategy;
        input.constraints = cameras_params[i].recording.constraints;
        validation_inputs.push_back(std::move(input));
    }

    const std::vector<CameraRecordingValidationSummary> summaries =
        validate_recording_configuration(
            validation_inputs,
            [](const int source_gpu_id, const int helper_gpu_id) {
                return build_recording_validation_gpu_path_info(source_gpu_id, helper_gpu_id);
            });

    int invalid_camera_count = 0;
    for (const auto& summary : summaries) {
        if (!summary.record_enabled || !summary.split_gop_enabled) {
            continue;
        }
        if (!summary.valid()) {
            ++invalid_camera_count;
        }
    }

    ImGui::TextDisabled(
        "Validation summary for record-enabled split-GOP cameras using the current in-memory per-camera settings.");
    if (invalid_camera_count == 0) {
        ImGui::TextColored(
            ImVec4(0.2f, 0.8f, 0.2f, 1.0f),
            "Split-GOP validation OK for %d camera(s).",
            record_enabled_split_gop_count);
    } else {
        ImGui::TextColored(
            ImVec4(0.9f, 0.3f, 0.3f, 1.0f),
            "%d of %d record-enabled split-GOP camera(s) have validation errors.",
            invalid_camera_count,
            record_enabled_split_gop_count);
    }

    if (!ImGui::TreeNode("Per-camera split-GOP summary")) {
        ImGui::TreePop();
        return;
    }

    for (const auto& summary : summaries) {
        if (!summary.record_enabled) {
            continue;
        }

        const std::string header =
            summary.camera_serial + (summary.split_gop_enabled ? "" : " (single_session)");
        if (!ImGui::TreeNode(header.c_str())) {
            continue;
        }

        if (!summary.split_gop_enabled) {
            ImGui::TextDisabled("This camera participates in recording, but not in split-GOP mode.");
            ImGui::TreePop();
            continue;
        }

        ImGui::Text(
            "Source GPU %d -> helper GPU(s) %s",
            summary.source_gpu_id,
            join_gpu_ids(summary.helper_gpu_ids).c_str());
        ImGui::Text(
            "Claimed GPU set: %s",
            join_gpu_ids(summary.claimed_gpu_ids).c_str());
        ImGui::Text(
            "Transfer %s | policy %s",
            summary.transfer_mode.empty() ? "(none)" : summary.transfer_mode.c_str(),
            summary.source_encoder_policy.empty() ? "(none)" : summary.source_encoder_policy.c_str());
        ImGui::Text(
            "Preferred topology: %s | peer access required: %s",
            summary.preferred_topology_class.empty() ? "(none)" : summary.preferred_topology_class.c_str(),
            summary.require_peer_access ? "yes" : "no");

        if (summary.valid()) {
            ImGui::TextColored(ImVec4(0.2f, 0.8f, 0.2f, 1.0f), "Validation status: valid");
        } else {
            ImGui::TextColored(ImVec4(0.9f, 0.3f, 0.3f, 1.0f), "Validation status: invalid");
        }

        for (const auto& helper : summary.helpers) {
            std::ostringstream helper_line;
            helper_line << "Helper GPU " << helper.helper_gpu_id
                        << ": topology "
                        << (helper.topology_class.empty() ? "(unknown)" : helper.topology_class)
                        << ", peer access ";
            if (helper.can_access_peer_known) {
                helper_line << (helper.can_access_peer ? "yes" : "no");
            } else {
                helper_line << "(unknown)";
            }
            ImGui::TextWrapped("%s", helper_line.str().c_str());
            if (!helper.topology_error.empty()) {
                ImGui::TextDisabled("  topology lookup: %s", helper.topology_error.c_str());
            }
        }

        for (const std::string& error : summary.local_errors) {
            ImGui::TextColored(ImVec4(0.9f, 0.3f, 0.3f, 1.0f), "%s", error.c_str());
        }
        for (const std::string& error : summary.session_errors) {
            ImGui::TextColored(ImVec4(0.9f, 0.3f, 0.3f, 1.0f), "%s", error.c_str());
        }

        ImGui::TreePop();
    }

    ImGui::TreePop();
    ImGui::TreePop();
}

}  // namespace

namespace orange::gui {

RecordingPanelActions render_recording_config_panel(std::string* input_folder,
                                                    EncoderConfig* encoder_config,
                                                    const bool camera_open,
                                                    const bool streaming_active,
                                                    CameraParams* cameras_params,
                                                    CameraEachSelect* cameras_select,
                                                    const int num_cameras,
                                                    AppStorageConfig* app_storage_config,
                                                    const std::string* config_defaults_status,
                                                    const bool config_defaults_status_warning,
                                                    const std::vector<std::string>* preflight_errors)
{
    RecordingPanelActions actions;
    if (!input_folder || !encoder_config) {
        return actions;
    }

    static const char* const kCodecValues[] = {"h264", "hevc"};
    static const char* const kCodecLabels[] = {"h264", "hevc"};
    static const char* const kPresetValues[] = {"p1", "p3", "p5", "p7"};
    static const char* const kPresetLabels[] = {"p1", "p3", "p5", "p7"};
    static const char* const kTuningValues[] = {"hq", "ll", "ull", "lossless"};
    static const char* const kTuningLabels[] = {"hq", "ll", "ull", "lossless"};
    static const char* const kRateControlValues[] = {"vbr", "vbr_cq", "cqp"};
    static const char* const kRateControlLabels[] = {"vbr", "vbr_cq", "cqp"};

    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.5f, 0.0f, 0.7f, 1.0f));
    if (ImGui::Button("Save to")) {
        actions.choose_recording_dir_requested = true;
    }
    ImGui::PopStyleColor(1);
    ImGui::SameLine();
    ImGui::Text("%s", input_folder->c_str());

    if (config_defaults_status && !config_defaults_status->empty()) {
        ImGui::Spacing();
        if (config_defaults_status_warning) {
            ImGui::TextColored(
                ImVec4(0.95f, 0.75f, 0.2f, 1.0f),
                "%s",
                config_defaults_status->c_str());
        } else {
            ImGui::TextColored(
                ImVec4(0.35f, 0.85f, 0.45f, 1.0f),
                "%s",
                config_defaults_status->c_str());
        }
    }

    if (preflight_errors && !preflight_errors->empty()) {
        ImGui::Spacing();
        ImGui::TextColored(
            ImVec4(0.9f, 0.3f, 0.3f, 1.0f),
            "Last recording preflight failure:");
        for (const std::string& error : *preflight_errors) {
            ImGui::BulletText("%s", error.c_str());
        }
    }

    orange::recording::sanitize_record_output_config(
        &encoder_config->record_output_mode,
        &encoder_config->record_downsample_factor,
        &encoder_config->record_output_width,
        &encoder_config->record_output_height);

    combo_select_string("codec", &encoder_config->encoder_codec, kCodecValues, kCodecLabels, IM_ARRAYSIZE(kCodecValues));
    combo_select_string("preset", &encoder_config->encoder_preset, kPresetValues, kPresetLabels, IM_ARRAYSIZE(kPresetValues));
    combo_select_string("tuning", &encoder_config->tuning_info, kTuningValues, kTuningLabels, IM_ARRAYSIZE(kTuningValues));
    if (encoder_config->tuning_info.empty()) {
        encoder_config->tuning_info = "ll";
    }
    combo_select_string(
        "rate control",
        &encoder_config->rate_control_mode,
        kRateControlValues,
        kRateControlLabels,
        IM_ARRAYSIZE(kRateControlValues));
    if (encoder_config->rate_control_mode.empty()) {
        encoder_config->rate_control_mode = "vbr";
    }

    combo_select_encoder_toggle("spatial AQ", &encoder_config->aq);
    combo_select_encoder_toggle("temporal AQ", &encoder_config->temporal_aq);
    ImGui::TextDisabled("auto uses the NVENC profile default; off is the current low-latency candidate.");

    if (encoder_config->rate_control_mode != "vbr") {
        ImGui::SliderInt("quality value", &encoder_config->quality_value, 1, 51, "%d");
        ImGui::TextDisabled("Lower values preserve more detail.");
    }

    encoder_config->gop_length = sanitize_recording_gop_length(encoder_config->gop_length);
    ImGui::BeginDisabled(encoder_config->tuning_info == "lossless");
    if (ImGui::InputInt("gop length (frames)", &encoder_config->gop_length)) {
        encoder_config->gop_length = sanitize_recording_gop_length(encoder_config->gop_length);
    }
    ImGui::EndDisabled();

    if (encoder_config->tuning_info == "lossless") {
        ImGui::TextDisabled("Lossless tuning forces GOP length 1.");
    } else if (encoder_config->gop_length == 0) {
        if (camera_open && cameras_params != nullptr && num_cameras > 0) {
            std::ostringstream auto_gop_summary;
            auto_gop_summary << "Auto GOP by camera: ";
            for (int i = 0; i < num_cameras; ++i) {
                if (i > 0) {
                    auto_gop_summary << ", ";
                }
                auto_gop_summary << cameras_params[i].camera_serial
                                 << "="
                                 << resolve_recording_gop_length(
                                        cameras_params[i],
                                        encoder_config->tuning_info,
                                        encoder_config->gop_length);
            }
            ImGui::TextDisabled("%s", auto_gop_summary.str().c_str());
        } else {
            ImGui::TextDisabled("0 = auto (uses camera FPS, about 1 second between IDRs).");
        }
    } else {
        ImGui::TextDisabled("IDR period follows the selected GOP length.");
    }

    ImGui::SeparatorText("Recording Resize");
    ImGui::BeginDisabled(streaming_active);
    {
        const char* items[] = {"factor", "exact_size"};
        int record_output_mode_current = (encoder_config->record_output_mode == "exact_size") ? 1 : 0;
        if (ImGui::Combo("record output mode", &record_output_mode_current, items, IM_ARRAYSIZE(items))) {
            encoder_config->record_output_mode = items[record_output_mode_current];
        }
    }
    if (encoder_config->record_output_mode == "exact_size") {
        ImGui::InputInt("record output width", &encoder_config->record_output_width);
        ImGui::InputInt("record output height", &encoder_config->record_output_height);
        orange::recording::sanitize_record_output_config(
            &encoder_config->record_output_mode,
            &encoder_config->record_downsample_factor,
            &encoder_config->record_output_width,
            &encoder_config->record_output_height);
    } else {
        const char* items[] = {"1", "2", "4", "8", "16"};
        static const int item_numbers[] = {1, 2, 4, 8, 16};
        int record_factor_current = 0;
        for (int i = 0; i < IM_ARRAYSIZE(item_numbers); ++i) {
            if (encoder_config->record_downsample_factor == item_numbers[i]) {
                record_factor_current = i;
                break;
            }
        }
        if (ImGui::Combo("record downsample", &record_factor_current, items, IM_ARRAYSIZE(items))) {
            encoder_config->record_downsample_factor = item_numbers[record_factor_current];
        }
    }
    ImGui::EndDisabled();
    if (streaming_active) {
        ImGui::TextDisabled("Recording resize is locked while streaming is active. Stop streaming to change it.");
    }
    ImGui::TextDisabled(
        "Recording resize applies when recording workers are created. Effective default: %s",
        orange::recording::record_output_summary(
            encoder_config->record_output_mode,
            encoder_config->record_downsample_factor,
            encoder_config->record_output_width,
            encoder_config->record_output_height
        ).c_str());

    render_gui_external_rolling_controls(app_storage_config, streaming_active);

    if (camera_open && cameras_params != nullptr && cameras_select != nullptr && num_cameras > 0) {
        ImGui::SeparatorText("Estimated Recording Bitrate");
        for (int i = 0; i < num_cameras; ++i) {
            std::string output_warning;
            const RecordingOutputConfig recording_output_config =
                orange::recording::resolve_recording_output_config(
                    cameras_params[i],
                    *encoder_config,
                    cameras_select[i],
                    &output_warning);
            const RecordingBitrateEstimate bitrate_estimate =
                estimate_recording_bitrate(cameras_params[i], recording_output_config);

            std::ostringstream line;
            line << cameras_params[i].camera_serial
                 << ": "
                 << recording_output_config.resolved_width
                 << "x"
                 << recording_output_config.resolved_height
                 << " @ "
                 << cameras_params[i].frame_rate
                 << " fps";

            if (encoder_config->rate_control_mode == "cqp") {
                line << " -> CQP mode (no fixed bitrate target).";
            } else {
                line << " -> avg "
                     << orange::recording::format_bitrate_mbps(bitrate_estimate.average_bitrate)
                     << " Mbps, max "
                     << orange::recording::format_bitrate_mbps(bitrate_estimate.max_bitrate)
                     << " Mbps";
                if (encoder_config->rate_control_mode == "vbr_cq") {
                    line << ", CQ=" << encoder_config->quality_value;
                }
                line << " (" << std::fixed << std::setprecision(2) << bitrate_estimate.target_bpp << " bpp";
                if (bitrate_estimate.average_clamped_to_min) {
                    line << ", floor-clamped";
                } else if (bitrate_estimate.average_clamped_to_max) {
                    line << ", avg-capped";
                }
                if (bitrate_estimate.max_clamped_to_max) {
                    line << ", max-capped";
                }
                line << ")";
            }

            ImGui::TextWrapped("%s", line.str().c_str());
            if (!output_warning.empty()) {
                ImGui::TextDisabled("  output fallback: %s", output_warning.c_str());
            }
        }
    } else {
        ImGui::TextDisabled("Estimated bitrate becomes available once cameras are open.");
    }

    if (camera_open && cameras_params != nullptr && cameras_select != nullptr && num_cameras > 0 &&
        ImGui::TreeNode("Recording resize overrides")) {
        ImGui::BeginDisabled(streaming_active);
        if (ImGui::BeginTable(
                "RecordingResizeOverrides",
                5,
                ImGuiTableFlags_Resizable | ImGuiTableFlags_NoSavedSettings |
                    ImGuiTableFlags_Borders)) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::Text("Serial");
            ImGui::TableNextColumn();
            ImGui::Text("Override");
            ImGui::TableNextColumn();
            ImGui::Text("Mode");
            ImGui::TableNextColumn();
            ImGui::Text("Value");
            ImGui::TableNextColumn();
            ImGui::Text("Effective");

            for (int i = 0; i < num_cameras; ++i) {
                orange::recording::sanitize_record_output_config(
                    &cameras_select[i].record_output_mode,
                    &cameras_select[i].record_downsample_factor,
                    &cameras_select[i].record_output_width,
                    &cameras_select[i].record_output_height);

                ImGui::PushID(i);
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::Text("%s", cameras_params[i].camera_serial.c_str());

                ImGui::TableNextColumn();
                if (ImGui::Checkbox("##record_output_override", &cameras_select[i].record_output_override) &&
                    cameras_select[i].record_output_override) {
                    cameras_select[i].record_output_mode = encoder_config->record_output_mode;
                    cameras_select[i].record_downsample_factor = encoder_config->record_downsample_factor;
                    cameras_select[i].record_output_width = encoder_config->record_output_width;
                    cameras_select[i].record_output_height = encoder_config->record_output_height;
                }

                ImGui::TableNextColumn();
                ImGui::BeginDisabled(!cameras_select[i].record_output_override);
                {
                    const char* items[] = {"factor", "exact_size"};
                    int mode_current = (cameras_select[i].record_output_mode == "exact_size") ? 1 : 0;
                    if (ImGui::Combo("##record_output_mode", &mode_current, items, IM_ARRAYSIZE(items))) {
                        cameras_select[i].record_output_mode = items[mode_current];
                    }
                }

                ImGui::TableNextColumn();
                if (cameras_select[i].record_output_mode == "exact_size") {
                    ImGui::SetNextItemWidth(80.0f);
                    ImGui::InputInt("##record_output_width", &cameras_select[i].record_output_width);
                    ImGui::SameLine();
                    ImGui::Text("x");
                    ImGui::SameLine();
                    ImGui::SetNextItemWidth(80.0f);
                    ImGui::InputInt("##record_output_height", &cameras_select[i].record_output_height);
                } else {
                    const char* items[] = {"1", "2", "4", "8", "16"};
                    static const int item_numbers[] = {1, 2, 4, 8, 16};
                    int factor_current = 0;
                    for (int item_index = 0; item_index < IM_ARRAYSIZE(item_numbers); ++item_index) {
                        if (cameras_select[i].record_downsample_factor == item_numbers[item_index]) {
                            factor_current = item_index;
                            break;
                        }
                    }
                    if (ImGui::Combo("##record_downsample_factor", &factor_current, items, IM_ARRAYSIZE(items))) {
                        cameras_select[i].record_downsample_factor = item_numbers[factor_current];
                    }
                }
                ImGui::EndDisabled();

                ImGui::TableNextColumn();
                const std::string effective_mode = cameras_select[i].record_output_override
                    ? cameras_select[i].record_output_mode
                    : encoder_config->record_output_mode;
                const int effective_factor = cameras_select[i].record_output_override
                    ? cameras_select[i].record_downsample_factor
                    : encoder_config->record_downsample_factor;
                const int effective_width = cameras_select[i].record_output_override
                    ? cameras_select[i].record_output_width
                    : encoder_config->record_output_width;
                const int effective_height = cameras_select[i].record_output_override
                    ? cameras_select[i].record_output_height
                    : encoder_config->record_output_height;
                ImGui::TextUnformatted(
                    orange::recording::record_output_summary(
                        effective_mode,
                        effective_factor,
                        effective_width,
                        effective_height
                    ).c_str());
                ImGui::PopID();
            }

            ImGui::EndTable();
        }
        ImGui::EndDisabled();
        if (streaming_active) {
            ImGui::TextDisabled("Overrides are locked while streaming is active. Stop streaming to edit them.");
        }
        ImGui::TextDisabled(
            "Invalid recording resize settings fall back to native size when workers are created and log a warning.");
        ImGui::TreePop();
    }

    render_advanced_recording_controls(
        cameras_params,
        cameras_select,
        num_cameras,
        camera_open,
        streaming_active);
    render_advanced_recording_validation_summary(cameras_params, cameras_select, num_cameras);

    return actions;
}

}  // namespace orange::gui
