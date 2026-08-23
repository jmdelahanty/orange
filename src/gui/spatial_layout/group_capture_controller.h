#pragma once

#include "gui/spatial_layout/state.h"
#include "spatial_snapshot_worker.h"

#include <cstdint>
#include <string>

namespace orange::gui::spatial_layout {

int pending_group_snapshot_count(const SpatialLayoutUiState& ui_state);

int failed_group_snapshot_count(const SpatialLayoutUiState& ui_state);

bool group_capture_workflow_active(const SpatialLayoutUiState& ui_state);

std::string resolve_group_capture_scene_recipe(const SpatialLayoutUiState& ui_state);

void initialize_group_capture_camera_scope(
    SpatialLayoutUiState* ui_state,
    const CameraParams* cameras_params,
    const CameraEachSelect* cameras_select,
    SpatialSnapshotWorker* const* spatial_snapshot_workers,
    int num_cameras);

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
    std::string* error_out,
    const std::string& transaction_id_override = std::string(),
    const std::string& operation_id_override = std::string(),
    const std::string& parent_transaction_owner_kind = std::string());

// Captures fresh full-resolution frames from the selected streaming cameras
// without contacting Citrus or requiring a canvas/arena mapping. The current
// camera, illumination, filter, dish, and projector states are recorded as
// metadata; this function does not change them.
bool request_group_full_resolution_snapshots_camera_only(
    SpatialLayoutUiState* ui_state,
    const CameraParams* cameras_params,
    const CameraEachSelect* cameras_select,
    int num_cameras,
    SpatialSnapshotWorker* const* spatial_snapshot_workers,
    uint32_t target_frame_count,
    std::string* error_out,
    const std::string& transaction_id_override = std::string(),
    const std::string& operation_id_override = std::string(),
    const std::string& parent_transaction_owner_kind = std::string());

bool request_group_full_resolution_snapshots_for_arena_centering(
    SpatialLayoutUiState* ui_state,
    const CameraParams* cameras_params,
    const CameraEachSelect* cameras_select,
    int num_cameras,
    SpatialSnapshotWorker* const* spatial_snapshot_workers,
    uint32_t target_frame_count,
    const std::string& centering_transaction_id,
    const std::string& centering_stage_id,
    const std::string& centering_operation_id,
    std::string* error_out,
    const std::string& parent_transaction_owner_kind);

// Captures a transient candidate preview that was already presented by the
// Citrus daily-registration controller. Unlike a normal calibration-scene
// capture this does not set or restore the scene: accept/reject/abort owns that
// lifecycle.
bool request_group_full_resolution_snapshots_for_daily_registration_preview(
    SpatialLayoutUiState* ui_state,
    const CameraParams* cameras_params,
    const CameraEachSelect* cameras_select,
    int num_cameras,
    SpatialSnapshotWorker* const* spatial_snapshot_workers,
    uint32_t target_frame_count,
    const std::string& daily_transaction_id,
    const std::string& candidate_sha256,
    const std::string& preview_operation_id,
    std::string* error_out);

void advance_group_capture_workflow(
    SpatialLayoutUiState* ui_state,
    const CameraParams* cameras_params,
    const CameraEachSelect* cameras_select,
    int num_cameras,
    SpatialSnapshotWorker* const* spatial_snapshot_workers);

}  // namespace orange::gui::spatial_layout
