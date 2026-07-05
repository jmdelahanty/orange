#pragma once

#include <opencv2/core.hpp>

enum class RulerAlignmentOrientation {
    kHorizontal = 0,
    kVertical = 1
};

struct RulerAlignmentMetrics {
    bool has_detected_line = false;
    double line_angle_deg = 0.0;
    double angle_error_deg = 0.0;
    double center_offset_px = 0.0;
    double center_offset_fraction = 0.0;
    int x0 = 0;
    int y0 = 0;
    int x1 = 0;
    int y1 = 0;
};

const char* ruler_alignment_orientation_label(RulerAlignmentOrientation orientation);

RulerAlignmentMetrics detect_ruler_alignment(
    const cv::Mat& gray,
    RulerAlignmentOrientation orientation);
