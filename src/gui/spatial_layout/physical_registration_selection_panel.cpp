#include "gui/spatial_layout/physical_registration_selection_panel.h"

#include "gui/spatial_layout/physical_registration_selection.h"
#include "gui/spatial_layout/session_io.h"
#include "imgui.h"
#include "project.h"

#include <algorithm>
#include <filesystem>
#include <sstream>

namespace orange::gui::spatial_layout {
namespace {

PhysicalRegistrationArtifactCandidateUiState to_ui_candidate(
    const PhysicalRegistrationArtifactCandidate& candidate)
{
    PhysicalRegistrationArtifactCandidateUiState out;
    out.artifact_id = candidate.artifact_id;
    out.camera_serial = candidate.camera_serial;
    out.created_utc = candidate.created_utc;
    out.observation_path = candidate.observation_path.string();
    out.observation_sha256 = candidate.observation_sha256;
    out.dish_fill_state = candidate.dish_fill_state;
    out.physical_state_summary = candidate.physical_state_summary;
    out.width_px = candidate.width_px;
    out.height_px = candidate.height_px;
    out.accepted_center_x_px = candidate.accepted_center_x_px;
    out.accepted_center_y_px = candidate.accepted_center_y_px;
    out.accepted_radius_px = candidate.accepted_radius_px;
    out.centroid_gate_outset_px = candidate.centroid_gate_outset_px;
    out.operator_confirmed = candidate.operator_confirmed;
    out.compatible = candidate.compatible;
    out.compatibility_reason = candidate.compatibility_reason;
    return out;
}

PhysicalRegistrationArtifactCandidate from_ui_candidate(
    const PhysicalRegistrationArtifactCandidateUiState& candidate,
    const std::string& pixel_format)
{
    PhysicalRegistrationArtifactCandidate out;
    out.artifact_id = candidate.artifact_id;
    out.camera_serial = candidate.camera_serial;
    out.created_utc = candidate.created_utc;
    out.observation_path = candidate.observation_path;
    out.observation_sha256 = candidate.observation_sha256;
    out.dish_fill_state = candidate.dish_fill_state;
    out.physical_state_summary = candidate.physical_state_summary;
    out.pixel_format = pixel_format;
    out.width_px = candidate.width_px;
    out.height_px = candidate.height_px;
    out.accepted_center_x_px = candidate.accepted_center_x_px;
    out.accepted_center_y_px = candidate.accepted_center_y_px;
    out.accepted_radius_px = candidate.accepted_radius_px;
    out.centroid_gate_outset_px = candidate.centroid_gate_outset_px;
    out.operator_confirmed = candidate.operator_confirmed;
    out.compatible = candidate.compatible;
    out.compatibility_reason = candidate.compatibility_reason;
    return out;
}

void refresh(
    PhysicalRegistrationSelectionUiState* state,
    const CameraParams& camera,
    const std::string& artifact_root_dir)
{
    if (state == nullptr) return;
    const std::filesystem::path calibration_base =
        calibration_base_dir_from_artifact_root(artifact_root_dir);
    std::vector<std::string> warnings;
    const auto candidates = discover_physical_registration_artifacts(
        calibration_base,
        camera.camera_serial,
        static_cast<int>(camera.width),
        static_cast<int>(camera.height),
        camera.pixel_format,
        &warnings);
    state->candidates.clear();
    state->candidates.reserve(candidates.size());
    for (const auto& candidate : candidates) {
        state->candidates.push_back(to_ui_candidate(candidate));
    }
    const auto active = resolve_active_physical_registration(
        calibration_base,
        camera.camera_serial,
        static_cast<int>(camera.width),
        static_cast<int>(camera.height),
        camera.pixel_format);
    state->active_pointer = active.pointer;
    state->selected_candidate_index = -1;
    if (active.selected) {
        for (std::size_t index = 0; index < state->candidates.size(); ++index) {
            if (state->candidates[index].artifact_id ==
                    active.candidate.artifact_id &&
                state->candidates[index].observation_sha256 ==
                    active.candidate.observation_sha256) {
                state->selected_candidate_index = static_cast<int>(index);
                break;
            }
        }
    }
    state->loaded_camera_serial = camera.camera_serial;
    state->initialized = true;
    state->replace_selection_armed = false;
    state->clear_selection_armed = false;
    if (active.status == "invalid_pointer" ||
        active.status == "invalid_selected") {
        state->error = active.error;
        state->status = "Selected physical registration is invalid; it was not replaced automatically.";
    } else {
        state->error.clear();
        std::ostringstream status;
        status << "Found " << state->candidates.size()
               << " accepted physical registration artifact(s); active status="
               << active.status << ".";
        if (!warnings.empty()) {
            status << " Discovery warnings=" << warnings.size() << ".";
        }
        state->status = status.str();
    }
}

}  // namespace

void render_physical_registration_selection_panel(
    SpatialLayoutUiState* ui_state,
    const CameraParams& selected_camera,
    const std::string& artifact_root_dir,
    bool recording_mutation_locked)
{
    if (ui_state == nullptr) return;
    auto& state = ui_state->physical_registration_selection;
    if (!state.initialized ||
        state.loaded_camera_serial != selected_camera.camera_serial) {
        refresh(&state, selected_camera, artifact_root_dir);
    }

    ImGui::SeparatorText("Physical Registration Selection");
    ImGui::TextWrapped(
        "Accepted evidence and runtime selection are separate. Selecting atomically updates Orange's per-camera active pointer; clearing never deletes immutable artifacts.");
    if (ImGui::Button("Refresh Physical Registrations")) {
        refresh(&state, selected_camera, artifact_root_dir);
    }
    if (!state.status.empty()) ImGui::TextWrapped("%s", state.status.c_str());
    if (!state.error.empty()) {
        ImGui::TextColored(
            ImVec4(1.0f, 0.45f, 0.25f, 1.0f),
            "%s", state.error.c_str());
    }
    nlohmann::json active_selection = nlohmann::json::object();
    if (state.active_pointer.is_object()) {
        const auto selection_it = state.active_pointer.find("selection");
        if (selection_it != state.active_pointer.end() &&
            selection_it->is_object()) {
            active_selection = *selection_it;
        }
    }
    const std::string active_artifact = active_selection.is_object()
        ? active_selection.value("artifact_id", std::string())
        : std::string();
    ImGui::Text("Active: %s",
        active_artifact.empty() ? "not selected" : active_artifact.c_str());

    if (state.candidates.empty()) {
        ImGui::TextDisabled(
            "No accepted schema-v2 physical registration exists for this camera and native raster.");
    } else if (ImGui::BeginTable(
            "physical-registration-candidates", 7,
            ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("Choose");
        ImGui::TableSetupColumn("Created");
        ImGui::TableSetupColumn("Artifact");
        ImGui::TableSetupColumn("Raster");
        ImGui::TableSetupColumn("Accepted circle");
        ImGui::TableSetupColumn("Centroid outset");
        ImGui::TableSetupColumn("Compatibility");
        ImGui::TableHeadersRow();
        for (std::size_t index = 0; index < state.candidates.size(); ++index) {
            const auto& candidate = state.candidates[index];
            ImGui::PushID(static_cast<int>(index));
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::RadioButton(
                "##candidate", &state.selected_candidate_index,
                static_cast<int>(index));
            ImGui::TableSetColumnIndex(1);
            ImGui::TextUnformatted(candidate.created_utc.c_str());
            ImGui::TableSetColumnIndex(2);
            ImGui::TextWrapped("%s", candidate.artifact_id.c_str());
            ImGui::TableSetColumnIndex(3);
            ImGui::Text("%dx%d", candidate.width_px, candidate.height_px);
            ImGui::TableSetColumnIndex(4);
            ImGui::Text("(%.1f, %.1f) r=%.1f",
                candidate.accepted_center_x_px,
                candidate.accepted_center_y_px,
                candidate.accepted_radius_px);
            ImGui::TableSetColumnIndex(5);
            ImGui::Text("%.2f px", candidate.centroid_gate_outset_px);
            ImGui::TableSetColumnIndex(6);
            ImGui::TextColored(
                candidate.compatible
                    ? ImVec4(0.25f, 0.9f, 0.35f, 1.0f)
                    : ImVec4(1.0f, 0.45f, 0.25f, 1.0f),
                "%s", candidate.compatibility_reason.c_str());
            ImGui::PopID();
        }
        ImGui::EndTable();
    }

    const bool candidate_index_valid =
        state.selected_candidate_index >= 0 &&
        state.selected_candidate_index <
            static_cast<int>(state.candidates.size());
    const bool candidate_compatible = candidate_index_valid &&
        state.candidates[static_cast<std::size_t>(
            state.selected_candidate_index)].compatible;
    ImGui::Checkbox(
        "I intend to replace the active selection with this exact artifact",
        &state.replace_selection_armed);
    ImGui::BeginDisabled(
        recording_mutation_locked || !candidate_compatible ||
        !state.replace_selection_armed);
    if (ImGui::Button("Select Exact Physical Registration")) {
        const auto& selected = state.candidates[static_cast<std::size_t>(
            state.selected_candidate_index)];
        std::string error;
        if (!select_physical_registration_artifact(
                calibration_base_dir_from_artifact_root(artifact_root_dir),
                from_ui_candidate(selected, selected_camera.pixel_format),
                get_current_utc_timestamp(), &error)) {
            state.error = error;
        } else {
            refresh(&state, selected_camera, artifact_root_dir);
            state.status =
                "Selected exact physical registration and revalidated it from the active pointer.";
        }
    }
    ImGui::EndDisabled();

    ImGui::Checkbox(
        "I intend to clear the active selection without deleting evidence",
        &state.clear_selection_armed);
    ImGui::BeginDisabled(
        recording_mutation_locked || !state.clear_selection_armed);
    if (ImGui::Button("Clear Active Physical Registration")) {
        std::string error;
        if (!clear_active_physical_registration(
                calibration_base_dir_from_artifact_root(artifact_root_dir),
                selected_camera.camera_serial,
                get_current_utc_timestamp(), &error)) {
            state.error = error;
        } else {
            refresh(&state, selected_camera, artifact_root_dir);
            state.status =
                "Active physical registration cleared; immutable evidence remains available.";
        }
    }
    ImGui::EndDisabled();
}

}  // namespace orange::gui::spatial_layout
