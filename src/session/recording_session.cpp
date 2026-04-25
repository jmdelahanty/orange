#include "session/recording_session.h"

#include "project.h"
#include "recording_ingress.h"
#include "recording_output_utils.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <iostream>

namespace orange::session {

namespace {

bool is_valid_pipeline_index(const RecordingSessionState& state, const int camera_index)
{
    return camera_index >= 0 &&
           camera_index < static_cast<int>(state.recording_pipelines.size());
}

bool env_flag_enabled(const char* name)
{
    const char* value = std::getenv(name);
    if (!value) {
        return false;
    }
    std::string text(value);
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return !(text.empty() || text == "0" || text == "false" || text == "no" || text == "off");
}

std::string resolve_gui_recording_sink_mode()
{
    std::string requested;
    if (const char* env = std::getenv("ORANGE_GUI_RECORDING_SINK_MODE")) {
        requested = env;
    }
    if (env_flag_enabled("ORANGE_GUI_DIAGNOSTIC_NO_FULL_FRAME")) {
        requested = "immediate_recycle";
    }

    std::string normalized = normalize_recording_sink_mode(requested);
    if (normalized.empty()) {
        std::cerr << "[recording_session] Unsupported ORANGE_GUI_RECORDING_SINK_MODE='"
                  << requested << "'; using real full-frame recording." << std::endl;
        normalized = "real";
    }
    if (normalized != "real") {
        std::cout << "[recording_session] GUI recording sink mode: " << normalized
                  << " (full-frame video disabled for non-real sink modes)" << std::endl;
    }
    return normalized;
}

}  // namespace

void create_recording_pipelines_for_stream(RecordingSessionState* state,
                                           CameraParams* cameras_params,
                                           CameraEachSelect* cameras_select,
                                           const int num_cameras,
                                           const EncoderConfig& encoder_config,
                                           CameraResources* camera_resources,
                                           CameraControl* camera_control)
{
    if (!state || !cameras_params || !cameras_select || !camera_resources || !camera_control || num_cameras <= 0) {
        return;
    }

    state->recording_pipelines.clear();
    state->recording_pipelines.resize(num_cameras);
    state->recording_sink_mode = resolve_gui_recording_sink_mode();

    for (int i = 0; i < num_cameras; ++i) {
        if (!cameras_select[i].record) {
            continue;
        }

        std::string recording_output_warning;
        const RecordingOutputConfig recording_output_config =
            orange::recording::resolve_recording_output_config(
                cameras_params[i],
                encoder_config,
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
        recording_overrides.codec = encoder_config.encoder_codec;
        recording_overrides.preset = encoder_config.encoder_preset;
        recording_overrides.tuning = encoder_config.tuning_info;
        recording_overrides.rate_control_mode = encoder_config.rate_control_mode;
        recording_overrides.quality_value = encoder_config.quality_value;
        recording_overrides.gop_length = encoder_config.gop_length;
        recording_overrides.encoder_control_overrides.aq = encoder_config.aq;
        recording_overrides.encoder_control_overrides.temporal_aq = encoder_config.temporal_aq;
        recording_overrides.base_folder_name = encoder_config.folder_name;

        const ResolvedRecordingConfig resolved_recording_config =
            build_resolved_recording_config(cameras_params[i], recording_overrides);
        state->recording_pipelines[i] = std::make_unique<ModernRecordingPipeline>(
            &cameras_params[i],
            resolved_recording_config,
            *camera_resources[i].recycle_queue,
            camera_control,
            state->recording_sink_mode);
    }
}

RecordingRunStartResult begin_recording_run(CameraControl* camera_control,
                                            CameraParams* cameras_params,
                                            const int num_cameras,
                                            const std::string& base_folder,
                                            PTPParams* ptp_params,
                                            const std::string& recording_sink_mode)
{
    RecordingRunStartResult result;
    if (!camera_control || !cameras_params || num_cameras <= 0) {
        return result;
    }

    camera_control->recording_draining = false;
    camera_control->stop_record = false;

    std::string recording_id = get_current_date_time();
    std::string recording_folder;
    std::string resolved_base_folder = base_folder;
    {
        std::lock_guard<std::mutex> lock(camera_control->recording_folder_mutex);
        if (camera_control->recording_folder.empty()) {
            camera_control->recording_folder = resolved_base_folder + "/" + recording_id;
        } else {
            recording_id = std::filesystem::path(camera_control->recording_folder).filename().string();
        }
        recording_folder = camera_control->recording_folder;
    }

    if (resolved_base_folder.empty() && !recording_folder.empty()) {
        const std::filesystem::path parent = std::filesystem::path(recording_folder).parent_path();
        if (parent.empty() || parent == "/") {
            resolved_base_folder = recording_folder;
        } else {
            resolved_base_folder = parent.string();
        }
    }

    make_folder(recording_folder);
    write_recording_snapshot(
        recording_folder,
        recording_id,
        cameras_params,
        num_cameras,
        resolved_base_folder,
        true,
        camera_control->sync_camera,
        ptp_params,
        recording_sink_mode);
    initialize_ptp_sync_summary(
        recording_folder,
        recording_id,
        num_cameras,
        camera_control->sync_camera,
        ptp_params);

    camera_control->record_video = true;
    result.ok = true;
    result.recording_folder = std::move(recording_folder);
    result.recording_sink_mode = recording_sink_mode;
    return result;
}

void request_stop_recording_run(CameraControl* camera_control)
{
    if (!camera_control) {
        return;
    }

    camera_control->record_video = false;
    camera_control->recording_draining = true;
    camera_control->stop_record = true;
    if (camera_control->active_recorders.load(std::memory_order_relaxed) == 0) {
        camera_control->recording_draining = false;
        camera_control->stop_record = false;
        std::lock_guard<std::mutex> lock(camera_control->recording_folder_mutex);
        camera_control->recording_folder.clear();
    }
}

std::string current_recording_folder(CameraControl* camera_control)
{
    if (!camera_control) {
        return {};
    }

    std::lock_guard<std::mutex> lock(camera_control->recording_folder_mutex);
    return camera_control->recording_folder;
}

void start_recording_pipeline_for_camera(RecordingSessionState* state, const int camera_index)
{
    if (!state || !is_valid_pipeline_index(*state, camera_index)) {
        return;
    }
    if (state->recording_pipelines[camera_index]) {
        state->recording_pipelines[camera_index]->start();
    }
}

void request_stop_recording_pipeline_for_camera(RecordingSessionState* state, const int camera_index)
{
    if (!state || !is_valid_pipeline_index(*state, camera_index)) {
        return;
    }
    if (state->recording_pipelines[camera_index]) {
        state->recording_pipelines[camera_index]->request_stop();
    }
}

void shutdown_recording_pipeline_for_camera(RecordingSessionState* state, const int camera_index)
{
    if (!state || !is_valid_pipeline_index(*state, camera_index)) {
        return;
    }
    if (state->recording_pipelines[camera_index]) {
        state->recording_pipelines[camera_index]->shutdown();
        state->recording_pipelines[camera_index].reset();
    }
}

void clear_recording_pipelines(RecordingSessionState* state)
{
    if (!state) {
        return;
    }
    state->recording_pipelines.clear();
}

RecordingIngress* recording_ingress_for_camera(const RecordingSessionState& state, const int camera_index)
{
    if (!is_valid_pipeline_index(state, camera_index)) {
        return nullptr;
    }
    return state.recording_pipelines[camera_index]
        ? state.recording_pipelines[camera_index]->recording_ingress()
        : nullptr;
}

}  // namespace orange::session
