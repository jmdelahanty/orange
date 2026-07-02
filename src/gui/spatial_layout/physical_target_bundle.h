#pragma once

#include "calibration_image_set.h"

#include <filesystem>
#include <string>

namespace orange::gui::spatial_layout {

std::filesystem::path default_physical_calibration_target_json_path();

bool materialize_physical_target_bundle_for_request(
    orange::calibration::CalibrationImageSetRequest* request,
    const std::filesystem::path& artifact_dir,
    std::string* error_out);

}  // namespace orange::gui::spatial_layout
