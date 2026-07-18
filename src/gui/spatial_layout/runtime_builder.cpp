#include "gui/spatial_layout/runtime_builder.h"

#include "gui/spatial_layout/geometry.h"
#include "gui/spatial_layout/layout_state.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <sstream>
#include <utility>

namespace orange::gui::spatial_layout {
namespace {

using orange::spatial::ArenaLayoutZone;
using orange::spatial::CalibrationRef;
using orange::spatial::CoordinateSpace;
using orange::spatial::LayoutGeometry;
using orange::spatial::LayoutGeometryType;
using orange::spatial::ObservationSource;
using orange::spatial::RegistrationSource;
using orange::spatial::RegistrationType;
using orange::spatial::ResolvedZoneOverlay;
using orange::spatial::RuntimeGeometry;
using orange::spatial::RuntimeGeometryType;
using orange::spatial::ViewRegistration;
using orange::spatial::VisibilityStatus;

constexpr double kPi = 3.14159265358979323846;

RuntimeGeometry offset_runtime_geometry(const RuntimeGeometry& geometry, double radial_offset_px)
{
    if (geometry.type == RuntimeGeometryType::kCircle) {
        return runtime_circle(
            geometry.circle.cx,
            geometry.circle.cy,
            std::max(0.0, geometry.circle.r + radial_offset_px));
    }

    return runtime_oriented_rectangle(
        geometry.oriented_rectangle.cx,
        geometry.oriented_rectangle.cy,
        std::max(0.0, geometry.oriented_rectangle.width + 2.0 * radial_offset_px),
        std::max(0.0, geometry.oriented_rectangle.height + 2.0 * radial_offset_px),
        geometry.oriented_rectangle.rotation_deg_clockwise);
}

bool point_inside_image(const Point2d& point, int image_width, int image_height)
{
    return point.x >= 0.0 && point.x <= static_cast<double>(image_width) &&
           point.y >= 0.0 && point.y <= static_cast<double>(image_height);
}

}  // namespace

void apply_view_registration_to_editor_state(
    SpatialLayoutUiState* ui_state,
    const ViewRegistration& registration)
{
    if (ui_state == nullptr) {
        return;
    }

    ui_state->registration = registration;
    ui_state->registration.layout_coordinate_space = ui_state->layout_artifact.layout.coordinate_space;
    ui_state->registration_tx_px = registration.layout_to_camera_matrix[2];
    ui_state->registration_ty_px = registration.layout_to_camera_matrix[5];

    if (registration.type == RegistrationType::kSimilarity) {
        ui_state->registration_scale = std::max(
            1e-6,
            std::sqrt(registration.layout_to_camera_matrix[0] * registration.layout_to_camera_matrix[0] +
                      registration.layout_to_camera_matrix[3] * registration.layout_to_camera_matrix[3]));
        ui_state->registration_rotation_deg_clockwise = normalize_angle_deg(
            std::atan2(registration.layout_to_camera_matrix[3], registration.layout_to_camera_matrix[0]) * 180.0 / kPi);
    } else {
        ui_state->registration_scale = 1.0;
        ui_state->registration_rotation_deg_clockwise = 0.0;
    }
}

RuntimeGeometry transform_layout_geometry(
    const LayoutGeometry& layout_geometry,
    const std::array<double, 9>& layout_to_camera_matrix,
    double rotation_deg_clockwise)
{
    if (layout_geometry.type == LayoutGeometryType::kCircle) {
        const Point2d center = transform_point(layout_to_camera_matrix, layout_geometry.circle.cx, layout_geometry.circle.cy);
        const double scale =
            std::sqrt(layout_to_camera_matrix[0] * layout_to_camera_matrix[0] +
                      layout_to_camera_matrix[3] * layout_to_camera_matrix[3]);
        return runtime_circle(center.x, center.y, std::abs(scale) * layout_geometry.circle.r);
    }

    const double center_x = layout_geometry.rectangle.x + layout_geometry.rectangle.width * 0.5;
    const double center_y = layout_geometry.rectangle.y + layout_geometry.rectangle.height * 0.5;
    const Point2d center = transform_point(layout_to_camera_matrix, center_x, center_y);
    const double scale =
        std::sqrt(layout_to_camera_matrix[0] * layout_to_camera_matrix[0] +
                  layout_to_camera_matrix[3] * layout_to_camera_matrix[3]);
    return runtime_oriented_rectangle(
        center.x,
        center.y,
        std::abs(scale) * layout_geometry.rectangle.width,
        std::abs(scale) * layout_geometry.rectangle.height,
        rotation_deg_clockwise);
}

VisibilityStatus compute_visibility_status(
    const RuntimeGeometry& geometry,
    int image_width,
    int image_height)
{
    if (image_width <= 0 || image_height <= 0) {
        return VisibilityStatus::kFull;
    }

    if (geometry.type == RuntimeGeometryType::kCircle) {
        const double left = geometry.circle.cx - geometry.circle.r;
        const double right = geometry.circle.cx + geometry.circle.r;
        const double top = geometry.circle.cy - geometry.circle.r;
        const double bottom = geometry.circle.cy + geometry.circle.r;
        if (right < 0.0 || bottom < 0.0 ||
            left > static_cast<double>(image_width) ||
            top > static_cast<double>(image_height)) {
            return VisibilityStatus::kOccluded;
        }
        if (left >= 0.0 && top >= 0.0 &&
            right <= static_cast<double>(image_width) &&
            bottom <= static_cast<double>(image_height)) {
            return VisibilityStatus::kFull;
        }
        return VisibilityStatus::kPartial;
    }

    const std::array<Point2d, 4> corners = oriented_rectangle_corners(geometry);
    int inside_count = 0;
    double min_x = std::numeric_limits<double>::max();
    double max_x = std::numeric_limits<double>::lowest();
    double min_y = std::numeric_limits<double>::max();
    double max_y = std::numeric_limits<double>::lowest();
    for (const Point2d& corner : corners) {
        if (point_inside_image(corner, image_width, image_height)) {
            ++inside_count;
        }
        min_x = std::min(min_x, corner.x);
        max_x = std::max(max_x, corner.x);
        min_y = std::min(min_y, corner.y);
        max_y = std::max(max_y, corner.y);
    }

    if (inside_count == 4) {
        return VisibilityStatus::kFull;
    }
    if (max_x < 0.0 || max_y < 0.0 ||
        min_x > static_cast<double>(image_width) ||
        min_y > static_cast<double>(image_height)) {
        return VisibilityStatus::kOccluded;
    }
    return VisibilityStatus::kPartial;
}

void rebuild_schema_preview(SpatialLayoutUiState* ui_state, const CameraParams* selected_camera)
{
    if (ui_state == nullptr) {
        return;
    }

    ui_state->layout_artifact.calibration_ref.artifact_id = ui_state->layout_artifact.artifact_id;
    ui_state->layout_artifact.calibration_ref.artifact_schema_id = orange::spatial::kArenaLayoutArtifactSchemaId;
    ui_state->layout_artifact.calibration_ref.artifact_schema_version = orange::spatial::kArenaLayoutArtifactSchemaVersion;
    if (ui_state->layout_artifact.calibration_ref.fingerprint.empty()) {
        ui_state->layout_artifact.calibration_ref.fingerprint = "preview-only";
    }

    ui_state->registration.layout_coordinate_space = ui_state->layout_artifact.layout.coordinate_space;
    ui_state->registration.layout_to_camera_matrix = build_layout_to_camera_matrix(*ui_state);
    ui_state->registration.has_camera_to_layout_matrix =
        invert_affine_3x3(ui_state->registration.layout_to_camera_matrix, &ui_state->registration.camera_to_layout_matrix);

    const double effective_rotation_deg =
        (ui_state->registration.type == RegistrationType::kSimilarity)
            ? ui_state->registration_rotation_deg_clockwise
            : 0.0;

    ui_state->arena_layout_runtime = {};
    ui_state->arena_layout_runtime.enabled = true;
    ui_state->arena_layout_runtime.layout_id = ui_state->layout_artifact.layout_id;
    ui_state->arena_layout_runtime.coordinate_space = CoordinateSpace::kCameraNativePixels;
    ui_state->arena_layout_runtime.registration = ui_state->registration;
    ui_state->arena_layout_runtime.zones.clear();

    for (const ArenaLayoutZone& zone : ui_state->layout_artifact.layout.zones) {
        ResolvedZoneOverlay overlay;
        overlay.zone_id = zone.zone_id;
        overlay.has_zone_index = zone.has_zone_index;
        overlay.zone_index = zone.zone_index;
        overlay.geometry =
            transform_layout_geometry(zone.geometry, ui_state->registration.layout_to_camera_matrix, effective_rotation_deg);
        overlay.visibility_status = compute_visibility_status(
            overlay.geometry,
            ui_state->captured_texture_width,
            ui_state->captured_texture_height);
        ui_state->arena_layout_runtime.zones.push_back(std::move(overlay));
    }

    ui_state->dish_mask_runtime = {};
    ui_state->dish_mask_runtime.enabled = true;
    ui_state->dish_mask_runtime.has_geometry = true;
    switch (ui_state->registration.source) {
        case RegistrationSource::kDetectedFit:
            ui_state->dish_mask_runtime.source = ObservationSource::kDetectedFit;
            break;
        case RegistrationSource::kImported:
            ui_state->dish_mask_runtime.source = ObservationSource::kImported;
            break;
        case RegistrationSource::kManual:
            ui_state->dish_mask_runtime.source = ObservationSource::kManual;
            break;
        case RegistrationSource::kIdentity:
        case RegistrationSource::kManualFit:
        default:
            ui_state->dish_mask_runtime.source = ObservationSource::kManualFit;
            break;
    }
    ui_state->dish_mask_runtime.geometry.coordinate_space = CoordinateSpace::kCameraNativePixels;
    ui_state->dish_mask_runtime.geometry.outer_geometry =
        transform_layout_geometry(ui_state->layout_artifact.layout.outer_geometry,
                                  ui_state->registration.layout_to_camera_matrix,
                                  effective_rotation_deg);
    ui_state->dish_mask_runtime.geometry.valid_geometry =
        offset_runtime_geometry(
            ui_state->dish_mask_runtime.geometry.outer_geometry,
            ui_state->centroid_gate_outset_px - ui_state->edge_margin_px);
    ui_state->dish_mask_runtime.geometry.edge_margin_px = ui_state->edge_margin_px;
    ui_state->dish_mask_runtime.geometry.centroid_gate_outset_px =
        ui_state->centroid_gate_outset_px;

    ui_state->preview_calibration = {};
    ui_state->preview_calibration.has_dish_mask = true;
    ui_state->preview_calibration.dish_mask.calibration_ref = CalibrationRef{
        "preview.dish_mask",
        orange::spatial::kDishMaskArtifactSchemaId,
        orange::spatial::kDishMaskArtifactSchemaVersion,
        "preview-only"
    };
    ui_state->preview_calibration.dish_mask.runtime = ui_state->dish_mask_runtime;
    ui_state->preview_calibration.has_arena_layout = true;
    ui_state->preview_calibration.arena_layout.calibration_ref = ui_state->layout_artifact.calibration_ref;
    ui_state->preview_calibration.arena_layout.runtime = ui_state->arena_layout_runtime;

    ui_state->canonical_layout_json = orange::spatial::arena_layout_artifact_to_json(ui_state->layout_artifact).dump(2);
    ui_state->runtime_preview_json = orange::spatial::camera_spatial_calibration_to_json(ui_state->preview_calibration).dump(2);

    std::string error;
    ui_state->preview_valid =
        orange::spatial::validate_arena_layout_artifact(ui_state->layout_artifact, &error) &&
        orange::spatial::validate_dish_mask_runtime(ui_state->dish_mask_runtime, &error) &&
        orange::spatial::validate_arena_layout_runtime_against_artifact(
            ui_state->arena_layout_runtime,
            ui_state->layout_artifact,
            &error) &&
        orange::spatial::validate_camera_spatial_calibration(ui_state->preview_calibration, &error);

    if (selected_camera != nullptr) {
        std::ostringstream status;
        status << (ui_state->preview_valid ? "Preview valid" : "Preview invalid")
               << " for " << selected_camera->camera_serial;
        if (ui_state->has_capture) {
            status << " (" << ui_state->captured_texture_width << "x" << ui_state->captured_texture_height << ")";
        }
        ui_state->preview_status = status.str();
    }
    ui_state->preview_error = ui_state->preview_valid ? std::string() : error;
}

}  // namespace orange::gui::spatial_layout
