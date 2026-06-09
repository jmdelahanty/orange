#include "gui/spatial_layout/citrus_import.h"

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

bool load_homography_matrix_from_citrus_sidecar(const std::filesystem::path& config_path,
                                                const std::string& config_name,
                                                const std::string& camera_id,
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

    std::string safe_camera_id = camera_id;
    std::replace(safe_camera_id.begin(), safe_camera_id.end(), ' ', '_');
    const std::filesystem::path homography_path =
        config_path.parent_path() / "calibration_artifacts" /
        ("homography_" + config_name + "_" + safe_camera_id + ".yml");
    if (!std::filesystem::exists(homography_path)) {
        return false;
    }

    try {
        cv::FileStorage fs(homography_path.string(), cv::FileStorage::READ);
        if (!fs.isOpened()) {
            if (error_out) {
                *error_out = "Found Citrus homography sidecar but could not open it: " + homography_path.string();
            }
            return false;
        }
        cv::Mat homography;
        fs["homography_matrix"] >> homography;
        fs.release();
        if (homography.empty() || homography.rows != 3 || homography.cols != 3) {
            if (error_out) {
                *error_out = "Citrus homography sidecar did not contain a valid 3x3 matrix: " + homography_path.string();
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
        if (error_out) {
            *error_out = std::string("Failed to load Citrus homography sidecar: ") + ex.what();
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

    std::string homography_error;
    load_homography_matrix_from_citrus_sidecar(
        config_path,
        template_state.source_config_name,
        template_state.source_camera_id,
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
