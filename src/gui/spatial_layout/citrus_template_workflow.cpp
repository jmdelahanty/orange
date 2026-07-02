#include "gui/spatial_layout/citrus_template_workflow.h"

#include "gui/spatial_layout/citrus_import.h"
#include "gui/spatial_layout/geometry.h"
#include "gui/spatial_layout/layout_state.h"
#include "gui/spatial_layout/session_io.h"
#include "project.h"

#include <algorithm>
#include <filesystem>
#include <iomanip>
#include <sstream>
#include <utility>
#include <vector>

namespace orange::gui::spatial_layout {
namespace {

using orange::spatial::ArenaLayoutProvenanceSource;
using orange::spatial::CircleGeometry;
using orange::spatial::CoordinateSpace;
using orange::spatial::LayoutGeometry;
using orange::spatial::LayoutGeometryType;
using orange::spatial::RegistrationSource;
using orange::spatial::RegistrationType;
using orange::spatial::RuntimeGeometry;

constexpr int kProjectedCircleSampleCount = 96;

std::string join_strings(const std::vector<std::string>& values, const std::string& separator)
{
    std::ostringstream oss;
    for (size_t idx = 0; idx < values.size(); ++idx) {
        if (idx > 0) {
            oss << separator;
        }
        oss << values[idx];
    }
    return oss.str();
}

bool update_citrus_projected_circle_preview(
    SpatialLayoutUiState* ui_state,
    double* rms_error_out,
    std::string* error_out)
{
    if (ui_state == nullptr) {
        if (error_out) {
            *error_out = "Spatial layout UI state is null.";
        }
        return false;
    }
    ui_state->has_citrus_projected_circle = false;
    ui_state->citrus_projected_circle_geometry = RuntimeGeometry{};
    ui_state->citrus_projected_outline_camera_points.clear();
    if (!ui_state->citrus_template.available ||
        !ui_state->citrus_template.has_canvas_to_camera_homography) {
        if (error_out) {
            *error_out = "Imported Citrus template does not have a canvas-to-camera homography.";
        }
        return false;
    }

    std::vector<Point2d> camera_points;
    if (!sample_citrus_experimental_area_outline_in_camera_px(
            ui_state->citrus_template,
            make_point(ui_state->citrus_template.experimental_area_center_x_px,
                       ui_state->citrus_template.experimental_area_center_y_px),
            &camera_points,
            error_out)) {
        return false;
    }

    CircleGeometry fitted_circle;
    double rms_error = 0.0;
    if (!fit_circle_to_points(camera_points, &fitted_circle, &rms_error, error_out)) {
        return false;
    }

    ui_state->has_citrus_projected_circle = true;
    ui_state->citrus_projected_circle_geometry =
        runtime_circle(fitted_circle.cx, fitted_circle.cy, fitted_circle.r);
    ui_state->citrus_projected_outline_camera_points = std::move(camera_points);
    if (rms_error_out != nullptr) {
        *rms_error_out = rms_error;
    }
    return true;
}

bool apply_citrus_template_to_spatial_layout(
    SpatialLayoutUiState* ui_state,
    const CitrusSpatialTemplateState& template_state,
    std::string* status_out)
{
    if (ui_state == nullptr || !template_state.available) {
        return false;
    }

    ui_state->citrus_template = template_state;
    ui_state->has_citrus_projected_circle = false;
    ui_state->citrus_projected_circle_geometry = RuntimeGeometry{};
    ui_state->citrus_projected_outline_camera_points.clear();

    LayoutGeometry imported_outer;
    imported_outer.type = LayoutGeometryType::kCircle;
    imported_outer.circle.cx = template_state.experimental_area_center_x_px;
    imported_outer.circle.cy = template_state.experimental_area_center_y_px;
    imported_outer.circle.r = template_state.experimental_area_radius_px;

    ui_state->layout_artifact.artifact_id = "preview.citrus_import";
    ui_state->layout_artifact.created_utc = get_current_utc_timestamp();
    ui_state->layout_artifact.calibration_ref.artifact_id = ui_state->layout_artifact.artifact_id;
    ui_state->layout_artifact.calibration_ref.artifact_schema_id = orange::spatial::kArenaLayoutArtifactSchemaId;
    ui_state->layout_artifact.calibration_ref.artifact_schema_version = orange::spatial::kArenaLayoutArtifactSchemaVersion;
    ui_state->layout_artifact.calibration_ref.fingerprint = "preview-only";
    ui_state->layout_artifact.layout_id =
        "citrus_" + sanitize_artifact_component(template_state.source_canvas_name) + "_" +
        sanitize_artifact_component(template_state.source_config_name);
    ui_state->layout_artifact.layout.coordinate_space = CoordinateSpace::kLayoutUnits;
    ui_state->layout_artifact.layout.outer_geometry = imported_outer;
    reset_to_single_experimental_area_zone(ui_state);
    ui_state->layout_artifact.context.canvas_id = template_state.source_canvas_name;
    ui_state->layout_artifact.context.arena_id = template_state.source_arena_name;
    ui_state->layout_artifact.context.dish_design_id = template_state.source_dish_type_name;
    ui_state->layout_artifact.provenance.source = ArenaLayoutProvenanceSource::kImportedTemplate;
    ui_state->layout_artifact.provenance.ordering_rule = "single_circle_imported_from_citrus";
    ui_state->layout_artifact.provenance.notes =
        "Imported from Citrus config " + template_state.source_config_path +
        " for camera " + template_state.source_camera_id + ".";
    ui_state->selected_zone_index = 0;

    ui_state->registration.layout_coordinate_space = ui_state->layout_artifact.layout.coordinate_space;
    if (ui_state->has_capture) {
        reset_registration_from_frame(ui_state);
    }

    std::ostringstream status;
    status << "Selected Citrus circle from " << template_state.source_canvas_name
           << " / " << template_state.source_config_name
           << " for camera " << template_state.source_camera_id;
    if (template_state.has_canvas_to_camera_homography) {
        double preview_rms = 0.0;
        std::string preview_error;
        if (update_citrus_projected_circle_preview(ui_state, &preview_rms, &preview_error)) {
            status << ". Homography loaded; projected-outline circle-fit RMS "
                   << std::fixed << std::setprecision(2)
                   << preview_rms << " px.";
        } else {
            status << ". Homography loaded but preview seed failed (" << preview_error << ").";
        }
    } else {
        status << ". No homography sidecar found.";
    }

    if (status_out) {
        *status_out = status.str();
    }
    return true;
}

}  // namespace

void clear_citrus_template_import(SpatialLayoutUiState* ui_state)
{
    if (ui_state == nullptr) {
        return;
    }
    ui_state->citrus_template = {};
    ui_state->citrus_canvas_templates.clear();
    ui_state->citrus_canvas_template_index = -1;
    ui_state->citrus_canvas_config_path.clear();
    ui_state->has_citrus_projected_circle = false;
    ui_state->citrus_projected_circle_geometry = RuntimeGeometry{};
    ui_state->citrus_projected_outline_camera_points.clear();
    ui_state->citrus_import_status.clear();
    ui_state->citrus_import_error.clear();
}

bool seed_registration_from_citrus_homography(
    SpatialLayoutUiState* ui_state,
    std::string* error_out)
{
    if (ui_state == nullptr) {
        if (error_out) {
            *error_out = "Spatial layout UI state is null.";
        }
        return false;
    }
    if (ui_state->layout_artifact.layout.outer_geometry.type != LayoutGeometryType::kCircle) {
        if (error_out) {
            *error_out = "Citrus homography seeding currently supports only circular outer geometry.";
        }
        return false;
    }

    double rms_error = 0.0;
    if (!update_citrus_projected_circle_preview(ui_state, &rms_error, error_out)) {
        return false;
    }

    const double canonical_radius = ui_state->layout_artifact.layout.outer_geometry.circle.r;
    if (canonical_radius <= 0.0) {
        if (error_out) {
            *error_out = "Canonical outer radius must be positive.";
        }
        return false;
    }

    const Point2d center(
        make_point(ui_state->citrus_projected_circle_geometry.circle.cx,
                   ui_state->citrus_projected_circle_geometry.circle.cy));
    const double scale =
        ui_state->citrus_projected_circle_geometry.circle.r / canonical_radius;
    set_registration_transform(
        ui_state,
        RegistrationType::kSimilarity,
        center,
        scale,
        0.0,
        RegistrationSource::kImported);
    ui_state->registration.fit_point_count = kProjectedCircleSampleCount;
    ui_state->registration.residual_px = std::max(0.0, rms_error);
    return true;
}

bool select_citrus_template_by_index(
    SpatialLayoutUiState* ui_state,
    int index,
    std::string* status_out,
    std::string* error_out)
{
    if (ui_state == nullptr) {
        if (error_out) {
            *error_out = "Spatial layout UI state is null.";
        }
        return false;
    }
    if (index < 0 || index >= static_cast<int>(ui_state->citrus_canvas_templates.size())) {
        if (error_out) {
            *error_out = "Citrus canvas template index is out of range.";
        }
        return false;
    }
    ui_state->citrus_canvas_template_index = index;
    if (!apply_citrus_template_to_spatial_layout(
            ui_state,
            ui_state->citrus_canvas_templates[static_cast<size_t>(index)],
            status_out)) {
        if (error_out) {
            *error_out = "Failed to apply Citrus canvas template.";
        }
        return false;
    }
    return true;
}

bool import_citrus_canvas_templates(
    SpatialLayoutUiState* ui_state,
    const CameraParams& selected_camera,
    const std::filesystem::path& config_path,
    std::string* status_out,
    std::string* error_out)
{
    if (ui_state == nullptr) {
        if (error_out) {
            *error_out = "Spatial layout UI state is null.";
        }
        return false;
    }

    nlohmann::json root;
    if (!read_json_file(config_path, &root, error_out)) {
        return false;
    }
    if (!root.is_object()) {
        if (error_out) {
            *error_out = "Citrus canvas config root must be a JSON object.";
        }
        return false;
    }

    std::vector<CitrusSpatialTemplateState> templates;
    std::vector<std::string> available_camera_ids;
    if (!collect_citrus_single_circle_templates(
            config_path,
            root,
            &templates,
            &available_camera_ids,
            error_out)) {
        return false;
    }

    ui_state->citrus_canvas_templates = std::move(templates);
    ui_state->citrus_canvas_config_path = config_path.string();
    ui_state->citrus_canvas_template_index = -1;

    int selected_index = find_citrus_template_index_for_camera(
        *ui_state,
        selected_camera.camera_serial);
    if (selected_index < 0 && !ui_state->citrus_canvas_templates.empty()) {
        selected_index = 0;
    }

    std::string selected_status;
    std::string selected_error;
    if (!select_citrus_template_by_index(
            ui_state,
            selected_index,
            &selected_status,
            &selected_error)) {
        if (error_out) {
            *error_out = selected_error;
        }
        return false;
    }

    std::ostringstream status;
    status << "Loaded Citrus canvas " << config_path.parent_path().filename().string()
           << " with " << ui_state->citrus_canvas_templates.size()
           << " supported single-circle arena template(s). "
           << selected_status;
    if (ui_state->citrus_template.source_camera_id != selected_camera.camera_serial) {
        status << " No template matched selected Orange camera "
               << selected_camera.camera_serial << ".";
        if (!available_camera_ids.empty()) {
            status << " Available camera_ids: "
                   << join_strings(available_camera_ids, ", ") << ".";
        }
    }
    if (status_out) {
        *status_out = status.str();
    }
    return true;
}

}  // namespace orange::gui::spatial_layout
