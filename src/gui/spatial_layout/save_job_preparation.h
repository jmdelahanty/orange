#pragma once

#include "gui/spatial_layout/save_jobs.h"
#include "gui/spatial_layout/state.h"

#include <string>

namespace orange::gui::spatial_layout {

bool prepare_dish_top_rim_observation_save_job_from_spatial_layout(
    SpatialLayoutUiState* ui_state,
    const CameraParams& selected_camera,
    const std::string& artifact_root_dir,
    TopRimObservationSaveJob* job_out,
    std::string* error_out);

bool prepare_generic_calibration_image_set_save_job_from_spatial_layout(
    SpatialLayoutUiState* ui_state,
    const CameraParams& selected_camera,
    const std::string& artifact_root_dir,
    GenericCalibrationImageSetSaveJob* job_out,
    std::string* error_out);

bool queue_group_calibration_image_set_save_jobs(
    SpatialLayoutUiState* ui_state,
    const CameraParams* cameras_params,
    int num_cameras,
    const CameraParams& selected_camera,
    const std::string& artifact_root_dir,
    std::string* status_out,
    std::string* error_out);

}  // namespace orange::gui::spatial_layout
