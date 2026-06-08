#ifndef ORANGE_CAMERA_CONFIG_SCHEMA_H
#define ORANGE_CAMERA_CONFIG_SCHEMA_H

#include "camera.h"
#include "json.hpp"
#include <limits>
#include <string>
#include <utility>

namespace orange::camera_config {

namespace detail {

inline bool try_get_nonnegative_int(const nlohmann::json& object,
                                    const char* key,
                                    int* out_value)
{
    if (!out_value || !object.contains(key)) {
        return false;
    }

    const nlohmann::json& value = object[key];
    if (!value.is_number_integer() && !value.is_number_unsigned()) {
        return false;
    }

    const long long parsed = value.get<long long>();
    if (parsed < 0) {
        return false;
    }

    *out_value = static_cast<int>(parsed);
    return true;
}

inline bool try_get_int(const nlohmann::json& object,
                        const char* key,
                        int* out_value)
{
    if (!out_value || !object.contains(key)) {
        return false;
    }

    const nlohmann::json& value = object[key];
    if (!value.is_number_integer() && !value.is_number_unsigned()) {
        return false;
    }

    const long long parsed = value.get<long long>();
    if (parsed > static_cast<long long>(std::numeric_limits<int>::max())) {
        *out_value = std::numeric_limits<int>::max();
    } else if (parsed < static_cast<long long>(std::numeric_limits<int>::min())) {
        *out_value = std::numeric_limits<int>::min();
    } else {
        *out_value = static_cast<int>(parsed);
    }
    return true;
}

inline bool try_get_string(const nlohmann::json& object,
                           const char* key,
                           std::string* out_value)
{
    if (!out_value || !object.contains(key) || !object[key].is_string()) {
        return false;
    }
    *out_value = object[key].get<std::string>();
    return true;
}

inline bool try_get_positive_double(const nlohmann::json& object,
                                    const char* key,
                                    double* out_value)
{
    if (!out_value || !object.contains(key) || !object[key].is_number()) {
        return false;
    }
    const double parsed = object[key].get<double>();
    if (parsed <= 0.0) {
        return false;
    }
    *out_value = parsed;
    return true;
}

}  // namespace detail

inline void parse_crop_pipeline_config(const nlohmann::json& camera_config,
                                       CameraParams* camera_params)
{
    if (!camera_params) {
        return;
    }

    camera_params->crop_pipeline = CameraCropPipelineConfig();
    if (!camera_config.contains("crop_pipeline") ||
        !camera_config["crop_pipeline"].is_object()) {
        return;
    }

    const nlohmann::json& crop_pipeline = camera_config["crop_pipeline"];
    int crop_size_px = camera_params->crop_pipeline.crop_size_px;
    if (!detail::try_get_nonnegative_int(crop_pipeline, "crop_size_px", &crop_size_px) &&
        !detail::try_get_nonnegative_int(crop_pipeline, "size_px", &crop_size_px)) {
        int crop_width = 0;
        int crop_height = 0;
        const bool has_width = detail::try_get_nonnegative_int(crop_pipeline, "width", &crop_width);
        const bool has_height = detail::try_get_nonnegative_int(crop_pipeline, "height", &crop_height);
        if (has_width && has_height && crop_width == crop_height) {
            crop_size_px = crop_width;
        } else if (has_width && !has_height) {
            crop_size_px = crop_width;
        } else if (has_height && !has_width) {
            crop_size_px = crop_height;
        }
    }

    camera_params->crop_pipeline.crop_size_px =
        sanitize_camera_crop_size_px(crop_size_px);

    int preview_max_fps = camera_params->crop_pipeline.preview_max_fps;
    if (detail::try_get_int(crop_pipeline, "preview_max_fps", &preview_max_fps)) {
        camera_params->crop_pipeline.preview_max_fps =
            sanitize_camera_crop_preview_max_fps(preview_max_fps);
    }
}

inline nlohmann::json build_crop_pipeline_config(const CameraParams& camera_params)
{
    return {
        {"crop_size_px", sanitize_camera_crop_size_px(camera_params.crop_pipeline.crop_size_px)},
        {"preview_max_fps", sanitize_camera_crop_preview_max_fps(
            camera_params.crop_pipeline.preview_max_fps)}
    };
}

inline void parse_rig_io_config(const nlohmann::json& camera_config,
                                CameraParams* camera_params)
{
    if (!camera_params) {
        return;
    }

    camera_params->rig_io_connections.clear();
    if (!camera_config.contains("rig_io")) {
        return;
    }

    const nlohmann::json* connections_json = nullptr;
    const nlohmann::json& rig_io = camera_config["rig_io"];
    if (rig_io.is_array()) {
        connections_json = &rig_io;
    } else if (rig_io.is_object() &&
               rig_io.contains("connections") &&
               rig_io["connections"].is_array()) {
        connections_json = &rig_io["connections"];
    }

    if (!connections_json) {
        return;
    }

    for (const auto& connection_json : *connections_json) {
        if (!connection_json.is_object()) {
            continue;
        }

        CameraRigIoConnection connection;
        detail::try_get_string(connection_json, "purpose", &connection.purpose);
        detail::try_get_string(connection_json, "direction", &connection.direction);
        if (!detail::try_get_string(connection_json, "camera_line", &connection.camera_line)) {
            detail::try_get_string(connection_json, "logical_signal", &connection.camera_line);
        }
        detail::try_get_nonnegative_int(connection_json, "physical_pin", &connection.physical_pin);
        detail::try_get_string(connection_json, "reference_line", &connection.reference_line);
        detail::try_get_nonnegative_int(connection_json, "reference_pin", &connection.reference_pin);
        detail::try_get_string(connection_json, "electrical", &connection.electrical);
        detail::try_get_string(connection_json, "active_level", &connection.active_level);
        detail::try_get_string(connection_json, "inactive_level", &connection.inactive_level);
        detail::try_get_string(connection_json, "normal_output_mode", &connection.normal_output_mode);
        if (connection_json.contains("normal_polarity") &&
            connection_json["normal_polarity"].is_boolean()) {
            connection.normal_polarity = connection_json["normal_polarity"].get<bool>();
        }
        if (connection.normal_output_mode.empty() &&
            connection.purpose == "nir_strobe_trigger") {
            connection.normal_output_mode = "Exposure";
            connection.normal_polarity = false;
        }
        detail::try_get_string(connection_json, "controlled_device", &connection.controlled_device);
        detail::try_get_positive_double(
            connection_json, "nominal_wavelength_nm", &connection.nominal_wavelength_nm);
        if (connection_json.contains("verified") && connection_json["verified"].is_boolean()) {
            connection.verified = connection_json["verified"].get<bool>();
        }
        detail::try_get_string(connection_json, "notes", &connection.notes);

        const bool has_content =
            !connection.purpose.empty() ||
            !connection.direction.empty() ||
            !connection.camera_line.empty() ||
            connection.physical_pin >= 0 ||
            !connection.reference_line.empty() ||
            connection.reference_pin >= 0 ||
            !connection.electrical.empty() ||
            !connection.active_level.empty() ||
            !connection.inactive_level.empty() ||
            !connection.normal_output_mode.empty() ||
            !connection.controlled_device.empty() ||
            connection.nominal_wavelength_nm > 0.0 ||
            connection.verified ||
            !connection.notes.empty();
        if (!has_content) {
            continue;
        }

        camera_params->rig_io_connections.push_back(std::move(connection));
    }
}

inline nlohmann::json build_rig_io_config(const CameraParams& camera_params)
{
    nlohmann::json connections = nlohmann::json::array();
    for (const auto& connection : camera_params.rig_io_connections) {
        nlohmann::json connection_json = nlohmann::json::object();
        if (!connection.purpose.empty()) {
            connection_json["purpose"] = connection.purpose;
        }
        if (!connection.direction.empty()) {
            connection_json["direction"] = connection.direction;
        }
        if (!connection.camera_line.empty()) {
            connection_json["camera_line"] = connection.camera_line;
        }
        if (connection.physical_pin >= 0) {
            connection_json["physical_pin"] = connection.physical_pin;
        }
        if (!connection.reference_line.empty()) {
            connection_json["reference_line"] = connection.reference_line;
        }
        if (connection.reference_pin >= 0) {
            connection_json["reference_pin"] = connection.reference_pin;
        }
        if (!connection.electrical.empty()) {
            connection_json["electrical"] = connection.electrical;
        }
        if (!connection.active_level.empty()) {
            connection_json["active_level"] = connection.active_level;
        }
        if (!connection.inactive_level.empty()) {
            connection_json["inactive_level"] = connection.inactive_level;
        }
        if (!connection.normal_output_mode.empty()) {
            connection_json["normal_output_mode"] = connection.normal_output_mode;
            connection_json["normal_polarity"] = connection.normal_polarity;
        }
        if (!connection.controlled_device.empty()) {
            connection_json["controlled_device"] = connection.controlled_device;
        }
        if (connection.nominal_wavelength_nm > 0.0) {
            connection_json["nominal_wavelength_nm"] = connection.nominal_wavelength_nm;
        }
        connection_json["verified"] = connection.verified;
        if (!connection.notes.empty()) {
            connection_json["notes"] = connection.notes;
        }

        if (connection_json.size() == 1 && connection_json.contains("verified") &&
            !connection.verified) {
            continue;
        }
        connections.push_back(std::move(connection_json));
    }

    return {
        {"schema_id", "orange.camera.rig_io"},
        {"schema_version", 1},
        {"connections", std::move(connections)}
    };
}

}  // namespace orange::camera_config

#endif  // ORANGE_CAMERA_CONFIG_SCHEMA_H
