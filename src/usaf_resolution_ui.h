#ifndef ORANGE_USAF_RESOLUTION_UI_H
#define ORANGE_USAF_RESOLUTION_UI_H

#include "camera.h"
#include "video_capture.h"
#include "usaf_resolution_calibration.h"
#include "image_canvas.h"

#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <GL/glew.h>

struct LiveUsafPreviewState {
    bool available = false;
    int width = 0;
    int height = 0;
    int capture_width = 0;
    int capture_height = 0;
    uint64_t frame_serial = 0;
    std::vector<unsigned char> rgba;
    std::vector<unsigned char> raw_rgb;
    std::string status_message = "Idle";
    std::string error_message;
};


struct UsafResolutionUiState {
    bool show_window = false;
    int selected_camera = 0;
    int configured_camera_index = -1;
    int preview_fps = 20;
    int target_polarity = static_cast<int>(UsafTargetPolarity::kNegative);
    bool enable_fov_calibration = false;
    float working_distance_mm = 700.0f;
    float pixel_pitch_um = 2.74f;
    float field_width_mm = 0.0f;
    float field_height_mm = 0.0f;
    char illumination_mode[64] = "transmission";
    char operator_notes[256] = "";
    char output_dir[512] = "";
    char output_prefix[128] = "usaf_resolution";
    int capture_label_index = 0;
    std::thread preview_worker;
    std::atomic<bool> preview_running{false};
    std::atomic<bool> preview_stop_requested{false};
    std::mutex preview_mutex;
    LiveUsafPreviewState live_preview;
    GLuint preview_texture = 0;
    int preview_texture_width = 0;
    int preview_texture_height = 0;
    uint64_t preview_uploaded_serial = 0;
    orange::ui::ImageCanvasViewState live_canvas_view;
    std::vector<UsafCapturedPosition> positions;
    int selected_position_index = 0;
    GLuint captured_texture = 0;
    int captured_texture_width = 0;
    int captured_texture_height = 0;
    int captured_texture_position_index = -1;
    orange::ui::ImageCanvasViewState captured_canvas_view;
    bool roi_drag_active = false;
    double roi_drag_start_x = 0.0;
    double roi_drag_start_y = 0.0;
    double roi_drag_current_x = 0.0;
    double roi_drag_current_y = 0.0;
    std::thread worker;
    std::atomic<bool> running{false};
    std::mutex mutex;
    std::string status_message = "Idle";
    std::string error_message;
    std::string output_artifact_id;
    std::string output_artifact_dir;
    std::string output_manifest_path;
    std::string output_fingerprint;
    std::string output_json_path;
    std::string output_positions_csv_path;
    bool has_result = false;
    UsafResolutionResult last_result;
    FovCalibrationData last_fov_calibration;
    std::string last_camera_serial;
    unsigned int last_focus = 0;
    unsigned int last_exposure = 0;
};

void join_usaf_preview_worker_if_finished(UsafResolutionUiState* ui_state);
void stop_usaf_preview_worker(UsafResolutionUiState* ui_state);
void clear_usaf_preview_texture(UsafResolutionUiState* ui_state);
void clear_usaf_captured_texture(UsafResolutionUiState* ui_state);
void join_usaf_worker_if_finished(UsafResolutionUiState* ui_state);
void render_usaf_resolution_window(
    UsafResolutionUiState* ui_state,
    CameraControl* camera_control,
    CameraEmergent* ecams,
    CameraParams* cameras_params,
    int num_cameras,
    const std::string& default_output_dir);

#endif
