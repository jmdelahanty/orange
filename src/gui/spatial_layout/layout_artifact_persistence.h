#pragma once

#include "gui/spatial_layout/state.h"

#include <string>

namespace orange::gui::spatial_layout {

bool reject_legacy_top_level_calibration_artifact_root(
    const std::string& artifact_root_dir,
    std::string* error_out);

bool persist_spatial_layout_artifact_bundle(
    orange::spatial::ArenaLayoutArtifact artifact,
    const orange::spatial::DishMaskRuntime& dish_mask_runtime,
    const orange::spatial::ArenaLayoutRuntime& arena_layout_runtime,
    const CameraParams& camera_params,
    const std::string& artifact_root_dir,
    const std::string& session_dir,
    orange::spatial::ArenaLayoutArtifact* saved_artifact_out,
    std::string* saved_artifact_dir_out,
    std::string* error_out);

bool save_spatial_layout_artifact(
    SpatialLayoutUiState* ui_state,
    const CameraParams& selected_camera,
    const std::string& artifact_root_dir,
    const std::string& session_dir,
    std::string* status_out,
    std::string* error_out);

}  // namespace orange::gui::spatial_layout
