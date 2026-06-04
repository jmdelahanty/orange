#ifndef ORANGE_DISH_TOP_RIM_OBSERVATION_H
#define ORANGE_DISH_TOP_RIM_OBSERVATION_H

#include "json.hpp"

#include <opencv2/core.hpp>

#include <string>

namespace orange::calibration {

inline constexpr const char* kCalibrationManifestSchemaId = "orange.calibration.manifest";
inline constexpr int kCalibrationManifestSchemaVersion = 1;
inline constexpr const char* kCalibrationRegistrySchemaId = "orange.calibration.registry";
inline constexpr int kCalibrationRegistrySchemaVersion = 1;
inline constexpr const char* kDishTopRimObservationSchemaId =
    "orange.calibration.dish_top_rim_observation";
inline constexpr int kDishTopRimObservationSchemaVersion = 1;
inline constexpr const char* kDishTopRimObservationMethod =
    "orange_acquisition_circle_hough_operator_confirmed_v1";
inline constexpr const char* kCalibrationFingerprintAlgorithm = "fnv1a64";

struct DishTopRimPoint {
    double x = 0.0;
    double y = 0.0;
};

struct DishTopRimCircle {
    DishTopRimPoint center;
    double radius_px = 0.0;
};

struct DishTopRimCameraInfo {
    std::string serial;
    std::string name;
    int width = 0;
    int height = 0;
    std::string pixel_format;
};

struct DishTopRimHoughParams {
    double dp = 1.2;
    double min_dist_px = 0.0;
    double param1 = 100.0;
    double param2 = 35.0;
    int min_radius_px = 0;
    int max_radius_px = 0;
    double radius_adjustment_px = 0.0;
};

struct DishTopRimCaptureContext {
    std::string operation_id;
    std::string capture_mode = "session_local_operator_still";
    std::string filter_state = "unknown";
    std::string runtime_filter_state = "unknown";
    bool projector_visible_to_camera = false;
    double exposure_us = 0.0;
    double frame_rate_hz = 0.0;
    bool requires_camera_mount_unchanged = true;
    bool requires_filter_reinstalled_repeatably = false;
};

struct DishTopRimRuntimeVerification {
    std::string status = "unknown";
    std::string reason = "not_checked";
};

struct DishTopRimSoftwareInfo {
    std::string orange_git_commit;
    bool orange_git_dirty_tracked = false;
    std::string orange_version;
};

struct DishTopRimObservationRequest {
    std::string artifact_id;
    std::string created_utc;
    DishTopRimCameraInfo camera;
    DishTopRimCaptureContext capture;
    std::string source_array_role = "images_full";
    int source_frame_index = 0;
    double valid_region_erosion_px = 0.0;
    bool operator_confirmed = true;
    std::string operator_status = "orange_operator_confirmed";
    DishTopRimRuntimeVerification runtime_verification;
    DishTopRimSoftwareInfo software;
    bool write_palette_export = true;
};

struct DishTopRimObservationArtifactPaths {
    std::string artifact_id;
    std::string artifact_dir;
    std::string manifest_path;
    std::string observation_json_path;
    std::string source_frame_path;
    std::string review_overlay_path;
    std::string valid_detection_overlay_path;
    std::string palette_export_path;
};

struct DishTopRimObservationWriteResult {
    std::string artifact_id;
    std::string artifact_dir;
    std::string fingerprint;
    nlohmann::json manifest;
    nlohmann::json observation;
    nlohmann::json palette_export;
};

std::string build_dish_top_rim_observation_artifact_id(
    const std::string& camera_serial,
    const std::string& timestamp_label);

DishTopRimObservationArtifactPaths make_dish_top_rim_observation_artifact_paths(
    const std::string& artifact_root_dir,
    const std::string& artifact_id);

bool detect_dish_top_rim_hough_circle(const cv::Mat& source_image,
                                      const DishTopRimHoughParams& params,
                                      DishTopRimCircle* detected_circle_out,
                                      std::string* error_out = nullptr);

nlohmann::json dish_top_rim_observation_to_json(
    const DishTopRimObservationRequest& request,
    const DishTopRimCircle& detected_circle,
    const DishTopRimCircle& accepted_circle,
    const DishTopRimObservationArtifactPaths& paths,
    const std::string& source_frame_checksum,
    const std::string& review_overlay_checksum,
    const std::string& valid_detection_overlay_checksum,
    const std::string& fingerprint);

nlohmann::json dish_top_rim_observation_manifest_to_json(
    const DishTopRimObservationRequest& request,
    const DishTopRimObservationArtifactPaths& paths,
    const std::string& source_frame_checksum,
    const std::string& review_overlay_checksum,
    const std::string& valid_detection_overlay_checksum,
    const std::string& fingerprint);

nlohmann::json dish_top_rim_palette_export_to_json(
    const nlohmann::json& observation_json);

std::string compute_dish_top_rim_observation_fingerprint(
    const nlohmann::json& observation_json,
    const DishTopRimObservationArtifactPaths& paths,
    std::string* error_out = nullptr);

bool write_dish_top_rim_observation_artifact(
    const std::string& artifact_root_dir,
    const DishTopRimObservationRequest& request,
    const cv::Mat& source_image,
    const DishTopRimHoughParams& hough_params,
    const DishTopRimCircle& accepted_circle,
    DishTopRimObservationWriteResult* result_out,
    std::string* error_out = nullptr);

nlohmann::json build_dish_top_rim_recording_snapshot_entry(
    const nlohmann::json& observation_json,
    bool active_for_detection_gating);

bool apply_dish_top_rim_observation_to_snapshot_json(
    nlohmann::json* snapshot,
    const std::string& camera_serial,
    const nlohmann::json& observation_json,
    bool active_for_detection_gating,
    std::string* error_out = nullptr);

} // namespace orange::calibration

#endif // ORANGE_DISH_TOP_RIM_OBSERVATION_H
