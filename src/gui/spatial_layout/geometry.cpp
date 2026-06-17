#include "gui/spatial_layout/geometry.h"

#include <opencv2/opencv.hpp>

#include <algorithm>
#include <cmath>

namespace orange::gui::spatial_layout {
namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr int kProjectedCircleSampleCount = 96;

}  // namespace

Point2d make_point(double x, double y)
{
    return Point2d{x, y};
}

Point2d citrus_arena_origin_canvas_px(const CitrusSpatialTemplateState& template_state)
{
    if (!template_state.has_arena_canvas_region) {
        return make_point(0.0, 0.0);
    }
    return make_point(
        template_state.arena_center_x_px - template_state.arena_width_px * 0.5,
        template_state.arena_center_y_px - template_state.arena_height_px * 0.5);
}

Point2d citrus_arena_relative_to_canvas_px(
    const CitrusSpatialTemplateState& template_state,
    const Point2d& arena_relative_point)
{
    const Point2d origin = citrus_arena_origin_canvas_px(template_state);
    return make_point(origin.x + arena_relative_point.x,
                      origin.y + arena_relative_point.y);
}

Point2d citrus_canvas_to_arena_relative_px(
    const CitrusSpatialTemplateState& template_state,
    const Point2d& canvas_point)
{
    const Point2d origin = citrus_arena_origin_canvas_px(template_state);
    return make_point(canvas_point.x - origin.x,
                      canvas_point.y - origin.y);
}

bool transform_point_projective(const std::array<double, 9>& matrix,
                                const Point2d& input,
                                Point2d* output)
{
    if (output == nullptr) {
        return false;
    }
    const double w = matrix[6] * input.x + matrix[7] * input.y + matrix[8];
    if (!std::isfinite(w) || std::abs(w) < 1e-12) {
        return false;
    }
    output->x = (matrix[0] * input.x + matrix[1] * input.y + matrix[2]) / w;
    output->y = (matrix[3] * input.x + matrix[4] * input.y + matrix[5]) / w;
    return std::isfinite(output->x) && std::isfinite(output->y);
}

std::vector<Point2d> sample_circle_boundary_points(
    double cx,
    double cy,
    double radius,
    int point_count)
{
    std::vector<Point2d> points;
    point_count = std::max(3, point_count);
    points.reserve(static_cast<size_t>(point_count));
    for (int idx = 0; idx < point_count; ++idx) {
        const double theta = (2.0 * kPi * static_cast<double>(idx)) / static_cast<double>(point_count);
        points.push_back(make_point(cx + radius * std::cos(theta),
                                    cy + radius * std::sin(theta)));
    }
    return points;
}

bool sample_citrus_experimental_area_outline_in_camera_px(
    const CitrusSpatialTemplateState& template_state,
    const Point2d& center_arena_relative_px,
    std::vector<Point2d>* camera_points_out,
    std::string* error_out)
{
    if (camera_points_out == nullptr) {
        if (error_out) {
            *error_out = "Null Citrus outline destination.";
        }
        return false;
    }
    camera_points_out->clear();
    if (!template_state.available ||
        !template_state.has_arena_canvas_region ||
        !template_state.has_canvas_to_camera_homography ||
        template_state.experimental_area_radius_px <= 0.0) {
        if (error_out) {
            *error_out = "Citrus outline projection requires arena canvas region, canvas-to-camera homography, and positive radius.";
        }
        return false;
    }

    const std::vector<Point2d> arena_relative_points = sample_circle_boundary_points(
        center_arena_relative_px.x,
        center_arena_relative_px.y,
        template_state.experimental_area_radius_px,
        kProjectedCircleSampleCount);
    camera_points_out->reserve(arena_relative_points.size());
    for (const Point2d& arena_relative_point : arena_relative_points) {
        const Point2d canvas_point =
            citrus_arena_relative_to_canvas_px(template_state, arena_relative_point);
        Point2d camera_point{};
        if (!transform_point_projective(
                template_state.canvas_to_camera_homography,
                canvas_point,
                &camera_point)) {
            camera_points_out->clear();
            if (error_out) {
                *error_out = "Failed to project Citrus outline point into camera space.";
            }
            return false;
        }
        camera_points_out->push_back(camera_point);
    }
    return !camera_points_out->empty();
}

std::array<Point2d, 4> oriented_rectangle_corners(
    const orange::spatial::RuntimeGeometry& geometry)
{
    const auto& rect = geometry.oriented_rectangle;
    const double half_width = rect.width * 0.5;
    const double half_height = rect.height * 0.5;
    const double theta = rect.rotation_deg_clockwise * kPi / 180.0;
    const double cos_theta = std::cos(theta);
    const double sin_theta = std::sin(theta);

    auto rotate_and_translate = [&](double local_x, double local_y) -> Point2d {
        return make_point(
            rect.cx + cos_theta * local_x - sin_theta * local_y,
            rect.cy + sin_theta * local_x + cos_theta * local_y);
    };

    return {
        rotate_and_translate(-half_width, -half_height),
        rotate_and_translate(half_width, -half_height),
        rotate_and_translate(half_width, half_height),
        rotate_and_translate(-half_width, half_height)
    };
}

bool fit_circle_to_points(const std::vector<Point2d>& points,
                          orange::spatial::CircleGeometry* circle_out,
                          double* rms_error_out,
                          std::string* error_out)
{
    if (circle_out == nullptr) {
        if (error_out) {
            *error_out = "Null circle destination.";
        }
        return false;
    }
    if (points.size() < 3) {
        if (error_out) {
            *error_out = "Need at least three points to fit a circle.";
        }
        return false;
    }

    cv::Mat design(static_cast<int>(points.size()), 3, CV_64F);
    cv::Mat rhs(static_cast<int>(points.size()), 1, CV_64F);
    for (int row = 0; row < static_cast<int>(points.size()); ++row) {
        const double x = points[static_cast<size_t>(row)].x;
        const double y = points[static_cast<size_t>(row)].y;
        design.at<double>(row, 0) = x;
        design.at<double>(row, 1) = y;
        design.at<double>(row, 2) = 1.0;
        rhs.at<double>(row, 0) = -(x * x + y * y);
    }

    cv::Mat solution;
    if (!cv::solve(design, rhs, solution, cv::DECOMP_SVD) || solution.rows != 3) {
        if (error_out) {
            *error_out = "Failed to solve circle fit.";
        }
        return false;
    }

    const double d = solution.at<double>(0, 0);
    const double e = solution.at<double>(1, 0);
    const double f = solution.at<double>(2, 0);
    const double cx = -0.5 * d;
    const double cy = -0.5 * e;
    const double radius_sq = cx * cx + cy * cy - f;
    if (!std::isfinite(radius_sq) || radius_sq <= 0.0) {
        if (error_out) {
            *error_out = "Circle fit produced an invalid radius.";
        }
        return false;
    }

    circle_out->cx = cx;
    circle_out->cy = cy;
    circle_out->r = std::sqrt(radius_sq);

    if (rms_error_out != nullptr) {
        double sum_sq = 0.0;
        for (const Point2d& point : points) {
            const double dx = point.x - circle_out->cx;
            const double dy = point.y - circle_out->cy;
            const double radial_error = std::sqrt(dx * dx + dy * dy) - circle_out->r;
            sum_sq += radial_error * radial_error;
        }
        *rms_error_out = std::sqrt(sum_sq / static_cast<double>(points.size()));
    }

    return true;
}

Point2d transform_point(const std::array<double, 9>& matrix, double x, double y)
{
    return make_point(
        matrix[0] * x + matrix[1] * y + matrix[2],
        matrix[3] * x + matrix[4] * y + matrix[5]);
}

}  // namespace orange::gui::spatial_layout
