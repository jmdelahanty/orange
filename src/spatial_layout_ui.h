#ifndef ORANGE_SPATIAL_LAYOUT_UI_H
#define ORANGE_SPATIAL_LAYOUT_UI_H

#include "gui/spatial_layout/state.h"

class SpatialSnapshotWorker;

void clear_spatial_layout_texture(SpatialLayoutUiState* ui_state);
void render_spatial_layout_window(
    SpatialLayoutUiState* ui_state,
    CameraControl* camera_control,
    CameraEmergent* ecams,
    CameraParams* cameras_params,
    CameraEachSelect* cameras_select,
    int num_cameras,
    bool other_calibration_tool_busy,
    const std::string& artifact_root_dir,
    const GLuint* live_preview_texture_ids = nullptr,
    const uint64_t* live_preview_uploaded_serials = nullptr,
    SpatialSnapshotWorker* const* spatial_snapshot_workers = nullptr);

#endif
