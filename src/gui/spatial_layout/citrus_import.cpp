#include "gui/spatial_layout/citrus_import.h"
#include "gui/spatial_layout/canvas_projection_geometry_identity.h"
#include "gui/spatial_layout/sha256.h"

#include <opencv2/opencv.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <filesystem>
#include <sstream>

namespace orange::gui::spatial_layout {
namespace {

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

bool parse_required_json_number(const nlohmann::json& node,
                                const char* field,
                                double* out,
                                std::string* error_out)
{
    if (out == nullptr) {
        if (error_out) {
            *error_out = "Null number destination.";
        }
        return false;
    }
    if (!node.contains(field) || !node.at(field).is_number()) {
        if (error_out) {
            *error_out = std::string("Missing numeric field: ") + field;
        }
        return false;
    }
    *out = node.at(field).get<double>();
    return true;
}

bool parse_optional_json_number(const nlohmann::json& node,
                                const char* field,
                                double* out)
{
    if (out == nullptr || !node.contains(field) || !node.at(field).is_number()) {
        return false;
    }
    *out = node.at(field).get<double>();
    return true;
}

bool json_string_equals_ignore_case(const nlohmann::json& node,
                                    const char* field,
                                    const std::string& expected_uppercase)
{
    if (!node.contains(field) || !node.at(field).is_string()) {
        return false;
    }
    std::string value = node.at(field).get<std::string>();
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::toupper(c));
    });
    return value == expected_uppercase;
}

std::string safe_path_component(std::string value)
{
    for (char& ch : value) {
        if (!(std::isalnum(static_cast<unsigned char>(ch)) || ch == '-' ||
              ch == '_' || ch == '.')) {
            ch = '_';
        }
    }
    return value.empty() ? "unknown" : value;
}

bool read_json_file(const std::filesystem::path& path,
                    nlohmann::json* value_out,
                    std::string* bytes_out,
                    std::string* error_out)
{
    std::string bytes;
    if (!checksum::read_file(path, &bytes, error_out)) return false;
    nlohmann::json value = nlohmann::json::parse(bytes, nullptr, false);
    if (value.is_discarded()) {
        if (error_out) *error_out = "Invalid JSON: " + path.string();
        return false;
    }
    if (value_out) *value_out = std::move(value);
    if (bytes_out) *bytes_out = std::move(bytes);
    return true;
}

bool path_is_inside(const std::filesystem::path& child,
                    const std::filesystem::path& parent)
{
    std::error_code error;
    const auto canonical_child = std::filesystem::weakly_canonical(child, error);
    if (error) return false;
    const auto canonical_parent = std::filesystem::weakly_canonical(parent, error);
    if (error) return false;
    auto child_it = canonical_child.begin();
    for (auto parent_it = canonical_parent.begin();
         parent_it != canonical_parent.end(); ++parent_it, ++child_it) {
        if (child_it == canonical_child.end() || *child_it != *parent_it) {
            return false;
        }
    }
    return true;
}

bool sha256_matches(const std::string& bytes, const std::string& expected)
{
    const std::string digest = checksum::sha256_hex(bytes);
    return expected == digest || expected == "sha256:" + digest;
}

std::string expected_homography_configuration_fingerprint(
    const CitrusSpatialTemplateState& template_state,
    const nlohmann::json& camera_calibration_json,
    const std::string& target_plane)
{
    const nlohmann::json identity = {
        {"schema_id", "citrus.homography.configuration_identity"},
        {"schema_version", 1},
        {"rig_id", template_state.source_rig_name},
        {"canvas_name", template_state.source_canvas_name},
        {"arena_id", template_state.source_config_name},
        {"camera_id", template_state.source_camera_id},
        {"camera_native_size_px", {
            {"width", camera_calibration_json.value("native_width_px", 0)},
            {"height", camera_calibration_json.value("native_height_px", 0)}}},
        {"arena_center_final_display_canvas_px", {
            camera_calibration_json.value("arena_center_x_px", 0),
            camera_calibration_json.value("arena_center_y_px", 0)}},
        {"arena_size_canvas_px", {
            camera_calibration_json.value("arena_width_px", 0),
            camera_calibration_json.value("arena_height_px", 0)}},
        {"target_plane", target_plane},
        {"homography_direction",
         "camera_native_px_to_final_display_canvas_px"},
    };
    return "sha256:" + checksum::sha256_hex(identity.dump());
}

bool load_homography_matrix_from_citrus_sidecar(const std::filesystem::path& config_path,
                                                const std::string& config_name,
                                                const std::string& camera_id,
                                                const nlohmann::json& camera_calibration_json,
                                                CitrusSpatialTemplateState* template_state,
                                                std::string* error_out)
{
    if (template_state == nullptr) {
        if (error_out) {
            *error_out = "Null Citrus template state.";
        }
        return false;
    }
    if (config_name.empty() || camera_id.empty()) {
        return false;
    }

    const std::filesystem::path artifact_root =
        config_path.parent_path() / "calibration_artifacts";
    const std::filesystem::path legacy_homography_path = artifact_root /
        ("homography_" + safe_path_component(config_name) + "_" +
         safe_path_component(camera_id) + ".yml");
    const std::filesystem::path active_pointer_path = artifact_root /
        ("homography_active_" + safe_path_component(config_name) + "_" +
         safe_path_component(camera_id) + ".json");
    template_state->homography_active_pointer_path =
        active_pointer_path.string();
    template_state->homography_canvas_compatibility_basis.clear();
    template_state->homography_canvas_compatibility_warning.clear();
    template_state->homography_canvas_geometry_fingerprint.clear();
    template_state->homography_commissioning_release_id.clear();
    if (!std::filesystem::is_regular_file(active_pointer_path)) {
        template_state->homography_authority_status =
            std::filesystem::is_regular_file(legacy_homography_path)
                ? "legacy_unverified" : "missing";
        template_state->homography_import_error =
            std::filesystem::is_regular_file(legacy_homography_path)
                ? "Legacy homography exists, but no accepted active artifact pointer is present."
                : "No accepted active homography artifact is present.";
        if (error_out) *error_out = template_state->homography_import_error;
        return false;
    }

    try {
        nlohmann::json pointer;
        std::string error;
        if (!read_json_file(
                active_pointer_path, &pointer, nullptr, &error)) {
            template_state->homography_authority_status = "invalid_active_pointer";
            template_state->homography_import_error = error;
            if (error_out) *error_out = error;
            return false;
        }
        const std::string target_plane = pointer.value("target_plane", "");
        const std::string direction = pointer.value("homography_direction", "");
        const std::string expected_fingerprint =
            expected_homography_configuration_fingerprint(
                *template_state, camera_calibration_json, target_plane);
        std::string canvas_checksum;
        if (!checksum::file_sha256(config_path, &canvas_checksum, &error)) {
            template_state->homography_authority_status = "canvas_unreadable";
            template_state->homography_import_error = error;
            if (error_out) *error_out = error;
            return false;
        }
        const auto canvas_compatibility_result =
            canvas_compatibility::validate_active_artifact_canvas(
                template_state->source_rig_name,
                template_state->source_canvas_name,
                config_name,
                camera_id,
                "homography",
                config_path,
                pointer.value("canvas_checksum_at_acceptance", ""),
                active_pointer_path,
                artifact_root / "commissioning_active.json");
        template_state->homography_canvas_compatibility_basis =
            canvas_compatibility_result.basis;
        template_state->homography_canvas_compatibility_warning =
            canvas_compatibility_result.warning;
        template_state->homography_canvas_geometry_fingerprint =
            canvas_compatibility_result.current_geometry_fingerprint;
        template_state->homography_commissioning_release_id =
            canvas_compatibility_result.commissioning_release_id;
        if (pointer.value("schema_id", "") !=
                "citrus.calibration.active_homography" ||
            pointer.value("schema_version", 0) != 1 ||
            pointer.value("status", "") != "accepted" ||
            pointer.value("rig_id", "") != template_state->source_rig_name ||
            pointer.value("canvas_name", "") != template_state->source_canvas_name ||
            pointer.value("arena_id", "") != config_name ||
            pointer.value("camera_id", "") != camera_id ||
            target_plane != "projected_surface" ||
            direction != "camera_native_px_to_final_display_canvas_px" ||
            pointer.value("configuration_fingerprint", "") !=
                expected_fingerprint ||
            !canvas_compatibility_result.compatible ||
            pointer.value("verification", nlohmann::json::object()).value(
                "status", "") != "passed" ||
            pointer.value("quality", nlohmann::json::object()).value(
                "status", "") != "passed" ||
            pointer.value("orientation_validation", nlohmann::json::object()).value(
                "status", "") != "passed" ||
            pointer.value("source_photometry", nlohmann::json::object()).value(
                "status", "") != "passed") {
            template_state->homography_authority_status = "stale_or_incompatible";
            template_state->homography_import_error =
                "Accepted homography identity, coordinate semantics, canvas geometry, "
                "or configuration fingerprint does not match the current canvas.";
            if (!canvas_compatibility_result.error.empty()) {
                template_state->homography_import_error +=
                    " Canvas compatibility: " + canvas_compatibility_result.error + ".";
            }
            if (error_out) *error_out = template_state->homography_import_error;
            return false;
        }

        const std::filesystem::path candidate_set_dir =
            pointer.value("candidate_set_dir", "");
        const std::filesystem::path candidate_json_path =
            pointer.value("candidate_json_path", "");
        const std::filesystem::path homography_path =
            pointer.value("homography_yaml_path", "");
        if (candidate_set_dir.empty() || candidate_json_path.empty() ||
            homography_path.empty() ||
            !path_is_inside(candidate_set_dir,
                            artifact_root / "homography_candidates") ||
            !path_is_inside(candidate_json_path, candidate_set_dir) ||
            !path_is_inside(homography_path, candidate_set_dir) ||
            !std::filesystem::is_regular_file(candidate_json_path) ||
            !std::filesystem::is_regular_file(homography_path)) {
            template_state->homography_authority_status = "invalid_artifact_paths";
            template_state->homography_import_error =
                "Accepted homography paths are absent or outside the canvas artifact root.";
            if (error_out) *error_out = template_state->homography_import_error;
            return false;
        }

        nlohmann::json candidate;
        std::string candidate_bytes;
        std::string yaml_bytes;
        if (!read_json_file(
                candidate_json_path, &candidate, &candidate_bytes, &error) ||
            !sha256_matches(candidate_bytes,
                            pointer.value("candidate_json_checksum", "")) ||
            !checksum::read_file(homography_path, &yaml_bytes, &error) ||
            !sha256_matches(yaml_bytes,
                            pointer.value("homography_yaml_checksum", ""))) {
            template_state->homography_authority_status = "artifact_checksum_failed";
            template_state->homography_import_error = error.empty()
                ? "Accepted candidate or homography checksum did not match."
                : error;
            if (error_out) *error_out = template_state->homography_import_error;
            return false;
        }
        const auto quality = candidate.value("quality", nlohmann::json::object());
        if (candidate.value("schema_id", "") !=
                "citrus.calibration.homography_candidate" ||
            candidate.value("schema_version", 0) != 1 ||
            candidate.value("status", "") != "ready_for_review" ||
            candidate.value("mutation_allowed", true) ||
            candidate.value("candidate_id", "") !=
                pointer.value("candidate_id", "") ||
            candidate.value("candidate_set_id", "") !=
                pointer.value("candidate_set_id", "") ||
            candidate.value("arena_id", "") != config_name ||
            candidate.value("camera_id", "") != camera_id ||
            candidate.value("target_plane", "") != target_plane ||
            candidate.value("homography_direction", "") != direction ||
            candidate.value("configuration_fingerprint", "") !=
                expected_fingerprint ||
            quality.value("status", "") != "passed" ||
            !quality.value("full_fit", nlohmann::json::object()).value(
                "passed", false) ||
            !quality.value("holdout", nlohmann::json::object()).value(
                "passed", false) ||
            candidate.value("orientation_validation", nlohmann::json::object()).value(
                "status", "") != "passed" ||
            candidate.value("source_photometry", nlohmann::json::object()).value(
                "status", "") != "passed") {
            template_state->homography_authority_status = "candidate_gate_failed";
            template_state->homography_import_error =
                "Accepted candidate provenance or quality gates are invalid.";
            if (error_out) *error_out = template_state->homography_import_error;
            return false;
        }

        cv::FileStorage fs(homography_path.string(), cv::FileStorage::READ);
        if (!fs.isOpened()) {
            template_state->homography_authority_status = "load_failed";
            template_state->homography_import_error =
                "Found accepted Citrus homography YAML but could not open it: " +
                homography_path.string();
            if (error_out) {
                *error_out = template_state->homography_import_error;
            }
            return false;
        }
        cv::Mat homography;
        std::string yaml_candidate_id;
        std::string yaml_candidate_set_id;
        std::string yaml_direction;
        std::string yaml_target_plane;
        std::string yaml_fingerprint;
        fs["candidate_id"] >> yaml_candidate_id;
        fs["candidate_set_id"] >> yaml_candidate_set_id;
        fs["homography_direction"] >> yaml_direction;
        fs["target_plane"] >> yaml_target_plane;
        fs["configuration_fingerprint"] >> yaml_fingerprint;
        fs["homography_matrix"] >> homography;
        fs.release();
        if (homography.empty() || homography.rows != 3 || homography.cols != 3 ||
            yaml_candidate_id != pointer.value("candidate_id", "") ||
            yaml_candidate_set_id != pointer.value("candidate_set_id", "") ||
            yaml_direction != direction || yaml_target_plane != target_plane ||
            yaml_fingerprint != expected_fingerprint) {
            template_state->homography_authority_status = "load_failed";
            template_state->homography_import_error =
                "Accepted Citrus homography YAML did not contain the expected "
                "3x3 matrix and artifact identity: " + homography_path.string();
            if (error_out) {
                *error_out = template_state->homography_import_error;
            }
            return false;
        }

        homography.convertTo(homography, CV_64F);
        for (int row = 0; row < 3; ++row) {
            for (int col = 0; col < 3; ++col) {
                template_state->camera_to_canvas_homography[static_cast<size_t>(row * 3 + col)] =
                    homography.at<double>(row, col);
            }
        }
        template_state->has_camera_to_canvas_homography = true;
        template_state->has_authoritative_camera_to_canvas_homography = true;
        template_state->homography_authority_status = "accepted_compatible";
        template_state->homography_import_error.clear();
        template_state->homography_candidate_set_id =
            pointer.value("candidate_set_id", "");
        template_state->homography_candidate_id =
            pointer.value("candidate_id", "");
        template_state->homography_candidate_json_path = candidate_json_path.string();
        template_state->homography_yaml_path = homography_path.string();
        template_state->homography_target_plane = target_plane;
        template_state->homography_direction = direction;
        template_state->homography_configuration_fingerprint =
            expected_fingerprint;
        template_state->homography_canvas_checksum = canvas_checksum;
        template_state->homography_accepted_at_utc =
            pointer.value("accepted_at_utc", "");
        const auto source = pointer.value("source", nlohmann::json::object());
        template_state->homography_source_image_path =
            source.value("image_path", "");
        template_state->homography_source_image_checksum =
            source.value("checksum", "");
        const auto photometry = pointer.value(
            "source_photometry", nlohmann::json::object());
        const auto commissioning = photometry.value(
            "commissioning_provenance", nlohmann::json::object());
        template_state->homography_projector_intensity_report_path =
            commissioning.value("report_path", "");
        template_state->homography_projector_intensity_report_sha256 =
            commissioning.value("report_sha256", "");
        const auto full_fit = quality.value("full_fit", nlohmann::json::object());
        const auto holdout = quality.value("holdout", nlohmann::json::object());
        template_state->has_homography_quality = true;
        template_state->homography_rms_reprojection_error_canvas_px =
            full_fit.value("rms_reprojection_error_canvas_px", 0.0);
        template_state->homography_maximum_reprojection_error_canvas_px =
            full_fit.value("maximum_reprojection_error_canvas_px", 0.0);
        template_state->homography_holdout_rms_error_canvas_px =
            holdout.value("rms_error_canvas_px", 0.0);
        template_state->homography_holdout_maximum_error_canvas_px =
            holdout.value("maximum_error_canvas_px", 0.0);

        const double det = cv::determinant(homography);
        if (std::isfinite(det) && std::abs(det) > 1e-12) {
            const cv::Mat inverse = homography.inv();
            for (int row = 0; row < 3; ++row) {
                for (int col = 0; col < 3; ++col) {
                    template_state->canvas_to_camera_homography[static_cast<size_t>(row * 3 + col)] =
                        inverse.at<double>(row, col);
                }
            }
            template_state->has_canvas_to_camera_homography = true;
        }
    } catch (const cv::Exception& ex) {
        template_state->homography_authority_status = "load_failed";
        template_state->homography_import_error = ex.what();
        if (error_out) {
            *error_out = std::string("Failed to load Citrus homography sidecar: ") + ex.what();
        }
        return false;
    } catch (const std::exception& ex) {
        template_state->homography_authority_status = "load_failed";
        template_state->homography_import_error = ex.what();
        if (error_out) {
            *error_out = std::string(
                "Failed to validate Citrus active homography artifact: ") +
                ex.what();
        }
        return false;
    }

    return true;
}

bool build_citrus_single_circle_template_state(
    const std::filesystem::path& config_path,
    const nlohmann::json& arena_json,
    const std::string& arena_name,
    const nlohmann::json& camera_calibration_json,
    CitrusSpatialTemplateState* template_state_out,
    std::string* error_out)
{
    if (template_state_out == nullptr) {
        if (error_out) {
            *error_out = "Null Citrus template destination.";
        }
        return false;
    }
    if (!arena_json.is_object() || !camera_calibration_json.is_object()) {
        if (error_out) {
            *error_out = "Citrus arena/camera calibration entries must be JSON objects.";
        }
        return false;
    }
    if (!json_string_equals_ignore_case(arena_json, "experimental_area_shape", "CIRCLE")) {
        if (error_out) {
            *error_out = "Citrus import v1 currently supports only experimental_area_shape = CIRCLE.";
        }
        return false;
    }

    CitrusSpatialTemplateState template_state;
    template_state.available = true;
    template_state.source_config_path = config_path.string();
    template_state.source_canvas_name = config_path.parent_path().filename().string();
    template_state.source_rig_name = config_path.parent_path().parent_path().filename().string();
    template_state.source_arena_name = arena_name;
    template_state.source_config_name = arena_json.value("config_name", arena_name);
    template_state.source_camera_id = camera_calibration_json.value("camera_id", "");
    template_state.source_dish_type_name = arena_json.value("selected_dish_type_name", "");
    if (arena_json.contains("dish_config") &&
        arena_json["dish_config"].is_object() &&
        arena_json["dish_config"].contains("dimensions") &&
        arena_json["dish_config"]["dimensions"].is_object()) {
        const nlohmann::json& dimensions =
            arena_json["dish_config"]["dimensions"];
        constexpr const char* kInnerDiameterFields[] = {
            "inner_diameter_mm",
            "usable_area_diameter_mm",
            "diameter_mm"
        };
        for (const char* field : kInnerDiameterFields) {
            if (dimensions.contains(field) && dimensions[field].is_number()) {
                const double value = dimensions[field].get<double>();
                if (value > 0.0) {
                    template_state.has_inner_diameter_mm = true;
                    template_state.inner_diameter_mm = value;
                    template_state.inner_diameter_source_field =
                        std::string("dish_config.dimensions.") + field;
                    break;
                }
            }
        }
    }
    const bool has_arena_center =
        parse_optional_json_number(camera_calibration_json, "arena_center_x_px", &template_state.arena_center_x_px) &&
        parse_optional_json_number(camera_calibration_json, "arena_center_y_px", &template_state.arena_center_y_px);
    const bool has_arena_size =
        parse_optional_json_number(camera_calibration_json, "arena_width_px", &template_state.arena_width_px) &&
        parse_optional_json_number(camera_calibration_json, "arena_height_px", &template_state.arena_height_px);
    template_state.has_arena_canvas_region =
        has_arena_center &&
        has_arena_size &&
        template_state.arena_width_px > 0.0 &&
        template_state.arena_height_px > 0.0;

    if (!parse_required_json_number(arena_json, "experimental_area_center_x_px",
                                    &template_state.experimental_area_center_x_px, error_out) ||
        !parse_required_json_number(arena_json, "experimental_area_center_y_px",
                                    &template_state.experimental_area_center_y_px, error_out) ||
        !parse_required_json_number(arena_json, "experimental_area_radius_px",
                                    &template_state.experimental_area_radius_px, error_out)) {
        return false;
    }
    if (arena_json.contains("experimental_area_radius_mm") &&
        arena_json.at("experimental_area_radius_mm").is_number()) {
        template_state.has_radius_mm = true;
        template_state.experimental_area_radius_mm =
            arena_json.at("experimental_area_radius_mm").get<double>();
    }
    if (camera_calibration_json.contains("pixels_per_mm_projector") &&
        camera_calibration_json.at("pixels_per_mm_projector").is_number()) {
        template_state.has_pixels_per_mm_projector = true;
        template_state.pixels_per_mm_projector =
            camera_calibration_json.at("pixels_per_mm_projector").get<double>();
    }
    template_state.calibration_pattern_mode =
        arena_json.value("calibration_pattern_mode", "");
    template_state.calibration_pattern_mask_policy =
        arena_json.value("calibration_pattern_mask_policy", "");
    if (arena_json.contains("calibration_ring_outer_radius_px") &&
        arena_json["calibration_ring_outer_radius_px"].is_number()) {
        const double ring_outer_radius_px =
            arena_json["calibration_ring_outer_radius_px"].get<double>();
        if (ring_outer_radius_px > 0.0) {
            template_state.has_calibration_ring_outer_radius_px = true;
            template_state.calibration_ring_outer_radius_px =
                ring_outer_radius_px;
        }
    }
    if (camera_calibration_json.contains("pixels_per_mm_camera") &&
        camera_calibration_json.at("pixels_per_mm_camera").is_number()) {
        const double pixels_per_mm_camera =
            camera_calibration_json.at("pixels_per_mm_camera").get<double>();
        if (pixels_per_mm_camera > 0.0) {
            template_state.has_pixels_per_mm_camera = true;
            template_state.pixels_per_mm_camera = pixels_per_mm_camera;
            template_state.pixels_per_mm_camera_target_plane =
                "unknown_legacy_camera_scale_plane";
            if (camera_calibration_json.contains("scale_models") &&
                camera_calibration_json["scale_models"].is_array()) {
                for (const nlohmann::json& scale_model :
                     camera_calibration_json["scale_models"]) {
                    if (!scale_model.is_object() ||
                        !scale_model.contains("pixels_per_mm_camera") ||
                        !scale_model["pixels_per_mm_camera"].is_number()) {
                        continue;
                    }
                    const double model_pixels_per_mm =
                        scale_model["pixels_per_mm_camera"].get<double>();
                    if (std::abs(model_pixels_per_mm - pixels_per_mm_camera) >
                        1e-6 * std::max(1.0, pixels_per_mm_camera)) {
                        continue;
                    }
                    template_state.pixels_per_mm_camera_target_plane =
                        scale_model.value(
                            "target_plane",
                            "unknown_legacy_camera_scale_plane");
                    break;
                }
            }
        }
    }

    std::string homography_error;
    load_homography_matrix_from_citrus_sidecar(
        config_path,
        template_state.source_config_name,
        template_state.source_camera_id,
        camera_calibration_json,
        &template_state,
        &homography_error);

    *template_state_out = std::move(template_state);
    return true;
}

} // namespace

std::string default_citrus_rigs_root()
{
    const std::filesystem::path citrus_rigs_root("/home/jeremy/citrus/targets/rigs");
    if (std::filesystem::exists(citrus_rigs_root)) {
        return citrus_rigs_root.string();
    }
    return ".";
}

std::string citrus_template_display_label(const CitrusSpatialTemplateState& template_state)
{
    std::ostringstream label;
    label << (template_state.source_arena_name.empty()
                  ? std::string("arena_unknown")
                  : template_state.source_arena_name);
    if (!template_state.source_camera_id.empty()) {
        label << " / Cam" << template_state.source_camera_id;
    }
    if (!template_state.source_config_name.empty() &&
        template_state.source_config_name != template_state.source_arena_name) {
        label << " / " << template_state.source_config_name;
    }
    if (template_state.projection_geometry_authority_mode ==
            "inherit_active_commissioning" &&
        !template_state.projection_geometry_authority_canvas_name.empty()) {
        label << " / projection geometry from "
              << template_state.projection_geometry_authority_canvas_name;
    }
    return label.str();
}

bool collect_citrus_single_circle_templates(
    const std::filesystem::path& config_path,
    const nlohmann::json& root,
    std::vector<CitrusSpatialTemplateState>* templates_out,
    std::vector<std::string>* available_camera_ids_out,
    std::string* error_out)
{
    if (templates_out == nullptr) {
        if (error_out) {
            *error_out = "Null Citrus template list destination.";
        }
        return false;
    }
    templates_out->clear();
    if (available_camera_ids_out != nullptr) {
        available_camera_ids_out->clear();
    }

    std::string projection_authority_mode = "local_canvas";
    std::string projection_authority_canvas_name =
        config_path.parent_path().filename().string();
    std::filesystem::path projection_authority_config_path = config_path;
    nlohmann::json projection_authority_root = root;
    const auto authority_it = root.find("projection_geometry_authority");
    if (authority_it != root.end()) {
        if (!authority_it->is_object() ||
            authority_it->value("schema_version", 0) != 1 ||
            authority_it->value("mode", std::string()) !=
                "inherit_active_commissioning" ||
            authority_it->value("geometry_scope", std::string()) !=
                "arena_placement_homography_and_projected_surface_scale" ||
            authority_it->value("experimental_area_owner", std::string()) !=
                "selected_canvas") {
            if (error_out) {
                *error_out =
                    "Unsupported Citrus projection_geometry_authority contract.";
            }
            return false;
        }
        projection_authority_mode = "inherit_active_commissioning";
        projection_authority_canvas_name =
            authority_it->value("source_canvas_name", std::string());
        if (projection_authority_canvas_name.empty() ||
            projection_authority_canvas_name ==
                config_path.parent_path().filename().string()) {
            if (error_out) {
                *error_out =
                    "Inherited Citrus projection authority identity is invalid.";
            }
            return false;
        }
        projection_authority_config_path =
            config_path.parent_path().parent_path() /
            projection_authority_canvas_name /
            (projection_authority_canvas_name + ".json");
        if (!read_json_file(
                projection_authority_config_path,
                &projection_authority_root, nullptr, error_out)) {
            return false;
        }
        if (projection_authority_root.value("canvas_name", std::string()) !=
                projection_authority_canvas_name ||
            projection_authority_root.contains("projection_geometry_authority") ||
            root.value("canvas_width_px", 0) !=
                projection_authority_root.value("canvas_width_px", -1) ||
            root.value("canvas_height_px", 0) !=
                projection_authority_root.value("canvas_height_px", -1)) {
            if (error_out) {
                *error_out =
                    "Inherited Citrus projection authority canvas is incompatible.";
            }
            return false;
        }
    }

    auto find_authority_arena_and_camera = [&]
        (const std::string& config_name,
         const std::string& camera_id,
         const nlohmann::json** arena_out,
         const nlohmann::json** camera_out,
         std::string* arena_name_out) -> bool {
        if (arena_out == nullptr || camera_out == nullptr ||
            arena_name_out == nullptr ||
            !projection_authority_root.contains("arenas") ||
            !projection_authority_root.at("arenas").is_object()) {
            return false;
        }
        *arena_out = nullptr;
        *camera_out = nullptr;
        for (auto it = projection_authority_root.at("arenas").begin();
             it != projection_authority_root.at("arenas").end(); ++it) {
            if (!it.value().is_object() ||
                it.value().value("config_name", it.key()) != config_name ||
                !it.value().contains("camera_calibrations") ||
                !it.value().at("camera_calibrations").is_array()) {
                continue;
            }
            for (const auto& camera :
                 it.value().at("camera_calibrations")) {
                if (camera.is_object() &&
                    camera.value("camera_id", std::string()) == camera_id) {
                    if (*arena_out != nullptr) return false;
                    *arena_out = &it.value();
                    *camera_out = &camera;
                    *arena_name_out = it.key();
                }
            }
        }
        return *arena_out != nullptr && *camera_out != nullptr;
    };

    const auto copy_projection_authority = [](
        const CitrusSpatialTemplateState& authority,
        CitrusSpatialTemplateState* selected) {
        selected->has_arena_canvas_region = authority.has_arena_canvas_region;
        selected->arena_center_x_px = authority.arena_center_x_px;
        selected->arena_center_y_px = authority.arena_center_y_px;
        selected->arena_width_px = authority.arena_width_px;
        selected->arena_height_px = authority.arena_height_px;
        selected->has_pixels_per_mm_camera = authority.has_pixels_per_mm_camera;
        selected->pixels_per_mm_camera = authority.pixels_per_mm_camera;
        selected->pixels_per_mm_camera_target_plane =
            authority.pixels_per_mm_camera_target_plane;
        selected->has_pixels_per_mm_projector =
            authority.has_pixels_per_mm_projector;
        selected->pixels_per_mm_projector = authority.pixels_per_mm_projector;
        selected->has_camera_to_canvas_homography =
            authority.has_camera_to_canvas_homography;
        selected->has_authoritative_camera_to_canvas_homography =
            authority.has_authoritative_camera_to_canvas_homography;
        selected->homography_authority_status =
            authority.homography_authority_status;
        selected->homography_import_error = authority.homography_import_error;
        selected->homography_active_pointer_path =
            authority.homography_active_pointer_path;
        selected->homography_candidate_set_id =
            authority.homography_candidate_set_id;
        selected->homography_candidate_id = authority.homography_candidate_id;
        selected->homography_candidate_json_path =
            authority.homography_candidate_json_path;
        selected->homography_yaml_path = authority.homography_yaml_path;
        selected->homography_target_plane = authority.homography_target_plane;
        selected->homography_direction = authority.homography_direction;
        selected->homography_configuration_fingerprint =
            authority.homography_configuration_fingerprint;
        selected->homography_canvas_checksum =
            authority.homography_canvas_checksum;
        selected->homography_canvas_compatibility_basis =
            authority.homography_canvas_compatibility_basis;
        selected->homography_canvas_compatibility_warning =
            authority.homography_canvas_compatibility_warning;
        selected->homography_canvas_geometry_fingerprint =
            authority.homography_canvas_geometry_fingerprint;
        selected->homography_commissioning_release_id =
            authority.homography_commissioning_release_id;
        selected->homography_accepted_at_utc =
            authority.homography_accepted_at_utc;
        selected->homography_source_image_path =
            authority.homography_source_image_path;
        selected->homography_source_image_checksum =
            authority.homography_source_image_checksum;
        selected->homography_projector_intensity_report_path =
            authority.homography_projector_intensity_report_path;
        selected->homography_projector_intensity_report_sha256 =
            authority.homography_projector_intensity_report_sha256;
        selected->has_homography_quality = authority.has_homography_quality;
        selected->homography_rms_reprojection_error_canvas_px =
            authority.homography_rms_reprojection_error_canvas_px;
        selected->homography_maximum_reprojection_error_canvas_px =
            authority.homography_maximum_reprojection_error_canvas_px;
        selected->homography_holdout_rms_error_canvas_px =
            authority.homography_holdout_rms_error_canvas_px;
        selected->homography_holdout_maximum_error_canvas_px =
            authority.homography_holdout_maximum_error_canvas_px;
        selected->camera_to_canvas_homography =
            authority.camera_to_canvas_homography;
        selected->has_canvas_to_camera_homography =
            authority.has_canvas_to_camera_homography;
        selected->canvas_to_camera_homography =
            authority.canvas_to_camera_homography;
    };

    auto append_arena_templates =
        [&](const nlohmann::json& arena_json, const std::string& arena_name) -> bool {
            if (!arena_json.is_object() ||
                !arena_json.contains("camera_calibrations") ||
                !arena_json.at("camera_calibrations").is_array()) {
                return true;
            }
            for (const auto& camera_json : arena_json.at("camera_calibrations")) {
                if (!camera_json.is_object()) {
                    continue;
                }
                const std::string camera_id = camera_json.value("camera_id", "");
                if (!camera_id.empty() && available_camera_ids_out != nullptr) {
                    available_camera_ids_out->push_back(camera_id);
                }
                if (!json_string_equals_ignore_case(arena_json, "experimental_area_shape", "CIRCLE")) {
                    continue;
                }
                CitrusSpatialTemplateState template_state;
                std::string template_error;
                if (!build_citrus_single_circle_template_state(
                        config_path,
                        arena_json,
                        arena_name,
                        camera_json,
                        &template_state,
                        &template_error)) {
                    if (error_out) {
                        std::ostringstream oss;
                        oss << "Failed to import Citrus arena " << arena_name;
                        if (!camera_id.empty()) {
                            oss << " camera " << camera_id;
                        }
                        if (!template_error.empty()) {
                            oss << ": " << template_error;
                        }
                        *error_out = oss.str();
                    }
                    return false;
                }
                template_state.projection_geometry_authority_mode =
                    projection_authority_mode;
                template_state.projection_geometry_authority_canvas_name =
                    projection_authority_canvas_name;
                template_state.projection_geometry_authority_config_path =
                    projection_authority_config_path.string();
                if (projection_authority_mode ==
                    "inherit_active_commissioning") {
                    const nlohmann::json* authority_arena = nullptr;
                    const nlohmann::json* authority_camera = nullptr;
                    std::string authority_arena_name;
                    if (!find_authority_arena_and_camera(
                            template_state.source_config_name,
                            template_state.source_camera_id,
                            &authority_arena, &authority_camera,
                            &authority_arena_name)) {
                        if (error_out) {
                            *error_out =
                                "Projection authority is missing arena/camera " +
                                template_state.source_config_name + "/" +
                                template_state.source_camera_id + ".";
                        }
                        return false;
                    }
                    CitrusSpatialTemplateState authority_template;
                    if (!build_citrus_single_circle_template_state(
                            projection_authority_config_path,
                            *authority_arena,
                            authority_arena_name,
                            *authority_camera,
                            &authority_template,
                            &template_error) ||
                        !authority_template
                             .has_authoritative_camera_to_canvas_homography ||
                        !authority_template.has_canvas_to_camera_homography) {
                        if (error_out) {
                            *error_out =
                                "Could not load the inherited authoritative "
                                "Citrus homography for " +
                                template_state.source_config_name + "/" +
                                template_state.source_camera_id +
                                (template_error.empty()
                                    ? std::string(".")
                                    : ": " + template_error);
                        }
                        return false;
                    }
                    copy_projection_authority(
                        authority_template, &template_state);
                    if (template_state.has_radius_mm &&
                        template_state.has_pixels_per_mm_projector) {
                        template_state.experimental_area_radius_px =
                            template_state.experimental_area_radius_mm *
                            template_state.pixels_per_mm_projector;
                    }
                }
                templates_out->push_back(std::move(template_state));
            }
            return true;
        };

    if (root.contains("arenas") && root.at("arenas").is_object()) {
        for (auto it = root.at("arenas").begin(); it != root.at("arenas").end(); ++it) {
            if (!append_arena_templates(it.value(), it.key())) {
                return false;
            }
        }
    } else {
        const std::string fallback_arena_name = root.value("config_name", config_path.stem().string());
        if (!append_arena_templates(root, fallback_arena_name)) {
            return false;
        }
    }

    if (available_camera_ids_out != nullptr) {
        std::sort(available_camera_ids_out->begin(), available_camera_ids_out->end());
        available_camera_ids_out->erase(
            std::unique(available_camera_ids_out->begin(), available_camera_ids_out->end()),
            available_camera_ids_out->end());
    }

    if (templates_out->empty()) {
        if (error_out) {
            std::ostringstream oss;
            oss << "No supported Citrus single-circle arena templates found in "
                << config_path.string();
            if (available_camera_ids_out != nullptr && !available_camera_ids_out->empty()) {
                oss << ". Available camera_ids: "
                    << join_strings(*available_camera_ids_out, ", ");
            }
            *error_out = oss.str();
        }
        return false;
    }
    return true;
}

int find_citrus_template_index_for_camera(
    const SpatialLayoutUiState& ui_state,
    const std::string& camera_serial)
{
    if (camera_serial.empty()) {
        return -1;
    }
    for (size_t idx = 0; idx < ui_state.citrus_canvas_templates.size(); ++idx) {
        if (ui_state.citrus_canvas_templates[idx].source_camera_id == camera_serial) {
            return static_cast<int>(idx);
        }
    }
    return -1;
}

} // namespace orange::gui::spatial_layout
