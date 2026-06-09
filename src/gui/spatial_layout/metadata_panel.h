#pragma once

#include "gui/spatial_layout/state.h"

#include <string>

struct CameraControl;
struct CameraEmergent;
struct CameraParams;

namespace orange::gui::spatial_layout {

struct CalibrationCaptureMetadataPanelActions {
    void (*apply_calibration_image_set_purpose_defaults)(
        SpatialLayoutUiState* ui_state,
        const std::string& purpose) = nullptr;
};

void apply_illumination_preset(SpatialLayoutUiState* ui_state, const std::string& preset);

void render_calibration_capture_metadata_panel(
    SpatialLayoutUiState* ui_state,
    CameraControl* camera_control,
    CameraEmergent* ecams,
    CameraParams* cameras_params,
    int num_cameras,
    CameraParams& selected_camera,
    const CalibrationCaptureMetadataPanelActions& actions);

} // namespace orange::gui::spatial_layout
