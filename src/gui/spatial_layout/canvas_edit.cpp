#include "gui/spatial_layout/canvas_edit.h"

#include "gui/spatial_layout/geometry.h"
#include "gui/spatial_layout/layout_state.h"
#include "imgui.h"
#include "implot.h"

#include <algorithm>
#include <cmath>

namespace orange::gui::spatial_layout {
namespace {

using orange::spatial::ArenaLayoutZone;
using orange::spatial::LayoutGeometry;
using orange::spatial::LayoutGeometryType;
using orange::spatial::OrientationStatus;
using orange::spatial::RegistrationSource;
using orange::spatial::RegistrationType;
using orange::spatial::ResolvedZoneOverlay;
using orange::spatial::RuntimeGeometry;
using orange::spatial::RuntimeGeometryType;

constexpr double kPi = 3.14159265358979323846;

double effective_registration_scale(const SpatialLayoutUiState& ui_state)
{
    if (ui_state.registration.type == RegistrationType::kIdentity ||
        ui_state.registration.type == RegistrationType::kTranslation) {
        return 1.0;
    }
    return std::max(1e-6, ui_state.registration_scale);
}

double effective_registration_rotation_deg(const SpatialLayoutUiState& ui_state)
{
    return ui_state.registration.type == RegistrationType::kSimilarity
        ? ui_state.registration_rotation_deg_clockwise
        : 0.0;
}

Point2d runtime_geometry_center(const RuntimeGeometry& geometry)
{
    if (geometry.type == RuntimeGeometryType::kCircle) {
        return make_point(geometry.circle.cx, geometry.circle.cy);
    }
    return make_point(geometry.oriented_rectangle.cx, geometry.oriented_rectangle.cy);
}

Point2d point_from_local_offset(
    const Point2d& center,
    double local_x,
    double local_y,
    double rotation_deg_clockwise)
{
    const double theta = rotation_deg_clockwise * kPi / 180.0;
    const double cos_theta = std::cos(theta);
    const double sin_theta = std::sin(theta);
    return make_point(
        center.x + cos_theta * local_x - sin_theta * local_y,
        center.y + sin_theta * local_x + cos_theta * local_y);
}

Point2d runtime_geometry_right_handle(const RuntimeGeometry& geometry)
{
    const Point2d center = runtime_geometry_center(geometry);
    if (geometry.type == RuntimeGeometryType::kCircle) {
        return make_point(geometry.circle.cx + geometry.circle.r, geometry.circle.cy);
    }
    return point_from_local_offset(
        center,
        geometry.oriented_rectangle.width * 0.5,
        0.0,
        geometry.oriented_rectangle.rotation_deg_clockwise);
}

Point2d runtime_geometry_bottom_handle(const RuntimeGeometry& geometry)
{
    const Point2d center = runtime_geometry_center(geometry);
    if (geometry.type == RuntimeGeometryType::kCircle) {
        return make_point(geometry.circle.cx, geometry.circle.cy + geometry.circle.r);
    }
    return point_from_local_offset(
        center,
        0.0,
        geometry.oriented_rectangle.height * 0.5,
        geometry.oriented_rectangle.rotation_deg_clockwise);
}

Point2d runtime_geometry_rotation_handle(const RuntimeGeometry& geometry)
{
    const Point2d center = runtime_geometry_center(geometry);
    Point2d top_boundary = center;
    if (geometry.type == RuntimeGeometryType::kCircle) {
        top_boundary = make_point(geometry.circle.cx, geometry.circle.cy - geometry.circle.r);
    } else {
        top_boundary = point_from_local_offset(
            center,
            0.0,
            -geometry.oriented_rectangle.height * 0.5,
            geometry.oriented_rectangle.rotation_deg_clockwise);
    }

    const ImVec2 center_px = ImPlot::PlotToPixels(ImPlotPoint(center.x, center.y));
    const ImVec2 top_px = ImPlot::PlotToPixels(ImPlotPoint(top_boundary.x, top_boundary.y));
    ImVec2 direction(top_px.x - center_px.x, top_px.y - center_px.y);
    const float length = std::sqrt(direction.x * direction.x + direction.y * direction.y);
    if (length < 1e-3f) {
        direction = ImVec2(0.0f, -1.0f);
    } else {
        direction.x /= length;
        direction.y /= length;
    }
    const ImVec2 handle_px(
        center_px.x + direction.x * (length + 28.0f),
        center_px.y + direction.y * (length + 28.0f));
    const ImPlotPoint handle_plot = ImPlot::PixelsToPlot(handle_px);
    return make_point(handle_plot.x, handle_plot.y);
}

Point2d camera_point_to_layout_point(
    const SpatialLayoutUiState& ui_state,
    const Point2d& camera_point)
{
    return transform_point(
        ui_state.registration.camera_to_layout_matrix,
        camera_point.x,
        camera_point.y);
}

bool update_drag_point(int id,
                       const Point2d& initial_point,
                       const ImVec4& color,
                       float radius_px,
                       Point2d* updated_point)
{
    if (updated_point == nullptr) {
        return false;
    }

    double x = initial_point.x;
    double y = initial_point.y;
    const bool changed = ImPlot::DragPoint(
        id,
        &x,
        &y,
        color,
        radius_px,
        ImPlotDragToolFlags_NoFit);
    *updated_point = make_point(x, y);
    return changed;
}

}  // namespace

bool handle_registration_canvas_edit(SpatialLayoutUiState* ui_state)
{
    if (ui_state == nullptr || !ui_state->dish_mask_runtime.has_geometry) {
        return false;
    }

    const RuntimeGeometry& outer = ui_state->dish_mask_runtime.geometry.outer_geometry;
    const LayoutGeometry& canonical_outer = ui_state->layout_artifact.layout.outer_geometry;
    const Point2d current_center = runtime_geometry_center(outer);
    const double current_scale = effective_registration_scale(*ui_state);
    const double current_rotation_deg = effective_registration_rotation_deg(*ui_state);

    bool changed = false;
    Point2d center_handle{};
    if (update_drag_point(4100, current_center, ImVec4(0.25f, 0.80f, 1.0f, 1.0f), 6.0f, &center_handle)) {
        const RegistrationType new_type =
            ui_state->registration.type == RegistrationType::kIdentity
                ? RegistrationType::kTranslation
                : ui_state->registration.type;
        set_registration_transform(
            ui_state,
            new_type,
            center_handle,
            current_scale,
            current_rotation_deg,
            RegistrationSource::kManualFit);
        ui_state->registration.fit_point_count = 0;
        ui_state->registration.residual_px = 0.0;
        changed = true;
    }

    if (ui_state->registration.type != RegistrationType::kSimilarity) {
        return changed;
    }

    Point2d scale_handle{};
    if (update_drag_point(
            4101,
            runtime_geometry_right_handle(outer),
            ImVec4(1.0f, 0.82f, 0.18f, 1.0f),
            5.0f,
            &scale_handle)) {
        double new_scale = current_scale;
        if (canonical_outer.type == LayoutGeometryType::kCircle && canonical_outer.circle.r > 0.0) {
            const double dx = scale_handle.x - current_center.x;
            const double dy = scale_handle.y - current_center.y;
            new_scale = std::max(1e-6, std::sqrt(dx * dx + dy * dy) / canonical_outer.circle.r);
        } else if (canonical_outer.type == LayoutGeometryType::kRectangle && canonical_outer.rectangle.width > 0.0) {
            const Point2d layout_center = layout_geometry_center(canonical_outer);
            const Point2d mouse_layout = camera_point_to_layout_point(*ui_state, scale_handle);
            new_scale = std::max(
                1e-6,
                (2.0 * std::abs(mouse_layout.x - layout_center.x)) / canonical_outer.rectangle.width);
        }
        set_registration_transform(
            ui_state,
            RegistrationType::kSimilarity,
            current_center,
            new_scale,
            current_rotation_deg,
            RegistrationSource::kManualFit);
        ui_state->registration.fit_point_count = 0;
        ui_state->registration.residual_px = 0.0;
        changed = true;
    }

    Point2d rotation_handle{};
    if (update_drag_point(
            4102,
            runtime_geometry_rotation_handle(outer),
            ImVec4(1.0f, 0.35f, 0.80f, 1.0f),
            5.0f,
            &rotation_handle)) {
        const double dx = rotation_handle.x - current_center.x;
        const double dy = rotation_handle.y - current_center.y;
        if ((dx * dx + dy * dy) > 1e-8) {
            const double angle_deg = std::atan2(dy, dx) * 180.0 / kPi;
            const double new_rotation_deg = normalize_angle_deg(angle_deg + 90.0);
            set_registration_transform(
                ui_state,
                RegistrationType::kSimilarity,
                current_center,
                current_scale,
                new_rotation_deg,
                RegistrationSource::kManualFit);
            ui_state->registration.fit_point_count = 0;
            ui_state->registration.residual_px = 0.0;
            ui_state->registration.has_orientation_status = true;
            ui_state->registration.orientation_status = OrientationStatus::kManualConfirmed;
            changed = true;
        }
    }

    return changed;
}

bool handle_selected_zone_canvas_edit(SpatialLayoutUiState* ui_state)
{
    if (ui_state == nullptr ||
        !ui_state->registration.has_camera_to_layout_matrix ||
        ui_state->selected_zone_index < 0 ||
        ui_state->selected_zone_index >= static_cast<int>(ui_state->layout_artifact.layout.zones.size()) ||
        ui_state->selected_zone_index >= static_cast<int>(ui_state->arena_layout_runtime.zones.size())) {
        return false;
    }

    ArenaLayoutZone& zone =
        ui_state->layout_artifact.layout.zones[static_cast<size_t>(ui_state->selected_zone_index)];
    const ResolvedZoneOverlay& overlay =
        ui_state->arena_layout_runtime.zones[static_cast<size_t>(ui_state->selected_zone_index)];
    const Point2d current_center = runtime_geometry_center(overlay.geometry);
    bool changed = false;

    Point2d center_handle{};
    if (update_drag_point(4200, current_center, ImVec4(0.15f, 0.95f, 0.55f, 1.0f), 6.0f, &center_handle)) {
        const Point2d layout_center = camera_point_to_layout_point(*ui_state, center_handle);
        if (zone.geometry.type == LayoutGeometryType::kCircle) {
            zone.geometry.circle.cx = layout_center.x;
            zone.geometry.circle.cy = layout_center.y;
        } else {
            zone.geometry.rectangle.x = layout_center.x - zone.geometry.rectangle.width * 0.5;
            zone.geometry.rectangle.y = layout_center.y - zone.geometry.rectangle.height * 0.5;
        }
        changed = true;
    }

    const double current_scale = std::max(1e-6, effective_registration_scale(*ui_state));
    if (zone.geometry.type == LayoutGeometryType::kCircle) {
        Point2d radius_handle{};
        if (update_drag_point(
                4201,
                runtime_geometry_right_handle(overlay.geometry),
                ImVec4(0.95f, 0.95f, 0.25f, 1.0f),
                5.0f,
                &radius_handle)) {
            const double dx = radius_handle.x - current_center.x;
            const double dy = radius_handle.y - current_center.y;
            zone.geometry.circle.r = std::max(0.0, std::sqrt(dx * dx + dy * dy) / current_scale);
            changed = true;
        }
        return changed;
    }

    const Point2d layout_center = layout_geometry_center(zone.geometry);
    Point2d width_handle{};
    if (update_drag_point(
            4202,
            runtime_geometry_right_handle(overlay.geometry),
            ImVec4(0.95f, 0.95f, 0.25f, 1.0f),
            5.0f,
            &width_handle)) {
        const Point2d layout_handle = camera_point_to_layout_point(*ui_state, width_handle);
        zone.geometry.rectangle.width = std::max(0.0, 2.0 * std::abs(layout_handle.x - layout_center.x));
        zone.geometry.rectangle.x = layout_center.x - zone.geometry.rectangle.width * 0.5;
        changed = true;
    }

    Point2d height_handle{};
    if (update_drag_point(
            4203,
            runtime_geometry_bottom_handle(overlay.geometry),
            ImVec4(0.95f, 0.60f, 0.25f, 1.0f),
            5.0f,
            &height_handle)) {
        const Point2d layout_handle = camera_point_to_layout_point(*ui_state, height_handle);
        zone.geometry.rectangle.height = std::max(0.0, 2.0 * std::abs(layout_handle.y - layout_center.y));
        zone.geometry.rectangle.y = layout_center.y - zone.geometry.rectangle.height * 0.5;
        changed = true;
    }

    return changed;
}

}  // namespace orange::gui::spatial_layout
