#include "session/recording_session.h"

#include "external_recorder_supervisor.h"
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

constexpr const char* kGuiExternalRecorderNotImplementedReason =
    "external recorder GUI supervision is not implemented yet; use headless supervised spec or in-process recording";

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

bool write_json_file(const std::filesystem::path& path,
                     const nlohmann::json& payload,
                     std::string* error_out)
{
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    if (ec) {
        if (error_out) {
            *error_out = "failed to create directory for " + path.string() + ": " + ec.message();
        }
        return false;
    }

    std::ofstream output(path);
    if (!output.is_open()) {
        if (error_out) {
            *error_out = "failed to open " + path.string() + " for writing";
        }
        return false;
    }
    output << payload.dump(2) << "\n";
    if (!output.good()) {
        if (error_out) {
            *error_out = "failed to write " + path.string();
        }
        return false;
    }
    return true;
}

bool read_json_file(const std::filesystem::path& path,
                    nlohmann::json* payload_out,
                    std::string* error_out)
{
    if (!payload_out) {
        if (error_out) {
            *error_out = "internal error: null JSON destination";
        }
        return false;
    }
    std::ifstream input(path);
    if (!input.is_open()) {
        if (error_out) {
            *error_out = "failed to open " + path.string();
        }
        return false;
    }
    try {
        input >> *payload_out;
    } catch (const std::exception& ex) {
        if (error_out) {
            *error_out = "failed to parse " + path.string() + ": " + ex.what();
        }
        return false;
    }
    return true;
}

std::string replace_all(std::string value,
                        const std::string& needle,
                        const std::string& replacement)
{
    if (needle.empty()) {
        return value;
    }
    size_t pos = 0;
    while ((pos = value.find(needle, pos)) != std::string::npos) {
        value.replace(pos, needle.size(), replacement);
        pos += replacement.size();
    }
    return value;
}

std::string expand_gui_external_recorder_path_template(std::string value,
                                                       const std::string& recording_folder,
                                                       const std::string& recording_id)
{
    value = replace_all(std::move(value), "{recording_folder}", recording_folder);
    value = replace_all(std::move(value), "{recording_id}", recording_id);
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

nlohmann::json extract_external_recorder_contract_object(const nlohmann::json& payload)
{
    if (payload.is_object() &&
        payload.contains("external_recorder_contract") &&
        payload["external_recorder_contract"].is_object()) {
        return payload["external_recorder_contract"];
    }
    return payload.is_object() ? payload : nlohmann::json::object();
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
        contract = app_storage_config->gui_external_recorder_contract;
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
        nlohmann::json loaded;
        std::string error;
        if (read_json_file(contract_path, &loaded, &error)) {
            contract = extract_external_recorder_contract_object(loaded);
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

void set_json_default(nlohmann::json* object, const char* key, nlohmann::json value)
{
    if (!object || !object->is_object() || object->contains(key)) {
        return;
    }
    (*object)[key] = std::move(value);
}

std::vector<int> default_gui_external_recorder_shards(const CameraParams& camera_params)
{
    std::vector<int> shards;
    if (camera_params.recording.strategy.split_gop_enabled()) {
        for (int gpu_id : camera_params.recording.strategy.split_gop.encoder_gpu_ids) {
            if (gpu_id >= 0 && std::find(shards.begin(), shards.end(), gpu_id) == shards.end()) {
                shards.push_back(gpu_id);
            }
        }
    }
    if (shards.empty() && camera_params.gpu_id >= 0) {
        shards.push_back(camera_params.gpu_id);
    }
    return shards;
}

std::vector<int> json_gpu_id_array_or_default(const nlohmann::json& object,
                                              const char* key,
                                              std::vector<int> fallback)
{
    if (!object.is_object() || !object.contains(key) || !object[key].is_array()) {
        return fallback;
    }
    std::vector<int> gpu_ids;
    for (const auto& value : object[key]) {
        if (!value.is_number_integer()) {
            continue;
        }
        const int gpu_id = value.get<int>();
        if (gpu_id >= 0 && std::find(gpu_ids.begin(), gpu_ids.end(), gpu_id) == gpu_ids.end()) {
            gpu_ids.push_back(gpu_id);
        }
    }
    return gpu_ids.empty() ? fallback : gpu_ids;
}

uint64_t gui_frame_bytes(const CameraParams& camera_params)
{
    return static_cast<uint64_t>(camera_params.width) *
           static_cast<uint64_t>(camera_params.height);
}

nlohmann::json materialize_gui_external_recorder_contract(
    const nlohmann::json* contract_config,
    const std::string& recording_folder,
    const std::string& recording_id,
    const CameraParams* cameras_params,
    const CameraEachSelect* cameras_select,
    const int num_cameras)
{
    nlohmann::json contract =
        (contract_config && contract_config->is_object()) ? *contract_config : nlohmann::json::object();
    contract["schema_id"] = "orange.external_recorder.contract";
    contract["schema_version"] = 1;
    contract["mode"] = "diagnostic_ipc_v1";
    contract["supervise_processes"] = true;
    set_json_default(&contract, "session_id", recording_id);
    set_json_default(
        &contract,
        "artifact_root",
        (std::filesystem::path(recording_folder) / "external_recorder").string());
    set_json_default(&contract, "require_summary", true);
    set_json_default(&contract, "require_video_sanity", true);
    set_json_default(&contract, "require_merged_mp4", true);
    set_json_default(&contract, "require_gop_routing", true);

    contract["artifact_root"] = expand_gui_external_recorder_path_template(
        contract.value("artifact_root", std::string()),
        recording_folder,
        recording_id);
    const std::string artifact_root = contract.value("artifact_root", std::string());

    nlohmann::json streams =
        contract.contains("streams") && contract["streams"].is_object()
            ? contract["streams"]
            : nlohmann::json::object();

    for (int i = 0; i < num_cameras; ++i) {
        if (cameras_select && !cameras_select[i].record) {
            continue;
        }
        const CameraParams& camera = cameras_params[i];
        const std::string serial = camera.camera_serial;
        if (serial.empty()) {
            continue;
        }

        nlohmann::json stream =
            streams.contains(serial) && streams[serial].is_object()
                ? streams[serial]
                : nlohmann::json::object();

        const std::vector<int> shard_gpu_ids = json_gpu_id_array_or_default(
            stream,
            "expected_shard_gpu_ids",
            default_gui_external_recorder_shards(camera));
        const int recorder_gpu_id =
            !shard_gpu_ids.empty() ? shard_gpu_ids.front() : camera.gpu_id;
        const bool multi_shard = shard_gpu_ids.size() > 1;
        const std::string prefix =
            (std::filesystem::path(artifact_root) / ("Cam" + serial + "_external")).string();

        set_json_default(&stream, "stream_id", serial);
        set_json_default(&stream, "camera_serial", serial);
        set_json_default(&stream, "analytics_gpu_id", camera.gpu_id);
        set_json_default(&stream, "recorder_gpu_id", recorder_gpu_id);
        set_json_default(&stream, "expected_shard_gpu_ids", shard_gpu_ids);
        set_json_default(&stream, "routing_policy", multi_shard ? "gop_modulo" : "single_shard");
        set_json_default(&stream, "summary_json", prefix + "_summary.json");
        set_json_default(&stream, "video_sanity_json", prefix + "_video_sanity.json");
        set_json_default(&stream, "mp4", prefix + ".mp4");
        set_json_default(&stream, "gop_routing_csv", prefix + "_gop_routing.csv");
        set_json_default(&stream, "socket_path", "/tmp/orange_external_recorder_" + serial + ".sock");
        const int camera_frame_rate = static_cast<int>(camera.frame_rate);
        set_json_default(&stream, "encode_fps", std::max(1, camera_frame_rate));
        set_json_default(&stream, "encode_max_fps", 0);
        set_json_default(&stream, "encode_queue_depth", 32);
        set_json_default(&stream, "prewarm_slots", 4);
        set_json_default(&stream, "prewarm_bytes", gui_frame_bytes(camera));
        set_json_default(&stream, "prewarm_peer_copy", true);
        set_json_default(&stream, "codec", camera.recording.encode.codec);
        set_json_default(&stream, "preset", camera.recording.encode.preset);
        set_json_default(&stream, "tuning", camera.recording.encode.tuning);
        set_json_default(&stream, "gop", camera.recording.encode.gop_length > 0
                                     ? camera.recording.encode.gop_length
                                     : std::max(1, camera_frame_rate));
        set_json_default(&stream, "bitrate_bps", 150000000);
        set_json_default(&stream, "max_bitrate_bps", 150000000);
        set_json_default(&stream, "vbv_buffer_size", 150000000);

        for (const char* key : {
                 "summary_json",
                 "video_sanity_json",
                 "mp4",
                 "gop_routing_csv",
                 "socket_path"}) {
            if (stream.contains(key) && stream[key].is_string()) {
                stream[key] = expand_gui_external_recorder_path_template(
                    stream[key].get<std::string>(),
                    recording_folder,
                    recording_id);
            }
        }
        streams[serial] = std::move(stream);
    }

    contract["streams"] = std::move(streams);
    return contract;
}

bool write_gui_external_recorder_failfast_artifacts(
    const std::string& recording_folder,
    const std::string& recording_id,
    const nlohmann::json& contract,
    RecordingRunStartResult* result)
{
    if (!result) {
        return false;
    }

    const std::filesystem::path folder(recording_folder);
    const std::filesystem::path contract_path =
        folder / "external_recorder_contract.json";
    const std::filesystem::path plan_path =
        folder / "external_recorder_supervisor_plan.json";
    const std::filesystem::path session_path =
        folder / "recording_session.json";

    std::string error;
    if (!write_json_file(contract_path, contract, &error)) {
        result->error_message =
            std::string(kGuiExternalRecorderNotImplementedReason) +
            "; additionally failed to write external recorder contract: " + error;
        return false;
    }
    result->external_recorder_contract_path = contract_path.string();

    orange::external_recorder::SupervisorPlanOptions plan_options;
    plan_options.default_session_id = recording_id;
    orange::external_recorder::SupervisorPlan plan;
    if (orange::external_recorder::BuildSupervisorPlanFromContract(
            contract,
            plan_options,
            &plan,
            &error)) {
        if (write_json_file(
                plan_path,
                orange::external_recorder::SupervisorPlanToJson(plan),
                &error)) {
            result->external_recorder_supervisor_plan_path = plan_path.string();
        }
    }
    if (result->external_recorder_supervisor_plan_path.empty() && !error.empty()) {
        std::cerr << "[recording_session] GUI external recorder plan validation failed: "
                  << error << std::endl;
    }

    nlohmann::json session_manifest = {
        {"schema_id", "orange.recording_session"},
        {"schema_version", 1},
        {"producer", "orange_gui"},
        {"recording_id", recording_id},
        {"recording_folder", recording_folder},
        {"status", "failed"},
        {"reason", kGuiExternalRecorderNotImplementedReason},
        {"recording_backend", {
            {"mode", "external_ipc"},
            {"status", "not_implemented"},
            {"external_recorder_contract_path", result->external_recorder_contract_path},
            {"external_recorder_supervisor_plan_path", result->external_recorder_supervisor_plan_path}
        }}
    };
    if (!write_json_file(session_path, session_manifest, &error)) {
        std::cerr << "[recording_session] Failed to write GUI recording session manifest: "
                  << error << std::endl;
    }
    return true;
}

}  // namespace

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
        const nlohmann::json contract = materialize_gui_external_recorder_contract(
            external_recorder_contract_config,
            recording_folder,
            recording_id,
            cameras_params,
            cameras_select,
            num_cameras);
        write_gui_external_recorder_failfast_artifacts(
            recording_folder,
            recording_id,
            contract,
            &result);
        result.ok = false;
        result.recording_folder = recording_folder;
        result.recording_sink_mode = normalized_sink_mode;
        if (result.error_message.empty()) {
            result.error_message = kGuiExternalRecorderNotImplementedReason;
        }
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
