#pragma once

#include "gui/spatial_layout/state.h"

#include <string>

class SpatialSnapshotWorker;

namespace orange::gui::spatial_layout {

// Progresses only asynchronous capture/save/control acknowledgements. It does
// not cross an operator-review boundary on its own.
void advance_daily_registration_workflow(
    SpatialLayoutUiState* ui_state,
    const CameraParams* cameras_params,
    const CameraEachSelect* cameras_select,
    int num_cameras,
    SpatialSnapshotWorker* const* spatial_snapshot_workers,
    const std::string& artifact_root_dir);

// Renders the opt-in daily workflow and issues semantic actions only from
// explicit operator buttons/confirmations.
void render_daily_registration_workflow_panel(
    SpatialLayoutUiState* ui_state,
    const CameraParams* cameras_params,
    const CameraEachSelect* cameras_select,
    int num_cameras,
    SpatialSnapshotWorker* const* spatial_snapshot_workers,
    const std::string& artifact_root_dir,
    bool recording_mutation_locked);

}  // namespace orange::gui::spatial_layout
