#include "gui/spatial_layout/calibration_workflow.h"

#include "gui/spatial_layout/metadata_panel.h"
#include "gui/spatial_layout/session_io.h"
#include "imgui.h"
#include "project.h"

#include <algorithm>
#include <sstream>

namespace orange::gui::spatial_layout {
namespace {

constexpr const char* kHoyaR72FilterInstalled =
    "installed: HOYA Creative Filter Infrared R72 67 mm (Kenko Tokina)";
constexpr const char* kHoyaR72FilterRemoved =
    "removed: HOYA Creative Filter Infrared R72 67 mm (Kenko Tokina)";
constexpr const char* kAcrylicPhysicalTargetId =
    "acrylic_hole_target_78mm_pitch5_margin3_v002";
constexpr const char* kAcrylicPhysicalTargetDesign =
    "opaque_acrylic_hole_mask_78mm_pitch5_margin3_v002";
constexpr const char* kDryPhysicalTargetHeightParallaxDiagnosticPurpose =
    "dry_physical_target_height_parallax_diagnostic";

void ensure_wet_runtime_stack_parity_group_id(SpatialLayoutUiState* ui_state)
{
    if (ui_state == nullptr || !ui_state->calibration_matched_parity_group_id.empty()) {
        return;
    }
    std::ostringstream id;
    id << "wet_runtime_stack_" << sanitize_artifact_component(get_current_utc_timestamp());
    if (ui_state->citrus_template.available &&
        !ui_state->citrus_template.source_canvas_name.empty()) {
        id << "_" << sanitize_artifact_component(ui_state->citrus_template.source_canvas_name);
    }
    if (ui_state->citrus_template.available &&
        !ui_state->citrus_template.source_arena_name.empty()) {
        id << "_" << sanitize_artifact_component(ui_state->citrus_template.source_arena_name);
    }
    ui_state->calibration_matched_parity_group_id = id.str();
}

void ensure_holder_installed_parity_group_id(SpatialLayoutUiState* ui_state)
{
    if (ui_state == nullptr || !ui_state->calibration_matched_parity_group_id.empty()) {
        return;
    }
    std::ostringstream id;
    id << "holder_installed_" << sanitize_artifact_component(get_current_utc_timestamp());
    if (ui_state->citrus_template.available &&
        !ui_state->citrus_template.source_canvas_name.empty()) {
        id << "_" << sanitize_artifact_component(ui_state->citrus_template.source_canvas_name);
    }
    if (ui_state->citrus_template.available &&
        !ui_state->citrus_template.source_arena_name.empty()) {
        id << "_" << sanitize_artifact_component(ui_state->citrus_template.source_arena_name);
    }
    ui_state->calibration_matched_parity_group_id = id.str();
}

void ensure_camera_physical_parity_group_id(SpatialLayoutUiState* ui_state)
{
    if (ui_state == nullptr || !ui_state->calibration_matched_parity_group_id.empty()) {
        return;
    }
    std::ostringstream id;
    id << "camera_physical_planes_" << sanitize_artifact_component(get_current_utc_timestamp());
    if (ui_state->citrus_template.available &&
        !ui_state->citrus_template.source_canvas_name.empty()) {
        id << "_" << sanitize_artifact_component(ui_state->citrus_template.source_canvas_name);
    }
    if (ui_state->citrus_template.available &&
        !ui_state->citrus_template.source_arena_name.empty()) {
        id << "_" << sanitize_artifact_component(ui_state->citrus_template.source_arena_name);
    }
    ui_state->calibration_matched_parity_group_id = id.str();
}

void ensure_projected_surface_scale_group_id(SpatialLayoutUiState* ui_state)
{
    if (ui_state == nullptr || !ui_state->calibration_matched_parity_group_id.empty()) {
        return;
    }
    std::ostringstream id;
    id << "projected_surface_scale_"
       << sanitize_artifact_component(get_current_utc_timestamp());
    if (ui_state->citrus_template.available &&
        !ui_state->citrus_template.source_canvas_name.empty()) {
        id << "_" << sanitize_artifact_component(
            ui_state->citrus_template.source_canvas_name);
    }
    ui_state->calibration_matched_parity_group_id = id.str();
}

void apply_projected_surface_scale_calibration_defaults(
    SpatialLayoutUiState* ui_state)
{
    if (ui_state == nullptr) {
        return;
    }
    // Physical-scale commissioning is valid both on the unobstructed dry
    // surface and on the installed holder's operational diffuser surface.
    // Preserve the selected fixture profile while applying the common target
    // and plane contract; otherwise choosing this purpose silently relabels a
    // holder-installed capture as holder_removed.
    const bool holder_installed =
        ui_state->calibration_workflow_profile_id ==
        "holder_installed_projected_surface";
    const std::string fixture_visibility_shape =
        ui_state->calibration_visibility_domain_shape;
    ui_state->calibration_matched_parity_group_id.clear();
    ensure_projected_surface_scale_group_id(ui_state);
    ui_state->calibration_capture_stage =
        "projected_surface_physical_scale_commissioning";
    ui_state->calibration_workflow_profile_id =
        "unobstructed_canvas_commissioning";
    ui_state->calibration_fixture_state = "holder_removed";
    ui_state->calibration_homography_role = "not_applicable";
    ui_state->calibration_visibility_domain_id =
        "unobstructed_arena_rectangle";
    ui_state->calibration_visibility_domain_shape = "rectangle";
    ui_state->calibration_visibility_domain_geometry_status = "not_embedded";
    ui_state->calibration_image_set_purpose =
        "projected_surface_scale_calibration";
    ui_state->calibration_image_set_target_plane = "projected_surface";
    ui_state->calibration_image_set_image_role = "physical_target";
    ui_state->calibration_image_set_projected_pattern_id = "none";
    ui_state->calibration_image_set_projected_pattern_type = "none";
    ui_state->calibration_image_set_scale_target_type =
        "known_physical_xy_target";
    ui_state->calibration_filter_state = kHoyaR72FilterRemoved;
    ui_state->calibration_runtime_filter_state = kHoyaR72FilterInstalled;
    ui_state->calibration_light_handling = "suppress_mapped_strobe";
    apply_illumination_preset(ui_state, "visible_projector_broadband");
    ui_state->calibration_projector_state = "uniform_gray_illumination";
    ui_state->calibration_projector_visible_to_camera = true;
    ui_state->calibration_wet_or_dry = "dry";
    ui_state->calibration_imaging_shelf_installed = false;
    ui_state->calibration_dish_installed = false;
    ui_state->calibration_dish_id.clear();
    ui_state->calibration_fill_state = "dry_or_empty";
    ui_state->calibration_has_water_fill_mm = false;
    ui_state->calibration_open_water_surface_present = false;
    ui_state->calibration_water_settled_status = "not_applicable";
    ui_state->calibration_target_method = "physical_target_known_xy";
    ui_state->calibration_pattern_type = "physical_point_set";
    ui_state->calibration_pattern_domain = "full_projected_surface";
    ui_state->calibration_parity_group_role = "projected_surface_scale";
    ui_state->calibration_reference_only = false;
    ui_state->calibration_physical_target_used = true;
    ui_state->calibration_projected_pattern_used_as_coordinate_target = false;
    ui_state->calibration_plane_id = "projected_surface_physical";
    ui_state->calibration_plane_z_mm_nominal = 0.0;
    ui_state->calibration_has_plane_z_mm_nominal = true;
    ui_state->calibration_plane_z_mm_uncertainty = 0.0;
    ui_state->calibration_has_plane_z_mm_uncertainty = false;
    ui_state->calibration_z_mm_relative_to_projection_surface = 0.0;
    ui_state->calibration_has_z_mm_relative_to_projection_surface = true;
    ui_state->calibration_target_id = kAcrylicPhysicalTargetId;
    ui_state->calibration_target_design = kAcrylicPhysicalTargetDesign;
    ui_state->calibration_physical_target_grid_spacing_mm = 5.0;
    ui_state->calibration_has_physical_target_grid_spacing_mm = true;
    ui_state->calibration_physical_target_origin_definition =
        "center of large center marker C";
    ui_state->calibration_physical_target_x_orientation_marker_definition =
        "positive X from C toward larger XPLUS orientation marker";
    ui_state->calibration_image_set_notes =
        "Authoritative projected-surface scale observation input. Orange detects "
        "physical hole centers and writes correspondences/QC; Citrus refits and owns "
        "candidate promotion. The target is 3 mm thick, while transmitted hole "
        "centers locate the illuminated projected surface at z=0.";
    if (holder_installed) {
        ui_state->calibration_workflow_profile_id =
            "holder_installed_projected_surface";
        ui_state->calibration_fixture_state = "holder_installed_dish_absent";
        ui_state->calibration_visibility_domain_id = "imaging_shelf_aperture";
        ui_state->calibration_visibility_domain_shape =
            fixture_visibility_shape.empty() ? "circle" : fixture_visibility_shape;
        ui_state->calibration_visibility_domain_geometry_status = "unmeasured";
        ui_state->calibration_imaging_shelf_installed = true;
        ui_state->calibration_image_set_notes +=
            " The holder is installed and its flattened diffuser gel is the "
            "operational projected surface; dishes and water are absent.";
    }
}

void apply_physical_target_common_defaults(
    SpatialLayoutUiState* ui_state,
    const std::string& capture_stage,
    const std::string& target_plane,
    const std::string& plane_id,
    const std::string& parity_role,
    const double z_mm,
    const double z_uncertainty_mm)
{
    if (ui_state == nullptr) {
        return;
    }
    ensure_camera_physical_parity_group_id(ui_state);
    ui_state->calibration_capture_stage = capture_stage;
    ui_state->calibration_workflow_profile_id = "camera_physical_planes";
    ui_state->calibration_fixture_state = "holder_installed_dish_installed";
    ui_state->calibration_homography_role = "not_applicable";
    ui_state->calibration_visibility_domain_id = "imaging_shelf_aperture";
    ui_state->calibration_visibility_domain_shape = "circle";
    ui_state->calibration_visibility_domain_geometry_status = "unmeasured";
    ui_state->calibration_image_set_purpose =
        kDryPhysicalTargetHeightParallaxDiagnosticPurpose;
    ui_state->calibration_image_set_target_plane = target_plane;
    ui_state->calibration_image_set_image_role = "physical_target";
    ui_state->calibration_image_set_projected_pattern_id = "none";
    ui_state->calibration_image_set_projected_pattern_type = "none";
    ui_state->calibration_image_set_scale_target_type = "known_physical_xy_target";
    ui_state->calibration_filter_state = kHoyaR72FilterInstalled;
    ui_state->calibration_runtime_filter_state = kHoyaR72FilterInstalled;
    ui_state->calibration_light_handling = "keep_or_restore_mapped_pulse";
    apply_illumination_preset(ui_state, "custom_ttl_nir_strobe_855nm");
    ui_state->calibration_projector_state = "off";
    ui_state->calibration_projector_visible_to_camera = false;
    ui_state->calibration_wet_or_dry = "dry";
    ui_state->calibration_imaging_shelf_installed = true;
    ui_state->calibration_dish_installed = true;
    ui_state->calibration_fill_state = "dry_or_empty";
    ui_state->calibration_has_water_fill_mm = false;
    ui_state->calibration_open_water_surface_present = false;
    ui_state->calibration_water_settled_status = "not_applicable";
    ui_state->calibration_target_method = "physical_target_known_xy";
    ui_state->calibration_pattern_type = "physical_grid";
    ui_state->calibration_pattern_domain = "circular_experimental_domain";
    ui_state->calibration_parity_group_role = parity_role;
    ui_state->calibration_reference_only = true;
    ui_state->calibration_physical_target_used = true;
    ui_state->calibration_projected_pattern_used_as_coordinate_target = false;
    ui_state->calibration_plane_id = plane_id;
    ui_state->calibration_plane_z_mm_nominal = z_mm;
    ui_state->calibration_has_plane_z_mm_nominal = true;
    ui_state->calibration_plane_z_mm_uncertainty = std::max(0.0, z_uncertainty_mm);
    ui_state->calibration_has_plane_z_mm_uncertainty = true;
    ui_state->calibration_z_mm_relative_to_projection_surface = z_mm;
    ui_state->calibration_has_z_mm_relative_to_projection_surface = true;
    ui_state->calibration_target_id = kAcrylicPhysicalTargetId;
    ui_state->calibration_target_design = kAcrylicPhysicalTargetDesign;
    ui_state->calibration_physical_target_grid_spacing_mm = 5.0;
    ui_state->calibration_has_physical_target_grid_spacing_mm = true;
    ui_state->calibration_physical_target_origin_definition =
        "center of large center marker C";
    ui_state->calibration_physical_target_x_orientation_marker_definition =
        "positive X from C toward larger XPLUS orientation marker";
    ui_state->calibration_image_set_notes =
        "Dry camera/lens height-parallax diagnostic only; no water path is included, "
        "fiducial detection is currently operator-reported unreliable, and this capture "
        "is not runtime-correction ready.";
}

void apply_camera_physical_projected_surface_defaults(SpatialLayoutUiState* ui_state)
{
    apply_physical_target_common_defaults(
        ui_state,
        "camera_physical_projected_surface",
        "projected_surface",
        "projected_surface_physical",
        "physical_projected_surface",
        0.0,
        0.5);
}

void apply_camera_physical_dish_base_defaults(SpatialLayoutUiState* ui_state)
{
    apply_physical_target_common_defaults(
        ui_state,
        "camera_physical_dish_base_inner_surface",
        "tank_bottom_inner_surface",
        "dish_base_inner_surface_physical",
        "physical_dish_base",
        8.0,
        0.5);
}

void apply_camera_physical_fish_height_defaults(SpatialLayoutUiState* ui_state)
{
    apply_physical_target_common_defaults(
        ui_state,
        "camera_physical_fish_height",
        "estimated_fish_plane",
        "fish_height_physical_assumed",
        "physical_fish_height",
        9.5,
        0.5);
}

void apply_dry_projection_surface_stage_defaults(SpatialLayoutUiState* ui_state)
{
    if (ui_state == nullptr) {
        return;
    }
    ui_state->calibration_capture_stage = "projected_surface_dry_reference";
    ui_state->calibration_workflow_profile_id = "unobstructed_canvas_commissioning";
    ui_state->calibration_fixture_state = "holder_removed";
    ui_state->calibration_homography_role = "commissioning_reference";
    ui_state->calibration_visibility_domain_id = "unobstructed_arena_rectangle";
    ui_state->calibration_visibility_domain_shape = "rectangle";
    ui_state->calibration_visibility_domain_geometry_status = "not_embedded";
    ui_state->calibration_image_set_target_plane = "projected_surface";
    ui_state->calibration_wet_or_dry = "dry";
    ui_state->calibration_imaging_shelf_installed = false;
    ui_state->calibration_dish_installed = false;
    ui_state->calibration_fill_state = "dry_or_empty";
    ui_state->calibration_has_water_fill_mm = false;
    ui_state->calibration_open_water_surface_present = false;
    ui_state->calibration_water_settled_status = "not_applicable";
    ui_state->calibration_target_method = "projected_pattern_on_diffuser";
    ui_state->calibration_pattern_domain = "full_projected_surface";
    ui_state->calibration_matched_parity_group_id.clear();
    ui_state->calibration_parity_group_role = "dry_reference";
    ui_state->calibration_reference_only = true;
    ui_state->calibration_physical_target_used = false;
    ui_state->calibration_projected_pattern_used_as_coordinate_target = true;
    ui_state->calibration_plane_id = "projector_surface";
}

void apply_holder_installed_projection_surface_stage_defaults(
    SpatialLayoutUiState* ui_state)
{
    if (ui_state == nullptr) {
        return;
    }
    ensure_holder_installed_parity_group_id(ui_state);
    ui_state->calibration_capture_stage = "projected_surface_holder_installed";
    ui_state->calibration_workflow_profile_id = "holder_installed_projected_surface";
    ui_state->calibration_fixture_state = "holder_installed_dish_absent";
    ui_state->calibration_homography_role = "operational_candidate";
    ui_state->calibration_visibility_domain_id = "imaging_shelf_aperture";
    ui_state->calibration_visibility_domain_shape = "circle";
    ui_state->calibration_visibility_domain_geometry_status = "unmeasured";
    ui_state->calibration_image_set_target_plane = "projected_surface";
    ui_state->calibration_wet_or_dry = "dry";
    ui_state->calibration_imaging_shelf_installed = true;
    ui_state->calibration_dish_installed = false;
    ui_state->calibration_dish_id.clear();
    ui_state->calibration_fill_state = "dry_or_empty";
    ui_state->calibration_has_water_fill_mm = false;
    ui_state->calibration_open_water_surface_present = false;
    ui_state->calibration_water_settled_status = "not_applicable";
    ui_state->calibration_target_method = "projected_pattern_on_diffuser";
    ui_state->calibration_pattern_domain = "circular_experimental_domain";
    ui_state->calibration_parity_group_role = "holder_installed_projected_surface";
    ui_state->calibration_reference_only = false;
    ui_state->calibration_physical_target_used = false;
    ui_state->calibration_projected_pattern_used_as_coordinate_target = true;
    ui_state->calibration_plane_id = "projector_surface";
}

void apply_wet_projection_surface_stage_defaults(SpatialLayoutUiState* ui_state)
{
    if (ui_state == nullptr) {
        return;
    }
    ensure_wet_runtime_stack_parity_group_id(ui_state);
    ui_state->calibration_capture_stage = "projected_surface_wet_runtime_stack";
    ui_state->calibration_workflow_profile_id = "wet_tank_projected_surface";
    ui_state->calibration_fixture_state = "holder_installed_dish_installed";
    ui_state->calibration_homography_role = "operational_candidate";
    ui_state->calibration_visibility_domain_id = "imaging_shelf_aperture";
    ui_state->calibration_visibility_domain_shape = "circle";
    ui_state->calibration_visibility_domain_geometry_status = "unmeasured";
    ui_state->calibration_image_set_target_plane = "projected_surface";
    ui_state->calibration_wet_or_dry = "wet";
    ui_state->calibration_imaging_shelf_installed = true;
    ui_state->calibration_dish_installed = true;
    ui_state->calibration_fill_state = "recording_fill_level";
    ui_state->calibration_open_water_surface_present = true;
    ui_state->calibration_water_settled_status = "settled";
    ui_state->calibration_target_method = "projected_pattern_on_diffuser";
    ui_state->calibration_pattern_domain = "circular_experimental_domain";
    ui_state->calibration_parity_group_role = "wet_projected_surface";
    ui_state->calibration_reference_only = false;
    ui_state->calibration_physical_target_used = false;
    ui_state->calibration_projected_pattern_used_as_coordinate_target = true;
    ui_state->calibration_plane_id = "projector_surface";
}

void apply_dish_top_observation_stage_defaults(SpatialLayoutUiState* ui_state)
{
    if (ui_state == nullptr) {
        return;
    }
    ui_state->calibration_capture_stage = "dish_top_observation";
    ui_state->calibration_workflow_profile_id = "installed_tank_registration";
    ui_state->calibration_fixture_state = "holder_installed_dish_installed";
    ui_state->calibration_homography_role = "not_applicable";
    ui_state->calibration_visibility_domain_id = "imaging_shelf_aperture";
    ui_state->calibration_visibility_domain_shape = "circle";
    ui_state->calibration_visibility_domain_geometry_status = "unmeasured";
    ui_state->calibration_image_set_target_plane = "dish_top_rim";
    ui_state->calibration_wet_or_dry = "wet";
    ui_state->calibration_imaging_shelf_installed = true;
    ui_state->calibration_dish_installed = true;
    ui_state->calibration_fill_state = "recording_fill_level";
    ui_state->calibration_open_water_surface_present = true;
    ui_state->calibration_water_settled_status = "settled";
    ui_state->calibration_target_method = "physical_target";
    ui_state->calibration_pattern_type = "not_applicable";
    ui_state->calibration_pattern_domain = "circular_experimental_domain";
    ui_state->calibration_parity_group_role.clear();
    ui_state->calibration_reference_only = false;
}

void apply_common_visible_projection_defaults(SpatialLayoutUiState* ui_state)
{
    if (ui_state == nullptr) {
        return;
    }
    ui_state->calibration_filter_state = kHoyaR72FilterRemoved;
    ui_state->calibration_runtime_filter_state = kHoyaR72FilterInstalled;
    ui_state->calibration_light_handling = "suppress_mapped_strobe";
    apply_illumination_preset(ui_state, "visible_projector_broadband");
    ui_state->calibration_projector_visible_to_camera = true;
}

void apply_scale_task_defaults(SpatialLayoutUiState* ui_state)
{
    if (ui_state == nullptr) {
        return;
    }
    ui_state->calibration_image_set_purpose = "scale_image";
    ui_state->calibration_image_set_image_role = "scale_target";
    ui_state->calibration_image_set_projected_pattern_id = "none";
    ui_state->calibration_image_set_projected_pattern_type = "none";
    ui_state->calibration_image_set_scale_target_type = "clear_plastic_ruler";
    ui_state->calibration_filter_state = kHoyaR72FilterInstalled;
    ui_state->calibration_runtime_filter_state = kHoyaR72FilterInstalled;
    ui_state->calibration_light_handling = "keep_or_restore_mapped_pulse";
    apply_illumination_preset(ui_state, "custom_ttl_nir_strobe_855nm");
    ui_state->calibration_projector_state = "off";
    ui_state->calibration_projector_visible_to_camera = false;
    ui_state->calibration_target_method = "ruler_only";
    ui_state->calibration_pattern_type = "ruler";
    ui_state->calibration_homography_role = "validation_only";
}

void apply_rings_homography_task_defaults(
    SpatialLayoutUiState* ui_state,
    const std::string& pattern_id,
    const std::string& projector_state)
{
    if (ui_state == nullptr) {
        return;
    }
    ui_state->calibration_image_set_purpose = "homography_grid";
    ui_state->calibration_image_set_image_role = "grid_on";
    ui_state->calibration_image_set_projected_pattern_id = pattern_id;
    ui_state->calibration_image_set_projected_pattern_type = "circular_rings";
    ui_state->calibration_image_set_scale_target_type = "unknown";
    ui_state->calibration_pattern_type = "circular_rings";
    ui_state->calibration_target_method = "projected_pattern_on_diffuser";
    if (ui_state->calibration_capture_stage ==
        "projected_surface_holder_installed") {
        ui_state->calibration_homography_role = "validation_only";
    } else {
        ui_state->calibration_homography_role =
            ui_state->calibration_capture_stage == "projected_surface_dry_reference"
                ? "commissioning_reference"
                : "operational_candidate";
    }
    apply_common_visible_projection_defaults(ui_state);
    ui_state->calibration_projector_state = projector_state;
}

void apply_validation_pattern_task_defaults(
    SpatialLayoutUiState* ui_state,
    const std::string& pattern_id,
    const std::string& projector_state)
{
    if (ui_state == nullptr) {
        return;
    }
    ui_state->calibration_image_set_purpose = "validation_pattern";
    ui_state->calibration_image_set_image_role = "validation_pattern_on";
    ui_state->calibration_image_set_projected_pattern_id = pattern_id;
    ui_state->calibration_image_set_projected_pattern_type = "validation_pattern";
    ui_state->calibration_image_set_scale_target_type = "unknown";
    ui_state->calibration_pattern_type = "validation_pattern";
    ui_state->calibration_target_method = "projected_pattern_on_diffuser";
    ui_state->calibration_homography_role = "validation_only";
    apply_common_visible_projection_defaults(ui_state);
    ui_state->calibration_projector_state = projector_state;
}

void apply_crosshair_task_defaults(
    SpatialLayoutUiState* ui_state,
    const std::string& pattern_id,
    const std::string& projector_state)
{
    if (ui_state == nullptr) {
        return;
    }
    ui_state->calibration_image_set_purpose = "crosshair_alignment";
    ui_state->calibration_image_set_image_role = "crosshair_on";
    ui_state->calibration_image_set_projected_pattern_id = pattern_id;
    ui_state->calibration_image_set_projected_pattern_type = "crosshair";
    ui_state->calibration_image_set_scale_target_type = "unknown";
    ui_state->calibration_pattern_type = "crosshair";
    ui_state->calibration_target_method = "projected_pattern_on_diffuser";
    ui_state->calibration_homography_role = "validation_only";
    apply_common_visible_projection_defaults(ui_state);
    ui_state->calibration_projector_state = projector_state;
}

void apply_calibration_workflow_tab_defaults(SpatialLayoutUiState* ui_state, const int tab)
{
    if (ui_state == nullptr) {
        return;
    }
    if (tab == 0) {
        apply_camera_physical_projected_surface_defaults(ui_state);
    } else if (tab == 1) {
        apply_dry_projection_surface_stage_defaults(ui_state);
        apply_calibration_image_set_purpose_defaults(ui_state, "homography_grid");
    } else if (tab == 2) {
        apply_holder_installed_projection_surface_stage_defaults(ui_state);
        apply_rings_homography_task_defaults(
            ui_state,
            "citrus_holder_installed_circular_rings_v1",
            "holder_installed_rings_on");
    } else if (tab == 3) {
        apply_wet_projection_surface_stage_defaults(ui_state);
        apply_rings_homography_task_defaults(
            ui_state,
            "citrus_wet_projected_surface_circular_rings_v1",
            "wet_projected_surface_rings_on");
    } else if (tab == 4) {
        apply_dish_top_observation_stage_defaults(ui_state);
        ui_state->calibration_light_handling = "keep_or_restore_mapped_pulse";
        apply_illumination_preset(ui_state, "custom_ttl_nir_strobe_855nm");
        ui_state->calibration_projector_state = "off";
        ui_state->calibration_projector_visible_to_camera = false;
    }
}

} // namespace

bool apply_calibration_workflow_profile_defaults(
    SpatialLayoutUiState* ui_state,
    const std::string& workflow_profile_id,
    std::string* error_out)
{
    if (ui_state == nullptr) {
        if (error_out != nullptr) {
            *error_out = "Calibration workflow profile destination is null.";
        }
        return false;
    }
    if (workflow_profile_id == "camera_physical_planes") {
        apply_camera_physical_projected_surface_defaults(ui_state);
        return true;
    }
    if (workflow_profile_id == "unobstructed_canvas_commissioning") {
        apply_dry_projection_surface_stage_defaults(ui_state);
        apply_calibration_image_set_purpose_defaults(ui_state, "homography_grid");
        return true;
    }
    if (workflow_profile_id == "holder_installed_projected_surface") {
        apply_holder_installed_projection_surface_stage_defaults(ui_state);
        apply_rings_homography_task_defaults(
            ui_state,
            "citrus_holder_installed_circular_rings_v1",
            "holder_installed_rings_on");
        return true;
    }
    if (workflow_profile_id == "wet_tank_projected_surface") {
        apply_wet_projection_surface_stage_defaults(ui_state);
        apply_rings_homography_task_defaults(
            ui_state,
            "citrus_wet_projected_surface_circular_rings_v1",
            "wet_projected_surface_rings_on");
        return true;
    }
    if (workflow_profile_id == "installed_tank_registration") {
        apply_dish_top_observation_stage_defaults(ui_state);
        apply_crosshair_task_defaults(
            ui_state,
            "citrus_installed_tank_registration_crosshair_v1",
            "installed_tank_registration_crosshair_on");
        ui_state->calibration_image_set_target_plane = "dish_top_rim";
        ui_state->calibration_pattern_domain = "circular_experimental_domain";
        ui_state->calibration_homography_role = "validation_only";
        return true;
    }
    if (error_out != nullptr) {
        *error_out = "Unsupported calibration workflow profile: " + workflow_profile_id;
    }
    return false;
}

void apply_calibration_image_set_purpose_defaults(
    SpatialLayoutUiState* ui_state,
    const std::string& purpose)
{
    if (ui_state == nullptr) {
        return;
    }
    ui_state->calibration_image_set_purpose = purpose;
    if (purpose == "projected_surface_scale_calibration") {
        apply_projected_surface_scale_calibration_defaults(ui_state);
        return;
    }
    if (purpose == "camera_only_physical_target_calibration" ||
        purpose == kDryPhysicalTargetHeightParallaxDiagnosticPurpose) {
        if (ui_state->calibration_capture_stage ==
            "camera_physical_dish_base_inner_surface") {
            apply_camera_physical_dish_base_defaults(ui_state);
        } else if (ui_state->calibration_capture_stage ==
                   "camera_physical_fish_height") {
            apply_camera_physical_fish_height_defaults(ui_state);
        } else {
            apply_camera_physical_projected_surface_defaults(ui_state);
        }
        return;
    }
    if (ui_state->calibration_capture_stage == "projected_surface_wet_runtime_stack") {
        if (purpose == "homography_grid") {
            apply_rings_homography_task_defaults(
                ui_state,
                "citrus_wet_projected_surface_circular_rings_v1",
                "wet_projected_surface_rings_on");
            return;
        }
        if (purpose == "scale_image") {
            apply_scale_task_defaults(ui_state);
            ui_state->calibration_image_set_target_plane = "projected_surface";
            return;
        }
        if (purpose == "crosshair_alignment") {
            apply_crosshair_task_defaults(
                ui_state,
                "citrus_wet_projected_surface_crosshair_v1",
                "wet_projected_surface_crosshair_on");
            return;
        }
        if (purpose == "validation_pattern") {
            apply_validation_pattern_task_defaults(
                ui_state,
                "citrus_wet_projected_surface_validation_pattern_v1",
                "wet_projected_surface_validation_pattern_on");
            return;
        }
    }
    if (ui_state->calibration_capture_stage == "projected_surface_holder_installed") {
        if (purpose == "homography_grid") {
            apply_rings_homography_task_defaults(
                ui_state,
                "citrus_holder_installed_circular_rings_v1",
                "holder_installed_rings_on");
            ui_state->calibration_pattern_domain = "circular_experimental_domain";
            return;
        }
        if (purpose == "scale_image") {
            apply_scale_task_defaults(ui_state);
            ui_state->calibration_image_set_target_plane = "projected_surface";
            ui_state->calibration_pattern_domain = "circular_experimental_domain";
            return;
        }
        if (purpose == "crosshair_alignment") {
            apply_crosshair_task_defaults(
                ui_state,
                "citrus_holder_installed_crosshair_v1",
                "holder_installed_crosshair_on");
            ui_state->calibration_pattern_domain = "circular_experimental_domain";
            return;
        }
        if (purpose == "validation_pattern") {
            apply_validation_pattern_task_defaults(
                ui_state,
                "citrus_holder_installed_validation_pattern_v1",
                "holder_installed_validation_pattern_on");
            ui_state->calibration_pattern_domain = "circular_experimental_domain";
            return;
        }
    }
    if (ui_state->calibration_capture_stage == "projected_surface_dry_reference" &&
        purpose == "scale_image") {
        apply_scale_task_defaults(ui_state);
        ui_state->calibration_image_set_target_plane = "projected_surface";
        ui_state->calibration_pattern_domain = "full_projected_surface";
        return;
    }
    if (ui_state->calibration_capture_stage == "projected_surface_dry_reference" &&
        purpose == "validation_pattern") {
        apply_validation_pattern_task_defaults(
            ui_state,
            "citrus_dry_projected_surface_validation_pattern_v1",
            "validation_pattern_on");
        ui_state->calibration_pattern_domain = "full_projected_surface";
        return;
    }
    if (purpose == "homography_grid") {
        ui_state->calibration_filter_state = kHoyaR72FilterRemoved;
        ui_state->calibration_runtime_filter_state = kHoyaR72FilterInstalled;
        ui_state->calibration_image_set_target_plane = "projected_surface";
        ui_state->calibration_image_set_image_role = "grid_on";
        ui_state->calibration_image_set_projected_pattern_id = "citrus_homography_grid_v1";
        ui_state->calibration_image_set_projected_pattern_type = "dot_grid";
        ui_state->calibration_pattern_type = "rectangular_grid";
        ui_state->calibration_pattern_domain = "full_projected_surface";
        ui_state->calibration_target_method = "projected_pattern_on_diffuser";
        ui_state->calibration_projected_pattern_used_as_coordinate_target = true;
        ui_state->calibration_light_handling = "suppress_mapped_strobe";
        apply_illumination_preset(ui_state, "visible_projector_broadband");
        ui_state->calibration_projector_state = "homography_grid_on";
        ui_state->calibration_projector_visible_to_camera = true;
        ui_state->calibration_homography_role =
            ui_state->calibration_capture_stage == "projected_surface_dry_reference"
                ? "commissioning_reference"
                : "operational_candidate";
    } else if (purpose == "verification_dots") {
        ui_state->calibration_filter_state = kHoyaR72FilterRemoved;
        ui_state->calibration_runtime_filter_state = kHoyaR72FilterInstalled;
        ui_state->calibration_image_set_target_plane = "projected_surface";
        ui_state->calibration_image_set_image_role = "verification_dots_on";
        ui_state->calibration_image_set_projected_pattern_id =
            "citrus_verification_dots_v1";
        ui_state->calibration_image_set_projected_pattern_type = "verification_dots";
        ui_state->calibration_pattern_type = "other";
        ui_state->calibration_pattern_domain = "circular_experimental_domain";
        ui_state->calibration_image_set_scale_target_type = "unknown";
        ui_state->calibration_light_handling = "suppress_mapped_strobe";
        apply_illumination_preset(ui_state, "visible_projector_broadband");
        ui_state->calibration_projector_state = "verification_dots_on";
        ui_state->calibration_projector_visible_to_camera = true;
        ui_state->calibration_homography_role = "validation_only";
    } else if (purpose == "validation_pattern") {
        ui_state->calibration_filter_state = kHoyaR72FilterRemoved;
        ui_state->calibration_runtime_filter_state = kHoyaR72FilterInstalled;
        ui_state->calibration_image_set_image_role = "validation_pattern_on";
        ui_state->calibration_image_set_projected_pattern_id =
            "citrus_validation_pattern_v1";
        ui_state->calibration_image_set_projected_pattern_type = "validation_pattern";
        ui_state->calibration_pattern_type = "validation_pattern";
        ui_state->calibration_pattern_domain =
            ui_state->calibration_image_set_target_plane == "projected_surface"
                ? "full_projected_surface"
                : "circular_experimental_domain";
        ui_state->calibration_image_set_scale_target_type = "unknown";
        ui_state->calibration_light_handling = "suppress_mapped_strobe";
        apply_illumination_preset(ui_state, "visible_projector_broadband");
        ui_state->calibration_projector_state = "validation_pattern_on";
        ui_state->calibration_projector_visible_to_camera = true;
        ui_state->calibration_homography_role = "validation_only";
    } else if (purpose == "arena_projection") {
        ui_state->calibration_filter_state = kHoyaR72FilterRemoved;
        ui_state->calibration_runtime_filter_state = kHoyaR72FilterInstalled;
        ui_state->calibration_image_set_target_plane = "projected_surface";
        ui_state->calibration_image_set_image_role = "projected_arena";
        ui_state->calibration_image_set_projected_pattern_id = "citrus_arena_projection";
        ui_state->calibration_image_set_projected_pattern_type =
            "arena_outline_with_center_fiducial";
        ui_state->calibration_pattern_type = "other";
        ui_state->calibration_pattern_domain = "full_projected_surface";
        ui_state->calibration_target_method = "projected_pattern_on_diffuser";
        ui_state->calibration_projected_pattern_used_as_coordinate_target = false;
        ui_state->calibration_light_handling = "suppress_mapped_strobe";
        apply_illumination_preset(ui_state, "visible_projector_broadband");
        ui_state->calibration_projector_state =
            "arena_outline_with_center_fiducial_on";
        ui_state->calibration_projector_visible_to_camera = true;
        ui_state->calibration_homography_role = "validation_only";
    } else if (purpose == "scale_image") {
        ui_state->calibration_filter_state = kHoyaR72FilterInstalled;
        ui_state->calibration_runtime_filter_state = kHoyaR72FilterInstalled;
        ui_state->calibration_image_set_target_plane = "tank_bottom_inner_surface";
        ui_state->calibration_image_set_image_role = "scale_target";
        ui_state->calibration_image_set_projected_pattern_id = "none";
        ui_state->calibration_image_set_projected_pattern_type = "none";
        ui_state->calibration_image_set_scale_target_type = "clear_plastic_ruler";
        ui_state->calibration_target_method = "ruler_only";
        ui_state->calibration_pattern_type = "ruler";
        ui_state->calibration_light_handling = "keep_or_restore_mapped_pulse";
        apply_illumination_preset(ui_state, "custom_ttl_nir_strobe_855nm");
        ui_state->calibration_projector_state = "off";
        ui_state->calibration_projector_visible_to_camera = false;
        ui_state->calibration_homography_role = "validation_only";
    } else if (purpose == "crosshair_alignment") {
        ui_state->calibration_filter_state = kHoyaR72FilterRemoved;
        ui_state->calibration_runtime_filter_state = kHoyaR72FilterInstalled;
        ui_state->calibration_image_set_target_plane = "tank_bottom_inner_surface";
        ui_state->calibration_image_set_image_role = "crosshair_on";
        ui_state->calibration_image_set_projected_pattern_id = "citrus_crosshair_alignment";
        ui_state->calibration_image_set_projected_pattern_type = "crosshair";
        ui_state->calibration_target_method = "projected_pattern_on_diffuser";
        ui_state->calibration_pattern_type = "crosshair";
        ui_state->calibration_light_handling = "suppress_mapped_strobe";
        apply_illumination_preset(ui_state, "visible_projector_broadband");
        ui_state->calibration_projector_state = "crosshair_on";
        ui_state->calibration_projector_visible_to_camera = true;
        ui_state->calibration_homography_role = "validation_only";
    }
}

void render_calibration_workflow_tabs(
    SpatialLayoutUiState* ui_state,
    const HoughCirclePanelActions& hough_actions)
{
    if (ui_state == nullptr) {
        return;
    }

    ImGui::SeparatorText("Calibration Workflow");
    if (ImGui::BeginTabBar("SpatialCalibrationWorkflowTabs")) {
        if (ImGui::BeginTabItem("Camera Physical Planes")) {
            if (ui_state->calibration_workflow_tab != 0) {
                ui_state->calibration_workflow_tab = 0;
                apply_calibration_workflow_tab_defaults(ui_state, 0);
            }
            ImGui::TextWrapped(
                "Camera-only physical target captures for C0, Cbase, and Cfish. Projector coordinates are not coordinate targets.");
            if (ImGui::Button("New physical parity group")) {
                ui_state->calibration_matched_parity_group_id.clear();
                ensure_camera_physical_parity_group_id(ui_state);
            }
            ImGui::SameLine();
            ImGui::TextDisabled(
                "%s",
                ui_state->calibration_matched_parity_group_id.empty()
                    ? "(no physical parity group)"
                    : ui_state->calibration_matched_parity_group_id.c_str());
            if (ImGui::Button("C0 gel/projection surface")) {
                apply_camera_physical_projected_surface_defaults(ui_state);
            }
            ImGui::SameLine();
            if (ImGui::Button("Cbase dish/base inner surface")) {
                apply_camera_physical_dish_base_defaults(ui_state);
            }
            ImGui::SameLine();
            if (ImGui::Button("Cfish assumed fish height")) {
                apply_camera_physical_fish_height_defaults(ui_state);
            }
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Unobstructed Canvas")) {
            if (ui_state->calibration_workflow_tab != 1) {
                ui_state->calibration_workflow_tab = 1;
                apply_calibration_workflow_tab_defaults(ui_state, 1);
            }
            ImGui::TextWrapped(
                "Holder removed. Commission the full rectangular arena region seen by this camera. "
                "These captures are stable canvas evidence, not Cbase or Cfish physical calibration.");
            if (ImGui::Button("Arena definition defaults")) {
                apply_dry_projection_surface_stage_defaults(ui_state);
                apply_calibration_image_set_purpose_defaults(ui_state, "arena_projection");
            }
            ImGui::SameLine();
            if (ImGui::Button("Physical disk scale commissioning")) {
                apply_projected_surface_scale_calibration_defaults(ui_state);
            }
            ImGui::SameLine();
            if (ImGui::Button("Surface homography defaults")) {
                apply_dry_projection_surface_stage_defaults(ui_state);
                apply_calibration_image_set_purpose_defaults(ui_state, "homography_grid");
            }
            ImGui::SameLine();
            if (ImGui::Button("Surface validation defaults")) {
                apply_dry_projection_surface_stage_defaults(ui_state);
                apply_validation_pattern_task_defaults(
                    ui_state,
                    "citrus_projected_surface_validation_pattern_v1",
                    "validation_pattern_on");
                ui_state->calibration_pattern_domain = "full_projected_surface";
            }
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Holder Installed")) {
            if (ui_state->calibration_workflow_tab != 2) {
                ui_state->calibration_workflow_tab = 2;
                apply_calibration_workflow_tab_defaults(ui_state, 2);
            }
            ImGui::TextWrapped(
                "Shelf/tank holder installed with no dish or water. Capture the circularly visible "
                "operational support independently from the tank experimental area.");
            if (ImGui::Button("New holder calibration group")) {
                ui_state->calibration_matched_parity_group_id.clear();
                ensure_holder_installed_parity_group_id(ui_state);
            }
            ImGui::SameLine();
            ImGui::TextDisabled(
                "%s",
                ui_state->calibration_matched_parity_group_id.empty()
                    ? "(no holder calibration group)"
                    : ui_state->calibration_matched_parity_group_id.c_str());
            if (ImGui::Button("Holder homography defaults")) {
                apply_holder_installed_projection_surface_stage_defaults(ui_state);
                apply_rings_homography_task_defaults(
                    ui_state,
                    "citrus_holder_installed_circular_rings_v1",
                    "holder_installed_rings_on");
            }
            ImGui::SameLine();
            if (ImGui::Button("Holder crosshair defaults")) {
                apply_holder_installed_projection_surface_stage_defaults(ui_state);
                apply_crosshair_task_defaults(
                    ui_state,
                    "citrus_holder_installed_crosshair_v1",
                    "holder_installed_crosshair_on");
            }
            ImGui::SameLine();
            if (ImGui::Button("Holder validation defaults")) {
                apply_holder_installed_projection_surface_stage_defaults(ui_state);
                apply_validation_pattern_task_defaults(
                    ui_state,
                    "citrus_holder_installed_validation_pattern_v1",
                    "holder_installed_validation_pattern_on");
            }
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Wet Tank Installed")) {
            if (ui_state->calibration_workflow_tab != 3) {
                ui_state->calibration_workflow_tab = 3;
                apply_calibration_workflow_tab_defaults(ui_state, 3);
            }
            ImGui::TextWrapped(
                "Wet projection-surface projector/render captures at the gel plane. "
                "These are not dish-base or fish-height physical calibration maps.");
            if (ImGui::Button("New wet projection group")) {
                ui_state->calibration_matched_parity_group_id.clear();
                ensure_wet_runtime_stack_parity_group_id(ui_state);
            }
            ImGui::SameLine();
            ImGui::TextDisabled(
                "%s",
                ui_state->calibration_matched_parity_group_id.empty()
                    ? "(no wet projection group)"
                    : ui_state->calibration_matched_parity_group_id.c_str());
            if (ImGui::Button("Wet projection scale defaults")) {
                apply_wet_projection_surface_stage_defaults(ui_state);
                apply_scale_task_defaults(ui_state);
                ui_state->calibration_image_set_target_plane = "projected_surface";
            }
            ImGui::SameLine();
            if (ImGui::Button("Wet projection homography defaults")) {
                apply_wet_projection_surface_stage_defaults(ui_state);
                apply_rings_homography_task_defaults(
                    ui_state,
                    "citrus_wet_projected_surface_circular_rings_v1",
                    "wet_projected_surface_rings_on");
            }
            ImGui::SameLine();
            if (ImGui::Button("Wet projection crosshair defaults")) {
                apply_wet_projection_surface_stage_defaults(ui_state);
                apply_crosshair_task_defaults(
                    ui_state,
                    "citrus_wet_projected_surface_crosshair_v1",
                    "wet_projected_surface_crosshair_on");
            }
            ImGui::SameLine();
            if (ImGui::Button("Wet projection validation defaults")) {
                apply_wet_projection_surface_stage_defaults(ui_state);
                apply_validation_pattern_task_defaults(
                    ui_state,
                    "citrus_wet_projected_surface_validation_pattern_v1",
                    "wet_projected_surface_validation_pattern_on");
            }
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Dish / Valid Area")) {
            if (ui_state->calibration_workflow_tab != 4) {
                ui_state->calibration_workflow_tab = 4;
                apply_calibration_workflow_tab_defaults(ui_state, 4);
            }
            ImGui::TextWrapped(
                "Use for daily dish top-rim fits and valid-area/mask artifacts. "
                "This tab prepares capture metadata for top-rim observation saves; Citrus still owns accepted runtime geometry.");
            if (ImGui::Button("Top-rim capture defaults")) {
                apply_calibration_workflow_tab_defaults(ui_state, 4);
            }
            render_hough_circle_tuning(ui_state, hough_actions);
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
}

} // namespace orange::gui::spatial_layout
