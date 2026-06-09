#pragma once

#include "gui/spatial_layout/state.h"

#include <array>
#include <string>
#include <vector>

namespace orange::gui::spatial_layout {

Point2d make_point(double x, double y);

Point2d citrus_arena_origin_canvas_px(const CitrusSpatialTemplateState& template_state);

Point2d citrus_arena_relative_to_canvas_px(
    const CitrusSpatialTemplateState& template_state,
    const Point2d& arena_relative_point);

Point2d citrus_canvas_to_arena_relative_px(
    const CitrusSpatialTemplateState& template_state,
    const Point2d& canvas_point);

bool transform_point_projective(const std::array<double, 9>& matrix,
                                const Point2d& input,
                                Point2d* output);

std::vector<Point2d> sample_circle_boundary_points(double cx,
                                                   double cy,
                                                   double radius,
                                                   int point_count);

bool fit_circle_to_points(const std::vector<Point2d>& points,
                          orange::spatial::CircleGeometry* circle_out,
                          double* rms_error_out,
                          std::string* error_out);

Point2d transform_point(const std::array<double, 9>& matrix, double x, double y);

}  // namespace orange::gui::spatial_layout
