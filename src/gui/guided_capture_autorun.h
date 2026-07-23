#pragma once

#include "gui/spatial_layout/state.h"
#include "json.hpp"
#include "spatial_snapshot_worker.h"
#include "video_capture.h"

#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

namespace orange::gui {

enum class GuidedCaptureAutorunStage {
    kDisabled = 0,
    kWaitForStream,
    kPrepare,
    kWaitForPreflightSettle,
    kWaitForCapture,
    kQueueSave,
    kWaitForSave,
    kRequestHomographyFit,
    kWaitForHomographyFit,
    kReleaseHomographyCandidate,
    kWaitForHomographyRelease,
    kAnalyzeProjectedSurfaceScale,
    kRequestProjectedSurfaceScaleFit,
    kWaitForProjectedSurfaceScaleFit,
    kWaitForProjectedSurfaceScaleReview,
    kWaitForProjectedSurfaceScalePromotion,
    kStopStream,
    kWaitForStreamStop,
    kWriteResult,
    kDone,
    kFailed
};

struct GuidedCaptureAutorunConfig {
    bool enabled = false;
    bool save_captures = false;
    bool exit_after_completion = true;
    std::string citrus_config_path;
    std::string workflow_profile_id;
    std::string recipe = "black_reference";
    std::string purpose;
    std::vector<std::string> camera_serials;
    std::uint32_t frame_count = 1;
    bool apply_calibration_preflight = true;
    std::uint32_t calibration_frame_rate_hz = 5;
    std::uint32_t calibration_exposure_us = 100000;
    std::uint8_t foreground_gray_u8 = 255;
    std::vector<std::string> recipe_sequence;
    std::string fixture_aperture_shape = "circle";
    std::vector<std::uint8_t> sweep_foreground_grays_u8;
    int sweep_repeats = 1;
    bool include_arena_outline_reference = false;
    bool projected_surface_targets_ready_confirmed = false;
    bool accept_projected_surface_scales_armed = false;
    bool fit_homographies_after_capture = false;
    int preflight_settle_milliseconds = 1000;
    int startup_timeout_seconds = 120;
    int workflow_timeout_seconds = 60;
    std::string result_json_path;
};

struct GuidedCaptureAutorunState {
    GuidedCaptureAutorunStage stage = GuidedCaptureAutorunStage::kDisabled;
    std::chrono::steady_clock::time_point stage_started_at{};
    std::chrono::steady_clock::time_point run_started_at{};
    bool action_requested = false;
    bool run_passed = false;
    bool result_written = false;
    bool preflight_applied = false;
    bool preflight_restore_attempted = false;
    bool preflight_restore_ok = false;
    std::string preflight_prepare_status;
    std::string preflight_restore_status;
    nlohmann::json capture_camera_settings = nlohmann::json::array();
    std::vector<std::string> prepared_camera_serials;
    std::size_t recipe_sequence_index = 0;
    std::size_t sweep_level_index = 0;
    int sweep_repeat_index = 1;
    bool current_sample_is_arena_outline = false;
    nlohmann::json completed_samples = nlohmann::json::array();
    std::string projected_surface_scale_transaction_id;
    std::string projected_surface_scale_canvas_sha256;
    nlohmann::json projected_surface_scale_observations = nlohmann::json::array();
    nlohmann::json projected_surface_scale_verification = nlohmann::json::object();
    nlohmann::json projected_surface_scale_candidate_status = nlohmann::json::object();
    std::string projected_surface_scale_manifest_path;
    std::chrono::steady_clock::time_point projected_surface_scale_last_poll_at{};
    std::string homography_transaction_id;
    std::string homography_canvas_sha256;
    std::string homography_capture_group_id;
    nlohmann::json homography_targets = nlohmann::json::array();
    nlohmann::json homography_candidate_status = nlohmann::json::object();
    std::chrono::steady_clock::time_point homography_last_poll_at{};
    bool homography_fit_requested = false;
    bool homography_candidate_released = false;
    std::string error_message;
    std::string result_write_error;
};

struct GuidedCaptureAutorunRequests {
    bool toggle_streaming = false;
    bool close_window = false;
};

GuidedCaptureAutorunConfig resolve_guided_capture_autorun_config();

const char* guided_capture_autorun_stage_name(GuidedCaptureAutorunStage stage);

void guided_capture_autorun_start(GuidedCaptureAutorunState* state,
                                  const GuidedCaptureAutorunConfig& config);

GuidedCaptureAutorunRequests guided_capture_autorun_update(
    GuidedCaptureAutorunState* state,
    const GuidedCaptureAutorunConfig& config,
    SpatialLayoutUiState* spatial_state,
    CameraControl* camera_control,
    CameraEmergent* ecams,
    CameraParams* cameras_params,
    CameraEachSelect* cameras_select,
    int num_cameras,
    SpatialSnapshotWorker* const* spatial_snapshot_workers,
    const std::string& calibration_sessions_root);

nlohmann::json guided_capture_autorun_result_json(
    const GuidedCaptureAutorunState& state,
    const GuidedCaptureAutorunConfig& config,
    const SpatialLayoutUiState& spatial_state,
    bool stream_stopped);

}  // namespace orange::gui
