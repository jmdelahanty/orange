#ifndef ORANGE_CALIBRATION_IMAGE_SET_H
#define ORANGE_CALIBRATION_IMAGE_SET_H

#include "json.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace orange::calibration {

inline constexpr const char* kCalibrationImageSetSchemaId = "orange.calibration.image_set";
inline constexpr int kCalibrationImageSetSchemaVersion = 1;

struct CalibrationImageSetShape {
    int height = 0;
    int width = 0;
};

struct CalibrationImageSetCameraInfo {
    std::string serial;
    std::string name;
    CalibrationImageSetShape image_shape;
    std::string pixel_format;
    int configured_width = 0;
    int configured_height = 0;
    bool has_gpu_direct = false;
    bool gpu_direct = false;
    nlohmann::json camera_config_ref = nlohmann::json::object();
};

struct CalibrationImageSetCaptureContext {
    std::string operation_id;
    std::string timestamp_utc;
    uint64_t frame_id = 0;
    bool has_frame_id = false;
    uint64_t recording_frame_id = 0;
    bool has_recording_frame_id = false;
    std::string capture_mode;
    double exposure_us = 0.0;
    bool has_exposure_us = false;
    double frame_rate_hz = 0.0;
    bool has_frame_rate_hz = false;
    double gain = 0.0;
    bool has_gain = false;
    std::string filter_state;
    std::string runtime_filter_state;
    std::string light_state;
    std::string projector_state;
    bool projector_visible_to_camera = false;
    bool has_projector_visible_to_camera = false;
    bool requires_camera_mount_unchanged = false;
    bool has_requires_camera_mount_unchanged = false;
    bool requires_filter_reinstalled_repeatably = false;
    bool has_requires_filter_reinstalled_repeatably = false;
};

struct CalibrationImageSetImageRef {
    std::string role;
    std::string path;
    std::string checksum_algorithm;
    std::string checksum;
    std::string coordinate_space = "camera_native_pixels";
    CalibrationImageSetShape image_shape;
    std::string description;
};

struct CalibrationImageSetArtifactRef {
    std::string artifact_id;
    std::string artifact_schema_id;
    int artifact_schema_version = 0;
    std::string fingerprint;
};

struct CalibrationImageSetRequest {
    std::string artifact_id;
    std::string created_utc;
    std::string purpose;
    std::string target_plane;
    std::string coordinate_space = "camera_native_pixels";
    CalibrationImageSetCameraInfo camera;
    CalibrationImageSetCaptureContext capture;
    nlohmann::json rig_context = nlohmann::json::object();
    std::vector<CalibrationImageSetImageRef> images;
    std::vector<CalibrationImageSetArtifactRef> derived_artifacts;
    nlohmann::json projected_pattern = nlohmann::json::object();
    nlohmann::json scale_target = nlohmann::json::object();
    nlohmann::json observations = nlohmann::json::object();
    nlohmann::json review_artifacts = nlohmann::json::object();
    nlohmann::json citrus_preview = nlohmann::json::object();
    std::string operator_notes;
};

struct CalibrationImageSetWriteResult {
    std::string image_set_json_path;
    nlohmann::json image_set;
};

nlohmann::json calibration_image_set_to_json(const CalibrationImageSetRequest& request);

bool write_calibration_image_set_json_file(
    const std::string& image_set_json_path,
    const CalibrationImageSetRequest& request,
    CalibrationImageSetWriteResult* result_out = nullptr,
    std::string* error_out = nullptr);

} // namespace orange::calibration

#endif // ORANGE_CALIBRATION_IMAGE_SET_H
