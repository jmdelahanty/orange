#pragma once

#include "gui/spatial_layout/state.h"
#include "json.hpp"

#include <filesystem>
#include <string>

namespace orange::gui::projected_surface_scale {

struct GroupArtifactResult {
    bool ok = false;
    bool quality_pass = false;
    std::string error;
    nlohmann::json observations = nlohmann::json::array();
    nlohmann::json verification = nlohmann::json::object();
    std::filesystem::path manifest_path;
    std::string canvas_sha256;
};

GroupArtifactResult analyze_and_write_group(
    const SpatialLayoutUiState& spatial_state,
    const std::filesystem::path& calibration_session_dir,
    const std::filesystem::path& citrus_canvas_path);

}  // namespace orange::gui::projected_surface_scale
