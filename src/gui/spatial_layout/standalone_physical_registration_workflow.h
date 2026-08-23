#pragma once

#include "gui/spatial_layout/state.h"

#include <string>

class SpatialSnapshotWorker;

namespace orange::gui::spatial_layout {

void advance_standalone_physical_registration_workflow(
    SpatialLayoutUiState* ui_state,
    const CameraParams* cameras_params,
    const CameraEachSelect* cameras_select,
    int num_cameras,
    SpatialSnapshotWorker* const* spatial_snapshot_workers,
    const std::string& artifact_root_dir);

void render_standalone_physical_registration_workflow_panel(
    SpatialLayoutUiState* ui_state,
    const CameraParams* cameras_params,
    const CameraEachSelect* cameras_select,
    int num_cameras,
    SpatialSnapshotWorker* const* spatial_snapshot_workers,
    const std::string& artifact_root_dir,
    bool recording_mutation_locked);

}  // namespace orange::gui::spatial_layout
