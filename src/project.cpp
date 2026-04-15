// src/project.cpp
#include "project.h"
#include "fsuid_guard.h"
#include <unistd.h>      // For gethostname in client_send_bringup_message
#include <sys/stat.h>    // For mkdir
#include <iostream>
#include <fstream>       // For std::ifstream
#include <filesystem>    // For std::filesystem
#include <algorithm>     // For std::sort, std::find_if
#include <numeric>       // For std::iota (if used, though it's in camera.cpp sort_indexes)
#include <iomanip>       // For std::put_time, std::setfill, std::setw
#include <sstream>       // For std::ostringstream
#include <ctime>         // For std::gmtime
#include <cctype>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <map>
#include <utility>
#include <mutex>
#include <set>
#include <cuda_runtime.h>
#include "json.hpp"      // For nlohmann::json
#include "fetch_generated.h" // For FetchGame:: enums and builders
#include "flatbuffers/flatbuffers.h" // For flatbuffers::FlatBufferBuilder

// --- Definitions of all functions previously in project.h ---

void prepare_application_folders(std::string orange_root_dir_str)
{
    orange::ScopedFsuid fsuid_guard;
    (void)fsuid_guard;
    // ... (implementation from project.h)
    std::string recordings_str = orange_root_dir_str + "/exp/unsorted";
    std::filesystem::path recordings_path(recordings_str);
    if (!std::filesystem::exists(recordings_path)) {
        if(std::filesystem::create_directories(recordings_path)) {
            std::cout << "Create recording folder..." << std::endl;
        }
    }

    std::string detect_str = orange_root_dir_str + "/detect";
    std::filesystem::path detect_path(detect_str);
    if (!std::filesystem::exists(detect_path)) {
        if(std::filesystem::create_directory(detect_path)) {
            std::cout << "Create detecting folder..." << std::endl;
        }
    }

    std::string config_local = orange_root_dir_str + "/config/local";
    std::filesystem::path config_local_path(config_local);
    if (!std::filesystem::exists(config_local_path)) {
        if(std::filesystem::create_directories(config_local_path)) {
            std::cout << "Create config/local folder..." << std::endl;
        }
    }

    std::string config_network = orange_root_dir_str + "/config/network";
    std::filesystem::path config_network_path(config_network);
    if (!std::filesystem::exists(config_network_path)) {
        if(std::filesystem::create_directory(config_network_path)) {
            std::cout << "Create config/network folder..." << std::endl;
        }
    }

    std::string picture_str = orange_root_dir_str + "/pictures";
    std::filesystem::path picture_path(picture_str);
    if (!std::filesystem::exists(picture_path)) {
        if(std::filesystem::create_directory(picture_path)) {
            std::cout << "Create picture folder..." << std::endl;
        }
    }

    std::string calibration_str = orange_root_dir_str + "/exp/calibration";
    std::filesystem::path calibration_path(calibration_str);
    if (!std::filesystem::exists(calibration_path)) {
        if(std::filesystem::create_directory(calibration_path)) {
            std::cout << "Create calibration folder..." << std::endl;
        }
    }

    std::string calibration_artifacts_str = orange_root_dir_str + "/calibrations/artifacts";
    std::filesystem::path calibration_artifacts_path(calibration_artifacts_str);
    if (!std::filesystem::exists(calibration_artifacts_path)) {
        if (std::filesystem::create_directories(calibration_artifacts_path)) {
            std::cout << "Create calibration artifacts folder..." << std::endl;
        }
    }
}

void intialize_servers(ConnectedServer* my_servers)
{
    // ... (implementation from project.h)
    my_servers[0].server_state = FetchGame::ManagerState_IDLE;
    my_servers[0].num_cameras = 0;
    my_servers[0].peer = nullptr;
    my_servers[0].ip_add[0] = 192;
    my_servers[0].ip_add[1] = 168;
    my_servers[0].ip_add[2] = 20;
    my_servers[0].ip_add[3] = 60;
    my_servers[0].port = 3333;
    my_servers[0].connected = false;
    strcpy(my_servers[0].name, "waffle-0");


    my_servers[1].server_state = FetchGame::ManagerState_IDLE;
    my_servers[1].num_cameras = 0;
    my_servers[1].peer = nullptr;
    my_servers[1].ip_add[0] = 192;
    my_servers[1].ip_add[1] = 168;
    my_servers[1].ip_add[2] = 20;
    my_servers[1].ip_add[3] = 61;
    my_servers[1].port = 3333;
    my_servers[1].connected = false;
    strcpy(my_servers[1].name, "waffle-1");
}

std::vector<std::string> string_split(std::string s, std::string delimiter) {
    // ... (implementation from project.h)
    size_t pos_start = 0, pos_end, delim_len = delimiter.length();
    std::string token;
    std::vector<std::string> res;

    while ((pos_end = s.find(delimiter, pos_start)) != std::string::npos) {
        token = s.substr (pos_start, pos_end - pos_start);
        pos_start = pos_end + delim_len;
        res.push_back (token);
    }

    res.push_back (s.substr (pos_start));
    return res;
}

nlohmann::json build_gpu_runtime_info(int gpu_id) {
    nlohmann::json info = nlohmann::json::object();
    info["id"] = gpu_id;

    if (gpu_id < 0) {
        info["lookup_error"] = "invalid gpu id";
        return info;
    }

    cudaDeviceProp props{};
    const cudaError_t props_status = cudaGetDeviceProperties(&props, gpu_id);
    if (props_status != cudaSuccess) {
        info["lookup_error"] = cudaGetErrorString(props_status);
        return info;
    }

    info["name"] = std::string(props.name);
    info["compute_capability"] = {
        {"major", props.major},
        {"minor", props.minor},
    };
    info["total_global_mem_bytes"] = static_cast<uint64_t>(props.totalGlobalMem);

    char pci_bus_id[32] = {0};
    const cudaError_t pci_status = cudaDeviceGetPCIBusId(pci_bus_id, sizeof(pci_bus_id), gpu_id);
    if (pci_status == cudaSuccess && pci_bus_id[0] != '\0') {
        info["pci_bus_id"] = std::string(pci_bus_id);
    } else if (pci_status != cudaSuccess) {
        info["pci_bus_id_lookup_error"] = cudaGetErrorString(pci_status);
    }

    return info;
}

namespace {

std::vector<std::string> split_whitespace_tokens(const std::string& line)
{
    std::istringstream iss(line);
    std::vector<std::string> tokens;
    std::string token;
    while (iss >> token) {
        tokens.push_back(token);
    }
    return tokens;
}

std::string strip_ansi_escape_sequences(const std::string& input)
{
    std::string output;
    output.reserve(input.size());

    for (std::size_t i = 0; i < input.size(); ++i) {
        const unsigned char ch = static_cast<unsigned char>(input[i]);
        if (ch == 0x1b && i + 1 < input.size() && input[i + 1] == '[') {
            i += 2;
            while (i < input.size()) {
                const unsigned char c = static_cast<unsigned char>(input[i]);
                if (c >= '@' && c <= '~') {
                    break;
                }
                ++i;
            }
            continue;
        }
        output.push_back(static_cast<char>(ch));
    }

    return output;
}

struct NvidiaSmiTopologyCache {
    bool success = false;
    std::string error;
    std::vector<std::string> gpu_headers;
    std::map<std::string, std::vector<std::string>> rows;
};

NvidiaSmiTopologyCache load_nvidia_smi_topology_cache()
{
    NvidiaSmiTopologyCache cache;
    FILE* pipe = popen("nvidia-smi topo -m 2>/dev/null", "r");
    if (!pipe) {
        cache.error = "failed to execute `nvidia-smi topo -m`";
        return cache;
    }

    char buffer[4096];
    while (fgets(buffer, sizeof(buffer), pipe) != nullptr) {
        const std::string clean_line = strip_ansi_escape_sequences(buffer);
        const std::vector<std::string> tokens = split_whitespace_tokens(clean_line);
        if (tokens.empty()) {
            continue;
        }

        if (cache.gpu_headers.empty() &&
            tokens.size() > 1 &&
            tokens.front().rfind("GPU", 0) == 0 &&
            tokens[1].rfind("GPU", 0) == 0) {
            for (const std::string& token : tokens) {
                if (token.rfind("GPU", 0) != 0) {
                    break;
                }
                cache.gpu_headers.push_back(token);
            }
            continue;
        }

        if (tokens.front().rfind("GPU", 0) != 0 || cache.gpu_headers.empty()) {
            continue;
        }

        if (tokens.size() < cache.gpu_headers.size() + 1) {
            continue;
        }

        std::vector<std::string> connections;
        connections.reserve(cache.gpu_headers.size());
        for (std::size_t i = 0; i < cache.gpu_headers.size(); ++i) {
            connections.push_back(tokens[i + 1]);
        }
        cache.rows[tokens.front()] = std::move(connections);
    }

    const int close_status = pclose(pipe);
    if (close_status != 0) {
        cache.error = "`nvidia-smi topo -m` exited with status " + std::to_string(close_status);
        return cache;
    }
    if (cache.gpu_headers.empty() || cache.rows.empty()) {
        cache.error = "`nvidia-smi topo -m` did not return a parseable GPU topology table";
        return cache;
    }

    cache.success = true;
    return cache;
}

const NvidiaSmiTopologyCache& get_nvidia_smi_topology_cache()
{
    static std::mutex cache_mutex;
    static bool initialized = false;
    static NvidiaSmiTopologyCache cache;

    std::lock_guard<std::mutex> lock(cache_mutex);
    if (!initialized) {
        cache = load_nvidia_smi_topology_cache();
        initialized = true;
    }
    return cache;
}

} // namespace

std::string lookup_nvidia_smi_topology_class(int source_gpu_id,
                                             int target_gpu_id,
                                             std::string* error_out)
{
    if (error_out) {
        error_out->clear();
    }
    if (source_gpu_id < 0 || target_gpu_id < 0) {
        if (error_out) {
            *error_out = "invalid GPU ids";
        }
        return "";
    }

    const NvidiaSmiTopologyCache& cache = get_nvidia_smi_topology_cache();
    if (!cache.success) {
        if (error_out) {
            *error_out = cache.error.empty() ? "topology cache unavailable" : cache.error;
        }
        return "";
    }

    const std::string row_key = "GPU" + std::to_string(source_gpu_id);
    const std::string col_key = "GPU" + std::to_string(target_gpu_id);
    const auto row_it = cache.rows.find(row_key);
    if (row_it == cache.rows.end()) {
        if (error_out) {
            *error_out = "row not found in `nvidia-smi topo -m`: " + row_key;
        }
        return "";
    }

    const auto header_it = std::find(cache.gpu_headers.begin(), cache.gpu_headers.end(), col_key);
    if (header_it == cache.gpu_headers.end()) {
        if (error_out) {
            *error_out = "column not found in `nvidia-smi topo -m`: " + col_key;
        }
        return "";
    }

    const std::size_t column_index = static_cast<std::size_t>(
        std::distance(cache.gpu_headers.begin(), header_it));
    if (column_index >= row_it->second.size()) {
        if (error_out) {
            *error_out = "topology matrix index out of range";
        }
        return "";
    }

    return row_it->second[column_index];
}

nlohmann::json build_gpu_copy_path_static_topology_info(int source_gpu_id, int target_gpu_id)
{
    nlohmann::json info = {
        {"source_gpu_id", source_gpu_id},
        {"target_gpu_id", target_gpu_id},
        {"source_gpu", build_gpu_runtime_info(source_gpu_id)},
        {"target_gpu", build_gpu_runtime_info(target_gpu_id)},
        {"same_gpu", source_gpu_id == target_gpu_id},
        {"copy_direction", "source_to_target"},
        {"peer_access_capability", {
            {"can_access_peer_query_direction", {
                {"accessing_gpu_id", target_gpu_id},
                {"peer_gpu_id", source_gpu_id}
            }}
        }}
    };

    std::string topology_error;
    const std::string topology_class =
        lookup_nvidia_smi_topology_class(source_gpu_id, target_gpu_id, &topology_error);
    if (!topology_class.empty()) {
        info["topology_class"] = topology_class;
    } else if (!topology_error.empty()) {
        info["topology_lookup_error"] = topology_error;
    }

    if (source_gpu_id < 0 || target_gpu_id < 0) {
        info["peer_access_capability"]["can_access_peer"] = false;
        info["peer_access_capability"]["peer_access_required"] = false;
        return info;
    }

    if (source_gpu_id == target_gpu_id) {
        info["peer_access_capability"]["can_access_peer"] = true;
        info["peer_access_capability"]["peer_access_required"] = false;
        return info;
    }

    int can_access_peer = 0;
    const cudaError_t peer_status =
        cudaDeviceCanAccessPeer(&can_access_peer, target_gpu_id, source_gpu_id);
    if (peer_status == cudaSuccess) {
        info["peer_access_capability"]["can_access_peer"] = (can_access_peer != 0);
    } else {
        info["peer_access_capability"]["can_access_peer"] = false;
        info["peer_access_capability"]["can_access_peer_lookup_error"] =
            cudaGetErrorString(peer_status);
    }
    info["peer_access_capability"]["peer_access_required"] = true;

    return info;
}

std::vector<std::string> string_split_char(char* string_c, std::string delimiter) {
    // ... (implementation from project.h)
    std::string s = std::string(string_c);
    size_t pos_start = 0, pos_end, delim_len = delimiter.length();
    std::string token;
    std::vector<std::string> res;

    while ((pos_end = s.find(delimiter, pos_start)) != std::string::npos) {
        token = s.substr (pos_start, pos_end - pos_start);
        pos_start = pos_end + delim_len;
        res.push_back (token);
    }

    res.push_back (s.substr (pos_start));
    return res;
}

namespace {
constexpr const char* kCameraConfigSchemaId = "orange.camera.config";
constexpr int kCameraConfigSchemaVersion = 3;

std::string lower_ascii_copy(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return value;
}

std::string normalize_camera_sync_mode_string(std::string value) {
    value = lower_ascii_copy(std::move(value));
    if (value == "ptp_gate" || value == "free_run" || value == "external_trigger" || value == "software_trigger") {
        return value;
    }
    return "free_run";
}

std::string normalize_camera_scan_type_string(std::string value) {
    value = lower_ascii_copy(std::move(value));
    if (value == "area_scan" || value == "line_scan" || value == "unknown") {
        return value;
    }
    return "unknown";
}

std::string normalize_gpio_connector_variant_string(std::string value) {
    value = lower_ascii_copy(std::move(value));
    if (value == "area_scan_12_pin" || value == "area_scan_8_pin" ||
        value == "line_scan_12_pin" || value == "unknown") {
        return value;
    }
    return "unknown";
}

std::string normalize_recording_mode_string(std::string value) {
    value = lower_ascii_copy(std::move(value));
    if (value == "single_session" || value == "split_gop") {
        return value;
    }
    return "single_session";
}

std::string normalize_split_gop_placement_string(std::string value) {
    value = lower_ascii_copy(std::move(value));
    if (value == "single_gpu" || value == "multi_gpu") {
        return value;
    }
    return "single_gpu";
}

std::string normalize_split_gop_source_encoder_policy_string(std::string value) {
    value = lower_ascii_copy(std::move(value));
    if (value == "local_only" || value == "hybrid_split" || value == "pure_offload") {
        return value;
    }
    return "local_only";
}

std::string normalize_split_gop_transfer_mode_string(std::string value) {
    value = lower_ascii_copy(std::move(value));
    if (value == "prepared") {
        return "prepared_nv12";
    }
    if (value == "auto" || value == "raw" || value == "prepared_nv12") {
        return value;
    }
    return "auto";
}

std::string normalize_recording_output_mode_string(std::string value) {
    value = lower_ascii_copy(std::move(value));
    if (value == "factor" || value == "resolution") {
        return value;
    }
    return "factor";
}

std::string normalize_preferred_topology_class_string(std::string value) {
    if (value.empty()) {
        return value;
    }
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::toupper(c));
    });
    return value;
}

bool try_get_nonnegative_u64(const nlohmann::json& object, const char* key, uint64_t* out_value) {
    if (!out_value || !object.contains(key)) {
        return false;
    }
    const nlohmann::json& value = object[key];
    if (value.is_number_unsigned()) {
        *out_value = value.get<uint64_t>();
        return true;
    }
    if (value.is_number_integer()) {
        const long long parsed = value.get<long long>();
        if (parsed < 0) {
            return false;
        }
        *out_value = static_cast<uint64_t>(parsed);
        return true;
    }
    return false;
}

bool try_get_nonnegative_int(const nlohmann::json& object, const char* key, int* out_value) {
    if (!out_value || !object.contains(key)) {
        return false;
    }
    const nlohmann::json& value = object[key];
    if (!value.is_number_integer() && !value.is_number_unsigned()) {
        return false;
    }
    const long long parsed = value.get<long long>();
    if (parsed < 0) {
        return false;
    }
    *out_value = static_cast<int>(parsed);
    return true;
}

void parse_split_gop_encoder_gpu_ids(const nlohmann::json& split_gop,
                                     std::vector<int>* encoder_gpu_ids_out) {
    if (!encoder_gpu_ids_out || !split_gop.contains("encoder_gpu_ids") ||
        !split_gop["encoder_gpu_ids"].is_array()) {
        return;
    }

    std::set<int> seen_gpu_ids;
    for (const auto& value : split_gop["encoder_gpu_ids"]) {
        if (!value.is_number_integer() && !value.is_number_unsigned()) {
            continue;
        }
        const long long parsed = value.get<long long>();
        if (parsed < 0) {
            continue;
        }
        const int gpu_id = static_cast<int>(parsed);
        if (seen_gpu_ids.insert(gpu_id).second) {
            encoder_gpu_ids_out->push_back(gpu_id);
        }
    }
}

void normalize_recording_strategy_config(RecordingStrategyConfig* config) {
    if (!config) {
        return;
    }

    config->requested_mode = normalize_recording_mode_string(
        config->requested_mode.empty() ? config->mode : config->requested_mode);
    config->mode = config->requested_mode;
    config->split_gop.placement = normalize_split_gop_placement_string(config->split_gop.placement);
    config->split_gop.source_encoder_policy =
        normalize_split_gop_source_encoder_policy_string(config->split_gop.source_encoder_policy);
    config->split_gop.transfer_mode =
        normalize_split_gop_transfer_mode_string(config->split_gop.transfer_mode);
    if (config->split_gop.encoder_gpu_ids.size() > 1) {
        config->split_gop.placement = "multi_gpu";
    }
    config->split_gop.enabled = config->mode == "split_gop";
}

void normalize_camera_recording_config(CameraRecordingConfig* config) {
    if (!config) {
        return;
    }

    config->encode.codec = lower_ascii_copy(config->encode.codec);
    config->encode.preset = lower_ascii_copy(config->encode.preset);
    config->encode.tuning = lower_ascii_copy(config->encode.tuning);
    config->encode.rate_control_mode = lower_ascii_copy(config->encode.rate_control_mode);
    config->output.mode = normalize_recording_output_mode_string(config->output.mode);
    config->constraints.preferred_topology_class =
        normalize_preferred_topology_class_string(config->constraints.preferred_topology_class);
    normalize_recording_strategy_config(&config->strategy);
}

bool parse_recording_strategy_json_object(const nlohmann::json& recording,
                                          RecordingStrategyConfig* recording_strategy_out,
                                          std::string* error_out) {
    if (error_out) {
        error_out->clear();
    }
    if (!recording_strategy_out) {
        if (error_out) {
            *error_out = "recording strategy destination is null";
        }
        return false;
    }
    if (!recording.is_object()) {
        if (error_out) {
            *error_out = "recording strategy must be a JSON object";
        }
        return false;
    }

    RecordingStrategyConfig recording_strategy;
    recording_strategy.requested_mode =
        normalize_recording_mode_string(recording.value("mode", recording_strategy.requested_mode));
    recording_strategy.mode = recording_strategy.requested_mode;
    if (recording.contains("split_gop") && recording["split_gop"].is_object()) {
        const nlohmann::json& split_gop = recording["split_gop"];
        recording_strategy.split_gop.placement =
            normalize_split_gop_placement_string(
                split_gop.value("placement", recording_strategy.split_gop.placement));
        recording_strategy.split_gop.source_encoder_policy =
            normalize_split_gop_source_encoder_policy_string(split_gop.value(
                "source_encoder_policy", recording_strategy.split_gop.source_encoder_policy));
        recording_strategy.split_gop.transfer_mode =
            normalize_split_gop_transfer_mode_string(
                split_gop.value("transfer_mode", recording_strategy.split_gop.transfer_mode));
        parse_split_gop_encoder_gpu_ids(
            split_gop, &recording_strategy.split_gop.encoder_gpu_ids);
        try_get_nonnegative_u64(
            split_gop, "max_inflight_gops",
            &recording_strategy.split_gop.max_inflight_gops);
        try_get_nonnegative_u64(
            split_gop, "max_buffered_bytes",
            &recording_strategy.split_gop.max_buffered_bytes);
        recording_strategy.split_gop.strict =
            split_gop.value("strict", recording_strategy.split_gop.strict);

        if (split_gop.contains("writer_queue") && split_gop["writer_queue"].is_object()) {
            const nlohmann::json& writer_queue = split_gop["writer_queue"];
            try_get_nonnegative_u64(
                writer_queue, "max_packets",
                &recording_strategy.split_gop.writer_queue.max_packets);
            try_get_nonnegative_u64(
                writer_queue, "max_bytes",
                &recording_strategy.split_gop.writer_queue.max_bytes);
            recording_strategy.split_gop.writer_queue.fail_on_overflow =
                writer_queue.value(
                    "fail_on_overflow",
                    recording_strategy.split_gop.writer_queue.fail_on_overflow);
        }
    }

    normalize_recording_strategy_config(&recording_strategy);
    *recording_strategy_out = std::move(recording_strategy);
    return true;
}

void parse_recording_config_from_json(const nlohmann::json& camera_config,
                                      CameraParams* camera_params) {
    if (!camera_params) {
        return;
    }

    camera_params->recording = CameraRecordingConfig();
    if (!camera_config.contains("recording") || !camera_config["recording"].is_object()) {
        return;
    }

    const nlohmann::json& recording = camera_config["recording"];
    camera_params->recording.profile_name =
        recording.value("profile_name", camera_params->recording.profile_name);

    if (recording.contains("encode") && recording["encode"].is_object()) {
        const nlohmann::json& encode = recording["encode"];
        camera_params->recording.encode.codec =
            lower_ascii_copy(encode.value("codec", camera_params->recording.encode.codec));
        camera_params->recording.encode.preset =
            lower_ascii_copy(encode.value("preset", camera_params->recording.encode.preset));
        camera_params->recording.encode.tuning =
            lower_ascii_copy(encode.value("tuning", camera_params->recording.encode.tuning));
        camera_params->recording.encode.rate_control_mode = lower_ascii_copy(
            encode.value("rate_control_mode", camera_params->recording.encode.rate_control_mode));
        camera_params->recording.encode.quality_value =
            encode.value("quality_value", camera_params->recording.encode.quality_value);
        camera_params->recording.encode.gop_length =
            encode.value("gop_length", camera_params->recording.encode.gop_length);
        camera_params->recording.encode.nvenc_direct_input =
            encode.value("nvenc_direct_input", camera_params->recording.encode.nvenc_direct_input);
    }

    if (recording.contains("output") && recording["output"].is_object()) {
        const nlohmann::json& output = recording["output"];
        camera_params->recording.output.mode =
            normalize_recording_output_mode_string(
                output.value("mode", camera_params->recording.output.mode));
        camera_params->recording.output.downsample_factor =
            output.value("downsample_factor", camera_params->recording.output.downsample_factor);
        camera_params->recording.output.requested_width =
            output.value("requested_width", camera_params->recording.output.requested_width);
        camera_params->recording.output.requested_height =
            output.value("requested_height", camera_params->recording.output.requested_height);
    }

    if (recording.contains("constraints") && recording["constraints"].is_object()) {
        const nlohmann::json& constraints = recording["constraints"];
        camera_params->recording.constraints.require_peer_access =
            constraints.value("require_peer_access", camera_params->recording.constraints.require_peer_access);
        camera_params->recording.constraints.preferred_topology_class =
            normalize_preferred_topology_class_string(constraints.value(
                "preferred_topology_class",
                camera_params->recording.constraints.preferred_topology_class));
    }

    if (recording.contains("resources") && recording["resources"].is_object()) {
        const nlohmann::json& resources = recording["resources"];
        try_get_nonnegative_int(
            resources, "acquire_work_entries",
            &camera_params->recording.resources.acquire_work_entries);
        try_get_nonnegative_int(
            resources, "encoder_entry_pool_size",
            &camera_params->recording.resources.encoder_entry_pool_size);
    }

    RecordingStrategyConfig parsed_strategy;
    if (parse_recording_strategy_json_object(recording, &parsed_strategy, nullptr)) {
        camera_params->recording.strategy = std::move(parsed_strategy);
    }

    normalize_camera_recording_config(&camera_params->recording);
}

nlohmann::json build_recording_strategy_json_object(const RecordingStrategyConfig& recording_strategy_in) {
    RecordingStrategyConfig recording_strategy = recording_strategy_in;
    normalize_recording_strategy_config(&recording_strategy);

    nlohmann::json recording = nlohmann::json::object();
    recording["mode"] = recording_strategy.requested_mode;
    recording["split_gop"] = {
        {"placement", recording_strategy.split_gop.placement},
        {"encoder_gpu_ids", recording_strategy.split_gop.encoder_gpu_ids},
        {"source_encoder_policy", recording_strategy.split_gop.source_encoder_policy},
        {"transfer_mode", recording_strategy.split_gop.transfer_mode},
        {"max_inflight_gops", recording_strategy.split_gop.max_inflight_gops},
        {"max_buffered_bytes", recording_strategy.split_gop.max_buffered_bytes},
        {"strict", recording_strategy.split_gop.strict},
        {"writer_queue", {
            {"max_packets", recording_strategy.split_gop.writer_queue.max_packets},
            {"max_bytes", recording_strategy.split_gop.writer_queue.max_bytes},
            {"fail_on_overflow", recording_strategy.split_gop.writer_queue.fail_on_overflow}
        }}
    };
    return recording;
}

nlohmann::json build_recording_config_json_from_params(const CameraParams& camera_params)
{
    CameraRecordingConfig recording = camera_params.recording;
    normalize_camera_recording_config(&recording);

    nlohmann::json recording_json = build_recording_strategy_json_object(recording.strategy);
    recording_json["profile_name"] = recording.profile_name;
    recording_json["encode"] = {
        {"codec", recording.encode.codec},
        {"preset", recording.encode.preset},
        {"tuning", recording.encode.tuning},
        {"rate_control_mode", recording.encode.rate_control_mode},
        {"quality_value", recording.encode.quality_value},
        {"gop_length", recording.encode.gop_length},
        {"nvenc_direct_input", recording.encode.nvenc_direct_input}
    };
    recording_json["output"] = {
        {"mode", recording.output.mode},
        {"downsample_factor", recording.output.downsample_factor},
        {"requested_width", recording.output.requested_width},
        {"requested_height", recording.output.requested_height}
    };
    recording_json["constraints"] = {
        {"require_peer_access", recording.constraints.require_peer_access},
        {"preferred_topology_class", recording.constraints.preferred_topology_class}
    };
    recording_json["resources"] = {
        {"acquire_work_entries", recording.resources.acquire_work_entries},
        {"encoder_entry_pool_size", recording.resources.encoder_entry_pool_size}
    };
    return recording_json;
}

std::string canonicalize_gpio_recipe_string(std::string value) {
    const std::string normalized = lower_ascii_copy(value);
    if (normalized == "area_scan_hw_trigger_internal_gpi4" ||
        normalized == "area_scan_hw_trigger_external_gpi4" ||
        normalized == "line_scan_hw_frame_gpi1_internal_line" ||
        normalized == "line_scan_hw_frame_gpi1_encoder_line" ||
        normalized == "line_scan_encoder_frame_encoder_line" ||
        normalized == "line_scan_hw_gate_gpi1_encoder_frame_encoder_line") {
        return normalized;
    }
    return value;
}

std::string canonicalize_camera_serial_string(std::string value) {
    value.erase(value.begin(), std::find_if(value.begin(), value.end(), [](unsigned char c) {
        return !std::isspace(c);
    }));
    value.erase(std::find_if(value.rbegin(), value.rend(), [](unsigned char c) {
        return !std::isspace(c);
    }).base(), value.end());

    if (value.empty()) {
        return value;
    }

    const bool is_numeric = std::all_of(value.begin(), value.end(), [](unsigned char c) {
        return std::isdigit(c) != 0;
    });
    if (!is_numeric) {
        return value;
    }

    const auto first_non_zero = value.find_first_not_of('0');
    if (first_non_zero == std::string::npos) {
        return "0";
    }
    return value.substr(first_non_zero);
}

bool contains_case_insensitive(const std::string& haystack, const std::string& needle) {
    return lower_ascii_copy(haystack).find(lower_ascii_copy(needle)) != std::string::npos;
}

bool starts_with_case_insensitive(const std::string& value, const std::string& prefix) {
    const std::string lower_value = lower_ascii_copy(value);
    const std::string lower_prefix = lower_ascii_copy(prefix);
    return lower_value.rfind(lower_prefix, 0) == 0;
}

bool model_is_area_scan_family(const std::string& model) {
    return starts_with_case_insensitive(model, "HB") ||
           starts_with_case_insensitive(model, "HZ") ||
           starts_with_case_insensitive(model, "HR") ||
           starts_with_case_insensitive(model, "HT") ||
           starts_with_case_insensitive(model, "HE");
}

bool model_is_line_scan_family(const std::string& model) {
    return starts_with_case_insensitive(model, "LB") ||
           starts_with_case_insensitive(model, "TLB") ||
           starts_with_case_insensitive(model, "LR") ||
           starts_with_case_insensitive(model, "TLR") ||
           starts_with_case_insensitive(model, "LT") ||
           starts_with_case_insensitive(model, "LZ") ||
           starts_with_case_insensitive(model, "TLZ");
}

bool model_uses_area_scan_8_pin_connector(const std::string& model) {
    return contains_case_insensitive(model, "eros") || starts_with_case_insensitive(model, "HE");
}

void infer_camera_gpio_metadata(CameraParams* camera_params) {
    if (!camera_params) {
        return;
    }

    if (camera_params->camera_scan_type == "unknown") {
        if (camera_params->device_model == "HB-65000GM" ||
            camera_params->device_model == "HB-65000GC" ||
            camera_params->device_model == "HB-7000SC" ||
            camera_params->device_model == "HB-7000SM") {
            camera_params->camera_scan_type = "area_scan";
        } else if (model_is_area_scan_family(camera_params->device_model)) {
            camera_params->camera_scan_type = "area_scan";
        } else if (model_is_line_scan_family(camera_params->device_model)) {
            camera_params->camera_scan_type = "line_scan";
        } else if (contains_case_insensitive(camera_params->device_model, "eros")) {
            camera_params->camera_scan_type = "area_scan";
        }
    }

    if (camera_params->gpio_connector_variant == "unknown") {
        if (camera_params->camera_scan_type == "line_scan") {
            camera_params->gpio_connector_variant = "line_scan_12_pin";
        } else if (camera_params->camera_scan_type == "area_scan") {
            if (model_uses_area_scan_8_pin_connector(camera_params->device_model)) {
                camera_params->gpio_connector_variant = "area_scan_8_pin";
            } else {
                camera_params->gpio_connector_variant = "area_scan_12_pin";
            }
        }
    }
}

void reset_camera_config_extensions(CameraParams* camera_params) {
    camera_params->config_schema_id.clear();
    camera_params->config_schema_version = 0;
    camera_params->device_model.clear();
    camera_params->camera_scan_type = "unknown";
    camera_params->gpio_connector_variant = "unknown";
    camera_params->gpio_recipe.clear();
    camera_params->sync_mode = "free_run";
    camera_params->trigger_enabled = false;
    camera_params->trigger_selector = "AcquisitionStart";
    camera_params->trigger_source = "Software";
    camera_params->trigger_activation = "RisingEdge";
    camera_params->ptp_mode.clear();
    camera_params->gpio_nodes.clear();
    camera_params->recording = CameraRecordingConfig();
}

void parse_gpio_nodes_from_json(const nlohmann::json& camera_config, CameraParams* camera_params) {
    if (!camera_config.contains("gpio")) {
        return;
    }

    const nlohmann::json* nodes_json = nullptr;
    const nlohmann::json& gpio = camera_config["gpio"];
    if (gpio.is_array()) {
        nodes_json = &gpio;
    } else if (gpio.is_object() && gpio.contains("nodes") && gpio["nodes"].is_array()) {
        nodes_json = &gpio["nodes"];
    }

    if (!nodes_json) {
        return;
    }

    for (const auto& node_json : *nodes_json) {
        if (!node_json.is_object()) {
            continue;
        }
        if (!node_json.contains("name") || !node_json["name"].is_string()) {
            continue;
        }

        CameraGpioNodeConfig node;
        node.name = node_json["name"].get<std::string>();
        node.type = lower_ascii_copy(node_json.value("type", std::string("enum")));

        if (!node_json.contains("value")) {
            std::cerr << "Skipping GPIO node without value: " << node.name << std::endl;
            continue;
        }

        const nlohmann::json& value = node_json["value"];
        if (node.type == "enum") {
            if (!value.is_string()) {
                std::cerr << "Skipping GPIO enum node with non-string value: " << node.name << std::endl;
                continue;
            }
            node.value_string = value.get<std::string>();
        } else if (node.type == "bool") {
            if (!value.is_boolean()) {
                std::cerr << "Skipping GPIO bool node with non-bool value: " << node.name << std::endl;
                continue;
            }
            node.value_bool = value.get<bool>();
        } else if (node.type == "uint") {
            if (!value.is_number_unsigned() && !value.is_number_integer()) {
                std::cerr << "Skipping GPIO uint node with non-integer value: " << node.name << std::endl;
                continue;
            }
            const auto parsed = value.get<long long>();
            if (parsed < 0) {
                std::cerr << "Skipping GPIO uint node with negative value: " << node.name << std::endl;
                continue;
            }
            node.value_uint = static_cast<uint32_t>(parsed);
        } else {
            std::cerr << "Skipping GPIO node with unsupported type `" << node.type
                      << "`: " << node.name << std::endl;
            continue;
        }

        camera_params->gpio_nodes.push_back(std::move(node));
    }
}

std::string config_filename_stem(const std::string& config_path) {
    return canonicalize_camera_serial_string(std::filesystem::path(config_path).stem().string());
}

std::string extract_config_serial_match_key(const std::string& config_path) {
    std::ifstream f(config_path);
    if (!f.good()) {
        return config_filename_stem(config_path);
    }

    try {
        nlohmann::json camera_config = nlohmann::json::parse(f);
        if (camera_config.contains("device_serial_number")) {
            const nlohmann::json& serial = camera_config["device_serial_number"];
            if (serial.is_string()) {
                const std::string value = serial.get<std::string>();
                if (!value.empty()) {
                    return canonicalize_camera_serial_string(value);
                }
            } else if (serial.is_number_integer() || serial.is_number_unsigned()) {
                return canonicalize_camera_serial_string(std::to_string(serial.get<long long>()));
            }
        }
    } catch (const std::exception& ex) {
        std::cerr << "Failed to parse camera config while matching serial: " << config_path
                  << " (" << ex.what() << ")" << std::endl;
    }

    return config_filename_stem(config_path);
}

std::vector<std::string>::const_iterator find_camera_config_for_serial(
    const std::vector<std::string>& camera_config_files,
    const std::string& camera_serial)
{
    const std::string canonical_camera_serial = canonicalize_camera_serial_string(camera_serial);
    return std::find_if(camera_config_files.begin(), camera_config_files.end(),
                        [&](const std::string& config_path) {
                            return extract_config_serial_match_key(config_path) == canonical_camera_serial;
                        });
}

bool camera_config_schema_version_supported(int schema_version) {
    return schema_version == 1 || schema_version == 2 || schema_version == 3;
}

bool try_parse_source_gpu_id_from_json(const nlohmann::json& camera_config,
                                       int* gpu_id_out,
                                       bool* used_legacy_gpu_id_out,
                                       std::string* error_out) {
    if (gpu_id_out) {
        *gpu_id_out = -1;
    }
    if (used_legacy_gpu_id_out) {
        *used_legacy_gpu_id_out = false;
    }
    if (error_out) {
        error_out->clear();
    }

    const bool has_source_gpu_id = camera_config.contains("source_gpu_id");
    const bool has_legacy_gpu_id = camera_config.contains("gpu_id");
    if (!has_source_gpu_id && !has_legacy_gpu_id) {
        if (error_out) {
            *error_out = "camera config missing source_gpu_id";
        }
        return false;
    }

    auto parse_gpu_value = [&](const nlohmann::json& value, int* parsed_out) -> bool {
        if (!parsed_out) {
            return false;
        }
        if (!value.is_number_integer() && !value.is_number_unsigned()) {
            return false;
        }
        const long long parsed = value.get<long long>();
        if (parsed < 0) {
            return false;
        }
        *parsed_out = static_cast<int>(parsed);
        return true;
    };

    int source_gpu_id = -1;
    int legacy_gpu_id = -1;
    if (has_source_gpu_id && !parse_gpu_value(camera_config["source_gpu_id"], &source_gpu_id)) {
        if (error_out) {
            *error_out = "camera config field `source_gpu_id` must be a non-negative integer";
        }
        return false;
    }
    if (has_legacy_gpu_id && !parse_gpu_value(camera_config["gpu_id"], &legacy_gpu_id)) {
        if (error_out) {
            *error_out = "camera config field `gpu_id` must be a non-negative integer";
        }
        return false;
    }
    if (has_source_gpu_id && has_legacy_gpu_id && source_gpu_id != legacy_gpu_id) {
        if (error_out) {
            *error_out = "`source_gpu_id` and legacy `gpu_id` disagree";
        }
        return false;
    }

    if (gpu_id_out) {
        *gpu_id_out = has_source_gpu_id ? source_gpu_id : legacy_gpu_id;
    }
    if (used_legacy_gpu_id_out) {
        *used_legacy_gpu_id_out = !has_source_gpu_id && has_legacy_gpu_id;
    }
    return true;
}

nlohmann::json build_camera_config_json_from_params(const CameraParams& camera_params)
{
    nlohmann::json camera_config = nlohmann::json::object();
    camera_config["name"] = camera_params.camera_name;
    camera_config["width"] = camera_params.width;
    camera_config["height"] = camera_params.height;
    camera_config["frame_rate"] = camera_params.frame_rate;
    camera_config["gain"] = camera_params.gain;
    camera_config["exposure"] = camera_params.exposure;
    camera_config["pixel_format"] = camera_params.pixel_format;
    camera_config["color_temp"] = camera_params.color_temp;
    camera_config["source_gpu_id"] = camera_params.gpu_id;
    camera_config["gpu_direct"] = camera_params.gpu_direct;
    camera_config["focus_uart_bootstrap"] = camera_params.focus_uart_bootstrap;
    camera_config["color"] = camera_params.color;
    camera_config["focus"] = camera_params.focus;
    camera_config["iris"] = camera_params.iris;
    camera_config["offset_x"] = camera_params.offsetx;
    camera_config["offset_y"] = camera_params.offsety;
    camera_config["schema_id"] = kCameraConfigSchemaId;
    camera_config["schema_version"] = kCameraConfigSchemaVersion;
    camera_config["device_model_name"] = camera_params.device_model;
    camera_config["device_serial_number"] = canonicalize_camera_serial_string(camera_params.camera_serial);
    camera_config["camera_scan_type"] = normalize_camera_scan_type_string(camera_params.camera_scan_type);
    camera_config["gpio_connector_variant"] =
        normalize_gpio_connector_variant_string(camera_params.gpio_connector_variant);
    camera_config["gpio_recipe"] = canonicalize_gpio_recipe_string(camera_params.gpio_recipe);
    camera_config["sync_mode"] = normalize_camera_sync_mode_string(camera_params.sync_mode);
    camera_config["trigger"] = {
        {"enabled", camera_params.trigger_enabled},
        {"selector", camera_params.trigger_selector},
        {"source", camera_params.trigger_source},
        {"activation", camera_params.trigger_activation}
    };
    camera_config["ptp"] = {
        {"enabled", camera_sync_mode_uses_ptp(&camera_params)}
    };
    if (camera_sync_mode_uses_ptp(&camera_params)) {
        camera_config["ptp"]["mode"] = camera_params.ptp_mode.empty() ? "TwoStep" : camera_params.ptp_mode;
    }

    nlohmann::json gpio_nodes = nlohmann::json::array();
    for (const auto& node : camera_params.gpio_nodes) {
        nlohmann::json node_json;
        node_json["name"] = node.name;
        node_json["type"] = lower_ascii_copy(node.type);
        if (lower_ascii_copy(node.type) == "enum") {
            node_json["value"] = node.value_string;
        } else if (lower_ascii_copy(node.type) == "bool") {
            node_json["value"] = node.value_bool;
        } else if (lower_ascii_copy(node.type) == "uint") {
            node_json["value"] = node.value_uint;
        } else {
            continue;
        }
        gpio_nodes.push_back(std::move(node_json));
    }
    camera_config["gpio"] = {
        {"nodes", std::move(gpio_nodes)}
    };
    camera_config["recording"] = build_recording_config_json_from_params(camera_params);
    return camera_config;
}

nlohmann::json build_camera_runtime_snapshot(const CameraParams& camera_params)
{
    nlohmann::json snapshot = nlohmann::json::object();
    snapshot["source"] = {
        {"camera_config_path", camera_params.config_path},
        {"configured_source_gpu_id", camera_params.configured_gpu_id},
        {"configured_gpu_id", camera_params.configured_gpu_id},
        {"gpu_id_runtime_overridden", camera_params.gpu_id_runtime_overridden}
    };
    snapshot["runtime"] = build_camera_config_json_from_params(camera_params);
    return snapshot;
}
} // namespace

void load_camera_json_config_files(std::string file_name, CameraParams* camera_params, int camera_id, int num_cameras) {
    // ... (implementation from project.h)
    std::ifstream f(file_name);
    nlohmann::json camera_config = nlohmann::json::parse(f);

    camera_params->camera_id = camera_id;
    camera_params->num_cameras = num_cameras;
    camera_params->need_reorder = false;
    reset_camera_config_extensions(camera_params);

    camera_params->camera_name = camera_config["name"];
    camera_params->width = camera_config["width"];
    camera_params->height = camera_config["height"];
    camera_params->frame_rate = camera_config["frame_rate"];
    camera_params->gain = camera_config["gain"];
    camera_params->exposure = camera_config["exposure"];
    camera_params->pixel_format = camera_config["pixel_format"];
    camera_params->color_temp = camera_config["color_temp"];
    std::string source_gpu_error;
    if (!try_parse_source_gpu_id_from_json(camera_config,
                                           &camera_params->configured_gpu_id,
                                           nullptr,
                                           &source_gpu_error)) {
        throw std::runtime_error("Invalid camera config `" + file_name + "`: " + source_gpu_error);
    }
    camera_params->gpu_id = camera_params->configured_gpu_id;
    camera_params->gpu_id_runtime_overridden = false;
    camera_params->gpu_direct = camera_config["gpu_direct"];
    camera_params->focus_uart_bootstrap = camera_config.value("focus_uart_bootstrap", false);
    camera_params->color = camera_config["color"];
    camera_params->focus = camera_config["focus"];
    camera_params->iris = camera_config["iris"];
    camera_params->offsetx = camera_config.value("offset_x", 0u);
    camera_params->offsety = camera_config.value("offset_y", 0u);

    camera_params->config_schema_id = camera_config.value("schema_id", std::string());
    camera_params->config_schema_version = camera_config.value("schema_version", 0);
    camera_params->device_model =
        camera_config.value("device_model",
            camera_config.value("device_model_name", camera_params->device_model));
    if (camera_params->camera_serial.empty()) {
        camera_params->camera_serial = canonicalize_camera_serial_string(
            camera_config.value("device_serial_number", camera_params->camera_serial));
    }
    camera_params->camera_scan_type =
        normalize_camera_scan_type_string(camera_config.value("camera_scan_type", camera_params->camera_scan_type));
    camera_params->gpio_connector_variant = normalize_gpio_connector_variant_string(
        camera_config.value("gpio_connector_variant", camera_params->gpio_connector_variant));
    camera_params->gpio_recipe = canonicalize_gpio_recipe_string(camera_config.value("gpio_recipe", std::string()));
    if (!camera_params->config_schema_id.empty() &&
        camera_params->config_schema_id != kCameraConfigSchemaId) {
        std::cerr << "Camera config schema_id mismatch for " << file_name
                  << ": " << camera_params->config_schema_id
                  << " (expected " << kCameraConfigSchemaId << ")" << std::endl;
    }
    if (camera_params->config_schema_version > 0 &&
        !camera_config_schema_version_supported(camera_params->config_schema_version)) {
        std::cerr << "Camera config schema_version mismatch for " << file_name
                  << ": " << camera_params->config_schema_version
                  << " (expected " << kCameraConfigSchemaVersion << ")" << std::endl;
    }

    if (camera_config.contains("sync_mode") && camera_config["sync_mode"].is_string()) {
        camera_params->sync_mode = normalize_camera_sync_mode_string(camera_config["sync_mode"].get<std::string>());
    }

    if (camera_config.contains("trigger") && camera_config["trigger"].is_object()) {
        const nlohmann::json& trigger = camera_config["trigger"];
        camera_params->trigger_enabled = trigger.value("enabled", false);
        camera_params->trigger_selector = trigger.value("selector", camera_params->trigger_selector);
        camera_params->trigger_source = trigger.value("source", camera_params->trigger_source);
        camera_params->trigger_activation = trigger.value("activation", camera_params->trigger_activation);
    }

    if (camera_config.contains("ptp") && camera_config["ptp"].is_object()) {
        const nlohmann::json& ptp = camera_config["ptp"];
        if (ptp.contains("mode") && ptp["mode"].is_string()) {
            camera_params->ptp_mode = ptp["mode"].get<std::string>();
        } else if (!ptp.value("enabled", false)) {
            camera_params->ptp_mode.clear();
        }
        if ((!camera_config.contains("sync_mode") || !camera_config["sync_mode"].is_string()) &&
            ptp.value("enabled", false)) {
            camera_params->sync_mode = "ptp_gate";
        }
    }

    parse_gpio_nodes_from_json(camera_config, camera_params);
    parse_recording_config_from_json(camera_config, camera_params);
    infer_camera_gpio_metadata(camera_params);
}

bool parse_recording_strategy_json(const nlohmann::json& recording_json,
                                   RecordingStrategyConfig* recording_strategy_out,
                                   std::string* error_out) {
    return parse_recording_strategy_json_object(recording_json, recording_strategy_out, error_out);
}

nlohmann::json build_recording_strategy_json(const RecordingStrategyConfig& recording_strategy) {
    return build_recording_strategy_json_object(recording_strategy);
}

bool runtime_env_is_set(const char* name) {
    const char* env = std::getenv(name);
    return env && *env != '\0';
}

bool parse_runtime_env_flag(const char* name, bool default_value) {
    const char* env = std::getenv(name);
    if (!env || *env == '\0') {
        return default_value;
    }
    if (strcmp(env, "0") == 0 || strcmp(env, "false") == 0 || strcmp(env, "FALSE") == 0 ||
        strcmp(env, "off") == 0 || strcmp(env, "OFF") == 0) {
        return false;
    }
    return true;
}

uint64_t parse_runtime_env_u64(const char* name, uint64_t default_value) {
    const char* env = std::getenv(name);
    if (!env || *env == '\0') {
        return default_value;
    }
    char* end = nullptr;
    const unsigned long long value = std::strtoull(env, &end, 10);
    if (end == env || (end && *end != '\0')) {
        return default_value;
    }
    return static_cast<uint64_t>(value);
}

std::string parse_runtime_env_string(const char* name, std::string default_value) {
    const char* env = std::getenv(name);
    if (!env || *env == '\0') {
        return default_value;
    }
    return std::string(env);
}

std::vector<int> parse_runtime_env_int_list(const char* name) {
    std::vector<int> values;
    const char* env = std::getenv(name);
    if (!env || *env == '\0') {
        return values;
    }

    std::stringstream ss(env);
    std::string token;
    while (std::getline(ss, token, ',')) {
        token.erase(std::remove_if(token.begin(), token.end(), [](unsigned char ch) {
            return std::isspace(ch) != 0;
        }), token.end());
        if (token.empty()) {
            continue;
        }
        char* end = nullptr;
        const long value = std::strtol(token.c_str(), &end, 10);
        if (end == token.c_str() || (end && *end != '\0')) {
            continue;
        }
        values.push_back(static_cast<int>(value));
    }
    return values;
}

void append_runtime_resolution_note(std::string* note, const std::string& message) {
    if (!note || message.empty()) {
        return;
    }
    if (!note->empty()) {
        *note += "; ";
    }
    *note += message;
}

void apply_runtime_recording_strategy_env_overrides(RecordingStrategyConfig* config) {
    if (!config) {
        return;
    }

    if (runtime_env_is_set("ORANGE_RECORDING_MODE") || runtime_env_is_set("ORANGE_GOP_EXPERIMENT")) {
        const bool legacy_split_gop_enabled = parse_runtime_env_flag("ORANGE_GOP_EXPERIMENT", false);
        config->requested_mode = normalize_recording_mode_string(parse_runtime_env_string(
            "ORANGE_RECORDING_MODE",
            legacy_split_gop_enabled ? "split_gop" : "single_session"));
    }

    if (runtime_env_is_set("ORANGE_SPLIT_GOP_SOURCE_ENCODER_POLICY") ||
        runtime_env_is_set("ORANGE_GOP_ENCODER_POLICY")) {
        config->split_gop.source_encoder_policy = normalize_split_gop_source_encoder_policy_string(
            parse_runtime_env_string(
                "ORANGE_SPLIT_GOP_SOURCE_ENCODER_POLICY",
                parse_runtime_env_string(
                    "ORANGE_GOP_ENCODER_POLICY",
                    config->split_gop.source_encoder_policy)));
    }

    if (runtime_env_is_set("ORANGE_SPLIT_GOP_TRANSFER_MODE") ||
        runtime_env_is_set("ORANGE_GOP_TRANSFER_MODE")) {
        config->split_gop.transfer_mode = normalize_split_gop_transfer_mode_string(
            parse_runtime_env_string(
                "ORANGE_SPLIT_GOP_TRANSFER_MODE",
                parse_runtime_env_string(
                    "ORANGE_GOP_TRANSFER_MODE",
                    config->split_gop.transfer_mode)));
    }

    if (runtime_env_is_set("ORANGE_SPLIT_GOP_PLACEMENT")) {
        config->split_gop.placement = normalize_split_gop_placement_string(
            parse_runtime_env_string("ORANGE_SPLIT_GOP_PLACEMENT", config->split_gop.placement));
    }

    if (runtime_env_is_set("ORANGE_SPLIT_GOP_ENCODER_GPU_IDS")) {
        config->split_gop.encoder_gpu_ids = parse_runtime_env_int_list("ORANGE_SPLIT_GOP_ENCODER_GPU_IDS");
    }

    if (runtime_env_is_set("ORANGE_SPLIT_GOP_MAX_INFLIGHT_GOPS") ||
        runtime_env_is_set("ORANGE_GOP_MAX_INFLIGHT_GOPS")) {
        config->split_gop.max_inflight_gops = parse_runtime_env_u64(
            "ORANGE_SPLIT_GOP_MAX_INFLIGHT_GOPS",
            parse_runtime_env_u64(
                "ORANGE_GOP_MAX_INFLIGHT_GOPS",
                config->split_gop.max_inflight_gops));
    }

    if (runtime_env_is_set("ORANGE_SPLIT_GOP_MAX_BUFFERED_BYTES") ||
        runtime_env_is_set("ORANGE_GOP_MAX_BUFFERED_BYTES")) {
        config->split_gop.max_buffered_bytes = parse_runtime_env_u64(
            "ORANGE_SPLIT_GOP_MAX_BUFFERED_BYTES",
            parse_runtime_env_u64(
                "ORANGE_GOP_MAX_BUFFERED_BYTES",
                config->split_gop.max_buffered_bytes));
    }

    if (runtime_env_is_set("ORANGE_SPLIT_GOP_MAX_WRITER_QUEUE_PACKETS") ||
        runtime_env_is_set("ORANGE_GOP_MAX_WRITER_QUEUE_PACKETS")) {
        config->split_gop.writer_queue.max_packets = parse_runtime_env_u64(
            "ORANGE_SPLIT_GOP_MAX_WRITER_QUEUE_PACKETS",
            parse_runtime_env_u64(
                "ORANGE_GOP_MAX_WRITER_QUEUE_PACKETS",
                config->split_gop.writer_queue.max_packets));
    }

    if (runtime_env_is_set("ORANGE_SPLIT_GOP_MAX_WRITER_QUEUE_BYTES") ||
        runtime_env_is_set("ORANGE_GOP_MAX_WRITER_QUEUE_BYTES")) {
        config->split_gop.writer_queue.max_bytes = parse_runtime_env_u64(
            "ORANGE_SPLIT_GOP_MAX_WRITER_QUEUE_BYTES",
            parse_runtime_env_u64(
                "ORANGE_GOP_MAX_WRITER_QUEUE_BYTES",
                config->split_gop.writer_queue.max_bytes));
    }

    if (runtime_env_is_set("ORANGE_SPLIT_GOP_FAIL_ON_WRITER_OVERFLOW") ||
        runtime_env_is_set("ORANGE_GOP_FAIL_ON_WRITER_OVERFLOW")) {
        config->split_gop.writer_queue.fail_on_overflow = parse_runtime_env_flag(
            "ORANGE_SPLIT_GOP_FAIL_ON_WRITER_OVERFLOW",
            parse_runtime_env_flag(
                "ORANGE_GOP_FAIL_ON_WRITER_OVERFLOW",
                config->split_gop.writer_queue.fail_on_overflow));
    }

    if (runtime_env_is_set("ORANGE_SPLIT_GOP_STRICT")) {
        config->split_gop.strict = parse_runtime_env_flag(
            "ORANGE_SPLIT_GOP_STRICT",
            config->split_gop.strict);
    }
}

bool has_runtime_recording_strategy_env_override() {
    return runtime_env_is_set("ORANGE_RECORDING_MODE") ||
           runtime_env_is_set("ORANGE_GOP_EXPERIMENT") ||
           runtime_env_is_set("ORANGE_SPLIT_GOP_SOURCE_ENCODER_POLICY") ||
           runtime_env_is_set("ORANGE_GOP_ENCODER_POLICY") ||
           runtime_env_is_set("ORANGE_SPLIT_GOP_TRANSFER_MODE") ||
           runtime_env_is_set("ORANGE_GOP_TRANSFER_MODE") ||
           runtime_env_is_set("ORANGE_SPLIT_GOP_PLACEMENT") ||
           runtime_env_is_set("ORANGE_SPLIT_GOP_ENCODER_GPU_IDS") ||
           runtime_env_is_set("ORANGE_SPLIT_GOP_MAX_INFLIGHT_GOPS") ||
           runtime_env_is_set("ORANGE_GOP_MAX_INFLIGHT_GOPS") ||
           runtime_env_is_set("ORANGE_SPLIT_GOP_MAX_BUFFERED_BYTES") ||
           runtime_env_is_set("ORANGE_GOP_MAX_BUFFERED_BYTES") ||
           runtime_env_is_set("ORANGE_SPLIT_GOP_MAX_WRITER_QUEUE_PACKETS") ||
           runtime_env_is_set("ORANGE_GOP_MAX_WRITER_QUEUE_PACKETS") ||
           runtime_env_is_set("ORANGE_SPLIT_GOP_MAX_WRITER_QUEUE_BYTES") ||
           runtime_env_is_set("ORANGE_GOP_MAX_WRITER_QUEUE_BYTES") ||
           runtime_env_is_set("ORANGE_SPLIT_GOP_FAIL_ON_WRITER_OVERFLOW") ||
           runtime_env_is_set("ORANGE_GOP_FAIL_ON_WRITER_OVERFLOW") ||
           runtime_env_is_set("ORANGE_SPLIT_GOP_STRICT");
}

RecordingStrategyConfig resolve_runtime_recording_strategy_config(const CameraParams& camera_params) {
    RecordingStrategyConfig config = camera_params.recording.strategy;
    normalize_recording_strategy_config(&config);
    if (!has_runtime_recording_strategy_env_override()) {
        return config;
    }

    append_runtime_resolution_note(&config.resolution_note, "env override active");
    apply_runtime_recording_strategy_env_overrides(&config);
    normalize_recording_strategy_config(&config);
    return config;
}

ResolvedRecordingConfig build_resolved_recording_config(
    const CameraParams& camera_params,
    int recording_gpu_id,
    const RecordingOutputConfig& recording_output_config,
    const std::string& codec,
    const std::string& preset,
    const std::string& tuning,
    const std::string& rate_control_mode,
    int quality_value,
    int gop_length,
    const EncoderControlOverrides& encoder_control_overrides,
    const ImportanceMapConfig& importance_map_config,
    const std::string& base_folder_name,
    const PreEncoderReferenceCaptureConfig& pre_encoder_reference_capture_config)
{
    ResolvedRecordingConfig resolved;
    resolved.source_gpu_id = camera_params.gpu_id;
    resolved.recording_gpu_id = recording_gpu_id >= 0 ? recording_gpu_id : camera_params.gpu_id;
    resolved.encode = camera_params.recording.encode;
    if (!codec.empty()) {
        resolved.encode.codec = lower_ascii_copy(codec);
    }
    if (!preset.empty()) {
        resolved.encode.preset = lower_ascii_copy(preset);
    }
    if (!tuning.empty()) {
        resolved.encode.tuning = lower_ascii_copy(tuning);
    }
    if (!rate_control_mode.empty()) {
        resolved.encode.rate_control_mode = lower_ascii_copy(rate_control_mode);
    }
    resolved.encode.quality_value = quality_value;
    resolved.encode.gop_length = gop_length;
    resolved.encode.nvenc_direct_input = parse_runtime_env_flag(
        "ORANGE_NVENC_DIRECT_INPUT",
        resolved.encode.nvenc_direct_input);
    resolved.output = recording_output_config;
    resolved.strategy = resolve_runtime_recording_strategy_config(camera_params);
    resolved.constraints = camera_params.recording.constraints;
    resolved.resources = camera_params.recording.resources;
    resolved.encoder_control_overrides = encoder_control_overrides;
    resolved.importance_map = importance_map_config;
    resolved.base_folder_name = base_folder_name;
    resolved.pre_encoder_reference_capture = pre_encoder_reference_capture_config;
    return resolved;
}

std::string get_current_utc_timestamp() {
    auto now = std::chrono::system_clock::now();
    auto time_t_now = std::chrono::system_clock::to_time_t(now);
    std::tm utc_time = *std::gmtime(&time_t_now);
    std::ostringstream oss;
    oss << std::put_time(&utc_time, "%Y-%m-%dT%H:%M:%SZ");
    return oss.str();
}

namespace {
constexpr const char* kCalibrationRegistrySchemaId = "orange.calibration.registry";
constexpr int kCalibrationRegistrySchemaVersion = 1;

std::mutex& recording_snapshot_mutex() {
    static std::mutex m;
    return m;
}

std::mutex& calibration_registry_mutex() {
    static std::mutex m;
    return m;
}

std::mutex& ptp_sync_summary_mutex() {
    static std::mutex m;
    return m;
}

std::mutex& camera_config_mutex() {
    static std::mutex m;
    return m;
}

nlohmann::json build_recording_sync_snapshot(bool sync_camera_enabled,
                                             const PTPParams* ptp_params,
                                             int num_cameras) {
    nlohmann::json sync = nlohmann::json::object();
    sync["schema_version"] = 1;
    sync["captured_at_utc"] = get_current_utc_timestamp();
    sync["camera_sync_enabled"] = sync_camera_enabled;
    sync["num_cameras_expected"] = num_cameras;

    const bool has_ptp_state = (ptp_params != nullptr);
    const bool network_sync = has_ptp_state ? ptp_params->network_sync : false;
    sync["mode"] = !sync_camera_enabled ? "none" : (network_sync ? "ptp_network" : "ptp_local");
    sync["network_sync"] = network_sync;

    nlohmann::json gate_times = nlohmann::json::object();
    if (has_ptp_state && ptp_params->ptp_global_time != 0) {
        gate_times["start_ns"] = ptp_params->ptp_global_time;
    }
    if (has_ptp_state && ptp_params->ptp_stop_time != 0) {
        gate_times["stop_ns"] = ptp_params->ptp_stop_time;
    }
    sync["gate_times"] = gate_times;

    sync["barriers"] = {
        {"start", {
            {"participants_reached", has_ptp_state ? ptp_params->ptp_counter : 0},
            {"all_reached", has_ptp_state ? ptp_params->ptp_start_reached : false}
        }},
        {"stop", {
            {"participants_reached", has_ptp_state ? ptp_params->ptp_stop_counter : 0},
            {"all_reached", has_ptp_state ? ptp_params->ptp_stop_reached : false}
        }}
    };

    sync["signals"] = {
        {"start_observed", has_ptp_state ? ptp_params->network_set_start_ptp : false},
        {"stop_observed", has_ptp_state ? ptp_params->network_set_stop_ptp : false}
    };

    return sync;
}

nlohmann::json build_ptp_sync_summary_base(const std::string& recording_folder,
                                          const std::string& recording_id,
                                          int num_cameras,
                                          bool sync_camera_enabled,
                                          const PTPParams* ptp_params) {
    nlohmann::json summary = nlohmann::json::object();
    summary["schema_version"] = 1;
    summary["recording_id"] =
        recording_id.empty() ? std::filesystem::path(recording_folder).filename().string() : recording_id;
    summary["recording_folder"] = recording_folder;
    summary["created_at_utc"] = get_current_utc_timestamp();
    summary["updated_at_utc"] = summary["created_at_utc"];
    summary["sync"] = build_recording_sync_snapshot(sync_camera_enabled, ptp_params, num_cameras);
    summary["cameras"] = nlohmann::json::object();
    return summary;
}

std::string read_file_to_string(const std::string& path, std::string* error) {
    std::ifstream input(path, std::ios::in | std::ios::binary);
    if (!input.is_open()) {
        if (error) {
            *error = "failed to open file";
        }
        return {};
    }
    std::ostringstream buffer;
    buffer << input.rdbuf();
    return buffer.str();
}

bool write_json_atomic(const std::filesystem::path& path,
                       const nlohmann::json& data,
                       std::filesystem::perms perms,
                       bool set_perms,
                       const char* label,
                       std::string* error_out = nullptr) {
    if (error_out) {
        error_out->clear();
    }
    std::filesystem::path tmp = path;
    tmp += ".tmp";

    std::ofstream out(tmp.string(), std::ios::trunc);
    if (!out.is_open()) {
        if (error_out) {
            *error_out = std::string("failed to open temp file: ") + tmp.string();
        }
        std::cerr << "Failed to write " << label << " temp file: " << tmp.string() << std::endl;
        return false;
    }
    out << data.dump(2) << std::endl;
    out.close();

    if (set_perms) {
        std::error_code ec;
        std::filesystem::permissions(tmp, perms, std::filesystem::perm_options::replace, ec);
        if (ec) {
            if (error_out && error_out->empty()) {
                *error_out = std::string("failed to set permissions on temp file: ") + ec.message();
            }
            std::cerr << "Failed to set permissions on " << tmp.string() << " ("
                      << ec.message() << ")" << std::endl;
        }
    }

    std::error_code ec;
    std::filesystem::rename(tmp, path, ec);
    if (ec) {
        if (error_out) {
            *error_out = std::string("failed to rename temp file: ") + ec.message();
        }
        std::cerr << "Failed to rename " << label << " temp file: " << tmp.string()
                  << " -> " << path.string() << " (" << ec.message() << ")" << std::endl;
        std::filesystem::remove(tmp);
        return false;
    }

    return true;
}

bool write_latest_recording_pointer(const std::string& base_folder,
                                    const std::string& recording_folder,
                                    const std::string& recording_id,
                                    const std::string& timestamp_utc) {
    if (recording_folder.empty()) {
        return false;
    }

    nlohmann::json pointer;
    pointer["recording_id"] = recording_id;
    pointer["timestamp_utc"] = timestamp_utc;
    pointer["recording_folder"] = recording_folder;
    pointer["snapshot_path"] = (std::filesystem::path(recording_folder) / "recording_snapshot.json").string();

    bool wrote_any = false;

    if (!base_folder.empty()) {
        std::filesystem::path meta_dir = std::filesystem::path(base_folder) / ".orange";
        std::filesystem::path pointer_path = meta_dir / "latest_recording.json";

        {
            orange::ScopedFsuid fsuid_guard;
            (void)fsuid_guard;

            std::error_code ec;
            std::filesystem::create_directories(meta_dir, ec);
            if (ec) {
                std::cerr << "Error creating metadata folder: " << meta_dir.string()
                          << " (" << ec.message() << ")" << std::endl;
            } else {
                if (!write_json_atomic(pointer_path, pointer, std::filesystem::perms::unknown, false,
                                       "latest recording pointer")) {
                    std::cerr << "Failed to write latest recording pointer: " << pointer_path.string() << std::endl;
                } else {
                    wrote_any = true;
                }
            }
        }
    }

    {
        std::filesystem::path run_dir = "/run/orange";
        std::filesystem::path run_pointer = run_dir / "latest_recording.json";
        std::error_code ec;
        std::filesystem::create_directories(run_dir, ec);
        if (ec) {
            std::cerr << "Error creating run metadata folder: " << run_dir.string()
                      << " (" << ec.message() << ")" << std::endl;
        } else {
            std::filesystem::permissions(
                run_dir,
                std::filesystem::perms::owner_read | std::filesystem::perms::owner_write |
                    std::filesystem::perms::owner_exec | std::filesystem::perms::group_read |
                    std::filesystem::perms::group_exec | std::filesystem::perms::others_read |
                    std::filesystem::perms::others_exec,
                std::filesystem::perm_options::replace,
                ec);
            if (ec) {
                std::cerr << "Failed to set permissions on " << run_dir.string()
                          << " (" << ec.message() << ")" << std::endl;
            }

            std::filesystem::perms run_perms =
                std::filesystem::perms::owner_read | std::filesystem::perms::owner_write |
                std::filesystem::perms::group_read | std::filesystem::perms::others_read;
            if (!write_json_atomic(run_pointer, pointer, run_perms, true, "run latest recording pointer")) {
                std::cerr << "Failed to write run latest recording pointer: " << run_pointer.string() << std::endl;
            } else {
                wrote_any = true;
            }
        }
    }

    return wrote_any;
}
} // namespace

std::string get_current_time_milliseconds() {
    // ... (implementation from project.h)
    auto now = std::chrono::system_clock::now();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()) % 1000;
    auto time = std::chrono::system_clock::to_time_t(now);
    auto tm = *std::localtime(&time);

    std::ostringstream oss;
    oss << std::put_time(&tm, "%H_%M_%S_") << std::setfill('0') << std::setw(3) << ms.count();

    return oss.str();
}

std::string get_current_date() {
    // ... (implementation from project.h)
    auto now = std::chrono::system_clock::now();
    auto time_t_now = std::chrono::system_clock::to_time_t(now);
    std::tm local_time = *std::localtime(&time_t_now);
    std::ostringstream oss;
    oss << std::put_time(&local_time, "%Y_%m_%d");
    return oss.str();
}

std::string get_current_date_time() {
    // ... (implementation from project.h)
    auto now = std::chrono::system_clock::now();
    auto time_t_now = std::chrono::system_clock::to_time_t(now);
    std::tm local_time = *std::localtime(&time_t_now);
    std::ostringstream oss;
    oss << std::put_time(&local_time, "%Y_%m_%d_%H_%M_%S");
    return oss.str();
}

std::string format_elapsed_time(std::chrono::seconds elapsed_seconds) {
    // ... (implementation from project.h)
    int hours = static_cast<int>(elapsed_seconds.count() / 3600);
    int minutes = static_cast<int>((elapsed_seconds.count() % 3600) / 60);
    int seconds = static_cast<int>(elapsed_seconds.count() % 60);

    std::ostringstream oss;
    oss << std::setfill('0') << std::setw(2) << hours << ":"
        << std::setfill('0') << std::setw(2) << minutes << ":"
        << std::setfill('0') << std::setw(2) << seconds;
    return oss.str();
}

void init_galvo_camera_params(CameraParams* camera_params, int camera_id, int num_cameras, int gain, int exposure)
{
    // ... (implementation from project.h)
    camera_params->width = 1280;
    camera_params->height = 1280;
    camera_params->frame_rate = 100;
    camera_params->gain = gain;
    camera_params->exposure = exposure;
    camera_params->pixel_format = "BayerRG8";
    camera_params->color_temp = "CT_3000K";
    camera_params->camera_id = camera_id;
    camera_params->gpu_id = 1;
    camera_params->configured_gpu_id = camera_params->gpu_id;
    camera_params->gpu_id_runtime_overridden = false;
    camera_params->num_cameras = num_cameras;
    camera_params->gpu_direct = false;
    camera_params->focus_uart_bootstrap = false;
    camera_params->need_reorder = false;
    camera_params->color = true;
    camera_params->iris = 0;
}

void init_65MP_camera_params_mono(CameraParams* camera_params, int camera_id, int num_cameras, int gain, int exposure, int gpu_id, int frame_rate)
{
    // ... (implementation from project.h)
    camera_params->width = 512;
    camera_params->height = 512;
    camera_params->frame_rate = frame_rate;
    camera_params->gain = gain;
    camera_params->exposure = exposure;
    camera_params->pixel_format = "Mono8";
    camera_params->gpu_id = gpu_id;
    camera_params->configured_gpu_id = camera_params->gpu_id;
    camera_params->gpu_id_runtime_overridden = false;
    camera_params->num_cameras = num_cameras;
    camera_params->gpu_direct = false;
    camera_params->focus_uart_bootstrap = false;
    camera_params->need_reorder = false;
    camera_params->focus = 4311;
    camera_params->camera_id = camera_id;
    camera_params->color = false;
    camera_params->iris = 0;
}

void init_65MP_camera_params_color(CameraParams* camera_params, int camera_id, int num_cameras, int gain, int exposure, int gpu_id, int frame_rate)
{
    // ... (implementation from project.h)
    camera_params->width = 512; 
    camera_params->height = 512; 
    camera_params->frame_rate = frame_rate;
    camera_params->gain = gain;
    camera_params->exposure = exposure;
    camera_params->pixel_format = "BayerGB8";
    camera_params->gpu_id = gpu_id;
    camera_params->configured_gpu_id = camera_params->gpu_id;
    camera_params->gpu_id_runtime_overridden = false;
    camera_params->num_cameras = num_cameras;
    camera_params->gpu_direct = false;
    camera_params->focus_uart_bootstrap = false;
    camera_params->need_reorder = false;
    camera_params->focus = 4419;
    camera_params->camera_id = camera_id;
    camera_params->color = true;
    camera_params->color_temp = "CT_3000K";
    camera_params->iris = 0;
}

void init_7MP_camera_params_color(CameraParams* camera_params, int camera_id, int num_cameras, int gain, int exposure, int gpu_id, int frame_rate)
{
    // ... (implementation from project.h)
    camera_params->width = 3208;
    camera_params->height = 2200;
    camera_params->frame_rate = frame_rate;
    camera_params->gain = gain;
    camera_params->exposure = exposure;
    camera_params->pixel_format = "BayerRG8";
    camera_params->color_temp = "CT_3000K";
    camera_params->gpu_id = gpu_id;
    camera_params->configured_gpu_id = camera_params->gpu_id;
    camera_params->gpu_id_runtime_overridden = false;
    camera_params->num_cameras = num_cameras;
    camera_params->gpu_direct = false;
    camera_params->focus_uart_bootstrap = false;
    camera_params->need_reorder = false;
    camera_params->focus = 345;
    camera_params->camera_id = camera_id;
    camera_params->color = true;
    camera_params->iris = 0;
}

void init_7MP_camera_params_mono(CameraParams* camera_params, int camera_id, int num_cameras, int gain, int exposure, int gpu_id, int frame_rate)
{
    // ... (implementation from project.h)
    camera_params->width = 3208;
    camera_params->height = 2200;
    camera_params->frame_rate = frame_rate;
    camera_params->gain = gain;
    camera_params->exposure = exposure;
    camera_params->pixel_format = "Mono8";
    camera_params->color_temp = "CT_3000K";
    camera_params->gpu_id = gpu_id;
    camera_params->configured_gpu_id = camera_params->gpu_id;
    camera_params->gpu_id_runtime_overridden = false;
    camera_params->num_cameras = num_cameras;
    camera_params->gpu_direct = false;
    camera_params->focus_uart_bootstrap = false;
    camera_params->need_reorder = false;
    camera_params->focus = 4700;
    camera_params->camera_id = camera_id;
    camera_params->color = false;
    camera_params->iris = 0;
}

bool make_folder(std::string folder_name)
{
    orange::ScopedFsuid fsuid_guard;
    (void)fsuid_guard;
    // ... (implementation from project.h)
    if (!std::filesystem::exists(folder_name))
    {
        if(!std::filesystem::create_directories(folder_name)) {
            std::cerr << "Error creating folder: " << folder_name << std::endl;
        }
        return false;
    }
    return true;
}

bool ensure_directory_exists(const std::string& folder_name, std::string* error_out)
{
    if (error_out) {
        error_out->clear();
    }
    if (folder_name.empty()) {
        if (error_out) {
            *error_out = "folder path missing";
        }
        return false;
    }

    const std::filesystem::path folder_path(folder_name);
    std::error_code ec;
    if (std::filesystem::exists(folder_path, ec)) {
        if (ec) {
            if (error_out) {
                *error_out = std::string("failed to query folder: ") + ec.message();
            }
            return false;
        }
        if (!std::filesystem::is_directory(folder_path, ec) || ec) {
            if (error_out) {
                *error_out = ec ? std::string("failed to inspect folder: ") + ec.message()
                                : "path exists but is not a directory";
            }
            return false;
        }
        return true;
    }

    orange::ScopedFsuid fsuid_guard;
    (void)fsuid_guard;
    if (!std::filesystem::create_directories(folder_path, ec) && ec) {
        if (error_out) {
            *error_out = std::string("failed to create folder: ") + ec.message();
        }
        return false;
    }
    return true;
}

void list_child_directories(const std::string& root_folder, std::vector<std::string>& child_directories)
{
    child_directories.clear();
    std::error_code ec;
    if (!std::filesystem::exists(root_folder, ec) || ec) {
        return;
    }

    for (const auto& entry : std::filesystem::directory_iterator(root_folder, ec)) {
        if (ec) {
            child_directories.clear();
            return;
        }
        if (entry.is_directory(ec) && !ec) {
            child_directories.push_back(entry.path().string());
        }
    }
    std::sort(child_directories.begin(), child_directories.end());
}

void update_camera_configs(std::vector<std::string>& camera_config_files, std::string input_folder)
{
    // ... (implementation from project.h)
    camera_config_files.clear();
    std::string camera_config_dir = input_folder;
    for (const auto &entry : std::filesystem::directory_iterator(camera_config_dir))
    {
        std::string entry_str = entry.path().string();
        if (entry_str.find(".json") != std::string::npos)
            camera_config_files.push_back(entry_str);
    }
    std::sort(camera_config_files.begin(), camera_config_files.end());
}

std::string build_camera_config_path(const std::string& config_folder, const CameraParams& camera_params)
{
    if (config_folder.empty() || camera_params.camera_serial.empty()) {
        return {};
    }
    return (std::filesystem::path(config_folder) /
            (canonicalize_camera_serial_string(camera_params.camera_serial) + ".json")).string();
}

void assign_camera_config_paths(CameraParams* cameras_params, int num_cameras, const std::string& config_folder)
{
    if (!cameras_params) {
        return;
    }
    for (int i = 0; i < num_cameras; ++i) {
        cameras_params[i].config_path = build_camera_config_path(config_folder, cameras_params[i]);
    }
}

void select_cameras_have_configs(std::vector<std::string>& camera_config_files, GigEVisionDeviceInfo* device_info, bool* check, int cam_count)
{
    // ... (implementation from project.h)
    for (int i=0; i<cam_count; i++) {
        std::string camera_serial = device_info[i].serialNumber;
        auto it = find_camera_config_for_serial(camera_config_files, camera_serial);
        if (it != camera_config_files.end()) {
            check[i] = true;
        } else {
            check[i] = false;
        }
    }
}

bool set_camera_params(CameraParams* camera_params, GigEVisionDeviceInfo* device_info, std::vector<std::string>& camera_config_files, int camera_idx, int num_cameras)
{
    // ... (implementation from project.h)
    camera_params->camera_serial.assign(canonicalize_camera_serial_string(device_info->serialNumber));
    camera_params->device_model = device_info->modelName;
    camera_params->camera_name = camera_params->camera_serial;
    camera_params->config_path.clear();

    auto it = find_camera_config_for_serial(camera_config_files, camera_params->camera_serial);

    if (it == camera_config_files.end())
    {
        if (strcmp(device_info->modelName, "HB-65000GM")==0) {
            int gpu_id = 0;
            init_65MP_camera_params_mono(camera_params, camera_idx, num_cameras, 2000, 1000, gpu_id, 400);
        } else if (strcmp(device_info->modelName, "HB-7000SC")==0) {
            int gpu_id = 0;
            init_7MP_camera_params_color(camera_params, camera_idx, num_cameras, 1500, 3000, gpu_id, 30);
        } else if (strcmp(device_info->modelName, "HB-65000GC")==0) {
            int gpu_id = 0;
            init_65MP_camera_params_color(camera_params, camera_idx, num_cameras, 2000, 28000, gpu_id, 10);
        } else if (strcmp(device_info->modelName, "HB-7000SM")==0) {
            int gpu_id = 0;
            init_7MP_camera_params_mono(camera_params, camera_idx, num_cameras, 1000, 3000, gpu_id, 30);
        } else {
            printf("Use default parameters. \n");
            return false;
        }
    } else {
        auto config_idx = std::distance(camera_config_files.cbegin(), it);
        std::cout << "Load camera json file: " << camera_config_files[config_idx] << std::endl;
        camera_params->config_path = camera_config_files[config_idx];
        load_camera_json_config_files(camera_config_files[config_idx], camera_params, camera_idx, num_cameras);
    }
    infer_camera_gpio_metadata(camera_params);
    return true;
}

bool write_recording_snapshot(const std::string& recording_folder,
                              const std::string& recording_id,
                              const CameraParams* cameras_params,
                              int num_cameras,
                              const std::string& base_folder,
                              bool sync_camera_enabled,
                              const PTPParams* ptp_params) {
    if (!cameras_params || num_cameras <= 0) {
        return false;
    }

    std::lock_guard<std::mutex> lock(recording_snapshot_mutex());

    nlohmann::json snapshot;
    std::string resolved_recording_id = recording_id.empty() ? get_current_date_time() : recording_id;
    std::string timestamp_utc = get_current_utc_timestamp();
    snapshot["recording_id"] = resolved_recording_id;
    snapshot["timestamp_utc"] = timestamp_utc;
    snapshot["producer_version"] = "unknown";

    nlohmann::json cameras = nlohmann::json::object();
    nlohmann::json camera_runtime = nlohmann::json::object();

    for (int i = 0; i < num_cameras; ++i) {
        const CameraParams& params = cameras_params[i];
        std::string config_error;
        std::string config_contents;
        nlohmann::json config_json;
        bool config_ok = false;

        if (!params.config_path.empty()) {
            config_contents = read_file_to_string(params.config_path, &config_error);
            if (!config_contents.empty()) {
                try {
                    config_json = nlohmann::json::parse(config_contents);
                    config_ok = true;
                } catch (const std::exception& ex) {
                    config_error = std::string("config parse failed: ") + ex.what();
                }
            } else if (config_error.empty()) {
                config_error = "config file empty";
            }
        } else {
            config_error = "config path missing";
        }

        std::string camera_key = params.camera_serial;
        if (camera_key.empty() && config_ok && config_json.contains("device_serial_number") &&
            config_json["device_serial_number"].is_string()) {
            camera_key = config_json["device_serial_number"].get<std::string>();
        }
        if (camera_key.empty()) {
            camera_key = std::to_string(params.camera_id);
        }
        if (config_ok) {
            cameras[camera_key] = config_json;
        } else {
            cameras[camera_key] = nullptr;
            if (!config_error.empty()) {
                std::cerr << "Camera " << params.camera_id << " config missing: " << config_error << std::endl;
            }
        }
        camera_runtime[camera_key] = build_camera_runtime_snapshot(params);
    }

    snapshot["cameras"] = cameras;
    snapshot["camera_runtime"] = camera_runtime;
    snapshot["sync"] = build_recording_sync_snapshot(sync_camera_enabled, ptp_params, num_cameras);

    nlohmann::json gpu_inventory = nlohmann::json::object();
    std::set<int> seen_gpu_ids;
    for (int i = 0; i < num_cameras; ++i) {
        const int gpu_id = cameras_params[i].gpu_id;
        if (!seen_gpu_ids.insert(gpu_id).second) {
            continue;
        }
        gpu_inventory[std::to_string(gpu_id)] = build_gpu_runtime_info(gpu_id);
    }
    snapshot["gpu_inventory"] = gpu_inventory;

    bool wrote_snapshot = false;
    if (!recording_folder.empty()) {
        orange::ScopedFsuid fsuid_guard;
        (void)fsuid_guard;
        std::filesystem::path out_path = std::filesystem::path(recording_folder) / "recording_snapshot.json";
        if (!write_json_atomic(out_path, snapshot, std::filesystem::perms::unknown, false, "recording snapshot")) {
            return false;
        }
        wrote_snapshot = true;
    }

    if (wrote_snapshot) {
        if (!write_latest_recording_pointer(base_folder, recording_folder, resolved_recording_id, timestamp_utc)) {
            std::cerr << "Failed to update latest recording pointer in base folder." << std::endl;
        }
    }

    return true;
}

bool initialize_ptp_sync_summary(const std::string& recording_folder,
                                 const std::string& recording_id,
                                 int num_cameras,
                                 bool sync_camera_enabled,
                                 const PTPParams* ptp_params) {
    if (recording_folder.empty()) {
        return false;
    }

    std::lock_guard<std::mutex> lock(ptp_sync_summary_mutex());
    const std::filesystem::path summary_path =
        std::filesystem::path(recording_folder) / "ptp_sync_summary.json";
    const nlohmann::json summary = build_ptp_sync_summary_base(
        recording_folder,
        recording_id,
        num_cameras,
        sync_camera_enabled,
        ptp_params);

    orange::ScopedFsuid fsuid_guard;
    (void)fsuid_guard;
    return write_json_atomic(summary_path, summary, std::filesystem::perms::unknown, false, "ptp sync summary");
}

bool update_ptp_sync_summary_camera(const std::string& recording_folder,
                                    const std::string& camera_serial,
                                    const nlohmann::json& camera_summary) {
    if (recording_folder.empty() || camera_serial.empty()) {
        return false;
    }

    const std::filesystem::path summary_path =
        std::filesystem::path(recording_folder) / "ptp_sync_summary.json";

    std::lock_guard<std::mutex> lock(ptp_sync_summary_mutex());

    nlohmann::json summary;
    std::string error;
    const std::string contents = read_file_to_string(summary_path.string(), &error);
    if (!contents.empty()) {
        try {
            summary = nlohmann::json::parse(contents);
        } catch (const std::exception& ex) {
            std::cerr << "Failed to parse ptp sync summary: " << summary_path.string()
                      << " (" << ex.what() << ")" << std::endl;
            summary = nlohmann::json::object();
        }
    } else {
        if (!error.empty() && error != "failed to open file") {
            std::cerr << "Failed to read ptp sync summary: " << summary_path.string()
                      << " (" << error << ")" << std::endl;
        }
        summary = build_ptp_sync_summary_base(recording_folder, "", 0, false, nullptr);
    }

    if (!summary.is_object()) {
        summary = nlohmann::json::object();
    }
    if (!summary.contains("cameras") || !summary["cameras"].is_object()) {
        summary["cameras"] = nlohmann::json::object();
    }

    summary["updated_at_utc"] = get_current_utc_timestamp();
    summary["cameras"][camera_serial] = camera_summary;

    orange::ScopedFsuid fsuid_guard;
    (void)fsuid_guard;
    return write_json_atomic(summary_path, summary, std::filesystem::perms::unknown, false, "ptp sync summary");
}

bool update_recording_snapshot_encoder(const std::string& recording_folder,
                                       const std::string& camera_serial,
                                       const nlohmann::json& encoder_info) {
    if (recording_folder.empty() || camera_serial.empty()) {
        return false;
    }

    const std::filesystem::path snapshot_path =
        std::filesystem::path(recording_folder) / "recording_snapshot.json";

    std::lock_guard<std::mutex> lock(recording_snapshot_mutex());

    std::string error;
    std::string contents = read_file_to_string(snapshot_path.string(), &error);
    if (contents.empty()) {
        std::cerr << "Failed to read recording snapshot: " << snapshot_path.string()
                  << " (" << (error.empty() ? "empty file" : error) << ")" << std::endl;
        return false;
    }

    nlohmann::json snapshot;
    try {
        snapshot = nlohmann::json::parse(contents);
    } catch (const std::exception& ex) {
        std::cerr << "Failed to parse recording snapshot: " << snapshot_path.string()
                  << " (" << ex.what() << ")" << std::endl;
        return false;
    }

    if (!snapshot.is_object()) {
        snapshot = nlohmann::json::object();
    }
    if (!snapshot.contains("encoders") || !snapshot["encoders"].is_object()) {
        snapshot["encoders"] = nlohmann::json::object();
    }

    snapshot["encoders"][camera_serial] = encoder_info;

    orange::ScopedFsuid fsuid_guard;
    (void)fsuid_guard;
    if (!write_json_atomic(snapshot_path, snapshot, std::filesystem::perms::unknown, false, "recording snapshot")) {
        return false;
    }

    return true;
}

bool update_recording_snapshot_pipeline_metrics(const std::string& recording_folder,
                                                const std::string& camera_serial,
                                                const nlohmann::json& pipeline_info) {
    if (recording_folder.empty() || camera_serial.empty()) {
        return false;
    }

    const std::filesystem::path snapshot_path =
        std::filesystem::path(recording_folder) / "recording_snapshot.json";

    std::lock_guard<std::mutex> lock(recording_snapshot_mutex());

    std::string error;
    std::string contents = read_file_to_string(snapshot_path.string(), &error);
    if (contents.empty()) {
        std::cerr << "Failed to read recording snapshot: " << snapshot_path.string()
                  << " (" << (error.empty() ? "empty file" : error) << ")" << std::endl;
        return false;
    }

    nlohmann::json snapshot;
    try {
        snapshot = nlohmann::json::parse(contents);
    } catch (const std::exception& ex) {
        std::cerr << "Failed to parse recording snapshot: " << snapshot_path.string()
                  << " (" << ex.what() << ")" << std::endl;
        return false;
    }

    if (!snapshot.is_object()) {
        snapshot = nlohmann::json::object();
    }
    if (!snapshot.contains("pipeline_metrics") || !snapshot["pipeline_metrics"].is_object()) {
        snapshot["pipeline_metrics"] = nlohmann::json::object();
    }

    snapshot["pipeline_metrics"][camera_serial] = pipeline_info;

    orange::ScopedFsuid fsuid_guard;
    (void)fsuid_guard;
    if (!write_json_atomic(snapshot_path, snapshot, std::filesystem::perms::unknown, false, "recording snapshot")) {
        return false;
    }

    return true;
}

bool update_recording_snapshot_gpu_monitoring(const std::string& recording_folder,
                                              const std::string& monitor_name,
                                              const nlohmann::json& monitor_info) {
    if (recording_folder.empty() || monitor_name.empty()) {
        return false;
    }

    const std::filesystem::path snapshot_path =
        std::filesystem::path(recording_folder) / "recording_snapshot.json";

    std::lock_guard<std::mutex> lock(recording_snapshot_mutex());

    std::string error;
    std::string contents = read_file_to_string(snapshot_path.string(), &error);
    if (contents.empty()) {
        std::cerr << "Failed to read recording snapshot: " << snapshot_path.string()
                  << " (" << (error.empty() ? "empty file" : error) << ")" << std::endl;
        return false;
    }

    nlohmann::json snapshot;
    try {
        snapshot = nlohmann::json::parse(contents);
    } catch (const std::exception& ex) {
        std::cerr << "Failed to parse recording snapshot: " << snapshot_path.string()
                  << " (" << ex.what() << ")" << std::endl;
        return false;
    }

    if (!snapshot.is_object()) {
        snapshot = nlohmann::json::object();
    }
    if (!snapshot.contains("gpu_monitoring") || !snapshot["gpu_monitoring"].is_object()) {
        snapshot["gpu_monitoring"] = nlohmann::json::object();
    }

    snapshot["gpu_monitoring"][monitor_name] = monitor_info;

    orange::ScopedFsuid fsuid_guard;
    (void)fsuid_guard;
    if (!write_json_atomic(snapshot_path, snapshot, std::filesystem::perms::unknown, false, "recording snapshot")) {
        return false;
    }

    return true;
}

bool read_camera_config_snapshot(const CameraParams& camera_params,
                                 std::string* config_contents,
                                 std::string* error_out) {
    if (config_contents) {
        config_contents->clear();
    }
    if (error_out) {
        error_out->clear();
    }
    if (camera_params.config_path.empty()) {
        if (error_out) {
            *error_out = "config path missing";
        }
        return false;
    }

    std::lock_guard<std::mutex> lock(camera_config_mutex());
    std::string read_error;
    const std::string contents = read_file_to_string(camera_params.config_path, &read_error);
    if (contents.empty()) {
        if (error_out) {
            *error_out = read_error.empty() ? "config file empty" : read_error;
        }
        return false;
    }

    if (config_contents) {
        *config_contents = contents;
    }
    return true;
}

bool save_camera_json_config(const CameraParams& camera_params,
                             std::string* error_out) {
    if (error_out) {
        error_out->clear();
    }
    if (camera_params.config_path.empty()) {
        if (error_out) {
            *error_out = "config path missing";
        }
        return false;
    }

    std::lock_guard<std::mutex> lock(camera_config_mutex());
    const std::filesystem::path config_path(camera_params.config_path);
    std::string mkdir_error;
    if (!ensure_directory_exists(config_path.parent_path().string(), &mkdir_error)) {
        if (error_out) {
            *error_out = mkdir_error.empty() ? "failed to create config folder" : mkdir_error;
        }
        return false;
    }

    std::string read_error;
    const std::string contents = read_file_to_string(camera_params.config_path, &read_error);
    nlohmann::json camera_config;
    if (contents.empty()) {
        if (read_error == "failed to open file" || read_error.empty()) {
            camera_config = nlohmann::json::object();
        } else {
            if (error_out) {
                *error_out = read_error;
            }
            return false;
        }
    } else {
        try {
            camera_config = nlohmann::json::parse(contents);
        } catch (const std::exception& ex) {
            if (error_out) {
                *error_out = std::string("failed to parse config json: ") + ex.what();
            }
            return false;
        }
    }
    if (!camera_config.is_object()) {
        if (error_out) {
            *error_out = "camera config must be a JSON object";
        }
        return false;
    }

    camera_config = build_camera_config_json_from_params(camera_params);

    std::error_code perms_error;
    const std::filesystem::file_status status = std::filesystem::status(config_path, perms_error);
    const bool set_perms = !perms_error;
    const std::filesystem::perms perms = set_perms ? status.permissions() : std::filesystem::perms::unknown;

    orange::ScopedFsuid fsuid_guard;
    (void)fsuid_guard;
    std::string write_error;
    if (!write_json_atomic(config_path, camera_config, perms, set_perms, "camera config", &write_error)) {
        if (error_out) {
            if (!write_error.empty()) {
                *error_out = std::string("failed to write camera config: ") + camera_params.config_path +
                             " (" + write_error + ")";
            } else {
                *error_out = std::string("failed to write camera config: ") + camera_params.config_path;
            }
        }
        return false;
    }
    return true;
}

bool update_calibration_artifact_registry(const std::string& artifact_root_dir,
                                          const nlohmann::json& manifest,
                                          std::string* error_out) {
    if (artifact_root_dir.empty()) {
        if (error_out) {
            *error_out = "Calibration artifact root is empty.";
        }
        return false;
    }
    if (!manifest.is_object()) {
        if (error_out) {
            *error_out = "Calibration manifest is not a JSON object.";
        }
        return false;
    }
    if (!manifest.contains("artifact_id") || !manifest["artifact_id"].is_string()) {
        if (error_out) {
            *error_out = "Calibration manifest is missing artifact_id.";
        }
        return false;
    }
    if (!manifest.contains("artifact_schema_id") || !manifest["artifact_schema_id"].is_string()) {
        if (error_out) {
            *error_out = "Calibration manifest is missing artifact_schema_id.";
        }
        return false;
    }

    const std::string artifact_id = manifest["artifact_id"].get<std::string>();
    const std::string artifact_schema_id = manifest["artifact_schema_id"].get<std::string>();
    const std::filesystem::path registry_path = std::filesystem::path(artifact_root_dir) / "index.json";

    std::lock_guard<std::mutex> lock(calibration_registry_mutex());

    nlohmann::json registry;
    if (std::filesystem::exists(registry_path)) {
        std::string read_error;
        const std::string existing = read_file_to_string(registry_path.string(), &read_error);
        if (existing.empty()) {
            if (error_out) {
                *error_out = "Failed to read calibration registry: " +
                             (read_error.empty() ? registry_path.string() : read_error);
            }
            return false;
        }
        try {
            registry = nlohmann::json::parse(existing);
        } catch (const std::exception& ex) {
            if (error_out) {
                *error_out = std::string("Failed to parse calibration registry: ") + ex.what();
            }
            return false;
        }
    }

    if (!registry.is_object()) {
        registry = nlohmann::json::object();
    }
    registry["schema_id"] = kCalibrationRegistrySchemaId;
    registry["schema_version"] = kCalibrationRegistrySchemaVersion;
    registry["artifact_root"] = artifact_root_dir;
    registry["updated_utc"] = get_current_utc_timestamp();
    if (!registry.contains("artifacts_by_id") || !registry["artifacts_by_id"].is_object()) {
        registry["artifacts_by_id"] = nlohmann::json::object();
    }
    if (!registry.contains("latest_by_schema") || !registry["latest_by_schema"].is_object()) {
        registry["latest_by_schema"] = nlohmann::json::object();
    }

    std::string fingerprint;
    if (manifest.contains("calibration_ref") && manifest["calibration_ref"].is_object()) {
        fingerprint = manifest["calibration_ref"].value("fingerprint", "");
    }

    nlohmann::json entry;
    entry["artifact_id"] = artifact_id;
    entry["artifact_schema_id"] = artifact_schema_id;
    entry["artifact_schema_version"] = manifest.value("artifact_schema_version", 0);
    entry["created_utc"] = manifest.value("created_utc", "");
    entry["fingerprint"] = fingerprint;
    entry["relative_manifest_path"] =
        (std::filesystem::path(artifact_id) / "manifest.json").generic_string();
    if (manifest.contains("producer")) {
        entry["producer"] = manifest["producer"];
    }
    if (manifest.contains("compatibility")) {
        entry["compatibility"] = manifest["compatibility"];
    }
    if (manifest.contains("summary")) {
        entry["summary"] = manifest["summary"];
    }

    registry["artifacts_by_id"][artifact_id] = entry;
    registry["latest_by_schema"][artifact_schema_id] = artifact_id;
    registry["artifact_count"] = registry["artifacts_by_id"].size();

    orange::ScopedFsuid fsuid_guard;
    (void)fsuid_guard;
    if (!write_json_atomic(registry_path, registry, std::filesystem::perms::unknown, false,
                           "calibration registry")) {
        if (error_out) {
            *error_out = "Failed to write calibration registry: " + registry_path.string();
        }
        return false;
    }

    return true;
}

void allocate_camera_frame_buffers(CameraEmergent* ecams, CameraParams* cameras_params, int evt_buffer_size, int num_cameras)
{
    // ... (implementation from project.h)
    for (int i = 0; i < num_cameras; i++)
    {
        camera_open_stream(&ecams[i].camera, &cameras_params[i], "project_allocate_camera_frame_buffers");
        ecams[i].evt_frame = new Emergent::CEmergentFrame[evt_buffer_size];
        allocate_frame_buffer(&ecams[i].camera, ecams[i].evt_frame, &cameras_params[i], evt_buffer_size);
        if (cameras_params[i].need_reorder && cameras_params[i].gpu_direct)
        {
            allocate_frame_reorder_buffer(&ecams[i].camera, &ecams[i].frame_reorder, &cameras_params[i]);
        }
    }
}

void client_send_bringup_message(EnetContext* enet_context, flatbuffers::FlatBufferBuilder* builder, ENetPeer *server_connection, int cam_count, FetchGame::ManagerState server_state)
{
    // ... (implementation from project.h)
    char hostname[100];
    gethostname(hostname, 100);
    builder->Clear();
    auto server_name = builder->CreateString(hostname);
    auto message_fb = FetchGame::Createbring_up_message(*builder, server_name, cam_count);
    FetchGame::ServerBuilder server_builder(*builder);
    server_builder.add_signal_type(FetchGame::SignalType_ClientBringup);
    server_builder.add_server_mesg(message_fb);
    server_builder.add_server_state(server_state);
    auto server_fb = server_builder.Finish();
    builder->Finish(server_fb);
    uint8_t *server_buffer = builder->GetBufferPointer();
    int server_buf_size = builder->GetSize();
    ENetPacket* enet_packet = enet_packet_create(server_buffer, server_buf_size, 0);
    enet_peer_send(server_connection, 0, enet_packet);
}

void client_send_state_update_message(EnetContext* enet_context, flatbuffers::FlatBufferBuilder* builder, ENetPeer *server_connection, FetchGame::ManagerState server_state)
{
    // ... (implementation from project.h)
    builder->Clear();
    FetchGame::ServerBuilder server_builder(*builder);
    server_builder.add_signal_type(FetchGame::SignalType_ClientStateUpdate);
    server_builder.add_server_state(server_state);
    auto server_fb = server_builder.Finish();
    builder->Finish(server_fb);
    uint8_t *server_buffer = builder->GetBufferPointer();
    int server_buf_size = builder->GetSize();
    ENetPacket* enet_packet = enet_packet_create(server_buffer, server_buf_size, 0);
    enet_peer_send(server_connection, 0, enet_packet);
}

void host_broadcast_open_cameras(flatbuffers::FlatBufferBuilder* builder, EnetContext* server, std::string config_file_name)
{
    // ... (implementation from project.h)
    builder->Clear();
    auto config_message = builder->CreateString(config_file_name);
    FetchGame::ServerBuilder server_builder(*builder);
    server_builder.add_config_folder(config_message);
    server_builder.add_control(FetchGame::ServerControl_OPENCAMERA);
    auto my_server = server_builder.Finish();
    builder->Finish(my_server);
    uint8_t *server_buffer = builder->GetBufferPointer();
    int server_buf_size = builder->GetSize();
    ENetPacket* enet_packet = enet_packet_create(server_buffer, server_buf_size, 0);
    enet_host_broadcast(server->m_pNetwork, 0, enet_packet);
}

void host_broadcast_start_threads(flatbuffers::FlatBufferBuilder* builder, EnetContext* server, std::string record_folder_name, std::string encoder_basic_setup)
{
    // ... (implementation from project.h)
    builder->Clear();
    auto record_folder_message = builder->CreateString(record_folder_name);
    auto encoder_setup_message = builder->CreateString(encoder_basic_setup);
    FetchGame::ServerBuilder server_builder(*builder);
    server_builder.add_record_folder(record_folder_message);
    server_builder.add_encoder_setup(encoder_setup_message);
    server_builder.add_control(FetchGame::ServerControl_STARTTHREAD);
    auto my_server = server_builder.Finish();
    builder->Finish(my_server);
    uint8_t *server_buffer = builder->GetBufferPointer();
    int server_buf_size = builder->GetSize();
    ENetPacket* enet_packet = enet_packet_create(server_buffer, server_buf_size, 0);
    enet_host_broadcast(server->m_pNetwork, 0, enet_packet);
}

void host_broadcast_set_start_ptp(flatbuffers::FlatBufferBuilder* builder, EnetContext* server, unsigned long long ptp_global_time)
{
    // ... (implementation from project.h)
    builder->Clear();
    FetchGame::ServerBuilder server_builder(*builder);
    server_builder.add_control(FetchGame::ServerControl_STARTRECORDING);
    server_builder.add_ptp_global_time(ptp_global_time);
    auto my_server = server_builder.Finish();
    builder->Finish(my_server);
    uint8_t *server_buffer = builder->GetBufferPointer();
    int server_buf_size = builder->GetSize();
    ENetPacket* enet_packet = enet_packet_create(server_buffer, server_buf_size, 0);
    enet_host_broadcast(server->m_pNetwork, 0, enet_packet);
}
