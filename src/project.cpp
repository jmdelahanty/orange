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
#include "json.hpp"      // For nlohmann::json
#include "fetch_generated.h" // For FetchGame:: enums and builders
#include "flatbuffers/flatbuffers.h" // For flatbuffers::FlatBufferBuilder

// --- Definitions of all functions previously in project.h ---

void prepare_application_folders(std::string orange_root_dir_str)
{
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

void load_camera_json_config_files(std::string file_name, CameraParams* camera_params, int camera_id, int num_cameras) {
    // ... (implementation from project.h)
    std::ifstream f(file_name);
    nlohmann::json camera_config = nlohmann::json::parse(f);

    camera_params->camera_id = camera_id;
    camera_params->num_cameras = num_cameras;
    camera_params->need_reorder = false;

    camera_params->camera_name = camera_config["name"];
    camera_params->width = camera_config["width"];
    camera_params->height = camera_config["height"];
    camera_params->frame_rate = camera_config["frame_rate"];
    camera_params->gain = camera_config["gain"];
    camera_params->exposure = camera_config["exposure"];
    camera_params->pixel_format = camera_config["pixel_format"];
    camera_params->color_temp = camera_config["color_temp"];
    camera_params->gpu_id = camera_config["gpu_id"];
    camera_params->gpu_direct = camera_config["gpu_direct"];
    camera_params->color = camera_config["color"];
    camera_params->focus = camera_config["focus"];
    camera_params->iris = camera_config["iris"];
}

namespace {
std::string get_current_utc_timestamp() {
    auto now = std::chrono::system_clock::now();
    auto time_t_now = std::chrono::system_clock::to_time_t(now);
    std::tm utc_time = *std::gmtime(&time_t_now);
    std::ostringstream oss;
    oss << std::put_time(&utc_time, "%Y-%m-%dT%H:%M:%SZ");
    return oss.str();
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
                       const char* label) {
    std::filesystem::path tmp = path;
    tmp += ".tmp";

    std::ofstream out(tmp.string(), std::ios::trunc);
    if (!out.is_open()) {
        std::cerr << "Failed to write " << label << " temp file: " << tmp.string() << std::endl;
        return false;
    }
    out << data.dump(2) << std::endl;
    out.close();

    if (set_perms) {
        std::error_code ec;
        std::filesystem::permissions(tmp, perms, std::filesystem::perm_options::replace, ec);
        if (ec) {
            std::cerr << "Failed to set permissions on " << tmp.string() << " ("
                      << ec.message() << ")" << std::endl;
        }
    }

    std::error_code ec;
    std::filesystem::rename(tmp, path, ec);
    if (ec) {
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
    camera_params->num_cameras = num_cameras;
    camera_params->gpu_direct = false;
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
    camera_params->num_cameras = num_cameras;
    camera_params->gpu_direct = false;
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
    camera_params->num_cameras = num_cameras;
    camera_params->gpu_direct = false;
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
    camera_params->num_cameras = num_cameras;
    camera_params->gpu_direct = false;
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
    camera_params->num_cameras = num_cameras;
    camera_params->gpu_direct = false;
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

void select_cameras_have_configs(std::vector<std::string>& camera_config_files, GigEVisionDeviceInfo* device_info, bool* check, int cam_count)
{
    // ... (implementation from project.h)
    for (int i=0; i<cam_count; i++) {
        std::string camera_serial = device_info[i].serialNumber;
        std::string sub_str = camera_serial + ".json";
        auto it = std::find_if(camera_config_files.begin(), camera_config_files.end(), [&](const std::string& str) {return str.find(sub_str) != std::string::npos;});
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
    camera_params->camera_serial.append(device_info->serialNumber);
    camera_params->camera_name = camera_params->camera_serial;
    camera_params->config_path.clear();

    std::string sub_str = camera_params->camera_serial + ".json";
    auto it = std::find_if(camera_config_files.begin(), camera_config_files.end(), [&](const std::string& str) {return str.find(sub_str) != std::string::npos;});

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
        auto config_idx = std::distance(camera_config_files.begin(), it);
        std::cout << "Load camera json file: " << camera_config_files[config_idx] << std::endl;
        camera_params->config_path = camera_config_files[config_idx];
        load_camera_json_config_files(camera_config_files[config_idx], camera_params, camera_idx, num_cameras);
    }
    return true;
}

bool write_recording_snapshot(const std::string& recording_folder,
                              const std::string& recording_id,
                              const CameraParams* cameras_params,
                              int num_cameras,
                              const std::string& base_folder) {
    if (!cameras_params || num_cameras <= 0) {
        return false;
    }

    nlohmann::json snapshot;
    std::string resolved_recording_id = recording_id.empty() ? get_current_date_time() : recording_id;
    std::string timestamp_utc = get_current_utc_timestamp();
    snapshot["recording_id"] = resolved_recording_id;
    snapshot["timestamp_utc"] = timestamp_utc;
    snapshot["producer_version"] = "unknown";

    nlohmann::json cameras = nlohmann::json::object();

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
    }

    snapshot["cameras"] = cameras;

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

void allocate_camera_frame_buffers(CameraEmergent* ecams, CameraParams* cameras_params, int evt_buffer_size, int num_cameras)
{
    // ... (implementation from project.h)
    for (int i = 0; i < num_cameras; i++)
    {
        camera_open_stream(&ecams[i].camera, &cameras_params[i]);
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
