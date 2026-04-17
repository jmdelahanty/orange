#include "gui/recording_panel.h"

#include "encoder_hw_worker.h"
#include "imgui.h"
#include "project.h"

#include <algorithm>
#include <iomanip>
#include <map>
#include <set>
#include <sstream>

namespace {

bool is_supported_record_output_factor(const int factor)
{
    return factor == 1 || factor == 2 || factor == 4 || factor == 8 || factor == 16;
}

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

std::vector<int> build_unique_gpu_id_list(const int source_gpu_id,
                                          const std::vector<int>& encode_gpu_ids)
{
    std::vector<int> result;
    std::set<int> seen_gpu_ids;
    if (source_gpu_id >= 0 && seen_gpu_ids.insert(source_gpu_id).second) {
        result.push_back(source_gpu_id);
    }
    for (int gpu_id : encode_gpu_ids) {
        if (gpu_id < 0) {
            continue;
        }
        if (seen_gpu_ids.insert(gpu_id).second) {
            result.push_back(gpu_id);
        }
    }
    return result;
}

std::vector<int> build_helper_gpu_id_list(const int source_gpu_id,
                                          const std::vector<int>& encode_gpu_ids)
{
    std::vector<int> result;
    std::set<int> seen_gpu_ids;
    for (int gpu_id : encode_gpu_ids) {
        if (gpu_id < 0 || gpu_id == source_gpu_id) {
            continue;
        }
        if (seen_gpu_ids.insert(gpu_id).second) {
            result.push_back(gpu_id);
        }
    }
    return result;
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

struct HelperGpuValidationSummary {
    int helper_gpu_id = -1;
    std::string topology_class;
    std::string topology_error;
    bool can_access_peer = false;
    bool can_access_peer_known = false;
    bool topology_matches_preference = true;
    bool peer_requirement_satisfied = true;
};

struct CameraAdvancedRecordingValidationSummary {
    int camera_index = -1;
    std::string camera_serial;
    bool record_enabled = false;
    bool split_gop_enabled = false;
    int source_gpu_id = -1;
    std::vector<int> claimed_gpu_ids;
    std::vector<int> helper_gpu_ids;
    std::vector<HelperGpuValidationSummary> helpers;
    std::vector<std::string> local_errors;
    std::vector<std::string> session_errors;
    std::string preferred_topology_class;
    bool require_peer_access = false;
    std::string transfer_mode;
    std::string source_encoder_policy;

    bool valid() const { return local_errors.empty() && session_errors.empty(); }
};

CameraAdvancedRecordingValidationSummary build_camera_advanced_recording_validation_summary(
    const CameraParams& camera_params,
    const CameraEachSelect& camera_select,
    const int camera_index)
{
    CameraAdvancedRecordingValidationSummary summary;
    summary.camera_index = camera_index;
    summary.camera_serial = camera_params.camera_serial;
    summary.record_enabled = camera_select.record;
    summary.split_gop_enabled = camera_params.recording.strategy.split_gop_enabled();
    summary.source_gpu_id = camera_params.gpu_id;
    summary.preferred_topology_class =
        camera_params.recording.constraints.preferred_topology_class;
    summary.require_peer_access =
        camera_params.recording.constraints.require_peer_access;
    summary.transfer_mode =
        camera_params.recording.strategy.split_gop.transfer_mode;
    summary.source_encoder_policy =
        camera_params.recording.strategy.split_gop.source_encoder_policy;

    if (!summary.record_enabled || !summary.split_gop_enabled) {
        return summary;
    }

    summary.claimed_gpu_ids = build_unique_gpu_id_list(
        summary.source_gpu_id,
        camera_params.recording.strategy.split_gop.encoder_gpu_ids);
    summary.helper_gpu_ids = build_helper_gpu_id_list(
        summary.source_gpu_id,
        camera_params.recording.strategy.split_gop.encoder_gpu_ids);

    if (summary.helper_gpu_ids.empty()) {
        summary.local_errors.push_back(
            "No non-source helper GPU is configured for split-GOP recording.");
    } else if (summary.helper_gpu_ids.size() != 1) {
        std::ostringstream oss;
        oss << "GUI validation currently supports exactly one helper GPU; found "
            << summary.helper_gpu_ids.size() << ".";
        summary.local_errors.push_back(oss.str());
    }

    for (int helper_gpu_id : summary.helper_gpu_ids) {
        HelperGpuValidationSummary helper_summary;
        helper_summary.helper_gpu_id = helper_gpu_id;

        const nlohmann::json copy_path_info =
            build_gpu_copy_path_static_topology_info(summary.source_gpu_id, helper_gpu_id);

        if (copy_path_info.contains("topology_class") &&
            copy_path_info["topology_class"].is_string()) {
            helper_summary.topology_class = copy_path_info["topology_class"].get<std::string>();
        }
        if (copy_path_info.contains("topology_lookup_error") &&
            copy_path_info["topology_lookup_error"].is_string()) {
            helper_summary.topology_error =
                copy_path_info["topology_lookup_error"].get<std::string>();
        }

        if (copy_path_info.contains("peer_access_capability") &&
            copy_path_info["peer_access_capability"].is_object()) {
            const nlohmann::json& peer_access = copy_path_info["peer_access_capability"];
            if (peer_access.contains("can_access_peer") &&
                peer_access["can_access_peer"].is_boolean()) {
                helper_summary.can_access_peer = peer_access["can_access_peer"].get<bool>();
                helper_summary.can_access_peer_known = true;
            }
        }

        if (!summary.preferred_topology_class.empty() &&
            helper_summary.topology_class != summary.preferred_topology_class) {
            helper_summary.topology_matches_preference = false;
            std::ostringstream oss;
            oss << "Helper GPU " << helper_gpu_id
                << " is " << (helper_summary.topology_class.empty()
                                  ? std::string("(unknown topology)")
                                  : helper_summary.topology_class)
                << " relative to source GPU " << summary.source_gpu_id
                << "; expected " << summary.preferred_topology_class << ".";
            summary.local_errors.push_back(oss.str());
        }

        if (summary.require_peer_access &&
            (!helper_summary.can_access_peer_known || !helper_summary.can_access_peer)) {
            helper_summary.peer_requirement_satisfied = false;
            std::ostringstream oss;
            oss << "Helper GPU " << helper_gpu_id
                << " does not satisfy the peer-access requirement for source GPU "
                << summary.source_gpu_id << ".";
            summary.local_errors.push_back(oss.str());
        }

        summary.helpers.push_back(std::move(helper_summary));
    }

    return summary;
}

void populate_split_gop_session_conflicts(
    std::vector<CameraAdvancedRecordingValidationSummary>* summaries)
{
    if (!summaries) {
        return;
    }

    std::map<int, std::vector<std::size_t>> gpu_claims;
    for (std::size_t i = 0; i < summaries->size(); ++i) {
        const CameraAdvancedRecordingValidationSummary& summary = (*summaries)[i];
        if (!summary.record_enabled || !summary.split_gop_enabled) {
            continue;
        }
        for (int gpu_id : summary.claimed_gpu_ids) {
            gpu_claims[gpu_id].push_back(i);
        }
    }

    for (const auto& [gpu_id, claiming_indices] : gpu_claims) {
        if (claiming_indices.size() < 2) {
            continue;
        }

        std::ostringstream cameras_oss;
        for (std::size_t position = 0; position < claiming_indices.size(); ++position) {
            if (position > 0) {
                cameras_oss << ", ";
            }
            cameras_oss << (*summaries)[claiming_indices[position]].camera_serial;
        }

        for (std::size_t summary_index : claiming_indices) {
            std::ostringstream conflict;
            conflict << "GPU " << gpu_id
                     << " is claimed by multiple record-enabled split-GOP cameras: "
                     << cameras_oss.str() << ".";
            (*summaries)[summary_index].session_errors.push_back(conflict.str());
        }
    }
}

void render_advanced_recording_validation_summary(CameraParams* cameras_params,
                                                  CameraEachSelect* cameras_select,
                                                  const int num_cameras)
{
    ImGui::SeparatorText("Advanced Recording Validation");

    if (!cameras_params || !cameras_select || num_cameras <= 0) {
        ImGui::TextDisabled("Open cameras to inspect advanced recording validation.");
        return;
    }

    std::vector<CameraAdvancedRecordingValidationSummary> summaries;
    summaries.reserve(static_cast<std::size_t>(num_cameras));
    int record_enabled_count = 0;
    int record_enabled_split_gop_count = 0;
    for (int i = 0; i < num_cameras; ++i) {
        summaries.push_back(build_camera_advanced_recording_validation_summary(
            cameras_params[i], cameras_select[i], i));
        if (cameras_select[i].record) {
            ++record_enabled_count;
            if (cameras_params[i].recording.strategy.split_gop_enabled()) {
                ++record_enabled_split_gop_count;
            }
        }
    }
    populate_split_gop_session_conflicts(&summaries);

    if (record_enabled_count == 0) {
        ImGui::TextDisabled("No cameras are currently selected for recording.");
        return;
    }

    if (record_enabled_split_gop_count == 0) {
        ImGui::TextDisabled("No record-enabled cameras are currently using split-GOP recording.");
        return;
    }

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
        "Read-only summary for record-enabled split-GOP cameras. Editing comes in a later slice.");
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
}

}  // namespace

namespace orange::gui {

void sanitize_record_output_config(std::string* mode, int* factor, int* width, int* height)
{
    if (*mode != "exact_size") {
        *mode = "factor";
    }
    if (!is_supported_record_output_factor(*factor)) {
        *factor = 1;
    }
    if (*width < 1) {
        *width = 1024;
    }
    if (*height < 1) {
        *height = 1024;
    }
}

std::string record_output_summary(const std::string& mode, const int factor, const int width, const int height)
{
    if (mode == "exact_size") {
        return std::string("exact ") + std::to_string(width) + "x" + std::to_string(height);
    }
    return std::string("factor ") + std::to_string(factor) + "x";
}

std::string format_bitrate_mbps(const uint32_t bitrate_bps)
{
    std::ostringstream oss;
    oss << std::fixed << std::setprecision(1)
        << (static_cast<double>(bitrate_bps) / 1000000.0);
    return oss.str();
}

RecordingOutputConfig resolve_recording_output_config(
    const CameraParams& camera_params,
    const EncoderConfig& encoder_config,
    const CameraEachSelect& camera_select,
    std::string* warning_out)
{
    std::string mode = camera_select.record_output_override
        ? camera_select.record_output_mode
        : encoder_config.record_output_mode;
    int factor = camera_select.record_output_override
        ? camera_select.record_downsample_factor
        : encoder_config.record_downsample_factor;
    int width = camera_select.record_output_override
        ? camera_select.record_output_width
        : encoder_config.record_output_width;
    int height = camera_select.record_output_override
        ? camera_select.record_output_height
        : encoder_config.record_output_height;
    sanitize_record_output_config(&mode, &factor, &width, &height);
    CameraRecordingOutputConfig requested_output;
    requested_output.mode = mode;
    requested_output.downsample_factor = factor;
    requested_output.requested_width = width;
    requested_output.requested_height = height;
    return resolve_effective_recording_output_config(camera_params, requested_output, warning_out);
}

RecordingPanelActions render_recording_config_panel(std::string* input_folder,
                                                    EncoderConfig* encoder_config,
                                                    const bool camera_open,
                                                    const bool streaming_active,
                                                    CameraParams* cameras_params,
                                                    CameraEachSelect* cameras_select,
                                                    const int num_cameras)
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

    sanitize_record_output_config(
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
        sanitize_record_output_config(
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
        record_output_summary(
            encoder_config->record_output_mode,
            encoder_config->record_downsample_factor,
            encoder_config->record_output_width,
            encoder_config->record_output_height
        ).c_str());

    if (camera_open && cameras_params != nullptr && cameras_select != nullptr && num_cameras > 0) {
        ImGui::SeparatorText("Estimated Recording Bitrate");
        for (int i = 0; i < num_cameras; ++i) {
            std::string output_warning;
            const RecordingOutputConfig recording_output_config =
                resolve_recording_output_config(
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
                     << format_bitrate_mbps(bitrate_estimate.average_bitrate)
                     << " Mbps, max "
                     << format_bitrate_mbps(bitrate_estimate.max_bitrate)
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
                sanitize_record_output_config(
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
                    record_output_summary(
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

    render_advanced_recording_validation_summary(cameras_params, cameras_select, num_cameras);

    return actions;
}

}  // namespace orange::gui
