#include "gui/spatial_layout/daily_registration_preview.h"

#include <cmath>
#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>

namespace spatial = orange::gui::spatial_layout;

namespace {

void Require(bool condition, const std::string& message)
{
    if (!condition) throw std::runtime_error(message);
}

void RequireNear(
    double actual,
    double expected,
    double tolerance,
    const std::string& message)
{
    if (std::abs(actual - expected) > tolerance) {
        throw std::runtime_error(
            message + ": actual=" + std::to_string(actual) +
            " expected=" + std::to_string(expected));
    }
}

CitrusSpatialTemplateState MakeTemplate()
{
    CitrusSpatialTemplateState result;
    result.available = true;
    result.source_camera_id = "cam1";
    result.source_config_name = "arena_1";
    result.source_canvas_name = "shadow";
    result.experimental_area_radius_px = 40.0;
    result.has_canvas_to_camera_homography = true;
    result.has_authoritative_camera_to_canvas_homography = true;
    result.homography_authority_status = "accepted_compatible";
    result.homography_target_plane = "projected_surface";
    result.homography_candidate_id = "homography_cam1";
    return result;
}

nlohmann::json MakeSelectedStatus()
{
    nlohmann::json target = {
        {"rim_center_camera_px", {{"x", 100.0}, {"y", 200.0}}},
        {"observed_rim_radius_camera_px", 50.0},
        {"desired_experimental_area_center_canvas_px",
         {{"x", 100.2}, {"y", 199.8}}},
        {"effective_experimental_area_center_canvas_px",
         {{"x", 100.0}, {"y", 200.0}}},
        {"homography", {
            {"candidate_id", "homography_cam1"},
            {"selected_canvas_name", "shadow"}}},
        {"rim_observation", {
            {"artifact_id", "dishrim_cam1"},
            {"path", "/calibration/dishrim_cam1/observation.json"},
            {"sha256", "sha256:rim"}}}
    };
    nlohmann::json runtime_target = {
        {"camera_id", "cam1"},
        {"arena_id", "arena_1"},
        {"state", "selected_valid"},
        {"applied", true},
        {"registration_id", "dailyreg_1"},
        {"registration_path", "/calibration/dailyreg_1/registration.json"},
        {"registration_sha256", "sha256:registration"},
        {"target", std::move(target)}
    };
    return {
        {"schema_id", "citrus.daily_registration.status"},
        {"schema_version", 1},
        {"transaction_id", "dailyregtxn_1"},
        {"runtime", {
            {"schema_id",
             "citrus.runtime_daily_registration_compatibility"},
            {"schema_version", 1},
            {"mode", "selected_daily_registration"},
            {"targets", nlohmann::json::array({std::move(runtime_target)})}
        }}
    };
}

void TestSelectedRuntimeResolvesExactGeometry()
{
    SpatialLayoutUiState state;
    state.captured_camera_serial = "cam1";
    state.citrus_canvas_templates.push_back(MakeTemplate());
    state.daily_registration_status = MakeSelectedStatus();

    spatial::DailyRegistrationPreviewOverlay overlay;
    Require(
        spatial::resolve_daily_registration_preview_overlay(state, &overlay),
        "selected compatible runtime should resolve");
    Require(overlay.available, "resolved overlay should be available");
    Require(overlay.selected_for_runtime,
            "runtime overlay should be marked selected");
    Require(!overlay.has_raw_hough_circle,
            "runtime status exposes accepted rim, not a separate raw Hough fit");
    Require(overlay.registration_id == "dailyreg_1",
            "registration identity should be retained");
    Require(overlay.rim_artifact_id == "dishrim_cam1",
            "rim artifact identity should be retained");
    RequireNear(
        overlay.accepted_rim_circle.circle.cx, 100.0, 1e-9,
        "accepted rim center x should be retained");
    RequireNear(
        overlay.accepted_rim_circle.circle.r, 50.0, 1e-9,
        "accepted rim radius should be retained");
    RequireNear(
        overlay.registered_center_camera_px.x, 100.0, 1e-9,
        "effective registered center x should be inverse-projected");
    RequireNear(
        overlay.registered_center_camera_px.y, 200.0, 1e-9,
        "effective registered center y should be inverse-projected");
    Require(overlay.registered_outline_camera_px.size() == 96,
            "canonical registered outline should keep all review samples");
}

void TestHomographyIdentityMismatchFailsClosed()
{
    SpatialLayoutUiState state;
    state.captured_camera_serial = "cam1";
    state.citrus_canvas_templates.push_back(MakeTemplate());
    state.daily_registration_status = MakeSelectedStatus();
    state.daily_registration_status["runtime"]["targets"][0]["target"]
        ["homography"]["candidate_id"] = "different_homography";

    spatial::DailyRegistrationPreviewOverlay overlay;
    Require(
        !spatial::resolve_daily_registration_preview_overlay(state, &overlay),
        "mismatched homography identity must not produce an overlay");
    Require(!overlay.available,
            "failed identity check must leave the overlay unavailable");
}

void TestCompletedWorkflowRetainsRawAndAcceptedFits()
{
    SpatialLayoutUiState state;
    state.captured_camera_serial = "cam1";
    auto& workflow = state.daily_registration_workflow;
    workflow.stage = "complete";
    workflow.transaction_id = "dailyregtxn_workflow";
    workflow.accepted_registration_path = "/tmp/registration.json";
    DailyRegistrationTargetUiState target;
    target.camera_serial = "cam1";
    target.detected_rim_center_x_camera_px = 99.0;
    target.detected_rim_center_y_camera_px = 201.0;
    target.detected_rim_radius_camera_px = 50.5;
    target.accepted_rim_center_x_camera_px = 100.0;
    target.accepted_rim_center_y_camera_px = 200.0;
    target.accepted_rim_radius_camera_px = 50.0;
    target.geometry_corrected_center_x_camera_px = 100.25;
    target.geometry_corrected_center_y_camera_px = 199.75;
    target.geometry_outline_camera_px = {
        {60.0, 200.0}, {100.0, 160.0}, {140.0, 200.0}};
    target.rim_observation_artifact_id = "dishrim_workflow";
    workflow.targets.push_back(target);

    spatial::DailyRegistrationPreviewOverlay overlay;
    Require(
        spatial::resolve_daily_registration_preview_overlay(state, &overlay),
        "completed in-memory workflow should resolve before runtime fallback");
    Require(overlay.selected_for_runtime,
            "completed accepted workflow should be marked selected");
    Require(overlay.has_raw_hough_circle,
            "workflow review should preserve its separate raw Hough fit");
    RequireNear(
        overlay.raw_hough_circle.circle.cx, 99.0, 1e-9,
        "raw Hough fit should remain distinct");
    RequireNear(
        overlay.accepted_rim_circle.circle.cx, 100.0, 1e-9,
        "accepted fit should remain distinct");
    Require(overlay.registered_outline_camera_px.size() == 3,
            "workflow's exact reviewed outline should be reused");
}

void TestNullAndMalformedStatusDoNotThrow()
{
    SpatialLayoutUiState state;
    state.captured_camera_serial = "cam1";
    state.daily_registration_status = nullptr;
    state.captured_citrus_daily_registration_pre_capture = nullptr;
    state.captured_citrus_daily_registration_post_capture = nullptr;
    state.daily_registration_workflow.citrus_runtime_selection_status =
        nullptr;

    spatial::DailyRegistrationPreviewOverlay overlay;
    Require(
        !spatial::resolve_daily_registration_preview_overlay(state, &overlay),
        "null status values should fail closed without throwing");
}

}  // namespace

int main()
{
    try {
        TestSelectedRuntimeResolvesExactGeometry();
        TestHomographyIdentityMismatchFailsClosed();
        TestCompletedWorkflowRetainsRawAndAcceptedFits();
        TestNullAndMalformedStatusDoNotThrow();
        std::cout << "daily registration preview tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "daily registration preview tests failed: "
                  << error.what() << '\n';
        return 1;
    }
}
