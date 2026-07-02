#pragma once

#include "gui/spatial_layout/state.h"
#include "spatial_snapshot_worker.h"

#include <string>

namespace orange::gui::spatial_layout {

bool capture_single_camera_frame(
    SpatialLayoutUiState* ui_state,
    CameraEmergent* ecams,
    CameraParams* cameras_params,
    std::string* error_out);

bool capture_live_stream_preview_texture(
    SpatialLayoutUiState* ui_state,
    const CameraParams& camera_params,
    const CameraEachSelect& camera_select,
    GLuint preview_texture,
    std::string* error_out);

bool apply_full_resolution_stream_snapshot(
    SpatialLayoutUiState* ui_state,
    const SpatialSnapshotResult& result,
    std::string* error_out);

}  // namespace orange::gui::spatial_layout
