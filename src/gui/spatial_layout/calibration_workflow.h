#pragma once

#include "gui/spatial_layout/hough_panel.h"
#include "gui/spatial_layout/state.h"

#include <string>

namespace orange::gui::spatial_layout {

void apply_calibration_image_set_purpose_defaults(
    SpatialLayoutUiState* ui_state,
    const std::string& purpose);

bool apply_calibration_workflow_profile_defaults(
    SpatialLayoutUiState* ui_state,
    const std::string& workflow_profile_id,
    std::string* error_out = nullptr);

void render_calibration_workflow_tabs(
    SpatialLayoutUiState* ui_state,
    const HoughCirclePanelActions& hough_actions);

} // namespace orange::gui::spatial_layout
