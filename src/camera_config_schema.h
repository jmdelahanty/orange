#ifndef ORANGE_CAMERA_CONFIG_SCHEMA_H
#define ORANGE_CAMERA_CONFIG_SCHEMA_H

#include "camera.h"
#include "json.hpp"

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
}

inline nlohmann::json build_crop_pipeline_config(const CameraParams& camera_params)
{
    return {
        {"crop_size_px", sanitize_camera_crop_size_px(camera_params.crop_pipeline.crop_size_px)}
    };
}

}  // namespace orange::camera_config

#endif  // ORANGE_CAMERA_CONFIG_SCHEMA_H
