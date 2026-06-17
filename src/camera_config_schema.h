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

inline bool try_get_nonnegative_double(const nlohmann::json& object,
                                       const char* key,
                                       double* out_value)
{
    if (!out_value || !object.contains(key) || !object[key].is_number()) {
        return false;
    }
    const double parsed = object[key].get<double>();
    if (parsed < 0.0) {
        return false;
    }
    *out_value = parsed;
    return true;
}

inline bool try_get_bool(const nlohmann::json& object,
                         const char* key,
                         bool* out_value)
{
    if (!out_value || !object.contains(key) || !object[key].is_boolean()) {
        return false;
    }
    *out_value = object[key].get<bool>();
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

inline bool lens_config_has_content(const CameraLensConfig& lens)
{
    return lens.configured ||
           lens.present ||
           !lens.manufacturer.empty() ||
           !lens.model.empty() ||
           !lens.serial.empty() ||
           !lens.mount.empty() ||
           lens.focal_length_mm > 0.0 ||
           lens.aperture_f_number > 0.0 ||
           !lens.focus_control.empty() ||
           !lens.iris_control.empty() ||
           !lens.notes.empty();
}

inline bool optical_filter_config_has_content(const CameraOpticalFilterConfig& filter)
{
    return !filter.id.empty() ||
           !filter.manufacturer.empty() ||
           !filter.model.empty() ||
           !filter.label.empty() ||
           !filter.type.empty() ||
           !filter.thread_size.empty() ||
           !filter.state.empty() ||
           !filter.runtime_role.empty() ||
           filter.cutoff_wavelength_nm > 0.0 ||
           filter.center_wavelength_nm > 0.0 ||
           filter.min_wavelength_nm > 0.0 ||
           filter.max_wavelength_nm > 0.0 ||
           filter.bandwidth_fwhm_nm > 0.0 ||
           !filter.notes.empty();
}

inline void parse_optics_config(const nlohmann::json& camera_config,
                                CameraParams* camera_params)
{
    if (!camera_params) {
        return;
    }

    camera_params->optics = CameraOpticsConfig();
    if (!camera_config.contains("optics") ||
        !camera_config["optics"].is_object()) {
        return;
    }

    const nlohmann::json& optics = camera_config["optics"];
    if (optics.contains("lens") && optics["lens"].is_object()) {
        const nlohmann::json& lens_json = optics["lens"];
        CameraLensConfig lens;
        lens.configured = true;
        detail::try_get_bool(lens_json, "present", &lens.present);
        detail::try_get_string(lens_json, "manufacturer", &lens.manufacturer);
        detail::try_get_string(lens_json, "model", &lens.model);
        detail::try_get_string(lens_json, "serial", &lens.serial);
        detail::try_get_string(lens_json, "mount", &lens.mount);
        detail::try_get_nonnegative_double(lens_json, "focal_length_mm", &lens.focal_length_mm);
        detail::try_get_nonnegative_double(lens_json, "aperture_f_number", &lens.aperture_f_number);
        detail::try_get_string(lens_json, "focus_control", &lens.focus_control);
        detail::try_get_string(lens_json, "iris_control", &lens.iris_control);
        detail::try_get_string(lens_json, "notes", &lens.notes);
        camera_params->optics.lens = std::move(lens);
    }

    const nlohmann::json* filters_json = nullptr;
    if (optics.contains("filter_stack") && optics["filter_stack"].is_array()) {
        filters_json = &optics["filter_stack"];
    } else if (optics.contains("filters") && optics["filters"].is_array()) {
        filters_json = &optics["filters"];
    }
    if (!filters_json) {
        return;
    }

    for (const nlohmann::json& filter_json : *filters_json) {
        if (!filter_json.is_object()) {
            continue;
        }
        CameraOpticalFilterConfig filter;
        detail::try_get_string(filter_json, "id", &filter.id);
        detail::try_get_string(filter_json, "manufacturer", &filter.manufacturer);
        detail::try_get_string(filter_json, "model", &filter.model);
        detail::try_get_string(filter_json, "label", &filter.label);
        detail::try_get_string(filter_json, "type", &filter.type);
        detail::try_get_string(filter_json, "thread_size", &filter.thread_size);
        detail::try_get_string(filter_json, "state", &filter.state);
        detail::try_get_string(filter_json, "runtime_role", &filter.runtime_role);
        detail::try_get_nonnegative_double(
            filter_json, "cutoff_wavelength_nm", &filter.cutoff_wavelength_nm);
        detail::try_get_nonnegative_double(
            filter_json, "center_wavelength_nm", &filter.center_wavelength_nm);
        detail::try_get_nonnegative_double(
            filter_json, "min_wavelength_nm", &filter.min_wavelength_nm);
        detail::try_get_nonnegative_double(
            filter_json, "max_wavelength_nm", &filter.max_wavelength_nm);
        detail::try_get_nonnegative_double(
            filter_json, "bandwidth_fwhm_nm", &filter.bandwidth_fwhm_nm);
        detail::try_get_string(filter_json, "notes", &filter.notes);
        if (optical_filter_config_has_content(filter)) {
            camera_params->optics.filter_stack.push_back(std::move(filter));
        }
    }
}

inline nlohmann::json build_optics_config(const CameraParams& camera_params)
{
    nlohmann::json optics = nlohmann::json::object();
    const CameraLensConfig& lens = camera_params.optics.lens;
    if (lens_config_has_content(lens)) {
        nlohmann::json lens_json = nlohmann::json::object();
        lens_json["present"] = lens.present;
        if (!lens.manufacturer.empty()) {
            lens_json["manufacturer"] = lens.manufacturer;
        }
        if (!lens.model.empty()) {
            lens_json["model"] = lens.model;
        }
        if (!lens.serial.empty()) {
            lens_json["serial"] = lens.serial;
        }
        if (!lens.mount.empty()) {
            lens_json["mount"] = lens.mount;
        }
        if (lens.focal_length_mm > 0.0) {
            lens_json["focal_length_mm"] = lens.focal_length_mm;
        }
        if (lens.aperture_f_number > 0.0) {
            lens_json["aperture_f_number"] = lens.aperture_f_number;
        }
        if (!lens.focus_control.empty()) {
            lens_json["focus_control"] = lens.focus_control;
        }
        if (!lens.iris_control.empty()) {
            lens_json["iris_control"] = lens.iris_control;
        }
        if (!lens.notes.empty()) {
            lens_json["notes"] = lens.notes;
        }
        optics["lens"] = std::move(lens_json);
    }

    nlohmann::json filter_stack = nlohmann::json::array();
    for (const CameraOpticalFilterConfig& filter : camera_params.optics.filter_stack) {
        if (!optical_filter_config_has_content(filter)) {
            continue;
        }
        nlohmann::json filter_json = nlohmann::json::object();
        if (!filter.id.empty()) {
            filter_json["id"] = filter.id;
        }
        if (!filter.manufacturer.empty()) {
            filter_json["manufacturer"] = filter.manufacturer;
        }
        if (!filter.model.empty()) {
            filter_json["model"] = filter.model;
        }
        if (!filter.label.empty()) {
            filter_json["label"] = filter.label;
        }
        if (!filter.type.empty()) {
            filter_json["type"] = filter.type;
        }
        if (!filter.thread_size.empty()) {
            filter_json["thread_size"] = filter.thread_size;
        }
        if (!filter.state.empty()) {
            filter_json["state"] = filter.state;
        }
        if (!filter.runtime_role.empty()) {
            filter_json["runtime_role"] = filter.runtime_role;
        }
        if (filter.cutoff_wavelength_nm > 0.0) {
            filter_json["cutoff_wavelength_nm"] = filter.cutoff_wavelength_nm;
        }
        if (filter.center_wavelength_nm > 0.0) {
            filter_json["center_wavelength_nm"] = filter.center_wavelength_nm;
        }
        if (filter.min_wavelength_nm > 0.0) {
            filter_json["min_wavelength_nm"] = filter.min_wavelength_nm;
        }
        if (filter.max_wavelength_nm > 0.0) {
            filter_json["max_wavelength_nm"] = filter.max_wavelength_nm;
        }
        if (filter.bandwidth_fwhm_nm > 0.0) {
            filter_json["bandwidth_fwhm_nm"] = filter.bandwidth_fwhm_nm;
        }
        if (!filter.notes.empty()) {
            filter_json["notes"] = filter.notes;
        }
        filter_stack.push_back(std::move(filter_json));
    }
    if (!filter_stack.empty()) {
        optics["filter_stack"] = std::move(filter_stack);
    }

    if (!optics.empty()) {
        optics["schema_id"] = "orange.camera.optics";
        optics["schema_version"] = 1;
    }
    return optics;
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
