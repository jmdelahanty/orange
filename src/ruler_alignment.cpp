#include "ruler_alignment.h"

#include <opencv2/imgproc.hpp>

#include <algorithm>
#include <cmath>
#include <vector>

const char* ruler_alignment_orientation_label(RulerAlignmentOrientation orientation)
{
    switch (orientation) {
        case RulerAlignmentOrientation::kVertical:
            return "vertical";
        case RulerAlignmentOrientation::kHorizontal:
        default:
            return "horizontal";
    }
}

RulerAlignmentMetrics detect_ruler_alignment(
    const cv::Mat& gray,
    RulerAlignmentOrientation orientation)
{
    RulerAlignmentMetrics best;
    if (gray.empty()) {
        return best;
    }

    auto detect_boundary_anchor = [&](const cv::Mat& source_gray) -> int {
        cv::Mat directionally_smoothed;
        if (orientation == RulerAlignmentOrientation::kHorizontal) {
            cv::GaussianBlur(source_gray, directionally_smoothed, cv::Size(61, 7), 0.0, 0.0, cv::BORDER_REPLICATE);
        } else {
            cv::GaussianBlur(source_gray, directionally_smoothed, cv::Size(7, 61), 0.0, 0.0, cv::BORDER_REPLICATE);
        }

        cv::Mat gradient;
        if (orientation == RulerAlignmentOrientation::kHorizontal) {
            cv::Sobel(directionally_smoothed, gradient, CV_32F, 0, 1, 3);
        } else {
            cv::Sobel(directionally_smoothed, gradient, CV_32F, 1, 0, 3);
        }
        cv::Mat abs_gradient = cv::abs(gradient);

        cv::Mat profile;
        if (orientation == RulerAlignmentOrientation::kHorizontal) {
            cv::reduce(abs_gradient, profile, 1, cv::REDUCE_AVG, CV_32F);
            cv::GaussianBlur(profile, profile, cv::Size(1, 31), 0.0, 0.0, cv::BORDER_REPLICATE);
        } else {
            cv::reduce(abs_gradient, profile, 0, cv::REDUCE_AVG, CV_32F);
            cv::GaussianBlur(profile, profile, cv::Size(31, 1), 0.0, 0.0, cv::BORDER_REPLICATE);
        }

        const int profile_length =
            orientation == RulerAlignmentOrientation::kHorizontal ? profile.rows : profile.cols;
        if (profile_length <= 0) {
            return -1;
        }

        const int margin = std::max(4, profile_length / 40);
        int best_index = -1;
        double best_score = -1.0;
        for (int i = margin; i < profile_length - margin; ++i) {
            const float response =
                orientation == RulerAlignmentOrientation::kHorizontal ? profile.at<float>(i, 0) : profile.at<float>(0, i);
            const double boundary_preference =
                1.0 - std::clamp(static_cast<double>(i) / std::max(1.0, static_cast<double>(profile_length - 1)), 0.0, 1.0);
            const double score = static_cast<double>(response) * (0.35 + 0.65 * boundary_preference);
            if (score > best_score) {
                best_score = score;
                best_index = i;
            }
        }
        return best_index;
    };

    const int boundary_anchor = detect_boundary_anchor(gray);

    cv::Mat blurred;
    cv::GaussianBlur(gray, blurred, cv::Size(5, 5), 0.0);
    cv::Mat edges;
    cv::Canny(blurred, edges, 60.0, 180.0, 3);

    cv::Rect search_roi(0, 0, gray.cols, gray.rows);
    if (boundary_anchor >= 0) {
        const int search_half_band = std::max(16, static_cast<int>(std::round(
            0.04 * static_cast<double>(
                orientation == RulerAlignmentOrientation::kHorizontal ? gray.rows : gray.cols))));
        if (orientation == RulerAlignmentOrientation::kHorizontal) {
            const int y0 = std::max(0, boundary_anchor - search_half_band);
            const int y1 = std::min(gray.rows, boundary_anchor + search_half_band + 1);
            search_roi = cv::Rect(0, y0, gray.cols, std::max(1, y1 - y0));
        } else {
            const int x0 = std::max(0, boundary_anchor - search_half_band);
            const int x1 = std::min(gray.cols, boundary_anchor + search_half_band + 1);
            search_roi = cv::Rect(x0, 0, std::max(1, x1 - x0), gray.rows);
        }
    }

    std::vector<cv::Vec4i> lines;
    cv::HoughLinesP(
        edges(search_roi),
        lines,
        1.0,
        CV_PI / 180.0,
        80,
        std::max(search_roi.width, search_roi.height) / 5.0,
        20.0);
    const double center_x = 0.5 * static_cast<double>(gray.cols);
    const double center_y = 0.5 * static_cast<double>(gray.rows);
    double best_score = -1.0;

    for (const cv::Vec4i& local_line : lines) {
        const cv::Vec4i line(
            local_line[0] + search_roi.x,
            local_line[1] + search_roi.y,
            local_line[2] + search_roi.x,
            local_line[3] + search_roi.y);
        const double dx = static_cast<double>(line[2] - line[0]);
        const double dy = static_cast<double>(line[3] - line[1]);
        const double length = std::hypot(dx, dy);
        if (length < std::max(gray.cols, gray.rows) * 0.15) {
            continue;
        }

        double angle = std::atan2(dy, dx) * 180.0 / CV_PI;
        while (angle > 90.0) angle -= 180.0;
        while (angle <= -90.0) angle += 180.0;

        double angle_error = 0.0;
        double center_offset_px = 0.0;
        double boundary_preference = 0.0;
        double anchor_distance_px = 0.0;
        if (orientation == RulerAlignmentOrientation::kHorizontal) {
            angle_error = std::abs(angle);
            const double line_center_y = (static_cast<double>(line[1]) + static_cast<double>(line[3])) * 0.5;
            center_offset_px = line_center_y - center_y;
            boundary_preference = 1.0 - std::clamp(line_center_y / std::max(1.0, static_cast<double>(gray.rows - 1)), 0.0, 1.0);
            anchor_distance_px = boundary_anchor >= 0 ? std::abs(line_center_y - static_cast<double>(boundary_anchor)) : 0.0;
        } else {
            angle_error = std::abs(90.0 - std::abs(angle));
            const double line_center_x = (static_cast<double>(line[0]) + static_cast<double>(line[2])) * 0.5;
            center_offset_px = line_center_x - center_x;
            boundary_preference = 1.0 - std::clamp(line_center_x / std::max(1.0, static_cast<double>(gray.cols - 1)), 0.0, 1.0);
            anchor_distance_px = boundary_anchor >= 0 ? std::abs(line_center_x - static_cast<double>(boundary_anchor)) : 0.0;
        }
        if (angle_error > 25.0) {
            continue;
        }

        const double center_extent =
            orientation == RulerAlignmentOrientation::kHorizontal ? center_y : center_x;
        const double angle_score = 1.0 - std::min(angle_error / 25.0, 1.0);
        const double anchor_score =
            boundary_anchor >= 0
                ? 1.0 - std::min(anchor_distance_px /
                                     std::max(1.0, 0.5 * static_cast<double>(
                                                       orientation == RulerAlignmentOrientation::kHorizontal
                                                           ? search_roi.height
                                                           : search_roi.width)),
                                 1.0)
                : 1.0;
        const double score =
            length * (0.15 + 0.85 * angle_score) * (0.25 + 0.75 * boundary_preference) * (0.20 + 0.80 * anchor_score);
        if (score > best_score) {
            best_score = score;
            best.has_detected_line = true;
            best.line_angle_deg = angle;
            best.angle_error_deg = angle_error;
            best.center_offset_px = center_offset_px;
            best.center_offset_fraction = center_extent > 0.0 ? center_offset_px / center_extent : 0.0;
            best.x0 = line[0];
            best.y0 = line[1];
            best.x1 = line[2];
            best.y1 = line[3];
        }
    }

    if (!best.has_detected_line && boundary_anchor >= 0) {
        best.has_detected_line = true;
        best.line_angle_deg = orientation == RulerAlignmentOrientation::kHorizontal ? 0.0 : 90.0;
        best.angle_error_deg = 0.0;
        if (orientation == RulerAlignmentOrientation::kHorizontal) {
            best.center_offset_px = static_cast<double>(boundary_anchor) - center_y;
            best.center_offset_fraction = center_y > 0.0 ? best.center_offset_px / center_y : 0.0;
            best.x0 = 0;
            best.x1 = gray.cols - 1;
            best.y0 = boundary_anchor;
            best.y1 = boundary_anchor;
        } else {
            best.center_offset_px = static_cast<double>(boundary_anchor) - center_x;
            best.center_offset_fraction = center_x > 0.0 ? best.center_offset_px / center_x : 0.0;
            best.x0 = boundary_anchor;
            best.x1 = boundary_anchor;
            best.y0 = 0;
            best.y1 = gray.rows - 1;
        }
    }

    return best;
}
