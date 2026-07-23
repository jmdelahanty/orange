#include "gui/spatial_layout/save_jobs.h"

#include "gui/spatial_layout/physical_target_bundle.h"
#include "gui/spatial_layout/session_io.h"
#include "gui/spatial_layout/session_review.h"
#include "project.h"

#include <opencv2/opencv.hpp>

#include <condition_variable>
#include <deque>
#include <filesystem>
#include <mutex>
#include <optional>
#include <thread>

namespace orange::gui::spatial_layout {
namespace {

bool is_legacy_top_level_calibration_artifact_root(const std::string& artifact_root_dir)
{
    if (artifact_root_dir.empty()) {
        return false;
    }
    const std::filesystem::path root =
        std::filesystem::path(artifact_root_dir).lexically_normal();
    return root.filename() == "artifacts" &&
           root.parent_path().filename() == "calibrations";
}

void refresh_current_session_review_after_save(SpatialLayoutUiState* ui_state)
{
    if (ui_state == nullptr || ui_state->calibration_session_dir.empty()) {
        return;
    }
    std::string status;
    std::string error;
    if (!load_spatial_calibration_session_review(
            ui_state,
            ui_state->calibration_session_dir,
            &status,
            &error) &&
        !error.empty()) {
        ui_state->session_review_warnings.push_back(
            "Session capture matrix refresh failed after save: " + error);
    }
}

bool reject_legacy_top_level_calibration_artifact_root(
    const std::string& artifact_root_dir,
    std::string* error_out)
{
    if (!is_legacy_top_level_calibration_artifact_root(artifact_root_dir)) {
        return false;
    }
    if (error_out) {
        *error_out =
            "Spatial Layout artifacts must be saved inside a calibration session, "
            "not the legacy top-level calibrations/artifacts folder. Use "
            "calibrations/sessions/<session_id>/artifacts/.";
    }
    return true;
}

} // namespace

struct TopRimObservationSaveResult {
    bool ok = false;
    std::string status;
    std::string error;
};

nlohmann::json make_top_rim_observation_link_json(
    const std::string& artifact_root_dir,
    const orange::calibration::DishTopRimObservationRequest& request,
    const orange::calibration::DishTopRimObservationWriteResult& write_result)
{
    const orange::calibration::DishTopRimObservationArtifactPaths paths =
        orange::calibration::make_dish_top_rim_observation_artifact_paths(
            artifact_root_dir,
            write_result.artifact_id,
            request.storage_relative_artifact_dir);
    const std::filesystem::path aggregate_dir =
        std::filesystem::path(artifact_root_dir) /
        request.arena_context.value("associated_image_set_artifact_id", std::string());

    std::error_code rel_error;
    std::filesystem::path relative_manifest =
        std::filesystem::relative(paths.manifest_path, aggregate_dir, rel_error);
    if (rel_error || relative_manifest.empty()) {
        relative_manifest =
            std::filesystem::path("..") / write_result.artifact_id /
            kSpatialLayoutManifestFilename;
    }
    rel_error.clear();
    std::filesystem::path relative_observation =
        std::filesystem::relative(paths.observation_json_path, aggregate_dir, rel_error);
    if (rel_error || relative_observation.empty()) {
        relative_observation =
            std::filesystem::path("..") / write_result.artifact_id / "observation.json";
    }

    nlohmann::json link = {
        {"artifact_id", write_result.artifact_id},
        {"artifact_schema_id", orange::calibration::kDishTopRimObservationSchemaId},
        {"artifact_schema_version", orange::calibration::kDishTopRimObservationSchemaVersion},
        {"fingerprint", write_result.fingerprint},
        {"relative_manifest_path", relative_manifest.generic_string()},
        {"relative_observation_path", relative_observation.generic_string()},
        {"selection_policy", "latest_saved_for_camera_arena"},
        {"target_plane", "dish_top_rim"},
        {"coordinate_space", "camera_native_pixels"},
        {"camera_serial", request.camera.serial},
        {"accepted_at_utc", request.created_utc}
    };
    if (!request.arena_context.empty()) {
        link["arena_context"] = request.arena_context;
        const std::string arena_id =
            request.arena_context.value("arena_id", std::string());
        const std::string canvas_id =
            request.arena_context.value("canvas_id", std::string());
        if (!arena_id.empty()) {
            link["arena_id"] = arena_id;
        }
        if (!canvas_id.empty()) {
            link["canvas_id"] = canvas_id;
        }
    }
    if (write_result.observation.is_object()) {
        link["accepted_mask"] =
            write_result.observation.value("accepted_mask", nlohmann::json::object());
        link["observed_boundary"] =
            write_result.observation.value("observed_boundary", nlohmann::json::object());
        link["accepted_inner_rim_boundary"] =
            write_result.observation.value(
                "accepted_inner_rim_boundary",
                write_result.observation.value(
                    "accepted_experimental_area_boundary",
                    nlohmann::json::object()));
        link["accepted_experimental_area_boundary"] =
            write_result.observation.value(
                "accepted_experimental_area_boundary",
                nlohmann::json::object());
        link["boundary_interpretation"] =
            write_result.observation.value(
                "boundary_interpretation",
                nlohmann::json::object());
        link["valid_detection_region"] =
            write_result.observation.value(
                "valid_detection_region",
                nlohmann::json::object());
    }
    return link;
}

void append_unique_observation_history_entry(
    nlohmann::json* linked_observations,
    const nlohmann::json& link)
{
    if (linked_observations == nullptr || !linked_observations->is_object()) {
        return;
    }
    if (!linked_observations->contains("top_rim_observation_history") ||
        !(*linked_observations)["top_rim_observation_history"].is_array()) {
        (*linked_observations)["top_rim_observation_history"] =
            nlohmann::json::array();
    }
    const std::string artifact_id = link.value("artifact_id", std::string());
    nlohmann::json history = nlohmann::json::array();
    for (const auto& existing : (*linked_observations)["top_rim_observation_history"]) {
        if (!existing.is_object() ||
            existing.value("artifact_id", std::string()) == artifact_id) {
            continue;
        }
        history.push_back(existing);
    }
    history.push_back(link);
    (*linked_observations)["top_rim_observation_history"] = std::move(history);
}

bool link_top_rim_observation_to_camera_arena_aggregate(
    const TopRimObservationSaveJob& job,
    const orange::calibration::DishTopRimObservationWriteResult& write_result,
    std::string* error_out)
{
    if (!job.request.arena_context.is_object()) {
        return true;
    }
    const std::string associated_image_set_artifact_id =
        job.request.arena_context.value(
            "associated_image_set_artifact_id",
            std::string());
    if (associated_image_set_artifact_id.empty()) {
        return true;
    }

    const std::filesystem::path aggregate_dir =
        std::filesystem::path(job.artifact_root_dir) /
        associated_image_set_artifact_id;
    const std::filesystem::path image_set_path =
        aggregate_dir / "image_set.json";
    const std::filesystem::path manifest_path =
        aggregate_dir / kSpatialLayoutManifestFilename;
    if (!std::filesystem::exists(image_set_path) ||
        !std::filesystem::exists(manifest_path)) {
        return true;
    }

    nlohmann::json image_set;
    if (!read_json_file(image_set_path, &image_set, error_out)) {
        return false;
    }
    if (!image_set.is_object()) {
        if (error_out) {
            *error_out = "Cannot link top-rim observation: aggregate image_set.json is not an object: " +
                         image_set_path.generic_string();
        }
        return false;
    }

    const nlohmann::json link =
        make_top_rim_observation_link_json(
            job.artifact_root_dir,
            job.request,
            write_result);
    if (!image_set.contains("linked_observations") ||
        !image_set["linked_observations"].is_object()) {
        image_set["linked_observations"] = nlohmann::json::object();
    }
    image_set["linked_observations"]["accepted_top_rim_observation"] = link;
    append_unique_observation_history_entry(&image_set["linked_observations"], link);
    image_set["updated_utc"] = job.request.created_utc;
    const std::string image_set_fingerprint = compute_json_fingerprint(image_set);
    if (!write_json_file(image_set_path, image_set, error_out)) {
        return false;
    }

    nlohmann::json manifest;
    if (!read_json_file(manifest_path, &manifest, error_out)) {
        return false;
    }
    if (!manifest.is_object()) {
        if (error_out) {
            *error_out = "Cannot link top-rim observation: aggregate manifest is not an object: " +
                         manifest_path.generic_string();
        }
        return false;
    }
    if (!manifest.contains("linked_observations") ||
        !manifest["linked_observations"].is_object()) {
        manifest["linked_observations"] = nlohmann::json::object();
    }
    manifest["linked_observations"]["accepted_top_rim_observation"] = link;
    append_unique_observation_history_entry(&manifest["linked_observations"], link);
    if (!manifest.contains("summary") || !manifest["summary"].is_object()) {
        manifest["summary"] = nlohmann::json::object();
    }
    manifest["summary"]["accepted_top_rim_observation_artifact_id"] =
        write_result.artifact_id;
    manifest["summary"]["accepted_top_rim_observation_fingerprint"] =
        write_result.fingerprint;
    manifest["summary"]["accepted_top_rim_observation_created_utc"] =
        job.request.created_utc;
    manifest["updated_utc"] = job.request.created_utc;
    if (!manifest.contains("calibration_ref") ||
        !manifest["calibration_ref"].is_object()) {
        manifest["calibration_ref"] = {
            {"artifact_id", associated_image_set_artifact_id},
            {"artifact_schema_id", orange::calibration::kCalibrationImageSetSchemaId},
            {"artifact_schema_version",
             orange::calibration::kCalibrationImageSetSchemaVersion}
        };
    }
    manifest["calibration_ref"]["fingerprint"] = image_set_fingerprint;
    if (!write_json_file(manifest_path, manifest, error_out)) {
        return false;
    }
    if (!update_spatial_calibration_session_index(
            job.session_dir,
            job.artifact_root_dir,
            manifest,
            error_out)) {
        return false;
    }
    return update_calibration_artifact_registry(
        job.artifact_root_dir,
        manifest,
        error_out);
}

TopRimObservationSaveResult run_top_rim_observation_save_job(TopRimObservationSaveJob job)
{
    TopRimObservationSaveResult save_result;
    try {
        if (reject_legacy_top_level_calibration_artifact_root(
                job.artifact_root_dir,
                &save_result.error)) {
            save_result.ok = false;
            return save_result;
        }
        orange::calibration::DishTopRimObservationWriteResult write_result;
        if (!orange::calibration::write_dish_top_rim_observation_artifact(
                job.artifact_root_dir,
                job.request,
                job.source_gray,
                job.hough_params,
                job.accepted_circle,
                &write_result,
                &save_result.error)) {
            save_result.ok = false;
            if (save_result.error.empty()) {
                save_result.error = "Top-rim observation save failed.";
            }
            return save_result;
        }

        const orange::calibration::DishTopRimObservationArtifactPaths paths =
            orange::calibration::make_dish_top_rim_observation_artifact_paths(
                job.artifact_root_dir,
                write_result.artifact_id,
                job.request.storage_relative_artifact_dir);
        if (!update_spatial_calibration_session_index(
                job.session_dir,
                job.artifact_root_dir,
                write_result.manifest,
                &save_result.error)) {
            save_result.ok = false;
            return save_result;
        }
        if (!link_top_rim_observation_to_camera_arena_aggregate(
                job,
                write_result,
                &save_result.error)) {
            save_result.ok = false;
            return save_result;
        }
        save_result.ok = true;
        save_result.status =
            "Saved top-rim observation to " + write_result.artifact_dir +
            ", image-set companion to " + paths.image_set_json_path +
            " and spatial dish-mask runtime export to " +
            paths.spatial_dish_mask_runtime_export_path;
    } catch (const std::exception& ex) {
        save_result.ok = false;
        save_result.error = std::string("Top-rim observation save threw: ") + ex.what();
    } catch (...) {
        save_result.ok = false;
        save_result.error = "Top-rim observation save threw an unknown exception.";
    }
    return save_result;
}

class TopRimObservationSaveWorker {
public:
    TopRimObservationSaveWorker()
        : worker_thread_(&TopRimObservationSaveWorker::thread_main, this)
    {
    }

    ~TopRimObservationSaveWorker()
    {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stop_requested_ = true;
        }
        cv_.notify_all();
        if (worker_thread_.joinable()) {
            worker_thread_.join();
        }
    }

    TopRimObservationSaveWorker(const TopRimObservationSaveWorker&) = delete;
    TopRimObservationSaveWorker& operator=(const TopRimObservationSaveWorker&) = delete;

    bool Submit(TopRimObservationSaveJob job, std::string* error_out)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (stop_requested_) {
            if (error_out) {
                *error_out = "Top-rim save worker is stopping.";
            }
            return false;
        }
        queued_jobs_.push_back(std::move(job));
        cv_.notify_one();
        return true;
    }

    bool IsBusy() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return running_ || !queued_jobs_.empty();
    }

    bool PopCompleted(TopRimObservationSaveResult* result_out)
    {
        if (result_out == nullptr) {
            return false;
        }
        std::lock_guard<std::mutex> lock(mutex_);
        if (completed_results_.empty()) {
            return false;
        }
        *result_out = std::move(completed_results_.front());
        completed_results_.pop_front();
        return true;
    }

private:
    void thread_main()
    {
        for (;;) {
            std::optional<TopRimObservationSaveJob> job;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                cv_.wait(lock, [this] {
                    return stop_requested_ || !queued_jobs_.empty();
                });
                if (stop_requested_ && queued_jobs_.empty()) {
                    return;
                }
                job = std::move(queued_jobs_.front());
                queued_jobs_.pop_front();
                running_ = true;
            }

            TopRimObservationSaveResult result =
                run_top_rim_observation_save_job(std::move(*job));

            {
                std::lock_guard<std::mutex> lock(mutex_);
                running_ = false;
                completed_results_.push_back(std::move(result));
            }
        }
    }

    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::thread worker_thread_;
    bool stop_requested_ = false;
    bool running_ = false;
    std::deque<TopRimObservationSaveJob> queued_jobs_;
    std::deque<TopRimObservationSaveResult> completed_results_;
};

TopRimObservationSaveWorker& top_rim_observation_save_worker()
{
    static TopRimObservationSaveWorker worker;
    return worker;
}

void poll_top_rim_observation_save_worker(SpatialLayoutUiState* ui_state)
{
    if (ui_state == nullptr) {
        return;
    }
    TopRimObservationSaveResult result;
    while (top_rim_observation_save_worker().PopCompleted(&result)) {
        if (result.ok) {
            ui_state->persistence_status = result.status;
            ui_state->persistence_error.clear();
            refresh_current_session_review_after_save(ui_state);
        } else {
            ui_state->persistence_error =
                result.error.empty() ? "Top-rim observation save failed." : result.error;
            ui_state->persistence_status.clear();
        }
    }
}

struct GenericCalibrationImageSetSaveResult {
    bool ok = false;
    std::string status;
    std::string error;
};

nlohmann::json make_generic_calibration_image_set_manifest(
    const orange::calibration::CalibrationImageSetRequest& request,
    const GenericCalibrationImageSetFiles& files,
    const nlohmann::json& image_set,
    const std::string& image_set_fingerprint)
{
    nlohmann::json available_purposes = nlohmann::json::array();
    if (image_set.contains("available_purposes") &&
        image_set["available_purposes"].is_array()) {
        available_purposes = image_set["available_purposes"];
    }
    nlohmann::json available_capture_stages = nlohmann::json::array();
    if (image_set.contains("available_capture_stages") &&
        image_set["available_capture_stages"].is_array()) {
        available_capture_stages = image_set["available_capture_stages"];
    }
    const int image_count =
        image_set.contains("images") && image_set["images"].is_array()
            ? static_cast<int>(image_set["images"].size())
            : 0;
    const std::string latest_source_frame =
        files.source_frame_relative_path.generic_string();
    nlohmann::json manifest = {
        {"schema_id", kCalibrationManifestSchemaId},
        {"schema_version", kCalibrationManifestSchemaVersion},
        {"artifact_id", request.artifact_id},
        {"artifact_schema_id", orange::calibration::kCalibrationImageSetSchemaId},
        {"artifact_schema_version", orange::calibration::kCalibrationImageSetSchemaVersion},
        {"created_utc", image_set.value("created_utc", request.created_utc)},
        {"updated_utc", request.created_utc},
        {"producer", "orange_spatial_layout_ui"},
        {"calibration_ref", {
            {"artifact_id", request.artifact_id},
            {"artifact_schema_id", orange::calibration::kCalibrationImageSetSchemaId},
            {"artifact_schema_version", orange::calibration::kCalibrationImageSetSchemaVersion},
            {"fingerprint", image_set_fingerprint}
        }},
        {"files", {
            {"image_set_json", files.image_set_path.filename().generic_string()},
            {"latest_source_frame", latest_source_frame},
            {"captures_dir", "captures"}
        }},
        {"summary", {
            {"purpose", "camera_arena_calibration_set"},
            {"target_plane", "multiple"},
            {"latest_purpose", request.purpose},
            {"latest_target_plane", request.target_plane},
            {"latest_capture_stage", request.capture_stage},
            {"available_purposes", available_purposes},
            {"available_capture_stages", available_capture_stages},
            {"image_count", image_count},
            {"coordinate_space", request.coordinate_space},
            {"camera_serial", request.camera.serial},
            {"capture_mode", request.capture.capture_mode},
            {"capture_group_id", request.capture.capture_group_id}
        }}
    };
    if (request.physical_target.is_object() && !request.physical_target.empty()) {
        const std::string target_id =
            request.physical_target.value("physical_target_id", request.target_id);
        if (!target_id.empty()) {
            manifest["summary"]["latest_physical_target_id"] = target_id;
        }
        const std::string bundle_dir =
            request.physical_target.value("session_bundle_dir", std::string());
        if (!bundle_dir.empty()) {
            manifest["files"]["physical_target_bundle_dir"] = bundle_dir;
        }
        const std::string target_json =
            request.physical_target.value("session_json_path", std::string());
        if (!target_json.empty()) {
            manifest["files"]["physical_target_json"] = target_json;
        }
    }
    return manifest;
}

nlohmann::json make_generic_calibration_image_set_image_entry(
    const orange::calibration::CalibrationImageSetRequest& request,
    const orange::calibration::CalibrationImageSetImageRef& image)
{
    orange::calibration::CalibrationImageSetRequest single_image_request = request;
    single_image_request.images.clear();
    single_image_request.images.push_back(image);
    const nlohmann::json single_image_set =
        orange::calibration::calibration_image_set_to_json(single_image_request);

    nlohmann::json entry = single_image_set["images"].at(0);
    entry["purpose"] = request.purpose;
    entry["target_plane"] = request.target_plane;
    entry["capture"] = single_image_set.value("capture", nlohmann::json::object());
    const char* metadata_keys[] = {
        "artifact_schema_id",
        "artifact_schema_version",
        "capture_timestamp_utc",
        "capture_stage",
        "workflow_profile_id",
        "fixture_state",
        "homography_role",
        "visibility_domain",
        "plane_z_mm_nominal",
        "plane_z_mm_uncertainty",
        "wet_or_dry",
        "imaging_shelf_installed",
        "dish_installed",
        "dish_id",
        "water_fill_mm",
        "fill_state",
        "open_water_surface_present",
        "water_settled_status",
        "target_method",
        "pattern_type",
        "pattern_domain",
        "matched_parity_group_id",
        "parity_group_id",
        "parity_group_role",
        "reference_only",
        "physical_target_used",
        "projected_pattern_used_as_coordinate_target",
        "plane_id",
        "z_mm_relative_to_projection_surface",
        "target_id",
        "target_design",
        "physical_target_grid_spacing_mm",
        "physical_target_origin_definition",
        "physical_target_x_orientation_marker_definition"
    };
    for (const char* key : metadata_keys) {
        if (single_image_set.contains(key)) {
            entry[key] = single_image_set.at(key);
        }
    }
    if (!request.projected_pattern.empty()) {
        entry["projected_pattern"] = request.projected_pattern;
    }
    if (!request.physical_target.empty()) {
        entry["physical_target"] = request.physical_target;
    }
    if (!request.scale_target.empty()) {
        entry["scale_target"] = request.scale_target;
    }
    if (!request.runtime_role.empty()) {
        entry["runtime_role"] = request.runtime_role;
    }
    if (!request.observations.empty()) {
        entry["observations"] = request.observations;
    }
    if (!request.citrus_projection_snapshot_pre_capture.empty()) {
        entry["citrus_projection_snapshot_pre_capture"] =
            request.citrus_projection_snapshot_pre_capture;
    }
    if (!request.citrus_projection_snapshot_post_capture.empty()) {
        entry["citrus_projection_snapshot_post_capture"] =
            request.citrus_projection_snapshot_post_capture;
    }
    if (!request.citrus_projection_epoch_consistency.empty()) {
        entry["citrus_projection_epoch_consistency"] =
            request.citrus_projection_epoch_consistency;
    }
    if (!request.citrus_calibration_scene_pre_capture.empty()) {
        entry["citrus_calibration_scene_pre_capture"] =
            request.citrus_calibration_scene_pre_capture;
    }
    if (!request.citrus_calibration_scene_post_capture.empty()) {
        entry["citrus_calibration_scene_post_capture"] =
            request.citrus_calibration_scene_post_capture;
    }
    if (!request.citrus_calibration_scene_consistency.empty()) {
        entry["citrus_calibration_scene_consistency"] =
            request.citrus_calibration_scene_consistency;
    }
    if (!request.citrus_calibration_scene_restore_status.empty()) {
        entry["citrus_calibration_scene_restore_status"] =
            request.citrus_calibration_scene_restore_status;
    }
    if (!request.citrus_arena_centering_pre_capture.empty()) {
        entry["citrus_arena_centering_pre_capture"] =
            request.citrus_arena_centering_pre_capture;
    }
    if (!request.citrus_arena_centering_post_capture.empty()) {
        entry["citrus_arena_centering_post_capture"] =
            request.citrus_arena_centering_post_capture;
    }
    if (!request.citrus_arena_centering_consistency.empty()) {
        entry["citrus_arena_centering_consistency"] =
            request.citrus_arena_centering_consistency;
    }
    if (!request.citrus_daily_registration_pre_capture.empty()) {
        entry["citrus_daily_registration_pre_capture"] =
            request.citrus_daily_registration_pre_capture;
    }
    if (!request.citrus_daily_registration_post_capture.empty()) {
        entry["citrus_daily_registration_post_capture"] =
            request.citrus_daily_registration_post_capture;
    }
    if (!request.citrus_daily_registration_consistency.empty()) {
        entry["citrus_daily_registration_consistency"] =
            request.citrus_daily_registration_consistency;
    }
    if (!request.capture_group_membership.empty()) {
        entry["capture_group_membership"] = request.capture_group_membership;
    }
    if (!request.operator_notes.empty()) {
        entry["operator_notes"] = request.operator_notes;
    }
    return entry;
}

nlohmann::json make_empty_aggregate_calibration_image_set(
    const orange::calibration::CalibrationImageSetRequest& request)
{
    orange::calibration::CalibrationImageSetRequest aggregate_request = request;
    aggregate_request.purpose = "camera_arena_calibration_set";
    aggregate_request.target_plane = "multiple";
    aggregate_request.images.clear();
    aggregate_request.projected_pattern = nlohmann::json::object();
    aggregate_request.scale_target = nlohmann::json::object();
    aggregate_request.runtime_role = nlohmann::json::object();
    aggregate_request.observations = nlohmann::json::object();
    aggregate_request.review_artifacts = nlohmann::json::object();
    aggregate_request.capture_stage.clear();
    aggregate_request.workflow_profile_id.clear();
    aggregate_request.fixture_state.clear();
    aggregate_request.homography_role.clear();
    aggregate_request.visibility_domain = nlohmann::json::object();
    aggregate_request.has_plane_z_mm_nominal = false;
    aggregate_request.has_plane_z_mm_uncertainty = false;
    aggregate_request.wet_or_dry.clear();
    aggregate_request.has_imaging_shelf_installed = false;
    aggregate_request.has_dish_installed = false;
    aggregate_request.dish_id.clear();
    aggregate_request.has_water_fill_mm = false;
    aggregate_request.fill_state.clear();
    aggregate_request.has_open_water_surface_present = false;
    aggregate_request.water_settled_status.clear();
    aggregate_request.target_method.clear();
    aggregate_request.pattern_type.clear();
    aggregate_request.pattern_domain.clear();
    aggregate_request.matched_parity_group_id.clear();
    aggregate_request.parity_group_id.clear();
    aggregate_request.parity_group_role.clear();
    aggregate_request.has_reference_only = false;
    aggregate_request.has_physical_target_used = false;
    aggregate_request.has_projected_pattern_used_as_coordinate_target = false;
    aggregate_request.plane_id.clear();
    aggregate_request.has_z_mm_relative_to_projection_surface = false;
    aggregate_request.target_id.clear();
    aggregate_request.target_design.clear();
    aggregate_request.has_physical_target_grid_spacing_mm = false;
    aggregate_request.physical_target_origin_definition.clear();
    aggregate_request.physical_target_x_orientation_marker_definition.clear();
    aggregate_request.physical_target = nlohmann::json::object();
    aggregate_request.citrus_projection_snapshot_pre_capture = nlohmann::json::object();
    aggregate_request.citrus_projection_snapshot_post_capture = nlohmann::json::object();
    aggregate_request.citrus_projection_epoch_consistency = nlohmann::json::object();
    aggregate_request.citrus_calibration_scene_pre_capture = nlohmann::json::object();
    aggregate_request.citrus_calibration_scene_post_capture = nlohmann::json::object();
    aggregate_request.citrus_calibration_scene_consistency = nlohmann::json::object();
    aggregate_request.citrus_calibration_scene_restore_status = nlohmann::json::object();
    aggregate_request.citrus_arena_centering_pre_capture = nlohmann::json::object();
    aggregate_request.citrus_arena_centering_post_capture = nlohmann::json::object();
    aggregate_request.citrus_arena_centering_consistency = nlohmann::json::object();
    aggregate_request.citrus_daily_registration_pre_capture = nlohmann::json::object();
    aggregate_request.citrus_daily_registration_post_capture = nlohmann::json::object();
    aggregate_request.citrus_daily_registration_consistency = nlohmann::json::object();
    aggregate_request.capture_group_membership = nlohmann::json::object();
    aggregate_request.operator_notes.clear();

    nlohmann::json image_set =
        orange::calibration::calibration_image_set_to_json(aggregate_request);
    image_set["images"] = nlohmann::json::array();
    image_set["description"] =
        "Session-scoped camera/arena calibration image set assembled by Orange Spatial Layout.";
    return image_set;
}

void refresh_aggregate_calibration_image_set_summary(nlohmann::json* image_set)
{
    if (image_set == nullptr) {
        return;
    }
    if (!image_set->contains("images") || !(*image_set)["images"].is_array()) {
        (*image_set)["images"] = nlohmann::json::array();
    }

    nlohmann::json available_purposes = nlohmann::json::array();
    nlohmann::json available_capture_stages = nlohmann::json::array();
    for (const auto& image : (*image_set)["images"]) {
        const std::string purpose = image.value("purpose", "");
        if (!purpose.empty()) {
            bool already_present = false;
            for (const auto& existing : available_purposes) {
                if (existing.is_string() && existing.get<std::string>() == purpose) {
                    already_present = true;
                    break;
                }
            }
            if (!already_present) {
                available_purposes.push_back(purpose);
            }
        }
        const std::string capture_stage = image.value("capture_stage", "");
        if (!capture_stage.empty()) {
            bool already_present = false;
            for (const auto& existing : available_capture_stages) {
                if (existing.is_string() && existing.get<std::string>() == capture_stage) {
                    already_present = true;
                    break;
                }
            }
            if (!already_present) {
                available_capture_stages.push_back(capture_stage);
            }
        }
    }

    (*image_set)["purpose"] = "camera_arena_calibration_set";
    (*image_set)["target_plane"] = "multiple";
    (*image_set)["available_purposes"] = available_purposes;
    (*image_set)["available_capture_stages"] = available_capture_stages;
    (*image_set)["image_count"] = (*image_set)["images"].size();
}

GenericCalibrationImageSetSaveResult run_generic_calibration_image_set_save_job(
    GenericCalibrationImageSetSaveJob job)
{
    GenericCalibrationImageSetSaveResult result;
    try {
        if (reject_legacy_top_level_calibration_artifact_root(
                job.artifact_root_dir,
                &result.error)) {
            result.ok = false;
            return result;
        }
        const GenericCalibrationImageSetFiles files =
            make_generic_calibration_image_set_files(
                job.artifact_root_dir,
                job.request.artifact_id,
                job.capture_filename);
        if (!materialize_physical_target_bundle_for_request(
                &job.request,
                files.artifact_dir,
                &result.error)) {
            result.ok = false;
            return result;
        }
        if (!write_image_file(files.source_frame_path, job.source_gray, &result.error)) {
            result.ok = false;
            return result;
        }

        const std::string source_checksum =
            compute_file_fingerprint(files.source_frame_path, &result.error);
        if (source_checksum.empty()) {
            result.ok = false;
            return result;
        }

        job.request.images.clear();
        const orange::calibration::CalibrationImageSetImageRef image_ref{
            job.image_role.empty() ? std::string("source") : job.image_role,
            files.source_frame_relative_path.generic_string(),
            kCalibrationFingerprintAlgorithm,
            source_checksum,
            "camera_native_pixels",
            orange::calibration::CalibrationImageSetShape{
                job.request.camera.image_shape.height,
                job.request.camera.image_shape.width},
            job.image_description};

        nlohmann::json image_set = nlohmann::json::object();
        if (std::filesystem::exists(files.image_set_path)) {
            if (!read_json_file(files.image_set_path, &image_set, &result.error)) {
                result.ok = false;
                return result;
            }
            if (!image_set.is_object()) {
                result.ok = false;
                result.error = "Existing image_set.json is not a JSON object: " +
                               files.image_set_path.generic_string();
                return result;
            }
        } else {
            image_set = make_empty_aggregate_calibration_image_set(job.request);
        }
        if (!image_set.contains("created_utc") ||
            !image_set["created_utc"].is_string() ||
            image_set["created_utc"].get<std::string>().empty()) {
            image_set["created_utc"] = job.request.created_utc;
        }
        image_set["schema_id"] = orange::calibration::kCalibrationImageSetSchemaId;
        image_set["schema_version"] = orange::calibration::kCalibrationImageSetSchemaVersion;
        image_set["artifact_id"] = job.request.artifact_id;
        image_set["coordinate_space"] = job.request.coordinate_space;
        image_set.erase("projected_pattern");
        image_set.erase("scale_target");
        image_set.erase("runtime_role");
        image_set.erase("observations");
        image_set.erase("operator_notes");
        image_set["updated_utc"] = job.request.created_utc;
        image_set["camera"] =
            orange::calibration::calibration_image_set_to_json(job.request).at("camera");
        if (!job.request.rig_context.empty()) {
            image_set["rig_context"] = job.request.rig_context;
        }
        image_set["capture"] =
            orange::calibration::calibration_image_set_to_json(job.request).at("capture");
        image_set["latest_capture"] = {
            {"purpose", job.request.purpose},
            {"target_plane", job.request.target_plane},
            {"capture_stage", job.request.capture_stage},
            {"path", files.source_frame_relative_path.generic_string()},
            {"timestamp_utc", job.request.capture.timestamp_utc}
        };
        if (!job.request.capture.capture_group_id.empty()) {
            image_set["latest_capture"]["capture_group_id"] =
                job.request.capture.capture_group_id;
        }
        if (!image_set.contains("images") || !image_set["images"].is_array()) {
            image_set["images"] = nlohmann::json::array();
        }
        image_set["images"].push_back(
            make_generic_calibration_image_set_image_entry(job.request, image_ref));
        refresh_aggregate_calibration_image_set_summary(&image_set);

        if (!write_json_file(files.image_set_path, image_set, &result.error)) {
            result.ok = false;
            return result;
        }

        const std::string image_set_fingerprint =
            compute_json_fingerprint(image_set);
        const nlohmann::json manifest =
            make_generic_calibration_image_set_manifest(
                job.request,
                files,
                image_set,
                image_set_fingerprint);
        if (!write_json_file(files.manifest_path, manifest, &result.error)) {
            result.ok = false;
            return result;
        }
        if (!update_spatial_calibration_session_index(
                job.session_dir,
                job.artifact_root_dir,
                manifest,
                &result.error)) {
            result.ok = false;
            return result;
        }
        if (!update_calibration_artifact_registry(job.artifact_root_dir, manifest, &result.error)) {
            result.ok = false;
            return result;
        }

        result.ok = true;
        result.status =
            "Saved " + job.request.purpose + " capture to " +
            files.source_frame_path.generic_string() +
            " and updated image_set.json (" +
            std::to_string(image_set.value("image_count", 0)) + " images).";
    } catch (const std::exception& ex) {
        result.ok = false;
        result.error = std::string("Calibration image-set save threw: ") + ex.what();
    } catch (...) {
        result.ok = false;
        result.error = "Calibration image-set save threw an unknown exception.";
    }
    return result;
}

class GenericCalibrationImageSetSaveWorker {
public:
    GenericCalibrationImageSetSaveWorker()
        : worker_thread_(&GenericCalibrationImageSetSaveWorker::thread_main, this)
    {
    }

    ~GenericCalibrationImageSetSaveWorker()
    {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stop_requested_ = true;
        }
        cv_.notify_all();
        if (worker_thread_.joinable()) {
            worker_thread_.join();
        }
    }

    GenericCalibrationImageSetSaveWorker(const GenericCalibrationImageSetSaveWorker&) = delete;
    GenericCalibrationImageSetSaveWorker& operator=(const GenericCalibrationImageSetSaveWorker&) = delete;

    bool Submit(GenericCalibrationImageSetSaveJob job, std::string* error_out)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (stop_requested_) {
            if (error_out) {
                *error_out = "Calibration image-set save worker is stopping.";
            }
            return false;
        }
        queued_jobs_.push_back(std::move(job));
        cv_.notify_one();
        return true;
    }

    bool IsBusy() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return running_ || !queued_jobs_.empty();
    }

    size_t PendingCount() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return queued_jobs_.size();
    }

    bool PopCompleted(GenericCalibrationImageSetSaveResult* result_out)
    {
        if (result_out == nullptr) {
            return false;
        }
        std::lock_guard<std::mutex> lock(mutex_);
        if (completed_results_.empty()) {
            return false;
        }
        *result_out = std::move(completed_results_.front());
        completed_results_.pop_front();
        return true;
    }

private:
    void thread_main()
    {
        for (;;) {
            std::optional<GenericCalibrationImageSetSaveJob> job;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                cv_.wait(lock, [this] {
                    return stop_requested_ || !queued_jobs_.empty();
                });
                if (stop_requested_ && queued_jobs_.empty()) {
                    return;
                }
                job = std::move(queued_jobs_.front());
                queued_jobs_.pop_front();
                running_ = true;
            }

            GenericCalibrationImageSetSaveResult result =
                run_generic_calibration_image_set_save_job(std::move(*job));

            {
                std::lock_guard<std::mutex> lock(mutex_);
                running_ = false;
                completed_results_.push_back(std::move(result));
            }
        }
    }

    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::thread worker_thread_;
    bool stop_requested_ = false;
    bool running_ = false;
    std::deque<GenericCalibrationImageSetSaveJob> queued_jobs_;
    std::deque<GenericCalibrationImageSetSaveResult> completed_results_;
};

GenericCalibrationImageSetSaveWorker& generic_calibration_image_set_save_worker()
{
    static GenericCalibrationImageSetSaveWorker worker;
    return worker;
}

size_t queued_generic_calibration_image_set_save_job_count()
{
    return generic_calibration_image_set_save_worker().PendingCount();
}

bool submit_top_rim_observation_save_job(
    TopRimObservationSaveJob job,
    std::string* error_out)
{
    return top_rim_observation_save_worker().Submit(std::move(job), error_out);
}

bool top_rim_observation_save_worker_is_busy()
{
    return top_rim_observation_save_worker().IsBusy();
}

bool submit_generic_calibration_image_set_save_job(
    GenericCalibrationImageSetSaveJob job,
    std::string* error_out)
{
    return generic_calibration_image_set_save_worker().Submit(std::move(job), error_out);
}

bool generic_calibration_image_set_save_worker_is_busy()
{
    return generic_calibration_image_set_save_worker().IsBusy();
}

void poll_generic_calibration_image_set_save_worker(SpatialLayoutUiState* ui_state)
{
    if (ui_state == nullptr) {
        return;
    }
    GenericCalibrationImageSetSaveResult result;
    while (generic_calibration_image_set_save_worker().PopCompleted(&result)) {
        if (result.ok) {
            ui_state->persistence_status = result.status;
            ui_state->persistence_error.clear();
            refresh_current_session_review_after_save(ui_state);
        } else {
            ui_state->persistence_error =
                result.error.empty() ? "Calibration image-set save failed." : result.error;
            ui_state->persistence_status.clear();
        }
        const size_t queued = queued_generic_calibration_image_set_save_job_count();
        if (queued > 0 && ui_state->persistence_error.empty()) {
            ui_state->persistence_status +=
                " " + std::to_string(queued) + " grouped save job(s) remain queued.";
        }
    }
}
} // namespace orange::gui::spatial_layout
