#include "gui/camera_properties_panel.h"

#include "imgui.h"
#include "misc/cpp/imgui_stdlib.h"
#include "project.h"

#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

namespace {

bool combo_select_string(const char* label,
                         std::string* value,
                         const char* const* option_values,
                         const char* const* option_labels,
                         int option_count)
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

std::string describe_gpio_recipe_validation(const CameraParams& camera_params)
{
    if (!camera_params.gpio_recipe.empty()) {
        std::vector<CameraGpioNodeConfig> recipe_nodes;
        std::string recipe_error;
        if (!build_gpio_recipe_preview_nodes(&camera_params, &recipe_nodes, &recipe_error)) {
            return recipe_error;
        }
    }

    for (const auto& node : camera_params.gpio_nodes) {
        if (node.name.empty()) {
            return "Each gpio.nodes entry needs a node name.";
        }

        const std::string node_type = node.type;
        if (node_type != "enum" && node_type != "bool" && node_type != "uint") {
            return "gpio.nodes supports only enum, bool, and uint node types.";
        }

        if (node_type == "enum" && node.value_string.empty()) {
            return "Enum gpio.nodes entries need a non-empty value.";
        }
    }

    return {};
}

std::string format_gpio_node_value(const CameraGpioNodeConfig& node)
{
    if (node.type == "enum") {
        return node.value_string;
    }
    if (node.type == "bool") {
        return node.value_bool ? "true" : "false";
    }
    return std::to_string(node.value_uint);
}

}  // namespace

namespace orange::gui {

void render_camera_properties_panel(CameraEmergent* ecams,
                                    CameraParams* cameras_params,
                                    const int num_cameras,
                                    std::vector<std::string>& color_temps,
                                    const std::string& selected_local_config_folder)
{
    static const char* const kSyncModeValues[] = {
        "free_run",
        "ptp_gate",
        "external_trigger",
        "software_trigger"
    };
    static const char* const kSyncModeLabels[] = {
        "Free Run",
        "PTP Gate",
        "External Trigger",
        "Software Trigger"
    };
    static const char* const kScanTypeValues[] = {
        "unknown",
        "area_scan",
        "line_scan"
    };
    static const char* const kScanTypeLabels[] = {
        "Unknown",
        "Area Scan",
        "Line Scan"
    };
    static const char* const kConnectorValues[] = {
        "unknown",
        "area_scan_12_pin",
        "area_scan_8_pin",
        "line_scan_12_pin"
    };
    static const char* const kConnectorLabels[] = {
        "Unknown",
        "Area Scan 12-pin",
        "Area Scan 8-pin",
        "Line Scan 12-pin"
    };
    static const char* const kRecipeValues[] = {
        "",
        "area_scan_hw_trigger_internal_gpi4",
        "area_scan_hw_trigger_external_gpi4",
        "line_scan_hw_frame_gpi1_internal_line",
        "line_scan_hw_frame_gpi1_encoder_line",
        "line_scan_encoder_frame_encoder_line",
        "line_scan_hw_gate_gpi1_encoder_frame_encoder_line"
    };
    static const char* const kRecipeLabels[] = {
        "(none)",
        "Area scan HW trigger GPI4, internal end",
        "Area scan HW trigger GPI4, external end",
        "Line scan HW frame GPI1, internal line",
        "Line scan HW frame GPI1, encoder line",
        "Line scan encoder frame, encoder line",
        "Line scan HW gate GPI1, encoder frame + line"
    };
    static const char* const kTriggerSourceValues[] = {
        "Software",
        "Hardware"
    };
    static const char* const kTriggerSourceLabels[] = {
        "Software",
        "Hardware"
    };
    static const char* const kTriggerActivationValues[] = {
        "RisingEdge",
        "FallingEdge",
        "LevelHigh",
        "LevelLow"
    };
    static const char* const kTriggerActivationLabels[] = {
        "RisingEdge",
        "FallingEdge",
        "LevelHigh",
        "LevelLow"
    };
    static const char* const kPtpModeValues[] = {
        "",
        "TwoStep",
        "OneStep"
    };
    static const char* const kPtpModeLabels[] = {
        "(unset)",
        "TwoStep",
        "OneStep"
    };
    static const char* const kGpioNodeTypeValues[] = {
        "enum",
        "bool",
        "uint"
    };
    static const char* const kGpioNodeTypeLabels[] = {
        "enum",
        "bool",
        "uint"
    };

    if (ImGui::TreeNode("Camera Property")) {
        static int selected_camera = 0;
        static int slider_gain, slider_exposure, slider_frame_rate, slider_width, slider_height, OffsetX, OffsetY, slider_focus, slider_iris;
        static std::string config_save_status;
        static bool config_save_error = false;
        static int config_save_status_camera = -1;

        if (num_cameras <= 0) {
            ImGui::TreePop();
            return;
        }

        selected_camera = std::clamp(selected_camera, 0, num_cameras - 1);

        for (int n = 0; n < num_cameras; n++) {
            if (ImGui::Selectable(cameras_params[n].camera_name.c_str(), selected_camera == n)) {
                selected_camera = n;
            }
            slider_gain = cameras_params[selected_camera].gain;
            slider_iris = cameras_params[selected_camera].iris;
            slider_focus = cameras_params[selected_camera].focus;
            slider_width = cameras_params[selected_camera].width;
            slider_height = cameras_params[selected_camera].height;
            slider_exposure = cameras_params[selected_camera].exposure;
            slider_frame_rate = cameras_params[selected_camera].frame_rate;
            OffsetX = cameras_params[selected_camera].offsetx;
            OffsetY = cameras_params[selected_camera].offsety;
        }

        ImGui::Checkbox("GPU Direct", &cameras_params[selected_camera].gpu_direct);
        ImGui::Checkbox("Color", &cameras_params[selected_camera].color);

        if (cameras_params[selected_camera].color) {
            auto it = std::find(color_temps.begin(), color_temps.end(), cameras_params->color_temp);
            int item_current_idx = (it != color_temps.end()) ? std::distance(color_temps.begin(), it) : 0;
            std::vector<const char*> item_cstrs;
            for (const auto& item : color_temps) {
                item_cstrs.push_back(item.c_str());
            }
            if (ImGui::Combo("Color Temp", &item_current_idx, item_cstrs.data(), color_temps.size())) {
                update_color_temperature(&ecams[selected_camera].camera, color_temps[item_current_idx], &cameras_params[selected_camera]);
            }
        }

        if (ImGui::SliderInt("Width", &slider_width, cameras_params[selected_camera].width_min, cameras_params[selected_camera].width_max, "%d")) {
            slider_width = (slider_width / 16) * 16;
            update_width_value(&ecams[selected_camera].camera, slider_width, &cameras_params[selected_camera]);
        }

        if (ImGui::SliderInt("Height", &slider_height, cameras_params[selected_camera].height_min, cameras_params[selected_camera].height_max, "%d")) {
            slider_height = (slider_height / 16) * 16;
            update_height_value(&ecams[selected_camera].camera, slider_height, &cameras_params[selected_camera]);
        }

        if (ImGui::SliderInt("OffsetX", &OffsetX, cameras_params[selected_camera].offsetx_min, cameras_params[selected_camera].offsetx_max, "%d")) {
            OffsetX = (OffsetX / 16) * 16;
            update_offsetX_value(&ecams[selected_camera].camera, OffsetX, &cameras_params[selected_camera]);
        }

        if (ImGui::SliderInt("OffsetY", &OffsetY, cameras_params[selected_camera].offsety_min, cameras_params[selected_camera].offsety_max, "%d")) {
            OffsetY = (OffsetY / 16) * 16;
            update_offsetY_value(&ecams[selected_camera].camera, OffsetY, &cameras_params[selected_camera]);
        }

        if (ImGui::SliderInt("Gain", &slider_gain, cameras_params[selected_camera].gain_min, cameras_params[selected_camera].gain_max, "%d")) {
            update_gain_value(&ecams[selected_camera].camera, slider_gain, &cameras_params[selected_camera]);
        }

        if (ImGui::SliderInt("Focus", &slider_focus, cameras_params[selected_camera].focus_min, cameras_params[selected_camera].focus_max, "%d")) {
            update_focus_value(&ecams[selected_camera].camera, slider_focus, &cameras_params[selected_camera]);
        }

        if (ImGui::SliderInt("Iris", &slider_iris, cameras_params[selected_camera].iris_min, cameras_params[selected_camera].iris_max, "%d")) {
            update_iris_value(&ecams[selected_camera].camera, slider_iris, &cameras_params[selected_camera]);
        }

        if (ImGui::SliderInt("Exposure", &slider_exposure, cameras_params[selected_camera].exposure_min, cameras_params[selected_camera].exposure_max, "%d")) {
            update_exposure_framerate_value(&ecams[selected_camera].camera, slider_exposure, &slider_frame_rate, &cameras_params[selected_camera]);
        }

        char label[32];
        sprintf(label, "FrameRate (%d -> %d)", cameras_params[selected_camera].frame_rate_min, cameras_params[selected_camera].frame_rate_max);
        if (ImGui::SliderInt(label, &slider_frame_rate, cameras_params[selected_camera].frame_rate_min, cameras_params[selected_camera].frame_rate_max, "%d")) {
            update_frame_rate_value(&ecams[selected_camera].camera, slider_frame_rate, &cameras_params[selected_camera]);
        }

        ImGui::Separator();
        ImGui::Text("Schema: orange.camera.config v3");
        ImGui::TextWrapped("Device model: %s", cameras_params[selected_camera].device_model.empty()
                                                   ? "(unknown)"
                                                   : cameras_params[selected_camera].device_model.c_str());
        ImGui::TextWrapped("Device serial: %s", cameras_params[selected_camera].camera_serial.c_str());
        ImGui::TextDisabled("Sync/GPIO/crop changes are saved to config and applied the next time the camera is opened.");
        ImGui::Text("Crop pipeline crop_size_px: %d", cameras_params[selected_camera].crop_pipeline.crop_size_px);
        ImGui::TextDisabled("Edit crop size in the main Orange panel; Save to config persists it for this camera.");

        ImGui::Separator();
        ImGui::Checkbox("Focus UART Bootstrap", &cameras_params[selected_camera].focus_uart_bootstrap);
        const std::string previous_sync_mode = cameras_params[selected_camera].sync_mode;
        if (combo_select_string("Sync Mode",
                                &cameras_params[selected_camera].sync_mode,
                                kSyncModeValues,
                                kSyncModeLabels,
                                IM_ARRAYSIZE(kSyncModeValues))) {
            if (previous_sync_mode == "ptp_gate" && cameras_params[selected_camera].sync_mode != "ptp_gate") {
                cameras_params[selected_camera].ptp_mode.clear();
            }
        }
        combo_select_string("Camera Scan Type",
                            &cameras_params[selected_camera].camera_scan_type,
                            kScanTypeValues,
                            kScanTypeLabels,
                            IM_ARRAYSIZE(kScanTypeValues));
        combo_select_string("GPIO Connector Variant",
                            &cameras_params[selected_camera].gpio_connector_variant,
                            kConnectorValues,
                            kConnectorLabels,
                            IM_ARRAYSIZE(kConnectorValues));
        combo_select_string("GPIO Recipe",
                            &cameras_params[selected_camera].gpio_recipe,
                            kRecipeValues,
                            kRecipeLabels,
                            IM_ARRAYSIZE(kRecipeValues));
        std::vector<CameraGpioNodeConfig> recipe_preview_nodes;
        std::string recipe_preview_error;
        const bool has_recipe_preview = build_gpio_recipe_preview_nodes(
            &cameras_params[selected_camera],
            &recipe_preview_nodes,
            &recipe_preview_error);
        if (has_recipe_preview && ImGui::TreeNode("Recipe Node Writes")) {
            ImGui::TextDisabled("These node writes come from the selected GPIO recipe.");
            for (int recipe_node_idx = 0; recipe_node_idx < static_cast<int>(recipe_preview_nodes.size()); ++recipe_node_idx) {
                const CameraGpioNodeConfig& node = recipe_preview_nodes[recipe_node_idx];
                ImGui::BulletText("%s = %s", node.name.c_str(), format_gpio_node_value(node).c_str());
            }
            if (!cameras_params[selected_camera].gpio_nodes.empty()) {
                ImGui::Separator();
                ImGui::TextDisabled("Explicit gpio.nodes entries below are applied after the recipe and can override these values.");
            }
            ImGui::TreePop();
        } else if (!recipe_preview_error.empty()) {
            ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.35f, 1.0f), "%s", recipe_preview_error.c_str());
        }

        ImGui::Checkbox("Trigger Enabled", &cameras_params[selected_camera].trigger_enabled);
        ImGui::InputText("Trigger Selector", &cameras_params[selected_camera].trigger_selector);
        combo_select_string("Trigger Source",
                            &cameras_params[selected_camera].trigger_source,
                            kTriggerSourceValues,
                            kTriggerSourceLabels,
                            IM_ARRAYSIZE(kTriggerSourceValues));
        combo_select_string("Trigger Activation",
                            &cameras_params[selected_camera].trigger_activation,
                            kTriggerActivationValues,
                            kTriggerActivationLabels,
                            IM_ARRAYSIZE(kTriggerActivationValues));
        combo_select_string("PTP Mode",
                            &cameras_params[selected_camera].ptp_mode,
                            kPtpModeValues,
                            kPtpModeLabels,
                            IM_ARRAYSIZE(kPtpModeValues));
        if (cameras_params[selected_camera].sync_mode != "ptp_gate") {
            ImGui::TextDisabled("PTP mode is unused unless Sync Mode is PTP Gate and will be omitted on save.");
        }

        if (ImGui::TreeNode("GPIO Nodes")) {
            int remove_gpio_node_index = -1;
            for (int node_idx = 0; node_idx < static_cast<int>(cameras_params[selected_camera].gpio_nodes.size()); ++node_idx) {
                CameraGpioNodeConfig& node = cameras_params[selected_camera].gpio_nodes[node_idx];
                ImGui::PushID(node_idx);
                ImGui::Separator();
                ImGui::Text("Node %d", node_idx + 1);
                ImGui::InputText("Name", &node.name);
                combo_select_string("Type",
                                    &node.type,
                                    kGpioNodeTypeValues,
                                    kGpioNodeTypeLabels,
                                    IM_ARRAYSIZE(kGpioNodeTypeValues));

                if (node.type == "enum") {
                    ImGui::InputText("Value", &node.value_string);
                } else if (node.type == "bool") {
                    ImGui::Checkbox("Value", &node.value_bool);
                } else {
                    int value_uint = static_cast<int>(node.value_uint);
                    if (ImGui::InputInt("Value", &value_uint)) {
                        node.value_uint = static_cast<uint32_t>(std::max(0, value_uint));
                    }
                }

                if (ImGui::Button("Remove node")) {
                    remove_gpio_node_index = node_idx;
                }
                ImGui::PopID();
            }

            if (remove_gpio_node_index >= 0) {
                cameras_params[selected_camera].gpio_nodes.erase(
                    cameras_params[selected_camera].gpio_nodes.begin() + remove_gpio_node_index);
            }

            if (ImGui::Button("Add GPIO node")) {
                cameras_params[selected_camera].gpio_nodes.push_back(CameraGpioNodeConfig{});
            }
            ImGui::TreePop();
        }

        const std::string gpio_validation_warning = describe_gpio_recipe_validation(cameras_params[selected_camera]);
        if (!gpio_validation_warning.empty()) {
            ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.35f, 1.0f), "%s", gpio_validation_warning.c_str());
        } else if (!cameras_params[selected_camera].gpio_recipe.empty()) {
            ImGui::TextColored(ImVec4(0.35f, 0.85f, 0.45f, 1.0f),
                               "GPIO recipe is compatible with the selected scan type and connector.");
        }

        if (!cameras_params[selected_camera].gpio_recipe.empty() && cameras_params[selected_camera].trigger_enabled) {
            ImGui::TextDisabled("Note: gpio_recipe is the primary trigger programming path; generic trigger fields are mainly fallback/override metadata.");
        }
        if (cameras_params[selected_camera].sync_mode == "ptp_gate") {
            ImGui::TextDisabled("Note: ptp_gate uses the existing gated-start flow and does not use the generic trigger block as its primary runtime path.");
        }

        ImGui::Separator();
        const bool has_config_path = !cameras_params[selected_camera].config_path.empty();
        const bool has_selected_local_folder = !selected_local_config_folder.empty();
        const std::string suggested_config_path =
            has_selected_local_folder
                ? build_camera_config_path(selected_local_config_folder, cameras_params[selected_camera])
                : std::string();
        ImGui::TextWrapped("Config file: %s", has_config_path ? cameras_params[selected_camera].config_path.c_str() : "No loaded config file");
        if (!has_config_path && has_selected_local_folder && !suggested_config_path.empty()) {
            ImGui::TextWrapped("Selected local folder save target: %s", suggested_config_path.c_str());
        }
        if (has_selected_local_folder && !suggested_config_path.empty()) {
            if (ImGui::Button("Use selected local folder")) {
                cameras_params[selected_camera].config_path = suggested_config_path;
                config_save_status = std::string("Set save target: ") + suggested_config_path;
                config_save_error = false;
                config_save_status_camera = selected_camera;
            }
            ImGui::SameLine();
        }
        if (!has_config_path && !has_selected_local_folder) {
            ImGui::BeginDisabled();
        }
        if (ImGui::Button("Save to config")) {
            if (cameras_params[selected_camera].config_path.empty() && !suggested_config_path.empty()) {
                cameras_params[selected_camera].config_path = suggested_config_path;
            }
            std::string save_error;
            if (save_camera_json_config(cameras_params[selected_camera], &save_error)) {
                config_save_status = std::string("Saved camera config: ") + cameras_params[selected_camera].config_path;
                config_save_error = false;
            } else {
                config_save_status = save_error.empty() ? "Failed to save camera config." : save_error;
                config_save_error = true;
            }
            config_save_status_camera = selected_camera;
        }
        if (!has_config_path && !has_selected_local_folder) {
            ImGui::EndDisabled();
        }
        if (config_save_status_camera == selected_camera && !config_save_status.empty()) {
            if (config_save_error) {
                ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.35f, 1.0f), "%s", config_save_status.c_str());
            } else {
                ImGui::TextColored(ImVec4(0.35f, 0.85f, 0.45f, 1.0f), "%s", config_save_status.c_str());
            }
        }

        ImGui::TreePop();
    }
}

}  // namespace orange::gui
