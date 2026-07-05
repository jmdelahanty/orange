#include "ruler_alignment.h"

#include <opencv2/core.hpp>

#include <cmath>
#include <iostream>
#include <stdexcept>
#include <string>

// Contract of detect_ruler_alignment (inferred from src/ruler_alignment.cpp):
//
// Input: a single-channel grayscale image plus the expected ruler orientation.
// The detector looks for the dominant intensity boundary running along the
// requested axis (kHorizontal: a boundary between an upper and lower region;
// kVertical: between a left and right region), with a scoring bias toward
// boundaries nearer the top/left edge of the frame.
//
// Pipeline: (1) gradient-profile anchor: directional Gaussian smoothing,
// Sobel across the axis, per-row/column mean |gradient| profile, peak picked
// while excluding a margin of max(4, extent / 40) pixels at both ends;
// (2) Canny + probabilistic Hough restricted to a band of
// +/- max(16, 4% of extent) pixels around the anchor, keeping lines whose
// angle deviates <= 25 deg from the requested axis; (3) if Hough finds no
// acceptable line but an anchor exists, a synthetic axis-aligned line at the
// anchor is reported with angle_error_deg == 0.
//
// Outputs (units): line_angle_deg in (-90, 90] degrees (0 == horizontal,
// +/-90 == vertical); angle_error_deg = |deviation from the requested axis|
// in degrees; center_offset_px = signed pixel distance of the line midpoint
// from the image centre, measured perpendicular to the requested axis
// (positive = below centre for kHorizontal, right of centre for kVertical);
// center_offset_fraction = center_offset_px / (half image extent);
// (x0, y0)-(x1, y1) = detected segment endpoints in full-image pixel coords.
//
// Failure mode: has_detected_line == false (all other fields default zero) is
// returned only when the image is empty, or when the scanned extent is too
// small for the profile scan (extent <= 2 * margin, e.g. <= 8 rows for
// kHorizontal). Notably, a *uniform* image of normal size does NOT report
// failure: the all-zero gradient profile ties at score 0, so the first
// scanned index (== margin) becomes the anchor and the fallback path reports
// a synthetic axis-aligned line at row/column == margin.

namespace {

void require(bool condition, const std::string& message)
{
    if (!condition) {
        throw std::runtime_error(message);
    }
}

void test_horizontal_boundary_detected_at_known_row()
{
    constexpr int kRows = 480;
    constexpr int kCols = 640;
    constexpr int kBoundaryRow = 160;
    cv::Mat gray(kRows, kCols, CV_8UC1, cv::Scalar(40));
    gray.rowRange(kBoundaryRow, kRows).setTo(cv::Scalar(200));

    const RulerAlignmentMetrics metrics =
        detect_ruler_alignment(gray, RulerAlignmentOrientation::kHorizontal);

    require(metrics.has_detected_line, "horizontal: boundary should be detected");
    require(std::abs(metrics.line_angle_deg) <= 2.0,
            "horizontal: line angle should be ~0 deg, got " +
                std::to_string(metrics.line_angle_deg));
    require(metrics.angle_error_deg <= 2.0,
            "horizontal: angle error should be ~0 deg, got " +
                std::to_string(metrics.angle_error_deg));

    const double line_row = 0.5 * (static_cast<double>(metrics.y0) + static_cast<double>(metrics.y1));
    require(std::abs(line_row - static_cast<double>(kBoundaryRow)) <= 5.0,
            "horizontal: boundary row should be ~160, got " + std::to_string(line_row));

    const double expected_offset = line_row - 0.5 * static_cast<double>(kRows);
    require(std::abs(metrics.center_offset_px - expected_offset) <= 1e-6,
            "horizontal: center_offset_px should equal line row minus centre row");
    require(std::abs(metrics.center_offset_fraction -
                     expected_offset / (0.5 * static_cast<double>(kRows))) <= 1e-6,
            "horizontal: center_offset_fraction should be offset over half height");
}

void test_vertical_boundary_detected_at_known_column()
{
    constexpr int kRows = 480;
    constexpr int kCols = 640;
    constexpr int kBoundaryCol = 200;
    cv::Mat gray(kRows, kCols, CV_8UC1, cv::Scalar(40));
    gray.colRange(kBoundaryCol, kCols).setTo(cv::Scalar(200));

    const RulerAlignmentMetrics metrics =
        detect_ruler_alignment(gray, RulerAlignmentOrientation::kVertical);

    require(metrics.has_detected_line, "vertical: boundary should be detected");
    require(std::abs(std::abs(metrics.line_angle_deg) - 90.0) <= 2.0,
            "vertical: line angle should be ~+/-90 deg, got " +
                std::to_string(metrics.line_angle_deg));
    require(metrics.angle_error_deg <= 2.0,
            "vertical: angle error should be ~0 deg, got " +
                std::to_string(metrics.angle_error_deg));

    const double line_col = 0.5 * (static_cast<double>(metrics.x0) + static_cast<double>(metrics.x1));
    require(std::abs(line_col - static_cast<double>(kBoundaryCol)) <= 5.0,
            "vertical: boundary column should be ~200, got " + std::to_string(line_col));

    const double expected_offset = line_col - 0.5 * static_cast<double>(kCols);
    require(std::abs(metrics.center_offset_px - expected_offset) <= 1e-6,
            "vertical: center_offset_px should equal line column minus centre column");
}

void test_empty_image_reports_no_detection()
{
    const cv::Mat empty;
    const RulerAlignmentMetrics metrics =
        detect_ruler_alignment(empty, RulerAlignmentOrientation::kHorizontal);
    require(!metrics.has_detected_line, "empty image: has_detected_line should be false");
    require(metrics.line_angle_deg == 0.0 && metrics.angle_error_deg == 0.0 &&
                metrics.center_offset_px == 0.0 && metrics.center_offset_fraction == 0.0 &&
                metrics.x0 == 0 && metrics.y0 == 0 && metrics.x1 == 0 && metrics.y1 == 0,
            "empty image: metrics should stay default-initialized");
}

void test_uniform_image_falls_back_to_margin_anchor()
{
    // A uniform image of normal size does not report failure (see contract
    // note above): the tie-at-zero gradient profile makes the anchor land on
    // the first scanned index (== margin == max(4, rows / 40) == 12 here) and
    // the fallback reports a synthetic full-width horizontal line there.
    constexpr int kRows = 480;
    constexpr int kCols = 640;
    cv::Mat gray(kRows, kCols, CV_8UC1, cv::Scalar(128));

    const RulerAlignmentMetrics metrics =
        detect_ruler_alignment(gray, RulerAlignmentOrientation::kHorizontal);

    require(metrics.has_detected_line,
            "uniform image: fallback should still report a synthetic line");
    require(metrics.line_angle_deg == 0.0 && metrics.angle_error_deg == 0.0,
            "uniform image: fallback line should be exactly axis-aligned");
    constexpr int kExpectedMargin = 12;  // max(4, 480 / 40)
    require(metrics.y0 == kExpectedMargin && metrics.y1 == kExpectedMargin,
            "uniform image: fallback line should sit at the profile margin row, got y0=" +
                std::to_string(metrics.y0));
    require(metrics.x0 == 0 && metrics.x1 == kCols - 1,
            "uniform image: fallback line should span the full width");
}

void test_too_thin_uniform_image_reports_no_detection()
{
    // With only 8 rows the profile margin (max(4, 8 / 40) == 4) leaves no
    // scannable interior, so no anchor is found; a uniform strip also yields
    // no Canny edges, which is the genuine no-detection path.
    cv::Mat gray(8, 640, CV_8UC1, cv::Scalar(128));
    const RulerAlignmentMetrics metrics =
        detect_ruler_alignment(gray, RulerAlignmentOrientation::kHorizontal);
    require(!metrics.has_detected_line,
            "thin uniform strip: has_detected_line should be false");
}

void test_orientation_labels()
{
    require(std::string(ruler_alignment_orientation_label(
                RulerAlignmentOrientation::kHorizontal)) == "horizontal",
            "label for kHorizontal should be \"horizontal\"");
    require(std::string(ruler_alignment_orientation_label(
                RulerAlignmentOrientation::kVertical)) == "vertical",
            "label for kVertical should be \"vertical\"");
}

} // namespace

int main()
{
    try {
        test_horizontal_boundary_detected_at_known_row();
        test_vertical_boundary_detected_at_known_column();
        test_empty_image_reports_no_detection();
        test_uniform_image_falls_back_to_margin_anchor();
        test_too_thin_uniform_image_reports_no_detection();
        test_orientation_labels();
    } catch (const std::exception& ex) {
        std::cerr << "ruler_alignment_tests failed: " << ex.what() << std::endl;
        return 1;
    }
    std::cout << "ruler_alignment_tests passed" << std::endl;
    return 0;
}
