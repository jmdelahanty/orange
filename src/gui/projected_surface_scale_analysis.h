#pragma once

#include "json.hpp"

#include <opencv2/core.hpp>

#include <filesystem>
#include <string>
#include <vector>

namespace orange::gui::projected_surface_scale {

struct TargetPoint {
    std::string point_id;
    cv::Point2d target_mm;
    std::string marker_role;
    double nominal_diameter_mm = 0.0;
    bool include_in_fit = true;
};

struct ObservedPoint {
    std::string point_id;
    cv::Point2d target_mm;
    cv::Point2d camera_px;
    std::string marker_role;
    bool used_for_fit = false;
    bool holdout = false;
    double residual_mm = 0.0;
};

struct QualityThresholds {
    int minimum_matched_points = 80;
    double minimum_matched_fraction = 0.75;
    double maximum_fit_rms_mm = 0.15;
    double maximum_fit_error_mm = 0.50;
    double maximum_holdout_rms_mm = 0.25;
    double maximum_holdout_error_mm = 0.75;
    double maximum_c_to_xplus_error_mm = 0.30;
    double maximum_camera_scale_anisotropy_fraction = 0.03;
    double maximum_outer_diameter_error_mm = 1.50;
};

struct AnalysisResult {
    bool ok = false;
    bool quality_pass = false;
    std::string error;
    std::vector<TargetPoint> target_points;
    std::vector<ObservedPoint> correspondences;
    cv::Mat target_mm_to_camera_px;
    cv::Mat camera_px_to_target_mm;
    cv::Mat target_mm_to_canvas_px;
    cv::Mat overlay_bgr;
    nlohmann::json report = nlohmann::json::object();
};

bool load_target_definition(
    const std::filesystem::path& target_json_path,
    std::vector<TargetPoint>* points_out,
    nlohmann::json* target_definition_out,
    std::string* error_out = nullptr);

AnalysisResult analyze(
    const cv::Mat& source_image,
    const std::vector<TargetPoint>& target_points,
    const nlohmann::json& target_definition,
    const cv::Mat& camera_px_to_canvas_px = cv::Mat(),
    const QualityThresholds& thresholds = {});

nlohmann::json matrix_json(const cv::Mat& matrix);

}  // namespace orange::gui::projected_surface_scale
