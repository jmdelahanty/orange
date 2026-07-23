#include "gui/spatial_layout/daily_registration_workflow.h"

#include "dish_top_rim_observation.h"
#include "fsuid_guard.h"
#include "gui/arena_centering_analysis.h"
#include "gui/daily_registration_geometry.h"
#include "gui/spatial_layout/calibration_workflow.h"
#include "gui/spatial_layout/citrus_import.h"
#include "gui/spatial_layout/group_capture_controller.h"
#include "gui/spatial_layout/metadata_panel.h"
#include "gui/spatial_layout/projection_snapshot_client.h"
#include "gui/spatial_layout/save_job_preparation.h"
#include "gui/spatial_layout/save_jobs.h"
#include "gui/spatial_layout/session_io.h"
#include "gui/spatial_layout/sha256.h"
#include "imgui.h"
#include "misc/cpp/imgui_stdlib.h"
#include "project.h"

#include <opencv2/imgproc.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <ctime>
#include <filesystem>
#include <future>
#include <iomanip>
#include <limits>
#include <set>
#include <sstream>
#include <utility>

#include <unistd.h>

namespace orange::gui::spatial_layout {
namespace {

namespace fs = std::filesystem;
namespace centering = orange::gui::arena_centering;
namespace daily_geometry = orange::gui::daily_registration;

constexpr const char* kRimOnlyAlignmentBasis =
    "commissioned_homography_and_canonical_experimental_center";

std::string JsonString(const nlohmann::json& value, const char* key)
{
    const auto it = value.find(key);
    return it != value.end() && it->is_string()
        ? it->get<std::string>()
        : std::string();
}

std::string NextOperationId(const std::string& action)
{
    static std::atomic<std::uint64_t> sequence{1};
    const auto ticks = std::chrono::steady_clock::now()
        .time_since_epoch().count();
    return "orange_daily_registration_" + sanitize_artifact_component(action) +
        "_" + std::to_string(static_cast<long long>(::getpid())) + "_" +
        std::to_string(static_cast<long long>(ticks)) + "_" +
        std::to_string(sequence.fetch_add(1, std::memory_order_relaxed));
}

std::string NextTransactionId(const std::string& created_utc)
{
    static std::atomic<std::uint64_t> sequence{1};
    const auto ticks = std::chrono::steady_clock::now()
        .time_since_epoch().count();
    return "dailyregtxn_" + sanitize_artifact_component(created_utc) + "_" +
        std::to_string(static_cast<long long>(::getpid())) + "_" +
        std::to_string(static_cast<long long>(ticks)) + "_" +
        std::to_string(sequence.fetch_add(1, std::memory_order_relaxed));
}

std::string DefaultValidUntilUtc()
{
    const std::time_t now = std::time(nullptr);
    std::tm local{};
    localtime_r(&now, &local);
    local.tm_hour = 0;
    local.tm_min = 0;
    local.tm_sec = 0;
    local.tm_mday += 1;
    const std::time_t next_local_midnight = std::mktime(&local);
    std::tm utc{};
    gmtime_r(&next_local_midnight, &utc);
    char buffer[32]{};
    std::strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%SZ", &utc);
    return buffer;
}

const CameraParams* FindCamera(
    const CameraParams* cameras,
    int count,
    const std::string& serial)
{
    if (cameras == nullptr) return nullptr;
    for (int index = 0; index < count; ++index) {
        if (cameras[index].camera_serial == serial) return &cameras[index];
    }
    return nullptr;
}

const SpatialLayoutGroupCaptureFrame* FindCapture(
    const SpatialLayoutUiState& ui_state,
    const std::string& serial)
{
    for (const auto& capture : ui_state.group_captures) {
        if (capture.valid && capture.camera_serial == serial) return &capture;
    }
    return nullptr;
}

std::vector<unsigned char> GrayFromRgba(
    const SpatialLayoutGroupCaptureFrame& capture)
{
    if (!capture.valid || capture.width <= 0 || capture.height <= 0 ||
        capture.rgba.size() != static_cast<std::size_t>(capture.width) *
            static_cast<std::size_t>(capture.height) * 4u) {
        return {};
    }
    cv::Mat rgba(capture.height, capture.width, CV_8UC4,
                 const_cast<unsigned char*>(capture.rgba.data()));
    cv::Mat gray;
    cv::cvtColor(rgba, gray, cv::COLOR_RGBA2GRAY);
    return std::vector<unsigned char>(
        gray.data, gray.data + gray.total() * gray.elemSize());
}

std::vector<unsigned char> DifferenceRgba(
    const SpatialLayoutGroupCaptureFrame& on,
    const std::vector<unsigned char>& black_gray)
{
    const std::size_t pixels =
        static_cast<std::size_t>(std::max(0, on.width)) *
        static_cast<std::size_t>(std::max(0, on.height));
    if (!on.valid || on.rgba.size() != pixels * 4u ||
        black_gray.size() != pixels) {
        return {};
    }
    std::vector<unsigned char> difference(pixels * 4u, 255u);
    for (std::size_t pixel = 0; pixel < pixels; ++pixel) {
        const std::size_t offset = pixel * 4u;
        const int current =
            (static_cast<int>(on.rgba[offset + 0]) +
             static_cast<int>(on.rgba[offset + 1]) +
             static_cast<int>(on.rgba[offset + 2])) / 3;
        const unsigned char delta = static_cast<unsigned char>(
            std::min(255, std::abs(current - static_cast<int>(black_gray[pixel]))));
        difference[offset + 0] = delta;
        difference[offset + 1] = delta;
        difference[offset + 2] = delta;
    }
    return difference;
}

bool DetectDailyRim(
    const SpatialLayoutGroupCaptureFrame& capture,
    const SpatialLayoutUiState& ui_state,
    orange::calibration::DishTopRimCircle* circle_out,
    std::vector<unsigned char>* gray_out,
    std::string* error_out)
{
    std::vector<unsigned char> gray_bytes = GrayFromRgba(capture);
    if (gray_bytes.empty()) {
        if (error_out) *error_out = "invalid_daily_rim_frame";
        return false;
    }
    cv::Mat full(capture.height, capture.width, CV_8UC1,
                 const_cast<unsigned char*>(gray_bytes.data()));
    cv::Mat detection = full;
    const int max_dimension = std::clamp(
        ui_state.hough_max_detection_dimension_px, 256, 8192);
    const int source_max = std::max(full.cols, full.rows);
    double scale = 1.0;
    if (source_max > max_dimension) {
        scale = static_cast<double>(max_dimension) /
            static_cast<double>(source_max);
        cv::resize(full, detection, cv::Size(), scale, scale, cv::INTER_AREA);
    }
    int blur = std::clamp(ui_state.hough_median_blur_ksize, 1, 31);
    if ((blur % 2) == 0) ++blur;
    cv::Mat blurred;
    if (blur > 1) cv::medianBlur(detection, blurred, blur);
    else blurred = detection;
    const double min_dimension = std::min(blurred.cols, blurred.rows);
    const int min_radius = std::max(4, static_cast<int>(std::lround(
        min_dimension * std::clamp(
            ui_state.hough_min_radius_fraction, 0.001, 1.0))));
    const int max_radius = std::max(min_radius + 1,
        static_cast<int>(std::lround(min_dimension * std::clamp(
            ui_state.hough_max_radius_fraction, 0.001, 1.5))));
    std::vector<cv::Vec3f> circles;
    auto run = [&](double p1, double p2, double dp) {
        cv::HoughCircles(
            blurred, circles, cv::HOUGH_GRADIENT,
            std::clamp(dp, 1.0, 3.0),
            std::max(1.0, min_dimension * std::clamp(
                ui_state.hough_min_dist_fraction, 0.01, 2.0)),
            std::clamp(p1, 1.0, 500.0),
            std::clamp(p2, 1.0, 500.0), min_radius, max_radius);
    };
    try {
        run(ui_state.hough_param1, ui_state.hough_param2, ui_state.hough_dp);
        if (circles.empty() && ui_state.hough_fallback_enabled) {
            run(ui_state.hough_param1 * 0.75,
                ui_state.hough_param2 * 0.73,
                ui_state.hough_dp * 0.96);
        }
    } catch (const cv::Exception& error) {
        if (gray_out != nullptr) *gray_out = std::move(gray_bytes);
        if (error_out) *error_out = error.what();
        return false;
    }
    if (circles.empty()) {
        if (gray_out != nullptr) *gray_out = std::move(gray_bytes);
        if (error_out) *error_out = "no_water_side_inner_rim_circle_detected";
        return false;
    }
    const cv::Point2d image_center(
        (blurred.cols - 1.0) * 0.5, (blurred.rows - 1.0) * 0.5);
    const cv::Vec3f* best = nullptr;
    double best_score = -std::numeric_limits<double>::infinity();
    for (const auto& circle : circles) {
        const double distance = cv::norm(
            cv::Point2d(circle[0], circle[1]) - image_center);
        const double score = static_cast<double>(circle[2]) - 0.35 * distance;
        if (score > best_score) {
            best_score = score;
            best = &circle;
        }
    }
    const double inverse = 1.0 / scale;
    circle_out->center.x = best->operator[](0) * inverse;
    circle_out->center.y = best->operator[](1) * inverse;
    circle_out->radius_px = std::max(
        1.0, best->operator[](2) * inverse + ui_state.hough_radius_adjustment_px);
    if (gray_out != nullptr) *gray_out = std::move(gray_bytes);
    return true;
}

bool WriteJsonAtomically(
    const fs::path& path,
    const nlohmann::json& value,
    std::string* error_out)
{
    std::string directory_error;
    if (!ensure_directory_for_spatial_session(path.parent_path(), &directory_error)) {
        if (error_out) *error_out = directory_error;
        return false;
    }
    const fs::path temporary = path.string() + ".tmp." +
        std::to_string(static_cast<long long>(::getpid()));
    if (!write_json_file(temporary, value, error_out)) return false;
    orange::ScopedFsuid fsuid_guard;
    (void)fsuid_guard;
    std::error_code rename_error;
    fs::rename(temporary, path, rename_error);
    if (rename_error) {
        std::error_code ignored;
        fs::remove(temporary, ignored);
        if (error_out) {
            *error_out = "Failed to atomically publish " + path.string() +
                ": " + rename_error.message();
        }
        return false;
    }
    return true;
}

nlohmann::json CaptureProvenance(
    const SpatialLayoutGroupCaptureFrame& capture)
{
    return {
        {"capture_group_id", capture.capture_group_id},
        {"capture_mode", capture.capture_mode},
        {"source_array_role", capture.source_array_role},
        {"width", capture.width},
        {"height", capture.height},
        {"source_frame_count", capture.source_frame_count},
        {"first_local_frame_id", capture.first_local_frame_id},
        {"last_local_frame_id", capture.last_local_frame_id},
        {"first_camera_frame_id", capture.first_camera_frame_id},
        {"last_camera_frame_id", capture.last_camera_frame_id},
        {"camera_timestamp_ns", capture.camera_timestamp_ns},
        {"timestamp_sys_ns", capture.timestamp_sys_ns},
        {"capture_group_membership", capture.metadata.capture_group_membership},
    };
}

nlohmann::json WorkflowSnapshot(const DailyRegistrationWorkflowUiState& workflow)
{
    nlohmann::json targets = nlohmann::json::array();
    for (const auto& target : workflow.targets) {
        targets.push_back({
            {"camera_serial", target.camera_serial},
            {"arena_id", target.arena_id},
            {"alignment_basis", target.alignment_basis},
            {"rim", {
                {"detection_ok", target.rim_detection_ok},
                {"detection_error", target.rim_detection_error},
                {"detected_center_camera_px", {
                    {"x", target.detected_rim_center_x_camera_px},
                    {"y", target.detected_rim_center_y_camera_px}}},
                {"detected_radius_camera_px", target.detected_rim_radius_camera_px},
                {"accepted_center_camera_px", {
                    {"x", target.accepted_rim_center_x_camera_px},
                    {"y", target.accepted_rim_center_y_camera_px}}},
                {"accepted_radius_camera_px", target.accepted_rim_radius_camera_px},
                {"operator_confirmed", target.rim_operator_confirmed},
                {"artifact_id", target.rim_observation_artifact_id},
                {"path", target.rim_observation_path},
                {"sha256", target.rim_observation_sha256}}},
            {"projected_center", {
                {"detection_ok", target.projected_center_detection_ok},
                {"detection_error", target.projected_center_detection_error},
                {"center_camera_px", {
                    {"x", target.projected_center_x_camera_px},
                    {"y", target.projected_center_y_camera_px}}},
                {"operator_confirmed", target.projected_center_operator_confirmed},
                {"image_set_path", target.projected_center_image_set_path},
                {"manifest_path", target.projected_center_manifest_path},
                {"observation_path", target.projected_center_observation_path},
                {"observation_sha256", target.projected_center_observation_sha256}}},
            {"preview", {
                {"detection_ok", target.preview_detection_ok},
                {"detection_error", target.preview_detection_error},
                {"center_residual_camera_px", {
                    {"x", target.preview_residual_x_camera_px},
                    {"y", target.preview_residual_y_camera_px},
                    {"norm", target.preview_residual_norm_camera_px}}},
                {"image_set_path", target.preview_image_set_path},
                {"manifest_path", target.preview_manifest_path},
                {"validation_observation_path", target.validation_observation_path},
                {"validation_observation_sha256", target.validation_observation_sha256}}},
            {"geometry_review", {
                {"ok", target.geometry_review_ok},
                {"error", target.geometry_review_error},
                {"translation_canvas_px", {
                    {"requested_x", target.requested_translation_x_canvas_px},
                    {"requested_y", target.requested_translation_y_canvas_px},
                    {"applied_x", target.applied_translation_x_canvas_px},
                    {"applied_y", target.applied_translation_y_canvas_px},
                    {"rounding_residual_x",
                     target.translation_rounding_residual_x_canvas_px},
                    {"rounding_residual_y",
                     target.translation_rounding_residual_y_canvas_px}}},
                {"corrected_center_camera_px", {
                    {"x", target.geometry_corrected_center_x_camera_px},
                    {"y", target.geometry_corrected_center_y_camera_px}}},
                {"center_residual_camera_px", {
                    {"x", target.geometry_center_residual_x_camera_px},
                    {"y", target.geometry_center_residual_y_camera_px},
                    {"norm", target.geometry_center_residual_norm_camera_px}}},
                {"integer_translation_quantization_bound_camera_px",
                 target.geometry_center_quantization_bound_camera_px},
                {"predicted_radius_camera_px", {
                    {"minimum", target.geometry_predicted_radius_min_camera_px},
                    {"mean", target.geometry_predicted_radius_mean_camera_px},
                    {"maximum", target.geometry_predicted_radius_max_camera_px}}},
                {"rim_radial_rms_error_camera_px",
                 target.geometry_rim_radial_rms_error_camera_px},
                {"maximum_outside_rim_camera_px",
                 target.geometry_maximum_outside_rim_camera_px},
                {"observation_path", target.geometry_review_observation_path},
                {"observation_sha256", target.geometry_review_observation_sha256},
                {"overlay_path", target.geometry_review_overlay_path},
                {"overlay_sha256", target.geometry_review_overlay_sha256}}},
        });
    }
    return {
        {"schema_id", "orange.calibration.guided_daily_registration_transaction"},
        {"schema_version", 2},
        {"transaction_id", workflow.transaction_id},
        {"created_utc", workflow.created_utc},
        {"stage", workflow.stage},
        {"active", workflow.active},
        {"status", workflow.status},
        {"error", workflow.error},
        {"pending", {
            {"operation_id", workflow.pending_operation_id},
            {"terminal_stage", workflow.pending_terminal_stage},
            {"terminal_reason", workflow.pending_terminal_reason}}},
        {"candidate_ref", {
            {"path", workflow.candidate_path},
            {"sha256", workflow.candidate_sha256}}},
        {"accepted_registration_ref", {
            {"path", workflow.accepted_registration_path},
            {"sha256", workflow.accepted_registration_sha256}}},
        {"valid_until_utc", workflow.valid_until_utc},
        {"quality_policy", {
            {"maximum_residual_beyond_integer_translation_quantization_camera_px",
             workflow.maximum_geometry_residual_beyond_quantization_camera_px},
            {"radius_is_qc_only", true},
            {"canonical_radius_may_change", false}}},
        {"operator_confirmations", {
            {"physical_state_confirmed", workflow.physical_state_confirmed},
            {"visible_projection_path_confirmed", workflow.visible_projection_path_confirmed},
            {"preview_outline_containment_confirmed",
             workflow.preview_outline_containment_confirmed},
            {"geometry_outline_operator_confirmed",
             workflow.geometry_outline_operator_confirmed},
            {"runtime_optical_state_restored_confirmed",
             workflow.runtime_optical_state_restored_confirmed}}},
        {"citrus", {
            {"begin_status", workflow.citrus_begin_status},
            {"candidate_status", workflow.citrus_candidate_status},
            {"accept_status", workflow.citrus_accept_status},
            {"runtime_selection_status",
             workflow.citrus_runtime_selection_status}}},
        {"targets", std::move(targets)},
    };
}

void Checkpoint(DailyRegistrationWorkflowUiState* workflow)
{
    if (workflow == nullptr || workflow->transaction_dir.empty()) return;
    std::string ignored;
    WriteJsonAtomically(
        fs::path(workflow->transaction_dir) / "transaction.json",
        WorkflowSnapshot(*workflow),
        &ignored);
}

void SetFailure(
    DailyRegistrationWorkflowUiState* workflow,
    const std::string& error)
{
    if (workflow == nullptr) return;
    workflow->error = error;
    workflow->status = "Daily registration stopped safely.";
    workflow->stage = "failed";
    workflow->active = false;
    Checkpoint(workflow);
}

bool RequestAbort(
    DailyRegistrationWorkflowUiState* workflow,
    const std::string& reason,
    const std::string& terminal_stage,
    std::string* error_out)
{
    if (workflow == nullptr || workflow->transaction_id.empty()) {
        if (error_out != nullptr) *error_out = "daily_transaction_identity_missing";
        return false;
    }
    const std::string operation = NextOperationId("abort");
    const auto result = abort_citrus_daily_registration(
        workflow->transaction_id, reason, operation);
    if (!result.ok) {
        const std::string error =
            "Citrus did not accept the daily-registration abort request: " +
            result.reason;
        workflow->error = error;
        workflow->status =
            "The Citrus transaction state is unresolved. Retry abort or inspect Citrus status; do not start an experiment yet.";
        workflow->stage = "abort_request_failed";
        workflow->active = true;
        workflow->pending_terminal_stage = terminal_stage;
        workflow->pending_terminal_reason = reason;
        Checkpoint(workflow);
        if (error_out != nullptr) *error_out = error;
        return false;
    }
    workflow->pending_operation_id = operation;
    workflow->pending_terminal_stage = terminal_stage;
    workflow->pending_terminal_reason = reason;
    workflow->stage = "waiting_abort";
    workflow->active = true;
    workflow->status =
        "Abort requested; waiting for Citrus to restore projection state and acknowledge the transaction as inactive.";
    workflow->error = terminal_stage == "failed" ? reason : std::string();
    Checkpoint(workflow);
    return true;
}

bool AllRimsConfirmed(const DailyRegistrationWorkflowUiState& workflow)
{
    return !workflow.targets.empty() && std::all_of(
        workflow.targets.begin(), workflow.targets.end(), [](const auto& target) {
            return target.rim_detection_ok && target.rim_operator_confirmed &&
                target.accepted_rim_radius_camera_px > 0.0;
        });
}

bool AllProjectedCentersConfirmed(
    const DailyRegistrationWorkflowUiState& workflow)
{
    return !workflow.targets.empty() && std::all_of(
        workflow.targets.begin(), workflow.targets.end(), [](const auto& target) {
            return target.projected_center_detection_ok &&
                target.projected_center_operator_confirmed &&
                !target.rim_observation_sha256.empty() &&
                !target.projected_center_source_path.empty();
        });
}

bool PreviewPasses(const DailyRegistrationWorkflowUiState& workflow)
{
    return !workflow.targets.empty() && std::all_of(
        workflow.targets.begin(), workflow.targets.end(), [&](const auto& target) {
            return target.preview_detection_ok &&
                target.preview_residual_norm_camera_px <=
                    workflow.maximum_preview_center_residual_camera_px &&
                !target.validation_observation_sha256.empty();
        });
}

bool GeometryReviewPasses(const DailyRegistrationWorkflowUiState& workflow)
{
    return !workflow.targets.empty() && std::all_of(
        workflow.targets.begin(), workflow.targets.end(), [&](const auto& target) {
            return target.alignment_basis == kRimOnlyAlignmentBasis &&
                target.geometry_review_ok &&
                target.geometry_center_residual_norm_camera_px <=
                    target.geometry_center_quantization_bound_camera_px +
                    workflow
                        .maximum_geometry_residual_beyond_quantization_camera_px &&
                !target.geometry_review_observation_sha256.empty() &&
                !target.geometry_review_overlay_sha256.empty();
        });
}

void ClearCapturePixels(SpatialLayoutGroupCaptureFrame* capture)
{
    if (capture == nullptr) return;
    capture->rgba.clear();
    capture->rgba.shrink_to_fit();
    capture->texture = 0;
}

bool WriteReviewOverlay(
    const fs::path& path,
    const std::vector<unsigned char>& rgba,
    int width,
    int height,
    std::string* sha_out,
    std::string* error_out)
{
    if (rgba.size() != static_cast<std::size_t>(width) *
            static_cast<std::size_t>(height) * 4u) {
        if (error_out) *error_out = "invalid_review_overlay_shape";
        return false;
    }
    cv::Mat source(height, width, CV_8UC4,
                   const_cast<unsigned char*>(rgba.data()));
    cv::Mat review = source;
    const int max_dimension = std::max(width, height);
    if (max_dimension > 1600) {
        const double scale = 1600.0 / static_cast<double>(max_dimension);
        cv::resize(source, review, cv::Size(), scale, scale, cv::INTER_AREA);
    }
    if (!write_image_file(path, review, error_out)) return false;
    return checksum::file_sha256(path.string(), sha_out, error_out);
}

bool WriteProjectedCenterObservation(
    DailyRegistrationWorkflowUiState* workflow,
    DailyRegistrationTargetUiState* target,
    std::string* error_out)
{
    std::string source_sha;
    if (!checksum::file_sha256(
            target->projected_center_source_path, &source_sha, error_out)) {
        return false;
    }
    target->projected_center_source_sha256 = source_sha;
    const fs::path target_dir = fs::path(workflow->transaction_dir) /
        "targets" / sanitize_artifact_component(
            target->camera_serial + "_" + target->arena_id);
    const fs::path overlay_path = target_dir / "base_center_detection_overlay.png";
    std::string overlay_sha;
    const auto overlay_it = target->projected_center_detection.find("overlay_rgba");
    (void)overlay_it;
    const std::vector<unsigned char> difference = DifferenceRgba(
        target->projected_center_capture, target->rim_gray);
    centering::FiducialDetectorConfig config;
    config.has_expected_center_camera_px = true;
    config.expected_center_camera_px = {
        target->accepted_rim_center_x_camera_px,
        target->accepted_rim_center_y_camera_px};
    const auto detection = centering::DetectArenaCenterFiducial(
        {target->camera_serial,
         target->projected_center_capture.width,
         target->projected_center_capture.height,
         &difference}, config);
    const bool same_detection = detection.ok &&
        std::abs(detection.center_camera_px.x -
                 target->projected_center_x_camera_px) <= 1e-6 &&
        std::abs(detection.center_camera_px.y -
                 target->projected_center_y_camera_px) <= 1e-6 &&
        std::abs(detection.radius_camera_px -
                 target->projected_center_radius_camera_px) <= 1e-6;
    if (!same_detection) {
        if (error_out != nullptr) {
            *error_out =
                "projected_center_detection_changed_before_persistence:" +
                target->camera_serial;
        }
        return false;
    }
    if (!detection.ok || !WriteReviewOverlay(
            overlay_path, detection.overlay_rgba,
            target->projected_center_capture.width,
            target->projected_center_capture.height,
            &overlay_sha, error_out)) {
        if (error_out && error_out->empty()) *error_out = detection.error;
        return false;
    }
    const fs::path observation_path =
        target_dir / "projected_center_observation.json";
    target->projected_center_observation_artifact_id =
        "daily_marker_" + sanitize_artifact_component(
            workflow->transaction_id + "_" + target->camera_serial);
    nlohmann::json observation = {
        {"schema_id", "orange.calibration.projected_center_observation"},
        {"schema_version", 1},
        {"artifact_id", target->projected_center_observation_artifact_id},
        {"transaction_id", workflow->transaction_id},
        {"camera_serial", target->camera_serial},
        {"arena_id", target->arena_id},
        {"coordinate_space", "camera_native_pixels"},
        {"target_plane", "projected_surface"},
        {"detection", target->projected_center_detection},
        {"operator_confirmed", target->projected_center_operator_confirmed},
        {"source_capture", CaptureProvenance(target->projected_center_capture)},
        {"source_image", {
            {"path", target->projected_center_source_path},
            {"sha256", source_sha}}},
        {"black_reference", {
            {"rim_observation_path", target->rim_observation_path},
            {"rim_observation_sha256", target->rim_observation_sha256}}},
        {"review_overlay", {
            {"path", overlay_path.string()}, {"sha256", overlay_sha}}},
        {"citrus_scene_pre_capture",
         target->projected_center_capture.metadata.citrus_calibration_scene_pre_capture},
        {"citrus_scene_post_capture",
         target->projected_center_capture.metadata.citrus_calibration_scene_post_capture},
        {"citrus_scene_consistency",
         target->projected_center_capture.metadata.citrus_calibration_scene_consistency},
    };
    if (!WriteJsonAtomically(observation_path, observation, error_out)) return false;
    target->projected_center_observation_path = observation_path.string();
    return checksum::file_sha256(
        target->projected_center_observation_path,
        &target->projected_center_observation_sha256,
        error_out);
}

bool WriteValidationObservation(
    DailyRegistrationWorkflowUiState* workflow,
    DailyRegistrationTargetUiState* target,
    std::string* error_out)
{
    if (!checksum::file_sha256(
            target->preview_source_path,
            &target->preview_source_sha256,
            error_out)) return false;
    const fs::path target_dir = fs::path(workflow->transaction_dir) /
        "targets" / sanitize_artifact_component(
            target->camera_serial + "_" + target->arena_id);
    const std::vector<unsigned char> difference = DifferenceRgba(
        target->preview_capture, target->rim_gray);
    centering::FiducialDetectorConfig config;
    config.has_expected_center_camera_px = true;
    config.expected_center_camera_px = {
        target->accepted_rim_center_x_camera_px,
        target->accepted_rim_center_y_camera_px};
    const auto detection = centering::DetectArenaCenterFiducial(
        {target->camera_serial,
         target->preview_capture.width,
         target->preview_capture.height,
         &difference}, config);
    const bool same_detection = detection.ok &&
        std::abs(detection.center_camera_px.x -
                 target->preview_center_x_camera_px) <= 1e-6 &&
        std::abs(detection.center_camera_px.y -
                 target->preview_center_y_camera_px) <= 1e-6;
    if (!same_detection) {
        if (error_out != nullptr) {
            *error_out =
                "preview_center_detection_changed_before_persistence:" +
                target->camera_serial;
        }
        return false;
    }
    const fs::path overlay_path = target_dir / "candidate_validation_overlay.png";
    std::string overlay_sha;
    if (!detection.ok || !WriteReviewOverlay(
            overlay_path, detection.overlay_rgba,
            target->preview_capture.width,
            target->preview_capture.height,
            &overlay_sha, error_out)) {
        if (error_out && error_out->empty()) *error_out = detection.error;
        return false;
    }
    const fs::path observation_path = target_dir / "validation_observation.json";
    nlohmann::json observation = {
        {"schema_id", "orange.calibration.daily_registration_validation"},
        {"schema_version", 1},
        {"artifact_id", "daily_validation_" + sanitize_artifact_component(
            workflow->transaction_id + "_" + target->camera_serial)},
        {"transaction_id", workflow->transaction_id},
        {"camera_serial", target->camera_serial},
        {"arena_id", target->arena_id},
        {"candidate_sha256", workflow->candidate_sha256},
        {"coordinate_space", "camera_native_pixels"},
        {"center_detection", target->preview_detection},
        {"center_residual_camera_px", {
            {"x", target->preview_residual_x_camera_px},
            {"y", target->preview_residual_y_camera_px},
            {"norm", target->preview_residual_norm_camera_px}}},
        {"center_residual_threshold_camera_px",
         workflow->maximum_preview_center_residual_camera_px},
        {"center_gate_passed",
         target->preview_residual_norm_camera_px <=
             workflow->maximum_preview_center_residual_camera_px},
        {"source_capture", CaptureProvenance(target->preview_capture)},
        {"source_image", {
            {"path", target->preview_source_path},
            {"sha256", target->preview_source_sha256}}},
        {"review_overlay", {
            {"path", overlay_path.string()}, {"sha256", overlay_sha}}},
        {"citrus_daily_registration_pre_capture",
         target->preview_capture.metadata.citrus_daily_registration_pre_capture},
        {"citrus_daily_registration_post_capture",
         target->preview_capture.metadata.citrus_daily_registration_post_capture},
        {"citrus_daily_registration_consistency",
         target->preview_capture.metadata.citrus_daily_registration_consistency},
    };
    if (!WriteJsonAtomically(observation_path, observation, error_out)) return false;
    target->validation_observation_path = observation_path.string();
    return checksum::file_sha256(
        target->validation_observation_path,
        &target->validation_observation_sha256,
        error_out);
}

bool QueueGenericCaptureSaves(
    SpatialLayoutUiState* ui_state,
    DailyRegistrationWorkflowUiState* workflow,
    const CameraParams* cameras,
    int camera_count,
    bool preview,
    std::string* error_out)
{
    if (generic_calibration_image_set_save_worker_is_busy()) {
        if (error_out) *error_out =
            "Wait for existing calibration image saves before continuing.";
        return false;
    }
    std::vector<GenericCalibrationImageSetSaveJob> jobs;
    jobs.reserve(workflow->targets.size());
    for (auto& target : workflow->targets) {
        auto& capture = preview
            ? target.preview_capture
            : target.projected_center_capture;
        const CameraParams* camera = FindCamera(
            cameras, camera_count, target.camera_serial);
        if (camera == nullptr) {
            if (error_out) *error_out = "daily_capture_camera_closed:" +
                target.camera_serial;
            return false;
        }
        GenericCalibrationImageSetSaveJob job;
        if (!prepare_generic_calibration_image_set_save_job_from_group_capture(
                ui_state, capture, *camera, workflow->session_artifact_root,
                &job, error_out)) return false;
        job.session_dir = ui_state->calibration_session_dir;
        const auto files = make_generic_calibration_image_set_files(
            job.artifact_root_dir, job.request.artifact_id,
            job.capture_filename);
        if (preview) {
            target.preview_source_path = files.source_frame_path.string();
            target.preview_image_set_path = files.image_set_path.string();
            target.preview_manifest_path = files.manifest_path.string();
        } else {
            target.projected_center_source_path = files.source_frame_path.string();
            target.projected_center_image_set_path = files.image_set_path.string();
            target.projected_center_manifest_path = files.manifest_path.string();
        }
        jobs.push_back(std::move(job));
    }
    for (auto& job : jobs) {
        if (!submit_generic_calibration_image_set_save_job(
                std::move(job), error_out)) return false;
    }
    return true;
}

bool VerifyGenericCaptureSave(
    const SpatialLayoutGroupCaptureFrame& capture,
    const std::string& source_path,
    const std::string& image_set_path,
    const std::string& manifest_path,
    const std::string& expected_capture_stage,
    std::string* source_sha256_out,
    std::string* error_out)
{
    if (!fs::is_regular_file(source_path) ||
        !fs::is_regular_file(image_set_path) ||
        !fs::is_regular_file(manifest_path)) {
        if (error_out != nullptr) {
            *error_out = "daily_capture_save_incomplete:" +
                capture.camera_serial;
        }
        return false;
    }
    if (!checksum::file_sha256(
            source_path, source_sha256_out, error_out)) return false;
    nlohmann::json image_set;
    nlohmann::json manifest;
    if (!read_json_file(image_set_path, &image_set, error_out) ||
        !read_json_file(manifest_path, &manifest, error_out)) {
        return false;
    }
    bool matching_entry = false;
    if (image_set.is_object() && image_set.contains("images") &&
        image_set["images"].is_array()) {
        const std::string source_filename =
            fs::path(source_path).filename().string();
        for (const auto& entry : image_set["images"]) {
            const auto capture_metadata = entry.value(
                "capture", nlohmann::json::object());
            if (entry.is_object() &&
                entry.value("capture_stage", std::string()) ==
                    expected_capture_stage &&
                capture_metadata.value("capture_group_id", std::string()) ==
                    capture.capture_group_id &&
                fs::path(entry.value("path", std::string())).filename() ==
                    source_filename &&
                !entry.value("checksum", std::string()).empty()) {
                matching_entry = true;
                break;
            }
        }
    }
    const auto summary = manifest.value("summary", nlohmann::json::object());
    if (!matching_entry ||
        manifest.value("schema_id", std::string()) !=
            kCalibrationManifestSchemaId ||
        summary.value("latest_capture_stage", std::string()) !=
            expected_capture_stage) {
        if (error_out != nullptr) {
            *error_out = "daily_capture_manifest_identity_mismatch:" +
                capture.camera_serial;
        }
        return false;
    }
    return true;
}

bool AnalyzeRimCaptures(
    SpatialLayoutUiState* ui_state,
    DailyRegistrationWorkflowUiState* workflow,
    std::string* error_out)
{
    struct RimDetection {
        bool ok = false;
        orange::calibration::DishTopRimCircle circle;
        std::vector<unsigned char> gray;
        std::string error;
    };
    for (auto& target : workflow->targets) {
        const auto* capture = FindCapture(*ui_state, target.camera_serial);
        if (capture == nullptr) {
            if (error_out) *error_out =
                "missing_daily_rim_capture:" + target.camera_serial;
            return false;
        }
        target.rim_capture = *capture;
        target.rim_capture.texture = 0;
    }

    std::vector<std::future<RimDetection>> futures;
    futures.reserve(workflow->targets.size());
    for (auto& target : workflow->targets) {
        futures.push_back(std::async(
            std::launch::async, [&capture = target.rim_capture, ui_state]() {
                RimDetection result;
                result.ok = DetectDailyRim(
                    capture, *ui_state, &result.circle, &result.gray,
                    &result.error);
                return result;
            }));
    }
    for (std::size_t index = 0; index < workflow->targets.size(); ++index) {
        RimDetection result = futures[index].get();
        auto& target = workflow->targets[index];
        target.rim_detection_ok = result.ok;
        target.rim_detection_error = std::move(result.error);
        target.rim_gray = std::move(result.gray);
        if (target.rim_detection_ok) {
            target.detected_rim_center_x_camera_px = result.circle.center.x;
            target.detected_rim_center_y_camera_px = result.circle.center.y;
            target.detected_rim_radius_camera_px = result.circle.radius_px;
            target.accepted_rim_center_x_camera_px = result.circle.center.x;
            target.accepted_rim_center_y_camera_px = result.circle.center.y;
            target.accepted_rim_radius_camera_px = result.circle.radius_px;
        }
    }
    return true;
}

bool AnalyzeMarkerCaptures(
    SpatialLayoutUiState* ui_state,
    DailyRegistrationWorkflowUiState* workflow,
    bool preview,
    std::string* error_out)
{
    struct Pending {
        DailyRegistrationTargetUiState* target = nullptr;
        SpatialLayoutGroupCaptureFrame capture;
        std::vector<unsigned char> difference;
    };
    std::vector<Pending> pending;
    pending.reserve(workflow->targets.size());
    for (auto& target : workflow->targets) {
        const auto* source = FindCapture(*ui_state, target.camera_serial);
        if (source == nullptr) {
            if (error_out) *error_out = "missing_daily_marker_capture:" +
                target.camera_serial;
            return false;
        }
        Pending row;
        row.target = &target;
        row.capture = *source;
        row.capture.texture = 0;
        row.difference = DifferenceRgba(*source, target.rim_gray);
        if (row.difference.empty()) {
            if (error_out) *error_out =
                "daily_marker_black_reference_shape_mismatch:" +
                target.camera_serial;
            return false;
        }
        pending.push_back(std::move(row));
    }
    // Launch only after the owned rows are in their final reserved storage so
    // every task observes a stable per-camera difference buffer.
    std::vector<std::future<centering::FiducialDetection>> futures;
    futures.reserve(pending.size());
    for (auto& row : pending) {
        const auto serial = row.target->camera_serial;
        const int width = row.capture.width;
        const int height = row.capture.height;
        const auto expected = centering::Point2d{
            row.target->accepted_rim_center_x_camera_px,
            row.target->accepted_rim_center_y_camera_px};
        futures.push_back(std::async(std::launch::async,
            [serial, width, height, expected, &difference = row.difference]() {
                centering::FiducialDetectorConfig config;
                config.has_expected_center_camera_px = true;
                config.expected_center_camera_px = expected;
                return centering::DetectArenaCenterFiducial(
                    {serial, width, height, &difference}, config);
            }));
    }
    for (std::size_t index = 0; index < pending.size(); ++index) {
        auto detection = futures[index].get();
        auto& target = *pending[index].target;
        if (preview) {
            target.preview_capture = std::move(pending[index].capture);
            target.preview_detection_ok = detection.ok;
            target.preview_detection_error = detection.error;
            target.preview_detection = detection.ToJson();
            target.preview_center_x_camera_px = detection.center_camera_px.x;
            target.preview_center_y_camera_px = detection.center_camera_px.y;
            target.preview_residual_x_camera_px =
                detection.center_camera_px.x -
                target.accepted_rim_center_x_camera_px;
            target.preview_residual_y_camera_px =
                detection.center_camera_px.y -
                target.accepted_rim_center_y_camera_px;
            target.preview_residual_norm_camera_px = std::hypot(
                target.preview_residual_x_camera_px,
                target.preview_residual_y_camera_px);
        } else {
            target.projected_center_capture = std::move(pending[index].capture);
            target.projected_center_detection_ok = detection.ok;
            target.projected_center_detection_error = detection.error;
            target.projected_center_detection = detection.ToJson();
            target.projected_center_x_camera_px = detection.center_camera_px.x;
            target.projected_center_y_camera_px = detection.center_camera_px.y;
            target.projected_center_radius_camera_px = detection.radius_camera_px;
            target.base_center_residual_x_camera_px =
                detection.center_camera_px.x -
                target.accepted_rim_center_x_camera_px;
            target.base_center_residual_y_camera_px =
                detection.center_camera_px.y -
                target.accepted_rim_center_y_camera_px;
        }
    }
    return true;
}

bool QueueRimArtifacts(
    SpatialLayoutUiState* ui_state,
    DailyRegistrationWorkflowUiState* workflow,
    const CameraParams* cameras,
    int camera_count,
    std::string* error_out)
{
    if (top_rim_observation_save_worker_is_busy()) {
        if (error_out) *error_out =
            "Wait for the existing top-rim save before continuing.";
        return false;
    }
    std::vector<TopRimObservationSaveJob> jobs;
    jobs.reserve(workflow->targets.size());
    for (auto& target : workflow->targets) {
        const CameraParams* camera = FindCamera(
            cameras, camera_count, target.camera_serial);
        if (camera == nullptr) {
            if (error_out) *error_out =
                "daily_rim_camera_closed:" + target.camera_serial;
            return false;
        }
        orange::calibration::DishTopRimCircle detected;
        detected.center = {target.detected_rim_center_x_camera_px,
                           target.detected_rim_center_y_camera_px};
        detected.radius_px = target.detected_rim_radius_camera_px;
        orange::calibration::DishTopRimCircle accepted;
        accepted.center = {target.accepted_rim_center_x_camera_px,
                           target.accepted_rim_center_y_camera_px};
        accepted.radius_px = target.accepted_rim_radius_camera_px;
        TopRimObservationSaveJob job;
        if (!prepare_dish_top_rim_observation_save_job_from_group_capture(
                ui_state, target.rim_capture, *camera,
                workflow->session_artifact_root,
                detected, accepted, &job, error_out)) return false;
        job.session_dir = ui_state->calibration_session_dir;
        const auto paths =
            orange::calibration::make_dish_top_rim_observation_artifact_paths(
                job.artifact_root_dir,
                job.request.artifact_id,
                job.request.storage_relative_artifact_dir);
        target.rim_observation_artifact_id = job.request.artifact_id;
        target.rim_observation_path = paths.observation_json_path;
        jobs.push_back(std::move(job));
    }
    for (auto& job : jobs) {
        if (!submit_top_rim_observation_save_job(
                std::move(job), error_out)) return false;
    }
    return true;
}

nlohmann::json CandidateObservations(
    const DailyRegistrationWorkflowUiState& workflow)
{
    nlohmann::json observations = nlohmann::json::array();
    for (const auto& target : workflow.targets) {
        observations.push_back({
            {"arena_id", target.arena_id},
            {"camera_id", target.camera_serial},
            {"alignment_basis", kRimOnlyAlignmentBasis},
            {"rim_center_camera_px", {
                {"x", target.accepted_rim_center_x_camera_px},
                {"y", target.accepted_rim_center_y_camera_px}}},
            {"observed_rim_radius_camera_px",
             target.accepted_rim_radius_camera_px},
            {"rim_observation", {
                {"artifact_id", target.rim_observation_artifact_id},
                {"path", target.rim_observation_path},
                {"sha256", target.rim_observation_sha256}}},
        });
    }
    return observations;
}

bool JsonNumber(const nlohmann::json& object,
                const char* key,
                double* value_out)
{
    if (value_out == nullptr || !object.is_object()) return false;
    const auto it = object.find(key);
    if (it == object.end() || !it->is_number()) return false;
    *value_out = it->get<double>();
    return std::isfinite(*value_out);
}

bool DrawAndWriteGeometryReviewOverlay(
    DailyRegistrationWorkflowUiState* workflow,
    DailyRegistrationTargetUiState* target,
    std::string* error_out)
{
    if (workflow == nullptr || target == nullptr ||
        !target->rim_capture.valid || target->rim_capture.width <= 0 ||
        target->rim_capture.height <= 0 ||
        target->rim_capture.rgba.size() !=
            static_cast<std::size_t>(target->rim_capture.width) *
                static_cast<std::size_t>(target->rim_capture.height) * 4u ||
        target->geometry_outline_camera_px.size() < 3) {
        if (error_out) *error_out = "invalid_daily_geometry_overlay_source";
        return false;
    }
    const fs::path target_dir = fs::path(workflow->transaction_dir) /
        "targets" / sanitize_artifact_component(
            target->camera_serial + "_" + target->arena_id);
    if (!ensure_directory_for_spatial_session(target_dir, error_out)) {
        return false;
    }
    cv::Mat source(
        target->rim_capture.height, target->rim_capture.width, CV_8UC4,
        target->rim_capture.rgba.data());
    cv::Mat overlay = source.clone();
    const cv::Point rim_center(
        static_cast<int>(std::lround(target->accepted_rim_center_x_camera_px)),
        static_cast<int>(std::lround(target->accepted_rim_center_y_camera_px)));
    cv::circle(
        overlay, rim_center,
        std::max(1, static_cast<int>(std::lround(
            target->accepted_rim_radius_camera_px))),
        cv::Scalar(255, 165, 32, 255), 7, cv::LINE_AA);
    std::vector<cv::Point> outline;
    outline.reserve(target->geometry_outline_camera_px.size());
    for (const auto& point : target->geometry_outline_camera_px) {
        outline.emplace_back(
            static_cast<int>(std::lround(point.x)),
            static_cast<int>(std::lround(point.y)));
    }
    cv::polylines(
        overlay, outline, true, cv::Scalar(70, 255, 150, 255),
        7, cv::LINE_AA);
    const cv::Point corrected_center(
        static_cast<int>(std::lround(
            target->geometry_corrected_center_x_camera_px)),
        static_cast<int>(std::lround(
            target->geometry_corrected_center_y_camera_px)));
    constexpr int arm = 24;
    cv::line(
        overlay, {corrected_center.x - arm, corrected_center.y},
        {corrected_center.x + arm, corrected_center.y},
        cv::Scalar(70, 255, 150, 255), 7, cv::LINE_AA);
    cv::line(
        overlay, {corrected_center.x, corrected_center.y - arm},
        {corrected_center.x, corrected_center.y + arm},
        cv::Scalar(70, 255, 150, 255), 7, cv::LINE_AA);
    cv::drawMarker(
        overlay, rim_center, cv::Scalar(255, 165, 32, 255),
        cv::MARKER_TILTED_CROSS, 38, 6, cv::LINE_AA);

    target->geometry_review_overlay_path =
        (target_dir / "rim_only_geometry_review_overlay.png").string();
    std::vector<unsigned char> overlay_rgba(
        overlay.data, overlay.data + overlay.total() * overlay.elemSize());
    return WriteReviewOverlay(
        target->geometry_review_overlay_path,
        overlay_rgba, overlay.cols, overlay.rows,
        &target->geometry_review_overlay_sha256, error_out);
}

bool WriteGeometryReviewObservation(
    DailyRegistrationWorkflowUiState* workflow,
    DailyRegistrationTargetUiState* target,
    const CitrusSpatialTemplateState& template_state,
    std::string* error_out)
{
    if (!DrawAndWriteGeometryReviewOverlay(workflow, target, error_out)) {
        return false;
    }
    nlohmann::json outline = nlohmann::json::array();
    for (const auto& point : target->geometry_outline_camera_px) {
        outline.push_back({{"x", point.x}, {"y", point.y}});
    }
    const fs::path target_dir = fs::path(workflow->transaction_dir) /
        "targets" / sanitize_artifact_component(
            target->camera_serial + "_" + target->arena_id);
    target->geometry_review_observation_path =
        (target_dir / "rim_only_geometry_review.json").string();
    nlohmann::json observation = {
        {"schema_id", "orange.calibration.daily_registration_geometry_review"},
        {"schema_version", 1},
        {"artifact_id", "daily_geometry_review_" +
            sanitize_artifact_component(
                workflow->transaction_id + "_" + target->camera_serial)},
        {"transaction_id", workflow->transaction_id},
        {"candidate", {
            {"path", workflow->candidate_path},
            {"sha256", workflow->candidate_sha256}}},
        {"camera_serial", target->camera_serial},
        {"arena_id", target->arena_id},
        {"alignment_basis", target->alignment_basis},
        {"translation_policy",
         "integer_canvas_translation_move_arena_and_experimental_area_together"},
        {"translation_canvas_px", {
            {"requested_x", target->requested_translation_x_canvas_px},
            {"requested_y", target->requested_translation_y_canvas_px},
            {"applied_x", target->applied_translation_x_canvas_px},
            {"applied_y", target->applied_translation_y_canvas_px},
            {"rounding_residual_x",
             target->translation_rounding_residual_x_canvas_px},
            {"rounding_residual_y",
             target->translation_rounding_residual_y_canvas_px}}},
        {"experimental_area_centers_canvas_px", {
            {"base", {
                {"x", target->base_experimental_center_x_canvas_px},
                {"y", target->base_experimental_center_y_canvas_px}}},
            {"desired_from_rim", {
                {"x", target->desired_experimental_center_x_canvas_px},
                {"y", target->desired_experimental_center_y_canvas_px}}},
            {"effective_after_integer_translation", {
                {"x", target->effective_experimental_center_x_canvas_px},
                {"y", target->effective_experimental_center_y_canvas_px}}}}},
        {"canonical_experimental_area", {
            {"shape", "circle"},
            {"radius_canvas_px", template_state.experimental_area_radius_px},
            {"radius_mm", template_state.has_radius_mm
                ? nlohmann::json(template_state.experimental_area_radius_mm)
                : nlohmann::json(nullptr)},
            {"radius_changed_by_daily_registration", false}}},
        {"camera_space_review", {
            {"corrected_center_camera_px", {
                {"x", target->geometry_corrected_center_x_camera_px},
                {"y", target->geometry_corrected_center_y_camera_px}}},
            {"accepted_rim_center_camera_px", {
                {"x", target->accepted_rim_center_x_camera_px},
                {"y", target->accepted_rim_center_y_camera_px}}},
            {"accepted_rim_radius_camera_px",
             target->accepted_rim_radius_camera_px},
            {"center_residual_camera_px", {
                {"x", target->geometry_center_residual_x_camera_px},
                {"y", target->geometry_center_residual_y_camera_px},
                {"norm", target->geometry_center_residual_norm_camera_px}}},
            {"integer_translation_quantization_bound_camera_px",
             target->geometry_center_quantization_bound_camera_px},
            {"predicted_radius_camera_px", {
                {"minimum", target->geometry_predicted_radius_min_camera_px},
                {"mean", target->geometry_predicted_radius_mean_camera_px},
                {"maximum", target->geometry_predicted_radius_max_camera_px}}},
            {"rim_radial_rms_error_camera_px",
             target->geometry_rim_radial_rms_error_camera_px},
            {"maximum_outside_rim_camera_px",
             target->geometry_maximum_outside_rim_camera_px},
            {"canonical_outline_camera_px", std::move(outline)}}},
        {"homography", {
            {"candidate_id", target->candidate_homography_id},
            {"candidate_path", target->candidate_homography_path},
            {"candidate_sha256", target->candidate_homography_sha256},
            {"selected_canvas_name", template_state.source_canvas_name},
            {"authority_mode",
             template_state.projection_geometry_authority_mode},
            {"authority_canvas_name",
             template_state.projection_geometry_authority_canvas_name},
            {"target_plane", template_state.homography_target_plane},
            {"direction", template_state.homography_direction},
            {"configuration_fingerprint",
             template_state.homography_configuration_fingerprint}}},
        {"rim_observation", {
            {"artifact_id", target->rim_observation_artifact_id},
            {"path", target->rim_observation_path},
            {"sha256", target->rim_observation_sha256}}},
        {"source_capture", CaptureProvenance(target->rim_capture)},
        {"review_overlay", {
            {"path", target->geometry_review_overlay_path},
            {"sha256", target->geometry_review_overlay_sha256}}},
        {"plane_contract", {
            {"observed_boundary_plane", "dish_top_rim"},
            {"position_mapping_homography_plane", "projected_surface"},
            {"daily_scope", "translation_only"},
            {"warning",
             "The commissioned projected-surface homography maps the fitted rim center; radius comparison is QC only."}}},
    };
    if (!WriteJsonAtomically(
            target->geometry_review_observation_path,
            observation, error_out)) return false;
    return checksum::file_sha256(
        target->geometry_review_observation_path,
        &target->geometry_review_observation_sha256,
        error_out);
}

bool LoadCandidateAndBuildGeometryReview(
    const SpatialLayoutUiState& ui_state,
    DailyRegistrationWorkflowUiState* workflow,
    std::string* error_out)
{
    if (workflow == nullptr || workflow->candidate_path.empty() ||
        workflow->candidate_sha256.empty()) {
        if (error_out) *error_out = "daily_candidate_reference_missing";
        return false;
    }
    std::string actual_sha;
    if (!checksum::file_sha256(
            workflow->candidate_path, &actual_sha, error_out) ||
        actual_sha != workflow->candidate_sha256) {
        if (error_out && error_out->empty()) {
            *error_out = "daily_candidate_checksum_mismatch";
        }
        return false;
    }
    nlohmann::json candidate;
    if (!read_json_file(workflow->candidate_path, &candidate, error_out)) {
        return false;
    }
    if (candidate.value("schema_id", std::string()) !=
            "citrus.calibration.daily_registration_candidate" ||
        candidate.value("schema_version", 0) != 1 ||
        candidate.value("status", std::string()) != "candidate" ||
        candidate.value("transaction_id", std::string()) !=
            workflow->transaction_id ||
        !candidate.contains("targets") || !candidate["targets"].is_array() ||
        candidate["targets"].size() != workflow->targets.size()) {
        if (error_out) *error_out = "daily_candidate_identity_mismatch";
        return false;
    }

    for (auto& target : workflow->targets) {
        const nlohmann::json* candidate_target = nullptr;
        for (const auto& row : candidate["targets"]) {
            if (row.is_object() &&
                row.value("arena_id", std::string()) == target.arena_id &&
                row.value("camera_id", std::string()) == target.camera_serial) {
                if (candidate_target != nullptr) {
                    if (error_out) *error_out =
                        "duplicate_daily_candidate_target:" + target.arena_id;
                    return false;
                }
                candidate_target = &row;
            }
        }
        if (candidate_target == nullptr ||
            candidate_target->value("alignment_basis", std::string()) !=
                kRimOnlyAlignmentBasis ||
            candidate_target->value("target_plane", std::string()) !=
                "projected_surface") {
            if (error_out) *error_out =
                "daily_candidate_alignment_contract_mismatch:" +
                target.arena_id;
            return false;
        }
        const auto rim_center = candidate_target->value(
            "rim_center_camera_px", nlohmann::json::object());
        double rim_x = 0.0;
        double rim_y = 0.0;
        if (!JsonNumber(rim_center, "x", &rim_x) ||
            !JsonNumber(rim_center, "y", &rim_y) ||
            std::abs(rim_x - target.accepted_rim_center_x_camera_px) > 1e-6 ||
            std::abs(rim_y - target.accepted_rim_center_y_camera_px) > 1e-6 ||
            std::abs(candidate_target->value(
                "observed_rim_radius_camera_px", 0.0) -
                target.accepted_rim_radius_camera_px) > 1e-6) {
            if (error_out) *error_out =
                "daily_candidate_rim_observation_mismatch:" +
                target.arena_id;
            return false;
        }

        const int template_index = find_citrus_template_index_for_camera(
            ui_state, target.camera_serial);
        if (template_index < 0 ||
            template_index >= static_cast<int>(
                ui_state.citrus_canvas_templates.size())) {
            if (error_out) *error_out =
                "daily_candidate_citrus_template_missing:" +
                target.camera_serial;
            return false;
        }
        const auto& template_state =
            ui_state.citrus_canvas_templates[template_index];
        if (!template_state.available ||
            template_state.source_config_name != target.arena_id ||
            !template_state.has_arena_canvas_region ||
            !template_state.has_authoritative_camera_to_canvas_homography ||
            !template_state.has_canvas_to_camera_homography ||
            template_state.homography_authority_status !=
                "accepted_compatible" ||
            template_state.homography_target_plane != "projected_surface" ||
            template_state.experimental_area_radius_px <= 0.0) {
            if (error_out) *error_out =
                "daily_candidate_authoritative_homography_unavailable:" +
                target.arena_id;
            return false;
        }
        const auto homography = candidate_target->value(
            "homography", nlohmann::json::object());
        target.candidate_homography_id =
            homography.value("candidate_id", std::string());
        target.candidate_homography_path =
            homography.value("candidate_path", std::string());
        target.candidate_homography_sha256 =
            homography.value("candidate_sha256", std::string());
        std::string imported_candidate_sha;
        if (target.candidate_homography_id.empty() ||
            target.candidate_homography_id !=
                template_state.homography_candidate_id ||
            homography.value("selected_canvas_name", std::string()) !=
                template_state.source_canvas_name ||
            homography.value("authority_mode", std::string()) !=
                template_state.projection_geometry_authority_mode ||
            homography.value("authority_canvas_name", std::string()) !=
                template_state.projection_geometry_authority_canvas_name ||
            target.candidate_homography_path.empty() ||
            target.candidate_homography_sha256.empty() ||
            !checksum::file_sha256(
                template_state.homography_candidate_json_path,
                &imported_candidate_sha, error_out) ||
            imported_candidate_sha != target.candidate_homography_sha256) {
            if (error_out && error_out->empty()) {
                *error_out =
                    "daily_candidate_homography_identity_mismatch:" +
                    target.arena_id;
            }
            return false;
        }

        const auto translation = candidate_target->value(
            "translation_canvas_px", nlohmann::json::object());
        const auto base_center = candidate_target->value(
            "base_experimental_area_center_canvas_px",
            nlohmann::json::object());
        const auto desired_center = candidate_target->value(
            "desired_experimental_area_center_canvas_px",
            nlohmann::json::object());
        const auto effective_center = candidate_target->value(
            "effective_experimental_area_center_canvas_px",
            nlohmann::json::object());
        if (!JsonNumber(translation, "requested_x",
                        &target.requested_translation_x_canvas_px) ||
            !JsonNumber(translation, "requested_y",
                        &target.requested_translation_y_canvas_px) ||
            !translation.contains("applied_x") ||
            !translation["applied_x"].is_number_integer() ||
            !translation.contains("applied_y") ||
            !translation["applied_y"].is_number_integer() ||
            !JsonNumber(translation, "rounding_residual_x",
                        &target.translation_rounding_residual_x_canvas_px) ||
            !JsonNumber(translation, "rounding_residual_y",
                        &target.translation_rounding_residual_y_canvas_px) ||
            !JsonNumber(base_center, "x",
                        &target.base_experimental_center_x_canvas_px) ||
            !JsonNumber(base_center, "y",
                        &target.base_experimental_center_y_canvas_px) ||
            !JsonNumber(desired_center, "x",
                        &target.desired_experimental_center_x_canvas_px) ||
            !JsonNumber(desired_center, "y",
                        &target.desired_experimental_center_y_canvas_px) ||
            !JsonNumber(effective_center, "x",
                        &target.effective_experimental_center_x_canvas_px) ||
            !JsonNumber(effective_center, "y",
                        &target.effective_experimental_center_y_canvas_px)) {
            if (error_out) *error_out =
                "daily_candidate_translation_contract_invalid:" +
                target.arena_id;
            return false;
        }
        target.applied_translation_x_canvas_px =
            translation["applied_x"].get<int>();
        target.applied_translation_y_canvas_px =
            translation["applied_y"].get<int>();
        const double imported_origin_x =
            template_state.arena_center_x_px -
            static_cast<double>(
                static_cast<int>(template_state.arena_width_px) / 2);
        const double imported_origin_y =
            template_state.arena_center_y_px -
            static_cast<double>(
                static_cast<int>(template_state.arena_height_px) / 2);
        const double imported_base_x = imported_origin_x +
            template_state.experimental_area_center_x_px;
        const double imported_base_y = imported_origin_y +
            template_state.experimental_area_center_y_px;
        if (std::abs(imported_base_x -
                     target.base_experimental_center_x_canvas_px) > 1e-6 ||
            std::abs(imported_base_y -
                     target.base_experimental_center_y_canvas_px) > 1e-6 ||
            std::abs(target.effective_experimental_center_x_canvas_px -
                     (target.base_experimental_center_x_canvas_px +
                      target.applied_translation_x_canvas_px)) > 1e-6 ||
            std::abs(target.effective_experimental_center_y_canvas_px -
                     (target.base_experimental_center_y_canvas_px +
                      target.applied_translation_y_canvas_px)) > 1e-6) {
            if (error_out) *error_out =
                "daily_candidate_imported_geometry_mismatch:" +
                target.arena_id;
            return false;
        }

        daily_geometry::GeometryReviewInput review_input;
        review_input.canvas_to_camera_homography =
            template_state.canvas_to_camera_homography;
        review_input.desired_experimental_center_canvas_x_px =
            target.desired_experimental_center_x_canvas_px;
        review_input.desired_experimental_center_canvas_y_px =
            target.desired_experimental_center_y_canvas_px;
        review_input.effective_experimental_center_canvas_x_px =
            target.effective_experimental_center_x_canvas_px;
        review_input.effective_experimental_center_canvas_y_px =
            target.effective_experimental_center_y_canvas_px;
        review_input.canonical_experimental_radius_canvas_px =
            template_state.experimental_area_radius_px;
        review_input.accepted_rim_center_camera_x_px =
            target.accepted_rim_center_x_camera_px;
        review_input.accepted_rim_center_camera_y_px =
            target.accepted_rim_center_y_camera_px;
        review_input.accepted_rim_radius_camera_px =
            target.accepted_rim_radius_camera_px;
        const auto review = daily_geometry::ComputeGeometryReview(review_input);
        target.geometry_review_ok = review.ok;
        target.geometry_review_error = review.error;
        if (!review.ok) {
            if (error_out) *error_out = review.error + ":" + target.arena_id;
            return false;
        }
        target.alignment_basis = kRimOnlyAlignmentBasis;
        target.geometry_corrected_center_x_camera_px =
            review.corrected_center_camera_px.x;
        target.geometry_corrected_center_y_camera_px =
            review.corrected_center_camera_px.y;
        target.geometry_center_residual_x_camera_px =
            review.center_residual_x_camera_px;
        target.geometry_center_residual_y_camera_px =
            review.center_residual_y_camera_px;
        target.geometry_center_residual_norm_camera_px =
            review.center_residual_norm_camera_px;
        target.geometry_center_quantization_bound_camera_px =
            review.integer_translation_quantization_bound_camera_px;
        target.geometry_predicted_radius_min_camera_px =
            review.predicted_radius_min_camera_px;
        target.geometry_predicted_radius_mean_camera_px =
            review.predicted_radius_mean_camera_px;
        target.geometry_predicted_radius_max_camera_px =
            review.predicted_radius_max_camera_px;
        target.geometry_rim_radial_rms_error_camera_px =
            review.rim_radial_rms_error_camera_px;
        target.geometry_maximum_outside_rim_camera_px =
            review.maximum_outside_rim_camera_px;
        target.geometry_outline_camera_px.clear();
        target.geometry_outline_camera_px.reserve(
            review.canonical_outline_camera_px.size());
        for (const auto& point : review.canonical_outline_camera_px) {
            target.geometry_outline_camera_px.push_back({point.x, point.y});
        }
        if (!WriteGeometryReviewObservation(
                workflow, &target, template_state, error_out)) {
            return false;
        }
    }
    return true;
}

void AbortWorkflow(
    DailyRegistrationWorkflowUiState* workflow,
    const std::string& reason)
{
    if (workflow == nullptr) return;
    const std::string terminal_stage =
        workflow->stage == "abort_request_failed" &&
            !workflow->pending_terminal_stage.empty()
        ? workflow->pending_terminal_stage
        : "aborted";
    const std::string effective_reason =
        workflow->stage == "abort_request_failed" &&
            !workflow->pending_terminal_reason.empty()
        ? workflow->pending_terminal_reason
        : reason;
    std::string ignored;
    (void)RequestAbort(
        workflow, effective_reason, terminal_stage, &ignored);
}

bool BeginWorkflow(
    SpatialLayoutUiState* ui_state,
    const CameraParams* cameras,
    const CameraEachSelect* selections,
    int camera_count,
    SpatialSnapshotWorker* const* workers,
    const std::string& artifact_root_dir,
    std::string* error_out)
{
    if (ui_state == nullptr || cameras == nullptr || selections == nullptr ||
        workers == nullptr || camera_count <= 0) {
        if (error_out) *error_out = "Open and stream cameras before daily registration.";
        return false;
    }
    initialize_group_capture_camera_scope(
        ui_state, cameras, selections, workers, camera_count);
    if (ui_state->group_capture_selected_camera_serials.empty()) {
        if (error_out) *error_out = "Select at least one daily-registration camera.";
        return false;
    }
    if (group_capture_workflow_active(*ui_state) ||
        top_rim_observation_save_worker_is_busy() ||
        generic_calibration_image_set_save_worker_is_busy()) {
        if (error_out) *error_out =
            "Finish the active capture/save operation before daily registration.";
        return false;
    }
    DailyRegistrationWorkflowUiState workflow;
    workflow.active = true;
    workflow.stage = "waiting_base_only";
    workflow.created_utc = get_current_utc_timestamp();
    workflow.transaction_id = NextTransactionId(workflow.created_utc);
    workflow.valid_until_utc = DefaultValidUntilUtc();
    workflow.physical_state_confirmed =
        ui_state->daily_registration_workflow.start_physical_state_armed;

    std::set<std::string> arenas;
    for (const std::string& serial :
         ui_state->group_capture_selected_camera_serials) {
        const CameraParams* camera = FindCamera(cameras, camera_count, serial);
        if (camera == nullptr) {
            if (error_out) *error_out = "Selected camera is not open: " + serial;
            return false;
        }
        std::string arena_id;
        const CitrusSpatialTemplateState* matched_template = nullptr;
        for (const auto& mapping : ui_state->citrus_canvas_templates) {
            if (mapping.available && mapping.source_camera_id == serial) {
                if (matched_template == nullptr) {
                    matched_template = &mapping;
                    arena_id = mapping.source_config_name;
                } else {
                    if (error_out) {
                        *error_out =
                            "Multiple Citrus templates map to camera " + serial +
                            "; reload the intended canvas before registration.";
                    }
                    return false;
                }
            }
        }
        if (matched_template == nullptr || arena_id.empty() ||
            !matched_template->has_arena_canvas_region ||
            !matched_template
                 ->has_authoritative_camera_to_canvas_homography ||
            !matched_template->has_canvas_to_camera_homography ||
            matched_template->homography_authority_status !=
                "accepted_compatible" ||
            !arenas.insert(arena_id).second) {
            if (error_out) *error_out =
                "Load one unambiguous Citrus arena with an accepted compatible "
                "projection homography for camera " + serial + ".";
            return false;
        }
        DailyRegistrationTargetUiState target;
        target.camera_serial = serial;
        target.arena_id = arena_id;
        workflow.targets.push_back(std::move(target));
    }
    std::string session_root;
    int session_camera_index = ui_state->selected_camera;
    if (session_camera_index < 0 || session_camera_index >= camera_count) {
        session_camera_index = 0;
    }
    if (!ensure_spatial_calibration_session(
            ui_state, cameras[session_camera_index], artifact_root_dir,
            &session_root, error_out)) return false;
    workflow.session_artifact_root = session_root;
    workflow.transaction_dir =
        (fs::path(ui_state->calibration_session_dir) /
         "guided_registrations" / workflow.transaction_id).string();
    if (!ensure_directory_for_spatial_session(
            workflow.transaction_dir, error_out)) return false;

    const std::string operation = NextOperationId("select_base_only_for_measurement");
    const auto base = select_citrus_daily_registration_runtime_mode(
        "base_only", "", "", true, operation);
    if (!base.ok) {
        if (error_out) *error_out =
            "Could not select commissioned base geometry for measurement: " +
            base.reason;
        return false;
    }
    workflow.pending_operation_id = operation;
    workflow.status =
        "Selecting commissioned base-only geometry before measuring today's dish placement.";
    ui_state->daily_registration_workflow = std::move(workflow);
    Checkpoint(&ui_state->daily_registration_workflow);
    return true;
}

bool RequestSceneCapture(
    SpatialLayoutUiState* ui_state,
    const CameraParams* cameras,
    const CameraEachSelect* selections,
    int camera_count,
    SpatialSnapshotWorker* const* workers,
    const std::string& kind,
    std::string* error_out)
{
    auto& workflow = ui_state->daily_registration_workflow;
    if (kind == "rim") {
        apply_calibration_workflow_profile_defaults(
            ui_state, "installed_tank_registration", nullptr);
        ui_state->calibration_image_set_purpose = "dish_top_rim_observation";
        ui_state->calibration_image_set_target_plane = "dish_top_rim";
        ui_state->calibration_capture_stage = "daily_registration_rim";
        ui_state->calibration_projector_state = "black_reference";
        ui_state->calibration_projector_visible_to_camera = false;
        ui_state->calibration_filter_state =
            "installed: HOYA Creative Filter Infrared R72 67 mm (Kenko Tokina)";
        ui_state->calibration_runtime_filter_state =
            ui_state->calibration_filter_state;
        ui_state->calibration_requires_filter_reinstalled_repeatably = false;
        ui_state->calibration_light_handling =
            "keep_or_restore_mapped_pulse";
        apply_illumination_preset(
            ui_state, "custom_ttl_nir_strobe_855nm");
        ui_state->group_capture_scene_recipe = "black_reference";
    } else {
        apply_calibration_workflow_profile_defaults(
            ui_state, "installed_tank_registration", nullptr);
        ui_state->calibration_image_set_purpose = "crosshair_alignment";
        ui_state->calibration_image_set_target_plane = "projected_surface";
        ui_state->calibration_capture_stage = "daily_registration_base_center";
        ui_state->calibration_projector_state = "calibration_pattern";
        ui_state->calibration_projector_visible_to_camera = true;
        ui_state->group_capture_scene_recipe =
            "experimental_area_center_and_outline";
    }
    ui_state->group_capture_scene_options = nlohmann::json::object();
    const std::string operation = NextOperationId(kind + "_scene");
    if (!request_group_full_resolution_snapshots(
            ui_state, cameras, selections, camera_count, workers,
            std::max(1, ui_state->calibration_average_frame_count),
            error_out, workflow.transaction_id, operation)) {
        return false;
    }
    workflow.pending_operation_id = operation;
    workflow.pending_group_kind = kind;
    workflow.stage = kind == "rim"
        ? "waiting_rim_capture"
        : "waiting_projected_center_capture";
    workflow.status = kind == "rim"
        ? "Capturing a black-reference view of each water-side inner rim."
        : "Capturing each base projected center against its physical rim.";
    Checkpoint(&workflow);
    return true;
}

bool RequestCandidate(
    DailyRegistrationWorkflowUiState* workflow,
    std::string* error_out)
{
    const std::string operation = NextOperationId("create_candidate");
    const auto result = create_citrus_daily_registration_candidate(
        workflow->transaction_id,
        CandidateObservations(*workflow),
        operation);
    if (!result.ok) {
        if (error_out) *error_out = result.reason;
        return false;
    }
    workflow->pending_operation_id = operation;
    workflow->stage = "waiting_candidate";
    workflow->status = "Citrus is computing the translation-only candidate.";
    Checkpoint(workflow);
    return true;
}

bool RequestPreview(
    DailyRegistrationWorkflowUiState* workflow,
    std::string* error_out)
{
    const std::string operation = NextOperationId("preview_candidate");
    const auto result = preview_citrus_daily_registration_candidate(
        workflow->transaction_id, operation);
    if (!result.ok) {
        if (error_out) *error_out = result.reason;
        return false;
    }
    workflow->pending_operation_id = operation;
    workflow->stage = "waiting_preview_presented";
    workflow->status =
        "Waiting for Citrus to present the transient candidate preview.";
    Checkpoint(workflow);
    return true;
}

bool AcceptAndSelect(
    DailyRegistrationWorkflowUiState* workflow,
    std::string* error_out)
{
    nlohmann::json target_checks = nlohmann::json::array();
    for (const auto& target : workflow->targets) {
        target_checks.push_back({
            {"arena_id", target.arena_id},
            {"camera_id", target.camera_serial},
            {"alignment_basis", target.alignment_basis},
            {"translation_canvas_px", {
                {"requested_x", target.requested_translation_x_canvas_px},
                {"requested_y", target.requested_translation_y_canvas_px},
                {"applied_x", target.applied_translation_x_canvas_px},
                {"applied_y", target.applied_translation_y_canvas_px}}},
            {"center_residual_camera_px", {
                {"x", target.geometry_center_residual_x_camera_px},
                {"y", target.geometry_center_residual_y_camera_px},
                {"norm", target.geometry_center_residual_norm_camera_px}}},
            {"integer_translation_quantization_bound_camera_px",
             target.geometry_center_quantization_bound_camera_px},
            {"radius_qc_camera_px", {
                {"predicted_minimum",
                 target.geometry_predicted_radius_min_camera_px},
                {"predicted_mean",
                 target.geometry_predicted_radius_mean_camera_px},
                {"predicted_maximum",
                 target.geometry_predicted_radius_max_camera_px},
                {"accepted_rim", target.accepted_rim_radius_camera_px},
                {"maximum_outside_rim",
                 target.geometry_maximum_outside_rim_camera_px}}},
            {"geometry_review_observation", {
                {"path", target.geometry_review_observation_path},
                {"sha256", target.geometry_review_observation_sha256}}},
        });
    }
    const nlohmann::json verification = {
        {"result", "passed"},
        {"policy", "orange_guided_daily_registration_rim_only_v2"},
        {"alignment_basis", kRimOnlyAlignmentBasis},
        {"maximum_residual_beyond_integer_translation_quantization_camera_px",
         workflow->maximum_geometry_residual_beyond_quantization_camera_px},
        {"operator_confirmed_outline_containment",
         workflow->geometry_outline_operator_confirmed},
        {"runtime_optical_path_changed", false},
        {"canonical_experimental_area_resized", false},
        {"targets", std::move(target_checks)},
    };
    const std::string operation = NextOperationId("accept_candidate");
    const auto result = accept_citrus_daily_registration(
        workflow->transaction_id,
        workflow->candidate_sha256,
        workflow->valid_until_utc,
        verification,
        true,
        operation);
    if (!result.ok) {
        if (error_out) *error_out = result.reason;
        return false;
    }
    workflow->pending_operation_id = operation;
    workflow->stage = "waiting_accept";
    workflow->status =
        "Citrus is writing the immutable accepted registration; runtime selection remains separate.";
    Checkpoint(workflow);
    return true;
}

void RenderDailyReviewImages(
    const SpatialLayoutUiState& ui_state,
    const DailyRegistrationWorkflowUiState& workflow,
    const std::string& mode)
{
    const int columns = std::clamp(
        static_cast<int>(workflow.targets.size()), 1, 4);
    if (!ImGui::BeginTable(
            ("daily-review-images-" + mode).c_str(), columns,
            ImGuiTableFlags_SizingStretchSame)) {
        return;
    }
    for (const auto& target : workflow.targets) {
        ImGui::TableNextColumn();
        ImGui::Text("%s / Cam%s", target.arena_id.c_str(),
                    target.camera_serial.c_str());
        const auto* capture = FindCapture(ui_state, target.camera_serial);
        if (capture == nullptr || capture->texture == 0 ||
            capture->width <= 0 || capture->height <= 0) {
            ImGui::TextDisabled("Review image unavailable");
            continue;
        }
        const float available = std::max(
            1.0f, ImGui::GetContentRegionAvail().x);
        const float scale = std::min(
            1.0f,
            std::min(
                available / static_cast<float>(capture->width),
                300.0f / static_cast<float>(capture->height)));
        const ImVec2 display_size(
            std::max(1.0f, capture->width * scale),
            std::max(1.0f, capture->height * scale));
        ImGui::Image(
            reinterpret_cast<ImTextureID>(
                static_cast<intptr_t>(capture->texture)),
            display_size, ImVec2(0, 0), ImVec2(1, 1));
        const ImVec2 origin = ImGui::GetItemRectMin();
        ImDrawList* draw = ImGui::GetWindowDrawList();
        const auto point = [&](double x, double y) {
            return ImVec2(
                origin.x + static_cast<float>(x) * scale,
                origin.y + static_cast<float>(y) * scale);
        };
        const auto circle = [&](double x, double y, double radius,
                                ImU32 color, float thickness) {
            if (radius > 0.0) {
                draw->AddCircle(
                    point(x, y), static_cast<float>(radius) * scale,
                    color, 128, thickness);
            }
        };
        const auto cross = [&](double x, double y, ImU32 color) {
            const ImVec2 center = point(x, y);
            constexpr float arm = 7.0f;
            draw->AddLine(
                ImVec2(center.x - arm, center.y),
                ImVec2(center.x + arm, center.y), color, 2.0f);
            draw->AddLine(
                ImVec2(center.x, center.y - arm),
                ImVec2(center.x, center.y + arm), color, 2.0f);
        };
        if (mode == "rim") {
            circle(
                target.detected_rim_center_x_camera_px,
                target.detected_rim_center_y_camera_px,
                target.detected_rim_radius_camera_px,
                IM_COL32(255, 32, 210, 255), 1.5f);
        }
        circle(
            target.accepted_rim_center_x_camera_px,
            target.accepted_rim_center_y_camera_px,
            target.accepted_rim_radius_camera_px,
            IM_COL32(255, 165, 32, 255), 2.0f);
        if (mode == "base_center") {
            cross(
                target.projected_center_x_camera_px,
                target.projected_center_y_camera_px,
                IM_COL32(32, 220, 255, 255));
        } else if (mode == "preview") {
            const bool pass = target.preview_detection_ok &&
                target.preview_residual_norm_camera_px <=
                    workflow.maximum_preview_center_residual_camera_px;
            cross(
                target.preview_center_x_camera_px,
                target.preview_center_y_camera_px,
                pass ? IM_COL32(40, 230, 100, 255)
                     : IM_COL32(255, 70, 50, 255));
        } else if (mode == "geometry") {
            const bool pass = target.geometry_review_ok &&
                target.geometry_center_residual_norm_camera_px <=
                    target.geometry_center_quantization_bound_camera_px +
                    workflow
                        .maximum_geometry_residual_beyond_quantization_camera_px;
            const ImU32 outline_color = pass
                ? IM_COL32(70, 255, 150, 255)
                : IM_COL32(255, 70, 50, 255);
            for (std::size_t index = 0;
                 index < target.geometry_outline_camera_px.size(); ++index) {
                const auto& a = target.geometry_outline_camera_px[index];
                const auto& b = target.geometry_outline_camera_px[
                    (index + 1) % target.geometry_outline_camera_px.size()];
                draw->AddLine(
                    point(a.x, a.y), point(b.x, b.y),
                    outline_color, 2.4f);
            }
            cross(
                target.geometry_corrected_center_x_camera_px,
                target.geometry_corrected_center_y_camera_px,
                outline_color);
        }
    }
    ImGui::EndTable();
    if (mode == "rim") {
        ImGui::TextDisabled(
            "Magenta: raw Hough proposal. Orange: operator-adjustable accepted water-side inner rim.");
    } else if (mode == "base_center") {
        ImGui::TextDisabled(
            "Orange: accepted physical inner rim. Cyan cross: detected commissioned/base projected center.");
    } else if (mode == "geometry") {
        ImGui::TextDisabled(
            "Orange: accepted physical inner rim. Green/red: unchanged canonical experimental area inverse-projected through the accepted commissioning homography after Citrus's exact integer translation.");
    } else {
        ImGui::TextDisabled(
            "Orange: accepted physical inner rim. Green/red cross: corrected center pass/fail; inspect the projected outline in the image.");
    }
}

void RenderTargetTable(
    DailyRegistrationWorkflowUiState* workflow,
    bool rim,
    bool preview)
{
    if (!ImGui::BeginTable(
            rim ? "daily-rim-targets" :
                (preview ? "daily-preview-targets" : "daily-marker-targets"),
            rim ? 6 : 5,
            ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                ImGuiTableFlags_SizingStretchProp)) return;
    ImGui::TableSetupColumn("Target");
    ImGui::TableSetupColumn(rim ? "Center X" : "Detected X");
    ImGui::TableSetupColumn(rim ? "Center Y" : "Detected Y");
    ImGui::TableSetupColumn(rim ? "Radius" : "Residual");
    ImGui::TableSetupColumn("QC");
    if (rim) ImGui::TableSetupColumn("Confirm inner rim");
    ImGui::TableHeadersRow();
    for (auto& target : workflow->targets) {
        ImGui::PushID((target.camera_serial + target.arena_id +
                       (rim ? "rim" : preview ? "preview" : "marker")).c_str());
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        ImGui::Text("%s / %s", target.arena_id.c_str(),
                    target.camera_serial.c_str());
        ImGui::TableSetColumnIndex(1);
        if (rim) ImGui::InputDouble("##cx", &target.accepted_rim_center_x_camera_px,
                                    0.1, 1.0, "%.3f");
        else ImGui::Text("%.3f", preview ? target.preview_center_x_camera_px :
                                      target.projected_center_x_camera_px);
        ImGui::TableSetColumnIndex(2);
        if (rim) ImGui::InputDouble("##cy", &target.accepted_rim_center_y_camera_px,
                                    0.1, 1.0, "%.3f");
        else ImGui::Text("%.3f", preview ? target.preview_center_y_camera_px :
                                      target.projected_center_y_camera_px);
        ImGui::TableSetColumnIndex(3);
        if (rim) ImGui::InputDouble("##r", &target.accepted_rim_radius_camera_px,
                                    0.1, 1.0, "%.3f");
        else if (preview) ImGui::Text("%.3f px",
                                      target.preview_residual_norm_camera_px);
        else ImGui::Text("(%.2f, %.2f)",
                         target.base_center_residual_x_camera_px,
                         target.base_center_residual_y_camera_px);
        ImGui::TableSetColumnIndex(4);
        const bool ok = rim ? target.rim_detection_ok :
            (preview ? target.preview_detection_ok :
                       target.projected_center_detection_ok);
        ImGui::TextColored(ok ? ImVec4(0.25f, 0.9f, 0.35f, 1.0f)
                              : ImVec4(1.0f, 0.45f, 0.25f, 1.0f),
                           "%s", ok ? "detected" : "FAILED");
        if (rim) {
            ImGui::TableSetColumnIndex(5);
            ImGui::Checkbox("##confirm", &target.rim_operator_confirmed);
        } else if (!preview) {
            ImGui::SameLine();
            ImGui::Checkbox("confirm##marker",
                            &target.projected_center_operator_confirmed);
        }
        ImGui::PopID();
    }
    ImGui::EndTable();
}

void RenderGeometryReviewTable(
    const DailyRegistrationWorkflowUiState& workflow)
{
    if (!ImGui::BeginTable(
            "daily-rim-only-geometry-targets", 7,
            ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                ImGuiTableFlags_SizingStretchProp)) return;
    ImGui::TableSetupColumn("Target");
    ImGui::TableSetupColumn("Applied canvas shift");
    ImGui::TableSetupColumn("Center residual");
    ImGui::TableSetupColumn("Canonical radius in camera");
    ImGui::TableSetupColumn("Fitted rim radius");
    ImGui::TableSetupColumn("Max outside rim");
    ImGui::TableSetupColumn("QC");
    ImGui::TableHeadersRow();
    for (const auto& target : workflow.targets) {
        const bool pass = target.geometry_review_ok &&
            target.geometry_center_residual_norm_camera_px <=
                target.geometry_center_quantization_bound_camera_px +
                workflow.maximum_geometry_residual_beyond_quantization_camera_px;
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        ImGui::Text("%s / %s", target.arena_id.c_str(),
                    target.camera_serial.c_str());
        ImGui::TableSetColumnIndex(1);
        ImGui::Text("(%d, %d) px",
                    target.applied_translation_x_canvas_px,
                    target.applied_translation_y_canvas_px);
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip(
                "Requested (%.4f, %.4f) px; integer-rounding residual (%.4f, %.4f) px",
                target.requested_translation_x_canvas_px,
                target.requested_translation_y_canvas_px,
                target.translation_rounding_residual_x_canvas_px,
                target.translation_rounding_residual_y_canvas_px);
        }
        ImGui::TableSetColumnIndex(2);
        ImGui::Text("%.3f px (quant. <= %.3f)",
                    target.geometry_center_residual_norm_camera_px,
                    target.geometry_center_quantization_bound_camera_px);
        ImGui::TableSetColumnIndex(3);
        ImGui::Text("%.2f [%.2f, %.2f] px",
                    target.geometry_predicted_radius_mean_camera_px,
                    target.geometry_predicted_radius_min_camera_px,
                    target.geometry_predicted_radius_max_camera_px);
        ImGui::TableSetColumnIndex(4);
        ImGui::Text("%.2f px", target.accepted_rim_radius_camera_px);
        ImGui::TableSetColumnIndex(5);
        ImGui::Text("%.2f px",
                    target.geometry_maximum_outside_rim_camera_px);
        ImGui::TableSetColumnIndex(6);
        ImGui::TextColored(
            pass ? ImVec4(0.25f, 0.9f, 0.35f, 1.0f)
                 : ImVec4(1.0f, 0.45f, 0.25f, 1.0f),
            "%s", pass ? "center passed" : "FAILED");
    }
    ImGui::EndTable();
    ImGui::TextDisabled(
        "Radius values are QC only. This workflow never changes the canonical experimental-area radius.");
}

}  // namespace

void advance_daily_registration_workflow(
    SpatialLayoutUiState* ui_state,
    const CameraParams* cameras,
    const CameraEachSelect* selections,
    int camera_count,
    SpatialSnapshotWorker* const* workers,
    const std::string&)
{
    if (ui_state == nullptr) return;
    auto& workflow = ui_state->daily_registration_workflow;
    if (!workflow.active) return;
    const auto fail = [&](const std::string& error) {
        std::string ignored;
        if (!RequestAbort(&workflow, error, "failed", &ignored) &&
            workflow.stage != "abort_request_failed") {
            SetFailure(&workflow, error);
        }
    };

    if (workflow.stage == "waiting_abort" ||
        workflow.stage == "waiting_reject") {
        const auto status = query_citrus_daily_registration_status(
            workflow.stage == "waiting_abort"
                ? "guided_daily_waiting_abort"
                : "guided_daily_waiting_reject");
        if (!status.ok) return;
        const auto& daily = status.daily_registration;
        const std::string expected_transition =
            workflow.stage == "waiting_abort" ? "aborted" : "rejected";
        if (daily.value("active", true) ||
            JsonString(daily, "operation_id") != workflow.pending_operation_id ||
            JsonString(daily, "transition") != expected_transition) {
            return;
        }
        workflow.active = false;
        workflow.stage = workflow.pending_terminal_stage.empty()
            ? expected_transition
            : workflow.pending_terminal_stage;
        workflow.status = workflow.stage == "failed"
            ? "Daily registration stopped after Citrus acknowledged safe restoration; commissioned base geometry remains available."
            : (workflow.stage == "rejected"
                ? "Candidate rejected after Citrus acknowledged safe restoration; the prior runtime selection was not replaced."
                : "Daily registration aborted after Citrus acknowledged safe restoration; commissioned base geometry remains available.");
        workflow.pending_terminal_stage.clear();
        workflow.pending_terminal_reason.clear();
        Checkpoint(&workflow);
        return;
    }

    if (workflow.stage == "waiting_base_only") {
        const auto status = query_citrus_daily_registration_status(
            "guided_daily_waiting_base_only");
        if (!status.ok) return;
        const auto& daily = status.daily_registration;
        const auto runtime = status.daily_registration.value(
            "runtime", nlohmann::json::object());
        if (JsonString(daily, "operation_id") != workflow.pending_operation_id ||
            JsonString(daily, "transition") != "base_only_selected" ||
            runtime.value("mode", std::string()) != "base_only" ||
            !runtime.value("all_selected_runtime_safe", false)) {
            return;
        }
        nlohmann::json targets = nlohmann::json::array();
        for (const auto& target : workflow.targets) {
            targets.push_back({{"arena_id", target.arena_id},
                               {"camera_id", target.camera_serial}});
        }
        const std::string operation = NextOperationId("begin");
        const auto result = begin_citrus_daily_registration(
            workflow.transaction_id, targets, operation);
        if (!result.ok) {
            SetFailure(&workflow, result.reason);
            return;
        }
        workflow.pending_operation_id = operation;
        workflow.stage = "waiting_begin";
        workflow.status = "Acquiring the Citrus daily-registration lease.";
        Checkpoint(&workflow);
        return;
    }
    if (workflow.stage == "waiting_begin") {
        const auto status = query_citrus_daily_registration_status(
            "guided_daily_waiting_begin");
        if (!status.ok) return;
        if (status.daily_registration.value("active", false) &&
            JsonString(status.daily_registration, "transaction_id") ==
                workflow.transaction_id &&
            JsonString(status.daily_registration, "operation_id") ==
                workflow.pending_operation_id &&
            JsonString(status.daily_registration, "transition") == "begun") {
            workflow.citrus_begin_status = status.daily_registration;
            workflow.stage = "ready_rim_capture";
            workflow.status =
                "Lease acquired. Capture and review the water-side inner rims.";
            Checkpoint(&workflow);
        }
        return;
    }
    if (workflow.stage == "waiting_rim_capture" ||
        workflow.stage == "waiting_projected_center_capture" ||
        workflow.stage == "waiting_preview_capture") {
        if (group_capture_workflow_active(*ui_state)) return;
        if (ui_state->group_capture_workflow_state == "failed" ||
            ui_state->group_capture_terminal_outcome != "complete") {
            fail("daily_group_capture_failed:" + ui_state->group_capture_error);
            return;
        }
        if (ui_state->group_capture_workflow_state != "complete") return;
        std::string error;
        if (workflow.stage == "waiting_rim_capture") {
            if (!AnalyzeRimCaptures(ui_state, &workflow, &error)) {
                fail(error);
                return;
            }
            workflow.stage = "review_rims";
            workflow.status =
                "Review every proposed circle and confirm the water-side inner rim.";
        } else {
            const bool preview = workflow.stage == "waiting_preview_capture";
            if (!AnalyzeMarkerCaptures(
                    ui_state, &workflow, preview, &error) ||
                !QueueGenericCaptureSaves(
                    ui_state, &workflow, cameras, camera_count,
                    preview, &error)) {
                fail(error);
                return;
            }
            workflow.stage = preview
                ? "waiting_preview_save"
                : "waiting_projected_center_save";
            workflow.status = preview
                ? "Saving candidate validation captures and overlays."
                : "Saving base-center captures and detection evidence.";
        }
        Checkpoint(&workflow);
        return;
    }
    if (workflow.stage == "waiting_rim_save") {
        if (top_rim_observation_save_worker_is_busy()) return;
        for (auto& target : workflow.targets) {
            std::string error;
            if (!fs::exists(target.rim_observation_path) ||
                !checksum::file_sha256(
                    target.rim_observation_path,
                    &target.rim_observation_sha256,
                    &error)) {
                fail(error.empty() ? "daily_rim_artifact_missing" : error);
                return;
            }
        }
        std::string error;
        if (!RequestCandidate(&workflow, &error)) {
            fail(error.empty() ? "daily_candidate_request_failed" : error);
        }
        return;
    }
    if (workflow.stage == "waiting_projected_center_save") {
        if (generic_calibration_image_set_save_worker_is_busy()) return;
        for (auto& target : workflow.targets) {
            std::string error;
            if (!VerifyGenericCaptureSave(
                    target.projected_center_capture,
                    target.projected_center_source_path,
                    target.projected_center_image_set_path,
                    target.projected_center_manifest_path,
                    "daily_registration_base_center",
                    &target.projected_center_source_sha256,
                    &error)) {
                fail(error.empty() ? "projected_center_evidence_missing" : error);
                return;
            }
        }
        workflow.stage = "review_projected_centers";
        workflow.status =
            "Review each detected projected center before asking Citrus for a candidate.";
        Checkpoint(&workflow);
        return;
    }
    if (workflow.stage == "waiting_candidate") {
        const auto status = query_citrus_daily_registration_status(
            "guided_daily_waiting_candidate");
        if (!status.ok) return;
        if (JsonString(status.daily_registration, "transaction_id") !=
                workflow.transaction_id ||
            JsonString(status.daily_registration, "operation_id") !=
                workflow.pending_operation_id ||
            JsonString(status.daily_registration, "transition") !=
                "candidate_created" ||
            JsonString(status.daily_registration, "candidate_sha256").empty()) return;
        workflow.citrus_candidate_status = status.daily_registration;
        workflow.candidate_path =
            JsonString(status.daily_registration, "candidate_path");
        workflow.candidate_sha256 =
            JsonString(status.daily_registration, "candidate_sha256");
        std::string error;
        if (!LoadCandidateAndBuildGeometryReview(
                *ui_state, &workflow, &error)) {
            fail(error.empty()
                ? "daily_geometry_review_generation_failed"
                : error);
            return;
        }
        workflow.stage = "review_geometry_candidate";
        workflow.status =
            "Translation-only candidate ready. Review the inverse-projected canonical outline; no visible marker or runtime preview was required.";
        Checkpoint(&workflow);
        return;
    }
    if (workflow.stage == "waiting_preview_presented") {
        const auto status = query_citrus_daily_registration_status(
            "guided_daily_waiting_preview");
        if (!status.ok) return;
        const auto& daily = status.daily_registration;
        if (!daily.value("preview_active", false) ||
            !daily.value("visible", false) ||
            JsonString(daily, "transaction_id") != workflow.transaction_id ||
            JsonString(daily, "candidate_sha256") != workflow.candidate_sha256 ||
            JsonString(daily, "operation_id") != workflow.pending_operation_id) return;
        std::string error;
        apply_calibration_workflow_profile_defaults(
            ui_state, "installed_tank_registration", nullptr);
        ui_state->calibration_image_set_purpose = "crosshair_alignment";
        ui_state->calibration_image_set_target_plane = "projected_surface";
        ui_state->calibration_capture_stage = "daily_registration_candidate_validation";
        ui_state->calibration_projector_state = "calibration_pattern";
        ui_state->calibration_projector_visible_to_camera = true;
        if (!request_group_full_resolution_snapshots_for_daily_registration_preview(
                ui_state, cameras, selections, camera_count, workers,
                std::max(1, ui_state->calibration_average_frame_count),
                workflow.transaction_id,
                workflow.candidate_sha256,
                workflow.pending_operation_id,
                &error)) {
            fail(error);
            return;
        }
        workflow.stage = "waiting_preview_capture";
        workflow.status = "Capturing the transient corrected center and outline.";
        Checkpoint(&workflow);
        return;
    }
    if (workflow.stage == "waiting_preview_save") {
        if (generic_calibration_image_set_save_worker_is_busy()) return;
        for (auto& target : workflow.targets) {
            std::string error;
            if (!VerifyGenericCaptureSave(
                    target.preview_capture,
                    target.preview_source_path,
                    target.preview_image_set_path,
                    target.preview_manifest_path,
                    "daily_registration_candidate_validation",
                    &target.preview_source_sha256,
                    &error) ||
                !WriteValidationObservation(&workflow, &target, &error)) {
                fail(error.empty() ? "preview_validation_evidence_missing" : error);
                return;
            }
            ClearCapturePixels(&target.preview_capture);
            target.rim_gray.clear();
            target.rim_gray.shrink_to_fit();
        }
        workflow.stage = "review_preview";
        workflow.status =
            "Review center residuals and confirm the projected outline is contained by each physical inner rim.";
        Checkpoint(&workflow);
        return;
    }
    if (workflow.stage == "waiting_accept") {
        const auto status = query_citrus_daily_registration_status(
            "guided_daily_waiting_accept");
        if (!status.ok) return;
        if (status.daily_registration.value("active", true) ||
            JsonString(status.daily_registration, "transaction_id") !=
                workflow.transaction_id ||
            JsonString(status.daily_registration, "operation_id") !=
                workflow.pending_operation_id ||
            JsonString(status.daily_registration, "transition") !=
                "accepted_not_selected") {
            return;
        }
        const std::string path =
            JsonString(status.daily_registration, "accepted_registration_path");
        const std::string sha =
            JsonString(status.daily_registration, "accepted_registration_sha256");
        if (path.empty() || sha.empty()) return;
        workflow.citrus_accept_status = status.daily_registration;
        workflow.accepted_registration_path = path;
        workflow.accepted_registration_sha256 = sha;
        const std::string operation = NextOperationId("select_accepted");
        const auto selection = select_citrus_daily_registration_runtime_mode(
            "selected_daily_registration", path, sha, true, operation);
        if (!selection.ok) {
            SetFailure(&workflow,
                "registration_accepted_but_runtime_selection_failed:" +
                selection.reason);
            return;
        }
        workflow.pending_operation_id = operation;
        workflow.stage = "waiting_runtime_selection";
        workflow.status =
            "Registration accepted; explicitly selecting that exact artifact for runtime.";
        Checkpoint(&workflow);
        return;
    }
    if (workflow.stage == "waiting_runtime_selection") {
        const auto status = query_citrus_daily_registration_status(
            "guided_daily_waiting_runtime_selection");
        if (!status.ok) return;
        if (JsonString(status.daily_registration, "operation_id") !=
                workflow.pending_operation_id ||
            JsonString(status.daily_registration, "transition") !=
                "daily_registration_selected") {
            return;
        }
        const auto runtime = status.daily_registration.value(
            "runtime", nlohmann::json::object());
        bool exact_registration =
            runtime.value("mode", std::string()) ==
                "selected_daily_registration" &&
            runtime.value("all_selected_runtime_safe", false) &&
            runtime.contains("targets") && runtime["targets"].is_array() &&
            !runtime["targets"].empty();
        if (exact_registration) {
            for (const auto& target : runtime["targets"]) {
                exact_registration = exact_registration && target.is_object() &&
                    target.value("registration_sha256", std::string()) ==
                        workflow.accepted_registration_sha256 &&
                    target.value("state", std::string()) == "selected_valid";
            }
        }
        if (!exact_registration) return;
        workflow.citrus_runtime_selection_status = runtime;
        workflow.stage = "complete";
        workflow.active = false;
        workflow.status =
            "Daily registration accepted and selected for runtime.";
        for (auto& target : workflow.targets) {
            ClearCapturePixels(&target.rim_capture);
            target.rim_gray.clear();
            target.rim_gray.shrink_to_fit();
        }
        Checkpoint(&workflow);
        std::string ignored;
        WriteJsonAtomically(
            fs::path(workflow.transaction_dir) / "manifest.json",
            WorkflowSnapshot(workflow), &ignored);
        ui_state->daily_registration_status = status.daily_registration;
    }
}

void render_daily_registration_workflow_panel(
    SpatialLayoutUiState* ui_state,
    const CameraParams* cameras,
    const CameraEachSelect* selections,
    int camera_count,
    SpatialSnapshotWorker* const* workers,
    const std::string& artifact_root_dir)
{
    if (ui_state == nullptr) return;
    auto& workflow = ui_state->daily_registration_workflow;
    ImGui::SeparatorText("Guided Daily Dish Registration (Optional)");
    ImGui::TextWrapped(
        "This opt-in procedure measures only today's dish-placement translation. "
        "It never rewrites commissioned canvas geometry, homographies, scale, "
        "arena size, or the canonical 40 mm experimental-area radius. It runs "
        "with the normal IR filters and experiment illumination path; no visible "
        "projector marker or filter change is required.");
    if (workflow.stage == "idle" || workflow.stage == "complete" ||
        workflow.stage == "failed" || workflow.stage == "aborted" ||
        workflow.stage == "rejected") {
        ImGui::Checkbox(
            "Holder and dishes installed; water is settled; normal IR filters and illumination are in place",
            &workflow.start_physical_state_armed);
        ImGui::BeginDisabled(!workflow.start_physical_state_armed);
        if (ImGui::Button("Start Guided Daily Registration")) {
            std::string error;
            if (!BeginWorkflow(
                    ui_state, cameras, selections, camera_count, workers,
                    artifact_root_dir, &error)) {
                workflow.error = error;
            }
        }
        ImGui::EndDisabled();
    }
    ImGui::Text("Stage: %s", workflow.stage.c_str());
    if (!workflow.transaction_id.empty()) {
        ImGui::TextDisabled("Transaction: %s", workflow.transaction_id.c_str());
    }
    if (!workflow.status.empty()) ImGui::TextWrapped("%s", workflow.status.c_str());
    if (!workflow.error.empty()) {
        ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.25f, 1.0f),
                           "%s", workflow.error.c_str());
    }

    if (workflow.stage == "ready_rim_capture") {
        ImGui::TextWrapped(
            "Citrus will show black while Orange keeps the mapped experiment IR "
            "pulse path. Orange will acquire one PTP-grouped full-resolution "
            "temporal mean and propose each water-side inner rim.");
        if (ImGui::Button("Capture Physical Inner Rims")) {
            std::string error;
            if (!RequestSceneCapture(
                    ui_state, cameras, selections, camera_count, workers,
                    "rim", &error)) workflow.error = error;
        }
    } else if (workflow.stage == "review_rims") {
        RenderDailyReviewImages(*ui_state, workflow, "rim");
        RenderTargetTable(&workflow, true, false);
        ImGui::BeginDisabled(!AllRimsConfirmed(workflow));
        if (ImGui::Button("Accept Rims And Compute Translation")) {
            std::string error;
            if (!QueueRimArtifacts(
                    ui_state, &workflow, cameras, camera_count, &error)) {
                workflow.error = error;
            } else {
                workflow.stage = "waiting_rim_save";
                workflow.status = "Saving per-camera schema-v2 top-rim artifacts.";
                workflow.error.clear();
                Checkpoint(&workflow);
            }
        }
        ImGui::EndDisabled();
    } else if (workflow.stage == "review_geometry_candidate") {
        ImGui::TextWrapped(
            "Candidate: %s. Orange has inverse-projected the unchanged canonical "
            "experimental area using the accepted commissioning homography and "
            "Citrus's exact integer canvas translation.",
            workflow.candidate_sha256.c_str());
        RenderDailyReviewImages(*ui_state, workflow, "geometry");
        RenderGeometryReviewTable(workflow);
        ImGui::InputDouble(
            "Allowed residual beyond integer-translation quantization (camera px)",
            &workflow.maximum_geometry_residual_beyond_quantization_camera_px,
            0.1, 0.5, "%.3f");
        ImGui::Checkbox(
            "I confirm each computed canonical outline is acceptable relative to the physical inner rim",
            &workflow.geometry_outline_operator_confirmed);
        ImGui::InputText("Valid until UTC", &workflow.valid_until_utc);
        ImGui::Checkbox(
            "I accept this exact candidate and its computed geometry evidence",
            &workflow.accept_registration_armed);
        ImGui::Checkbox(
            "After acceptance, select this exact registration for runtime",
            &workflow.select_runtime_mode_armed);
        const bool ready = GeometryReviewPasses(workflow) &&
            workflow.geometry_outline_operator_confirmed &&
            workflow.accept_registration_armed &&
            workflow.select_runtime_mode_armed &&
            !workflow.valid_until_utc.empty();
        ImGui::BeginDisabled(!ready);
        if (ImGui::Button("Accept And Select Daily Registration")) {
            std::string error;
            if (!AcceptAndSelect(&workflow, &error)) workflow.error = error;
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button("Reject Candidate")) {
            const std::string operation = NextOperationId("reject_candidate");
            const auto result = reject_citrus_daily_registration(
                workflow.transaction_id,
                "operator_rejected_rim_only_candidate", operation);
            if (!result.ok) workflow.error = result.reason;
            else {
                workflow.pending_operation_id = operation;
                workflow.pending_terminal_stage = "rejected";
                workflow.pending_terminal_reason =
                    "operator_rejected_rim_only_candidate";
                workflow.stage = "waiting_reject";
                workflow.status =
                    "Candidate rejection requested; waiting for Citrus acknowledgement.";
                workflow.error.clear();
                Checkpoint(&workflow);
            }
        }
    } else if (workflow.stage == "ready_projected_center_capture") {
        ImGui::Checkbox(
            "Projected visible marker can be seen by every selected camera",
            &workflow.visible_projection_path_confirmed);
        ImGui::TextWrapped(
            "Confirm filters/illumination physically; Orange does not infer their state from yesterday's session.");
        ImGui::BeginDisabled(!workflow.visible_projection_path_confirmed);
        if (ImGui::Button("Capture Base Projected Centers")) {
            std::string error;
            if (!RequestSceneCapture(
                    ui_state, cameras, selections, camera_count, workers,
                    "projected_center", &error)) workflow.error = error;
        }
        ImGui::EndDisabled();
    } else if (workflow.stage == "review_projected_centers") {
        RenderDailyReviewImages(*ui_state, workflow, "base_center");
        RenderTargetTable(&workflow, false, false);
        ImGui::BeginDisabled(!AllProjectedCentersConfirmed(workflow));
        if (ImGui::Button("Create Translation-Only Candidate")) {
            std::string error;
            bool evidence_ok = true;
            for (auto& target : workflow.targets) {
                if (!WriteProjectedCenterObservation(
                        &workflow, &target, &error)) {
                    evidence_ok = false;
                    break;
                }
            }
            if (evidence_ok && RequestCandidate(&workflow, &error)) {
                for (auto& target : workflow.targets) {
                    ClearCapturePixels(&target.projected_center_capture);
                }
                workflow.error.clear();
            } else {
                workflow.error = error;
            }
        }
        ImGui::EndDisabled();
    } else if (workflow.stage == "candidate_ready") {
        ImGui::TextWrapped("Candidate: %s", workflow.candidate_sha256.c_str());
        if (ImGui::Button("Render And Capture Candidate Preview")) {
            std::string error;
            if (!RequestPreview(&workflow, &error)) workflow.error = error;
        }
        ImGui::SameLine();
        if (ImGui::Button("Reject Candidate")) {
            const std::string operation = NextOperationId("reject_candidate");
            const auto result = reject_citrus_daily_registration(
                workflow.transaction_id, "operator_rejected_candidate", operation);
            if (!result.ok) workflow.error = result.reason;
            else {
                workflow.pending_operation_id = operation;
                workflow.pending_terminal_stage = "rejected";
                workflow.pending_terminal_reason =
                    "operator_rejected_candidate";
                workflow.stage = "waiting_reject";
                workflow.status =
                    "Candidate rejection requested; waiting for Citrus to restore the preview and acknowledge the transaction as inactive.";
                workflow.error.clear();
                Checkpoint(&workflow);
            }
        }
    } else if (workflow.stage == "review_preview") {
        RenderDailyReviewImages(*ui_state, workflow, "preview");
        RenderTargetTable(&workflow, false, true);
        ImGui::InputDouble(
            "Maximum center residual (camera px)",
            &workflow.maximum_preview_center_residual_camera_px,
            0.25, 1.0, "%.3f");
        ImGui::Checkbox(
            "I confirm each candidate outline is acceptably contained by the physical inner rim",
            &workflow.preview_outline_containment_confirmed);
        ImGui::Checkbox(
            "Runtime camera filters and illumination path are restored for experiments",
            &workflow.runtime_optical_state_restored_confirmed);
        ImGui::InputText("Valid until UTC", &workflow.valid_until_utc);
        ImGui::Checkbox(
            "I accept this exact candidate and its validation evidence",
            &workflow.accept_registration_armed);
        ImGui::Checkbox(
            "After acceptance, select this exact registration for runtime",
            &workflow.select_runtime_mode_armed);
        const bool ready = PreviewPasses(workflow) &&
            workflow.preview_outline_containment_confirmed &&
            workflow.runtime_optical_state_restored_confirmed &&
            workflow.accept_registration_armed &&
            workflow.select_runtime_mode_armed &&
            !workflow.valid_until_utc.empty();
        ImGui::BeginDisabled(!ready);
        if (ImGui::Button("Accept And Select Daily Registration")) {
            std::string error;
            if (!AcceptAndSelect(&workflow, &error)) workflow.error = error;
        }
        ImGui::EndDisabled();
    } else if (workflow.stage == "complete") {
        ImGui::TextWrapped(
            "Accepted daily geometry remains visible for review below. "
            "Orange: accepted top-rim fit. Green: exact selected experimental "
            "area after Citrus's integer canvas translation; the canonical "
            "radius is unchanged.");
        RenderDailyReviewImages(*ui_state, workflow, "geometry");
    }

    if (workflow.active) {
        ImGui::Separator();
        const bool capture_or_save_active =
            group_capture_workflow_active(*ui_state) ||
            top_rim_observation_save_worker_is_busy() ||
            generic_calibration_image_set_save_worker_is_busy();
        ImGui::BeginDisabled(capture_or_save_active ||
                             workflow.stage == "waiting_abort" ||
                             workflow.stage == "waiting_reject");
        if (ImGui::Button("Abort Daily Registration Safely")) {
            AbortWorkflow(&workflow, "operator_aborted");
        }
        ImGui::EndDisabled();
        if (capture_or_save_active) {
            ImGui::TextDisabled(
                "Abort becomes available after the active grouped capture/save reaches a terminal state.");
        }
    }
}

}  // namespace orange::gui::spatial_layout
