#pragma once

#include <array>
#include <string>
#include <vector>

namespace orange::gui::daily_registration {

struct Point2d {
    double x = 0.0;
    double y = 0.0;
};

struct TranslationCompositionInput {
    double base_center_canvas_x_px = 0.0;
    double base_center_canvas_y_px = 0.0;
    int automatic_x_canvas_px = 0;
    int automatic_y_canvas_px = 0;
    int manual_delta_x_canvas_px = 0;
    int manual_delta_y_canvas_px = 0;
};

struct TranslationCompositionResult {
    bool ok = false;
    std::string error;
    int applied_x_canvas_px = 0;
    int applied_y_canvas_px = 0;
    double effective_center_canvas_x_px = 0.0;
    double effective_center_canvas_y_px = 0.0;
};

// The operator delta is absolute relative to the immutable automatic result.
// Keeping this arithmetic in one checked helper prevents UI preview, artifact
// validation, and acceptance from developing different sign conventions.
TranslationCompositionResult ComposeTranslation(
    const TranslationCompositionInput& input);

struct GeometryReviewInput {
    std::array<double, 9> canvas_to_camera_homography{
        1.0, 0.0, 0.0,
        0.0, 1.0, 0.0,
        0.0, 0.0, 1.0};
    double desired_experimental_center_canvas_x_px = 0.0;
    double desired_experimental_center_canvas_y_px = 0.0;
    double effective_experimental_center_canvas_x_px = 0.0;
    double effective_experimental_center_canvas_y_px = 0.0;
    double canonical_experimental_radius_canvas_px = 0.0;
    double accepted_rim_center_camera_x_px = 0.0;
    double accepted_rim_center_camera_y_px = 0.0;
    double accepted_rim_radius_camera_px = 0.0;
    int outline_sample_count = 96;
};

struct GeometryReviewResult {
    bool ok = false;
    std::string error;
    Point2d corrected_center_camera_px;
    double center_residual_x_camera_px = 0.0;
    double center_residual_y_camera_px = 0.0;
    double center_residual_norm_camera_px = 0.0;
    double integer_translation_quantization_bound_camera_px = 0.0;
    double predicted_radius_min_camera_px = 0.0;
    double predicted_radius_mean_camera_px = 0.0;
    double predicted_radius_max_camera_px = 0.0;
    double rim_radial_rms_error_camera_px = 0.0;
    double maximum_outside_rim_camera_px = 0.0;
    std::vector<Point2d> canonical_outline_camera_px;
};

GeometryReviewResult ComputeGeometryReview(const GeometryReviewInput& input);

}  // namespace orange::gui::daily_registration
