#include "gui/spatial_layout/calibration_metadata.h"

#include <string>

namespace orange::gui::spatial_layout {
namespace {

using orange::spatial::DishMaskGeometry;
using orange::spatial::RuntimeGeometry;
using orange::spatial::RuntimeGeometryType;

std::string metadata_or_unknown(const std::string& value)
{
    return value.empty() ? std::string("unknown") : value;
}

void populate_calibration_domain_metadata_from_runtime(
    SpatialLayoutCalibrationImageSetMetadata* metadata,
    const SpatialLayoutUiState& ui_state)
{
    if (metadata == nullptr || !ui_state.dish_mask_runtime.has_geometry) {
        return;
    }

    const DishMaskGeometry& geometry = ui_state.dish_mask_runtime.geometry;
    metadata->has_calibration_domain = true;
    metadata->calibration_domain_source =
        std::string("orange_spatial_layout_runtime:") +
        orange::spatial::observation_source_to_string(ui_state.dish_mask_runtime.source);
    metadata->calibration_domain_coordinate_space =
        orange::spatial::coordinate_space_to_string(geometry.coordinate_space);
    metadata->calibration_domain_edge_margin_px = geometry.edge_margin_px;
    metadata->calibration_domain_centroid_gate_outset_px =
        geometry.centroid_gate_outset_px;

    const RuntimeGeometry& outer = geometry.outer_geometry;
    const RuntimeGeometry& valid = geometry.valid_geometry;
    if (outer.type == RuntimeGeometryType::kCircle && outer.circle.r > 0.0) {
        metadata->calibration_domain_shape = "circle";
        metadata->calibration_domain_center_x_px = outer.circle.cx;
        metadata->calibration_domain_center_y_px = outer.circle.cy;
        metadata->calibration_domain_radius_px = outer.circle.r;
        if (valid.type == RuntimeGeometryType::kCircle && valid.circle.r > 0.0) {
            metadata->has_calibration_domain_valid_circle = true;
            metadata->calibration_domain_valid_center_x_px = valid.circle.cx;
            metadata->calibration_domain_valid_center_y_px = valid.circle.cy;
            metadata->calibration_domain_valid_radius_px = valid.circle.r;
        }
        return;
    }

    if (outer.type == RuntimeGeometryType::kOrientedRectangle &&
        outer.oriented_rectangle.width > 0.0 &&
        outer.oriented_rectangle.height > 0.0) {
        metadata->calibration_domain_shape = "oriented_rectangle";
        metadata->calibration_domain_center_x_px = outer.oriented_rectangle.cx;
        metadata->calibration_domain_center_y_px = outer.oriented_rectangle.cy;
        metadata->calibration_domain_width_px = outer.oriented_rectangle.width;
        metadata->calibration_domain_height_px = outer.oriented_rectangle.height;
        metadata->calibration_domain_rotation_deg_clockwise =
            outer.oriented_rectangle.rotation_deg_clockwise;
        if (valid.type == RuntimeGeometryType::kOrientedRectangle &&
            valid.oriented_rectangle.width > 0.0 &&
            valid.oriented_rectangle.height > 0.0) {
            metadata->has_calibration_domain_valid_rectangle = true;
            metadata->calibration_domain_valid_width_px =
                valid.oriented_rectangle.width;
            metadata->calibration_domain_valid_height_px =
                valid.oriented_rectangle.height;
        }
    }
}

nlohmann::json calibration_domain_geometry_json(
    const SpatialLayoutCalibrationImageSetMetadata& metadata,
    bool valid_geometry)
{
    if (metadata.calibration_domain_shape == "circle") {
        const double cx = valid_geometry && metadata.has_calibration_domain_valid_circle
                              ? metadata.calibration_domain_valid_center_x_px
                              : metadata.calibration_domain_center_x_px;
        const double cy = valid_geometry && metadata.has_calibration_domain_valid_circle
                              ? metadata.calibration_domain_valid_center_y_px
                              : metadata.calibration_domain_center_y_px;
        const double r = valid_geometry && metadata.has_calibration_domain_valid_circle
                             ? metadata.calibration_domain_valid_radius_px
                             : metadata.calibration_domain_radius_px;
        return {
            {"type", "circle"},
            {"cx", cx},
            {"cy", cy},
            {"r", r}
        };
    }

    if (metadata.calibration_domain_shape == "oriented_rectangle") {
        const double width =
            valid_geometry && metadata.has_calibration_domain_valid_rectangle
                ? metadata.calibration_domain_valid_width_px
                : metadata.calibration_domain_width_px;
        const double height =
            valid_geometry && metadata.has_calibration_domain_valid_rectangle
                ? metadata.calibration_domain_valid_height_px
                : metadata.calibration_domain_height_px;
        return {
            {"type", "oriented_rectangle"},
            {"cx", metadata.calibration_domain_center_x_px},
            {"cy", metadata.calibration_domain_center_y_px},
            {"width", width},
            {"height", height},
            {"rotation_deg_clockwise",
             metadata.calibration_domain_rotation_deg_clockwise}
        };
    }

    return nlohmann::json::object();
}

nlohmann::json calibration_domain_observation_json(
    const SpatialLayoutCalibrationImageSetMetadata& metadata,
    const std::string& target_plane)
{
    if (!metadata.has_calibration_domain ||
        (metadata.calibration_domain_shape != "circle" &&
         metadata.calibration_domain_shape != "oriented_rectangle")) {
        return nlohmann::json::object();
    }

    nlohmann::json domain = {
        {"shape", metadata.calibration_domain_shape},
        {"source", metadata.calibration_domain_source},
        {"target_plane", metadata_or_unknown(target_plane)},
        {"coordinate_space", metadata.calibration_domain_coordinate_space},
        {"outer_geometry", calibration_domain_geometry_json(metadata, false)},
        {"edge_margin_px", metadata.calibration_domain_edge_margin_px},
        {"centroid_gate_outset_px",
         metadata.calibration_domain_centroid_gate_outset_px}
    };

    if (metadata.calibration_domain_shape == "circle") {
        domain["center_px"] = {
            metadata.calibration_domain_center_x_px,
            metadata.calibration_domain_center_y_px
        };
        domain["radius_px"] = metadata.calibration_domain_radius_px;
        if (metadata.has_calibration_domain_valid_circle) {
            domain["valid_geometry"] =
                calibration_domain_geometry_json(metadata, true);
        }
    } else if (metadata.calibration_domain_shape == "oriented_rectangle") {
        domain["center_px"] = {
            metadata.calibration_domain_center_x_px,
            metadata.calibration_domain_center_y_px
        };
        domain["width_px"] = metadata.calibration_domain_width_px;
        domain["height_px"] = metadata.calibration_domain_height_px;
        domain["rotation_deg_clockwise"] =
            metadata.calibration_domain_rotation_deg_clockwise;
        if (metadata.has_calibration_domain_valid_rectangle) {
            domain["valid_geometry"] =
                calibration_domain_geometry_json(metadata, true);
        }
    }

    return domain;
}

bool should_attach_observed_domain_for_target_plane(const std::string& target_plane)
{
    return target_plane == "tank_bottom_inner_surface" ||
           target_plane == "tank_bottom_outer_surface" ||
           target_plane == "estimated_fish_plane" ||
           target_plane == "dish_top_rim";
}

}  // namespace

void attach_calibration_domain_observation(
    orange::calibration::CalibrationImageSetRequest* request,
    const SpatialLayoutCalibrationImageSetMetadata& metadata)
{
    if (request == nullptr) {
        return;
    }
    if (!should_attach_observed_domain_for_target_plane(request->target_plane)) {
        return;
    }

    const nlohmann::json domain =
        calibration_domain_observation_json(metadata, request->target_plane);
    if (domain.empty()) {
        return;
    }
    if (!request->observations.is_object()) {
        request->observations = nlohmann::json::object();
    }
    request->observations["calibration_domain"] = domain;
    request->observations["observed_domain"] = domain;

    if (request->purpose == "homography_grid") {
        request->observations["homography_fit_intent"] = {
            {"authority", "citrus_fits_and_accepts"},
            {"target_plane", request->target_plane},
            {"domain_shape", domain.value("shape", "unknown")},
            {"expected_destination_coordinate_space", "final_display_canvas_px"},
            {"orange_role", "image_acquisition_and_camera_space_observation"}
        };
    }
}

void attach_projection_surface_authored_domain_hint(
    orange::calibration::CalibrationImageSetRequest* request)
{
    if (request == nullptr || request->target_plane != "projected_surface") {
        return;
    }
    if (!request->observations.is_object()) {
        request->observations = nlohmann::json::object();
    }
    request->observations["authored_domain"] = {
        {"shape", "oriented_rectangle"},
        {"source", "operator_selected_projection_surface_default"},
        {"target_plane", "projected_surface"},
        {"coordinate_space", "final_display_canvas_px"},
        {"geometry_available", false},
        {"authority", "citrus_provides_geometry"}
    };
}

void attach_runtime_role_metadata(
    orange::calibration::CalibrationImageSetRequest* request)
{
    if (request == nullptr || request->target_plane != "tank_bottom_inner_surface") {
        return;
    }
    request->runtime_role = {
        {"role", "behavior_plane_proxy"},
        {"behavior_plane_id", "estimated_fish_plane"},
        {"source", "fallback_to_tank_bottom_inner_surface"},
        {"authority", "citrus_decides_runtime_application"}
    };
}

void apply_capture_stage_metadata_to_request(
    orange::calibration::CalibrationImageSetRequest* request,
    const SpatialLayoutCalibrationImageSetMetadata& metadata)
{
    if (request == nullptr) {
        return;
    }
    request->capture_stage = metadata.capture_stage;
    request->plane_z_mm_nominal = metadata.plane_z_mm_nominal;
    request->has_plane_z_mm_nominal = metadata.has_plane_z_mm_nominal;
    request->plane_z_mm_uncertainty = metadata.plane_z_mm_uncertainty;
    request->has_plane_z_mm_uncertainty = metadata.has_plane_z_mm_uncertainty;
    request->wet_or_dry = metadata.wet_or_dry;
    request->imaging_shelf_installed = metadata.imaging_shelf_installed;
    request->has_imaging_shelf_installed = true;
    request->dish_installed = metadata.dish_installed;
    request->has_dish_installed = true;
    request->dish_id = metadata.dish_id;
    request->water_fill_mm = metadata.water_fill_mm;
    request->has_water_fill_mm = metadata.has_water_fill_mm;
    request->fill_state = metadata.fill_state;
    request->open_water_surface_present = metadata.open_water_surface_present;
    request->has_open_water_surface_present = true;
    request->water_settled_status = metadata.water_settled_status;
    request->target_method = metadata.target_method;
    request->pattern_type = metadata.pattern_type;
    request->pattern_domain = metadata.pattern_domain;
    request->matched_parity_group_id = metadata.matched_parity_group_id;
    request->parity_group_id = metadata.matched_parity_group_id;
    request->parity_group_role = metadata.parity_group_role;
    request->reference_only = metadata.reference_only;
    request->has_reference_only = true;
    request->physical_target_used = metadata.physical_target_used;
    request->has_physical_target_used = true;
    request->projected_pattern_used_as_coordinate_target =
        metadata.projected_pattern_used_as_coordinate_target;
    request->has_projected_pattern_used_as_coordinate_target = true;
    request->plane_id = metadata.plane_id;
    request->z_mm_relative_to_projection_surface =
        metadata.z_mm_relative_to_projection_surface;
    request->has_z_mm_relative_to_projection_surface =
        metadata.has_z_mm_relative_to_projection_surface;
    request->target_id = metadata.target_id;
    request->target_design = metadata.target_design;
    request->physical_target_grid_spacing_mm =
        metadata.physical_target_grid_spacing_mm;
    request->has_physical_target_grid_spacing_mm =
        metadata.has_physical_target_grid_spacing_mm;
    request->physical_target_origin_definition =
        metadata.physical_target_origin_definition;
    request->physical_target_x_orientation_marker_definition =
        metadata.physical_target_x_orientation_marker_definition;
    if (metadata.physical_target_used) {
        request->physical_target = {
            {"target_id", metadata_or_unknown(metadata.target_id)},
            {"target_design", metadata_or_unknown(metadata.target_design)},
            {"origin_definition", metadata_or_unknown(metadata.physical_target_origin_definition)},
            {"x_orientation_marker_definition",
             metadata_or_unknown(metadata.physical_target_x_orientation_marker_definition)}
        };
        if (metadata.has_physical_target_grid_spacing_mm) {
            request->physical_target["grid_spacing_mm"] =
                metadata.physical_target_grid_spacing_mm;
        }
    }
}

SpatialLayoutCalibrationImageSetMetadata make_calibration_image_set_metadata_from_ui(
    const SpatialLayoutUiState& ui_state)
{
    SpatialLayoutCalibrationImageSetMetadata metadata;
    metadata.filter_state = ui_state.calibration_filter_state;
    metadata.runtime_filter_state = ui_state.calibration_runtime_filter_state;
    metadata.light_handling = ui_state.calibration_light_handling;
    metadata.light_state = ui_state.calibration_light_state;
    metadata.illumination_spectrum = ui_state.calibration_illumination_spectrum;
    metadata.illumination_source = ui_state.calibration_illumination_source;
    metadata.illumination_center_wavelength_nm =
        ui_state.calibration_illumination_center_wavelength_nm;
    metadata.has_illumination_center_wavelength_nm =
        ui_state.calibration_has_illumination_center_wavelength_nm;
    metadata.illumination_min_wavelength_nm =
        ui_state.calibration_illumination_min_wavelength_nm;
    metadata.has_illumination_min_wavelength_nm =
        ui_state.calibration_has_illumination_min_wavelength_nm;
    metadata.illumination_max_wavelength_nm =
        ui_state.calibration_illumination_max_wavelength_nm;
    metadata.has_illumination_max_wavelength_nm =
        ui_state.calibration_has_illumination_max_wavelength_nm;
    metadata.illumination_bandwidth_fwhm_nm =
        ui_state.calibration_illumination_bandwidth_fwhm_nm;
    metadata.has_illumination_bandwidth_fwhm_nm =
        ui_state.calibration_has_illumination_bandwidth_fwhm_nm;
    metadata.illumination_wavelength_confidence =
        ui_state.calibration_illumination_wavelength_confidence;
    metadata.projector_state = ui_state.calibration_projector_state;
    metadata.projector_visible_to_camera = ui_state.calibration_projector_visible_to_camera;
    metadata.requires_filter_reinstalled_repeatably =
        ui_state.calibration_requires_filter_reinstalled_repeatably;
    metadata.operator_notes = ui_state.calibration_operator_notes;
    metadata.image_set_purpose = ui_state.calibration_image_set_purpose;
    metadata.image_set_target_plane = ui_state.calibration_image_set_target_plane;
    metadata.image_set_image_role = ui_state.calibration_image_set_image_role;
    metadata.image_set_projected_pattern_id =
        ui_state.calibration_image_set_projected_pattern_id;
    metadata.image_set_projected_pattern_type =
        ui_state.calibration_image_set_projected_pattern_type;
    metadata.image_set_scale_target_type = ui_state.calibration_image_set_scale_target_type;
    metadata.image_set_notes = ui_state.calibration_image_set_notes;
    metadata.capture_stage = ui_state.calibration_capture_stage;
    metadata.plane_z_mm_nominal = ui_state.calibration_plane_z_mm_nominal;
    metadata.has_plane_z_mm_nominal = ui_state.calibration_has_plane_z_mm_nominal;
    metadata.plane_z_mm_uncertainty = ui_state.calibration_plane_z_mm_uncertainty;
    metadata.has_plane_z_mm_uncertainty = ui_state.calibration_has_plane_z_mm_uncertainty;
    metadata.wet_or_dry = ui_state.calibration_wet_or_dry;
    metadata.imaging_shelf_installed = ui_state.calibration_imaging_shelf_installed;
    metadata.dish_installed = ui_state.calibration_dish_installed;
    metadata.dish_id = ui_state.calibration_dish_id;
    metadata.water_fill_mm = ui_state.calibration_water_fill_mm;
    metadata.has_water_fill_mm = ui_state.calibration_has_water_fill_mm;
    metadata.fill_state = ui_state.calibration_fill_state;
    metadata.open_water_surface_present = ui_state.calibration_open_water_surface_present;
    metadata.water_settled_status = ui_state.calibration_water_settled_status;
    metadata.target_method = ui_state.calibration_target_method;
    metadata.pattern_type = ui_state.calibration_pattern_type;
    metadata.pattern_domain = ui_state.calibration_pattern_domain;
    metadata.matched_parity_group_id = ui_state.calibration_matched_parity_group_id;
    metadata.parity_group_role = ui_state.calibration_parity_group_role;
    metadata.reference_only = ui_state.calibration_reference_only;
    metadata.physical_target_used = ui_state.calibration_physical_target_used;
    metadata.projected_pattern_used_as_coordinate_target =
        ui_state.calibration_projected_pattern_used_as_coordinate_target;
    metadata.plane_id = ui_state.calibration_plane_id;
    metadata.z_mm_relative_to_projection_surface =
        ui_state.calibration_z_mm_relative_to_projection_surface;
    metadata.has_z_mm_relative_to_projection_surface =
        ui_state.calibration_has_z_mm_relative_to_projection_surface;
    metadata.target_id = ui_state.calibration_target_id;
    metadata.target_design = ui_state.calibration_target_design;
    metadata.physical_target_grid_spacing_mm =
        ui_state.calibration_physical_target_grid_spacing_mm;
    metadata.has_physical_target_grid_spacing_mm =
        ui_state.calibration_has_physical_target_grid_spacing_mm;
    metadata.physical_target_origin_definition =
        ui_state.calibration_physical_target_origin_definition;
    metadata.physical_target_x_orientation_marker_definition =
        ui_state.calibration_physical_target_x_orientation_marker_definition;
    metadata.citrus_projection_snapshot_pre_capture =
        ui_state.captured_citrus_projection_snapshot_pre_capture.is_object()
            ? ui_state.captured_citrus_projection_snapshot_pre_capture
            : nlohmann::json::object();
    metadata.citrus_projection_snapshot_post_capture =
        ui_state.captured_citrus_projection_snapshot_post_capture.is_object()
            ? ui_state.captured_citrus_projection_snapshot_post_capture
            : nlohmann::json::object();
    metadata.citrus_projection_epoch_consistency =
        ui_state.captured_citrus_projection_epoch_consistency.is_object()
            ? ui_state.captured_citrus_projection_epoch_consistency
            : nlohmann::json::object();
    populate_calibration_domain_metadata_from_runtime(&metadata, ui_state);
    return metadata;
}

void apply_calibration_image_set_metadata_to_ui(
    SpatialLayoutUiState* ui_state,
    const SpatialLayoutCalibrationImageSetMetadata& metadata)
{
    if (ui_state == nullptr) {
        return;
    }
    ui_state->calibration_filter_state = metadata.filter_state;
    ui_state->calibration_runtime_filter_state = metadata.runtime_filter_state;
    ui_state->calibration_light_handling = metadata.light_handling;
    ui_state->calibration_light_state = metadata.light_state;
    ui_state->calibration_illumination_spectrum = metadata.illumination_spectrum;
    ui_state->calibration_illumination_source = metadata.illumination_source;
    ui_state->calibration_illumination_center_wavelength_nm =
        metadata.illumination_center_wavelength_nm;
    ui_state->calibration_has_illumination_center_wavelength_nm =
        metadata.has_illumination_center_wavelength_nm;
    ui_state->calibration_illumination_min_wavelength_nm =
        metadata.illumination_min_wavelength_nm;
    ui_state->calibration_has_illumination_min_wavelength_nm =
        metadata.has_illumination_min_wavelength_nm;
    ui_state->calibration_illumination_max_wavelength_nm =
        metadata.illumination_max_wavelength_nm;
    ui_state->calibration_has_illumination_max_wavelength_nm =
        metadata.has_illumination_max_wavelength_nm;
    ui_state->calibration_illumination_bandwidth_fwhm_nm =
        metadata.illumination_bandwidth_fwhm_nm;
    ui_state->calibration_has_illumination_bandwidth_fwhm_nm =
        metadata.has_illumination_bandwidth_fwhm_nm;
    ui_state->calibration_illumination_wavelength_confidence =
        metadata.illumination_wavelength_confidence;
    ui_state->calibration_projector_state = metadata.projector_state;
    ui_state->calibration_projector_visible_to_camera = metadata.projector_visible_to_camera;
    ui_state->calibration_requires_filter_reinstalled_repeatably =
        metadata.requires_filter_reinstalled_repeatably;
    ui_state->calibration_operator_notes = metadata.operator_notes;
    ui_state->calibration_image_set_purpose = metadata.image_set_purpose;
    ui_state->calibration_image_set_target_plane = metadata.image_set_target_plane;
    ui_state->calibration_image_set_image_role = metadata.image_set_image_role;
    ui_state->calibration_image_set_projected_pattern_id =
        metadata.image_set_projected_pattern_id;
    ui_state->calibration_image_set_projected_pattern_type =
        metadata.image_set_projected_pattern_type;
    ui_state->calibration_image_set_scale_target_type = metadata.image_set_scale_target_type;
    ui_state->calibration_image_set_notes = metadata.image_set_notes;
    ui_state->calibration_capture_stage = metadata.capture_stage;
    ui_state->calibration_plane_z_mm_nominal = metadata.plane_z_mm_nominal;
    ui_state->calibration_has_plane_z_mm_nominal = metadata.has_plane_z_mm_nominal;
    ui_state->calibration_plane_z_mm_uncertainty = metadata.plane_z_mm_uncertainty;
    ui_state->calibration_has_plane_z_mm_uncertainty = metadata.has_plane_z_mm_uncertainty;
    ui_state->calibration_wet_or_dry = metadata.wet_or_dry;
    ui_state->calibration_imaging_shelf_installed = metadata.imaging_shelf_installed;
    ui_state->calibration_dish_installed = metadata.dish_installed;
    ui_state->calibration_dish_id = metadata.dish_id;
    ui_state->calibration_water_fill_mm = metadata.water_fill_mm;
    ui_state->calibration_has_water_fill_mm = metadata.has_water_fill_mm;
    ui_state->calibration_fill_state = metadata.fill_state;
    ui_state->calibration_open_water_surface_present = metadata.open_water_surface_present;
    ui_state->calibration_water_settled_status = metadata.water_settled_status;
    ui_state->calibration_target_method = metadata.target_method;
    ui_state->calibration_pattern_type = metadata.pattern_type;
    ui_state->calibration_pattern_domain = metadata.pattern_domain;
    ui_state->calibration_matched_parity_group_id = metadata.matched_parity_group_id;
    ui_state->calibration_parity_group_role = metadata.parity_group_role;
    ui_state->calibration_reference_only = metadata.reference_only;
    ui_state->calibration_physical_target_used = metadata.physical_target_used;
    ui_state->calibration_projected_pattern_used_as_coordinate_target =
        metadata.projected_pattern_used_as_coordinate_target;
    ui_state->calibration_plane_id = metadata.plane_id;
    ui_state->calibration_z_mm_relative_to_projection_surface =
        metadata.z_mm_relative_to_projection_surface;
    ui_state->calibration_has_z_mm_relative_to_projection_surface =
        metadata.has_z_mm_relative_to_projection_surface;
    ui_state->calibration_target_id = metadata.target_id;
    ui_state->calibration_target_design = metadata.target_design;
    ui_state->calibration_physical_target_grid_spacing_mm =
        metadata.physical_target_grid_spacing_mm;
    ui_state->calibration_has_physical_target_grid_spacing_mm =
        metadata.has_physical_target_grid_spacing_mm;
    ui_state->calibration_physical_target_origin_definition =
        metadata.physical_target_origin_definition;
    ui_state->calibration_physical_target_x_orientation_marker_definition =
        metadata.physical_target_x_orientation_marker_definition;
    ui_state->captured_citrus_projection_snapshot_pre_capture =
        metadata.citrus_projection_snapshot_pre_capture.is_object()
            ? metadata.citrus_projection_snapshot_pre_capture
            : nlohmann::json::object();
    ui_state->captured_citrus_projection_snapshot_post_capture =
        metadata.citrus_projection_snapshot_post_capture.is_object()
            ? metadata.citrus_projection_snapshot_post_capture
            : nlohmann::json::object();
    ui_state->captured_citrus_projection_epoch_consistency =
        metadata.citrus_projection_epoch_consistency.is_object()
            ? metadata.citrus_projection_epoch_consistency
            : nlohmann::json::object();
}

}  // namespace orange::gui::spatial_layout
