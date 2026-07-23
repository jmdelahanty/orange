#pragma once

#include "json.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace orange::gui::arena_centering {

struct Point2d {
    double x = 0.0;
    double y = 0.0;
};

struct RgbaFrameView {
    std::string camera_serial;
    int width = 0;
    int height = 0;
    const std::vector<unsigned char>* rgba = nullptr;
};

struct FiducialDetectorConfig {
    // Arena commissioning normally searches around the camera raster center.
    // Daily dish registration instead searches around the independently
    // measured physical-rim center. Keeping that target explicit prevents a
    // slightly displaced dish from being rejected merely because it is not at
    // the sensor center.
    bool has_expected_center_camera_px = false;
    Point2d expected_center_camera_px;
    double search_half_extent_fraction = 0.18;
    double min_radius_fraction = 0.002;
    double max_radius_fraction = 0.06;
    // A stable ring/disk is close to circular in this perpendicular camera
    // layout. Larger elongation is characteristic of a mixed old/new
    // projection exposure and must never be accepted by the solver.
    double max_axis_ratio = 1.20;
    double min_ring_contrast_u8 = 4.0;
    double min_filled_disk_contrast_u8 = 8.0;
    double min_center_dot_contrast_u8 = 1.0;
    double max_center_distance_fraction = 0.16;
};

struct FiducialDetection {
    bool ok = false;
    std::string camera_serial;
    std::string error;
    int sensor_width_px = 0;
    int sensor_height_px = 0;
    Point2d sensor_raster_center_camera_px;
    Point2d center_camera_px;
    double radius_camera_px = 0.0;
    double ellipse_major_axis_px = 0.0;
    double ellipse_minor_axis_px = 0.0;
    double axis_ratio = 0.0;
    double ring_contrast_u8 = 0.0;
    double filled_disk_contrast_u8 = 0.0;
    double center_dot_contrast_u8 = 0.0;
    double center_error_x_camera_px = 0.0;
    double center_error_y_camera_px = 0.0;
    double score = 0.0;
    std::string strategy;
    std::uint64_t worker_thread_id_hash = 0;
    std::vector<unsigned char> overlay_rgba;

    nlohmann::json ToJson() const;
};

struct ConcurrentDetectionBatch {
    std::vector<FiducialDetection> detections;
    std::size_t task_count = 0;
    std::size_t maximum_concurrent_tasks = 0;
    std::string execution_policy =
        "one_std_launch_async_task_per_owned_camera_frame_then_join";

    nlohmann::json ToJson() const;
};

FiducialDetection DetectArenaCenterFiducial(
    const RgbaFrameView& frame,
    const FiducialDetectorConfig& config = {});

ConcurrentDetectionBatch DetectArenaCenterFiducialsConcurrently(
    const std::vector<RgbaFrameView>& frames,
    const FiducialDetectorConfig& config = {});

struct RectangleBoundaryDetectorConfig {
    int downsample_max_dimension_px = 1280;
    double canny_low_threshold_u8 = 10.0;
    double canny_high_threshold_u8 = 35.0;
    double hough_min_line_length_fraction = 0.45;
    double hough_max_line_gap_fraction = 0.10;
    double maximum_axis_angle_degrees = 4.0;
    double outer_search_fraction = 0.30;
    double maximum_edge_band_spread_fraction = 0.05;
    double minimum_visible_margin_camera_px = 32.0;
};

struct RectangleBoundaryDetection {
    bool ok = false;
    bool fully_visible_with_margin = false;
    std::string camera_serial;
    std::string error;
    int sensor_width_px = 0;
    int sensor_height_px = 0;
    Point2d top_left_camera_px;
    Point2d top_right_camera_px;
    Point2d bottom_right_camera_px;
    Point2d bottom_left_camera_px;
    Point2d diagonal_intersection_camera_px;
    double left_margin_camera_px = 0.0;
    double top_margin_camera_px = 0.0;
    double right_margin_camera_px = 0.0;
    double bottom_margin_camera_px = 0.0;
    double minimum_margin_camera_px = 0.0;
    double required_minimum_margin_camera_px = 0.0;
    double top_edge_band_spread_camera_px = 0.0;
    double right_edge_band_spread_camera_px = 0.0;
    double bottom_edge_band_spread_camera_px = 0.0;
    double left_edge_band_spread_camera_px = 0.0;
    int top_edge_candidate_count = 0;
    int right_edge_candidate_count = 0;
    int bottom_edge_candidate_count = 0;
    int left_edge_candidate_count = 0;
    double quadrilateral_area_fraction = 0.0;
    std::uint64_t worker_thread_id_hash = 0;
    std::vector<unsigned char> overlay_rgba;

    nlohmann::json ToJson() const;
};

struct ConcurrentRectangleDetectionBatch {
    std::vector<RectangleBoundaryDetection> detections;
    std::size_t task_count = 0;
    std::size_t maximum_concurrent_tasks = 0;
    std::string execution_policy =
        "one_std_launch_async_rectangle_task_per_owned_camera_frame_then_join";

    nlohmann::json ToJson() const;
};

RectangleBoundaryDetection DetectArenaRectangleBoundary(
    const RgbaFrameView& frame,
    const RectangleBoundaryDetectorConfig& config = {});

ConcurrentRectangleDetectionBatch DetectArenaRectangleBoundariesConcurrently(
    const std::vector<RgbaFrameView>& frames,
    const RectangleBoundaryDetectorConfig& config = {});

struct MaximalSquareProposalConfig {
    double safety_margin_camera_px = 32.0;
    int minimum_side_canvas_px = 32;
    double maximum_scale_change_fraction = 0.20;
};

struct MaximalSquareProposal {
    bool ok = false;
    std::string error;
    int center_x_canvas_px = 0;
    int center_y_canvas_px = 0;
    int source_width_canvas_px = 0;
    int source_height_canvas_px = 0;
    int proposed_width_canvas_px = 0;
    int proposed_height_canvas_px = 0;
    double scale_relative_to_source_width = 0.0;
    double scale_relative_to_source_height = 0.0;
    double required_safety_margin_camera_px = 0.0;
    double predicted_left_margin_camera_px = 0.0;
    double predicted_top_margin_camera_px = 0.0;
    double predicted_right_margin_camera_px = 0.0;
    double predicted_bottom_margin_camera_px = 0.0;
    std::vector<Point2d> predicted_corners_camera_px;

    nlohmann::json ToJson() const;
};

MaximalSquareProposal ProposeMaximalVisibleArenaSquare(
    const RectangleBoundaryDetection& detected_source_rectangle,
    int sensor_width_px,
    int sensor_height_px,
    int canvas_width_px,
    int canvas_height_px,
    int center_x_canvas_px,
    int center_y_canvas_px,
    int source_width_canvas_px,
    int source_height_canvas_px,
    const MaximalSquareProposalConfig& config = {});

struct ProbeObservation {
    Point2d camera_center_px;
    Point2d canvas_center_px;
};

struct SymmetricProbeObservations {
    ProbeObservation baseline;
    ProbeObservation plus_x;
    ProbeObservation minus_x;
    ProbeObservation plus_y;
    ProbeObservation minus_y;
};

struct SolverConfig {
    double min_probe_displacement_camera_px = 5.0;
    double max_symmetric_midpoint_error_camera_px = 3.0;
    double max_condition_number = 30.0;
    double min_abs_determinant = 1e-4;
    double max_candidate_move_canvas_px = 64.0;
    bool require_positive_determinant = true;
};

struct CenteringSolveResult {
    bool ok = false;
    std::string error;
    Point2d target_camera_px;
    Point2d baseline_error_camera_px;
    Point2d delta_canvas_px;
    Point2d candidate_canvas_px;
    Point2d predicted_integer_quantization_residual_camera_px;
    double predicted_integer_quantization_residual_norm_camera_px = 0.0;
    int candidate_center_x_canvas_px = 0;
    int candidate_center_y_canvas_px = 0;
    double jacobian_camera_px_per_canvas_px[2][2]{{0.0, 0.0}, {0.0, 0.0}};
    double determinant = 0.0;
    double condition_number = 0.0;
    double plus_minus_x_displacement_camera_px = 0.0;
    double plus_minus_y_displacement_camera_px = 0.0;
    double symmetric_midpoint_x_error_camera_px = 0.0;
    double symmetric_midpoint_y_error_camera_px = 0.0;

    nlohmann::json ToJson() const;
};

CenteringSolveResult SolveSymmetricArenaCentering(
    const SymmetricProbeObservations& observations,
    Point2d target_camera_px,
    const SolverConfig& config = {});

Point2d ApplyJacobianCorrection(
    const CenteringSolveResult& solved,
    Point2d camera_error_px);

}  // namespace orange::gui::arena_centering
