#pragma once

#include "json.hpp"

#include <string>

struct CameraEachSelect;
struct CameraParams;

bool gui_camera_has_acquisition_work(const CameraEachSelect& camera_select);

nlohmann::json build_gui_detect_model_snapshot(const CameraParams& camera_params,
                                               const CameraEachSelect& camera_select,
                                               const std::string& selected_yolo_model);

void update_gui_detect_model_snapshots(const std::string& recording_folder,
                                       const CameraParams* cameras_params,
                                       const CameraEachSelect* cameras_select,
                                       int num_cameras,
                                       const std::string& selected_yolo_model);

nlohmann::json build_gui_crop_output_snapshot(const CameraParams& camera_params,
                                              const CameraEachSelect& camera_select,
                                              int crop_size_px);

void update_gui_crop_output_snapshots(const std::string& recording_folder,
                                      const CameraParams* cameras_params,
                                      const CameraEachSelect* cameras_select,
                                      int num_cameras,
                                      int crop_size_px);

nlohmann::json build_gui_pose_model_snapshot(const CameraParams& camera_params,
                                             const CameraEachSelect& camera_select);

void update_gui_pose_model_snapshots(const std::string& recording_folder,
                                     const CameraParams* cameras_params,
                                     const CameraEachSelect* cameras_select,
                                     int num_cameras);

std::string spatial_calibration_artifact_env_name(const std::string& camera_serial);

std::string resolve_gui_spatial_calibration_artifact_path(const std::string& camera_serial);

void update_gui_spatial_calibration_snapshots(const std::string& recording_folder,
                                              const CameraParams* cameras_params,
                                              const CameraEachSelect* cameras_select,
                                              int num_cameras);

std::string resolve_gui_citrus_recording_canvas_config_path(
    const std::string& ui_selected_path,
    std::string* source_out = nullptr);

// Writes the always-present Orange recording geometry contract. A selected
// Citrus canvas contributes immutable commissioning metadata without requiring
// a live Citrus process; missing/invalid optional metadata is recorded and is
// deliberately non-blocking.
nlohmann::json build_gui_recording_geometry_contract(
    const std::string& ui_selected_citrus_canvas_config_path,
    const CameraParams* cameras_params,
    const CameraEachSelect* cameras_select,
    int num_cameras);

bool write_gui_recording_geometry_contract(
    const std::string& recording_folder,
    const nlohmann::json& contract,
    std::string* error_out = nullptr);

void update_gui_recording_geometry_contract(
    const std::string& recording_folder,
    const std::string& ui_selected_citrus_canvas_config_path,
    const CameraParams* cameras_params,
    const CameraEachSelect* cameras_select,
    int num_cameras);

// Best-effort, non-blocking snapshot of Citrus commissioning/daily-selection
// authority. Citrus unavailability is recorded explicitly and never prevents
// Orange recording.
void update_gui_citrus_runtime_geometry_snapshot(
    const std::string& recording_folder);
