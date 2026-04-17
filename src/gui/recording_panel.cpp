#include "gui/recording_panel.h"

#include "encoder_hw_worker.h"
#include "imgui.h"
#include "project.h"

#include <iomanip>
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

    return actions;
}

}  // namespace orange::gui
