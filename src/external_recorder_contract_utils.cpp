#include "external_recorder_contract_utils.h"

#include "external_recorder_supervisor.h"
#include "video_capture.h"

#include <algorithm>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <utility>
#include <vector>

namespace orange::external_recorder {
namespace {

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

std::string expand_path_template(std::string value,
                                 const std::string& recording_folder,
                                 const std::string& recording_id)
{
    value = replace_all(std::move(value), "{recording_folder}", recording_folder);
    value = replace_all(std::move(value), "{recording_id}", recording_id);
    return value;
}

void set_json_default(nlohmann::json* object, const char* key, nlohmann::json value)
{
    if (!object || !object->is_object() || object->contains(key)) {
        return;
    }
    (*object)[key] = std::move(value);
}

std::vector<int> default_external_recorder_shards(const CameraParams& camera_params)
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

uint64_t frame_bytes(const CameraParams& camera_params)
{
    return static_cast<uint64_t>(camera_params.width) *
           static_cast<uint64_t>(camera_params.height);
}

}  // namespace

nlohmann::json ExtractExternalRecorderContractObject(const nlohmann::json& payload)
{
    if (payload.is_object() &&
        payload.contains("external_recorder_contract") &&
        payload["external_recorder_contract"].is_object()) {
        return payload["external_recorder_contract"];
    }
    return payload.is_object() ? payload : nlohmann::json::object();
}

bool ReadExternalRecorderContractConfigFile(const std::string& path,
                                            nlohmann::json* contract_out,
                                            std::string* error_out)
{
    nlohmann::json loaded;
    if (!read_json_file(path, &loaded, error_out)) {
        return false;
    }
    if (contract_out) {
        *contract_out = ExtractExternalRecorderContractObject(loaded);
    }
    return true;
}

nlohmann::json MaterializeExternalRecorderContractForCameras(
    const CameraContractMaterializationInput& input)
{
    nlohmann::json contract =
        (input.contract_config && input.contract_config->is_object())
            ? *input.contract_config
            : nlohmann::json::object();
    contract["schema_id"] = "orange.external_recorder.contract";
    contract["schema_version"] = 1;
    contract["mode"] = "diagnostic_ipc_v1";
    contract["supervise_processes"] = true;
    set_json_default(&contract, "session_id", input.recording_id);
    set_json_default(
        &contract,
        "artifact_root",
        (std::filesystem::path(input.recording_folder) / "external_recorder").string());
    set_json_default(&contract, "require_summary", true);
    set_json_default(&contract, "require_video_sanity", true);
    set_json_default(&contract, "require_merged_mp4", true);
    set_json_default(&contract, "require_gop_routing", true);

    contract["artifact_root"] = expand_path_template(
        contract.value("artifact_root", std::string()),
        input.recording_folder,
        input.recording_id);
    const std::string artifact_root = contract.value("artifact_root", std::string());

    nlohmann::json streams =
        contract.contains("streams") && contract["streams"].is_object()
            ? contract["streams"]
            : nlohmann::json::object();

    if (!input.cameras_params || input.num_cameras <= 0) {
        contract["streams"] = std::move(streams);
        return contract;
    }

    for (int i = 0; i < input.num_cameras; ++i) {
        if (input.cameras_select && !input.cameras_select[i].record) {
            continue;
        }
        const CameraParams& camera = input.cameras_params[i];
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
            default_external_recorder_shards(camera));
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
        set_json_default(&stream, "prewarm_bytes", frame_bytes(camera));
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
                stream[key] = expand_path_template(
                    stream[key].get<std::string>(),
                    input.recording_folder,
                    input.recording_id);
            }
        }
        streams[serial] = std::move(stream);
    }

    contract["streams"] = std::move(streams);
    return contract;
}

FailFastArtifactResult WriteExternalRecorderFailFastArtifacts(
    const FailFastArtifactOptions& options)
{
    FailFastArtifactResult result;
    const std::filesystem::path folder(options.recording_folder);
    const std::filesystem::path contract_path =
        folder / "external_recorder_contract.json";
    const std::filesystem::path plan_path =
        folder / "external_recorder_supervisor_plan.json";
    const std::filesystem::path session_path =
        folder / "recording_session.json";

    std::string error;
    if (!write_json_file(contract_path, options.contract, &error)) {
        result.error_message =
            options.reason +
            "; additionally failed to write external recorder contract: " + error;
        return result;
    }
    result.external_recorder_contract_path = contract_path.string();

    SupervisorPlanOptions plan_options;
    plan_options.default_session_id = options.recording_id;
    SupervisorPlan plan;
    if (BuildSupervisorPlanFromContract(
            options.contract,
            plan_options,
            &plan,
            &error)) {
        if (write_json_file(
                plan_path,
                SupervisorPlanToJson(plan),
                &error)) {
            result.external_recorder_supervisor_plan_path = plan_path.string();
        }
    }
    if (result.external_recorder_supervisor_plan_path.empty() && !error.empty()) {
        std::cerr << "[external_recorder_contract] Supervisor plan validation failed: "
                  << error << std::endl;
    }

    nlohmann::json session_manifest = {
        {"schema_id", "orange.recording_session"},
        {"schema_version", 1},
        {"producer", options.producer},
        {"recording_id", options.recording_id},
        {"recording_folder", options.recording_folder},
        {"status", "failed"},
        {"reason", options.reason},
        {"recording_backend", {
            {"mode", "external_ipc"},
            {"status", "not_implemented"},
            {"external_recorder_contract_path", result.external_recorder_contract_path},
            {"external_recorder_supervisor_plan_path", result.external_recorder_supervisor_plan_path}
        }}
    };
    if (write_json_file(session_path, session_manifest, &error)) {
        result.recording_session_path = session_path.string();
    } else {
        std::cerr << "[external_recorder_contract] Failed to write recording session manifest: "
                  << error << std::endl;
    }

    result.ok = true;
    result.error_message = options.reason;
    return result;
}

}  // namespace orange::external_recorder
