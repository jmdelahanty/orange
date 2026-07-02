#include "gui/spatial_layout/group_capture_controller.h"

#include "camera_preview_utils.h"
#include "gui/spatial_layout/calibration_metadata.h"
#include "gui/spatial_layout/layout_state.h"
#include "gui/spatial_layout/preview_capture.h"
#include "gui/spatial_layout/projection_snapshot_client.h"
#include "gui/spatial_layout/session_io.h"
#include "gui/spatial_layout/session_review.h"
#include "project.h"

#include <algorithm>
#include <sstream>
#include <utility>
#include <vector>

namespace orange::gui::spatial_layout {
namespace {

SpatialLayoutGroupCaptureFrame make_group_capture_from_snapshot(
    const SpatialSnapshotResult& result,
    const CameraParams* cameras_params,
    int num_cameras,
    const std::string& capture_group_id,
    const std::string& capture_mode,
    const SpatialLayoutCalibrationImageSetMetadata& metadata)
{
    SpatialLayoutGroupCaptureFrame capture;
    capture.valid = result.ok && result.width > 0 && result.height > 0 && !result.rgba.empty();
    capture.capture_group_id = capture_group_id;
    capture.metadata = metadata;
    capture.camera_serial = result.camera_serial;
    capture.camera_index =
        find_camera_index_by_serial(cameras_params, num_cameras, result.camera_serial);
    if (capture.camera_index >= 0) {
        const CameraParams& camera_params = cameras_params[capture.camera_index];
        capture.camera_name = camera_params.camera_name;
        capture.camera_configured_width = camera_params.width;
        capture.camera_configured_height = camera_params.height;
        capture.camera_pixel_format = camera_params.pixel_format;
        capture.camera_exposure_us = static_cast<double>(camera_params.exposure);
        capture.has_camera_exposure_us = true;
        capture.camera_frame_rate_hz = static_cast<double>(camera_params.frame_rate);
        capture.has_camera_frame_rate_hz = true;
        capture.camera_gain = static_cast<double>(camera_params.gain);
        capture.has_camera_gain = true;
    }
    capture.width = result.width;
    capture.height = result.height;
    capture.rgba = result.rgba;
    capture.source_array_role =
        result.source_array_role.empty() ? "images_full" : result.source_array_role;
    capture.capture_mode = capture_mode.empty() ? "operator_group_next_frame" : capture_mode;
    capture.source_frame_count = std::max<uint32_t>(1u, result.completed_frame_count);
    capture.first_local_frame_id = result.first_local_frame_id;
    capture.last_local_frame_id = result.last_local_frame_id;
    capture.first_camera_frame_id = result.first_camera_frame_id;
    capture.last_camera_frame_id = result.last_camera_frame_id;
    return capture;
}

void upsert_group_capture(
    SpatialLayoutUiState* ui_state,
    SpatialLayoutGroupCaptureFrame capture)
{
    if (ui_state == nullptr || capture.camera_serial.empty()) {
        return;
    }
    for (SpatialLayoutGroupCaptureFrame& existing : ui_state->group_captures) {
        if (existing.camera_serial == capture.camera_serial) {
            orange::preview::clear_texture(
                &existing.texture,
                &existing.texture_width,
                &existing.texture_height);
            existing = std::move(capture);
            return;
        }
    }
    ui_state->group_captures.push_back(std::move(capture));
}

std::string build_group_capture_id(
    const SpatialLayoutUiState& ui_state,
    const SpatialLayoutCalibrationImageSetMetadata& metadata,
    const std::string& timestamp)
{
    std::ostringstream oss;
    oss << "calgrp_" << sanitize_artifact_component(timestamp);
    if (ui_state.citrus_template.available &&
        !ui_state.citrus_template.source_canvas_name.empty()) {
        oss << "_" << sanitize_artifact_component(ui_state.citrus_template.source_canvas_name);
    }
    if (!metadata.image_set_purpose.empty()) {
        oss << "_" << sanitize_artifact_component(metadata.image_set_purpose);
    }
    if (!metadata.image_set_target_plane.empty()) {
        oss << "_" << sanitize_artifact_component(metadata.image_set_target_plane);
    }
    return oss.str();
}

bool camera_is_group_capture_eligible(
    const CameraEachSelect* cameras_select,
    SpatialSnapshotWorker* const* spatial_snapshot_workers,
    int camera_index)
{
    return cameras_select != nullptr &&
           spatial_snapshot_workers != nullptr &&
           camera_index >= 0 &&
           cameras_select[camera_index].stream_on &&
           spatial_snapshot_workers[camera_index] != nullptr;
}

}  // namespace

int pending_group_snapshot_count(const SpatialLayoutUiState& ui_state)
{
    int count = 0;
    for (const SpatialLayoutPendingGroupSnapshotRequest& request :
         ui_state.pending_group_snapshot_requests) {
        if (!request.completed && !request.failed) {
            ++count;
        }
    }
    return count;
}

int failed_group_snapshot_count(const SpatialLayoutUiState& ui_state)
{
    int count = 0;
    for (const SpatialLayoutPendingGroupSnapshotRequest& request :
         ui_state.pending_group_snapshot_requests) {
        if (request.failed) {
            ++count;
        }
    }
    return count;
}

int find_camera_index_by_serial(
    const CameraParams* cameras_params,
    int num_cameras,
    const std::string& camera_serial)
{
    if (cameras_params == nullptr || camera_serial.empty()) {
        return -1;
    }
    for (int i = 0; i < num_cameras; ++i) {
        if (cameras_params[i].camera_serial == camera_serial) {
            return i;
        }
    }
    return -1;
}

void clear_group_captures(SpatialLayoutUiState* ui_state)
{
    if (ui_state == nullptr) {
        return;
    }
    for (SpatialLayoutGroupCaptureFrame& capture : ui_state->group_captures) {
        orange::preview::clear_texture(
            &capture.texture,
            &capture.texture_width,
            &capture.texture_height);
    }
    ui_state->group_captures.clear();
    ui_state->pending_group_snapshot_requests.clear();
}

bool apply_group_capture_to_active_preview(
    SpatialLayoutUiState* ui_state,
    const SpatialLayoutGroupCaptureFrame& capture,
    std::string* error_out)
{
    SpatialSnapshotResult result;
    result.ok = capture.valid;
    result.camera_serial = capture.camera_serial;
    result.capture_mode = capture.capture_mode;
    result.source_array_role = capture.source_array_role;
    result.width = capture.width;
    result.height = capture.height;
    result.completed_frame_count = std::max<uint32_t>(1u, capture.source_frame_count);
    result.first_local_frame_id = capture.first_local_frame_id;
    result.last_local_frame_id = capture.last_local_frame_id;
    result.first_camera_frame_id = capture.first_camera_frame_id;
    result.last_camera_frame_id = capture.last_camera_frame_id;
    result.local_frame_id = capture.last_local_frame_id;
    result.camera_frame_id = capture.last_camera_frame_id;
    result.rgba = capture.rgba;
    const bool ok = apply_full_resolution_stream_snapshot(ui_state, result, error_out);
    if (ok) {
        ui_state->captured_capture_group_id = capture.capture_group_id;
        set_captured_citrus_projection_snapshots(
            ui_state,
            capture.metadata.citrus_projection_snapshot_pre_capture,
            capture.metadata.citrus_projection_snapshot_post_capture);
        ui_state->captured_citrus_projection_epoch_consistency =
            capture.metadata.citrus_projection_epoch_consistency.is_object()
                ? capture.metadata.citrus_projection_epoch_consistency
                : nlohmann::json::object();
        ui_state->preview_status =
            "Showing grouped full-resolution capture from " + capture.camera_serial +
            " (" + capture.capture_group_id + ").";
    }
    return ok;
}

bool apply_session_review_image_to_active_preview(
    SpatialLayoutUiState* ui_state,
    CameraParams* cameras_params,
    int num_cameras,
    std::string* error_out)
{
    if (ui_state == nullptr) {
        if (error_out) {
            *error_out = "Spatial layout UI state is null.";
        }
        return false;
    }
    if (ui_state->selected_session_review_image < 0 ||
        ui_state->selected_session_review_image >=
            static_cast<int>(ui_state->session_review_images.size())) {
        if (error_out) {
            *error_out = "No calibration session image is selected.";
        }
        return false;
    }
    const SpatialLayoutSessionReviewImage& entry =
        ui_state->session_review_images[
            static_cast<size_t>(ui_state->selected_session_review_image)];
    if (!entry.valid || entry.image_path.empty()) {
        if (error_out) {
            *error_out = "Selected calibration session image is not loadable.";
        }
        return false;
    }

    std::vector<unsigned char> rgba;
    int width = 0;
    int height = 0;
    if (!load_rgba_image_from_path(entry.image_path, &rgba, &width, &height, error_out)) {
        return false;
    }

    std::string texture_error;
    if (!orange::preview::update_rgba_texture(
            &ui_state->captured_texture,
            &ui_state->captured_texture_width,
            &ui_state->captured_texture_height,
            rgba,
            width,
            height,
            &texture_error)) {
        if (error_out) {
            *error_out = texture_error;
        }
        return false;
    }

    const int camera_index =
        find_camera_index_by_serial(cameras_params, num_cameras, entry.camera_serial);
    if (camera_index >= 0) {
        ui_state->selected_camera = camera_index;
        ui_state->configured_camera_index = camera_index;
    }

    ui_state->captured_rgba = std::move(rgba);
    ui_state->captured_camera_serial = entry.camera_serial;
    ui_state->captured_source_array_role =
        entry.source_array_role.empty() ? "images_full" : entry.source_array_role;
    ui_state->captured_capture_mode =
        entry.capture_mode.empty() ? "loaded_calibration_session_image" : entry.capture_mode;
    ui_state->captured_capture_group_id = entry.capture_group_id;
    ui_state->captured_source_frame_count = 1;
    ui_state->captured_first_local_frame_id = 0;
    ui_state->captured_last_local_frame_id = 0;
    ui_state->captured_first_camera_frame_id = 0;
    ui_state->captured_last_camera_frame_id = 0;
    ui_state->has_capture = true;
    ui_state->captured_canvas_view.fit_requested = true;
    ui_state->preview_error.clear();
    clear_detected_experimental_area_circle(ui_state);
    apply_calibration_image_set_metadata_to_ui(ui_state, entry.metadata);

    if (entry.has_accepted_circle && entry.accepted_circle_r > 0.0) {
        ui_state->has_detected_experimental_area_circle = true;
        ui_state->detected_experimental_area_geometry =
            runtime_circle(
                entry.accepted_circle_cx,
                entry.accepted_circle_cy,
                entry.accepted_circle_r);
        ui_state->detection_status =
            "Loaded accepted top-rim circle from " + entry.artifact_id + ".";
    }

    reset_registration_from_frame(ui_state);

    std::ostringstream status;
    status << "Loaded calibration session image " << width << "x" << height
           << " from " << entry.image_path;
    if (!entry.camera_serial.empty()) {
        status << " for Cam" << entry.camera_serial;
    }
    ui_state->preview_status = status.str();
    return true;
}

bool consume_group_snapshot_result(
    SpatialLayoutUiState* ui_state,
    const SpatialSnapshotResult& result,
    const CameraParams* cameras_params,
    int num_cameras,
    int selected_camera_index)
{
    if (ui_state == nullptr || ui_state->pending_group_snapshot_requests.empty()) {
        return false;
    }

    for (SpatialLayoutPendingGroupSnapshotRequest& request :
         ui_state->pending_group_snapshot_requests) {
        if (request.camera_serial != result.camera_serial ||
            request.request_id != result.request_id ||
            request.completed ||
            request.failed) {
            continue;
        }

        if (result.ok && result.width > 0 && result.height > 0 && !result.rgba.empty()) {
            SpatialLayoutCalibrationImageSetMetadata capture_metadata =
                ui_state->group_capture_metadata;
            const CitrusProjectionSnapshotQueryResult post_snapshot =
                query_citrus_active_projection_snapshot(
                    "post_capture",
                    ui_state->group_capture_id + "_Cam" + result.camera_serial);
            capture_metadata.citrus_projection_snapshot_post_capture =
                post_snapshot.ok ? post_snapshot.snapshot : nlohmann::json::object();
            capture_metadata.citrus_projection_epoch_consistency =
                nlohmann::json::object();
            SpatialLayoutGroupCaptureFrame capture =
                make_group_capture_from_snapshot(
                    result,
                    cameras_params,
                    num_cameras,
                    ui_state->group_capture_id,
                    ui_state->group_capture_mode,
                    capture_metadata);
            std::string texture_error;
            if (!orange::preview::update_rgba_texture(
                    &capture.texture,
                    &capture.texture_width,
                    &capture.texture_height,
                    capture.rgba,
                    capture.width,
                    capture.height,
                    &texture_error)) {
                request.failed = true;
                request.completed = false;
                request.error = texture_error.empty()
                                    ? "Grouped capture texture upload failed."
                                    : texture_error;
            } else {
                request.completed = true;
                upsert_group_capture(ui_state, capture);
                if (selected_camera_index >= 0 &&
                    selected_camera_index < num_cameras &&
                    cameras_params[selected_camera_index].camera_serial == result.camera_serial) {
                    std::string preview_error;
                    if (!apply_group_capture_to_active_preview(ui_state, capture, &preview_error)) {
                        ui_state->preview_error = preview_error;
                    }
                }
            }
        } else {
            request.failed = true;
            request.error = result.error.empty()
                                ? "Grouped full-resolution snapshot failed."
                                : result.error;
        }

        const int pending = pending_group_snapshot_count(*ui_state);
        const int failed = failed_group_snapshot_count(*ui_state);
        std::ostringstream status;
        status << "Grouped capture " << ui_state->group_capture_id
               << ": completed=" << ui_state->group_captures.size()
               << " pending=" << pending
               << " failed=" << failed << ".";
        ui_state->group_capture_status = status.str();
        if (failed > 0) {
            std::ostringstream error;
            for (const SpatialLayoutPendingGroupSnapshotRequest& pending_request :
                 ui_state->pending_group_snapshot_requests) {
                if (!pending_request.failed) {
                    continue;
                }
                if (error.tellp() > 0) {
                    error << " ";
                }
                error << pending_request.camera_serial << ": "
                      << (pending_request.error.empty()
                              ? "capture failed"
                              : pending_request.error);
            }
            ui_state->group_capture_error = error.str();
        } else {
            ui_state->group_capture_error.clear();
        }
        if (pending == 0 && failed == 0) {
            ui_state->group_capture_status =
                "Grouped capture " + ui_state->group_capture_id +
                " complete for " + std::to_string(ui_state->group_captures.size()) +
                " camera(s) as " + ui_state->group_capture_metadata.image_set_purpose +
                " on " + ui_state->group_capture_metadata.image_set_target_plane + ".";
        }
        return true;
    }
    return false;
}

int eligible_group_capture_camera_count(
    const CameraEachSelect* cameras_select,
    SpatialSnapshotWorker* const* spatial_snapshot_workers,
    int num_cameras)
{
    int count = 0;
    for (int i = 0; i < num_cameras; ++i) {
        if (camera_is_group_capture_eligible(cameras_select, spatial_snapshot_workers, i)) {
            ++count;
        }
    }
    return count;
}

bool request_group_full_resolution_snapshots(
    SpatialLayoutUiState* ui_state,
    const CameraParams* cameras_params,
    const CameraEachSelect* cameras_select,
    int num_cameras,
    SpatialSnapshotWorker* const* spatial_snapshot_workers,
    uint32_t target_frame_count,
    std::string* error_out)
{
    if (ui_state == nullptr || cameras_params == nullptr || cameras_select == nullptr ||
        spatial_snapshot_workers == nullptr || num_cameras <= 0) {
        if (error_out) {
            *error_out = "Grouped capture requires open cameras and snapshot workers.";
        }
        return false;
    }
    if (pending_group_snapshot_count(*ui_state) > 0) {
        if (error_out) {
            *error_out = "A grouped capture is already pending.";
        }
        return false;
    }

    clear_group_captures(ui_state);
    ui_state->group_capture_error.clear();

    const std::string timestamp = get_current_utc_timestamp();
    ui_state->group_capture_metadata =
        make_calibration_image_set_metadata_from_ui(*ui_state);
    ui_state->group_capture_id =
        build_group_capture_id(*ui_state, ui_state->group_capture_metadata, timestamp);
    ui_state->group_capture_mode =
        target_frame_count > 1 ? "operator_group_temporal_mean" : "operator_group_next_frame";
    const CitrusProjectionSnapshotQueryResult pre_snapshot =
        query_citrus_active_projection_snapshot(
            "pre_capture",
            ui_state->group_capture_id);
    ui_state->group_capture_metadata.citrus_projection_snapshot_pre_capture =
        pre_snapshot.ok ? pre_snapshot.snapshot : nlohmann::json::object();
    ui_state->group_capture_metadata.citrus_projection_snapshot_post_capture =
        nlohmann::json::object();
    ui_state->group_capture_metadata.citrus_projection_epoch_consistency =
        nlohmann::json::object();

    int requested = 0;
    std::ostringstream request_errors;
    for (int camera_index = 0; camera_index < num_cameras; ++camera_index) {
        if (!camera_is_group_capture_eligible(cameras_select, spatial_snapshot_workers, camera_index)) {
            continue;
        }

        SpatialSnapshotWorker* worker = spatial_snapshot_workers[camera_index];
        uint64_t request_id = 0;
        std::string request_error;
        std::ostringstream operation_id;
        operation_id << ui_state->group_capture_id
                     << "_Cam" << cameras_params[camera_index].camera_serial;
        if (!worker->RequestSnapshot(
                operation_id.str(),
                &request_id,
                &request_error,
                std::max<uint32_t>(1u, target_frame_count))) {
            if (request_errors.tellp() > 0) {
                request_errors << " ";
            }
            request_errors << cameras_params[camera_index].camera_serial
                           << ": "
                           << (request_error.empty()
                                   ? "request rejected"
                                   : request_error);
            SpatialLayoutPendingGroupSnapshotRequest failed_request;
            failed_request.camera_serial = cameras_params[camera_index].camera_serial;
            failed_request.failed = true;
            failed_request.error = request_error.empty() ? "request rejected" : request_error;
            ui_state->pending_group_snapshot_requests.push_back(std::move(failed_request));
            continue;
        }

        SpatialLayoutPendingGroupSnapshotRequest pending_request;
        pending_request.camera_serial = cameras_params[camera_index].camera_serial;
        pending_request.request_id = request_id;
        ui_state->pending_group_snapshot_requests.push_back(std::move(pending_request));
        ++requested;
    }

    if (requested == 0) {
        ui_state->group_capture_status.clear();
        ui_state->group_capture_error =
            request_errors.tellp() > 0
                ? request_errors.str()
                : "No streaming cameras with spatial snapshot workers are available.";
        if (error_out) {
            *error_out = ui_state->group_capture_error;
        }
        return false;
    }

    std::ostringstream status;
    status << "Requested grouped full-resolution capture "
           << ui_state->group_capture_id
           << " from " << requested << " camera(s)"
           << " as " << ui_state->group_capture_metadata.image_set_purpose
           << " on " << ui_state->group_capture_metadata.image_set_target_plane;
    if (target_frame_count > 1) {
        status << " averaging " << target_frame_count << " frames";
    }
    status << ".";
    ui_state->group_capture_status = status.str();
    ui_state->group_capture_error =
        request_errors.tellp() > 0 ? request_errors.str() : std::string();
    return true;
}

}  // namespace orange::gui::spatial_layout
