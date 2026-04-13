#ifndef ORANGE_SPATIAL_LAYOUT_UI_H
#define ORANGE_SPATIAL_LAYOUT_UI_H

#include "camera.h"
#include "image_canvas.h"
#include "spatial_layout_schema.h"
#include "video_capture.h"

#include <GL/glew.h>

#include <array>
#include <string>
#include <vector>

struct CitrusSpatialTemplateState {
    bool available = false;
    std::string source_config_path;
    std::string source_rig_name;
    std::string source_canvas_name;
    std::string source_arena_name;
    std::string source_config_name;
    std::string source_camera_id;
    std::string source_dish_type_name;
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
    bool has_capture = false;
    orange::ui::ImageCanvasViewState captured_canvas_view;

    int selected_zone_index = 0;
    int canvas_edit_mode = 0;

    bool preview_valid = false;
    std::string preview_status = "Capture a frame to preview experimental-area registration.";
    std::string preview_error;
    bool has_detected_experimental_area_circle = false;
    orange::spatial::RuntimeGeometry detected_experimental_area_geometry;
    std::string detection_status;
    std::string detection_error;
    CitrusSpatialTemplateState citrus_template;
    bool has_citrus_projected_circle = false;
    orange::spatial::RuntimeGeometry citrus_projected_circle_geometry;
    std::string citrus_import_status;
    std::string citrus_import_error;
    std::string persistence_status;
    std::string persistence_error;
    std::string canonical_layout_json;
    std::string runtime_preview_json;
};

void clear_spatial_layout_texture(SpatialLayoutUiState* ui_state);
void render_spatial_layout_window(
    SpatialLayoutUiState* ui_state,
    CameraControl* camera_control,
    CameraEmergent* ecams,
    CameraParams* cameras_params,
    int num_cameras,
    bool other_calibration_tool_busy,
    const std::string& artifact_root_dir);

#endif
