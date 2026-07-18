#include "gui/spatial_layout/preview_overlay.h"

#include "gui/spatial_layout/geometry.h"
#include "implot.h"

#include <algorithm>
#include <cmath>

namespace orange::gui::spatial_layout {
namespace {

using orange::spatial::ArenaLayoutArtifact;
using orange::spatial::ArenaLayoutZone;
using orange::spatial::ResolvedZoneOverlay;
using orange::spatial::RuntimeGeometry;
using orange::spatial::RuntimeGeometryType;

void draw_circle_geometry(const RuntimeGeometry& geometry, ImU32 color, float thickness)
{
    const ImPlotPoint center_point(geometry.circle.cx, geometry.circle.cy);
    const ImPlotPoint edge_point(geometry.circle.cx + geometry.circle.r, geometry.circle.cy);
    const ImVec2 center = ImPlot::PlotToPixels(center_point);
    const ImVec2 edge = ImPlot::PlotToPixels(edge_point);
    const float radius_px = std::abs(edge.x - center.x);
    ImPlot::GetPlotDrawList()->AddCircle(center, radius_px, color, 0, thickness);
}

void draw_oriented_rectangle_geometry(const RuntimeGeometry& geometry, ImU32 color, float thickness)
{
    const std::array<Point2d, 4> corners = oriented_rectangle_corners(geometry);
    ImDrawList* draw_list = ImPlot::GetPlotDrawList();
    for (size_t i = 0; i < corners.size(); ++i) {
        const Point2d& a = corners[i];
        const Point2d& b = corners[(i + 1) % corners.size()];
        draw_list->AddLine(
            ImPlot::PlotToPixels(ImPlotPoint(a.x, a.y)),
            ImPlot::PlotToPixels(ImPlotPoint(b.x, b.y)),
            color,
            thickness);
    }
}

void draw_runtime_geometry(const RuntimeGeometry& geometry, ImU32 color, float thickness)
{
    if (geometry.type == RuntimeGeometryType::kCircle) {
        draw_circle_geometry(geometry, color, thickness);
        return;
    }
    draw_oriented_rectangle_geometry(geometry, color, thickness);
}

void draw_hough_proposal_overlay(const RuntimeGeometry& geometry)
{
    if (geometry.type != RuntimeGeometryType::kCircle || geometry.circle.r <= 0.0) {
        return;
    }

    const ImU32 color = IM_COL32(255, 30, 190, 255);
    draw_circle_geometry(geometry, color, 3.2f);

    const ImVec2 center = ImPlot::PlotToPixels(
        ImPlotPoint(geometry.circle.cx, geometry.circle.cy));
    ImDrawList* draw_list = ImPlot::GetPlotDrawList();
    constexpr float cross = 9.0f;
    draw_list->AddLine(
        ImVec2(center.x - cross, center.y),
        ImVec2(center.x + cross, center.y),
        color,
        2.2f);
    draw_list->AddLine(
        ImVec2(center.x, center.y - cross),
        ImVec2(center.x, center.y + cross),
        color,
        2.2f);
    draw_list->AddText(
        ImVec2(center.x + 10.0f, center.y - 20.0f),
        color,
        "Hough");
}

void draw_projected_outline_polyline(const std::vector<Point2d>& camera_points,
                                     ImU32 color,
                                     float thickness)
{
    if (camera_points.size() < 3) {
        return;
    }
    ImDrawList* draw_list = ImPlot::GetPlotDrawList();
    for (size_t idx = 0; idx < camera_points.size(); ++idx) {
        const Point2d& a = camera_points[idx];
        const Point2d& b = camera_points[(idx + 1) % camera_points.size()];
        draw_list->AddLine(
            ImPlot::PlotToPixels(ImPlotPoint(a.x, a.y)),
            ImPlot::PlotToPixels(ImPlotPoint(b.x, b.y)),
            color,
            thickness);
    }
}

void draw_citrus_projected_outline_overlay(const std::vector<Point2d>& camera_points,
                                           const RuntimeGeometry& fitted_geometry,
                                           ImU32 color,
                                           const char* label)
{
    if (camera_points.size() < 3 ||
        fitted_geometry.type != RuntimeGeometryType::kCircle ||
        fitted_geometry.circle.r <= 0.0) {
        return;
    }

    draw_projected_outline_polyline(camera_points, color, 2.0f);

    const ImVec2 center = ImPlot::PlotToPixels(
        ImPlotPoint(fitted_geometry.circle.cx, fitted_geometry.circle.cy));
    ImDrawList* draw_list = ImPlot::GetPlotDrawList();
    constexpr float marker = 9.0f;
    const ImVec2 p0(center.x, center.y - marker);
    const ImVec2 p1(center.x - marker * 0.866f, center.y + marker * 0.5f);
    const ImVec2 p2(center.x + marker * 0.866f, center.y + marker * 0.5f);
    draw_list->AddTriangleFilled(p0, p1, p2, color);
    draw_list->AddTriangle(p0, p1, p2, IM_COL32(10, 20, 35, 240), 1.5f);
    draw_list->AddText(
        ImVec2(center.x + 10.0f, center.y + 6.0f),
        color,
        label);
}

bool compute_citrus_corrected_outline_overlay(
    const SpatialLayoutUiState& ui_state,
    Point2d* corrected_center_camera_out,
    std::vector<Point2d>* corrected_outline_camera_out)
{
    if (!ui_state.has_detected_experimental_area_circle ||
        ui_state.detected_experimental_area_geometry.type != RuntimeGeometryType::kCircle ||
        !ui_state.citrus_template.available ||
        !ui_state.citrus_template.has_camera_to_canvas_homography ||
        !ui_state.citrus_template.has_canvas_to_camera_homography ||
        !ui_state.citrus_template.has_arena_canvas_region) {
        return false;
    }

    const Point2d observed_center_camera = make_point(
        ui_state.detected_experimental_area_geometry.circle.cx,
        ui_state.detected_experimental_area_geometry.circle.cy);
    Point2d observed_center_canvas{};
    if (!transform_point_projective(
            ui_state.citrus_template.camera_to_canvas_homography,
            observed_center_camera,
            &observed_center_canvas)) {
        return false;
    }

    const Point2d corrected_center_arena_relative =
        citrus_canvas_to_arena_relative_px(
            ui_state.citrus_template,
            observed_center_canvas);
    if (!sample_citrus_experimental_area_outline_in_camera_px(
            ui_state.citrus_template,
            corrected_center_arena_relative,
            corrected_outline_camera_out,
            nullptr)) {
        return false;
    }

    const Point2d corrected_center_canvas =
        citrus_arena_relative_to_canvas_px(
            ui_state.citrus_template,
            corrected_center_arena_relative);
    Point2d corrected_center_camera{};
    if (!transform_point_projective(
            ui_state.citrus_template.canvas_to_camera_homography,
            corrected_center_canvas,
            &corrected_center_camera)) {
        return false;
    }
    if (corrected_center_camera_out != nullptr) {
        *corrected_center_camera_out = corrected_center_camera;
    }
    return true;
}

void draw_citrus_corrected_center_overlay(
    const RuntimeGeometry& current_citrus_geometry,
    const Point2d& corrected_center_camera,
    const std::vector<Point2d>& corrected_outline_camera)
{
    if (corrected_outline_camera.size() < 3) {
        return;
    }

    const ImU32 color = IM_COL32(70, 255, 150, 255);
    const ImVec2 corrected = ImPlot::PlotToPixels(
        ImPlotPoint(corrected_center_camera.x, corrected_center_camera.y));
    ImDrawList* draw_list = ImPlot::GetPlotDrawList();

    for (size_t idx = 0; idx < corrected_outline_camera.size(); ++idx) {
        const Point2d& a = corrected_outline_camera[idx];
        const Point2d& b = corrected_outline_camera[(idx + 1) % corrected_outline_camera.size()];
        draw_list->AddLine(
            ImPlot::PlotToPixels(ImPlotPoint(a.x, a.y)),
            ImPlot::PlotToPixels(ImPlotPoint(b.x, b.y)),
            color,
            2.6f);
    }

    if (current_citrus_geometry.type == RuntimeGeometryType::kCircle &&
        current_citrus_geometry.circle.r > 0.0) {
        const ImVec2 current = ImPlot::PlotToPixels(
            ImPlotPoint(current_citrus_geometry.circle.cx, current_citrus_geometry.circle.cy));
        draw_list->AddLine(current, corrected, color, 2.0f);
        draw_list->AddCircleFilled(current, 3.0f, color, 12);
    }

    constexpr float marker = 8.0f;
    const ImVec2 p0(corrected.x, corrected.y - marker);
    const ImVec2 p1(corrected.x + marker, corrected.y);
    const ImVec2 p2(corrected.x, corrected.y + marker);
    const ImVec2 p3(corrected.x - marker, corrected.y);
    draw_list->AddQuadFilled(p0, p1, p2, p3, color);
    draw_list->AddQuad(p0, p1, p2, p3, IM_COL32(10, 35, 20, 240), 1.5f);
    draw_list->AddText(
        ImVec2(corrected.x + 10.0f, corrected.y - 4.0f),
        color,
        "Corrected");
}

void draw_zone_label(const ResolvedZoneOverlay& zone, const ArenaLayoutArtifact& artifact, ImU32 color)
{
    std::string label = zone.zone_id;
    for (const ArenaLayoutZone& canonical_zone : artifact.layout.zones) {
        if (canonical_zone.zone_id == zone.zone_id && !canonical_zone.display_label.empty()) {
            label = canonical_zone.display_label;
            break;
        }
    }

    Point2d center{};
    if (zone.geometry.type == RuntimeGeometryType::kCircle) {
        center = make_point(zone.geometry.circle.cx, zone.geometry.circle.cy);
    } else {
        center = make_point(zone.geometry.oriented_rectangle.cx, zone.geometry.oriented_rectangle.cy);
    }
    ImPlot::GetPlotDrawList()->AddText(
        ImPlot::PlotToPixels(ImPlotPoint(center.x, center.y)),
        color,
        label.c_str());
}

} // namespace

bool draw_runtime_preview(
    SpatialLayoutUiState* ui_state,
    const PreviewOverlayActions& actions)
{
    if (ui_state == nullptr || ui_state->captured_texture == 0 ||
        ui_state->captured_texture_width <= 0 || ui_state->captured_texture_height <= 0) {
        return false;
    }

    if (!orange::ui::begin_image_canvas(
            "Spatial Layout View",
            ui_state->captured_texture,
            ui_state->captured_texture_width,
            ui_state->captured_texture_height,
            &ui_state->captured_canvas_view,
            0.62f)) {
        return false;
    }

    bool changed = false;
    ImPlot::PushPlotClipRect();
    if (ui_state->has_citrus_projected_circle &&
        (ui_state->citrus_template.source_camera_id.empty() ||
         ui_state->captured_camera_serial.empty() ||
         ui_state->citrus_template.source_camera_id == ui_state->captured_camera_serial)) {
        draw_citrus_projected_outline_overlay(
            ui_state->citrus_projected_outline_camera_points,
            ui_state->citrus_projected_circle_geometry,
            IM_COL32(100, 190, 255, 230),
            "Citrus experimental area");
    }
    if (ui_state->has_citrus_projected_fit_ring &&
        (ui_state->citrus_template.source_camera_id.empty() ||
         ui_state->captured_camera_serial.empty() ||
         ui_state->citrus_template.source_camera_id ==
             ui_state->captured_camera_serial)) {
        draw_citrus_projected_outline_overlay(
            ui_state->citrus_projected_fit_ring_outline_camera_points,
            ui_state->citrus_projected_fit_ring_geometry,
            IM_COL32(185, 120, 255, 220),
            "Citrus fit ring");
    }
    draw_runtime_geometry(
        ui_state->dish_mask_runtime.geometry.outer_geometry,
        IM_COL32(255, 165, 0, 230),
        2.0f);
    draw_runtime_geometry(
        ui_state->dish_mask_runtime.geometry.valid_geometry,
        IM_COL32(255, 220, 70, 210),
        2.0f);

    for (size_t idx = 0; idx < ui_state->arena_layout_runtime.zones.size(); ++idx) {
        const ResolvedZoneOverlay& zone = ui_state->arena_layout_runtime.zones[idx];
        const bool selected = static_cast<int>(idx) == ui_state->selected_zone_index;
        const ImU32 color = selected
            ? IM_COL32(90, 235, 255, 255)
            : IM_COL32(40, 220, 120, 235);
        const float thickness = selected ? 2.6f : 1.8f;
        draw_runtime_geometry(zone.geometry, color, thickness);
        draw_zone_label(zone, ui_state->layout_artifact, color);
    }
    if (ui_state->show_hough_proposal_overlay &&
        ui_state->has_detected_experimental_area_circle) {
        draw_hough_proposal_overlay(ui_state->detected_experimental_area_geometry);
    }
    if (ui_state->show_citrus_corrected_center_overlay &&
        ui_state->has_detected_experimental_area_circle &&
        ui_state->has_citrus_projected_circle) {
        Point2d corrected_center_camera{};
        std::vector<Point2d> corrected_outline_camera;
        if (compute_citrus_corrected_outline_overlay(
                *ui_state,
                &corrected_center_camera,
                &corrected_outline_camera)) {
            draw_citrus_corrected_center_overlay(
                ui_state->citrus_projected_circle_geometry,
                corrected_center_camera,
                corrected_outline_camera);
        }
    }
    ImPlot::PopPlotClipRect();

    if (ui_state->canvas_edit_mode == 0) {
        changed =
            (actions.handle_registration_canvas_edit != nullptr &&
             actions.handle_registration_canvas_edit(ui_state)) ||
            changed;
    } else {
        changed =
            (actions.handle_selected_zone_canvas_edit != nullptr &&
             actions.handle_selected_zone_canvas_edit(ui_state)) ||
            changed;
    }
    ImPlot::EndPlot();
    return changed;
}

} // namespace orange::gui::spatial_layout
