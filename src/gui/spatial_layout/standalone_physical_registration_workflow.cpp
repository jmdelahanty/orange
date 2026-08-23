#include "gui/spatial_layout/standalone_physical_registration_workflow.h"

#include "dish_top_rim_observation.h"
#include "fsuid_guard.h"
#include "gui/spatial_layout/calibration_transaction_bridge.h"
#include "gui/spatial_layout/calibration_workflow.h"
#include "gui/spatial_layout/group_capture_controller.h"
#include "gui/spatial_layout/metadata_panel.h"
#include "gui/spatial_layout/physical_registration_selection.h"
#include "gui/spatial_layout/save_job_preparation.h"
#include "gui/spatial_layout/save_jobs.h"
#include "gui/spatial_layout/session_io.h"
#include "gui/spatial_layout/sha256.h"
#include "imgui.h"
#include "project.h"

#include <opencv2/imgproc.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <future>
#include <iomanip>
#include <limits>
#include <sstream>

#include <unistd.h>

namespace orange::gui::spatial_layout {
namespace {

namespace fs = std::filesystem;

std::string next_transaction_id(const std::string& created_utc)
{
    static std::atomic<std::uint64_t> sequence{1};
    return "physicalregtxn_" + sanitize_artifact_component(created_utc) +
        "_" + std::to_string(static_cast<long long>(::getpid())) + "_" +
        std::to_string(sequence.fetch_add(1, std::memory_order_relaxed));
}

const CameraParams* find_camera(
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

const SpatialLayoutGroupCaptureFrame* find_capture(
    const SpatialLayoutUiState& ui_state,
    const std::string& serial)
{
    for (const auto& capture : ui_state.group_captures) {
        if (capture.valid && capture.camera_serial == serial) return &capture;
    }
    return nullptr;
}

std::vector<unsigned char> gray_from_rgba(
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

struct HoughSettings {
    double dp = 1.25;
    double min_dist_fraction = 0.20;
    double param1 = 120.0;
    double param2 = 30.0;
    double min_radius_fraction = 0.18;
    double max_radius_fraction = 0.49;
    double radius_adjustment_px = 0.0;
    int median_blur_ksize = 5;
    int max_detection_dimension_px = 2048;
    bool fallback_enabled = true;
};

HoughSettings hough_settings_from_ui(const SpatialLayoutUiState& ui_state)
{
    HoughSettings settings;
    settings.dp = ui_state.hough_dp;
    settings.min_dist_fraction = ui_state.hough_min_dist_fraction;
    settings.param1 = ui_state.hough_param1;
    settings.param2 = ui_state.hough_param2;
    settings.min_radius_fraction = ui_state.hough_min_radius_fraction;
    settings.max_radius_fraction = ui_state.hough_max_radius_fraction;
    settings.radius_adjustment_px = ui_state.hough_radius_adjustment_px;
    settings.median_blur_ksize = ui_state.hough_median_blur_ksize;
    settings.max_detection_dimension_px =
        ui_state.hough_max_detection_dimension_px;
    settings.fallback_enabled = ui_state.hough_fallback_enabled;
    return settings;
}

bool detect_rim(
    const SpatialLayoutGroupCaptureFrame& capture,
    const HoughSettings& settings,
    orange::calibration::DishTopRimCircle* circle_out,
    std::vector<unsigned char>* gray_out,
    std::string* error_out)
{
    std::vector<unsigned char> gray = gray_from_rgba(capture);
    if (gray.empty()) {
        if (error_out) *error_out = "invalid_camera_only_rim_frame";
        return false;
    }
    cv::Mat full(capture.height, capture.width, CV_8UC1, gray.data());
    cv::Mat detection = full;
    const int max_dimension = std::clamp(
        settings.max_detection_dimension_px, 256, 8192);
    const int source_max = std::max(full.cols, full.rows);
    double scale = 1.0;
    if (source_max > max_dimension) {
        scale = static_cast<double>(max_dimension) /
            static_cast<double>(source_max);
        cv::resize(full, detection, cv::Size(), scale, scale, cv::INTER_AREA);
    }
    int blur_size = std::clamp(settings.median_blur_ksize, 1, 31);
    if ((blur_size % 2) == 0) ++blur_size;
    cv::Mat blurred;
    if (blur_size > 1) cv::medianBlur(detection, blurred, blur_size);
    else blurred = detection;
    const double minimum_dimension = std::min(blurred.cols, blurred.rows);
    const int minimum_radius = std::max(4, static_cast<int>(std::lround(
        minimum_dimension * std::clamp(
            settings.min_radius_fraction, 0.001, 1.0))));
    const int maximum_radius = std::max(
        minimum_radius + 1,
        static_cast<int>(std::lround(minimum_dimension * std::clamp(
            settings.max_radius_fraction, 0.001, 1.5))));
    std::vector<cv::Vec3f> circles;
    auto run = [&](double param1, double param2, double dp) {
        cv::HoughCircles(
            blurred, circles, cv::HOUGH_GRADIENT,
            std::clamp(dp, 1.0, 3.0),
            std::max(1.0, minimum_dimension * std::clamp(
                settings.min_dist_fraction, 0.01, 2.0)),
            std::clamp(param1, 1.0, 500.0),
            std::clamp(param2, 1.0, 500.0),
            minimum_radius, maximum_radius);
    };
    try {
        run(settings.param1, settings.param2, settings.dp);
        if (circles.empty() && settings.fallback_enabled) {
            run(settings.param1 * 0.75,
                settings.param2 * 0.73,
                settings.dp * 0.96);
        }
    } catch (const cv::Exception& exception) {
        if (gray_out) *gray_out = std::move(gray);
        if (error_out) *error_out = exception.what();
        return false;
    }
    if (circles.empty()) {
        if (gray_out) *gray_out = std::move(gray);
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
    const double inverse_scale = 1.0 / scale;
    circle_out->center.x = (*best)[0] * inverse_scale;
    circle_out->center.y = (*best)[1] * inverse_scale;
    circle_out->radius_px = std::max(
        1.0,
        (*best)[2] * inverse_scale + settings.radius_adjustment_px);
    if (gray_out) *gray_out = std::move(gray);
    return true;
}

bool write_json_atomically(
    const fs::path& path,
    const nlohmann::json& value,
    std::string* error_out)
{
    if (!ensure_directory_for_spatial_session(path.parent_path(), error_out)) {
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

bool all_rims_confirmed(
    const StandalonePhysicalRegistrationWorkflowUiState& workflow)
{
    return !workflow.targets.empty() && std::all_of(
        workflow.targets.begin(), workflow.targets.end(),
        [](const auto& target) {
            return target.rim_detection_ok &&
                target.rim_operator_confirmed &&
                target.accepted_rim_radius_camera_px > 0.0 &&
                std::isfinite(target.accepted_rim_center_x_camera_px) &&
                std::isfinite(target.accepted_rim_center_y_camera_px) &&
                std::isfinite(target.accepted_rim_radius_camera_px);
        });
}

bool capture_identity_is_valid(
    const SpatialLayoutGroupCaptureFrame& capture,
    const CameraParams& camera,
    std::string* error_out)
{
    if (!capture.valid || capture.camera_serial != camera.camera_serial) {
        if (error_out) *error_out = "capture_camera_identity_mismatch";
        return false;
    }
    if (capture.source_array_role != "images_full" ||
        capture.width != static_cast<int>(camera.width) ||
        capture.height != static_cast<int>(camera.height)) {
        if (error_out) *error_out = "capture_not_native_full_resolution";
        return false;
    }
    if (capture.camera_timestamp_ns == 0 || capture.timestamp_sys_ns == 0) {
        if (error_out) *error_out = "capture_timestamp_missing";
        return false;
    }
    if (capture.last_camera_frame_id == 0 || capture.last_local_frame_id == 0) {
        if (error_out) *error_out = "fresh_frame_identity_missing";
        return false;
    }
    return true;
}

struct RimAnalysisResult {
    bool ok = false;
    orange::calibration::DishTopRimCircle circle;
    std::vector<unsigned char> gray;
    std::string error;
};

struct RimAnalysisBatchResult {
    bool ok = false;
    std::string error;
    std::vector<RimAnalysisResult> targets;
};

class RimAnalysisWorker {
public:
    bool Start(
        std::vector<SpatialLayoutGroupCaptureFrame> captures,
        HoughSettings settings,
        std::string* error_out)
    {
        if (future_.valid()) {
            if (error_out) *error_out = "physical_rim_analysis_already_active";
            return false;
        }
        future_ = std::async(
            std::launch::async,
            [captures = std::move(captures), settings]() mutable {
                RimAnalysisBatchResult batch;
                try {
                    std::vector<std::future<RimAnalysisResult>> futures;
                    futures.reserve(captures.size());
                    for (auto& capture : captures) {
                        futures.push_back(std::async(
                            std::launch::async,
                            [capture = std::move(capture), settings]() mutable {
                                RimAnalysisResult result;
                                result.ok = detect_rim(
                                    capture, settings, &result.circle,
                                    &result.gray, &result.error);
                                return result;
                            }));
                    }
                    batch.targets.reserve(futures.size());
                    for (auto& future : futures) {
                        batch.targets.push_back(future.get());
                    }
                    batch.ok = true;
                } catch (const std::exception& exception) {
                    batch.error = std::string("physical_rim_analysis_threw:") +
                        exception.what();
                } catch (...) {
                    batch.error = "physical_rim_analysis_threw_unknown_exception";
                }
                return batch;
            });
        return true;
    }

    bool IsBusy() const
    {
        return future_.valid() &&
            future_.wait_for(std::chrono::seconds(0)) !=
                std::future_status::ready;
    }

    bool TryTake(RimAnalysisBatchResult* result_out)
    {
        if (result_out == nullptr || !future_.valid() || IsBusy()) return false;
        *result_out = future_.get();
        return true;
    }

private:
    mutable std::future<RimAnalysisBatchResult> future_;
};

RimAnalysisWorker& rim_analysis_worker()
{
    static RimAnalysisWorker worker;
    return worker;
}

bool prepare_captures_for_analysis(
    SpatialLayoutUiState* ui_state,
    StandalonePhysicalRegistrationWorkflowUiState* workflow,
    const CameraParams* cameras,
    int camera_count,
    std::string* error_out)
{
    for (auto& target : workflow->targets) {
        const auto* capture = find_capture(*ui_state, target.camera_serial);
        const auto* camera = find_camera(
            cameras, camera_count, target.camera_serial);
        if (capture == nullptr || camera == nullptr) {
            if (error_out) {
                *error_out = "missing_camera_only_rim_capture:" +
                    target.camera_serial;
            }
            return false;
        }
        std::string identity_error;
        if (!capture_identity_is_valid(*capture, *camera, &identity_error)) {
            if (error_out) {
                *error_out = target.camera_serial + ":" + identity_error;
            }
            return false;
        }
        target.rim_capture = *capture;
        target.rim_capture.texture = 0;
    }
    const nlohmann::json membership = ui_state->group_capture_metadata
        .capture_group_membership;
    const nlohmann::json timing = membership.value(
        "capture_timing", nlohmann::json::object());
    if (!timing.value("all_camera_timestamps_nonzero", false) ||
        timing.value("camera_timestamp_span_ns", std::uint64_t{0}) >
            workflow->maximum_group_camera_timestamp_span_ns) {
        if (error_out) {
            *error_out =
                "camera_group_timestamp_span_exceeds_registration_policy";
        }
        return false;
    }

    std::vector<SpatialLayoutGroupCaptureFrame> analysis_captures;
    analysis_captures.reserve(workflow->targets.size());
    for (auto& target : workflow->targets) {
        analysis_captures.push_back(target.rim_capture);
    }
    return rim_analysis_worker().Start(
        std::move(analysis_captures), hough_settings_from_ui(*ui_state),
        error_out);
}

bool consume_analysis_results(
    StandalonePhysicalRegistrationWorkflowUiState* workflow,
    std::string* error_out)
{
    RimAnalysisBatchResult batch;
    if (!rim_analysis_worker().TryTake(&batch)) return false;
    if (!batch.ok || batch.targets.size() != workflow->targets.size()) {
        if (error_out) {
            *error_out = batch.error.empty()
                ? "physical_rim_analysis_result_count_mismatch"
                : batch.error;
        }
        return true;
    }
    for (std::size_t index = 0; index < workflow->targets.size(); ++index) {
        RimAnalysisResult result = std::move(batch.targets[index]);
        auto& target = workflow->targets[index];
        target.rim_detection_ok = result.ok;
        target.rim_detection_error = std::move(result.error);
        target.rim_gray = std::move(result.gray);
        if (result.ok) {
            target.detected_rim_center_x_camera_px = result.circle.center.x;
            target.detected_rim_center_y_camera_px = result.circle.center.y;
            target.detected_rim_radius_camera_px = result.circle.radius_px;
            target.accepted_rim_center_x_camera_px = result.circle.center.x;
            target.accepted_rim_center_y_camera_px = result.circle.center.y;
            target.accepted_rim_radius_camera_px = result.circle.radius_px;
        }
    }
    if (error_out) error_out->clear();
    return true;
}

bool queue_artifacts(
    SpatialLayoutUiState* ui_state,
    StandalonePhysicalRegistrationWorkflowUiState* workflow,
    const CameraParams* cameras,
    int camera_count,
    std::string* error_out)
{
    if (top_rim_observation_save_worker_is_busy()) {
        if (error_out) *error_out = "top_rim_writer_busy";
        return false;
    }
    std::vector<TopRimObservationSaveJob> jobs;
    jobs.reserve(workflow->targets.size());
    for (auto& target : workflow->targets) {
        const CameraParams* camera = find_camera(
            cameras, camera_count, target.camera_serial);
        if (camera == nullptr) {
            if (error_out) {
                *error_out = "camera_closed_before_save:" +
                    target.camera_serial;
            }
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
                workflow->session_artifact_root, detected, accepted,
                &job, error_out)) {
            return false;
        }
        job.session_dir = ui_state->calibration_session_dir;
        job.request.capture.projector_state = workflow->projector_state;
        job.request.capture.projector_visible_to_camera =
            workflow->projector_state == "external_static";
        job.request.operator_status =
            "orange_standalone_daily_physical_registration_operator_confirmed";
        job.request.operator_notes =
            "Standalone grouped physical registration: operator confirmed the water-side inner rim.";
        job.request.arena_context["projection_registration"] = {
            {"product_id",
             orange::calibration::kDailyProjectionRegistrationProductId},
            {"authority", "citrus"},
            {"status", "not_applicable"},
            {"reason", "no_active_projection_transaction"},
        };
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

nlohmann::json target_manifest_json(
    const PhysicalDishRegistrationTargetUiState& target)
{
    return {
        {"camera_serial", target.camera_serial},
        {"raw_hough_circle", {
            {"center_px", {
                {"x", target.detected_rim_center_x_camera_px},
                {"y", target.detected_rim_center_y_camera_px}}},
            {"radius_px", target.detected_rim_radius_camera_px},
        }},
        {"accepted_inner_rim", {
            {"center_px", {
                {"x", target.accepted_rim_center_x_camera_px},
                {"y", target.accepted_rim_center_y_camera_px}}},
            {"radius_px", target.accepted_rim_radius_camera_px},
            {"operator_confirmed", target.rim_operator_confirmed},
        }},
        {"observation", {
            {"artifact_id", target.rim_observation_artifact_id},
            {"path", target.rim_observation_path},
            {"sha256", target.rim_observation_sha256},
            {"completion_manifest_path", target.rim_manifest_path},
            {"completion_manifest_sha256", target.rim_manifest_sha256},
        }},
        {"capture", {
            {"capture_group_id", target.rim_capture.capture_group_id},
            {"source_array_role", target.rim_capture.source_array_role},
            {"width", target.rim_capture.width},
            {"height", target.rim_capture.height},
            {"camera_timestamp_ns", target.rim_capture.camera_timestamp_ns},
            {"timestamp_sys_ns", target.rim_capture.timestamp_sys_ns},
            {"last_camera_frame_id", target.rim_capture.last_camera_frame_id},
            {"last_local_frame_id", target.rim_capture.last_local_frame_id},
        }},
    };
}

bool finalize_group_manifest(
    SpatialLayoutUiState* ui_state,
    StandalonePhysicalRegistrationWorkflowUiState* workflow,
    std::string* error_out)
{
    nlohmann::json targets = nlohmann::json::array();
    for (auto& target : workflow->targets) {
        const PhysicalRegistrationArtifactCandidate candidate =
            validate_physical_registration_artifact(
                workflow->calibration_base_dir,
                target.rim_observation_path,
                target.camera_serial,
                target.rim_capture.width,
                target.rim_capture.height,
                target.rim_capture.camera_pixel_format);
        if (!candidate.compatible ||
            candidate.artifact_id != target.rim_observation_artifact_id) {
            if (error_out) {
                *error_out = "saved_observation_incomplete_or_invalid:" +
                    target.camera_serial + ":" +
                    candidate.compatibility_reason;
            }
            return false;
        }
        target.rim_observation_sha256 = candidate.observation_sha256;
        target.rim_manifest_path = candidate.manifest_path.string();
        target.rim_manifest_sha256 = candidate.manifest_sha256;
        targets.push_back(target_manifest_json(target));
    }
    const nlohmann::json manifest = {
        {"schema_id",
         "orange.calibration.standalone_physical_registration_group"},
        {"schema_version", 1},
        {"status", "accepted"},
        {"transaction_id", workflow->transaction_id},
        {"created_utc", workflow->created_utc},
        {"authority", "orange"},
        {"coordinate_space", "camera_native_pixels"},
        {"physical_registration_product_id",
         orange::calibration::kDailyPhysicalDishRegistrationProductId},
        {"projection_registration", {
            {"product_id",
             orange::calibration::kDailyProjectionRegistrationProductId},
            {"authority", "citrus"},
            {"status", "not_applicable"},
            {"reason", "no_active_projection_transaction"},
        }},
        {"projector_state", workflow->projector_state},
        {"capture_group_membership",
         ui_state->group_capture_metadata.capture_group_membership},
        {"targets", targets},
        {"selection", {
            {"status", "available_not_selected"},
            {"reason", "operator_selection_is_a_separate_action"},
        }},
    };
    workflow->grouped_manifest_path =
        (fs::path(workflow->transaction_dir) / "manifest.json").string();
    if (!write_json_atomically(
            workflow->grouped_manifest_path, manifest, error_out) ||
        !checksum::file_sha256(
            workflow->grouped_manifest_path,
            &workflow->grouped_manifest_sha256,
            error_out)) {
        return false;
    }
    return true;
}

bool begin_workflow(
    SpatialLayoutUiState* ui_state,
    const CameraParams* cameras,
    const CameraEachSelect* selections,
    int camera_count,
    SpatialSnapshotWorker* const* workers,
    const std::string& artifact_root_dir,
    bool recording_mutation_locked,
    std::string* error_out)
{
    if (ui_state == nullptr || cameras == nullptr || selections == nullptr ||
        workers == nullptr || camera_count <= 0) {
        if (error_out) *error_out = "Open and stream cameras before physical registration.";
        return false;
    }
    if (recording_mutation_locked) {
        if (error_out) {
            *error_out = "Stop and finalize recording before physical registration.";
        }
        return false;
    }
    initialize_group_capture_camera_scope(
        ui_state, cameras, selections, workers, camera_count);
    if (ui_state->group_capture_selected_camera_serials.empty()) {
        if (error_out) *error_out = "Select at least one physical-registration camera.";
        return false;
    }
    if (group_capture_workflow_active(*ui_state) ||
        rim_analysis_worker().IsBusy() ||
        top_rim_observation_save_worker_is_busy() ||
        generic_calibration_image_set_save_worker_is_busy()) {
        if (error_out) *error_out = "Finish the active capture/save operation first.";
        return false;
    }
    const bool physical_state_confirmed =
        ui_state->standalone_physical_registration_workflow
            .physical_state_confirmed;
    if (!physical_state_confirmed) {
        if (error_out) {
            *error_out =
                "Confirm dishes, water, holder, filters, and production illumination before starting.";
        }
        return false;
    }

    StandalonePhysicalRegistrationWorkflowUiState workflow;
    workflow.active = true;
    workflow.stage = "ready_capture";
    workflow.created_utc = get_current_utc_timestamp();
    workflow.transaction_id = next_transaction_id(workflow.created_utc);
    workflow.physical_state_confirmed = true;
    workflow.projector_state =
        ui_state->standalone_physical_registration_workflow.projector_state;
    workflow.maximum_group_camera_timestamp_span_ns =
        ui_state->standalone_physical_registration_workflow
            .maximum_group_camera_timestamp_span_ns;
    for (const std::string& serial :
         ui_state->group_capture_selected_camera_serials) {
        if (find_camera(cameras, camera_count, serial) == nullptr) {
            if (error_out) *error_out = "Selected camera is not open: " + serial;
            return false;
        }
        PhysicalDishRegistrationTargetUiState target;
        target.camera_serial = serial;
        workflow.targets.push_back(std::move(target));
    }

    int session_camera_index = ui_state->selected_camera;
    if (session_camera_index < 0 || session_camera_index >= camera_count) {
        session_camera_index = 0;
    }
    std::string session_root;
    if (!ensure_spatial_calibration_session(
            ui_state, cameras[session_camera_index], artifact_root_dir,
            &session_root, error_out)) {
        return false;
    }
    workflow.session_artifact_root = session_root;
    workflow.calibration_base_dir =
        calibration_base_dir_from_artifact_root(artifact_root_dir).string();
    workflow.transaction_dir =
        (fs::path(ui_state->calibration_session_dir) /
         "physical_registrations" / workflow.transaction_id).string();
    if (!ensure_directory_for_spatial_session(
            workflow.transaction_dir, error_out)) return false;

    if (!acquire_spatial_calibration_transaction(
            ui_state,
            kStandalonePhysicalRegistrationTransactionOwner,
            workflow.transaction_id,
            orange::calibration::WorkflowKind::kDailyRegistration,
            ui_state->group_capture_selected_camera_serials,
            orange::calibration::mutation_set(
                orange::calibration::Mutation::kCameraParameters),
            "Capture and accept today's camera-native physical dish rims without projection registration.",
            error_out)) {
        return false;
    }
    workflow.status =
        "Ready to capture all selected dishes in production optical conditions; Citrus is not required.";
    ui_state->standalone_physical_registration_workflow =
        std::move(workflow);
    return true;
}

bool request_capture(
    SpatialLayoutUiState* ui_state,
    const CameraParams* cameras,
    const CameraEachSelect* selections,
    int camera_count,
    SpatialSnapshotWorker* const* workers,
    std::string* error_out)
{
    auto& workflow = ui_state->standalone_physical_registration_workflow;
    apply_calibration_workflow_profile_defaults(
        ui_state, "installed_tank_registration", nullptr);
    ui_state->calibration_image_set_purpose = "dish_top_rim_observation";
    ui_state->calibration_image_set_target_plane = "dish_top_rim";
    ui_state->calibration_image_set_image_role =
        "physical_inner_rim_source";
    ui_state->calibration_image_set_projected_pattern_id = "none";
    ui_state->calibration_image_set_projected_pattern_type = "none";
    ui_state->calibration_image_set_scale_target_type = "unknown";
    ui_state->calibration_capture_stage =
        "standalone_daily_physical_registration";
    ui_state->calibration_projector_state = workflow.projector_state;
    ui_state->calibration_projector_visible_to_camera =
        workflow.projector_state == "external_static";
    ui_state->calibration_filter_state =
        "installed: HOYA Creative Filter Infrared R72 67 mm (Kenko Tokina)";
    ui_state->calibration_runtime_filter_state =
        ui_state->calibration_filter_state;
    ui_state->calibration_requires_filter_reinstalled_repeatably = false;
    ui_state->calibration_light_handling = "keep_or_restore_mapped_pulse";
    ui_state->calibration_dish_fill_state = "recording_fill_level";
    ui_state->calibration_target_method = "physical_dish_inner_rim";
    ui_state->calibration_pattern_type = "not_applicable";
    ui_state->calibration_pattern_domain = "circular_experimental_domain";
    ui_state->calibration_homography_role = "not_applicable";
    ui_state->calibration_reference_only = false;
    ui_state->calibration_physical_target_used = true;
    ui_state->calibration_projected_pattern_used_as_coordinate_target = false;
    apply_illumination_preset(ui_state, "custom_ttl_nir_strobe_855nm");
    ui_state->group_capture_scene_recipe = "none";
    ui_state->group_capture_scene_options = nlohmann::json::object();
    if (!request_group_full_resolution_snapshots_camera_only(
            ui_state, cameras, selections, camera_count, workers,
            std::max(1, ui_state->calibration_average_frame_count),
            error_out, workflow.transaction_id,
            workflow.transaction_id + "_capture",
            kStandalonePhysicalRegistrationTransactionOwner)) {
        return false;
    }
    workflow.stage = "waiting_capture";
    workflow.status =
        "Capturing fresh full-resolution grouped frames in the current production optical state.";
    workflow.error.clear();
    return true;
}

void render_review_images(
    const SpatialLayoutUiState& ui_state,
    StandalonePhysicalRegistrationWorkflowUiState* workflow)
{
    if (workflow == nullptr || workflow->targets.empty()) return;
    if (!ImGui::BeginTable(
            "standalone-physical-rim-images",
            static_cast<int>(workflow->targets.size()),
            ImGuiTableFlags_Borders | ImGuiTableFlags_SizingStretchSame)) {
        return;
    }
    for (const auto& target : workflow->targets) {
        ImGui::TableSetupColumn(target.camera_serial.c_str());
    }
    ImGui::TableHeadersRow();
    ImGui::TableNextRow();
    for (int index = 0; index < static_cast<int>(workflow->targets.size());
         ++index) {
        ImGui::TableSetColumnIndex(index);
        const auto& target = workflow->targets[static_cast<std::size_t>(index)];
        const auto* capture = find_capture(ui_state, target.camera_serial);
        if (capture == nullptr || capture->texture == 0 ||
            capture->texture_width <= 0 || capture->texture_height <= 0) {
            ImGui::TextDisabled("No review image");
            continue;
        }
        const float width = std::max(80.0f, ImGui::GetContentRegionAvail().x);
        const float height = width * static_cast<float>(capture->texture_height) /
            static_cast<float>(capture->texture_width);
        const ImVec2 top_left = ImGui::GetCursorScreenPos();
        ImGui::Image(
            reinterpret_cast<ImTextureID>(
                static_cast<intptr_t>(capture->texture)),
            ImVec2(width, height));
        ImDrawList* draw = ImGui::GetWindowDrawList();
        const float scale_x = width / static_cast<float>(capture->width);
        const float scale_y = height / static_cast<float>(capture->height);
        const auto point = [&](double x, double y) {
            return ImVec2(
                top_left.x + static_cast<float>(x) * scale_x,
                top_left.y + static_cast<float>(y) * scale_y);
        };
        const auto circle = [&](double x, double y, double radius,
                                ImU32 color, float thickness) {
            if (radius <= 0.0) return;
            draw->AddCircle(
                point(x, y),
                static_cast<float>(radius) *
                    0.5f * (scale_x + scale_y),
                color, 128, thickness);
        };
        circle(
            target.detected_rim_center_x_camera_px,
            target.detected_rim_center_y_camera_px,
            target.detected_rim_radius_camera_px,
            IM_COL32(255, 32, 210, 255), 1.5f);
        circle(
            target.accepted_rim_center_x_camera_px,
            target.accepted_rim_center_y_camera_px,
            target.accepted_rim_radius_camera_px,
            IM_COL32(255, 165, 32, 255), 2.2f);
    }
    ImGui::EndTable();
    ImGui::TextDisabled(
        "Magenta: raw Hough proposal. Orange: operator-adjustable accepted water-side inner rim.");
}

void render_target_table(
    StandalonePhysicalRegistrationWorkflowUiState* workflow)
{
    if (workflow == nullptr || !ImGui::BeginTable(
            "standalone-physical-rim-targets", 6,
            ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                ImGuiTableFlags_SizingStretchProp)) {
        return;
    }
    ImGui::TableSetupColumn("Camera");
    ImGui::TableSetupColumn("Center X");
    ImGui::TableSetupColumn("Center Y");
    ImGui::TableSetupColumn("Radius");
    ImGui::TableSetupColumn("QC");
    ImGui::TableSetupColumn("Confirm inner rim");
    ImGui::TableHeadersRow();
    for (auto& target : workflow->targets) {
        ImGui::PushID(("standalone-rim-" + target.camera_serial).c_str());
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        ImGui::Text("Cam%s", target.camera_serial.c_str());
        ImGui::TableSetColumnIndex(1);
        ImGui::InputDouble("##cx", &target.accepted_rim_center_x_camera_px,
                           0.1, 1.0, "%.3f");
        ImGui::TableSetColumnIndex(2);
        ImGui::InputDouble("##cy", &target.accepted_rim_center_y_camera_px,
                           0.1, 1.0, "%.3f");
        ImGui::TableSetColumnIndex(3);
        ImGui::InputDouble("##r", &target.accepted_rim_radius_camera_px,
                           0.1, 1.0, "%.3f");
        ImGui::TableSetColumnIndex(4);
        ImGui::TextColored(
            target.rim_detection_ok
                ? ImVec4(0.25f, 0.9f, 0.35f, 1.0f)
                : ImVec4(1.0f, 0.45f, 0.25f, 1.0f),
            "%s", target.rim_detection_ok ? "detected" : "FAILED");
        ImGui::TableSetColumnIndex(5);
        ImGui::Checkbox("##confirm", &target.rim_operator_confirmed);
        ImGui::PopID();
    }
    ImGui::EndTable();
}

void abort_workflow(
    SpatialLayoutUiState* ui_state,
    const std::string& reason)
{
    if (ui_state == nullptr) return;
    auto& workflow = ui_state->standalone_physical_registration_workflow;
    workflow.active = false;
    workflow.stage = "aborted";
    workflow.status = "Standalone physical registration aborted; no active selection changed.";
    workflow.error = reason;
    release_spatial_calibration_transaction(
        ui_state, "aborted", reason);
}

}  // namespace

void advance_standalone_physical_registration_workflow(
    SpatialLayoutUiState* ui_state,
    const CameraParams* cameras_params,
    const CameraEachSelect*,
    int num_cameras,
    SpatialSnapshotWorker* const*,
    const std::string&)
{
    if (ui_state == nullptr) return;
    auto& workflow = ui_state->standalone_physical_registration_workflow;
    if (!workflow.active) return;

    auto fail = [&](const std::string& error) {
        workflow.active = false;
        workflow.stage = "failed";
        workflow.status =
            "Standalone physical registration failed; immutable evidence already written, if any, is preserved and no active selection changed.";
        workflow.error = error;
        release_spatial_calibration_transaction(
            ui_state, "failed", error);
    };

    if (workflow.stage == "waiting_capture") {
        if (group_capture_workflow_active(*ui_state)) return;
        if (ui_state->group_capture_workflow_state != "complete" ||
            ui_state->group_capture_terminal_outcome != "complete") {
            fail("camera_only_group_capture_failed:" +
                 ui_state->group_capture_error);
            return;
        }
        std::string error;
        if (!prepare_captures_for_analysis(
                ui_state, &workflow, cameras_params, num_cameras, &error)) {
            fail(error);
            return;
        }
        workflow.stage = "waiting_analysis";
        workflow.status =
            "Analyzing all selected camera rims concurrently off the GUI thread.";
        return;
    }
    if (workflow.stage == "waiting_analysis") {
        if (rim_analysis_worker().IsBusy()) return;
        std::string error;
        if (!consume_analysis_results(&workflow, &error)) return;
        if (!error.empty()) {
            fail(error);
            return;
        }
        workflow.stage = "review_rims";
        workflow.status =
            "Review every raw Hough proposal, adjust only if needed, and confirm the water-side inner rim.";
        return;
    }
    if (workflow.stage == "waiting_save") {
        if (top_rim_observation_save_worker_is_busy()) return;
        std::string error;
        if (!finalize_group_manifest(ui_state, &workflow, &error)) {
            fail(error.empty() ? "physical_registration_save_incomplete" : error);
            return;
        }
        workflow.active = false;
        workflow.stage = "complete";
        workflow.status =
            "Accepted physical registrations saved. They are available but not selected; choose each camera's intended artifact below.";
        workflow.error.clear();
        release_spatial_calibration_transaction(
            ui_state, "complete", workflow.status);
        ui_state->physical_registration_selection.initialized = false;
    }
}

void render_standalone_physical_registration_workflow_panel(
    SpatialLayoutUiState* ui_state,
    const CameraParams* cameras_params,
    const CameraEachSelect* cameras_select,
    int num_cameras,
    SpatialSnapshotWorker* const* spatial_snapshot_workers,
    const std::string& artifact_root_dir,
    bool recording_mutation_locked)
{
    if (ui_state == nullptr) return;
    auto& workflow = ui_state->standalone_physical_registration_workflow;
    ImGui::SeparatorText("Standalone Daily Physical Dish Registration");
    ImGui::TextWrapped(
        "Orange captures and accepts the camera-native water-side inner rim. Citrus, a canvas, and a projector are not required. Projection alignment remains a separate optional workflow.");
    if (workflow.stage == "idle" || workflow.stage == "complete" ||
        workflow.stage == "failed" || workflow.stage == "aborted") {
        const char* projector_states[] = {"off", "not_in_use", "external_static"};
        int projector_index = workflow.projector_state == "not_in_use"
            ? 1
            : workflow.projector_state == "external_static" ? 2 : 0;
        if (ImGui::Combo(
                "Observed projector state", &projector_index,
                projector_states, IM_ARRAYSIZE(projector_states))) {
            workflow.projector_state = projector_states[projector_index];
        }
        ImGui::Checkbox(
            "Dishes, recording fill, holder, filters, and production IR/TTL state are ready",
            &workflow.physical_state_confirmed);
        ImGui::InputScalar(
            "Maximum grouped camera timestamp span (ns)",
            ImGuiDataType_U64,
            &workflow.maximum_group_camera_timestamp_span_ns);
        ImGui::BeginDisabled(
            !workflow.physical_state_confirmed || recording_mutation_locked);
        if (ImGui::Button("Start Standalone Physical Registration")) {
            std::string error;
            if (!begin_workflow(
                    ui_state, cameras_params, cameras_select, num_cameras,
                    spatial_snapshot_workers, artifact_root_dir,
                    recording_mutation_locked, &error)) {
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
        ImGui::TextColored(
            ImVec4(1.0f, 0.45f, 0.25f, 1.0f),
            "%s", workflow.error.c_str());
    }

    if (workflow.stage == "ready_capture") {
        ImGui::TextWrapped(
            "The current production camera and illumination state will be preserved. Orange requests a fresh full-resolution temporal mean from every selected streaming camera.");
        if (ImGui::Button("Capture Physical Inner Rims")) {
            std::string error;
            if (!request_capture(
                    ui_state, cameras_params, cameras_select, num_cameras,
                    spatial_snapshot_workers, &error)) {
                workflow.error = error;
            }
        }
    } else if (workflow.stage == "review_rims") {
        render_review_images(*ui_state, &workflow);
        render_target_table(&workflow);
        ImGui::Checkbox(
            "I accept these physical fits; save does not select them for runtime",
            &workflow.save_accepted_rims_armed);
        ImGui::BeginDisabled(
            !all_rims_confirmed(workflow) ||
            !workflow.save_accepted_rims_armed);
        if (ImGui::Button("Save Accepted Physical Registrations")) {
            std::string error;
            if (!queue_artifacts(
                    ui_state, &workflow, cameras_params, num_cameras, &error)) {
                workflow.error = error;
            } else {
                workflow.stage = "waiting_save";
                workflow.status =
                    "Writing per-camera schema-v2 observations before atomically publishing the grouped transaction manifest.";
                workflow.error.clear();
            }
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button("Retry Entire Group")) {
            workflow.stage = "ready_capture";
            workflow.status = "Ready to request a new fresh grouped capture.";
            workflow.error.clear();
            for (auto& target : workflow.targets) {
                const std::string camera_serial = target.camera_serial;
                target = PhysicalDishRegistrationTargetUiState{};
                target.camera_serial = camera_serial;
            }
        }
    } else if (workflow.stage == "complete") {
        ImGui::Text("Group manifest: %s", workflow.grouped_manifest_path.c_str());
        ImGui::TextDisabled("SHA-256: %s", workflow.grouped_manifest_sha256.c_str());
    }

    if (workflow.active) {
        const bool busy = group_capture_workflow_active(*ui_state) ||
            rim_analysis_worker().IsBusy() ||
            top_rim_observation_save_worker_is_busy();
        ImGui::BeginDisabled(busy);
        if (ImGui::Button("Abort Physical Registration")) {
            abort_workflow(ui_state, "operator_aborted");
        }
        ImGui::EndDisabled();
    }
}

}  // namespace orange::gui::spatial_layout
