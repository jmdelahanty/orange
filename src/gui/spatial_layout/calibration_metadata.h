#pragma once

#include "calibration_image_set.h"
#include "gui/spatial_layout/state.h"

namespace orange::gui::spatial_layout {

SpatialLayoutCalibrationImageSetMetadata make_calibration_image_set_metadata_from_ui(
    const SpatialLayoutUiState& ui_state);

void apply_calibration_image_set_metadata_to_ui(
    SpatialLayoutUiState* ui_state,
    const SpatialLayoutCalibrationImageSetMetadata& metadata);

void apply_capture_stage_metadata_to_request(
    orange::calibration::CalibrationImageSetRequest* request,
    const SpatialLayoutCalibrationImageSetMetadata& metadata);

void attach_calibration_domain_observation(
    orange::calibration::CalibrationImageSetRequest* request,
    const SpatialLayoutCalibrationImageSetMetadata& metadata);

void attach_projection_surface_authored_domain_hint(
    orange::calibration::CalibrationImageSetRequest* request);

void attach_runtime_role_metadata(
    orange::calibration::CalibrationImageSetRequest* request);

}  // namespace orange::gui::spatial_layout
