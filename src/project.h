// src/project.h
#ifndef ORANGE_PROJECT
#define ORANGE_PROJECT

#include <string>
#include <vector>
#include <chrono>
#include "network_base.h" // For EnetContext, ENetPeer, FetchGame::ManagerState (via fetch_generated.h)
#include "camera.h"     // For CameraParams, GigEVisionDeviceInfo, CameraEmergent
#include "recording_validation.h"
#include "json.hpp" // For JSON handling (nlohmann::json)
#include <filesystem> // For filesystem operations
#include <fstream>   // For file operations
#include <iostream>  // For console output
#include <map>

struct ConnectedServer {
    char name[80];
    uint8_t ip_add[4];
    uint16_t port;
    ENetPeer* peer;
    int num_cameras;
    FetchGame::ManagerState server_state;
    bool connected;
};

struct AppStorageConfig {
    std::string schema_id;
    int schema_version = 0;
    std::string default_detect_engine;
    std::string default_recording_root;
    std::string gui_recording_sink_mode = "real";
    bool gui_recording_sink_mode_configured = false;
    int gui_recording_record_for_seconds = 0;
    int gui_recording_clip_seconds = 0;
    std::string gui_crop_recording_sink_mode = "in_process";
    int gui_crop_external_encode_queue_depth = -1;
    int gui_crop_external_recorder_gpu_id = -1;
    std::map<std::string, int> gui_crop_external_recorder_gpu_ids_by_serial;
    int gui_crop_frame_pool_size = -1;
    std::string gui_external_recorder_contract_path;
    nlohmann::json gui_external_recorder_contract = nlohmann::json::object();
    int gui_ptp_register_read_decimate = 1;
    int gui_stream_downsample = -1;
    std::string gui_display_profile;
    int gui_display_preview_max_fps = -1;
    int gui_swap_interval = -1;
    int gui_frame_max_fps = -1;
    bool gui_show_speed_graphs = false;
    bool gui_local_control_recording_start_enabled = false;
    bool gui_local_control_recording_stop_enabled = false;
    bool gui_local_control_citrus_completion_stop_enabled = false;
    bool gui_local_control_exit_after_finalize = false;
    int gui_local_control_drain_timeout_seconds = -1;
    bool write_local_pointer = true;
    std::string canonical_pointer_root;
    bool write_run_pointer = true;
    std::string run_pointer_path;
};

// Function Declarations
void prepare_application_folders(std::string orange_root_dir_str);
std::string build_default_orange_root_dir(std::string* warning_out = nullptr);
std::string build_default_app_config_path(const std::string& orange_root_dir_str);
bool load_app_storage_config(const std::string& orange_root_dir_str,
                             AppStorageConfig* config_out,
                             std::string* error_out = nullptr);
std::string resolve_default_detect_engine(const std::string& orange_root_dir_str,
                                          std::string* warning_out = nullptr);
std::string resolve_default_recording_root(const std::string& orange_root_dir_str,
                                           std::string* warning_out = nullptr);
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
                              bool update_latest_pointer = true,
                              bool sync_camera_enabled = false,
                              const PTPParams* ptp_params = nullptr,
                              const std::string& recording_sink_mode = "real");
nlohmann::json build_gpu_runtime_info(int gpu_id);
RecordingValidationGpuPathInfo build_recording_validation_gpu_path_info(int source_gpu_id,
                                                                        int helper_gpu_id);
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
bool update_recording_snapshot_pipeline_metrics(const std::string& recording_folder,
                                                const std::string& camera_serial,
                                                const nlohmann::json& pipeline_info);
bool update_recording_snapshot_gpu_monitoring(const std::string& recording_folder,
                                              const std::string& monitor_name,
                                              const nlohmann::json& monitor_info);
bool update_recording_snapshot_session_artifacts(const std::string& recording_folder,
                                                 const nlohmann::json& session_info);
bool update_recording_snapshot_recording_outputs(const std::string& recording_folder,
                                                 const nlohmann::json& recording_outputs);
bool update_recording_snapshot_model(const std::string& recording_folder,
                                     const std::string& camera_serial,
                                     const std::string& model_kind,
                                     const nlohmann::json& model_info);
bool update_recording_snapshot_crop_output(const std::string& recording_folder,
                                           const std::string& camera_serial,
                                           const nlohmann::json& crop_output_info);
bool update_recording_snapshot_spatial_calibration(const std::string& recording_folder,
                                                   const std::string& camera_serial,
                                                   const nlohmann::json& spatial_calibration);
bool update_recording_snapshot_spatial_calibration_from_artifact(const std::string& recording_folder,
                                                                 const std::string& camera_serial,
                                                                 const std::string& artifact_dir,
                                                                 std::string* error_out = nullptr);
std::string build_model_id_from_path(const std::string& model_path);
nlohmann::json build_gpu_copy_path_static_topology_info(int source_gpu_id, int target_gpu_id);
std::string lookup_nvidia_smi_topology_class(int source_gpu_id,
                                             int target_gpu_id,
                                             std::string* error_out = nullptr);
bool parse_recording_strategy_json(const nlohmann::json& recording_json,
                                   RecordingStrategyConfig* recording_strategy_out,
                                   std::string* error_out = nullptr);
nlohmann::json build_recording_strategy_json(const RecordingStrategyConfig& recording_strategy);
bool parse_camera_recording_json(const nlohmann::json& recording_json,
                                 CameraRecordingConfig* recording_out,
                                 std::string* error_out = nullptr);
nlohmann::json build_camera_recording_json(const CameraRecordingConfig& recording);
RecordingOutputConfig resolve_effective_recording_output_config(
    const CameraParams& camera_params,
    const CameraRecordingOutputConfig& requested_output,
    std::string* warning_out = nullptr);
ResolvedRecordingConfig build_resolved_recording_config(
    const CameraParams& camera_params,
    const ResolvedRecordingConfigOverrides& overrides = {});
bool read_camera_config_snapshot(const CameraParams& camera_params,
                                 std::string* config_contents,
                                 std::string* error_out);
bool save_camera_json_config(const CameraParams& camera_params,
                             std::string* error_out);
bool update_calibration_artifact_registry(const std::string& artifact_root_dir,
                                          const nlohmann::json& manifest,
                                          std::string* error_out);

#endif // ORANGE_PROJECT
