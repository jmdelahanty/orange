#include "gui/spatial_layout/layout_state.h"

#include "gui/spatial_layout/geometry.h"
#include "project.h"

#include <algorithm>
#include <cmath>
#include <string>
#include <utility>

namespace orange::gui::spatial_layout {
namespace {

using orange::spatial::ArenaLayoutArtifact;
using orange::spatial::ArenaLayoutProvenanceSource;
using orange::spatial::ArenaLayoutZone;
using orange::spatial::CoordinateSpace;
using orange::spatial::LayoutGeometry;
using orange::spatial::LayoutGeometryType;
using orange::spatial::OrientationStatus;
using orange::spatial::RegistrationSource;
using orange::spatial::RegistrationType;
using orange::spatial::RuntimeGeometry;
using orange::spatial::RuntimeGeometryType;

constexpr double kPi = 3.14159265358979323846;
constexpr const char* kExperimentalAreaZoneId = "experimental_area";
constexpr const char* kExperimentalAreaZoneLabel = "Experimental Area";

}  // namespace

double normalize_angle_deg(double angle_deg)
{
    while (angle_deg <= -180.0) {
        angle_deg += 360.0;
    }
    while (angle_deg > 180.0) {
        angle_deg -= 360.0;
    }
    return angle_deg;
}

Point2d layout_geometry_center(const LayoutGeometry& geometry)
{
    if (geometry.type == LayoutGeometryType::kCircle) {
        return make_point(geometry.circle.cx, geometry.circle.cy);
    }
    return make_point(geometry.rectangle.x + geometry.rectangle.width * 0.5,
                      geometry.rectangle.y + geometry.rectangle.height * 0.5);
}

double layout_geometry_max_dimension(const LayoutGeometry& geometry)
{
    if (geometry.type == LayoutGeometryType::kCircle) {
        return geometry.circle.r * 2.0;
    }
    return std::max(geometry.rectangle.width, geometry.rectangle.height);
}

RuntimeGeometry runtime_circle(double cx, double cy, double r)
{
    RuntimeGeometry geometry;
    geometry.type = RuntimeGeometryType::kCircle;
    geometry.circle.cx = cx;
    geometry.circle.cy = cy;
    geometry.circle.r = r;
    return geometry;
}

RuntimeGeometry runtime_oriented_rectangle(
    double cx,
    double cy,
    double width,
    double height,
    double rotation_deg_clockwise)
{
    RuntimeGeometry geometry;
    geometry.type = RuntimeGeometryType::kOrientedRectangle;
    geometry.oriented_rectangle.cx = cx;
    geometry.oriented_rectangle.cy = cy;
    geometry.oriented_rectangle.width = width;
    geometry.oriented_rectangle.height = height;
    geometry.oriented_rectangle.rotation_deg_clockwise = rotation_deg_clockwise;
    return geometry;
}

LayoutGeometry default_outer_geometry()
{
    LayoutGeometry geometry;
    geometry.type = LayoutGeometryType::kCircle;
    geometry.circle.cx = 0.0;
    geometry.circle.cy = 0.0;
    geometry.circle.r = 50.0;
    return geometry;
}

ArenaLayoutZone make_experimental_area_zone(const LayoutGeometry& outer_geometry)
{
    ArenaLayoutZone zone;
    zone.zone_id = kExperimentalAreaZoneId;
    zone.has_zone_index = true;
    zone.zone_index = 0;
    zone.display_label = kExperimentalAreaZoneLabel;
    zone.geometry = outer_geometry;
    return zone;
}

bool has_single_experimental_area_zone(const SpatialLayoutUiState& ui_state)
{
    return ui_state.layout_artifact.layout.zones.size() == 1 &&
           ui_state.layout_artifact.layout.zones.front().zone_id == kExperimentalAreaZoneId;
}

void sync_single_experimental_area_zone(SpatialLayoutUiState* ui_state)
{
    if (ui_state == nullptr || !has_single_experimental_area_zone(*ui_state)) {
        return;
    }

    ArenaLayoutZone& zone = ui_state->layout_artifact.layout.zones.front();
    zone.has_zone_index = true;
    zone.zone_index = 0;
    if (zone.display_label.empty() || zone.display_label == "Zone 0") {
        zone.display_label = kExperimentalAreaZoneLabel;
    }
    zone.geometry = ui_state->layout_artifact.layout.outer_geometry;
    ui_state->selected_zone_index = 0;
}

void reset_to_single_experimental_area_zone(SpatialLayoutUiState* ui_state)
{
    if (ui_state == nullptr) {
        return;
    }

    ui_state->layout_artifact.layout.zones.clear();
    ui_state->layout_artifact.layout.zones.push_back(
        make_experimental_area_zone(ui_state->layout_artifact.layout.outer_geometry));
    ui_state->selected_zone_index = 0;
}

ArenaLayoutZone make_default_zone(const LayoutGeometry& outer_geometry, int zone_index)
{
    ArenaLayoutZone zone;
    zone.zone_id = "z" + std::to_string(zone_index);
    zone.has_zone_index = true;
    zone.zone_index = zone_index;
    zone.display_label = "Zone " + std::to_string(zone_index);

    const int col = zone_index % 3;
    const int row = (zone_index / 3) % 3;
    const double norm_x = (static_cast<double>(col) - 1.0) * 0.35;
    const double norm_y = (static_cast<double>(row) - 1.0) * 0.35;

    zone.geometry.type = LayoutGeometryType::kCircle;
    if (outer_geometry.type == LayoutGeometryType::kCircle) {
        zone.geometry.circle.cx = outer_geometry.circle.cx + norm_x * outer_geometry.circle.r;
        zone.geometry.circle.cy = outer_geometry.circle.cy + norm_y * outer_geometry.circle.r;
        zone.geometry.circle.r = outer_geometry.circle.r * 0.18;
    } else {
        zone.geometry.circle.cx = outer_geometry.rectangle.x + outer_geometry.rectangle.width * (0.5 + norm_x * 0.8);
        zone.geometry.circle.cy = outer_geometry.rectangle.y + outer_geometry.rectangle.height * (0.5 + norm_y * 0.8);
        zone.geometry.circle.r = std::min(outer_geometry.rectangle.width, outer_geometry.rectangle.height) * 0.12;
    }
    return zone;
}

void initialize_spatial_layout_defaults(SpatialLayoutUiState* ui_state)
{
    if (ui_state == nullptr || !ui_state->layout_artifact.layout_id.empty()) {
        return;
    }

    ArenaLayoutArtifact artifact;
    artifact.artifact_id = "preview.arena_layout";
    artifact.created_utc = get_current_utc_timestamp();
    artifact.calibration_ref.artifact_id = artifact.artifact_id;
    artifact.calibration_ref.artifact_schema_id = orange::spatial::kArenaLayoutArtifactSchemaId;
    artifact.calibration_ref.artifact_schema_version = orange::spatial::kArenaLayoutArtifactSchemaVersion;
    artifact.calibration_ref.fingerprint = "preview-only";
    artifact.layout_id = "layout_preview";
    artifact.layout.coordinate_space = CoordinateSpace::kLayoutUnits;
    artifact.layout.outer_geometry = default_outer_geometry();
    artifact.layout.zones.push_back(make_experimental_area_zone(artifact.layout.outer_geometry));
    artifact.provenance.source = ArenaLayoutProvenanceSource::kManualTemplate;
    artifact.provenance.ordering_rule = "row_major_top_left";
    artifact.provenance.notes = "Preview-only layout authoring state.";
    artifact.context.canvas_id = "preview_canvas";
    ui_state->layout_artifact = std::move(artifact);

    ui_state->registration.type = RegistrationType::kSimilarity;
    ui_state->registration.layout_coordinate_space = ui_state->layout_artifact.layout.coordinate_space;
    ui_state->registration.source = RegistrationSource::kManualFit;
    ui_state->registration.fit_point_count = 0;
    ui_state->registration.residual_px = 0.0;
    ui_state->registration.has_orientation_status = true;
    ui_state->registration.orientation_status = OrientationStatus::kUnknown;

    ui_state->registration_tx_px = 512.0;
    ui_state->registration_ty_px = 512.0;
    ui_state->registration_scale = 8.0;
    ui_state->registration_rotation_deg_clockwise = 0.0;
    ui_state->edge_margin_px = 0.0;
    ui_state->centroid_gate_outset_px = 12.0;
    ui_state->centroid_gate_outset_mm = 0.0;
    ui_state->centroid_gate_outset_authored_mm = false;
    ui_state->centroid_gate_outset_mm_camera_serial.clear();
}

void reset_registration_from_frame(SpatialLayoutUiState* ui_state)
{
    if (ui_state == nullptr ||
        ui_state->captured_texture_width <= 0 ||
        ui_state->captured_texture_height <= 0) {
        return;
    }

    const LayoutGeometry& outer = ui_state->layout_artifact.layout.outer_geometry;
    const Point2d center = layout_geometry_center(outer);
    const double layout_dim = std::max(layout_geometry_max_dimension(outer), 1e-6);
    const double frame_dim = static_cast<double>(
        std::min(ui_state->captured_texture_width, ui_state->captured_texture_height));
    ui_state->registration_scale = std::max(0.1, 0.85 * frame_dim / layout_dim);
    ui_state->registration_rotation_deg_clockwise = 0.0;
    ui_state->registration_tx_px =
        static_cast<double>(ui_state->captured_texture_width) * 0.5 -
        ui_state->registration_scale * center.x;
    ui_state->registration_ty_px =
        static_cast<double>(ui_state->captured_texture_height) * 0.5 -
        ui_state->registration_scale * center.y;
    ui_state->calibration_inner_rim_target_confirmed = false;
    ui_state->captured_canvas_view.fit_requested = true;
}

void clear_detected_experimental_area_circle(SpatialLayoutUiState* ui_state)
{
    if (ui_state == nullptr) {
        return;
    }
    ui_state->has_detected_experimental_area_circle = false;
    ui_state->detected_experimental_area_geometry = RuntimeGeometry{};
    ui_state->calibration_inner_rim_target_confirmed = false;
    ui_state->detection_status.clear();
    ui_state->detection_error.clear();
}

bool invert_affine_3x3(const std::array<double, 9>& matrix, std::array<double, 9>* out)
{
    if (out == nullptr) {
        return false;
    }

    const double a = matrix[0];
    const double b = matrix[1];
    const double c = matrix[2];
    const double d = matrix[3];
    const double e = matrix[4];
    const double f = matrix[5];
    const double det = a * e - b * d;
    if (!std::isfinite(det) || std::abs(det) < 1e-12) {
        return false;
    }

    *out = {
        e / det, -b / det, (b * f - e * c) / det,
        -d / det, a / det, (d * c - a * f) / det,
        0.0, 0.0, 1.0
    };
    return true;
}

std::array<double, 9> build_layout_to_camera_matrix(const SpatialLayoutUiState& ui_state)
{
    double tx = ui_state.registration_tx_px;
    double ty = ui_state.registration_ty_px;
    double scale = ui_state.registration_scale;
    double rotation_deg_clockwise = ui_state.registration_rotation_deg_clockwise;

    if (ui_state.registration.type == RegistrationType::kIdentity) {
        tx = 0.0;
        ty = 0.0;
        scale = 1.0;
        rotation_deg_clockwise = 0.0;
    } else if (ui_state.registration.type == RegistrationType::kTranslation) {
        scale = 1.0;
        rotation_deg_clockwise = 0.0;
    }

    scale = std::max(scale, 1e-6);
    const double theta = rotation_deg_clockwise * kPi / 180.0;
    const double cos_theta = std::cos(theta);
    const double sin_theta = std::sin(theta);

    return {
        scale * cos_theta, -scale * sin_theta, tx,
        scale * sin_theta, scale * cos_theta, ty,
        0.0, 0.0, 1.0
    };
}

void set_registration_transform(SpatialLayoutUiState* ui_state,
                                RegistrationType type,
                                const Point2d& desired_outer_center,
                                double scale,
                                double rotation_deg_clockwise,
                                RegistrationSource source)
{
    if (ui_state == nullptr) {
        return;
    }

    if (type == RegistrationType::kIdentity) {
        scale = 1.0;
        rotation_deg_clockwise = 0.0;
    } else if (type == RegistrationType::kTranslation) {
        scale = 1.0;
        rotation_deg_clockwise = 0.0;
    } else {
        scale = std::max(1e-6, scale);
    }

    const Point2d canonical_center =
        layout_geometry_center(ui_state->layout_artifact.layout.outer_geometry);
    const double theta = rotation_deg_clockwise * kPi / 180.0;
    const double rotated_center_x =
        scale * (std::cos(theta) * canonical_center.x - std::sin(theta) * canonical_center.y);
    const double rotated_center_y =
        scale * (std::sin(theta) * canonical_center.x + std::cos(theta) * canonical_center.y);

    ui_state->registration.type = type;
    ui_state->registration.source = source;
    ui_state->registration_tx_px = desired_outer_center.x - rotated_center_x;
    ui_state->registration_ty_px = desired_outer_center.y - rotated_center_y;
    ui_state->registration_scale = scale;
    ui_state->registration_rotation_deg_clockwise = normalize_angle_deg(rotation_deg_clockwise);
}

}  // namespace orange::gui::spatial_layout
