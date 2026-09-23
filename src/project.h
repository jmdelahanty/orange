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
    int gui_crop_external_interleave = -1;  // recording.crop.external_ipc.interleave -> ORANGE_CROP_EXTERNAL_INTERLEAVE (GOP-parity two-shard crops)
    int gui_crop_frame_pool_size = -1;
    // recording.external_ipc: the validated full-frame external recorder
    // shape (2026-09-21 gate). Exported as env if absent; env wins.
    bool gui_external_ipc_owner_push = false;
    bool gui_external_ipc_owner_push_configured = false;
    int gui_external_ipc_owner_push_slots = -1;
    int gui_external_ipc_full_frame_extra_output_delay = -1;
    bool gui_external_ipc_native_local_input = false;
    bool gui_external_ipc_native_local_input_configured = false;
    // TEST ONLY: corrupt one PREPARE handle so the recorder reports a failed
    // import and the strict GUI readiness gate must refuse the start.
    bool gui_external_ipc_prepare_fault_inject = false;
    std::string gui_external_ipc_native_kernel_ptx;
    std::string gui_external_ipc_recorder_tool_path;
    // analytics.*: the fused analytics shape the 2026-09-21 gate used
    // (device ROI, device crop, fused per-slot graph, copy after pose).
    // Exported as ORANGE_ANALYTICS_* env if absent; -1 = not configured.
    int gui_analytics_device_roi = -1;
    int gui_analytics_device_crop = -1;
    int gui_analytics_fused_frame = -1;
    int gui_analytics_copy_after_pose = -1;
    int gui_analytics_early_owned_frame = -1;
    int gui_analytics_copy_stream = -1;
    // analytics.yolo.*
    int gui_analytics_yolo_prewarm_iterations = -1;   // ORANGE_YOLO_PREWARM_ITERATIONS (fused graph capture)
    int gui_analytics_yolo_decimate = -1;             // ORANGE_YOLO_DECIMATE
    int gui_analytics_yolo_sync_event = -1;
    int gui_analytics_yolo_gpu_timing = -1;
    int gui_analytics_yolo_detach_input = -1;
    int gui_analytics_yolo_ready_event_fastpath = -1;
    // analytics.pose.*
    int gui_analytics_pose_device_stage = -1;         // ORANGE_POSE_DEVICE_STAGE (GUI EnableDeviceStage gate)
    int gui_analytics_pose_prewarm_iterations = -1;   // ORANGE_POSE_PREWARM_ITERATIONS
    int gui_analytics_pose_device_graph = -1;         // ORANGE_POSE_DEVICE_GRAPH
    int gui_analytics_pose_device_slots = -1;         // ORANGE_POSE_DEVICE_SLOTS
    int gui_analytics_pose_queue_depth = -1;          // ORANGE_POSE_QUEUE_DEPTH
    // models.pose_*: pose worker (ORANGE_POSE_ENGINE_PATH / _MODE /
    // _SKELETON_ID / _CROP_SIZE_PX if absent).
    std::string pose_engine_path;
    std::string pose_mode;
    std::string pose_skeleton_id;
    std::string pose_skeleton_path;  // models.pose_skeleton_path -> ORANGE_POSE_SKELETON_PATH
    int pose_crop_size_px = -1;
    std::string gui_external_recorder_contract_path;
    nlohmann::json gui_external_recorder_contract = nlohmann::json::object();
    int gui_ptp_register_read_decimate = 1;
    int gui_stream_downsample = -1;
    std::string gui_display_profile;
    int gui_display_preview_max_fps = -1;
    // Diagnostic (2026-09-22): when > 0 (the recording GOP length), the
    // full-frame preview skips frames whose GOP is routed to the peer shard,
    // so the 20 MB display read never overlaps an owner push on the same die.
    int gui_display_skip_pushed_gops = 0;
    // Pose keypoint overlay on the previews (ORANGE_GUI_POSE_OVERLAY).
    bool gui_display_pose_overlay = false;
    // gui.exposure_check: absolute brightness check at stream start against a
    // per-camera expected preview mean (the EF iris is an open-loop stepper;
    // only the image proves the glass is where the counter says).
    bool gui_exposure_check_enabled = false;
    double gui_exposure_check_tolerance_fraction = 0.15;
    bool gui_exposure_check_rehome_iris = true;
    std::map<std::string, double> gui_exposure_check_expected_preview_mean_by_serial;
    int gui_swap_interval = -1;
    int gui_frame_max_fps = -1;
    bool gui_show_speed_graphs = false;
    bool gui_incremental_clip_shadow = false;
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

struct CameraEachSelect;

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
bool create_unique_timestamped_folder(const std::string& base_folder,
                                      const std::string& timestamp_id,
                                      std::string* folder_out,
                                      std::string* id_out,
                                      std::string* error_out = nullptr);
void list_child_directories(const std::string& root_folder, std::vector<std::string>& child_directories);
void update_camera_configs(std::vector<std::string>& camera_config_files, std::string input_folder);
void select_cameras_have_configs(std::vector<std::string>& camera_config_files, GigEVisionDeviceInfo* device_info, bool* check, int cam_count);
bool set_camera_params(CameraParams* camera_params, GigEVisionDeviceInfo* device_info, std::vector<std::string>& camera_config_files, int camera_idx, int num_cameras);
std::string build_camera_config_path(const std::string& config_folder, const CameraParams& camera_params);
void assign_camera_config_paths(CameraParams* cameras_params, int num_cameras, const std::string& config_folder);
bool save_camera_json_configs_to_folder(CameraParams* cameras_params,
                                        int num_cameras,
                                        const std::string& config_folder,
                                        std::string* error_out = nullptr);
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
                              const std::string& recording_sink_mode = "real",
                              const ResolvedRecordingConfig* resolved_recording_configs = nullptr,
                              int num_resolved_recording_configs = 0,
                              const CameraEachSelect* cameras_select = nullptr);
// Seal the current recording_snapshot.json bytes into the create-once
// recording_snapshot_start.json artifact. The mutable snapshot receives a
// digest-bound reference, but later snapshot enrichment never rewrites the
// sealed file. Repeated calls verify and return the existing artifact.
bool seal_immutable_recording_start_snapshot(
    const std::string& recording_folder,
    nlohmann::json* reference_out = nullptr,
    std::string* error_out = nullptr);
bool update_recording_snapshot_observation_binding_requests(
    const std::string& recording_folder,
    const nlohmann::json& request_collection,
    std::string* error_out = nullptr);
bool update_recording_snapshot_observation_binding_pre_arm(
    const std::string& recording_folder,
    const nlohmann::json& pre_arm_decision,
    std::string* error_out = nullptr);
bool publish_latest_recording_pointer(const std::string& base_folder,
                                      const std::string& recording_folder,
                                      const std::string& recording_id);
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
bool update_recording_snapshot_system_monitoring(const std::string& recording_folder,
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
bool update_recording_snapshot_citrus_runtime_geometry(
    const std::string& recording_folder,
    const nlohmann::json& citrus_runtime_geometry);
bool write_recording_geometry_contract(
    const std::string& recording_folder,
    const nlohmann::json& geometry_contract,
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
