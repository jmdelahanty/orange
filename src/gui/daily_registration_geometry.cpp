#include "gui/daily_registration_geometry.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace orange::gui::daily_registration {
namespace {

constexpr double kPi = 3.14159265358979323846;

bool Project(const std::array<double, 9>& matrix,
             double x,
             double y,
             Point2d* output)
{
    if (output == nullptr) return false;
    const double w = matrix[6] * x + matrix[7] * y + matrix[8];
    if (!std::isfinite(w) || std::abs(w) < 1e-12) return false;
    output->x = (matrix[0] * x + matrix[1] * y + matrix[2]) / w;
    output->y = (matrix[3] * x + matrix[4] * y + matrix[5]) / w;
    return std::isfinite(output->x) && std::isfinite(output->y);
}

bool FiniteInput(const GeometryReviewInput& input)
{
    for (const double value : input.canvas_to_camera_homography) {
        if (!std::isfinite(value)) return false;
    }
    return std::isfinite(input.desired_experimental_center_canvas_x_px) &&
        std::isfinite(input.desired_experimental_center_canvas_y_px) &&
        std::isfinite(input.effective_experimental_center_canvas_x_px) &&
        std::isfinite(input.effective_experimental_center_canvas_y_px) &&
        std::isfinite(input.canonical_experimental_radius_canvas_px) &&
        std::isfinite(input.accepted_rim_center_camera_x_px) &&
        std::isfinite(input.accepted_rim_center_camera_y_px) &&
        std::isfinite(input.accepted_rim_radius_camera_px);
}

}  // namespace

GeometryReviewResult ComputeGeometryReview(const GeometryReviewInput& input)
{
    GeometryReviewResult result;
    if (!FiniteInput(input) ||
        input.canonical_experimental_radius_canvas_px <= 0.0 ||
        input.accepted_rim_radius_camera_px <= 0.0 ||
        input.outline_sample_count < 8 || input.outline_sample_count > 4096) {
        result.error = "invalid_daily_registration_geometry_review_input";
        return result;
    }
    if (!Project(
            input.canvas_to_camera_homography,
            input.effective_experimental_center_canvas_x_px,
            input.effective_experimental_center_canvas_y_px,
            &result.corrected_center_camera_px)) {
        result.error = "daily_registration_center_inverse_projection_failed";
        return result;
    }

    result.center_residual_x_camera_px =
        result.corrected_center_camera_px.x -
        input.accepted_rim_center_camera_x_px;
    result.center_residual_y_camera_px =
        result.corrected_center_camera_px.y -
        input.accepted_rim_center_camera_y_px;
    result.center_residual_norm_camera_px = std::hypot(
        result.center_residual_x_camera_px,
        result.center_residual_y_camera_px);

    Point2d desired_center_camera;
    if (!Project(
            input.canvas_to_camera_homography,
            input.desired_experimental_center_canvas_x_px,
            input.desired_experimental_center_canvas_y_px,
            &desired_center_camera)) {
        result.error = "daily_registration_desired_center_inverse_projection_failed";
        return result;
    }
    for (const double dx : {-0.5, 0.5}) {
        for (const double dy : {-0.5, 0.5}) {
            Point2d corner_camera;
            if (!Project(
                    input.canvas_to_camera_homography,
                    input.desired_experimental_center_canvas_x_px + dx,
                    input.desired_experimental_center_canvas_y_px + dy,
                    &corner_camera)) {
                result.error =
                    "daily_registration_quantization_bound_projection_failed";
                return result;
            }
            result.integer_translation_quantization_bound_camera_px =
                std::max(
                    result.integer_translation_quantization_bound_camera_px,
                    std::hypot(
                        corner_camera.x - desired_center_camera.x,
                        corner_camera.y - desired_center_camera.y));
        }
    }

    result.canonical_outline_camera_px.reserve(
        static_cast<std::size_t>(input.outline_sample_count));
    double radius_sum = 0.0;
    double squared_radial_error_sum = 0.0;
    result.predicted_radius_min_camera_px =
        std::numeric_limits<double>::infinity();
    result.predicted_radius_max_camera_px = 0.0;
    for (int index = 0; index < input.outline_sample_count; ++index) {
        const double angle = 2.0 * kPi * static_cast<double>(index) /
            static_cast<double>(input.outline_sample_count);
        Point2d camera_point;
        if (!Project(
                input.canvas_to_camera_homography,
                input.effective_experimental_center_canvas_x_px +
                    input.canonical_experimental_radius_canvas_px *
                        std::cos(angle),
                input.effective_experimental_center_canvas_y_px +
                    input.canonical_experimental_radius_canvas_px *
                        std::sin(angle),
                &camera_point)) {
            result.canonical_outline_camera_px.clear();
            result.error =
                "daily_registration_outline_inverse_projection_failed";
            return result;
        }
        const double radius = std::hypot(
            camera_point.x - input.accepted_rim_center_camera_x_px,
            camera_point.y - input.accepted_rim_center_camera_y_px);
        const double radial_error =
            radius - input.accepted_rim_radius_camera_px;
        radius_sum += radius;
        squared_radial_error_sum += radial_error * radial_error;
        result.predicted_radius_min_camera_px = std::min(
            result.predicted_radius_min_camera_px, radius);
        result.predicted_radius_max_camera_px = std::max(
            result.predicted_radius_max_camera_px, radius);
        result.maximum_outside_rim_camera_px = std::max(
            result.maximum_outside_rim_camera_px,
            std::max(0.0, radial_error));
        result.canonical_outline_camera_px.push_back(camera_point);
    }
    const double sample_count = static_cast<double>(input.outline_sample_count);
    result.predicted_radius_mean_camera_px = radius_sum / sample_count;
    result.rim_radial_rms_error_camera_px =
        std::sqrt(squared_radial_error_sum / sample_count);
    result.ok = true;
    return result;
}

}  // namespace orange::gui::daily_registration
