#pragma once

#include "gui/spatial_layout/state.h"

#include <string>

namespace orange::gui::spatial_layout {

struct HoughCirclePanelActions {
    void (*reset_registration_from_frame)(SpatialLayoutUiState* ui_state) = nullptr;
};

bool detect_experimental_area_circle_from_capture(
    SpatialLayoutUiState* ui_state,
    std::string* error_out);

bool seed_registration_from_detected_experimental_area_circle(
    SpatialLayoutUiState* ui_state,
    std::string* error_out);

void render_hough_circle_tuning(
    SpatialLayoutUiState* ui_state,
    const HoughCirclePanelActions& actions);

} // namespace orange::gui::spatial_layout
