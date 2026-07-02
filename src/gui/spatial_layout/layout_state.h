#pragma once

#include "gui/spatial_layout/state.h"

#include <array>

namespace orange::gui::spatial_layout {

double normalize_angle_deg(double angle_deg);

Point2d layout_geometry_center(const orange::spatial::LayoutGeometry& geometry);

double layout_geometry_max_dimension(const orange::spatial::LayoutGeometry& geometry);

orange::spatial::RuntimeGeometry runtime_circle(double cx, double cy, double r);

orange::spatial::RuntimeGeometry runtime_oriented_rectangle(
    double cx,
    double cy,
    double width,
    double height,
    double rotation_deg_clockwise);

orange::spatial::LayoutGeometry default_outer_geometry();

orange::spatial::ArenaLayoutZone make_experimental_area_zone(
    const orange::spatial::LayoutGeometry& outer_geometry);

bool has_single_experimental_area_zone(const SpatialLayoutUiState& ui_state);

void sync_single_experimental_area_zone(SpatialLayoutUiState* ui_state);

void reset_to_single_experimental_area_zone(SpatialLayoutUiState* ui_state);

orange::spatial::ArenaLayoutZone make_default_zone(
    const orange::spatial::LayoutGeometry& outer_geometry,
    int zone_index);

void initialize_spatial_layout_defaults(SpatialLayoutUiState* ui_state);

void reset_registration_from_frame(SpatialLayoutUiState* ui_state);

void clear_detected_experimental_area_circle(SpatialLayoutUiState* ui_state);

bool invert_affine_3x3(const std::array<double, 9>& matrix, std::array<double, 9>* out);

std::array<double, 9> build_layout_to_camera_matrix(const SpatialLayoutUiState& ui_state);

void set_registration_transform(SpatialLayoutUiState* ui_state,
                                orange::spatial::RegistrationType type,
                                const Point2d& desired_outer_center,
                                double scale,
                                double rotation_deg_clockwise,
                                orange::spatial::RegistrationSource source);

}  // namespace orange::gui::spatial_layout
