#include "gui/spatial_layout/preview_capture.h"

#include "camera_preview_utils.h"
#include "gui/spatial_layout/calibration_transaction_bridge.h"
#include "gui/spatial_layout/layout_state.h"
#include "gui/spatial_layout/projection_snapshot_client.h"

#include <algorithm>
#include <iomanip>
#include <sstream>
#include <utility>
#include <vector>

namespace orange::gui::spatial_layout {
namespace {

constexpr int kSpatialCaptureBufferCount = 2;

}  // namespace

bool capture_single_camera_frame(
    SpatialLayoutUiState* ui_state,
    CameraEmergent* ecams,
    CameraParams* cameras_params,
    std::string* error_out)
{
    if (ui_state == nullptr || ecams == nullptr || cameras_params == nullptr) {
        if (error_out) {
            *error_out = "Capture frame received invalid state or camera pointers.";
        }
        return false;
    }

    const int selected_camera = ui_state->selected_camera;
    CameraEmergent* ecam = &ecams[selected_camera];
    CameraParams* camera_params = &cameras_params[selected_camera];

    bool owns_transaction = false;
    std::string transaction_error;
    if (ui_state->calibration_transaction_lease) {
        if (!spatial_calibration_transaction_owned_by(
                *ui_state, kManualCameraPreflightTransactionOwner) ||
            !require_spatial_calibration_transaction(
                *ui_state,
                {camera_params->camera_serial},
                orange::calibration::Mutation::kCameraStreamLifecycle,
                &transaction_error)) {
            if (transaction_error.empty()) {
                transaction_error =
                    "Direct still capture cannot borrow the active calibration transaction.";
            }
            if (error_out) *error_out = transaction_error;
            return false;
        }
    } else {
        owns_transaction = acquire_spatial_calibration_transaction(
            ui_state,
            kSpatialDirectCaptureTransactionOwner,
            std::string(kSpatialDirectCaptureTransactionOwner) + "_Cam" +
                camera_params->camera_serial,
            orange::calibration::WorkflowKind::kSpatialDirectCapture,
            {camera_params->camera_serial},
            orange::calibration::mutation_set(
                orange::calibration::Mutation::kCameraStreamLifecycle),
            "Open one camera stream, capture one calibration still, and restore the stream state.",
            &transaction_error);
        if (!owns_transaction) {
            if (error_out) *error_out = transaction_error;
            return false;
        }
    }

    const CitrusProjectionSnapshotQueryResult pre_snapshot =
        query_citrus_active_projection_snapshot(
            "pre_capture",
            "single_camera_direct_still_" + camera_params->camera_serial);

    int dropped_frames = 0;
    int width = 0;
    int height = 0;
    std::string capture_error;
    if (!orange::preview::capture_single_frame_rgba(
            &ecam->camera,
            camera_params,
            kSpatialCaptureBufferCount,
            1000,
            &ui_state->captured_rgba,
            &width,
            &height,
            &dropped_frames,
            &capture_error)) {
        if (error_out) {
            *error_out = capture_error;
        }
        if (owns_transaction) {
            release_spatial_calibration_transaction(
                ui_state, "failed", capture_error);
        }
        return false;
    }

    const CitrusProjectionSnapshotQueryResult post_snapshot =
        query_citrus_active_projection_snapshot(
            "post_capture",
            "single_camera_direct_still_" + camera_params->camera_serial);

    orange::preview::update_rgba_texture(
        &ui_state->captured_texture,
        &ui_state->captured_texture_width,
        &ui_state->captured_texture_height,
        ui_state->captured_rgba,
        width,
        height);
    clear_detected_experimental_area_circle(ui_state);
    ui_state->has_capture = true;
    ui_state->captured_camera_serial = camera_params->camera_serial;
    ui_state->captured_source_array_role = "images_full";
    ui_state->captured_capture_mode = "single_camera_direct_still";
    ui_state->captured_capture_group_id.clear();
    ui_state->captured_source_frame_count = 1;
    ui_state->captured_first_local_frame_id = 0;
    ui_state->captured_last_local_frame_id = 0;
    ui_state->captured_first_camera_frame_id = 0;
    ui_state->captured_last_camera_frame_id = 0;
    ui_state->preview_error.clear();
    set_captured_citrus_projection_snapshots(
        ui_state,
        pre_snapshot.ok ? pre_snapshot.snapshot : nlohmann::json::object(),
        post_snapshot.ok ? post_snapshot.snapshot : nlohmann::json::object());

    std::ostringstream status;
    status << "Captured " << width << "x" << height << " from " << camera_params->camera_serial;
    if (dropped_frames > 0) {
        status << " (dropped " << dropped_frames << " buffered frames)";
    }
    ui_state->preview_status = status.str();
    if (owns_transaction) {
        release_spatial_calibration_transaction(
            ui_state, "complete", ui_state->preview_status);
    }
    return true;
}

bool capture_live_stream_preview_texture(
    SpatialLayoutUiState* ui_state,
    const CameraParams& camera_params,
    const CameraEachSelect& camera_select,
    GLuint preview_texture,
    std::string* error_out)
{
    if (ui_state == nullptr) {
        if (error_out) {
            *error_out = "Live stream snapshot received a null UI state.";
        }
        return false;
    }
    if (!camera_select.stream_on || preview_texture == 0) {
        if (error_out) {
            *error_out = "Start streaming and wait for a live preview frame before taking a live stream snapshot.";
        }
        return false;
    }

    const CitrusProjectionSnapshotQueryResult pre_snapshot =
        query_citrus_active_projection_snapshot(
            "pre_capture",
            "live_stream_preview_snapshot_" + camera_params.camera_serial);

    const int width = std::max(1, static_cast<int>(camera_params.width) / std::max(1, camera_select.downsample));
    const int height = std::max(1, static_cast<int>(camera_params.height) / std::max(1, camera_select.downsample));
    std::vector<unsigned char> rgba(static_cast<size_t>(width) * static_cast<size_t>(height) * 4u);

    GLint previous_texture = 0;
    GLint previous_pack_alignment = 4;
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &previous_texture);
    glGetIntegerv(GL_PACK_ALIGNMENT, &previous_pack_alignment);
    glBindTexture(GL_TEXTURE_2D, preview_texture);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
    const GLenum gl_status = glGetError();
    glPixelStorei(GL_PACK_ALIGNMENT, previous_pack_alignment);
    glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(previous_texture));
    if (gl_status != GL_NO_ERROR) {
        if (error_out) {
            std::ostringstream oss;
            oss << "Failed to read live preview texture for " << camera_params.camera_serial
                << " (GL error 0x" << std::hex << gl_status << ").";
            *error_out = oss.str();
        }
        return false;
    }

    const CitrusProjectionSnapshotQueryResult post_snapshot =
        query_citrus_active_projection_snapshot(
            "post_capture",
            "live_stream_preview_snapshot_" + camera_params.camera_serial);

    ui_state->captured_rgba = std::move(rgba);
    ui_state->captured_texture_width = width;
    ui_state->captured_texture_height = height;
    ui_state->captured_camera_serial = camera_params.camera_serial;
    ui_state->captured_source_array_role =
        camera_select.downsample > 1 ? "images_ds" : "images_full";
    ui_state->captured_capture_mode = "live_stream_preview_snapshot";
    ui_state->captured_capture_group_id.clear();
    ui_state->captured_source_frame_count = 1;
    ui_state->captured_first_local_frame_id = 0;
    ui_state->captured_last_local_frame_id = 0;
    ui_state->captured_first_camera_frame_id = 0;
    ui_state->captured_last_camera_frame_id = 0;
    ui_state->has_capture = true;
    ui_state->preview_error.clear();
    clear_detected_experimental_area_circle(ui_state);
    set_captured_citrus_projection_snapshots(
        ui_state,
        pre_snapshot.ok ? pre_snapshot.snapshot : nlohmann::json::object(),
        post_snapshot.ok ? post_snapshot.snapshot : nlohmann::json::object());

    std::string texture_error;
    if (!orange::preview::update_rgba_texture(
            &ui_state->captured_texture,
            &ui_state->captured_texture_width,
            &ui_state->captured_texture_height,
            ui_state->captured_rgba,
            width,
            height,
            &texture_error)) {
        if (error_out) {
            *error_out = texture_error;
        }
        return false;
    }

    reset_registration_from_frame(ui_state);

    std::ostringstream status;
    status << "Captured live stream preview " << width << "x" << height
           << " from " << camera_params.camera_serial
           << " as " << ui_state->captured_source_array_role;
    if (camera_select.downsample > 1) {
        status << " (display downsample " << camera_select.downsample << "x)";
    }
    ui_state->preview_status = status.str();
    return true;
}

bool apply_full_resolution_stream_snapshot(
    SpatialLayoutUiState* ui_state,
    const SpatialSnapshotResult& result,
    std::string* error_out)
{
    if (ui_state == nullptr) {
        if (error_out) {
            *error_out = "Full-resolution stream snapshot received a null UI state.";
        }
        return false;
    }
    if (!result.ok) {
        if (error_out) {
            *error_out = result.error.empty()
                             ? "Full-resolution stream snapshot failed."
                             : result.error;
        }
        return false;
    }
    if (result.width <= 0 || result.height <= 0 || result.rgba.empty()) {
        if (error_out) {
            *error_out = "Full-resolution stream snapshot returned an empty image.";
        }
        return false;
    }

    ui_state->captured_rgba = result.rgba;
    ui_state->captured_camera_serial = result.camera_serial;
    ui_state->captured_source_array_role = "images_full";
    ui_state->captured_capture_mode =
        result.capture_mode.empty() ? "full_resolution_stream_snapshot" : result.capture_mode;
    ui_state->captured_capture_group_id.clear();
    ui_state->captured_source_frame_count =
        std::max<uint32_t>(1u, result.completed_frame_count);
    ui_state->captured_first_local_frame_id = result.first_local_frame_id;
    ui_state->captured_last_local_frame_id = result.last_local_frame_id;
    ui_state->captured_first_camera_frame_id = result.first_camera_frame_id;
    ui_state->captured_last_camera_frame_id = result.last_camera_frame_id;
    ui_state->has_capture = true;
    ui_state->pending_full_res_snapshot_request_id = 0;
    ui_state->pending_full_res_snapshot_camera_serial.clear();
    ui_state->pending_full_res_snapshot_target_frame_count = 1;
    ui_state->preview_error.clear();
    clear_detected_experimental_area_circle(ui_state);

    std::string texture_error;
    if (!orange::preview::update_rgba_texture(
            &ui_state->captured_texture,
            &ui_state->captured_texture_width,
            &ui_state->captured_texture_height,
            ui_state->captured_rgba,
            result.width,
            result.height,
            &texture_error)) {
        if (error_out) {
            *error_out = texture_error;
        }
        return false;
    }

    reset_registration_from_frame(ui_state);

    std::ostringstream status;
    status << "Captured full-resolution stream snapshot "
           << result.width << "x" << result.height
           << " from " << result.camera_serial
           << " frame=" << result.local_frame_id
           << " camera_frame=" << result.camera_frame_id;
    if (ui_state->captured_source_frame_count > 1) {
        status << " averaged_frames=" << ui_state->captured_source_frame_count
               << " local_frame_range=" << ui_state->captured_first_local_frame_id
               << "-" << ui_state->captured_last_local_frame_id
               << " camera_frame_range=" << ui_state->captured_first_camera_frame_id
               << "-" << ui_state->captured_last_camera_frame_id;
    }
    ui_state->preview_status = status.str();
    return true;
}

}  // namespace orange::gui::spatial_layout
