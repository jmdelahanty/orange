#pragma once

#include "gui/arena_centering_analysis.h"
#include "gui/spatial_layout/state.h"
#include "json.hpp"
#include "spatial_snapshot_worker.h"
#include "video_capture.h"

#include <chrono>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace orange::gui {

enum class ArenaCenteringAutorunStage {
    kDisabled = 0,
    kWaitForStream,
    kPrepare,
    kWaitForPreflightSettle,
    kBeginTransaction,
    kWaitForBeginPresented,
    kWaitForProjectionSettle,
    kRequestCapture,
    kWaitForCapture,
    kWaitForStabilityConfirmation,
    kAnalyzeCapture,
    kQueueSave,
    kWaitForSave,
    kAdvance,
    kCommitOrAbort,
    kWaitForTerminalReceipt,
    kRequestHomographyCapture,
    kWaitForHomographyCapture,
    kQueueHomographySave,
    kWaitForHomographySave,
    kRequestHomographyFit,
    kWaitForHomographyFit,
    kPromoteHomography,
    kWaitForHomographyPromotion,
    kRestorePreflight,
    kStopStream,
    kWaitForStreamStop,
    kWriteResult,
    kDone,
    kFailed,
};

struct ArenaCenteringAutorunConfig {
    bool enabled = false;
    bool save_captures = true;
    bool save_verified_centers_armed = false;
    bool resize_arenas = false;
    bool save_verified_layout_armed = false;
    bool fit_homographies_after_centering = false;
    bool accept_homographies_armed = false;
    bool exit_after_completion = true;
    std::string citrus_config_path;
    std::vector<std::string> camera_serials;
    std::uint32_t frame_count = 1;
    std::uint8_t foreground_gray_u8 = 72;
    int symmetric_probe_canvas_px = 3;
    double verification_tolerance_camera_px = 2.0;
    double maximum_refinement_canvas_px = 4.0;
    double rectangle_safety_margin_camera_px = 32.0;
    // Proposals are fitted from one capture and verified on a later capture.
    // This reserve absorbs observed edge-fit jitter while the safety margin
    // remains the strict measured acceptance gate.
    double rectangle_prediction_reserve_camera_px = 8.0;
    double rectangle_center_marker_tolerance_camera_px = 20.0;
    double rectangle_prediction_tolerance_camera_px = 20.0;
    double rectangle_maximality_slack_camera_px = 24.0;
    double maximum_arena_scale_change_fraction = 0.20;
    std::uint64_t maximum_ptp_capture_span_ns = 1000;
    bool apply_calibration_preflight = true;
    std::uint32_t calibration_frame_rate_hz = 5;
    std::uint32_t calibration_exposure_us = 100000;
    int preflight_settle_milliseconds = 1000;
    int projection_settle_milliseconds = 1000;
    bool require_projection_stability_capture = true;
    int projection_stability_interval_milliseconds = 300;
    int projection_stability_max_capture_attempts = 5;
    double projection_stability_max_center_delta_camera_px = 2.0;
    double projection_stability_max_corner_delta_camera_px = 12.0;
    double homography_maximum_rms_reprojection_error_canvas_px = 0.5;
    double homography_maximum_point_reprojection_error_canvas_px = 1.5;
    double homography_minimum_inlier_ratio = 0.95;
    double homography_maximum_holdout_rms_error_canvas_px = 0.75;
    double homography_maximum_holdout_error_canvas_px = 2.0;
    int homography_saturation_pixel_threshold_u8 = 250;
    double homography_maximum_dot_core_saturation_fraction = 0.005;
    double homography_minimum_dot_background_contrast_u8 = 20.0;
    std::string projector_intensity_report_path;
    std::string projector_intensity_report_sha256;
    int startup_timeout_seconds = 120;
    int workflow_timeout_seconds = 90;
    std::string result_json_path;
};

struct ArenaCenteringTargetState {
    std::string camera_serial;
    std::string arena_id;
    int original_center_x_canvas_px = 0;
    int original_center_y_canvas_px = 0;
    int candidate_center_x_canvas_px = 0;
    int candidate_center_y_canvas_px = 0;
    int original_width_canvas_px = 0;
    int original_height_canvas_px = 0;
    int candidate_width_canvas_px = 0;
    int candidate_height_canvas_px = 0;
};

struct ArenaCenteringAutorunState {
    ArenaCenteringAutorunStage stage = ArenaCenteringAutorunStage::kDisabled;
    std::chrono::steady_clock::time_point run_started_at{};
    std::chrono::steady_clock::time_point stage_started_at{};
    bool action_requested = false;
    bool run_passed = false;
    bool transaction_started = false;
    bool transaction_terminal = false;
    bool commit_requested = false;
    bool abort_requested = false;
    bool verification_passed = false;
    bool refinement_attempted = false;
    bool result_written = false;
    bool preflight_applied = false;
    bool preflight_restore_attempted = false;
    bool preflight_restore_ok = false;
    bool homography_fit_requested = false;
    bool homography_promotion_requested = false;
    bool homography_committed = false;
    std::string transaction_id;
    std::string base_canvas_checksum;
    std::string current_stage_id;
    std::string current_operation_id;
    std::string terminal_intent;
    std::string error_message;
    // A completed capture/analysis gate may fail after useful evidence already
    // exists in memory. Keep the transaction fail-closed while allowing that
    // one owned batch and its overlays to reach the calibration session before
    // entering the normal abort path.
    std::string pending_abort_after_persistence_reason;
    std::string result_write_error;
    std::string homography_transaction_id;
    std::string homography_capture_group_id;
    std::string committed_canvas_checksum;
    std::string preflight_prepare_status;
    std::string preflight_restore_status;
    nlohmann::json capture_camera_settings = nlohmann::json::array();
    nlohmann::json stage_records = nlohmann::json::array();
    nlohmann::json solve_results = nlohmann::json::object();
    nlohmann::json verification = nlohmann::json::object();
    nlohmann::json terminal_status = nlohmann::json::object();
    nlohmann::json homography_candidate_status = nlohmann::json::object();
    nlohmann::json homography_verification = nlohmann::json::object();
    nlohmann::json pending_detection_batch = nlohmann::json::object();
    nlohmann::json stability_reference_capture = nlohmann::json::object();
    nlohmann::json stability_capture_rejections = nlohmann::json::array();
    nlohmann::json resize_proposals = nlohmann::json::object();
    int canvas_width_px = 0;
    int canvas_height_px = 0;
    std::vector<ArenaCenteringTargetState> targets;
    std::map<std::string,
             std::map<std::string, arena_centering::FiducialDetection>>
        detections_by_stage_and_camera;
    std::map<std::string,
             std::map<std::string, arena_centering::RectangleBoundaryDetection>>
        rectangles_by_stage_and_camera;
    bool stability_reference_ready = false;
    int stability_capture_attempts = 0;
    std::string stability_reference_stage_id;
    std::string stability_reference_capture_group_id;
    std::map<std::string, arena_centering::FiducialDetection>
        stability_reference_detections;
    std::map<std::string, arena_centering::RectangleBoundaryDetection>
        stability_reference_rectangles;
    std::map<std::string, arena_centering::CenteringSolveResult>
        solves_by_camera;
    std::map<std::string, arena_centering::MaximalSquareProposal>
        resize_proposals_by_camera;
};

struct ArenaCenteringAutorunRequests {
    bool toggle_streaming = false;
    bool close_window = false;
};

ArenaCenteringAutorunConfig resolve_arena_centering_autorun_config();

const char* arena_centering_autorun_stage_name(ArenaCenteringAutorunStage stage);

void arena_centering_autorun_start(
    ArenaCenteringAutorunState* state,
    const ArenaCenteringAutorunConfig& config);

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
    const std::string& calibration_sessions_root);

nlohmann::json arena_centering_autorun_result_json(
    const ArenaCenteringAutorunState& state,
    const ArenaCenteringAutorunConfig& config,
    const SpatialLayoutUiState& spatial_state,
    bool stream_stopped);

}  // namespace orange::gui
