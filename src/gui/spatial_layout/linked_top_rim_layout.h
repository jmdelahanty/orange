#pragma once

#include "gui/spatial_layout/state.h"

#include <string>

namespace orange::gui::spatial_layout {

bool save_linked_arena_layout_artifacts(
    SpatialLayoutUiState* ui_state,
    const CameraParams* cameras_params,
    int num_cameras,
    const std::string& session_artifact_root,
    std::string* status_out,
    std::string* error_out);

}  // namespace orange::gui::spatial_layout
