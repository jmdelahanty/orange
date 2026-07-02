#include "gui/spatial_layout/layout_editor.h"

#include "gui/spatial_layout/layout_state.h"
#include "imgui.h"
#include "misc/cpp/imgui_stdlib.h"

#include <algorithm>
#include <sstream>
#include <string>
#include <vector>

namespace orange::gui::spatial_layout {
namespace {

using orange::spatial::ArenaLayoutZone;
using orange::spatial::LayoutGeometry;
using orange::spatial::LayoutGeometryType;
using orange::spatial::OrientationStatus;
using orange::spatial::RegistrationSource;
using orange::spatial::RegistrationType;

template <typename T>
T clamp_index(T value, T count)
{
    if (count <= 0) {
        return 0;
    }
    return std::clamp(value, static_cast<T>(0), static_cast<T>(count - 1));
}

}  // namespace

void render_layout_geometry_editor(const char* label_prefix, LayoutGeometry* geometry)
{
    if (geometry == nullptr) {
        return;
    }

    int geometry_type = geometry->type == LayoutGeometryType::kCircle ? 0 : 1;
    const char* geometry_items[] = {"circle", "rectangle"};
    if (ImGui::Combo((std::string(label_prefix) + " shape").c_str(), &geometry_type, geometry_items, IM_ARRAYSIZE(geometry_items))) {
        geometry->type = geometry_type == 0 ? LayoutGeometryType::kCircle : LayoutGeometryType::kRectangle;
    }

    if (geometry->type == LayoutGeometryType::kCircle) {
        ImGui::InputDouble((std::string(label_prefix) + " cx").c_str(), &geometry->circle.cx, 0.5);
        ImGui::InputDouble((std::string(label_prefix) + " cy").c_str(), &geometry->circle.cy, 0.5);
        ImGui::InputDouble((std::string(label_prefix) + " r").c_str(), &geometry->circle.r, 0.5);
        geometry->circle.r = std::max(0.0, geometry->circle.r);
    } else {
        ImGui::InputDouble((std::string(label_prefix) + " x").c_str(), &geometry->rectangle.x, 0.5);
        ImGui::InputDouble((std::string(label_prefix) + " y").c_str(), &geometry->rectangle.y, 0.5);
        ImGui::InputDouble((std::string(label_prefix) + " width").c_str(), &geometry->rectangle.width, 0.5);
        ImGui::InputDouble((std::string(label_prefix) + " height").c_str(), &geometry->rectangle.height, 0.5);
        geometry->rectangle.width = std::max(0.0, geometry->rectangle.width);
        geometry->rectangle.height = std::max(0.0, geometry->rectangle.height);
    }
}

void render_registration_editor(SpatialLayoutUiState* ui_state)
{
    const char* registration_type_items[] = {"identity", "translation", "similarity"};
    int registration_type = 2;
    if (ui_state->registration.type == RegistrationType::kIdentity) {
        registration_type = 0;
    } else if (ui_state->registration.type == RegistrationType::kTranslation) {
        registration_type = 1;
    }
    if (ImGui::Combo("Registration type", &registration_type, registration_type_items, IM_ARRAYSIZE(registration_type_items))) {
        ui_state->registration.type =
            registration_type == 0 ? RegistrationType::kIdentity :
            (registration_type == 1 ? RegistrationType::kTranslation : RegistrationType::kSimilarity);
    }

    const char* source_items[] = {"manual", "manual_fit", "detected_fit", "imported", "identity"};
    const RegistrationSource source_values[] = {
        RegistrationSource::kManual,
        RegistrationSource::kManualFit,
        RegistrationSource::kDetectedFit,
        RegistrationSource::kImported,
        RegistrationSource::kIdentity
    };
    int current_source = 1;
    for (int idx = 0; idx < IM_ARRAYSIZE(source_values); ++idx) {
        if (ui_state->registration.source == source_values[idx]) {
            current_source = idx;
            break;
        }
    }
    if (ImGui::Combo("Registration source", &current_source, source_items, IM_ARRAYSIZE(source_items))) {
        ui_state->registration.source = source_values[current_source];
    }

    const char* orientation_items[] = {"unknown", "trusted", "manual_confirmed", "ambiguous"};
    const OrientationStatus orientation_values[] = {
        OrientationStatus::kUnknown,
        OrientationStatus::kTrusted,
        OrientationStatus::kManualConfirmed,
        OrientationStatus::kAmbiguous
    };
    int current_orientation = 0;
    if (!ui_state->registration.has_orientation_status) {
        current_orientation = 0;
    } else {
        for (int idx = 0; idx < IM_ARRAYSIZE(orientation_values); ++idx) {
            if (ui_state->registration.orientation_status == orientation_values[idx]) {
                current_orientation = idx;
                break;
            }
        }
    }
    if (ImGui::Combo("Orientation status", &current_orientation, orientation_items, IM_ARRAYSIZE(orientation_items))) {
        ui_state->registration.has_orientation_status = true;
        ui_state->registration.orientation_status = orientation_values[current_orientation];
    }

    const bool translation_enabled =
        ui_state->registration.type == RegistrationType::kTranslation ||
        ui_state->registration.type == RegistrationType::kSimilarity;
    const bool similarity_enabled = ui_state->registration.type == RegistrationType::kSimilarity;

    ImGui::BeginDisabled(!translation_enabled);
    ImGui::InputDouble("Translate X (px)", &ui_state->registration_tx_px, 1.0, 20.0, "%.2f");
    ImGui::InputDouble("Translate Y (px)", &ui_state->registration_ty_px, 1.0, 20.0, "%.2f");
    ImGui::EndDisabled();

    ImGui::BeginDisabled(!similarity_enabled);
    ImGui::InputDouble("Scale (px / layout unit)", &ui_state->registration_scale, 0.05, 1.0, "%.4f");
    ImGui::InputDouble("Rotation CW (deg)", &ui_state->registration_rotation_deg_clockwise, 0.25, 2.0, "%.2f");
    ImGui::EndDisabled();
    ui_state->registration_scale = std::max(0.0001, ui_state->registration_scale);

    ImGui::InputInt("Fit point count", &ui_state->registration.fit_point_count);
    ui_state->registration.fit_point_count = std::max(0, ui_state->registration.fit_point_count);
    ImGui::InputDouble("Residual (px)", &ui_state->registration.residual_px, 0.1, 1.0, "%.3f");
    ui_state->registration.residual_px = std::max(0.0, ui_state->registration.residual_px);
}

void render_zone_editor(SpatialLayoutUiState* ui_state)
{
    if (ui_state == nullptr) {
        return;
    }

    if (ui_state->layout_artifact.layout.zones.empty()) {
        ImGui::TextDisabled("No zones yet.");
        if (ImGui::Button("Use experimental area as single zone")) {
            reset_to_single_experimental_area_zone(ui_state);
        }
        return;
    }

    sync_single_experimental_area_zone(ui_state);
    const bool single_experimental_area_zone = has_single_experimental_area_zone(*ui_state);
    if (single_experimental_area_zone) {
        ImGui::TextDisabled("Single-zone mode: zone 0 mirrors the experimental area.");
    } else if (ImGui::Button("Reset to single experimental-area zone")) {
        reset_to_single_experimental_area_zone(ui_state);
        return;
    }

    ui_state->selected_zone_index = clamp_index(
        ui_state->selected_zone_index,
        static_cast<int>(ui_state->layout_artifact.layout.zones.size()));

    std::vector<std::string> zone_labels_storage;
    std::vector<const char*> zone_labels;
    zone_labels_storage.reserve(ui_state->layout_artifact.layout.zones.size());
    zone_labels.reserve(ui_state->layout_artifact.layout.zones.size());
    for (const ArenaLayoutZone& zone : ui_state->layout_artifact.layout.zones) {
        std::ostringstream label;
        label << zone.zone_id;
        if (!zone.display_label.empty()) {
            label << " (" << zone.display_label << ")";
        }
        zone_labels_storage.push_back(label.str());
    }
    for (const std::string& label : zone_labels_storage) {
        zone_labels.push_back(label.c_str());
    }
    ImGui::Combo(
        "Selected zone",
        &ui_state->selected_zone_index,
        zone_labels.data(),
        static_cast<int>(zone_labels.size()));

    ArenaLayoutZone& zone =
        ui_state->layout_artifact.layout.zones[static_cast<size_t>(ui_state->selected_zone_index)];
    ImGui::BeginDisabled(single_experimental_area_zone);
    ImGui::InputText("Zone ID", &zone.zone_id);
    ImGui::Checkbox("Has zone index", &zone.has_zone_index);
    if (zone.has_zone_index) {
        ImGui::InputInt("Zone index", &zone.zone_index);
        zone.zone_index = std::max(0, zone.zone_index);
    }
    ImGui::InputText("Display label", &zone.display_label);
    render_layout_geometry_editor("Zone", &zone.geometry);
    ImGui::EndDisabled();

    if (ImGui::Button("Add zone")) {
        const int next_index = static_cast<int>(ui_state->layout_artifact.layout.zones.size());
        ui_state->layout_artifact.layout.zones.push_back(
            make_default_zone(ui_state->layout_artifact.layout.outer_geometry, next_index));
        ui_state->selected_zone_index = next_index;
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(single_experimental_area_zone);
    if (ImGui::Button("Remove zone") && !ui_state->layout_artifact.layout.zones.empty()) {
        ui_state->layout_artifact.layout.zones.erase(
            ui_state->layout_artifact.layout.zones.begin() + ui_state->selected_zone_index);
        ui_state->selected_zone_index = std::max(0, ui_state->selected_zone_index - 1);
    }
    ImGui::EndDisabled();
}

}  // namespace orange::gui::spatial_layout
