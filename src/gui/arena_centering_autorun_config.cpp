#include "gui/arena_centering_autorun.h"

#include "gui/env_util.h"

#include <algorithm>
#include <cstdlib>
#include <set>
#include <sstream>

namespace orange::gui {
namespace {

std::string env_string(const char* name)
{
    const char* value = std::getenv(name);
    return value == nullptr ? std::string() : std::string(value);
}

std::vector<std::string> split_csv(const std::string& value)
{
    std::vector<std::string> out;
    std::set<std::string> seen;
    std::istringstream stream(value);
    std::string item;
    while (std::getline(stream, item, ',')) {
        const auto first = item.find_first_not_of(" \t\r\n");
        const auto last = item.find_last_not_of(" \t\r\n");
        if (first == std::string::npos) {
            continue;
        }
        item = item.substr(first, last - first + 1);
        if (seen.insert(item).second) {
            out.push_back(item);
        }
    }
    return out;
}

double env_double(const char* name, double fallback)
{
    const std::string value = env_string(name);
    if (value.empty()) {
        return fallback;
    }
    try {
        return std::stod(value);
    } catch (const std::exception&) {
        return fallback;
    }
}

}  // namespace

ArenaCenteringAutorunConfig resolve_arena_centering_autorun_config()
{
    ArenaCenteringAutorunConfig config;
    config.enabled = gui_env_flag_enabled(
        "ORANGE_GUI_ARENA_CENTERING_AUTORUN", false);
    config.save_captures = gui_env_flag_enabled(
        "ORANGE_GUI_ARENA_CENTERING_SAVE_CAPTURES", true);
    config.save_verified_centers_armed = gui_env_flag_enabled(
        "ORANGE_GUI_ARENA_CENTERING_SAVE_VERIFIED_CENTERS_ARMED", false);
    config.resize_arenas = gui_env_flag_enabled(
        "ORANGE_GUI_ARENA_CENTERING_RESIZE_ARENAS", false);
    config.save_verified_layout_armed = gui_env_flag_enabled(
        "ORANGE_GUI_ARENA_CENTERING_SAVE_VERIFIED_LAYOUT_ARMED", false);
    config.fit_homographies_after_centering = gui_env_flag_enabled(
        "ORANGE_GUI_ARENA_CENTERING_FIT_HOMOGRAPHIES", false);
    config.accept_homographies_armed = gui_env_flag_enabled(
        "ORANGE_GUI_ARENA_CENTERING_ACCEPT_HOMOGRAPHIES_ARMED", false);
    config.exit_after_completion = gui_env_flag_enabled(
        "ORANGE_GUI_ARENA_CENTERING_EXIT_AFTER_COMPLETION", true);
    config.citrus_config_path = env_string(
        "ORANGE_GUI_ARENA_CENTERING_CITRUS_CONFIG_PATH");
    config.camera_serials = split_csv(env_string(
        "ORANGE_GUI_ARENA_CENTERING_CAMERAS"));
    config.frame_count = static_cast<std::uint32_t>(std::clamp(
        gui_env_int("ORANGE_GUI_ARENA_CENTERING_FRAME_COUNT", 1, 1), 1, 64));
    config.foreground_gray_u8 = static_cast<std::uint8_t>(std::clamp(
        gui_env_int("ORANGE_GUI_ARENA_CENTERING_FOREGROUND_GRAY_U8", 72, 0),
        0,
        255));
    config.symmetric_probe_canvas_px = std::clamp(
        gui_env_int("ORANGE_GUI_ARENA_CENTERING_PROBE_CANVAS_PX", 3, 1),
        1,
        32);
    config.verification_tolerance_camera_px = std::clamp(
        env_double("ORANGE_GUI_ARENA_CENTERING_VERIFICATION_TOLERANCE_CAMERA_PX", 2.0),
        0.1,
        50.0);
    config.maximum_refinement_canvas_px = std::clamp(
        env_double("ORANGE_GUI_ARENA_CENTERING_MAX_REFINEMENT_CANVAS_PX", 4.0),
        0.0,
        32.0);
    config.rectangle_safety_margin_camera_px = std::clamp(
        env_double("ORANGE_GUI_ARENA_CENTERING_RECTANGLE_SAFETY_MARGIN_CAMERA_PX", 32.0),
        4.0,
        500.0);
    config.rectangle_center_marker_tolerance_camera_px = std::clamp(
        env_double("ORANGE_GUI_ARENA_CENTERING_RECTANGLE_CENTER_TOLERANCE_CAMERA_PX", 20.0),
        1.0,
        200.0);
    config.rectangle_prediction_tolerance_camera_px = std::clamp(
        env_double("ORANGE_GUI_ARENA_CENTERING_RECTANGLE_PREDICTION_TOLERANCE_CAMERA_PX", 20.0),
        1.0,
        200.0);
    config.rectangle_maximality_slack_camera_px = std::clamp(
        env_double("ORANGE_GUI_ARENA_CENTERING_RECTANGLE_MAXIMALITY_SLACK_CAMERA_PX", 24.0),
        1.0,
        200.0);
    config.maximum_arena_scale_change_fraction = std::clamp(
        env_double("ORANGE_GUI_ARENA_CENTERING_MAX_ARENA_SCALE_CHANGE_FRACTION", 0.20),
        0.001,
        1.0);
    config.maximum_ptp_capture_span_ns = static_cast<std::uint64_t>(std::clamp(
        gui_env_int("ORANGE_GUI_ARENA_CENTERING_MAX_PTP_SPAN_NS", 1000, 1),
        1,
        100000000));
    config.apply_calibration_preflight = gui_env_flag_enabled(
        "ORANGE_GUI_ARENA_CENTERING_APPLY_CALIBRATION_PREFLIGHT", true);
    config.calibration_frame_rate_hz = static_cast<std::uint32_t>(std::clamp(
        gui_env_int("ORANGE_GUI_ARENA_CENTERING_FRAME_RATE_HZ", 5, 1), 1, 1000));
    config.calibration_exposure_us = static_cast<std::uint32_t>(std::clamp(
        gui_env_int("ORANGE_GUI_ARENA_CENTERING_EXPOSURE_US", 100000, 1),
        1,
        1000000));
    config.preflight_settle_milliseconds = std::clamp(
        gui_env_int("ORANGE_GUI_ARENA_CENTERING_PREFLIGHT_SETTLE_MS", 1000, 0),
        0,
        30000);
    config.projection_settle_milliseconds = std::clamp(
        gui_env_int("ORANGE_GUI_ARENA_CENTERING_PROJECTION_SETTLE_MS", 1000, 0),
        0,
        30000);
    config.require_projection_stability_capture = gui_env_flag_enabled(
        "ORANGE_GUI_ARENA_CENTERING_REQUIRE_STABILITY_CAPTURE", true);
    config.projection_stability_interval_milliseconds = std::clamp(
        gui_env_int(
            "ORANGE_GUI_ARENA_CENTERING_STABILITY_INTERVAL_MS", 300, 0),
        0,
        30000);
    config.projection_stability_max_capture_attempts = std::clamp(
        gui_env_int(
            "ORANGE_GUI_ARENA_CENTERING_STABILITY_MAX_CAPTURE_ATTEMPTS",
            5,
            2),
        2,
        10);
    config.projection_stability_max_center_delta_camera_px = std::clamp(
        env_double(
            "ORANGE_GUI_ARENA_CENTERING_STABILITY_MAX_CENTER_DELTA_CAMERA_PX",
            2.0),
        0.1,
        50.0);
    config.projection_stability_max_corner_delta_camera_px = std::clamp(
        env_double(
            "ORANGE_GUI_ARENA_CENTERING_STABILITY_MAX_CORNER_DELTA_CAMERA_PX",
            12.0),
        0.1,
        100.0);
    config.homography_maximum_rms_reprojection_error_canvas_px = std::clamp(
        env_double("ORANGE_GUI_HOMOGRAPHY_MAXIMUM_RMS_ERROR_CANVAS_PX", 0.5),
        0.01,
        20.0);
    config.homography_maximum_point_reprojection_error_canvas_px = std::clamp(
        env_double("ORANGE_GUI_HOMOGRAPHY_MAXIMUM_POINT_ERROR_CANVAS_PX", 1.5),
        0.01,
        50.0);
    config.homography_minimum_inlier_ratio = std::clamp(
        env_double("ORANGE_GUI_HOMOGRAPHY_MINIMUM_INLIER_RATIO", 0.95),
        0.01,
        1.0);
    config.homography_maximum_holdout_rms_error_canvas_px = std::clamp(
        env_double("ORANGE_GUI_HOMOGRAPHY_MAXIMUM_HOLDOUT_RMS_ERROR_CANVAS_PX", 0.75),
        0.01,
        20.0);
    config.homography_maximum_holdout_error_canvas_px = std::clamp(
        env_double("ORANGE_GUI_HOMOGRAPHY_MAXIMUM_HOLDOUT_ERROR_CANVAS_PX", 2.0),
        0.01,
        50.0);
    config.homography_saturation_pixel_threshold_u8 = std::clamp(
        gui_env_int("ORANGE_GUI_HOMOGRAPHY_SATURATION_PIXEL_THRESHOLD_U8", 250, 0),
        1,
        255);
    config.homography_maximum_dot_core_saturation_fraction = std::clamp(
        env_double(
            "ORANGE_GUI_HOMOGRAPHY_MAXIMUM_DOT_CORE_SATURATION_FRACTION",
            0.005),
        0.0,
        1.0);
    config.homography_minimum_dot_background_contrast_u8 = std::clamp(
        env_double(
            "ORANGE_GUI_HOMOGRAPHY_MINIMUM_DOT_BACKGROUND_CONTRAST_U8",
            20.0),
        0.0,
        255.0);
    config.projector_intensity_report_path = env_string(
        "ORANGE_GUI_ARENA_CENTERING_PROJECTOR_INTENSITY_REPORT_PATH");
    config.projector_intensity_report_sha256 = env_string(
        "ORANGE_GUI_ARENA_CENTERING_PROJECTOR_INTENSITY_REPORT_SHA256");
    config.startup_timeout_seconds = std::clamp(
        gui_env_int("ORANGE_GUI_ARENA_CENTERING_STARTUP_TIMEOUT_SECONDS", 120, 1),
        1,
        600);
    config.workflow_timeout_seconds = std::clamp(
        gui_env_int("ORANGE_GUI_ARENA_CENTERING_WORKFLOW_TIMEOUT_SECONDS", 90, 1),
        1,
        600);
    config.result_json_path = env_string(
        "ORANGE_GUI_ARENA_CENTERING_RESULT_JSON");
    if (config.result_json_path.empty()) {
        config.result_json_path = "/tmp/orange_gui_arena_centering_result.json";
    }
    return config;
}

}  // namespace orange::gui
