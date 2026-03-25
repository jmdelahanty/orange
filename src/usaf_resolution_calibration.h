#ifndef ORANGE_USAF_RESOLUTION_CALIBRATION_H
#define ORANGE_USAF_RESOLUTION_CALIBRATION_H

#include "aperture_characterization.h"
#include "camera.h"
#include "json.hpp"

#include <string>
#include <vector>

inline constexpr const char* kUsafResolutionCalibrationArtifactSchemaId =
    "orange.calibration.usaf1951_resolution";
inline constexpr int kUsafResolutionCalibrationArtifactSchemaVersion = 1;

enum class UsafTargetPolarity {
    kNegative,
    kPositive
};

struct UsafRoi {
    bool has_roi = false;
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;
};

struct UsafResolvedElementSelection {
    bool available = false;
    int group = 0;
    int element = 1;
};

struct UsafResolvedElementMetrics {
    bool available = false;
    int group = 0;
    int element = 1;
    double lp_per_mm = 0.0;
    double line_pair_period_um = 0.0;
    double single_bar_width_um = 0.0;
    bool has_pixels_per_mm = false;
    double pixels_per_mm = 0.0;
    bool has_pixels_per_line_pair = false;
    double pixels_per_line_pair = 0.0;
    bool has_pixels_per_bar = false;
    double pixels_per_bar = 0.0;
};

struct UsafCapturedPosition {
    std::string label;
    int width = 0;
    int height = 0;
    std::vector<unsigned char> rgb;
    UsafRoi roi;
    UsafResolvedElementSelection horizontal_bars;
    UsafResolvedElementSelection vertical_bars;
    std::string notes;
};

struct UsafPerPositionResult {
    std::string label;
    int width = 0;
    int height = 0;
    UsafRoi roi;
    std::string reference_frame_path;
    std::string analysis_overlay_path;
    std::string notes;
    UsafResolvedElementMetrics horizontal_bars;
    UsafResolvedElementMetrics vertical_bars;
    bool has_position_summary = false;
    double position_best_single_bar_width_um = 0.0;
    double position_worst_single_bar_width_um = 0.0;
};

struct UsafResolutionRequest {
    std::string target_name = "USAF 1951";
    UsafTargetPolarity target_polarity = UsafTargetPolarity::kNegative;
    std::string illumination_mode = "transmission";
    std::string operator_notes;
    CameraConfigSnapshotProvenance camera_config_snapshot;
    FovCalibrationData fov_calibration;
    std::vector<UsafCapturedPosition> positions;
};

struct UsafResolutionResult {
    std::vector<UsafPerPositionResult> positions;
    bool has_center_single_bar_width_um = false;
    double center_single_bar_width_um = 0.0;
    bool has_best_field_single_bar_width_um = false;
    double best_field_single_bar_width_um = 0.0;
    bool has_worst_field_single_bar_width_um = false;
    double worst_field_single_bar_width_um = 0.0;
    bool has_field_resolution_range_um = false;
    double field_resolution_range_um = 0.0;
    bool has_field_resolution_cv = false;
    double field_resolution_cv = 0.0;
};

struct UsafResolutionArtifactPaths {
    std::string artifact_id;
    std::string artifact_dir;
    std::string manifest_path;
    std::string measurement_json_path;
    std::string positions_csv_path;
    std::string camera_config_snapshot_path;
    std::string target_reference_frames_dir;
    std::string analysis_overlays_dir;
};

const char* usaf_target_polarity_to_string(UsafTargetPolarity polarity);
double usaf_lp_per_mm(int group, int element);
UsafResolvedElementMetrics build_usaf_resolved_metrics(
    const UsafResolvedElementSelection& selection,
    bool use_vertical_sampling,
    const FovCalibrationData& fov_calibration);
UsafResolutionResult evaluate_usaf_resolution_request(const UsafResolutionRequest& request);
std::string build_usaf_resolution_artifact_id(
    const std::string& prefix_base,
    const CameraParams& camera_params,
    const std::string& timestamp_label);
UsafResolutionArtifactPaths make_usaf_resolution_artifact_paths(
    const std::string& artifact_root_dir,
    const std::string& artifact_id);
bool write_usaf_rgb_image_ppm(
    const std::string& path,
    const std::vector<unsigned char>& rgb,
    int width,
    int height,
    const UsafRoi* roi,
    std::string* error_out);
nlohmann::json usaf_resolution_to_json(
    const UsafResolutionResult& result,
    const UsafResolutionRequest& request,
    const CameraParams& camera_params,
    const std::string& lens_name,
    const std::string& artifact_id,
    const std::string& created_utc,
    const std::string& fingerprint,
    const UsafResolutionArtifactPaths& paths);
nlohmann::json usaf_resolution_manifest_to_json(
    const UsafResolutionResult& result,
    const UsafResolutionRequest& request,
    const CameraParams& camera_params,
    const std::string& lens_name,
    const std::string& artifact_id,
    const std::string& created_utc,
    const std::string& fingerprint,
    const UsafResolutionArtifactPaths& paths);
std::string compute_usaf_resolution_fingerprint(
    const nlohmann::json& measurement_json,
    const UsafResolutionArtifactPaths& paths,
    std::string* error_out);
bool write_usaf_resolution_json(
    const std::string& path,
    const nlohmann::json& data,
    std::string* error_out);
bool write_usaf_resolution_positions_csv(
    const std::string& path,
    const UsafResolutionResult& result,
    std::string* error_out);

#endif
