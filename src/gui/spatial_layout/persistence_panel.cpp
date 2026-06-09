#include "gui/spatial_layout/persistence_panel.h"

#include "gui/spatial_layout/session_io.h"
#include "imgui.h"

namespace orange::gui::spatial_layout {

SpatialLayoutPersistencePanelEvent render_spatial_layout_persistence_panel(
    SpatialLayoutUiState* ui_state,
    const SpatialLayoutPersistencePanelState& panel_state)
{
    if (ui_state == nullptr) {
        return SpatialLayoutPersistencePanelEvent::None;
    }

    SpatialLayoutPersistencePanelEvent event = SpatialLayoutPersistencePanelEvent::None;

    ImGui::SeparatorText("Persistence");
    ImGui::Text("Calibration session: %s",
                ui_state->calibration_session_id.empty()
                    ? "(not started; first save creates one)"
                    : ui_state->calibration_session_id.c_str());
    if (!ui_state->calibration_session_dir.empty()) {
        ImGui::TextDisabled("%s", ui_state->calibration_session_dir.c_str());
    }
    if (ImGui::Button("Start New Calibration Session")) {
        event = SpatialLayoutPersistencePanelEvent::StartNewCalibrationSession;
    }

    ImGui::BeginDisabled(!panel_state.can_save_top_rim_observation);
    if (ImGui::Button("Save Top-Rim Observation")) {
        event = SpatialLayoutPersistencePanelEvent::SaveTopRimObservation;
    }
    ImGui::EndDisabled();
    if (panel_state.top_rim_save_busy) {
        ImGui::TextDisabled("Top-rim observation save is running in the background.");
    }

    ImGui::SameLine();
    ImGui::BeginDisabled(!panel_state.can_save_generic_image_set);
    if (ImGui::Button("Save Calibration Image Set")) {
        event = SpatialLayoutPersistencePanelEvent::SaveCalibrationImageSet;
    }
    ImGui::EndDisabled();
    if (panel_state.generic_image_set_save_busy) {
        ImGui::TextDisabled("Calibration image-set save is running in the background.");
    }

    ImGui::SameLine();
    ImGui::BeginDisabled(!panel_state.can_save_group_image_sets);
    if (ImGui::Button("Save Group Calibration Image Sets")) {
        event = SpatialLayoutPersistencePanelEvent::SaveGroupCalibrationImageSets;
    }
    ImGui::EndDisabled();
    if (!ui_state->group_captures.empty()) {
        ImGui::TextDisabled(
            "Grouped save writes one image_set.json per captured camera and ties them with capture_group_id=%s.",
            ui_state->group_capture_id.c_str());
    }
    if (ui_state->has_capture && !panel_state.captured_in_full_resolution) {
        ImGui::TextDisabled(
            "Top-rim observations and calibration image sets require full-resolution camera coordinates. "
            "This live snapshot is preview/downsample space only.");
    }
    if (!panel_state.citrus_template_matches_selected_camera) {
        ImGui::TextDisabled(
            "Spatial calibration saves are blocked until the active Citrus template camera matches the selected Orange camera.");
    }

    ImGui::SameLine();
    ImGui::BeginDisabled(!panel_state.citrus_template_matches_selected_camera);
    if (ImGui::Button("Save Arena Layout Artifact")) {
        event = SpatialLayoutPersistencePanelEvent::SaveArenaLayoutArtifact;
    }
    ImGui::EndDisabled();

    ImGui::SameLine();
    if (ImGui::Button("Load Arena Layout Artifact...")) {
        event = SpatialLayoutPersistencePanelEvent::LoadArenaLayoutArtifact;
    }

    ImGui::TextDisabled(
        "Spatial calibration saves are grouped under calibrations/sessions/<session_id>/artifacts/<artifact_id>. Arena save writes %s, %s, %s, and %s.",
        kSpatialLayoutMeasurementFilename,
        kSpatialLayoutManifestFilename,
        kSpatialLayoutArenaLayoutRuntimeFilename,
        kSpatialLayoutDishMaskRuntimeFilename);

    ImGui::Separator();
    ImGui::Text("Preview valid: %s", ui_state->preview_valid ? "yes" : "no");

    if (ImGui::TreeNode("Canonical Layout JSON")) {
        if (ImGui::SmallButton("Copy canonical JSON")) {
            ImGui::SetClipboardText(ui_state->canonical_layout_json.c_str());
        }
        ImGui::BeginChild(
            "SpatialCanonicalJson",
            ImVec2(0.0f, 180.0f),
            true,
            ImGuiWindowFlags_HorizontalScrollbar);
        ImGui::TextUnformatted(ui_state->canonical_layout_json.c_str());
        ImGui::EndChild();
        ImGui::TreePop();
    }

    if (ImGui::TreeNode("Runtime Calibration JSON")) {
        if (ImGui::SmallButton("Copy runtime JSON")) {
            ImGui::SetClipboardText(ui_state->runtime_preview_json.c_str());
        }
        ImGui::BeginChild(
            "SpatialRuntimeJson",
            ImVec2(0.0f, 220.0f),
            true,
            ImGuiWindowFlags_HorizontalScrollbar);
        ImGui::TextUnformatted(ui_state->runtime_preview_json.c_str());
        ImGui::EndChild();
        ImGui::TreePop();
    }

    return event;
}

} // namespace orange::gui::spatial_layout
