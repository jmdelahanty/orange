#pragma once

#include "gui/spatial_layout/state.h"

#include <filesystem>
#include <string>
#include <vector>

namespace orange::gui::spatial_layout {

bool load_spatial_calibration_session_review(
    SpatialLayoutUiState* ui_state,
    const std::filesystem::path& selected_path,
    std::string* status_out,
    std::string* error_out);

bool load_rgba_image_from_path(
    const std::filesystem::path& image_path,
    std::vector<unsigned char>* rgba_out,
    int* width_out,
    int* height_out,
    std::string* error_out);

} // namespace orange::gui::spatial_layout
