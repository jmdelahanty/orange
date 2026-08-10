#include "gui/daily_registration_geometry.h"

#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>

namespace daily = orange::gui::daily_registration;

namespace {

void Require(bool condition, const std::string& message)
{
    if (!condition) throw std::runtime_error(message);
}

void RequireNear(double actual, double expected, double tolerance,
                 const std::string& message)
{
    if (std::abs(actual - expected) > tolerance) {
        throw std::runtime_error(
            message + ": actual=" + std::to_string(actual) +
            " expected=" + std::to_string(expected));
    }
}

void TestIdentityReviewPreservesCanonicalRadius()
{
    daily::GeometryReviewInput input;
    input.desired_experimental_center_canvas_x_px = 100.5;
    input.desired_experimental_center_canvas_y_px = 99.5;
    input.effective_experimental_center_canvas_x_px = 101.0;
    input.effective_experimental_center_canvas_y_px = 99.0;
    input.canonical_experimental_radius_canvas_px = 40.0;
    input.accepted_rim_center_camera_x_px = 100.5;
    input.accepted_rim_center_camera_y_px = 99.5;
    input.accepted_rim_radius_camera_px = 42.0;
    const auto result = daily::ComputeGeometryReview(input);
    Require(result.ok, "identity geometry review should succeed");
    RequireNear(result.corrected_center_camera_px.x, 101.0, 1e-9,
                "effective center x should inverse-project exactly");
    RequireNear(result.center_residual_norm_camera_px,
                std::sqrt(0.5), 1e-9,
                "center residual should be measured against the fitted rim");
    RequireNear(result.integer_translation_quantization_bound_camera_px,
                std::sqrt(0.5), 1e-9,
                "identity quantization bound should span half a canvas pixel per axis");
    RequireNear(result.predicted_radius_min_camera_px, 39.2936, 0.01,
                "rim-relative radius should include center rounding residual");
    RequireNear(result.predicted_radius_mean_camera_px, 40.0031, 0.01,
                "canonical radius must remain unchanged by registration");
    RequireNear(result.maximum_outside_rim_camera_px, 0.0, 1e-9,
                "a smaller canonical outline should remain within the rim");
    Require(result.canonical_outline_camera_px.size() == 96,
            "the full review outline should be retained");
}

void TestAffineCameraScaleProducesCameraSpaceQc()
{
    daily::GeometryReviewInput input;
    input.canvas_to_camera_homography = {
        2.0, 0.0, 10.0,
        0.0, 3.0, -5.0,
        0.0, 0.0, 1.0};
    input.effective_experimental_center_canvas_x_px = 20.0;
    input.effective_experimental_center_canvas_y_px = 30.0;
    input.desired_experimental_center_canvas_x_px = 20.0;
    input.desired_experimental_center_canvas_y_px = 30.0;
    input.canonical_experimental_radius_canvas_px = 10.0;
    input.accepted_rim_center_camera_x_px = 50.0;
    input.accepted_rim_center_camera_y_px = 85.0;
    input.accepted_rim_radius_camera_px = 24.0;
    const auto result = daily::ComputeGeometryReview(input);
    Require(result.ok, "affine geometry review should succeed");
    RequireNear(result.center_residual_norm_camera_px, 0.0, 1e-9,
                "aligned affine center should have zero residual");
    RequireNear(result.integer_translation_quantization_bound_camera_px,
                std::sqrt(3.25), 1e-9,
                "quantization bound should account for both camera-axis scales");
    RequireNear(result.predicted_radius_min_camera_px, 20.0, 1e-9,
                "x camera scale should define the minimum radius");
    RequireNear(result.predicted_radius_max_camera_px, 30.0, 1e-9,
                "y camera scale should define the maximum radius");
    RequireNear(result.maximum_outside_rim_camera_px, 6.0, 1e-9,
                "outside-rim QC should report the worst projected point");
}

void TestSingularProjectionFailsClosed()
{
    daily::GeometryReviewInput input;
    input.canvas_to_camera_homography = {
        1.0, 0.0, 0.0,
        0.0, 1.0, 0.0,
        0.0, 0.0, 0.0};
    input.canonical_experimental_radius_canvas_px = 10.0;
    input.accepted_rim_radius_camera_px = 10.0;
    const auto result = daily::ComputeGeometryReview(input);
    Require(!result.ok, "singular inverse projection must fail closed");
    Require(result.error ==
                "daily_registration_center_inverse_projection_failed",
            "singular projection should expose a stable error");
}

void TestAutomaticAndManualTranslationsComposeInCanvasCoordinates()
{
    daily::TranslationCompositionInput input;
    input.base_center_canvas_x_px = 100.0;
    input.base_center_canvas_y_px = 100.0;
    input.automatic_x_canvas_px = 6;
    input.automatic_y_canvas_px = 5;
    input.manual_delta_x_canvas_px = -7;
    input.manual_delta_y_canvas_px = -13;

    const auto translation = daily::ComposeTranslation(input);
    Require(translation.ok, "valid translation composition should succeed");
    Require(translation.applied_x_canvas_px == -1,
            "manual x is an absolute delta from the automatic result");
    Require(translation.applied_y_canvas_px == -8,
            "manual y is an absolute delta from the automatic result");
    RequireNear(translation.effective_center_canvas_x_px, 99.0, 1e-9,
                "final x center should use the composed canvas translation");
    RequireNear(translation.effective_center_canvas_y_px, 92.0, 1e-9,
                "final y center should use the composed canvas translation");

    daily::GeometryReviewInput review;
    review.desired_experimental_center_canvas_x_px = 99.0;
    review.desired_experimental_center_canvas_y_px = 92.0;
    review.effective_experimental_center_canvas_x_px =
        translation.effective_center_canvas_x_px;
    review.effective_experimental_center_canvas_y_px =
        translation.effective_center_canvas_y_px;
    review.canonical_experimental_radius_canvas_px = 40.0;
    review.accepted_rim_center_camera_x_px = 99.0;
    review.accepted_rim_center_camera_y_px = 92.0;
    review.accepted_rim_radius_camera_px = 40.0;
    const auto geometry = daily::ComputeGeometryReview(review);
    Require(geometry.ok, "composed translation should produce valid geometry");
    RequireNear(geometry.center_residual_norm_camera_px, 0.0, 1e-9,
                "identity projection should preserve composed direction");
    RequireNear(geometry.predicted_radius_mean_camera_px, 40.0, 1e-9,
                "translation must not resize the canonical radius");
}

void TestZeroManualDeltaRestoresAutomaticResult()
{
    daily::TranslationCompositionInput input;
    input.base_center_canvas_x_px = 258.0;
    input.base_center_canvas_y_px = 331.0;
    input.automatic_x_canvas_px = 7;
    input.automatic_y_canvas_px = -5;
    const auto translation = daily::ComposeTranslation(input);
    Require(translation.ok, "automatic-only translation should succeed");
    Require(translation.applied_x_canvas_px == 7 &&
                translation.applied_y_canvas_px == -5,
            "zero manual delta should exactly restore automatic translation");
    RequireNear(translation.effective_center_canvas_x_px, 265.0, 1e-9,
                "automatic-only x center mismatch");
    RequireNear(translation.effective_center_canvas_y_px, 326.0, 1e-9,
                "automatic-only y center mismatch");
}

void TestTranslationOverflowFailsClosed()
{
    daily::TranslationCompositionInput input;
    input.automatic_x_canvas_px = std::numeric_limits<int>::max();
    input.manual_delta_x_canvas_px = 1;
    const auto translation = daily::ComposeTranslation(input);
    Require(!translation.ok, "integer overflow must fail closed");
    Require(translation.error ==
                "daily_registration_translation_integer_overflow",
            "overflow should expose a stable error");
}

}  // namespace

int main()
{
    try {
        TestIdentityReviewPreservesCanonicalRadius();
        TestAffineCameraScaleProducesCameraSpaceQc();
        TestSingularProjectionFailsClosed();
        TestAutomaticAndManualTranslationsComposeInCanvasCoordinates();
        TestZeroManualDeltaRestoresAutomaticResult();
        TestTranslationOverflowFailsClosed();
        std::cout << "daily registration geometry tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "daily registration geometry tests failed: "
                  << error.what() << '\n';
        return 1;
    }
}
