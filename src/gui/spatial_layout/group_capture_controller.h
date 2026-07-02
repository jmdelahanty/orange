#pragma once

#include "gui/spatial_layout/state.h"
#include "spatial_snapshot_worker.h"

#include <cstdint>
#include <string>

namespace orange::gui::spatial_layout {

int pending_group_snapshot_count(const SpatialLayoutUiState& ui_state);

int failed_group_snapshot_count(const SpatialLayoutUiState& ui_state);

int find_camera_index_by_serial(
    const CameraParams* cameras_params,
    int num_cameras,
    const std::string& camera_serial);

void clear_group_captures(SpatialLayoutUiState* ui_state);

bool apply_group_capture_to_active_preview(
    SpatialLayoutUiState* ui_state,
    const SpatialLayoutGroupCaptureFrame& capture,
    std::string* error_out);

bool apply_session_review_image_to_active_preview(
    SpatialLayoutUiState* ui_state,
    CameraParams* cameras_params,
    int num_cameras,
    std::string* error_out);

bool consume_group_snapshot_result(
    SpatialLayoutUiState* ui_state,
    const SpatialSnapshotResult& result,
    const CameraParams* cameras_params,
    int num_cameras,
    int selected_camera_index);

int eligible_group_capture_camera_count(
    const CameraEachSelect* cameras_select,
    SpatialSnapshotWorker* const* spatial_snapshot_workers,
    int num_cameras);

bool request_group_full_resolution_snapshots(
    SpatialLayoutUiState* ui_state,
    const CameraParams* cameras_params,
    const CameraEachSelect* cameras_select,
    int num_cameras,
    SpatialSnapshotWorker* const* spatial_snapshot_workers,
    uint32_t target_frame_count,
    std::string* error_out);

}  // namespace orange::gui::spatial_layout
