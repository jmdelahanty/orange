#include "gui/arena_centering_autorun.h"

#include "fsuid_guard.h"
#include "gui/spatial_layout/calibration_workflow.h"
#include "gui/spatial_layout/citrus_import.h"
#include "gui/spatial_layout/citrus_template_workflow.h"
#include "gui/spatial_layout/group_capture_controller.h"
#include "gui/spatial_layout/layout_state.h"
#include "gui/spatial_layout/preflight.h"
#include "gui/spatial_layout/projection_snapshot_client.h"
#include "gui/spatial_layout/save_job_preparation.h"
#include "gui/spatial_layout/save_jobs.h"
#include "gui/spatial_layout/session_io.h"
#include "project.h"

#include <opencv2/imgproc.hpp>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <set>
#include <sstream>

namespace orange::gui {
namespace {

using arena_centering::ApplyJacobianCorrection;
using arena_centering::ConcurrentDetectionBatch;
using arena_centering::ConcurrentRectangleDetectionBatch;
using arena_centering::DetectArenaCenterFiducialsConcurrently;
using arena_centering::DetectArenaRectangleBoundariesConcurrently;
using arena_centering::MaximalSquareProposalConfig;
using arena_centering::Point2d;
using arena_centering::ProposeMaximalVisibleArenaSquare;
using arena_centering::RectangleBoundaryDetectorConfig;
using arena_centering::ProbeObservation;
using arena_centering::RgbaFrameView;
using arena_centering::SolveSymmetricArenaCentering;
using arena_centering::SolverConfig;
using arena_centering::SymmetricProbeObservations;
using orange::gui::spatial_layout::CalibrationCaptureTiming;
using orange::gui::spatial_layout::abort_citrus_arena_centering;
using orange::gui::spatial_layout::apply_calibration_image_set_purpose_defaults;
using orange::gui::spatial_layout::apply_calibration_workflow_profile_defaults;
using orange::gui::spatial_layout::begin_citrus_arena_centering;
using orange::gui::spatial_layout::camera_has_exposed_mapped_nir_strobe;
using orange::gui::spatial_layout::commit_citrus_arena_centering;
using orange::gui::spatial_layout::generic_calibration_image_set_save_worker_is_busy;
using orange::gui::spatial_layout::fit_citrus_homography_candidates;
using orange::gui::spatial_layout::import_citrus_canvas_templates;
using orange::gui::spatial_layout::initialize_spatial_layout_defaults;
using orange::gui::spatial_layout::prepare_calibration_capture_preflight_camera_serials;
using orange::gui::spatial_layout::query_citrus_arena_centering_status;
using orange::gui::spatial_layout::query_citrus_homography_candidate_status;
using orange::gui::spatial_layout::queue_group_calibration_image_set_save_jobs;
using orange::gui::spatial_layout::queued_generic_calibration_image_set_save_job_count;
using orange::gui::spatial_layout::request_group_full_resolution_snapshots_for_arena_centering;
using orange::gui::spatial_layout::request_group_full_resolution_snapshots;
using orange::gui::spatial_layout::promote_citrus_homography_candidates;
using orange::gui::spatial_layout::reject_citrus_homography_candidates;
using orange::gui::spatial_layout::restore_calibration_capture_preflight_all_cameras;
using orange::gui::spatial_layout::set_calibration_preflight_result;
using orange::gui::spatial_layout::stage_citrus_arena_centers;

constexpr const char* kBaseline = "baseline";
constexpr const char* kPlusX = "probe_plus_x";
constexpr const char* kMinusX = "probe_minus_x";
constexpr const char* kPlusY = "probe_plus_y";
constexpr const char* kMinusY = "probe_minus_y";
constexpr const char* kCandidate = "candidate";
constexpr const char* kRefinedCandidate = "refined_candidate";
constexpr const char* kResizedCandidate = "resized_candidate";

int find_camera_index(const CameraParams* cameras_params,
                      int num_cameras,
                      const std::string& serial)
{
    if (cameras_params == nullptr) {
        return -1;
    }
    for (int index = 0; index < num_cameras; ++index) {
        if (cameras_params[index].camera_serial == serial) {
            return index;
        }
    }
    return -1;
}

double elapsed_seconds(const ArenaCenteringAutorunState& state)
{
    if (state.stage_started_at.time_since_epoch().count() == 0) {
        return 0.0;
    }
    return std::chrono::duration<double>(
        std::chrono::steady_clock::now() - state.stage_started_at).count();
}

void enter_stage(ArenaCenteringAutorunState* state,
                 ArenaCenteringAutorunStage stage)
{
    state->stage = stage;
    state->stage_started_at = std::chrono::steady_clock::now();
    state->action_requested = false;
    std::cout << "[GUI][arena_centering] stage="
              << arena_centering_autorun_stage_name(stage);
    if (!state->current_stage_id.empty()) {
        std::cout << " capture_stage=" << state->current_stage_id;
    }
    std::cout << std::endl;
}

std::string make_identity(const std::string& prefix)
{
    std::string value = prefix + "_" + get_current_utc_timestamp();
    for (char& ch : value) {
        if (!((ch >= 'a' && ch <= 'z') ||
              (ch >= 'A' && ch <= 'Z') ||
              (ch >= '0' && ch <= '9') || ch == '_' || ch == '-')) {
            ch = '_';
        }
    }
    return value;
}

nlohmann::json capture_json(const SpatialLayoutGroupCaptureFrame& capture)
{
    return {
        {"camera_serial", capture.camera_serial},
        {"width", capture.width},
        {"height", capture.height},
        {"capture_mode", capture.capture_mode},
        {"source_frame_count", capture.source_frame_count},
        {"first_local_frame_id", capture.first_local_frame_id},
        {"last_local_frame_id", capture.last_local_frame_id},
        {"first_camera_frame_id", capture.first_camera_frame_id},
        {"last_camera_frame_id", capture.last_camera_frame_id},
        {"camera_timestamp_ns", capture.camera_timestamp_ns},
        {"timestamp_sys_ns", capture.timestamp_sys_ns},
    };
}

nlohmann::json ptp_alignment_json(const SpatialLayoutUiState& spatial_state)
{
    std::uint64_t minimum = std::numeric_limits<std::uint64_t>::max();
    std::uint64_t maximum = 0;
    std::size_t count = 0;
    for (const auto& capture : spatial_state.group_captures) {
        if (!capture.valid || capture.camera_timestamp_ns == 0) {
            continue;
        }
        minimum = std::min(minimum, capture.camera_timestamp_ns);
        maximum = std::max(maximum, capture.camera_timestamp_ns);
        ++count;
    }
    return {
        {"clock_domain", "camera_ptp_timestamp_ns"},
        {"camera_count", count},
        {"minimum_timestamp_ns", count == 0 ? 0 : minimum},
        {"maximum_timestamp_ns", count == 0 ? 0 : maximum},
        {"span_ns", count == 0 ? 0 : maximum - minimum},
    };
}

bool stage_requires_rectangle_analysis(const std::string& stage_id)
{
    return stage_id == kBaseline || stage_id == kCandidate ||
           stage_id == kRefinedCandidate || stage_id == kResizedCandidate;
}

bool rectangle_has_two_sided_edge_support(
    const arena_centering::RectangleBoundaryDetection& rectangle,
    std::string* weak_edge_out = nullptr)
{
    const std::pair<const char*, int> supports[] = {
        {"top", rectangle.top_edge_candidate_count},
        {"right", rectangle.right_edge_candidate_count},
        {"bottom", rectangle.bottom_edge_candidate_count},
        {"left", rectangle.left_edge_candidate_count},
    };
    for (const auto& [edge, count] : supports) {
        if (count < 2) {
            if (weak_edge_out != nullptr) *weak_edge_out = edge;
            return false;
        }
    }
    return true;
}

void record_stability_capture_rejection(
    ArenaCenteringAutorunState* state,
    const SpatialLayoutUiState& spatial_state,
    const std::string& reason)
{
    if (state == nullptr) return;
    if (!state->stability_capture_rejections.is_array()) {
        state->stability_capture_rejections = nlohmann::json::array();
    }
    state->stability_capture_rejections.push_back({
        {"stage_id", state->current_stage_id},
        {"attempt", state->stability_capture_attempts},
        {"capture_group_id", spatial_state.group_capture_id},
        {"reason", reason},
        {"action", "recapture_same_presented_scene"},
    });
}

void clear_stability_reference(ArenaCenteringAutorunState* state)
{
    if (state == nullptr) return;
    state->stability_reference_ready = false;
    state->stability_reference_stage_id.clear();
    state->stability_reference_capture_group_id.clear();
    state->stability_reference_capture = nlohmann::json::object();
    state->stability_capture_rejections = nlohmann::json::array();
    state->stability_capture_attempts = 0;
    state->stability_reference_detections.clear();
    state->stability_reference_rectangles.clear();
}

bool prepare_projection_stability_reference(
    ArenaCenteringAutorunState* state,
    const ArenaCenteringAutorunConfig& config,
    const SpatialLayoutUiState& spatial_state,
    std::string* error_out)
{
    const nlohmann::json ptp = ptp_alignment_json(spatial_state);
    const std::size_t camera_count = ptp.value("camera_count", std::size_t{0});
    const std::uint64_t span_ns = ptp.value("span_ns", std::uint64_t{0});
    if (camera_count != state->targets.size() ||
        span_ns > config.maximum_ptp_capture_span_ns) {
        if (error_out != nullptr) {
            *error_out =
                "projection stability reference failed PTP alignment gate";
        }
        return false;
    }

    std::vector<RgbaFrameView> views;
    views.reserve(spatial_state.group_captures.size());
    nlohmann::json captures = nlohmann::json::array();
    for (const auto& capture : spatial_state.group_captures) {
        views.push_back({capture.camera_serial,
                         capture.width,
                         capture.height,
                         &capture.rgba});
        captures.push_back(capture_json(capture));
    }
    const ConcurrentDetectionBatch centers =
        DetectArenaCenterFiducialsConcurrently(views);
    std::map<std::string, arena_centering::FiducialDetection> by_camera;
    for (auto detection : centers.detections) {
        if (!detection.ok) {
            if (error_out != nullptr) {
                *error_out =
                    "projection stability reference center detection failed for camera " +
                    detection.camera_serial + ": " + detection.error;
            }
            return false;
        }
        detection.overlay_rgba.clear();
        by_camera[detection.camera_serial] = std::move(detection);
    }
    if (by_camera.size() != state->targets.size() ||
        centers.task_count != state->targets.size()) {
        if (error_out != nullptr) {
            *error_out =
                "projection stability reference did not analyze every camera";
        }
        return false;
    }

    nlohmann::json rectangles_json = nullptr;
    std::map<std::string, arena_centering::RectangleBoundaryDetection>
        rectangles_by_camera;
    if (stage_requires_rectangle_analysis(state->current_stage_id)) {
        RectangleBoundaryDetectorConfig rectangle_config;
        rectangle_config.minimum_visible_margin_camera_px =
            config.rectangle_safety_margin_camera_px;
        const ConcurrentRectangleDetectionBatch rectangles =
            DetectArenaRectangleBoundariesConcurrently(views, rectangle_config);
        rectangles_json = rectangles.ToJson();
        for (auto detection : rectangles.detections) {
            if (!detection.ok) {
                if (error_out != nullptr) {
                    *error_out =
                        "projection stability reference rectangle detection failed for camera " +
                        detection.camera_serial + ": " + detection.error;
                }
                return false;
            }
            std::string weak_edge;
            const bool weak_support = !rectangle_has_two_sided_edge_support(
                detection, &weak_edge);
            const auto center = by_camera.find(detection.camera_serial);
            const double center_delta = center == by_camera.end()
                ? std::numeric_limits<double>::infinity()
                : std::hypot(
                    center->second.center_camera_px.x -
                        detection.diagonal_intersection_camera_px.x,
                    center->second.center_camera_px.y -
                        detection.diagonal_intersection_camera_px.y);
            if (center == by_camera.end() || !center->second.ok ||
                center_delta >
                    config.rectangle_center_marker_tolerance_camera_px) {
                if (error_out != nullptr) {
                    *error_out = weak_support
                        ? "projection stability reference rectangle has one-sided " +
                            weak_edge + " edge support and fails center agreement for camera " +
                            detection.camera_serial
                        : "projection stability reference rectangle fails center agreement for camera " +
                            detection.camera_serial;
                }
                return false;
            }
            detection.overlay_rgba.clear();
            rectangles_by_camera[detection.camera_serial] = std::move(detection);
        }
        if (rectangles_by_camera.size() != state->targets.size() ||
            rectangles.task_count != state->targets.size()) {
            if (error_out != nullptr) {
                *error_out =
                    "projection stability reference did not analyze every rectangle";
            }
            return false;
        }
    }

    state->stability_reference_ready = true;
    state->stability_reference_stage_id = state->current_stage_id;
    state->stability_reference_capture_group_id = spatial_state.group_capture_id;
    state->stability_reference_detections = std::move(by_camera);
    state->stability_reference_rectangles = std::move(rectangles_by_camera);
    state->stability_reference_capture = {
        {"capture_group_id", spatial_state.group_capture_id},
        {"captures", std::move(captures)},
        {"ptp_alignment", ptp},
        {"center_detections", centers.ToJson()},
        {"rectangle_detections", std::move(rectangles_json)},
        {"citrus_stage_consistency",
         spatial_state.group_capture_metadata.citrus_arena_centering_consistency},
    };
    return true;
}

double point_delta(const Point2d& a, const Point2d& b)
{
    return std::hypot(a.x - b.x, a.y - b.y);
}

nlohmann::json compare_projection_stability(
    const ArenaCenteringAutorunState& state,
    const ArenaCenteringAutorunConfig& config,
    const SpatialLayoutUiState& spatial_state,
    std::string* error_out)
{
    nlohmann::json result = {
        {"schema_id", "orange.arena_projection_stability_gate"},
        {"schema_version", 2},
        {"status", "failed"},
        {"policy", "two_independent_post_fence_grouped_captures_uncertainty_aware_v2"},
        {"stage_id", state.current_stage_id},
        {"reference_capture_group_id", state.stability_reference_capture_group_id},
        {"confirmation_capture_group_id", spatial_state.group_capture_id},
        {"inter_capture_wait_milliseconds",
         config.projection_stability_interval_milliseconds},
        {"capture_attempts", state.stability_capture_attempts},
        {"maximum_capture_attempts",
         config.projection_stability_max_capture_attempts},
        {"rejected_captures", state.stability_capture_rejections},
        {"maximum_center_delta_camera_px",
         config.projection_stability_max_center_delta_camera_px},
        {"maximum_corner_delta_camera_px",
         config.projection_stability_max_corner_delta_camera_px},
        {"reference", state.stability_reference_capture},
        {"cameras", nlohmann::json::array()},
    };
    std::string failure;
    if (!state.stability_reference_ready ||
        state.stability_reference_stage_id != state.current_stage_id) {
        failure = "projection stability reference is missing or belongs to another stage";
    }

    const nlohmann::json reference_scene =
        state.stability_reference_capture.value(
            "citrus_stage_consistency", nlohmann::json::object());
    const nlohmann::json confirmation_scene =
        spatial_state.group_capture_metadata.citrus_arena_centering_consistency;
    if (failure.empty() &&
        (reference_scene.value("status", std::string()) != "same_stage" ||
         confirmation_scene.value("status", std::string()) != "same_stage" ||
         reference_scene.value("scene_revision", std::uint64_t{0}) == 0 ||
         reference_scene.value("scene_revision", std::uint64_t{0}) !=
             confirmation_scene.value("scene_revision", std::uint64_t{0}) ||
         reference_scene.value("content_fingerprint", std::string()).empty() ||
         reference_scene.value("content_fingerprint", std::string()) !=
             confirmation_scene.value("content_fingerprint", std::string()))) {
        failure =
            "Citrus scene identity changed between stability captures";
    }

    for (const auto& target : state.targets) {
        nlohmann::json camera = {{"camera_serial", target.camera_serial}};
        const auto reference =
            state.stability_reference_detections.find(target.camera_serial);
        const auto confirmation_stage =
            state.detections_by_stage_and_camera.find(state.current_stage_id);
        const auto confirmation =
            confirmation_stage == state.detections_by_stage_and_camera.end()
                ? std::map<std::string,
                           arena_centering::FiducialDetection>::const_iterator{}
                : confirmation_stage->second.find(target.camera_serial);
        if (reference == state.stability_reference_detections.end() ||
            confirmation_stage == state.detections_by_stage_and_camera.end() ||
            confirmation == confirmation_stage->second.end() ||
            !reference->second.ok || !confirmation->second.ok) {
            camera["status"] = "missing_center_detection";
            if (failure.empty()) {
                failure = "projection stability center evidence is incomplete for camera " +
                          target.camera_serial;
            }
            result["cameras"].push_back(std::move(camera));
            continue;
        }
        const double center_delta = point_delta(
            reference->second.center_camera_px,
            confirmation->second.center_camera_px);
        camera["center_delta_camera_px"] = center_delta;
        double maximum_corner_delta = 0.0;
        if (stage_requires_rectangle_analysis(state.current_stage_id)) {
            const auto reference_rectangle =
                state.stability_reference_rectangles.find(target.camera_serial);
            const auto confirmation_rectangles =
                state.rectangles_by_stage_and_camera.find(state.current_stage_id);
            if (reference_rectangle == state.stability_reference_rectangles.end() ||
                confirmation_rectangles == state.rectangles_by_stage_and_camera.end()) {
                if (failure.empty()) {
                    failure =
                        "projection stability rectangle evidence is incomplete for camera " +
                        target.camera_serial;
                }
            } else {
                const auto confirmation_rectangle =
                    confirmation_rectangles->second.find(target.camera_serial);
                if (confirmation_rectangle == confirmation_rectangles->second.end() ||
                    !reference_rectangle->second.ok ||
                    !confirmation_rectangle->second.ok) {
                    if (failure.empty()) {
                        failure =
                            "projection stability rectangle evidence is invalid for camera " +
                            target.camera_serial;
                    }
                } else {
                    maximum_corner_delta = std::max({
                        point_delta(reference_rectangle->second.top_left_camera_px,
                                    confirmation_rectangle->second.top_left_camera_px),
                        point_delta(reference_rectangle->second.top_right_camera_px,
                                    confirmation_rectangle->second.top_right_camera_px),
                        point_delta(reference_rectangle->second.bottom_right_camera_px,
                                    confirmation_rectangle->second.bottom_right_camera_px),
                        point_delta(reference_rectangle->second.bottom_left_camera_px,
                                    confirmation_rectangle->second.bottom_left_camera_px),
                    });
                    camera["maximum_corner_delta_camera_px"] =
                        maximum_corner_delta;
                    // The projected rectangle is intentionally a broad
                    // luminous band. Hough may observe one band edge in one
                    // frame and both edges in the other, so the centerline's
                    // honest measurement uncertainty is as large as half the
                    // observed band. Translation remains independently
                    // constrained by the much tighter center-fiducial gate.
                    const double maximum_edge_band_spread = std::max({
                        reference_rectangle->second.top_edge_band_spread_camera_px,
                        reference_rectangle->second.right_edge_band_spread_camera_px,
                        reference_rectangle->second.bottom_edge_band_spread_camera_px,
                        reference_rectangle->second.left_edge_band_spread_camera_px,
                        confirmation_rectangle->second.top_edge_band_spread_camera_px,
                        confirmation_rectangle->second.right_edge_band_spread_camera_px,
                        confirmation_rectangle->second.bottom_edge_band_spread_camera_px,
                        confirmation_rectangle->second.left_edge_band_spread_camera_px,
                    });
                    const double corner_measurement_uncertainty =
                        0.5 * maximum_edge_band_spread;
                    camera["corner_measurement_uncertainty_camera_px"] =
                        corner_measurement_uncertainty;
                    camera["base_corner_repeatability_allowance_camera_px"] =
                        config.projection_stability_max_corner_delta_camera_px;
                    camera["allowed_corner_delta_camera_px"] =
                        config.projection_stability_max_corner_delta_camera_px +
                        corner_measurement_uncertainty;
                }
            }
        }
        const double allowed_corner_delta = camera.value(
            "allowed_corner_delta_camera_px",
            config.projection_stability_max_corner_delta_camera_px);
        const bool camera_stable =
            center_delta <=
                config.projection_stability_max_center_delta_camera_px &&
            (!stage_requires_rectangle_analysis(state.current_stage_id) ||
             maximum_corner_delta <= allowed_corner_delta);
        camera["status"] = camera_stable ? "stable" : "unstable";
        if (!camera_stable && failure.empty()) {
            failure = "projected calibration geometry moved between captures for camera " +
                      target.camera_serial;
        }
        result["cameras"].push_back(std::move(camera));
    }
    result["status"] = failure.empty() ? "passed" : "failed";
    result["error"] = failure.empty()
        ? nlohmann::json(nullptr)
        : nlohmann::json(failure);
    if (!failure.empty() && error_out != nullptr) *error_out = failure;
    return result;
}

const CitrusSpatialTemplateState* template_for_camera(
    const SpatialLayoutUiState& spatial_state,
    const std::string& camera_serial)
{
    const CitrusSpatialTemplateState* match = nullptr;
    for (const auto& candidate : spatial_state.citrus_canvas_templates) {
        if (candidate.source_camera_id != camera_serial) {
            continue;
        }
        if (match != nullptr) {
            return nullptr;
        }
        match = &candidate;
    }
    return match;
}

nlohmann::json citrus_targets_json(
    const std::vector<ArenaCenteringTargetState>& targets)
{
    nlohmann::json out = nlohmann::json::array();
    for (const auto& target : targets) {
        out.push_back({
            {"arena_id", target.arena_id},
            {"camera_id", target.camera_serial},
        });
    }
    return out;
}

nlohmann::json staged_centers_json(
    const ArenaCenteringAutorunState& state,
    const std::string& stage_id,
    int probe_canvas_px)
{
    int dx = 0;
    int dy = 0;
    if (stage_id == kPlusX) dx = probe_canvas_px;
    if (stage_id == kMinusX) dx = -probe_canvas_px;
    if (stage_id == kPlusY) dy = probe_canvas_px;
    if (stage_id == kMinusY) dy = -probe_canvas_px;
    nlohmann::json centers = nlohmann::json::array();
    for (const auto& target : state.targets) {
        int x = target.original_center_x_canvas_px + dx;
        int y = target.original_center_y_canvas_px + dy;
        if (stage_id == kCandidate || stage_id == kRefinedCandidate ||
            stage_id == kResizedCandidate) {
            x = target.candidate_center_x_canvas_px;
            y = target.candidate_center_y_canvas_px;
        }
        nlohmann::json placement = {
            {"arena_id", target.arena_id},
            {"camera_id", target.camera_serial},
            {"center_x_canvas_px", x},
            {"center_y_canvas_px", y},
        };
        if (stage_id == kResizedCandidate) {
            placement["arena_width_canvas_px"] = target.candidate_width_canvas_px;
            placement["arena_height_canvas_px"] = target.candidate_height_canvas_px;
        }
        centers.push_back(std::move(placement));
    }
    return centers;
}

Point2d canvas_center_for_stage(const ArenaCenteringTargetState& target,
                                const std::string& stage_id,
                                int probe_canvas_px)
{
    if (stage_id == kPlusX) {
        return {static_cast<double>(target.original_center_x_canvas_px + probe_canvas_px),
                static_cast<double>(target.original_center_y_canvas_px)};
    }
    if (stage_id == kMinusX) {
        return {static_cast<double>(target.original_center_x_canvas_px - probe_canvas_px),
                static_cast<double>(target.original_center_y_canvas_px)};
    }
    if (stage_id == kPlusY) {
        return {static_cast<double>(target.original_center_x_canvas_px),
                static_cast<double>(target.original_center_y_canvas_px + probe_canvas_px)};
    }
    if (stage_id == kMinusY) {
        return {static_cast<double>(target.original_center_x_canvas_px),
                static_cast<double>(target.original_center_y_canvas_px - probe_canvas_px)};
    }
    if (stage_id == kCandidate || stage_id == kRefinedCandidate ||
        stage_id == kResizedCandidate) {
        return {static_cast<double>(target.candidate_center_x_canvas_px),
                static_cast<double>(target.candidate_center_y_canvas_px)};
    }
    return {static_cast<double>(target.original_center_x_canvas_px),
            static_cast<double>(target.original_center_y_canvas_px)};
}

void prepare_stage_metadata(SpatialLayoutUiState* spatial_state,
                            const std::string& transaction_id,
                            const std::string& stage_id)
{
    apply_calibration_image_set_purpose_defaults(spatial_state, "arena_projection");
    spatial_state->calibration_workflow_profile_id =
        "unobstructed_canvas_commissioning";
    spatial_state->calibration_fixture_state = "holder_removed";
    spatial_state->calibration_homography_role = "commissioning_reference";
    spatial_state->calibration_visibility_domain_id =
        "unobstructed_arena_rectangle";
    spatial_state->calibration_image_set_projected_pattern_id =
        "citrus_arena_outline_center_fiducial_" + stage_id;
    spatial_state->calibration_image_set_projected_pattern_type =
        "arena_outline_with_center_fiducial";
    spatial_state->calibration_pattern_type = "other";
    spatial_state->calibration_pattern_domain = "full_projected_surface";
    spatial_state->calibration_projector_state = "calibration_pattern";
    spatial_state->calibration_projector_visible_to_camera = true;
    spatial_state->calibration_projected_pattern_used_as_coordinate_target = true;
    spatial_state->calibration_reference_only = true;
    spatial_state->calibration_operator_notes =
        "Automated canonical arena-centering commissioning transaction " +
        transaction_id + ", stage " + stage_id +
        ". Dry shelf, holder removed, dish absent, filters removed.";
    spatial_state->calibration_image_set_notes =
        spatial_state->calibration_operator_notes;
}

void prepare_homography_metadata(SpatialLayoutUiState* spatial_state,
                                 const std::string& transaction_id,
                                 std::uint8_t foreground_gray_u8)
{
    apply_calibration_image_set_purpose_defaults(spatial_state, "homography_grid");
    spatial_state->calibration_workflow_profile_id =
        "unobstructed_canvas_commissioning";
    spatial_state->calibration_fixture_state = "holder_removed";
    spatial_state->calibration_homography_role = "commissioning_reference";
    spatial_state->calibration_visibility_domain_id =
        "unobstructed_arena_rectangle";
    spatial_state->calibration_capture_stage =
        "projected_surface_dry_reference";
    spatial_state->calibration_image_set_projected_pattern_id =
        "citrus_rectangular_homography_grid";
    spatial_state->calibration_image_set_projected_pattern_type =
        "rectangular_grid";
    spatial_state->calibration_pattern_type = "rectangular_grid";
    spatial_state->calibration_pattern_domain = "full_projected_surface";
    spatial_state->calibration_projector_state = "calibration_pattern";
    spatial_state->calibration_projector_visible_to_camera = true;
    spatial_state->calibration_projected_pattern_used_as_coordinate_target = true;
    spatial_state->calibration_reference_only = false;
    spatial_state->group_capture_scene_recipe = "homography_grid";
    spatial_state->group_capture_scene_options = {
        {"foreground_gray_u8", foreground_gray_u8},
    };
    spatial_state->calibration_operator_notes =
        "Automated dry-shelf rectangular homography candidate capture " +
        transaction_id +
        ". Arena placement was committed and verified first; holder removed, "
        "dish absent, filters removed.";
    spatial_state->calibration_image_set_notes =
        spatial_state->calibration_operator_notes;
}

nlohmann::json homography_targets_json(
    const std::vector<ArenaCenteringTargetState>& targets)
{
    nlohmann::json out = nlohmann::json::array();
    for (const auto& target : targets) {
        out.push_back({
            {"arena_id", target.arena_id},
            {"camera_id", target.camera_serial},
            {"orange_artifact_id",
             "Cam" + orange::gui::spatial_layout::sanitize_artifact_component(
                         target.camera_serial) +
                 "_" + orange::gui::spatial_layout::sanitize_artifact_component(
                             target.arena_id)},
        });
    }
    return out;
}

nlohmann::json homography_quality_thresholds_json(
    const ArenaCenteringAutorunConfig& config)
{
    return {
        {"maximum_rms_reprojection_error_canvas_px",
         config.homography_maximum_rms_reprojection_error_canvas_px},
        {"maximum_point_reprojection_error_canvas_px",
         config.homography_maximum_point_reprojection_error_canvas_px},
        {"minimum_inlier_ratio", config.homography_minimum_inlier_ratio},
        {"maximum_holdout_rms_error_canvas_px",
         config.homography_maximum_holdout_rms_error_canvas_px},
        {"maximum_holdout_error_canvas_px",
         config.homography_maximum_holdout_error_canvas_px},
        {"saturation_pixel_threshold_u8",
         config.homography_saturation_pixel_threshold_u8},
        {"maximum_dot_core_saturation_fraction",
         config.homography_maximum_dot_core_saturation_fraction},
        {"minimum_dot_background_contrast_u8",
         config.homography_minimum_dot_background_contrast_u8},
        {"projector_intensity_report_path",
         config.projector_intensity_report_path},
        {"projector_intensity_report_sha256",
         config.projector_intensity_report_sha256},
        {"commissioned_foreground_gray_u8", config.foreground_gray_u8},
    };
}

void schedule_abort(ArenaCenteringAutorunState* state,
                    const std::string& error)
{
    if (!error.empty() && state->error_message.empty()) {
        state->error_message = error;
    }
    state->run_passed = false;
    if (state->transaction_started && !state->transaction_terminal) {
        state->terminal_intent = "abort";
        enter_stage(state, ArenaCenteringAutorunStage::kCommitOrAbort);
    } else {
        enter_stage(state, ArenaCenteringAutorunStage::kRestorePreflight);
    }
}

bool stage_centers(ArenaCenteringAutorunState* state,
                   const ArenaCenteringAutorunConfig& config,
                   const std::string& stage_id,
                   std::string* error_out)
{
    clear_stability_reference(state);
    state->current_stage_id = stage_id;
    state->current_operation_id =
        state->transaction_id + "_stage_" + stage_id;
    const auto result = stage_citrus_arena_centers(
        state->transaction_id,
        stage_id,
        staged_centers_json(*state, stage_id, config.symmetric_probe_canvas_px),
        state->current_operation_id);
    if (!result.ok) {
        if (error_out) {
            *error_out = "Citrus rejected stage " + stage_id + ": " + result.reason;
        }
        return false;
    }
    return true;
}

bool solve_all_targets(ArenaCenteringAutorunState* state,
                       const ArenaCenteringAutorunConfig& config,
                       std::string* error_out)
{
    state->solve_results = nlohmann::json::object();
    state->solves_by_camera.clear();
    for (auto& target : state->targets) {
        auto detection = [&](const std::string& stage_id)
            -> const arena_centering::FiducialDetection* {
            const auto stage = state->detections_by_stage_and_camera.find(stage_id);
            if (stage == state->detections_by_stage_and_camera.end()) return nullptr;
            const auto camera = stage->second.find(target.camera_serial);
            return camera == stage->second.end() ? nullptr : &camera->second;
        };
        const auto* baseline = detection(kBaseline);
        const auto* plus_x = detection(kPlusX);
        const auto* minus_x = detection(kMinusX);
        const auto* plus_y = detection(kPlusY);
        const auto* minus_y = detection(kMinusY);
        if (baseline == nullptr || plus_x == nullptr || minus_x == nullptr ||
            plus_y == nullptr || minus_y == nullptr) {
            if (error_out) {
                *error_out = "missing symmetric probe detection for camera " +
                    target.camera_serial;
            }
            return false;
        }
        SymmetricProbeObservations observations;
        observations.baseline = {
            baseline->center_camera_px,
            canvas_center_for_stage(target, kBaseline,
                                    config.symmetric_probe_canvas_px)};
        observations.plus_x = {
            plus_x->center_camera_px,
            canvas_center_for_stage(target, kPlusX,
                                    config.symmetric_probe_canvas_px)};
        observations.minus_x = {
            minus_x->center_camera_px,
            canvas_center_for_stage(target, kMinusX,
                                    config.symmetric_probe_canvas_px)};
        observations.plus_y = {
            plus_y->center_camera_px,
            canvas_center_for_stage(target, kPlusY,
                                    config.symmetric_probe_canvas_px)};
        observations.minus_y = {
            minus_y->center_camera_px,
            canvas_center_for_stage(target, kMinusY,
                                    config.symmetric_probe_canvas_px)};
        SolverConfig solver_config;
        // A reflected camera/projector mounting is valid. The symmetric probes
        // establish the sign empirically; invertibility and conditioning are
        // the safety gates.
        solver_config.require_positive_determinant = false;
        const auto solved = SolveSymmetricArenaCentering(
            observations,
            baseline->sensor_raster_center_camera_px,
            solver_config);
        state->solve_results[target.camera_serial] = solved.ToJson();
        state->solves_by_camera[target.camera_serial] = solved;
        if (!solved.ok) {
            if (error_out) {
                *error_out = "arena-centering solve failed for camera " +
                    target.camera_serial + ": " + solved.error;
            }
            return false;
        }
        target.candidate_center_x_canvas_px = solved.candidate_center_x_canvas_px;
        target.candidate_center_y_canvas_px = solved.candidate_center_y_canvas_px;
    }
    return true;
}

bool propose_all_arena_resizes(ArenaCenteringAutorunState* state,
                               const ArenaCenteringAutorunConfig& config,
                               std::string* error_out)
{
    const auto rectangles = state->rectangles_by_stage_and_camera.find(
        state->current_stage_id);
    if (rectangles == state->rectangles_by_stage_and_camera.end()) {
        if (error_out) *error_out = "candidate rectangle evidence is missing";
        return false;
    }
    state->resize_proposals = nlohmann::json::object();
    state->resize_proposals_by_camera.clear();
    for (auto& target : state->targets) {
        const auto rectangle = rectangles->second.find(target.camera_serial);
        if (rectangle == rectangles->second.end()) {
            if (error_out) {
                *error_out = "candidate rectangle missing for camera " +
                    target.camera_serial;
            }
            return false;
        }
        MaximalSquareProposalConfig proposal_config;
        proposal_config.safety_margin_camera_px =
            config.rectangle_safety_margin_camera_px +
            config.rectangle_prediction_reserve_camera_px;
        proposal_config.maximum_scale_change_fraction =
            config.maximum_arena_scale_change_fraction;
        const auto proposal = ProposeMaximalVisibleArenaSquare(
            rectangle->second,
            rectangle->second.sensor_width_px,
            rectangle->second.sensor_height_px,
            state->canvas_width_px,
            state->canvas_height_px,
            target.candidate_center_x_canvas_px,
            target.candidate_center_y_canvas_px,
            target.original_width_canvas_px,
            target.original_height_canvas_px,
            proposal_config);
        state->resize_proposals[target.camera_serial] = proposal.ToJson();
        state->resize_proposals_by_camera[target.camera_serial] = proposal;
        if (!proposal.ok) {
            if (error_out) {
                *error_out = "arena resize proposal failed for camera " +
                    target.camera_serial + ": " + proposal.error;
            }
            return false;
        }
        target.candidate_width_canvas_px = proposal.proposed_width_canvas_px;
        target.candidate_height_canvas_px = proposal.proposed_height_canvas_px;
    }
    return true;
}

nlohmann::json build_verification(
    const ArenaCenteringAutorunState& state,
    const ArenaCenteringAutorunConfig& config,
    bool* passed_out)
{
    bool passed = true;
    nlohmann::json cameras = nlohmann::json::array();
    const auto stage = state.detections_by_stage_and_camera.find(
        state.current_stage_id);
    for (const auto& target : state.targets) {
        const auto detection = stage == state.detections_by_stage_and_camera.end()
            ? std::map<std::string, arena_centering::FiducialDetection>::const_iterator{}
            : stage->second.find(target.camera_serial);
        if (stage == state.detections_by_stage_and_camera.end() ||
            detection == stage->second.end()) {
            passed = false;
            cameras.push_back({
                {"camera_serial", target.camera_serial},
                {"status", "missing_detection"},
            });
            continue;
        }
        const double error_x = detection->second.sensor_raster_center_camera_px.x -
                               detection->second.center_camera_px.x;
        const double error_y = detection->second.sensor_raster_center_camera_px.y -
                               detection->second.center_camera_px.y;
        const double raw_norm = std::hypot(error_x, error_y);
        const auto solved = state.solves_by_camera.find(target.camera_serial);
        if (solved == state.solves_by_camera.end()) {
            passed = false;
            cameras.push_back({
                {"camera_serial", target.camera_serial},
                {"arena_id", target.arena_id},
                {"status", "missing_solve"},
            });
            continue;
        }
        const double quantized_dx =
            static_cast<double>(target.candidate_center_x_canvas_px) -
            solved->second.candidate_canvas_px.x;
        const double quantized_dy =
            static_cast<double>(target.candidate_center_y_canvas_px) -
            solved->second.candidate_canvas_px.y;
        const auto& jacobian = solved->second.jacobian_camera_px_per_canvas_px;
        const double predicted_quantization_error_x =
            -(jacobian[0][0] * quantized_dx + jacobian[0][1] * quantized_dy);
        const double predicted_quantization_error_y =
            -(jacobian[1][0] * quantized_dx + jacobian[1][1] * quantized_dy);
        const double excess_error_x = error_x - predicted_quantization_error_x;
        const double excess_error_y = error_y - predicted_quantization_error_y;
        const double excess_norm = std::hypot(excess_error_x, excess_error_y);
        bool camera_passed =
            excess_norm <= config.verification_tolerance_camera_px;
        nlohmann::json rectangle_verification = nullptr;
        if (config.resize_arenas && state.current_stage_id == kResizedCandidate) {
            const auto rectangle_stage =
                state.rectangles_by_stage_and_camera.find(kResizedCandidate);
            const auto rectangle = rectangle_stage ==
                    state.rectangles_by_stage_and_camera.end()
                ? std::map<std::string,
                           arena_centering::RectangleBoundaryDetection>::const_iterator{}
                : rectangle_stage->second.find(target.camera_serial);
            const auto proposal =
                state.resize_proposals_by_camera.find(target.camera_serial);
            if (rectangle_stage == state.rectangles_by_stage_and_camera.end() ||
                rectangle == rectangle_stage->second.end() ||
                proposal == state.resize_proposals_by_camera.end()) {
                camera_passed = false;
                rectangle_verification = {
                    {"status", "missing_rectangle_or_proposal"}};
            } else {
                const auto& measured = rectangle->second;
                const auto& predicted = proposal->second.predicted_corners_camera_px;
                const std::vector<Point2d> measured_corners = {
                    measured.top_left_camera_px,
                    measured.top_right_camera_px,
                    measured.bottom_right_camera_px,
                    measured.bottom_left_camera_px};
                double maximum_corner_error = 0.0;
                if (predicted.size() != measured_corners.size()) {
                    maximum_corner_error =
                        std::numeric_limits<double>::infinity();
                } else {
                    for (std::size_t index = 0; index < predicted.size(); ++index) {
                        maximum_corner_error = std::max(
                            maximum_corner_error,
                            std::hypot(
                                predicted[index].x - measured_corners[index].x,
                                predicted[index].y - measured_corners[index].y));
                    }
                }
                const double center_marker_delta = std::hypot(
                    detection->second.center_camera_px.x -
                        measured.diagonal_intersection_camera_px.x,
                    detection->second.center_camera_px.y -
                        measured.diagonal_intersection_camera_px.y);
                const bool square_even =
                    target.candidate_width_canvas_px ==
                        target.candidate_height_canvas_px &&
                    target.candidate_width_canvas_px % 2 == 0;
                const bool close_to_maximal =
                    measured.minimum_margin_camera_px <=
                    config.rectangle_safety_margin_camera_px +
                        config.rectangle_maximality_slack_camera_px;
                const bool rectangle_passed =
                    measured.ok && measured.fully_visible_with_margin &&
                    square_even && close_to_maximal &&
                    center_marker_delta <=
                        config.rectangle_center_marker_tolerance_camera_px &&
                    maximum_corner_error <=
                        config.rectangle_prediction_tolerance_camera_px;
                camera_passed = camera_passed && rectangle_passed;
                rectangle_verification = {
                    {"status", rectangle_passed ? "passed" : "failed"},
                    {"detected_boundary", measured.ToJson()},
                    {"proposal", proposal->second.ToJson()},
                    {"candidate_size_canvas_px", {
                        {"width", target.candidate_width_canvas_px},
                        {"height", target.candidate_height_canvas_px}}},
                    {"square_even", square_even},
                    {"center_fiducial_to_diagonal_intersection_camera_px",
                     center_marker_delta},
                    {"maximum_predicted_to_measured_corner_error_camera_px",
                     maximum_corner_error},
                    {"close_to_maximal", close_to_maximal},
                    {"maximality_slack_camera_px",
                     config.rectangle_maximality_slack_camera_px},
                };
            }
        }
        passed = passed && camera_passed;
        cameras.push_back({
            {"camera_serial", target.camera_serial},
            {"arena_id", target.arena_id},
            {"status", camera_passed ? "passed" : "failed"},
            {"target_camera_px", {
                {"x", detection->second.sensor_raster_center_camera_px.x},
                {"y", detection->second.sensor_raster_center_camera_px.y}}},
            {"detected_center_camera_px", {
                {"x", detection->second.center_camera_px.x},
                {"y", detection->second.center_camera_px.y}}},
            {"residual_camera_px", {{"x", error_x}, {"y", error_y}}},
            {"residual_norm_camera_px", raw_norm},
            {"predicted_integer_quantization_residual_camera_px", {
                {"x", predicted_quantization_error_x},
                {"y", predicted_quantization_error_y}}},
            {"predicted_integer_quantization_residual_norm_camera_px",
             std::hypot(
                 predicted_quantization_error_x,
                 predicted_quantization_error_y)},
            {"excess_residual_after_quantization_camera_px", {
                {"x", excess_error_x}, {"y", excess_error_y}}},
            {"excess_residual_after_quantization_norm_camera_px", excess_norm},
            {"candidate_center_canvas_px", {
                {"x", target.candidate_center_x_canvas_px},
                {"y", target.candidate_center_y_canvas_px}}},
            {"original_size_canvas_px", {
                {"width", target.original_width_canvas_px},
                {"height", target.original_height_canvas_px}}},
            {"candidate_size_canvas_px", {
                {"width", target.candidate_width_canvas_px},
                {"height", target.candidate_height_canvas_px}}},
            {"rectangle_verification", std::move(rectangle_verification)},
        });
    }
    if (passed_out) *passed_out = passed;
    return {
        {"schema_id", "orange.arena_centering.verification"},
        {"schema_version", 1},
        {"status", passed ? "passed" : "failed"},
        {"stage_id", state.current_stage_id},
        {"tolerance_camera_px", config.verification_tolerance_camera_px},
        {"tolerance_applies_to",
         "excess_residual_after_integer_projector_quantization"},
        {"renderable_center_policy",
         "nearest_integer_canvas_position_under_measured_jacobian"},
        {"all_cameras_required", true},
        {"arena_resize_enabled", config.resize_arenas},
        {"experimental_area_local_center_rebased_for_size", config.resize_arenas},
        {"experimental_area_shape_or_size_changed", false},
        {"experimental_area_offset_from_arena_center_preserved", true},
        {"experimental_area_coordinate_space", "arena_relative"},
        {"cameras", std::move(cameras)},
    };
}

bool apply_one_refinement(ArenaCenteringAutorunState* state,
                          const ArenaCenteringAutorunConfig& config,
                          std::string* error_out)
{
    const auto stage = state->detections_by_stage_and_camera.find(kCandidate);
    if (stage == state->detections_by_stage_and_camera.end()) {
        if (error_out) *error_out = "candidate detections are missing";
        return false;
    }
    for (auto& target : state->targets) {
        const auto detection = stage->second.find(target.camera_serial);
        const auto solved = state->solves_by_camera.find(target.camera_serial);
        if (detection == stage->second.end() || solved == state->solves_by_camera.end()) {
            if (error_out) *error_out = "candidate refinement evidence is incomplete";
            return false;
        }
        const Point2d camera_error{
            detection->second.sensor_raster_center_camera_px.x -
                detection->second.center_camera_px.x,
            detection->second.sensor_raster_center_camera_px.y -
                detection->second.center_camera_px.y};
        const Point2d correction = ApplyJacobianCorrection(solved->second, camera_error);
        if (!std::isfinite(correction.x) || !std::isfinite(correction.y) ||
            std::hypot(correction.x, correction.y) >
                config.maximum_refinement_canvas_px) {
            if (error_out) {
                *error_out = "candidate residual requires an unsafe refinement for camera " +
                    target.camera_serial;
            }
            return false;
        }
        target.candidate_center_x_canvas_px +=
            static_cast<int>(std::lround(correction.x));
        target.candidate_center_y_canvas_px +=
            static_cast<int>(std::lround(correction.y));
    }
    state->refinement_attempted = true;
    return true;
}

bool write_analysis_artifacts(ArenaCenteringAutorunState* state,
                              SpatialLayoutUiState* spatial_state,
                              std::string* error_out)
{
    if (spatial_state->calibration_session_dir.empty()) {
        if (error_out) *error_out = "calibration session directory is empty";
        return false;
    }
    const std::filesystem::path root =
        std::filesystem::path(spatial_state->calibration_session_dir) /
        "analysis" / "arena_centering" /
        orange::gui::spatial_layout::sanitize_artifact_component(
            state->transaction_id) /
        orange::gui::spatial_layout::sanitize_artifact_component(
            state->current_stage_id);
    nlohmann::json artifacts = {
        {"root", root.generic_string()},
        {"detection_json", (root / "detections.json").generic_string()},
        {"overlays", nlohmann::json::array()},
        {"rectangle_overlays", nlohmann::json::array()},
    };
    const auto stage = state->detections_by_stage_and_camera.find(
        state->current_stage_id);
    if (stage == state->detections_by_stage_and_camera.end()) {
        if (error_out) *error_out = "pending detection stage is missing";
        return false;
    }
    for (const auto& [camera_serial, detection] : stage->second) {
        if (detection.overlay_rgba.empty()) {
            if (error_out) *error_out = "detection overlay is empty for " + camera_serial;
            return false;
        }
        const auto capture = std::find_if(
            spatial_state->group_captures.begin(),
            spatial_state->group_captures.end(),
            [&](const SpatialLayoutGroupCaptureFrame& item) {
                return item.camera_serial == camera_serial;
            });
        if (capture == spatial_state->group_captures.end()) {
            if (error_out) *error_out = "capture dimensions missing for " + camera_serial;
            return false;
        }
        cv::Mat rgba(capture->height,
                     capture->width,
                     CV_8UC4,
                     const_cast<unsigned char*>(detection.overlay_rgba.data()));
        cv::Mat bgra;
        cv::cvtColor(rgba, bgra, cv::COLOR_RGBA2BGRA);
        const std::filesystem::path path =
            root / ("Cam" + camera_serial + "_center_overlay.png");
        if (!orange::gui::spatial_layout::write_image_file(path, bgra, error_out)) {
            return false;
        }
        artifacts["overlays"].push_back({
            {"camera_serial", camera_serial},
            {"path", path.generic_string()},
        });
    }
    const auto rectangles = state->rectangles_by_stage_and_camera.find(
        state->current_stage_id);
    if (rectangles != state->rectangles_by_stage_and_camera.end()) {
        for (const auto& [camera_serial, detection] : rectangles->second) {
            if (detection.overlay_rgba.empty()) {
                if (error_out) {
                    *error_out = "rectangle overlay is empty for " + camera_serial;
                }
                return false;
            }
            const auto capture = std::find_if(
                spatial_state->group_captures.begin(),
                spatial_state->group_captures.end(),
                [&](const SpatialLayoutGroupCaptureFrame& item) {
                    return item.camera_serial == camera_serial;
                });
            if (capture == spatial_state->group_captures.end()) {
                if (error_out) {
                    *error_out = "rectangle capture dimensions missing for " +
                        camera_serial;
                }
                return false;
            }
            cv::Mat rgba(
                capture->height,
                capture->width,
                CV_8UC4,
                const_cast<unsigned char*>(detection.overlay_rgba.data()));
            cv::Mat bgra;
            cv::cvtColor(rgba, bgra, cv::COLOR_RGBA2BGRA);
            const std::filesystem::path path =
                root / ("Cam" + camera_serial + "_rectangle_overlay.png");
            if (!orange::gui::spatial_layout::write_image_file(
                    path, bgra, error_out)) {
                return false;
            }
            artifacts["rectangle_overlays"].push_back({
                {"camera_serial", camera_serial},
                {"path", path.generic_string()},
            });
        }
    }
    if (!orange::gui::spatial_layout::write_json_file(
            root / "detections.json", state->pending_detection_batch, error_out)) {
        return false;
    }
    if (!state->stage_records.empty()) {
        state->stage_records.back()["analysis_artifacts"] = std::move(artifacts);
    }
    return true;
}

bool write_json_atomically(const std::string& path,
                           const nlohmann::json& payload,
                           std::string* error_out)
{
    orange::ScopedFsuid guard;
    (void)guard;
    const std::filesystem::path destination(path);
    std::error_code ec;
    std::filesystem::create_directories(destination.parent_path(), ec);
    if (ec) {
        if (error_out) *error_out = "could not create result directory: " + ec.message();
        return false;
    }
    const std::filesystem::path temporary = destination.string() + ".tmp";
    {
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        if (!output) {
            if (error_out) *error_out = "could not open result temporary file";
            return false;
        }
        output << payload.dump(2) << '\n';
        output.flush();
        if (!output) {
            if (error_out) *error_out = "could not write result temporary file";
            return false;
        }
    }
    std::filesystem::rename(temporary, destination, ec);
    if (ec) {
        std::filesystem::remove(destination, ec);
        ec.clear();
        std::filesystem::rename(temporary, destination, ec);
    }
    if (ec) {
        if (error_out) *error_out = "could not publish result: " + ec.message();
        return false;
    }
    return true;
}

}  // namespace

const char* arena_centering_autorun_stage_name(ArenaCenteringAutorunStage stage)
{
    switch (stage) {
        case ArenaCenteringAutorunStage::kDisabled: return "disabled";
        case ArenaCenteringAutorunStage::kWaitForStream: return "wait_for_stream";
        case ArenaCenteringAutorunStage::kPrepare: return "prepare";
        case ArenaCenteringAutorunStage::kWaitForPreflightSettle: return "wait_preflight_settle";
        case ArenaCenteringAutorunStage::kBeginTransaction: return "begin_transaction";
        case ArenaCenteringAutorunStage::kWaitForBeginPresented: return "wait_begin_presented";
        case ArenaCenteringAutorunStage::kWaitForProjectionSettle: return "wait_projection_settle";
        case ArenaCenteringAutorunStage::kRequestCapture: return "request_capture";
        case ArenaCenteringAutorunStage::kWaitForCapture: return "wait_capture";
        case ArenaCenteringAutorunStage::kWaitForStabilityConfirmation: return "wait_stability_confirmation";
        case ArenaCenteringAutorunStage::kAnalyzeCapture: return "analyze_capture";
        case ArenaCenteringAutorunStage::kQueueSave: return "queue_save";
        case ArenaCenteringAutorunStage::kWaitForSave: return "wait_save";
        case ArenaCenteringAutorunStage::kAdvance: return "advance";
        case ArenaCenteringAutorunStage::kCommitOrAbort: return "commit_or_abort";
        case ArenaCenteringAutorunStage::kWaitForTerminalReceipt: return "wait_terminal_receipt";
        case ArenaCenteringAutorunStage::kRequestHomographyCapture: return "request_homography_capture";
        case ArenaCenteringAutorunStage::kWaitForHomographyCapture: return "wait_homography_capture";
        case ArenaCenteringAutorunStage::kQueueHomographySave: return "queue_homography_save";
        case ArenaCenteringAutorunStage::kWaitForHomographySave: return "wait_homography_save";
        case ArenaCenteringAutorunStage::kRequestHomographyFit: return "request_homography_fit";
        case ArenaCenteringAutorunStage::kWaitForHomographyFit: return "wait_homography_fit";
        case ArenaCenteringAutorunStage::kPromoteHomography: return "promote_homography";
        case ArenaCenteringAutorunStage::kWaitForHomographyPromotion: return "wait_homography_promotion";
        case ArenaCenteringAutorunStage::kRestorePreflight: return "restore_preflight";
        case ArenaCenteringAutorunStage::kStopStream: return "stop_stream";
        case ArenaCenteringAutorunStage::kWaitForStreamStop: return "wait_stream_stop";
        case ArenaCenteringAutorunStage::kWriteResult: return "write_result";
        case ArenaCenteringAutorunStage::kDone: return "done";
        case ArenaCenteringAutorunStage::kFailed: return "failed";
    }
    return "unknown";
}

void arena_centering_autorun_start(ArenaCenteringAutorunState* state,
                                   const ArenaCenteringAutorunConfig& config)
{
    if (state == nullptr) return;
    *state = ArenaCenteringAutorunState{};
    if (!config.enabled) return;
    state->run_started_at = std::chrono::steady_clock::now();
    state->stage_started_at = state->run_started_at;
    state->stage = ArenaCenteringAutorunStage::kWaitForStream;
}

ArenaCenteringAutorunRequests arena_centering_autorun_update(
    ArenaCenteringAutorunState* state,
    const ArenaCenteringAutorunConfig& config,
    SpatialLayoutUiState* spatial_state,
    CameraControl* camera_control,
    CameraEmergent* ecams,
    CameraParams* cameras_params,
    CameraEachSelect* cameras_select,
    int num_cameras,
    SpatialSnapshotWorker* const* spatial_snapshot_workers,
    const std::string& calibration_sessions_root)
{
    ArenaCenteringAutorunRequests requests;
    if (state == nullptr || spatial_state == nullptr || camera_control == nullptr ||
        !config.enabled || state->stage == ArenaCenteringAutorunStage::kDisabled ||
        state->stage == ArenaCenteringAutorunStage::kDone ||
        state->stage == ArenaCenteringAutorunStage::kFailed) {
        return requests;
    }

    switch (state->stage) {
    case ArenaCenteringAutorunStage::kWaitForStream:
        if (camera_control->subscribe && ecams != nullptr && cameras_params != nullptr &&
            cameras_select != nullptr && spatial_snapshot_workers != nullptr &&
            num_cameras > 0) {
            enter_stage(state, ArenaCenteringAutorunStage::kPrepare);
        } else if (elapsed_seconds(*state) >= config.startup_timeout_seconds) {
            schedule_abort(state, "timed out waiting for Orange streaming cameras");
        }
        break;

    case ArenaCenteringAutorunStage::kPrepare: {
        if (config.citrus_config_path.empty() ||
            !std::filesystem::is_regular_file(config.citrus_config_path)) {
            schedule_abort(state, "Citrus canvas config is missing: " +
                                      config.citrus_config_path);
            break;
        }
        std::vector<std::string> cameras = config.camera_serials;
        if (cameras.empty()) {
            for (int i = 0; i < num_cameras; ++i) {
                if (!cameras_params[i].camera_serial.empty()) {
                    cameras.push_back(cameras_params[i].camera_serial);
                }
            }
        }
        if (cameras.empty()) {
            schedule_abort(state, "arena centering has no camera scope");
            break;
        }
        for (const std::string& serial : cameras) {
            if (find_camera_index(cameras_params, num_cameras, serial) < 0) {
                schedule_abort(state, "requested camera is not open: " + serial);
                break;
            }
        }
        if (state->stage != ArenaCenteringAutorunStage::kPrepare) break;

        initialize_spatial_layout_defaults(spatial_state);
        spatial_state->show_window = true;
        const int selected = find_camera_index(cameras_params, num_cameras, cameras.front());
        spatial_state->selected_camera = selected;
        spatial_state->configured_camera_index = selected;
        std::string import_status;
        std::string import_error;
        if (!import_citrus_canvas_templates(
                spatial_state,
                cameras_params[selected],
                config.citrus_config_path,
                &import_status,
                &import_error)) {
            schedule_abort(state, "Citrus canvas import failed: " + import_error);
            break;
        }
        std::string profile_error;
        if (!apply_calibration_workflow_profile_defaults(
                spatial_state,
                "unobstructed_canvas_commissioning",
                &profile_error)) {
            schedule_abort(state, profile_error);
            break;
        }
        state->targets.clear();
        std::set<std::string> arena_ids;
        for (const std::string& serial : cameras) {
            const auto* template_state = template_for_camera(*spatial_state, serial);
            if (template_state == nullptr ||
                !template_state->has_arena_canvas_region ||
                template_state->source_arena_name.empty() ||
                !arena_ids.insert(template_state->source_arena_name).second) {
                schedule_abort(
                    state,
                    "Citrus import must provide one unique arena template for camera " +
                        serial);
                break;
            }
            state->targets.push_back({
                serial,
                template_state->source_arena_name,
                static_cast<int>(std::lround(template_state->arena_center_x_px)),
                static_cast<int>(std::lround(template_state->arena_center_y_px)),
                static_cast<int>(std::lround(template_state->arena_center_x_px)),
                static_cast<int>(std::lround(template_state->arena_center_y_px)),
                static_cast<int>(std::lround(template_state->arena_width_px)),
                static_cast<int>(std::lround(template_state->arena_height_px)),
                static_cast<int>(std::lround(template_state->arena_width_px)),
                static_cast<int>(std::lround(template_state->arena_height_px)),
            });
        }
        if (state->stage != ArenaCenteringAutorunStage::kPrepare) break;
        spatial_state->group_capture_selected_camera_serials = cameras;
        spatial_state->group_capture_camera_scope_initialized = true;
        state->transaction_id = make_identity("arena_centering");
        if (config.apply_calibration_preflight) {
            int light_index = -1;
            for (int i = 0; i < num_cameras; ++i) {
                if (camera_has_exposed_mapped_nir_strobe(cameras_params[i])) {
                    light_index = i;
                    break;
                }
            }
            const bool mapped_strobe = light_index >= 0;
            const CalibrationCaptureTiming timing{
                config.calibration_frame_rate_hz,
                config.calibration_exposure_us};
            const bool ok = prepare_calibration_capture_preflight_camera_serials(
                spatial_state,
                ecams,
                cameras_params,
                num_cameras,
                cameras,
                mapped_strobe ? &ecams[light_index] : nullptr,
                mapped_strobe ? &cameras_params[light_index] : nullptr,
                mapped_strobe,
                camera_control->record_video || camera_control->recording_draining,
                "suppress_mapped_strobe",
                &state->preflight_prepare_status,
                timing);
            set_calibration_preflight_result(
                spatial_state, ok, state->preflight_prepare_status);
            if (!ok) {
                schedule_abort(state, "calibration preflight failed: " +
                                          state->preflight_prepare_status);
                break;
            }
            state->preflight_applied = true;
            for (const auto& target : state->targets) {
                const int index = find_camera_index(
                    cameras_params, num_cameras, target.camera_serial);
                state->capture_camera_settings.push_back({
                    {"camera_serial", target.camera_serial},
                    {"frame_rate_hz", cameras_params[index].frame_rate},
                    {"exposure_us", cameras_params[index].exposure},
                    {"sync_mode", cameras_params[index].sync_mode},
                    {"ptp_mode", cameras_params[index].ptp_mode},
                });
            }
            enter_stage(state, ArenaCenteringAutorunStage::kWaitForPreflightSettle);
        } else {
            enter_stage(state, ArenaCenteringAutorunStage::kBeginTransaction);
        }
        break;
    }

    case ArenaCenteringAutorunStage::kWaitForPreflightSettle:
        if (elapsed_seconds(*state) * 1000.0 >=
            config.preflight_settle_milliseconds) {
            enter_stage(state, ArenaCenteringAutorunStage::kBeginTransaction);
        }
        break;

    case ArenaCenteringAutorunStage::kBeginTransaction: {
        state->current_stage_id = kBaseline;
        state->current_operation_id = state->transaction_id + "_begin_baseline";
        const auto result = begin_citrus_arena_centering(
            state->transaction_id,
            config.citrus_config_path,
            citrus_targets_json(state->targets),
            state->current_operation_id,
            config.foreground_gray_u8);
        if (!result.ok) {
            schedule_abort(state, "Citrus arena-centering begin failed: " + result.reason);
            break;
        }
        state->transaction_started = true;
        // The local-control reply acknowledges that the command was queued.
        // Its embedded status was sampled before the GUI thread applied the
        // command, so it must not be treated as the active transaction state.
        enter_stage(state, ArenaCenteringAutorunStage::kWaitForBeginPresented);
        break;
    }

    case ArenaCenteringAutorunStage::kWaitForBeginPresented: {
        const auto result = query_citrus_arena_centering_status(
            state->transaction_id, "begin_presented");
        if (result.ok &&
            result.centering.value("transaction_id", std::string()) ==
                state->transaction_id &&
            result.centering.value("active", false) &&
            result.centering.value("presented", false) &&
            result.centering.value("state", std::string()) == "presented") {
            const nlohmann::json canvas_size = result.centering.value(
                "canvas_size_px", nlohmann::json::object());
            state->canvas_width_px = canvas_size.value("width", 0);
            state->canvas_height_px = canvas_size.value("height", 0);
            state->base_canvas_checksum = result.centering.value(
                "base_canvas_checksum", std::string());
            if (state->canvas_width_px <= 0 || state->canvas_height_px <= 0) {
                schedule_abort(
                    state,
                    "applied Citrus centering status omitted canvas dimensions");
                break;
            }
            if (state->base_canvas_checksum.rfind("sha256:", 0) != 0) {
                schedule_abort(
                    state,
                    "applied Citrus centering status omitted base canvas checksum");
                break;
            }
            enter_stage(state, ArenaCenteringAutorunStage::kWaitForProjectionSettle);
            break;
        }
        if (elapsed_seconds(*state) >= config.workflow_timeout_seconds) {
            schedule_abort(
                state,
                "timed out waiting for Citrus begin transaction presentation fence");
        }
        break;
    }

    case ArenaCenteringAutorunStage::kWaitForProjectionSettle:
        // A Citrus presentation fence proves that the new pixels have reached
        // the display. It cannot retroactively exclude an Orange camera
        // exposure that began before that fence. Drain at least the configured
        // 100 ms exposure and one 5 fps frame interval before arming a fresh
        // grouped snapshot request. The default 500 ms includes margin for
        // projector/display and GUI scheduling jitter.
        if (elapsed_seconds(*state) * 1000.0 >=
            config.projection_settle_milliseconds) {
            enter_stage(state, ArenaCenteringAutorunStage::kRequestCapture);
        }
        break;

    case ArenaCenteringAutorunStage::kRequestCapture: {
        prepare_stage_metadata(
            spatial_state, state->transaction_id, state->current_stage_id);
        std::string error;
        if (!request_group_full_resolution_snapshots_for_arena_centering(
                spatial_state,
                cameras_params,
                cameras_select,
                num_cameras,
                spatial_snapshot_workers,
                config.frame_count,
                state->transaction_id,
                state->current_stage_id,
                state->current_operation_id,
                &error)) {
            schedule_abort(state, "arena-centering grouped capture request failed: " + error);
            break;
        }
        enter_stage(state, ArenaCenteringAutorunStage::kWaitForCapture);
        break;
    }

    case ArenaCenteringAutorunStage::kWaitForCapture:
        if (spatial_state->group_capture_workflow_state == "complete") {
            if (config.require_projection_stability_capture) {
                ++state->stability_capture_attempts;
            }
            const std::string membership =
                spatial_state->group_capture_metadata.capture_group_membership.value(
                    "status", std::string());
            const std::string scene_consistency =
                spatial_state->group_capture_metadata.citrus_calibration_scene_consistency.value(
                    "status", std::string());
            const std::string centering_consistency =
                spatial_state->group_capture_metadata.citrus_arena_centering_consistency.value(
                    "status", std::string());
            if (membership != "complete" || scene_consistency != "same_scene" ||
                centering_consistency != "same_stage") {
                schedule_abort(
                    state,
                    "capture group failed identity gates (membership=" + membership +
                        ", scene=" + scene_consistency +
                        ", centering=" + centering_consistency + ")");
            } else if (config.require_projection_stability_capture &&
                       !state->stability_reference_ready) {
                std::string reference_error;
                if (!prepare_projection_stability_reference(
                        state, config, *spatial_state, &reference_error)) {
                    std::cout
                        << "[GUI][arena_centering] projection stability reference rejected: "
                        << reference_error << std::endl;
                    if (state->stability_capture_attempts <
                        config.projection_stability_max_capture_attempts) {
                        record_stability_capture_rejection(
                            state, *spatial_state, reference_error);
                        enter_stage(
                            state,
                            ArenaCenteringAutorunStage::kWaitForStabilityConfirmation);
                    } else {
                        // The final failed attempt is analyzed and persisted
                        // before the normal abort path.
                        enter_stage(
                            state, ArenaCenteringAutorunStage::kAnalyzeCapture);
                    }
                } else {
                    enter_stage(
                        state,
                        ArenaCenteringAutorunStage::kWaitForStabilityConfirmation);
                }
            } else {
                enter_stage(state, ArenaCenteringAutorunStage::kAnalyzeCapture);
            }
        } else if (spatial_state->group_capture_workflow_state == "failed") {
            schedule_abort(
                state,
                spatial_state->group_capture_error.empty()
                    ? "arena-centering grouped capture failed"
                    : spatial_state->group_capture_error);
        } else if (elapsed_seconds(*state) >= config.workflow_timeout_seconds) {
            schedule_abort(state, "timed out waiting for arena-centering capture");
        }
        break;

    case ArenaCenteringAutorunStage::kWaitForStabilityConfirmation:
        if (elapsed_seconds(*state) * 1000.0 >=
            config.projection_stability_interval_milliseconds) {
            enter_stage(state, ArenaCenteringAutorunStage::kRequestCapture);
        }
        break;

    case ArenaCenteringAutorunStage::kAnalyzeCapture: {
        const nlohmann::json ptp = ptp_alignment_json(*spatial_state);
        const std::size_t camera_count = ptp.value("camera_count", std::size_t{0});
        const std::uint64_t span_ns = ptp.value("span_ns", std::uint64_t{0});
        std::string analysis_failure;
        const auto remember_analysis_failure = [&](const std::string& error) {
            if (analysis_failure.empty()) analysis_failure = error;
        };
        if (camera_count != state->targets.size() ||
            span_ns > config.maximum_ptp_capture_span_ns) {
            remember_analysis_failure(
                "grouped capture failed PTP alignment gate (count=" +
                    std::to_string(camera_count) + ", span_ns=" +
                    std::to_string(span_ns) + ")");
        }
        std::vector<RgbaFrameView> views;
        views.reserve(spatial_state->group_captures.size());
        for (const auto& capture : spatial_state->group_captures) {
            views.push_back({capture.camera_serial,
                             capture.width,
                             capture.height,
                             &capture.rgba});
        }
        const ConcurrentDetectionBatch batch =
            DetectArenaCenterFiducialsConcurrently(views);
        std::map<std::string, arena_centering::FiducialDetection> detections;
        for (const auto& detection : batch.detections) {
            detections[detection.camera_serial] = detection;
            if (!detection.ok) {
                remember_analysis_failure(
                    "center fiducial detection failed for camera " +
                        detection.camera_serial + ": " + detection.error);
            }
        }
        if (detections.size() != state->targets.size() ||
            batch.task_count != state->targets.size()) {
            remember_analysis_failure(
                "camera analyses did not create and join exactly one task per camera");
        }
        state->detections_by_stage_and_camera[state->current_stage_id] =
            std::move(detections);
        state->pending_detection_batch = batch.ToJson();
        const bool analyze_rectangle =
            stage_requires_rectangle_analysis(state->current_stage_id);
        if (analyze_rectangle) {
            RectangleBoundaryDetectorConfig rectangle_config;
            rectangle_config.minimum_visible_margin_camera_px =
                config.rectangle_safety_margin_camera_px;
            const ConcurrentRectangleDetectionBatch rectangles =
                DetectArenaRectangleBoundariesConcurrently(views, rectangle_config);
            std::map<std::string, arena_centering::RectangleBoundaryDetection>
                by_camera;
            std::string weak_rectangle_support_error;
            for (const auto& rectangle : rectangles.detections) {
                by_camera[rectangle.camera_serial] = rectangle;
                if (!rectangle.ok) {
                    remember_analysis_failure(
                        "arena rectangle detection failed for camera " +
                            rectangle.camera_serial + ": " + rectangle.error);
                    continue;
                }
                std::string weak_edge;
                const bool weak_support = !rectangle_has_two_sided_edge_support(
                    rectangle, &weak_edge);
                const auto center = state->detections_by_stage_and_camera
                    .at(state->current_stage_id).find(rectangle.camera_serial);
                if (center == state->detections_by_stage_and_camera
                                  .at(state->current_stage_id).end() ||
                    !center->second.ok) {
                    remember_analysis_failure(
                        "rectangle/center analysis identity mismatch for camera " +
                            rectangle.camera_serial);
                    continue;
                }
                const double center_delta = std::hypot(
                    center->second.center_camera_px.x -
                        rectangle.diagonal_intersection_camera_px.x,
                    center->second.center_camera_px.y -
                        rectangle.diagonal_intersection_camera_px.y);
                if (center_delta >
                    config.rectangle_center_marker_tolerance_camera_px) {
                    const std::string center_error =
                        "rectangle diagonal center disagrees with center fiducial for camera " +
                        rectangle.camera_serial + " (delta=" +
                        std::to_string(center_delta) + " camera px)";
                    if (weak_support && weak_rectangle_support_error.empty()) {
                        weak_rectangle_support_error =
                            "arena rectangle has one-sided " + weak_edge +
                            " edge support and fails center agreement for camera " +
                            rectangle.camera_serial + " (delta=" +
                            std::to_string(center_delta) + " camera px)";
                    } else {
                        remember_analysis_failure(center_error);
                    }
                }
            }
            if (by_camera.size() != state->targets.size() ||
                rectangles.task_count != state->targets.size()) {
                remember_analysis_failure(
                    "rectangle analyses did not create and join exactly one task per camera");
            }
            if (!weak_rectangle_support_error.empty() &&
                config.require_projection_stability_capture &&
                state->stability_capture_attempts <
                    config.projection_stability_max_capture_attempts) {
                record_stability_capture_rejection(
                    state, *spatial_state, weak_rectangle_support_error);
                std::cout
                    << "[GUI][arena_centering] weak rectangle confirmation recaptured: "
                    << weak_rectangle_support_error << std::endl;
                enter_stage(
                    state,
                    ArenaCenteringAutorunStage::kWaitForStabilityConfirmation);
                break;
            }
            if (!weak_rectangle_support_error.empty()) {
                remember_analysis_failure(weak_rectangle_support_error);
            }
            state->rectangles_by_stage_and_camera[state->current_stage_id] =
                std::move(by_camera);
            state->pending_detection_batch["rectangle_boundaries"] =
                rectangles.ToJson();
        }
        if (config.require_projection_stability_capture) {
            if (state->stability_reference_ready) {
                std::string stability_error;
                state->pending_detection_batch["projection_stability"] =
                    compare_projection_stability(
                        *state, config, *spatial_state, &stability_error);
                if (!stability_error.empty()) {
                    remember_analysis_failure(stability_error);
                }
            } else {
                state->pending_detection_batch["projection_stability"] = {
                    {"schema_id", "orange.arena_projection_stability_gate"},
                    {"schema_version", 2},
                    {"status", "failed"},
                    {"policy", "two_independent_post_fence_grouped_captures_uncertainty_aware_v2"},
                    {"error", "reference capture failed the projected-content gate"},
                };
                remember_analysis_failure(
                    "projection stability reference capture failed the projected-content gate");
            }
        } else {
            state->pending_detection_batch["projection_stability"] = {
                {"schema_id", "orange.arena_projection_stability_gate"},
                {"schema_version", 1},
                {"status", "disabled"},
            };
        }
        if (state->base_canvas_checksum.empty()) {
            state->base_canvas_checksum = spatial_state->group_capture_metadata
                .citrus_arena_centering_pre_capture.value(
                    "base_canvas_checksum", std::string());
        }
        nlohmann::json captures = nlohmann::json::array();
        for (const auto& capture : spatial_state->group_captures) {
            captures.push_back(capture_json(capture));
        }
        const bool persistence_required =
            config.save_captures || !analysis_failure.empty();
        state->pending_abort_after_persistence_reason = analysis_failure;
        state->stage_records.push_back({
            {"stage_id", state->current_stage_id},
            {"operation_id", state->current_operation_id},
            {"capture_group_id", spatial_state->group_capture_id},
            {"captures", std::move(captures)},
            {"ptp_alignment", ptp},
            {"concurrent_analysis", state->pending_detection_batch},
            {"analysis_gate", {
                {"status", analysis_failure.empty() ? "passed" : "failed"},
                {"error", analysis_failure.empty()
                    ? nlohmann::json(nullptr)
                    : nlohmann::json(analysis_failure)},
                {"next_action", analysis_failure.empty()
                    ? "continue_after_persistence"
                    : "abort_after_persistence"},
            }},
            {"persistence", {
                {"requested", persistence_required},
                {"requested_by_config", config.save_captures},
                {"forced_by_analysis_failure", !analysis_failure.empty()},
                {"session_id", spatial_state->calibration_session_id},
                {"session_dir", spatial_state->calibration_session_dir},
            }},
            {"projection_settle_milliseconds",
             config.projection_settle_milliseconds},
            {"projection_stability", {
                {"required", config.require_projection_stability_capture},
                {"inter_capture_wait_milliseconds",
                 config.projection_stability_interval_milliseconds},
                {"maximum_capture_attempts",
                 config.projection_stability_max_capture_attempts},
                {"maximum_center_delta_camera_px",
                 config.projection_stability_max_center_delta_camera_px},
                {"maximum_corner_delta_camera_px",
                 config.projection_stability_max_corner_delta_camera_px},
            }},
        });
        enter_stage(
            state,
            persistence_required
                ? ArenaCenteringAutorunStage::kQueueSave
                : ArenaCenteringAutorunStage::kAdvance);
        break;
    }

    case ArenaCenteringAutorunStage::kQueueSave: {
        std::string status;
        std::string error;
        const int selected = std::clamp(
            spatial_state->selected_camera, 0, std::max(0, num_cameras - 1));
        if (!queue_group_calibration_image_set_save_jobs(
                spatial_state,
                cameras_params,
                num_cameras,
                cameras_params[selected],
                calibration_sessions_root,
                &status,
                &error)) {
            const std::string context =
                state->pending_abort_after_persistence_reason.empty()
                    ? std::string()
                    : " while preserving failed analysis evidence (" +
                          state->pending_abort_after_persistence_reason + ")";
            schedule_abort(
                state,
                "calibration evidence save queue failed" + context + ": " + error);
            break;
        }
        spatial_state->persistence_status = status;
        spatial_state->persistence_error.clear();
        enter_stage(state, ArenaCenteringAutorunStage::kWaitForSave);
        break;
    }

    case ArenaCenteringAutorunStage::kWaitForSave:
        if (!spatial_state->persistence_error.empty()) {
            const std::string context =
                state->pending_abort_after_persistence_reason.empty()
                    ? std::string()
                    : " while preserving failed analysis evidence (" +
                          state->pending_abort_after_persistence_reason + ")";
            schedule_abort(state, "calibration evidence save failed" + context +
                                      ": " + spatial_state->persistence_error);
        } else if (!generic_calibration_image_set_save_worker_is_busy() &&
                   queued_generic_calibration_image_set_save_job_count() == 0) {
            std::string error;
            if (!write_analysis_artifacts(state, spatial_state, &error)) {
                const std::string context =
                    state->pending_abort_after_persistence_reason.empty()
                        ? std::string()
                        : " after analysis gate failure (" +
                              state->pending_abort_after_persistence_reason + ")";
                schedule_abort(
                    state, "analysis artifact save failed" + context + ": " + error);
            } else {
                auto& persistence =
                    state->stage_records.back()["persistence"];
                persistence["requested"] = true;
                persistence["status"] = spatial_state->persistence_status;
                persistence["session_id"] = spatial_state->calibration_session_id;
                persistence["session_dir"] = spatial_state->calibration_session_dir;
                if (!state->pending_abort_after_persistence_reason.empty()) {
                    state->stage_records.back()["analysis_gate"]
                        ["evidence_persisted_before_abort"] = true;
                    schedule_abort(
                        state, state->pending_abort_after_persistence_reason);
                } else {
                    enter_stage(state, ArenaCenteringAutorunStage::kAdvance);
                }
            }
        } else if (elapsed_seconds(*state) >= config.workflow_timeout_seconds) {
            const std::string context =
                state->pending_abort_after_persistence_reason.empty()
                    ? std::string()
                    : " after analysis gate failure (" +
                          state->pending_abort_after_persistence_reason + ")";
            schedule_abort(
                state,
                "timed out waiting for calibration evidence save" + context);
        }
        break;

    case ArenaCenteringAutorunStage::kAdvance: {
        std::string next;
        if (state->current_stage_id == kBaseline) next = kPlusX;
        else if (state->current_stage_id == kPlusX) next = kMinusX;
        else if (state->current_stage_id == kMinusX) next = kPlusY;
        else if (state->current_stage_id == kPlusY) next = kMinusY;
        if (!next.empty()) {
            std::string error;
            if (!stage_centers(state, config, next, &error)) {
                schedule_abort(state, error);
            } else {
                enter_stage(
                    state,
                    ArenaCenteringAutorunStage::kWaitForProjectionSettle);
            }
            break;
        }
        if (state->current_stage_id == kMinusY) {
            std::string error;
            if (!solve_all_targets(state, config, &error) ||
                !stage_centers(state, config, kCandidate, &error)) {
                schedule_abort(state, error);
            } else {
                enter_stage(
                    state,
                    ArenaCenteringAutorunStage::kWaitForProjectionSettle);
            }
            break;
        }
        if (state->current_stage_id == kCandidate ||
            state->current_stage_id == kRefinedCandidate ||
            state->current_stage_id == kResizedCandidate) {
            bool passed = false;
            state->verification = build_verification(*state, config, &passed);
            state->verification_passed = passed;
            if (passed) {
                if (config.resize_arenas &&
                    state->current_stage_id != kResizedCandidate) {
                    std::string error;
                    if (!propose_all_arena_resizes(state, config, &error) ||
                        !stage_centers(state, config, kResizedCandidate, &error)) {
                        schedule_abort(state, error);
                    } else {
                        enter_stage(
                            state,
                            ArenaCenteringAutorunStage::kWaitForProjectionSettle);
                    }
                } else {
                    const bool persistence_armed =
                        config.save_verified_centers_armed &&
                        (!config.resize_arenas ||
                         config.save_verified_layout_armed);
                    state->terminal_intent = persistence_armed
                        ? "commit" : "abort";
                    state->run_passed = true;
                    enter_stage(state, ArenaCenteringAutorunStage::kCommitOrAbort);
                }
            } else if (state->current_stage_id == kCandidate &&
                       !state->refinement_attempted) {
                std::string error;
                if (!apply_one_refinement(state, config, &error) ||
                    !stage_centers(state, config, kRefinedCandidate, &error)) {
                    schedule_abort(state, error);
                } else {
                    enter_stage(
                        state,
                        ArenaCenteringAutorunStage::kWaitForProjectionSettle);
                }
            } else {
                schedule_abort(
                    state,
                    state->current_stage_id == kResizedCandidate
                        ? "resized candidate did not pass the all-camera center/edge gates"
                        : "candidate center did not pass the all-camera residual gate");
            }
        }
        break;
    }

    case ArenaCenteringAutorunStage::kCommitOrAbort:
        if (state->terminal_intent == "commit") {
            const auto result = commit_citrus_arena_centering(
                state->transaction_id,
                state->base_canvas_checksum,
                state->verification,
                config.save_verified_centers_armed,
                config.save_verified_layout_armed,
                state->transaction_id + "_commit_verified_centers");
            state->commit_requested = result.ok;
            if (!result.ok) {
                state->run_passed = false;
                if (state->error_message.empty()) {
                    state->error_message =
                        "Citrus arena-center commit request failed: " + result.reason;
                }
            }
        } else {
            const bool save_arm_missing =
                !config.save_verified_centers_armed ||
                (config.resize_arenas && !config.save_verified_layout_armed);
            const std::string reason = state->verification_passed && save_arm_missing
                ? (config.resize_arenas
                       ? "verification_passed_but_save_verified_layout_not_armed"
                       : "verification_passed_but_save_verified_centers_not_armed")
                : (state->error_message.empty()
                       ? "arena_centering_orchestrator_abort"
                       : state->error_message);
            const auto result = abort_citrus_arena_centering(
                state->transaction_id,
                reason,
                state->transaction_id + "_abort");
            state->abort_requested = result.ok;
            if (!result.ok && state->error_message.empty()) {
                state->error_message =
                    "Citrus arena-centering abort request failed: " + result.reason;
                state->run_passed = false;
            }
        }
        enter_stage(state, ArenaCenteringAutorunStage::kWaitForTerminalReceipt);
        break;

    case ArenaCenteringAutorunStage::kWaitForTerminalReceipt: {
        const auto result = query_citrus_arena_centering_status(
            state->transaction_id, "terminal_receipt");
        if (result.ok) {
            state->terminal_status = result.centering;
            const std::string resolved = result.centering.value("state", std::string());
            const bool active = result.centering.value("active", true);
            const nlohmann::json receipt = result.centering.value(
                "receipt", nlohmann::json::object());
            const std::string outcome = receipt.value("outcome", std::string());
            if (!active && (resolved == "committed" || resolved == "aborted") &&
                !receipt.empty()) {
                state->transaction_terminal = true;
                const bool intended_commit = state->terminal_intent == "commit";
                if ((intended_commit && outcome != "committed") ||
                    (!intended_commit && outcome != "aborted")) {
                    state->run_passed = false;
                    if (state->error_message.empty()) {
                        state->error_message = "Citrus terminal receipt outcome mismatch";
                    }
                }
                if (outcome == "committed") {
                    state->committed_canvas_checksum = receipt.value(
                        "new_canvas_checksum", std::string());
                }
                if (config.fit_homographies_after_centering &&
                    outcome == "committed" && state->run_passed) {
                    if (state->committed_canvas_checksum.rfind("sha256:", 0) != 0) {
                        state->run_passed = false;
                        state->error_message =
                            "Citrus center-commit receipt omitted the new canvas checksum";
                        enter_stage(state, ArenaCenteringAutorunStage::kRestorePreflight);
                    } else {
                        enter_stage(
                            state,
                            ArenaCenteringAutorunStage::kRequestHomographyCapture);
                    }
                } else {
                    if (config.fit_homographies_after_centering &&
                        outcome != "committed" && state->error_message.empty()) {
                        state->run_passed = false;
                        state->error_message =
                            "homography fitting requires a committed centered canvas";
                    }
                    enter_stage(state, ArenaCenteringAutorunStage::kRestorePreflight);
                }
                break;
            }
        }
        if (elapsed_seconds(*state) >= config.workflow_timeout_seconds) {
            state->run_passed = false;
            if (state->error_message.empty()) {
                state->error_message =
                    "timed out waiting for Citrus terminal receipt and scene restore";
            }
            enter_stage(state, ArenaCenteringAutorunStage::kRestorePreflight);
        }
        break;
    }

    case ArenaCenteringAutorunStage::kRequestHomographyCapture: {
        state->homography_transaction_id = make_identity("homography_fit");
        const int selected = std::clamp(
            spatial_state->selected_camera, 0, std::max(0, num_cameras - 1));
        std::string import_status;
        std::string import_error;
        if (!import_citrus_canvas_templates(
                spatial_state,
                cameras_params[selected],
                config.citrus_config_path,
                &import_status,
                &import_error)) {
            state->run_passed = false;
            state->error_message =
                "could not refresh committed Citrus geometry before homography capture: " +
                import_error;
            enter_stage(state, ArenaCenteringAutorunStage::kRestorePreflight);
            break;
        }
        spatial_state->citrus_import_status = import_status;
        spatial_state->citrus_import_error.clear();
        prepare_homography_metadata(
            spatial_state,
            state->homography_transaction_id,
            config.foreground_gray_u8);
        std::string error;
        if (!request_group_full_resolution_snapshots(
                spatial_state,
                cameras_params,
                cameras_select,
                num_cameras,
                spatial_snapshot_workers,
                config.frame_count,
                &error)) {
            state->run_passed = false;
            state->error_message =
                "homography grouped capture request failed: " + error;
            enter_stage(state, ArenaCenteringAutorunStage::kRestorePreflight);
            break;
        }
        state->homography_capture_group_id = spatial_state->group_capture_id;
        enter_stage(state, ArenaCenteringAutorunStage::kWaitForHomographyCapture);
        break;
    }

    case ArenaCenteringAutorunStage::kWaitForHomographyCapture:
        if (spatial_state->group_capture_workflow_state == "complete") {
            const std::string membership =
                spatial_state->group_capture_metadata.capture_group_membership.value(
                    "status", std::string());
            const std::string consistency =
                spatial_state->group_capture_metadata.citrus_calibration_scene_consistency.value(
                    "status", std::string());
            const std::string restore_state =
                spatial_state->group_capture_scene_restore_status.value(
                    "state", std::string());
            if (membership != "complete" || consistency != "same_scene" ||
                restore_state != "restored") {
                state->run_passed = false;
                state->error_message =
                    "homography capture lacked a complete stable group and restored scene";
                enter_stage(state, ArenaCenteringAutorunStage::kRestorePreflight);
            } else {
                enter_stage(state, ArenaCenteringAutorunStage::kQueueHomographySave);
            }
        } else if (spatial_state->group_capture_workflow_state == "failed") {
            state->run_passed = false;
            state->error_message = spatial_state->group_capture_error.empty()
                ? "homography grouped capture failed"
                : spatial_state->group_capture_error;
            enter_stage(state, ArenaCenteringAutorunStage::kRestorePreflight);
        } else if (elapsed_seconds(*state) >= config.workflow_timeout_seconds) {
            state->run_passed = false;
            state->error_message = "timed out waiting for homography grouped capture";
            enter_stage(state, ArenaCenteringAutorunStage::kRestorePreflight);
        }
        break;

    case ArenaCenteringAutorunStage::kQueueHomographySave: {
        std::string status;
        std::string error;
        const int selected = std::clamp(
            spatial_state->selected_camera, 0, std::max(0, num_cameras - 1));
        if (!queue_group_calibration_image_set_save_jobs(
                spatial_state,
                cameras_params,
                num_cameras,
                cameras_params[selected],
                calibration_sessions_root,
                &status,
                &error)) {
            state->run_passed = false;
            state->error_message = "homography evidence save queue failed: " + error;
            enter_stage(state, ArenaCenteringAutorunStage::kRestorePreflight);
            break;
        }
        spatial_state->persistence_status = status;
        spatial_state->persistence_error.clear();
        enter_stage(state, ArenaCenteringAutorunStage::kWaitForHomographySave);
        break;
    }

    case ArenaCenteringAutorunStage::kWaitForHomographySave:
        if (!spatial_state->persistence_error.empty()) {
            state->run_passed = false;
            state->error_message = "homography evidence save failed: " +
                spatial_state->persistence_error;
            enter_stage(state, ArenaCenteringAutorunStage::kRestorePreflight);
        } else if (!generic_calibration_image_set_save_worker_is_busy() &&
                   queued_generic_calibration_image_set_save_job_count() == 0) {
            enter_stage(state, ArenaCenteringAutorunStage::kRequestHomographyFit);
        } else if (elapsed_seconds(*state) >= config.workflow_timeout_seconds) {
            state->run_passed = false;
            state->error_message = "timed out waiting for homography evidence saves";
            enter_stage(state, ArenaCenteringAutorunStage::kRestorePreflight);
        }
        break;

    case ArenaCenteringAutorunStage::kRequestHomographyFit: {
        const auto result = fit_citrus_homography_candidates(
            state->homography_transaction_id,
            config.citrus_config_path,
            state->committed_canvas_checksum,
            spatial_state->calibration_session_dir,
            state->homography_capture_group_id,
            homography_targets_json(state->targets),
            homography_quality_thresholds_json(config),
            state->homography_transaction_id + "_fit");
        state->homography_fit_requested = result.ok;
        if (!result.ok) {
            state->run_passed = false;
            state->error_message =
                "Citrus homography fit request failed: " + result.reason;
            enter_stage(state, ArenaCenteringAutorunStage::kRestorePreflight);
        } else {
            enter_stage(state, ArenaCenteringAutorunStage::kWaitForHomographyFit);
        }
        break;
    }

    case ArenaCenteringAutorunStage::kWaitForHomographyFit: {
        const auto result = query_citrus_homography_candidate_status(
            state->homography_transaction_id, "fit_completion");
        if (result.ok &&
            result.candidate.value("transaction_id", std::string()) ==
                state->homography_transaction_id) {
            state->homography_candidate_status = result.candidate;
            const std::string candidate_state = result.candidate.value(
                "state", std::string());
            if (candidate_state == "ready_for_review") {
                state->homography_verification = {
                    {"schema_id", "orange.homography_candidate.verification"},
                    {"schema_version", 1},
                    {"status", "passed"},
                    {"method", "citrus_disjoint_support_holdout_quality_gates"},
                    {"capture_group_id", state->homography_capture_group_id},
                    {"all_cameras_required", true},
                    {"candidate_set_id", result.candidate.value(
                        "candidate_set_id", std::string())},
                    {"review_artifacts", result.candidate.value(
                        "candidates", nlohmann::json::array())},
                };
                enter_stage(state, ArenaCenteringAutorunStage::kPromoteHomography);
                break;
            }
            if (candidate_state == "fit_failed") {
                state->run_passed = false;
                state->error_message = "Citrus homography candidate quality gate failed";
                const auto rejection = reject_citrus_homography_candidates(
                    state->homography_transaction_id,
                    state->error_message,
                    state->homography_transaction_id + "_reject_failed_fit");
                if (!rejection.ok) {
                    state->error_message += "; candidate rejection failed: " +
                        rejection.reason;
                }
                enter_stage(
                    state,
                    ArenaCenteringAutorunStage::kWaitForHomographyPromotion);
                break;
            }
        }
        if (elapsed_seconds(*state) >= config.workflow_timeout_seconds) {
            state->run_passed = false;
            state->error_message = "timed out waiting for Citrus homography fit";
            enter_stage(state, ArenaCenteringAutorunStage::kRestorePreflight);
        }
        break;
    }

    case ArenaCenteringAutorunStage::kPromoteHomography: {
        if (config.accept_homographies_armed) {
            const auto result = promote_citrus_homography_candidates(
                state->homography_transaction_id,
                state->committed_canvas_checksum,
                state->homography_verification,
                true,
                state->homography_transaction_id + "_promote");
            state->homography_promotion_requested = result.ok;
            if (!result.ok) {
                state->run_passed = false;
                state->error_message =
                    "Citrus homography promotion request failed: " + result.reason;
                enter_stage(state, ArenaCenteringAutorunStage::kRestorePreflight);
                break;
            }
        } else {
            const auto result = reject_citrus_homography_candidates(
                state->homography_transaction_id,
                "quality_passed_but_accept_homographies_not_armed",
                state->homography_transaction_id + "_reject_unarmed");
            if (!result.ok) {
                state->run_passed = false;
                state->error_message =
                    "Citrus homography candidate cleanup failed: " + result.reason;
                enter_stage(state, ArenaCenteringAutorunStage::kRestorePreflight);
                break;
            }
        }
        enter_stage(state, ArenaCenteringAutorunStage::kWaitForHomographyPromotion);
        break;
    }

    case ArenaCenteringAutorunStage::kWaitForHomographyPromotion: {
        const auto result = query_citrus_homography_candidate_status(
            state->homography_transaction_id, "terminal_receipt");
        if (result.ok &&
            result.candidate.value("transaction_id", std::string()) ==
                state->homography_transaction_id) {
            state->homography_candidate_status = result.candidate;
            const std::string candidate_state = result.candidate.value(
                "state", std::string());
            const bool active = result.candidate.value("active", true);
            if (!active && (candidate_state == "committed" ||
                            candidate_state == "rejected")) {
                const std::string outcome = result.candidate.value(
                    "receipt", nlohmann::json::object()).value(
                        "outcome", std::string());
                if (config.accept_homographies_armed) {
                    state->homography_committed =
                        candidate_state == "committed" && outcome == "committed";
                    if (!state->homography_committed) {
                        state->run_passed = false;
                        state->error_message =
                            "Citrus homography promotion receipt did not commit";
                    }
                } else if (candidate_state != "rejected" || outcome != "rejected") {
                    state->run_passed = false;
                    state->error_message =
                        "unarmed homography candidate was not safely rejected";
                }
                enter_stage(state, ArenaCenteringAutorunStage::kRestorePreflight);
                break;
            }
        }
        if (elapsed_seconds(*state) >= config.workflow_timeout_seconds) {
            state->run_passed = false;
            state->error_message =
                "timed out waiting for Citrus homography terminal receipt";
            enter_stage(state, ArenaCenteringAutorunStage::kRestorePreflight);
        }
        break;
    }

    case ArenaCenteringAutorunStage::kRestorePreflight: {
        if (state->preflight_applied && !state->preflight_restore_attempted) {
            state->preflight_restore_attempted = true;
            int light_index = -1;
            for (int i = 0; i < num_cameras; ++i) {
                if (camera_has_exposed_mapped_nir_strobe(cameras_params[i])) {
                    light_index = i;
                    break;
                }
            }
            const bool mapped_strobe = light_index >= 0;
            state->preflight_restore_ok =
                restore_calibration_capture_preflight_all_cameras(
                    spatial_state,
                    ecams,
                    cameras_params,
                    num_cameras,
                    mapped_strobe ? &ecams[light_index] : nullptr,
                    mapped_strobe ? &cameras_params[light_index] : nullptr,
                    mapped_strobe,
                    camera_control->record_video || camera_control->recording_draining,
                    &state->preflight_restore_status);
            set_calibration_preflight_result(
                spatial_state,
                state->preflight_restore_ok,
                state->preflight_restore_status);
            if (!state->preflight_restore_ok) {
                state->run_passed = false;
                if (state->error_message.empty()) {
                    state->error_message = "camera preflight restore failed: " +
                        state->preflight_restore_status;
                }
            }
        }
        enter_stage(state, ArenaCenteringAutorunStage::kStopStream);
        break;
    }

    case ArenaCenteringAutorunStage::kStopStream:
        if (camera_control->subscribe) {
            requests.toggle_streaming = true;
            enter_stage(state, ArenaCenteringAutorunStage::kWaitForStreamStop);
        } else {
            enter_stage(state, ArenaCenteringAutorunStage::kWriteResult);
        }
        break;

    case ArenaCenteringAutorunStage::kWaitForStreamStop:
        if (!camera_control->subscribe || elapsed_seconds(*state) >= 60.0) {
            if (camera_control->subscribe) {
                state->run_passed = false;
                if (state->error_message.empty()) {
                    state->error_message = "timed out stopping Orange stream";
                }
            }
            enter_stage(state, ArenaCenteringAutorunStage::kWriteResult);
        }
        break;

    case ArenaCenteringAutorunStage::kWriteResult: {
        const nlohmann::json result = arena_centering_autorun_result_json(
            *state, config, *spatial_state, !camera_control->subscribe);
        state->result_written = write_json_atomically(
            config.result_json_path, result, &state->result_write_error);
        if (!state->result_written) {
            state->run_passed = false;
            if (state->error_message.empty()) {
                state->error_message = state->result_write_error;
            }
        }
        enter_stage(
            state,
            state->result_written && state->run_passed
                ? ArenaCenteringAutorunStage::kDone
                : ArenaCenteringAutorunStage::kFailed);
        if (config.exit_after_completion) requests.close_window = true;
        break;
    }

    case ArenaCenteringAutorunStage::kDisabled:
    case ArenaCenteringAutorunStage::kDone:
    case ArenaCenteringAutorunStage::kFailed:
        break;
    }
    return requests;
}

nlohmann::json arena_centering_autorun_result_json(
    const ArenaCenteringAutorunState& state,
    const ArenaCenteringAutorunConfig& config,
    const SpatialLayoutUiState& spatial_state,
    bool stream_stopped)
{
    nlohmann::json targets = nlohmann::json::array();
    for (const auto& target : state.targets) {
        targets.push_back({
            {"camera_serial", target.camera_serial},
            {"arena_id", target.arena_id},
            {"original_center_canvas_px", {
                {"x", target.original_center_x_canvas_px},
                {"y", target.original_center_y_canvas_px}}},
            {"candidate_center_canvas_px", {
                {"x", target.candidate_center_x_canvas_px},
                {"y", target.candidate_center_y_canvas_px}}},
        });
    }
    return {
        {"schema_id", "orange.gui_arena_centering_commissioning_result"},
        {"schema_version", 3},
        {"created_utc", get_current_utc_timestamp()},
        {"status", state.run_passed ? "pass" : "fail"},
        {"stage", arena_centering_autorun_stage_name(state.stage)},
        {"error", state.error_message.empty()
                      ? nlohmann::json(nullptr)
                      : nlohmann::json(state.error_message)},
        {"transaction_id", state.transaction_id},
        {"config", {
            {"citrus_config_path", config.citrus_config_path},
            {"camera_serials", config.camera_serials},
            {"frame_count", config.frame_count},
            {"foreground_gray_u8", config.foreground_gray_u8},
            {"symmetric_probe_canvas_px", config.symmetric_probe_canvas_px},
            {"verification_tolerance_camera_px",
             config.verification_tolerance_camera_px},
            {"maximum_refinement_canvas_px", config.maximum_refinement_canvas_px},
            {"resize_arenas", config.resize_arenas},
            {"rectangle_safety_margin_camera_px",
             config.rectangle_safety_margin_camera_px},
            {"rectangle_prediction_reserve_camera_px",
             config.rectangle_prediction_reserve_camera_px},
            {"rectangle_center_marker_tolerance_camera_px",
             config.rectangle_center_marker_tolerance_camera_px},
            {"rectangle_prediction_tolerance_camera_px",
             config.rectangle_prediction_tolerance_camera_px},
            {"rectangle_maximality_slack_camera_px",
             config.rectangle_maximality_slack_camera_px},
            {"maximum_arena_scale_change_fraction",
             config.maximum_arena_scale_change_fraction},
            {"maximum_ptp_capture_span_ns", config.maximum_ptp_capture_span_ns},
            {"projection_settle_milliseconds",
             config.projection_settle_milliseconds},
            {"require_projection_stability_capture",
             config.require_projection_stability_capture},
            {"projection_stability_interval_milliseconds",
             config.projection_stability_interval_milliseconds},
            {"projection_stability_max_capture_attempts",
             config.projection_stability_max_capture_attempts},
            {"projection_stability_max_center_delta_camera_px",
             config.projection_stability_max_center_delta_camera_px},
            {"projection_stability_max_corner_delta_camera_px",
             config.projection_stability_max_corner_delta_camera_px},
            {"save_captures", config.save_captures},
            {"save_verified_centers_armed",
             config.save_verified_centers_armed},
            {"save_verified_layout_armed",
             config.save_verified_layout_armed},
            {"fit_homographies_after_centering",
             config.fit_homographies_after_centering},
            {"accept_homographies_armed",
             config.accept_homographies_armed},
            {"homography_quality_thresholds",
             homography_quality_thresholds_json(config)},
        }},
        {"ownership", {
            {"mutated_geometry", config.resize_arenas
                ? "canonical_arena_square_placement_canvas_px"
                : "canonical_arena_center_canvas_px"},
            {"experimental_area_coordinate_space", "arena_relative"},
            {"experimental_area_local_center_rebased_for_size",
             config.resize_arenas},
            {"experimental_area_shape_or_size_changed", false},
            {"experimental_area_offset_from_arena_center_preserved", true},
            {"daily_installed_dish_registration_changed", false},
        }},
        {"targets", std::move(targets)},
        {"base_canvas_checksum", state.base_canvas_checksum},
        {"concurrent_analysis_policy",
         "one_task_per_camera_per_projection_state_then_join_barrier"},
        {"stage_records", state.stage_records},
        {"solve_results", state.solve_results},
        {"resize_proposals", state.resize_proposals},
        {"verification", state.verification},
        {"refinement_attempted", state.refinement_attempted},
        {"terminal_intent", state.terminal_intent},
        {"terminal_status", state.terminal_status},
        {"homography", {
            {"transaction_id", state.homography_transaction_id},
            {"capture_group_id", state.homography_capture_group_id},
            {"committed_canvas_checksum", state.committed_canvas_checksum},
            {"fit_requested", state.homography_fit_requested},
            {"promotion_requested", state.homography_promotion_requested},
            {"committed", state.homography_committed},
            {"candidate_status", state.homography_candidate_status},
            {"verification", state.homography_verification},
            {"pattern_placement_policy",
             "centered_on_committed_arena_geometry_without_geometry_mutation"},
            {"transform_direction",
             "camera_native_px_to_final_display_canvas_px"},
        }},
        {"persistence", {
            {"session_policy", "one_calibration_session_per_centering_transaction"},
            {"session_id", spatial_state.calibration_session_id},
            {"session_dir", spatial_state.calibration_session_dir},
        }},
        {"calibration_preflight", {
            {"applied", state.preflight_applied},
            {"prepare_status", state.preflight_prepare_status},
            {"capture_camera_settings", state.capture_camera_settings},
            {"restore_attempted", state.preflight_restore_attempted},
            {"restore_ok", state.preflight_restore_ok},
            {"restore_status", state.preflight_restore_status},
        }},
        {"shutdown", {{"stream_stopped", stream_stopped}}},
    };
}

}  // namespace orange::gui
