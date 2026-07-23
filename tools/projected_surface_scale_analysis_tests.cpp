#include "gui/projected_surface_scale_analysis.h"

#include <opencv2/imgproc.hpp>

#include <cmath>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using orange::gui::projected_surface_scale::AnalysisResult;
using orange::gui::projected_surface_scale::TargetPoint;

void require(bool condition, const std::string& message)
{
    if (!condition) {
        throw std::runtime_error(message);
    }
}

std::filesystem::path target_json_path()
{
    return "/home/jeremy/citrus/targets/physical_calibration_targets/"
           "acrylic_hole_target_78mm_pitch5_margin3_v002/"
           "acrylic_hole_target_78mm_pitch5_margin3_v002.json";
}

cv::Mat synthetic_target_image(const std::vector<TargetPoint>& points,
                               cv::Mat* target_mm_to_camera_out,
                               cv::Mat* camera_to_canvas_out,
                               bool bounded_uniform_gray = false)
{
    constexpr double rectified_ppm = 10.0;
    constexpr int rectified_size = 900;
    cv::Mat rectified(
        rectified_size, rectified_size, CV_8UC1,
        cv::Scalar(bounded_uniform_gray ? 24 : 180));
    const cv::Point center(rectified_size / 2, rectified_size / 2);
    if (bounded_uniform_gray) {
        const int arena_half_extent_px = static_cast<int>(41.5 * rectified_ppm);
        cv::rectangle(
            rectified,
            cv::Rect(
                center.x - arena_half_extent_px,
                center.y - arena_half_extent_px,
                arena_half_extent_px * 2,
                arena_half_extent_px * 2),
            cv::Scalar(180), cv::FILLED);
    }
    cv::circle(rectified, center, static_cast<int>(38.5 * rectified_ppm),
               cv::Scalar(24), cv::FILLED, cv::LINE_AA);
    for (const auto& point : points) {
        const cv::Point pixel(
            static_cast<int>(std::lround(center.x + point.target_mm.x * rectified_ppm)),
            static_cast<int>(std::lround(center.y + point.target_mm.y * rectified_ppm)));
        cv::circle(
            rectified,
            pixel,
            std::max(3, static_cast<int>(std::lround(
                point.nominal_diameter_mm * 0.5 * rectified_ppm))),
            cv::Scalar(242),
            cv::FILLED,
            cv::LINE_AA);
    }
    cv::GaussianBlur(rectified, rectified, cv::Size(3, 3), 0.7);

    cv::Mat target_to_camera = (cv::Mat_<double>(3, 3) <<
        12.15, 0.38, 704.0,
        -0.22, 11.85, 591.0,
        0.00012, -0.00008, 1.0);
    cv::Mat target_to_rectified = (cv::Mat_<double>(3, 3) <<
        rectified_ppm, 0.0, rectified_size * 0.5,
        0.0, rectified_ppm, rectified_size * 0.5,
        0.0, 0.0, 1.0);
    cv::Mat rectified_to_camera = target_to_camera * target_to_rectified.inv();
    cv::Mat camera(
        1200, 1400, CV_8UC1,
        cv::Scalar(bounded_uniform_gray ? 24 : 180));
    cv::warpPerspective(
        rectified,
        camera,
        rectified_to_camera,
        camera.size(),
        cv::INTER_LINEAR,
        cv::BORDER_TRANSPARENT);

    cv::Mat camera_to_canvas = (cv::Mat_<double>(3, 3) <<
        0.348, 0.004, 18.0,
        -0.003, 0.352, 31.0,
        0.000002, -0.000003, 1.0);
    *target_mm_to_camera_out = target_to_camera;
    *camera_to_canvas_out = camera_to_canvas;
    return camera;
}

void test_loads_target_and_preserves_physical_contract()
{
    std::vector<TargetPoint> points;
    nlohmann::json definition;
    std::string error;
    require(
        orange::gui::projected_surface_scale::load_target_definition(
            target_json_path(), &points, &definition, &error),
        "target definition should load: " + error);
    require(points.size() == 161, "target should expose all 161 hole centers");
    require(definition["geometry"].value("fabricated_outer_diameter_mm", 0.0) == 77.0,
            "target should preserve measured 77 mm outside diameter");
}

void test_synthetic_fit_and_holdout()
{
    std::vector<TargetPoint> points;
    nlohmann::json definition;
    std::string error;
    require(
        orange::gui::projected_surface_scale::load_target_definition(
            target_json_path(), &points, &definition, &error),
        "target definition should load: " + error);
    cv::Mat expected_target_to_camera;
    cv::Mat camera_to_canvas;
    const cv::Mat image = synthetic_target_image(
        points, &expected_target_to_camera, &camera_to_canvas);
    const AnalysisResult result = orange::gui::projected_surface_scale::analyze(
        image, points, definition, camera_to_canvas);
    require(result.ok, "synthetic target should fit: " + result.error);
    require(result.quality_pass,
            "synthetic target should pass quality gates: " + result.report.dump());
    const auto& metrics = result.report.at("metrics");
    require(metrics.value("matched_point_count", 0) >= 145,
            "synthetic fit should match nearly all holes");
    require(metrics.value("fit_rms_mm", 1.0) < 0.08,
            "synthetic fit RMS should be sub-0.08 mm");
    require(metrics.value("holdout_rms_mm", 1.0) < 0.12,
            "perimeter holdout should predict accurately");
    require(std::abs(metrics.value("c_to_xplus_measured_mm", 0.0) - 25.0) < 0.15,
            "C-to-XPLUS should independently validate the 25 mm span");
    require(std::abs(metrics.value("outer_diameter_measured_mm", 0.0) - 77.0) < 1.0,
            "outside diameter should validate without rescaling coordinates");
    require(!result.target_mm_to_canvas_px.empty(),
            "accepted homography should compose into a canvas scale transform");
    require(result.report["plane_contract"].value(
                "occluding_target_thickness_mm", 0.0) == 3.0,
            "measurement records the actual 3 mm target thickness");
    require(!result.report["authority"].value(
                "outer_diameter_used_for_rescaling", true),
            "outside diameter must remain validation-only");
}

void test_blank_image_fails_closed()
{
    std::vector<TargetPoint> points;
    nlohmann::json definition;
    std::string error;
    require(
        orange::gui::projected_surface_scale::load_target_definition(
            target_json_path(), &points, &definition, &error),
        "target definition should load");
    const cv::Mat blank(1000, 1000, CV_8UC1, cv::Scalar(128));
    const AnalysisResult result = orange::gui::projected_surface_scale::analyze(
        blank, points, definition);
    require(!result.ok, "blank image must not produce a scale observation");
    require(!result.quality_pass, "blank image must fail quality gates");
}

void test_bounded_uniform_gray_preserves_disk_perimeter()
{
    std::vector<TargetPoint> points;
    nlohmann::json definition;
    std::string error;
    require(
        orange::gui::projected_surface_scale::load_target_definition(
            target_json_path(), &points, &definition, &error),
        "target definition should load: " + error);
    cv::Mat expected_target_to_camera;
    cv::Mat camera_to_canvas;
    const cv::Mat image = synthetic_target_image(
        points, &expected_target_to_camera, &camera_to_canvas, true);
    const AnalysisResult result = orange::gui::projected_surface_scale::analyze(
        image, points, definition, camera_to_canvas);
    require(result.ok, "bounded gray target should fit: " + result.error);
    require(result.quality_pass,
            "bounded gray target should pass every quality gate: " +
                result.report.dump());
    require(
        std::abs(result.report["metrics"].value(
                     "outer_diameter_measured_mm", 0.0) - 77.0) < 1.0,
        "narrow full-arena margin must preserve the 77 mm disk contour");
}

}  // namespace

int main()
{
    try {
        test_loads_target_and_preserves_physical_contract();
        test_synthetic_fit_and_holdout();
        test_bounded_uniform_gray_preserves_disk_perimeter();
        test_blank_image_fails_closed();
    } catch (const std::exception& error) {
        std::cerr << "projected_surface_scale_analysis_tests failed: "
                  << error.what() << std::endl;
        return 1;
    }
    std::cout << "projected_surface_scale_analysis_tests passed" << std::endl;
    return 0;
}
