#pragma once

#include "gui/spatial_layout/hough_panel.h"
#include "gui/spatial_layout/state.h"

#include <string>

namespace orange::gui::spatial_layout {

void apply_calibration_image_set_purpose_defaults(
    SpatialLayoutUiState* ui_state,
    const std::string& purpose);

void render_calibration_workflow_tabs(
    SpatialLayoutUiState* ui_state,
    const HoughCirclePanelActions& hough_actions);

} // namespace orange::gui::spatial_layout
