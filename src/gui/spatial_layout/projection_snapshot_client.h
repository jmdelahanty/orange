#pragma once

#include "gui/spatial_layout/state.h"
#include "json.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace orange::calibration {
struct CalibrationImageSetRequest;
}

namespace orange::gui::spatial_layout {

struct CitrusProjectionSnapshotQueryResult {
    bool attempted = false;
    bool ok = false;
    nlohmann::json snapshot = nlohmann::json::object();
    std::string reason;
};

struct CitrusCalibrationSceneControlResult {
    bool attempted = false;
    bool ok = false;
    bool accepted = false;
    nlohmann::json response = nlohmann::json::object();
    nlohmann::json scene = nlohmann::json::object();
    std::string reason;
};

struct CitrusArenaCenteringControlResult {
    bool attempted = false;
    bool ok = false;
    bool accepted = false;
    nlohmann::json response = nlohmann::json::object();
    nlohmann::json centering = nlohmann::json::object();
    std::string reason;
};

struct CitrusHomographyCandidateControlResult {
    bool attempted = false;
    bool ok = false;
    bool accepted = false;
    nlohmann::json response = nlohmann::json::object();
    nlohmann::json candidate = nlohmann::json::object();
    std::string reason;
};

struct CitrusProjectedSurfaceScaleControlResult {
    bool attempted = false;
    bool ok = false;
    bool accepted = false;
    nlohmann::json response = nlohmann::json::object();
    nlohmann::json candidate = nlohmann::json::object();
    std::string reason;
};

struct CitrusRigCanvasCommissioningControlResult {
    bool attempted = false;
    bool ok = false;
    bool accepted = false;
    nlohmann::json response = nlohmann::json::object();
    nlohmann::json commissioning = nlohmann::json::object();
    std::string reason;
};

struct CitrusDailyRegistrationControlResult {
    bool attempted = false;
    bool ok = false;
    bool accepted = false;
    nlohmann::json response = nlohmann::json::object();
    nlohmann::json daily_registration = nlohmann::json::object();
    std::string reason;
};

CitrusProjectionSnapshotQueryResult query_citrus_active_projection_snapshot(
    const std::string& phase,
    const std::string& operation_id);

CitrusCalibrationSceneControlResult set_citrus_calibration_scene(
    const std::string& transaction_id,
    const std::string& recipe_id,
    const std::vector<std::string>& arena_ids,
    const std::string& operation_id,
    const nlohmann::json& scene_options = nlohmann::json::object());

CitrusCalibrationSceneControlResult query_citrus_calibration_scene_status(
    const std::string& transaction_id,
    const std::string& phase);

CitrusCalibrationSceneControlResult restore_citrus_calibration_scene(
    const std::string& transaction_id,
    const std::string& operation_id);

CitrusArenaCenteringControlResult begin_citrus_arena_centering(
    const std::string& transaction_id,
    const std::string& canvas_path,
    const nlohmann::json& targets,
    const std::string& operation_id,
    std::uint8_t foreground_gray_u8 = 72);

CitrusArenaCenteringControlResult stage_citrus_arena_centers(
    const std::string& transaction_id,
    const std::string& stage_id,
    const nlohmann::json& centers,
    const std::string& operation_id);

CitrusArenaCenteringControlResult query_citrus_arena_centering_status(
    const std::string& transaction_id,
    const std::string& phase);

CitrusArenaCenteringControlResult commit_citrus_arena_centering(
    const std::string& transaction_id,
    const std::string& expected_base_checksum,
    const nlohmann::json& verification,
    bool save_verified_centers_armed,
    bool save_verified_layout_armed,
    const std::string& operation_id);

CitrusArenaCenteringControlResult abort_citrus_arena_centering(
    const std::string& transaction_id,
    const std::string& reason,
    const std::string& operation_id);

CitrusHomographyCandidateControlResult query_citrus_homography_candidate_status(
    const std::string& transaction_id,
    const std::string& phase);

CitrusHomographyCandidateControlResult fit_citrus_homography_candidates(
    const std::string& transaction_id,
    const std::string& canvas_path,
    const std::string& expected_canvas_checksum,
    const std::string& orange_session_dir,
    const std::string& capture_group_id,
    const nlohmann::json& targets,
    const nlohmann::json& quality_thresholds,
    const std::string& operation_id);

CitrusHomographyCandidateControlResult
load_citrus_homography_candidate_set_for_review(
    const std::string& candidate_set_dir,
    const std::string& expected_candidate_set_id,
    const std::string& expected_canvas_checksum,
    const nlohmann::json& targets,
    const std::string& operation_id);

CitrusHomographyCandidateControlResult promote_citrus_homography_candidates(
    const std::string& transaction_id,
    const std::string& expected_canvas_checksum,
    const nlohmann::json& verification,
    bool accept_homographies_armed,
    const std::string& operation_id);

CitrusHomographyCandidateControlResult reject_citrus_homography_candidates(
    const std::string& transaction_id,
    const std::string& reason,
    const std::string& operation_id);

CitrusProjectedSurfaceScaleControlResult
query_citrus_projected_surface_scale_candidate_status(
    const std::string& transaction_id,
    const std::string& phase);

CitrusProjectedSurfaceScaleControlResult
fit_citrus_projected_surface_scale_candidates(
    const std::string& transaction_id,
    const std::string& canvas_path,
    const std::string& expected_canvas_checksum,
    const nlohmann::json& observations,
    const std::string& operation_id);

CitrusProjectedSurfaceScaleControlResult
load_citrus_projected_surface_scale_candidate_set_for_review(
    const std::string& candidate_set_dir,
    const std::string& expected_candidate_set_id,
    const std::string& expected_canvas_checksum,
    const nlohmann::json& targets,
    const std::string& operation_id);

CitrusProjectedSurfaceScaleControlResult
promote_citrus_projected_surface_scale_candidates(
    const std::string& transaction_id,
    const std::string& expected_canvas_checksum,
    const nlohmann::json& verification,
    bool accept_scales_armed,
    const std::string& operation_id);

CitrusProjectedSurfaceScaleControlResult
reject_citrus_projected_surface_scale_candidates(
    const std::string& transaction_id,
    const std::string& reason,
    const std::string& operation_id);

CitrusRigCanvasCommissioningControlResult
query_citrus_rig_canvas_commissioning_status(const std::string& phase);

CitrusRigCanvasCommissioningControlResult
finalize_citrus_rig_canvas_commissioning(
    const std::string& transaction_id,
    const std::string& canvas_path,
    const std::string& expected_canvas_checksum,
    const nlohmann::json& orange_session_dirs,
    bool accept_commissioning_armed,
    const std::string& operation_id);

CitrusDailyRegistrationControlResult query_citrus_daily_registration_status(
    const std::string& phase);

CitrusDailyRegistrationControlResult begin_citrus_daily_registration(
    const std::string& transaction_id,
    const nlohmann::json& targets,
    const std::string& operation_id);

CitrusDailyRegistrationControlResult create_citrus_daily_registration_candidate(
    const std::string& transaction_id,
    const nlohmann::json& observations,
    const std::string& operation_id);

CitrusDailyRegistrationControlResult preview_citrus_daily_registration_candidate(
    const std::string& transaction_id,
    const std::string& operation_id);

CitrusDailyRegistrationControlResult restore_citrus_daily_registration_preview(
    const std::string& transaction_id,
    const std::string& operation_id);

CitrusDailyRegistrationControlResult abort_citrus_daily_registration(
    const std::string& transaction_id,
    const std::string& reason,
    const std::string& operation_id);

CitrusDailyRegistrationControlResult accept_citrus_daily_registration(
    const std::string& transaction_id,
    const std::string& expected_candidate_sha256,
    const std::string& valid_until_utc,
    const nlohmann::json& verification,
    bool accept_registration_armed,
    const std::string& operation_id);

CitrusDailyRegistrationControlResult reject_citrus_daily_registration(
    const std::string& transaction_id,
    const std::string& reason,
    const std::string& operation_id);

CitrusDailyRegistrationControlResult select_citrus_daily_registration_runtime_mode(
    const std::string& mode,
    const std::string& registration_path,
    const std::string& expected_registration_sha256,
    bool select_runtime_mode_armed,
    const std::string& operation_id);

void clear_captured_citrus_projection_snapshot_metadata(SpatialLayoutUiState* ui_state);

void set_captured_citrus_projection_snapshots(
    SpatialLayoutUiState* ui_state,
    const nlohmann::json& pre_capture,
    const nlohmann::json& post_capture);

bool snapshot_projection_matches_context(
    const nlohmann::json& snapshot,
    const orange::calibration::CalibrationImageSetRequest& request);

nlohmann::json make_citrus_projection_epoch_consistency(
    const orange::calibration::CalibrationImageSetRequest& request);

} // namespace orange::gui::spatial_layout
