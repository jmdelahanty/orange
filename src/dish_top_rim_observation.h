#ifndef ORANGE_DISH_TOP_RIM_OBSERVATION_H
#define ORANGE_DISH_TOP_RIM_OBSERVATION_H

#include "calibration_image_set.h"
#include "json.hpp"

#include <opencv2/core.hpp>

#include <cstdint>
#include <string>

namespace orange::calibration {

inline constexpr const char* kCalibrationManifestSchemaId = "orange.calibration.manifest";
inline constexpr int kCalibrationManifestSchemaVersion = 1;
inline constexpr const char* kCalibrationRegistrySchemaId = "orange.calibration.registry";
inline constexpr int kCalibrationRegistrySchemaVersion = 1;
inline constexpr const char* kDishTopRimObservationSchemaId =
    "orange.calibration.dish_top_rim_observation";
inline constexpr int kDishTopRimObservationSchemaVersionV1 = 1;
inline constexpr int kDishTopRimObservationSchemaVersion = 2;
inline constexpr const char* kDishTopRimObservationMethod =
    "orange_acquisition_circle_hough_operator_confirmed_inner_rim_v2";
inline constexpr const char* kDishTopRimTargetPlane = "dish_top_rim";
inline constexpr const char* kDishTopRimTargetFeature =
    "dish_inner_rim_water_side_edge";
inline constexpr const char* kDishTopRimRegion = "water_accessible_footprint";
inline constexpr const char* kDishTopRimBoundaryRole =
    "orange_physical_boundary_evidence";
inline constexpr const char* kDishTopRimBoundaryInterpretation =
    "operator_confirmed_dish_inner_rim_water_side_edge";
inline constexpr const char* kDishTopRimBoundaryInclusionPolicy =
    "follow_observed_inner_rim_without_silent_offset";
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
    int max_detection_dimension_px = 0;
    double detection_scale = 1.0;
    double radius_adjustment_px = 0.0;
};

struct DishTopRimCaptureContext {
    std::string operation_id;
    std::string capture_mode = "session_local_operator_still";
    uint32_t source_frame_count = 1;
    bool has_source_frame_count = false;
    std::string temporal_compositing_method;
    uint64_t first_local_frame_id = 0;
    uint64_t last_local_frame_id = 0;
    bool has_local_frame_range = false;
    uint64_t first_camera_frame_id = 0;
    uint64_t last_camera_frame_id = 0;
    bool has_camera_frame_range = false;
    std::string filter_state = "unknown";
    std::string runtime_filter_state = "unknown";
    std::string light_handling = "leave_current";
    std::string light_state;
    std::string illumination_spectrum = "unknown";
    std::string illumination_source = "unknown";
    double illumination_center_wavelength_nm = 0.0;
    bool has_illumination_center_wavelength_nm = false;
    double illumination_min_wavelength_nm = 0.0;
    bool has_illumination_min_wavelength_nm = false;
    double illumination_max_wavelength_nm = 0.0;
    bool has_illumination_max_wavelength_nm = false;
    double illumination_bandwidth_fwhm_nm = 0.0;
    bool has_illumination_bandwidth_fwhm_nm = false;
    std::string illumination_wavelength_confidence = "unknown";
    std::string projector_state;
    bool projector_visible_to_camera = false;
    double exposure_us = 0.0;
    double frame_rate_hz = 0.0;
    std::string dish_fill_state = "unknown";
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
    std::string storage_relative_artifact_dir;
    std::string created_utc;
    DishTopRimCameraInfo camera;
    DishTopRimCaptureContext capture;
    std::string source_array_role = "images_full";
    int source_frame_index = 0;
    bool has_detected_circle = false;
    DishTopRimCircle detected_circle;
    std::string detected_circle_source;
    double valid_region_erosion_px = 0.0;
    double centroid_gate_outset_px = 0.0;
    bool has_physical_inner_diameter_mm = false;
    double physical_inner_diameter_mm = 0.0;
    std::string physical_inner_diameter_source;
    std::string dish_design_id;
    bool has_reference_camera_pixels_per_mm = false;
    double reference_camera_pixels_per_mm = 0.0;
    std::string reference_camera_scale_target_plane;
    std::string accepted_boundary_role = kDishTopRimBoundaryRole;
    std::string accepted_boundary_interpretation =
        kDishTopRimBoundaryInterpretation;
    std::string boundary_inclusion_policy = kDishTopRimBoundaryInclusionPolicy;
    std::string operator_boundary_target = kDishTopRimTargetFeature;
    bool operator_confirmed = false;
    std::string operator_status = "orange_operator_confirmed";
    std::string operator_notes;
    DishTopRimRuntimeVerification runtime_verification;
    DishTopRimSoftwareInfo software;
    nlohmann::json arena_context = nlohmann::json::object();
    nlohmann::json image_set_rig_context = nlohmann::json::object();
    bool write_palette_export = true;
    bool write_image_set_companion = true;
};

struct DishTopRimObservationArtifactPaths {
    std::string artifact_id;
    std::string artifact_root_dir;
    std::string relative_artifact_dir;
    std::string artifact_dir;
    std::string manifest_path;
    std::string observation_json_path;
    std::string image_set_json_path;
    std::string source_frame_path;
    std::string review_overlay_path;
    std::string registration_hough_overlay_path;
    std::string valid_detection_overlay_path;
    std::string palette_export_path;
    std::string spatial_dish_mask_runtime_export_path;
};

struct DishTopRimObservationWriteResult {
    std::string artifact_id;
    std::string artifact_dir;
    std::string fingerprint;
    nlohmann::json manifest;
    nlohmann::json observation;
    nlohmann::json image_set;
    nlohmann::json palette_export;
};

std::string build_dish_top_rim_observation_artifact_id(
    const std::string& camera_serial,
    const std::string& timestamp_label);

DishTopRimObservationArtifactPaths make_dish_top_rim_observation_artifact_paths(
    const std::string& artifact_root_dir,
    const std::string& artifact_id,
    const std::string& relative_artifact_dir = "");

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
    const std::string& registration_hough_overlay_checksum,
    const std::string& valid_detection_overlay_checksum,
    const std::string& fingerprint);

nlohmann::json dish_top_rim_observation_manifest_to_json(
    const DishTopRimObservationRequest& request,
    const DishTopRimObservationArtifactPaths& paths,
    const std::string& source_frame_checksum,
    const std::string& review_overlay_checksum,
    const std::string& registration_hough_overlay_checksum,
    const std::string& valid_detection_overlay_checksum,
    const std::string& fingerprint);

nlohmann::json dish_top_rim_palette_export_to_json(
    const nlohmann::json& observation_json);

nlohmann::json dish_top_rim_spatial_dish_mask_runtime_export_to_json(
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
