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
    std::string capture_group_id;
    std::string timestamp_utc;
    uint64_t frame_id = 0;
    bool has_frame_id = false;
    uint64_t recording_frame_id = 0;
    bool has_recording_frame_id = false;
    uint32_t source_frame_count = 1;
    bool has_source_frame_count = false;
    std::string temporal_compositing_method;
    uint64_t first_local_frame_id = 0;
    uint64_t last_local_frame_id = 0;
    bool has_local_frame_range = false;
    uint64_t first_camera_frame_id = 0;
    uint64_t last_camera_frame_id = 0;
    bool has_camera_frame_range = false;
    std::string capture_mode;
    double exposure_us = 0.0;
    bool has_exposure_us = false;
    double frame_rate_hz = 0.0;
    bool has_frame_rate_hz = false;
    double gain = 0.0;
    bool has_gain = false;
    std::string filter_state;
    std::string runtime_filter_state;
    std::string light_handling;
    std::string light_state;
    std::string illumination_spectrum;
    std::string illumination_source;
    double illumination_center_wavelength_nm = 0.0;
    bool has_illumination_center_wavelength_nm = false;
    double illumination_min_wavelength_nm = 0.0;
    bool has_illumination_min_wavelength_nm = false;
    double illumination_max_wavelength_nm = 0.0;
    bool has_illumination_max_wavelength_nm = false;
    double illumination_bandwidth_fwhm_nm = 0.0;
    bool has_illumination_bandwidth_fwhm_nm = false;
    std::string illumination_wavelength_confidence;
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
    std::string capture_stage;
    double plane_z_mm_nominal = 0.0;
    bool has_plane_z_mm_nominal = false;
    double plane_z_mm_uncertainty = 0.0;
    bool has_plane_z_mm_uncertainty = false;
    std::string wet_or_dry;
    bool imaging_shelf_installed = false;
    bool has_imaging_shelf_installed = false;
    bool dish_installed = false;
    bool has_dish_installed = false;
    std::string dish_id;
    double water_fill_mm = 0.0;
    bool has_water_fill_mm = false;
    std::string fill_state;
    bool open_water_surface_present = false;
    bool has_open_water_surface_present = false;
    std::string water_settled_status;
    std::string target_method;
    std::string pattern_type;
    std::string pattern_domain;
    std::string matched_parity_group_id;
    std::string parity_group_id;
    std::string parity_group_role;
    bool reference_only = false;
    bool has_reference_only = false;
    bool physical_target_used = false;
    bool has_physical_target_used = false;
    bool projected_pattern_used_as_coordinate_target = false;
    bool has_projected_pattern_used_as_coordinate_target = false;
    std::string plane_id;
    double z_mm_relative_to_projection_surface = 0.0;
    bool has_z_mm_relative_to_projection_surface = false;
    std::string target_id;
    std::string target_design;
    double physical_target_grid_spacing_mm = 0.0;
    bool has_physical_target_grid_spacing_mm = false;
    std::string physical_target_origin_definition;
    std::string physical_target_x_orientation_marker_definition;
    std::vector<CalibrationImageSetImageRef> images;
    std::vector<CalibrationImageSetArtifactRef> derived_artifacts;
    nlohmann::json physical_target = nlohmann::json::object();
    nlohmann::json projected_pattern = nlohmann::json::object();
    nlohmann::json scale_target = nlohmann::json::object();
    nlohmann::json runtime_role = nlohmann::json::object();
    nlohmann::json observations = nlohmann::json::object();
    nlohmann::json review_artifacts = nlohmann::json::object();
    nlohmann::json citrus_preview = nlohmann::json::object();
    nlohmann::json citrus_projection_snapshot_pre_capture = nlohmann::json::object();
    nlohmann::json citrus_projection_snapshot_post_capture = nlohmann::json::object();
    nlohmann::json citrus_projection_epoch_consistency = nlohmann::json::object();
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
