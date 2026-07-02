#pragma once

#include "gui/spatial_layout/state.h"

#include <filesystem>
#include <string>

namespace orange::gui::spatial_layout {

void clear_citrus_template_import(SpatialLayoutUiState* ui_state);

bool seed_registration_from_citrus_homography(
    SpatialLayoutUiState* ui_state,
    std::string* error_out);

bool select_citrus_template_by_index(
    SpatialLayoutUiState* ui_state,
    int index,
    std::string* status_out,
    std::string* error_out);

bool import_citrus_canvas_templates(
    SpatialLayoutUiState* ui_state,
    const CameraParams& selected_camera,
    const std::filesystem::path& config_path,
    std::string* status_out,
    std::string* error_out);

}  // namespace orange::gui::spatial_layout
