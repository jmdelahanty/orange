#include "gui/guided_capture_autorun.h"

#include "gui/env_util.h"

#include <algorithm>
#include <cstdlib>
#include <set>
#include <sstream>
#include <utility>

namespace orange::gui {
namespace {

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
            out.push_back(std::move(item));
        }
    }
    return out;
}

std::vector<std::string> split_ordered_csv(const std::string& value)
{
    std::vector<std::string> out;
    std::istringstream stream(value);
    std::string item;
    while (std::getline(stream, item, ',')) {
        const auto first = item.find_first_not_of(" \t\r\n");
        const auto last = item.find_last_not_of(" \t\r\n");
        if (first != std::string::npos) {
            out.push_back(item.substr(first, last - first + 1));
        }
    }
    return out;
}

std::vector<std::uint8_t> split_gray_csv(const std::string& value)
{
    std::vector<std::uint8_t> out;
    for (const std::string& item : split_csv(value)) {
        try {
            const int gray = std::stoi(item);
            if (gray >= 0 && gray <= 255 &&
                std::find(out.begin(), out.end(), static_cast<std::uint8_t>(gray)) == out.end()) {
                out.push_back(static_cast<std::uint8_t>(gray));
            }
        } catch (const std::exception&) {
        }
    }
    return out;
}

std::string env_string(const char* name)
{
    const char* value = std::getenv(name);
    return value == nullptr ? std::string() : std::string(value);
}

std::string default_purpose_for_recipe(const std::string& recipe)
{
    if (recipe == "uniform_gray") {
        return "projected_surface_scale_calibration";
    }
    if (recipe == "arena_outline") {
        return "arena_projection";
    }
    if (recipe == "experimental_area_center_and_outline") {
        return "crosshair_alignment";
    }
    if (recipe == "homography_grid" || recipe == "homography_rings") {
        return "homography_grid";
    }
    if (recipe == "verification_dots") {
        return "verification_dots";
    }
    return "diagnostic_black_reference";
}

std::string default_recipe_for_profile(const std::string& profile_id)
{
    if (profile_id == "unobstructed_canvas_commissioning") {
        return "homography_grid";
    }
    if (profile_id == "holder_installed_projected_surface" ||
        profile_id == "wet_tank_projected_surface") {
        return "homography_rings";
    }
    if (profile_id == "installed_tank_registration") {
        return "experimental_area_center_and_outline";
    }
    return {};
}

}  // namespace

GuidedCaptureAutorunConfig resolve_guided_capture_autorun_config()
{
    GuidedCaptureAutorunConfig config;
    config.enabled = gui_env_flag_enabled("ORANGE_GUI_GUIDED_CAPTURE_AUTORUN", false);
    config.save_captures =
        gui_env_flag_enabled("ORANGE_GUI_GUIDED_CAPTURE_SAVE", false);
    config.exit_after_completion =
        gui_env_flag_enabled("ORANGE_GUI_GUIDED_CAPTURE_EXIT_AFTER_COMPLETION", true);
    config.citrus_config_path =
        env_string("ORANGE_GUI_GUIDED_CAPTURE_CITRUS_CONFIG_PATH");
    config.workflow_profile_id =
        env_string("ORANGE_GUI_GUIDED_CAPTURE_PROFILE");
    const std::string recipe = env_string("ORANGE_GUI_GUIDED_CAPTURE_RECIPE");
    if (!recipe.empty()) {
        config.recipe = recipe;
    } else if (!config.workflow_profile_id.empty()) {
        const std::string profile_recipe =
            default_recipe_for_profile(config.workflow_profile_id);
        if (!profile_recipe.empty()) {
            config.recipe = profile_recipe;
        }
    }
    config.purpose = env_string("ORANGE_GUI_GUIDED_CAPTURE_PURPOSE");
    if (config.purpose.empty()) {
        config.purpose = default_purpose_for_recipe(config.recipe);
    }
    config.camera_serials =
        split_csv(env_string("ORANGE_GUI_GUIDED_CAPTURE_CAMERAS"));
    config.frame_count = static_cast<std::uint32_t>(std::clamp(
        gui_env_int("ORANGE_GUI_GUIDED_CAPTURE_FRAME_COUNT", 1, 1), 1, 600));
    config.apply_calibration_preflight = gui_env_flag_enabled(
        "ORANGE_GUI_GUIDED_CAPTURE_APPLY_CALIBRATION_PREFLIGHT", true);
    config.calibration_frame_rate_hz = static_cast<std::uint32_t>(std::clamp(
        gui_env_int("ORANGE_GUI_GUIDED_CAPTURE_FRAME_RATE_HZ", 5, 1), 1, 1000));
    config.calibration_exposure_us = static_cast<std::uint32_t>(std::clamp(
        gui_env_int("ORANGE_GUI_GUIDED_CAPTURE_EXPOSURE_US", 100000, 1),
        1,
        1000000));
    config.foreground_gray_u8 = static_cast<std::uint8_t>(std::clamp(
        gui_env_int("ORANGE_GUI_GUIDED_CAPTURE_FOREGROUND_GRAY_U8", 255, 0),
        0,
        255));
    config.recipe_sequence = split_ordered_csv(
        env_string("ORANGE_GUI_GUIDED_CAPTURE_RECIPE_SEQUENCE"));
    const std::string fixture_aperture_shape =
        env_string("ORANGE_GUI_FIXTURE_APERTURE_SHAPE");
    if (fixture_aperture_shape == "circle" ||
        fixture_aperture_shape == "rectangle" ||
        fixture_aperture_shape == "rounded_rectangle" ||
        fixture_aperture_shape == "polygon" ||
        fixture_aperture_shape == "unknown") {
        config.fixture_aperture_shape = fixture_aperture_shape;
    }
    config.sweep_foreground_grays_u8 = split_gray_csv(
        env_string("ORANGE_GUI_GUIDED_CAPTURE_SWEEP_FOREGROUND_GRAYS_U8"));
    config.sweep_repeats = std::clamp(
        gui_env_int("ORANGE_GUI_GUIDED_CAPTURE_SWEEP_REPEATS", 1, 1), 1, 100);
    config.include_arena_outline_reference = gui_env_flag_enabled(
        "ORANGE_GUI_GUIDED_CAPTURE_INCLUDE_ARENA_OUTLINE_REFERENCE", false);
    config.projected_surface_targets_ready_confirmed = gui_env_flag_enabled(
        "ORANGE_GUI_PROJECTED_SURFACE_SCALE_TARGETS_READY", false);
    config.accept_projected_surface_scales_armed = gui_env_flag_enabled(
        "ORANGE_GUI_ACCEPT_PROJECTED_SURFACE_SCALES_ARMED", false);
    config.fit_homographies_after_capture = gui_env_flag_enabled(
        "ORANGE_GUI_GUIDED_CAPTURE_FIT_HOMOGRAPHIES", false);
    config.startup_timeout_seconds = std::clamp(
        gui_env_int("ORANGE_GUI_GUIDED_CAPTURE_STARTUP_TIMEOUT_SECONDS", 120, 1),
        1,
        600);
    config.workflow_timeout_seconds = std::clamp(
        gui_env_int("ORANGE_GUI_GUIDED_CAPTURE_WORKFLOW_TIMEOUT_SECONDS", 60, 1),
        1,
        600);
    config.result_json_path =
        env_string("ORANGE_GUI_GUIDED_CAPTURE_RESULT_JSON");
    if (config.result_json_path.empty()) {
        config.result_json_path = "/tmp/orange_gui_guided_capture_result.json";
    }
    return config;
}

const char* guided_capture_autorun_stage_name(const GuidedCaptureAutorunStage stage)
{
    switch (stage) {
        case GuidedCaptureAutorunStage::kDisabled: return "disabled";
        case GuidedCaptureAutorunStage::kWaitForStream: return "wait_for_stream";
        case GuidedCaptureAutorunStage::kPrepare: return "prepare";
        case GuidedCaptureAutorunStage::kWaitForPreflightSettle: return "wait_for_preflight_settle";
        case GuidedCaptureAutorunStage::kWaitForCapture: return "wait_for_capture";
        case GuidedCaptureAutorunStage::kQueueSave: return "queue_save";
        case GuidedCaptureAutorunStage::kWaitForSave: return "wait_for_save";
        case GuidedCaptureAutorunStage::kRequestHomographyFit: return "request_homography_fit";
        case GuidedCaptureAutorunStage::kWaitForHomographyFit: return "wait_for_homography_fit";
        case GuidedCaptureAutorunStage::kReleaseHomographyCandidate: return "release_homography_candidate";
        case GuidedCaptureAutorunStage::kWaitForHomographyRelease: return "wait_for_homography_release";
        case GuidedCaptureAutorunStage::kAnalyzeProjectedSurfaceScale: return "analyze_projected_surface_scale";
        case GuidedCaptureAutorunStage::kRequestProjectedSurfaceScaleFit: return "request_projected_surface_scale_fit";
        case GuidedCaptureAutorunStage::kWaitForProjectedSurfaceScaleFit: return "wait_for_projected_surface_scale_fit";
        case GuidedCaptureAutorunStage::kWaitForProjectedSurfaceScaleReview: return "wait_for_projected_surface_scale_review";
        case GuidedCaptureAutorunStage::kWaitForProjectedSurfaceScalePromotion: return "wait_for_projected_surface_scale_promotion";
        case GuidedCaptureAutorunStage::kStopStream: return "stop_stream";
        case GuidedCaptureAutorunStage::kWaitForStreamStop: return "wait_for_stream_stop";
        case GuidedCaptureAutorunStage::kWriteResult: return "write_result";
        case GuidedCaptureAutorunStage::kDone: return "done";
        case GuidedCaptureAutorunStage::kFailed: return "failed";
    }
    return "unknown";
}

void guided_capture_autorun_start(GuidedCaptureAutorunState* state,
                                  const GuidedCaptureAutorunConfig& config)
{
    if (state == nullptr) {
        return;
    }
    *state = GuidedCaptureAutorunState{};
    if (!config.enabled) {
        return;
    }
    state->run_started_at = std::chrono::steady_clock::now();
    state->stage = GuidedCaptureAutorunStage::kWaitForStream;
    state->stage_started_at = state->run_started_at;
}

}  // namespace orange::gui
