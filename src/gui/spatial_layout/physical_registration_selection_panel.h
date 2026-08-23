#pragma once

#include "gui/spatial_layout/state.h"

#include <string>

namespace orange::gui::spatial_layout {

void render_physical_registration_selection_panel(
    SpatialLayoutUiState* ui_state,
    const CameraParams& selected_camera,
    const std::string& artifact_root_dir,
    bool recording_mutation_locked);

}  // namespace orange::gui::spatial_layout
