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
