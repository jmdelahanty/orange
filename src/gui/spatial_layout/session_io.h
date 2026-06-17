#pragma once

#include "gui/spatial_layout/state.h"
#include "json.hpp"

#include <filesystem>
#include <string>

namespace cv {
class Mat;
}

namespace orange::gui::spatial_layout {

inline constexpr const char* kCalibrationManifestSchemaId = "orange.calibration.manifest";
inline constexpr int kCalibrationManifestSchemaVersion = 1;
inline constexpr const char* kCalibrationFingerprintAlgorithm = "fnv1a64";
inline constexpr const char* kSpatialLayoutMeasurementFilename = "measurement.json";
inline constexpr const char* kSpatialLayoutManifestFilename = "manifest.json";
inline constexpr const char* kSpatialLayoutArenaLayoutRuntimeFilename = "arena_layout_runtime.json";
inline constexpr const char* kSpatialLayoutDishMaskRuntimeFilename = "dish_mask_runtime.json";
inline constexpr const char* kCalibrationSessionFilename = "session.json";
inline constexpr const char* kCalibrationSessionIndexFilename = "session_index.json";
inline constexpr const char* kCalibrationSessionArenaLayoutSetFilename = "arena_layout_set.json";

struct SpatialLayoutPersistedFiles {
    std::filesystem::path artifact_dir;
    std::filesystem::path measurement_path;
    std::filesystem::path manifest_path;
    std::filesystem::path arena_layout_runtime_path;
    std::filesystem::path dish_mask_runtime_path;
};

struct GenericCalibrationImageSetFiles {
    std::filesystem::path artifact_dir;
    std::filesystem::path image_set_path;
    std::filesystem::path manifest_path;
    std::filesystem::path source_frame_path;
    std::filesystem::path source_frame_relative_path;
};

std::string sanitize_artifact_component(const std::string& value);

std::string compute_json_fingerprint(const nlohmann::json& value);
std::string compute_file_fingerprint(const std::filesystem::path& path, std::string* error_out);

bool write_image_file(const std::filesystem::path& path, const cv::Mat& image, std::string* error_out);
bool write_json_file(const std::filesystem::path& path,
                     const nlohmann::json& value,
                     std::string* error_out);
bool read_json_file(const std::filesystem::path& path,
                    nlohmann::json* value,
                    std::string* error_out);

std::filesystem::path calibration_base_dir_from_artifact_root(const std::string& artifact_root_dir);
std::filesystem::path calibration_sessions_dir_from_artifact_root(const std::string& artifact_root_dir);

std::string build_spatial_calibration_session_id(
    const SpatialLayoutUiState* ui_state,
    const std::string& timestamp);

bool ensure_directory_for_spatial_session(const std::filesystem::path& path, std::string* error_out);

bool write_spatial_calibration_session_manifest(
    const SpatialLayoutUiState* ui_state,
    const CameraParams& selected_camera,
    const std::string& artifact_root_dir,
    const std::filesystem::path& session_dir,
    const std::filesystem::path& session_artifact_root,
    std::string* error_out);

bool ensure_spatial_calibration_session(
    SpatialLayoutUiState* ui_state,
    const CameraParams& selected_camera,
    const std::string& artifact_root_dir,
    std::string* session_artifact_root_out,
    std::string* error_out);

void clear_spatial_calibration_session(SpatialLayoutUiState* ui_state);

bool update_spatial_calibration_session_index(
    const std::string& session_dir_string,
    const std::string& session_artifact_root_string,
    const nlohmann::json& manifest,
    std::string* error_out);

std::string build_arena_layout_artifact_id(
    const std::string& prefix_base,
    const CameraParams& camera_params,
    const std::string& timestamp_label);

std::string build_camera_arena_calibration_image_set_artifact_id(
    const SpatialLayoutUiState* ui_state,
    const CameraParams& camera_params);

std::string build_calibration_capture_filename(
    const std::string& purpose,
    const std::string& timestamp_label);

SpatialLayoutPersistedFiles make_spatial_layout_persisted_files(
    const std::string& artifact_root_dir,
    const std::string& artifact_id);

GenericCalibrationImageSetFiles make_generic_calibration_image_set_files(
    const std::string& artifact_root_dir,
    const std::string& artifact_id,
    const std::string& source_frame_filename);

} // namespace orange::gui::spatial_layout
