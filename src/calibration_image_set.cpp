#include "calibration_image_set.h"

#include "fsuid_guard.h"

#include <filesystem>
#include <fstream>

namespace orange::calibration {
namespace {

bool set_error(std::string* error_out, const std::string& message)
{
    if (error_out) {
        *error_out = message;
    }
    return false;
}

bool is_dry_physical_target_height_parallax_diagnostic(const std::string& value)
{
    return value == "dry_physical_target_height_parallax_diagnostic";
}

bool is_camera_only_physical_target_purpose(const std::string& value)
{
    return value == "camera_only_physical_target_calibration" ||
           is_dry_physical_target_height_parallax_diagnostic(value);
}

bool is_allowed_purpose(const std::string& value)
{
    return value == "camera_arena_calibration_set" ||
           value == "arena_projection" ||
           value == "homography_grid" ||
           value == "scale_image" ||
           value == "dish_top_rim" ||
           value == "verification_dots" ||
           value == "validation_pattern" ||
           is_camera_only_physical_target_purpose(value) ||
           value == "crosshair_alignment";
}

bool is_allowed_target_plane(const std::string& value)
{
    return value == "multiple" ||
           value == "projected_surface" ||
           value == "tank_bottom_outer_surface" ||
           value == "tank_bottom_inner_surface" ||
           value == "estimated_fish_plane" ||
           value == "dish_top_rim" ||
           value == "unknown";
}

bool is_allowed_coordinate_space(const std::string& value)
{
    return value == "camera_native_pixels" || value == "camera_view_px";
}

bool is_allowed_capture_stage(const std::string& value)
{
    return value.empty() ||
           value == "projected_surface_dry_reference" ||
           value == "projected_surface_wet_runtime_stack" ||
           value == "camera_physical_projected_surface" ||
           value == "camera_physical_dish_base_inner_surface" ||
           value == "camera_physical_fish_height" ||
           value == "projector_surface_validation" ||
           value == "dish_top_observation" ||
           value == "unknown";
}

bool is_allowed_wet_or_dry(const std::string& value)
{
    return value.empty() ||
           value == "wet" ||
           value == "dry" ||
           value == "unknown" ||
           value == "not_applicable";
}

bool is_allowed_target_method(const std::string& value)
{
    return value.empty() ||
           value == "projected_pattern_on_diffuser" ||
           value == "physical_target_known_xy" ||
           value == "physical_target" ||
           value == "ruler_only" ||
           value == "inferred" ||
           value == "other" ||
           value == "unknown" ||
           value == "not_applicable";
}

bool is_allowed_pattern_type(const std::string& value)
{
    return value.empty() ||
           value == "rectangular_grid" ||
           value == "circular_rings" ||
           value == "ruler" ||
           value == "crosshair" ||
           value == "validation_pattern" ||
           value == "physical_grid" ||
           value == "physical_point_set" ||
           value == "none" ||
           value == "other" ||
           value == "unknown" ||
           value == "not_applicable";
}

bool is_allowed_pattern_domain(const std::string& value)
{
    return value.empty() ||
           value == "full_projected_surface" ||
           value == "circular_experimental_domain" ||
           value == "other" ||
           value == "unknown" ||
           value == "not_applicable";
}

bool is_allowed_parity_group_role(const std::string& value)
{
    return value.empty() ||
           value == "dry_reference" ||
           value == "wet_projected_surface" ||
           value == "physical_projected_surface" ||
           value == "physical_dish_base" ||
           value == "physical_fish_height" ||
           value == "projector_surface_validation";
}

bool is_allowed_plane_id(const std::string& value)
{
    return value.empty() ||
           value == "projected_surface_physical" ||
           value == "dish_base_inner_surface_physical" ||
           value == "fish_height_physical_assumed" ||
           value == "projector_surface" ||
           value == "unknown";
}

std::string effective_parity_group_id(const CalibrationImageSetRequest& request)
{
    return request.parity_group_id.empty()
               ? request.matched_parity_group_id
               : request.parity_group_id;
}

nlohmann::json image_shape_to_json(const CalibrationImageSetShape& shape)
{
    return {{"height", shape.height}, {"width", shape.width}};
}

bool write_json_file(const std::filesystem::path& path,
                     const nlohmann::json& value,
                     std::string* error_out)
{
    orange::ScopedFsuid fsuid_guard;
    (void)fsuid_guard;
    std::filesystem::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::out | std::ios::trunc);
    if (!out.is_open()) {
        return set_error(error_out, "failed to open image-set JSON for writing: " + path.string());
    }
    out << value.dump(2) << '\n';
    return true;
}

bool validate_request(const CalibrationImageSetRequest& request, std::string* error_out)
{
    if (request.artifact_id.empty()) {
        return set_error(error_out, "image_set artifact_id is empty");
    }
    if (request.created_utc.empty()) {
        return set_error(error_out, "image_set created_utc is empty");
    }
    if (!is_allowed_purpose(request.purpose)) {
        return set_error(error_out, "image_set purpose is not supported: " + request.purpose);
    }
    if (!is_allowed_target_plane(request.target_plane)) {
        return set_error(error_out, "image_set target_plane is not supported: " + request.target_plane);
    }
    if (!is_allowed_coordinate_space(request.coordinate_space)) {
        return set_error(
            error_out,
            "image_set coordinate_space must be camera_native_pixels or camera_view_px");
    }
    if (request.capture_stage == "tank_bottom_inner_surface_wet_runtime_stack" ||
        request.capture_stage == "fish_plane_wet_runtime_stack") {
        return set_error(
            error_out,
            request.capture_stage +
                " is deprecated for new writes; use dry_physical_target_height_parallax_diagnostic "
                "or camera_only_physical_target_calibration "
                "with a physical target instead of projected-pattern tank-bottom/fish-plane captures");
    }
    if (!is_allowed_capture_stage(request.capture_stage)) {
        return set_error(
            error_out,
            "image_set capture_stage is not supported: " + request.capture_stage);
    }
    if (!is_allowed_wet_or_dry(request.wet_or_dry)) {
        return set_error(error_out, "image_set wet_or_dry is not supported: " + request.wet_or_dry);
    }
    if (!is_allowed_target_method(request.target_method)) {
        return set_error(
            error_out,
            "image_set target_method is not supported: " + request.target_method);
    }
    if (!is_allowed_pattern_type(request.pattern_type)) {
        return set_error(
            error_out,
            "image_set pattern_type is not supported: " + request.pattern_type);
    }
    if (!is_allowed_pattern_domain(request.pattern_domain)) {
        return set_error(
            error_out,
            "image_set pattern_domain is not supported: " + request.pattern_domain);
    }
    if (!is_allowed_parity_group_role(request.parity_group_role)) {
        return set_error(
            error_out,
            "image_set parity_group_role is not supported: " + request.parity_group_role);
    }
    if (!is_allowed_plane_id(request.plane_id)) {
        return set_error(error_out, "image_set plane_id is not supported: " + request.plane_id);
    }
    if (request.has_plane_z_mm_uncertainty && request.plane_z_mm_uncertainty < 0.0) {
        return set_error(error_out, "image_set plane_z_mm_uncertainty must be non-negative");
    }
    if (request.has_water_fill_mm && request.water_fill_mm < 0.0) {
        return set_error(error_out, "image_set water_fill_mm must be non-negative");
    }
    if (request.has_physical_target_grid_spacing_mm &&
        request.physical_target_grid_spacing_mm <= 0.0) {
        return set_error(error_out, "image_set physical_target_grid_spacing_mm must be positive");
    }
    if (!request.projected_pattern.empty() &&
        (request.target_plane == "tank_bottom_inner_surface" ||
         request.target_plane == "estimated_fish_plane")) {
        return set_error(
            error_out,
            "projected_pattern captures at tank-bottom or fish-height planes are deprecated; "
            "use dry_physical_target_height_parallax_diagnostic or "
            "camera_only_physical_target_calibration with physical target coordinates");
    }
    if (request.capture_stage == "projected_surface_dry_reference") {
        if (request.target_plane != "projected_surface") {
            return set_error(
                error_out,
                "projected_surface_dry_reference must use target_plane=projected_surface");
        }
        if (!request.wet_or_dry.empty() && request.wet_or_dry != "dry") {
            return set_error(
                error_out,
                "projected_surface_dry_reference must use wet_or_dry=dry");
        }
        if (request.parity_group_role != "dry_reference") {
            return set_error(
                error_out,
                "projected_surface_dry_reference must use parity_group_role=dry_reference");
        }
        if (!request.has_reference_only || !request.reference_only) {
            return set_error(
                error_out,
                "projected_surface_dry_reference must be reference_only=true");
        }
    }
    if (request.capture_stage == "projected_surface_wet_runtime_stack") {
        if (request.target_plane != "projected_surface") {
            return set_error(
                error_out,
                "projected_surface_wet_runtime_stack must use target_plane=projected_surface");
        }
        if (request.parity_group_role != "wet_projected_surface") {
            return set_error(
                error_out,
                "projected_surface_wet_runtime_stack must use parity_group_role=wet_projected_surface");
        }
    }
    if (request.capture_stage == "projected_surface_wet_runtime_stack") {
        if (!request.wet_or_dry.empty() && request.wet_or_dry != "wet") {
            return set_error(error_out, request.capture_stage + " must use wet_or_dry=wet");
        }
        if (request.matched_parity_group_id.empty()) {
            return set_error(
                error_out,
                request.capture_stage + " requires matched_parity_group_id");
        }
    }
    if (is_camera_only_physical_target_purpose(request.purpose)) {
        if (!request.has_physical_target_used || !request.physical_target_used) {
            return set_error(
                error_out,
                request.purpose + " requires physical_target_used=true");
        }
        if (!request.has_projected_pattern_used_as_coordinate_target ||
            request.projected_pattern_used_as_coordinate_target) {
            return set_error(
                error_out,
                request.purpose + " requires "
                "projected_pattern_used_as_coordinate_target=false");
        }
        if (!request.projected_pattern.empty()) {
            return set_error(
                error_out,
                request.purpose + " must not carry projected_pattern "
                "coordinate metadata");
        }
        if (request.plane_id.empty() || request.plane_id == "unknown") {
            return set_error(
                error_out,
                request.purpose + " requires plane_id");
        }
        if (effective_parity_group_id(request).empty()) {
            return set_error(
                error_out,
                request.purpose + " requires parity_group_id");
        }
        if (request.target_id.empty()) {
            return set_error(
                error_out,
                request.purpose + " requires target_id");
        }
        if (request.target_design.empty()) {
            return set_error(
                error_out,
                request.purpose + " requires target_design");
        }
        if (request.physical_target_origin_definition.empty()) {
            return set_error(
                error_out,
                request.purpose + " requires physical target origin definition");
        }
        if (request.physical_target_x_orientation_marker_definition.empty()) {
            return set_error(
                error_out,
                request.purpose + " requires physical +x orientation marker definition");
        }
        const bool has_known_points =
            request.physical_target.is_object() &&
            request.physical_target.contains("known_points_mm") &&
            request.physical_target["known_points_mm"].is_array() &&
            !request.physical_target["known_points_mm"].empty();
        if (!request.has_physical_target_grid_spacing_mm && !has_known_points) {
            return set_error(
                error_out,
                request.purpose + " requires grid spacing or known physical points");
        }
        if (is_dry_physical_target_height_parallax_diagnostic(request.purpose)) {
            if (request.wet_or_dry != "dry") {
                return set_error(
                    error_out,
                    request.purpose + " requires wet_or_dry=dry");
            }
            if (request.fill_state != "dry_or_empty" &&
                request.fill_state != "not_applicable") {
                return set_error(
                    error_out,
                    request.purpose + " requires fill_state=dry_or_empty or not_applicable");
            }
            if (request.has_open_water_surface_present &&
                request.open_water_surface_present) {
                return set_error(
                    error_out,
                    request.purpose + " requires open_water_surface_present=false");
            }
            if (!request.water_settled_status.empty() &&
                request.water_settled_status != "not_applicable") {
                return set_error(
                    error_out,
                    request.purpose + " requires water_settled_status=not_applicable");
            }
            if (!request.has_reference_only || !request.reference_only) {
                return set_error(
                    error_out,
                    request.purpose + " requires reference_only=true");
            }
        }
        if (request.capture_stage == "camera_physical_projected_surface") {
            if (request.target_plane != "projected_surface" ||
                request.plane_id != "projected_surface_physical") {
                return set_error(
                    error_out,
                    "camera_physical_projected_surface requires target_plane=projected_surface "
                    "and plane_id=projected_surface_physical");
            }
            if (request.parity_group_role != "physical_projected_surface") {
                return set_error(
                    error_out,
                    "camera_physical_projected_surface requires parity_group_role=physical_projected_surface");
            }
        } else if (request.capture_stage == "camera_physical_dish_base_inner_surface") {
            if (request.target_plane != "tank_bottom_inner_surface" ||
                request.plane_id != "dish_base_inner_surface_physical") {
                return set_error(
                    error_out,
                    "camera_physical_dish_base_inner_surface requires "
                    "target_plane=tank_bottom_inner_surface and plane_id=dish_base_inner_surface_physical");
            }
            if (request.parity_group_role != "physical_dish_base") {
                return set_error(
                    error_out,
                    "camera_physical_dish_base_inner_surface requires parity_group_role=physical_dish_base");
            }
        } else if (request.capture_stage == "camera_physical_fish_height") {
            if (request.target_plane != "estimated_fish_plane" ||
                request.plane_id != "fish_height_physical_assumed") {
                return set_error(
                    error_out,
                    "camera_physical_fish_height requires target_plane=estimated_fish_plane "
                    "and plane_id=fish_height_physical_assumed");
            }
            if (request.parity_group_role != "physical_fish_height") {
                return set_error(
                    error_out,
                    "camera_physical_fish_height requires parity_group_role=physical_fish_height");
            }
        } else {
            return set_error(
                error_out,
                request.purpose + " requires a camera_physical_* capture_stage");
        }
    }
    if (request.camera.serial.empty()) {
        return set_error(error_out, "image_set camera.serial is empty");
    }
    if (request.camera.image_shape.height <= 0 || request.camera.image_shape.width <= 0) {
        return set_error(error_out, "image_set camera.image_shape must be positive");
    }
    if (request.capture.timestamp_utc.empty()) {
        return set_error(error_out, "image_set capture.timestamp_utc is empty");
    }
    if (request.images.empty()) {
        return set_error(error_out, "image_set images must contain at least one image");
    }
    for (size_t idx = 0; idx < request.images.size(); ++idx) {
        const CalibrationImageSetImageRef& image = request.images[idx];
        const std::string prefix = "image_set images[" + std::to_string(idx) + "]";
        if (image.role.empty()) {
            return set_error(error_out, prefix + ".role is empty");
        }
        if (image.path.empty()) {
            return set_error(error_out, prefix + ".path is empty");
        }
        if (image.checksum_algorithm.empty()) {
            return set_error(error_out, prefix + ".checksum_algorithm is empty");
        }
        if (image.checksum.empty()) {
            return set_error(error_out, prefix + ".checksum is empty");
        }
        if (!is_allowed_coordinate_space(image.coordinate_space)) {
            return set_error(error_out, prefix + ".coordinate_space is not supported");
        }
        if (image.image_shape.height <= 0 || image.image_shape.width <= 0) {
            return set_error(error_out, prefix + ".image_shape must be positive");
        }
    }
    for (size_t idx = 0; idx < request.derived_artifacts.size(); ++idx) {
        const CalibrationImageSetArtifactRef& ref = request.derived_artifacts[idx];
        const std::string prefix = "image_set derived_artifacts[" + std::to_string(idx) + "]";
        if (ref.artifact_id.empty()) {
            return set_error(error_out, prefix + ".artifact_id is empty");
        }
        if (ref.artifact_schema_id.empty()) {
            return set_error(error_out, prefix + ".artifact_schema_id is empty");
        }
        if (ref.artifact_schema_version <= 0) {
            return set_error(error_out, prefix + ".artifact_schema_version must be positive");
        }
    }
    return true;
}

nlohmann::json camera_to_json(const CalibrationImageSetCameraInfo& camera)
{
    nlohmann::json out = {
        {"serial", camera.serial},
        {"image_shape", image_shape_to_json(camera.image_shape)}
    };
    if (!camera.name.empty()) {
        out["name"] = camera.name;
    }
    if (!camera.pixel_format.empty()) {
        out["pixel_format"] = camera.pixel_format;
    }
    if (camera.configured_width > 0) {
        out["configured_width"] = camera.configured_width;
    }
    if (camera.configured_height > 0) {
        out["configured_height"] = camera.configured_height;
    }
    if (camera.has_gpu_direct) {
        out["gpu_direct"] = camera.gpu_direct;
    }
    if (!camera.camera_config_ref.empty()) {
        out["camera_config_ref"] = camera.camera_config_ref;
    }
    return out;
}

nlohmann::json capture_to_json(const CalibrationImageSetCaptureContext& capture)
{
    nlohmann::json out = {{"timestamp_utc", capture.timestamp_utc}};
    if (!capture.operation_id.empty()) {
        out["operation_id"] = capture.operation_id;
    }
    if (!capture.capture_group_id.empty()) {
        out["capture_group_id"] = capture.capture_group_id;
    }
    if (capture.has_frame_id) {
        out["frame_id"] = capture.frame_id;
    }
    if (capture.has_recording_frame_id) {
        out["recording_frame_id"] = capture.recording_frame_id;
    }
    if (capture.has_source_frame_count) {
        out["source_frame_count"] = capture.source_frame_count;
    }
    if (!capture.temporal_compositing_method.empty()) {
        out["temporal_compositing_method"] = capture.temporal_compositing_method;
    }
    if (capture.has_local_frame_range) {
        out["first_local_frame_id"] = capture.first_local_frame_id;
        out["last_local_frame_id"] = capture.last_local_frame_id;
    }
    if (capture.has_camera_frame_range) {
        out["first_camera_frame_id"] = capture.first_camera_frame_id;
        out["last_camera_frame_id"] = capture.last_camera_frame_id;
    }
    if (!capture.capture_mode.empty()) {
        out["capture_mode"] = capture.capture_mode;
    }
    if (capture.has_exposure_us) {
        out["exposure_us"] = capture.exposure_us;
    }
    if (capture.has_frame_rate_hz) {
        out["frame_rate_hz"] = capture.frame_rate_hz;
    }
    if (capture.has_gain) {
        out["gain"] = capture.gain;
    }
    if (!capture.filter_state.empty()) {
        out["filter_state"] = capture.filter_state;
    }
    if (!capture.runtime_filter_state.empty()) {
        out["runtime_filter_state"] = capture.runtime_filter_state;
    }
    if (!capture.light_handling.empty()) {
        out["light_handling"] = capture.light_handling;
    }
    if (!capture.light_state.empty()) {
        out["light_state"] = capture.light_state;
    }
    nlohmann::json illumination = nlohmann::json::object();
    if (!capture.illumination_spectrum.empty()) {
        illumination["spectrum"] = capture.illumination_spectrum;
    }
    if (!capture.illumination_source.empty()) {
        illumination["source"] = capture.illumination_source;
    }
    if (capture.has_illumination_center_wavelength_nm) {
        illumination["center_wavelength_nm"] = capture.illumination_center_wavelength_nm;
    }
    if (capture.has_illumination_min_wavelength_nm) {
        illumination["min_wavelength_nm"] = capture.illumination_min_wavelength_nm;
    }
    if (capture.has_illumination_max_wavelength_nm) {
        illumination["max_wavelength_nm"] = capture.illumination_max_wavelength_nm;
    }
    if (capture.has_illumination_bandwidth_fwhm_nm) {
        illumination["bandwidth_fwhm_nm"] = capture.illumination_bandwidth_fwhm_nm;
    }
    if (!capture.illumination_wavelength_confidence.empty()) {
        illumination["wavelength_confidence"] =
            capture.illumination_wavelength_confidence;
    }
    if (!illumination.empty()) {
        out["illumination"] = illumination;
    }
    if (!capture.projector_state.empty()) {
        out["projector_state"] = capture.projector_state;
    }
    if (capture.has_projector_visible_to_camera) {
        out["projector_visible_to_camera"] = capture.projector_visible_to_camera;
    }
    if (capture.has_requires_camera_mount_unchanged) {
        out["requires_camera_mount_unchanged"] = capture.requires_camera_mount_unchanged;
    }
    if (capture.has_requires_filter_reinstalled_repeatably) {
        out["requires_filter_reinstalled_repeatably"] =
            capture.requires_filter_reinstalled_repeatably;
    }
    return out;
}

nlohmann::json image_ref_to_json(const CalibrationImageSetImageRef& image)
{
    nlohmann::json out = {
        {"role", image.role},
        {"path", image.path},
        {"checksum_algorithm", image.checksum_algorithm},
        {"checksum", image.checksum},
        {"coordinate_space", image.coordinate_space},
        {"image_shape", image_shape_to_json(image.image_shape)}
    };
    if (!image.description.empty()) {
        out["description"] = image.description;
    }
    return out;
}

nlohmann::json artifact_ref_to_json(const CalibrationImageSetArtifactRef& ref)
{
    nlohmann::json out = {
        {"artifact_id", ref.artifact_id},
        {"artifact_schema_id", ref.artifact_schema_id},
        {"artifact_schema_version", ref.artifact_schema_version}
    };
    if (!ref.fingerprint.empty()) {
        out["fingerprint"] = ref.fingerprint;
    }
    return out;
}

} // namespace

nlohmann::json calibration_image_set_to_json(const CalibrationImageSetRequest& request)
{
    nlohmann::json out = {
        {"schema_id", kCalibrationImageSetSchemaId},
        {"schema_version", kCalibrationImageSetSchemaVersion},
        {"artifact_id", request.artifact_id},
        {"created_utc", request.created_utc},
        {"purpose", request.purpose},
        {"target_plane", request.target_plane},
        {"coordinate_space", request.coordinate_space},
        {"camera", camera_to_json(request.camera)},
        {"capture", capture_to_json(request.capture)},
        {"capture_timestamp_utc", request.capture.timestamp_utc},
        {"images", nlohmann::json::array()}
    };

    for (const CalibrationImageSetImageRef& image : request.images) {
        out["images"].push_back(image_ref_to_json(image));
    }
    if (!request.rig_context.empty()) {
        out["rig_context"] = request.rig_context;
    }
    out["artifact_schema_id"] = kCalibrationImageSetSchemaId;
    out["artifact_schema_version"] = kCalibrationImageSetSchemaVersion;
    if (!request.capture_stage.empty()) {
        out["capture_stage"] = request.capture_stage;
    }
    if (request.has_plane_z_mm_nominal) {
        out["plane_z_mm_nominal"] = request.plane_z_mm_nominal;
    }
    if (request.has_plane_z_mm_uncertainty) {
        out["plane_z_mm_uncertainty"] = request.plane_z_mm_uncertainty;
    }
    if (!request.wet_or_dry.empty()) {
        out["wet_or_dry"] = request.wet_or_dry;
    }
    if (request.has_imaging_shelf_installed) {
        out["imaging_shelf_installed"] = request.imaging_shelf_installed;
    }
    if (request.has_dish_installed) {
        out["dish_installed"] = request.dish_installed;
    }
    if (!request.dish_id.empty()) {
        out["dish_id"] = request.dish_id;
    }
    if (request.has_water_fill_mm) {
        out["water_fill_mm"] = request.water_fill_mm;
    }
    if (!request.fill_state.empty()) {
        out["fill_state"] = request.fill_state;
    }
    if (request.has_open_water_surface_present) {
        out["open_water_surface_present"] = request.open_water_surface_present;
    }
    if (!request.water_settled_status.empty()) {
        out["water_settled_status"] = request.water_settled_status;
    }
    if (!request.target_method.empty()) {
        out["target_method"] = request.target_method;
    }
    if (!request.pattern_type.empty()) {
        out["pattern_type"] = request.pattern_type;
    }
    if (!request.pattern_domain.empty()) {
        out["pattern_domain"] = request.pattern_domain;
    }
    if (!request.matched_parity_group_id.empty()) {
        out["matched_parity_group_id"] = request.matched_parity_group_id;
    }
    if (!request.parity_group_id.empty()) {
        out["parity_group_id"] = request.parity_group_id;
    } else if (!request.matched_parity_group_id.empty()) {
        out["parity_group_id"] = request.matched_parity_group_id;
    }
    if (!request.parity_group_role.empty()) {
        out["parity_group_role"] = request.parity_group_role;
    }
    if (request.has_reference_only) {
        out["reference_only"] = request.reference_only;
    }
    if (request.has_physical_target_used) {
        out["physical_target_used"] = request.physical_target_used;
    }
    if (request.has_projected_pattern_used_as_coordinate_target) {
        out["projected_pattern_used_as_coordinate_target"] =
            request.projected_pattern_used_as_coordinate_target;
    }
    if (!request.plane_id.empty()) {
        out["plane_id"] = request.plane_id;
    }
    if (request.has_z_mm_relative_to_projection_surface) {
        out["z_mm_relative_to_projection_surface"] =
            request.z_mm_relative_to_projection_surface;
    }
    if (!request.target_id.empty()) {
        out["target_id"] = request.target_id;
    }
    if (!request.target_design.empty()) {
        out["target_design"] = request.target_design;
    }
    if (request.has_physical_target_grid_spacing_mm) {
        out["physical_target_grid_spacing_mm"] =
            request.physical_target_grid_spacing_mm;
    }
    if (!request.physical_target_origin_definition.empty()) {
        out["physical_target_origin_definition"] =
            request.physical_target_origin_definition;
    }
    if (!request.physical_target_x_orientation_marker_definition.empty()) {
        out["physical_target_x_orientation_marker_definition"] =
            request.physical_target_x_orientation_marker_definition;
    }
    if (!request.derived_artifacts.empty()) {
        out["derived_artifacts"] = nlohmann::json::array();
        for (const CalibrationImageSetArtifactRef& ref : request.derived_artifacts) {
            out["derived_artifacts"].push_back(artifact_ref_to_json(ref));
        }
    }
    if (!request.physical_target.empty()) {
        out["physical_target"] = request.physical_target;
    }
    if (!request.projected_pattern.empty()) {
        out["projected_pattern"] = request.projected_pattern;
    }
    if (!request.scale_target.empty()) {
        out["scale_target"] = request.scale_target;
    }
    if (!request.runtime_role.empty()) {
        out["runtime_role"] = request.runtime_role;
    }
    if (!request.observations.empty()) {
        out["observations"] = request.observations;
    }
    if (!request.review_artifacts.empty()) {
        out["review_artifacts"] = request.review_artifacts;
    }
    if (!request.citrus_preview.empty()) {
        out["citrus_preview"] = request.citrus_preview;
    }
    if (!request.operator_notes.empty()) {
        out["operator_notes"] = request.operator_notes;
    }
    return out;
}

bool write_calibration_image_set_json_file(
    const std::string& image_set_json_path,
    const CalibrationImageSetRequest& request,
    CalibrationImageSetWriteResult* result_out,
    std::string* error_out)
{
    if (image_set_json_path.empty()) {
        return set_error(error_out, "image_set_json_path is empty");
    }
    if (!validate_request(request, error_out)) {
        return false;
    }

    const nlohmann::json image_set = calibration_image_set_to_json(request);
    if (!write_json_file(image_set_json_path, image_set, error_out)) {
        return false;
    }

    if (result_out) {
        result_out->image_set_json_path = image_set_json_path;
        result_out->image_set = image_set;
    }
    return true;
}

} // namespace orange::calibration
