#pragma once

#include "gui/spatial_layout/state.h"

#include <array>

namespace orange::gui::spatial_layout {

void apply_view_registration_to_editor_state(
    SpatialLayoutUiState* ui_state,
    const orange::spatial::ViewRegistration& registration);

orange::spatial::RuntimeGeometry transform_layout_geometry(
    const orange::spatial::LayoutGeometry& layout_geometry,
    const std::array<double, 9>& layout_to_camera_matrix,
    double rotation_deg_clockwise);

orange::spatial::VisibilityStatus compute_visibility_status(
    const orange::spatial::RuntimeGeometry& geometry,
    int image_width,
    int image_height);

void rebuild_schema_preview(SpatialLayoutUiState* ui_state, const CameraParams* selected_camera);

}  // namespace orange::gui::spatial_layout
