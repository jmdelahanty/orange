#pragma once
#include "json.hpp"
#include <filesystem>
namespace orange::recording {
struct CropOnlyResultPolicy {
    double target_fps = 0, tolerance_percent = 5;
    bool zero_camera_drops = true, zero_acquisition_starvation = true, zero_preprocess_drops = true;
};
// Camera-free post-run evaluation. Existing acquisition telemetry stays in row;
// media success comes from the authenticated master/crop/context collection.
void EvaluateCropOnlyCameraResult(const std::filesystem::path&, const std::string& serial,
    const CropOnlyResultPolicy&, nlohmann::json* row);
}
