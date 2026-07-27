#include "usaf_resolution_ui.h"

#include "camera_preview_utils.h"
#include "fsuid_guard.h"
#include "imgui.h"
#include "implot.h"
#include "project.h"

#include <opencv2/opencv.hpp>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <limits>
#include <sstream>
#include <utility>

namespace {

struct UsafPreviewAnchor {
    const char* label;
    const char* button_label;
    double norm_x;
    double norm_y;
};

constexpr UsafPreviewAnchor kUsafPreviewAnchors[] = {
    {"center", "C", 0.5, 0.5},
    {"north", "N", 0.5, 0.12},
    {"south", "S", 0.5, 0.88},
    {"east", "E", 0.88, 0.5},
    {"west", "W", 0.12, 0.5},
    {"north_east", "NE", 0.88, 0.12},
    {"north_west", "NW", 0.12, 0.12},
    {"south_east", "SE", 0.88, 0.88},
    {"south_west", "SW", 0.12, 0.88}
};

constexpr int kUsafPreviewBufferCount = 2;
constexpr const char* kUsafPreviewTransactionOwner = "usaf_resolution_preview";
constexpr const char* kUsafArtifactTransactionOwner = "usaf_resolution_artifact";

bool acquire_usaf_transaction(
    UsafResolutionUiState* ui_state,
    const char* owner_kind,
    const CameraParams& camera_params,
    const orange::calibration::MutationSet allowed_mutations,
    const std::string& reason,
    std::string* error_out)
{
    if (ui_state == nullptr || owner_kind == nullptr) {
        if (error_out) *error_out = "USAF calibration transaction state is unavailable.";
        return false;
    }
    if (ui_state->transaction_lease && ui_state->transaction_lease->active()) {
        if (error_out) *error_out = "USAF calibration already owns an active transaction.";
        return false;
    }
    ui_state->transaction_lease.reset();
    orange::calibration::TransactionRequest request;
    request.owner_id = std::string(owner_kind) + "_Cam" +
        camera_params.camera_serial + "_" + get_current_utc_timestamp();
    request.workflow = orange::calibration::WorkflowKind::kUsafResolution;
    request.camera_serials = {camera_params.camera_serial};
    request.allowed_owner_mutations = allowed_mutations;
    request.reason = reason;
    auto acquired =
        orange::calibration::global_transaction_coordinator().TryAcquire(
            std::move(request));
    if (!acquired.ok()) {
        if (error_out) *error_out = acquired.error;
        return false;
    }
    ui_state->transaction_lease = std::move(acquired.lease);
    ui_state->transaction_owner_kind = owner_kind;
    if (error_out) error_out->clear();
    return true;
}

void release_usaf_transaction_if_owned(
    UsafResolutionUiState* ui_state,
    const char* owner_kind,
    const std::string& terminal_status,
    const std::string& terminal_reason)
{
    if (ui_state == nullptr || owner_kind == nullptr ||
        ui_state->transaction_owner_kind != owner_kind ||
        !ui_state->transaction_lease) {
        return;
    }
    ui_state->transaction_lease->Release(terminal_status, terminal_reason);
    ui_state->transaction_lease.reset();
    ui_state->transaction_owner_kind.clear();
}

template <size_t N>
void copy_string_to_buffer(char (&buffer)[N], const std::string& value)
{
    std::snprintf(buffer, N, "%s", value.c_str());
}

bool upload_live_preview_texture(UsafResolutionUiState* ui_state)
{
    std::lock_guard<std::mutex> lock(ui_state->preview_mutex);
    const LiveUsafPreviewState& preview = ui_state->live_preview;
    if (!preview.available || preview.rgba.empty() || preview.frame_serial == ui_state->preview_uploaded_serial) {
        return preview.available;
    }

    orange::preview::update_rgba_texture(
        &ui_state->preview_texture,
        &ui_state->preview_texture_width,
        &ui_state->preview_texture_height,
        preview.rgba,
        preview.width,
        preview.height);
    ui_state->preview_uploaded_serial = preview.frame_serial;
    return true;
}

bool ensure_captured_texture(UsafResolutionUiState* ui_state, int position_index)
{
    if (position_index < 0 || position_index >= static_cast<int>(ui_state->positions.size())) {
        clear_usaf_captured_texture(ui_state);
        return false;
    }
    const UsafCapturedPosition& position = ui_state->positions[static_cast<size_t>(position_index)];
    if (ui_state->captured_texture != 0 && ui_state->captured_texture_position_index == position_index &&
        ui_state->captured_texture_width == position.width && ui_state->captured_texture_height == position.height) {
        return true;
    }

    clear_usaf_captured_texture(ui_state);
    if (position.width <= 0 || position.height <= 0 || position.rgb.empty()) {
        return false;
    }

    std::vector<unsigned char> rgba;
    if (!orange::preview::rgb_to_rgba(position.rgb, position.width, position.height, &rgba)) {
        return false;
    }
    orange::preview::update_rgba_texture(
        &ui_state->captured_texture,
        &ui_state->captured_texture_width,
        &ui_state->captured_texture_height,
        rgba,
        position.width,
        position.height);
    ui_state->captured_texture_width = position.width;
    ui_state->captured_texture_height = position.height;
    ui_state->captured_texture_position_index = position_index;
    return true;
}

UsafRoi normalize_roi(double x0, double y0, double x1, double y1, int image_width, int image_height)
{
    UsafRoi roi;
    const double min_x = std::clamp(std::min(x0, x1), 0.0, std::max(0.0, static_cast<double>(image_width)));
    const double max_x = std::clamp(std::max(x0, x1), 0.0, std::max(0.0, static_cast<double>(image_width)));
    const double min_y = std::clamp(std::min(y0, y1), 0.0, std::max(0.0, static_cast<double>(image_height)));
    const double max_y = std::clamp(std::max(y0, y1), 0.0, std::max(0.0, static_cast<double>(image_height)));
    roi.x = static_cast<int>(std::floor(min_x));
    roi.y = static_cast<int>(std::floor(min_y));
    roi.width = std::max(0, static_cast<int>(std::ceil(max_x)) - roi.x);
    roi.height = std::max(0, static_cast<int>(std::ceil(max_y)) - roi.y);
    roi.has_roi = roi.width > 0 && roi.height > 0;
    return roi;
}

bool capture_current_usaf_snapshot(UsafResolutionUiState* ui_state, const std::string& label, std::string* error_out)
{
    std::lock_guard<std::mutex> lock(ui_state->preview_mutex);
    if (!ui_state->live_preview.available || ui_state->live_preview.capture_width <= 0 || ui_state->live_preview.capture_height <= 0 ||
        ui_state->live_preview.raw_rgb.empty()) {
        if (error_out) {
            *error_out = "No live target preview frame is available to capture.";
        }
        return false;
    }

    auto it = std::find_if(ui_state->positions.begin(), ui_state->positions.end(),
                           [&](const UsafCapturedPosition& position) { return position.label == label; });
    if (it == ui_state->positions.end()) {
        ui_state->positions.push_back({});
        it = std::prev(ui_state->positions.end());
        it->label = label;
    }
    it->width = ui_state->live_preview.capture_width;
    it->height = ui_state->live_preview.capture_height;
    it->rgb = ui_state->live_preview.raw_rgb;
    return true;
}

bool is_usaf_position_captured(const UsafResolutionUiState* ui_state, const char* label)
{
    return std::any_of(ui_state->positions.begin(), ui_state->positions.end(),
                       [&](const UsafCapturedPosition& position) { return position.label == label; });
}

void select_usaf_capture_label(UsafResolutionUiState* ui_state, int anchor_index)
{
    ui_state->capture_label_index = std::clamp(anchor_index, 0, static_cast<int>(IM_ARRAYSIZE(kUsafPreviewAnchors)) - 1);
    const char* selected_label = kUsafPreviewAnchors[ui_state->capture_label_index].label;
    auto it = std::find_if(ui_state->positions.begin(), ui_state->positions.end(),
                           [&](const UsafCapturedPosition& position) { return position.label == selected_label; });
    if (it != ui_state->positions.end()) {
        ui_state->selected_position_index = static_cast<int>(std::distance(ui_state->positions.begin(), it));
    }
}

void render_usaf_position_selector(UsafResolutionUiState* ui_state)
{
    const int rows[][3] = {
        {6, 1, 5},
        {4, 0, 3},
        {8, 2, 7},
    };

    ImGui::TextUnformatted("Target position");
    const float spacing = ImGui::GetStyle().ItemSpacing.x;
    const float button_width = std::max(52.0f, (ImGui::GetContentRegionAvail().x - 2.0f * spacing) / 3.0f);
    for (const auto& row : rows) {
        for (int column = 0; column < 3; ++column) {
            const int anchor_index = row[column];
            const UsafPreviewAnchor& anchor = kUsafPreviewAnchors[anchor_index];
            const bool active = (ui_state->capture_label_index == anchor_index);
            const bool captured = is_usaf_position_captured(ui_state, anchor.label);
            const ImVec4 button_color = active ? ImVec4(0.82f, 0.62f, 0.16f, 1.0f)
                                               : (captured ? ImVec4(0.18f, 0.50f, 0.26f, 1.0f)
                                                           : ImVec4(0.22f, 0.26f, 0.34f, 1.0f));
            const ImVec4 hovered_color = active ? ImVec4(0.90f, 0.72f, 0.22f, 1.0f)
                                                : (captured ? ImVec4(0.24f, 0.58f, 0.32f, 1.0f)
                                                            : ImVec4(0.28f, 0.32f, 0.40f, 1.0f));
            const ImVec4 active_color = active ? ImVec4(0.72f, 0.52f, 0.12f, 1.0f)
                                               : (captured ? ImVec4(0.14f, 0.40f, 0.20f, 1.0f)
                                                           : ImVec4(0.18f, 0.22f, 0.30f, 1.0f));
            ImGui::PushID(anchor.label);
            ImGui::PushStyleColor(ImGuiCol_Button, button_color);
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, hovered_color);
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, active_color);
            if (ImGui::Button(anchor.button_label, ImVec2(button_width, 0.0f))) {
                select_usaf_capture_label(ui_state, anchor_index);
            }
            ImGui::PopStyleColor(3);
            ImGui::PopID();
            if (column < 2) {
                ImGui::SameLine();
            }
        }
    }
    ImGui::TextDisabled("Active target is gold. Captured positions are green.");
}

void render_usaf_position_guides(const UsafResolutionUiState* ui_state, int image_width, int image_height)
{
    if (image_width <= 0 || image_height <= 0) {
        return;
    }

    ImPlot::PushPlotClipRect();
    ImDrawList* draw_list = ImPlot::GetPlotDrawList();
    for (int i = 0; i < static_cast<int>(IM_ARRAYSIZE(kUsafPreviewAnchors)); ++i) {
        const UsafPreviewAnchor& anchor = kUsafPreviewAnchors[i];
        const bool active = (ui_state->capture_label_index == i);
        const bool captured = is_usaf_position_captured(ui_state, anchor.label);
        const ImU32 color = active ? IM_COL32(255, 210, 70, 255)
                                   : (captured ? IM_COL32(90, 225, 120, 220)
                                               : IM_COL32(170, 210, 255, 180));
        const float thickness = active ? 2.5f : 1.5f;
        const float arm = active ? 18.0f : 13.0f;
        const float radius = active ? 5.0f : 4.0f;
        const ImPlotPoint point(anchor.norm_x * static_cast<double>(image_width),
                                anchor.norm_y * static_cast<double>(image_height));
        const ImVec2 center = ImPlot::PlotToPixels(point);
        draw_list->AddLine(ImVec2(center.x - arm, center.y), ImVec2(center.x + arm, center.y), color, thickness);
        draw_list->AddLine(ImVec2(center.x, center.y - arm), ImVec2(center.x, center.y + arm), color, thickness);
        draw_list->AddCircle(center, radius, color, 0, thickness);
        draw_list->AddText(ImVec2(center.x + arm + 4.0f, center.y - arm - 2.0f), color, anchor.button_label);
    }
    ImPlot::PopPlotClipRect();
}


void start_usaf_preview_worker(UsafResolutionUiState* ui_state, CameraEmergent* ecams, CameraParams* cameras_params)
{
    join_usaf_preview_worker_if_finished(ui_state);
    if (ui_state->preview_running.load(std::memory_order_acquire)) {
        return;
    }
    std::string transaction_error;
    if (!acquire_usaf_transaction(
            ui_state,
            kUsafPreviewTransactionOwner,
            cameras_params[ui_state->selected_camera],
            orange::calibration::Mutation::kCameraParameters |
                orange::calibration::Mutation::kCameraStreamLifecycle,
            "Acquire native-resolution USAF target views while preserving camera timing.",
            &transaction_error)) {
        std::lock_guard<std::mutex> lock(ui_state->preview_mutex);
        ui_state->live_preview.status_message = "USAF preview was not started.";
        ui_state->live_preview.error_message = transaction_error;
        return;
    }
    if (ui_state->preview_running.exchange(true, std::memory_order_acq_rel)) {
        release_usaf_transaction_if_owned(
            ui_state,
            kUsafPreviewTransactionOwner,
            "not_started",
            "USAF preview was already running.");
        return;
    }
    ui_state->preview_stop_requested.store(false, std::memory_order_release);

    const int selected_camera = ui_state->selected_camera;
    CameraEmergent* ecam = &ecams[selected_camera];
    CameraParams* camera_params = &cameras_params[selected_camera];
    const int preview_target_fps = std::max(1, ui_state->preview_fps);

    {
        std::lock_guard<std::mutex> lock(ui_state->preview_mutex);
        ui_state->live_preview = {};
        ui_state->live_preview.status_message = "Starting USAF preview...";
    }

    ui_state->preview_worker = std::thread([=]() {
        Emergent::CEmergentFrame* frames = nullptr;
        bool stream_opened = false;
        bool buffers_allocated = false;
        bool acquisition_started = false;
        bool frame_rate_changed = false;
        unsigned int original_frame_rate = camera_params->frame_rate;
        unsigned int active_preview_fps = static_cast<unsigned int>(preview_target_fps);

        auto publish_error = [&](const std::string& message) {
            std::lock_guard<std::mutex> lock(ui_state->preview_mutex);
            ui_state->live_preview.error_message = message;
            ui_state->live_preview.status_message = "USAF preview failed.";
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

            camera_open_stream(&ecam->camera, camera_params);
            stream_opened = true;

            frames = new Emergent::CEmergentFrame[kUsafPreviewBufferCount]();
            allocate_frame_buffer(&ecam->camera, frames, camera_params, kUsafPreviewBufferCount);
            buffers_allocated = true;

            check_camera_errors(EVT_CameraExecuteCommand(&ecam->camera, "AcquisitionStart"),
                                camera_params->camera_serial.c_str());
            acquisition_started = true;

            while (!ui_state->preview_stop_requested.load(std::memory_order_acquire)) {
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

                cv::Mat preview_rgba;
                cv::cvtColor(preview_bgr, preview_rgba, cv::COLOR_BGR2RGBA);
                cv::Mat full_rgb;
                cv::cvtColor(bgr, full_rgb, cv::COLOR_BGR2RGB);

                {
                    std::lock_guard<std::mutex> lock(ui_state->preview_mutex);
                    ui_state->live_preview.available = true;
                    ui_state->live_preview.width = preview_rgba.cols;
                    ui_state->live_preview.height = preview_rgba.rows;
                    ui_state->live_preview.capture_width = full_rgb.cols;
                    ui_state->live_preview.capture_height = full_rgb.rows;
                    ui_state->live_preview.frame_serial += 1;
                    ui_state->live_preview.rgba.assign(preview_rgba.data,
                                                       preview_rgba.data + preview_rgba.total() * preview_rgba.elemSize());
                    ui_state->live_preview.raw_rgb.assign(full_rgb.data,
                                                          full_rgb.data + full_rgb.total() * full_rgb.elemSize());
                    ui_state->live_preview.error_message.clear();
                    ui_state->live_preview.status_message =
                        std::string("Live USAF preview @ ") + std::to_string(active_preview_fps) +
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
                destroy_frame_buffer(&ecam->camera, frames, kUsafPreviewBufferCount, camera_params);
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
        ui_state->preview_running.store(false, std::memory_order_release);
    });
}

FovCalibrationData build_ui_fov_calibration(const UsafResolutionUiState* ui_state, const CameraParams& camera_params)
{
    FovCalibrationData fov_calibration;
    fov_calibration.enabled = ui_state->enable_fov_calibration;
    if (!fov_calibration.enabled) {
        return fov_calibration;
    }

    fov_calibration.working_distance_mm = std::max(0.0f, ui_state->working_distance_mm);
    fov_calibration.pixel_pitch_um = std::max(0.0f, ui_state->pixel_pitch_um);
    fov_calibration.sensor_width_mm =
        static_cast<double>(camera_params.width) * static_cast<double>(fov_calibration.pixel_pitch_um) / 1000.0;
    fov_calibration.sensor_height_mm =
        static_cast<double>(camera_params.height) * static_cast<double>(fov_calibration.pixel_pitch_um) / 1000.0;
    if (ui_state->field_width_mm > 0.0f) {
        fov_calibration.has_field_width_mm = true;
        fov_calibration.field_width_mm = ui_state->field_width_mm;
        fov_calibration.has_magnification_x = true;
        fov_calibration.magnification_x = fov_calibration.sensor_width_mm / fov_calibration.field_width_mm;
    }
    if (ui_state->field_height_mm > 0.0f) {
        fov_calibration.has_field_height_mm = true;
        fov_calibration.field_height_mm = ui_state->field_height_mm;
        fov_calibration.has_magnification_y = true;
        fov_calibration.magnification_y = fov_calibration.sensor_height_mm / fov_calibration.field_height_mm;
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
    return fov_calibration;
}

void start_usaf_artifact_worker(UsafResolutionUiState* ui_state, CameraEmergent* ecams, CameraParams* cameras_params)
{
    join_usaf_worker_if_finished(ui_state);
    stop_usaf_preview_worker(ui_state);
    if (ui_state->running.load(std::memory_order_acquire)) {
        return;
    }
    std::string transaction_error;
    if (!acquire_usaf_transaction(
            ui_state,
            kUsafArtifactTransactionOwner,
            cameras_params[ui_state->selected_camera],
            orange::calibration::mutation_set(
                orange::calibration::Mutation::kNone),
            "Write a USAF optical characterization from operator-selected native-resolution captures.",
            &transaction_error)) {
        std::lock_guard<std::mutex> lock(ui_state->mutex);
        ui_state->status_message = "USAF artifact was not started.";
        ui_state->error_message = transaction_error;
        return;
    }
    if (ui_state->running.exchange(true, std::memory_order_acq_rel)) {
        release_usaf_transaction_if_owned(
            ui_state,
            kUsafArtifactTransactionOwner,
            "not_started",
            "USAF artifact worker was already running.");
        return;
    }

    const int selected_camera = ui_state->selected_camera;
    CameraEmergent* ecam = &ecams[selected_camera];
    CameraParams* camera_params = &cameras_params[selected_camera];
    const UsafTargetPolarity target_polarity = static_cast<UsafTargetPolarity>(ui_state->target_polarity);
    const std::string illumination_mode = ui_state->illumination_mode;
    const std::string operator_notes = ui_state->operator_notes;
    const std::string output_dir = ui_state->output_dir;
    const std::string output_prefix = ui_state->output_prefix;
    const FovCalibrationData fov_calibration = build_ui_fov_calibration(ui_state, *camera_params);
    const std::vector<UsafCapturedPosition> captured_positions = ui_state->positions;

    {
        std::lock_guard<std::mutex> lock(ui_state->mutex);
        ui_state->status_message = "Preparing USAF calibration artifact...";
        ui_state->error_message.clear();
        ui_state->output_artifact_id.clear();
        ui_state->output_artifact_dir.clear();
        ui_state->output_manifest_path.clear();
        ui_state->output_fingerprint.clear();
        ui_state->output_json_path.clear();
        ui_state->output_positions_csv_path.clear();
        ui_state->has_result = false;
    }

    ui_state->worker = std::thread([=]() {
        auto finish = [&]() { ui_state->running.store(false, std::memory_order_release); };
        try {
            if (captured_positions.empty()) {
                throw std::runtime_error("Capture at least one USAF target position before writing an artifact.");
            }
            bool has_any_selection = false;
            for (const UsafCapturedPosition& position : captured_positions) {
                has_any_selection = has_any_selection || position.horizontal_bars.available || position.vertical_bars.available;
            }
            if (!has_any_selection) {
                throw std::runtime_error("Mark at least one resolved USAF element before writing an artifact.");
            }

            const std::filesystem::path artifact_root_dir(output_dir);
            {
                orange::ScopedFsuid fsuid_guard;
                (void)fsuid_guard;
                std::filesystem::create_directories(artifact_root_dir);
            }
            const std::string timestamp = get_current_date_time();
            const std::string created_utc = get_current_utc_timestamp();
            const std::string artifact_id = build_usaf_resolution_artifact_id(output_prefix, *camera_params, timestamp);
            const UsafResolutionArtifactPaths artifact_paths =
                make_usaf_resolution_artifact_paths(artifact_root_dir.string(), artifact_id);
            {
                orange::ScopedFsuid fsuid_guard;
                (void)fsuid_guard;
                std::filesystem::create_directories(artifact_paths.artifact_dir);
                std::filesystem::create_directories(artifact_paths.target_reference_frames_dir);
                std::filesystem::create_directories(artifact_paths.analysis_overlays_dir);
            }

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
                            "Failed to open camera config snapshot output path: " + artifact_paths.camera_config_snapshot_path;
                    } else {
                        config_out.write(config_snapshot_contents.data(),
                                         static_cast<std::streamsize>(config_snapshot_contents.size()));
                        config_out.close();
                        if (!config_out) {
                            camera_config_snapshot.error =
                                "Failed to write camera config snapshot output path: " + artifact_paths.camera_config_snapshot_path;
                        } else {
                            camera_config_snapshot.has_snapshot = true;
                            camera_config_snapshot.snapshot_path = artifact_paths.camera_config_snapshot_path;
                            camera_config_snapshot.error.clear();
                        }
                    }
                }
            }

            UsafResolutionRequest request;
            request.target_polarity = target_polarity;
            request.illumination_mode = illumination_mode;
            request.operator_notes = operator_notes;
            request.camera_config_snapshot = camera_config_snapshot;
            request.fov_calibration = fov_calibration;
            request.positions = captured_positions;

            std::string lens_name;
            get_camera_string_param(&ecam->camera, "LensName", &lens_name);

            UsafResolutionResult result = evaluate_usaf_resolution_request(request);
            std::string write_error;
            for (size_t i = 0; i < result.positions.size(); ++i) {
                const std::string base_name = request.positions[i].label;
                const std::string reference_path =
                    (std::filesystem::path(artifact_paths.target_reference_frames_dir) / (base_name + ".ppm")).string();
                const std::string overlay_path =
                    (std::filesystem::path(artifact_paths.analysis_overlays_dir) / (base_name + "_overlay.ppm")).string();
                if (!write_usaf_rgb_image_ppm(reference_path,
                                              request.positions[i].rgb,
                                              request.positions[i].width,
                                              request.positions[i].height,
                                              nullptr,
                                              &write_error) ||
                    !write_usaf_rgb_image_ppm(overlay_path,
                                              request.positions[i].rgb,
                                              request.positions[i].width,
                                              request.positions[i].height,
                                              &request.positions[i].roi,
                                              &write_error)) {
                    throw std::runtime_error(write_error);
                }
                result.positions[i].reference_frame_path = reference_path;
                result.positions[i].analysis_overlay_path = overlay_path;
            }

            const nlohmann::json measurement_json = usaf_resolution_to_json(
                result,
                request,
                *camera_params,
                lens_name,
                artifact_id,
                created_utc,
                "",
                artifact_paths);
            const std::string fingerprint = compute_usaf_resolution_fingerprint(measurement_json, artifact_paths, &write_error);
            if (fingerprint.empty()) {
                throw std::runtime_error(write_error.empty() ? "Failed to compute USAF artifact fingerprint." : write_error);
            }
            const nlohmann::json measurement_json_with_fingerprint = usaf_resolution_to_json(
                result,
                request,
                *camera_params,
                lens_name,
                artifact_id,
                created_utc,
                fingerprint,
                artifact_paths);
            const nlohmann::json manifest_json = usaf_resolution_manifest_to_json(
                result,
                request,
                *camera_params,
                lens_name,
                artifact_id,
                created_utc,
                fingerprint,
                artifact_paths);

            if (!write_usaf_resolution_json(artifact_paths.manifest_path, manifest_json, &write_error) ||
                !write_usaf_resolution_json(artifact_paths.measurement_json_path, measurement_json_with_fingerprint, &write_error) ||
                !write_usaf_resolution_positions_csv(artifact_paths.positions_csv_path, result, &write_error)) {
                throw std::runtime_error(write_error);
            }
            if (!update_calibration_artifact_registry(artifact_root_dir.string(), manifest_json, &write_error)) {
                throw std::runtime_error(write_error);
            }

            {
                std::lock_guard<std::mutex> lock(ui_state->mutex);
                ui_state->status_message = "USAF resolution calibration completed.";
                ui_state->error_message.clear();
                ui_state->output_artifact_id = artifact_id;
                ui_state->output_artifact_dir = artifact_paths.artifact_dir;
                ui_state->output_manifest_path = artifact_paths.manifest_path;
                ui_state->output_fingerprint = fingerprint;
                ui_state->output_json_path = artifact_paths.measurement_json_path;
                ui_state->output_positions_csv_path = artifact_paths.positions_csv_path;
                ui_state->has_result = true;
                ui_state->last_result = std::move(result);
                ui_state->last_fov_calibration = request.fov_calibration;
                ui_state->last_camera_serial = camera_params->camera_serial;
                ui_state->last_focus = camera_params->focus;
                ui_state->last_exposure = camera_params->exposure;
            }
        } catch (const std::exception& ex) {
            std::lock_guard<std::mutex> lock(ui_state->mutex);
            ui_state->status_message = "USAF resolution calibration failed.";
            ui_state->error_message = ex.what();
        }
        finish();
    });
}

void render_roi_overlay(const UsafCapturedPosition& position)
{
    if (position.roi.has_roi && position.roi.width > 0 && position.roi.height > 0) {
        ImPlot::PushPlotClipRect();
        ImDrawList* draw_list = ImPlot::GetPlotDrawList();
        const ImPlotPoint top_left(position.roi.x, position.roi.y);
        const ImPlotPoint bottom_right(position.roi.x + position.roi.width, position.roi.y + position.roi.height);
        const ImVec2 tl = ImPlot::PlotToPixels(top_left);
        const ImVec2 br = ImPlot::PlotToPixels(bottom_right);
        draw_list->AddRect(tl, br, IM_COL32(0, 220, 0, 255), 0.0f, 0, 2.0f);
        ImPlot::PopPlotClipRect();
    }
}

void handle_roi_drag(UsafResolutionUiState* ui_state, UsafCapturedPosition* position)
{
    if (position == nullptr || !ImPlot::IsPlotHovered()) {
        return;
    }

    const bool roi_modifier_active = ImGui::GetIO().KeyShift;
    if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) && roi_modifier_active) {
        const ImPlotPoint mouse = ImPlot::GetPlotMousePos();
        ui_state->roi_drag_active = true;
        ui_state->roi_drag_start_x = mouse.x;
        ui_state->roi_drag_start_y = mouse.y;
        ui_state->roi_drag_current_x = mouse.x;
        ui_state->roi_drag_current_y = mouse.y;
    }

    if (ui_state->roi_drag_active) {
        const ImPlotPoint mouse = ImPlot::GetPlotMousePos();
        ui_state->roi_drag_current_x = mouse.x;
        ui_state->roi_drag_current_y = mouse.y;
        if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            position->roi = normalize_roi(
                ui_state->roi_drag_start_x,
                ui_state->roi_drag_start_y,
                ui_state->roi_drag_current_x,
                ui_state->roi_drag_current_y,
                position->width,
                position->height);
            ui_state->roi_drag_active = false;
        }
    }

    if (ui_state->roi_drag_active) {
        const UsafRoi drag_roi = normalize_roi(
            ui_state->roi_drag_start_x,
            ui_state->roi_drag_start_y,
            ui_state->roi_drag_current_x,
            ui_state->roi_drag_current_y,
            position->width,
            position->height);
        if (drag_roi.has_roi) {
            ImPlot::PushPlotClipRect();
            ImDrawList* draw_list = ImPlot::GetPlotDrawList();
            const ImVec2 tl = ImPlot::PlotToPixels(ImPlotPoint(drag_roi.x, drag_roi.y));
            const ImVec2 br = ImPlot::PlotToPixels(ImPlotPoint(drag_roi.x + drag_roi.width, drag_roi.y + drag_roi.height));
            draw_list->AddRect(tl, br, IM_COL32(255, 220, 0, 255), 0.0f, 0, 2.0f);
            ImPlot::PopPlotClipRect();
        }
    }
}

} // namespace

void join_usaf_preview_worker_if_finished(UsafResolutionUiState* ui_state)
{
    if (!ui_state->preview_running.load(std::memory_order_acquire) && ui_state->preview_worker.joinable()) {
        ui_state->preview_worker.join();
        std::string error;
        std::string status;
        {
            std::lock_guard<std::mutex> lock(ui_state->preview_mutex);
            error = ui_state->live_preview.error_message;
            status = ui_state->live_preview.status_message;
        }
        release_usaf_transaction_if_owned(
            ui_state,
            kUsafPreviewTransactionOwner,
            error.empty() ? "complete" : "failed",
            error.empty() ? status : error);
    }
}

void stop_usaf_preview_worker(UsafResolutionUiState* ui_state)
{
    if (ui_state->preview_running.exchange(false, std::memory_order_acq_rel)) {
        ui_state->preview_stop_requested.store(true, std::memory_order_release);
    }
    if (ui_state->preview_worker.joinable()) {
        ui_state->preview_worker.join();
    }
    ui_state->preview_stop_requested.store(false, std::memory_order_release);
    std::string error;
    std::string status;
    {
        std::lock_guard<std::mutex> lock(ui_state->preview_mutex);
        error = ui_state->live_preview.error_message;
        status = ui_state->live_preview.status_message;
    }
    release_usaf_transaction_if_owned(
        ui_state,
        kUsafPreviewTransactionOwner,
        error.empty() ? "stopped" : "failed",
        error.empty() ? status : error);
}

void clear_usaf_preview_texture(UsafResolutionUiState* ui_state)
{
    orange::preview::clear_texture(
        &ui_state->preview_texture,
        &ui_state->preview_texture_width,
        &ui_state->preview_texture_height);
    ui_state->preview_uploaded_serial = 0;
    ui_state->live_canvas_view.fit_requested = true;
    ui_state->live_canvas_view.last_image_width = 0;
    ui_state->live_canvas_view.last_image_height = 0;
}

void clear_usaf_captured_texture(UsafResolutionUiState* ui_state)
{
    orange::preview::clear_texture(
        &ui_state->captured_texture,
        &ui_state->captured_texture_width,
        &ui_state->captured_texture_height);
    ui_state->captured_texture_position_index = -1;
    ui_state->captured_canvas_view.fit_requested = true;
    ui_state->captured_canvas_view.last_image_width = 0;
    ui_state->captured_canvas_view.last_image_height = 0;
}

void join_usaf_worker_if_finished(UsafResolutionUiState* ui_state)
{
    if (!ui_state->running.load(std::memory_order_acquire) && ui_state->worker.joinable()) {
        ui_state->worker.join();
        std::string error;
        std::string status;
        {
            std::lock_guard<std::mutex> lock(ui_state->mutex);
            error = ui_state->error_message;
            status = ui_state->status_message;
        }
        release_usaf_transaction_if_owned(
            ui_state,
            kUsafArtifactTransactionOwner,
            error.empty() ? "complete" : "failed",
            error.empty() ? status : error);
    }
}

void render_usaf_resolution_window(
    UsafResolutionUiState* ui_state,
    CameraControl* camera_control,
    CameraEmergent* ecams,
    CameraParams* cameras_params,
    int num_cameras,
    const std::string& default_output_dir)
{
    if (!ui_state->show_window) {
        stop_usaf_preview_worker(ui_state);
        return;
    }

    join_usaf_worker_if_finished(ui_state);
    join_usaf_preview_worker_if_finished(ui_state);

    if (ui_state->output_dir[0] == '\0') {
        copy_string_to_buffer(ui_state->output_dir, default_output_dir);
    }

    if (!ImGui::Begin("USAF Resolution Calibration", &ui_state->show_window)) {
        ImGui::End();
        return;
    }

    if (num_cameras <= 0 || cameras_params == nullptr || ecams == nullptr || !camera_control->open) {
        ImGui::TextDisabled("Open cameras before using USAF resolution calibration.");
        ImGui::End();
        return;
    }

    ui_state->selected_camera = std::clamp(ui_state->selected_camera, 0, std::max(0, num_cameras - 1));
    const CameraParams& selected_camera = cameras_params[ui_state->selected_camera];
    if (ui_state->configured_camera_index != ui_state->selected_camera) {
        ui_state->configured_camera_index = ui_state->selected_camera;
        ui_state->positions.clear();
        ui_state->selected_position_index = 0;
        clear_usaf_captured_texture(ui_state);
        if (ui_state->output_dir[0] == '\0') {
            copy_string_to_buffer(ui_state->output_dir, default_output_dir);
        }
    }

    const bool running = ui_state->running.load(std::memory_order_acquire);
    const bool preview_running = ui_state->preview_running.load(std::memory_order_acquire);

    const bool can_preview = !running && !camera_control->subscribe && !camera_control->record_video;
    const bool can_write = !running && !preview_running && !camera_control->subscribe && !camera_control->record_video;

    std::vector<std::string> camera_labels_storage;
    std::vector<const char*> camera_labels;
    camera_labels_storage.reserve(num_cameras);
    camera_labels.reserve(num_cameras);
    for (int i = 0; i < num_cameras; ++i) {
        std::ostringstream label;
        label << i << ": " << cameras_params[i].camera_serial;
        camera_labels_storage.push_back(label.str());
    }
    for (const std::string& label : camera_labels_storage) {
        camera_labels.push_back(label.c_str());
    }

    ImGui::Combo("Camera", &ui_state->selected_camera, camera_labels.data(), num_cameras);
    ImGui::Text("Current settings: focus=%u iris=%u exposure=%u gain=%u",
                selected_camera.focus,
                selected_camera.iris,
                selected_camera.exposure,
                selected_camera.gain);

    const char* polarity_items[] = {"Negative", "Positive"};
    ImGui::Combo("Target polarity", &ui_state->target_polarity, polarity_items, IM_ARRAYSIZE(polarity_items));
    ImGui::InputText("Illumination mode", ui_state->illumination_mode, IM_ARRAYSIZE(ui_state->illumination_mode));
    ImGui::InputTextMultiline("Operator notes", ui_state->operator_notes, IM_ARRAYSIZE(ui_state->operator_notes), ImVec2(-1.0f, 80.0f));

    ImGui::Checkbox("Include FOV metadata", &ui_state->enable_fov_calibration);
    if (ui_state->enable_fov_calibration) {
        ImGui::InputFloat("Working distance (mm)", &ui_state->working_distance_mm, 5.0f, 25.0f, "%.1f");
        ImGui::InputFloat("Pixel pitch (um)", &ui_state->pixel_pitch_um, 0.01f, 0.1f, "%.3f");
        ImGui::InputFloat("Field width (mm)", &ui_state->field_width_mm, 1.0f, 10.0f, "%.2f");
        ImGui::InputFloat("Field height (mm)", &ui_state->field_height_mm, 1.0f, 10.0f, "%.2f");
        ui_state->working_distance_mm = std::max(0.0f, ui_state->working_distance_mm);
        ui_state->pixel_pitch_um = std::max(0.0f, ui_state->pixel_pitch_um);
        ui_state->field_width_mm = std::max(0.0f, ui_state->field_width_mm);
        ui_state->field_height_mm = std::max(0.0f, ui_state->field_height_mm);

        const double sensor_width_mm =
            static_cast<double>(selected_camera.width) * static_cast<double>(ui_state->pixel_pitch_um) / 1000.0;
        const double sensor_height_mm =
            static_cast<double>(selected_camera.height) * static_cast<double>(ui_state->pixel_pitch_um) / 1000.0;
        ImGui::Text("Derived sensor size: %.3f mm x %.3f mm", sensor_width_mm, sensor_height_mm);
        if (ui_state->field_width_mm > 0.0f) {
            ImGui::Text("Pixels/mm X: %.3f", static_cast<double>(selected_camera.width) / ui_state->field_width_mm);
        }
        if (ui_state->field_height_mm > 0.0f) {
            ImGui::Text("Pixels/mm Y: %.3f", static_cast<double>(selected_camera.height) / ui_state->field_height_mm);
        }
    }

    ImGui::SeparatorText("Live Target Preview");
    ImGui::InputInt("Preview FPS", &ui_state->preview_fps);
    ui_state->preview_fps = std::clamp(ui_state->preview_fps, 1, 120);

    if (!can_preview) {
        ImGui::TextDisabled("Stop streaming and recording before using the USAF preview.");
    }
    if (!preview_running) {
        if (ImGui::Button("Start target preview") && can_preview) {
            start_usaf_preview_worker(ui_state, ecams, cameras_params);
        }
    } else {
        if (ImGui::Button("Stop target preview")) {
            stop_usaf_preview_worker(ui_state);
        }
    }
    ImGui::SameLine();
    ImGui::Text("%s", preview_running ? "Live" : "Stopped");

    std::string preview_status;
    std::string preview_error;
    bool preview_available = false;
    {
        std::lock_guard<std::mutex> lock(ui_state->preview_mutex);
        preview_status = ui_state->live_preview.status_message;
        preview_error = ui_state->live_preview.error_message;
        preview_available = ui_state->live_preview.available;
    }
    if (!preview_status.empty()) {
        ImGui::TextWrapped("%s", preview_status.c_str());
    }
    if (!preview_error.empty()) {
        ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.35f, 1.0f), "%s", preview_error.c_str());
    }

    if (ImGui::Button("Fit live view")) {
        ui_state->live_canvas_view.fit_requested = true;
    }
    if (preview_available && upload_live_preview_texture(ui_state) && ui_state->preview_texture != 0) {
        if (orange::ui::begin_image_canvas("USAF Live Preview",
                                    ui_state->preview_texture,
                                    ui_state->preview_texture_width,
                                    ui_state->preview_texture_height,
                                    &ui_state->live_canvas_view,
                                    0.45f)) {
            render_usaf_position_guides(ui_state,
                                        ui_state->preview_texture_width,
                                        ui_state->preview_texture_height);
            ImPlot::EndPlot();
        }
        ImGui::TextDisabled("Wheel zooms. Drag to pan. Use Fit live view to reset. Position guides show the next capture target.");
    }

    render_usaf_position_selector(ui_state);
    std::string capture_error;
    if (ImGui::Button("Capture position")) {
        const char* capture_label = kUsafPreviewAnchors[ui_state->capture_label_index].label;
        if (!capture_current_usaf_snapshot(ui_state, capture_label, &capture_error)) {
            std::lock_guard<std::mutex> lock(ui_state->preview_mutex);
            ui_state->live_preview.error_message = capture_error;
        } else {
            auto it = std::find_if(ui_state->positions.begin(), ui_state->positions.end(),
                                   [&](const UsafCapturedPosition& position) {
                                       return position.label == capture_label;
                                   });
            if (it != ui_state->positions.end()) {
                ui_state->selected_position_index = static_cast<int>(std::distance(ui_state->positions.begin(), it));
                clear_usaf_captured_texture(ui_state);
            }
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Clear captures")) {
        ui_state->positions.clear();
        ui_state->selected_position_index = 0;
        clear_usaf_captured_texture(ui_state);
    }

    ImGui::SeparatorText("Captured Positions");
    if (ui_state->positions.empty()) {
        ImGui::TextDisabled("No captured USAF positions yet.");
    } else {
        std::vector<std::string> position_labels_storage;
        std::vector<const char*> position_labels;
        position_labels_storage.reserve(ui_state->positions.size());
        position_labels.reserve(ui_state->positions.size());
        for (const UsafCapturedPosition& position : ui_state->positions) {
            std::ostringstream label;
            label << position.label << " (" << position.width << "x" << position.height << ")";
            position_labels_storage.push_back(label.str());
        }
        for (const std::string& label : position_labels_storage) {
            position_labels.push_back(label.c_str());
        }
        ui_state->selected_position_index =
            std::clamp(ui_state->selected_position_index, 0, static_cast<int>(ui_state->positions.size()) - 1);
        ImGui::Combo("Selected position", &ui_state->selected_position_index, position_labels.data(),
                     static_cast<int>(position_labels.size()));

        UsafCapturedPosition& selected_position = ui_state->positions[static_cast<size_t>(ui_state->selected_position_index)];
        if (ImGui::Button("Fit captured view")) {
            ui_state->captured_canvas_view.fit_requested = true;
        }
        if (ensure_captured_texture(ui_state, ui_state->selected_position_index) && ui_state->captured_texture != 0) {
            if (orange::ui::begin_image_canvas("USAF Position Overlay",
                                        ui_state->captured_texture,
                                        selected_position.width,
                                        selected_position.height,
                                        &ui_state->captured_canvas_view,
                                        0.60f)) {
                render_roi_overlay(selected_position);
                handle_roi_drag(ui_state, &selected_position);
                ImPlot::EndPlot();
            }
            ImGui::TextDisabled("Wheel zooms. Drag to pan. Hold Shift and drag to set the USAF ROI. Use Fit captured view to reset.");
        }

        ImGui::InputInt("ROI x", &selected_position.roi.x);
        ImGui::InputInt("ROI y", &selected_position.roi.y);
        ImGui::InputInt("ROI width", &selected_position.roi.width);
        ImGui::InputInt("ROI height", &selected_position.roi.height);
        selected_position.roi.x = std::clamp(selected_position.roi.x, 0, std::max(0, selected_position.width - 1));
        selected_position.roi.y = std::clamp(selected_position.roi.y, 0, std::max(0, selected_position.height - 1));
        selected_position.roi.width = std::clamp(selected_position.roi.width, 0, selected_position.width - selected_position.roi.x);
        selected_position.roi.height = std::clamp(selected_position.roi.height, 0, selected_position.height - selected_position.roi.y);
        selected_position.roi.has_roi = selected_position.roi.width > 0 && selected_position.roi.height > 0;
        if (ImGui::Button("Use full frame ROI")) {
            selected_position.roi.has_roi = true;
            selected_position.roi.x = 0;
            selected_position.roi.y = 0;
            selected_position.roi.width = selected_position.width;
            selected_position.roi.height = selected_position.height;
        }
        ImGui::SameLine();
        if (ImGui::Button("Clear ROI")) {
            selected_position.roi = {};
        }

        ImGui::Checkbox("Horizontal bars resolved", &selected_position.horizontal_bars.available);
        if (selected_position.horizontal_bars.available) {
            ImGui::InputInt("Horizontal group", &selected_position.horizontal_bars.group);
            ImGui::InputInt("Horizontal element", &selected_position.horizontal_bars.element);
            selected_position.horizontal_bars.element = std::clamp(selected_position.horizontal_bars.element, 1, 6);
        }
        ImGui::Checkbox("Vertical bars resolved", &selected_position.vertical_bars.available);
        if (selected_position.vertical_bars.available) {
            ImGui::InputInt("Vertical group", &selected_position.vertical_bars.group);
            ImGui::InputInt("Vertical element", &selected_position.vertical_bars.element);
            selected_position.vertical_bars.element = std::clamp(selected_position.vertical_bars.element, 1, 6);
        }
        if (selected_position.horizontal_bars.available) {
            const double lp = usaf_lp_per_mm(selected_position.horizontal_bars.group, selected_position.horizontal_bars.element);
            if (lp > 0.0) {
                ImGui::Text("Horizontal bars: %.3f lp/mm, %.2f um/bar", lp, 500.0 / lp);
            }
        }
        if (selected_position.vertical_bars.available) {
            const double lp = usaf_lp_per_mm(selected_position.vertical_bars.group, selected_position.vertical_bars.element);
            if (lp > 0.0) {
                ImGui::Text("Vertical bars: %.3f lp/mm, %.2f um/bar", lp, 500.0 / lp);
            }
        }
        std::string note_label = std::string("Notes for ") + selected_position.label;
        char notes_buffer[256] = "";
        copy_string_to_buffer(notes_buffer, selected_position.notes);
        if (ImGui::InputTextMultiline(note_label.c_str(), notes_buffer, IM_ARRAYSIZE(notes_buffer), ImVec2(-1.0f, 60.0f))) {
            selected_position.notes = notes_buffer;
        }
        if (ImGui::Button("Remove selected position")) {
            ui_state->positions.erase(ui_state->positions.begin() + ui_state->selected_position_index);
            ui_state->selected_position_index = std::max(0, ui_state->selected_position_index - 1);
            clear_usaf_captured_texture(ui_state);
        }
    }

    ImGui::Separator();
    ImGui::InputText("Artifact root", ui_state->output_dir, IM_ARRAYSIZE(ui_state->output_dir));
    ImGui::InputText("Artifact label", ui_state->output_prefix, IM_ARRAYSIZE(ui_state->output_prefix));

    if (!can_write) {
        ImGui::TextDisabled("Stop preview, streaming, and recording before writing the USAF artifact.");
    }
    if (ImGui::Button("Write USAF Artifact") && can_write) {
        start_usaf_artifact_worker(ui_state, ecams, cameras_params);
    }
    if (running) {
        ImGui::SameLine();
        ImGui::Text("Writing...");
    }

    std::string status_message;
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
    {
        std::lock_guard<std::mutex> lock(ui_state->mutex);
        status_message = ui_state->status_message;
        error_message = ui_state->error_message;
        output_artifact_id = ui_state->output_artifact_id;
        output_artifact_dir = ui_state->output_artifact_dir;
        output_manifest_path = ui_state->output_manifest_path;
        output_fingerprint = ui_state->output_fingerprint;
        output_json_path = ui_state->output_json_path;
        output_positions_csv_path = ui_state->output_positions_csv_path;
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
        if (ImGui::SmallButton("Copy USAF Artifact ID")) {
            ImGui::SetClipboardText(output_artifact_id.c_str());
        }
        ImGui::TextWrapped("Fingerprint: %s", output_fingerprint.c_str());
        ImGui::TextWrapped("Artifact dir: %s", output_artifact_dir.c_str());
        ImGui::TextWrapped("Manifest: %s", output_manifest_path.c_str());
        ImGui::TextWrapped("Measurement JSON: %s", output_json_path.c_str());
        ImGui::TextWrapped("Positions CSV: %s", output_positions_csv_path.c_str());
        if (last_result.has_center_single_bar_width_um) {
            ImGui::Text("Center resolved single-bar width: %.2f um", last_result.center_single_bar_width_um);
        }
        if (last_result.has_best_field_single_bar_width_um && last_result.has_worst_field_single_bar_width_um) {
            ImGui::Text("Field range: best %.2f um, worst %.2f um",
                        last_result.best_field_single_bar_width_um,
                        last_result.worst_field_single_bar_width_um);
        }
        if (last_result.has_field_resolution_cv) {
            ImGui::Text("Field resolution CV: %.4f", last_result.field_resolution_cv);
        }
        if (last_fov_calibration.enabled) {
            ImGui::Text("FOV metadata: working_distance=%.1f mm pixel_pitch=%.3f um",
                        last_fov_calibration.working_distance_mm,
                        last_fov_calibration.pixel_pitch_um);
        }

        if (ImGui::BeginTable("USAF Position Summary", 7,
                              ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchSame)) {
            ImGui::TableSetupColumn("Position");
            ImGui::TableSetupColumn("H bars");
            ImGui::TableSetupColumn("H um/bar");
            ImGui::TableSetupColumn("V bars");
            ImGui::TableSetupColumn("V um/bar");
            ImGui::TableSetupColumn("Worst um/bar");
            ImGui::TableSetupColumn("Notes");
            ImGui::TableHeadersRow();
            for (const UsafPerPositionResult& position : last_result.positions) {
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(position.label.c_str());
                ImGui::TableNextColumn();
                if (position.horizontal_bars.available) {
                    ImGui::Text("G%d E%d", position.horizontal_bars.group, position.horizontal_bars.element);
                } else {
                    ImGui::TextDisabled("-");
                }
                ImGui::TableNextColumn();
                if (position.horizontal_bars.available) {
                    ImGui::Text("%.2f", position.horizontal_bars.single_bar_width_um);
                } else {
                    ImGui::TextDisabled("-");
                }
                ImGui::TableNextColumn();
                if (position.vertical_bars.available) {
                    ImGui::Text("G%d E%d", position.vertical_bars.group, position.vertical_bars.element);
                } else {
                    ImGui::TextDisabled("-");
                }
                ImGui::TableNextColumn();
                if (position.vertical_bars.available) {
                    ImGui::Text("%.2f", position.vertical_bars.single_bar_width_um);
                } else {
                    ImGui::TextDisabled("-");
                }
                ImGui::TableNextColumn();
                if (position.has_position_summary) {
                    ImGui::Text("%.2f", position.position_worst_single_bar_width_um);
                } else {
                    ImGui::TextDisabled("-");
                }
                ImGui::TableNextColumn();
                ImGui::TextWrapped("%s", position.notes.c_str());
            }
            ImGui::EndTable();
        }
    }

    ImGui::End();
}
