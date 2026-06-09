#pragma once

#include "gui/spatial_layout/state.h"

#include <string>

struct CameraParams;

namespace orange::gui::spatial_layout {

struct GroupCapturePanelActions {
    bool (*use_group_capture_for_fit)(
        SpatialLayoutUiState* ui_state,
        SpatialLayoutGroupCaptureFrame* capture,
        const CameraParams* cameras_params,
        int num_cameras,
        std::string* error_out) = nullptr;
};

void render_group_capture_panels(
    SpatialLayoutUiState* ui_state,
    const CameraParams* cameras_params,
    int num_cameras,
    const GroupCapturePanelActions& actions);

} // namespace orange::gui::spatial_layout
