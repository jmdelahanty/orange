#include "gui/spatial_layout/capture_panel.h"

#include "imgui.h"

#include <algorithm>
#include <cstdint>

namespace orange::gui::spatial_layout {
namespace {

ImVec2 fit_group_capture_image_size(
    int image_width,
    int image_height,
    const ImVec2& available,
    float max_height)
{
    if (image_width <= 0 || image_height <= 0) {
        return ImVec2(1.0f, 1.0f);
    }
    const float width = std::max(1.0f, static_cast<float>(image_width));
    const float height = std::max(1.0f, static_cast<float>(image_height));
    const float width_scale = available.x > 0.0f ? available.x / width : 1.0f;
    const float height_scale = max_height > 0.0f ? max_height / height : 1.0f;
    const float scale = std::min(1.0f, std::min(width_scale, height_scale));
    return ImVec2(std::max(1.0f, width * scale), std::max(1.0f, height * scale));
}

} // namespace

void render_group_capture_panels(
    SpatialLayoutUiState* ui_state,
    const CameraParams* cameras_params,
    int num_cameras,
    const GroupCapturePanelActions& actions)
{
    if (ui_state == nullptr || ui_state->group_captures.empty()) {
        return;
    }

    ImGui::SeparatorText("Grouped Captures");
    if (!ui_state->group_capture_status.empty()) {
        ImGui::TextWrapped("%s", ui_state->group_capture_status.c_str());
    }
    if (!ui_state->group_capture_error.empty()) {
        ImGui::TextColored(
            ImVec4(1.0f, 0.65f, 0.35f, 1.0f),
            "%s",
            ui_state->group_capture_error.c_str());
    }

    const float available_width = ImGui::GetContentRegionAvail().x;
    const int columns =
        std::clamp(static_cast<int>(available_width / 230.0f), 1, 4);
    if (!ImGui::BeginTable("SpatialGroupCapturePanels", columns, ImGuiTableFlags_SizingStretchSame)) {
        return;
    }

    for (size_t idx = 0; idx < ui_state->group_captures.size(); ++idx) {
        SpatialLayoutGroupCaptureFrame& capture = ui_state->group_captures[idx];
        ImGui::TableNextColumn();
        const std::string child_id =
            "SpatialGroupCapturePanel_" + capture.camera_serial;
        ImGui::BeginChild(child_id.c_str(), ImVec2(0.0f, 235.0f), true);
        ImGui::Text("Cam%s", capture.camera_serial.c_str());
        ImGui::TextDisabled(
            "%s / %s",
            capture.metadata.image_set_purpose.c_str(),
            capture.metadata.image_set_target_plane.c_str());
        ImGui::TextDisabled(
            "%s",
            capture.metadata.capture_stage.empty()
                ? "unknown stage"
                : capture.metadata.capture_stage.c_str());
        ImGui::TextDisabled(
            "%dx%d %s",
            capture.width,
            capture.height,
            capture.source_frame_count > 1 ? "mean" : "frame");
        if (capture.texture != 0 && capture.width > 0 && capture.height > 0) {
            const ImVec2 image_size =
                fit_group_capture_image_size(
                    capture.width,
                    capture.height,
                    ImGui::GetContentRegionAvail(),
                    145.0f);
            const float x_offset =
                std::max(0.0f, (ImGui::GetContentRegionAvail().x - image_size.x) * 0.5f);
            if (x_offset > 0.0f) {
                ImGui::SetCursorPosX(ImGui::GetCursorPosX() + x_offset);
            }
            ImGui::Image(
                (ImTextureID)(intptr_t)capture.texture,
                image_size,
                ImVec2(0, 0),
                ImVec2(1, 1));
        }
        const bool can_use =
            capture.valid &&
            capture.camera_index >= 0 &&
            capture.camera_index < num_cameras &&
            cameras_params != nullptr &&
            actions.use_group_capture_for_fit != nullptr;
        ImGui::BeginDisabled(!can_use);
        const std::string use_button = "Use for fit##" + capture.camera_serial;
        if (ImGui::SmallButton(use_button.c_str())) {
            std::string preview_error;
            if (!actions.use_group_capture_for_fit(
                    ui_state,
                    &capture,
                    cameras_params,
                    num_cameras,
                    &preview_error)) {
                ui_state->preview_error = preview_error;
            }
        }
        ImGui::EndDisabled();
        ImGui::EndChild();
    }
    ImGui::EndTable();
}

} // namespace orange::gui::spatial_layout
