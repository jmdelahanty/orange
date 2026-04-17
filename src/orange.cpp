// src/orange.cpp

#include "video_capture.h"
#include <iostream>
#include "camera.h"
#include "imgui.h"
#include "implot.h"
#include <ImGuiFileDialog.h>
#include "project.h"
#include "gui.h"
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <cuda.h>
#include <unordered_map>
#include <cuda_runtime.h>
#include "NvEncoder/NvCodecUtils.h"
#include "network_base.h"
#include "enet_thread.h"
#include "yolo_worker.h"
#include "global.h"
#include "encoder_preprocess_worker.h"
#include "encoder_hw_worker.h"
#include "modern_recording_pipeline.h"
#include "opengldisplay.h"
#include "image_writer_worker.h"
#include "yolov8_det.h"
#include "crop_and_encode_worker.h"
#include "frame_ipc_manager.h"
#include "fsuid_guard.h"
#include "aperture_characterization.h"
#include "camera_preview_utils.h"
#include "gui/camera_properties_panel.h"
#include "gui/frame_ipc_panel.h"
#include "gui/host_ptp_panel.h"
#include "gui/recording_panel.h"
#include "image_canvas.h"
#include "recording_validation.h"
#include "spatial_layout_ui.h"
#include "usaf_resolution_ui.h"
#include <opencv2/opencv.hpp>

#include <cstdio>
#include <cstring>
#include <fstream>
#include <array>
#include <limits>
#include <sstream>

std::vector<YOLOv8Worker*> yolo_workers; // For managing YOLO workers
ENetPeer* external_data_consumer_peer = nullptr; // Store the peer for YOLO data
std::vector<SpeedTrackingData> speed_tracking_data;

namespace {

enum class RulerAlignmentOrientation {
    kHorizontal = 0,
    kVertical = 1
};

struct RulerAlignmentMetrics {
    bool has_detected_line = false;
    double line_angle_deg = 0.0;
    double angle_error_deg = 0.0;
    double center_offset_px = 0.0;
    double center_offset_fraction = 0.0;
    int x0 = 0;
    int y0 = 0;
    int x1 = 0;
    int y1 = 0;
};

struct FovCaptureSnapshot {
    bool available = false;
    int width = 0;
    int height = 0;
    std::vector<unsigned char> rgb;
    RulerAlignmentMetrics metrics;
};

struct LiveFovPreviewState {
    bool available = false;
    int width = 0;
    int height = 0;
    uint64_t frame_serial = 0;
    std::vector<unsigned char> rgba;
    std::vector<unsigned char> raw_rgb;
    RulerAlignmentMetrics metrics;
    std::string status_message = "Idle";
    std::string error_message;
};

struct ApertureCharacterizationUiState {
    bool show_window = false;
    int selected_camera = 0;
    int configured_camera_index = -1;
    bool use_explicit_iris_values = false;
    char iris_values_csv[256] = "";
    int iris_start = 0;
    int iris_stop = 0;
    int iris_step_multiple = 1;
    int frames_per_step = 3;
    int settle_frames = 30;
    int buffer_count = 4;
    int grab_timeout_ms = 1000;
    int grid_rows = 8;
    int grid_cols = 8;
    bool restore_original_iris = true;
    bool save_representative_frames = true;
    bool use_reference_iris = false;
    int reference_iris = 0;
    bool use_reference_f_number = false;
    float reference_f_number = 2.8f;
    bool enable_fov_calibration = false;
    float working_distance_mm = 700.0f;
    float pixel_pitch_um = 2.74f;
    float field_width_mm = 0.0f;
    float field_height_mm = 0.0f;
    int alignment_preview_fps = 20;
    float saturated_white_fraction = 0.001f;
    float saturated_p99_min = 254.0f;
    float dim_mean_max = 10.0f;
    float dim_p95_max = 20.0f;
    float dim_black_fraction_min = 0.80f;
    char output_dir[512] = "";
    char output_prefix[128] = "aperture_characterization";
    std::thread worker;
    std::atomic<bool> running{false};
    std::atomic<int> progress_completed_steps{0};
    std::atomic<int> progress_total_steps{0};
    std::atomic<int> progress_iris{0};
    std::mutex mutex;
    std::string status_message = "Idle";
    std::string error_message;
    std::string output_artifact_id;
    std::string output_artifact_dir;
    std::string output_manifest_path;
    std::string output_fingerprint;
    std::string output_json_path;
    std::string output_steps_csv_path;
    std::string output_frames_csv_path;
    std::string output_frame_image_dir;
    GLuint preview_texture = 0;
    int preview_texture_width = 0;
    int preview_texture_height = 0;
    std::string preview_texture_path;
    std::string preview_texture_error;
    orange::ui::ImageCanvasViewState representative_canvas_view;
    std::thread alignment_worker;
    std::atomic<bool> alignment_running{false};
    std::atomic<bool> alignment_stop_requested{false};
    std::atomic<int> alignment_orientation{static_cast<int>(RulerAlignmentOrientation::kHorizontal)};
    std::mutex alignment_mutex;
    LiveFovPreviewState live_fov_preview;
    GLuint alignment_texture = 0;
    int alignment_texture_width = 0;
    int alignment_texture_height = 0;
    uint64_t alignment_uploaded_serial = 0;
    FovCaptureSnapshot horizontal_capture;
    FovCaptureSnapshot vertical_capture;
    bool has_result = false;
    int selected_heatmap_step = 0;
    ApertureCharacterizationResult last_result;
    FovCalibrationData last_fov_calibration;
    std::string last_camera_serial;
    unsigned int last_focus = 0;
    unsigned int last_exposure = 0;
};

template <size_t N>
void copy_string_to_buffer(char (&buffer)[N], const std::string& value)
{
    std::snprintf(buffer, N, "%s", value.c_str());
}

std::string trim_ascii_whitespace(const std::string& input)
{
    size_t start = 0;
    while (start < input.size() && std::isspace(static_cast<unsigned char>(input[start]))) {
        ++start;
    }
    size_t end = input.size();
    while (end > start && std::isspace(static_cast<unsigned char>(input[end - 1]))) {
        --end;
    }
    return input.substr(start, end - start);
}

std::vector<RecordingValidationCameraInput> build_gui_recording_validation_inputs(
    const CameraParams* cameras_params,
    const CameraEachSelect* cameras_select,
    const int num_cameras)
{
    std::vector<RecordingValidationCameraInput> inputs;
    if (!cameras_params || !cameras_select || num_cameras <= 0) {
        return inputs;
    }

    inputs.reserve(static_cast<std::size_t>(num_cameras));
    for (int i = 0; i < num_cameras; ++i) {
        RecordingValidationCameraInput input;
        input.camera_index = i;
        input.camera_serial = cameras_params[i].camera_serial;
        input.record_enabled = cameras_select[i].record;
        input.source_gpu_id = cameras_params[i].gpu_id;
        input.strategy = cameras_params[i].recording.strategy;
        input.constraints = cameras_params[i].recording.constraints;
        inputs.push_back(std::move(input));
    }

    return inputs;
}

RecordingPreflightResult run_gui_recording_preflight(const CameraParams* cameras_params,
                                                     const CameraEachSelect* cameras_select,
                                                     const int num_cameras)
{
    return run_recording_preflight(
        build_gui_recording_validation_inputs(cameras_params, cameras_select, num_cameras),
        [](const int source_gpu_id, const int helper_gpu_id) {
            return build_recording_validation_gpu_path_info(source_gpu_id, helper_gpu_id);
        });
}

void log_recording_preflight_failure(const char* context,
                                     const RecordingPreflightResult& preflight)
{
    if (preflight.ok || preflight.errors.empty()) {
        return;
    }

    std::cerr << "[recording_preflight] " << context << " blocked" << std::endl;
    for (const std::string& error : preflight.errors) {
        std::cerr << "  - " << error << std::endl;
    }
}

bool parse_uint_csv_text(const char* text, std::vector<unsigned int>* out_values, std::string* error_out)
{
    std::stringstream ss(text == nullptr ? "" : text);
    std::string item;
    while (std::getline(ss, item, ',')) {
        std::string trimmed;
        for (char c : item) {
            if (!std::isspace(static_cast<unsigned char>(c))) {
                trimmed.push_back(c);
            }
        }
        if (trimmed.empty()) {
            continue;
        }
        try {
            size_t consumed = 0;
            unsigned long value = std::stoul(trimmed, &consumed, 10);
            if (consumed != trimmed.size() || value > std::numeric_limits<unsigned int>::max()) {
                if (error_out) {
                    *error_out = "Invalid iris value in CSV: " + trimmed;
                }
                return false;
            }
            out_values->push_back(static_cast<unsigned int>(value));
        } catch (...) {
            if (error_out) {
                *error_out = "Invalid iris value in CSV: " + trimmed;
            }
            return false;
        }
    }

    if (out_values->empty()) {
        if (error_out) {
            *error_out = "Explicit iris list is empty.";
        }
        return false;
    }
    return true;
}

const char* ruler_alignment_orientation_label(RulerAlignmentOrientation orientation)
{
    switch (orientation) {
        case RulerAlignmentOrientation::kVertical:
            return "vertical";
        case RulerAlignmentOrientation::kHorizontal:
        default:
            return "horizontal";
    }
}

RulerAlignmentMetrics detect_ruler_alignment(
    const cv::Mat& gray,
    RulerAlignmentOrientation orientation)
{
    RulerAlignmentMetrics best;
    if (gray.empty()) {
        return best;
    }

    auto detect_boundary_anchor = [&](const cv::Mat& source_gray) -> int {
        cv::Mat directionally_smoothed;
        if (orientation == RulerAlignmentOrientation::kHorizontal) {
            cv::GaussianBlur(source_gray, directionally_smoothed, cv::Size(61, 7), 0.0, 0.0, cv::BORDER_REPLICATE);
        } else {
            cv::GaussianBlur(source_gray, directionally_smoothed, cv::Size(7, 61), 0.0, 0.0, cv::BORDER_REPLICATE);
        }

        cv::Mat gradient;
        if (orientation == RulerAlignmentOrientation::kHorizontal) {
            cv::Sobel(directionally_smoothed, gradient, CV_32F, 0, 1, 3);
        } else {
            cv::Sobel(directionally_smoothed, gradient, CV_32F, 1, 0, 3);
        }
        cv::Mat abs_gradient = cv::abs(gradient);

        cv::Mat profile;
        if (orientation == RulerAlignmentOrientation::kHorizontal) {
            cv::reduce(abs_gradient, profile, 1, cv::REDUCE_AVG, CV_32F);
            cv::GaussianBlur(profile, profile, cv::Size(1, 31), 0.0, 0.0, cv::BORDER_REPLICATE);
        } else {
            cv::reduce(abs_gradient, profile, 0, cv::REDUCE_AVG, CV_32F);
            cv::GaussianBlur(profile, profile, cv::Size(31, 1), 0.0, 0.0, cv::BORDER_REPLICATE);
        }

        const int profile_length =
            orientation == RulerAlignmentOrientation::kHorizontal ? profile.rows : profile.cols;
        if (profile_length <= 0) {
            return -1;
        }

        const int margin = std::max(4, profile_length / 40);
        int best_index = -1;
        double best_score = -1.0;
        for (int i = margin; i < profile_length - margin; ++i) {
            const float response =
                orientation == RulerAlignmentOrientation::kHorizontal ? profile.at<float>(i, 0) : profile.at<float>(0, i);
            const double boundary_preference =
                1.0 - std::clamp(static_cast<double>(i) / std::max(1.0, static_cast<double>(profile_length - 1)), 0.0, 1.0);
            const double score = static_cast<double>(response) * (0.35 + 0.65 * boundary_preference);
            if (score > best_score) {
                best_score = score;
                best_index = i;
            }
        }
        return best_index;
    };

    const int boundary_anchor = detect_boundary_anchor(gray);

    cv::Mat blurred;
    cv::GaussianBlur(gray, blurred, cv::Size(5, 5), 0.0);
    cv::Mat edges;
    cv::Canny(blurred, edges, 60.0, 180.0, 3);

    cv::Rect search_roi(0, 0, gray.cols, gray.rows);
    if (boundary_anchor >= 0) {
        const int search_half_band = std::max(16, static_cast<int>(std::round(
            0.04 * static_cast<double>(
                orientation == RulerAlignmentOrientation::kHorizontal ? gray.rows : gray.cols))));
        if (orientation == RulerAlignmentOrientation::kHorizontal) {
            const int y0 = std::max(0, boundary_anchor - search_half_band);
            const int y1 = std::min(gray.rows, boundary_anchor + search_half_band + 1);
            search_roi = cv::Rect(0, y0, gray.cols, std::max(1, y1 - y0));
        } else {
            const int x0 = std::max(0, boundary_anchor - search_half_band);
            const int x1 = std::min(gray.cols, boundary_anchor + search_half_band + 1);
            search_roi = cv::Rect(x0, 0, std::max(1, x1 - x0), gray.rows);
        }
    }

    std::vector<cv::Vec4i> lines;
    cv::HoughLinesP(
        edges(search_roi),
        lines,
        1.0,
        CV_PI / 180.0,
        80,
        std::max(search_roi.width, search_roi.height) / 5.0,
        20.0);
    const double center_x = 0.5 * static_cast<double>(gray.cols);
    const double center_y = 0.5 * static_cast<double>(gray.rows);
    double best_score = -1.0;

    for (const cv::Vec4i& local_line : lines) {
        const cv::Vec4i line(
            local_line[0] + search_roi.x,
            local_line[1] + search_roi.y,
            local_line[2] + search_roi.x,
            local_line[3] + search_roi.y);
        const double dx = static_cast<double>(line[2] - line[0]);
        const double dy = static_cast<double>(line[3] - line[1]);
        const double length = std::hypot(dx, dy);
        if (length < std::max(gray.cols, gray.rows) * 0.15) {
            continue;
        }

        double angle = std::atan2(dy, dx) * 180.0 / CV_PI;
        while (angle > 90.0) angle -= 180.0;
        while (angle <= -90.0) angle += 180.0;

        double angle_error = 0.0;
        double center_offset_px = 0.0;
        double boundary_preference = 0.0;
        double anchor_distance_px = 0.0;
        if (orientation == RulerAlignmentOrientation::kHorizontal) {
            angle_error = std::abs(angle);
            const double line_center_y = (static_cast<double>(line[1]) + static_cast<double>(line[3])) * 0.5;
            center_offset_px = line_center_y - center_y;
            boundary_preference = 1.0 - std::clamp(line_center_y / std::max(1.0, static_cast<double>(gray.rows - 1)), 0.0, 1.0);
            anchor_distance_px = boundary_anchor >= 0 ? std::abs(line_center_y - static_cast<double>(boundary_anchor)) : 0.0;
        } else {
            angle_error = std::abs(90.0 - std::abs(angle));
            const double line_center_x = (static_cast<double>(line[0]) + static_cast<double>(line[2])) * 0.5;
            center_offset_px = line_center_x - center_x;
            boundary_preference = 1.0 - std::clamp(line_center_x / std::max(1.0, static_cast<double>(gray.cols - 1)), 0.0, 1.0);
            anchor_distance_px = boundary_anchor >= 0 ? std::abs(line_center_x - static_cast<double>(boundary_anchor)) : 0.0;
        }
        if (angle_error > 25.0) {
            continue;
        }

        const double center_extent =
            orientation == RulerAlignmentOrientation::kHorizontal ? center_y : center_x;
        const double angle_score = 1.0 - std::min(angle_error / 25.0, 1.0);
        const double anchor_score =
            boundary_anchor >= 0
                ? 1.0 - std::min(anchor_distance_px /
                                     std::max(1.0, 0.5 * static_cast<double>(
                                                       orientation == RulerAlignmentOrientation::kHorizontal
                                                           ? search_roi.height
                                                           : search_roi.width)),
                                 1.0)
                : 1.0;
        const double score =
            length * (0.15 + 0.85 * angle_score) * (0.25 + 0.75 * boundary_preference) * (0.20 + 0.80 * anchor_score);
        if (score > best_score) {
            best_score = score;
            best.has_detected_line = true;
            best.line_angle_deg = angle;
            best.angle_error_deg = angle_error;
            best.center_offset_px = center_offset_px;
            best.center_offset_fraction = center_extent > 0.0 ? center_offset_px / center_extent : 0.0;
            best.x0 = line[0];
            best.y0 = line[1];
            best.x1 = line[2];
            best.y1 = line[3];
        }
    }

    if (!best.has_detected_line && boundary_anchor >= 0) {
        best.has_detected_line = true;
        best.line_angle_deg = orientation == RulerAlignmentOrientation::kHorizontal ? 0.0 : 90.0;
        best.angle_error_deg = 0.0;
        if (orientation == RulerAlignmentOrientation::kHorizontal) {
            best.center_offset_px = static_cast<double>(boundary_anchor) - center_y;
            best.center_offset_fraction = center_y > 0.0 ? best.center_offset_px / center_y : 0.0;
            best.x0 = 0;
            best.x1 = gray.cols - 1;
            best.y0 = boundary_anchor;
            best.y1 = boundary_anchor;
        } else {
            best.center_offset_px = static_cast<double>(boundary_anchor) - center_x;
            best.center_offset_fraction = center_x > 0.0 ? best.center_offset_px / center_x : 0.0;
            best.x0 = boundary_anchor;
            best.x1 = boundary_anchor;
            best.y0 = 0;
            best.y1 = gray.rows - 1;
        }
    }

    return best;
}

void draw_ruler_alignment_overlay(
    cv::Mat* bgr_image,
    const RulerAlignmentMetrics& metrics,
    RulerAlignmentOrientation orientation)
{
    if (bgr_image == nullptr || bgr_image->empty()) {
        return;
    }

    const int width = bgr_image->cols;
    const int height = bgr_image->rows;
    const cv::Scalar guide_color(255, 255, 0);
    const cv::Scalar line_color = metrics.has_detected_line
                                      ? (metrics.angle_error_deg <= 2.0 && std::abs(metrics.center_offset_fraction) <= 0.05
                                             ? cv::Scalar(0, 220, 0)
                                             : cv::Scalar(0, 140, 255))
                                      : cv::Scalar(0, 0, 255);

    if (orientation == RulerAlignmentOrientation::kHorizontal) {
        cv::line(*bgr_image, cv::Point(0, height / 2), cv::Point(width - 1, height / 2), guide_color, 1, cv::LINE_AA);
    } else {
        cv::line(*bgr_image, cv::Point(width / 2, 0), cv::Point(width / 2, height - 1), guide_color, 1, cv::LINE_AA);
    }

    if (metrics.has_detected_line) {
        cv::line(*bgr_image,
                 cv::Point(metrics.x0, metrics.y0),
                 cv::Point(metrics.x1, metrics.y1),
                 line_color,
                 2,
                 cv::LINE_AA);
    }

    std::ostringstream oss;
    if (metrics.has_detected_line) {
        oss << ruler_alignment_orientation_label(orientation)
            << " angle_err=" << std::fixed << std::setprecision(2) << metrics.angle_error_deg
            << "deg offset=" << std::showpos << std::setprecision(1) << metrics.center_offset_px << "px";
    } else {
        oss << "No ruler line detected";
    }
    cv::putText(*bgr_image, oss.str(), cv::Point(20, 32), cv::FONT_HERSHEY_SIMPLEX, 0.75, line_color, 2, cv::LINE_AA);
}

bool write_rgb_image_ppm(
    const std::string& path,
    const std::vector<unsigned char>& rgb,
    int width,
    int height,
    std::string* error_out)
{
    if (width <= 0 || height <= 0 || rgb.size() != static_cast<size_t>(width) * static_cast<size_t>(height) * 3U) {
        if (error_out) {
            *error_out = "RGB preview buffer is invalid for PPM write.";
        }
        return false;
    }

    orange::ScopedFsuid fsuid_guard;
    (void)fsuid_guard;
    std::ofstream out(path, std::ios::binary);
    if (!out) {
        if (error_out) {
            *error_out = "Failed to open FOV capture output path: " + path;
        }
        return false;
    }
    out << "P6\n" << width << " " << height << "\n255\n";
    out.write(reinterpret_cast<const char*>(rgb.data()), static_cast<std::streamsize>(rgb.size()));
    return static_cast<bool>(out);
}

bool read_pnm_token(std::istream& input, std::string* token)
{
    token->clear();
    while (true) {
        int ch = input.peek();
        if (ch == EOF) {
            return false;
        }
        if (std::isspace(static_cast<unsigned char>(ch))) {
            input.get();
            continue;
        }
        if (ch == '#') {
            input.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
            continue;
        }
        break;
    }

    while (true) {
        int ch = input.peek();
        if (ch == EOF || std::isspace(static_cast<unsigned char>(ch)) || ch == '#') {
            break;
        }
        token->push_back(static_cast<char>(input.get()));
    }
    return !token->empty();
}

bool load_representative_frame_rgba(
    const std::string& image_path,
    std::vector<unsigned char>* rgba,
    int* width,
    int* height,
    std::string* error_out)
{
    std::ifstream input(image_path, std::ios::binary);
    if (!input.is_open()) {
        if (error_out) {
            *error_out = "Failed to open representative frame: " + image_path;
        }
        return false;
    }

    std::string magic;
    std::string width_token;
    std::string height_token;
    std::string maxval_token;
    if (!read_pnm_token(input, &magic) ||
        !read_pnm_token(input, &width_token) ||
        !read_pnm_token(input, &height_token) ||
        !read_pnm_token(input, &maxval_token)) {
        if (error_out) {
            *error_out = "Failed to parse representative frame header: " + image_path;
        }
        return false;
    }

    if (magic != "P5" && magic != "P6") {
        if (error_out) {
            *error_out = "Representative frame is not a binary PGM/PPM file: " + image_path;
        }
        return false;
    }

    const int parsed_width = std::stoi(width_token);
    const int parsed_height = std::stoi(height_token);
    const int maxval = std::stoi(maxval_token);
    if (parsed_width <= 0 || parsed_height <= 0 || maxval != 255) {
        if (error_out) {
            *error_out = "Representative frame has unsupported dimensions or max value: " + image_path;
        }
        return false;
    }

    input.get(); // consume the single whitespace byte before raster data

    const int channel_count = magic == "P6" ? 3 : 1;
    const size_t src_size =
        static_cast<size_t>(parsed_width) * static_cast<size_t>(parsed_height) * static_cast<size_t>(channel_count);
    std::vector<unsigned char> src(src_size);
    input.read(reinterpret_cast<char*>(src.data()), static_cast<std::streamsize>(src_size));
    if (input.gcount() != static_cast<std::streamsize>(src_size)) {
        if (error_out) {
            *error_out = "Representative frame raster is truncated: " + image_path;
        }
        return false;
    }

    rgba->resize(static_cast<size_t>(parsed_width) * static_cast<size_t>(parsed_height) * 4U);
    for (int i = 0; i < parsed_width * parsed_height; ++i) {
        const size_t dst_base = static_cast<size_t>(i) * 4U;
        if (channel_count == 1) {
            const unsigned char v = src[static_cast<size_t>(i)];
            (*rgba)[dst_base + 0] = v;
            (*rgba)[dst_base + 1] = v;
            (*rgba)[dst_base + 2] = v;
        } else {
            const size_t src_base = static_cast<size_t>(i) * 3U;
            (*rgba)[dst_base + 0] = src[src_base + 0];
            (*rgba)[dst_base + 1] = src[src_base + 1];
            (*rgba)[dst_base + 2] = src[src_base + 2];
        }
        (*rgba)[dst_base + 3] = 255;
    }

    *width = parsed_width;
    *height = parsed_height;
    return true;
}

void reset_aperture_defaults_for_camera(
    ApertureCharacterizationUiState* ui_state,
    const CameraParams& camera_params,
    int selected_camera,
    const std::string& default_output_dir)
{
    ui_state->selected_camera = selected_camera;
    ui_state->configured_camera_index = selected_camera;
    ui_state->use_explicit_iris_values = false;
    ui_state->iris_values_csv[0] = '\0';
    ui_state->iris_start = static_cast<int>(camera_params.iris_min);
    ui_state->iris_stop = static_cast<int>(camera_params.iris_max);
    ui_state->iris_step_multiple = 1;
    ui_state->reference_iris = static_cast<int>(camera_params.iris_min);
    {
        std::lock_guard<std::mutex> lock(ui_state->alignment_mutex);
        ui_state->horizontal_capture = {};
        ui_state->vertical_capture = {};
        ui_state->live_fov_preview = {};
        ui_state->live_fov_preview.status_message = "Idle";
    }
    if (ui_state->output_dir[0] == '\0') {
        copy_string_to_buffer(ui_state->output_dir, default_output_dir);
    }
}

void join_aperture_worker_if_finished(ApertureCharacterizationUiState* ui_state)
{
    if (!ui_state->running.load(std::memory_order_acquire) && ui_state->worker.joinable()) {
        ui_state->worker.join();
    }
}

void clear_aperture_preview_texture(ApertureCharacterizationUiState* ui_state)
{
    orange::preview::clear_texture(
        &ui_state->preview_texture,
        &ui_state->preview_texture_width,
        &ui_state->preview_texture_height);
    ui_state->preview_texture_path.clear();
    ui_state->preview_texture_error.clear();
    ui_state->representative_canvas_view.fit_requested = true;
    ui_state->representative_canvas_view.last_image_width = 0;
    ui_state->representative_canvas_view.last_image_height = 0;
}

void clear_alignment_preview_texture(ApertureCharacterizationUiState* ui_state)
{
    orange::preview::clear_texture(
        &ui_state->alignment_texture,
        &ui_state->alignment_texture_width,
        &ui_state->alignment_texture_height);
    ui_state->alignment_uploaded_serial = 0;
}

bool ensure_aperture_preview_texture(
    ApertureCharacterizationUiState* ui_state,
    const std::string& image_path)
{
    if (image_path.empty()) {
        clear_aperture_preview_texture(ui_state);
        ui_state->preview_texture_error = "Selected step has no representative frame.";
        return false;
    }
    if (ui_state->preview_texture != 0 && ui_state->preview_texture_path == image_path) {
        return true;
    }

    clear_aperture_preview_texture(ui_state);

    int width = 0;
    int height = 0;
    std::vector<unsigned char> rgba;
    if (!load_representative_frame_rgba(image_path, &rgba, &width, &height, &ui_state->preview_texture_error)) {
        return false;
    }

    orange::preview::update_rgba_texture(
        &ui_state->preview_texture,
        &ui_state->preview_texture_width,
        &ui_state->preview_texture_height,
        rgba,
        width,
        height,
        &ui_state->preview_texture_error);
    ui_state->preview_texture_path = image_path;
    ui_state->preview_texture_error.clear();
    return true;
}

void join_alignment_worker_if_finished(ApertureCharacterizationUiState* ui_state)
{
    if (!ui_state->alignment_running.load(std::memory_order_acquire) && ui_state->alignment_worker.joinable()) {
        ui_state->alignment_worker.join();
    }
}

void stop_fov_alignment_worker(ApertureCharacterizationUiState* ui_state)
{
    if (ui_state->alignment_running.exchange(false, std::memory_order_acq_rel)) {
        ui_state->alignment_stop_requested.store(true, std::memory_order_release);
    }
    if (ui_state->alignment_worker.joinable()) {
        ui_state->alignment_worker.join();
    }
    ui_state->alignment_stop_requested.store(false, std::memory_order_release);
}

bool upload_latest_alignment_texture(ApertureCharacterizationUiState* ui_state)
{
    std::lock_guard<std::mutex> lock(ui_state->alignment_mutex);
    const LiveFovPreviewState& preview = ui_state->live_fov_preview;
    if (!preview.available || preview.rgba.empty() || preview.frame_serial == ui_state->alignment_uploaded_serial) {
        return preview.available;
    }

    orange::preview::update_rgba_texture(
        &ui_state->alignment_texture,
        &ui_state->alignment_texture_width,
        &ui_state->alignment_texture_height,
        preview.rgba,
        preview.width,
        preview.height);
    ui_state->alignment_uploaded_serial = preview.frame_serial;
    return true;
}

bool capture_current_fov_snapshot(
    ApertureCharacterizationUiState* ui_state,
    bool horizontal_capture,
    std::string* error_out)
{
    std::lock_guard<std::mutex> lock(ui_state->alignment_mutex);
    if (!ui_state->live_fov_preview.available ||
        ui_state->live_fov_preview.width <= 0 ||
        ui_state->live_fov_preview.height <= 0 ||
        ui_state->live_fov_preview.raw_rgb.empty()) {
        if (error_out) {
            *error_out = "No live FOV preview frame is available to capture.";
        }
        return false;
    }

    FovCaptureSnapshot snapshot;
    snapshot.available = true;
    snapshot.width = ui_state->live_fov_preview.width;
    snapshot.height = ui_state->live_fov_preview.height;
    snapshot.rgb = ui_state->live_fov_preview.raw_rgb;
    snapshot.metrics = ui_state->live_fov_preview.metrics;
    if (horizontal_capture) {
        ui_state->horizontal_capture = std::move(snapshot);
    } else {
        ui_state->vertical_capture = std::move(snapshot);
    }
    return true;
}

void start_fov_alignment_worker(
    ApertureCharacterizationUiState* ui_state,
    CameraEmergent* ecams,
    CameraParams* cameras_params)
{
    join_alignment_worker_if_finished(ui_state);
    if (ui_state->alignment_running.exchange(true, std::memory_order_acq_rel)) {
        return;
    }
    ui_state->alignment_stop_requested.store(false, std::memory_order_release);

    const int selected_camera = ui_state->selected_camera;
    CameraEmergent* ecam = &ecams[selected_camera];
    CameraParams* camera_params = &cameras_params[selected_camera];
    const int preview_target_fps = std::max(1, ui_state->alignment_preview_fps);

    {
        std::lock_guard<std::mutex> lock(ui_state->alignment_mutex);
        ui_state->live_fov_preview = {};
        ui_state->live_fov_preview.status_message = "Starting live ruler alignment...";
    }

    ui_state->alignment_worker = std::thread([=]() {
        constexpr int kAlignmentPreviewBufferCount = 2;
        Emergent::CEmergentFrame* frames = nullptr;
        bool stream_opened = false;
        bool buffers_allocated = false;
        bool acquisition_started = false;
        bool frame_rate_changed = false;
        unsigned int original_frame_rate = camera_params->frame_rate;
        unsigned int active_preview_fps = static_cast<unsigned int>(preview_target_fps);

        auto publish_error = [&](const std::string& message) {
            std::lock_guard<std::mutex> lock(ui_state->alignment_mutex);
            ui_state->live_fov_preview.error_message = message;
            ui_state->live_fov_preview.status_message = "Live ruler alignment failed.";
        };

        try {
            unsigned int frame_rate_min = camera_params->frame_rate_min;
            unsigned int frame_rate_max = camera_params->frame_rate_max;
            if (get_camera_uint32_param_range(&ecam->camera, "FrameRate", &frame_rate_min, &frame_rate_max)) {
                camera_params->frame_rate_min = frame_rate_min;
                camera_params->frame_rate_max = frame_rate_max;
            }
            const unsigned int clamped_preview_fps =
                std::clamp(static_cast<unsigned int>(preview_target_fps), frame_rate_min, frame_rate_max);
            active_preview_fps = clamped_preview_fps;
            if (clamped_preview_fps != original_frame_rate) {
                check_camera_errors(
                    EVT_CameraSetUInt32Param(&ecam->camera, "FrameRate", clamped_preview_fps),
                    camera_params->camera_serial.c_str());
                camera_params->frame_rate = clamped_preview_fps;
                frame_rate_changed = true;
            }

            camera_open_stream(&ecam->camera, camera_params, "gui_alignment_preview");
            stream_opened = true;

            frames = new Emergent::CEmergentFrame[kAlignmentPreviewBufferCount]();
            allocate_frame_buffer(&ecam->camera, frames, camera_params, kAlignmentPreviewBufferCount);
            buffers_allocated = true;

            check_camera_errors(
                EVT_CameraExecuteCommand(&ecam->camera, "AcquisitionStart"),
                camera_params->camera_serial.c_str());
            acquisition_started = true;

            while (!ui_state->alignment_stop_requested.load(std::memory_order_acquire)) {
                Emergent::CEmergentFrame frame{};
                int dropped_frames = 0;
                if (!orange::preview::grab_latest_frame(&ecam->camera, camera_params, 250, &frame, &dropped_frames)) {
                    continue;
                }

                cv::Mat bgr;
                std::string convert_error;
                if (!orange::preview::frame_to_bgr(frame, &bgr, &convert_error)) {
                    EVT_CameraQueueFrame(&ecam->camera, &frame);
                    throw std::runtime_error(convert_error);
                }

                const int max_preview_dimension = 1920;
                double scale = 1.0;
                if (std::max(bgr.cols, bgr.rows) > max_preview_dimension) {
                    scale = static_cast<double>(max_preview_dimension) /
                            static_cast<double>(std::max(bgr.cols, bgr.rows));
                }
                cv::Mat preview_bgr;
                if (scale < 1.0) {
                    cv::resize(bgr, preview_bgr, cv::Size(), scale, scale, cv::INTER_AREA);
                } else {
                    preview_bgr = bgr;
                }

                cv::Mat gray;
                cv::cvtColor(preview_bgr, gray, cv::COLOR_BGR2GRAY);
                const RulerAlignmentOrientation orientation =
                    ui_state->alignment_orientation.load(std::memory_order_acquire) == static_cast<int>(RulerAlignmentOrientation::kVertical)
                        ? RulerAlignmentOrientation::kVertical
                        : RulerAlignmentOrientation::kHorizontal;
                const RulerAlignmentMetrics metrics = detect_ruler_alignment(gray, orientation);

                cv::Mat overlay_bgr = preview_bgr.clone();
                draw_ruler_alignment_overlay(&overlay_bgr, metrics, orientation);
                cv::Mat overlay_rgba;
                cv::cvtColor(overlay_bgr, overlay_rgba, cv::COLOR_BGR2RGBA);
                cv::Mat preview_rgb;
                cv::cvtColor(preview_bgr, preview_rgb, cv::COLOR_BGR2RGB);

                {
                    std::lock_guard<std::mutex> lock(ui_state->alignment_mutex);
                    ui_state->live_fov_preview.available = true;
                    ui_state->live_fov_preview.width = overlay_rgba.cols;
                    ui_state->live_fov_preview.height = overlay_rgba.rows;
                    ui_state->live_fov_preview.frame_serial += 1;
                    ui_state->live_fov_preview.rgba.assign(
                        overlay_rgba.data,
                        overlay_rgba.data + overlay_rgba.total() * overlay_rgba.elemSize());
                    ui_state->live_fov_preview.raw_rgb.assign(
                        preview_rgb.data,
                        preview_rgb.data + preview_rgb.total() * preview_rgb.elemSize());
                    ui_state->live_fov_preview.metrics = metrics;
                    ui_state->live_fov_preview.error_message.clear();
                    ui_state->live_fov_preview.status_message =
                        std::string("Live ") + ruler_alignment_orientation_label(orientation) +
                        " ruler alignment @ " + std::to_string(active_preview_fps) +
                        " FPS" + (dropped_frames > 0 ? " (dropped " + std::to_string(dropped_frames) + ")" : "");
                }

                check_camera_errors(EVT_CameraQueueFrame(&ecam->camera, &frame), camera_params->camera_serial.c_str());
            }
        } catch (const std::exception& ex) {
            publish_error(ex.what());
        }

        if (acquisition_started) {
            EVT_CameraExecuteCommand(&ecam->camera, "AcquisitionStop");
        }
        if (buffers_allocated && frames != nullptr) {
            try {
                destroy_frame_buffer(&ecam->camera, frames, kAlignmentPreviewBufferCount, camera_params);
            } catch (...) {
            }
        }
        delete[] frames;
        if (stream_opened) {
            EVT_CameraCloseStream(&ecam->camera);
        }
        if (frame_rate_changed) {
            EVT_CameraSetUInt32Param(&ecam->camera, "FrameRate", original_frame_rate);
            camera_params->frame_rate = original_frame_rate;
        }
        ui_state->alignment_running.store(false, std::memory_order_release);
    });
}

void start_aperture_characterization_worker(
    ApertureCharacterizationUiState* ui_state,
    CameraEmergent* ecams,
    CameraParams* cameras_params)
{
    join_aperture_worker_if_finished(ui_state);
    stop_fov_alignment_worker(ui_state);

    if (ui_state->running.exchange(true, std::memory_order_acq_rel)) {
        return;
    }

    const int selected_camera = ui_state->selected_camera;
    CameraEmergent* ecam = &ecams[selected_camera];
    CameraParams* camera_params = &cameras_params[selected_camera];

    const bool use_explicit_iris_values = ui_state->use_explicit_iris_values;
    const std::string iris_values_csv = ui_state->iris_values_csv;
    const int iris_start = ui_state->iris_start;
    const int iris_stop = ui_state->iris_stop;
    const int iris_step_multiple = ui_state->iris_step_multiple;
    const int frames_per_step = ui_state->frames_per_step;
    const int settle_frames = ui_state->settle_frames;
    const int buffer_count = ui_state->buffer_count;
    const int grab_timeout_ms = ui_state->grab_timeout_ms;
    const int grid_rows = ui_state->grid_rows;
    const int grid_cols = ui_state->grid_cols;
    const bool restore_original_iris = ui_state->restore_original_iris;
    const bool save_representative_frames = ui_state->save_representative_frames;
    const bool use_reference_iris = ui_state->use_reference_iris;
    const int reference_iris = ui_state->reference_iris;
    const bool use_reference_f_number = ui_state->use_reference_f_number;
    const float reference_f_number = ui_state->reference_f_number;
    const bool enable_fov_calibration = ui_state->enable_fov_calibration;
    const float working_distance_mm = ui_state->working_distance_mm;
    const float pixel_pitch_um = ui_state->pixel_pitch_um;
    const float field_width_mm = ui_state->field_width_mm;
    const float field_height_mm = ui_state->field_height_mm;
    const float saturated_white_fraction = ui_state->saturated_white_fraction;
    const float saturated_p99_min = ui_state->saturated_p99_min;
    const float dim_mean_max = ui_state->dim_mean_max;
    const float dim_p95_max = ui_state->dim_p95_max;
    const float dim_black_fraction_min = ui_state->dim_black_fraction_min;
    const std::string output_dir = ui_state->output_dir;
    const std::string output_prefix_base = ui_state->output_prefix;
    FovCaptureSnapshot horizontal_capture;
    FovCaptureSnapshot vertical_capture;
    {
        std::lock_guard<std::mutex> lock(ui_state->alignment_mutex);
        horizontal_capture = ui_state->horizontal_capture;
        vertical_capture = ui_state->vertical_capture;
    }

    ui_state->progress_completed_steps.store(0, std::memory_order_release);
    ui_state->progress_total_steps.store(0, std::memory_order_release);
    ui_state->progress_iris.store(0, std::memory_order_release);
    {
        std::lock_guard<std::mutex> lock(ui_state->mutex);
        ui_state->status_message = "Preparing aperture characterization...";
        ui_state->error_message.clear();
        ui_state->output_artifact_id.clear();
        ui_state->output_artifact_dir.clear();
        ui_state->output_manifest_path.clear();
        ui_state->output_fingerprint.clear();
        ui_state->output_json_path.clear();
        ui_state->output_steps_csv_path.clear();
        ui_state->output_frames_csv_path.clear();
        ui_state->output_frame_image_dir.clear();
        ui_state->has_result = false;
    }

    ui_state->worker = std::thread([=]() {
        auto finish = [&]() {
            ui_state->running.store(false, std::memory_order_release);
        };

        try {
            std::vector<unsigned int> iris_values;
            if (use_explicit_iris_values) {
                std::string error;
                if (!parse_uint_csv_text(iris_values_csv.c_str(), &iris_values, &error)) {
                    throw std::runtime_error(error);
                }
            } else {
                if (iris_start < static_cast<int>(camera_params->iris_min) ||
                    iris_stop > static_cast<int>(camera_params->iris_max) ||
                    iris_start > iris_stop) {
                    throw std::runtime_error("Generated iris sweep range is invalid for the selected camera.");
                }
                iris_values = build_iris_sweep(
                    static_cast<unsigned int>(iris_start),
                    static_cast<unsigned int>(iris_stop),
                    camera_params->iris_inc,
                    static_cast<unsigned int>(std::max(1, iris_step_multiple)));
            }

            if (iris_values.empty()) {
                throw std::runtime_error("No iris values were generated for this run.");
            }

            for (unsigned int iris_value : iris_values) {
                if (iris_value < camera_params->iris_min || iris_value > camera_params->iris_max) {
                    throw std::runtime_error("One or more requested iris values are outside the camera range.");
                }
            }

            ui_state->progress_total_steps.store(static_cast<int>(iris_values.size()), std::memory_order_release);

            const std::filesystem::path artifact_root_dir(output_dir);
            {
                orange::ScopedFsuid fsuid_guard;
                (void)fsuid_guard;
                std::filesystem::create_directories(artifact_root_dir);
            }
            const std::string timestamp = get_current_date_time();
            const std::string created_utc = get_current_utc_timestamp();
            const std::string artifact_id =
                build_aperture_characterization_artifact_id(output_prefix_base, *camera_params, timestamp);
            const ApertureCharacterizationArtifactPaths artifact_paths =
                make_aperture_characterization_artifact_paths(artifact_root_dir.string(), artifact_id);
            {
                orange::ScopedFsuid fsuid_guard;
                (void)fsuid_guard;
                std::filesystem::create_directories(artifact_paths.artifact_dir);
            }
            std::string write_error;
            CameraConfigSnapshotProvenance camera_config_snapshot;
            if (!camera_params->config_path.empty()) {
                camera_config_snapshot.has_source_path = true;
                camera_config_snapshot.source_path = camera_params->config_path;
            } else {
                camera_config_snapshot.error = "config path missing";
            }

            if (camera_config_snapshot.has_source_path) {
                std::string config_snapshot_contents;
                std::string config_snapshot_error;
                if (!read_camera_config_snapshot(*camera_params, &config_snapshot_contents, &config_snapshot_error)) {
                    camera_config_snapshot.error = config_snapshot_error;
                } else {
                    orange::ScopedFsuid fsuid_guard;
                    (void)fsuid_guard;
                    std::ofstream config_out(
                        artifact_paths.camera_config_snapshot_path,
                        std::ios::out | std::ios::binary | std::ios::trunc);
                    if (!config_out.is_open()) {
                        camera_config_snapshot.error =
                            "Failed to open camera config snapshot output path: " +
                            artifact_paths.camera_config_snapshot_path;
                    } else {
                        config_out.write(
                            config_snapshot_contents.data(),
                            static_cast<std::streamsize>(config_snapshot_contents.size()));
                        config_out.close();
                        if (!config_out) {
                            camera_config_snapshot.error =
                                "Failed to write camera config snapshot output path: " +
                                artifact_paths.camera_config_snapshot_path;
                        } else {
                            camera_config_snapshot.has_snapshot = true;
                            camera_config_snapshot.snapshot_path = artifact_paths.camera_config_snapshot_path;
                            camera_config_snapshot.error.clear();
                        }
                    }
                }
            }

            FovCalibrationData fov_calibration;
            fov_calibration.enabled = enable_fov_calibration;
            if (enable_fov_calibration) {
                fov_calibration.working_distance_mm = std::max(0.0f, working_distance_mm);
                fov_calibration.pixel_pitch_um = std::max(0.0f, pixel_pitch_um);
                fov_calibration.sensor_width_mm =
                    static_cast<double>(camera_params->width) * static_cast<double>(fov_calibration.pixel_pitch_um) / 1000.0;
                fov_calibration.sensor_height_mm =
                    static_cast<double>(camera_params->height) * static_cast<double>(fov_calibration.pixel_pitch_um) / 1000.0;
                if (field_width_mm > 0.0f) {
                    fov_calibration.has_field_width_mm = true;
                    fov_calibration.field_width_mm = field_width_mm;
                }
                if (field_height_mm > 0.0f) {
                    fov_calibration.has_field_height_mm = true;
                    fov_calibration.field_height_mm = field_height_mm;
                }
                if (fov_calibration.has_field_width_mm && fov_calibration.field_width_mm > 0.0) {
                    fov_calibration.has_magnification_x = true;
                    fov_calibration.magnification_x =
                        fov_calibration.sensor_width_mm / fov_calibration.field_width_mm;
                }
                if (fov_calibration.has_field_height_mm && fov_calibration.field_height_mm > 0.0) {
                    fov_calibration.has_magnification_y = true;
                    fov_calibration.magnification_y =
                        fov_calibration.sensor_height_mm / fov_calibration.field_height_mm;
                }
                if (fov_calibration.has_magnification_x || fov_calibration.has_magnification_y) {
                    const double mag_sum =
                        (fov_calibration.has_magnification_x ? fov_calibration.magnification_x : 0.0) +
                        (fov_calibration.has_magnification_y ? fov_calibration.magnification_y : 0.0);
                    const double mag_count =
                        (fov_calibration.has_magnification_x ? 1.0 : 0.0) +
                        (fov_calibration.has_magnification_y ? 1.0 : 0.0);
                    fov_calibration.has_mean_magnification = mag_count > 0.0;
                    fov_calibration.mean_magnification = mag_count > 0.0 ? (mag_sum / mag_count) : 0.0;
                }
                if (use_reference_f_number && fov_calibration.has_mean_magnification) {
                    fov_calibration.has_effective_reference_f_number = true;
                    fov_calibration.effective_reference_f_number =
                        static_cast<double>(reference_f_number) * (1.0 + fov_calibration.mean_magnification);
                }

                const bool has_any_fov_capture = horizontal_capture.available || vertical_capture.available;
                if (has_any_fov_capture) {
                    orange::ScopedFsuid fsuid_guard;
                    (void)fsuid_guard;
                    std::filesystem::create_directories(artifact_paths.fov_reference_frames_dir);
                }
                if (horizontal_capture.available) {
                    if (!write_rgb_image_ppm(
                            artifact_paths.fov_horizontal_capture_path,
                            horizontal_capture.rgb,
                            horizontal_capture.width,
                            horizontal_capture.height,
                            &write_error)) {
                        throw std::runtime_error(write_error);
                    }
                    fov_calibration.horizontal_capture.has_capture = true;
                    fov_calibration.horizontal_capture.capture_path = artifact_paths.fov_horizontal_capture_path;
                    fov_calibration.horizontal_capture.has_detected_line = horizontal_capture.metrics.has_detected_line;
                    fov_calibration.horizontal_capture.line_angle_deg = horizontal_capture.metrics.line_angle_deg;
                    fov_calibration.horizontal_capture.angle_error_deg = horizontal_capture.metrics.angle_error_deg;
                    fov_calibration.horizontal_capture.center_offset_px = horizontal_capture.metrics.center_offset_px;
                    fov_calibration.horizontal_capture.center_offset_fraction = horizontal_capture.metrics.center_offset_fraction;
                }
                if (vertical_capture.available) {
                    if (!write_rgb_image_ppm(
                            artifact_paths.fov_vertical_capture_path,
                            vertical_capture.rgb,
                            vertical_capture.width,
                            vertical_capture.height,
                            &write_error)) {
                        throw std::runtime_error(write_error);
                    }
                    fov_calibration.vertical_capture.has_capture = true;
                    fov_calibration.vertical_capture.capture_path = artifact_paths.fov_vertical_capture_path;
                    fov_calibration.vertical_capture.has_detected_line = vertical_capture.metrics.has_detected_line;
                    fov_calibration.vertical_capture.line_angle_deg = vertical_capture.metrics.line_angle_deg;
                    fov_calibration.vertical_capture.angle_error_deg = vertical_capture.metrics.angle_error_deg;
                    fov_calibration.vertical_capture.center_offset_px = vertical_capture.metrics.center_offset_px;
                    fov_calibration.vertical_capture.center_offset_fraction = vertical_capture.metrics.center_offset_fraction;
                }
            }

            ApertureCharacterizationRequest request;
            request.iris_values = iris_values;
            request.frames_per_step = static_cast<unsigned int>(std::max(1, frames_per_step));
            request.settle_frames = static_cast<unsigned int>(std::max(0, settle_frames));
            request.grab_timeout_ms = static_cast<unsigned int>(std::max(1, grab_timeout_ms));
            request.grid_rows = (grid_rows > 0 && grid_cols > 0) ? static_cast<unsigned int>(grid_rows) : 0U;
            request.grid_cols = (grid_rows > 0 && grid_cols > 0) ? static_cast<unsigned int>(grid_cols) : 0U;
            request.manage_acquisition = true;
            request.restore_original_iris = restore_original_iris;
            request.has_reference_iris = use_reference_iris;
            request.reference_iris = static_cast<unsigned int>(std::max(0, reference_iris));
            request.has_reference_f_number = use_reference_f_number;
            request.reference_f_number = reference_f_number;
            request.camera_config_snapshot = camera_config_snapshot;
            request.fov_calibration = fov_calibration;
            request.thresholds.saturated_white_fraction = saturated_white_fraction;
            request.thresholds.saturated_p99_min = saturated_p99_min;
            request.thresholds.dim_mean_max = dim_mean_max;
            request.thresholds.dim_p95_max = dim_p95_max;
            request.thresholds.dim_black_fraction_min = dim_black_fraction_min;
            request.save_representative_frames = save_representative_frames;
            request.representative_frame_dir = artifact_paths.representative_frames_dir;
            request.representative_frame_prefix = artifact_id;
            request.progress_callback = [ui_state](size_t completed_steps, size_t total_steps, unsigned int iris_value) {
                ui_state->progress_completed_steps.store(static_cast<int>(completed_steps), std::memory_order_release);
                ui_state->progress_total_steps.store(static_cast<int>(total_steps), std::memory_order_release);
                ui_state->progress_iris.store(static_cast<int>(iris_value), std::memory_order_release);
            };

            std::string lens_name;
            get_camera_string_param(&ecam->camera, "LensName", &lens_name);

            ApertureCharacterizationResult result = characterize_aperture_with_stream(
                &ecam->camera,
                camera_params,
                request,
                static_cast<unsigned int>(std::max(1, buffer_count)));

            const nlohmann::json measurement_json =
                aperture_characterization_to_json(
                    result,
                    request,
                    *camera_params,
                    lens_name,
                    artifact_id,
                    created_utc,
                    "",
                    artifact_paths);
            const std::string fingerprint =
                compute_aperture_characterization_fingerprint(measurement_json, artifact_paths, &write_error);
            if (fingerprint.empty()) {
                throw std::runtime_error(write_error.empty()
                                             ? "Failed to compute aperture artifact fingerprint."
                                             : write_error);
            }
            const nlohmann::json measurement_json_with_fingerprint =
                aperture_characterization_to_json(
                    result,
                    request,
                    *camera_params,
                    lens_name,
                    artifact_id,
                    created_utc,
                    fingerprint,
                    artifact_paths);
            const nlohmann::json manifest_json =
                aperture_characterization_manifest_to_json(
                    result,
                    request,
                    *camera_params,
                    lens_name,
                    artifact_id,
                    created_utc,
                    fingerprint,
                    artifact_paths);
            if (!write_aperture_characterization_json(artifact_paths.manifest_path, manifest_json, &write_error) ||
                !write_aperture_characterization_json(artifact_paths.measurement_json_path, measurement_json_with_fingerprint, &write_error) ||
                !write_aperture_characterization_step_csv(artifact_paths.steps_csv_path, result, artifact_paths, &write_error) ||
                !write_aperture_characterization_frame_csv(artifact_paths.frames_csv_path, result, &write_error)) {
                throw std::runtime_error(write_error);
            }
            if (!update_calibration_artifact_registry(artifact_root_dir.string(), manifest_json, &write_error)) {
                throw std::runtime_error(write_error);
            }

            {
                std::lock_guard<std::mutex> lock(ui_state->mutex);
                ui_state->status_message = "Aperture characterization completed.";
                ui_state->error_message.clear();
                ui_state->output_artifact_id = artifact_id;
                ui_state->output_artifact_dir = artifact_paths.artifact_dir;
                ui_state->output_manifest_path = artifact_paths.manifest_path;
                ui_state->output_fingerprint = fingerprint;
                ui_state->output_json_path = artifact_paths.measurement_json_path;
                ui_state->output_steps_csv_path = artifact_paths.steps_csv_path;
                ui_state->output_frames_csv_path = artifact_paths.frames_csv_path;
                ui_state->output_frame_image_dir = artifact_paths.representative_frames_dir;
                ui_state->has_result = true;
                ui_state->selected_heatmap_step = 0;
                ui_state->last_result = std::move(result);
                ui_state->last_fov_calibration = request.fov_calibration;
                ui_state->last_camera_serial = camera_params->camera_serial;
                ui_state->last_focus = camera_params->focus;
                ui_state->last_exposure = camera_params->exposure;
            }
        } catch (const std::exception& ex) {
            std::cerr << camera_params->camera_serial
                      << " [aperture_characterization] Run failed: "
                      << ex.what() << std::endl;
            std::lock_guard<std::mutex> lock(ui_state->mutex);
            ui_state->status_message = "Aperture characterization failed.";
            ui_state->error_message = ex.what();
        } catch (...) {
            std::cerr << camera_params->camera_serial
                      << " [aperture_characterization] Run failed: unknown error"
                      << std::endl;
            std::lock_guard<std::mutex> lock(ui_state->mutex);
            ui_state->status_message = "Aperture characterization failed.";
            ui_state->error_message = "Unknown error";
        }

        finish();
    });
}

void render_aperture_characterization_window(
    ApertureCharacterizationUiState* ui_state,
    CameraControl* camera_control,
    CameraEmergent* ecams,
    CameraParams* cameras_params,
    int num_cameras,
    const std::string& default_output_dir)
{
    if (!ui_state->show_window) {
        stop_fov_alignment_worker(ui_state);
        clear_aperture_preview_texture(ui_state);
        clear_alignment_preview_texture(ui_state);
        return;
    }

    join_aperture_worker_if_finished(ui_state);
    join_alignment_worker_if_finished(ui_state);

    if (!ImGui::Begin("Aperture Characterization", &ui_state->show_window)) {
        ImGui::End();
        return;
    }

    if (!camera_control->open || num_cameras <= 0 || cameras_params == nullptr || ecams == nullptr) {
        stop_fov_alignment_worker(ui_state);
        clear_alignment_preview_texture(ui_state);
        ImGui::TextDisabled("Open one or more cameras to run aperture characterization.");
        ImGui::End();
        return;
    }

    ui_state->selected_camera = std::clamp(ui_state->selected_camera, 0, num_cameras - 1);
    if (ui_state->configured_camera_index != ui_state->selected_camera) {
        reset_aperture_defaults_for_camera(
            ui_state, cameras_params[ui_state->selected_camera], ui_state->selected_camera, default_output_dir);
    }

    const bool running = ui_state->running.load(std::memory_order_acquire);
    const bool alignment_running = ui_state->alignment_running.load(std::memory_order_acquire);
    if (running) {
        ImGui::BeginDisabled();
    }

    std::vector<const char*> camera_labels;
    camera_labels.reserve(num_cameras);
    for (int i = 0; i < num_cameras; ++i) {
        camera_labels.push_back(cameras_params[i].camera_name.c_str());
    }
    if (ImGui::Combo("Camera", &ui_state->selected_camera, camera_labels.data(), num_cameras)) {
        stop_fov_alignment_worker(ui_state);
        clear_alignment_preview_texture(ui_state);
        reset_aperture_defaults_for_camera(
            ui_state, cameras_params[ui_state->selected_camera], ui_state->selected_camera, default_output_dir);
    }

    CameraParams& selected_camera = cameras_params[ui_state->selected_camera];
    ImGui::Text("Serial: %s", selected_camera.camera_serial.c_str());
    ImGui::Text("Focus: %u  Exposure: %u  Gain: %u  PixelFormat: %s",
                selected_camera.focus,
                selected_camera.exposure,
                selected_camera.gain,
                selected_camera.pixel_format.c_str());
    ImGui::Text("Iris range: [%u, %u] inc=%u",
                selected_camera.iris_min,
                selected_camera.iris_max,
                selected_camera.iris_inc);
    ImGui::TextWrapped(
        "Focus changes the measured transmission on a macro setup. Treat focus as part of the calibration key and rerun if focus changes materially.");

    ImGui::Separator();
    ImGui::Checkbox("Use explicit iris CSV", &ui_state->use_explicit_iris_values);
    if (ui_state->use_explicit_iris_values) {
        ImGui::InputText("Iris CSV", ui_state->iris_values_csv, IM_ARRAYSIZE(ui_state->iris_values_csv));
    } else {
        ImGui::InputInt("Iris start", &ui_state->iris_start);
        ImGui::InputInt("Iris stop", &ui_state->iris_stop);
        ImGui::InputInt("Iris step multiple", &ui_state->iris_step_multiple);
        if (ui_state->iris_step_multiple < 1) {
            ui_state->iris_step_multiple = 1;
        }
    }

    ImGui::InputInt("Frames per step", &ui_state->frames_per_step);
    ImGui::InputInt("Settle frames", &ui_state->settle_frames);
    ImGui::InputInt("Frame buffer count", &ui_state->buffer_count);
    ImGui::InputInt("Grab timeout (ms)", &ui_state->grab_timeout_ms);
    ImGui::InputInt("Grid rows (0=off)", &ui_state->grid_rows);
    ImGui::InputInt("Grid cols (0=off)", &ui_state->grid_cols);
    if (ui_state->frames_per_step < 1) ui_state->frames_per_step = 1;
    if (ui_state->settle_frames < 0) ui_state->settle_frames = 0;
    if (ui_state->buffer_count < 1) ui_state->buffer_count = 1;
    if (ui_state->grab_timeout_ms < 1) ui_state->grab_timeout_ms = 1;
    if (ui_state->grid_rows < 0) ui_state->grid_rows = 0;
    if (ui_state->grid_cols < 0) ui_state->grid_cols = 0;

    ImGui::Checkbox("Restore original iris", &ui_state->restore_original_iris);
    ImGui::Checkbox("Save representative frame per iris", &ui_state->save_representative_frames);
    ImGui::Checkbox("Use reference iris", &ui_state->use_reference_iris);
    if (ui_state->use_reference_iris) {
        ImGui::InputInt("Reference iris", &ui_state->reference_iris);
    }
    ImGui::Checkbox("Use reference f-number", &ui_state->use_reference_f_number);
    if (ui_state->use_reference_f_number) {
        ImGui::InputFloat("Reference f-number", &ui_state->reference_f_number, 0.1f, 1.0f, "%.2f");
        if (ui_state->reference_f_number <= 0.0f) {
            ui_state->reference_f_number = 2.8f;
        }
    }

    ImGui::Separator();
    ImGui::Checkbox("Include FOV calibration metadata", &ui_state->enable_fov_calibration);
    if (ui_state->enable_fov_calibration) {
        ImGui::InputFloat("Working distance (mm)", &ui_state->working_distance_mm, 5.0f, 25.0f, "%.1f");
        ImGui::InputFloat("Pixel pitch (um)", &ui_state->pixel_pitch_um, 0.01f, 0.1f, "%.3f");
        ImGui::InputFloat("Field width (mm)", &ui_state->field_width_mm, 1.0f, 10.0f, "%.2f");
        ImGui::InputFloat("Field height (mm)", &ui_state->field_height_mm, 1.0f, 10.0f, "%.2f");
        ImGui::InputInt("Preview FPS", &ui_state->alignment_preview_fps);
        ui_state->working_distance_mm = std::max(0.0f, ui_state->working_distance_mm);
        ui_state->pixel_pitch_um = std::max(0.0f, ui_state->pixel_pitch_um);
        ui_state->field_width_mm = std::max(0.0f, ui_state->field_width_mm);
        ui_state->field_height_mm = std::max(0.0f, ui_state->field_height_mm);
        ui_state->alignment_preview_fps = std::clamp(ui_state->alignment_preview_fps, 1, 120);

        const double sensor_width_mm =
            static_cast<double>(selected_camera.width) * static_cast<double>(ui_state->pixel_pitch_um) / 1000.0;
        const double sensor_height_mm =
            static_cast<double>(selected_camera.height) * static_cast<double>(ui_state->pixel_pitch_um) / 1000.0;
        ImGui::Text("Derived sensor size: %.3f mm x %.3f mm", sensor_width_mm, sensor_height_mm);
        if (ui_state->field_width_mm > 0.0f) {
            ImGui::Text("Magnification X: %.4f", sensor_width_mm / static_cast<double>(ui_state->field_width_mm));
        }
        if (ui_state->field_height_mm > 0.0f) {
            ImGui::Text("Magnification Y: %.4f", sensor_height_mm / static_cast<double>(ui_state->field_height_mm));
        }
        if (ui_state->use_reference_f_number && (ui_state->field_width_mm > 0.0f || ui_state->field_height_mm > 0.0f)) {
            double mag_sum = 0.0;
            double mag_count = 0.0;
            if (ui_state->field_width_mm > 0.0f) {
                mag_sum += sensor_width_mm / static_cast<double>(ui_state->field_width_mm);
                mag_count += 1.0;
            }
            if (ui_state->field_height_mm > 0.0f) {
                mag_sum += sensor_height_mm / static_cast<double>(ui_state->field_height_mm);
                mag_count += 1.0;
            }
            if (mag_count > 0.0) {
                const double effective_reference_f = static_cast<double>(ui_state->reference_f_number) * (1.0 + (mag_sum / mag_count));
                ImGui::Text("Approx effective reference f-number: %.3f", effective_reference_f);
            }
        }

        ImGui::SeparatorText("Live Ruler Alignment");
        const bool can_preview = !running && !camera_control->subscribe && !camera_control->record_video;
        int alignment_orientation = ui_state->alignment_orientation.load(std::memory_order_acquire);
        const char* orientation_items[] = {"Horizontal ruler", "Vertical ruler"};
        if (ImGui::Combo("Alignment target", &alignment_orientation, orientation_items, IM_ARRAYSIZE(orientation_items))) {
            ui_state->alignment_orientation.store(alignment_orientation, std::memory_order_release);
        }

        if (!can_preview) {
            ImGui::TextDisabled("Stop streaming and recording before using live ruler alignment.");
        }

        if (!alignment_running) {
            if (ImGui::Button("Start live alignment")) {
                if (can_preview) {
                    start_fov_alignment_worker(ui_state, ecams, cameras_params);
                }
            }
        } else {
            if (ImGui::Button("Stop live alignment")) {
                stop_fov_alignment_worker(ui_state);
            }
        }
        ImGui::SameLine();
        ImGui::Text("%s", alignment_running ? "Live" : "Stopped");

        std::string fov_status;
        std::string fov_error;
        bool preview_available = false;
        RulerAlignmentMetrics live_metrics;
        {
            std::lock_guard<std::mutex> lock(ui_state->alignment_mutex);
            fov_status = ui_state->live_fov_preview.status_message;
            fov_error = ui_state->live_fov_preview.error_message;
            preview_available = ui_state->live_fov_preview.available;
            live_metrics = ui_state->live_fov_preview.metrics;
        }
        if (!fov_status.empty()) {
            ImGui::TextWrapped("%s", fov_status.c_str());
        }
        if (!fov_error.empty()) {
            ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.35f, 1.0f), "%s", fov_error.c_str());
        }
        if (live_metrics.has_detected_line) {
            ImGui::Text("Line angle %.2f deg, angle error %.2f deg, center offset %.1f px (%.3f)",
                        live_metrics.line_angle_deg,
                        live_metrics.angle_error_deg,
                        live_metrics.center_offset_px,
                        live_metrics.center_offset_fraction);
        }

        if (preview_available && upload_latest_alignment_texture(ui_state) && ui_state->alignment_texture != 0) {
            const float width = static_cast<float>(ui_state->alignment_texture_width);
            const float height = static_cast<float>(ui_state->alignment_texture_height);
            const ImVec2 available = ImGui::GetContentRegionAvail();
            const float width_scale = available.x > 0.0f ? available.x / width : 1.0f;
            const float height_limit = std::clamp(ImGui::GetIO().DisplaySize.y * 0.55f, 480.0f, 1100.0f);
            const float height_scale = height_limit / std::max(1.0f, height);
            const float scale = std::min(1.0f, std::min(width_scale, height_scale));
            ImGui::Image(
                (ImTextureID)(intptr_t)ui_state->alignment_texture,
                ImVec2(width * scale, height * scale));
        }

        std::string capture_error;
        if (ImGui::Button("Capture horizontal ruler")) {
            if (!capture_current_fov_snapshot(ui_state, true, &capture_error)) {
                std::lock_guard<std::mutex> lock(ui_state->alignment_mutex);
                ui_state->live_fov_preview.error_message = capture_error;
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("Capture vertical ruler")) {
            if (!capture_current_fov_snapshot(ui_state, false, &capture_error)) {
                std::lock_guard<std::mutex> lock(ui_state->alignment_mutex);
                ui_state->live_fov_preview.error_message = capture_error;
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("Clear ruler captures")) {
            std::lock_guard<std::mutex> lock(ui_state->alignment_mutex);
            ui_state->horizontal_capture = {};
            ui_state->vertical_capture = {};
        }

        {
            std::lock_guard<std::mutex> lock(ui_state->alignment_mutex);
            if (ui_state->horizontal_capture.available) {
                ImGui::Text("Horizontal capture: %dx%d, angle error %.2f deg, offset %.3f",
                            ui_state->horizontal_capture.width,
                            ui_state->horizontal_capture.height,
                            ui_state->horizontal_capture.metrics.angle_error_deg,
                            ui_state->horizontal_capture.metrics.center_offset_fraction);
            }
            if (ui_state->vertical_capture.available) {
                ImGui::Text("Vertical capture: %dx%d, angle error %.2f deg, offset %.3f",
                            ui_state->vertical_capture.width,
                            ui_state->vertical_capture.height,
                            ui_state->vertical_capture.metrics.angle_error_deg,
                            ui_state->vertical_capture.metrics.center_offset_fraction);
            }
        }
    }

    ImGui::Separator();
    ImGui::InputFloat("Saturated white fraction", &ui_state->saturated_white_fraction, 0.0001f, 0.001f, "%.4f");
    ImGui::InputFloat("Saturated p99 min", &ui_state->saturated_p99_min, 1.0f, 5.0f, "%.1f");
    ImGui::InputFloat("Dim mean max", &ui_state->dim_mean_max, 1.0f, 5.0f, "%.1f");
    ImGui::InputFloat("Dim p95 max", &ui_state->dim_p95_max, 1.0f, 5.0f, "%.1f");
    ImGui::InputFloat("Dim black fraction min", &ui_state->dim_black_fraction_min, 0.01f, 0.1f, "%.2f");

    ImGui::Separator();
    ImGui::InputText("Artifact root", ui_state->output_dir, IM_ARRAYSIZE(ui_state->output_dir));
    ImGui::InputText("Artifact label", ui_state->output_prefix, IM_ARRAYSIZE(ui_state->output_prefix));

    if (running) {
        ImGui::EndDisabled();
    }

    const bool can_run = !running && !camera_control->subscribe && !camera_control->record_video;
    if (!can_run) {
        ImGui::TextDisabled("Stop streaming and recording before starting characterization.");
    }

    if (ImGui::Button("Run Characterization")) {
        if (can_run) {
            start_aperture_characterization_worker(ui_state, ecams, cameras_params);
        }
    }

    if (running) {
        const int completed_steps = ui_state->progress_completed_steps.load(std::memory_order_acquire);
        const int total_steps = std::max(1, ui_state->progress_total_steps.load(std::memory_order_acquire));
        const int current_iris = ui_state->progress_iris.load(std::memory_order_acquire);
        ImGui::SameLine();
        ImGui::Text("Running...");
        ImGui::ProgressBar(static_cast<float>(completed_steps) / static_cast<float>(total_steps), ImVec2(-1.0f, 0.0f));
        ImGui::Text("Completed %d / %d steps. Last iris=%d", completed_steps, total_steps, current_iris);
    }

    std::string status_message;
    std::string error_message;
    std::string output_artifact_id;
    std::string output_artifact_dir;
    std::string output_manifest_path;
    std::string output_fingerprint;
    std::string output_json_path;
    std::string output_steps_csv_path;
    std::string output_frames_csv_path;
    std::string output_frame_image_dir;
    bool has_result = false;
    ApertureCharacterizationResult last_result;
    FovCalibrationData last_fov_calibration;
    std::string last_camera_serial;
    unsigned int last_focus = 0;
    unsigned int last_exposure = 0;
    {
        std::lock_guard<std::mutex> lock(ui_state->mutex);
        status_message = ui_state->status_message;
        error_message = ui_state->error_message;
        output_artifact_id = ui_state->output_artifact_id;
        output_artifact_dir = ui_state->output_artifact_dir;
        output_manifest_path = ui_state->output_manifest_path;
        output_fingerprint = ui_state->output_fingerprint;
        output_json_path = ui_state->output_json_path;
        output_steps_csv_path = ui_state->output_steps_csv_path;
        output_frames_csv_path = ui_state->output_frames_csv_path;
        output_frame_image_dir = ui_state->output_frame_image_dir;
        has_result = ui_state->has_result;
        last_result = ui_state->last_result;
        last_fov_calibration = ui_state->last_fov_calibration;
        last_camera_serial = ui_state->last_camera_serial;
        last_focus = ui_state->last_focus;
        last_exposure = ui_state->last_exposure;
    }

    ImGui::Separator();
    ImGui::TextWrapped("%s", status_message.c_str());
    if (!error_message.empty()) {
        ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.35f, 1.0f), "%s", error_message.c_str());
    }

    if (has_result) {
        ImGui::Text("Last run: camera=%s focus=%u exposure=%u", last_camera_serial.c_str(), last_focus, last_exposure);
        ImGui::TextWrapped("Artifact ID: %s", output_artifact_id.c_str());
        if (ImGui::SmallButton("Copy Artifact ID")) {
            ImGui::SetClipboardText(output_artifact_id.c_str());
        }
        ImGui::TextWrapped("Fingerprint: %s", output_fingerprint.c_str());
        if (ImGui::SmallButton("Copy Fingerprint")) {
            ImGui::SetClipboardText(output_fingerprint.c_str());
        }
        ImGui::TextWrapped("Artifact dir: %s", output_artifact_dir.c_str());
        ImGui::TextWrapped("Manifest: %s", output_manifest_path.c_str());
        if (ImGui::SmallButton("Copy Manifest Path")) {
            ImGui::SetClipboardText(output_manifest_path.c_str());
        }
        ImGui::TextWrapped("Measurement JSON: %s", output_json_path.c_str());
        ImGui::TextWrapped("Steps CSV: %s", output_steps_csv_path.c_str());
        ImGui::TextWrapped("Frames CSV: %s", output_frames_csv_path.c_str());
        if (!output_frame_image_dir.empty()) {
            ImGui::TextWrapped("Representative frames: %s", output_frame_image_dir.c_str());
        }
        if (last_fov_calibration.enabled) {
            ImGui::Text("FOV metadata: working_distance=%.1f mm pixel_pitch=%.3f um",
                        last_fov_calibration.working_distance_mm,
                        last_fov_calibration.pixel_pitch_um);
            if (last_fov_calibration.has_field_width_mm || last_fov_calibration.has_field_height_mm) {
                if (last_fov_calibration.has_field_width_mm && last_fov_calibration.has_field_height_mm) {
                    ImGui::Text("Field size: %.3f mm x %.3f mm",
                                last_fov_calibration.field_width_mm,
                                last_fov_calibration.field_height_mm);
                } else if (last_fov_calibration.has_field_width_mm) {
                    ImGui::Text("Field width: %.3f mm", last_fov_calibration.field_width_mm);
                } else {
                    ImGui::Text("Field height: %.3f mm", last_fov_calibration.field_height_mm);
                }
            }
            if (last_fov_calibration.has_mean_magnification) {
                ImGui::Text("Mean magnification: %.4f", last_fov_calibration.mean_magnification);
            }
            if (last_fov_calibration.has_effective_reference_f_number) {
                ImGui::Text("Approx effective reference f-number: %.3f",
                            last_fov_calibration.effective_reference_f_number);
            }
        }
        if (last_result.has_saturation_boundary) {
            ImGui::Text("Saturation-limited through iris %u", last_result.saturation_limited_through_iris);
        }
        if (last_result.has_usable_window) {
            ImGui::Text("Usable iris range: [%u, %u]", last_result.usable_iris_min, last_result.usable_iris_max);
        }
        if (last_result.has_dim_boundary) {
            ImGui::Text("Too dim from iris %u onward", last_result.dim_limited_from_iris);
        }
        for (const std::string& warning : last_result.warnings) {
            ImGui::TextWrapped("Warning: %s", warning.c_str());
        }

        std::vector<double> plot_iris;
        std::vector<double> plot_step_mean;
        std::vector<double> plot_frame_iris;
        std::vector<double> plot_frame_mean;
        plot_iris.reserve(last_result.steps.size());
        plot_step_mean.reserve(last_result.steps.size());
        size_t total_samples = 0;
        for (const ApertureStepResult& step : last_result.steps) {
            total_samples += step.samples.size();
        }
        plot_frame_iris.reserve(total_samples);
        plot_frame_mean.reserve(total_samples);
        for (const ApertureStepResult& step : last_result.steps) {
            plot_iris.push_back(static_cast<double>(step.iris));
            plot_step_mean.push_back(step.summary.mean);
            for (const ApertureFrameSample& sample : step.samples) {
                plot_frame_iris.push_back(static_cast<double>(step.iris));
                plot_frame_mean.push_back(sample.stats.mean);
            }
        }

        if (!plot_iris.empty() && ImPlot::BeginPlot("Aperture Mean Intensity", ImVec2(-1.0f, 260.0f))) {
            ImPlot::SetupAxes("Iris", "Mean Intensity");
            ImPlot::SetupAxisLimits(ImAxis_X1,
                                    plot_iris.front() - 0.5,
                                    plot_iris.back() + 0.5,
                                    ImGuiCond_Always);
            ImPlot::SetupAxisLimits(ImAxis_Y1, 0.0, 255.0, ImGuiCond_Always);

            if (!plot_frame_iris.empty()) {
                ImPlot::SetNextMarkerStyle(ImPlotMarker_Circle, 3.0f, ImVec4(0.75f, 0.75f, 0.75f, 0.55f));
                ImPlot::PlotScatter("Frame mean", plot_frame_iris.data(), plot_frame_mean.data(),
                                    static_cast<int>(plot_frame_iris.size()));
            }

            ImPlot::SetNextLineStyle(ImVec4(0.15f, 0.7f, 0.3f, 1.0f), 2.0f);
            ImPlot::SetNextMarkerStyle(ImPlotMarker_Square, 4.0f, ImVec4(0.15f, 0.7f, 0.3f, 1.0f));
            ImPlot::PlotLine("Step mean", plot_iris.data(), plot_step_mean.data(),
                             static_cast<int>(plot_iris.size()));
            ImPlot::EndPlot();
        }

        if (!last_result.steps.empty()) {
            ui_state->selected_heatmap_step =
                std::clamp(ui_state->selected_heatmap_step, 0, static_cast<int>(last_result.steps.size()) - 1);

            std::vector<std::string> heatmap_labels_storage;
            std::vector<const char*> heatmap_labels;
            heatmap_labels_storage.reserve(last_result.steps.size());
            heatmap_labels.reserve(last_result.steps.size());
            for (const ApertureStepResult& step : last_result.steps) {
                std::ostringstream label;
                label << "iris " << step.iris << " (" << aperture_classification_to_string(step.classification) << ")";
                heatmap_labels_storage.push_back(label.str());
            }
            for (const std::string& label : heatmap_labels_storage) {
                heatmap_labels.push_back(label.c_str());
            }

            ImGui::Separator();
            ImGui::Combo("Heatmap step", &ui_state->selected_heatmap_step, heatmap_labels.data(),
                         static_cast<int>(heatmap_labels.size()));

            const ApertureStepResult& heatmap_step =
                last_result.steps[static_cast<size_t>(ui_state->selected_heatmap_step)];
            if (heatmap_step.has_grid &&
                !heatmap_step.grid.tile_relative_mean.empty() &&
                heatmap_step.grid.rows > 0 &&
                heatmap_step.grid.cols > 0) {
                ImGui::Text("Grid uniformity at iris %u: min=%.3fx max=%.3fx cv=%.4f",
                            heatmap_step.iris,
                            heatmap_step.grid.min_relative_mean,
                            heatmap_step.grid.max_relative_mean,
                            heatmap_step.grid.cv_relative_mean);
                if (ensure_aperture_preview_texture(ui_state, heatmap_step.representative_frame_path)) {
                    double scale_min = heatmap_step.grid.min_relative_mean;
                    double scale_max = heatmap_step.grid.max_relative_mean;
                    if (scale_max <= scale_min) {
                        scale_max = scale_min + 1e-6;
                    }
                    const double image_width = static_cast<double>(ui_state->preview_texture_width);
                    const double image_height = static_cast<double>(ui_state->preview_texture_height);
                    if (ImGui::Button("Fit representative view")) {
                        ui_state->representative_canvas_view.fit_requested = true;
                    }
                    if (orange::ui::begin_image_canvas("Representative Frame Overlay",
                                                       ui_state->preview_texture,
                                                       ui_state->preview_texture_width,
                                                       ui_state->preview_texture_height,
                                                       &ui_state->representative_canvas_view,
                                                       0.60f,
                                                       "##aperture_preview")) {
                        ImPlot::PushPlotClipRect();
                        ImDrawList* draw_list = ImPlot::GetPlotDrawList();
                        for (unsigned int row = 0; row < heatmap_step.grid.rows; ++row) {
                            const double y0 = (static_cast<double>(row) * image_height) / heatmap_step.grid.rows;
                            const double y1 = (static_cast<double>(row + 1) * image_height) / heatmap_step.grid.rows;
                            for (unsigned int col = 0; col < heatmap_step.grid.cols; ++col) {
                                const double x0 = (static_cast<double>(col) * image_width) / heatmap_step.grid.cols;
                                const double x1 = (static_cast<double>(col + 1) * image_width) / heatmap_step.grid.cols;
                                const size_t tile_index = static_cast<size_t>(row) * heatmap_step.grid.cols + col;
                                const double value = heatmap_step.grid.tile_relative_mean[tile_index];
                                const float t = static_cast<float>(std::clamp((value - scale_min) / (scale_max - scale_min), 0.0, 1.0));
                                ImVec4 color = ImPlot::SampleColormap(t, ImPlotColormap_Viridis);
                                color.w = 0.35f;
                                const ImVec2 p0 = ImPlot::PlotToPixels(x0, y0);
                                const ImVec2 p1 = ImPlot::PlotToPixels(x1, y1);
                                draw_list->AddRectFilled(p0, p1, ImGui::ColorConvertFloat4ToU32(color));
                                draw_list->AddRect(p0, p1, IM_COL32(255, 255, 255, 70));
                            }
                        }
                        ImPlot::PopPlotClipRect();
                        ImPlot::EndPlot();
                    }
                    ImGui::TextDisabled("Wheel zooms. Drag to pan. Use Fit representative view to reset.");
                    ImPlot::ColormapScale("Relative mean scale",
                                          scale_min,
                                          scale_max,
                                          ImVec2(60.0f, 200.0f),
                                          "%.3f",
                                          ImPlotColormapScaleFlags_None,
                                          ImPlotColormap_Viridis);
                } else if (!ui_state->preview_texture_error.empty()) {
                    ImGui::TextDisabled("%s", ui_state->preview_texture_error.c_str());
                }
            } else {
                ImGui::TextDisabled("Grid heatmap unavailable for the selected step.");
            }
        }

        if (ImGui::BeginTable("ApertureCharacterizationResults", 10,
                              ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchSame)) {
            ImGui::TableSetupColumn("Iris");
            ImGui::TableSetupColumn("RB Set");
            ImGui::TableSetupColumn("RB End");
            ImGui::TableSetupColumn("Class");
            ImGui::TableSetupColumn("Mean");
            ImGui::TableSetupColumn("P95");
            ImGui::TableSetupColumn("White%");
            ImGui::TableSetupColumn("dEV");
            ImGui::TableSetupColumn("f-num");
            ImGui::TableSetupColumn("eff f-num");
            ImGui::TableHeadersRow();
            for (size_t step_index = 0; step_index < last_result.steps.size(); ++step_index) {
                const ApertureStepResult& step = last_result.steps[step_index];
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                const std::string iris_label = std::to_string(step.iris) + "##aperture_step_" + std::to_string(step_index);
                if (ImGui::Selectable(iris_label.c_str(), ui_state->selected_heatmap_step == static_cast<int>(step_index))) {
                    ui_state->selected_heatmap_step = static_cast<int>(step_index);
                }
                ImGui::TableNextColumn();
                if (step.has_iris_readback_after_set) {
                    ImGui::Text("%u%s",
                                step.iris_readback_after_set,
                                step.iris_verified_after_set ? "" : " !");
                } else {
                    ImGui::TextUnformatted("-");
                }
                ImGui::TableNextColumn();
                if (step.has_iris_readback_after_capture) {
                    ImGui::Text("%u%s",
                                step.iris_readback_after_capture,
                                step.iris_verified_after_capture ? "" : " !");
                } else {
                    ImGui::TextUnformatted("-");
                }
                ImGui::TableNextColumn(); ImGui::Text("%s", aperture_classification_to_string(step.classification));
                ImGui::TableNextColumn(); ImGui::Text("%.2f", step.summary.mean);
                ImGui::TableNextColumn(); ImGui::Text("%.1f", step.summary.p95);
                ImGui::TableNextColumn(); ImGui::Text("%.3f", step.summary.white_fraction);
                ImGui::TableNextColumn(); ImGui::Text("%.2f", step.delta_ev);
                ImGui::TableNextColumn();
                if (step.has_estimated_f_number) {
                    ImGui::Text("%.2f", step.estimated_f_number);
                } else {
                    ImGui::TextUnformatted("-");
                }
                ImGui::TableNextColumn();
                if (step.has_estimated_effective_f_number) {
                    ImGui::Text("%.2f", step.estimated_effective_f_number);
                } else {
                    ImGui::TextUnformatted("-");
                }
            }
            ImGui::EndTable();
        }
    }

    ImGui::End();
}

}  // namespace


void RenderSpeedGraph(int camera_id, YOLOv8Worker* yolo_worker, SpeedTrackingData& speed_data) {
    if (!yolo_worker) return;
    
    // Get current tracked objects from YOLO worker
    auto tracked_objects = yolo_worker->getTrackedObjects();
    
    // Update speed data WITH CM/S SPEEDS
    static float app_time = 0.0f;
    app_time += ImGui::GetIO().DeltaTime;
    
    for (const auto& obj : tracked_objects) {
        // USE CALIBRATED SPEED IN CM/S
        speed_data.AddSpeedData(obj.track_id, obj.current_speed_physical_units, app_time);
    }
    
    // Clean up old tracks
    speed_data.ClearOldTracks(app_time);
    
    // Render the speed graph
    ImGui::Separator();
    ImGui::Text("Object Speed Tracking");
    
    if (ImGui::CollapsingHeader("Speed Graph", ImGuiTreeNodeFlags_DefaultOpen)) {
        static float history = 10.0f;
        ImGui::SliderFloat("Time Window", &history, 2.0f, 30.0f, "%.1f s");
        
        // Make graph size responsive
        ImVec2 available = ImGui::GetContentRegionAvail();
        available.y -= 80;
        ImVec2 graph_size = ImVec2(-1, std::max(250.0f, available.y));
        
        // CHANGE PLOT TITLE AND Y-AXIS LABEL
        if (ImPlot::BeginPlot("Speed (cm/sec)", graph_size)) {
            // Setup axes
            ImPlot::SetupAxes("Time (s)", "Speed (cm/s)");
            ImPlot::SetupAxisLimits(ImAxis_X1, app_time - history, app_time, ImGuiCond_Always);
            ImPlot::SetupAxisLimits(ImAxis_Y1, 0, speed_data.max_speed_seen);
            
            // Plot each track
            for (const auto& [track_id, buffer] : speed_data.track_buffers) {
                if (buffer.Data.size() > 1) {
                    ImPlot::SetNextLineStyle(speed_data.track_colors.at(track_id), 2.0f);
                    std::string label = "Track " + std::to_string(track_id);
                    ImPlot::PlotLine(label.c_str(), 
                                   &buffer.Data[0].x, &buffer.Data[0].y, 
                                   buffer.Data.size(), 0, buffer.Offset, 
                                   2 * sizeof(float));
                }
            }
            
            ImPlot::EndPlot();
        }
        
        // CHANGE CURRENT SPEEDS DISPLAY TO CM/S
        if (!tracked_objects.empty()) {
            ImGui::Text("Current Speeds:");
            for (const auto& obj : tracked_objects) {
                ImVec4 color = speed_data.track_colors[obj.track_id];
                ImGui::TextColored(color, "Track %d: %.2f cm/s", 
                                 obj.track_id, obj.current_speed_physical_units);
            }
        } else {
            ImGui::TextDisabled("No objects being tracked");
        }
    }
}


simplelogger::Logger *logger = simplelogger::LoggerFactory::CreateConsoleLogger();

#define display_gpu_id 0

int main(int argc, char **args) {

    // Initialize the YOLOv8 plugins
    YOLOv8::initialize_plugins();

    gx_context *window = (gx_context *) malloc(sizeof(gx_context));
    *window = (gx_context){
        .swap_interval = 1,
        .width = 1920,
        .height = 1080,
        .render_target_title = (char *) "Orange",
        .glsl_version = (char *) malloc(100)
    };

    render_initialize_target(window);

    int max_cameras = 20;
    int cam_count;
    GigEVisionDeviceInfo unsorted_device_info[max_cameras];
    cam_count = scan_cameras(max_cameras, unsorted_device_info);
    GigEVisionDeviceInfo device_info[max_cameras];
    sort_cameras_ip(unsorted_device_info, device_info, cam_count);

    std::filesystem::path cwd = std::filesystem::current_path();
    std::string delimiter = "/";
    std::vector<std::string> tokenized_path = string_split(cwd, delimiter);
    std::string orange_root_dir_str = "/home/" + tokenized_path[2] + "/orange_data";
    prepare_application_folders(orange_root_dir_str);
    std::string app_storage_warning;
    std::string input_folder = resolve_default_recording_root(orange_root_dir_str, &app_storage_warning);
    if (!app_storage_warning.empty()) {
        std::cerr << "App storage config warning: " << app_storage_warning << std::endl;
    }

    std::string yolo_model_folder = orange_root_dir_str + "/detect";
    std::string yolo_model = yolo_model_folder + "/fish_jinyao.engine";
    
    bool camera_is_selected[cam_count]{0};
    CameraParams *cameras_params = nullptr;
    CameraEachSelect *cameras_select = nullptr;
    CameraEmergent *ecams = nullptr;
    std::vector<std::thread> camera_threads;
    GL_Texture *tex = nullptr;
    GL_Texture* crop_tex = nullptr;
    int num_cameras = 0;
    int stream_downsample = 1;
    CameraControl *camera_control = new CameraControl();

    int evt_buffer_size{100};
    PTPParams *ptp_params = new PTPParams{0, 0, 0, 0, false, false, false, false};
    COpenGLDisplay** openGLDisplayWorkers = nullptr;
    CropAndEncodeWorker** cropAndEncodeWorkers = nullptr;
    ImageWriterWorker* image_writer = new ImageWriterWorker("ImageSaverThread");
    image_writer->StartThread();

    std::vector<CameraResources> camera_resources;
    std::vector<std::unique_ptr<ModernRecordingPipeline>> recording_pipelines;
    std::vector<std::unique_ptr<FrameIPCManager>> frame_ipc_managers;
    std::vector<std::string> frame_ipc_init_errors;
    std::vector<std::string> recording_preflight_errors;

    EncoderConfig *encoder_config = new EncoderConfig{
        "h264",
        "p1",
        "ll",
        "vbr",
        20,
        0,
        "factor",
        1,
        1024,
        1024,
        ""
    };
    std::vector<std::string> camera_config_files;

    ScrollingBuffer *realtime_plot_data = nullptr;
    bool show_realtime_plot = false;
    bool ptp_stream_sync = false;

    flatbuffers::FlatBufferBuilder *fb_builder = new flatbuffers::FlatBufferBuilder(1024);

    bool enet_runtime_initialized = false;
    bool enet_server_initialized = false;
    EnetContext server;
    if (::enet_initialize() != 0) {
        std::cerr << "[ENet] Global initialization failed; networking disabled." << std::endl;
    } else {
        enet_runtime_initialized = true;
        if (enet_initialize(&server, 3333, 5)) {
            enet_server_initialized = true;
            printf("Server Initiated\n");
        } else {
            std::cerr << "[ENet] Host initialization failed; ENet thread not started." << std::endl;
        }
    }
    ConnectedServer my_servers[2];
    intialize_servers(my_servers);

    INDIGOSignalBuilder indigo_signal_builder{};
    indigo_signal_builder = {
        .builder = fb_builder,
        .server = &server,
        .indigo_connection = nullptr
    };

    std::vector<std::string> network_config_folders;
    std::string network_start_folder_name = orange_root_dir_str + "/config/network";
    for (const auto &entry: std::filesystem::directory_iterator(network_start_folder_name)) {
        network_config_folders.push_back(entry.path().string());
    }
    int network_config_select = 0;

    std::vector<std::string> local_config_folders;
    std::string local_start_folder_name = orange_root_dir_str + "/config/local";
    list_child_directories(local_start_folder_name, local_config_folders);
    std::string picture_save_folder = orange_root_dir_str + "/pictures/" + get_current_date();
    std::string calib_save_folder = orange_root_dir_str + "/exp/calibration/" + get_current_date();
    std::string aperture_char_output_folder = orange_root_dir_str + "/calibrations/artifacts";
    std::string usaf_output_folder = orange_root_dir_str + "/calibrations/artifacts";
    orange::gui::HostPtpStackUiState host_ptp_stack_ui;
    ApertureCharacterizationUiState aperture_ui_state;
    copy_string_to_buffer(aperture_ui_state.output_dir, aperture_char_output_folder);
    UsafResolutionUiState usaf_ui_state;
    std::snprintf(usaf_ui_state.output_dir, sizeof(usaf_ui_state.output_dir), "%s", usaf_output_folder.c_str());
    SpatialLayoutUiState spatial_layout_ui_state;

    int local_config_select = 0;
    char new_local_config_folder_name[128] = "";
    std::string local_config_status;
    bool local_config_status_error = false;
    bool select_all_cameras = false;
    char *temp_string = (char *) malloc(64);
    *temp_string = '\0';
    bool save_image_all_ready = true;
    bool quite_enet = false;

    std::thread enet_thread;
    if (enet_server_initialized) {
        enet_thread = std::thread(&create_enet_thread, &server, my_servers, &indigo_signal_builder,
                                  &quite_enet);
    }
    std::vector<std::string> color_temps = { "CT_Off", "CT_2800K", "CT_3000K", "CT_4000K", "CT_5000K", "CT_6500K", "CT_Custom"};
    auto refresh_local_config_folders = [&]() {
        list_child_directories(local_start_folder_name, local_config_folders);
        if (local_config_select > static_cast<int>(local_config_folders.size())) {
            local_config_select = static_cast<int>(local_config_folders.size());
        }
    };

    while (!glfwWindowShouldClose(window->render_target)) {
        orange::gui::reap_host_ptp_stack_worker(&host_ptp_stack_ui);
        join_aperture_worker_if_finished(&aperture_ui_state);
        join_alignment_worker_if_finished(&aperture_ui_state);
        join_usaf_worker_if_finished(&usaf_ui_state);
        join_usaf_preview_worker_if_finished(&usaf_ui_state);
        const bool aperture_job_running = aperture_ui_state.running.load(std::memory_order_acquire);
        const bool aperture_alignment_running = aperture_ui_state.alignment_running.load(std::memory_order_acquire);
        const bool aperture_tool_busy = aperture_job_running || aperture_alignment_running;
        const bool usaf_job_running = usaf_ui_state.running.load(std::memory_order_acquire);
        const bool usaf_preview_running = usaf_ui_state.preview_running.load(std::memory_order_acquire);
        const bool usaf_tool_busy = usaf_job_running || usaf_preview_running;
        const bool calibration_tool_busy = aperture_tool_busy || usaf_tool_busy;
        create_new_frame();
        
        if (ImGui::Begin("Orange", nullptr, ImGuiWindowFlags_MenuBar)) {
            ImGui::Text("Application average %.3f ms/frame (%.1f FPS)", 1000.0f / ImGui::GetIO().Framerate,
                       ImGui::GetIO().Framerate);

            if (camera_control->open) {
                // ImGui::BeginDisabled();
            }

            if (ImGui::BeginTable("Cameras", 3,
                                  ImGuiTableFlags_Resizable | ImGuiTableFlags_NoSavedSettings |
                                  ImGuiTableFlags_Borders)) {
                for (int i = 0; i < cam_count; i++) {
                    sprintf(temp_string, "%d", i);
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    ImGui::Selectable(temp_string, &camera_is_selected[i], ImGuiSelectableFlags_SpanAllColumns);
                    ImGui::TableNextColumn();
                    ImGui::Text("%s", device_info[i].serialNumber);
                    ImGui::TableNextColumn();
                    ImGui::Text("%s", device_info[i].currentIp);
                }
                ImGui::EndTable();
            }

            if (ImGui::Button(select_all_cameras ? "Clear all" : "Select all")) {
                select_all_cameras = !select_all_cameras;
                if (select_all_cameras) {
                    for (int i = 0; i < cam_count; i++) {
                        camera_is_selected[i] = true;
                    }
                } else {
                    for (int i = 0; i < cam_count; i++) {
                        camera_is_selected[i] = false;
                    }
                }
            }

            if (camera_control->open) {
                // ImGui::EndDisabled();
            }

            if (camera_control->subscribe) {
                // ImGui::BeginDisabled();
            }

            ImGui::Separator();
            ImGui::Spacing();

            // selection for yolo model
            if (ImGui::Button("Select YOLO")) {
                IGFD::FileDialogConfig config;
                config.countSelectionMax = 1;
                config.path = yolo_model_folder;
                ImGuiFileDialog::Instance()->OpenDialog("ChooseYOLOFile", "Choose File", ".engine", config);
            }
            ImGui::SameLine();
            ImGui::Text("%s", yolo_model.c_str());

            if (camera_control->subscribe) {
                // ImGui::EndDisabled();
            }

            if (camera_control->record_video) {
                // ImGui::BeginDisabled();
            }

            const orange::gui::RecordingPanelActions recording_panel_actions =
                orange::gui::render_recording_config_panel(
                    &input_folder,
                    encoder_config,
                    camera_control->open,
                    camera_control->subscribe,
                    cameras_params,
                    cameras_select,
                    num_cameras,
                    &recording_preflight_errors);
            if (recording_panel_actions.choose_recording_dir_requested) {
                IGFD::FileDialogConfig config;
                config.countSelectionMax = 1;
                config.path = input_folder;
                ImGuiFileDialog::Instance()->OpenDialog("ChooseRecordingDir", "Choose a Directory", nullptr, config);
            }
            {
                const char *items[] = {"1", "2", "4", "8", "16"};
                static const int item_numbers[] = {1, 2, 4, 8, 16};
                static int downsample_current = 0;
                if(ImGui::Combo("downsample streaming", &downsample_current, items, IM_ARRAYSIZE(items))) {
                    for (int i = 0; i < num_cameras; i++) {
                        cameras_select[i].downsample = item_numbers[downsample_current];
                    }
                }
            }
            int fps_temp = streaming_target_fps.load(); // get the current atomic value

            if (ImGui::InputInt("streaming fps", &fps_temp)) {
                // Clamp if necessary
                if (fps_temp < 1) fps_temp = 1;
                if (fps_temp > 240) fps_temp = 240;
                streaming_target_fps.store(fps_temp); // write it back safely
            }
            
   
            if (camera_control->record_video) {
                // ImGui::EndDisabled();
            }

            if (camera_control->open) {
                if (camera_control->record_video) {
                    // ImGui::BeginDisabled();
                }

                ImGui::Checkbox("Show camera temperature", &show_realtime_plot);
                ImGui::SameLine();
                if (ImGui::Button("Aperture Characterization")) {
                    aperture_ui_state.show_window = true;
                    aperture_ui_state.selected_camera = std::clamp(aperture_ui_state.selected_camera, 0, std::max(0, num_cameras - 1));
                }
                ImGui::SameLine();
                if (ImGui::Button("USAF Resolution Calibration")) {
                    usaf_ui_state.show_window = true;
                    usaf_ui_state.selected_camera = std::clamp(usaf_ui_state.selected_camera, 0, std::max(0, num_cameras - 1));
                }
                ImGui::SameLine();
                if (ImGui::Button("Spatial Layout Registration")) {
                    spatial_layout_ui_state.show_window = true;
                    spatial_layout_ui_state.selected_camera =
                        std::clamp(spatial_layout_ui_state.selected_camera, 0, std::max(0, num_cameras - 1));
                }

                if (calibration_tool_busy) {
                    ImGui::BeginDisabled();
                }

                const std::string selected_local_config_folder =
                    (local_config_select >= 0 &&
                     local_config_select < static_cast<int>(local_config_folders.size()))
                        ? local_config_folders[local_config_select]
                        : std::string();
                orange::gui::render_camera_properties_panel(
                    ecams,
                    cameras_params,
                    num_cameras,
                    color_temps,
                    selected_local_config_folder);

                if (camera_control->record_video) {
                    // ImGui::EndDisabled();
                }

                if (camera_control->subscribe) {
                    // ImGui::BeginDisabled();
                }

                bool stream_all_cameras = true;
                for (int i = 0; i < num_cameras; i++) {
                    if (!cameras_select[i].stream_on) {
                        stream_all_cameras = false;
                        break;
                    }
                }

                bool record_all_cameras = true;
                for (int i = 0; i < num_cameras; i++) {
                    if (!cameras_select[i].record) {
                        record_all_cameras = false;
                        break;
                    }
                }

                if (ImGui::BeginTable("Camera Control Setting", 7,
                                      ImGuiTableFlags_Resizable | ImGuiTableFlags_NoSavedSettings |
                                      ImGuiTableFlags_Borders)) {
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    ImGui::Text("Name");
                    ImGui::TableNextColumn();
                    ImGui::Text("Serial");
                    ImGui::TableNextColumn();
                    ImGui::Text("Stream "); ImGui::SameLine();
                    if(ImGui::Checkbox("All##stream", &stream_all_cameras))
                    {
                        if (stream_all_cameras) {
                            for (int i = 0; i < num_cameras; i++) {
                                cameras_select[i].stream_on = true;
                            }
                        } else {
                            for (int i = 0; i < num_cameras; i++) {
                                cameras_select[i].stream_on = false;
                            }
                        }
                    }

                    ImGui::TableNextColumn();
                    ImGui::Text("Record "); ImGui::SameLine();
                    if(ImGui::Checkbox("All##record", &record_all_cameras))
                    {
                        if (record_all_cameras) {
                            for (int i = 0; i < num_cameras; i++) {
                                cameras_select[i].record = true;
                            }
                        } else {
                            for (int i = 0; i < num_cameras; i++) {
                                cameras_select[i].record = false;
                            }
                        }
                    }

                    ImGui::TableNextColumn();
                    ImGui::Text("YOLO "); ImGui::SameLine();

                    ImGui::TableNextColumn();
                    ImGui::Text("YOLO Debug");

                    ImGui::TableNextColumn();
                    ImGui::Text("YOLO ENet");


                    for (int i = 0; i < num_cameras; i++) {
                        ImGui::TableNextRow();
                        ImGui::TableNextColumn();
                        ImGui::Text("%s", cameras_params[i].camera_name.c_str());
                        ImGui::TableNextColumn();
                        ImGui::Text("%s", cameras_params[i].camera_serial.c_str());
                        ImGui::TableNextColumn();
                        sprintf(temp_string, "##checkbox_stream%d", i);
                        ImGui::Checkbox(temp_string, &cameras_select[i].stream_on);
                        ImGui::TableNextColumn();
                        sprintf(temp_string, "##checkbox_record%d", i);
                        ImGui::Checkbox(temp_string, &cameras_select[i].record);
                        ImGui::TableNextColumn();
                        sprintf(temp_string, "##checkbox_yolo%d", i);
                        ImGui::Checkbox(temp_string, &cameras_select[i].yolo);

                        ImGui::TableNextColumn();
                        if (cameras_select[i].yolo)
                        {
                            // Button is always visible and clickable if YOLO is selected.
                            sprintf(temp_string, "Dump Input##yolo_debug%d", i);
                            if (ImGui::Button(temp_string))
                            {
                                // We still check if the worker is ready before calling the function
                                // to prevent a crash, but the button is never grayed out.
                                if (camera_control->subscribe && i < yolo_workers.size() && yolo_workers[i])
                                {
                                    yolo_workers[i]->DumpNextFrame();
                                }
                                else
                                {
                                    std::cout << "Warning: Cannot dump frame. YOLO worker is not ready for camera "
                                              << cameras_params[i].camera_serial << std::endl;
                                }
                            }
                        }

                        ImGui::TableNextColumn();
                        sprintf(temp_string, "##yolo_enet%d", i);
                        ImGui::Checkbox(temp_string, &cameras_select[i].send_yolo_via_enet);
                    }
                    ImGui::EndTable();
                }

                orange::gui::render_frame_ipc_status_panel(
                    camera_control->subscribe,
                    cameras_select,
                    cameras_params,
                    num_cameras,
                    frame_ipc_managers,
                    frame_ipc_init_errors);

                if (camera_control->subscribe) {
                    // ImGui::EndDisabled();
                }

                if (calibration_tool_busy) {
                    ImGui::EndDisabled();
                }

                if (camera_control->subscribe == true) {
                    ImGui::Separator();
                    ImGui::Spacing();
                    if (ImGui::Button("Picture save to")) {
                        make_folder(picture_save_folder);
                        for (int i = 0; i < num_cameras; i++) {
                            cameras_select[i].pictures_counter = 0;
                        }
                        IGFD::FileDialogConfig config;
                        config.countSelectionMax = 1;
                        config.path = picture_save_folder;
                        ImGuiFileDialog::Instance()->OpenDialog("ChoosePictureDir", "Choose a Directory", nullptr,
                                                                config);
                    }
                    ImGui::SameLine();
                    ImGui::Text("%s", picture_save_folder.c_str());
                    static int current_picture_format = 0;
                    const char* picture_format_items[] = { "jpg", "tiff", "png"};
                    ImGui::Combo("Picture format", &current_picture_format, picture_format_items, IM_ARRAYSIZE(picture_format_items));
                    for (int i = 0; i < num_cameras; i++) {
                        cameras_select[i].frame_save_format = std::string(picture_format_items[current_picture_format]);
                    }

                    if (ImGui::TreeNode("Save pictures from capturing")) {
                        const int cols = 5;
                        for (int i = 0; i < num_cameras; ++i) {     
                            std::string label = cameras_params[i].camera_name + ": " + std::to_string(cameras_select[i].pictures_counter) + "##calibration_save";
                            if (ImGui::Selectable(label.c_str(), &cameras_select[i].selected_to_save,
                                                    ImGuiSelectableFlags_None,
                                                    ImVec2(150, 50))) {
                            }
    
                            // Keep items on the same line until end of row
                            if ((i + 1) % cols != 0)
                                ImGui::SameLine();
                        }
    
                        ImGui::NewLine();
    
                        ImGui::SeparatorText("Debug Info");
                        ImGui::Text("Window Focused: %d, Window Hovered: %d", ImGui::IsWindowFocused(ImGuiFocusedFlags_AnyWindow), ImGui::IsWindowHovered(ImGuiHoveredFlags_AnyWindow));
                        ImGui::Text("camera_control->subscribe = %s", camera_control->subscribe ? "true" : "false");
                        ImGui::Text("save_image_all_ready = %s", save_image_all_ready ? "true" : "false");
                        ImGui::Separator();
    
                        if (ImGui::Button("Save selected")) {
                            std::cout << "[GUI] 'Save selected' button clicked. Formatting save name." << std::endl;
                            make_folder(picture_save_folder);
                            std::string frame_save_name = get_current_time_milliseconds();
                            for (int i = 0; i < num_cameras; i++) {
                                cameras_select[i].frame_save_name = frame_save_name;
                                cameras_select[i].picture_save_folder = picture_save_folder;
                                if (cameras_select[i].selected_to_save) {
                                    cameras_select[i].frame_save_state = State_Write_New_Frame;
                                    std::cout << "[GUI]   - Flagging camera " << cameras_params[i].camera_serial << " to save frame." << std::endl;
                                }
                            }
                        }
                        ImGui::SameLine();
    
                        if (ImGui::Button("Save pictures all")) {
                            std::cout << "[GUI] 'Save pictures all' button clicked. Formatting save name." << std::endl;
                            make_folder(picture_save_folder);
                            std::string frame_save_name = get_current_time_milliseconds();
                            for (int i = 0; i < num_cameras; i++) {
                                cameras_select[i].frame_save_name = frame_save_name;
                                cameras_select[i].picture_save_folder = picture_save_folder;
                                cameras_select[i].frame_save_state = State_Write_New_Frame;
                            }
                            std::cout << "[GUI]   - Flagging ALL cameras to save frame." << std::endl;
                        }
    

                        if (calib_state == CalibSavePictures) {
                            std::cout << "[GUI] Calibration state is 'CalibSavePictures', triggering next pose." << std::endl;
                            send_indigo_message(indigo_signal_builder.server, indigo_signal_builder.builder, indigo_signal_builder.indigo_connection, FetchGame::SignalType_CalibrationNextPose);
                            calib_state = CalibNextPose;
                        }
                        
                        if (calib_state == CalibPoseReached) {
                            std::cout << "[GUI] Calibration state is 'CalibPoseReached', triggering frame save for all cameras." << std::endl;
                            make_folder(calib_save_folder);
                            for (int i = 0; i < num_cameras; i++) {
                                cameras_select[i].frame_save_name = std::to_string(cameras_select[i].pictures_counter);
                                cameras_select[i].picture_save_folder = calib_save_folder;
                                cameras_select[i].frame_save_state = State_Write_New_Frame;
                            }
                            calib_state = CalibSavePictures;
                        }

                        if (ImGui::Button("Calib save images with counter")) {
                            std::cout << "[GUI] 'Calib save images with counter' button clicked." << std::endl;
                            make_folder(calib_save_folder);
                            for (int i = 0; i < num_cameras; i++) {
                                cameras_select[i].frame_save_name = std::to_string(cameras_select[i].pictures_counter);
                                cameras_select[i].picture_save_folder = calib_save_folder;
                                cameras_select[i].frame_save_state = State_Write_New_Frame;
                            }
                        } 
                        
                        ImGui::TreePop();
                    }
                }
            }
        }
        ImGui::End();

        render_aperture_characterization_window(
            &aperture_ui_state,
            camera_control,
            ecams,
            cameras_params,
            num_cameras,
            aperture_char_output_folder);

        render_usaf_resolution_window(
            &usaf_ui_state,
            camera_control,
            ecams,
            cameras_params,
            num_cameras,
            usaf_output_folder);

        render_spatial_layout_window(
            &spatial_layout_ui_state,
            camera_control,
            ecams,
            cameras_params,
            num_cameras,
            calibration_tool_busy,
            aperture_char_output_folder);

        // file explorer display
        if (ImGuiFileDialog::Instance()->Display("ChooseYOLOFile")) {
            // => will show a dialog
            if (ImGuiFileDialog::Instance()->IsOk()) {
                // action if OK
                yolo_model = ImGuiFileDialog::Instance()->GetFilePathName();
            }
            // close
            ImGuiFileDialog::Instance()->Close();
        }

        if (ImGuiFileDialog::Instance()->Display("ChooseRecordingDir")) {
            // => will show a dialog
            if (ImGuiFileDialog::Instance()->IsOk()) {
                // action if OK
                auto selected_folder = ImGuiFileDialog::Instance()->GetSelection();
                input_folder = ImGuiFileDialog::Instance()->GetCurrentPath();
            }
            // close
            ImGuiFileDialog::Instance()->Close();
        }


        if (ImGuiFileDialog::Instance()->Display("ChoosePictureDir")) {
            // => will show a dialog
            if (ImGuiFileDialog::Instance()->IsOk()) {
                // action if OK
                auto selected_folder = ImGuiFileDialog::Instance()->GetSelection();
                picture_save_folder = ImGuiFileDialog::Instance()->GetCurrentPath();
            }
            // close
            ImGuiFileDialog::Instance()->Close();
        }


        if (ImGui::Begin("Local")) {
            if (camera_control->open) {
                // ImGui::BeginDisabled();
            }

            for (size_t i = 0; i < local_config_folders.size(); i++) {
                std::vector<std::string> folder_token = string_split(local_config_folders[i], "/");
                sprintf(temp_string, "%s", folder_token.back().c_str());
                ImGui::RadioButton(temp_string, &local_config_select, i);
                ImGui::SameLine();
            }
            ImGui::RadioButton("Null", &local_config_select, local_config_folders.size());
            const bool has_selected_local_folder =
                local_config_select >= 0 && local_config_select < static_cast<int>(local_config_folders.size());
            const std::string selected_local_config_folder =
                has_selected_local_folder ? local_config_folders[local_config_select] : std::string();

            ImGui::Separator();
            ImGui::TextWrapped("Selected local folder: %s",
                               has_selected_local_folder ? selected_local_config_folder.c_str() : "Null");
            ImGui::InputText("New local folder", new_local_config_folder_name, IM_ARRAYSIZE(new_local_config_folder_name));
            ImGui::SameLine();
            if (ImGui::Button("Create folder")) {
                const std::string folder_name = trim_ascii_whitespace(new_local_config_folder_name);
                if (folder_name.empty()) {
                    local_config_status = "Folder name is empty.";
                    local_config_status_error = true;
                } else if (folder_name == "." || folder_name == ".." ||
                           folder_name.find('/') != std::string::npos ||
                           folder_name.find('\\') != std::string::npos) {
                    local_config_status = "Folder name must not contain path separators or relative path tokens.";
                    local_config_status_error = true;
                } else {
                    const std::string new_folder_path =
                        (std::filesystem::path(local_start_folder_name) / folder_name).string();
                    std::string ensure_error;
                    if (ensure_directory_exists(new_folder_path, &ensure_error)) {
                        refresh_local_config_folders();
                        auto it = std::find(local_config_folders.begin(), local_config_folders.end(), new_folder_path);
                        if (it != local_config_folders.end()) {
                            local_config_select = static_cast<int>(std::distance(local_config_folders.begin(), it));
                        }
                        new_local_config_folder_name[0] = '\0';
                        local_config_status = std::string("Ready: ") + new_folder_path;
                        local_config_status_error = false;
                    } else {
                        local_config_status = ensure_error.empty() ? "Failed to create local config folder." : ensure_error;
                        local_config_status_error = true;
                    }
                }
            }

            if (camera_control->open && has_selected_local_folder) {
                if (ImGui::Button("Use selected folder for open cameras")) {
                    assign_camera_config_paths(cameras_params, num_cameras, selected_local_config_folder);
                    local_config_status = std::string("Open cameras now save into ") + selected_local_config_folder;
                    local_config_status_error = false;
                }
            }

            if (!local_config_status.empty()) {
                if (local_config_status_error) {
                    ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.35f, 1.0f), "%s", local_config_status.c_str());
                } else {
                    ImGui::TextColored(ImVec4(0.35f, 0.85f, 0.45f, 1.0f), "%s", local_config_status.c_str());
                }
            }

            if (camera_control->open) {
                // ImGui::EndDisabled();
            }

            if (camera_control->subscribe || calibration_tool_busy) {
                ImGui::BeginDisabled();
            }

            if (ImGui::Button(camera_control->open ? "Close Camera" : "Open camera")) {
                if (!camera_control->open) {
                    if (local_config_select < local_config_folders.size()) {
                        update_camera_configs(camera_config_files, local_config_folders[local_config_select]);
                        if (!camera_config_files.empty()) {
                            select_cameras_have_configs(camera_config_files, device_info, camera_is_selected, cam_count);
                        } else {
                            std::cout << "[GUI] Selected local config folder is empty; preserving current camera selection."
                                      << std::endl;
                        }
                    }

                    num_cameras = 0;
                    for (int i = 0; i < cam_count; i++) {
                        if (camera_is_selected[i]) {
                            num_cameras++;
                        }
                    }
                    if (num_cameras > 0) {
                        camera_control->open = true;
                        cameras_params = new CameraParams[num_cameras];
                        cameras_select = new CameraEachSelect[num_cameras];

                        std::vector<int> selected_cameras;
                        for (int i = 0; i < cam_count; i++) {
                            if (camera_is_selected[i]) {
                                selected_cameras.push_back(i);
                            }
                        }

                        std::vector<bool> skip_setting_params;
                        skip_setting_params.resize(num_cameras);
                        for (int i = 0; i < num_cameras; i++) {
                            if (!set_camera_params(&cameras_params[i], &device_info[selected_cameras[i]],
                                                   camera_config_files, selected_cameras[i], num_cameras)) {
                                skip_setting_params[i] = true;
                                cameras_params[i].camera_id = selected_cameras[i];
                                cameras_params[i].num_cameras = num_cameras;
                            } else {
                                skip_setting_params[i] = false;

                            }
                        }


                        for (int i = 0; i < num_cameras; i++) {
                            cameras_select[i].stream_on = false;
                            if (cameras_params[i].camera_name == "ceiling_center") {
                                cameras_select[i].stream_on = true;
                                cameras_select[i].yolo = false;
                            }

                            if (cameras_params[i].camera_name == "shelter") {
                                cameras_select[i].stream_on = true;
                            }

                        }

                        ecams = new CameraEmergent[num_cameras];
                        for (int i = 0; i < num_cameras; i++) {
                            if (!skip_setting_params[i]) {
                                open_camera_with_params(&ecams[i].camera, &device_info[cameras_params[i].camera_id],
                                                    &cameras_params[i], "gui_open_selected_cameras");
                            } else {
                                update_camera_params(&ecams[i].camera, &device_info[cameras_params[i].camera_id],
                                                    &cameras_params[i]);
                            }

                        }
                        int ptp_config_count = 0;
                        for (int i = 0; i < num_cameras; i++) {
                            if (camera_sync_mode_uses_ptp(&cameras_params[i])) {
                                ptp_config_count++;
                            }
                        }
                        if (ptp_config_count == num_cameras && num_cameras > 0) {
                            ptp_stream_sync = true;
                        } else {
                            if (ptp_config_count > 0) {
                                std::cout << "[GUI] Mixed ptp_gate/non-PTP camera configs loaded; leaving PTP Stream Sync unchecked."
                                          << std::endl;
                            }
                            ptp_stream_sync = false;
                        }
                        realtime_plot_data = new ScrollingBuffer[num_cameras];

                    }
                } else {
                    camera_control->open = false;
                    ptp_stream_sync = false;
                    for (int i = 0; i < num_cameras; i++) {
                        close_camera(&ecams[i].camera, &cameras_params[i]);
                    }
                    delete[] cameras_params;
                    cameras_params = nullptr;
                    delete[] cameras_select;
                    cameras_select = nullptr;
                    delete[] ecams;
                    ecams = nullptr;
                    delete[] realtime_plot_data;
                    realtime_plot_data = nullptr;
                }
            }
            if (camera_control->subscribe || calibration_tool_busy) {
                ImGui::EndDisabled();
            }

            orange::gui::render_host_ptp_stack_panel(
                &host_ptp_stack_ui,
                camera_control->subscribe,
                ptp_stream_sync);

            if (!camera_control->record_video && camera_control->open) {
                if (camera_control->subscribe) {
                    // ImGui::BeginDisabled();
                }
                ImGui::Checkbox("PTP Stream Sync", &ptp_stream_sync);
                ImGui::SameLine();
                if (camera_control->subscribe) {
                    // ImGui::EndDisabled();
                }
                if (calibration_tool_busy) {
                    ImGui::BeginDisabled();
                }
                if (ImGui::Button(camera_control->subscribe ? "Stop streaming" : "Start streaming")) {
                    const bool start_streaming = !camera_control->subscribe;
                    if (start_streaming) {
                        const RecordingPreflightResult preflight =
                            run_gui_recording_preflight(cameras_params, cameras_select, num_cameras);
                        if (!preflight.ok) {
                            recording_preflight_errors = preflight.errors;
                            log_recording_preflight_failure("gui_start_streaming", preflight);
                        } else {
                            recording_preflight_errors.clear();
                            camera_control->subscribe = true;
                        // START STREAMING
                            std::cout << "STARTING STREAMING SESSION..." << std::endl;

                            if (std::any_of(cameras_select, cameras_select + num_cameras, [](const CameraEachSelect& cs){ return cs.record || cs.crop_and_encode; })) {
                                // Store the base folder for recordings, not the final timestamped one.
                                encoder_config->folder_name = input_folder;
                            }

                            // This part remains the same
                            camera_resources.resize(num_cameras);
                            frame_ipc_managers.clear();
                            frame_ipc_managers.resize(num_cameras);
                            frame_ipc_init_errors.clear();
                            frame_ipc_init_errors.resize(num_cameras);
                            size_t max_frame_size_bytes = 0;
                            for (int i = 0; i < num_cameras; ++i) {
                                size_t current_size = (size_t)cameras_params[i].width * (size_t)cameras_params[i].height;
                                if (current_size > max_frame_size_bytes) {
                                    max_frame_size_bytes = current_size;
                                }
                            }
                            for (int i = 0; i < num_cameras; ++i) {
                                std::cout << "Initializing resources for camera " << i << " on GPU " << cameras_params[i].gpu_id << std::endl;
                                camera_resources[i].initialize(
                                    cameras_params[i].gpu_id,
                                    max_frame_size_bytes,
                                    cameras_select[i].yolo,
                                    cameras_params[i].recording.resources.acquire_work_entries);
                                if (cameras_select[i].send_frame_ipc) {
                                    frame_ipc_managers[i] = std::make_unique<FrameIPCManager>(&cameras_params[i]);
                                    if (!frame_ipc_managers[i]->isEnabled()) {
                                        frame_ipc_init_errors[i] = frame_ipc_managers[i]->getInitError();
                                        frame_ipc_managers[i].reset();
                                    }
                                }
                            }
                            // Create worker thread objects and GPU textures
                            openGLDisplayWorkers = new COpenGLDisplay*[num_cameras]();
                            cropAndEncodeWorkers = new CropAndEncodeWorker*[num_cameras]();
                            tex = new GL_Texture[num_cameras];
                            crop_tex = new GL_Texture[num_cameras];
                            yolo_workers.assign(num_cameras, nullptr);
                            recording_pipelines.clear();
                            recording_pipelines.resize(num_cameras);

                            // Initialize all worker pointers to nullptr
                            for(int i = 0; i < num_cameras; ++i) {
                                openGLDisplayWorkers[i] = nullptr;
                                cropAndEncodeWorkers[i] = nullptr;
                            }
                            cudaSetDevice(display_gpu_id);

                            // Allocate main textures for each camera's OpenGL display
                            for (int i = 0; i < num_cameras; i++) {
                                if (cameras_select[i].stream_on) {
                                    int w = (int)(cameras_params[i].width / cameras_select[i].downsample);
                                    int h = (int)(cameras_params[i].height / cameras_select[i].downsample);
                                    setup_texture(tex[i], w, h);
                                }
                            }

                            // Setup cropped textures for each crop/encode worker
                            for (int i = 0; i < num_cameras; i++) {
                                if (cameras_select[i].crop_and_encode) {
                                    // The crop view has a fixed size of 256x256
                                    setup_texture(crop_tex[i], 256, 256);
                                }
                            }

                            // CREATE AND LINK ALL WORKER THREADS
                            for (int i = 0; i < num_cameras; i++) {
                                if (cameras_select[i].stream_on) {
                                    std::string name = "OpenGLDisplay_Cam_" + cameras_params[i].camera_serial;
                                    openGLDisplayWorkers[i] = new COpenGLDisplay(name.c_str(), &cameras_params[i], &cameras_select[i], tex[i].cuda_buffer, &indigo_signal_builder, *camera_resources[i].recycle_queue);
                                }
                                if (cameras_select[i].yolo) {
                                    std::string name = "YOLO_Worker_Cam_" + cameras_params[i].camera_serial;
                                    cameras_select[i].yolo_model = yolo_model.c_str();
                                    yolo_workers[i] = new YOLOv8Worker(
                                        name.c_str(),
                                        &cameras_params[i],
                                        &cameras_select[i],
                                        camera_control,
                                        *camera_resources[i].recycle_queue
                                    );
                                    if (openGLDisplayWorkers[i]) {
                                        yolo_workers[i]->SetDisplayWorker(openGLDisplayWorkers[i]);
                                    }
                                }
                                if (cameras_select[i].record) {
                                    std::string recording_output_warning;
                                    const RecordingOutputConfig recording_output_config =
                                        orange::gui::resolve_recording_output_config(
                                            cameras_params[i],
                                            *encoder_config,
                                            cameras_select[i],
                                            &recording_output_warning);
                                    if (!recording_output_warning.empty()) {
                                        std::cerr << "[record_output] Cam " << cameras_params[i].camera_serial
                                                  << ": " << recording_output_warning
                                                  << ". Falling back to native "
                                                  << cameras_params[i].width << "x" << cameras_params[i].height
                                                  << "." << std::endl;
                                    }
                                    ResolvedRecordingConfigOverrides recording_overrides;
                                    recording_overrides.recording_gpu_id = cameras_params[i].gpu_id;
                                    recording_overrides.has_output_preferences_override = true;
                                    recording_overrides.output_preferences.mode =
                                        recording_output_config.mode == "resolution" ? "resolution"
                                        : (recording_output_config.mode == "exact_size" ? "exact_size" : "factor");
                                    recording_overrides.output_preferences.downsample_factor =
                                        recording_output_config.downsample_factor;
                                    recording_overrides.output_preferences.requested_width =
                                        recording_output_config.requested_width;
                                    recording_overrides.output_preferences.requested_height =
                                        recording_output_config.requested_height;
                                    recording_overrides.codec = encoder_config->encoder_codec;
                                    recording_overrides.preset = encoder_config->encoder_preset;
                                    recording_overrides.tuning = encoder_config->tuning_info;
                                    recording_overrides.rate_control_mode = encoder_config->rate_control_mode;
                                    recording_overrides.quality_value = encoder_config->quality_value;
                                    recording_overrides.gop_length = encoder_config->gop_length;
                                    recording_overrides.base_folder_name = encoder_config->folder_name;
                                    const ResolvedRecordingConfig resolved_recording_config =
                                        build_resolved_recording_config(
                                            cameras_params[i],
                                            recording_overrides);
                                    recording_pipelines[i] = std::make_unique<ModernRecordingPipeline>(
                                        &cameras_params[i],
                                        resolved_recording_config,
                                        *camera_resources[i].recycle_queue,
                                        camera_control);
                                }

                                if (cameras_select[i].crop_and_encode) {
                                    std::string name = "CropEncode_Cam_" + cameras_params[i].camera_serial;
                                    cropAndEncodeWorkers[i] = new CropAndEncodeWorker(
                                        name.c_str(),
                                        &cameras_params[i],
                                        encoder_config->folder_name,
                                        *camera_resources[i].recycle_queue,
                                        crop_tex[i].cuda_buffer,
                                        camera_control
                                    );
                                    // Immediately link it to the YOLO worker if it exists
                                    if (yolo_workers[i]) {
                                        yolo_workers[i]->SetCropAndEncodeWorker(cropAndEncodeWorkers[i]);
                                    }
                                }
                            }

                            // START ALL WORKER THREADS
                            for (int i = 0; i < num_cameras; i++) {
                                if (openGLDisplayWorkers[i]) {
                                    openGLDisplayWorkers[i]->SetMaxQueueSize(240); 
                                    openGLDisplayWorkers[i]->StartThread();
                                }
                                if (yolo_workers[i]) {
                                    yolo_workers[i]->SetMaxQueueSize(240);
                                    yolo_workers[i]->StartThread();
                                }
                                if (recording_pipelines[i]) {
                                    recording_pipelines[i]->start();
                                }
                                if (cropAndEncodeWorkers[i]) {
                                    cropAndEncodeWorkers[i]->SetMaxQueueSize(240);
                                    cropAndEncodeWorkers[i]->StartThread();
                                }
                            }

                            // PREPARE CAMERAS
                            for (int i = 0; i < num_cameras; i++) {
                                camera_open_stream(&ecams[i].camera, &cameras_params[i], "gui_start_streaming");
                                ecams[i].evt_frame = new Emergent::CEmergentFrame[evt_buffer_size];
                                allocate_frame_buffer(&ecams[i].camera, ecams[i].evt_frame, &cameras_params[i], evt_buffer_size);
                            }
                            if (ptp_stream_sync) {
                                for (int i = 0; i < num_cameras; i++) {
                                    ptp_camera_sync(&ecams[i].camera, &cameras_params[i]);
                                }
                                camera_control->sync_camera = true;
                            }

                            // Start acquisition threads
                            for (int i = 0; i < num_cameras; i++) {
                                camera_threads.emplace_back(
                                    &acquire_frames,
                                    &ecams[i],
                                    &cameras_params[i],
                                    &cameras_select[i],
                                    camera_control,
                                    ptp_params,
                                    &indigo_signal_builder,
                                    openGLDisplayWorkers[i],
                                    recording_pipelines[i] ? recording_pipelines[i]->recording_ingress() : nullptr,
                                    yolo_workers[i],
                                    image_writer,
                                    &camera_resources[i],
                                    frame_ipc_managers[i].get()
                                );
                            }
                        }
                    } else {
                        camera_control->subscribe = false;
                        // STOP STREAMING
                        std::cout << "STOPPING STREAMING SESSION..." << std::endl;

                        // 1. Stop the acquisition threads first.
                        // This prevents new frames from entering the pipeline.
                        for (auto &t : camera_threads) {
                            if (t.joinable()) t.join();
                        }
                        camera_threads.clear();
                        std::cout << "Acquisition threads joined." << std::endl;

                        // RESET PTP STATE
                        if (ptp_stream_sync) {
                            ptp_params->ptp_global_time = 0;
                            ptp_params->ptp_stop_time = 0;
                            ptp_params->ptp_counter = 0;
                            ptp_params->ptp_stop_counter = 0;
                            ptp_params->network_sync = false;
                            ptp_params->network_set_start_ptp = false;
                            ptp_params->ptp_stop_reached = false;
                            ptp_params->ptp_start_reached = false;
                            camera_control->sync_camera = false; // Also reset this flag
                            std::cout << "PTPParams state has been reset for the next run." << std::endl;
                        }

                        // 2. Signal all worker threads to stop processing NEW data from their queues.
                        // They will finish processing whatever is currently in their queue.
                        for (int i = 0; i < num_cameras; i++) {
                            if (yolo_workers[i]) yolo_workers[i]->StopThread();
                            if (openGLDisplayWorkers[i]) openGLDisplayWorkers[i]->StopThread();
                            if (cropAndEncodeWorkers[i]) cropAndEncodeWorkers[i]->StopThread();
                            if (recording_pipelines[i]) recording_pipelines[i]->request_stop();
                        }
                        std::cout << "All worker threads signaled to stop. Waiting for queues to drain..." << std::endl;

                        // 3. Join and delete workers in REVERSE pipeline order to ensure the pipeline is drained.
                        for (int i = 0; i < num_cameras; i++) {
                            // Endpoints are first.
                            if (openGLDisplayWorkers[i]) {
                                delete openGLDisplayWorkers[i];
                                openGLDisplayWorkers[i] = nullptr;
                            }

                            if (cropAndEncodeWorkers[i]) {
                                std::cout << "Flushing final packets for crop encoder " << cameras_params[i].camera_serial << "..." << std::endl;
                                cropAndEncodeWorkers[i]->flush_and_close();
                                delete cropAndEncodeWorkers[i];
                                cropAndEncodeWorkers[i] = nullptr;
                            }
                            
                            // Now the hardware encoder, which is fed by the preprocessor.
                            // The YOLO worker can be deleted now.
                            if (yolo_workers[i]) {
                                delete yolo_workers[i];
                                yolo_workers[i] = nullptr;
                            }

                            if (recording_pipelines[i]) {
                                recording_pipelines[i]->shutdown();
                                recording_pipelines[i].reset();
                            }
                        }
                        
                        // Clear the worker pointer vectors
                        yolo_workers.clear();
                        recording_pipelines.clear();
                        if(openGLDisplayWorkers) { delete[] openGLDisplayWorkers; openGLDisplayWorkers = nullptr; }
                        if(cropAndEncodeWorkers) { delete[] cropAndEncodeWorkers; cropAndEncodeWorkers = nullptr; }
                        std::cout << "Worker threads all cleaned up." << std::endl;
                        frame_ipc_managers.clear();
                        frame_ipc_init_errors.clear();

                        // 4. Final resource cleanup
                        for (int i = 0; i < num_cameras; i++) {
                            destroy_frame_buffer(&ecams[i].camera, ecams[i].evt_frame, evt_buffer_size, &cameras_params[i]);
                            delete[] ecams[i].evt_frame;
                            ecams[i].evt_frame = nullptr;
                            check_camera_errors(EVT_CameraCloseStream(&ecams[i].camera), cameras_params[i].camera_serial.c_str());
                        }

                        for (int i = 0; i < num_cameras; i++) {
                            if (cameras_select[i].stream_on) {
                                int w = int(cameras_params[i].width / cameras_select[i].downsample);
                                int h = int(cameras_params[i].height / cameras_select[i].downsample);
                                clear_upload_and_cleanup(tex[i], w, h);
                            }
                            // Add this block to clean up the crop textures
                            if (cameras_select[i].crop_and_encode) {
                                clear_upload_and_cleanup(crop_tex[i], 256, 256);
                            }
                        }

                        if(tex) delete[] tex;
                        tex = nullptr;
                        if(crop_tex) delete[] crop_tex;
                        crop_tex = nullptr;

                        for(int i = 0; i < num_cameras; ++i) {
                            camera_resources[i].cleanup();
                        }
                        camera_resources.clear();
                        std::cout << "Cleaned up all per-camera resources." << std::endl;
                    }
                }
                if (calibration_tool_busy) {
                    ImGui::EndDisabled();
                }
            }

            if (camera_control->stop_record) {
                ImGui::PushStyleColor(ImGuiCol_Button, ImVec4{0.5f, 0, 0, 1.0f});
            } else {
                ImGui::PushStyleColor(ImGuiCol_Button, ImVec4{0, 0.5f, 0, 1.0f});
            }

            if (camera_control->open) {

                if (!camera_control->subscribe) {
                    // ImGui::BeginDisabled();
                }

                if (calibration_tool_busy) {
                    ImGui::BeginDisabled();
                }
                if (ImGui::Button(camera_control->record_video ? ICON_FK_PAUSE : ICON_FK_PLAY)) {
                    if (!camera_control->record_video && camera_control->recording_draining) {
                        std::cout << "Recording is still draining. Please wait..." << std::endl;
                    } else {
                        bool next_record_state = !camera_control->record_video;
                        std::string resolved_recording_folder;
                        bool allow_transition = true;

                        if (next_record_state) {
                            const RecordingPreflightResult preflight =
                                run_gui_recording_preflight(cameras_params, cameras_select, num_cameras);
                            if (!preflight.ok) {
                                recording_preflight_errors = preflight.errors;
                                log_recording_preflight_failure("gui_start_recording", preflight);
                                allow_transition = false;
                            } else {
                                recording_preflight_errors.clear();
                                camera_control->recording_draining = false;
                                camera_control->stop_record = false;
                                std::string recording_id = get_current_date_time();
                                std::string recording_folder;
                                std::string base_folder = encoder_config->folder_name.empty() ? input_folder : encoder_config->folder_name;
                                {
                                    std::lock_guard<std::mutex> lock(camera_control->recording_folder_mutex);
                                    if (camera_control->recording_folder.empty()) {
                                        camera_control->recording_folder = base_folder + "/" + recording_id;
                                    } else {
                                        recording_id = std::filesystem::path(camera_control->recording_folder).filename().string();
                                    }
                                    recording_folder = camera_control->recording_folder;
                                }
                                if (base_folder.empty() && !recording_folder.empty()) {
                                    std::filesystem::path parent = std::filesystem::path(recording_folder).parent_path();
                                    if (parent.empty() || parent == "/") {
                                        base_folder = recording_folder;
                                    } else {
                                        base_folder = parent.string();
                                    }
                                }
                                resolved_recording_folder = recording_folder;
                                make_folder(recording_folder);
                                write_recording_snapshot(
                                    recording_folder,
                                    recording_id,
                                    cameras_params,
                                    num_cameras,
                                    base_folder,
                                    camera_control->sync_camera,
                                    ptp_params);
                                initialize_ptp_sync_summary(
                                    recording_folder,
                                    recording_id,
                                    num_cameras,
                                    camera_control->sync_camera,
                                    ptp_params);
                            }
                        }

                        if (allow_transition) {
                            camera_control->record_video = next_record_state;
                            if (!camera_control->record_video) {
                                camera_control->recording_draining = true;
                                camera_control->stop_record = true;
                                if (camera_control->active_recorders.load(std::memory_order_relaxed) == 0) {
                                    camera_control->recording_draining = false;
                                    camera_control->stop_record = false;
                                }
                            }

                            if (camera_control->record_video) {
                                // START RECORDING
                                try_start_timer();
                                std::cout << "Recording toggled ON." << std::endl;
                                if (!resolved_recording_folder.empty()) {
                                    std::cout << "Recording folder: " << resolved_recording_folder << std::endl;
                                }
                            } else {
                                // STOP RECORDING
                                try_stop_timer();
                                std::cout << "Recording toggled OFF. Encoders will drain queued frames." << std::endl;
                            }
                        }
                    }
                }
                if (calibration_tool_busy) {
                    ImGui::EndDisabled();
                }
                
                if (!camera_control->subscribe) {
                    // ImGui::EndDisabled(); // This can be uncommented if you want to disable the button
                }
            }

            std::string active_recording_folder;
            {
                std::lock_guard<std::mutex> lock(camera_control->recording_folder_mutex);
                active_recording_folder = camera_control->recording_folder;
            }
            if (!active_recording_folder.empty()) {
                ImGui::SameLine();
                if (ImGui::Button("Copy recording path")) {
                    ImGui::SetClipboardText(active_recording_folder.c_str());
                }
                ImGui::TextWrapped("Recording path: %s", active_recording_folder.c_str());
            }

            ImGui::PopStyleColor(1);
        }
        ImGui::End();


        if (camera_control->subscribe) {
            // Upload the texture data from the PBOs to the GPU textures
            for (int i = 0; i < num_cameras; i++) {
                if (cameras_select[i].stream_on) {
                    int camera_width = int(cameras_params[i].width / cameras_select[i].downsample);
                    int camera_height = int(cameras_params[i].height / cameras_select[i].downsample);
                    upload_texture_from_pbo(tex[i], camera_width, camera_height);
                }
                if (cameras_select[i].crop_and_encode) {
                    upload_texture_from_pbo(crop_tex[i], 256, 256);
                }
            }
            // Draw main camera views
            if (camera_control->record_video) {
                int64_t start_ns = record_start_time_ns.load();
                std::string g_formatted_elapsed_time;
                if (start_ns > 0) {
                    int64_t now_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                        std::chrono::steady_clock::now().time_since_epoch()).count();
                
                    auto elapsed_sec = std::chrono::seconds((now_ns - start_ns) / 1'000'000'000);
                    g_formatted_elapsed_time = format_elapsed_time(elapsed_sec);
                }
                // Resize speed tracking data
                if (speed_tracking_data.size() != num_cameras) {
                        speed_tracking_data.resize(num_cameras);
                }

                for (int i = 0; i < num_cameras; i++) {
                    
                    if (cameras_select[i].stream_on) {
                        std::string window_name = cameras_params[i].camera_name;
                        ImGui::Begin(window_name.c_str());
                        
                        if (start_ns > 0) {
                            ImGui::TextColored(ImVec4{0.0, 1.0f, 0, 1.0f}, "Elapsed Time: %s", g_formatted_elapsed_time.c_str());
                        } else {
                            ImGui::TextColored(ImVec4{1.0, 1.0f, 0, 1.0f}, "Recording starting...");
                        }ImGui::SameLine();
                        if (yolo_workers[i])
                        {
                            ImGui::SameLine();
                            ImGui::TextColored(ImVec4(1.0f, 0.55f, 0.0f, 1.0f), "YOLO FPS: %.1f", yolo_workers[i]->get_fps());
                        }
                        ImGui::SameLine();
                        ImGui::Text("Streaming FPS: %.1f", streaming_fps.load());    
                        
                        if (cameras_select[i].yolo && i < yolo_workers.size() && yolo_workers[i]) {
                            ImGui::SameLine();
                            ImGui::TextColored(ImVec4(1.0f, 0.55f, 0.0f, 1.0f), "YOLO FPS: %.1f", yolo_workers[i]->get_fps());
                        }
            
                        ImVec2 avail_size = ImGui::GetContentRegionAvail();
                        avail_size.y *= 0.5f;
    
                        static ImVec2 bmin(0, 0);
                        static ImVec2 uv0(0, 0);
                        static ImVec2 uv1(1, 1);
                        static ImVec4 tint(1, 1, 1, 1);
    
                        // ImGui::Image((void*)(intptr_t)texture[i], avail_size);
                        ImPlotAxisFlags axisFlags = ImPlotAxisFlags_NoTickLabels | ImPlotAxisFlags_NoTickMarks |
                                                    ImPlotAxisFlags_NoGridLines;
                        if (ImPlot::BeginPlot("##no_plot_name", avail_size, ImPlotFlags_Equal | ImPlotAxisFlags_AutoFit)) {
                            
                            // Calculate the correct display dimensions based on the downsample factor.
                            const float display_width = static_cast<float>(cameras_params[i].width / cameras_select[i].downsample);
                            const float display_height = static_cast<float>(cameras_params[i].height / cameras_select[i].downsample);

                            // Set the plot axes to match the dimensions of the image being displayed.
                            ImPlot::SetupAxesLimits(0, display_width, 0, display_height);
                            ImPlot::SetupAxis(ImAxis_X1, nullptr, axisFlags); 
                            ImPlot::SetupAxis(ImAxis_Y1, nullptr, axisFlags);

                            // Tell ImPlot to draw the image using these correct dimensions.
                            ImPlot::PlotImage("##no_image_name", (void *) (intptr_t) tex[i].texture, ImVec2(0, 0),
                                              ImVec2(display_width, display_height));
                                              

                            ImPlot::EndPlot();
                        }

                        if (cameras_select[i].yolo && i < yolo_workers.size() && yolo_workers[i]) {
                            RenderSpeedGraph(i, yolo_workers[i], speed_tracking_data[i]);
                        }
                        ImGui::End();
                    }
                }
            } else {
                for (int i = 0; i < num_cameras; i++) {
                    if (cameras_select[i].stream_on) {
                        std::string window_name = cameras_params[i].camera_name;
                        ImGui::Begin(window_name.c_str());
                        ImGui::TextColored(ImVec4{1.0, 0.0f, 0, 1.0f}, "NOT RECORDING, ");
                        ImGui::SameLine();
                        ImGui::Text("Streaming FPS: %.1f", streaming_fps.load());    
                        ImVec2 avail_size = ImGui::GetContentRegionAvail();

                        if (cameras_select[i].yolo && i < yolo_workers.size() && yolo_workers[i]) {
                            ImGui::SameLine();
                            ImGui::TextColored(ImVec4(1.0f, 0.55f, 0.0f, 1.0f), "YOLO FPS: %.1f", yolo_workers[i]->get_fps());
                        }
    
                        static ImVec2 bmin(0, 0);
                        static ImVec2 uv0(0, 0);
                        static ImVec2 uv1(1, 1);
                        static ImVec4 tint(1, 1, 1, 1);
    
                        // ImGui::Image((void*)(intptr_t)texture[i], avail_size);
                        ImPlotAxisFlags axisFlags = ImPlotAxisFlags_NoTickLabels | ImPlotAxisFlags_NoTickMarks |
                                                    ImPlotAxisFlags_NoGridLines;
                        if (ImPlot::BeginPlot("##no_plot_name", avail_size, ImPlotFlags_Equal | ImPlotAxisFlags_AutoFit)) {
                            ImPlot::SetupAxesLimits(0, cameras_params[i].width, 0, cameras_params[i].height);
                            ImPlot::SetupAxis(ImAxis_X1, nullptr, axisFlags); // X-axis
                            ImPlot::SetupAxis(ImAxis_Y1, nullptr, axisFlags); // Y-axis
                            ImPlot::PlotImage("##no_image_name", (void *) (intptr_t) tex[i].texture, ImVec2(0, 0),
                                                ImVec2(cameras_params[i].width, cameras_params[i].height));
                        
                            ImPlot::EndPlot();
                            
                        }
                        ImGui::End();
                    }
                }
            }
            for (int i = 0; i < num_cameras; i++) {
                    // Check if the crop and encode feature is enabled for this camera
                    if (cameras_select[i].crop_and_encode) {
                        // Create a unique name for the new window
                        std::string window_name = cameras_params[i].camera_name + " Crop";
                        ImGui::Begin(window_name.c_str());

                        // Use ImPlot to display the texture, just like the main view
                        if (ImPlot::BeginPlot("##crop_plot", ImGui::GetContentRegionAvail(), ImPlotFlags_Equal | ImPlotAxisFlags_AutoFit)) {
                            ImPlot::PlotImage("##crop_image", (void*)(intptr_t)crop_tex[i].texture, ImVec2(0, 0), ImVec2(256, 256));
                            ImPlot::EndPlot();
                        }
                        ImGui::End();
                    }
                }
        }

        if (camera_control->open && show_realtime_plot) {
            ImGui::Begin("Realtime Plots"); {
                static float t = 0;
                t += ImGui::GetIO().DeltaTime;
                for (int i = 0; i < num_cameras; i++) {
                    get_senstemp_value(&ecams[i].camera, &cameras_params[i]);
                    realtime_plot_data[i].AddPoint(t, cameras_params[i].sens_temp);
                }

                static float history = 10.0f;
                ImGui::SliderFloat("History", &history, 1, 30, "%.1f s");

                static ImPlotAxisFlags flags = ImPlotAxisFlags_NoTickMarks;
                ImVec2 avail_size = ImGui::GetContentRegionAvail();

                if (ImPlot::BeginPlot("Camera Sensor Temperature", avail_size)) {
                    ImPlot::SetupAxes(nullptr, nullptr, flags, flags);
                    ImPlot::SetupAxisLimits(ImAxis_X1, t - history, t, ImGuiCond_Always);
                    ImPlot::SetupAxisLimits(ImAxis_Y1, 30, 90);
                    ImPlot::SetNextFillStyle(IMPLOT_AUTO_COL, 0.5f);

                    for (int i = 0; i < num_cameras; i++) {
                        std::string line_name = std::string(cameras_params[i].camera_serial);
                        ImPlot::PlotLine(line_name.c_str(), &realtime_plot_data[i].Data[0].x,
                                         &realtime_plot_data[i].Data[0].y, realtime_plot_data[i].Data.size(), 0,
                                         realtime_plot_data[i].Offset, 2 * sizeof(float));
                    }
                    ImPlot::EndPlot();
                }
                ImGui::End();
            }
        }

        render_a_frame(window);
    }

    if (aperture_ui_state.worker.joinable()) {
        aperture_ui_state.worker.join();
    }
    stop_fov_alignment_worker(&aperture_ui_state);
    clear_aperture_preview_texture(&aperture_ui_state);
    clear_alignment_preview_texture(&aperture_ui_state);

    if (usaf_ui_state.worker.joinable()) {
        usaf_ui_state.worker.join();
    }
    stop_usaf_preview_worker(&usaf_ui_state);
    clear_usaf_preview_texture(&usaf_ui_state);
    clear_usaf_captured_texture(&usaf_ui_state);
    clear_spatial_layout_texture(&spatial_layout_ui_state);

    if (camera_control->open) {
        for (int i = 0; i < num_cameras; i++) {
            close_camera(&ecams[i].camera, &cameras_params[i]);
        }
        delete[] cameras_params;
        cameras_params = nullptr;
        delete[] cameras_select;
        cameras_select = nullptr;
        delete[] ecams;
        ecams = nullptr;
        delete[] realtime_plot_data;
        realtime_plot_data = nullptr;
    }

    std::cout << "GUI closed, initiating cleanup..." << std::endl;

    orange::gui::reap_host_ptp_stack_worker(&host_ptp_stack_ui);
    if (host_ptp_stack_ui.worker.joinable()) {
        std::cout << "Waiting for PTP stack command to finish..." << std::endl;
        host_ptp_stack_ui.worker.join();
    }

    // 1. Signal the ENet thread to stop
    quite_enet = true;

    // 2. Join the ENet thread before exiting
    if (enet_thread.joinable()) {
        std::cout << "Waiting for ENet thread to finish..." << std::endl;
        enet_thread.join();
        std::cout << "ENet thread joined successfully." << std::endl;
    }

    if (enet_server_initialized) {
        enet_release(&server);
    }
    if (enet_runtime_initialized) {
        enet_deinitialize();
    }

    // 3. Cleanup any remaining resources
    gx_cleanup(window);

    // 4. Free allocated memory
    free(window->glsl_version);
    free(window);

    std::cout << "Cleanup completed, exiting..." << std::endl;
}
