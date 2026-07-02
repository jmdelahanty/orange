#pragma once

#include "gui/spatial_layout/state.h"

namespace orange::gui::spatial_layout {

struct PreviewOverlayActions {
    bool (*handle_registration_canvas_edit)(SpatialLayoutUiState* ui_state) = nullptr;
    bool (*handle_selected_zone_canvas_edit)(SpatialLayoutUiState* ui_state) = nullptr;
};

bool draw_runtime_preview(
    SpatialLayoutUiState* ui_state,
    const PreviewOverlayActions& actions);

} // namespace orange::gui::spatial_layout
