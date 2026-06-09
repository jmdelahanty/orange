#pragma once

#include "gui/spatial_layout/state.h"

#include <string>

namespace orange::gui::spatial_layout {

const CameraRigIoConnection* find_mapped_nir_strobe_output_connection(
    const CameraParams& camera_params);

bool camera_has_exposed_mapped_nir_strobe(const CameraParams& camera_params);

bool calibration_light_handling_needs_mapped_strobe(const std::string& requested_handling);

bool has_calibration_capture_restore_state(
    const SpatialLayoutUiState* ui_state,
    const std::string& camera_serial);

void set_calibration_preflight_result(SpatialLayoutUiState* ui_state,
                                      bool ok,
                                      const std::string& message);

bool restore_calibration_capture_preflight(
    SpatialLayoutUiState* ui_state,
    CameraEmergent* capture_ecam,
    CameraParams* capture_params,
    CameraEmergent* light_ecam,
    const CameraParams* light_params,
    bool mapped_strobe_available,
    bool recording_mutation_locked,
    std::string* status_out);

bool prepare_calibration_capture_preflight(
    SpatialLayoutUiState* ui_state,
    CameraEmergent* capture_ecam,
    CameraParams* capture_params,
    CameraEmergent* light_ecam,
    const CameraParams* light_params,
    bool mapped_strobe_available,
    bool recording_mutation_locked,
    const std::string& requested_light_handling,
    std::string* status_out);

bool restore_calibration_capture_preflight_all_cameras(
    SpatialLayoutUiState* ui_state,
    CameraEmergent* ecams,
    CameraParams* cameras_params,
    int num_cameras,
    CameraEmergent* light_ecam,
    const CameraParams* light_params,
    bool mapped_strobe_available,
    bool recording_mutation_locked,
    std::string* status_out);

bool prepare_calibration_capture_preflight_all_cameras(
    SpatialLayoutUiState* ui_state,
    CameraEmergent* ecams,
    CameraParams* cameras_params,
    int num_cameras,
    CameraEmergent* light_ecam,
    const CameraParams* light_params,
    bool mapped_strobe_available,
    bool recording_mutation_locked,
    const std::string& requested_light_handling,
    std::string* status_out);

}  // namespace orange::gui::spatial_layout
