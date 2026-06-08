#ifndef ORANGE_SPATIAL_LAYOUT_UI_H
#define ORANGE_SPATIAL_LAYOUT_UI_H

#include "camera.h"
#include "image_canvas.h"
#include "spatial_layout_schema.h"
#include "video_capture.h"

#include <GL/glew.h>

#include <array>
#include <cstdint>
#include <string>
#include <vector>

class SpatialSnapshotWorker;

struct CitrusSpatialTemplateState {
    bool available = false;
    std::string source_config_path;
    std::string source_rig_name;
    std::string source_canvas_name;
    std::string source_arena_name;
    std::string source_config_name;
    std::string source_camera_id;
    std::string source_dish_type_name;
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
    bool has_camera_to_canvas_homography = false;
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
    double edge_margin_px = 12.0;

    GLuint captured_texture = 0;
    int captured_texture_width = 0;
    int captured_texture_height = 0;
    std::vector<unsigned char> captured_rgba;
    std::string captured_camera_serial;
    std::string captured_source_array_role = "images_full";
    std::string captured_capture_mode = "single_camera_direct_still";
    uint32_t captured_source_frame_count = 1;
    uint64_t captured_first_local_frame_id = 0;
    uint64_t captured_last_local_frame_id = 0;
    uint64_t captured_first_camera_frame_id = 0;
    uint64_t captured_last_camera_frame_id = 0;
    uint64_t pending_full_res_snapshot_request_id = 0;
    std::string pending_full_res_snapshot_camera_serial;
    uint32_t pending_full_res_snapshot_target_frame_count = 1;
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
    std::string detection_status;
    std::string detection_error;
    CitrusSpatialTemplateState citrus_template;
    std::vector<CitrusSpatialTemplateState> citrus_canvas_templates;
    int citrus_canvas_template_index = -1;
    std::string citrus_canvas_config_path;
    bool has_citrus_projected_circle = false;
    orange::spatial::RuntimeGeometry citrus_projected_circle_geometry;
    std::string citrus_import_status;
    std::string citrus_import_error;
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
    bool calibration_requires_filter_reinstalled_repeatably = false;
    std::string calibration_operator_notes;
    std::string calibration_image_set_purpose = "homography_grid";
    std::string calibration_image_set_target_plane = "projected_surface";
    std::string calibration_image_set_image_role = "grid_on";
    std::string calibration_image_set_projected_pattern_id = "citrus_homography_grid_v1";
    std::string calibration_image_set_projected_pattern_type = "dot_grid";
    std::string calibration_image_set_scale_target_type = "unknown";
    std::string calibration_image_set_notes;
    std::string canonical_layout_json;
    std::string runtime_preview_json;
};

void clear_spatial_layout_texture(SpatialLayoutUiState* ui_state);
void render_spatial_layout_window(
    SpatialLayoutUiState* ui_state,
    CameraControl* camera_control,
    CameraEmergent* ecams,
    CameraParams* cameras_params,
    CameraEachSelect* cameras_select,
    int num_cameras,
    bool other_calibration_tool_busy,
    const std::string& artifact_root_dir,
    const GLuint* live_preview_texture_ids = nullptr,
    const uint64_t* live_preview_uploaded_serials = nullptr,
    SpatialSnapshotWorker* const* spatial_snapshot_workers = nullptr);

#endif
