#include "spatial_layout_ui.h"

#include "camera_preview_utils.h"
#include "gui/spatial_layout/calibration_metadata.h"
#include "gui/spatial_layout/calibration_workflow.h"
#include "gui/spatial_layout/canvas_edit.h"
#include "gui/spatial_layout/capture_panel.h"
#include "gui/spatial_layout/citrus_import.h"
#include "gui/spatial_layout/citrus_template_workflow.h"
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
#include "imgui.h"
#include "misc/cpp/imgui_stdlib.h"
#include <ImGuiFileDialog.h>
#include "spatial_snapshot_worker.h"

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <sstream>
#include <vector>

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
using orange::gui::spatial_layout::find_citrus_template_index_for_camera;
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
using orange::gui::spatial_layout::render_layout_geometry_editor;
using orange::gui::spatial_layout::render_calibration_workflow_tabs;
using orange::gui::spatial_layout::render_calibration_capture_metadata_panel;
using orange::gui::spatial_layout::render_registration_editor;
using orange::gui::spatial_layout::render_spatial_layout_persistence_panel;
using orange::gui::spatial_layout::render_zone_editor;
using orange::gui::spatial_layout::reset_registration_from_frame;
using orange::gui::spatial_layout::rebuild_schema_preview;
using orange::gui::spatial_layout::request_group_full_resolution_snapshots;
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
using orange::gui::spatial_layout::submit_generic_calibration_image_set_save_job;
using orange::gui::spatial_layout::submit_top_rim_observation_save_job;
using orange::gui::spatial_layout::sync_single_experimental_area_zone;
using orange::gui::spatial_layout::top_rim_observation_save_worker_is_busy;
using orange::gui::spatial_layout::transform_point_projective;
using orange::gui::spatial_layout::queued_generic_calibration_image_set_save_job_count;

constexpr const char* kLoadSpatialLayoutDialogId = "LoadSpatialLayoutArtifact";
constexpr const char* kLoadCitrusArenaConfigDialogId = "LoadCitrusArenaConfig";
constexpr const char* kLoadCalibrationSessionDialogId = "LoadCalibrationSession";

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

    const bool can_capture =
        !camera_control->subscribe &&
        !camera_control->record_video &&
        !other_calibration_tool_busy;
    const bool can_capture_live_preview =
        camera_control->subscribe &&
        !camera_control->record_video &&
        !other_calibration_tool_busy &&
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
    const bool group_capture_pending =
        pending_group_snapshot_count(*ui_state) > 0;
    const bool can_capture_full_resolution_stream_snapshot =
        camera_control->subscribe &&
        !camera_control->record_video &&
        !other_calibration_tool_busy &&
        cameras_select[ui_state->selected_camera].stream_on &&
        selected_snapshot_worker != nullptr &&
        !full_res_request_pending_for_selected;
    const bool can_capture_group_full_resolution_stream_snapshot =
        camera_control->subscribe &&
        !camera_control->record_video &&
        !other_calibration_tool_busy &&
        eligible_group_camera_count > 0 &&
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
    ImGui::BeginDisabled(!can_capture_group_full_resolution_stream_snapshot);
    if (ImGui::Button("Capture Group Full-Resolution Snapshots")) {
        std::string request_error;
        if (!request_group_full_resolution_snapshots(
                ui_state,
                cameras_params,
                cameras_select,
                num_cameras,
                spatial_snapshot_workers,
                1,
                &request_error)) {
            ui_state->group_capture_error = request_error;
            ui_state->group_capture_status = "Grouped full-resolution capture request failed.";
        }
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(!can_capture_group_full_resolution_stream_snapshot);
    if (ImGui::Button("Capture Averaged Group Snapshots")) {
        std::string request_error;
        if (!request_group_full_resolution_snapshots(
                ui_state,
                cameras_params,
                cameras_select,
                num_cameras,
                spatial_snapshot_workers,
                static_cast<uint32_t>(ui_state->calibration_average_frame_count),
                &request_error)) {
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
            "Grouped capture %s is waiting on %d camera(s).",
            ui_state->group_capture_id.c_str(),
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
        if (!ui_state->citrus_canvas_config_path.empty()) {
            ImGui::TextDisabled("%s", ui_state->citrus_canvas_config_path.c_str());
        }
    }

    if (!ui_state->citrus_template.available) {
        ImGui::TextDisabled("Import a Citrus canvas config to seed the single circular experimental area.");
    } else {
        ImGui::TextWrapped(
            "Imported: rig=%s canvas=%s arena=%s config=%s camera=%s",
            ui_state->citrus_template.source_rig_name.c_str(),
            ui_state->citrus_template.source_canvas_name.c_str(),
            ui_state->citrus_template.source_arena_name.c_str(),
            ui_state->citrus_template.source_config_name.c_str(),
            ui_state->citrus_template.source_camera_id.c_str());
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
    const SpatialLayoutPersistencePanelState persistence_panel_state{
        citrus_template_matches_selected_camera,
        captured_in_full_resolution,
        top_rim_save_busy,
        generic_image_set_save_busy,
        ui_state->has_capture &&
            ui_state->calibration_inner_rim_target_confirmed &&
            ui_state->dish_mask_runtime.has_geometry &&
            ui_state->has_detected_experimental_area_circle &&
            ui_state->detected_experimental_area_geometry.type == RuntimeGeometryType::kCircle &&
            captured_in_full_resolution &&
            citrus_template_matches_selected_camera &&
            !spatial_save_busy,
        ui_state->has_capture &&
            captured_in_full_resolution &&
            citrus_template_matches_selected_camera &&
            !spatial_save_busy,
        !ui_state->group_captures.empty() &&
            pending_group_snapshot_count(*ui_state) == 0 &&
            !spatial_save_busy,
        !ui_state->calibration_session_dir.empty() &&
            (ui_state->citrus_template.available ||
             !ui_state->citrus_canvas_templates.empty()) &&
            !spatial_save_busy};
    const SpatialLayoutPersistencePanelEvent persistence_event =
        render_spatial_layout_persistence_panel(ui_state, persistence_panel_state);
    handle_spatial_layout_persistence_event(
        persistence_event,
        ui_state,
        selected_camera,
        cameras_params,
        num_cameras,
        artifact_root_dir);

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
        ImGui::TextDisabled("Blue outline/triangle: Citrus experimental area inverse-projected into camera pixels. Purple outline: separately configured Citrus calibration fit ring. Pink outline/cross: detected water-side inner-rim proposal. Green outline/diamond/line: corrected Citrus outline preserving the configured experimental radius with the proposed center. Orange outline: operator-adjusted physical inner-rim boundary. Yellow outline: outward centroid-gating region (legacy artifacts may instead use an inward margin). Green/cyan outlines: resolved zone overlays.");
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

    if (ImGuiFileDialog::Instance()->Display(kLoadCitrusArenaConfigDialogId)) {
        if (ImGuiFileDialog::Instance()->IsOk()) {
            std::string status;
            std::string error;
            if (!import_citrus_canvas_templates(
                    ui_state,
                    selected_camera,
                    ImGuiFileDialog::Instance()->GetFilePathName(),
                    &status,
                    &error)) {
                ui_state->citrus_import_error = error;
                ui_state->citrus_import_status.clear();
            } else {
                ui_state->citrus_import_status = status;
                ui_state->citrus_import_error.clear();
                rebuild_schema_preview(ui_state, &selected_camera);
            }
        }
        ImGuiFileDialog::Instance()->Close();
    }
}
