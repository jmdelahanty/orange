#include "gui/spatial_layout/linked_top_rim_layout.h"

#include "gui/spatial_layout/layout_artifact_persistence.h"
#include "gui/spatial_layout/layout_state.h"
#include "gui/spatial_layout/runtime_builder.h"
#include "gui/spatial_layout/session_io.h"
#include "project.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <sstream>
#include <utility>
#include <vector>

namespace orange::gui::spatial_layout {
namespace {

using orange::spatial::ArenaLayoutArtifact;
using orange::spatial::ArenaLayoutProvenanceSource;
using orange::spatial::ArenaLayoutRuntime;
using orange::spatial::ArenaLayoutZone;
using orange::spatial::CircleGeometry;
using orange::spatial::CoordinateSpace;
using orange::spatial::DishMaskRuntime;
using orange::spatial::LayoutGeometry;
using orange::spatial::LayoutGeometryType;
using orange::spatial::ObservationSource;
using orange::spatial::OrientationStatus;
using orange::spatial::RegistrationSource;
using orange::spatial::RegistrationType;
using orange::spatial::ResolvedZoneOverlay;
using orange::spatial::ViewRegistration;

constexpr double kPi = 3.14159265358979323846;

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

const CameraParams* find_camera_params_by_serial(
    const CameraParams* cameras_params,
    int num_cameras,
    const std::string& camera_serial)
{
    if (cameras_params == nullptr || camera_serial.empty()) {
        return nullptr;
    }
    for (int idx = 0; idx < num_cameras; ++idx) {
        if (cameras_params[idx].camera_serial == camera_serial) {
            return &cameras_params[idx];
        }
    }
    return nullptr;
}

const CitrusSpatialTemplateState* find_citrus_template_by_camera_serial(
    const SpatialLayoutUiState& ui_state,
    const std::string& camera_serial)
{
    if (camera_serial.empty()) {
        return nullptr;
    }
    for (const CitrusSpatialTemplateState& template_state :
         ui_state.citrus_canvas_templates) {
        if (template_state.available &&
            template_state.source_camera_id == camera_serial) {
            return &template_state;
        }
    }
    if (ui_state.citrus_template.available &&
        ui_state.citrus_template.source_camera_id == camera_serial) {
        return &ui_state.citrus_template;
    }
    return nullptr;
}

bool circle_geometry_from_json(
    const nlohmann::json& geometry,
    CircleGeometry* circle_out,
    std::string* error_out)
{
    if (circle_out == nullptr) {
        if (error_out) {
            *error_out = "Circle destination is null.";
        }
        return false;
    }
    if (!geometry.is_object() ||
        geometry.value("type", std::string()) != "circle" ||
        !geometry.contains("center_px") ||
        !geometry["center_px"].is_object()) {
        if (error_out) {
            *error_out = "Expected circle geometry with center_px.";
        }
        return false;
    }
    CircleGeometry circle;
    circle.cx = geometry["center_px"].value("x", 0.0);
    circle.cy = geometry["center_px"].value("y", 0.0);
    circle.r = geometry.value("radius_px", 0.0);
    if (circle.r <= 0.0) {
        if (error_out) {
            *error_out = "Circle radius must be positive.";
        }
        return false;
    }
    *circle_out = circle;
    return true;
}

std::array<double, 9> build_similarity_layout_to_camera_matrix(
    const LayoutGeometry& canonical_outer,
    const CircleGeometry& target_circle,
    double scale,
    double rotation_deg_clockwise)
{
    const Point2d canonical_center = layout_geometry_center(canonical_outer);
    scale = std::max(scale, 1e-6);
    const double theta = rotation_deg_clockwise * kPi / 180.0;
    const double cos_theta = std::cos(theta);
    const double sin_theta = std::sin(theta);
    const double rotated_center_x =
        scale * (cos_theta * canonical_center.x - sin_theta * canonical_center.y);
    const double rotated_center_y =
        scale * (sin_theta * canonical_center.x + cos_theta * canonical_center.y);
    return {
        scale * cos_theta, -scale * sin_theta, target_circle.cx - rotated_center_x,
        scale * sin_theta, scale * cos_theta, target_circle.cy - rotated_center_y,
        0.0, 0.0, 1.0
    };
}

bool build_arena_layout_bundle_from_linked_top_rim(
    const nlohmann::json& linked_top_rim,
    const CitrusSpatialTemplateState& template_state,
    const CameraParams& camera_params,
    const std::string& timestamp,
    double default_edge_margin_px,
    ArenaLayoutArtifact* artifact_out,
    DishMaskRuntime* dish_mask_runtime_out,
    ArenaLayoutRuntime* arena_layout_runtime_out,
    std::string* error_out)
{
    if (artifact_out == nullptr ||
        dish_mask_runtime_out == nullptr ||
        arena_layout_runtime_out == nullptr) {
        if (error_out) {
            *error_out = "Arena layout bundle destination is null.";
        }
        return false;
    }
    if (!template_state.available) {
        if (error_out) {
            *error_out = "Citrus template is not available.";
        }
        return false;
    }
    if (template_state.experimental_area_radius_px <= 0.0) {
        if (error_out) {
            *error_out = "Citrus experimental-area radius must be positive.";
        }
        return false;
    }

    const std::string camera_serial =
        linked_top_rim.value("camera_serial", camera_params.camera_serial);
    const std::string arena_id =
        linked_top_rim.value("arena_id", template_state.source_arena_name);
    const std::string canvas_id =
        linked_top_rim.value("canvas_id", template_state.source_canvas_name);
    if (!template_state.source_camera_id.empty() &&
        template_state.source_camera_id != camera_serial) {
        if (error_out) {
            *error_out = "Linked top-rim camera " + camera_serial +
                         " does not match Citrus template camera " +
                         template_state.source_camera_id + ".";
        }
        return false;
    }
    if (!template_state.source_arena_name.empty() &&
        !arena_id.empty() &&
        template_state.source_arena_name != arena_id) {
        if (error_out) {
            *error_out = "Linked top-rim arena " + arena_id +
                         " does not match Citrus template arena " +
                         template_state.source_arena_name + ".";
        }
        return false;
    }

    const nlohmann::json accepted_boundary =
        linked_top_rim.value(
            "accepted_inner_rim_boundary",
            linked_top_rim.value(
                "accepted_experimental_area_boundary",
                linked_top_rim.value(
                    "observed_boundary",
                    nlohmann::json::object())));
    CircleGeometry accepted_circle;
    if (!circle_geometry_from_json(
            accepted_boundary.value("geometry", nlohmann::json::object()),
            &accepted_circle,
            error_out)) {
        if (error_out && !linked_top_rim.value("artifact_id", std::string()).empty()) {
            *error_out += " artifact_id=" + linked_top_rim.value("artifact_id", std::string());
        }
        return false;
    }

    CircleGeometry valid_circle = accepted_circle;
    const nlohmann::json valid_region =
        linked_top_rim.value("valid_detection_region", nlohmann::json::object());
    if (valid_region.is_object() && valid_region.contains("geometry")) {
        std::string valid_error;
        if (!circle_geometry_from_json(
                valid_region["geometry"],
                &valid_circle,
                &valid_error)) {
            valid_circle = accepted_circle;
        }
    } else {
        valid_circle.r =
            std::max(0.0, accepted_circle.r - std::max(0.0, default_edge_margin_px));
    }
    const double edge_margin_px =
        std::max(0.0, accepted_circle.r - valid_circle.r);
    const double centroid_gate_outset_px =
        std::max(0.0, valid_circle.r - accepted_circle.r);

    ArenaLayoutArtifact artifact;
    artifact.artifact_id = build_arena_layout_artifact_id(
        "citrus_" + sanitize_artifact_component(template_state.source_canvas_name) +
            "_" + sanitize_artifact_component(template_state.source_config_name),
        camera_params,
        timestamp);
    artifact.created_utc = timestamp;
    artifact.layout_id =
        "citrus_" + sanitize_artifact_component(template_state.source_canvas_name) +
        "_" + sanitize_artifact_component(template_state.source_config_name);
    artifact.layout.coordinate_space = CoordinateSpace::kLayoutUnits;
    artifact.layout.outer_geometry.type = LayoutGeometryType::kCircle;
    artifact.layout.outer_geometry.circle.cx =
        template_state.experimental_area_center_x_px;
    artifact.layout.outer_geometry.circle.cy =
        template_state.experimental_area_center_y_px;
    artifact.layout.outer_geometry.circle.r =
        template_state.experimental_area_radius_px;
    artifact.layout.zones.push_back(make_experimental_area_zone(artifact.layout.outer_geometry));
    artifact.context.canvas_id = canvas_id;
    artifact.context.arena_id = arena_id;
    artifact.context.dish_design_id = template_state.source_dish_type_name;
    artifact.provenance.source = ArenaLayoutProvenanceSource::kImportedTemplate;
    artifact.provenance.ordering_rule = "single_circle_imported_from_citrus";
    artifact.provenance.notes =
        "Imported from Citrus config " + template_state.source_config_path +
        " for camera " + camera_serial +
        " and aligned to linked accepted top-rim observation " +
        linked_top_rim.value("artifact_id", std::string("unknown")) + ".";

    ViewRegistration registration;
    registration.type = RegistrationType::kSimilarity;
    registration.layout_coordinate_space = artifact.layout.coordinate_space;
    registration.source = RegistrationSource::kDetectedFit;
    registration.fit_point_count = 3;
    registration.residual_px = 0.0;
    registration.has_orientation_status = true;
    registration.orientation_status = OrientationStatus::kAmbiguous;
    const double scale =
        accepted_circle.r / std::max(1e-6, template_state.experimental_area_radius_px);
    registration.layout_to_camera_matrix =
        build_similarity_layout_to_camera_matrix(
            artifact.layout.outer_geometry,
            accepted_circle,
            scale,
            0.0);
    registration.has_camera_to_layout_matrix =
        invert_affine_3x3(
            registration.layout_to_camera_matrix,
            &registration.camera_to_layout_matrix);

    int image_width = camera_params.width;
    int image_height = camera_params.height;
    const nlohmann::json accepted_mask =
        linked_top_rim.value("accepted_mask", nlohmann::json::object());
    const nlohmann::json image_shape =
        accepted_mask.value("image_shape_px", nlohmann::json::object());
    if (image_shape.is_object()) {
        image_width = image_shape.value("width", image_width);
        image_height = image_shape.value("height", image_height);
    }

    ArenaLayoutRuntime arena_layout_runtime;
    arena_layout_runtime.enabled = true;
    arena_layout_runtime.layout_id = artifact.layout_id;
    arena_layout_runtime.coordinate_space = CoordinateSpace::kCameraNativePixels;
    arena_layout_runtime.registration = registration;
    for (const ArenaLayoutZone& zone : artifact.layout.zones) {
        ResolvedZoneOverlay overlay;
        overlay.zone_id = zone.zone_id;
        overlay.has_zone_index = zone.has_zone_index;
        overlay.zone_index = zone.zone_index;
        overlay.geometry =
            transform_layout_geometry(
                zone.geometry,
                registration.layout_to_camera_matrix,
                0.0);
        overlay.visibility_status =
            compute_visibility_status(overlay.geometry, image_width, image_height);
        arena_layout_runtime.zones.push_back(std::move(overlay));
    }

    DishMaskRuntime dish_mask_runtime;
    dish_mask_runtime.enabled = true;
    dish_mask_runtime.has_geometry = true;
    dish_mask_runtime.source = ObservationSource::kDetectedFit;
    dish_mask_runtime.geometry.coordinate_space = CoordinateSpace::kCameraNativePixels;
    dish_mask_runtime.geometry.outer_geometry =
        runtime_circle(accepted_circle.cx, accepted_circle.cy, accepted_circle.r);
    dish_mask_runtime.geometry.valid_geometry =
        runtime_circle(valid_circle.cx, valid_circle.cy, valid_circle.r);
    dish_mask_runtime.geometry.edge_margin_px = edge_margin_px;
    dish_mask_runtime.geometry.centroid_gate_outset_px =
        centroid_gate_outset_px;

    *artifact_out = std::move(artifact);
    *dish_mask_runtime_out = std::move(dish_mask_runtime);
    *arena_layout_runtime_out = std::move(arena_layout_runtime);
    return true;
}

}  // namespace

bool save_linked_arena_layout_artifacts(
    SpatialLayoutUiState* ui_state,
    const CameraParams* cameras_params,
    int num_cameras,
    const std::string& session_artifact_root,
    std::string* status_out,
    std::string* error_out)
{
    if (ui_state == nullptr) {
        if (error_out) {
            *error_out = "Spatial layout UI state is null.";
        }
        return false;
    }
    if (ui_state->calibration_session_dir.empty()) {
        if (error_out) {
            *error_out = "Start or load a calibration session before saving linked arena layouts.";
        }
        return false;
    }
    if (ui_state->citrus_canvas_templates.empty() &&
        !ui_state->citrus_template.available) {
        if (error_out) {
            *error_out =
                "Load a Citrus canvas before saving linked arena layouts.";
        }
        return false;
    }

    const std::filesystem::path session_dir(ui_state->calibration_session_dir);
    const std::filesystem::path index_path =
        session_dir / kCalibrationSessionIndexFilename;
    nlohmann::json index;
    if (!read_json_file(index_path, &index, error_out)) {
        return false;
    }
    const nlohmann::json artifacts_by_id =
        index.value("artifacts_by_id", nlohmann::json::object());
    if (!artifacts_by_id.is_object()) {
        if (error_out) {
            *error_out = "Calibration session index has no artifacts_by_id object.";
        }
        return false;
    }

    const std::string timestamp = get_current_utc_timestamp();
    std::vector<std::string> saved_labels;
    std::vector<std::string> skipped_labels;
    for (const auto& item : artifacts_by_id.items()) {
        const nlohmann::json& entry = item.value();
        if (!entry.is_object() ||
            entry.value("artifact_schema_id", std::string()) !=
                "orange.calibration.image_set") {
            continue;
        }
        const std::string relative_manifest_path =
            entry.value("relative_manifest_path", std::string());
        if (relative_manifest_path.empty()) {
            skipped_labels.push_back(item.key() + ": missing manifest path");
            continue;
        }
        nlohmann::json manifest;
        if (!read_json_file(session_dir / relative_manifest_path, &manifest, error_out)) {
            return false;
        }
        const nlohmann::json linked_top_rim =
            manifest.value("linked_observations", nlohmann::json::object())
                .value("accepted_top_rim_observation", nlohmann::json::object());
        if (!linked_top_rim.is_object() || linked_top_rim.empty()) {
            skipped_labels.push_back(item.key() + ": no linked accepted top rim");
            continue;
        }

        const std::string camera_serial =
            linked_top_rim.value(
                "camera_serial",
                manifest.value("summary", nlohmann::json::object())
                    .value("camera_serial", std::string()));
        const CameraParams* camera_params =
            find_camera_params_by_serial(cameras_params, num_cameras, camera_serial);
        if (camera_params == nullptr) {
            skipped_labels.push_back(item.key() + ": camera " + camera_serial + " not open/configured");
            continue;
        }
        const CitrusSpatialTemplateState* template_state =
            find_citrus_template_by_camera_serial(*ui_state, camera_serial);
        if (template_state == nullptr) {
            skipped_labels.push_back(item.key() + ": no Citrus template for camera " + camera_serial);
            continue;
        }

        ArenaLayoutArtifact artifact;
        DishMaskRuntime dish_mask_runtime;
        ArenaLayoutRuntime arena_layout_runtime;
        std::string bundle_error;
        if (!build_arena_layout_bundle_from_linked_top_rim(
                linked_top_rim,
                *template_state,
                *camera_params,
                timestamp,
                ui_state->edge_margin_px,
                &artifact,
                &dish_mask_runtime,
                &arena_layout_runtime,
                &bundle_error)) {
            skipped_labels.push_back(item.key() + ": " + bundle_error);
            continue;
        }

        ArenaLayoutArtifact saved_artifact;
        std::string saved_artifact_dir;
        if (!persist_spatial_layout_artifact_bundle(
                artifact,
                dish_mask_runtime,
                arena_layout_runtime,
                *camera_params,
                session_artifact_root,
                ui_state->calibration_session_dir,
                &saved_artifact,
                &saved_artifact_dir,
                error_out)) {
            return false;
        }
        saved_labels.push_back(
            camera_serial + "/" +
            saved_artifact.context.arena_id + " -> " +
            saved_artifact.artifact_id);
    }

    if (saved_labels.empty()) {
        if (error_out) {
            *error_out = "No linked arena layouts were saved.";
            if (!skipped_labels.empty()) {
                *error_out += " Skipped: " + join_strings(skipped_labels, "; ");
            }
        }
        return false;
    }

    if (status_out) {
        std::ostringstream status;
        status << "Saved " << saved_labels.size()
               << " linked arena layout artifact"
               << (saved_labels.size() == 1 ? "" : "s")
               << " in calibration session "
               << std::filesystem::path(ui_state->calibration_session_dir).filename().generic_string()
               << ": " << join_strings(saved_labels, ", ");
        if (!skipped_labels.empty()) {
            status << ". Skipped " << skipped_labels.size()
                   << ": " << join_strings(skipped_labels, "; ");
        }
        *status_out = status.str();
    }
    return true;
}

}  // namespace orange::gui::spatial_layout
