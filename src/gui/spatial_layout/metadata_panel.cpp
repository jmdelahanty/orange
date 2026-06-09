#include "gui/spatial_layout/metadata_panel.h"

#include "gui/spatial_layout/preflight.h"
#include "imgui.h"
#include "misc/cpp/imgui_stdlib.h"

#include <algorithm>
#include <iterator>
#include <sstream>
#include <vector>

namespace orange::gui::spatial_layout {
namespace {

constexpr const char* kHoyaR72FilterInstalled =
    "installed: HOYA Creative Filter Infrared R72 67 mm (Kenko Tokina)";
constexpr const char* kHoyaR72FilterRemoved =
    "removed: HOYA Creative Filter Infrared R72 67 mm (Kenko Tokina)";

bool render_string_preset_combo(
    const char* label,
    std::string* value,
    const char* const* presets,
    int preset_count)
{
    if (value == nullptr || presets == nullptr || preset_count <= 0) {
        return false;
    }

    const char* preview = value->empty() ? "unknown" : value->c_str();
    bool changed = false;
    if (ImGui::BeginCombo(label, preview)) {
        for (int idx = 0; idx < preset_count; ++idx) {
            const char* preset = presets[idx] ? presets[idx] : "";
            const bool selected = *value == preset;
            if (ImGui::Selectable(preset, selected)) {
                *value = preset;
                changed = true;
            }
            if (selected) {
                ImGui::SetItemDefaultFocus();
            }
        }
        ImGui::EndCombo();
    }
    return changed;
}

} // namespace

void apply_illumination_preset(SpatialLayoutUiState* ui_state, const std::string& preset)
{
    if (ui_state == nullptr) {
        return;
    }
    if (preset == "custom_ttl_nir_strobe_855nm") {
        ui_state->calibration_light_state = "ttl_nir_strobe_active";
        ui_state->calibration_illumination_spectrum = "narrowband_nir";
        ui_state->calibration_illumination_source = "custom_ttl_nir_strobe";
        ui_state->calibration_illumination_center_wavelength_nm = 855.0;
        ui_state->calibration_has_illumination_center_wavelength_nm = true;
        ui_state->calibration_has_illumination_min_wavelength_nm = false;
        ui_state->calibration_has_illumination_max_wavelength_nm = false;
        ui_state->calibration_has_illumination_bandwidth_fwhm_nm = false;
        ui_state->calibration_illumination_wavelength_confidence = "nominal";
    } else if (preset == "visible_projector_broadband") {
        ui_state->calibration_light_state = "visible_projector_only";
        ui_state->calibration_illumination_spectrum = "broadband_visible";
        ui_state->calibration_illumination_source = "visible_projector";
        ui_state->calibration_has_illumination_center_wavelength_nm = false;
        ui_state->calibration_illumination_min_wavelength_nm = 400.0;
        ui_state->calibration_has_illumination_min_wavelength_nm = true;
        ui_state->calibration_illumination_max_wavelength_nm = 700.0;
        ui_state->calibration_has_illumination_max_wavelength_nm = true;
        ui_state->calibration_has_illumination_bandwidth_fwhm_nm = false;
        ui_state->calibration_illumination_wavelength_confidence = "approximate_range";
    } else if (preset == "ambient_room_light_visible") {
        ui_state->calibration_light_state = "ambient_room_light";
        ui_state->calibration_illumination_spectrum = "broadband_visible";
        ui_state->calibration_illumination_source = "ambient_room_light";
        ui_state->calibration_has_illumination_center_wavelength_nm = false;
        ui_state->calibration_illumination_min_wavelength_nm = 400.0;
        ui_state->calibration_has_illumination_min_wavelength_nm = true;
        ui_state->calibration_illumination_max_wavelength_nm = 700.0;
        ui_state->calibration_has_illumination_max_wavelength_nm = true;
        ui_state->calibration_has_illumination_bandwidth_fwhm_nm = false;
        ui_state->calibration_illumination_wavelength_confidence = "approximate_range";
    } else if (preset == "external_continuous_visible_light") {
        ui_state->calibration_light_state = "external_continuous_visible_light";
        ui_state->calibration_illumination_spectrum = "broadband_visible";
        ui_state->calibration_illumination_source = "external_continuous_visible_light";
        ui_state->calibration_has_illumination_center_wavelength_nm = false;
        ui_state->calibration_illumination_min_wavelength_nm = 400.0;
        ui_state->calibration_has_illumination_min_wavelength_nm = true;
        ui_state->calibration_illumination_max_wavelength_nm = 700.0;
        ui_state->calibration_has_illumination_max_wavelength_nm = true;
        ui_state->calibration_has_illumination_bandwidth_fwhm_nm = false;
        ui_state->calibration_illumination_wavelength_confidence = "approximate_range";
    } else if (preset == "external_continuous_ir_nir_light") {
        ui_state->calibration_light_state = "external_continuous_ir_nir_light";
        ui_state->calibration_illumination_spectrum = "unknown_ir_nir";
        ui_state->calibration_illumination_source = "external_continuous_ir_nir_light";
        ui_state->calibration_has_illumination_center_wavelength_nm = false;
        ui_state->calibration_has_illumination_min_wavelength_nm = false;
        ui_state->calibration_has_illumination_max_wavelength_nm = false;
        ui_state->calibration_has_illumination_bandwidth_fwhm_nm = false;
        ui_state->calibration_illumination_wavelength_confidence = "unknown";
    } else if (preset == "lights_off") {
        ui_state->calibration_light_state = "lights_off";
        ui_state->calibration_illumination_spectrum = "none";
        ui_state->calibration_illumination_source = "none";
        ui_state->calibration_has_illumination_center_wavelength_nm = false;
        ui_state->calibration_has_illumination_min_wavelength_nm = false;
        ui_state->calibration_has_illumination_max_wavelength_nm = false;
        ui_state->calibration_has_illumination_bandwidth_fwhm_nm = false;
        ui_state->calibration_illumination_wavelength_confidence = "not_applicable";
    } else if (preset == "unknown") {
        ui_state->calibration_light_state = "unknown";
        ui_state->calibration_illumination_spectrum = "unknown";
        ui_state->calibration_illumination_source = "unknown";
        ui_state->calibration_has_illumination_center_wavelength_nm = false;
        ui_state->calibration_has_illumination_min_wavelength_nm = false;
        ui_state->calibration_has_illumination_max_wavelength_nm = false;
        ui_state->calibration_has_illumination_bandwidth_fwhm_nm = false;
        ui_state->calibration_illumination_wavelength_confidence = "unknown";
    }
}

void render_calibration_capture_metadata_panel(
    SpatialLayoutUiState* ui_state,
    CameraControl* camera_control,
    CameraEmergent* ecams,
    CameraParams* cameras_params,
    int num_cameras,
    CameraParams& selected_camera,
    const CalibrationCaptureMetadataPanelActions& actions)
{
    if (ui_state == nullptr ||
        camera_control == nullptr ||
        ecams == nullptr ||
        cameras_params == nullptr ||
        num_cameras <= 0) {
        return;
    }

    ImGui::SeparatorText("Calibration Capture Metadata");
    static constexpr const char* kFilterStatePresets[] = {
        "unknown",
        kHoyaR72FilterInstalled,
        kHoyaR72FilterRemoved,
        "no_filter_installed"
    };
    static constexpr const char* kRuntimeFilterStatePresets[] = {
        "unknown",
        kHoyaR72FilterInstalled,
        kHoyaR72FilterRemoved,
        "not_applicable"
    };
    static constexpr const char* kLightStatePresets[] = {
        "unknown",
        "ttl_nir_strobe_active",
        "ttl_nir_strobe_inactive",
        "visible_projector_only",
        "ambient_room_light",
        "external_continuous_visible_light",
        "external_continuous_ir_nir_light",
        "lights_off"
    };
    static constexpr const char* kLightHandlingPresets[] = {
        "leave_current",
        "suppress_mapped_strobe",
        "keep_or_restore_mapped_pulse",
        "force_manual_active",
        "operator_manual"
    };
    static constexpr const char* kProjectorStatePresets[] = {
        "unknown",
        "off",
        "black_or_idle",
        "crosshair_on",
        "homography_grid_on",
        "scale_pattern_on",
        "normal_stimulus_active"
    };
    static constexpr const char* kIlluminationSpectrumPresets[] = {
        "unknown",
        "narrowband_nir",
        "unknown_ir_nir",
        "broadband_visible",
        "broadband_visible_nir",
        "none"
    };
    static constexpr const char* kIlluminationSourcePresets[] = {
        "unknown",
        "custom_ttl_nir_strobe",
        "visible_projector",
        "ambient_room_light",
        "external_continuous_visible_light",
        "external_continuous_ir_nir_light",
        "none"
    };
    static constexpr const char* kIlluminationConfidencePresets[] = {
        "unknown",
        "nominal",
        "measured",
        "approximate_range",
        "not_applicable"
    };
    static constexpr const char* kIlluminationPresetIds[] = {
        "unknown",
        "custom_ttl_nir_strobe_855nm",
        "visible_projector_broadband",
        "ambient_room_light_visible",
        "external_continuous_visible_light",
        "external_continuous_ir_nir_light",
        "lights_off"
    };
    static constexpr const char* kIlluminationPresetLabels[] = {
        "Unknown",
        "Custom TTL NIR strobe, 855 nm nominal",
        "Visible projector, broadband visible",
        "Ambient room light, broadband visible",
        "External continuous visible light",
        "External continuous IR/NIR light",
        "Lights off"
    };
    render_string_preset_combo(
        "Filter state",
        &ui_state->calibration_filter_state,
        kFilterStatePresets,
        IM_ARRAYSIZE(kFilterStatePresets));
    render_string_preset_combo(
        "Runtime filter state",
        &ui_state->calibration_runtime_filter_state,
        kRuntimeFilterStatePresets,
        IM_ARRAYSIZE(kRuntimeFilterStatePresets));
    render_string_preset_combo(
        "Light handling",
        &ui_state->calibration_light_handling,
        kLightHandlingPresets,
        IM_ARRAYSIZE(kLightHandlingPresets));
    if (ImGui::BeginCombo("Illumination preset", "Apply preset...")) {
        for (int idx = 0; idx < IM_ARRAYSIZE(kIlluminationPresetIds); ++idx) {
            if (ImGui::Selectable(kIlluminationPresetLabels[idx])) {
                apply_illumination_preset(ui_state, kIlluminationPresetIds[idx]);
            }
        }
        ImGui::EndCombo();
    }
    render_string_preset_combo(
        "Illumination source",
        &ui_state->calibration_illumination_source,
        kIlluminationSourcePresets,
        IM_ARRAYSIZE(kIlluminationSourcePresets));
    const std::string selected_light_handling =
        ui_state->calibration_light_handling.empty()
            ? "leave_current"
            : ui_state->calibration_light_handling;
    const bool light_handling_needs_mapped_strobe =
        calibration_light_handling_needs_mapped_strobe(selected_light_handling);
    std::vector<int> light_control_camera_indices;
    light_control_camera_indices.reserve(num_cameras);
    for (int i = 0; i < num_cameras; ++i) {
        if (camera_has_exposed_mapped_nir_strobe(cameras_params[i])) {
            light_control_camera_indices.push_back(i);
        }
    }
    auto light_control_candidate_index = [&](const int camera_index) -> int {
        const auto it = std::find(
            light_control_camera_indices.begin(),
            light_control_camera_indices.end(),
            camera_index);
        if (it == light_control_camera_indices.end()) {
            return -1;
        }
        return static_cast<int>(std::distance(light_control_camera_indices.begin(), it));
    };
    if (light_control_camera_indices.empty()) {
        ui_state->calibration_light_control_camera = -1;
    } else if (light_control_candidate_index(ui_state->calibration_light_control_camera) < 0) {
        const int selected_light_control_index =
            light_control_candidate_index(ui_state->selected_camera);
        ui_state->calibration_light_control_camera =
            selected_light_control_index >= 0
                ? ui_state->selected_camera
                : light_control_camera_indices.front();
    }
    if (!light_control_camera_indices.empty()) {
        std::vector<std::string> light_control_labels_storage;
        std::vector<const char*> light_control_labels;
        light_control_labels_storage.reserve(light_control_camera_indices.size());
        light_control_labels.reserve(light_control_camera_indices.size());
        for (const int camera_index : light_control_camera_indices) {
            const CameraParams& light_camera = cameras_params[camera_index];
            const CameraRigIoConnection* connection =
                find_mapped_nir_strobe_output_connection(light_camera);
            std::ostringstream label;
            label << light_camera.camera_serial << " / "
                  << (connection != nullptr && !connection->camera_line.empty()
                          ? connection->camera_line
                          : "mapped output");
            light_control_labels_storage.push_back(label.str());
            light_control_labels.push_back(light_control_labels_storage.back().c_str());
        }
        int light_control_combo_index =
            light_control_candidate_index(ui_state->calibration_light_control_camera);
        if (light_control_combo_index < 0) {
            light_control_combo_index = 0;
        }
        if (ImGui::Combo(
                "Light control camera",
                &light_control_combo_index,
                light_control_labels.data(),
                static_cast<int>(light_control_labels.size()))) {
            ui_state->calibration_light_control_camera =
                light_control_camera_indices[static_cast<size_t>(light_control_combo_index)];
        }
    }
    const int light_control_camera = ui_state->calibration_light_control_camera;
    const bool light_control_camera_valid =
        light_control_camera >= 0 &&
        light_control_camera < num_cameras;
    CameraParams* light_control_params =
        light_control_camera_valid ? &cameras_params[light_control_camera] : nullptr;
    CameraEmergent* light_control_ecam =
        light_control_camera_valid ? &ecams[light_control_camera] : nullptr;
    const CameraRigIoConnection* mapped_strobe_connection =
        light_control_params != nullptr
            ? find_mapped_nir_strobe_output_connection(*light_control_params)
            : nullptr;
    const bool mapped_strobe_available =
        light_control_params != nullptr &&
        light_control_params->gpio_pinout_access == "exposed" &&
        mapped_strobe_connection != nullptr;
    const bool light_output_mutation_locked =
        camera_control->record_video || camera_control->recording_draining;
    const bool can_prepare_calibration_capture =
        !light_output_mutation_locked &&
        (!light_handling_needs_mapped_strobe || mapped_strobe_available);
    ImGui::BeginDisabled(!can_prepare_calibration_capture);
    if (ImGui::Button("Prepare Calibration Capture")) {
        std::string status;
        const bool ok = prepare_calibration_capture_preflight(
            ui_state,
            &ecams[ui_state->selected_camera],
            &selected_camera,
            light_control_ecam,
            light_control_params,
            mapped_strobe_available,
            light_output_mutation_locked,
            selected_light_handling,
            &status);
        set_calibration_preflight_result(ui_state, ok, status);
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(!can_prepare_calibration_capture);
    if (ImGui::Button("Prepare All Cameras")) {
        std::string status;
        const bool ok = prepare_calibration_capture_preflight_all_cameras(
            ui_state,
            ecams,
            cameras_params,
            num_cameras,
            light_control_ecam,
            light_control_params,
            mapped_strobe_available,
            light_output_mutation_locked,
            selected_light_handling,
            &status);
        set_calibration_preflight_result(ui_state, ok, status);
    }
    ImGui::EndDisabled();
    const bool has_any_capture_restore =
        !ui_state->calibration_capture_restore_states.empty();
    const bool has_selected_capture_restore =
        has_calibration_capture_restore_state(ui_state, selected_camera.camera_serial);
    const bool can_restore_calibration_capture =
        !light_output_mutation_locked &&
        (has_selected_capture_restore || mapped_strobe_available);
    const bool can_restore_all_calibration_capture =
        !light_output_mutation_locked &&
        (has_any_capture_restore || mapped_strobe_available);
    ImGui::BeginDisabled(!can_restore_calibration_capture);
    if (ImGui::Button("Restore Camera Config State")) {
        std::string status;
        const bool ok = restore_calibration_capture_preflight(
            ui_state,
            &ecams[ui_state->selected_camera],
            &selected_camera,
            light_control_ecam,
            light_control_params,
            mapped_strobe_available,
            light_output_mutation_locked,
            &status);
        set_calibration_preflight_result(ui_state, ok, status);
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(!can_restore_all_calibration_capture);
    if (ImGui::Button("Restore All Camera Config States")) {
        std::string status;
        const bool ok = restore_calibration_capture_preflight_all_cameras(
            ui_state,
            ecams,
            cameras_params,
            num_cameras,
            light_control_ecam,
            light_control_params,
            mapped_strobe_available,
            light_output_mutation_locked,
            &status);
        set_calibration_preflight_result(ui_state, ok, status);
    }
    ImGui::EndDisabled();
    if (ui_state->calibration_capture_profile_active) {
        ImGui::TextDisabled(
            "Active capture profile: %s capture_cam=%s light_cam=%s",
            ui_state->calibration_capture_profile_id.c_str(),
            ui_state->calibration_capture_profile_camera_serial.c_str(),
            ui_state->calibration_capture_profile_light_camera_serial.empty()
                ? "(none)"
                : ui_state->calibration_capture_profile_light_camera_serial.c_str());
    }
    if (light_output_mutation_locked) {
        ImGui::TextDisabled("Calibration prepare/restore actions are disabled while recording/finalizing.");
    } else if (light_handling_needs_mapped_strobe && light_control_camera_indices.empty()) {
        ImGui::TextDisabled("No open camera has an exposed nir_strobe_trigger output mapping.");
    } else if (light_handling_needs_mapped_strobe && !mapped_strobe_available) {
        ImGui::TextDisabled("Choose a light-control camera with an exposed nir_strobe_trigger output mapping.");
    } else {
        ImGui::TextDisabled(
            "Prepare applies selected light handling first, then temporarily sets camera timing to 10 FPS / 10 ms. All-camera prepare uses the same light-first ordering across open cameras.");
    }
    if (!ui_state->calibration_preflight_error.empty()) {
        ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.35f, 1.0f),
                           "%s",
                           ui_state->calibration_preflight_error.c_str());
    } else if (!ui_state->calibration_preflight_status.empty()) {
        ImGui::TextColored(ImVec4(0.35f, 0.85f, 0.45f, 1.0f),
                           "%s",
                           ui_state->calibration_preflight_status.c_str());
    }

    render_string_preset_combo(
        "Light state",
        &ui_state->calibration_light_state,
        kLightStatePresets,
        IM_ARRAYSIZE(kLightStatePresets));
    if (ui_state->calibration_image_set_purpose == "scale_image" &&
        ui_state->calibration_image_set_target_plane == "projected_surface") {
        ImGui::TextDisabled("Projection-surface scale images usually keep the TTL NIR strobe active so a clear ruler/target is visible to the camera.");
    } else if (ui_state->calibration_image_set_purpose == "scale_image") {
        ImGui::TextDisabled("Fish-plane scale images usually keep the TTL NIR strobe active so a ruler/target is visible to the camera.");
    } else if (ui_state->calibration_image_set_purpose == "arena_projection" ||
               ui_state->calibration_image_set_purpose == "homography_grid" ||
               ui_state->calibration_image_set_purpose == "crosshair_alignment") {
        ImGui::TextDisabled("Projector-pattern captures usually suppress mapped NIR strobe pulses and rely on the visible projection.");
    }
    render_string_preset_combo(
        "Illumination spectrum",
        &ui_state->calibration_illumination_spectrum,
        kIlluminationSpectrumPresets,
        IM_ARRAYSIZE(kIlluminationSpectrumPresets));
    ImGui::Checkbox(
        "Center wavelength nm",
        &ui_state->calibration_has_illumination_center_wavelength_nm);
    ImGui::SameLine();
    ImGui::BeginDisabled(!ui_state->calibration_has_illumination_center_wavelength_nm);
    ImGui::InputDouble(
        "##IlluminationCenterWavelengthNm",
        &ui_state->calibration_illumination_center_wavelength_nm,
        1.0,
        10.0,
        "%.1f");
    ImGui::EndDisabled();
    ImGui::Checkbox(
        "Min wavelength nm",
        &ui_state->calibration_has_illumination_min_wavelength_nm);
    ImGui::SameLine();
    ImGui::BeginDisabled(!ui_state->calibration_has_illumination_min_wavelength_nm);
    ImGui::InputDouble(
        "##IlluminationMinWavelengthNm",
        &ui_state->calibration_illumination_min_wavelength_nm,
        1.0,
        10.0,
        "%.1f");
    ImGui::EndDisabled();
    ImGui::Checkbox(
        "Max wavelength nm",
        &ui_state->calibration_has_illumination_max_wavelength_nm);
    ImGui::SameLine();
    ImGui::BeginDisabled(!ui_state->calibration_has_illumination_max_wavelength_nm);
    ImGui::InputDouble(
        "##IlluminationMaxWavelengthNm",
        &ui_state->calibration_illumination_max_wavelength_nm,
        1.0,
        10.0,
        "%.1f");
    ImGui::EndDisabled();
    ImGui::Checkbox(
        "Bandwidth FWHM nm",
        &ui_state->calibration_has_illumination_bandwidth_fwhm_nm);
    ImGui::SameLine();
    ImGui::BeginDisabled(!ui_state->calibration_has_illumination_bandwidth_fwhm_nm);
    ImGui::InputDouble(
        "##IlluminationBandwidthFwhmNm",
        &ui_state->calibration_illumination_bandwidth_fwhm_nm,
        1.0,
        10.0,
        "%.1f");
    ImGui::EndDisabled();
    render_string_preset_combo(
        "Wavelength confidence",
        &ui_state->calibration_illumination_wavelength_confidence,
        kIlluminationConfidencePresets,
        IM_ARRAYSIZE(kIlluminationConfidencePresets));
    ui_state->calibration_illumination_center_wavelength_nm =
        std::max(0.0, ui_state->calibration_illumination_center_wavelength_nm);
    ui_state->calibration_illumination_min_wavelength_nm =
        std::max(0.0, ui_state->calibration_illumination_min_wavelength_nm);
    ui_state->calibration_illumination_max_wavelength_nm =
        std::max(0.0, ui_state->calibration_illumination_max_wavelength_nm);
    ui_state->calibration_illumination_bandwidth_fwhm_nm =
        std::max(0.0, ui_state->calibration_illumination_bandwidth_fwhm_nm);
    render_string_preset_combo(
        "Projector state",
        &ui_state->calibration_projector_state,
        kProjectorStatePresets,
        IM_ARRAYSIZE(kProjectorStatePresets));
    ImGui::Checkbox(
        "Projector visible to camera",
        &ui_state->calibration_projector_visible_to_camera);
    ImGui::Checkbox(
        "Requires repeatable filter reinstall",
        &ui_state->calibration_requires_filter_reinstalled_repeatably);
    ImGui::InputTextMultiline(
        "Operator notes",
        &ui_state->calibration_operator_notes,
        ImVec2(-1.0f, ImGui::GetTextLineHeight() * 3.0f));

    ImGui::SeparatorText("Generic Calibration Image Set");
    static constexpr const char* kImageSetPurposePresets[] = {
        "arena_projection",
        "homography_grid",
        "scale_image",
        "crosshair_alignment"
    };
    static constexpr const char* kTargetPlanePresets[] = {
        "projected_surface",
        "tank_bottom_outer_surface",
        "tank_bottom_inner_surface",
        "estimated_fish_plane",
        "dish_top_rim",
        "unknown"
    };
    static constexpr const char* kImageRolePresets[] = {
        "projected_arena",
        "grid_on",
        "scale_target",
        "crosshair_on",
        "source"
    };
    if (ImGui::BeginCombo(
            "Image-set purpose",
            ui_state->calibration_image_set_purpose.empty()
                ? "homography_grid"
                : ui_state->calibration_image_set_purpose.c_str())) {
        for (const char* purpose : kImageSetPurposePresets) {
            const bool selected = ui_state->calibration_image_set_purpose == purpose;
            if (ImGui::Selectable(purpose, selected)) {
                if (actions.apply_calibration_image_set_purpose_defaults != nullptr) {
                    actions.apply_calibration_image_set_purpose_defaults(ui_state, purpose);
                } else {
                    ui_state->calibration_image_set_purpose = purpose;
                }
            }
            if (selected) {
                ImGui::SetItemDefaultFocus();
            }
        }
        ImGui::EndCombo();
    }
    render_string_preset_combo(
        "Target plane",
        &ui_state->calibration_image_set_target_plane,
        kTargetPlanePresets,
        IM_ARRAYSIZE(kTargetPlanePresets));
    render_string_preset_combo(
        "Image role",
        &ui_state->calibration_image_set_image_role,
        kImageRolePresets,
        IM_ARRAYSIZE(kImageRolePresets));
    ImGui::InputText(
        "Projected pattern ID",
        &ui_state->calibration_image_set_projected_pattern_id);
    ImGui::InputText(
        "Projected pattern type",
        &ui_state->calibration_image_set_projected_pattern_type);
    ImGui::InputText(
        "Scale target type",
        &ui_state->calibration_image_set_scale_target_type);
    ImGui::InputTextMultiline(
        "Image-set notes",
        &ui_state->calibration_image_set_notes,
        ImVec2(-1.0f, ImGui::GetTextLineHeight() * 2.0f));
    ImGui::TextDisabled(
        "Use this for piecewise homography/scale/crosshair image artifacts. "
        "It writes only the source image and image_set.json; Citrus fits and accepts later.");
}

} // namespace orange::gui::spatial_layout
