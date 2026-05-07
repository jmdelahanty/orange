#ifndef ORANGE_SPATIAL_CALIBRATION_SNAPSHOT_H
#define ORANGE_SPATIAL_CALIBRATION_SNAPSHOT_H

#include "json.hpp"
#include "spatial_layout_schema.h"

#include <string>

namespace orange::spatial {

bool load_camera_spatial_calibration_from_artifact_dir(
    const std::string& artifact_dir,
    CameraSpatialCalibration* calibration_out,
    std::string* error_out = nullptr);

bool load_camera_spatial_calibration_json_from_artifact_dir(
    const std::string& artifact_dir,
    nlohmann::json* calibration_json_out,
    std::string* error_out = nullptr);

bool apply_camera_spatial_calibration_to_snapshot_json(
    nlohmann::json* snapshot,
    const std::string& camera_serial,
    const CameraSpatialCalibration& calibration,
    std::string* error_out = nullptr);

} // namespace orange::spatial

#endif // ORANGE_SPATIAL_CALIBRATION_SNAPSHOT_H
