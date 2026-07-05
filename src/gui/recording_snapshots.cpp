#include "gui/recording_snapshots.h"

#include "camera.h"
#include "crop_and_encode_worker.h"
#include "crop_producer.h"
#include "project.h"
#include "video_capture.h"

#include <cstdlib>
#include <iostream>
#include <limits>
#include <string>

namespace {

int resolve_gui_crop_frame_pool_size()
{
    const char* raw = std::getenv("ORANGE_CROP_FRAME_POOL_SIZE");
    if (!raw || !*raw) {
        return CropProducer::kDefaultCropFramePoolSize;
    }
    char* end = nullptr;
    const long parsed = std::strtol(raw, &end, 10);
    if (end == raw || *end != '\0' ||
        parsed < CropProducer::kMinCropFramePoolSize ||
        parsed > CropProducer::kMaxCropFramePoolSize) {
        return CropProducer::kDefaultCropFramePoolSize;
    }
    return static_cast<int>(parsed);
}

int resolve_effective_crop_preview_max_fps(const CameraParams& camera_params)
{
    int resolved = camera_params.crop_pipeline.preview_max_fps;
    const char* env_value = std::getenv("ORANGE_CROP_PREVIEW_MAX_FPS");
    if (env_value && *env_value) {
        char* end = nullptr;
        const long parsed = std::strtol(env_value, &end, 10);
        if (end != env_value && end && *end == '\0') {
            if (parsed > std::numeric_limits<int>::max()) {
                resolved = std::numeric_limits<int>::max();
            } else if (parsed < std::numeric_limits<int>::min()) {
                resolved = std::numeric_limits<int>::min();
            } else {
                resolved = static_cast<int>(parsed);
            }
        }
    }
    return sanitize_camera_crop_preview_max_fps(resolved);
}

}  // namespace

bool gui_camera_has_acquisition_work(const CameraEachSelect& camera_select)
{
    return camera_select.stream_on ||
           camera_select.record ||
           camera_select.yolo ||
           camera_select.crop_and_encode ||
           camera_select.pose ||
           camera_select.frame_save_state == State_Write_New_Frame;
}

nlohmann::json build_gui_detect_model_snapshot(const CameraParams& camera_params,
                                               const CameraEachSelect& camera_select,
                                               const std::string& selected_yolo_model)
{
    const bool enabled = camera_select.yolo;
    std::string selected_engine_path = selected_yolo_model;
    if (enabled && camera_select.yolo_model && camera_select.yolo_model[0] != '\0') {
        selected_engine_path = camera_select.yolo_model;
    }
    const std::string engine_path = enabled ? selected_engine_path : "";
    return {
        {"enabled", enabled},
        {"source", {
            {"ui_selected", camera_select.yolo},
            {"camera_config_path", camera_params.config_path}
        }},
        {"runtime", {
            {"worker", "YoloWorker"},
            {"backend", enabled ? "tensorrt" : "none"},
            {"engine_path", engine_path},
            {"model_id", enabled ? build_model_id_from_path(engine_path) : "none"},
            {"gpu_id", camera_params.gpu_id}
        }}
    };
}

void update_gui_detect_model_snapshots(const std::string& recording_folder,
                                       const CameraParams* cameras_params,
                                       const CameraEachSelect* cameras_select,
                                       const int num_cameras,
                                       const std::string& selected_yolo_model)
{
    if (recording_folder.empty() || !cameras_params || !cameras_select || num_cameras <= 0) {
        return;
    }

    for (int i = 0; i < num_cameras; ++i) {
        std::string camera_key = cameras_params[i].camera_serial;
        if (camera_key.empty()) {
            camera_key = std::to_string(cameras_params[i].camera_id);
        }
        if (!update_recording_snapshot_model(
                recording_folder,
                camera_key,
                "detect",
                build_gui_detect_model_snapshot(
                    cameras_params[i],
                    cameras_select[i],
                    selected_yolo_model))) {
            std::cerr << "Failed to update recording snapshot detect model metadata for camera "
                      << camera_key << std::endl;
        }
    }
}

nlohmann::json build_gui_crop_output_snapshot(const CameraParams& camera_params,
                                              const CameraEachSelect& camera_select,
                                              const int crop_size_px)
{
    const bool enabled = camera_select.crop_and_encode;
    const int resolved_crop_size = CropAndEncodeWorker::SanitizeCropSize(crop_size_px);
    const std::string camera_serial = camera_params.camera_serial;

    nlohmann::json files = nlohmann::json::object();
    if (enabled && !camera_serial.empty()) {
        const std::string prefix = "Cam" + camera_serial + "_crop";
        files = {
            {"video", prefix + ".mp4"},
            {"metadata", prefix + "_meta.csv"},
            {"keyframes", prefix + "_keyframe.json"},
            {"perf", prefix + "_perf.csv"},
            {"sidecar_perf", prefix + "_sidecar_perf.csv"}
        };
    }

    return {
        {"schema_version", 1},
        {"enabled", enabled},
        {"mode", enabled ? "yolo_centered_square" : "disabled"},
        {"source", {
            {"ui_selected", enabled},
            {"requires_yolo", true},
            {"requires_recording", true},
            {"camera_config_path", camera_params.config_path}
        }},
        {"runtime", {
            {"worker", "CropAndEncodeWorker"},
            {"source_gpu_id", camera_params.gpu_id},
            {"crop_size_px", resolved_crop_size},
            {"crop_frame_pool_size", resolve_gui_crop_frame_pool_size()},
            {"preview_max_fps", resolve_effective_crop_preview_max_fps(camera_params)},
            {"width", enabled ? resolved_crop_size : 0},
            {"height", enabled ? resolved_crop_size : 0},
            {"coordinate_space", "full_frame_pixels"},
            {"selection_policy", "largest_detection_by_confidence"},
            {"blank_frame_policy", "encode_black_frame_when_no_detection"},
            {"codec", enabled ? "hevc" : "none"},
            {"container", enabled ? "mp4" : "none"},
            {"tuning", enabled ? "lossless" : "none"},
            {"frame_rate", camera_params.frame_rate},
            {"files", files}
        }}
    };
}

void update_gui_crop_output_snapshots(const std::string& recording_folder,
                                      const CameraParams* cameras_params,
                                      const CameraEachSelect* cameras_select,
                                      const int num_cameras,
                                      const int crop_size_px)
{
    if (recording_folder.empty() || !cameras_params || !cameras_select || num_cameras <= 0) {
        return;
    }

    for (int i = 0; i < num_cameras; ++i) {
        std::string camera_key = cameras_params[i].camera_serial;
        if (camera_key.empty()) {
            camera_key = std::to_string(cameras_params[i].camera_id);
        }
        if (!update_recording_snapshot_crop_output(
                recording_folder,
                camera_key,
                build_gui_crop_output_snapshot(
                    cameras_params[i],
                    cameras_select[i],
                    crop_size_px))) {
            std::cerr << "Failed to update recording snapshot crop output metadata for camera "
                      << camera_key << std::endl;
        }
    }
}

nlohmann::json build_gui_pose_model_snapshot(const CameraParams& camera_params,
                                             const CameraEachSelect& camera_select)
{
    const bool enabled = camera_select.pose;
    const std::string camera_serial = camera_params.camera_serial;

    nlohmann::json files = nlohmann::json::object();
    if (enabled && !camera_serial.empty()) {
        files = {
            {"perf", "Cam" + camera_serial + "_pose_perf.csv"},
            {"events", "Cam" + camera_serial + "_pose_events.jsonl"}
        };
    }
    std::string pose_engine_path;
    if (const char* env_pose_engine_path = std::getenv("ORANGE_POSE_ENGINE_PATH")) {
        pose_engine_path = env_pose_engine_path;
    }
    std::string pose_skeleton_id = "unknown";
    if (const char* env_pose_skeleton_id = std::getenv("ORANGE_POSE_SKELETON_ID")) {
        pose_skeleton_id = env_pose_skeleton_id;
    }
    std::string pose_skeleton_path;
    if (const char* env_pose_skeleton_path = std::getenv("ORANGE_POSE_SKELETON_PATH")) {
        pose_skeleton_path = env_pose_skeleton_path;
    }

    return {
        {"enabled", enabled},
        {"source", {
            {"ui_selected", enabled},
            {"requires_yolo", true},
            {"requires_crop_output", true},
            {"camera_config_path", camera_params.config_path}
        }},
        {"runtime", {
            {"worker", "PoseWorker"},
            {"backend", enabled ? "noop" : "none"},
            {"mode", enabled ? "noop" : "disabled"},
            {"engine_path", enabled ? pose_engine_path : ""},
            {"model_id", enabled && !pose_engine_path.empty() ? build_model_id_from_path(pose_engine_path) : "none"},
            {"skeleton_id", enabled ? pose_skeleton_id : "none"},
            {"skeleton_path", enabled ? pose_skeleton_path : ""},
            {"gpu_id", camera_params.gpu_id},
            {"queue_size", enabled ? 32 : 0},
            {"files", files}
        }}
    };
}

void update_gui_pose_model_snapshots(const std::string& recording_folder,
                                     const CameraParams* cameras_params,
                                     const CameraEachSelect* cameras_select,
                                     const int num_cameras)
{
    if (recording_folder.empty() || !cameras_params || !cameras_select || num_cameras <= 0) {
        return;
    }

    for (int i = 0; i < num_cameras; ++i) {
        std::string camera_key = cameras_params[i].camera_serial;
        if (camera_key.empty()) {
            camera_key = std::to_string(cameras_params[i].camera_id);
        }
        if (!update_recording_snapshot_model(
                recording_folder,
                camera_key,
                "pose",
                build_gui_pose_model_snapshot(cameras_params[i], cameras_select[i]))) {
            std::cerr << "Failed to update recording snapshot pose model metadata for camera "
                      << camera_key << std::endl;
        }
    }
}

std::string spatial_calibration_artifact_env_name(const std::string& camera_serial)
{
    return "ORANGE_SPATIAL_CALIBRATION_ARTIFACT_" + camera_serial;
}

std::string resolve_gui_spatial_calibration_artifact_path(const std::string& camera_serial)
{
    if (camera_serial.empty()) {
        return {};
    }
    const std::string env_name = spatial_calibration_artifact_env_name(camera_serial);
    const char* value = std::getenv(env_name.c_str());
    if (!value || value[0] == '\0') {
        return {};
    }
    return value;
}

void update_gui_spatial_calibration_snapshots(const std::string& recording_folder,
                                              const CameraParams* cameras_params,
                                              const CameraEachSelect* cameras_select,
                                              const int num_cameras)
{
    if (recording_folder.empty() || !cameras_params || !cameras_select || num_cameras <= 0) {
        return;
    }

    for (int i = 0; i < num_cameras; ++i) {
        if (!gui_camera_has_acquisition_work(cameras_select[i])) {
            continue;
        }

        std::string camera_key = cameras_params[i].camera_serial;
        if (camera_key.empty()) {
            camera_key = std::to_string(cameras_params[i].camera_id);
        }

        const std::string artifact_path = resolve_gui_spatial_calibration_artifact_path(camera_key);
        if (artifact_path.empty()) {
            continue;
        }

        std::string error;
        if (!update_recording_snapshot_spatial_calibration_from_artifact(
                recording_folder,
                camera_key,
                artifact_path,
                &error)) {
            std::cerr << "Failed to update recording snapshot spatial calibration for camera "
                      << camera_key << " from " << artifact_path;
            if (!error.empty()) {
                std::cerr << ": " << error;
            }
            std::cerr << std::endl;
            continue;
        }

        std::cout << "Recording snapshot spatial calibration for camera "
                  << camera_key << " loaded from " << artifact_path << std::endl;
    }
}
