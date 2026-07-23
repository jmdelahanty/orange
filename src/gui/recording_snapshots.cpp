#include "gui/recording_snapshots.h"
#include "citrus_recording_geometry.h"
#include "gui/spatial_layout/projection_snapshot_client.h"

#include "camera.h"
#include "crop_and_encode_worker.h"
#include "crop_producer.h"
#include "project.h"
#include "spatial_calibration_snapshot.h"
#include "spatial_layout_schema.h"
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

std::string resolve_gui_citrus_recording_canvas_config_path(
    const std::string& ui_selected_path,
    std::string* source_out)
{
    struct Candidate {
        const char* env_name;
        const char* source;
    };
    static constexpr Candidate candidates[] = {
        {"ORANGE_CITRUS_RECORDING_CANVAS_CONFIG_PATH", "explicit_recording_environment"},
        {"ORANGE_GUI_GUIDED_CAPTURE_CITRUS_CONFIG_PATH", "guided_capture_environment"},
        {"ORANGE_GUI_ARENA_CENTERING_CITRUS_CONFIG_PATH", "arena_centering_environment"},
    };
    for (const Candidate& candidate : candidates) {
        const char* value = std::getenv(candidate.env_name);
        if (value != nullptr && value[0] != '\0') {
            if (source_out) *source_out = candidate.source;
            return value;
        }
    }
    if (!ui_selected_path.empty()) {
        if (source_out) *source_out = "spatial_layout_ui_selection";
        return ui_selected_path;
    }
    if (source_out) *source_out = "none";
    return {};
}

nlohmann::json build_gui_recording_geometry_contract(
    const std::string& ui_selected_citrus_canvas_config_path,
    const CameraParams* cameras_params,
    const CameraEachSelect* cameras_select,
    const int num_cameras)
{
    if (!cameras_params || !cameras_select || num_cameras <= 0) {
        return {
            {"schema_id", "orange.recording.geometry_contract"},
            {"schema_version", 1},
            {"status", "invalid_request"},
            {"cameras", nlohmann::json::object()},
            {"warnings", nlohmann::json::array({
                "GUI recording geometry contract request had no cameras."})},
        };
    }

    orange::recording_geometry::CitrusGeometryResolveRequest request;
    request.selected_canvas_config_path =
        resolve_gui_citrus_recording_canvas_config_path(
            ui_selected_citrus_canvas_config_path,
            &request.selection_source);
    request.captured_at_utc = get_current_utc_timestamp();
    for (int index = 0; index < num_cameras; ++index) {
        if (!gui_camera_has_acquisition_work(cameras_select[index])) {
            continue;
        }
        std::string serial = cameras_params[index].camera_serial;
        if (serial.empty()) {
            serial = std::to_string(cameras_params[index].camera_id);
        }
        request.camera_serials.push_back(std::move(serial));
    }

    auto resolution =
        orange::recording_geometry::resolve_citrus_recording_geometry(request);
    bool has_orange_spatial_calibration = false;
    for (const std::string& serial : request.camera_serials) {
        nlohmann::json& camera = resolution.contract["cameras"][serial];
        const std::string artifact_path =
            resolve_gui_spatial_calibration_artifact_path(serial);
        if (artifact_path.empty()) {
            camera["orange_spatial_calibration"] = {
                {"status", "not_configured"},
            };
            continue;
        }
        orange::spatial::CameraSpatialCalibration calibration;
        std::string error;
        if (!orange::spatial::load_camera_spatial_calibration_from_artifact_dir(
                artifact_path, &calibration, &error)) {
            camera["orange_spatial_calibration"] = {
                {"status", "invalid"},
                {"source_artifact_dir", artifact_path},
                {"error", error},
            };
            resolution.contract["warnings"].push_back(
                "Orange spatial calibration for camera " + serial +
                " could not be embedded: " + error);
            continue;
        }
        camera["orange_spatial_calibration"] = {
            {"status", "resolved"},
            {"source_artifact_dir", artifact_path},
            {"runtime", orange::spatial::camera_spatial_calibration_to_json(
                calibration)},
        };
        has_orange_spatial_calibration = true;
    }
    if (!resolution.configured && has_orange_spatial_calibration) {
        resolution.contract["status"] = "orange_only";
    }
    return resolution.contract;
}

bool write_gui_recording_geometry_contract(
    const std::string& recording_folder,
    const nlohmann::json& contract,
    std::string* error_out)
{
    if (recording_folder.empty()) {
        if (error_out) {
            *error_out = "recording folder is empty";
        }
        return false;
    }
    std::string local_error;
    if (!write_recording_geometry_contract(
            recording_folder, contract, &local_error)) {
        if (error_out) {
            *error_out = local_error;
        }
        return false;
    }
    if (error_out) {
        error_out->clear();
    }
    std::cout << "Recording geometry contract written"
              << " status=" << contract.value("status", "unknown")
              << " citrus_canvas="
              << contract.value(
                    "selection", nlohmann::json::object()).value(
                        "selected_canvas_name", "none")
              << " folder=" << recording_folder << std::endl;
    return true;
}

void update_gui_recording_geometry_contract(
    const std::string& recording_folder,
    const std::string& ui_selected_citrus_canvas_config_path,
    const CameraParams* cameras_params,
    const CameraEachSelect* cameras_select,
    const int num_cameras)
{
    if (recording_folder.empty()) {
        return;
    }
    const nlohmann::json contract = build_gui_recording_geometry_contract(
        ui_selected_citrus_canvas_config_path,
        cameras_params,
        cameras_select,
        num_cameras);

    std::string error;
    if (!write_gui_recording_geometry_contract(
            recording_folder, contract, &error)) {
        std::cerr << "Failed to write recording geometry contract";
        if (!error.empty()) {
            std::cerr << ": " << error;
        }
        std::cerr << std::endl;
        return;
    }
}

void update_gui_citrus_runtime_geometry_snapshot(
    const std::string& recording_folder)
{
    if (recording_folder.empty()) {
        return;
    }
    const auto result = orange::gui::spatial_layout::
        query_citrus_daily_registration_status("recording-start");
    nlohmann::json snapshot = {
        {"schema_id", "orange.recording.citrus_runtime_geometry"},
        {"schema_version", 1},
        {"captured_at_utc", get_current_utc_timestamp()},
        {"capture_status", result.ok ? "captured" : "unavailable"},
        {"recording_blocked_by_capture_failure", false},
        {"daily_registration_optional", true},
        {"mode", "unknown"},
        {"daily_registration_status", "unavailable"},
        {"all_selected_runtime_safe", true},
    };
    if (result.ok) {
        const auto runtime = result.daily_registration.value(
            "runtime", nlohmann::json::object());
        snapshot["mode"] = runtime.value("mode", "base_only");
        snapshot["daily_registration_status"] = runtime.value(
            "daily_registration_status", "not_performed");
        snapshot["all_selected_runtime_safe"] = runtime.value(
            "all_selected_runtime_safe", true);
        snapshot["daily_registration"] = result.daily_registration;
        const auto response_status = result.response.value(
            "status", nlohmann::json::object());
        snapshot["rig_canvas_commissioning"] = response_status.value(
            "runtime_rig_canvas_commissioning_compatibility",
            nlohmann::json::object());
    } else {
        snapshot["capture_error"] = result.reason;
        snapshot["policy"] =
            "continue_recording_and_mark_citrus_runtime_geometry_unavailable";
    }
    if (!update_recording_snapshot_citrus_runtime_geometry(
            recording_folder, snapshot)) {
        std::cerr << "Failed to update recording snapshot Citrus runtime "
                     "geometry metadata" << std::endl;
    }
}
