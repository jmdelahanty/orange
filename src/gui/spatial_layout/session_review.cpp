#include "gui/spatial_layout/session_review.h"

#include "calibration_image_set.h"
#include "dish_top_rim_observation.h"
#include "gui/spatial_layout/session_io.h"

#include <opencv2/opencv.hpp>

#include <algorithm>
#include <iterator>
#include <sstream>

namespace orange::gui::spatial_layout {
namespace {

std::string json_string_or(const nlohmann::json& node,
                           const std::string& key,
                           const std::string& fallback = "")
{
    if (node.is_object() && node.contains(key) && node.at(key).is_string()) {
        return node.at(key).get<std::string>();
    }
    return fallback;
}

double json_number_or(const nlohmann::json& node,
                      const std::string& key,
                      double fallback = 0.0)
{
    if (node.is_object() && node.contains(key) && node.at(key).is_number()) {
        return node.at(key).get<double>();
    }
    return fallback;
}

bool json_bool_or(const nlohmann::json& node,
                  const std::string& key,
                  bool fallback = false)
{
    if (node.is_object() && node.contains(key) && node.at(key).is_boolean()) {
        return node.at(key).get<bool>();
    }
    return fallback;
}

const nlohmann::json& object_or_empty(const nlohmann::json& node,
                                      const std::string& key)
{
    static const nlohmann::json empty = nlohmann::json::object();
    if (node.is_object() && node.contains(key) && node.at(key).is_object()) {
        return node.at(key);
    }
    return empty;
}

void populate_metadata_from_capture_json(
    SpatialLayoutCalibrationImageSetMetadata* metadata,
    const nlohmann::json& capture)
{
    if (metadata == nullptr || !capture.is_object()) {
        return;
    }
    metadata->filter_state = json_string_or(capture, "filter_state", metadata->filter_state);
    metadata->runtime_filter_state =
        json_string_or(capture, "runtime_filter_state", metadata->runtime_filter_state);
    metadata->light_handling =
        json_string_or(capture, "light_handling", metadata->light_handling);
    metadata->light_state = json_string_or(capture, "light_state", metadata->light_state);
    metadata->projector_state =
        json_string_or(capture, "projector_state", metadata->projector_state);
    metadata->projector_visible_to_camera =
        json_bool_or(capture, "projector_visible_to_camera", metadata->projector_visible_to_camera);
    metadata->requires_filter_reinstalled_repeatably =
        json_bool_or(capture,
                     "requires_filter_reinstalled_repeatably",
                     metadata->requires_filter_reinstalled_repeatably);

    const nlohmann::json& illumination = object_or_empty(capture, "illumination");
    metadata->illumination_spectrum =
        json_string_or(illumination, "spectrum", metadata->illumination_spectrum);
    metadata->illumination_source =
        json_string_or(illumination, "source", metadata->illumination_source);
    metadata->illumination_wavelength_confidence =
        json_string_or(
            illumination,
            "wavelength_confidence",
            metadata->illumination_wavelength_confidence);
    if (illumination.contains("center_wavelength_nm") &&
        illumination["center_wavelength_nm"].is_number()) {
        metadata->illumination_center_wavelength_nm =
            illumination["center_wavelength_nm"].get<double>();
        metadata->has_illumination_center_wavelength_nm = true;
    }
    if (illumination.contains("min_wavelength_nm") &&
        illumination["min_wavelength_nm"].is_number()) {
        metadata->illumination_min_wavelength_nm =
            illumination["min_wavelength_nm"].get<double>();
        metadata->has_illumination_min_wavelength_nm = true;
    }
    if (illumination.contains("max_wavelength_nm") &&
        illumination["max_wavelength_nm"].is_number()) {
        metadata->illumination_max_wavelength_nm =
            illumination["max_wavelength_nm"].get<double>();
        metadata->has_illumination_max_wavelength_nm = true;
    }
    if (illumination.contains("bandwidth_fwhm_nm") &&
        illumination["bandwidth_fwhm_nm"].is_number()) {
        metadata->illumination_bandwidth_fwhm_nm =
            illumination["bandwidth_fwhm_nm"].get<double>();
        metadata->has_illumination_bandwidth_fwhm_nm = true;
    }
}

void populate_calibration_domain_metadata_from_json(
    SpatialLayoutCalibrationImageSetMetadata* metadata,
    const nlohmann::json& domain)
{
    if (metadata == nullptr || !domain.is_object()) {
        return;
    }
    metadata->has_calibration_domain = true;
    metadata->calibration_domain_shape =
        json_string_or(domain, "shape", metadata->calibration_domain_shape);
    metadata->calibration_domain_source =
        json_string_or(domain, "source", metadata->calibration_domain_source);
    metadata->calibration_domain_coordinate_space =
        json_string_or(domain, "coordinate_space", metadata->calibration_domain_coordinate_space);
    metadata->calibration_domain_edge_margin_px =
        json_number_or(domain, "edge_margin_px", metadata->calibration_domain_edge_margin_px);
    metadata->calibration_domain_centroid_gate_outset_px =
        json_number_or(
            domain,
            "centroid_gate_outset_px",
            metadata->calibration_domain_centroid_gate_outset_px);
    metadata->calibration_domain_radius_px =
        json_number_or(domain, "radius_px", metadata->calibration_domain_radius_px);
    metadata->calibration_domain_width_px =
        json_number_or(domain, "width_px", metadata->calibration_domain_width_px);
    metadata->calibration_domain_height_px =
        json_number_or(domain, "height_px", metadata->calibration_domain_height_px);
    metadata->calibration_domain_rotation_deg_clockwise =
        json_number_or(
            domain,
            "rotation_deg_clockwise",
            metadata->calibration_domain_rotation_deg_clockwise);

    if (domain.contains("center_px") &&
        domain["center_px"].is_array() &&
        domain["center_px"].size() >= 2 &&
        domain["center_px"][0].is_number() &&
        domain["center_px"][1].is_number()) {
        metadata->calibration_domain_center_x_px = domain["center_px"][0].get<double>();
        metadata->calibration_domain_center_y_px = domain["center_px"][1].get<double>();
    }
    const nlohmann::json& valid = object_or_empty(domain, "valid_geometry");
    if (json_string_or(valid, "type") == "circle") {
        metadata->has_calibration_domain_valid_circle = true;
        metadata->calibration_domain_valid_center_x_px =
            json_number_or(valid, "cx", metadata->calibration_domain_valid_center_x_px);
        metadata->calibration_domain_valid_center_y_px =
            json_number_or(valid, "cy", metadata->calibration_domain_valid_center_y_px);
        metadata->calibration_domain_valid_radius_px =
            json_number_or(valid, "r", metadata->calibration_domain_valid_radius_px);
    } else if (json_string_or(valid, "type") == "oriented_rectangle") {
        metadata->has_calibration_domain_valid_rectangle = true;
        metadata->calibration_domain_valid_width_px =
            json_number_or(valid, "width", metadata->calibration_domain_valid_width_px);
        metadata->calibration_domain_valid_height_px =
            json_number_or(valid, "height", metadata->calibration_domain_valid_height_px);
    }
}

bool metadata_string_missing(const std::string& value)
{
    return value.empty() || value == "unknown" || value == "multiple";
}

std::string review_plane_group_for_target_plane(const std::string& target_plane)
{
    if (target_plane == "projected_surface") {
        return "Projection Surface";
    }
    if (target_plane == "tank_bottom_inner_surface" ||
        target_plane == "tank_bottom_outer_surface") {
        return "Tank Bottom Inner Surface";
    }
    if (target_plane == "dish_top_rim") {
        return "Dish / Valid Area";
    }
    if (target_plane == "estimated_fish_plane") {
        return "Estimated Fish Plane";
    }
    return "Other / Unknown Plane";
}

bool is_known_review_target_plane(const std::string& target_plane)
{
    return target_plane == "projected_surface" ||
           target_plane == "tank_bottom_outer_surface" ||
           target_plane == "tank_bottom_inner_surface" ||
           target_plane == "estimated_fish_plane" ||
           target_plane == "dish_top_rim";
}

std::string citrus_config_path_from_context_json(const nlohmann::json& context)
{
    if (!context.is_object()) {
        return "";
    }
    const nlohmann::json& ref = object_or_empty(context, "citrus_config_ref");
    std::string path = json_string_or(ref, "path");
    if (!path.empty()) {
        return path;
    }
    path = json_string_or(context, "source_config_path");
    if (!path.empty()) {
        return path;
    }
    path = json_string_or(context, "config_path");
    if (!path.empty()) {
        return path;
    }
    const nlohmann::json& citrus = object_or_empty(context, "citrus");
    if (!citrus.empty()) {
        path = citrus_config_path_from_context_json(citrus);
        if (!path.empty()) {
            return path;
        }
    }
    const nlohmann::json& arena_context = object_or_empty(context, "arena_context");
    if (!arena_context.empty()) {
        path = citrus_config_path_from_context_json(arena_context);
        if (!path.empty()) {
            return path;
        }
    }
    return "";
}

void remember_citrus_config_path_if_present(const nlohmann::json& context,
                                            std::string* citrus_config_path)
{
    if (citrus_config_path == nullptr || !citrus_config_path->empty()) {
        return;
    }
    const std::string path = citrus_config_path_from_context_json(context);
    if (!path.empty()) {
        *citrus_config_path = path;
    }
}

SpatialLayoutCalibrationImageSetMetadata metadata_from_image_set_entry_json(
    const nlohmann::json& image_set,
    const nlohmann::json& image)
{
    SpatialLayoutCalibrationImageSetMetadata metadata;
    metadata.image_set_purpose =
        json_string_or(image, "purpose", json_string_or(image_set, "purpose", "unknown"));
    metadata.image_set_target_plane =
        json_string_or(image, "target_plane", json_string_or(image_set, "target_plane", "unknown"));
    metadata.image_set_image_role = json_string_or(image, "role", "unknown");
    metadata.image_set_notes =
        json_string_or(image, "notes", json_string_or(image_set, "operator_notes", ""));
    metadata.capture_stage =
        json_string_or(image, "capture_stage", json_string_or(image_set, "capture_stage", "unknown"));
    metadata.plane_z_mm_nominal =
        json_number_or(image, "plane_z_mm_nominal", json_number_or(image_set, "plane_z_mm_nominal", 0.0));
    metadata.has_plane_z_mm_nominal =
        (image.contains("plane_z_mm_nominal") && image["plane_z_mm_nominal"].is_number()) ||
        (image_set.contains("plane_z_mm_nominal") && image_set["plane_z_mm_nominal"].is_number());
    metadata.plane_z_mm_uncertainty =
        json_number_or(
            image,
            "plane_z_mm_uncertainty",
            json_number_or(image_set, "plane_z_mm_uncertainty", 0.0));
    metadata.has_plane_z_mm_uncertainty =
        (image.contains("plane_z_mm_uncertainty") && image["plane_z_mm_uncertainty"].is_number()) ||
        (image_set.contains("plane_z_mm_uncertainty") && image_set["plane_z_mm_uncertainty"].is_number());
    metadata.wet_or_dry =
        json_string_or(image, "wet_or_dry", json_string_or(image_set, "wet_or_dry", "unknown"));
    metadata.imaging_shelf_installed =
        json_bool_or(
            image,
            "imaging_shelf_installed",
            json_bool_or(image_set, "imaging_shelf_installed", false));
    metadata.dish_installed =
        json_bool_or(image, "dish_installed", json_bool_or(image_set, "dish_installed", false));
    metadata.dish_id = json_string_or(image, "dish_id", json_string_or(image_set, "dish_id"));
    metadata.water_fill_mm =
        json_number_or(image, "water_fill_mm", json_number_or(image_set, "water_fill_mm", 0.0));
    metadata.has_water_fill_mm =
        (image.contains("water_fill_mm") && image["water_fill_mm"].is_number()) ||
        (image_set.contains("water_fill_mm") && image_set["water_fill_mm"].is_number());
    metadata.fill_state =
        json_string_or(image, "fill_state", json_string_or(image_set, "fill_state", "unknown"));
    metadata.open_water_surface_present =
        json_bool_or(
            image,
            "open_water_surface_present",
            json_bool_or(image_set, "open_water_surface_present", false));
    metadata.water_settled_status =
        json_string_or(
            image,
            "water_settled_status",
            json_string_or(image_set, "water_settled_status", "unknown"));
    metadata.target_method =
        json_string_or(image, "target_method", json_string_or(image_set, "target_method", "unknown"));
    metadata.pattern_type =
        json_string_or(image, "pattern_type", json_string_or(image_set, "pattern_type", "unknown"));
    metadata.pattern_domain =
        json_string_or(image, "pattern_domain", json_string_or(image_set, "pattern_domain", "unknown"));
    metadata.matched_parity_group_id =
        json_string_or(
            image,
            "matched_parity_group_id",
            json_string_or(image_set, "matched_parity_group_id"));
    metadata.parity_group_role =
        json_string_or(
            image,
            "parity_group_role",
            json_string_or(image_set, "parity_group_role"));
    metadata.reference_only =
        json_bool_or(image, "reference_only", json_bool_or(image_set, "reference_only", false));
    metadata.physical_target_used =
        json_bool_or(
            image,
            "physical_target_used",
            json_bool_or(image_set, "physical_target_used", false));
    metadata.projected_pattern_used_as_coordinate_target =
        json_bool_or(
            image,
            "projected_pattern_used_as_coordinate_target",
            json_bool_or(image_set, "projected_pattern_used_as_coordinate_target", false));
    metadata.plane_id = json_string_or(image, "plane_id", json_string_or(image_set, "plane_id"));
    metadata.z_mm_relative_to_projection_surface =
        json_number_or(
            image,
            "z_mm_relative_to_projection_surface",
            json_number_or(image_set, "z_mm_relative_to_projection_surface", 0.0));
    metadata.has_z_mm_relative_to_projection_surface =
        (image.contains("z_mm_relative_to_projection_surface") &&
         image["z_mm_relative_to_projection_surface"].is_number()) ||
        (image_set.contains("z_mm_relative_to_projection_surface") &&
         image_set["z_mm_relative_to_projection_surface"].is_number());
    metadata.target_id =
        json_string_or(image, "target_id", json_string_or(image_set, "target_id"));
    metadata.target_design =
        json_string_or(image, "target_design", json_string_or(image_set, "target_design"));
    metadata.physical_target_grid_spacing_mm =
        json_number_or(
            image,
            "physical_target_grid_spacing_mm",
            json_number_or(image_set, "physical_target_grid_spacing_mm", 0.0));
    metadata.has_physical_target_grid_spacing_mm =
        (image.contains("physical_target_grid_spacing_mm") &&
         image["physical_target_grid_spacing_mm"].is_number()) ||
        (image_set.contains("physical_target_grid_spacing_mm") &&
         image_set["physical_target_grid_spacing_mm"].is_number());
    metadata.physical_target_origin_definition =
        json_string_or(
            image,
            "physical_target_origin_definition",
            json_string_or(image_set, "physical_target_origin_definition"));
    metadata.physical_target_x_orientation_marker_definition =
        json_string_or(
            image,
            "physical_target_x_orientation_marker_definition",
            json_string_or(image_set, "physical_target_x_orientation_marker_definition"));

    const nlohmann::json& capture =
        image.contains("capture") && image["capture"].is_object()
            ? image["capture"]
            : object_or_empty(image_set, "capture");
    populate_metadata_from_capture_json(&metadata, capture);

    const nlohmann::json& projected_pattern =
        image.contains("projected_pattern") && image["projected_pattern"].is_object()
            ? image["projected_pattern"]
            : object_or_empty(image_set, "projected_pattern");
    metadata.image_set_projected_pattern_id =
        json_string_or(projected_pattern, "pattern_id", "unknown");
    metadata.image_set_projected_pattern_type =
        json_string_or(projected_pattern, "type", "unknown");
    const nlohmann::json& scale_target =
        image.contains("scale_target") && image["scale_target"].is_object()
            ? image["scale_target"]
            : object_or_empty(image_set, "scale_target");
    metadata.image_set_scale_target_type =
        json_string_or(scale_target, "target_type", "unknown");

    const nlohmann::json& observations = object_or_empty(image, "observations");
    if (observations.contains("calibration_domain") &&
        observations["calibration_domain"].is_object()) {
        populate_calibration_domain_metadata_from_json(
            &metadata,
            observations["calibration_domain"]);
    } else if (observations.contains("observed_domain") &&
               observations["observed_domain"].is_object()) {
        populate_calibration_domain_metadata_from_json(
            &metadata,
            observations["observed_domain"]);
    }
    if (image.contains("citrus_projection_snapshot_pre_capture") &&
        image["citrus_projection_snapshot_pre_capture"].is_object()) {
        metadata.citrus_projection_snapshot_pre_capture =
            image["citrus_projection_snapshot_pre_capture"];
    }
    if (image.contains("citrus_projection_snapshot_post_capture") &&
        image["citrus_projection_snapshot_post_capture"].is_object()) {
        metadata.citrus_projection_snapshot_post_capture =
            image["citrus_projection_snapshot_post_capture"];
    }
    if (image.contains("citrus_projection_epoch_consistency") &&
        image["citrus_projection_epoch_consistency"].is_object()) {
        metadata.citrus_projection_epoch_consistency =
            image["citrus_projection_epoch_consistency"];
    }
    const auto guided_json = [&](const char* key) -> nlohmann::json {
        if (image.contains(key) && image[key].is_object()) {
            return image[key];
        }
        if (image_set.contains(key) && image_set[key].is_object()) {
            return image_set[key];
        }
        return nlohmann::json::object();
    };
    metadata.citrus_calibration_scene_pre_capture =
        guided_json("citrus_calibration_scene_pre_capture");
    metadata.citrus_calibration_scene_post_capture =
        guided_json("citrus_calibration_scene_post_capture");
    metadata.citrus_calibration_scene_consistency =
        guided_json("citrus_calibration_scene_consistency");
    metadata.citrus_calibration_scene_restore_status =
        guided_json("citrus_calibration_scene_restore_status");
    metadata.capture_group_membership =
        guided_json("capture_group_membership");
    return metadata;
}

void add_unique_session_review_warning(std::vector<std::string>* warnings,
                                       const std::string& warning)
{
    if (warnings == nullptr || warning.empty()) {
        return;
    }
    if (std::find(warnings->begin(), warnings->end(), warning) == warnings->end()) {
        warnings->push_back(warning);
    }
}

bool parse_top_rim_circle_from_observation(const nlohmann::json& observation,
                                           double* cx,
                                           double* cy,
                                           double* radius)
{
    if (cx == nullptr || cy == nullptr || radius == nullptr || !observation.is_object()) {
        return false;
    }
    const auto parse_boundary_geometry = [&](const char* field) {
        const nlohmann::json& boundary = object_or_empty(observation, field);
        const nlohmann::json& geometry = object_or_empty(boundary, "geometry");
        const nlohmann::json& center = object_or_empty(geometry, "center_px");
        if (!center.contains("x") || !center["x"].is_number() ||
            !center.contains("y") || !center["y"].is_number() ||
            !geometry.contains("radius_px") || !geometry["radius_px"].is_number()) {
            return false;
        }
        *cx = center["x"].get<double>();
        *cy = center["y"].get<double>();
        *radius = geometry["radius_px"].get<double>();
        return *radius > 0.0;
    };
    if (parse_boundary_geometry("accepted_inner_rim_boundary") ||
        parse_boundary_geometry("accepted_experimental_area_boundary") ||
        parse_boundary_geometry("observed_boundary")) {
        return true;
    }

    // Last-resort compatibility for old links that only embedded the eroded mask.
    const nlohmann::json& accepted_mask = object_or_empty(observation, "accepted_mask");
    const nlohmann::json& center = object_or_empty(accepted_mask, "center_px");
    if (center.contains("x") && center["x"].is_number() &&
        center.contains("y") && center["y"].is_number() &&
        accepted_mask.contains("radius_px") && accepted_mask["radius_px"].is_number()) {
        *cx = center["x"].get<double>();
        *cy = center["y"].get<double>();
        *radius = accepted_mask["radius_px"].get<double>();
        return *radius > 0.0;
    }
    return false;
}

std::string build_session_review_label(const SpatialLayoutSessionReviewImage& image)
{
    std::ostringstream label;
    if (!image.camera_serial.empty()) {
        label << "Cam" << image.camera_serial << " ";
    }
    label << (image.purpose.empty() ? "artifact" : image.purpose);
    if (!image.capture_stage.empty() && image.capture_stage != "unknown") {
        label << " / " << image.capture_stage;
    }
    if (!image.target_plane.empty() && image.target_plane != "unknown") {
        label << " / " << image.target_plane;
    }
    if (!image.role.empty() && image.role != "unknown") {
        label << " / " << image.role;
    }
    if (!image.created_utc.empty()) {
        label << " @ " << image.created_utc;
    }
    if (!image.artifact_id.empty()) {
        label << " [" << image.artifact_id;
        if (image.image_index >= 0) {
            label << "#" << image.image_index;
        }
        label << "]";
    }
    return label.str();
}

bool add_image_set_review_entries(const std::filesystem::path& artifact_dir,
                                  const std::string& artifact_id,
                                  const nlohmann::json& image_set,
                                  std::vector<SpatialLayoutSessionReviewImage>* entries,
                                  std::string* error_out)
{
    if (entries == nullptr) {
        if (error_out) {
            *error_out = "Null session-review image list.";
        }
        return false;
    }
    if (!image_set.contains("images") || !image_set["images"].is_array()) {
        return true;
    }

    const nlohmann::json& camera = object_or_empty(image_set, "camera");
    const std::string camera_serial = json_string_or(camera, "serial");
    const std::string camera_name = json_string_or(camera, "name");
    const nlohmann::json& rig_context = object_or_empty(image_set, "rig_context");
    const nlohmann::json& linked_observations =
        object_or_empty(image_set, "linked_observations");
    const nlohmann::json& linked_top_rim =
        object_or_empty(linked_observations, "accepted_top_rim_observation");
    double linked_top_rim_cx = 0.0;
    double linked_top_rim_cy = 0.0;
    double linked_top_rim_radius = 0.0;
    const bool has_linked_top_rim_circle =
        parse_top_rim_circle_from_observation(
            linked_top_rim,
            &linked_top_rim_cx,
            &linked_top_rim_cy,
            &linked_top_rim_radius);

    int image_index = 0;
    for (const nlohmann::json& image : image_set["images"]) {
        if (!image.is_object()) {
            ++image_index;
            continue;
        }
        const std::string relative_path = json_string_or(image, "path");
        if (relative_path.empty()) {
            ++image_index;
            continue;
        }
        SpatialLayoutSessionReviewImage entry;
        entry.valid = true;
        entry.artifact_id = artifact_id;
        entry.artifact_schema_id = orange::calibration::kCalibrationImageSetSchemaId;
        entry.image_index = image_index;
        entry.purpose = json_string_or(image, "purpose", json_string_or(image_set, "purpose"));
        entry.target_plane =
            json_string_or(image, "target_plane", json_string_or(image_set, "target_plane"));
        entry.capture_stage =
            json_string_or(image, "capture_stage", json_string_or(image_set, "capture_stage"));
        entry.plane_group = review_plane_group_for_target_plane(entry.target_plane);
        entry.role = json_string_or(image, "role");
        const nlohmann::json& image_rig_context =
            image.contains("rig_context") && image["rig_context"].is_object()
                ? image["rig_context"]
                : rig_context;
        entry.rig_id = json_string_or(image_rig_context, "rig_id");
        entry.canvas_id = json_string_or(image_rig_context, "canvas_id");
        entry.arena_id = json_string_or(image_rig_context, "arena_id");
        entry.camera_serial = camera_serial;
        entry.camera_name = camera_name;
        entry.image_set_path = (artifact_dir / "image_set.json").generic_string();
        entry.image_path = (artifact_dir / relative_path).lexically_normal().generic_string();
        entry.metadata = metadata_from_image_set_entry_json(image_set, image);
        entry.capture_group_id =
            json_string_or(object_or_empty(image, "capture"), "capture_group_id");
        entry.capture_mode =
            json_string_or(object_or_empty(image, "capture"), "capture_mode", entry.capture_mode);
        entry.created_utc =
            json_string_or(object_or_empty(image, "capture"), "timestamp_utc",
                           json_string_or(image_set, "created_utc"));
        entry.source_array_role =
            json_string_or(image, "source_array_role", "images_full");
        const nlohmann::json& shape = object_or_empty(image, "image_shape");
        entry.width = static_cast<int>(json_number_or(shape, "width", 0.0));
        entry.height = static_cast<int>(json_number_or(shape, "height", 0.0));
        entry.has_linked_accepted_top_rim = has_linked_top_rim_circle;
        if (has_linked_top_rim_circle && entry.target_plane != "projected_surface") {
            entry.has_accepted_circle = true;
            entry.accepted_circle_cx = linked_top_rim_cx;
            entry.accepted_circle_cy = linked_top_rim_cy;
            entry.accepted_circle_r = linked_top_rim_radius;
        }
        entry.label = build_session_review_label(entry);
        entries->push_back(std::move(entry));
        ++image_index;
    }
    return true;
}

void add_top_rim_review_entry(const std::filesystem::path& artifact_dir,
                              const nlohmann::json& observation,
                              const std::string& artifact_id,
                              const std::string& relative_path,
                              const std::string& role,
                              std::vector<SpatialLayoutSessionReviewImage>* entries)
{
    if (entries == nullptr || relative_path.empty()) {
        return;
    }
    SpatialLayoutSessionReviewImage entry;
    entry.valid = true;
    entry.artifact_id = artifact_id;
    entry.artifact_schema_id = orange::calibration::kDishTopRimObservationSchemaId;
    entry.purpose = "dish_top_rim";
    entry.target_plane = "dish_top_rim";
    entry.capture_stage = "dish_top_observation";
    entry.plane_group = review_plane_group_for_target_plane(entry.target_plane);
    entry.role = role;
    entry.image_path = (artifact_dir / relative_path).lexically_normal().generic_string();
    entry.image_set_path = (artifact_dir / "image_set.json").generic_string();
    entry.observation_path = (artifact_dir / "observation.json").generic_string();
    entry.created_utc = json_string_or(observation, "created_utc");
    const nlohmann::json& camera = object_or_empty(observation, "camera");
    entry.camera_serial = json_string_or(camera, "serial");
    entry.camera_name = json_string_or(camera, "name");
    const nlohmann::json& arena_context = object_or_empty(observation, "arena_context");
    entry.rig_id = json_string_or(arena_context, "rig_id");
    entry.canvas_id = json_string_or(arena_context, "canvas_id");
    entry.arena_id = json_string_or(arena_context, "arena_id");
    const nlohmann::json& capture = object_or_empty(observation, "capture");
    entry.capture_group_id = json_string_or(capture, "capture_group_id");
    entry.capture_mode = json_string_or(capture, "capture_mode", "loaded_top_rim_observation");
    populate_metadata_from_capture_json(&entry.metadata, capture);
    entry.metadata.image_set_purpose = "dish_top_rim";
    entry.metadata.image_set_target_plane = "dish_top_rim";
    entry.metadata.image_set_image_role = role;
    entry.metadata.image_set_projected_pattern_id = "none";
    entry.metadata.image_set_projected_pattern_type = "none";
    entry.metadata.image_set_scale_target_type = "unknown";
    entry.metadata.capture_stage = "dish_top_observation";

    double cx = 0.0;
    double cy = 0.0;
    double radius = 0.0;
    if (parse_top_rim_circle_from_observation(observation, &cx, &cy, &radius)) {
        entry.has_accepted_circle = true;
        entry.has_linked_accepted_top_rim = true;
        entry.accepted_circle_cx = cx;
        entry.accepted_circle_cy = cy;
        entry.accepted_circle_r = radius;
    }
    entry.label = build_session_review_label(entry);
    entries->push_back(std::move(entry));
}

bool add_top_rim_review_entries(const std::filesystem::path& artifact_dir,
                                const std::string& artifact_id,
                                const nlohmann::json& observation,
                                std::vector<SpatialLayoutSessionReviewImage>* entries)
{
    if (entries == nullptr || !observation.is_object()) {
        return false;
    }
    if (observation.contains("source_frames") &&
        observation["source_frames"].is_array() &&
        !observation["source_frames"].empty() &&
        observation["source_frames"][0].is_object()) {
        add_top_rim_review_entry(
            artifact_dir,
            observation,
            artifact_id,
            json_string_or(observation["source_frames"][0], "path"),
            "source_frame",
            entries);
    } else {
        add_top_rim_review_entry(
            artifact_dir,
            observation,
            artifact_id,
            json_string_or(object_or_empty(observation, "artifacts"), "source_frame_path"),
            "source_frame",
            entries);
    }
    const nlohmann::json& review = object_or_empty(observation, "review_artifacts");
    add_top_rim_review_entry(
        artifact_dir,
        observation,
        artifact_id,
        json_string_or(review, "top_rim_overlay_path"),
        "top_rim_overlay",
        entries);
    add_top_rim_review_entry(
        artifact_dir,
        observation,
        artifact_id,
        json_string_or(review, "valid_detection_overlay_path"),
        "valid_detection_overlay",
        entries);
    return true;
}

std::string session_review_context_label(const SpatialLayoutSessionReviewImage& image)
{
    std::ostringstream oss;
    if (!image.camera_serial.empty()) {
        oss << "Cam" << image.camera_serial;
    } else {
        oss << "unknown camera";
    }
    if (!image.canvas_id.empty()) {
        oss << " / " << image.canvas_id;
    }
    if (!image.arena_id.empty()) {
        oss << " / " << image.arena_id;
    }
    if (!image.artifact_id.empty()) {
        oss << " / " << image.artifact_id;
    }
    if (image.image_index >= 0) {
        oss << "#" << image.image_index;
    }
    if (!image.capture_stage.empty()) {
        oss << " / " << image.capture_stage;
    }
    return oss.str();
}

void add_session_review_image_warnings(
    const SpatialLayoutSessionReviewImage& image,
    std::vector<std::string>* warnings)
{
    const std::string context = session_review_context_label(image);
    if (metadata_string_missing(image.purpose)) {
        add_unique_session_review_warning(
            warnings,
            context + " is missing image-set purpose metadata.");
    }
    if (metadata_string_missing(image.capture_stage)) {
        add_unique_session_review_warning(
            warnings,
            context + " is missing capture_stage metadata.");
    }
    if (metadata_string_missing(image.target_plane)) {
        add_unique_session_review_warning(
            warnings,
            context + " is missing target_plane metadata.");
    } else if (!is_known_review_target_plane(image.target_plane)) {
        add_unique_session_review_warning(
            warnings,
            context + " uses unknown target_plane=" + image.target_plane + ".");
    } else if (image.target_plane == "estimated_fish_plane") {
        add_unique_session_review_warning(
            warnings,
            context +
                " uses target_plane=estimated_fish_plane; verify the target was physically "
                "raised to fish height. If it was on the dish/tank bottom, prefer "
                "target_plane=tank_bottom_inner_surface with behavior-plane proxy metadata.");
    }
    if (metadata_string_missing(image.role)) {
        add_unique_session_review_warning(
            warnings,
            context + " is missing image role metadata.");
    }
    if (image.purpose == "homography_grid" &&
        metadata_string_missing(image.metadata.image_set_projected_pattern_id)) {
        add_unique_session_review_warning(
            warnings,
            context + " homography_grid is missing projected_pattern.pattern_id.");
    }
    if (image.capture_stage == "projected_surface_dry_reference" &&
        !image.metadata.reference_only) {
        add_unique_session_review_warning(
            warnings,
            context + " dry projection-surface reference is not marked reference_only.");
    }
    if (image.capture_stage == "projected_surface_wet_runtime_stack" &&
        image.metadata.matched_parity_group_id.empty()) {
        add_unique_session_review_warning(
            warnings,
            context + " wet runtime-stack capture is missing matched_parity_group_id.");
    }
    if (image.target_plane == "tank_bottom_inner_surface" &&
        image.metadata.has_calibration_domain &&
        image.metadata.calibration_domain_shape == "circle" &&
        !image.has_linked_accepted_top_rim) {
        add_unique_session_review_warning(
            warnings,
            context +
                " has a circular tank-bottom domain but no linked accepted top-rim "
                "observation; Citrus should treat the capture-time observed_domain as "
                "diagnostic rather than authoritative.");
    }
    if (image.metadata.capture_group_membership.is_object() &&
        !image.metadata.capture_group_membership.empty()) {
        const std::string group_status =
            image.metadata.capture_group_membership.value(
                "status", std::string("unknown"));
        if (group_status != "complete") {
            add_unique_session_review_warning(
                warnings,
                context + " belongs to capture group " +
                    image.metadata.capture_group_membership.value(
                        "capture_group_id", image.capture_group_id) +
                    " with status=" + group_status + ".");
        }
    }
    if (image.metadata.citrus_calibration_scene_consistency.is_object() &&
        !image.metadata.citrus_calibration_scene_consistency.empty() &&
        image.metadata.citrus_calibration_scene_consistency.value(
            "status", std::string("unavailable")) != "same_scene") {
        add_unique_session_review_warning(
            warnings,
            context +
                " does not have one verified unchanged Citrus scene across capture.");
    }
    if (image.metadata.citrus_calibration_scene_pre_capture.is_object() &&
        !image.metadata.citrus_calibration_scene_pre_capture.empty() &&
        image.metadata.citrus_calibration_scene_restore_status.value(
            "state", std::string()) != "restored") {
        add_unique_session_review_warning(
            warnings,
            context + " lacks a verified Citrus prior-scene restore fence.");
    }
}

void rebuild_session_review_camera_groups(
    const std::vector<SpatialLayoutSessionReviewImage>& images,
    std::vector<SpatialLayoutSessionReviewCameraGroup>* groups,
    std::vector<std::string>* warnings)
{
    if (groups == nullptr || warnings == nullptr) {
        return;
    }
    groups->clear();
    warnings->clear();

    for (size_t image_index = 0; image_index < images.size(); ++image_index) {
        const SpatialLayoutSessionReviewImage& image = images[image_index];
        add_session_review_image_warnings(image, warnings);

        auto camera_group_it = std::find_if(
            groups->begin(),
            groups->end(),
            [&](const SpatialLayoutSessionReviewCameraGroup& group) {
                return group.camera_serial == image.camera_serial;
            });
        if (camera_group_it == groups->end()) {
            SpatialLayoutSessionReviewCameraGroup camera_group;
            camera_group.camera_serial = image.camera_serial;
            camera_group.camera_name = image.camera_name;
            if (!camera_group.camera_serial.empty()) {
                camera_group.label = "Cam" + camera_group.camera_serial;
            } else {
                camera_group.label = "Unknown camera";
            }
            if (!camera_group.camera_name.empty()) {
                camera_group.label += " (" + camera_group.camera_name + ")";
            }
            groups->push_back(std::move(camera_group));
            camera_group_it = std::prev(groups->end());
        }

        const std::string image_plane_group =
            image.plane_group.empty()
                ? review_plane_group_for_target_plane(image.target_plane)
                : image.plane_group;
        auto plane_group_it = std::find_if(
            camera_group_it->plane_groups.begin(),
            camera_group_it->plane_groups.end(),
            [&](const SpatialLayoutSessionReviewPlaneGroup& group) {
                return group.plane_group == image_plane_group;
            });
        if (plane_group_it == camera_group_it->plane_groups.end()) {
            SpatialLayoutSessionReviewPlaneGroup group;
            group.plane_group = image_plane_group;
            group.label = group.plane_group;
            group.camera_serial = image.camera_serial;
            group.camera_name = image.camera_name;
            group.rig_id = image.rig_id;
            group.canvas_id = image.canvas_id;
            group.arena_id = image.arena_id;
            camera_group_it->plane_groups.push_back(std::move(group));
            plane_group_it = std::prev(camera_group_it->plane_groups.end());
        }

        plane_group_it->has_linked_accepted_top_rim =
            plane_group_it->has_linked_accepted_top_rim ||
            image.has_linked_accepted_top_rim;
        plane_group_it->image_indices.push_back(static_cast<int>(image_index));
        auto purpose_it = std::find_if(
            plane_group_it->purpose_groups.begin(),
            plane_group_it->purpose_groups.end(),
            [&](const SpatialLayoutSessionReviewPurposeGroup& purpose_group) {
                return purpose_group.purpose == image.purpose &&
                       purpose_group.target_plane == image.target_plane;
            });
        if (purpose_it == plane_group_it->purpose_groups.end()) {
            SpatialLayoutSessionReviewPurposeGroup purpose_group;
            purpose_group.purpose = image.purpose.empty() ? "unknown" : image.purpose;
            purpose_group.target_plane =
                image.target_plane.empty() ? "unknown" : image.target_plane;
            plane_group_it->purpose_groups.push_back(std::move(purpose_group));
            purpose_it = std::prev(plane_group_it->purpose_groups.end());
        }
        purpose_it->image_indices.push_back(static_cast<int>(image_index));
    }
}

bool resolve_calibration_session_index_path(
    const std::filesystem::path& selected_path,
    std::filesystem::path* index_path_out,
    std::string* error_out)
{
    if (index_path_out == nullptr) {
        if (error_out) {
            *error_out = "Null calibration session index path destination.";
        }
        return false;
    }
    std::error_code status_error;
    if (std::filesystem::is_directory(selected_path, status_error)) {
        *index_path_out = selected_path / orange::gui::spatial_layout::kCalibrationSessionIndexFilename;
        return true;
    }
    if (selected_path.filename() == orange::gui::spatial_layout::kCalibrationSessionIndexFilename) {
        *index_path_out = selected_path;
        return true;
    }
    if (selected_path.filename() == orange::gui::spatial_layout::kCalibrationSessionFilename) {
        *index_path_out =
            selected_path.parent_path() / orange::gui::spatial_layout::kCalibrationSessionIndexFilename;
        return true;
    }

    nlohmann::json selected_json;
    if (!read_json_file(selected_path, &selected_json, error_out)) {
        return false;
    }
    const std::string schema_id = selected_json.value("schema_id", "");
    if (schema_id == "orange.calibration.session_index") {
        *index_path_out = selected_path;
        return true;
    }
    if (schema_id == "orange.calibration.session") {
        std::string session_index_filename =
            orange::gui::spatial_layout::kCalibrationSessionIndexFilename;
        const nlohmann::json& files = object_or_empty(selected_json, "files");
        session_index_filename =
            json_string_or(files, "session_index", session_index_filename);
        *index_path_out = selected_path.parent_path() / session_index_filename;
        return true;
    }
    if (error_out) {
        *error_out =
            "Select a calibration session folder, session.json, or session_index.json.";
    }
    return false;
}

} // namespace

bool load_spatial_calibration_session_review(
    SpatialLayoutUiState* ui_state,
    const std::filesystem::path& selected_path,
    std::string* status_out,
    std::string* error_out)
{
    if (ui_state == nullptr) {
        if (error_out) {
            *error_out = "Spatial layout UI state is null.";
        }
        return false;
    }
    std::filesystem::path index_path;
    if (!resolve_calibration_session_index_path(selected_path, &index_path, error_out)) {
        return false;
    }
    nlohmann::json index;
    if (!read_json_file(index_path, &index, error_out)) {
        return false;
    }
    if (index.value("schema_id", "") != "orange.calibration.session_index") {
        if (error_out) {
            *error_out = "Selected session index has unexpected schema_id: " +
                         index.value("schema_id", std::string());
        }
        return false;
    }
    const std::filesystem::path session_dir =
        index.contains("session_dir") && index["session_dir"].is_string()
            ? std::filesystem::path(index["session_dir"].get<std::string>())
            : index_path.parent_path();
    const std::filesystem::path artifacts_dir =
        index.contains("artifacts_dir") && index["artifacts_dir"].is_string()
            ? std::filesystem::path(index["artifacts_dir"].get<std::string>())
            : session_dir / "artifacts";
    std::string citrus_config_path;
    {
        nlohmann::json session;
        const std::filesystem::path session_path =
            index_path.parent_path() / orange::gui::spatial_layout::kCalibrationSessionFilename;
        if (std::filesystem::exists(session_path) &&
            read_json_file(session_path, &session, nullptr)) {
            remember_citrus_config_path_if_present(
                object_or_empty(session, "context"),
                &citrus_config_path);
        }
    }
    const nlohmann::json& artifacts_by_id = object_or_empty(index, "artifacts_by_id");
    std::vector<std::string> artifact_ids;
    if (index.contains("artifact_order") && index["artifact_order"].is_array()) {
        for (const nlohmann::json& value : index["artifact_order"]) {
            if (value.is_string()) {
                artifact_ids.push_back(value.get<std::string>());
            }
        }
    }
    for (auto it = artifacts_by_id.begin(); it != artifacts_by_id.end(); ++it) {
        if (std::find(artifact_ids.begin(), artifact_ids.end(), it.key()) == artifact_ids.end()) {
            artifact_ids.push_back(it.key());
        }
    }

    std::vector<SpatialLayoutSessionReviewImage> review_images;
    for (const std::string& artifact_id : artifact_ids) {
        if (!artifacts_by_id.contains(artifact_id) ||
            !artifacts_by_id[artifact_id].is_object()) {
            continue;
        }
        const nlohmann::json& artifact_entry = artifacts_by_id[artifact_id];
        remember_citrus_config_path_if_present(artifact_entry, &citrus_config_path);
        const std::string schema_id = json_string_or(artifact_entry, "artifact_schema_id");
        std::filesystem::path manifest_path;
        const std::string relative_manifest =
            json_string_or(artifact_entry, "relative_manifest_path");
        if (!relative_manifest.empty()) {
            manifest_path = session_dir / relative_manifest;
        } else {
            manifest_path = artifacts_dir / artifact_id / kSpatialLayoutManifestFilename;
        }
        const std::filesystem::path artifact_dir = manifest_path.parent_path();

        if (schema_id == orange::calibration::kCalibrationImageSetSchemaId) {
            nlohmann::json image_set;
            const std::filesystem::path image_set_path = artifact_dir / "image_set.json";
            if (std::filesystem::exists(image_set_path) &&
                !read_json_file(image_set_path, &image_set, error_out)) {
                return false;
            }
            if (image_set.is_object() &&
                !add_image_set_review_entries(
                    artifact_dir,
                    artifact_id,
                    image_set,
                    &review_images,
                    error_out)) {
                return false;
            }
            remember_citrus_config_path_if_present(image_set, &citrus_config_path);
        } else if (schema_id == orange::calibration::kDishTopRimObservationSchemaId) {
            nlohmann::json observation;
            const std::filesystem::path observation_path = artifact_dir / "observation.json";
            if (std::filesystem::exists(observation_path) &&
                !read_json_file(observation_path, &observation, error_out)) {
                return false;
            }
            if (observation.is_object() &&
                !add_top_rim_review_entries(
                    artifact_dir,
                    artifact_id,
                    observation,
                    &review_images)) {
                return false;
            }
            remember_citrus_config_path_if_present(observation, &citrus_config_path);
        }
    }

    std::vector<SpatialLayoutSessionReviewCameraGroup> review_groups;
    std::vector<std::string> review_warnings;
    rebuild_session_review_camera_groups(
        review_images,
        &review_groups,
        &review_warnings);

    ui_state->session_review_images = std::move(review_images);
    ui_state->session_review_camera_groups = std::move(review_groups);
    ui_state->session_review_warnings = std::move(review_warnings);
    ui_state->selected_session_review_image =
        ui_state->session_review_images.empty() ? -1 : 0;
    ui_state->loaded_calibration_session_index_path = index_path.generic_string();
    ui_state->loaded_calibration_session_citrus_config_path = citrus_config_path;
    ui_state->calibration_session_id = index.value("session_id", session_dir.filename().generic_string());
    ui_state->calibration_session_dir = session_dir.generic_string();

    if (status_out) {
        std::ostringstream status;
        status << "Loaded calibration session " << ui_state->calibration_session_id
               << " with " << ui_state->session_review_images.size()
               << " review image(s) across "
               << ui_state->session_review_camera_groups.size()
               << " camera group(s).";
        if (!ui_state->session_review_warnings.empty()) {
            status << " Warnings=" << ui_state->session_review_warnings.size() << ".";
        }
        *status_out = status.str();
    }
    return true;
}

bool load_rgba_image_from_path(const std::filesystem::path& image_path,
                               std::vector<unsigned char>* rgba_out,
                               int* width_out,
                               int* height_out,
                               std::string* error_out)
{
    if (rgba_out == nullptr || width_out == nullptr || height_out == nullptr) {
        if (error_out) {
            *error_out = "Null image-load destination.";
        }
        return false;
    }
    cv::Mat image = cv::imread(image_path.string(), cv::IMREAD_UNCHANGED);
    if (image.empty()) {
        if (error_out) {
            *error_out = "Failed to load calibration image: " + image_path.string();
        }
        return false;
    }
    cv::Mat rgba;
    if (image.channels() == 1) {
        cv::cvtColor(image, rgba, cv::COLOR_GRAY2RGBA);
    } else if (image.channels() == 3) {
        cv::cvtColor(image, rgba, cv::COLOR_BGR2RGBA);
    } else if (image.channels() == 4) {
        cv::cvtColor(image, rgba, cv::COLOR_BGRA2RGBA);
    } else {
        if (error_out) {
            *error_out = "Unsupported calibration image channel count in " +
                         image_path.string() + ": " + std::to_string(image.channels());
        }
        return false;
    }
    *width_out = rgba.cols;
    *height_out = rgba.rows;
    rgba_out->assign(rgba.data, rgba.data + rgba.total() * rgba.elemSize());
    return true;
}

} // namespace orange::gui::spatial_layout
