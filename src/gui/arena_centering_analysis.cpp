#include "gui/arena_centering_analysis.h"

#include <opencv2/imgproc.hpp>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <future>
#include <limits>
#include <mutex>
#include <numeric>
#include <thread>

namespace orange::gui::arena_centering {
namespace {

double Norm(Point2d value)
{
    return std::hypot(value.x, value.y);
}

Point2d Add(Point2d a, Point2d b)
{
    return {a.x + b.x, a.y + b.y};
}

Point2d Subtract(Point2d a, Point2d b)
{
    return {a.x - b.x, a.y - b.y};
}

Point2d Scale(Point2d value, double scale)
{
    return {value.x * scale, value.y * scale};
}

double MeanInAnnulus(const cv::Mat& gray,
                     cv::Point2d center,
                     double inner_radius,
                     double outer_radius)
{
    const int x0 = std::max(0, static_cast<int>(std::floor(center.x - outer_radius)));
    const int x1 = std::min(gray.cols - 1,
                            static_cast<int>(std::ceil(center.x + outer_radius)));
    const int y0 = std::max(0, static_cast<int>(std::floor(center.y - outer_radius)));
    const int y1 = std::min(gray.rows - 1,
                            static_cast<int>(std::ceil(center.y + outer_radius)));
    double sum = 0.0;
    std::size_t count = 0;
    const double inner2 = inner_radius * inner_radius;
    const double outer2 = outer_radius * outer_radius;
    for (int y = y0; y <= y1; ++y) {
        const auto* row = gray.ptr<unsigned char>(y);
        for (int x = x0; x <= x1; ++x) {
            const double dx = static_cast<double>(x) - center.x;
            const double dy = static_cast<double>(y) - center.y;
            const double distance2 = dx * dx + dy * dy;
            if (distance2 >= inner2 && distance2 <= outer2) {
                sum += row[x];
                ++count;
            }
        }
    }
    return count == 0 ? 0.0 : sum / static_cast<double>(count);
}

struct Candidate {
    cv::Point2d center;
    double radius = 0.0;
    double major_axis = 0.0;
    double minor_axis = 0.0;
    double axis_ratio = std::numeric_limits<double>::infinity();
    double ring_contrast = 0.0;
    double filled_disk_contrast = 0.0;
    double dot_contrast = 0.0;
    double score = -std::numeric_limits<double>::infinity();
    std::string strategy;
};

Candidate ScoreCandidate(const cv::Mat& gray,
                         cv::Point2d center,
                         double radius,
                         double major_axis,
                         double minor_axis,
                         const cv::Point2d& target,
                         const FiducialDetectorConfig& config,
                         const std::string& strategy)
{
    Candidate candidate;
    candidate.center = center;
    candidate.radius = radius;
    candidate.major_axis = major_axis;
    candidate.minor_axis = minor_axis;
    candidate.axis_ratio = minor_axis > 0.0
        ? major_axis / minor_axis
        : std::numeric_limits<double>::infinity();
    const double ring = MeanInAnnulus(
        gray, center, radius * 0.76, radius * 1.24);
    const double adjacent = 0.5 * (
        MeanInAnnulus(gray, center, radius * 0.42, radius * 0.68) +
        MeanInAnnulus(gray, center, radius * 1.34, radius * 1.70));
    const double center_dot = MeanInAnnulus(gray, center, 0.0, radius * 0.22);
    const double inner_background = MeanInAnnulus(
        gray, center, radius * 0.32, radius * 0.62);
    candidate.ring_contrast = ring - adjacent;
    candidate.dot_contrast = center_dot - inner_background;
    const double filled_disk = MeanInAnnulus(gray, center, 0.0, radius * 0.68);
    const double filled_background = MeanInAnnulus(
        gray, center, radius * 1.24, radius * 1.72);
    candidate.filled_disk_contrast = filled_disk - filled_background;
    const double center_distance = cv::norm(center - target);
    const double feature_contrast = std::max(
        candidate.ring_contrast, candidate.filled_disk_contrast);
    candidate.score = feature_contrast +
        0.35 * candidate.dot_contrast -
        0.025 * center_distance -
        15.0 * std::max(0.0, candidate.axis_ratio - 1.0);
    if (strategy == "threshold_ellipse") {
        // Prefer a fitted boundary over the quantized Hough accumulator when
        // both explain the same high-contrast disk or ring.
        candidate.score += 20.0;
    }
    candidate.strategy = strategy;
    const bool ring_feature_ok =
        candidate.ring_contrast >= config.min_ring_contrast_u8 &&
        candidate.dot_contrast >= config.min_center_dot_contrast_u8;
    const bool filled_disk_feature_ok =
        candidate.filled_disk_contrast >= config.min_filled_disk_contrast_u8;
    if (candidate.axis_ratio > config.max_axis_ratio ||
        (!ring_feature_ok && !filled_disk_feature_ok)) {
        candidate.score = -std::numeric_limits<double>::infinity();
    }
    return candidate;
}

void ConsiderCandidate(const Candidate& candidate, Candidate* best)
{
    if (best != nullptr && candidate.score > best->score) {
        *best = candidate;
    }
}

std::vector<unsigned char> MakeOverlay(const cv::Mat& rgba,
                                       const Candidate* candidate,
                                       cv::Point2d target)
{
    cv::Mat overlay_bgr;
    cv::cvtColor(rgba, overlay_bgr, cv::COLOR_RGBA2BGR);
    const cv::Point target_point(
        static_cast<int>(std::lround(target.x)),
        static_cast<int>(std::lround(target.y)));
    cv::drawMarker(
        overlay_bgr,
        target_point,
        cv::Scalar(0, 255, 255),
        cv::MARKER_CROSS,
        41,
        3,
        cv::LINE_AA);
    if (candidate != nullptr && std::isfinite(candidate->score)) {
        const cv::Point detected(
            static_cast<int>(std::lround(candidate->center.x)),
            static_cast<int>(std::lround(candidate->center.y)));
        cv::ellipse(
            overlay_bgr,
            detected,
            cv::Size(
                static_cast<int>(std::lround(candidate->major_axis * 0.5)),
                static_cast<int>(std::lround(candidate->minor_axis * 0.5))),
            0.0,
            0.0,
            360.0,
            cv::Scalar(0, 255, 0),
            3,
            cv::LINE_AA);
        cv::line(
            overlay_bgr,
            target_point,
            detected,
            cv::Scalar(255, 180, 0),
            2,
            cv::LINE_AA);
        cv::drawMarker(
            overlay_bgr,
            detected,
            cv::Scalar(0, 0, 255),
            cv::MARKER_TILTED_CROSS,
            31,
            3,
            cv::LINE_AA);
    }
    cv::Mat overlay_rgba;
    cv::cvtColor(overlay_bgr, overlay_rgba, cv::COLOR_BGR2RGBA);
    return std::vector<unsigned char>(
        overlay_rgba.data,
        overlay_rgba.data + overlay_rgba.total() * overlay_rgba.elemSize());
}

double ConditionNumber2x2(double a, double b, double c, double d)
{
    const double trace = a * a + b * b + c * c + d * d;
    const double determinant_squared = (a * d - b * c) * (a * d - b * c);
    const double discriminant = std::max(0.0, trace * trace - 4.0 * determinant_squared);
    const double lambda_max = 0.5 * (trace + std::sqrt(discriminant));
    const double lambda_min = 0.5 * (trace - std::sqrt(discriminant));
    if (lambda_min <= std::numeric_limits<double>::epsilon()) {
        return std::numeric_limits<double>::infinity();
    }
    return std::sqrt(lambda_max / lambda_min);
}

struct DetectedLine {
    // Normalized only by construction convention, not Euclidean length.
    // Horizontal lines use y = slope*x + intercept; vertical lines use
    // x = slope*y + intercept.
    double slope = 0.0;
    double center_intercept = 0.0;
    double length = 0.0;
};

struct RepresentativeLine {
    bool ok = false;
    bool horizontal = false;
    double a = 0.0;
    double b = 0.0;
    double c = 0.0;
    double band_spread = 0.0;
    int candidate_count = 0;
};

double Median(std::vector<double> values)
{
    if (values.empty()) return std::numeric_limits<double>::quiet_NaN();
    std::sort(values.begin(), values.end());
    const std::size_t middle = values.size() / 2;
    if (values.size() % 2 != 0) return values[middle];
    return 0.5 * (values[middle - 1] + values[middle]);
}

RepresentativeLine MakeRepresentativeLine(
    const std::vector<DetectedLine>& candidates,
    bool horizontal,
    double image_center_coordinate,
    double maximum_spread)
{
    RepresentativeLine result;
    result.horizontal = horizontal;
    result.candidate_count = static_cast<int>(candidates.size());
    if (candidates.empty()) return result;
    std::vector<double> slopes;
    std::vector<double> positions;
    slopes.reserve(candidates.size());
    positions.reserve(candidates.size());
    for (const auto& candidate : candidates) {
        slopes.push_back(candidate.slope);
        positions.push_back(candidate.center_intercept);
    }
    const auto [minimum, maximum] = std::minmax_element(
        positions.begin(), positions.end());
    result.band_spread = *maximum - *minimum;
    if (!std::isfinite(result.band_spread) ||
        result.band_spread > maximum_spread) {
        return result;
    }
    const double slope = Median(std::move(slopes));
    // The projected outline is a luminous band. Hough normally observes both
    // band edges; using the midpoint of the outermost mutually-consistent
    // candidates estimates the rendered centerline without favoring whichever
    // edge happened to receive more accumulator votes.
    const double center_value = 0.5 * (*minimum + *maximum);
    const double intercept = center_value - slope * image_center_coordinate;
    if (horizontal) {
        result.a = -slope;
        result.b = 1.0;
        result.c = -intercept;
    } else {
        result.a = 1.0;
        result.b = -slope;
        result.c = -intercept;
    }
    result.ok = true;
    return result;
}

bool IntersectLines(const RepresentativeLine& first,
                    const RepresentativeLine& second,
                    Point2d* intersection)
{
    const double determinant = first.a * second.b - second.a * first.b;
    if (intersection == nullptr || std::abs(determinant) < 1e-9) return false;
    intersection->x =
        (first.b * second.c - second.b * first.c) / determinant;
    intersection->y =
        (first.c * second.a - second.c * first.a) / determinant;
    return std::isfinite(intersection->x) && std::isfinite(intersection->y);
}

double PolygonArea(const std::vector<Point2d>& points)
{
    double twice_area = 0.0;
    for (std::size_t index = 0; index < points.size(); ++index) {
        const Point2d& current = points[index];
        const Point2d& next = points[(index + 1) % points.size()];
        twice_area += current.x * next.y - next.x * current.y;
    }
    return 0.5 * std::abs(twice_area);
}

Point2d DiagonalIntersection(const std::vector<Point2d>& corners)
{
    if (corners.size() != 4) {
        return {std::numeric_limits<double>::quiet_NaN(),
                std::numeric_limits<double>::quiet_NaN()};
    }
    const Point2d p = corners[0];
    const Point2d r = Subtract(corners[2], corners[0]);
    const Point2d q = corners[1];
    const Point2d s = Subtract(corners[3], corners[1]);
    const double cross_rs = r.x * s.y - r.y * s.x;
    if (std::abs(cross_rs) < 1e-9) {
        return {std::numeric_limits<double>::quiet_NaN(),
                std::numeric_limits<double>::quiet_NaN()};
    }
    const Point2d qp = Subtract(q, p);
    const double t = (qp.x * s.y - qp.y * s.x) / cross_rs;
    return Add(p, Scale(r, t));
}

std::vector<unsigned char> MakeRectangleOverlay(
    const cv::Mat& rgba,
    const RectangleBoundaryDetection* detection)
{
    cv::Mat overlay_bgr;
    cv::cvtColor(rgba, overlay_bgr, cv::COLOR_RGBA2BGR);
    if (detection != nullptr && detection->ok) {
        const std::vector<Point2d> values = {
            detection->top_left_camera_px,
            detection->top_right_camera_px,
            detection->bottom_right_camera_px,
            detection->bottom_left_camera_px};
        std::vector<cv::Point> polygon;
        for (const Point2d value : values) {
            polygon.emplace_back(
                static_cast<int>(std::lround(value.x)),
                static_cast<int>(std::lround(value.y)));
        }
        const cv::Scalar color = detection->fully_visible_with_margin
            ? cv::Scalar(0, 255, 0)
            : cv::Scalar(0, 165, 255);
        cv::polylines(overlay_bgr, polygon, true, color, 5, cv::LINE_AA);
        cv::drawMarker(
            overlay_bgr,
            {static_cast<int>(std::lround(
                 detection->diagonal_intersection_camera_px.x)),
             static_cast<int>(std::lround(
                 detection->diagonal_intersection_camera_px.y))},
            cv::Scalar(255, 255, 0),
            cv::MARKER_CROSS,
            45,
            4,
            cv::LINE_AA);
        const int inset = static_cast<int>(std::lround(
            detection->required_minimum_margin_camera_px));
        if (inset > 0 && inset * 2 < rgba.cols && inset * 2 < rgba.rows) {
            cv::rectangle(
                overlay_bgr,
                {inset, inset},
                {rgba.cols - 1 - inset, rgba.rows - 1 - inset},
                cv::Scalar(255, 0, 255),
                2,
                cv::LINE_AA);
        }
    }
    cv::Mat overlay_rgba;
    cv::cvtColor(overlay_bgr, overlay_rgba, cv::COLOR_BGR2RGBA);
    return std::vector<unsigned char>(
        overlay_rgba.data,
        overlay_rgba.data + overlay_rgba.total() * overlay_rgba.elemSize());
}

std::vector<Point2d> ProjectSquareCorners(
    const cv::Mat& homography,
    int center_x_canvas_px,
    int center_y_canvas_px,
    int side_canvas_px)
{
    const double half = 0.5 * static_cast<double>(side_canvas_px);
    const double left = center_x_canvas_px - half;
    const double top = center_y_canvas_px - half;
    // Citrus renders the inclusive outline centerlines at x_min/y_min and
    // x_min+width-1/y_min+height-1 inside a width-by-height texture.
    const double right = left + side_canvas_px - 1.0;
    const double bottom = top + side_canvas_px - 1.0;
    std::vector<cv::Point2f> canvas = {
        {static_cast<float>(left), static_cast<float>(top)},
        {static_cast<float>(right), static_cast<float>(top)},
        {static_cast<float>(right), static_cast<float>(bottom)},
        {static_cast<float>(left), static_cast<float>(bottom)}};
    std::vector<cv::Point2f> camera;
    cv::perspectiveTransform(canvas, camera, homography);
    std::vector<Point2d> result;
    result.reserve(camera.size());
    for (const auto& value : camera) result.push_back({value.x, value.y});
    return result;
}

void CornerMargins(const std::vector<Point2d>& corners,
                   int sensor_width_px,
                   int sensor_height_px,
                   double* left,
                   double* top,
                   double* right,
                   double* bottom)
{
    double minimum_x = std::numeric_limits<double>::infinity();
    double minimum_y = std::numeric_limits<double>::infinity();
    double maximum_x = -std::numeric_limits<double>::infinity();
    double maximum_y = -std::numeric_limits<double>::infinity();
    for (const auto& corner : corners) {
        minimum_x = std::min(minimum_x, corner.x);
        minimum_y = std::min(minimum_y, corner.y);
        maximum_x = std::max(maximum_x, corner.x);
        maximum_y = std::max(maximum_y, corner.y);
    }
    if (left) *left = minimum_x;
    if (top) *top = minimum_y;
    if (right) *right = static_cast<double>(sensor_width_px - 1) - maximum_x;
    if (bottom) *bottom = static_cast<double>(sensor_height_px - 1) - maximum_y;
}

}  // namespace

nlohmann::json FiducialDetection::ToJson() const
{
    return {
        {"schema_id", "orange.arena_center_fiducial_detection"},
        {"schema_version", 1},
        {"ok", ok},
        {"camera_serial", camera_serial},
        {"error", error.empty() ? nlohmann::json(nullptr) : nlohmann::json(error)},
        {"coordinate_space", "camera_native_pixels"},
        {"sensor_size_px", {{"width", sensor_width_px}, {"height", sensor_height_px}}},
        {"sensor_raster_center_camera_px", {
            {"x", sensor_raster_center_camera_px.x},
            {"y", sensor_raster_center_camera_px.y}}},
        {"center_camera_px", {{"x", center_camera_px.x}, {"y", center_camera_px.y}}},
        {"radius_camera_px", radius_camera_px},
        {"ellipse_major_axis_px", ellipse_major_axis_px},
        {"ellipse_minor_axis_px", ellipse_minor_axis_px},
        {"axis_ratio", axis_ratio},
        {"ring_contrast_u8", ring_contrast_u8},
        {"filled_disk_contrast_u8", filled_disk_contrast_u8},
        {"center_dot_contrast_u8", center_dot_contrast_u8},
        {"center_error_camera_px", {
            {"x", center_error_x_camera_px},
            {"y", center_error_y_camera_px}}},
        {"score", score},
        {"strategy", strategy},
        {"worker_thread_id_hash", worker_thread_id_hash},
    };
}

nlohmann::json ConcurrentDetectionBatch::ToJson() const
{
    nlohmann::json rows = nlohmann::json::array();
    for (const auto& detection : detections) {
        rows.push_back(detection.ToJson());
    }
    return {
        {"schema_id", "orange.arena_center_fiducial_detection_batch"},
        {"schema_version", 1},
        {"task_count", task_count},
        {"maximum_concurrent_tasks", maximum_concurrent_tasks},
        {"execution_policy", execution_policy},
        {"barrier", "all_camera_analyses_join_before_next_projection_state"},
        {"detections", std::move(rows)},
    };
}

FiducialDetection DetectArenaCenterFiducial(
    const RgbaFrameView& frame,
    const FiducialDetectorConfig& config)
{
    FiducialDetection result;
    result.camera_serial = frame.camera_serial;
    result.sensor_width_px = frame.width;
    result.sensor_height_px = frame.height;
    result.worker_thread_id_hash = static_cast<std::uint64_t>(
        std::hash<std::thread::id>{}(std::this_thread::get_id()));
    if (frame.width <= 0 || frame.height <= 0 || frame.rgba == nullptr ||
        frame.rgba->size() !=
            static_cast<std::size_t>(frame.width) *
                static_cast<std::size_t>(frame.height) * 4u) {
        result.error = "invalid_owned_rgba_frame";
        return result;
    }

    result.sensor_raster_center_camera_px = {
        (static_cast<double>(frame.width) - 1.0) * 0.5,
        (static_cast<double>(frame.height) - 1.0) * 0.5};
    const cv::Point2d target = config.has_expected_center_camera_px
        ? cv::Point2d(
              config.expected_center_camera_px.x,
              config.expected_center_camera_px.y)
        : cv::Point2d(
              result.sensor_raster_center_camera_px.x,
              result.sensor_raster_center_camera_px.y);
    if (!std::isfinite(target.x) || !std::isfinite(target.y) ||
        target.x < 0.0 || target.y < 0.0 ||
        target.x >= static_cast<double>(frame.width) ||
        target.y >= static_cast<double>(frame.height)) {
        result.error = "expected_fiducial_center_outside_camera_raster";
        return result;
    }
    const int min_dimension = std::min(frame.width, frame.height);
    const int half_extent = std::clamp(
        static_cast<int>(std::lround(
            min_dimension * config.search_half_extent_fraction)),
        64,
        min_dimension / 2);
    const int roi_x = std::clamp(
        static_cast<int>(std::floor(target.x)) - half_extent,
        0,
        std::max(0, frame.width - 1));
    const int roi_y = std::clamp(
        static_cast<int>(std::floor(target.y)) - half_extent,
        0,
        std::max(0, frame.height - 1));
    const int roi_width = std::min(frame.width - roi_x, half_extent * 2 + 1);
    const int roi_height = std::min(frame.height - roi_y, half_extent * 2 + 1);

    cv::Mat rgba(
        frame.height,
        frame.width,
        CV_8UC4,
        const_cast<unsigned char*>(frame.rgba->data()));
    cv::Mat gray;
    cv::cvtColor(rgba, gray, cv::COLOR_RGBA2GRAY);
    cv::Mat roi = gray(cv::Rect(roi_x, roi_y, roi_width, roi_height));
    cv::Mat blurred;
    cv::GaussianBlur(roi, blurred, cv::Size(7, 7), 1.4);

    const int min_radius = std::max(
        4,
        static_cast<int>(std::floor(
            min_dimension * config.min_radius_fraction)));
    const int max_radius = std::max(
        min_radius + 2,
        static_cast<int>(std::ceil(
            min_dimension * config.max_radius_fraction)));
    std::vector<cv::Vec3f> circles;
    cv::HoughCircles(
        blurred,
        circles,
        cv::HOUGH_GRADIENT,
        1.25,
        std::max(12.0, static_cast<double>(min_radius) * 2.0),
        80.0,
        18.0,
        min_radius,
        max_radius);

    Candidate best;
    const double max_center_distance =
        min_dimension * config.max_center_distance_fraction;
    for (const cv::Vec3f& circle : circles) {
        const cv::Point2d center(
            static_cast<double>(circle[0]) + roi_x,
            static_cast<double>(circle[1]) + roi_y);
        const double radius = circle[2];
        if (cv::norm(center - target) > max_center_distance) {
            continue;
        }
        ConsiderCandidate(
            ScoreCandidate(
                gray,
                center,
                radius,
                radius * 2.0,
                radius * 2.0,
                target,
                config,
                "hough_ring"),
            &best);
    }

    cv::Mat thresholded;
    cv::threshold(blurred, thresholded, 0, 255, cv::THRESH_BINARY | cv::THRESH_OTSU);
    std::vector<std::vector<cv::Point>> contours;
    std::vector<Candidate> threshold_shapes;
    cv::findContours(thresholded, contours, cv::RETR_LIST, cv::CHAIN_APPROX_NONE);
    for (const auto& contour : contours) {
        if (contour.size() < 20) {
            continue;
        }
        const cv::RotatedRect ellipse = cv::fitEllipse(contour);
        const double major_axis = std::max(ellipse.size.width, ellipse.size.height);
        const double minor_axis = std::min(ellipse.size.width, ellipse.size.height);
        const double radius = 0.25 * (major_axis + minor_axis);
        if (radius < min_radius || radius > max_radius) {
            continue;
        }
        const cv::Point2d center(
            static_cast<double>(ellipse.center.x) + roi_x,
            static_cast<double>(ellipse.center.y) + roi_y);
        if (cv::norm(center - target) > max_center_distance) {
            continue;
        }
        const Candidate shape = ScoreCandidate(
            gray,
            center,
            radius,
            major_axis,
            minor_axis,
            target,
            config,
            "threshold_ellipse");
        threshold_shapes.push_back(shape);
        ConsiderCandidate(shape, &best);
    }

    if (!std::isfinite(best.score)) {
        result.error = "arena_center_fiducial_not_found_or_quality_rejected";
        result.overlay_rgba = MakeOverlay(rgba, nullptr, target);
        return result;
    }
    for (const Candidate& shape : threshold_shapes) {
        const double center_separation = cv::norm(shape.center - best.center);
        const double radius_ratio = best.radius > 0.0
            ? shape.radius / best.radius
            : std::numeric_limits<double>::infinity();
        const bool shape_has_marker_contrast =
            shape.ring_contrast >= config.min_ring_contrast_u8 ||
            shape.filled_disk_contrast >= config.min_filled_disk_contrast_u8;
        if (center_separation <= std::max(12.0, best.radius) &&
            radius_ratio >= 0.65 && radius_ratio <= 2.00 &&
            shape_has_marker_contrast &&
            shape.axis_ratio > config.max_axis_ratio) {
            result.error = "arena_center_fiducial_shape_unstable_or_ghosted";
            result.overlay_rgba = MakeOverlay(rgba, &shape, target);
            return result;
        }
    }
    result.ok = true;
    result.center_camera_px = {best.center.x, best.center.y};
    result.radius_camera_px = best.radius;
    result.ellipse_major_axis_px = best.major_axis;
    result.ellipse_minor_axis_px = best.minor_axis;
    result.axis_ratio = best.axis_ratio;
    result.ring_contrast_u8 = best.ring_contrast;
    result.filled_disk_contrast_u8 = best.filled_disk_contrast;
    result.center_dot_contrast_u8 = best.dot_contrast;
    result.center_error_x_camera_px = best.center.x - target.x;
    result.center_error_y_camera_px = best.center.y - target.y;
    result.score = best.score;
    result.strategy = best.strategy;
    result.overlay_rgba = MakeOverlay(rgba, &best, target);
    return result;
}

ConcurrentDetectionBatch DetectArenaCenterFiducialsConcurrently(
    const std::vector<RgbaFrameView>& frames,
    const FiducialDetectorConfig& config)
{
    ConcurrentDetectionBatch batch;
    batch.task_count = frames.size();
    if (frames.empty()) {
        return batch;
    }

    std::atomic<std::size_t> ready{0};
    std::atomic<std::size_t> active{0};
    std::atomic<std::size_t> maximum_active{0};
    std::promise<void> start_promise;
    std::shared_future<void> start_gate(start_promise.get_future());
    std::vector<std::future<FiducialDetection>> futures;
    futures.reserve(frames.size());
    for (const RgbaFrameView& frame : frames) {
        futures.push_back(std::async(
            std::launch::async,
            [frame, &config, &ready, &active, &maximum_active, start_gate]() {
                ready.fetch_add(1, std::memory_order_acq_rel);
                start_gate.wait();
                const std::size_t now = active.fetch_add(1, std::memory_order_acq_rel) + 1;
                std::size_t observed = maximum_active.load(std::memory_order_relaxed);
                while (observed < now &&
                       !maximum_active.compare_exchange_weak(
                           observed, now, std::memory_order_release,
                           std::memory_order_relaxed)) {
                }
                FiducialDetection detection = DetectArenaCenterFiducial(frame, config);
                active.fetch_sub(1, std::memory_order_acq_rel);
                return detection;
            }));
    }
    while (ready.load(std::memory_order_acquire) < frames.size()) {
        std::this_thread::yield();
    }
    start_promise.set_value();
    batch.detections.reserve(frames.size());
    for (auto& future : futures) {
        batch.detections.push_back(future.get());
    }
    batch.maximum_concurrent_tasks = maximum_active.load(std::memory_order_acquire);
    return batch;
}

nlohmann::json RectangleBoundaryDetection::ToJson() const
{
    auto point = [](Point2d value) {
        return nlohmann::json{{"x", value.x}, {"y", value.y}};
    };
    return {
        {"schema_id", "orange.arena_rectangle_boundary_detection"},
        {"schema_version", 1},
        {"ok", ok},
        {"fully_visible_with_margin", fully_visible_with_margin},
        {"camera_serial", camera_serial},
        {"error", error.empty() ? nlohmann::json(nullptr) : nlohmann::json(error)},
        {"coordinate_space", "camera_native_pixels"},
        {"sensor_size_px", {{"width", sensor_width_px}, {"height", sensor_height_px}}},
        {"corners_camera_px", {
            {"top_left", point(top_left_camera_px)},
            {"top_right", point(top_right_camera_px)},
            {"bottom_right", point(bottom_right_camera_px)},
            {"bottom_left", point(bottom_left_camera_px)},
        }},
        {"diagonal_intersection_camera_px", point(diagonal_intersection_camera_px)},
        {"sensor_edge_margins_camera_px", {
            {"left", left_margin_camera_px},
            {"top", top_margin_camera_px},
            {"right", right_margin_camera_px},
            {"bottom", bottom_margin_camera_px},
            {"minimum", minimum_margin_camera_px},
            {"required_minimum", required_minimum_margin_camera_px},
        }},
        {"edge_band_spread_camera_px", {
            {"top", top_edge_band_spread_camera_px},
            {"right", right_edge_band_spread_camera_px},
            {"bottom", bottom_edge_band_spread_camera_px},
            {"left", left_edge_band_spread_camera_px},
        }},
        {"edge_candidate_count", {
            {"top", top_edge_candidate_count},
            {"right", right_edge_candidate_count},
            {"bottom", bottom_edge_candidate_count},
            {"left", left_edge_candidate_count},
        }},
        {"quadrilateral_area_fraction", quadrilateral_area_fraction},
        {"worker_thread_id_hash", worker_thread_id_hash},
    };
}

nlohmann::json ConcurrentRectangleDetectionBatch::ToJson() const
{
    nlohmann::json rows = nlohmann::json::array();
    for (const auto& detection : detections) rows.push_back(detection.ToJson());
    return {
        {"schema_id", "orange.arena_rectangle_boundary_detection_batch"},
        {"schema_version", 1},
        {"task_count", task_count},
        {"maximum_concurrent_tasks", maximum_concurrent_tasks},
        {"execution_policy", execution_policy},
        {"barrier", "all_camera_rectangle_analyses_join_before_next_projection_state"},
        {"detections", std::move(rows)},
    };
}

RectangleBoundaryDetection DetectArenaRectangleBoundary(
    const RgbaFrameView& frame,
    const RectangleBoundaryDetectorConfig& config)
{
    RectangleBoundaryDetection result;
    result.camera_serial = frame.camera_serial;
    result.sensor_width_px = frame.width;
    result.sensor_height_px = frame.height;
    result.required_minimum_margin_camera_px =
        config.minimum_visible_margin_camera_px;
    result.worker_thread_id_hash = static_cast<std::uint64_t>(
        std::hash<std::thread::id>{}(std::this_thread::get_id()));
    if (frame.width <= 0 || frame.height <= 0 || frame.rgba == nullptr ||
        frame.rgba->size() != static_cast<std::size_t>(frame.width) *
            static_cast<std::size_t>(frame.height) * 4u) {
        result.error = "invalid_owned_rgba_frame";
        return result;
    }

    cv::Mat rgba(
        frame.height,
        frame.width,
        CV_8UC4,
        const_cast<unsigned char*>(frame.rgba->data()));
    cv::Mat gray;
    cv::cvtColor(rgba, gray, cv::COLOR_RGBA2GRAY);
    const double resize_scale = std::min(
        1.0,
        static_cast<double>(std::max(128, config.downsample_max_dimension_px)) /
            static_cast<double>(std::max(frame.width, frame.height)));
    cv::Mat working;
    cv::resize(gray, working, cv::Size(), resize_scale, resize_scale, cv::INTER_AREA);
    cv::GaussianBlur(working, working, cv::Size(5, 5), 1.0);
    cv::Mat edges;
    cv::Canny(
        working,
        edges,
        config.canny_low_threshold_u8,
        config.canny_high_threshold_u8,
        3,
        true);
    std::vector<cv::Vec4i> hough_lines;
    const int minimum_dimension = std::min(working.cols, working.rows);
    cv::HoughLinesP(
        edges,
        hough_lines,
        1.0,
        CV_PI / 1800.0,
        std::max(40, static_cast<int>(std::lround(minimum_dimension * 0.28))),
        std::max(32.0, minimum_dimension * config.hough_min_line_length_fraction),
        std::max(8.0, minimum_dimension * config.hough_max_line_gap_fraction));

    std::vector<DetectedLine> top_candidates;
    std::vector<DetectedLine> right_candidates;
    std::vector<DetectedLine> bottom_candidates;
    std::vector<DetectedLine> left_candidates;
    const double tangent_limit = std::tan(
        config.maximum_axis_angle_degrees * CV_PI / 180.0);
    const double center_x = (working.cols - 1.0) * 0.5;
    const double center_y = (working.rows - 1.0) * 0.5;
    const double outer_x = working.cols * config.outer_search_fraction;
    const double outer_y = working.rows * config.outer_search_fraction;
    const double scale_x = static_cast<double>(frame.width) / working.cols;
    const double scale_y = static_cast<double>(frame.height) / working.rows;
    for (const cv::Vec4i& line : hough_lines) {
        const double x1 = line[0];
        const double y1 = line[1];
        const double x2 = line[2];
        const double y2 = line[3];
        const double dx = x2 - x1;
        const double dy = y2 - y1;
        const double length = std::hypot(dx, dy);
        if (std::abs(dx) >= std::abs(dy) &&
            std::abs(dy) <= tangent_limit * std::abs(dx) &&
            std::abs(dx) > 1e-6) {
            const double slope_working = dy / dx;
            const double slope = slope_working * scale_y / scale_x;
            const double y_at_center_working =
                0.5 * (y1 + y2) - slope_working * (0.5 * (x1 + x2) - center_x);
            const DetectedLine detected{
                slope,
                y_at_center_working * scale_y,
                length * scale_x};
            if (y_at_center_working <= outer_y) top_candidates.push_back(detected);
            if (y_at_center_working >= working.rows - 1.0 - outer_y) {
                bottom_candidates.push_back(detected);
            }
        } else if (std::abs(dy) > std::abs(dx) &&
                   std::abs(dx) <= tangent_limit * std::abs(dy) &&
                   std::abs(dy) > 1e-6) {
            const double slope_working = dx / dy;
            const double slope = slope_working * scale_x / scale_y;
            const double x_at_center_working =
                0.5 * (x1 + x2) - slope_working * (0.5 * (y1 + y2) - center_y);
            const DetectedLine detected{
                slope,
                x_at_center_working * scale_x,
                length * scale_y};
            if (x_at_center_working <= outer_x) left_candidates.push_back(detected);
            if (x_at_center_working >= working.cols - 1.0 - outer_x) {
                right_candidates.push_back(detected);
            }
        }
    }

    const double maximum_spread =
        std::min(frame.width, frame.height) *
        config.maximum_edge_band_spread_fraction;
    const RepresentativeLine top = MakeRepresentativeLine(
        top_candidates, true, (frame.width - 1.0) * 0.5, maximum_spread);
    const RepresentativeLine right = MakeRepresentativeLine(
        right_candidates, false, (frame.height - 1.0) * 0.5, maximum_spread);
    const RepresentativeLine bottom = MakeRepresentativeLine(
        bottom_candidates, true, (frame.width - 1.0) * 0.5, maximum_spread);
    const RepresentativeLine left = MakeRepresentativeLine(
        left_candidates, false, (frame.height - 1.0) * 0.5, maximum_spread);
    result.top_edge_candidate_count = top.candidate_count;
    result.right_edge_candidate_count = right.candidate_count;
    result.bottom_edge_candidate_count = bottom.candidate_count;
    result.left_edge_candidate_count = left.candidate_count;
    result.top_edge_band_spread_camera_px = top.band_spread;
    result.right_edge_band_spread_camera_px = right.band_spread;
    result.bottom_edge_band_spread_camera_px = bottom.band_spread;
    result.left_edge_band_spread_camera_px = left.band_spread;
    if (!top.ok || !right.ok || !bottom.ok || !left.ok) {
        result.error = "four_supported_rectangle_edges_not_found";
        result.overlay_rgba = MakeRectangleOverlay(rgba, nullptr);
        return result;
    }
    if (!IntersectLines(top, left, &result.top_left_camera_px) ||
        !IntersectLines(top, right, &result.top_right_camera_px) ||
        !IntersectLines(bottom, right, &result.bottom_right_camera_px) ||
        !IntersectLines(bottom, left, &result.bottom_left_camera_px)) {
        result.error = "rectangle_edge_intersection_degenerate";
        result.overlay_rgba = MakeRectangleOverlay(rgba, nullptr);
        return result;
    }
    const std::vector<Point2d> corners = {
        result.top_left_camera_px,
        result.top_right_camera_px,
        result.bottom_right_camera_px,
        result.bottom_left_camera_px};
    result.quadrilateral_area_fraction = PolygonArea(corners) /
        (static_cast<double>(frame.width) * frame.height);
    result.diagonal_intersection_camera_px = DiagonalIntersection(corners);
    if (!std::isfinite(result.diagonal_intersection_camera_px.x) ||
        !std::isfinite(result.diagonal_intersection_camera_px.y) ||
        result.quadrilateral_area_fraction < 0.20 ||
        result.quadrilateral_area_fraction > 1.10) {
        result.error = "rectangle_quadrilateral_geometry_rejected";
        result.overlay_rgba = MakeRectangleOverlay(rgba, nullptr);
        return result;
    }
    CornerMargins(
        corners,
        frame.width,
        frame.height,
        &result.left_margin_camera_px,
        &result.top_margin_camera_px,
        &result.right_margin_camera_px,
        &result.bottom_margin_camera_px);
    result.minimum_margin_camera_px = std::min({
        result.left_margin_camera_px,
        result.top_margin_camera_px,
        result.right_margin_camera_px,
        result.bottom_margin_camera_px});
    result.fully_visible_with_margin =
        result.minimum_margin_camera_px >=
        config.minimum_visible_margin_camera_px;
    result.ok = true;
    result.overlay_rgba = MakeRectangleOverlay(rgba, &result);
    return result;
}

ConcurrentRectangleDetectionBatch DetectArenaRectangleBoundariesConcurrently(
    const std::vector<RgbaFrameView>& frames,
    const RectangleBoundaryDetectorConfig& config)
{
    ConcurrentRectangleDetectionBatch batch;
    batch.task_count = frames.size();
    if (frames.empty()) return batch;
    std::atomic<std::size_t> ready{0};
    std::atomic<std::size_t> active{0};
    std::atomic<std::size_t> maximum_active{0};
    std::promise<void> start_promise;
    std::shared_future<void> start_gate(start_promise.get_future());
    std::vector<std::future<RectangleBoundaryDetection>> futures;
    futures.reserve(frames.size());
    for (const RgbaFrameView& frame : frames) {
        futures.push_back(std::async(
            std::launch::async,
            [frame, &config, &ready, &active, &maximum_active, start_gate]() {
                ready.fetch_add(1, std::memory_order_acq_rel);
                start_gate.wait();
                const std::size_t now =
                    active.fetch_add(1, std::memory_order_acq_rel) + 1;
                std::size_t observed = maximum_active.load(std::memory_order_relaxed);
                while (observed < now &&
                       !maximum_active.compare_exchange_weak(
                           observed,
                           now,
                           std::memory_order_release,
                           std::memory_order_relaxed)) {
                }
                auto detection = DetectArenaRectangleBoundary(frame, config);
                active.fetch_sub(1, std::memory_order_acq_rel);
                return detection;
            }));
    }
    while (ready.load(std::memory_order_acquire) < frames.size()) {
        std::this_thread::yield();
    }
    start_promise.set_value();
    batch.detections.reserve(frames.size());
    for (auto& future : futures) batch.detections.push_back(future.get());
    batch.maximum_concurrent_tasks = maximum_active.load(std::memory_order_acquire);
    return batch;
}

nlohmann::json MaximalSquareProposal::ToJson() const
{
    nlohmann::json corners = nlohmann::json::array();
    for (const auto& corner : predicted_corners_camera_px) {
        corners.push_back({{"x", corner.x}, {"y", corner.y}});
    }
    return {
        {"schema_id", "orange.arena_maximal_visible_square_proposal"},
        {"schema_version", 1},
        {"ok", ok},
        {"error", error.empty() ? nlohmann::json(nullptr) : nlohmann::json(error)},
        {"center_canvas_px", {
            {"x", center_x_canvas_px}, {"y", center_y_canvas_px}}},
        {"source_size_canvas_px", {
            {"width", source_width_canvas_px},
            {"height", source_height_canvas_px}}},
        {"proposed_size_canvas_px", {
            {"width", proposed_width_canvas_px},
            {"height", proposed_height_canvas_px}}},
        {"scale_relative_to_source", {
            {"width", scale_relative_to_source_width},
            {"height", scale_relative_to_source_height}}},
        {"required_safety_margin_camera_px", required_safety_margin_camera_px},
        {"predicted_sensor_edge_margins_camera_px", {
            {"left", predicted_left_margin_camera_px},
            {"top", predicted_top_margin_camera_px},
            {"right", predicted_right_margin_camera_px},
            {"bottom", predicted_bottom_margin_camera_px}}},
        {"predicted_corners_camera_px", std::move(corners)},
    };
}

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
    const MaximalSquareProposalConfig& config)
{
    MaximalSquareProposal result;
    result.center_x_canvas_px = center_x_canvas_px;
    result.center_y_canvas_px = center_y_canvas_px;
    result.source_width_canvas_px = source_width_canvas_px;
    result.source_height_canvas_px = source_height_canvas_px;
    result.required_safety_margin_camera_px = config.safety_margin_camera_px;
    if (!detected_source_rectangle.ok || sensor_width_px <= 0 ||
        sensor_height_px <= 0 || canvas_width_px <= 0 || canvas_height_px <= 0 ||
        source_width_canvas_px <= 0 || source_height_canvas_px <= 0 ||
        center_x_canvas_px < 0 || center_y_canvas_px < 0) {
        result.error = "invalid_rectangle_or_coordinate_domain";
        return result;
    }
    const double source_half_width = 0.5 * source_width_canvas_px;
    const double source_half_height = 0.5 * source_height_canvas_px;
    const double source_left = center_x_canvas_px - source_half_width;
    const double source_top = center_y_canvas_px - source_half_height;
    const double source_right = source_left + source_width_canvas_px - 1.0;
    const double source_bottom = source_top + source_height_canvas_px - 1.0;
    const std::vector<cv::Point2f> canvas_points = {
        {static_cast<float>(source_left), static_cast<float>(source_top)},
        {static_cast<float>(source_right), static_cast<float>(source_top)},
        {static_cast<float>(source_right), static_cast<float>(source_bottom)},
        {static_cast<float>(source_left), static_cast<float>(source_bottom)}};
    const std::vector<cv::Point2f> camera_points = {
        {static_cast<float>(detected_source_rectangle.top_left_camera_px.x),
         static_cast<float>(detected_source_rectangle.top_left_camera_px.y)},
        {static_cast<float>(detected_source_rectangle.top_right_camera_px.x),
         static_cast<float>(detected_source_rectangle.top_right_camera_px.y)},
        {static_cast<float>(detected_source_rectangle.bottom_right_camera_px.x),
         static_cast<float>(detected_source_rectangle.bottom_right_camera_px.y)},
        {static_cast<float>(detected_source_rectangle.bottom_left_camera_px.x),
         static_cast<float>(detected_source_rectangle.bottom_left_camera_px.y)}};
    const cv::Mat homography = cv::getPerspectiveTransform(
        canvas_points, camera_points, cv::DECOMP_SVD);
    if (homography.empty() || !cv::checkRange(homography)) {
        result.error = "source_rectangle_homography_failed";
        return result;
    }

    int maximum_side = 2 * std::min({
        center_x_canvas_px,
        canvas_width_px - center_x_canvas_px,
        center_y_canvas_px,
        canvas_height_px - center_y_canvas_px});
    maximum_side -= maximum_side % 2;
    int minimum_side = std::max(2, config.minimum_side_canvas_px);
    if (minimum_side % 2 != 0) ++minimum_side;
    if (maximum_side < minimum_side) {
        result.error = "canvas_has_no_supported_even_square_at_center";
        return result;
    }
    auto is_safe = [&](int side, std::vector<Point2d>* corners_out) {
        std::vector<Point2d> corners = ProjectSquareCorners(
            homography, center_x_canvas_px, center_y_canvas_px, side);
        double left = 0.0;
        double top = 0.0;
        double right = 0.0;
        double bottom = 0.0;
        CornerMargins(corners, sensor_width_px, sensor_height_px,
                      &left, &top, &right, &bottom);
        if (corners_out) *corners_out = std::move(corners);
        return std::min({left, top, right, bottom}) + 1e-6 >=
            config.safety_margin_camera_px;
    };
    if (!is_safe(minimum_side, nullptr)) {
        result.error = "minimum_square_does_not_fit_sensor_safety_domain";
        return result;
    }
    int low = minimum_side / 2;
    int high = maximum_side / 2;
    while (low < high) {
        const int middle = (low + high + 1) / 2;
        if (is_safe(middle * 2, nullptr)) low = middle;
        else high = middle - 1;
    }
    const int proposed_side = low * 2;
    const double width_scale =
        static_cast<double>(proposed_side) / source_width_canvas_px;
    const double height_scale =
        static_cast<double>(proposed_side) / source_height_canvas_px;
    if (std::abs(width_scale - 1.0) > config.maximum_scale_change_fraction ||
        std::abs(height_scale - 1.0) > config.maximum_scale_change_fraction) {
        result.error = "proposed_scale_change_exceeds_commissioning_gate";
        return result;
    }
    result.proposed_width_canvas_px = proposed_side;
    result.proposed_height_canvas_px = proposed_side;
    result.scale_relative_to_source_width = width_scale;
    result.scale_relative_to_source_height = height_scale;
    result.predicted_corners_camera_px = ProjectSquareCorners(
        homography, center_x_canvas_px, center_y_canvas_px, proposed_side);
    CornerMargins(
        result.predicted_corners_camera_px,
        sensor_width_px,
        sensor_height_px,
        &result.predicted_left_margin_camera_px,
        &result.predicted_top_margin_camera_px,
        &result.predicted_right_margin_camera_px,
        &result.predicted_bottom_margin_camera_px);
    result.ok = true;
    return result;
}

nlohmann::json CenteringSolveResult::ToJson() const
{
    return {
        {"schema_id", "orange.arena_centering.solve"},
        {"schema_version", 1},
        {"ok", ok},
        {"error", error.empty() ? nlohmann::json(nullptr) : nlohmann::json(error)},
        {"target_camera_px", {{"x", target_camera_px.x}, {"y", target_camera_px.y}}},
        {"baseline_error_camera_px", {
            {"x", baseline_error_camera_px.x}, {"y", baseline_error_camera_px.y}}},
        {"jacobian_camera_px_per_canvas_px", {
            {jacobian_camera_px_per_canvas_px[0][0],
             jacobian_camera_px_per_canvas_px[0][1]},
            {jacobian_camera_px_per_canvas_px[1][0],
             jacobian_camera_px_per_canvas_px[1][1]}}},
        {"determinant", determinant},
        {"condition_number", condition_number},
        {"plus_minus_x_displacement_camera_px", plus_minus_x_displacement_camera_px},
        {"plus_minus_y_displacement_camera_px", plus_minus_y_displacement_camera_px},
        {"symmetric_midpoint_x_error_camera_px", symmetric_midpoint_x_error_camera_px},
        {"symmetric_midpoint_y_error_camera_px", symmetric_midpoint_y_error_camera_px},
        {"delta_canvas_px", {{"x", delta_canvas_px.x}, {"y", delta_canvas_px.y}}},
        {"candidate_canvas_px", {
            {"x", candidate_canvas_px.x}, {"y", candidate_canvas_px.y}}},
        {"predicted_integer_quantization_residual_camera_px", {
            {"x", predicted_integer_quantization_residual_camera_px.x},
            {"y", predicted_integer_quantization_residual_camera_px.y}}},
        {"predicted_integer_quantization_residual_norm_camera_px",
         predicted_integer_quantization_residual_norm_camera_px},
        {"candidate_center_canvas_px_integer", {
            {"x", candidate_center_x_canvas_px},
            {"y", candidate_center_y_canvas_px}}},
    };
}

CenteringSolveResult SolveSymmetricArenaCentering(
    const SymmetricProbeObservations& observations,
    Point2d target_camera_px,
    const SolverConfig& config)
{
    CenteringSolveResult result;
    result.target_camera_px = target_camera_px;
    const double dx_canvas = observations.plus_x.canvas_center_px.x -
                             observations.minus_x.canvas_center_px.x;
    const double dy_canvas = observations.plus_y.canvas_center_px.y -
                             observations.minus_y.canvas_center_px.y;
    if (std::abs(dx_canvas) < 1.0 || std::abs(dy_canvas) < 1.0) {
        result.error = "symmetric_probe_canvas_baseline_too_small";
        return result;
    }
    const Point2d camera_dx = Subtract(
        observations.plus_x.camera_center_px,
        observations.minus_x.camera_center_px);
    const Point2d camera_dy = Subtract(
        observations.plus_y.camera_center_px,
        observations.minus_y.camera_center_px);
    result.plus_minus_x_displacement_camera_px = Norm(camera_dx);
    result.plus_minus_y_displacement_camera_px = Norm(camera_dy);
    if (result.plus_minus_x_displacement_camera_px <
            config.min_probe_displacement_camera_px ||
        result.plus_minus_y_displacement_camera_px <
            config.min_probe_displacement_camera_px) {
        result.error = "probe_camera_displacement_below_quality_gate";
        return result;
    }

    result.jacobian_camera_px_per_canvas_px[0][0] = camera_dx.x / dx_canvas;
    result.jacobian_camera_px_per_canvas_px[1][0] = camera_dx.y / dx_canvas;
    result.jacobian_camera_px_per_canvas_px[0][1] = camera_dy.x / dy_canvas;
    result.jacobian_camera_px_per_canvas_px[1][1] = camera_dy.y / dy_canvas;
    const double a = result.jacobian_camera_px_per_canvas_px[0][0];
    const double b = result.jacobian_camera_px_per_canvas_px[0][1];
    const double c = result.jacobian_camera_px_per_canvas_px[1][0];
    const double d = result.jacobian_camera_px_per_canvas_px[1][1];
    result.determinant = a * d - b * c;
    result.condition_number = ConditionNumber2x2(a, b, c, d);
    if (std::abs(result.determinant) < config.min_abs_determinant) {
        result.error = "jacobian_singular";
        return result;
    }
    if (config.require_positive_determinant && result.determinant <= 0.0) {
        result.error = "jacobian_reflection_rejected";
        return result;
    }
    if (!std::isfinite(result.condition_number) ||
        result.condition_number > config.max_condition_number) {
        result.error = "jacobian_condition_number_rejected";
        return result;
    }

    const Point2d midpoint_x = Scale(Add(
        observations.plus_x.camera_center_px,
        observations.minus_x.camera_center_px), 0.5);
    const Point2d midpoint_y = Scale(Add(
        observations.plus_y.camera_center_px,
        observations.minus_y.camera_center_px), 0.5);
    result.symmetric_midpoint_x_error_camera_px = Norm(Subtract(
        midpoint_x, observations.baseline.camera_center_px));
    result.symmetric_midpoint_y_error_camera_px = Norm(Subtract(
        midpoint_y, observations.baseline.camera_center_px));
    if (result.symmetric_midpoint_x_error_camera_px >
            config.max_symmetric_midpoint_error_camera_px ||
        result.symmetric_midpoint_y_error_camera_px >
            config.max_symmetric_midpoint_error_camera_px) {
        result.error = "symmetric_probe_nonlinearity_rejected";
        return result;
    }

    result.baseline_error_camera_px = Subtract(
        target_camera_px, observations.baseline.camera_center_px);
    result.delta_canvas_px = ApplyJacobianCorrection(
        result, result.baseline_error_camera_px);
    if (!std::isfinite(result.delta_canvas_px.x) ||
        !std::isfinite(result.delta_canvas_px.y) ||
        Norm(result.delta_canvas_px) > config.max_candidate_move_canvas_px) {
        result.error = "candidate_canvas_move_rejected";
        return result;
    }
    result.candidate_canvas_px = Add(
        observations.baseline.canvas_center_px,
        result.delta_canvas_px);
    double best_quantization_norm = std::numeric_limits<double>::infinity();
    const int floor_x = static_cast<int>(std::floor(result.candidate_canvas_px.x));
    const int floor_y = static_cast<int>(std::floor(result.candidate_canvas_px.y));
    for (int integer_y = floor_y - 1; integer_y <= floor_y + 2; ++integer_y) {
        for (int integer_x = floor_x - 1; integer_x <= floor_x + 2; ++integer_x) {
            const double quantized_dx =
                static_cast<double>(integer_x) - result.candidate_canvas_px.x;
            const double quantized_dy =
                static_cast<double>(integer_y) - result.candidate_canvas_px.y;
            // Verification residual is target minus detected. Moving from the
            // continuous ideal to an integer projector pixel changes detected
            // position by J*dq, so the unavoidable residual is -J*dq.
            const Point2d residual{
                -(a * quantized_dx + b * quantized_dy),
                -(c * quantized_dx + d * quantized_dy)};
            const double norm = Norm(residual);
            if (norm < best_quantization_norm) {
                best_quantization_norm = norm;
                result.candidate_center_x_canvas_px = integer_x;
                result.candidate_center_y_canvas_px = integer_y;
                result.predicted_integer_quantization_residual_camera_px = residual;
            }
        }
    }
    result.predicted_integer_quantization_residual_norm_camera_px =
        best_quantization_norm;
    result.ok = true;
    return result;
}

Point2d ApplyJacobianCorrection(
    const CenteringSolveResult& solved,
    Point2d camera_error_px)
{
    const double a = solved.jacobian_camera_px_per_canvas_px[0][0];
    const double b = solved.jacobian_camera_px_per_canvas_px[0][1];
    const double c = solved.jacobian_camera_px_per_canvas_px[1][0];
    const double d = solved.jacobian_camera_px_per_canvas_px[1][1];
    const double determinant = a * d - b * c;
    if (std::abs(determinant) <= std::numeric_limits<double>::epsilon()) {
        return {
            std::numeric_limits<double>::quiet_NaN(),
            std::numeric_limits<double>::quiet_NaN()};
    }
    return {
        (d * camera_error_px.x - b * camera_error_px.y) / determinant,
        (-c * camera_error_px.x + a * camera_error_px.y) / determinant};
}

}  // namespace orange::gui::arena_centering
