#include "gui/spatial_layout/hough_panel.h"

#include "gui/spatial_layout/geometry.h"
#include "imgui.h"

#include <opencv2/opencv.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <sstream>
#include <vector>

namespace orange::gui::spatial_layout {
namespace {

constexpr double kPi = 3.14159265358979323846;

orange::spatial::RuntimeGeometry runtime_circle(double cx, double cy, double r)
{
    orange::spatial::RuntimeGeometry geometry;
    geometry.type = orange::spatial::RuntimeGeometryType::kCircle;
    geometry.circle.cx = cx;
    geometry.circle.cy = cy;
    geometry.circle.r = r;
    return geometry;
}

void reset_hough_defaults(SpatialLayoutUiState* ui_state)
{
    if (ui_state == nullptr) {
        return;
    }
    ui_state->hough_dp = 1.25;
    ui_state->hough_min_dist_fraction = 0.20;
    ui_state->hough_param1 = 120.0;
    ui_state->hough_param2 = 30.0;
    ui_state->hough_min_radius_fraction = 0.18;
    ui_state->hough_max_radius_fraction = 0.49;
    ui_state->hough_radius_adjustment_px = 0.0;
    ui_state->hough_median_blur_ksize = 5;
    ui_state->hough_max_detection_dimension_px = 2048;
    ui_state->hough_fallback_enabled = true;
    ui_state->show_hough_proposal_overlay = true;
    ui_state->show_citrus_corrected_center_overlay = true;
}

void render_hough_registration_actions(
    SpatialLayoutUiState* ui_state,
    const HoughCirclePanelActions& actions)
{
    if (ui_state == nullptr) {
        return;
    }

    ImGui::BeginDisabled(!ui_state->has_capture);
    if (ImGui::Button("Fit Hough circle from capture")) {
        std::string detect_error;
        if (!detect_experimental_area_circle_from_capture(ui_state, &detect_error)) {
            ui_state->detection_error = detect_error;
            ui_state->detection_status = "Experimental-area detection failed.";
        }
    }
    ImGui::EndDisabled();

    ImGui::BeginDisabled(!ui_state->has_detected_experimental_area_circle);
    if (ImGui::Button("Use Hough fit for registration")) {
        std::string seed_error;
        if (!seed_registration_from_detected_experimental_area_circle(ui_state, &seed_error)) {
            ui_state->detection_error = seed_error;
        } else {
            ui_state->detection_error.clear();
            if (ui_state->detection_status.empty()) {
                ui_state->detection_status = "Seeded registration from detected experimental area.";
            } else {
                ui_state->detection_status += " Applied to registration.";
            }
        }
    }
    ImGui::EndDisabled();

    ImGui::BeginDisabled(!ui_state->has_capture || actions.reset_registration_from_frame == nullptr);
    if (ImGui::Button("Reset registration from frame")) {
        actions.reset_registration_from_frame(ui_state);
    }
    ImGui::EndDisabled();
}

} // namespace

bool detect_experimental_area_circle_from_capture(
    SpatialLayoutUiState* ui_state,
    std::string* error_out)
{
    if (ui_state == nullptr) {
        if (error_out) {
            *error_out = "Experimental-area detection requires non-null UI state.";
        }
        return false;
    }
    if (!ui_state->has_capture || ui_state->captured_texture_width <= 0 || ui_state->captured_texture_height <= 0 ||
        ui_state->captured_rgba.empty()) {
        if (error_out) {
            *error_out = "Capture a frame before running experimental-area detection.";
        }
        return false;
    }

    cv::Mat rgba(ui_state->captured_texture_height,
                 ui_state->captured_texture_width,
                 CV_8UC4,
                 ui_state->captured_rgba.data());
    cv::Mat gray;
    cv::cvtColor(rgba, gray, cv::COLOR_RGBA2GRAY);

    ui_state->hough_dp = std::clamp(ui_state->hough_dp, 1.0, 3.0);
    ui_state->hough_min_dist_fraction =
        std::clamp(ui_state->hough_min_dist_fraction, 0.01, 2.0);
    ui_state->hough_param1 = std::clamp(ui_state->hough_param1, 1.0, 500.0);
    ui_state->hough_param2 = std::clamp(ui_state->hough_param2, 1.0, 500.0);
    ui_state->hough_min_radius_fraction =
        std::clamp(ui_state->hough_min_radius_fraction, 0.001, 1.0);
    ui_state->hough_max_radius_fraction =
        std::clamp(ui_state->hough_max_radius_fraction, 0.001, 1.5);
    if (ui_state->hough_max_radius_fraction < ui_state->hough_min_radius_fraction) {
        std::swap(ui_state->hough_max_radius_fraction, ui_state->hough_min_radius_fraction);
    }
    ui_state->hough_max_detection_dimension_px =
        std::clamp(ui_state->hough_max_detection_dimension_px, 256, 8192);
    ui_state->hough_median_blur_ksize =
        std::clamp(ui_state->hough_median_blur_ksize, 1, 31);
    if ((ui_state->hough_median_blur_ksize % 2) == 0) {
        ++ui_state->hough_median_blur_ksize;
    }

    cv::Mat detection_gray = gray;
    double detection_scale = 1.0;
    const int max_dim = std::max(gray.cols, gray.rows);
    if (max_dim > ui_state->hough_max_detection_dimension_px) {
        detection_scale =
            static_cast<double>(ui_state->hough_max_detection_dimension_px) /
            static_cast<double>(max_dim);
        cv::resize(gray, detection_gray, cv::Size(), detection_scale, detection_scale, cv::INTER_AREA);
    }

    cv::Mat blurred;
    if (ui_state->hough_median_blur_ksize > 1) {
        cv::medianBlur(detection_gray, blurred, ui_state->hough_median_blur_ksize);
    } else {
        blurred = detection_gray;
    }

    const double min_dim = static_cast<double>(std::min(blurred.cols, blurred.rows));
    if (min_dim < 32.0) {
        if (error_out) {
            *error_out = "Experimental-area detection requires a larger captured frame.";
        }
        return false;
    }

    const int min_radius = std::max(
        1,
        static_cast<int>(std::round(min_dim * ui_state->hough_min_radius_fraction)));
    const int max_radius = std::max(
        min_radius + 1,
        static_cast<int>(std::round(min_dim * ui_state->hough_max_radius_fraction)));
    const double min_dist =
        std::max(1.0, min_dim * ui_state->hough_min_dist_fraction);

    std::vector<cv::Vec3f> circles;
    try {
        cv::HoughCircles(
            blurred,
            circles,
            cv::HOUGH_GRADIENT,
            ui_state->hough_dp,
            min_dist,
            ui_state->hough_param1,
            ui_state->hough_param2,
            min_radius,
            max_radius);

        if (circles.empty() && ui_state->hough_fallback_enabled) {
            cv::HoughCircles(
                blurred,
                circles,
                cv::HOUGH_GRADIENT,
                std::max(1.0, ui_state->hough_dp * 0.96),
                min_dist,
                std::max(1.0, ui_state->hough_param1 * 0.75),
                std::max(1.0, ui_state->hough_param2 * 0.73),
                min_radius,
                max_radius);
        }
    } catch (const cv::Exception& ex) {
        if (error_out) {
            *error_out = std::string("Hough circle detection failed: ") + ex.what();
        }
        return false;
    }

    if (circles.empty()) {
        if (error_out) {
            *error_out = "No experimental-area circle was detected in the captured frame.";
        }
        return false;
    }

    const Point2d image_center = make_point(blurred.cols * 0.5, blurred.rows * 0.5);
    double best_score = std::numeric_limits<double>::lowest();
    cv::Vec3f best_circle = circles.front();
    for (const cv::Vec3f& circle : circles) {
        const double dx = static_cast<double>(circle[0]) - image_center.x;
        const double dy = static_cast<double>(circle[1]) - image_center.y;
        const double dist = std::sqrt(dx * dx + dy * dy);
        const double score = static_cast<double>(circle[2]) - 0.35 * dist;
        if (score > best_score) {
            best_score = score;
            best_circle = circle;
        }
    }

    const double inv_scale = 1.0 / detection_scale;
    ui_state->detected_experimental_area_geometry =
        runtime_circle(
            static_cast<double>(best_circle[0]) * inv_scale,
            static_cast<double>(best_circle[1]) * inv_scale,
            std::max(
                1.0,
                static_cast<double>(best_circle[2]) * inv_scale +
                    ui_state->hough_radius_adjustment_px));
    ui_state->has_detected_experimental_area_circle = true;
    ui_state->detection_error.clear();

    std::ostringstream status;
    status << "Detected experimental-area circle at ("
           << std::lround(ui_state->detected_experimental_area_geometry.circle.cx) << ", "
           << std::lround(ui_state->detected_experimental_area_geometry.circle.cy) << ")"
           << " r=" << std::lround(ui_state->detected_experimental_area_geometry.circle.r);
    if (circles.size() > 1) {
        status << " from " << circles.size() << " Hough candidates";
    }
    status << " using dp=" << ui_state->hough_dp
           << " param1=" << ui_state->hough_param1
           << " param2=" << ui_state->hough_param2
           << " radius=[" << std::lround(static_cast<double>(min_radius) * inv_scale)
           << "," << std::lround(static_cast<double>(max_radius) * inv_scale) << "]";
    ui_state->detection_status = status.str();
    return true;
}

bool seed_registration_from_detected_experimental_area_circle(
    SpatialLayoutUiState* ui_state,
    std::string* error_out)
{
    if (ui_state == nullptr) {
        if (error_out) {
            *error_out = "Registration seeding requires non-null UI state.";
        }
        return false;
    }
    if (!ui_state->has_detected_experimental_area_circle) {
        if (error_out) {
            *error_out = "Run experimental-area detection before seeding registration.";
        }
        return false;
    }

    const orange::spatial::LayoutGeometry& canonical_outer =
        ui_state->layout_artifact.layout.outer_geometry;
    if (canonical_outer.type != orange::spatial::LayoutGeometryType::kCircle ||
        canonical_outer.circle.r <= 0.0) {
        if (error_out) {
            *error_out =
                "Experimental-area circle seeding currently requires a circular canonical experimental area.";
        }
        return false;
    }

    ui_state->registration.type = orange::spatial::RegistrationType::kSimilarity;
    ui_state->registration.source = orange::spatial::RegistrationSource::kDetectedFit;
    ui_state->registration.fit_point_count = 3;
    ui_state->registration.residual_px = 0.0;
    ui_state->registration.has_orientation_status = true;
    ui_state->registration.orientation_status = orange::spatial::OrientationStatus::kAmbiguous;

    const double detected_radius = ui_state->detected_experimental_area_geometry.circle.r;
    const double scale = std::max(1e-6, detected_radius / canonical_outer.circle.r);
    const Point2d desired_center = make_point(
        ui_state->detected_experimental_area_geometry.circle.cx,
        ui_state->detected_experimental_area_geometry.circle.cy);
    const double theta = ui_state->registration_rotation_deg_clockwise * kPi / 180.0;
    const double rotated_center_x =
        scale * (std::cos(theta) * canonical_outer.circle.cx - std::sin(theta) * canonical_outer.circle.cy);
    const double rotated_center_y =
        scale * (std::sin(theta) * canonical_outer.circle.cx + std::cos(theta) * canonical_outer.circle.cy);

    ui_state->registration.type = orange::spatial::RegistrationType::kSimilarity;
    ui_state->registration_scale = scale;
    ui_state->registration_tx_px = desired_center.x - rotated_center_x;
    ui_state->registration_ty_px = desired_center.y - rotated_center_y;
    return true;
}

void render_hough_circle_tuning(
    SpatialLayoutUiState* ui_state,
    const HoughCirclePanelActions& actions)
{
    if (ui_state == nullptr) {
        return;
    }
    if (!ImGui::CollapsingHeader("Hough Circle Tuning", ImGuiTreeNodeFlags_DefaultOpen)) {
        return;
    }
    render_hough_registration_actions(ui_state, actions);
    ImGui::Separator();
    if (ImGui::Button("Reset Hough Defaults")) {
        reset_hough_defaults(ui_state);
    }
    ImGui::Checkbox("Show Hough proposal overlay", &ui_state->show_hough_proposal_overlay);
    ImGui::Checkbox(
        "Show corrected Citrus outline overlay",
        &ui_state->show_citrus_corrected_center_overlay);
    ImGui::InputDouble("Hough dp", &ui_state->hough_dp, 0.05, 0.25, "%.3f");
    ImGui::InputDouble("Hough min distance fraction", &ui_state->hough_min_dist_fraction, 0.01, 0.05, "%.3f");
    ImGui::InputDouble("Hough param1", &ui_state->hough_param1, 5.0, 25.0, "%.1f");
    ImGui::InputDouble("Hough param2", &ui_state->hough_param2, 1.0, 5.0, "%.1f");
    ImGui::InputDouble("Hough min radius fraction", &ui_state->hough_min_radius_fraction, 0.01, 0.05, "%.3f");
    ImGui::InputDouble("Hough max radius fraction", &ui_state->hough_max_radius_fraction, 0.01, 0.05, "%.3f");
    ImGui::InputDouble("Hough radius adjustment px", &ui_state->hough_radius_adjustment_px, 1.0, 10.0, "%.2f");
    ImGui::InputInt("Hough median blur kernel", &ui_state->hough_median_blur_ksize, 2, 4);
    ImGui::InputInt("Hough max detection dimension px", &ui_state->hough_max_detection_dimension_px, 128, 512);
    ImGui::Checkbox("Hough fallback pass", &ui_state->hough_fallback_enabled);

    if (ui_state->has_detected_experimental_area_circle &&
        ui_state->detected_experimental_area_geometry.type == orange::spatial::RuntimeGeometryType::kCircle) {
        bool edited_detection = false;
        edited_detection |= ImGui::InputDouble(
            "Detected circle cx",
            &ui_state->detected_experimental_area_geometry.circle.cx,
            0.5,
            5.0,
            "%.2f");
        edited_detection |= ImGui::InputDouble(
            "Detected circle cy",
            &ui_state->detected_experimental_area_geometry.circle.cy,
            0.5,
            5.0,
            "%.2f");
        edited_detection |= ImGui::InputDouble(
            "Detected circle r",
            &ui_state->detected_experimental_area_geometry.circle.r,
            0.5,
            5.0,
            "%.2f");
        ui_state->detected_experimental_area_geometry.circle.r =
            std::max(1.0, ui_state->detected_experimental_area_geometry.circle.r);
        if (edited_detection) {
            ui_state->detection_error.clear();
            ui_state->detection_status = "Edited detected experimental-area circle.";
        }
    }
}

} // namespace orange::gui::spatial_layout
