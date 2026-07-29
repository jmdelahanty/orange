#pragma once

#include "calibration_transaction.h"
#include "camera.h"
#include "image_canvas.h"
#include "spatial_layout_schema.h"
#include "video_capture.h"

#include <GL/glew.h>

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace orange::gui::spatial_layout {

struct Point2d {
    double x = 0.0;
    double y = 0.0;
};

}  // namespace orange::gui::spatial_layout

struct CitrusSpatialTemplateState {
    bool available = false;
    std::string source_config_path;
    std::string source_rig_name;
    std::string source_canvas_name;
    std::string projection_geometry_authority_mode = "local_canvas";
    std::string projection_geometry_authority_canvas_name;
    std::string projection_geometry_authority_config_path;
    std::string source_arena_name;
    std::string source_config_name;
    std::string source_camera_id;
    std::string source_dish_type_name;
    bool has_inner_diameter_mm = false;
    double inner_diameter_mm = 0.0;
    std::string inner_diameter_source_field;
    bool has_pixels_per_mm_camera = false;
    double pixels_per_mm_camera = 0.0;
    std::string pixels_per_mm_camera_target_plane;
    bool has_arena_canvas_region = false;
    double arena_center_x_px = 0.0;
    double arena_center_y_px = 0.0;
    double arena_width_px = 0.0;
    double arena_height_px = 0.0;
    double experimental_area_center_x_px = 0.0;
    double experimental_area_center_y_px = 0.0;
    double experimental_area_radius_px = 0.0;
    bool has_radius_mm = false;
    double experimental_area_radius_mm = 0.0;
    bool has_pixels_per_mm_projector = false;
    double pixels_per_mm_projector = 0.0;
    std::string calibration_pattern_mode;
    std::string calibration_pattern_mask_policy;
    bool has_calibration_ring_outer_radius_px = false;
    double calibration_ring_outer_radius_px = 0.0;
    bool has_camera_to_canvas_homography = false;
    bool has_authoritative_camera_to_canvas_homography = false;
    std::string homography_authority_status = "not_loaded";
    std::string homography_import_error;
    std::string homography_active_pointer_path;
    std::string homography_candidate_set_id;
    std::string homography_candidate_id;
    std::string homography_candidate_json_path;
    std::string homography_yaml_path;
    std::string homography_target_plane;
    std::string homography_direction;
    std::string homography_configuration_fingerprint;
    std::string homography_canvas_checksum;
    std::string homography_canvas_compatibility_basis;
    std::string homography_canvas_compatibility_warning;
    std::string homography_canvas_geometry_fingerprint;
    std::string homography_commissioning_release_id;
    std::string homography_accepted_at_utc;
    std::string homography_source_image_path;
    std::string homography_source_image_checksum;
    std::string homography_projector_intensity_report_path;
    std::string homography_projector_intensity_report_sha256;
    bool has_homography_quality = false;
    double homography_rms_reprojection_error_canvas_px = 0.0;
    double homography_maximum_reprojection_error_canvas_px = 0.0;
    double homography_holdout_rms_error_canvas_px = 0.0;
    double homography_holdout_maximum_error_canvas_px = 0.0;
    std::array<double, 9> camera_to_canvas_homography{
        1.0, 0.0, 0.0,
        0.0, 1.0, 0.0,
        0.0, 0.0, 1.0};
    bool has_canvas_to_camera_homography = false;
    std::array<double, 9> canvas_to_camera_homography{
        1.0, 0.0, 0.0,
        0.0, 1.0, 0.0,
        0.0, 0.0, 1.0};
};

struct CalibrationCaptureCameraRestoreState {
    bool valid = false;
    std::string camera_serial;
    unsigned int exposure_us = 0;
    unsigned int frame_rate_hz = 0;
};

struct SpatialLayoutPendingGroupSnapshotRequest {
    std::string camera_serial;
    uint64_t request_id = 0;
    bool completed = false;
    bool failed = false;
    std::string error;
};

struct SpatialLayoutCalibrationImageSetMetadata {
    std::string filter_state = "unknown";
    std::string runtime_filter_state = "unknown";
    std::string light_handling = "leave_current";
    std::string light_state = "unknown";
    std::string illumination_spectrum = "unknown";
    std::string illumination_source = "unknown";
    double illumination_center_wavelength_nm = 0.0;
    bool has_illumination_center_wavelength_nm = false;
    double illumination_min_wavelength_nm = 0.0;
    bool has_illumination_min_wavelength_nm = false;
    double illumination_max_wavelength_nm = 0.0;
    bool has_illumination_max_wavelength_nm = false;
    double illumination_bandwidth_fwhm_nm = 0.0;
    bool has_illumination_bandwidth_fwhm_nm = false;
    std::string illumination_wavelength_confidence = "unknown";
    std::string projector_state = "unknown";
    bool projector_visible_to_camera = false;
    bool requires_filter_reinstalled_repeatably = false;
    std::string operator_notes;
    std::string image_set_purpose = "homography_grid";
    std::string image_set_target_plane = "projected_surface";
    std::string image_set_image_role = "grid_on";
    std::string image_set_projected_pattern_id = "citrus_homography_grid_v1";
    std::string image_set_projected_pattern_type = "dot_grid";
    std::string image_set_scale_target_type = "unknown";
    std::string image_set_notes;
    std::string capture_stage = "projected_surface_dry_reference";
    std::string workflow_profile_id = "unobstructed_canvas_commissioning";
    std::string fixture_state = "holder_removed";
    std::string homography_role = "commissioning_reference";
    std::string visibility_domain_id = "unobstructed_arena_rectangle";
    std::string visibility_domain_shape = "rectangle";
    std::string visibility_domain_geometry_status = "not_embedded";
    double plane_z_mm_nominal = 0.0;
    bool has_plane_z_mm_nominal = false;
    double plane_z_mm_uncertainty = 0.0;
    bool has_plane_z_mm_uncertainty = false;
    std::string wet_or_dry = "dry";
    bool imaging_shelf_installed = false;
    bool dish_installed = false;
    std::string dish_id;
    double water_fill_mm = 0.0;
    bool has_water_fill_mm = false;
    std::string fill_state = "dry_or_empty";
    bool open_water_surface_present = false;
    std::string water_settled_status = "not_applicable";
    std::string target_method = "projected_pattern_on_diffuser";
    std::string pattern_type = "rectangular_grid";
    std::string pattern_domain = "full_projected_surface";
    std::string matched_parity_group_id;
    std::string parity_group_role = "dry_reference";
    bool reference_only = true;
    bool physical_target_used = false;
    bool projected_pattern_used_as_coordinate_target = true;
    std::string plane_id;
    double z_mm_relative_to_projection_surface = 0.0;
    bool has_z_mm_relative_to_projection_surface = false;
    std::string target_id;
    std::string target_design;
    double physical_target_grid_spacing_mm = 0.0;
    bool has_physical_target_grid_spacing_mm = false;
    std::string physical_target_origin_definition;
    std::string physical_target_x_orientation_marker_definition;
    bool has_calibration_domain = false;
    std::string calibration_domain_shape = "unknown";
    std::string calibration_domain_source = "spatial_layout_runtime";
    std::string calibration_domain_coordinate_space = "camera_native_pixels";
    double calibration_domain_center_x_px = 0.0;
    double calibration_domain_center_y_px = 0.0;
    double calibration_domain_radius_px = 0.0;
    double calibration_domain_width_px = 0.0;
    double calibration_domain_height_px = 0.0;
    double calibration_domain_rotation_deg_clockwise = 0.0;
    double calibration_domain_edge_margin_px = 0.0;
    double calibration_domain_centroid_gate_outset_px = 0.0;
    bool has_calibration_domain_valid_circle = false;
    double calibration_domain_valid_center_x_px = 0.0;
    double calibration_domain_valid_center_y_px = 0.0;
    double calibration_domain_valid_radius_px = 0.0;
    bool has_calibration_domain_valid_rectangle = false;
    double calibration_domain_valid_width_px = 0.0;
    double calibration_domain_valid_height_px = 0.0;
    nlohmann::json citrus_projection_snapshot_pre_capture = nlohmann::json::object();
    nlohmann::json citrus_projection_snapshot_post_capture = nlohmann::json::object();
    nlohmann::json citrus_projection_epoch_consistency = nlohmann::json::object();
    nlohmann::json citrus_calibration_scene_pre_capture = nlohmann::json::object();
    nlohmann::json citrus_calibration_scene_post_capture = nlohmann::json::object();
    nlohmann::json citrus_calibration_scene_consistency = nlohmann::json::object();
    nlohmann::json citrus_calibration_scene_restore_status = nlohmann::json::object();
    nlohmann::json citrus_arena_centering_pre_capture = nlohmann::json::object();
    nlohmann::json citrus_arena_centering_post_capture = nlohmann::json::object();
    nlohmann::json citrus_arena_centering_consistency = nlohmann::json::object();
    nlohmann::json citrus_daily_registration_pre_capture = nlohmann::json::object();
    nlohmann::json citrus_daily_registration_post_capture = nlohmann::json::object();
    nlohmann::json citrus_daily_registration_consistency = nlohmann::json::object();
    nlohmann::json capture_group_membership = nlohmann::json::object();
};

struct SpatialLayoutGroupCaptureFrame {
    bool valid = false;
    std::string capture_group_id;
    SpatialLayoutCalibrationImageSetMetadata metadata;
    std::string camera_serial;
    std::string camera_name;
    int camera_index = -1;
    int camera_configured_width = 0;
    int camera_configured_height = 0;
    std::string camera_pixel_format;
    double camera_exposure_us = 0.0;
    bool has_camera_exposure_us = false;
    double camera_frame_rate_hz = 0.0;
    bool has_camera_frame_rate_hz = false;
    double camera_gain = 0.0;
    bool has_camera_gain = false;
    int width = 0;
    int height = 0;
    GLuint texture = 0;
    int texture_width = 0;
    int texture_height = 0;
    std::vector<unsigned char> rgba;
    std::string source_array_role = "images_full";
    std::string capture_mode = "operator_group_next_frame";
    uint32_t source_frame_count = 1;
    uint64_t first_local_frame_id = 0;
    uint64_t last_local_frame_id = 0;
    uint64_t first_camera_frame_id = 0;
    uint64_t last_camera_frame_id = 0;
    uint64_t camera_timestamp_ns = 0;
    uint64_t timestamp_sys_ns = 0;
};

struct DailyRegistrationTargetUiState {
    std::string camera_serial;
    std::string arena_id;
    std::string alignment_basis =
        "commissioned_homography_and_canonical_experimental_center";
    bool rim_detection_ok = false;
    std::string rim_detection_error;
    double detected_rim_center_x_camera_px = 0.0;
    double detected_rim_center_y_camera_px = 0.0;
    double detected_rim_radius_camera_px = 0.0;
    double accepted_rim_center_x_camera_px = 0.0;
    double accepted_rim_center_y_camera_px = 0.0;
    double accepted_rim_radius_camera_px = 0.0;
    bool rim_operator_confirmed = false;
    SpatialLayoutGroupCaptureFrame rim_capture;
    std::vector<unsigned char> rim_gray;
    std::string rim_observation_artifact_id;
    std::string rim_observation_path;
    std::string rim_observation_sha256;

    double requested_translation_x_canvas_px = 0.0;
    double requested_translation_y_canvas_px = 0.0;
    int applied_translation_x_canvas_px = 0;
    int applied_translation_y_canvas_px = 0;
    double base_experimental_center_x_canvas_px = 0.0;
    double base_experimental_center_y_canvas_px = 0.0;
    double desired_experimental_center_x_canvas_px = 0.0;
    double desired_experimental_center_y_canvas_px = 0.0;
    double effective_experimental_center_x_canvas_px = 0.0;
    double effective_experimental_center_y_canvas_px = 0.0;
    double translation_rounding_residual_x_canvas_px = 0.0;
    double translation_rounding_residual_y_canvas_px = 0.0;
    std::string candidate_homography_id;
    std::string candidate_homography_path;
    std::string candidate_homography_sha256;
    bool geometry_review_ok = false;
    std::string geometry_review_error;
    double geometry_corrected_center_x_camera_px = 0.0;
    double geometry_corrected_center_y_camera_px = 0.0;
    double geometry_center_residual_x_camera_px = 0.0;
    double geometry_center_residual_y_camera_px = 0.0;
    double geometry_center_residual_norm_camera_px = 0.0;
    double geometry_center_quantization_bound_camera_px = 0.0;
    double geometry_predicted_radius_min_camera_px = 0.0;
    double geometry_predicted_radius_mean_camera_px = 0.0;
    double geometry_predicted_radius_max_camera_px = 0.0;
    double geometry_rim_radial_rms_error_camera_px = 0.0;
    double geometry_maximum_outside_rim_camera_px = 0.0;
    std::vector<orange::gui::spatial_layout::Point2d>
        geometry_outline_camera_px;
    std::string geometry_review_observation_path;
    std::string geometry_review_observation_sha256;
    std::string geometry_review_overlay_path;
    std::string geometry_review_overlay_sha256;

    bool projected_center_detection_ok = false;
    std::string projected_center_detection_error;
    double projected_center_x_camera_px = 0.0;
    double projected_center_y_camera_px = 0.0;
    double projected_center_radius_camera_px = 0.0;
    double base_center_residual_x_camera_px = 0.0;
    double base_center_residual_y_camera_px = 0.0;
    bool projected_center_operator_confirmed = false;
    nlohmann::json projected_center_detection = nlohmann::json::object();
    SpatialLayoutGroupCaptureFrame projected_center_capture;
    std::string projected_center_source_path;
    std::string projected_center_source_sha256;
    std::string projected_center_image_set_path;
    std::string projected_center_manifest_path;
    std::string projected_center_observation_artifact_id;
    std::string projected_center_observation_path;
    std::string projected_center_observation_sha256;

    bool preview_detection_ok = false;
    std::string preview_detection_error;
    double preview_center_x_camera_px = 0.0;
    double preview_center_y_camera_px = 0.0;
    double preview_residual_x_camera_px = 0.0;
    double preview_residual_y_camera_px = 0.0;
    double preview_residual_norm_camera_px = 0.0;
    nlohmann::json preview_detection = nlohmann::json::object();
    SpatialLayoutGroupCaptureFrame preview_capture;
    std::string preview_source_path;
    std::string preview_source_sha256;
    std::string preview_image_set_path;
    std::string preview_manifest_path;
    std::string validation_observation_path;
    std::string validation_observation_sha256;
};

struct DailyRegistrationWorkflowUiState {
    bool active = false;
    std::string stage = "idle";
    std::string transaction_id;
    std::string created_utc;
    std::string transaction_dir;
    std::string session_artifact_root;
    std::string status;
    std::string error;
    std::string pending_group_kind;
    std::string pending_operation_id;
    std::string pending_terminal_stage;
    std::string pending_terminal_reason;
    std::string candidate_path;
    std::string candidate_sha256;
    std::string accepted_registration_path;
    std::string accepted_registration_sha256;
    std::string runtime_selection_confirmation;
    double runtime_selection_started_monotonic_seconds = 0.0;
    double runtime_selection_next_poll_monotonic_seconds = 0.0;
    double runtime_selection_timeout_seconds = 15.0;
    std::string valid_until_utc;
    bool start_physical_state_armed = false;
    bool physical_state_confirmed = false;
    bool visible_projection_path_confirmed = false;
    bool preview_outline_containment_confirmed = false;
    bool geometry_outline_operator_confirmed = false;
    bool runtime_optical_state_restored_confirmed = false;
    bool accept_registration_armed = false;
    bool select_runtime_mode_armed = false;
    double maximum_preview_center_residual_camera_px = 5.0;
    double maximum_geometry_residual_beyond_quantization_camera_px = 1.0;
    nlohmann::json citrus_begin_status = nlohmann::json::object();
    nlohmann::json citrus_candidate_status = nlohmann::json::object();
    nlohmann::json citrus_accept_status = nlohmann::json::object();
    nlohmann::json citrus_runtime_selection_status = nlohmann::json::object();
    std::vector<DailyRegistrationTargetUiState> targets;
};

struct SpatialLayoutSessionReviewImage {
    bool valid = false;
    std::string label;
    std::string artifact_id;
    std::string artifact_schema_id;
    int image_index = -1;
    std::string purpose;
    std::string target_plane;
    std::string capture_stage;
    std::string plane_group;
    std::string role;
    std::string rig_id;
    std::string canvas_id;
    std::string arena_id;
    std::string camera_serial;
    std::string camera_name;
    std::string capture_group_id;
    std::string capture_mode = "loaded_calibration_session_image";
    std::string source_array_role = "images_full";
    std::string created_utc;
    std::string image_path;
    std::string image_set_path;
    std::string observation_path;
    int width = 0;
    int height = 0;
    SpatialLayoutCalibrationImageSetMetadata metadata;
    bool has_accepted_circle = false;
    double accepted_circle_cx = 0.0;
    double accepted_circle_cy = 0.0;
    double accepted_circle_r = 0.0;
    bool has_linked_accepted_top_rim = false;
};

struct SpatialLayoutSessionReviewPurposeGroup {
    std::string purpose;
    std::string target_plane;
    std::vector<int> image_indices;
};

struct SpatialLayoutSessionReviewPlaneGroup {
    std::string label;
    std::string plane_group;
    std::string camera_serial;
    std::string camera_name;
    std::string rig_id;
    std::string canvas_id;
    std::string arena_id;
    bool has_linked_accepted_top_rim = false;
    std::vector<int> image_indices;
    std::vector<SpatialLayoutSessionReviewPurposeGroup> purpose_groups;
};

struct SpatialLayoutSessionReviewCameraGroup {
    std::string label;
    std::string camera_serial;
    std::string camera_name;
    std::vector<SpatialLayoutSessionReviewPlaneGroup> plane_groups;
};

struct SpatialLayoutUiState {
    bool show_window = false;
    int selected_camera = 0;
    int configured_camera_index = -1;

    orange::spatial::ArenaLayoutArtifact layout_artifact;
    orange::spatial::ViewRegistration registration;
    orange::spatial::DishMaskRuntime dish_mask_runtime;
    orange::spatial::ArenaLayoutRuntime arena_layout_runtime;
    orange::spatial::CameraSpatialCalibration preview_calibration;

    double registration_tx_px = 0.0;
    double registration_ty_px = 0.0;
    double registration_scale = 1.0;
    double registration_rotation_deg_clockwise = 0.0;
    double edge_margin_px = 0.0;
    double centroid_gate_outset_px = 12.0;
    double centroid_gate_outset_mm = 0.0;
    bool centroid_gate_outset_authored_mm = false;
    std::string centroid_gate_outset_mm_camera_serial;

    GLuint captured_texture = 0;
    int captured_texture_width = 0;
    int captured_texture_height = 0;
    std::vector<unsigned char> captured_rgba;
    std::string captured_camera_serial;
    std::string captured_source_array_role = "images_full";
    std::string captured_capture_mode = "single_camera_direct_still";
    std::string captured_capture_group_id;
    uint32_t captured_source_frame_count = 1;
    uint64_t captured_first_local_frame_id = 0;
    uint64_t captured_last_local_frame_id = 0;
    uint64_t captured_first_camera_frame_id = 0;
    uint64_t captured_last_camera_frame_id = 0;
    uint64_t pending_full_res_snapshot_request_id = 0;
    std::string pending_full_res_snapshot_camera_serial;
    uint32_t pending_full_res_snapshot_target_frame_count = 1;
    nlohmann::json pending_full_res_snapshot_pre_capture = nlohmann::json::object();
    nlohmann::json captured_citrus_projection_snapshot_pre_capture = nlohmann::json::object();
    nlohmann::json captured_citrus_projection_snapshot_post_capture = nlohmann::json::object();
    nlohmann::json captured_citrus_projection_epoch_consistency = nlohmann::json::object();
    nlohmann::json captured_citrus_calibration_scene_pre_capture = nlohmann::json::object();
    nlohmann::json captured_citrus_calibration_scene_post_capture = nlohmann::json::object();
    nlohmann::json captured_citrus_calibration_scene_consistency = nlohmann::json::object();
    nlohmann::json captured_citrus_calibration_scene_restore_status = nlohmann::json::object();
    nlohmann::json captured_citrus_arena_centering_pre_capture = nlohmann::json::object();
    nlohmann::json captured_citrus_arena_centering_post_capture = nlohmann::json::object();
    nlohmann::json captured_citrus_arena_centering_consistency = nlohmann::json::object();
    nlohmann::json captured_citrus_daily_registration_pre_capture = nlohmann::json::object();
    nlohmann::json captured_citrus_daily_registration_post_capture = nlohmann::json::object();
    nlohmann::json captured_citrus_daily_registration_consistency = nlohmann::json::object();
    nlohmann::json captured_group_membership = nlohmann::json::object();
    std::string group_capture_id;
    std::string group_capture_mode = "operator_group_next_frame";
    std::string group_capture_scene_recipe = "auto";
    std::string group_capture_scene_authority = "calibration_scene";
    std::string group_capture_expected_stage_id;
    nlohmann::json group_capture_scene_options = nlohmann::json::object();
    std::string group_capture_resolved_scene_recipe;
    std::string group_capture_workflow_state = "idle";
    std::string group_capture_terminal_outcome;
    std::string group_capture_transaction_id;
    std::string group_capture_scene_operation_id;
    std::string group_capture_scene_request_id;
    std::string group_capture_restore_operation_id;
    std::string group_capture_restore_request_id;
    std::vector<std::string> group_capture_selected_camera_serials;
    std::vector<std::string> group_capture_expected_camera_serials;
    std::vector<std::string> group_capture_arena_ids;
    bool group_capture_camera_scope_initialized = false;
    bool group_capture_restore_required = false;
    uint32_t group_capture_target_frame_count = 1;
    double group_capture_next_scene_poll_at_seconds = 0.0;
    double group_capture_scene_deadline_at_seconds = 0.0;
    double group_capture_presented_not_before_seconds = 0.0;
    nlohmann::json group_capture_scene_pre_capture = nlohmann::json::object();
    nlohmann::json group_capture_scene_post_capture = nlohmann::json::object();
    nlohmann::json group_capture_scene_restore_status = nlohmann::json::object();
    SpatialLayoutCalibrationImageSetMetadata group_capture_metadata;
    std::vector<SpatialLayoutPendingGroupSnapshotRequest> pending_group_snapshot_requests;
    std::vector<SpatialLayoutGroupCaptureFrame> group_captures;
    std::string group_capture_status;
    std::string group_capture_error;
    std::unique_ptr<orange::calibration::TransactionLease>
        calibration_transaction_lease;
    std::string calibration_transaction_owner_kind;
    bool group_capture_owns_calibration_transaction = false;
    std::vector<SpatialLayoutSessionReviewImage> session_review_images;
    std::vector<SpatialLayoutSessionReviewCameraGroup> session_review_camera_groups;
    std::vector<std::string> session_review_warnings;
    int selected_session_review_image = -1;
    int selected_session_capture_matrix_row = -1;
    int selected_session_capture_matrix_column = -1;
    std::string loaded_calibration_session_index_path;
    std::string loaded_calibration_session_citrus_config_path;
    bool has_capture = false;
    orange::ui::ImageCanvasViewState captured_canvas_view;

    int selected_zone_index = 0;
    int canvas_edit_mode = 0;

    bool preview_valid = false;
    std::string preview_status = "Capture a frame to preview experimental-area registration.";
    std::string preview_error;
    bool has_detected_experimental_area_circle = false;
    orange::spatial::RuntimeGeometry detected_experimental_area_geometry;
    double hough_dp = 1.25;
    double hough_min_dist_fraction = 0.20;
    double hough_param1 = 120.0;
    double hough_param2 = 30.0;
    double hough_min_radius_fraction = 0.18;
    double hough_max_radius_fraction = 0.49;
    double hough_radius_adjustment_px = 0.0;
    int hough_median_blur_ksize = 5;
    int hough_max_detection_dimension_px = 2048;
    bool hough_fallback_enabled = true;
    bool show_hough_proposal_overlay = true;
    bool show_citrus_corrected_center_overlay = true;
    bool show_daily_registration_overlay = true;
    std::string detection_status;
    std::string detection_error;
    CitrusSpatialTemplateState citrus_template;
    std::vector<CitrusSpatialTemplateState> citrus_canvas_templates;
    bool has_citrus_projected_fit_ring = false;
    orange::spatial::RuntimeGeometry citrus_projected_fit_ring_geometry;
    std::vector<orange::gui::spatial_layout::Point2d>
        citrus_projected_fit_ring_outline_camera_points;
    int citrus_canvas_template_index = -1;
    std::string citrus_canvas_config_path;
    bool has_citrus_projected_circle = false;
    orange::spatial::RuntimeGeometry citrus_projected_circle_geometry;
    std::vector<orange::gui::spatial_layout::Point2d> citrus_projected_outline_camera_points;
    std::string citrus_import_status;
    std::string citrus_import_error;
    std::string homography_candidate_review_manifest_path;
    nlohmann::json homography_candidate_review_manifest =
        nlohmann::json::object();
    nlohmann::json homography_candidate_review_status =
        nlohmann::json::object();
    bool homography_candidate_review_revalidated = false;
    bool accept_reviewed_homographies_armed = false;
    std::string homography_candidate_review_message;
    std::string homography_candidate_review_error;
    std::string projected_surface_scale_review_manifest_path;
    nlohmann::json projected_surface_scale_review_manifest =
        nlohmann::json::object();
    nlohmann::json projected_surface_scale_review_status =
        nlohmann::json::object();
    nlohmann::json projected_surface_scale_review_verification =
        nlohmann::json::object();
    std::string projected_surface_scale_review_transaction_id;
    std::string projected_surface_scale_review_canvas_sha256;
    std::string projected_surface_scale_candidate_manifest_path;
    nlohmann::json projected_surface_scale_candidate_manifest =
        nlohmann::json::object();
    bool projected_surface_scale_review_revalidated = false;
    bool accept_reviewed_projected_surface_scales_armed = false;
    std::string projected_surface_scale_review_message;
    std::string projected_surface_scale_review_error;
    nlohmann::json rig_canvas_commissioning_status = nlohmann::json::object();
    bool accept_rig_canvas_commissioning_armed = false;
    std::string rig_canvas_commissioning_message;
    std::string rig_canvas_commissioning_error;
    nlohmann::json daily_registration_status = nlohmann::json::object();
    DailyRegistrationWorkflowUiState daily_registration_workflow;
    bool base_only_runtime_mode_armed = false;
    std::string daily_registration_message;
    std::string daily_registration_error;
    std::string persistence_status;
    std::string persistence_error;
    std::string calibration_session_id;
    std::string calibration_session_dir;
    std::string calibration_session_created_utc;
    CameraRigIoOutputState calibration_light_restore_state;
    std::string calibration_light_restore_key;
    int calibration_light_control_camera = -1;
    std::vector<CalibrationCaptureCameraRestoreState> calibration_capture_restore_states;
    std::string calibration_preflight_status;
    std::string calibration_preflight_error;
    bool calibration_capture_profile_active = false;
    std::string calibration_capture_profile_id;
    std::string calibration_capture_profile_operation_id;
    std::string calibration_capture_profile_camera_serial;
    std::string calibration_capture_profile_light_camera_serial;
    int calibration_average_frame_count = 60;
    int calibration_workflow_tab = 0;
    std::string calibration_filter_state = "unknown";
    std::string calibration_runtime_filter_state = "unknown";
    std::string calibration_light_handling = "leave_current";
    std::string calibration_light_state = "unknown";
    std::string calibration_illumination_spectrum = "unknown";
    std::string calibration_illumination_source = "unknown";
    double calibration_illumination_center_wavelength_nm = 0.0;
    bool calibration_has_illumination_center_wavelength_nm = false;
    double calibration_illumination_min_wavelength_nm = 0.0;
    bool calibration_has_illumination_min_wavelength_nm = false;
    double calibration_illumination_max_wavelength_nm = 0.0;
    bool calibration_has_illumination_max_wavelength_nm = false;
    double calibration_illumination_bandwidth_fwhm_nm = 0.0;
    bool calibration_has_illumination_bandwidth_fwhm_nm = false;
    std::string calibration_illumination_wavelength_confidence = "unknown";
    std::string calibration_projector_state = "unknown";
    bool calibration_projector_visible_to_camera = false;
    std::string calibration_dish_fill_state = "unknown";
    bool calibration_inner_rim_target_confirmed = false;
    bool calibration_requires_filter_reinstalled_repeatably = false;
    std::string calibration_operator_notes;
    std::string calibration_image_set_purpose = "homography_grid";
    std::string calibration_image_set_target_plane = "projected_surface";
    std::string calibration_image_set_image_role = "grid_on";
    std::string calibration_image_set_projected_pattern_id = "citrus_homography_grid_v1";
    std::string calibration_image_set_projected_pattern_type = "dot_grid";
    std::string calibration_image_set_scale_target_type = "unknown";
    std::string calibration_image_set_notes;
    std::string calibration_capture_stage = "projected_surface_dry_reference";
    std::string calibration_workflow_profile_id = "unobstructed_canvas_commissioning";
    std::string calibration_fixture_state = "holder_removed";
    std::string calibration_homography_role = "commissioning_reference";
    std::string calibration_visibility_domain_id = "unobstructed_arena_rectangle";
    std::string calibration_visibility_domain_shape = "rectangle";
    std::string calibration_visibility_domain_geometry_status = "not_embedded";
    double calibration_plane_z_mm_nominal = 0.0;
    bool calibration_has_plane_z_mm_nominal = false;
    double calibration_plane_z_mm_uncertainty = 0.0;
    bool calibration_has_plane_z_mm_uncertainty = false;
    std::string calibration_wet_or_dry = "dry";
    bool calibration_imaging_shelf_installed = false;
    bool calibration_dish_installed = false;
    std::string calibration_dish_id;
    double calibration_water_fill_mm = 0.0;
    bool calibration_has_water_fill_mm = false;
    std::string calibration_fill_state = "dry_or_empty";
    bool calibration_open_water_surface_present = false;
    std::string calibration_water_settled_status = "not_applicable";
    std::string calibration_target_method = "projected_pattern_on_diffuser";
    std::string calibration_pattern_type = "rectangular_grid";
    std::string calibration_pattern_domain = "full_projected_surface";
    std::string calibration_matched_parity_group_id;
    std::string calibration_parity_group_role = "dry_reference";
    bool calibration_reference_only = true;
    bool calibration_physical_target_used = false;
    bool calibration_projected_pattern_used_as_coordinate_target = true;
    std::string calibration_plane_id;
    double calibration_z_mm_relative_to_projection_surface = 0.0;
    bool calibration_has_z_mm_relative_to_projection_surface = false;
    std::string calibration_target_id;
    std::string calibration_target_design;
    double calibration_physical_target_grid_spacing_mm = 0.0;
    bool calibration_has_physical_target_grid_spacing_mm = false;
    std::string calibration_physical_target_origin_definition;
    std::string calibration_physical_target_x_orientation_marker_definition;
    std::string canonical_layout_json;
    std::string runtime_preview_json;
};
