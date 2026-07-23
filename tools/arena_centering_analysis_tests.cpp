#include "gui/arena_centering_analysis.h"

#include <opencv2/imgproc.hpp>

#include <algorithm>
#include <cmath>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace ac = orange::gui::arena_centering;

namespace {

void Require(bool condition, const std::string& message)
{
    if (!condition) {
        throw std::runtime_error(message);
    }
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

std::vector<unsigned char> MakeRingFrame(int width, int height,
                                         double center_x, double center_y,
                                         int radius)
{
    cv::Mat rgba(height, width, CV_8UC4, cv::Scalar(5, 5, 5, 255));
    const cv::Point center(
        static_cast<int>(std::lround(center_x)),
        static_cast<int>(std::lround(center_y)));
    cv::circle(rgba, center, radius, cv::Scalar(180, 180, 180, 255), 5, cv::LINE_AA);
    cv::circle(rgba, center, 3, cv::Scalar(180, 180, 180, 255), cv::FILLED, cv::LINE_AA);
    return std::vector<unsigned char>(
        rgba.data, rgba.data + rgba.total() * rgba.elemSize());
}

std::vector<unsigned char> MakeFilledDiskFrame(int width, int height,
                                               double center_x, double center_y,
                                               int radius)
{
    cv::Mat rgba(height, width, CV_8UC4, cv::Scalar(28, 28, 28, 255));
    cv::circle(
        rgba,
        {static_cast<int>(std::lround(center_x)),
         static_cast<int>(std::lround(center_y))},
        radius,
        cv::Scalar(220, 220, 220, 255),
        cv::FILLED,
        cv::LINE_AA);
    return std::vector<unsigned char>(
        rgba.data, rgba.data + rgba.total() * rgba.elemSize());
}

std::vector<unsigned char> MakeGhostedEllipseFrame(int width, int height,
                                                   double center_x,
                                                   double center_y)
{
    cv::Mat rgba(height, width, CV_8UC4, cv::Scalar(28, 28, 28, 255));
    cv::ellipse(
        rgba,
        {static_cast<int>(std::lround(center_x)),
         static_cast<int>(std::lround(center_y))},
        {36, 24},
        0.0,
        0.0,
        360.0,
        cv::Scalar(220, 220, 220, 255),
        cv::FILLED,
        cv::LINE_AA);
    return std::vector<unsigned char>(
        rgba.data, rgba.data + rgba.total() * rgba.elemSize());
}

std::vector<unsigned char> MakeRectangleFrame(
    int width,
    int height,
    const std::vector<cv::Point>& corners,
    cv::Point center)
{
    cv::Mat rgba(height, width, CV_8UC4, cv::Scalar(24, 24, 24, 255));
    cv::polylines(
        rgba,
        corners,
        true,
        cv::Scalar(190, 190, 190, 255),
        7,
        cv::LINE_AA);
    cv::circle(
        rgba,
        center,
        25,
        cv::Scalar(190, 190, 190, 255),
        5,
        cv::LINE_AA);
    cv::circle(
        rgba,
        center,
        3,
        cv::Scalar(190, 190, 190, 255),
        cv::FILLED,
        cv::LINE_AA);
    return std::vector<unsigned char>(
        rgba.data, rgba.data + rgba.total() * rgba.elemSize());
}

void TestConcurrentDetectorOwnsOneTaskPerCamera()
{
    constexpr int width = 512;
    constexpr int height = 512;
    const std::vector<ac::Point2d> centers = {
        {251.0, 253.0}, {259.0, 248.0}, {255.0, 261.0}, {246.0, 257.0}};
    std::vector<std::vector<unsigned char>> pixels;
    std::vector<ac::RgbaFrameView> views;
    pixels.reserve(centers.size());
    views.reserve(centers.size());
    for (std::size_t index = 0; index < centers.size(); ++index) {
        pixels.push_back(index % 2 == 0
            ? MakeRingFrame(width, height, centers[index].x, centers[index].y, 28)
            : MakeFilledDiskFrame(
                  width, height, centers[index].x, centers[index].y, 28));
        views.push_back({
            "201009" + std::to_string(index + 3),
            width,
            height,
            &pixels.back(),
        });
    }

    const ac::ConcurrentDetectionBatch batch =
        ac::DetectArenaCenterFiducialsConcurrently(views);
    Require(batch.task_count == 4, "one task should be created per camera");
    Require(batch.maximum_concurrent_tasks == 4,
            "all four camera analyses should cross the common start barrier");
    Require(batch.detections.size() == centers.size(),
            "one result should be joined per camera");
    for (std::size_t index = 0; index < centers.size(); ++index) {
        const auto& detection = batch.detections[index];
        Require(detection.ok, "synthetic center fiducial should be detected");
        RequireNear(detection.center_camera_px.x, centers[index].x, 2.5,
                    "detected center x should be sub-ring accurate");
        RequireNear(detection.center_camera_px.y, centers[index].y, 2.5,
                    "detected center y should be sub-ring accurate");
        Require(detection.overlay_rgba.size() == pixels[index].size(),
                "overlay should preserve native frame shape");
        if (index % 2 != 0) {
            Require(detection.filled_disk_contrast_u8 > 100.0,
                    "filled Citrus center marker should use strong disk contrast");
        }
    }
}

void TestGhostedElongatedMarkerIsRejected()
{
    constexpr int width = 512;
    constexpr int height = 512;
    const auto pixels = MakeGhostedEllipseFrame(width, height, 256.0, 256.0);
    const ac::FiducialDetection detection = ac::DetectArenaCenterFiducial({
        "2010093", width, height, &pixels});
    Require(!detection.ok,
            "an elongated mixed-stage marker must not reach the solver: " +
                detection.ToJson().dump());
}

void TestDetectorCanTargetDailyRimCenterAwayFromSensorCenter()
{
    constexpr int width = 512;
    constexpr int height = 512;
    constexpr double center_x = 352.0;
    constexpr double center_y = 178.0;
    const auto pixels = MakeRingFrame(
        width, height, center_x, center_y, 28);
    ac::FiducialDetectorConfig config;
    config.has_expected_center_camera_px = true;
    config.expected_center_camera_px = {center_x, center_y};
    config.search_half_extent_fraction = 0.10;
    config.max_center_distance_fraction = 0.05;
    const ac::FiducialDetection detection =
        ac::DetectArenaCenterFiducial(
            {"2010093", width, height, &pixels}, config);
    Require(detection.ok,
            "daily marker should be found around the independent rim center: " +
                detection.ToJson().dump());
    RequireNear(detection.center_camera_px.x, center_x, 2.5,
                "daily marker x should follow the configured target");
    RequireNear(detection.center_camera_px.y, center_y, 2.5,
                "daily marker y should follow the configured target");
    RequireNear(detection.center_error_x_camera_px, 0.0, 2.5,
                "daily marker residual x should be rim-relative");
    RequireNear(detection.center_error_y_camera_px, 0.0, 2.5,
                "daily marker residual y should be rim-relative");
}

void TestRectangleBoundaryAndMaximalSquareProposal()
{
    constexpr int width = 512;
    constexpr int height = 512;
    const std::vector<cv::Point> expected = {
        {42, 48}, {470, 44}, {474, 465}, {38, 469}};
    const auto pixels = MakeRectangleFrame(
        width, height, expected, {256, 256});
    const ac::RectangleBoundaryDetection rectangle =
        ac::DetectArenaRectangleBoundary({"2010093", width, height, &pixels});
    Require(rectangle.ok,
            "complete synthetic rectangle should be detected: " +
                rectangle.ToJson().dump());
    Require(rectangle.fully_visible_with_margin,
            "synthetic rectangle should clear the default safety margin");
    RequireNear(rectangle.top_left_camera_px.x, 42.0, 6.0,
                "top-left x should follow projected outline centerline");
    RequireNear(rectangle.top_left_camera_px.y, 48.0, 6.0,
                "top-left y should follow projected outline centerline");
    RequireNear(rectangle.diagonal_intersection_camera_px.x, 256.0, 4.0,
                "diagonal intersection should recover projective center x");
    RequireNear(rectangle.diagonal_intersection_camera_px.y, 256.0, 4.0,
                "diagonal intersection should recover projective center y");

    ac::MaximalSquareProposalConfig proposal_config;
    proposal_config.safety_margin_camera_px = 24.0;
    const ac::MaximalSquareProposal proposal =
        ac::ProposeMaximalVisibleArenaSquare(
            rectangle,
            width,
            height,
            800,
            800,
            400,
            400,
            300,
            300,
            proposal_config);
    Require(proposal.ok,
            "well-supported rectangle should produce a square proposal: " +
                proposal.ToJson().dump());
    Require(proposal.proposed_width_canvas_px % 2 == 0,
            "Citrus placement proposal must use an even side");
    Require(proposal.proposed_width_canvas_px > 300,
            "available sensor margin should permit a larger square");
    Require(proposal.proposed_width_canvas_px ==
                proposal.proposed_height_canvas_px,
            "commissioning proposal must be square");
    Require(std::min({proposal.predicted_left_margin_camera_px,
                      proposal.predicted_top_margin_camera_px,
                      proposal.predicted_right_margin_camera_px,
                      proposal.predicted_bottom_margin_camera_px}) >= 23.9,
            "proposal must retain the configured camera safety margin");
}

void TestRectangleVisibilityMarginRejectsNearClipping()
{
    constexpr int width = 512;
    constexpr int height = 512;
    const std::vector<cv::Point> corners = {
        {10, 12}, {501, 12}, {501, 500}, {10, 500}};
    const auto pixels = MakeRectangleFrame(
        width, height, corners, {256, 256});
    const ac::RectangleBoundaryDetection rectangle =
        ac::DetectArenaRectangleBoundary({"2010093", width, height, &pixels});
    Require(rectangle.ok,
            "near-edge rectangle should remain measurable for diagnostics");
    Require(!rectangle.fully_visible_with_margin,
            "near-edge rectangle must fail the explicit safety margin gate");
}

ac::Point2d Project(ac::Point2d baseline_camera,
                    ac::Point2d baseline_canvas,
                    ac::Point2d canvas)
{
    const double dx = canvas.x - baseline_canvas.x;
    const double dy = canvas.y - baseline_canvas.y;
    return {
        baseline_camera.x + 10.0 * dx + 1.0 * dy,
        baseline_camera.y + 0.5 * dx + 8.0 * dy};
}

void TestSymmetricProbeSolver()
{
    const ac::Point2d baseline_canvas{100.0, 200.0};
    const ac::Point2d baseline_camera{260.0, 240.0};
    const ac::Point2d plus_x{103.0, 200.0};
    const ac::Point2d minus_x{97.0, 200.0};
    const ac::Point2d plus_y{100.0, 203.0};
    const ac::Point2d minus_y{100.0, 197.0};
    ac::SymmetricProbeObservations observations;
    observations.baseline = {baseline_camera, baseline_canvas};
    observations.plus_x = {Project(baseline_camera, baseline_canvas, plus_x), plus_x};
    observations.minus_x = {Project(baseline_camera, baseline_canvas, minus_x), minus_x};
    observations.plus_y = {Project(baseline_camera, baseline_canvas, plus_y), plus_y};
    observations.minus_y = {Project(baseline_camera, baseline_canvas, minus_y), minus_y};

    const ac::Point2d target{255.5, 255.5};
    const ac::CenteringSolveResult solved =
        ac::SolveSymmetricArenaCentering(observations, target);
    Require(solved.ok, "well-conditioned symmetric probes should solve");
    RequireNear(solved.jacobian_camera_px_per_canvas_px[0][0], 10.0, 1e-9,
                "J00 should match synthetic transform");
    RequireNear(solved.jacobian_camera_px_per_canvas_px[0][1], 1.0, 1e-9,
                "J01 should match synthetic transform");
    RequireNear(solved.jacobian_camera_px_per_canvas_px[1][0], 0.5, 1e-9,
                "J10 should match synthetic transform");
    RequireNear(solved.jacobian_camera_px_per_canvas_px[1][1], 8.0, 1e-9,
                "J11 should match synthetic transform");
    const ac::Point2d reprojection = Project(
        baseline_camera,
        baseline_canvas,
        solved.candidate_canvas_px);
    RequireNear(reprojection.x, target.x, 1e-9,
                "candidate should map to target x before integer quantization");
    RequireNear(reprojection.y, target.y, 1e-9,
                "candidate should map to target y before integer quantization");
}

void TestSymmetricProbeNonlinearityIsRejected()
{
    ac::SymmetricProbeObservations observations;
    observations.baseline = {{100.0, 100.0}, {50.0, 50.0}};
    observations.plus_x = {{125.0, 100.0}, {52.0, 50.0}};
    observations.minus_x = {{85.0, 100.0}, {48.0, 50.0}};
    observations.plus_y = {{100.0, 120.0}, {50.0, 52.0}};
    observations.minus_y = {{100.0, 80.0}, {50.0, 48.0}};
    const ac::CenteringSolveResult solved =
        ac::SolveSymmetricArenaCentering(observations, {100.0, 100.0});
    Require(!solved.ok, "asymmetric midpoint should fail the quality gate");
    Require(solved.error == "symmetric_probe_nonlinearity_rejected",
            "nonlinearity should have a stable error code");
}

}  // namespace

int main()
{
    try {
        TestConcurrentDetectorOwnsOneTaskPerCamera();
        TestGhostedElongatedMarkerIsRejected();
        TestDetectorCanTargetDailyRimCenterAwayFromSensorCenter();
        TestRectangleBoundaryAndMaximalSquareProposal();
        TestRectangleVisibilityMarginRejectsNearClipping();
        TestSymmetricProbeSolver();
        TestSymmetricProbeNonlinearityIsRejected();
    } catch (const std::exception& error) {
        std::cerr << "arena_centering_analysis_tests failed: "
                  << error.what() << std::endl;
        return 1;
    }
    std::cout << "arena_centering_analysis_tests passed" << std::endl;
    return 0;
}
