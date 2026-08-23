#include "spatial_layout_ui.h"

#include "camera_preview_utils.h"
#include "gui/spatial_layout/calibration_metadata.h"
#include "gui/spatial_layout/calibration_transaction_bridge.h"
#include "gui/spatial_layout/calibration_workflow.h"
#include "gui/spatial_layout/canvas_edit.h"
#include "gui/spatial_layout/capture_panel.h"
#include "gui/spatial_layout/citrus_import.h"
#include "gui/spatial_layout/citrus_template_workflow.h"
#include "gui/spatial_layout/commissioning_finalization.h"
#include "gui/spatial_layout/daily_registration_workflow.h"
#include "gui/spatial_layout/daily_physical_registration_preflight.h"
#include "gui/spatial_layout/geometry.h"
#include "gui/spatial_layout/group_capture_controller.h"
#include "gui/spatial_layout/hough_panel.h"
#include "gui/spatial_layout/layout_artifact_persistence.h"
#include "gui/spatial_layout/layout_editor.h"
#include "gui/spatial_layout/layout_state.h"
#include "gui/spatial_layout/linked_top_rim_layout.h"
#include "gui/spatial_layout/metadata_panel.h"
#include "gui/spatial_layout/persistence_panel.h"
#include "gui/spatial_layout/preview_capture.h"
#include "gui/spatial_layout/preview_overlay.h"
#include "gui/spatial_layout/session_io.h"
#include "gui/spatial_layout/projection_snapshot_client.h"
#include "gui/spatial_layout/runtime_builder.h"
#include "gui/spatial_layout/save_job_preparation.h"
#include "gui/spatial_layout/save_jobs.h"
#include "gui/spatial_layout/session_review.h"
#include "gui/spatial_layout/sha256.h"
#include "imgui.h"
#include "misc/cpp/imgui_stdlib.h"
#include <ImGuiFileDialog.h>
#include "spatial_snapshot_worker.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <map>
#include <set>
#include <sstream>
#include <vector>

#include <unistd.h>

namespace {

using orange::spatial::ArenaLayoutArtifact;
using orange::spatial::ArenaLayoutRuntime;
using orange::spatial::CoordinateSpace;
using orange::spatial::DishMaskRuntime;
using orange::spatial::RuntimeGeometry;
using orange::spatial::RuntimeGeometryType;

using orange::gui::spatial_layout::Point2d;
using orange::gui::spatial_layout::PreviewOverlayActions;
using orange::gui::spatial_layout::GenericCalibrationImageSetSaveJob;
using orange::gui::spatial_layout::GroupCapturePanelActions;
using orange::gui::spatial_layout::HoughCirclePanelActions;
using orange::gui::spatial_layout::CalibrationCaptureMetadataPanelActions;
using orange::gui::spatial_layout::CitrusProjectionSnapshotQueryResult;
using orange::gui::spatial_layout::SpatialLayoutPersistencePanelEvent;
using orange::gui::spatial_layout::SpatialLayoutPersistencePanelState;
using orange::gui::spatial_layout::TopRimObservationSaveJob;
using orange::gui::spatial_layout::apply_calibration_image_set_purpose_defaults;
using orange::gui::spatial_layout::apply_view_registration_to_editor_state;
using orange::gui::spatial_layout::apply_full_resolution_stream_snapshot;
using orange::gui::spatial_layout::capture_live_stream_preview_texture;
using orange::gui::spatial_layout::capture_single_camera_frame;
using orange::gui::spatial_layout::clear_citrus_template_import;
using orange::gui::spatial_layout::clear_spatial_calibration_session;
using orange::gui::spatial_layout::clear_detected_experimental_area_circle;
using orange::gui::spatial_layout::clear_captured_citrus_projection_snapshot_metadata;
using orange::gui::spatial_layout::citrus_arena_origin_canvas_px;
using orange::gui::spatial_layout::citrus_arena_relative_to_canvas_px;
using orange::gui::spatial_layout::citrus_template_display_label;
using orange::gui::spatial_layout::citrus_canvas_to_arena_relative_px;
using orange::gui::spatial_layout::default_citrus_rigs_root;
using orange::gui::spatial_layout::ensure_spatial_calibration_session;
using orange::gui::spatial_layout::eligible_group_capture_camera_count;
using orange::gui::spatial_layout::evaluate_daily_physical_registration_save_preflight;
using orange::gui::spatial_layout::advance_daily_registration_workflow;
using orange::gui::spatial_layout::advance_group_capture_workflow;
using orange::gui::spatial_layout::find_citrus_template_index_for_camera;
using orange::gui::spatial_layout::group_capture_workflow_active;
using orange::gui::spatial_layout::initialize_group_capture_camera_scope;
using orange::gui::spatial_layout::initialize_spatial_layout_defaults;
using orange::gui::spatial_layout::kCalibrationManifestSchemaId;
using orange::gui::spatial_layout::kSpatialLayoutArenaLayoutRuntimeFilename;
using orange::gui::spatial_layout::kSpatialLayoutDishMaskRuntimeFilename;
using orange::gui::spatial_layout::kSpatialLayoutMeasurementFilename;
using orange::gui::spatial_layout::load_spatial_calibration_session_review;
using orange::gui::spatial_layout::make_point;
using orange::gui::spatial_layout::generic_calibration_image_set_save_worker_is_busy;
using orange::gui::spatial_layout::handle_registration_canvas_edit;
using orange::gui::spatial_layout::handle_selected_zone_canvas_edit;
using orange::gui::spatial_layout::read_json_file;
using orange::gui::spatial_layout::render_group_capture_panels;
using orange::gui::spatial_layout::render_daily_registration_workflow_panel;
using orange::gui::spatial_layout::render_layout_geometry_editor;
using orange::gui::spatial_layout::render_calibration_workflow_tabs;
using orange::gui::spatial_layout::render_calibration_capture_metadata_panel;
using orange::gui::spatial_layout::render_registration_editor;
using orange::gui::spatial_layout::render_spatial_layout_persistence_panel;
using orange::gui::spatial_layout::render_zone_editor;
using orange::gui::spatial_layout::reset_registration_from_frame;
using orange::gui::spatial_layout::rebuild_schema_preview;
using orange::gui::spatial_layout::request_group_full_resolution_snapshots;
using orange::gui::spatial_layout::resolve_group_capture_scene_recipe;
using orange::gui::spatial_layout::draw_runtime_preview;
using orange::gui::spatial_layout::poll_generic_calibration_image_set_save_worker;
using orange::gui::spatial_layout::poll_top_rim_observation_save_worker;
using orange::gui::spatial_layout::pending_group_snapshot_count;
using orange::gui::spatial_layout::apply_group_capture_to_active_preview;
using orange::gui::spatial_layout::apply_session_review_image_to_active_preview;
using orange::gui::spatial_layout::consume_group_snapshot_result;
using orange::gui::spatial_layout::prepare_dish_top_rim_observation_save_job_from_spatial_layout;
using orange::gui::spatial_layout::prepare_generic_calibration_image_set_save_job_from_spatial_layout;
using orange::gui::spatial_layout::queue_group_calibration_image_set_save_jobs;
using orange::gui::spatial_layout::seed_registration_from_citrus_homography;
using orange::gui::spatial_layout::query_citrus_active_projection_snapshot;
using orange::gui::spatial_layout::set_captured_citrus_projection_snapshots;
using orange::gui::spatial_layout::select_citrus_template_by_index;
using orange::gui::spatial_layout::save_spatial_layout_artifact;
using orange::gui::spatial_layout::save_linked_arena_layout_artifacts;
using orange::gui::spatial_layout::import_citrus_canvas_templates;
using orange::gui::spatial_layout::load_citrus_homography_candidate_set_for_review;
using orange::gui::spatial_layout::promote_citrus_homography_candidates;
using orange::gui::spatial_layout::query_citrus_homography_candidate_status;
using orange::gui::spatial_layout::reject_citrus_homography_candidates;
using orange::gui::spatial_layout::promote_citrus_projected_surface_scale_candidates;
using orange::gui::spatial_layout::query_citrus_projected_surface_scale_candidate_status;
using orange::gui::spatial_layout::load_citrus_projected_surface_scale_candidate_set_for_review;
using orange::gui::spatial_layout::reject_citrus_projected_surface_scale_candidates;
using orange::gui::spatial_layout::finalize_citrus_rig_canvas_commissioning;
using orange::gui::spatial_layout::query_citrus_rig_canvas_commissioning_status;
using orange::gui::spatial_layout::CommissioningControlReply;
using orange::gui::spatial_layout::CommissioningFinalizationDisposition;
using orange::gui::spatial_layout::CommissioningFinalizationRequest;
using orange::gui::spatial_layout::CommissioningFinalizationResult;
using orange::gui::spatial_layout::CommissioningFinalizationWorker;
using orange::gui::spatial_layout::query_citrus_daily_registration_status;
using orange::gui::spatial_layout::select_citrus_daily_registration_runtime_mode;
using orange::gui::spatial_layout::submit_generic_calibration_image_set_save_job;
using orange::gui::spatial_layout::submit_top_rim_observation_save_job;
using orange::gui::spatial_layout::sync_single_experimental_area_zone;
using orange::gui::spatial_layout::top_rim_observation_save_worker_is_busy;
using orange::gui::spatial_layout::transform_point_projective;
using orange::gui::spatial_layout::queued_generic_calibration_image_set_save_job_count;

constexpr const char* kLoadSpatialLayoutDialogId = "LoadSpatialLayoutArtifact";
constexpr const char* kLoadCitrusArenaConfigDialogId = "LoadCitrusArenaConfig";
constexpr const char* kLoadCalibrationSessionDialogId = "LoadCalibrationSession";
constexpr const char* kLoadHomographyCandidateSetDialogId =
    "LoadHomographyCandidateSet";
constexpr const char* kLoadProjectedSurfaceScaleCandidateSetDialogId =
    "LoadProjectedSurfaceScaleCandidateSet";

std::string next_homography_review_operation_id(const std::string& action)
{
    static std::uint64_t sequence = 0;
    const auto ticks = std::chrono::steady_clock::now()
        .time_since_epoch().count();
    return "orange_homography_review_" + action + "_" +
        std::to_string(static_cast<long long>(::getpid())) + "_" +
        std::to_string(static_cast<long long>(ticks)) + "_" +
        std::to_string(++sequence);
}

std::string next_scale_review_operation_id(const std::string& action)
{
    static std::uint64_t sequence = 0;
    const auto ticks = std::chrono::steady_clock::now()
        .time_since_epoch().count();
    return "orange_projected_surface_scale_review_" + action + "_" +
        std::to_string(static_cast<long long>(::getpid())) + "_" +
        std::to_string(static_cast<long long>(ticks)) + "_" +
        std::to_string(++sequence);
}

std::string next_commissioning_operation_id(const std::string& action)
{
    static std::uint64_t sequence = 0;
    const auto ticks = std::chrono::steady_clock::now()
        .time_since_epoch().count();
    return "orange_rig_canvas_commissioning_" + action + "_" +
        std::to_string(static_cast<long long>(::getpid())) + "_" +
        std::to_string(static_cast<long long>(ticks)) + "_" +
        std::to_string(++sequence);
}

bool json_number(const nlohmann::json& object,
                 const char* key,
                 double* value_out)
{
    if (!object.is_object() || value_out == nullptr) return false;
    const auto value = object.find(key);
    if (value == object.end() || !value->is_number()) return false;
    *value_out = value->get<double>();
    return true;
}

nlohmann::json homography_targets_from_manifest(const nlohmann::json& manifest)
{
    nlohmann::json targets = nlohmann::json::array();
    for (const auto& candidate : manifest.value(
             "candidates", nlohmann::json::array())) {
        if (!candidate.is_object()) continue;
        targets.push_back({
            {"arena_id", candidate.value("arena_id", "")},
            {"camera_id", candidate.value("camera_id", "")},
        });
    }
    return targets;
}

struct PersistedScaleReviewBundle {
    bool ok = false;
    std::string error;
    std::filesystem::path candidate_manifest_path;
    nlohmann::json candidate_manifest = nlohmann::json::object();
    std::filesystem::path orange_manifest_path;
    nlohmann::json orange_manifest = nlohmann::json::object();
    nlohmann::json targets = nlohmann::json::array();
};

PersistedScaleReviewBundle load_persisted_scale_review_bundle(
    const std::filesystem::path& selected)
{
    PersistedScaleReviewBundle result;
    auto fail = [&](const std::string& error) {
        result.error = error;
        return result;
    };
    nlohmann::json manifest;
    std::string error;
    if (selected.filename() != "manifest.json" ||
        !read_json_file(selected, &manifest, &error)) {
        return fail(error.empty()
            ? "Choose a Citrus projected-surface scale manifest.json."
            : error);
    }
    const std::filesystem::path set_dir = selected.parent_path();
    const std::string candidate_set_id = manifest.value("candidate_set_id", "");
    if (manifest.value("schema_id", "") !=
            "citrus.calibration.projected_surface_scale_candidate_set" ||
        manifest.value("schema_version", 0) != 1 ||
        manifest.value("status", "") != "ready_for_review" ||
        candidate_set_id.empty() || set_dir.filename() != candidate_set_id ||
        manifest.value("canvas_checksum", "").rfind("sha256:", 0) != 0) {
        return fail("Selected JSON is not a complete ready-for-review Citrus "
                    "projected-surface scale candidate set.");
    }
    if (std::filesystem::is_regular_file(set_dir / "acceptance_receipt.json") ||
        std::filesystem::is_regular_file(set_dir / "rejection_receipt.json")) {
        return fail("This candidate set is already finalized and cannot be "
                    "re-opened for promotion.");
    }

    std::vector<std::filesystem::path> candidate_paths;
    std::error_code ec;
    for (const auto& child : std::filesystem::directory_iterator(set_dir, ec)) {
        if (ec) return fail("Could not enumerate the candidate-set directory.");
        const auto candidate_path = child.path() / "candidate.json";
        if (child.is_directory() &&
            std::filesystem::is_regular_file(candidate_path)) {
            candidate_paths.push_back(candidate_path);
        }
    }
    std::sort(candidate_paths.begin(), candidate_paths.end());
    if (candidate_paths.empty() ||
        candidate_paths.size() != manifest.value("candidate_count", 0U)) {
        return fail("Candidate-set file count does not match its manifest.");
    }

    std::map<std::string, nlohmann::json> expected_observations;
    nlohmann::json target_rows = nlohmann::json::array();
    std::filesystem::path orange_artifacts_root;
    for (const auto& candidate_path : candidate_paths) {
        nlohmann::json candidate;
        if (!read_json_file(candidate_path, &candidate, &error)) return fail(error);
        const std::string arena_id = candidate.value("arena_id", "");
        const std::string camera_id = candidate.value("camera_id", "");
        const auto source = candidate.value(
            "source_observation", nlohmann::json::object());
        const std::filesystem::path observation_path = source.value("path", "");
        const std::string observation_sha256 = source.value("sha256", "");
        const std::string key = arena_id + "\n" + camera_id;
        if (candidate.value("schema_id", "") !=
                "citrus.calibration.projected_surface_scale_candidate" ||
            candidate.value("schema_version", 0) != 1 ||
            candidate.value("status", "") != "ready_for_review" ||
            candidate.value("candidate_set_id", "") != candidate_set_id ||
            arena_id.empty() || camera_id.empty() || observation_path.empty() ||
            observation_sha256.rfind("sha256:", 0) != 0 ||
            !expected_observations.emplace(key, nlohmann::json{
                {"observation_path", observation_path.string()},
                {"observation_sha256", observation_sha256},
            }).second) {
            return fail("One or more persisted scale candidates has invalid "
                        "identity or source-observation provenance.");
        }
        target_rows.push_back({
            {"arena_id", arena_id},
            {"camera_id", camera_id},
            {"candidate_id", candidate.value("candidate_id", "")},
            {"candidate_json_path", candidate_path.string()},
            {"source_observation", source},
        });
        if (orange_artifacts_root.empty()) {
            auto parent = observation_path.parent_path();
            while (!parent.empty() && parent.filename() != "artifacts") {
                parent = parent.parent_path();
            }
            orange_artifacts_root = parent;
        }
    }
    if (orange_artifacts_root.empty() ||
        !std::filesystem::is_directory(orange_artifacts_root)) {
        return fail("Could not locate the Orange calibration-session artifacts "
                    "for this candidate set.");
    }

    std::vector<std::pair<std::filesystem::path, nlohmann::json>> matches;
    for (const auto& child :
         std::filesystem::directory_iterator(orange_artifacts_root, ec)) {
        if (ec) return fail("Could not enumerate Orange calibration artifacts.");
        const auto aggregate_path = child.path() / "manifest.json";
        if (!child.is_directory() ||
            !std::filesystem::is_regular_file(aggregate_path)) continue;
        nlohmann::json aggregate;
        std::string ignored;
        if (!read_json_file(aggregate_path, &aggregate, &ignored) ||
            aggregate.value("schema_id", "") !=
                "orange.calibration.projected_surface_scale_observation_set" ||
            aggregate.value("status", "") != "passed" ||
            aggregate.value("citrus_canvas_sha256", "") !=
                manifest.value("canvas_checksum", "")) continue;
        std::map<std::string, nlohmann::json> aggregate_observations;
        for (const auto& observation : aggregate.value(
                 "observations", nlohmann::json::array())) {
            aggregate_observations[observation.value("arena_id", "") + "\n" +
                observation.value("camera_id", "")] = {
                    {"observation_path",
                     observation.value("observation_path", "")},
                    {"observation_sha256",
                     observation.value("observation_sha256", "")},
                };
        }
        if (aggregate_observations == expected_observations) {
            matches.push_back({aggregate_path, std::move(aggregate)});
        }
    }
    if (matches.size() != 1) {
        return fail(matches.empty()
            ? "No Orange observation-set manifest exactly matches the "
              "candidate sources."
            : "More than one Orange observation-set manifest matches this "
              "candidate set; recovery is ambiguous.");
    }
    manifest["targets"] = target_rows;
    result.ok = true;
    result.candidate_manifest_path = selected;
    result.candidate_manifest = std::move(manifest);
    result.orange_manifest_path = matches.front().first;
    result.orange_manifest = std::move(matches.front().second);
    result.targets = std::move(target_rows);
    return result;
}

template <typename T>
T clamp_index(T value, T count)
{
    if (count <= 0) {
        return 0;
    }
    return std::clamp(value, static_cast<T>(0), static_cast<T>(count - 1));
}

bool resolve_measurement_json_path_from_selection(
    const std::filesystem::path& selected_path,
    std::filesystem::path* measurement_path_out,
    std::string* error_out)
{
    if (measurement_path_out == nullptr) {
        if (error_out) {
            *error_out = "Null measurement-path destination.";
        }
        return false;
    }

    nlohmann::json selected_json;
    if (!read_json_file(selected_path, &selected_json, error_out)) {
        return false;
    }

    const std::string schema_id = selected_json.value("schema_id", "");
    if (schema_id == orange::spatial::kArenaLayoutArtifactSchemaId) {
        *measurement_path_out = selected_path;
        return true;
    }
    if (schema_id != kCalibrationManifestSchemaId) {
        if (error_out) {
            *error_out = "Selected JSON is neither an arena layout artifact nor a calibration manifest.";
        }
        return false;
    }

    std::string measurement_filename = kSpatialLayoutMeasurementFilename;
    if (selected_json.contains("files") &&
        selected_json["files"].is_object() &&
        selected_json["files"].contains("measurement_json") &&
        selected_json["files"]["measurement_json"].is_string()) {
        measurement_filename = selected_json["files"]["measurement_json"].get<std::string>();
    }
    *measurement_path_out = selected_path.parent_path() / measurement_filename;
    return true;
}

bool load_spatial_layout_artifact(
    SpatialLayoutUiState* ui_state,
    const std::filesystem::path& selected_json_path,
    std::string* status_out,
    std::string* error_out)
{
    if (ui_state == nullptr) {
        if (error_out) {
            *error_out = "Spatial layout UI state is null.";
        }
        return false;
    }

    std::filesystem::path measurement_path;
    if (!resolve_measurement_json_path_from_selection(selected_json_path, &measurement_path, error_out)) {
        return false;
    }

    nlohmann::json measurement_json;
    if (!read_json_file(measurement_path, &measurement_json, error_out)) {
        return false;
    }

    ArenaLayoutArtifact artifact;
    if (!orange::spatial::parse_arena_layout_artifact_json(measurement_json, &artifact, error_out)) {
        return false;
    }

    ui_state->layout_artifact = artifact;
    ui_state->selected_zone_index = clamp_index(
        ui_state->selected_zone_index,
        static_cast<int>(ui_state->layout_artifact.layout.zones.size()));
    clear_citrus_template_import(ui_state);
    clear_detected_experimental_area_circle(ui_state);

    bool loaded_runtime_registration = false;
    std::vector<std::string> loaded_parts;
    const std::filesystem::path artifact_dir = measurement_path.parent_path();
    const std::filesystem::path arena_layout_runtime_path =
        artifact_dir / kSpatialLayoutArenaLayoutRuntimeFilename;
    const std::filesystem::path dish_mask_runtime_path =
        artifact_dir / kSpatialLayoutDishMaskRuntimeFilename;

    if (std::filesystem::exists(arena_layout_runtime_path)) {
        nlohmann::json runtime_json;
        if (!read_json_file(arena_layout_runtime_path, &runtime_json, error_out)) {
            return false;
        }
        ArenaLayoutRuntime runtime;
        if (!orange::spatial::parse_arena_layout_runtime_json(runtime_json, &runtime, error_out) ||
            !orange::spatial::validate_arena_layout_runtime_against_artifact(runtime, artifact, error_out)) {
            return false;
        }
        apply_view_registration_to_editor_state(ui_state, runtime.registration);
        loaded_runtime_registration = true;
        loaded_parts.push_back("registration");
    }

    if (std::filesystem::exists(dish_mask_runtime_path)) {
        nlohmann::json dish_mask_json;
        if (!read_json_file(dish_mask_runtime_path, &dish_mask_json, error_out)) {
            return false;
        }
        DishMaskRuntime dish_mask_runtime;
        if (!orange::spatial::parse_dish_mask_runtime_json(dish_mask_json, &dish_mask_runtime, error_out)) {
            return false;
        }
        if (dish_mask_runtime.has_geometry) {
            ui_state->edge_margin_px = std::max(0.0, dish_mask_runtime.geometry.edge_margin_px);
            ui_state->centroid_gate_outset_px =
                std::max(0.0, dish_mask_runtime.geometry.centroid_gate_outset_px);
            ui_state->centroid_gate_outset_authored_mm = false;
            ui_state->centroid_gate_outset_mm_camera_serial.clear();
            loaded_parts.push_back("centroid_gate_offset");
        }
    }

    if (!loaded_runtime_registration) {
        ui_state->registration.layout_coordinate_space = ui_state->layout_artifact.layout.coordinate_space;
        if (ui_state->has_capture) {
            reset_registration_from_frame(ui_state);
        }
    }

    if (status_out) {
        std::ostringstream status;
        status << "Loaded arena layout artifact from " << measurement_path.string();
        if (!loaded_parts.empty()) {
            status << " with ";
            for (size_t idx = 0; idx < loaded_parts.size(); ++idx) {
                if (idx > 0) {
                    status << (idx + 1 == loaded_parts.size() ? " and " : ", ");
                }
                status << loaded_parts[idx];
            }
            status << " sidecar";
            if (loaded_parts.size() > 1) {
                status << "s";
            }
        }
        *status_out = status.str();
    }
    return true;
}

bool use_group_capture_for_fit(
    SpatialLayoutUiState* ui_state,
    SpatialLayoutGroupCaptureFrame* capture,
    const CameraParams* cameras_params,
    int num_cameras,
    std::string* error_out)
{
    if (ui_state == nullptr || capture == nullptr) {
        if (error_out) {
            *error_out = "Grouped capture use-for-fit received null state.";
        }
        return false;
    }
    if (!capture->valid ||
        capture->camera_index < 0 ||
        capture->camera_index >= num_cameras ||
        cameras_params == nullptr) {
        if (error_out) {
            *error_out = "Grouped capture is not valid for the current camera set.";
        }
        return false;
    }

    ui_state->selected_camera = capture->camera_index;
    ui_state->configured_camera_index = capture->camera_index;
    if (!apply_group_capture_to_active_preview(ui_state, *capture, error_out)) {
        return false;
    }
    rebuild_schema_preview(ui_state, &cameras_params[capture->camera_index]);
    return true;
}


} // namespace

void clear_spatial_layout_texture(SpatialLayoutUiState* ui_state)
{
    if (ui_state == nullptr) {
        return;
    }
    orange::preview::clear_texture(
        &ui_state->captured_texture,
        &ui_state->captured_texture_width,
        &ui_state->captured_texture_height);
    clear_detected_experimental_area_circle(ui_state);
    ui_state->captured_rgba.clear();
    ui_state->has_capture = false;
    ui_state->captured_camera_serial.clear();
    ui_state->captured_source_array_role = "images_full";
    ui_state->captured_capture_mode = "single_camera_direct_still";
    ui_state->captured_capture_group_id.clear();
    ui_state->captured_source_frame_count = 1;
    ui_state->captured_first_local_frame_id = 0;
    ui_state->captured_last_local_frame_id = 0;
    ui_state->captured_first_camera_frame_id = 0;
    ui_state->captured_last_camera_frame_id = 0;
    ui_state->pending_full_res_snapshot_request_id = 0;
    ui_state->pending_full_res_snapshot_camera_serial.clear();
    ui_state->pending_full_res_snapshot_target_frame_count = 1;
    ui_state->pending_full_res_snapshot_pre_capture = nlohmann::json::object();
    clear_captured_citrus_projection_snapshot_metadata(ui_state);
    ui_state->captured_canvas_view.fit_requested = true;
    ui_state->captured_canvas_view.last_image_width = 0;
    ui_state->captured_canvas_view.last_image_height = 0;
}

void handle_spatial_layout_persistence_event(
    SpatialLayoutPersistencePanelEvent event,
    SpatialLayoutUiState* ui_state,
    CameraParams& selected_camera,
    CameraParams* cameras_params,
    int num_cameras,
    const std::string& artifact_root_dir)
{
    switch (event) {
    case SpatialLayoutPersistencePanelEvent::None:
        return;
    case SpatialLayoutPersistencePanelEvent::StartNewCalibrationSession:
        clear_spatial_calibration_session(ui_state);
        ui_state->persistence_status = "Next save will start a new calibration session.";
        ui_state->persistence_error.clear();
        return;
    case SpatialLayoutPersistencePanelEvent::SaveTopRimObservation: {
        TopRimObservationSaveJob job;
        std::string error;
        std::string session_artifact_root;
        if (!ensure_spatial_calibration_session(
                ui_state,
                selected_camera,
                artifact_root_dir,
                &session_artifact_root,
                &error) ||
            !prepare_dish_top_rim_observation_save_job_from_spatial_layout(
                ui_state,
                selected_camera,
                session_artifact_root,
                &job,
                &error)) {
            ui_state->persistence_error = error;
            ui_state->persistence_status.clear();
        } else {
            job.session_dir = ui_state->calibration_session_dir;
            if (!submit_top_rim_observation_save_job(std::move(job), &error)) {
                ui_state->persistence_error = error;
                ui_state->persistence_status.clear();
            } else {
                ui_state->persistence_status =
                    "Saving top-rim observation artifact in session " +
                    ui_state->calibration_session_id + "...";
                ui_state->persistence_error.clear();
            }
        }
        return;
    }
    case SpatialLayoutPersistencePanelEvent::SaveCalibrationImageSet: {
        GenericCalibrationImageSetSaveJob job;
        std::string error;
        std::string session_artifact_root;
        if (!ensure_spatial_calibration_session(
                ui_state,
                selected_camera,
                artifact_root_dir,
                &session_artifact_root,
                &error) ||
            !prepare_generic_calibration_image_set_save_job_from_spatial_layout(
                ui_state,
                selected_camera,
                session_artifact_root,
                &job,
                &error)) {
            ui_state->persistence_error = error;
            ui_state->persistence_status.clear();
        } else {
            job.session_dir = ui_state->calibration_session_dir;
            if (!submit_generic_calibration_image_set_save_job(std::move(job), &error)) {
                ui_state->persistence_error = error;
                ui_state->persistence_status.clear();
            } else {
                ui_state->persistence_status =
                    "Saving calibration image-set artifact in session " +
                    ui_state->calibration_session_id + "...";
                ui_state->persistence_error.clear();
            }
        }
        return;
    }
    case SpatialLayoutPersistencePanelEvent::SaveGroupCalibrationImageSets: {
        std::string status;
        std::string error;
        if (!queue_group_calibration_image_set_save_jobs(
                ui_state,
                cameras_params,
                num_cameras,
                selected_camera,
                artifact_root_dir,
                &status,
                &error)) {
            ui_state->persistence_error = error;
            ui_state->persistence_status.clear();
        } else {
            ui_state->persistence_status = status;
            ui_state->persistence_error.clear();
        }
        return;
    }
    case SpatialLayoutPersistencePanelEvent::SaveArenaLayoutArtifact: {
        std::string status;
        std::string error;
        std::string session_artifact_root;
        if (!ensure_spatial_calibration_session(
                ui_state,
                selected_camera,
                artifact_root_dir,
                &session_artifact_root,
                &error) ||
            !save_spatial_layout_artifact(
                ui_state,
                selected_camera,
                session_artifact_root,
                ui_state->calibration_session_dir,
                &status,
                &error)) {
            ui_state->persistence_error = error;
            ui_state->persistence_status.clear();
        } else {
            ui_state->persistence_status = status;
            ui_state->persistence_error.clear();
            rebuild_schema_preview(ui_state, &selected_camera);
        }
        return;
    }
    case SpatialLayoutPersistencePanelEvent::SaveLinkedArenaLayoutArtifacts: {
        std::string status;
        std::string error;
        std::string session_artifact_root;
        if (!ensure_spatial_calibration_session(
                ui_state,
                selected_camera,
                artifact_root_dir,
                &session_artifact_root,
                &error) ||
            !save_linked_arena_layout_artifacts(
                ui_state,
                cameras_params,
                num_cameras,
                session_artifact_root,
                &status,
                &error)) {
            ui_state->persistence_error = error;
            ui_state->persistence_status.clear();
        } else {
            ui_state->persistence_status = status;
            ui_state->persistence_error.clear();
        }
        return;
    }
    case SpatialLayoutPersistencePanelEvent::LoadArenaLayoutArtifact: {
        IGFD::FileDialogConfig config;
        config.path = !ui_state->calibration_session_dir.empty()
                          ? ui_state->calibration_session_dir
                          : (artifact_root_dir.empty() ? "." : artifact_root_dir);
        config.countSelectionMax = 1;
        ImGuiFileDialog::Instance()->OpenDialog(
            kLoadSpatialLayoutDialogId,
            "Choose Arena Layout JSON",
            ".json",
            config);
        return;
    }
    case SpatialLayoutPersistencePanelEvent::LoadCalibrationSession: {
        IGFD::FileDialogConfig config;
        if (!ui_state->calibration_session_dir.empty()) {
            config.path = ui_state->calibration_session_dir;
        } else {
            const std::filesystem::path root(
                artifact_root_dir.empty() ? "." : artifact_root_dir);
            if (root.filename() == "sessions") {
                config.path = root.generic_string();
            } else {
                config.path =
                    (root.filename() == "artifacts" && !root.parent_path().empty())
                        ? (root.parent_path() / "sessions").generic_string()
                        : (root / "sessions").generic_string();
            }
        }
        config.countSelectionMax = 1;
        ImGuiFileDialog::Instance()->OpenDialog(
            kLoadCalibrationSessionDialogId,
            "Choose Calibration Session JSON",
            ".json",
            config);
        return;
    }
    case SpatialLayoutPersistencePanelEvent::LoadSelectedSessionImage: {
        std::string error;
        if (!apply_session_review_image_to_active_preview(
                ui_state,
                cameras_params,
                num_cameras,
                &error)) {
            ui_state->persistence_error = error;
            ui_state->persistence_status.clear();
        } else {
            ui_state->persistence_status =
                "Loaded selected calibration session image into preview.";
            ui_state->persistence_error.clear();
            const int preview_camera_index = ui_state->selected_camera;
            if (cameras_params != nullptr &&
                preview_camera_index >= 0 &&
                preview_camera_index < num_cameras) {
                rebuild_schema_preview(ui_state, &cameras_params[preview_camera_index]);
            }
        }
        return;
    }
    }
}

void render_spatial_layout_window(
    SpatialLayoutUiState* ui_state,
    CameraControl* camera_control,
    CameraEmergent* ecams,
    CameraParams* cameras_params,
    CameraEachSelect* cameras_select,
    int num_cameras,
    bool other_calibration_tool_busy,
    const std::string& artifact_root_dir,
    const GLuint* live_preview_texture_ids,
    const uint64_t* live_preview_uploaded_serials,
    SpatialSnapshotWorker* const* spatial_snapshot_workers)
{
    if (ui_state == nullptr) {
        return;
    }

    initialize_spatial_layout_defaults(ui_state);

    if (!ui_state->show_window) {
        return;
    }

    if (!ImGui::Begin("Spatial Layout / Experimental Area Registration", &ui_state->show_window)) {
        ImGui::End();
        return;
    }

    poll_top_rim_observation_save_worker(ui_state);
    poll_generic_calibration_image_set_save_worker(ui_state);

    if (num_cameras <= 0 ||
        cameras_params == nullptr ||
        cameras_select == nullptr ||
        ecams == nullptr ||
        camera_control == nullptr ||
        !camera_control->open) {
        ImGui::TextDisabled("Open cameras before using spatial layout view registration.");
        ImGui::End();
        return;
    }

    ui_state->selected_camera = std::clamp(ui_state->selected_camera, 0, std::max(0, num_cameras - 1));

    std::vector<std::string> camera_labels_storage;
    std::vector<const char*> camera_labels;
    camera_labels_storage.reserve(num_cameras);
    camera_labels.reserve(num_cameras);
    for (int i = 0; i < num_cameras; ++i) {
        std::ostringstream label;
        label << i << ": " << cameras_params[i].camera_serial;
        camera_labels_storage.push_back(label.str());
    }
    for (const std::string& label : camera_labels_storage) {
        camera_labels.push_back(label.c_str());
    }

    ImGui::Combo("Camera", &ui_state->selected_camera, camera_labels.data(), num_cameras);
    if (ui_state->configured_camera_index != ui_state->selected_camera) {
        ui_state->configured_camera_index = ui_state->selected_camera;
        clear_spatial_layout_texture(ui_state);
        ui_state->preview_status = "Capture a frame to preview the selected camera.";
        ui_state->preview_error.clear();
        clear_detected_experimental_area_circle(ui_state);
    }

    CameraParams& selected_camera = cameras_params[ui_state->selected_camera];
    if (!ui_state->citrus_canvas_templates.empty()) {
        const int matching_citrus_index =
            find_citrus_template_index_for_camera(*ui_state, selected_camera.camera_serial);
        if (matching_citrus_index >= 0 &&
            matching_citrus_index != ui_state->citrus_canvas_template_index) {
            std::string status;
            std::string error;
            if (select_citrus_template_by_index(
                    ui_state,
                    matching_citrus_index,
                    &status,
                    &error)) {
                ui_state->citrus_import_status =
                    status + " Auto-selected from loaded Citrus canvas for selected Orange camera.";
                ui_state->citrus_import_error.clear();
            } else {
                ui_state->citrus_import_error = error;
            }
        }
    }
    SpatialSnapshotWorker* selected_snapshot_worker =
        spatial_snapshot_workers ? spatial_snapshot_workers[ui_state->selected_camera] : nullptr;
    initialize_group_capture_camera_scope(
        ui_state,
        cameras_params,
        cameras_select,
        spatial_snapshot_workers,
        num_cameras);
    advance_group_capture_workflow(
        ui_state,
        cameras_params,
        cameras_select,
        num_cameras,
        spatial_snapshot_workers);
    advance_daily_registration_workflow(
        ui_state,
        cameras_params,
        cameras_select,
        num_cameras,
        spatial_snapshot_workers,
        artifact_root_dir);
    if (spatial_snapshot_workers != nullptr) {
        for (int camera_index = 0; camera_index < num_cameras; ++camera_index) {
            SpatialSnapshotWorker* worker = spatial_snapshot_workers[camera_index];
            if (worker == nullptr) {
                continue;
            }
            SpatialSnapshotResult snapshot_result;
            while (worker->PopCompletedSnapshot(&snapshot_result)) {
                if (consume_group_snapshot_result(
                        ui_state,
                        snapshot_result,
                        cameras_params,
                        num_cameras,
                        ui_state->selected_camera)) {
                    continue;
                }
                const bool is_single_request =
                    ui_state->pending_full_res_snapshot_request_id != 0 &&
                    ui_state->pending_full_res_snapshot_request_id == snapshot_result.request_id &&
                    ui_state->pending_full_res_snapshot_camera_serial == snapshot_result.camera_serial;
                if (!is_single_request) {
                    continue;
                }
                const nlohmann::json pre_snapshot =
                    ui_state->pending_full_res_snapshot_pre_capture;
                const std::string operation_id =
                    ui_state->pending_full_res_snapshot_camera_serial.empty()
                        ? std::string("full_resolution_stream_snapshot")
                        : "full_resolution_stream_snapshot_" +
                              ui_state->pending_full_res_snapshot_camera_serial;
                std::string snapshot_error;
                if (!apply_full_resolution_stream_snapshot(
                        ui_state,
                        snapshot_result,
                        &snapshot_error)) {
                    ui_state->pending_full_res_snapshot_request_id = 0;
                    ui_state->pending_full_res_snapshot_camera_serial.clear();
                    ui_state->pending_full_res_snapshot_target_frame_count = 1;
                    ui_state->pending_full_res_snapshot_pre_capture =
                        nlohmann::json::object();
                    clear_captured_citrus_projection_snapshot_metadata(ui_state);
                    ui_state->preview_error = snapshot_error;
                    ui_state->preview_status = "Full-resolution stream snapshot failed.";
                } else {
                    const CitrusProjectionSnapshotQueryResult post_snapshot =
                        query_citrus_active_projection_snapshot(
                            "post_capture",
                            operation_id);
                    set_captured_citrus_projection_snapshots(
                        ui_state,
                        pre_snapshot,
                        post_snapshot.ok ? post_snapshot.snapshot : nlohmann::json::object());
                    ui_state->pending_full_res_snapshot_pre_capture =
                        nlohmann::json::object();
                }
            }
        }
    }
    advance_group_capture_workflow(
        ui_state,
        cameras_params,
        cameras_select,
        num_cameras,
        spatial_snapshot_workers);
    ImGui::Text("Current settings: focus=%u iris=%u exposure=%u frame_rate=%u gain=%u size=%ux%u",
                selected_camera.focus,
                selected_camera.iris,
                selected_camera.exposure,
                selected_camera.frame_rate,
                selected_camera.gain,
                selected_camera.width,
                selected_camera.height);

    const ImGuiTableFlags layout_table_flags =
        ImGuiTableFlags_Resizable |
        ImGuiTableFlags_BordersInnerV |
        ImGuiTableFlags_SizingStretchProp;
    if (!ImGui::BeginTable("SpatialLayoutPanels", 2, layout_table_flags, ImGui::GetContentRegionAvail())) {
        ImGui::End();
        return;
    }
    ImGui::TableSetupColumn("Controls", ImGuiTableColumnFlags_WidthStretch, 0.46f);
    ImGui::TableSetupColumn("Fit Preview", ImGuiTableColumnFlags_WidthStretch, 0.54f);
    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    ImGui::BeginChild("SpatialLayoutControlsPanel", ImVec2(0.0f, 0.0f), false);
    if (other_calibration_tool_busy) {
        ImGui::BeginDisabled();
    }

    const bool spatial_calibration_transaction_active =
        ui_state->calibration_transaction_lease &&
        ui_state->calibration_transaction_lease->active();
    const bool manual_spatial_capture_controls_available =
        !spatial_calibration_transaction_active ||
        orange::gui::spatial_layout::spatial_calibration_transaction_owned_by(
            *ui_state,
            orange::gui::spatial_layout::kManualCameraPreflightTransactionOwner);
    const bool manual_authority_controls_available =
        !spatial_calibration_transaction_active;
    const bool projected_scale_authority_controls_available =
        manual_authority_controls_available ||
        orange::gui::spatial_layout::spatial_calibration_transaction_owned_by(
            *ui_state,
            orange::gui::spatial_layout::kGuidedCommissioningTransactionOwner);
    const bool can_capture =
        !camera_control->subscribe &&
        !camera_control->record_video &&
        !other_calibration_tool_busy &&
        manual_spatial_capture_controls_available;
    const bool group_capture_pending =
        group_capture_workflow_active(*ui_state);
    const bool can_capture_live_preview =
        camera_control->subscribe &&
        !camera_control->record_video &&
        !other_calibration_tool_busy &&
        manual_spatial_capture_controls_available &&
        !group_capture_pending &&
        cameras_select[ui_state->selected_camera].stream_on &&
        live_preview_texture_ids != nullptr &&
        live_preview_texture_ids[ui_state->selected_camera] != 0 &&
        live_preview_uploaded_serials != nullptr &&
        live_preview_uploaded_serials[ui_state->selected_camera] !=
            std::numeric_limits<uint64_t>::max();
    const bool full_res_request_pending_for_selected =
        ui_state->pending_full_res_snapshot_request_id != 0 &&
        ui_state->pending_full_res_snapshot_camera_serial == selected_camera.camera_serial;
    const int eligible_group_camera_count =
        eligible_group_capture_camera_count(
            cameras_select,
            spatial_snapshot_workers,
            num_cameras);
    const bool can_capture_full_resolution_stream_snapshot =
        camera_control->subscribe &&
        !camera_control->record_video &&
        !other_calibration_tool_busy &&
        manual_spatial_capture_controls_available &&
        cameras_select[ui_state->selected_camera].stream_on &&
        selected_snapshot_worker != nullptr &&
        !full_res_request_pending_for_selected &&
        !group_capture_pending;
    const bool can_capture_group_full_resolution_stream_snapshot =
        camera_control->subscribe &&
        !camera_control->record_video &&
        !other_calibration_tool_busy &&
        manual_spatial_capture_controls_available &&
        eligible_group_camera_count > 0 &&
        !ui_state->group_capture_selected_camera_serials.empty() &&
        ui_state->pending_full_res_snapshot_request_id == 0 &&
        !group_capture_pending;

    if (!can_capture) {
        ImGui::TextDisabled("Direct still capture requires streaming, recording, and other calibration tools to be stopped.");
    }

    if (ImGui::Button("Capture Frame") && can_capture) {
        std::string capture_error;
        if (!capture_single_camera_frame(ui_state, ecams, cameras_params, &capture_error)) {
            ui_state->preview_error = capture_error;
            ui_state->preview_status = "Capture failed.";
        } else {
            reset_registration_from_frame(ui_state);
        }
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(!can_capture_live_preview);
    if (ImGui::Button("Capture Live Stream Snapshot")) {
        std::string capture_error;
        if (!capture_live_stream_preview_texture(
                ui_state,
                selected_camera,
                cameras_select[ui_state->selected_camera],
                live_preview_texture_ids ? live_preview_texture_ids[ui_state->selected_camera] : 0,
                &capture_error)) {
            ui_state->preview_error = capture_error;
            ui_state->preview_status = "Live stream snapshot failed.";
        }
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(!can_capture_full_resolution_stream_snapshot);
    if (ImGui::Button("Capture Full-Resolution Stream Snapshot")) {
        std::string request_error;
        uint64_t request_id = 0;
        std::ostringstream operation_id;
        operation_id << "spatial_layout_full_res_" << selected_camera.camera_serial;
        const CitrusProjectionSnapshotQueryResult pre_snapshot =
            query_citrus_active_projection_snapshot(
                "pre_capture",
                operation_id.str());
        if (!selected_snapshot_worker ||
            !selected_snapshot_worker->RequestSnapshot(
                operation_id.str(),
                &request_id,
                &request_error)) {
            ui_state->preview_error = request_error.empty()
                                          ? "Failed to request full-resolution stream snapshot."
                                          : request_error;
            ui_state->preview_status = "Full-resolution stream snapshot request failed.";
        } else {
            ui_state->pending_full_res_snapshot_request_id = request_id;
            ui_state->pending_full_res_snapshot_camera_serial = selected_camera.camera_serial;
            ui_state->pending_full_res_snapshot_target_frame_count = 1;
            ui_state->pending_full_res_snapshot_pre_capture =
                pre_snapshot.ok ? pre_snapshot.snapshot : nlohmann::json::object();
            clear_captured_citrus_projection_snapshot_metadata(ui_state);
            ui_state->preview_error.clear();
            ui_state->preview_status =
                "Waiting for full-resolution stream snapshot from " +
                selected_camera.camera_serial + ".";
        }
    }
    ImGui::EndDisabled();
    ui_state->calibration_average_frame_count =
        std::clamp(ui_state->calibration_average_frame_count, 2, 256);
    ImGui::SetNextItemWidth(120.0f);
    ImGui::InputInt("Average frames", &ui_state->calibration_average_frame_count, 1, 10);
    ui_state->calibration_average_frame_count =
        std::clamp(ui_state->calibration_average_frame_count, 2, 256);
    ImGui::SameLine();
    ImGui::BeginDisabled(!can_capture_full_resolution_stream_snapshot);
    if (ImGui::Button("Capture Averaged Full-Resolution Snapshot")) {
        std::string request_error;
        uint64_t request_id = 0;
        std::ostringstream operation_id;
        operation_id << "spatial_layout_avg_full_res_"
                     << selected_camera.camera_serial
                     << "_n" << ui_state->calibration_average_frame_count;
        const CitrusProjectionSnapshotQueryResult pre_snapshot =
            query_citrus_active_projection_snapshot(
                "pre_capture",
                operation_id.str());
        if (!selected_snapshot_worker ||
            !selected_snapshot_worker->RequestSnapshot(
                operation_id.str(),
                &request_id,
                &request_error,
                static_cast<uint32_t>(ui_state->calibration_average_frame_count))) {
            ui_state->preview_error = request_error.empty()
                                          ? "Failed to request averaged full-resolution stream snapshot."
                                          : request_error;
            ui_state->preview_status = "Averaged full-resolution stream snapshot request failed.";
        } else {
            ui_state->pending_full_res_snapshot_request_id = request_id;
            ui_state->pending_full_res_snapshot_camera_serial = selected_camera.camera_serial;
            ui_state->pending_full_res_snapshot_target_frame_count =
                static_cast<uint32_t>(ui_state->calibration_average_frame_count);
            ui_state->pending_full_res_snapshot_pre_capture =
                pre_snapshot.ok ? pre_snapshot.snapshot : nlohmann::json::object();
            clear_captured_citrus_projection_snapshot_metadata(ui_state);
            ui_state->preview_error.clear();
            ui_state->preview_status =
                "Waiting for averaged full-resolution stream snapshot from " +
                selected_camera.camera_serial + " (" +
                std::to_string(ui_state->calibration_average_frame_count) + " frames).";
        }
    }
    ImGui::EndDisabled();

    ImGui::SeparatorText("Guided Citrus Group Capture");
    ImGui::BeginDisabled(group_capture_pending);
    static constexpr const char* kGroupSceneRecipes[] = {
        "auto",
        "black_reference",
        "arena_outline",
        "experimental_area_center_and_outline",
        "homography_grid",
        "homography_rings",
        "verification_dots"
    };
    if (ImGui::BeginCombo(
            "Citrus scene recipe",
            ui_state->group_capture_scene_recipe.c_str())) {
        for (const char* recipe : kGroupSceneRecipes) {
            const bool selected = ui_state->group_capture_scene_recipe == recipe;
            if (ImGui::Selectable(recipe, selected)) {
                ui_state->group_capture_scene_recipe = recipe;
            }
            if (selected) {
                ImGui::SetItemDefaultFocus();
            }
        }
        ImGui::EndCombo();
    }
    ImGui::TextDisabled(
        "Resolved scene: %s",
        resolve_group_capture_scene_recipe(*ui_state).c_str());
    ImGui::Text("Expected cameras (intentional group scope):");
    for (int camera_index = 0; camera_index < num_cameras; ++camera_index) {
        const std::string& serial = cameras_params[camera_index].camera_serial;
        bool selected =
            std::find(
                ui_state->group_capture_selected_camera_serials.begin(),
                ui_state->group_capture_selected_camera_serials.end(),
                serial) != ui_state->group_capture_selected_camera_serials.end();
        const std::string label = "Cam" + serial + "##GroupScope" + serial;
        if (ImGui::Checkbox(label.c_str(), &selected)) {
            ui_state->group_capture_camera_scope_initialized = true;
            auto& scope = ui_state->group_capture_selected_camera_serials;
            scope.erase(std::remove(scope.begin(), scope.end(), serial), scope.end());
            if (selected) {
                scope.push_back(serial);
            }
        }
        ImGui::SameLine();
        const bool ready =
            cameras_select != nullptr && spatial_snapshot_workers != nullptr &&
            cameras_select[camera_index].stream_on &&
            spatial_snapshot_workers[camera_index] != nullptr;
        ImGui::TextDisabled("(%s)", ready ? "ready" : "not ready");
    }
    if (ImGui::SmallButton("Use all streaming cameras")) {
        ui_state->group_capture_selected_camera_serials.clear();
        for (int camera_index = 0; camera_index < num_cameras; ++camera_index) {
            if (cameras_select != nullptr && spatial_snapshot_workers != nullptr &&
                cameras_select[camera_index].stream_on &&
                spatial_snapshot_workers[camera_index] != nullptr) {
                ui_state->group_capture_selected_camera_serials.push_back(
                    cameras_params[camera_index].camera_serial);
            }
        }
        ui_state->group_capture_camera_scope_initialized = true;
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("Use selected camera only")) {
        ui_state->group_capture_selected_camera_serials = {
            selected_camera.camera_serial};
        ui_state->group_capture_camera_scope_initialized = true;
    }
    ImGui::EndDisabled();
    ImGui::TextDisabled(
        "Orange asks Citrus to present this scene, waits for one shared display fence, then requests fresh frames from exactly this camera set.");

    if (ui_state->calibration_capture_stage ==
            "projected_surface_holder_installed" &&
        (resolve_group_capture_scene_recipe(*ui_state) == "homography_grid" ||
         resolve_group_capture_scene_recipe(*ui_state) == "homography_rings")) {
        const nlohmann::json intensity_authority =
            ui_state->group_capture_scene_options.value(
                "projector_intensity_commissioning",
                nlohmann::json::object());
        if (intensity_authority.value("status", std::string()) == "validated") {
            ImGui::Text(
                "Projector foreground: %d (validated commissioning report)",
                ui_state->group_capture_scene_options.value(
                    "foreground_gray_u8", -1));
            ImGui::TextDisabled(
                "Report: %s",
                intensity_authority.value("report_path", std::string()).c_str());
        } else {
            ImGui::TextDisabled(
                "Projector foreground will be resolved from the immutable commissioning report when capture starts.");
        }
    }

    const std::string manual_group_parent_owner =
        orange::gui::spatial_layout::spatial_calibration_transaction_owned_by(
            *ui_state,
            orange::gui::spatial_layout::kManualCameraPreflightTransactionOwner)
        ? orange::gui::spatial_layout::kManualCameraPreflightTransactionOwner
        : std::string();

    ImGui::BeginDisabled(!can_capture_group_full_resolution_stream_snapshot);
    if (ImGui::Button("Run Guided Group Capture")) {
        std::string request_error;
        if (!request_group_full_resolution_snapshots(
                ui_state,
                cameras_params,
                cameras_select,
                num_cameras,
                spatial_snapshot_workers,
                1,
                &request_error,
                std::string(),
                std::string(),
                manual_group_parent_owner)) {
            ui_state->group_capture_error = request_error;
            ui_state->group_capture_status = "Grouped full-resolution capture request failed.";
        }
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(!can_capture_group_full_resolution_stream_snapshot);
    if (ImGui::Button("Run Guided Averaged Group Capture")) {
        std::string request_error;
        if (!request_group_full_resolution_snapshots(
                ui_state,
                cameras_params,
                cameras_select,
                num_cameras,
                spatial_snapshot_workers,
                static_cast<uint32_t>(ui_state->calibration_average_frame_count),
                &request_error,
                std::string(),
                std::string(),
                manual_group_parent_owner)) {
            ui_state->group_capture_error = request_error;
            ui_state->group_capture_status = "Grouped averaged capture request failed.";
        }
    }
    ImGui::EndDisabled();
    if (!camera_control->subscribe) {
        ImGui::TextDisabled("Stream snapshots use the active GUI stream, useful for TTL-lit rigs.");
    } else if (!can_capture_live_preview) {
        ImGui::TextDisabled("Wait for the selected camera's live preview texture before taking a live preview snapshot.");
    }
    if (full_res_request_pending_for_selected) {
        ImGui::TextDisabled(
            "Full-resolution snapshot request %llu is collecting %u frame(s).",
            static_cast<unsigned long long>(ui_state->pending_full_res_snapshot_request_id),
            static_cast<unsigned int>(
                std::max<uint32_t>(1u, ui_state->pending_full_res_snapshot_target_frame_count)));
    } else if (camera_control->subscribe && !can_capture_full_resolution_stream_snapshot) {
        ImGui::TextDisabled("Full-resolution stream snapshot worker is not available for the selected camera.");
    }
    if (group_capture_pending) {
        ImGui::TextDisabled(
            "Guided group %s state=%s, pending camera snapshots=%d.",
            ui_state->group_capture_id.c_str(),
            ui_state->group_capture_workflow_state.c_str(),
            pending_group_snapshot_count(*ui_state));
    } else if (camera_control->subscribe && eligible_group_camera_count <= 0) {
        ImGui::TextDisabled("No streaming cameras with spatial snapshot workers are available for grouped capture.");
    }
    if (!ui_state->group_capture_status.empty()) {
        ImGui::TextWrapped("%s", ui_state->group_capture_status.c_str());
    }
    if (!ui_state->group_capture_error.empty()) {
        ImGui::TextColored(
            ImVec4(1.0f, 0.65f, 0.35f, 1.0f),
            "%s",
            ui_state->group_capture_error.c_str());
    }

    const bool citrus_template_matches_selected_camera =
        !ui_state->citrus_template.available ||
        ui_state->citrus_template.source_camera_id.empty() ||
        ui_state->citrus_template.source_camera_id == selected_camera.camera_serial;

    ImGui::SeparatorText("Citrus Canvas Import");
    if (ImGui::Button("Import Citrus Canvas Config...")) {
        IGFD::FileDialogConfig config;
        config.path = default_citrus_rigs_root();
        config.countSelectionMax = 1;
        ImGuiFileDialog::Instance()->OpenDialog(
            kLoadCitrusArenaConfigDialogId,
            "Choose Citrus Canvas Config JSON",
            ".json",
            config);
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(
        !ui_state->citrus_template.available ||
        !ui_state->citrus_template.has_canvas_to_camera_homography ||
        !citrus_template_matches_selected_camera);
    if (ImGui::Button("Seed From Citrus Global Homography")) {
        std::string seed_error;
        if (!seed_registration_from_citrus_homography(ui_state, &seed_error)) {
            ui_state->citrus_import_error = seed_error;
        } else {
            ui_state->citrus_import_error.clear();
            if (ui_state->citrus_import_status.empty()) {
                ui_state->citrus_import_status =
                    "Seeded registration from Citrus global-canvas homography projection.";
            } else {
                ui_state->citrus_import_status += " Applied to registration.";
            }
        }
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Clear Citrus Import State")) {
        clear_citrus_template_import(ui_state);
    }

    if (!ui_state->citrus_canvas_templates.empty()) {
        std::vector<std::string> template_labels_storage;
        std::vector<const char*> template_labels;
        template_labels_storage.reserve(ui_state->citrus_canvas_templates.size());
        template_labels.reserve(ui_state->citrus_canvas_templates.size());
        for (const CitrusSpatialTemplateState& template_state : ui_state->citrus_canvas_templates) {
            template_labels_storage.push_back(citrus_template_display_label(template_state));
        }
        for (const std::string& label : template_labels_storage) {
            template_labels.push_back(label.c_str());
        }
        ui_state->citrus_canvas_template_index =
            clamp_index(
                ui_state->citrus_canvas_template_index,
                static_cast<int>(ui_state->citrus_canvas_templates.size()));
        int selected_template_index = ui_state->citrus_canvas_template_index;
        if (ImGui::Combo(
                "Citrus canvas arena/camera",
                &selected_template_index,
                template_labels.data(),
                static_cast<int>(template_labels.size())) &&
            selected_template_index != ui_state->citrus_canvas_template_index) {
            std::string status;
            std::string error;
            if (!select_citrus_template_by_index(
                    ui_state,
                    selected_template_index,
                    &status,
                    &error)) {
                ui_state->citrus_import_error = error;
            } else {
                ui_state->citrus_import_status = status;
                ui_state->citrus_import_error.clear();
                rebuild_schema_preview(ui_state, &selected_camera);
            }
        }
    }
    if (!ui_state->citrus_canvas_config_path.empty()) {
        ImGui::TextWrapped(
            "Recording metadata canvas: %s",
            ui_state->citrus_canvas_config_path.c_str());
    }

    if (!ui_state->citrus_template.available) {
        if (ui_state->citrus_canvas_config_path.empty()) {
            ImGui::TextDisabled(
                "Select a Citrus canvas for recording metadata. Circular canvases can also seed the legacy spatial preview.");
        } else {
            ImGui::TextDisabled(
                "Canvas selected for recording metadata; its shape is not supported by the legacy single-circle preview adapter.");
        }
    } else {
        ImGui::TextWrapped(
            "Imported: rig=%s canvas=%s arena=%s config=%s camera=%s",
            ui_state->citrus_template.source_rig_name.c_str(),
            ui_state->citrus_template.source_canvas_name.c_str(),
            ui_state->citrus_template.source_arena_name.c_str(),
            ui_state->citrus_template.source_config_name.c_str(),
            ui_state->citrus_template.source_camera_id.c_str());
        if (ui_state->citrus_template.has_authoritative_camera_to_canvas_homography) {
            ImGui::TextColored(
                ImVec4(0.35f, 0.95f, 0.45f, 1.0f),
                "Homography authority: accepted and compatible (%s)",
                ui_state->citrus_template.homography_candidate_set_id.c_str());
            ImGui::TextDisabled(
                "accepted=%s plane=%s direction=%s",
                ui_state->citrus_template.homography_accepted_at_utc.c_str(),
                ui_state->citrus_template.homography_target_plane.c_str(),
                ui_state->citrus_template.homography_direction.c_str());
            if (!ui_state->citrus_template.homography_canvas_compatibility_basis.empty()) {
                ImGui::TextDisabled(
                    "canvas compatibility: %s%s%s",
                    ui_state->citrus_template.homography_canvas_compatibility_basis.c_str(),
                    ui_state->citrus_template.homography_commissioning_release_id.empty()
                        ? "" : " release=",
                    ui_state->citrus_template.homography_commissioning_release_id.c_str());
            }
            if (!ui_state->citrus_template.homography_canvas_compatibility_warning.empty()) {
                ImGui::TextColored(
                    ImVec4(1.0f, 0.80f, 0.25f, 1.0f),
                    "Compatibility warning: %s",
                    ui_state->citrus_template.homography_canvas_compatibility_warning.c_str());
            }
            if (ui_state->citrus_template.has_homography_quality) {
                ImGui::TextDisabled(
                    "fit RMS/max %.4f/%.4f canvas px; holdout RMS/max %.4f/%.4f canvas px",
                    ui_state->citrus_template.homography_rms_reprojection_error_canvas_px,
                    ui_state->citrus_template.homography_maximum_reprojection_error_canvas_px,
                    ui_state->citrus_template.homography_holdout_rms_error_canvas_px,
                    ui_state->citrus_template.homography_holdout_maximum_error_canvas_px);
            }
            ImGui::TextDisabled(
                "active pointer: %s",
                ui_state->citrus_template.homography_active_pointer_path.c_str());
        } else {
            ImGui::TextColored(
                ImVec4(1.0f, 0.65f, 0.30f, 1.0f),
                "Homography authority: %s",
                ui_state->citrus_template.homography_authority_status.c_str());
            if (!ui_state->citrus_template.homography_import_error.empty()) {
                ImGui::TextWrapped(
                    "%s",
                    ui_state->citrus_template.homography_import_error.c_str());
            }
        }
        ImGui::TextWrapped(
            "Citrus experimental area: arena-relative center=(%.2f, %.2f) r=%.2f canvas px",
            ui_state->citrus_template.experimental_area_center_x_px,
            ui_state->citrus_template.experimental_area_center_y_px,
            ui_state->citrus_template.experimental_area_radius_px);
        if (ui_state->citrus_template.has_radius_mm) {
            ImGui::SameLine();
            ImGui::TextDisabled(
                "(%.4f mm)",
                ui_state->citrus_template.experimental_area_radius_mm);
        }
        if (ui_state->citrus_template.has_calibration_ring_outer_radius_px) {
            ImGui::TextWrapped(
                "Citrus calibration fit-ring outer dot-center radius: %.2f canvas px%s",
                ui_state->citrus_template.calibration_ring_outer_radius_px,
                ui_state->citrus_template.calibration_pattern_mode.empty()
                    ? ""
                    : " (separate from the experimental-area radius)");
            if (ui_state->citrus_template.has_pixels_per_mm_projector &&
                ui_state->citrus_template.pixels_per_mm_projector > 0.0) {
                const double fit_ring_radius_mm =
                    ui_state->citrus_template.calibration_ring_outer_radius_px /
                    ui_state->citrus_template.pixels_per_mm_projector;
                const double inset_canvas_px =
                    ui_state->citrus_template.experimental_area_radius_px -
                    ui_state->citrus_template.calibration_ring_outer_radius_px;
                ImGui::TextDisabled(
                    "Fit ring: %.4f mm radius; %.2f canvas px / %.4f mm inside configured experimental area",
                    fit_ring_radius_mm,
                    inset_canvas_px,
                    inset_canvas_px /
                        ui_state->citrus_template.pixels_per_mm_projector);
            }
        }
        if (ui_state->citrus_template.has_arena_canvas_region) {
            const Point2d origin = citrus_arena_origin_canvas_px(ui_state->citrus_template);
            const Point2d global_center = citrus_arena_relative_to_canvas_px(
                ui_state->citrus_template,
                make_point(
                    ui_state->citrus_template.experimental_area_center_x_px,
                    ui_state->citrus_template.experimental_area_center_y_px));
            ImGui::TextWrapped(
                "Citrus arena canvas region: center=(%.2f, %.2f) size=(%.2f, %.2f) origin=(%.2f, %.2f)",
                ui_state->citrus_template.arena_center_x_px,
                ui_state->citrus_template.arena_center_y_px,
                ui_state->citrus_template.arena_width_px,
                ui_state->citrus_template.arena_height_px,
                origin.x,
                origin.y);
            ImGui::TextWrapped(
                "Citrus current experimental center in global canvas: (%.2f, %.2f)",
                global_center.x,
                global_center.y);
            if (ui_state->has_citrus_projected_circle &&
                ui_state->citrus_projected_circle_geometry.type == RuntimeGeometryType::kCircle) {
                ImGui::TextWrapped(
                    "Citrus current experimental outline circle-fit diagnostic in camera px: center=(%.2f, %.2f), r=%.2f",
                    ui_state->citrus_projected_circle_geometry.circle.cx,
                    ui_state->citrus_projected_circle_geometry.circle.cy,
                    ui_state->citrus_projected_circle_geometry.circle.r);
                ImGui::TextWrapped(
                    "Citrus projected outline samples: %zu",
                    ui_state->citrus_projected_outline_camera_points.size());
                if (ui_state->has_citrus_projected_fit_ring &&
                    ui_state->citrus_projected_fit_ring_geometry.type ==
                        RuntimeGeometryType::kCircle) {
                    ImGui::TextWrapped(
                        "Citrus fit ring through the same homography: center=(%.2f, %.2f), r=%.2f camera px",
                        ui_state->citrus_projected_fit_ring_geometry.circle.cx,
                        ui_state->citrus_projected_fit_ring_geometry.circle.cy,
                        ui_state->citrus_projected_fit_ring_geometry.circle.r);
                }
                if (ui_state->has_detected_experimental_area_circle &&
                    ui_state->detected_experimental_area_geometry.type ==
                        RuntimeGeometryType::kCircle) {
                    const double radius_delta_px =
                        ui_state->detected_experimental_area_geometry.circle.r -
                        ui_state->citrus_projected_circle_geometry.circle.r;
                    ImGui::TextWrapped(
                        "Current Hough/fit rim minus Citrus experimental-area projection: %.2f camera px (%.2f%% of rim radius)",
                        radius_delta_px,
                        100.0 * radius_delta_px /
                            std::max(
                                1e-9,
                                ui_state->detected_experimental_area_geometry.circle.r));
                }
            }
        } else {
            ImGui::TextColored(
                ImVec4(1.0f, 0.75f, 0.35f, 1.0f),
                "Citrus arena canvas region fields are missing; arena-relative points cannot be mapped to global canvas correctly.");
        }
        if (ui_state->citrus_template.has_radius_mm) {
            ImGui::Text("Radius: %.3f mm", ui_state->citrus_template.experimental_area_radius_mm);
        }
        if (ui_state->citrus_template.has_pixels_per_mm_projector) {
            ImGui::Text("Projector scale: %.4f px/mm", ui_state->citrus_template.pixels_per_mm_projector);
        }
        ImGui::TextDisabled(
            "%s",
            ui_state->citrus_template.has_canvas_to_camera_homography
                ? "Canvas-to-camera homography loaded."
                : "No canvas-to-camera homography sidecar found.");
        if (!citrus_template_matches_selected_camera) {
            ImGui::TextColored(
                ImVec4(1.0f, 0.75f, 0.35f, 1.0f),
                "Imported config targets camera %s, but the selected Orange camera is %s.",
                ui_state->citrus_template.source_camera_id.c_str(),
                selected_camera.camera_serial.c_str());
        }
        if (ui_state->has_detected_experimental_area_circle &&
            ui_state->detected_experimental_area_geometry.type == RuntimeGeometryType::kCircle &&
            ui_state->citrus_template.has_camera_to_canvas_homography &&
            ui_state->citrus_template.has_arena_canvas_region) {
            const Point2d detected_camera_center = make_point(
                ui_state->detected_experimental_area_geometry.circle.cx,
                ui_state->detected_experimental_area_geometry.circle.cy);
            Point2d detected_canvas_center{};
            if (transform_point_projective(
                    ui_state->citrus_template.camera_to_canvas_homography,
                    detected_camera_center,
                    &detected_canvas_center)) {
                const Point2d detected_arena_relative =
                    citrus_canvas_to_arena_relative_px(
                        ui_state->citrus_template,
                        detected_canvas_center);
                const double delta_x =
                    detected_arena_relative.x -
                    ui_state->citrus_template.experimental_area_center_x_px;
                const double delta_y =
                    detected_arena_relative.y -
                    ui_state->citrus_template.experimental_area_center_y_px;
                ImGui::TextWrapped(
                    "Detected water-side inner-rim center maps to Citrus global canvas=(%.2f, %.2f), arena-relative=(%.2f, %.2f), delta=(%+.2f, %+.2f) px.",
                    detected_canvas_center.x,
                    detected_canvas_center.y,
                    detected_arena_relative.x,
                    detected_arena_relative.y,
                    delta_x,
                    delta_y);
            }
        }
    }

    ImGui::SeparatorText("Homography Candidate Review And Promotion");
    ImGui::TextWrapped(
        "Promotion is a separate, explicitly armed step. Citrus revalidates the "
        "persisted evidence against the currently loaded canvas before it can "
        "replace the active homographies.");
    ImGui::BeginDisabled(!manual_authority_controls_available);
    if (ImGui::Button("Choose Persisted Candidate Set...")) {
        IGFD::FileDialogConfig config;
        config.path = "/home/jeremy/citrus/targets/rigs";
        config.countSelectionMax = 1;
        ImGuiFileDialog::Instance()->OpenDialog(
            kLoadHomographyCandidateSetDialogId,
            "Choose candidate_set.json",
            ".json",
            config);
    }
    ImGui::EndDisabled();
    if (!ui_state->homography_candidate_review_manifest_path.empty()) {
        ImGui::TextDisabled(
            "%s",
            ui_state->homography_candidate_review_manifest_path.c_str());
    }
    const bool has_review_manifest =
        ui_state->homography_candidate_review_manifest.is_object() &&
        ui_state->homography_candidate_review_manifest.value(
            "schema_id", "") == "citrus.homography_candidate.status" &&
        ui_state->homography_candidate_review_manifest.value(
            "state", "") == "ready_for_review";
    ImGui::BeginDisabled(
        !has_review_manifest || !manual_authority_controls_available);
    if (ImGui::Button("Revalidate In Citrus")) {
        const auto& manifest = ui_state->homography_candidate_review_manifest;
        const auto result = load_citrus_homography_candidate_set_for_review(
            std::filesystem::path(
                ui_state->homography_candidate_review_manifest_path)
                .parent_path().string(),
            manifest.value("candidate_set_id", ""),
            manifest.value("canvas_checksum", ""),
            homography_targets_from_manifest(manifest),
            next_homography_review_operation_id("load"));
        ui_state->homography_candidate_review_status = result.candidate;
        ui_state->homography_candidate_review_revalidated = false;
        ui_state->accept_reviewed_homographies_armed = false;
        if (!result.ok) {
            ui_state->homography_candidate_review_error = result.reason;
            ui_state->homography_candidate_review_message.clear();
        } else {
            ui_state->homography_candidate_review_error.clear();
            ui_state->homography_candidate_review_message =
                "Citrus accepted the revalidation request. Refresh after it has "
                "processed the candidate set.";
        }
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(!has_review_manifest);
    if (ImGui::Button("Refresh Review Status")) {
        const std::string transaction_id =
            ui_state->homography_candidate_review_manifest.value(
                "transaction_id", "");
        const auto result = query_citrus_homography_candidate_status(
            transaction_id, "operator-review-refresh");
        if (!result.ok) {
            ui_state->homography_candidate_review_error = result.reason;
            ui_state->homography_candidate_review_revalidated = false;
        } else {
            ui_state->homography_candidate_review_status = result.candidate;
            const bool committed =
                result.candidate.value("state", "") == "committed" &&
                result.candidate.value("receipt", nlohmann::json::object()).value(
                    "outcome", "") == "committed";
            const auto revalidation = result.candidate.value(
                "revalidation", nlohmann::json::object());
            ui_state->homography_candidate_review_revalidated =
                result.candidate.value("active", false) &&
                result.candidate.value("state", "") == "ready_for_review" &&
                result.candidate.value("loaded_from_persisted_candidate_set", false) &&
                result.candidate.value("transaction_id", "") == transaction_id &&
                result.candidate.value("candidate_set_id", "") ==
                    ui_state->homography_candidate_review_manifest.value(
                        "candidate_set_id", "") &&
                revalidation.value("status", "") == "passed";
            ui_state->homography_candidate_review_error.clear();
            if (committed) {
                ui_state->accept_reviewed_homographies_armed = false;
                const std::string canvas_path =
                    ui_state->homography_candidate_review_manifest.value(
                        "canvas_path", "");
                std::string import_status;
                std::string import_error;
                if (canvas_path.empty() ||
                    !import_citrus_canvas_templates(
                        ui_state,
                        selected_camera,
                        canvas_path,
                        &import_status,
                        &import_error)) {
                    ui_state->homography_candidate_review_error =
                        "Promotion committed, but Orange could not reload the "
                        "authoritative canvas artifacts: " + import_error;
                } else if (!ui_state->citrus_template
                                .has_authoritative_camera_to_canvas_homography) {
                    ui_state->homography_candidate_review_error =
                        "Promotion committed, but the selected Orange camera did "
                        "not load an accepted compatible active pointer: " +
                        ui_state->citrus_template.homography_import_error;
                } else {
                    ui_state->citrus_import_status =
                        "Loaded newly promoted authoritative homographies. " +
                        import_status;
                    ui_state->citrus_import_error.clear();
                    rebuild_schema_preview(ui_state, &selected_camera);
                }
                ui_state->homography_candidate_review_message =
                    "Citrus committed the candidate set as the current canvas "
                    "authority; Orange reloaded its accepted active pointers.";
            } else if (!ui_state->homography_candidate_review_revalidated) {
                const auto error_it = result.candidate.find("error");
                ui_state->homography_candidate_review_error =
                    error_it != result.candidate.end() && error_it->is_string()
                        ? error_it->get<std::string>()
                        : "Citrus has not reported a passed persisted-set revalidation.";
            }
            if (!committed) {
                ui_state->homography_candidate_review_message =
                    ui_state->homography_candidate_review_revalidated
                        ? "Persisted set revalidated. Review every camera before arming promotion."
                        : "Citrus review status refreshed.";
            }
        }
    }
    ImGui::EndDisabled();

    const nlohmann::json review_rows =
        ui_state->homography_candidate_review_status.value(
            "candidates",
            ui_state->homography_candidate_review_manifest.value(
                "candidates", nlohmann::json::array()));
    if (review_rows.is_array() && !review_rows.empty() &&
        ImGui::BeginTable("homography_candidate_review_table", 6,
                          ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                              ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("Arena / Camera");
        ImGui::TableSetupColumn("State");
        ImGui::TableSetupColumn("Fit RMS / max");
        ImGui::TableSetupColumn("Holdout RMS / max");
        ImGui::TableSetupColumn("Photometry");
        ImGui::TableSetupColumn("Evidence");
        ImGui::TableHeadersRow();
        for (const auto& candidate : review_rows) {
            const auto quality = candidate.value(
                "quality", nlohmann::json::object());
            const auto full = quality.value("full_fit", nlohmann::json::object());
            const auto holdout = quality.value("holdout", nlohmann::json::object());
            const auto photometry = candidate.value(
                "source_photometry", nlohmann::json::object());
            const auto metrics = photometry.value(
                "metrics", nlohmann::json::object());
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::Text("%s / %s",
                        candidate.value("arena_id", "").c_str(),
                        candidate.value("camera_id", "").c_str());
            ImGui::TableSetColumnIndex(1);
            ImGui::TextUnformatted(candidate.value("status", "unknown").c_str());
            ImGui::TableSetColumnIndex(2);
            ImGui::Text("%.4f / %.4f",
                        full.value("rms_reprojection_error_canvas_px", 0.0),
                        full.value("maximum_reprojection_error_canvas_px", 0.0));
            ImGui::TableSetColumnIndex(3);
            ImGui::Text("%.4f / %.4f",
                        holdout.value("rms_error_canvas_px", 0.0),
                        holdout.value("maximum_error_canvas_px", 0.0));
            ImGui::TableSetColumnIndex(4);
            ImGui::Text("contrast %.1f, sat %.5f",
                        metrics.value("dot_background_contrast_u8", 0.0),
                        metrics.value(
                            "dot_core_saturation_fraction_ge_threshold", 0.0));
            ImGui::TableSetColumnIndex(5);
            const std::filesystem::path candidate_dir =
                std::filesystem::path(
                    ui_state->homography_candidate_review_manifest_path)
                    .parent_path() /
                (candidate.value("arena_id", "") + "_" +
                 candidate.value("camera_id", ""));
            ImGui::TextWrapped("%s", candidate_dir.string().c_str());
        }
        ImGui::EndTable();
    }

    ImGui::BeginDisabled(
        !ui_state->homography_candidate_review_revalidated ||
        !manual_authority_controls_available);
    ImGui::Checkbox(
        "I reviewed all detection, reprojection, coordinate-frame, and canvas-layout evidence",
        &ui_state->accept_reviewed_homographies_armed);
    ImGui::BeginDisabled(!ui_state->accept_reviewed_homographies_armed);
    if (ImGui::Button("Promote Reviewed Set As Canvas Authority")) {
        const auto& manifest = ui_state->homography_candidate_review_manifest;
        const nlohmann::json verification = {
            {"schema_id", "orange.homography_candidate.operator_review"},
            {"schema_version", 1},
            {"status", "passed"},
            {"candidate_set_id", manifest.value("candidate_set_id", "")},
            {"candidate_set_manifest_path",
             ui_state->homography_candidate_review_manifest_path},
            {"citrus_revalidation",
             ui_state->homography_candidate_review_status.value(
                 "revalidation", nlohmann::json::object())},
            {"operator_assertions", {
                {"all_camera_detection_overlays_reviewed", true},
                {"all_camera_reprojection_overlays_reviewed", true},
                {"coordinate_frame_evidence_reviewed", true},
                {"logical_canvas_layout_evidence_reviewed", true},
            }},
        };
        const auto result = promote_citrus_homography_candidates(
            manifest.value("transaction_id", ""),
            manifest.value("canvas_checksum", ""),
            verification,
            true,
            next_homography_review_operation_id("promote"));
        ui_state->accept_reviewed_homographies_armed = false;
        if (!result.ok) {
            ui_state->homography_candidate_review_error = result.reason;
            ui_state->homography_candidate_review_message.clear();
        } else {
            ui_state->homography_candidate_review_error.clear();
            ui_state->homography_candidate_review_message =
                "Promotion request accepted. Refresh the review status, then "
                "re-import the Citrus canvas to load the new authoritative artifacts.";
        }
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Release Without Promotion")) {
        const auto result = reject_citrus_homography_candidates(
            ui_state->homography_candidate_review_manifest.value(
                "transaction_id", ""),
            "operator_released_persisted_review_without_promotion",
            next_homography_review_operation_id("release"));
        ui_state->accept_reviewed_homographies_armed = false;
        ui_state->homography_candidate_review_revalidated = false;
        if (!result.ok) {
            ui_state->homography_candidate_review_error = result.reason;
        } else {
            ui_state->homography_candidate_review_error.clear();
            ui_state->homography_candidate_review_message =
                "Review transaction released; active homographies were unchanged.";
        }
    }
    ImGui::EndDisabled();
    if (!manual_authority_controls_available) {
        ImGui::TextDisabled(
            "Homography review and promotion are disabled until the active calibration transaction resolves.");
    }
    if (!ui_state->homography_candidate_review_message.empty()) {
        ImGui::TextWrapped(
            "%s", ui_state->homography_candidate_review_message.c_str());
    }
    if (!ui_state->homography_candidate_review_error.empty()) {
        ImGui::TextColored(
            ImVec4(1.0f, 0.55f, 0.30f, 1.0f),
            "%s", ui_state->homography_candidate_review_error.c_str());
    }

    ImGui::SeparatorText("Projected-Surface Physical Scale Review And Promotion");
    ImGui::TextWrapped(
        "Orange detects the 5 mm physical target and writes overlays; Citrus "
        "independently re-fits the correspondences and owns runtime activation. "
        "The 25 mm C-to-XPLUS span and 77 mm outer diameter are validation-only.");
    ImGui::BeginDisabled(!manual_authority_controls_available);
    if (ImGui::Button("Choose Persisted Physical-Scale Candidate Set...")) {
        IGFD::FileDialogConfig config;
        config.path = "/home/jeremy/citrus/targets/rigs";
        config.countSelectionMax = 1;
        ImGuiFileDialog::Instance()->OpenDialog(
            kLoadProjectedSurfaceScaleCandidateSetDialogId,
            "Choose projected-surface scale manifest.json",
            ".json",
            config);
    }
    ImGui::EndDisabled();
    if (!ui_state->projected_surface_scale_candidate_manifest_path.empty()) {
        ImGui::TextDisabled(
            "Citrus candidates: %s",
            ui_state->projected_surface_scale_candidate_manifest_path.c_str());
        ImGui::TextDisabled(
            "Orange QC: %s",
            ui_state->projected_surface_scale_review_manifest_path.c_str());
        ImGui::BeginDisabled(!manual_authority_controls_available);
        if (ImGui::Button("Revalidate Persisted Physical Scales In Citrus")) {
            const auto& manifest =
                ui_state->projected_surface_scale_candidate_manifest;
            const auto result =
                load_citrus_projected_surface_scale_candidate_set_for_review(
                    std::filesystem::path(
                        ui_state->projected_surface_scale_candidate_manifest_path)
                        .parent_path().string(),
                    manifest.value("candidate_set_id", ""),
                    manifest.value("canvas_checksum", ""),
                    manifest.value("targets", nlohmann::json::array()),
                    next_scale_review_operation_id("load"));
            ui_state->projected_surface_scale_review_status = result.candidate;
            ui_state->projected_surface_scale_review_revalidated = false;
            ui_state->accept_reviewed_projected_surface_scales_armed = false;
            if (!result.ok) {
                ui_state->projected_surface_scale_review_error = result.reason;
                ui_state->projected_surface_scale_review_message.clear();
            } else {
                ui_state->projected_surface_scale_review_error.clear();
                ui_state->projected_surface_scale_review_message =
                    "Citrus accepted the persisted-scale revalidation request. "
                    "Refresh after its main thread processes the candidate set.";
            }
        }
        ImGui::EndDisabled();
    }
    if (!ui_state->projected_surface_scale_review_manifest_path.empty()) {
        ImGui::TextDisabled(
            "%s", ui_state->projected_surface_scale_review_manifest_path.c_str());
    }
    const auto scale_targets =
        ui_state->projected_surface_scale_review_verification.value(
            "targets", nlohmann::json::array());
    if (scale_targets.is_array() && !scale_targets.empty() &&
        ImGui::BeginTable("projected_surface_scale_review_table", 6,
                          ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                              ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("Arena / Camera");
        ImGui::TableSetupColumn("Status");
        ImGui::TableSetupColumn("Fit RMS / max (mm)");
        ImGui::TableSetupColumn("Holdout RMS / max (mm)");
        ImGui::TableSetupColumn("C-X / OD (mm)");
        ImGui::TableSetupColumn("Overlay evidence");
        ImGui::TableHeadersRow();
        for (const auto& target : scale_targets) {
            const auto metrics = target.value("metrics", nlohmann::json::object());
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::Text("%s / %s",
                        target.value("arena_id", "").c_str(),
                        target.value("camera_id", "").c_str());
            ImGui::TableSetColumnIndex(1);
            ImGui::TextUnformatted(target.value("status", "unknown").c_str());
            ImGui::TableSetColumnIndex(2);
            ImGui::Text("%.4f / %.4f",
                        metrics.value("fit_rms_mm", 0.0),
                        metrics.value("fit_max_mm", 0.0));
            ImGui::TableSetColumnIndex(3);
            ImGui::Text("%.4f / %.4f",
                        metrics.value("holdout_rms_mm", 0.0),
                        metrics.value("holdout_max_mm", 0.0));
            ImGui::TableSetColumnIndex(4);
            double c_to_xplus_mm = 0.0;
            double outside_diameter_mm = 0.0;
            const bool has_c_to_xplus = json_number(
                metrics, "c_to_xplus_measured_mm", &c_to_xplus_mm);
            const bool has_outside_diameter = json_number(
                metrics, "outer_diameter_measured_mm", &outside_diameter_mm);
            if (has_c_to_xplus && has_outside_diameter) {
                ImGui::Text("%.3f / %.3f", c_to_xplus_mm, outside_diameter_mm);
            } else {
                ImGui::TextUnformatted("n/a");
            }
            ImGui::TableSetColumnIndex(5);
            ImGui::TextWrapped("%s", target.value("overlay_path", "").c_str());
        }
        ImGui::EndTable();
    }
    if (ImGui::Button("Refresh Physical Scale Review Status")) {
        const auto status = query_citrus_projected_surface_scale_candidate_status(
            ui_state->projected_surface_scale_review_transaction_id,
            "operator-review-refresh");
        if (!status.ok) {
            ui_state->projected_surface_scale_review_error = status.reason;
        } else {
            ui_state->projected_surface_scale_review_status = status.candidate;
            const auto revalidation = status.candidate.value(
                "revalidation", nlohmann::json::object());
            ui_state->projected_surface_scale_review_revalidated =
                status.candidate.value("active", false) &&
                status.candidate.value("state", "") == "ready_for_review" &&
                status.candidate.value(
                    "loaded_from_persisted_candidate_set", false) &&
                status.candidate.value("transaction_id", "") ==
                    ui_state->projected_surface_scale_review_transaction_id &&
                status.candidate.value("candidate_set_id", "") ==
                    ui_state->projected_surface_scale_candidate_manifest.value(
                        "candidate_set_id", "") &&
                revalidation.value("status", "") == "passed";
            ui_state->projected_surface_scale_review_error.clear();
            ui_state->projected_surface_scale_review_message =
                ui_state->projected_surface_scale_review_revalidated
                    ? "Persisted physical scales revalidated. Review every "
                      "camera overlay before arming promotion."
                    : "Citrus physical-scale review status refreshed.";
        }
    }
    const bool persisted_scale_review_selected =
        !ui_state->projected_surface_scale_candidate_manifest_path.empty();
    const bool scale_ready_for_review =
        ui_state->projected_surface_scale_review_verification.value(
            "status", "") == "passed" &&
        ui_state->projected_surface_scale_review_status.value(
            "active", false) &&
        ui_state->projected_surface_scale_review_status.value(
            "state", "") == "ready_for_review" &&
        (!persisted_scale_review_selected ||
         ui_state->projected_surface_scale_review_revalidated);
    ImGui::BeginDisabled(
        !scale_ready_for_review ||
        !projected_scale_authority_controls_available);
    ImGui::Checkbox(
        "I reviewed all scale overlays, orientation markers, holdout errors, and independent dimensions",
        &ui_state->accept_reviewed_projected_surface_scales_armed);
    ImGui::BeginDisabled(
        !ui_state->accept_reviewed_projected_surface_scales_armed);
    if (ImGui::Button("Promote Reviewed Physical Scales As Canvas Authority")) {
        nlohmann::json verification =
            ui_state->projected_surface_scale_review_verification;
        verification["operator_review"] = {
            {"status", "passed"},
            {"all_camera_overlays_reviewed", true},
            {"orientation_markers_reviewed", true},
            {"pitch_fit_and_holdout_reviewed", true},
            {"independent_dimensions_reviewed", true},
            {"three_mm_plane_contract_reviewed", true},
        };
        if (persisted_scale_review_selected) {
            verification["citrus_revalidation"] =
                ui_state->projected_surface_scale_review_status.value(
                    "revalidation", nlohmann::json::object());
            verification["candidate_set_manifest_path"] =
                ui_state->projected_surface_scale_candidate_manifest_path;
        }
        const auto promotion =
            promote_citrus_projected_surface_scale_candidates(
                ui_state->projected_surface_scale_review_transaction_id,
                ui_state->projected_surface_scale_review_canvas_sha256,
                verification,
                true,
                next_scale_review_operation_id("promote"));
        ui_state->accept_reviewed_projected_surface_scales_armed = false;
        ui_state->projected_surface_scale_review_revalidated = false;
        if (!promotion.ok) {
            ui_state->projected_surface_scale_review_error = promotion.reason;
        } else {
            ui_state->projected_surface_scale_review_error.clear();
            ui_state->projected_surface_scale_review_message =
                "Physical-scale promotion request accepted; the guided workflow "
                "will finish after Citrus commits all active pointers.";
        }
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Release Physical Scales Without Promotion")) {
        const auto rejection = reject_citrus_projected_surface_scale_candidates(
            ui_state->projected_surface_scale_review_transaction_id,
            "operator_released_scale_review_without_promotion",
            next_scale_review_operation_id("release"));
        ui_state->accept_reviewed_projected_surface_scales_armed = false;
        ui_state->projected_surface_scale_review_revalidated = false;
        if (!rejection.ok) {
            ui_state->projected_surface_scale_review_error = rejection.reason;
        } else {
            ui_state->projected_surface_scale_review_error.clear();
            ui_state->projected_surface_scale_review_message =
                "Physical-scale candidate released; active scale was unchanged.";
        }
    }
    ImGui::EndDisabled();
    if (!projected_scale_authority_controls_available) {
        ImGui::TextDisabled(
            "Physical-scale review and promotion are disabled until the active calibration transaction resolves.");
    }
    if (!ui_state->projected_surface_scale_review_message.empty()) {
        ImGui::TextWrapped(
            "%s", ui_state->projected_surface_scale_review_message.c_str());
    }
    if (!ui_state->projected_surface_scale_review_error.empty()) {
        ImGui::TextColored(
            ImVec4(1.0f, 0.55f, 0.30f, 1.0f),
            "%s", ui_state->projected_surface_scale_review_error.c_str());
    }

    ImGui::SeparatorText("Rig-Owned Canvas Commissioning Authority");
    ImGui::TextWrapped(
        "Finalize only after every camera has an accepted projected-surface "
        "homography and physical scale. Citrus writes an immutable release "
        "with frozen rig/canvas snapshots and atomically activates a small "
        "pointer. Daily dish registration must not edit this canvas geometry.");
    if (!ui_state->rig_canvas_commissioning_finalization_worker) {
        ui_state->rig_canvas_commissioning_finalization_worker =
            std::make_shared<CommissioningFinalizationWorker>();
    }
    CommissioningFinalizationResult finalization_result;
    if (ui_state->rig_canvas_commissioning_finalization_worker->Poll(
            &finalization_result)) {
        if (!finalization_result.commissioning.empty()) {
            ui_state->rig_canvas_commissioning_status =
                finalization_result.commissioning;
        }
        if (finalization_result.disposition ==
            CommissioningFinalizationDisposition::kPublished) {
            ui_state->rig_canvas_commissioning_error.clear();
            ui_state->rig_canvas_commissioning_message =
                "Published and activated stable commissioning release " +
                finalization_result.release_id + ". Manifest: " +
                finalization_result.manifest_path;
        } else {
            ui_state->rig_canvas_commissioning_message.clear();
            ui_state->rig_canvas_commissioning_error =
                finalization_result.error.empty()
                ? "Commissioning finalization did not complete."
                : finalization_result.error;
        }
    }
    const bool commissioning_finalization_running =
        ui_state->rig_canvas_commissioning_finalization_worker->running();
    ImGui::BeginDisabled(commissioning_finalization_running);
    if (ImGui::Button("Refresh Commissioning Readiness / Active Release")) {
        const auto result = query_citrus_rig_canvas_commissioning_status(
            "operator-commissioning-refresh");
        if (!result.ok) {
            ui_state->rig_canvas_commissioning_error = result.reason;
        } else {
            ui_state->rig_canvas_commissioning_status = result.commissioning;
            ui_state->rig_canvas_commissioning_error.clear();
            ui_state->rig_canvas_commissioning_message =
                "Citrus commissioning authority status refreshed.";
        }
    }
    ImGui::EndDisabled();
    if (commissioning_finalization_running) {
        ImGui::SameLine();
        ImGui::TextDisabled(
            "Publishing and verifying the exact active release...");
    }
    const auto& commissioning_status =
        ui_state->rig_canvas_commissioning_status;
    if (!commissioning_status.empty()) {
        ImGui::Text("State: %s", commissioning_status.value(
            "state", "unknown").c_str());
        ImGui::SameLine();
        ImGui::TextDisabled("ready to finalize: %s",
            commissioning_status.value("ready_to_finalize", false)
                ? "yes" : "no");
        if (!commissioning_status.value("release_id", "").empty()) {
            ImGui::Text("Active release: %s",
                commissioning_status.value("release_id", "").c_str());
            ImGui::TextDisabled("%s",
                commissioning_status.value("manifest_path", "").c_str());
        }
        const auto rows = commissioning_status.value(
            "finalize_requirements", nlohmann::json::object()).value(
                "targets", nlohmann::json::array());
        if (rows.is_array() && !rows.empty() &&
            ImGui::BeginTable("commissioning_requirements_table", 4,
                ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                    ImGuiTableFlags_SizingStretchProp)) {
            ImGui::TableSetupColumn("Arena / Camera");
            ImGui::TableSetupColumn("Homography");
            ImGui::TableSetupColumn("Scale");
            ImGui::TableSetupColumn("Missing / stale reason");
            ImGui::TableHeadersRow();
            for (const auto& row : rows) {
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::Text("%s / %s", row.value("arena_id", "").c_str(),
                            row.value("camera_id", "").c_str());
                ImGui::TableSetColumnIndex(1);
                ImGui::TextUnformatted(
                    row.value("homography_compatible", false) ? "OK" : "missing");
                ImGui::TableSetColumnIndex(2);
                ImGui::TextUnformatted(
                    row.value("scale_compatible", false) ? "OK" : "missing");
                ImGui::TableSetColumnIndex(3);
                const std::string reason = !row.value("homography_error", "").empty()
                    ? row.value("homography_error", "")
                    : row.value("scale_error", row.value("error", ""));
                ImGui::TextWrapped("%s", reason.c_str());
            }
            ImGui::EndTable();
        }
        const auto last_finalize = commissioning_status.value(
            "last_finalize", nlohmann::json::object());
        if (!last_finalize.empty() &&
            last_finalize.value("state", "not_requested") != "not_requested") {
            ImGui::Text("Last finalize: %s",
                last_finalize.value("state", "unknown").c_str());
            if (!last_finalize.value("error", "").empty()) {
                ImGui::TextColored(ImVec4(1.0f, 0.55f, 0.30f, 1.0f),
                    "%s", last_finalize.value("error", "").c_str());
            }
        }
    }
    const bool commissioning_ready =
        commissioning_status.value("ready_to_finalize", false);
    ImGui::BeginDisabled(
        !commissioning_ready || !manual_authority_controls_available ||
        commissioning_finalization_running);
    ImGui::Checkbox(
        "I accept this rig revision, canvas snapshot, homographies, scales, QC, and Citrus-derived source sessions as commissioning authority",
        &ui_state->accept_rig_canvas_commissioning_armed);
    ImGui::BeginDisabled(!ui_state->accept_rig_canvas_commissioning_armed);
    if (ImGui::Button("Finalize And Activate Immutable Canvas Commissioning")) {
        const std::string canvas_path = !ui_state->citrus_canvas_config_path.empty()
            ? ui_state->citrus_canvas_config_path
            : ui_state->loaded_calibration_session_citrus_config_path;
        std::string canvas_checksum;
        std::string checksum_error;
        if (canvas_path.empty()) {
            ui_state->rig_canvas_commissioning_error =
                "Load/import the Citrus canvas before finalizing commissioning.";
        } else if (!orange::gui::spatial_layout::checksum::file_sha256(
                       canvas_path, &canvas_checksum, &checksum_error)) {
            ui_state->rig_canvas_commissioning_error = checksum_error;
        } else {
            const std::string operation_id =
                next_commissioning_operation_id("finalize");
            CommissioningFinalizationRequest request;
            request.transaction_id = operation_id;
            request.operation_id = operation_id;
            request.canvas_path = canvas_path;
            request.expected_canvas_checksum = canvas_checksum;
            request.accept_commissioning_armed = true;
            std::string start_error;
            const bool started =
                ui_state->rig_canvas_commissioning_finalization_worker->Start(
                    request,
                    [](const CommissioningFinalizationRequest& input) {
                        const auto result =
                            finalize_citrus_rig_canvas_commissioning(
                                input.transaction_id,
                                input.canvas_path,
                                input.expected_canvas_checksum,
                                input.accept_commissioning_armed,
                                input.operation_id);
                        CommissioningControlReply reply;
                        reply.ok = result.ok;
                        reply.definitive_rejection =
                            result.attempted && !result.accepted &&
                            !result.response.empty();
                        reply.commissioning = result.commissioning;
                        reply.error = result.reason;
                        return reply;
                    },
                    [](const std::string& phase) {
                        const auto result =
                            query_citrus_rig_canvas_commissioning_status(phase);
                        CommissioningControlReply reply;
                        reply.ok = result.ok;
                        reply.commissioning = result.commissioning;
                        reply.error = result.reason;
                        return reply;
                    },
                    &start_error);
            if (!started) {
                ui_state->rig_canvas_commissioning_error = start_error;
            } else {
                ui_state->rig_canvas_commissioning_error.clear();
                ui_state->rig_canvas_commissioning_message =
                    "Publishing stable commissioning in the background; "
                    "Orange will verify the exact active release automatically.";
            }
        }
        ui_state->accept_rig_canvas_commissioning_armed = false;
    }
    ImGui::EndDisabled();
    ImGui::EndDisabled();
    if (!manual_authority_controls_available) {
        ImGui::TextDisabled(
            "Commissioning finalization is disabled until the active calibration transaction resolves.");
    }
    if (!ui_state->rig_canvas_commissioning_message.empty()) {
        ImGui::TextWrapped("%s",
            ui_state->rig_canvas_commissioning_message.c_str());
    }
    if (!ui_state->rig_canvas_commissioning_error.empty()) {
        ImGui::TextColored(ImVec4(1.0f, 0.55f, 0.30f, 1.0f), "%s",
            ui_state->rig_canvas_commissioning_error.c_str());
    }

    ImGui::SeparatorText("Optional Daily Registration Runtime Mode");
    ImGui::TextWrapped(
        "Daily dish registration is optional. Base-only uses the immutable "
        "commissioned canvas, homography, scale, and arena geometry. A missing "
        "daily registration never blocks base-only operation; only an explicitly "
        "selected invalid registration is blocking.");
    if (ImGui::Button("Refresh Daily Registration Status")) {
        const auto result = query_citrus_daily_registration_status(
            "operator-daily-registration-refresh");
        if (!result.ok) {
            ui_state->daily_registration_error = result.reason;
        } else {
            ui_state->daily_registration_status = result.daily_registration;
            ui_state->daily_registration_error.clear();
            ui_state->daily_registration_message =
                "Citrus daily-registration runtime status refreshed.";
        }
    }
    const auto daily_runtime = ui_state->daily_registration_status.value(
        "runtime", nlohmann::json::object());
    if (!daily_runtime.empty()) {
        ImGui::Text("Mode: %s", daily_runtime.value(
            "mode", "base_only").c_str());
        ImGui::SameLine();
        ImGui::TextDisabled("status: %s", daily_runtime.value(
            "daily_registration_status", "not_performed").c_str());
        if (!daily_runtime.value("all_selected_runtime_safe", true)) {
            ImGui::TextColored(
                ImVec4(1.0f, 0.55f, 0.30f, 1.0f),
                "The selected daily registration is invalid. Return to "
                "base-only to use commissioned geometry.");
        }
    }
    ImGui::Checkbox(
        "I intend to use commissioned base geometry without daily registration",
        &ui_state->base_only_runtime_mode_armed);
    const bool manual_runtime_selection_blocked =
        ui_state->calibration_transaction_lease &&
        ui_state->calibration_transaction_lease->active();
    ImGui::BeginDisabled(
        !ui_state->base_only_runtime_mode_armed ||
        manual_runtime_selection_blocked);
    if (ImGui::Button("Select Base-Only Runtime Mode")) {
        const std::string operation_id =
            next_commissioning_operation_id("select-base-only");
        const auto result = select_citrus_daily_registration_runtime_mode(
            "base_only", "", "", true, operation_id);
        if (!result.ok) {
            ui_state->daily_registration_error = result.reason;
        } else {
            ui_state->daily_registration_status = result.daily_registration;
            ui_state->daily_registration_error.clear();
            ui_state->daily_registration_message =
                "Base-only selected. Experiments will use immutable commissioned "
                "geometry and do not require a daily registration.";
        }
        ui_state->base_only_runtime_mode_armed = false;
    }
    ImGui::EndDisabled();
    if (manual_runtime_selection_blocked) {
        ImGui::TextDisabled(
            "Manual runtime-mode changes are disabled until the active calibration transaction resolves.");
    }
    if (!ui_state->daily_registration_message.empty()) {
        ImGui::TextWrapped("%s", ui_state->daily_registration_message.c_str());
    }
    if (!ui_state->daily_registration_error.empty()) {
        ImGui::TextColored(ImVec4(1.0f, 0.55f, 0.30f, 1.0f), "%s",
            ui_state->daily_registration_error.c_str());
    }

    render_daily_registration_workflow_panel(
        ui_state,
        cameras_params,
        cameras_select,
        num_cameras,
        spatial_snapshot_workers,
        artifact_root_dir,
        camera_control->record_video || camera_control->recording_draining);

    ImGui::SeparatorText("Detection And Canonical Layout");
    ImGui::InputText("Layout ID", &ui_state->layout_artifact.layout_id);
    ImGui::InputText("Artifact ID", &ui_state->layout_artifact.artifact_id);
    ImGui::InputText("Canvas ID", &ui_state->layout_artifact.context.canvas_id);
    ImGui::InputText("Arena ID", &ui_state->layout_artifact.context.arena_id);
    ImGui::InputText("Dish design ID", &ui_state->layout_artifact.context.dish_design_id);

    int coordinate_space = ui_state->layout_artifact.layout.coordinate_space == CoordinateSpace::kLayoutMm ? 0 : 1;
    const char* coordinate_items[] = {"layout_mm", "layout_units"};
    if (ImGui::Combo("Layout coordinate space", &coordinate_space, coordinate_items, IM_ARRAYSIZE(coordinate_items))) {
        ui_state->layout_artifact.layout.coordinate_space =
            coordinate_space == 0 ? CoordinateSpace::kLayoutMm : CoordinateSpace::kLayoutUnits;
    }
    ImGui::InputText("Ordering rule", &ui_state->layout_artifact.provenance.ordering_rule);
    if (render_layout_geometry_editor(
            "Experimental area",
            &ui_state->layout_artifact.layout.outer_geometry)) {
        ui_state->calibration_inner_rim_target_confirmed = false;
    }

    ImGui::SeparatorText("View Registration");
    render_registration_editor(ui_state);
    bool rendered_physical_centroid_gate_control = false;
    if (ui_state->citrus_template.has_inner_diameter_mm &&
        ui_state->dish_mask_runtime.has_geometry &&
        ui_state->dish_mask_runtime.geometry.outer_geometry.type ==
            orange::spatial::RuntimeGeometryType::kCircle) {
        const double inner_radius_mm =
            ui_state->citrus_template.inner_diameter_mm * 0.5;
        const double radius_px =
            ui_state->dish_mask_runtime.geometry.outer_geometry.circle.r;
        if (inner_radius_mm > 0.0 && radius_px > 0.0) {
            const double pixels_per_mm = radius_px / inner_radius_mm;
            if (!ui_state->centroid_gate_outset_authored_mm ||
                ui_state->centroid_gate_outset_mm_camera_serial !=
                    selected_camera.camera_serial) {
                ui_state->centroid_gate_outset_mm =
                    ui_state->centroid_gate_outset_px / pixels_per_mm;
                ui_state->centroid_gate_outset_authored_mm = true;
                ui_state->centroid_gate_outset_mm_camera_serial =
                    selected_camera.camera_serial;
            }
            if (ImGui::InputDouble(
                    "Centroid gate outward forgiveness (mm)",
                    &ui_state->centroid_gate_outset_mm,
                    0.01,
                    0.1,
                    "%.4f")) {
                ui_state->centroid_gate_outset_mm =
                    std::max(0.0, ui_state->centroid_gate_outset_mm);
                ui_state->edge_margin_px = 0.0;
            }
            ui_state->centroid_gate_outset_px =
                ui_state->centroid_gate_outset_mm * pixels_per_mm;
            ImGui::Text(
                "Physical inner radius: %.3f mm | top-rim scale: %.5f px/mm | forgiveness: %.2f px",
                inner_radius_mm,
                pixels_per_mm,
                ui_state->centroid_gate_outset_px);
            rendered_physical_centroid_gate_control = true;
        }
    }
    if (!rendered_physical_centroid_gate_control) {
        ui_state->centroid_gate_outset_authored_mm = false;
        ui_state->centroid_gate_outset_mm_camera_serial.clear();
        if (ImGui::InputDouble(
                "Centroid gate outward forgiveness (px)",
                &ui_state->centroid_gate_outset_px,
                0.5,
                5.0,
                "%.2f")) {
            ui_state->centroid_gate_outset_px =
                std::max(0.0, ui_state->centroid_gate_outset_px);
            ui_state->edge_margin_px = 0.0;
        }
    }

    ImGui::SeparatorText("Zones");
    render_zone_editor(ui_state);
    sync_single_experimental_area_zone(ui_state);

    rebuild_schema_preview(ui_state, &selected_camera);

    render_calibration_workflow_tabs(
        ui_state,
        HoughCirclePanelActions{reset_registration_from_frame});

    render_calibration_capture_metadata_panel(
        ui_state,
        camera_control,
        ecams,
        cameras_params,
        num_cameras,
        selected_camera,
        CalibrationCaptureMetadataPanelActions{apply_calibration_image_set_purpose_defaults});

    const bool captured_in_full_resolution =
        !ui_state->has_capture ||
        ui_state->captured_source_array_role.empty() ||
        ui_state->captured_source_array_role == "images_full";
    const bool top_rim_save_busy = top_rim_observation_save_worker_is_busy();
    const bool generic_image_set_save_busy =
        generic_calibration_image_set_save_worker_is_busy() ||
        queued_generic_calibration_image_set_save_job_count() > 0;
    const bool spatial_save_busy = top_rim_save_busy || generic_image_set_save_busy;
    const auto physical_registration_preflight =
        evaluate_daily_physical_registration_save_preflight({
            spatial_save_busy,
            ui_state->has_capture,
            selected_camera.camera_serial,
            ui_state->captured_camera_serial,
            ui_state->captured_texture_width,
            ui_state->captured_texture_height,
            static_cast<int>(selected_camera.width),
            static_cast<int>(selected_camera.height),
            ui_state->captured_source_array_role,
            ui_state->calibration_inner_rim_target_confirmed,
            ui_state->dish_mask_runtime.has_geometry &&
                ui_state->dish_mask_runtime.geometry.outer_geometry.type ==
                    RuntimeGeometryType::kCircle,
            ui_state->has_detected_experimental_area_circle &&
                ui_state->detected_experimental_area_geometry.type ==
                    RuntimeGeometryType::kCircle
        });
    SpatialLayoutPersistencePanelState persistence_panel_state;
    persistence_panel_state.citrus_linked_layout_matches_selected_camera =
        citrus_template_matches_selected_camera;
    persistence_panel_state.captured_in_full_resolution =
        captured_in_full_resolution;
    persistence_panel_state.top_rim_save_busy = top_rim_save_busy;
    persistence_panel_state.generic_image_set_save_busy =
        generic_image_set_save_busy;
    persistence_panel_state.can_save_top_rim_observation =
        physical_registration_preflight.allowed;
    persistence_panel_state.top_rim_ineligible_reason =
        physical_registration_preflight.message;
    persistence_panel_state.can_save_generic_image_set =
        ui_state->has_capture &&
        captured_in_full_resolution &&
        citrus_template_matches_selected_camera &&
        !spatial_save_busy;
    persistence_panel_state.can_save_group_image_sets =
        !ui_state->group_captures.empty() &&
        pending_group_snapshot_count(*ui_state) == 0 &&
        !group_capture_workflow_active(*ui_state) &&
        !spatial_save_busy;
    persistence_panel_state.can_save_linked_arena_layouts =
        !ui_state->calibration_session_dir.empty() &&
        (ui_state->citrus_template.available ||
         !ui_state->citrus_canvas_templates.empty()) &&
        !spatial_save_busy;
    const SpatialLayoutPersistencePanelEvent persistence_event =
        render_spatial_layout_persistence_panel(ui_state, persistence_panel_state);
    handle_spatial_layout_persistence_event(
        persistence_event,
        ui_state,
        selected_camera,
        cameras_params,
        num_cameras,
        artifact_root_dir);

    if (other_calibration_tool_busy) {
        ImGui::EndDisabled();
    }
    ImGui::EndChild();
    ImGui::TableNextColumn();
    ImGui::BeginChild("SpatialLayoutFitPreviewPanel", ImVec2(0.0f, 0.0f), true);

    render_group_capture_panels(
        ui_state,
        cameras_params,
        num_cameras,
        GroupCapturePanelActions{use_group_capture_for_fit});

    ImGui::SeparatorText("Fit Preview");
    const char* canvas_edit_items[] = {"registration", "selected_zone"};
    ImGui::Combo("Canvas edit mode", &ui_state->canvas_edit_mode, canvas_edit_items, IM_ARRAYSIZE(canvas_edit_items));
    if (!ui_state->has_capture) {
        ImGui::TextDisabled("Capture a frame to render the resolved camera-pixel overlays.");
    } else {
        const bool canvas_changed =
            draw_runtime_preview(
                ui_state,
                PreviewOverlayActions{
                    handle_registration_canvas_edit,
                    handle_selected_zone_canvas_edit});
        if (canvas_changed) {
            rebuild_schema_preview(ui_state, &selected_camera);
        }
        if (ui_state->canvas_edit_mode == 0) {
            ImGui::TextDisabled("Drag cyan to move the experimental area. Drag gold to scale it. Drag pink to rotate the layout.");
        } else {
            ImGui::TextDisabled("Drag green to move the selected zone. Drag gold/orange handles to resize it.");
        }
        ImGui::TextDisabled("Blue outline/triangle: commissioned/base Citrus experimental area inverse-projected into camera pixels. Purple outline: separately configured Citrus calibration fit ring. Pink outline/cross: transient water-side inner-rim proposal. Magenta 'Daily Hough / accepted rim': the top-rim fit used by daily registration; if the operator adjusted it, raw Hough remains magenta and the accepted rim is orange. Bright green 'Selected daily experimental area': the unchanged canonical area after the exact selected integer translation. Teal indicates the same geometry while still a candidate. Yellow is the outward centroid gate; green/cyan outlines are resolved zone overlays.");
    }

    ImGui::Separator();
    ImGui::TextWrapped("%s", ui_state->preview_status.c_str());
    if (!ui_state->preview_error.empty()) {
        ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.35f, 1.0f), "%s", ui_state->preview_error.c_str());
    }
    if (!ui_state->detection_status.empty()) {
        ImGui::TextWrapped("%s", ui_state->detection_status.c_str());
    }
    if (!ui_state->detection_error.empty()) {
        ImGui::TextColored(ImVec4(1.0f, 0.55f, 0.35f, 1.0f), "%s", ui_state->detection_error.c_str());
    }
    if (!ui_state->citrus_import_status.empty()) {
        ImGui::TextWrapped("%s", ui_state->citrus_import_status.c_str());
    }
    if (!ui_state->citrus_import_error.empty()) {
        ImGui::TextColored(ImVec4(0.95f, 0.65f, 0.35f, 1.0f), "%s", ui_state->citrus_import_error.c_str());
    }
    if (!ui_state->persistence_status.empty()) {
        ImGui::TextWrapped("%s", ui_state->persistence_status.c_str());
    }
    if (!ui_state->persistence_error.empty()) {
        ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.35f, 1.0f), "%s", ui_state->persistence_error.c_str());
    }
    ImGui::EndChild();
    ImGui::EndTable();

    ImGui::End();

    if (ImGuiFileDialog::Instance()->Display(kLoadSpatialLayoutDialogId)) {
        if (ImGuiFileDialog::Instance()->IsOk()) {
            std::string status;
            std::string error;
            if (!load_spatial_layout_artifact(
                    ui_state,
                    ImGuiFileDialog::Instance()->GetFilePathName(),
                    &status,
                    &error)) {
                ui_state->persistence_error = error;
                ui_state->persistence_status.clear();
            } else {
                ui_state->persistence_status = status;
                ui_state->persistence_error.clear();
                rebuild_schema_preview(ui_state, &selected_camera);
            }
        }
        ImGuiFileDialog::Instance()->Close();
    }

    if (ImGuiFileDialog::Instance()->Display(kLoadCalibrationSessionDialogId)) {
        if (ImGuiFileDialog::Instance()->IsOk()) {
            std::string status;
            std::string error;
            if (!load_spatial_calibration_session_review(
                    ui_state,
                    ImGuiFileDialog::Instance()->GetFilePathName(),
                    &status,
                    &error)) {
                ui_state->persistence_error = error;
                ui_state->persistence_status.clear();
            } else {
                ui_state->persistence_status = status;
                ui_state->persistence_error.clear();
                const std::string citrus_config_path =
                    ui_state->loaded_calibration_session_citrus_config_path;
                if (!citrus_config_path.empty()) {
                    if (!std::filesystem::exists(citrus_config_path)) {
                        ui_state->citrus_import_error =
                            "Calibration session references a Citrus canvas config that no longer exists: " +
                            citrus_config_path;
                    } else {
                        std::string citrus_status;
                        std::string citrus_error;
                        if (!import_citrus_canvas_templates(
                                ui_state,
                                selected_camera,
                                citrus_config_path,
                                &citrus_status,
                                &citrus_error)) {
                            ui_state->citrus_import_error =
                                "Failed to auto-load Citrus canvas from calibration session: " +
                                citrus_error;
                        } else {
                            ui_state->citrus_import_status =
                                "Auto-loaded from calibration session. " + citrus_status;
                            ui_state->citrus_import_error.clear();
                            ui_state->persistence_status +=
                                " Auto-loaded Citrus canvas config.";
                            rebuild_schema_preview(ui_state, &selected_camera);
                        }
                    }
                }
            }
        }
        ImGuiFileDialog::Instance()->Close();
    }

    if (ImGuiFileDialog::Instance()->Display(
            kLoadHomographyCandidateSetDialogId)) {
        if (ImGuiFileDialog::Instance()->IsOk()) {
            const std::filesystem::path selected =
                ImGuiFileDialog::Instance()->GetFilePathName();
            nlohmann::json manifest;
            std::string error;
            if (selected.filename() != "candidate_set.json" ||
                !read_json_file(selected, &manifest, &error) ||
                manifest.value("schema_id", "") !=
                    "citrus.homography_candidate.status" ||
                manifest.value("schema_version", 0) != 1 ||
                manifest.value("state", "") != "ready_for_review" ||
                manifest.value("candidate_set_id", "").empty() ||
                manifest.value("canvas_checksum", "").rfind("sha256:", 0) != 0 ||
                !manifest.value("candidates", nlohmann::json::array()).is_array() ||
                manifest.value("candidates", nlohmann::json::array()).empty()) {
                ui_state->homography_candidate_review_error = error.empty()
                    ? "Selected JSON is not a complete ready-for-review Citrus candidate set."
                    : error;
            } else {
                ui_state->homography_candidate_review_manifest_path =
                    selected.string();
                ui_state->homography_candidate_review_manifest =
                    std::move(manifest);
                ui_state->homography_candidate_review_status =
                    nlohmann::json::object();
                ui_state->homography_candidate_review_revalidated = false;
                ui_state->accept_reviewed_homographies_armed = false;
                ui_state->homography_candidate_review_error.clear();
                ui_state->homography_candidate_review_message =
                    "Candidate set loaded locally. Ask Citrus to revalidate it "
                    "before reviewing and promoting.";
            }
        }
        ImGuiFileDialog::Instance()->Close();
    }

    if (ImGuiFileDialog::Instance()->Display(
            kLoadProjectedSurfaceScaleCandidateSetDialogId)) {
        if (ImGuiFileDialog::Instance()->IsOk()) {
            const std::filesystem::path selected =
                ImGuiFileDialog::Instance()->GetFilePathName();
            auto bundle = load_persisted_scale_review_bundle(selected);
            if (!bundle.ok) {
                ui_state->projected_surface_scale_review_error = bundle.error;
            } else {
                ui_state->projected_surface_scale_candidate_manifest_path =
                    bundle.candidate_manifest_path.string();
                ui_state->projected_surface_scale_candidate_manifest =
                    std::move(bundle.candidate_manifest);
                ui_state->projected_surface_scale_review_manifest_path =
                    bundle.orange_manifest_path.string();
                ui_state->projected_surface_scale_review_manifest =
                    std::move(bundle.orange_manifest);
                ui_state->projected_surface_scale_review_verification =
                    ui_state->projected_surface_scale_review_manifest.value(
                        "verification", nlohmann::json::object());
                ui_state->projected_surface_scale_review_transaction_id =
                    ui_state->projected_surface_scale_candidate_manifest.value(
                        "transaction_id", "");
                ui_state->projected_surface_scale_review_canvas_sha256 =
                    ui_state->projected_surface_scale_candidate_manifest.value(
                        "canvas_checksum", "");
                ui_state->projected_surface_scale_review_status =
                    nlohmann::json::object();
                ui_state->projected_surface_scale_review_revalidated = false;
                ui_state->accept_reviewed_projected_surface_scales_armed = false;
                ui_state->projected_surface_scale_review_error.clear();
                ui_state->projected_surface_scale_review_message =
                    "Persisted Citrus candidates and their exact Orange QC "
                    "bundle were loaded locally. Ask Citrus to revalidate "
                    "before reviewing and promoting.";
            }
        }
        ImGuiFileDialog::Instance()->Close();
    }

    if (ImGuiFileDialog::Instance()->Display(kLoadCitrusArenaConfigDialogId)) {
        if (ImGuiFileDialog::Instance()->IsOk()) {
            std::string status;
            std::string error;
            const std::string selected_config_path =
                ImGuiFileDialog::Instance()->GetFilePathName();
            if (!import_citrus_canvas_templates(
                    ui_state,
                    selected_camera,
                    selected_config_path,
                    &status,
                    &error)) {
                ui_state->citrus_import_error = error;
                if (ui_state->citrus_canvas_config_path == selected_config_path) {
                    ui_state->citrus_import_status =
                        "Selected Citrus canvas for recording metadata. Spatial preview import is unavailable for this canvas.";
                } else {
                    ui_state->citrus_import_status.clear();
                }
            } else {
                ui_state->citrus_import_status = status;
                ui_state->citrus_import_error.clear();
                rebuild_schema_preview(ui_state, &selected_camera);
            }
        }
        ImGuiFileDialog::Instance()->Close();
    }
}
