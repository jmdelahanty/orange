// src/project.h
#ifndef ORANGE_PROJECT
#define ORANGE_PROJECT

#include <string>
#include <vector>
#include <chrono>
#include "network_base.h" // For EnetContext, ENetPeer, FetchGame::ManagerState (via fetch_generated.h)
#include "camera.h"     // For CameraParams, GigEVisionDeviceInfo, CameraEmergent
#include "json.hpp" // For JSON handling (nlohmann::json)
#include <filesystem> // For filesystem operations
#include <fstream>   // For file operations
#include <iostream>  // For console output

struct ConnectedServer {
    char name[80];
    uint8_t ip_add[4];
    uint16_t port;
    ENetPeer* peer;
    int num_cameras;
    FetchGame::ManagerState server_state;
    bool connected;
};

// Function Declarations
void prepare_application_folders(std::string orange_root_dir_str);
void intialize_servers(ConnectedServer* my_servers);
std::vector<std::string> string_split(std::string s, std::string delimiter);
std::vector<std::string> string_split_char(char* string_c, std::string delimiter);
void load_camera_json_config_files(std::string file_name, CameraParams* camera_params, int camera_id, int num_cameras);
std::string get_current_utc_timestamp();
std::string get_current_time_milliseconds();
std::string get_current_date();
std::string get_current_date_time();
std::string format_elapsed_time(std::chrono::seconds elapsed_seconds);
void init_galvo_camera_params(CameraParams* camera_params, int camera_id, int num_cameras, int gain, int exposure);
void init_65MP_camera_params_mono(CameraParams* camera_params, int camera_id, int num_cameras, int gain, int exposure, int gpu_id, int frame_rate);
void init_65MP_camera_params_color(CameraParams* camera_params, int camera_id, int num_cameras, int gain, int exposure, int gpu_id, int frame_rate);
void init_7MP_camera_params_color(CameraParams* camera_params, int camera_id, int num_cameras, int gain, int exposure, int gpu_id, int frame_rate);
void init_7MP_camera_params_mono(CameraParams* camera_params, int camera_id, int num_cameras, int gain, int exposure, int gpu_id, int frame_rate);
bool make_folder(std::string folder_name);
bool ensure_directory_exists(const std::string& folder_name, std::string* error_out = nullptr);
void list_child_directories(const std::string& root_folder, std::vector<std::string>& child_directories);
void update_camera_configs(std::vector<std::string>& camera_config_files, std::string input_folder);
void select_cameras_have_configs(std::vector<std::string>& camera_config_files, GigEVisionDeviceInfo* device_info, bool* check, int cam_count);
bool set_camera_params(CameraParams* camera_params, GigEVisionDeviceInfo* device_info, std::vector<std::string>& camera_config_files, int camera_idx, int num_cameras);
std::string build_camera_config_path(const std::string& config_folder, const CameraParams& camera_params);
void assign_camera_config_paths(CameraParams* cameras_params, int num_cameras, const std::string& config_folder);
void allocate_camera_frame_buffers(CameraEmergent* ecams, CameraParams* cameras_params, int evt_buffer_size, int num_cameras);
void client_send_bringup_message(EnetContext* enet_context, flatbuffers::FlatBufferBuilder* builder, ENetPeer *server_connection, int cam_count, FetchGame::ManagerState server_state);
void client_send_state_update_message(EnetContext* enet_context, flatbuffers::FlatBufferBuilder* builder, ENetPeer *server_connection, FetchGame::ManagerState server_state);
void host_broadcast_open_cameras(flatbuffers::FlatBufferBuilder* builder, EnetContext* server, std::string config_file_name);
void host_broadcast_start_threads(flatbuffers::FlatBufferBuilder* builder, EnetContext* server, std::string record_folder_name, std::string encoder_basic_setup);
void host_broadcast_set_start_ptp(flatbuffers::FlatBufferBuilder* builder, EnetContext* server, unsigned long long ptp_global_time);
bool write_recording_snapshot(const std::string& recording_folder,
                              const std::string& recording_id,
                              const CameraParams* cameras_params,
                              int num_cameras,
                              const std::string& base_folder,
                              bool sync_camera_enabled = false,
                              const PTPParams* ptp_params = nullptr);
bool initialize_ptp_sync_summary(const std::string& recording_folder,
                                 const std::string& recording_id,
                                 int num_cameras,
                                 bool sync_camera_enabled,
                                 const PTPParams* ptp_params);
bool update_ptp_sync_summary_camera(const std::string& recording_folder,
                                    const std::string& camera_serial,
                                    const nlohmann::json& camera_summary);
bool update_recording_snapshot_encoder(const std::string& recording_folder,
                                       const std::string& camera_serial,
                                       const nlohmann::json& encoder_info);
bool read_camera_config_snapshot(const CameraParams& camera_params,
                                 std::string* config_contents,
                                 std::string* error_out);
bool save_camera_json_config(const CameraParams& camera_params,
                             std::string* error_out);
bool update_calibration_artifact_registry(const std::string& artifact_root_dir,
                                          const nlohmann::json& manifest,
                                          std::string* error_out);

#endif // ORANGE_PROJECT
