#ifndef ORANGE_APERTURE_CHARACTERIZATION_H
#define ORANGE_APERTURE_CHARACTERIZATION_H

#include "camera.h"
#include "json.hpp"

#include <functional>
#include <string>
#include <vector>

inline constexpr const char* kCalibrationManifestSchemaId = "orange.calibration.manifest";
inline constexpr int kCalibrationManifestSchemaVersion = 1;
inline constexpr const char* kCalibrationRegistrySchemaId = "orange.calibration.registry";
inline constexpr int kCalibrationRegistrySchemaVersion = 1;
inline constexpr const char* kApertureCalibrationArtifactSchemaId = "orange.calibration.aperture";
inline constexpr int kApertureCalibrationArtifactSchemaVersion = 1;
inline constexpr const char* kCalibrationFingerprintAlgorithm = "fnv1a64";

enum class ApertureClassification {
    kUsable,
    kSaturated,
    kTooDim,
    kUnsupportedPixelFormat
};

struct FrameBrightnessStats {
    unsigned long long pixel_count = 0;
    double mean = 0.0;
    double median = 0.0;
    double p05 = 0.0;
    double p95 = 0.0;
    double p99 = 0.0;
    double stddev = 0.0;
    unsigned int min_value = 0;
    unsigned int max_value = 0;
    double black_fraction = 0.0;
    double white_fraction = 0.0;
};

struct ApertureCharacterizationThresholds {
    double saturated_white_fraction = 0.001;
    double saturated_p99_min = 254.0;
    double dim_mean_max = 10.0;
    double dim_p95_max = 20.0;
    double dim_black_fraction_min = 0.80;
};

struct BrightnessGridStats {
    unsigned int rows = 0;
    unsigned int cols = 0;
    std::vector<double> tile_mean;
    std::vector<double> tile_relative_mean;
    double min_relative_mean = 0.0;
    double max_relative_mean = 0.0;
    double cv_relative_mean = 0.0;
};

struct FovAlignmentCapture {
    bool has_capture = false;
    std::string capture_path;
    bool has_detected_line = false;
    double line_angle_deg = 0.0;
    double angle_error_deg = 0.0;
    double center_offset_px = 0.0;
    double center_offset_fraction = 0.0;
};

struct FovCalibrationData {
    bool enabled = false;
    double working_distance_mm = 0.0;
    double pixel_pitch_um = 0.0;
    bool has_field_width_mm = false;
    double field_width_mm = 0.0;
    bool has_field_height_mm = false;
    double field_height_mm = 0.0;
    double sensor_width_mm = 0.0;
    double sensor_height_mm = 0.0;
    bool has_magnification_x = false;
    double magnification_x = 0.0;
    bool has_magnification_y = false;
    double magnification_y = 0.0;
    bool has_mean_magnification = false;
    double mean_magnification = 0.0;
    bool has_effective_reference_f_number = false;
    double effective_reference_f_number = 0.0;
    FovAlignmentCapture horizontal_capture;
    FovAlignmentCapture vertical_capture;
};

struct CameraConfigSnapshotProvenance {
    bool has_source_path = false;
    std::string source_path;
    bool has_snapshot = false;
    std::string snapshot_path;
    std::string error;
};

struct ApertureFrameSample {
    unsigned int capture_index = 0;
    unsigned int frame_id = 0;
    unsigned long long timestamp = 0;
    FrameBrightnessStats stats;
};

struct ApertureStepResult {
    unsigned int iris = 0;
    bool iris_command_succeeded = false;
    bool has_iris_readback_after_set = false;
    unsigned int iris_readback_after_set = 0;
    bool iris_verified_after_set = false;
    bool has_iris_readback_after_capture = false;
    unsigned int iris_readback_after_capture = 0;
    bool iris_verified_after_capture = false;
    std::vector<ApertureFrameSample> samples;
    FrameBrightnessStats summary;
    bool has_grid = false;
    BrightnessGridStats grid;
    ApertureClassification classification = ApertureClassification::kUsable;
    bool has_representative_frame = false;
    std::string representative_frame_path;
    double relative_mean = 0.0;
    double delta_ev = 0.0;
    bool has_estimated_f_number = false;
    double estimated_f_number = 0.0;
    bool has_estimated_effective_f_number = false;
    double estimated_effective_f_number = 0.0;
};

struct ApertureCharacterizationRequest {
    std::vector<unsigned int> iris_values;
    unsigned int frames_per_step = 3;
    unsigned int settle_frames = 30;
    unsigned int grab_timeout_ms = 1000;
    bool manage_acquisition = true;
    bool restore_original_iris = true;
    bool has_reference_iris = false;
    unsigned int reference_iris = 0;
    bool has_reference_f_number = false;
    double reference_f_number = 0.0;
    CameraConfigSnapshotProvenance camera_config_snapshot;
    FovCalibrationData fov_calibration;
    ApertureCharacterizationThresholds thresholds;
    unsigned int grid_rows = 8;
    unsigned int grid_cols = 8;
    bool save_representative_frames = false;
    std::string representative_frame_dir;
    std::string representative_frame_prefix;
    std::function<void(size_t completed_steps, size_t total_steps, unsigned int iris_value)> progress_callback;
};

struct ApertureCharacterizationResult {
    bool acquisition_started = false;
    bool acquisition_stopped = false;
    bool restored_original_iris = false;
    unsigned int original_iris = 0;
    bool has_reference_iris = false;
    unsigned int reference_iris = 0;
    double reference_mean = 0.0;
    bool has_usable_window = false;
    unsigned int usable_iris_min = 0;
    unsigned int usable_iris_max = 0;
    bool has_saturation_boundary = false;
    unsigned int saturation_limited_through_iris = 0;
    bool has_dim_boundary = false;
    unsigned int dim_limited_from_iris = 0;
    std::vector<std::string> warnings;
    std::vector<ApertureStepResult> steps;
};

struct ApertureCharacterizationArtifactPaths {
    std::string artifact_id;
    std::string artifact_dir;
    std::string manifest_path;
    std::string measurement_json_path;
    std::string steps_csv_path;
    std::string frames_csv_path;
    std::string camera_config_snapshot_path;
    std::string representative_frames_dir;
    std::string fov_reference_frames_dir;
    std::string fov_horizontal_capture_path;
    std::string fov_vertical_capture_path;
};

FrameBrightnessStats compute_frame_brightness_stats(const Emergent::CEmergentFrame& frame);
ApertureClassification classify_aperture_step(
    const FrameBrightnessStats& stats,
    const ApertureCharacterizationThresholds& thresholds);
const char* aperture_classification_to_string(ApertureClassification classification);

std::vector<unsigned int> build_iris_sweep(
    unsigned int iris_min,
    unsigned int iris_max,
    unsigned int iris_inc,
    unsigned int step_multiplier = 1);

std::string build_aperture_characterization_artifact_id(
    const std::string& prefix_base,
    const CameraParams& camera_params,
    const std::string& timestamp_label);

ApertureCharacterizationArtifactPaths make_aperture_characterization_artifact_paths(
    const std::string& artifact_root_dir,
    const std::string& artifact_id);

ApertureCharacterizationResult characterize_aperture(
    Emergent::CEmergentCamera* camera,
    CameraParams* camera_params,
    const ApertureCharacterizationRequest& request);

ApertureCharacterizationResult characterize_aperture_with_stream(
    Emergent::CEmergentCamera* camera,
    CameraParams* camera_params,
    const ApertureCharacterizationRequest& request,
    unsigned int frame_buffer_count);

nlohmann::json aperture_characterization_to_json(
    const ApertureCharacterizationResult& result,
    const ApertureCharacterizationRequest& request,
    const CameraParams& camera_params,
    const std::string& lens_name,
    const std::string& artifact_id,
    const std::string& created_utc,
    const std::string& fingerprint,
    const ApertureCharacterizationArtifactPaths& paths);

nlohmann::json aperture_characterization_manifest_to_json(
    const ApertureCharacterizationResult& result,
    const ApertureCharacterizationRequest& request,
    const CameraParams& camera_params,
    const std::string& lens_name,
    const std::string& artifact_id,
    const std::string& created_utc,
    const std::string& fingerprint,
    const ApertureCharacterizationArtifactPaths& paths);

std::string compute_aperture_characterization_fingerprint(
    const nlohmann::json& measurement_json,
    const ApertureCharacterizationArtifactPaths& paths,
    std::string* error_out);

bool write_aperture_characterization_json(
    const std::string& path,
    const nlohmann::json& data,
    std::string* error_out);

bool write_aperture_characterization_step_csv(
    const std::string& path,
    const ApertureCharacterizationResult& result,
    const ApertureCharacterizationArtifactPaths& paths,
    std::string* error_out);

bool write_aperture_characterization_frame_csv(
    const std::string& path,
    const ApertureCharacterizationResult& result,
    std::string* error_out);

#endif
