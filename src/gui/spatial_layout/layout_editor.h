#pragma once

#include "gui/spatial_layout/state.h"

namespace orange::gui::spatial_layout {

void render_layout_geometry_editor(
    const char* label_prefix,
    orange::spatial::LayoutGeometry* geometry);

void render_registration_editor(SpatialLayoutUiState* ui_state);

void render_zone_editor(SpatialLayoutUiState* ui_state);

}  // namespace orange::gui::spatial_layout
