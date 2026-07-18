#include "gui/spatial_layout/persistence_panel.h"

#include "gui/spatial_layout/session_capture_matrix.h"
#include "gui/spatial_layout/session_io.h"
#include "imgui.h"

#include <algorithm>
#include <vector>

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
    ImGui::SameLine();
    if (ImGui::Button("Load Calibration Session...")) {
        event = SpatialLayoutPersistencePanelEvent::LoadCalibrationSession;
    }

    if (!ui_state->loaded_calibration_session_index_path.empty()) {
        ImGui::TextDisabled("%s", ui_state->loaded_calibration_session_index_path.c_str());
    }
    render_session_capture_matrix_panel(ui_state);
    if (!ui_state->session_review_images.empty()) {
        ui_state->selected_session_review_image =
            std::clamp(
                ui_state->selected_session_review_image,
                0,
                static_cast<int>(ui_state->session_review_images.size()) - 1);
        std::vector<const char*> labels;
        labels.reserve(ui_state->session_review_images.size());
        for (const SpatialLayoutSessionReviewImage& image : ui_state->session_review_images) {
            labels.push_back(image.label.c_str());
        }
        ImGui::SetNextItemWidth(-1.0f);
        ImGui::Combo(
            "Session image",
            &ui_state->selected_session_review_image,
            labels.data(),
            static_cast<int>(labels.size()));
        ImGui::BeginDisabled(ui_state->selected_session_review_image < 0);
        if (ImGui::Button("Load Selected Session Image")) {
            event = SpatialLayoutPersistencePanelEvent::LoadSelectedSessionImage;
        }
        ImGui::EndDisabled();
        ImGui::TextDisabled(
            "Loaded session exposes %zu review image(s).",
            ui_state->session_review_images.size());
        if (!ui_state->session_review_camera_groups.empty() &&
            ImGui::TreeNode("Session Review Groups")) {
            for (size_t camera_group_index = 0;
                 camera_group_index < ui_state->session_review_camera_groups.size();
                 ++camera_group_index) {
                const SpatialLayoutSessionReviewCameraGroup& camera_group =
                    ui_state->session_review_camera_groups[camera_group_index];
                ImGui::PushID(static_cast<int>(camera_group_index));
                const bool open = ImGui::TreeNode(
                    "CameraGroup",
                    "%s",
                    camera_group.label.c_str());
                if (open) {
                    for (size_t plane_group_index = 0;
                         plane_group_index < camera_group.plane_groups.size();
                         ++plane_group_index) {
                        const SpatialLayoutSessionReviewPlaneGroup& plane_group =
                            camera_group.plane_groups[plane_group_index];
                        ImGui::PushID(static_cast<int>(plane_group_index));
                        const bool plane_open = ImGui::TreeNode(
                            "CalibrationLevel",
                            "%s%s (%zu image%s)",
                            plane_group.label.c_str(),
                            plane_group.has_linked_accepted_top_rim
                                ? " [linked top-rim]"
                                : "",
                            plane_group.image_indices.size(),
                            plane_group.image_indices.size() == 1 ? "" : "s");
                        if (plane_open) {
                            for (int image_index : plane_group.image_indices) {
                                if (image_index < 0 ||
                                    image_index >=
                                        static_cast<int>(ui_state->session_review_images.size())) {
                                    continue;
                                }
                                const SpatialLayoutSessionReviewImage& image =
                                    ui_state->session_review_images[
                                        static_cast<size_t>(image_index)];
                                ImGui::PushID(image_index);
                                if (ImGui::SmallButton("Select")) {
                                    ui_state->selected_session_review_image = image_index;
                                    event =
                                        SpatialLayoutPersistencePanelEvent::LoadSelectedSessionImage;
                                }
                                ImGui::SameLine();
                                ImGui::TextUnformatted(image.label.c_str());
                                ImGui::TextDisabled(
                                    "canvas=%s arena=%s stage=%s purpose=%s role=%s",
                                    image.canvas_id.empty() ? "unknown" : image.canvas_id.c_str(),
                                    image.arena_id.empty() ? "unknown" : image.arena_id.c_str(),
                                    image.capture_stage.empty() ? "unknown" : image.capture_stage.c_str(),
                                    image.purpose.empty() ? "unknown" : image.purpose.c_str(),
                                    image.role.empty() ? "unknown" : image.role.c_str());
                                ImGui::PopID();
                            }
                            ImGui::TreePop();
                        }
                        ImGui::PopID();
                    }
                    ImGui::TreePop();
                }
                ImGui::PopID();
            }
            ImGui::TreePop();
        }
        if (!ui_state->session_review_warnings.empty() &&
            ImGui::TreeNode("Session Review Warnings")) {
            for (const std::string& warning : ui_state->session_review_warnings) {
                ImGui::TextColored(
                    ImVec4(1.0f, 0.75f, 0.25f, 1.0f),
                    "%s",
                    warning.c_str());
            }
            ImGui::TreePop();
        }
    }

    ImGui::BeginDisabled(!panel_state.can_save_top_rim_observation);
    if (ImGui::Button("Save Top-Rim Observation")) {
        event = SpatialLayoutPersistencePanelEvent::SaveTopRimObservation;
    }
    ImGui::EndDisabled();
    if (panel_state.top_rim_save_busy) {
        ImGui::TextDisabled("Top-rim observation save is running in the background.");
    } else if (!ui_state->calibration_inner_rim_target_confirmed) {
        ImGui::TextDisabled(
            "Confirm the water-side inner-rim target in Capture Metadata to enable schema-v2 save.");
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
    ImGui::BeginDisabled(!panel_state.can_save_linked_arena_layouts);
    if (ImGui::Button("Save Linked Arena Layout Artifacts")) {
        event = SpatialLayoutPersistencePanelEvent::SaveLinkedArenaLayoutArtifacts;
    }
    ImGui::EndDisabled();

    ImGui::SameLine();
    if (ImGui::Button("Load Arena Layout Artifact...")) {
        event = SpatialLayoutPersistencePanelEvent::LoadArenaLayoutArtifact;
    }

    ImGui::TextDisabled(
        "Spatial calibration saves are grouped under calibrations/sessions/<session_id>/artifacts/. Arena saves write %s, %s, %s, and %s inside per-artifact folders; image sets use camera/arena aggregate folders.",
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
