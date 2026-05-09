#include "session/recording_session.h"

#include "external_recorder_contract_utils.h"
#include "fsuid_guard.h"
#include "project.h"
#include "recording_ingress.h"
#include "recording_output_utils.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
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

std::string trim_ascii_copy(std::string value)
{
    auto is_space = [](unsigned char c) { return std::isspace(c) != 0; };
    value.erase(value.begin(), std::find_if(value.begin(), value.end(), [&](unsigned char c) {
        return !is_space(c);
    }));
    value.erase(std::find_if(value.rbegin(), value.rend(), [&](unsigned char c) {
        return !is_space(c);
    }).base(), value.end());
    return value;
}

std::string resolve_gui_recording_sink_mode(const AppStorageConfig* app_storage_config)
{
    std::string requested;
    if (app_storage_config && !app_storage_config->gui_recording_sink_mode.empty()) {
        requested = app_storage_config->gui_recording_sink_mode;
    }
    if (const char* env = std::getenv("ORANGE_GUI_RECORDING_SINK_MODE")) {
        requested = env;
    }
    if (env_flag_enabled("ORANGE_GUI_DIAGNOSTIC_NO_FULL_FRAME")) {
        requested = "immediate_recycle";
    }

    std::string normalized = normalize_recording_sink_mode(requested);
    if (normalized.empty()) {
        std::cerr << "[recording_session] Unsupported GUI recording sink mode '"
                  << requested << "'; using real full-frame recording." << std::endl;
        normalized = "real";
    }
    if (normalized != "real") {
        std::cout << "[recording_session] GUI recording sink mode: " << normalized
                  << " (full-frame video disabled for non-real sink modes)" << std::endl;
    }
    return normalized;
}

nlohmann::json resolve_gui_external_recorder_contract_config(
    const AppStorageConfig* app_storage_config,
    std::string* source_out,
    std::string* status_out)
{
    if (source_out) {
        source_out->clear();
    }
    if (status_out) {
        status_out->clear();
    }

    nlohmann::json contract = nlohmann::json::object();
    if (app_storage_config &&
        app_storage_config->gui_external_recorder_contract.is_object() &&
        !app_storage_config->gui_external_recorder_contract.empty()) {
        contract = orange::external_recorder::ExtractExternalRecorderContractObject(
            app_storage_config->gui_external_recorder_contract);
        if (source_out) {
            *source_out = "app_config_inline";
        }
    }

    std::string contract_path =
        app_storage_config ? app_storage_config->gui_external_recorder_contract_path : std::string();
    if (const char* env = std::getenv("ORANGE_GUI_EXTERNAL_RECORDER_CONTRACT"); env && *env) {
        contract_path = env;
    }
    if (const char* env = std::getenv("ORANGE_GUI_EXTERNAL_RECORDER_CONTRACT_PATH"); env && *env) {
        contract_path = env;
    }

    contract_path = trim_ascii_copy(contract_path);
    if (!contract_path.empty()) {
        std::string error;
        if (orange::external_recorder::ReadExternalRecorderContractConfigFile(
                contract_path,
                &contract,
                &error)) {
            if (source_out) {
                *source_out = contract_path;
            }
        } else if (status_out) {
            *status_out = "Failed to load GUI external recorder contract " +
                          contract_path + ": " + error;
        }
    }
    return contract.is_object() ? contract : nlohmann::json::object();
}

bool write_json_file(const std::filesystem::path& path,
                     const nlohmann::json& payload,
                     std::string* error_out)
{
    orange::ScopedFsuid fsuid_guard;
    (void)fsuid_guard;

    std::error_code create_error;
    std::filesystem::create_directories(path.parent_path(), create_error);
    if (create_error && !std::filesystem::exists(path.parent_path())) {
        if (error_out) {
            *error_out = "Failed to create parent directory for " +
                         path.string() + ": " + create_error.message();
        }
        return false;
    }

    std::ofstream output(path);
    if (!output) {
        if (error_out) {
            *error_out = "Failed to open " + path.string() + " for writing";
        }
        return false;
    }
    output << payload.dump(2) << "\n";
    if (!output.good()) {
        if (error_out) {
            *error_out = "Failed to write " + path.string();
        }
        return false;
    }
    return true;
}

}  // namespace

nlohmann::json build_recording_control_json(const RecordingControlConfig& config)
{
    return {
        {"record_for_seconds", config.record_for_seconds},
        {"clip_seconds", config.clip_seconds}
    };
}

bool validate_recording_control_config(const RecordingControlConfig& config,
                                       std::string* error_out,
                                       const std::string& context)
{
    const std::string prefix = context.empty() ? "" : context + ": ";
    if (config.record_for_seconds < 0) {
        if (error_out) {
            *error_out = prefix + "recording_control.record_for_seconds must be >= 0";
        }
        return false;
    }
    if (config.clip_seconds < 0) {
        if (error_out) {
            *error_out = prefix + "recording_control.clip_seconds must be >= 0";
        }
        return false;
    }
    if (config.clip_seconds > 0 && config.record_for_seconds <= 0) {
        if (error_out) {
            *error_out = prefix +
                "recording_control.clip_seconds requires record_for_seconds > 0";
        }
        return false;
    }
    return true;
}

namespace {

nlohmann::json build_camera_artifact_json(
    const RecordingSessionCameraArtifact& camera)
{
    nlohmann::json out = {
        {"video", camera.video_path},
        {"metadata", camera.metadata_path},
        {"keyframes", camera.keyframe_path}
    };
    if (camera.frame_count > 0 ||
        camera.first_recording_frame_id > 0 ||
        camera.last_recording_frame_id > 0) {
        out["frame_count"] = camera.frame_count;
        out["first_recording_frame_id"] = camera.first_recording_frame_id;
        out["last_recording_frame_id"] = camera.last_recording_frame_id;
        out["recording_frame_id_gaps"] = camera.recording_frame_id_gaps;
    }
    return out;
}

nlohmann::json build_clip_artifacts_json(
    const std::vector<RecordingSessionCameraArtifact>& cameras,
    nlohmann::json* camera_artifacts_out = nullptr,
    nlohmann::json* camera_serials_out = nullptr)
{
    nlohmann::json video_artifacts = nlohmann::json::object();
    nlohmann::json metadata_artifacts = nlohmann::json::object();
    nlohmann::json keyframe_artifacts = nlohmann::json::object();
    nlohmann::json camera_artifacts = nlohmann::json::object();
    nlohmann::json camera_serials = nlohmann::json::array();

    for (const RecordingSessionCameraArtifact& camera : cameras) {
        if (camera.camera_serial.empty()) {
            continue;
        }
        camera_serials.push_back(camera.camera_serial);
        video_artifacts[camera.camera_serial] = camera.video_path;
        metadata_artifacts[camera.camera_serial] = camera.metadata_path;
        keyframe_artifacts[camera.camera_serial] = camera.keyframe_path;
        camera_artifacts[camera.camera_serial] = build_camera_artifact_json(camera);
    }

    if (camera_artifacts_out) {
        *camera_artifacts_out = camera_artifacts;
    }
    if (camera_serials_out) {
        *camera_serials_out = camera_serials;
    }
    return {
        {"videos", video_artifacts},
        {"metadata", metadata_artifacts},
        {"keyframes", keyframe_artifacts}
    };
}

nlohmann::json build_rolling_clip_entry_json(
    const RollingClipManifestOptions& clip,
    const bool include_schema_fields)
{
    nlohmann::json camera_artifacts;
    nlohmann::json camera_serials;
    nlohmann::json artifacts =
        build_clip_artifacts_json(clip.cameras, &camera_artifacts, &camera_serials);

    nlohmann::json out = {
        {"producer", clip.producer},
        {"session_id", clip.session_id},
        {"clip_index", clip.clip_index},
        {"clip_id", clip.clip_id},
        {"recording_folder", clip.recording_folder},
        {"directory", clip.directory},
        {"status", clip.status},
        {"start_reason", clip.start_reason},
        {"stop_reason", clip.stop_reason},
        {"started_at_utc", clip.started_at_utc},
        {"started_at_elapsed_s", clip.started_at_elapsed_s},
        {"stop_requested_at_utc", clip.stop_requested_at_utc},
        {"stop_requested_at_elapsed_s", clip.stop_requested_at_elapsed_s},
        {"finalized_at_utc", clip.finalized_at_utc},
        {"finalized_at_elapsed_s", clip.finalized_at_elapsed_s},
        {"requested_duration_s", clip.requested_duration_s},
        {"actual_duration_s", clip.actual_duration_s},
        {"drain_duration_s", clip.drain_duration_s},
        {"rollover",
         {
             {"request_id", clip.rollover_request_id},
             {"rollover_at_recording_frame_id", clip.rollover_at_recording_frame_id},
             {"first_recording_frame_id", clip.first_recording_frame_id},
             {"last_recording_frame_id", clip.last_recording_frame_id},
             {"pending_next_clip", clip.pending_next_clip}
         }},
        {"timed_stop_hit", clip.timed_stop_hit},
        {"final_clip", clip.final_clip},
        {"drain_completed", clip.drain_completed},
        {"cameras", camera_serials},
        {"camera_artifacts", camera_artifacts},
        {"artifacts", artifacts}
    };
    if (include_schema_fields) {
        out["schema_id"] = "orange.recording_clip";
        out["schema_version"] = 1;
    }
    return out;
}

}  // namespace

nlohmann::json build_single_clip_recording_session_manifest(
    const SingleClipRecordingSessionManifestOptions& options)
{
    nlohmann::json cameras = nlohmann::json::array();
    nlohmann::json video_artifacts = nlohmann::json::object();
    nlohmann::json metadata_artifacts = nlohmann::json::object();
    nlohmann::json keyframe_artifacts = nlohmann::json::object();
    nlohmann::json camera_artifacts = nlohmann::json::object();

    for (const RecordingSessionCameraArtifact& camera : options.cameras) {
        if (camera.camera_serial.empty()) {
            continue;
        }
        cameras.push_back(camera.camera_serial);
        video_artifacts[camera.camera_serial] = camera.video_path;
        metadata_artifacts[camera.camera_serial] = camera.metadata_path;
        keyframe_artifacts[camera.camera_serial] = camera.keyframe_path;
        camera_artifacts[camera.camera_serial] = {
            {"video", camera.video_path},
            {"metadata", camera.metadata_path},
            {"keyframes", camera.keyframe_path}
        };
    }

    nlohmann::json manifest = {
        {"schema_id", "orange.recording_session"},
        {"schema_version", 1},
        {"producer", options.producer},
        {"session_id", options.session_id},
        {"created_at_utc", options.created_at_utc},
        {"updated_at_utc", options.updated_at_utc},
        {"recording_folder", options.recording_folder},
        {"mode", "single_clip"},
        {"status", options.status},
        {"cameras", cameras},
        {"camera_artifacts", camera_artifacts},
        {"stream",
         {
             {"requested_duration_seconds", options.requested_stream_duration_seconds},
             {"stream_start_delay_seconds", options.stream_start_delay_seconds},
             {"started_at_utc", options.stream_started_at_utc},
             {"finished_at_utc", options.stream_finished_at_utc},
             {"actual_elapsed_s", options.stream_actual_elapsed_s},
             {"interrupted", options.stream_interrupted}
         }},
        {"recording_control", build_recording_control_json(options.recording_control)},
        {"recording",
         {
             {"started", options.recording_started},
             {"started_at_utc", options.recording_started_at_utc},
             {"started_at_elapsed_s", options.recording_started_at_elapsed_s},
             {"stop_requested", options.recording_stop_requested},
             {"stop_requested_at_utc", options.recording_stop_requested_at_utc},
             {"stop_requested_at_elapsed_s", options.recording_stop_requested_at_elapsed_s},
             {"stop_reason", options.recording_stop_reason},
             {"drain_completed", options.recording_drain_completed},
             {"drained_at_utc", options.recording_drained_at_utc},
             {"drained_at_elapsed_s", options.recording_drained_at_elapsed_s},
             {"actual_recording_duration_s", options.actual_recording_duration_s},
             {"drain_duration_s", options.drain_duration_s}
         }},
        {"clips",
         nlohmann::json::array(
             {{
                 {"clip_index", 0},
                 {"clip_id", "clip_0000"},
                 {"recording_folder", options.recording_folder},
                 {"directory", "."},
                 {"started_at_utc", options.recording_started_at_utc},
                 {"stop_requested_at_utc", options.recording_stop_requested_at_utc},
                 {"finalized_at_utc", options.recording_drained_at_utc},
                 {"stop_reason", options.recording_stop_reason},
                 {"requested_duration_s", options.recording_control.record_for_seconds},
                 {"actual_duration_s", options.actual_recording_duration_s},
                 {"timed_stop_hit", options.timed_stop_hit},
                 {"drain_completed", options.recording_drain_completed},
                 {"artifacts",
                  {
                      {"videos", video_artifacts},
                      {"metadata", metadata_artifacts},
                      {"keyframes", keyframe_artifacts}
                  }}
             }})}
    };
    return manifest;
}

nlohmann::json build_recording_clip_manifest(
    const RollingClipManifestOptions& options)
{
    return build_rolling_clip_entry_json(options, true);
}

nlohmann::json build_rolling_clip_recording_session_manifest(
    const RollingRecordingSessionManifestOptions& options)
{
    nlohmann::json cameras = nlohmann::json::array();
    for (const std::string& camera_serial : options.camera_serials) {
        if (!camera_serial.empty()) {
            cameras.push_back(camera_serial);
        }
    }

    nlohmann::json clips = nlohmann::json::array();
    double sum_clip_actual_duration_s = options.sum_clip_actual_duration_s;
    if (sum_clip_actual_duration_s <= 0.0) {
        for (const RollingClipManifestOptions& clip : options.clips) {
            sum_clip_actual_duration_s += clip.actual_duration_s;
        }
    }
    for (const RollingClipManifestOptions& clip : options.clips) {
        clips.push_back(build_rolling_clip_entry_json(clip, false));
    }

    nlohmann::json manifest = {
        {"schema_id", "orange.recording_session"},
        {"schema_version", 1},
        {"producer", options.producer},
        {"session_id", options.session_id},
        {"created_at_utc", options.created_at_utc},
        {"updated_at_utc", options.updated_at_utc},
        {"recording_folder", options.recording_folder},
        {"mode", "rolling_clips"},
        {"status", options.status},
        {"cameras", cameras},
        {"stream",
         {
             {"requested_duration_seconds", options.requested_stream_duration_seconds},
             {"stream_start_delay_seconds", options.stream_start_delay_seconds},
             {"started_at_utc", options.stream_started_at_utc},
             {"finished_at_utc", options.stream_finished_at_utc},
             {"actual_elapsed_s", options.stream_actual_elapsed_s},
             {"interrupted", options.stream_interrupted}
         }},
        {"recording_control", build_recording_control_json(options.recording_control)},
        {"recording",
         {
             {"started", options.recording_started},
             {"started_at_utc", options.recording_started_at_utc},
             {"started_at_elapsed_s", options.recording_started_at_elapsed_s},
             {"stop_requested", options.recording_stop_requested},
             {"stop_requested_at_utc", options.recording_stop_requested_at_utc},
             {"stop_requested_at_elapsed_s", options.recording_stop_requested_at_elapsed_s},
             {"stop_reason", options.recording_stop_reason},
             {"drain_completed", options.recording_drain_completed},
             {"drained_at_utc", options.recording_drained_at_utc},
             {"drained_at_elapsed_s", options.recording_drained_at_elapsed_s},
             {"actual_recording_duration_s", options.actual_recording_duration_s},
             {"sum_clip_actual_duration_s", sum_clip_actual_duration_s},
             {"drain_duration_s", options.drain_duration_s}
        }},
        {"rollover",
         {
             {"implementation", options.rollover_implementation},
             {"seamless_writer_switch", true},
             {"records_during_rollover", true},
             {"boundary", "gop_first_frame_id"},
             {"next_writer_preopened", options.rollover_next_writer_preopened}
         }},
        {"clips", clips}
    };
    if (options.recording_backend.is_object() && !options.recording_backend.empty()) {
        manifest["recording_backend"] = options.recording_backend;
    }
    return manifest;
}

bool write_recording_session_manifest(const std::string& path,
                                      const nlohmann::json& manifest,
                                      std::string* error_out)
{
    if (path.empty()) {
        if (error_out) {
            *error_out = "recording session manifest path is empty";
        }
        return false;
    }
    return write_json_file(std::filesystem::path(path), manifest, error_out);
}

void create_recording_pipelines_for_stream(RecordingSessionState* state,
                                           CameraParams* cameras_params,
                                           CameraEachSelect* cameras_select,
                                           const int num_cameras,
                                           const EncoderConfig& encoder_config,
                                           CameraResources* camera_resources,
                                           CameraControl* camera_control,
                                           const AppStorageConfig* app_storage_config)
{
    if (!state || !cameras_params || !cameras_select || !camera_resources || !camera_control || num_cameras <= 0) {
        return;
    }

    state->recording_pipelines.clear();
    state->recording_pipelines.resize(num_cameras);
    state->recording_sink_mode = resolve_gui_recording_sink_mode(app_storage_config);
    state->external_recorder_contract_config =
        resolve_gui_external_recorder_contract_config(
            app_storage_config,
            &state->external_recorder_contract_source,
            &state->external_recorder_config_status);
    if (!state->external_recorder_config_status.empty()) {
        std::cerr << "[recording_session] "
                  << state->external_recorder_config_status << std::endl;
    }

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
                                            const CameraEachSelect* cameras_select,
                                            const int num_cameras,
                                            const std::string& base_folder,
                                            PTPParams* ptp_params,
                                            const std::string& recording_sink_mode,
                                            const nlohmann::json* external_recorder_contract_config)
{
    RecordingRunStartResult result;
    if (!camera_control || !cameras_params || num_cameras <= 0) {
        return result;
    }

    camera_control->recording_draining = false;
    camera_control->stop_record = false;
    camera_control->latest_recording_frame_id.store(0, std::memory_order_relaxed);
    {
        std::lock_guard<std::mutex> lock(camera_control->recording_folder_mutex);
        camera_control->pending_recording_output_folder.clear();
        camera_control->recording_rollover_at_frame_id = 0;
        camera_control->recording_rollover_request_id = 0;
        camera_control->recording_rollover_completed_request_id = 0;
        camera_control->recording_rollover_completed_frame_id = 0;
        camera_control->recording_rollover_completed_folder.clear();
    }

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

    const std::string normalized_sink_mode = normalize_recording_sink_mode(recording_sink_mode);
    const bool external_recorder_requested = normalized_sink_mode == "external_ipc";

    make_folder(recording_folder);
    write_recording_snapshot(
        recording_folder,
        recording_id,
        cameras_params,
        num_cameras,
        resolved_base_folder,
        !external_recorder_requested,
        camera_control->sync_camera,
        ptp_params,
        normalized_sink_mode.empty() ? recording_sink_mode : normalized_sink_mode);
    initialize_ptp_sync_summary(
        recording_folder,
        recording_id,
        num_cameras,
        camera_control->sync_camera,
        ptp_params);

    if (external_recorder_requested) {
        orange::external_recorder::CameraContractMaterializationInput contract_input;
        contract_input.contract_config = external_recorder_contract_config;
        contract_input.recording_folder = recording_folder;
        contract_input.recording_id = recording_id;
        contract_input.cameras_params = cameras_params;
        contract_input.cameras_select = cameras_select;
        contract_input.num_cameras = num_cameras;
        const nlohmann::json contract =
            orange::external_recorder::MaterializeExternalRecorderContractForCameras(
                contract_input);

        orange::external_recorder::FailFastArtifactOptions artifact_options;
        artifact_options.recording_folder = recording_folder;
        artifact_options.recording_id = recording_id;
        artifact_options.producer = "orange_gui";
        artifact_options.reason =
            orange::external_recorder::kGuiExternalRecorderNotImplementedReason;
        artifact_options.contract = contract;
        const orange::external_recorder::FailFastArtifactResult artifact_result =
            orange::external_recorder::WriteExternalRecorderFailFastArtifacts(
                artifact_options);
        result.ok = false;
        result.recording_folder = recording_folder;
        result.recording_sink_mode = normalized_sink_mode;
        result.error_message = artifact_result.error_message.empty()
            ? orange::external_recorder::kGuiExternalRecorderNotImplementedReason
            : artifact_result.error_message;
        result.external_recorder_contract_path =
            artifact_result.external_recorder_contract_path;
        result.external_recorder_supervisor_plan_path =
            artifact_result.external_recorder_supervisor_plan_path;
        {
            std::lock_guard<std::mutex> lock(camera_control->recording_folder_mutex);
            if (camera_control->recording_folder == recording_folder) {
                camera_control->recording_folder.clear();
            }
        }
        camera_control->record_video = false;
        camera_control->recording_draining = false;
        camera_control->stop_record = false;
        std::cerr << "[recording_session] " << result.error_message
                  << ". Wrote intended external recorder contract to "
                  << result.external_recorder_contract_path << std::endl;
        return result;
    }

    camera_control->record_video = true;
    result.ok = true;
    result.recording_folder = std::move(recording_folder);
    result.recording_sink_mode = normalized_sink_mode.empty() ? recording_sink_mode : normalized_sink_mode;
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
    {
        std::lock_guard<std::mutex> lock(camera_control->recording_folder_mutex);
        camera_control->pending_recording_output_folder.clear();
        camera_control->recording_rollover_at_frame_id = 0;
        camera_control->recording_rollover_request_id = 0;
    }
    if (camera_control->active_recorders.load(std::memory_order_relaxed) == 0) {
        camera_control->recording_draining = false;
        camera_control->stop_record = false;
        std::lock_guard<std::mutex> lock(camera_control->recording_folder_mutex);
        camera_control->recording_folder.clear();
    }
}

void request_drain_recording_run(RecordingSessionState* state, CameraControl* camera_control)
{
    request_stop_recording_run(camera_control);
    if (!state) {
        return;
    }
    for (auto& pipeline : state->recording_pipelines) {
        if (pipeline) {
            pipeline->request_recording_drain();
        }
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
