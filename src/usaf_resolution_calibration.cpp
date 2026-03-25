#include "usaf_resolution_calibration.h"

#include "fsuid_guard.h"

#include <opencv2/opencv.hpp>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <numeric>
#include <sstream>

namespace {

constexpr unsigned long long kFnv1a64Offset = 14695981039346656037ull;
constexpr unsigned long long kFnv1a64Prime = 1099511628211ull;

std::string sanitize_artifact_component(const std::string& value)
{
    std::string sanitized;
    sanitized.reserve(value.size());
    for (char c : value) {
        if (std::isalnum(static_cast<unsigned char>(c))) {
            sanitized.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
        } else if (c == '_' || c == '-') {
            sanitized.push_back(c);
        } else {
            sanitized.push_back('_');
        }
    }
    while (!sanitized.empty() && sanitized.front() == '_') {
        sanitized.erase(sanitized.begin());
    }
    while (!sanitized.empty() && sanitized.back() == '_') {
        sanitized.pop_back();
    }
    if (sanitized.empty()) {
        sanitized = "usaf_resolution";
    }
    return sanitized;
}

std::string artifact_relative_path(const std::string& artifact_dir, const std::string& path)
{
    if (artifact_dir.empty() || path.empty()) {
        return {};
    }
    std::error_code ec;
    const std::filesystem::path rel =
        std::filesystem::relative(std::filesystem::path(path), std::filesystem::path(artifact_dir), ec);
    if (ec) {
        return std::filesystem::path(path).filename().generic_string();
    }
    return rel.generic_string();
}

void fnv1a64_update_bytes(unsigned long long* hash, const void* data, size_t size)
{
    const auto* bytes = static_cast<const unsigned char*>(data);
    for (size_t i = 0; i < size; ++i) {
        *hash ^= static_cast<unsigned long long>(bytes[i]);
        *hash *= kFnv1a64Prime;
    }
}

bool fnv1a64_update_file(unsigned long long* hash, const std::filesystem::path& path, std::string* error_out)
{
    std::ifstream input(path, std::ios::binary);
    if (!input.is_open()) {
        if (error_out) {
            *error_out = "Failed to open file for fingerprinting: " + path.string();
        }
        return false;
    }

    char buffer[4096];
    while (input) {
        input.read(buffer, sizeof(buffer));
        const std::streamsize count = input.gcount();
        if (count > 0) {
            fnv1a64_update_bytes(hash, buffer, static_cast<size_t>(count));
        }
    }
    if (!input.eof()) {
        if (error_out) {
            *error_out = "Failed while fingerprinting file: " + path.string();
        }
        return false;
    }
    return true;
}

nlohmann::json make_roi_json(const UsafRoi& roi)
{
    return {
        {"has_roi", roi.has_roi},
        {"x", roi.x},
        {"y", roi.y},
        {"width", roi.width},
        {"height", roi.height}
    };
}

nlohmann::json make_selection_json(const UsafResolvedElementSelection& selection)
{
    return {
        {"available", selection.available},
        {"group", selection.group},
        {"element", selection.element}
    };
}

nlohmann::json make_metrics_json(const UsafResolvedElementMetrics& metrics)
{
    return {
        {"available", metrics.available},
        {"group", metrics.group},
        {"element", metrics.element},
        {"lp_per_mm", metrics.lp_per_mm},
        {"line_pair_period_um", metrics.line_pair_period_um},
        {"single_bar_width_um", metrics.single_bar_width_um},
        {"has_pixels_per_mm", metrics.has_pixels_per_mm},
        {"pixels_per_mm", metrics.pixels_per_mm},
        {"has_pixels_per_line_pair", metrics.has_pixels_per_line_pair},
        {"pixels_per_line_pair", metrics.pixels_per_line_pair},
        {"has_pixels_per_bar", metrics.has_pixels_per_bar},
        {"pixels_per_bar", metrics.pixels_per_bar}
    };
}

std::string metrics_dimension_label(bool use_vertical_sampling)
{
    return use_vertical_sampling ? "y" : "x";
}

} // namespace

const char* usaf_target_polarity_to_string(UsafTargetPolarity polarity)
{
    switch (polarity) {
        case UsafTargetPolarity::kPositive:
            return "positive";
        case UsafTargetPolarity::kNegative:
        default:
            return "negative";
    }
}

double usaf_lp_per_mm(int group, int element)
{
    if (element < 1 || element > 6) {
        return 0.0;
    }
    return std::pow(2.0, static_cast<double>(group) + static_cast<double>(element - 1) / 6.0);
}

UsafResolvedElementMetrics build_usaf_resolved_metrics(
    const UsafResolvedElementSelection& selection,
    bool use_vertical_sampling,
    const FovCalibrationData& fov_calibration)
{
    UsafResolvedElementMetrics metrics;
    if (!selection.available) {
        return metrics;
    }

    const double lp_per_mm = usaf_lp_per_mm(selection.group, selection.element);
    if (lp_per_mm <= 0.0) {
        return metrics;
    }

    metrics.available = true;
    metrics.group = selection.group;
    metrics.element = selection.element;
    metrics.lp_per_mm = lp_per_mm;
    metrics.line_pair_period_um = 1000.0 / lp_per_mm;
    metrics.single_bar_width_um = 500.0 / lp_per_mm;

    const bool use_height_dimension = use_vertical_sampling;
    const bool has_pixels_per_mm =
        use_height_dimension ? fov_calibration.has_field_height_mm : fov_calibration.has_field_width_mm;
    const double field_dimension_mm =
        use_height_dimension ? fov_calibration.field_height_mm : fov_calibration.field_width_mm;
    const double sensor_pixels =
        use_height_dimension ? fov_calibration.sensor_height_mm * 1000.0 / std::max(1e-9, fov_calibration.pixel_pitch_um)
                             : fov_calibration.sensor_width_mm * 1000.0 / std::max(1e-9, fov_calibration.pixel_pitch_um);

    if (has_pixels_per_mm && field_dimension_mm > 0.0 && sensor_pixels > 0.0) {
        metrics.has_pixels_per_mm = true;
        metrics.pixels_per_mm = sensor_pixels / field_dimension_mm;
        metrics.has_pixels_per_line_pair = true;
        metrics.pixels_per_line_pair = metrics.pixels_per_mm / lp_per_mm;
        metrics.has_pixels_per_bar = true;
        metrics.pixels_per_bar = metrics.pixels_per_line_pair * 0.5;
    }

    return metrics;
}

UsafResolutionResult evaluate_usaf_resolution_request(const UsafResolutionRequest& request)
{
    UsafResolutionResult result;
    std::vector<double> position_worst_values;

    for (const UsafCapturedPosition& captured : request.positions) {
        UsafPerPositionResult position;
        position.label = captured.label;
        position.width = captured.width;
        position.height = captured.height;
        position.roi = captured.roi;
        position.notes = captured.notes;
        position.horizontal_bars = build_usaf_resolved_metrics(captured.horizontal_bars, true, request.fov_calibration);
        position.vertical_bars = build_usaf_resolved_metrics(captured.vertical_bars, false, request.fov_calibration);

        std::vector<double> axis_widths;
        if (position.horizontal_bars.available) {
            axis_widths.push_back(position.horizontal_bars.single_bar_width_um);
        }
        if (position.vertical_bars.available) {
            axis_widths.push_back(position.vertical_bars.single_bar_width_um);
        }
        if (!axis_widths.empty()) {
            position.has_position_summary = true;
            position.position_best_single_bar_width_um =
                *std::min_element(axis_widths.begin(), axis_widths.end());
            position.position_worst_single_bar_width_um =
                *std::max_element(axis_widths.begin(), axis_widths.end());
            position_worst_values.push_back(position.position_worst_single_bar_width_um);
            if (position.label == "center") {
                result.has_center_single_bar_width_um = true;
                result.center_single_bar_width_um = position.position_worst_single_bar_width_um;
            }
        }

        result.positions.push_back(std::move(position));
    }

    if (!position_worst_values.empty()) {
        result.has_best_field_single_bar_width_um = true;
        result.best_field_single_bar_width_um = *std::min_element(position_worst_values.begin(), position_worst_values.end());
        result.has_worst_field_single_bar_width_um = true;
        result.worst_field_single_bar_width_um = *std::max_element(position_worst_values.begin(), position_worst_values.end());
        result.has_field_resolution_range_um = true;
        result.field_resolution_range_um =
            result.worst_field_single_bar_width_um - result.best_field_single_bar_width_um;

        const double mean = std::accumulate(position_worst_values.begin(), position_worst_values.end(), 0.0) /
                            static_cast<double>(position_worst_values.size());
        if (mean > 0.0 && position_worst_values.size() > 1) {
            double variance = 0.0;
            for (double value : position_worst_values) {
                const double diff = value - mean;
                variance += diff * diff;
            }
            variance /= static_cast<double>(position_worst_values.size());
            result.has_field_resolution_cv = true;
            result.field_resolution_cv = std::sqrt(variance) / mean;
        }
    }

    return result;
}

std::string build_usaf_resolution_artifact_id(
    const std::string& prefix_base,
    const CameraParams& camera_params,
    const std::string& timestamp_label)
{
    std::ostringstream oss;
    oss << "usafcal_" << sanitize_artifact_component(prefix_base)
        << "_" << sanitize_artifact_component(timestamp_label)
        << "_Cam" << camera_params.camera_serial
        << "_exp" << camera_params.exposure
        << "_focus" << camera_params.focus;
    return oss.str();
}

UsafResolutionArtifactPaths make_usaf_resolution_artifact_paths(
    const std::string& artifact_root_dir,
    const std::string& artifact_id)
{
    UsafResolutionArtifactPaths paths;
    paths.artifact_id = artifact_id;
    const std::filesystem::path artifact_dir = std::filesystem::path(artifact_root_dir) / artifact_id;
    paths.artifact_dir = artifact_dir.string();
    paths.manifest_path = (artifact_dir / "manifest.json").string();
    paths.measurement_json_path = (artifact_dir / "measurement.json").string();
    paths.positions_csv_path = (artifact_dir / "positions.csv").string();
    paths.camera_config_snapshot_path = (artifact_dir / "camera_config_snapshot.json").string();
    paths.target_reference_frames_dir = (artifact_dir / "target_reference_frames").string();
    paths.analysis_overlays_dir = (artifact_dir / "analysis_overlays").string();
    return paths;
}

bool write_usaf_rgb_image_ppm(
    const std::string& path,
    const std::vector<unsigned char>& rgb,
    int width,
    int height,
    const UsafRoi* roi,
    std::string* error_out)
{
    if (width <= 0 || height <= 0 || rgb.size() != static_cast<size_t>(width) * static_cast<size_t>(height) * 3U) {
        if (error_out) {
            *error_out = "RGB preview buffer is invalid for PPM write.";
        }
        return false;
    }

    cv::Mat bgr(height, width, CV_8UC3);
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const size_t src_base = (static_cast<size_t>(y) * static_cast<size_t>(width) + static_cast<size_t>(x)) * 3U;
            cv::Vec3b& pixel = bgr.at<cv::Vec3b>(y, x);
            pixel[0] = rgb[src_base + 2];
            pixel[1] = rgb[src_base + 1];
            pixel[2] = rgb[src_base + 0];
        }
    }

    if (roi != nullptr && roi->has_roi && roi->width > 0 && roi->height > 0) {
        const cv::Rect rect(roi->x, roi->y, roi->width, roi->height);
        cv::rectangle(bgr, rect, cv::Scalar(0, 220, 0), 2, cv::LINE_AA);
        cv::putText(bgr, "ROI", cv::Point(rect.x + 6, std::max(24, rect.y + 24)),
                    cv::FONT_HERSHEY_SIMPLEX, 0.8, cv::Scalar(0, 220, 0), 2, cv::LINE_AA);
    }

    cv::Mat rgb_image;
    cv::cvtColor(bgr, rgb_image, cv::COLOR_BGR2RGB);

    orange::ScopedFsuid fsuid_guard;
    (void)fsuid_guard;
    std::ofstream out(path, std::ios::binary);
    if (!out) {
        if (error_out) {
            *error_out = "Failed to open USAF image output path: " + path;
        }
        return false;
    }
    out << "P6\n" << width << " " << height << "\n255\n";
    out.write(reinterpret_cast<const char*>(rgb_image.data),
              static_cast<std::streamsize>(rgb_image.total() * rgb_image.elemSize()));
    return static_cast<bool>(out);
}

nlohmann::json usaf_resolution_to_json(
    const UsafResolutionResult& result,
    const UsafResolutionRequest& request,
    const CameraParams& camera_params,
    const std::string& lens_name,
    const std::string& artifact_id,
    const std::string& created_utc,
    const std::string& fingerprint,
    const UsafResolutionArtifactPaths& paths)
{
    nlohmann::json root;
    root["schema_id"] = kUsafResolutionCalibrationArtifactSchemaId;
    root["schema_version"] = kUsafResolutionCalibrationArtifactSchemaVersion;
    root["artifact_id"] = artifact_id;
    root["created_utc"] = created_utc;
    root["fingerprint_algorithm"] = kCalibrationFingerprintAlgorithm;
    root["fingerprint"] = fingerprint;

    const std::string relative_camera_config_snapshot_path =
        request.camera_config_snapshot.has_snapshot && !request.camera_config_snapshot.snapshot_path.empty()
            ? artifact_relative_path(paths.artifact_dir, request.camera_config_snapshot.snapshot_path)
            : std::string();

    root["producer"] = {
        {"name", "orange"},
        {"artifact_schema_id", kUsafResolutionCalibrationArtifactSchemaId},
        {"artifact_schema_version", kUsafResolutionCalibrationArtifactSchemaVersion}
    };
    root["compatibility"] = {
        {"camera_serial", camera_params.camera_serial},
        {"lens_name", lens_name},
        {"focus", camera_params.focus},
        {"iris", camera_params.iris},
        {"exposure", camera_params.exposure},
        {"gain", camera_params.gain},
        {"pixel_format", camera_params.pixel_format},
        {"width", camera_params.width},
        {"height", camera_params.height}
    };
    root["request"] = {
        {"target_name", request.target_name},
        {"target_polarity", usaf_target_polarity_to_string(request.target_polarity)},
        {"illumination_mode", request.illumination_mode},
        {"operator_notes", request.operator_notes},
        {"camera_config_snapshot", {
            {"has_source_path", request.camera_config_snapshot.has_source_path},
            {"source_path", request.camera_config_snapshot.source_path},
            {"has_snapshot", request.camera_config_snapshot.has_snapshot},
            {"snapshot_path", relative_camera_config_snapshot_path},
            {"error", request.camera_config_snapshot.error}
        }},
        {"fov_calibration", {
            {"enabled", request.fov_calibration.enabled},
            {"working_distance_mm", request.fov_calibration.working_distance_mm},
            {"pixel_pitch_um", request.fov_calibration.pixel_pitch_um},
            {"has_field_width_mm", request.fov_calibration.has_field_width_mm},
            {"field_width_mm", request.fov_calibration.field_width_mm},
            {"has_field_height_mm", request.fov_calibration.has_field_height_mm},
            {"field_height_mm", request.fov_calibration.field_height_mm},
            {"sensor_width_mm", request.fov_calibration.sensor_width_mm},
            {"sensor_height_mm", request.fov_calibration.sensor_height_mm},
            {"has_mean_magnification", request.fov_calibration.has_mean_magnification},
            {"mean_magnification", request.fov_calibration.mean_magnification}
        }}
    };

    nlohmann::json positions_json = nlohmann::json::array();
    for (const UsafPerPositionResult& position : result.positions) {
        positions_json.push_back({
            {"label", position.label},
            {"width", position.width},
            {"height", position.height},
            {"notes", position.notes},
            {"roi", make_roi_json(position.roi)},
            {"reference_frame_path", artifact_relative_path(paths.artifact_dir, position.reference_frame_path)},
            {"analysis_overlay_path", artifact_relative_path(paths.artifact_dir, position.analysis_overlay_path)},
            {"horizontal_bars", make_metrics_json(position.horizontal_bars)},
            {"vertical_bars", make_metrics_json(position.vertical_bars)},
            {"has_position_summary", position.has_position_summary},
            {"position_best_single_bar_width_um", position.position_best_single_bar_width_um},
            {"position_worst_single_bar_width_um", position.position_worst_single_bar_width_um}
        });
    }
    root["positions"] = positions_json;

    root["summary"] = {
        {"has_center_single_bar_width_um", result.has_center_single_bar_width_um},
        {"center_single_bar_width_um", result.center_single_bar_width_um},
        {"has_best_field_single_bar_width_um", result.has_best_field_single_bar_width_um},
        {"best_field_single_bar_width_um", result.best_field_single_bar_width_um},
        {"has_worst_field_single_bar_width_um", result.has_worst_field_single_bar_width_um},
        {"worst_field_single_bar_width_um", result.worst_field_single_bar_width_um},
        {"has_field_resolution_range_um", result.has_field_resolution_range_um},
        {"field_resolution_range_um", result.field_resolution_range_um},
        {"has_field_resolution_cv", result.has_field_resolution_cv},
        {"field_resolution_cv", result.field_resolution_cv}
    };

    return root;
}

nlohmann::json usaf_resolution_manifest_to_json(
    const UsafResolutionResult& result,
    const UsafResolutionRequest& request,
    const CameraParams& camera_params,
    const std::string& lens_name,
    const std::string& artifact_id,
    const std::string& created_utc,
    const std::string& fingerprint,
    const UsafResolutionArtifactPaths& paths)
{
    const std::string relative_camera_config_snapshot_path =
        request.camera_config_snapshot.has_snapshot && !request.camera_config_snapshot.snapshot_path.empty()
            ? artifact_relative_path(paths.artifact_dir, request.camera_config_snapshot.snapshot_path)
            : std::string();

    nlohmann::json manifest;
    manifest["schema_id"] = kCalibrationManifestSchemaId;
    manifest["schema_version"] = kCalibrationManifestSchemaVersion;
    manifest["artifact_id"] = artifact_id;
    manifest["artifact_schema_id"] = kUsafResolutionCalibrationArtifactSchemaId;
    manifest["artifact_schema_version"] = kUsafResolutionCalibrationArtifactSchemaVersion;
    manifest["created_utc"] = created_utc;
    manifest["producer"] = {
        {"name", "orange"},
        {"artifact_schema_id", kUsafResolutionCalibrationArtifactSchemaId},
        {"artifact_schema_version", kUsafResolutionCalibrationArtifactSchemaVersion}
    };
    manifest["calibration_ref"] = {
        {"artifact_id", artifact_id},
        {"artifact_schema_id", kUsafResolutionCalibrationArtifactSchemaId},
        {"artifact_schema_version", kUsafResolutionCalibrationArtifactSchemaVersion},
        {"fingerprint", fingerprint}
    };
    manifest["compatibility"] = {
        {"camera_serial", camera_params.camera_serial},
        {"lens_name", lens_name},
        {"focus", camera_params.focus},
        {"iris", camera_params.iris},
        {"exposure", camera_params.exposure},
        {"gain", camera_params.gain},
        {"pixel_format", camera_params.pixel_format},
        {"width", camera_params.width},
        {"height", camera_params.height}
    };
    manifest["request"] = {
        {"target_name", request.target_name},
        {"target_polarity", usaf_target_polarity_to_string(request.target_polarity)},
        {"illumination_mode", request.illumination_mode},
        {"operator_notes", request.operator_notes},
        {"position_count", static_cast<int>(request.positions.size())},
        {"camera_config_snapshot", {
            {"has_source_path", request.camera_config_snapshot.has_source_path},
            {"source_path", request.camera_config_snapshot.source_path},
            {"has_snapshot", request.camera_config_snapshot.has_snapshot},
            {"snapshot_path", relative_camera_config_snapshot_path},
            {"error", request.camera_config_snapshot.error}
        }},
        {"fov_calibration", {
            {"enabled", request.fov_calibration.enabled},
            {"working_distance_mm", request.fov_calibration.working_distance_mm},
            {"pixel_pitch_um", request.fov_calibration.pixel_pitch_um},
            {"has_field_width_mm", request.fov_calibration.has_field_width_mm},
            {"field_width_mm", request.fov_calibration.field_width_mm},
            {"has_field_height_mm", request.fov_calibration.has_field_height_mm},
            {"field_height_mm", request.fov_calibration.field_height_mm},
            {"has_mean_magnification", request.fov_calibration.has_mean_magnification},
            {"mean_magnification", request.fov_calibration.mean_magnification}
        }}
    };
    manifest["summary"] = {
        {"has_center_single_bar_width_um", result.has_center_single_bar_width_um},
        {"center_single_bar_width_um", result.center_single_bar_width_um},
        {"has_best_field_single_bar_width_um", result.has_best_field_single_bar_width_um},
        {"best_field_single_bar_width_um", result.best_field_single_bar_width_um},
        {"has_worst_field_single_bar_width_um", result.has_worst_field_single_bar_width_um},
        {"worst_field_single_bar_width_um", result.worst_field_single_bar_width_um},
        {"has_field_resolution_range_um", result.has_field_resolution_range_um},
        {"field_resolution_range_um", result.field_resolution_range_um},
        {"has_field_resolution_cv", result.has_field_resolution_cv},
        {"field_resolution_cv", result.field_resolution_cv}
    };
    manifest["files"] = {
        {"manifest", "manifest.json"},
        {"measurement_json", std::filesystem::path(paths.measurement_json_path).filename().string()},
        {"positions_csv", std::filesystem::path(paths.positions_csv_path).filename().string()},
        {"camera_config_snapshot",
         request.camera_config_snapshot.has_snapshot
             ? std::filesystem::path(paths.camera_config_snapshot_path).filename().string()
             : std::string()},
        {"target_reference_frames_dir", std::filesystem::path(paths.target_reference_frames_dir).filename().string()},
        {"analysis_overlays_dir", std::filesystem::path(paths.analysis_overlays_dir).filename().string()}
    };
    return manifest;
}

std::string compute_usaf_resolution_fingerprint(
    const nlohmann::json& measurement_json,
    const UsafResolutionArtifactPaths& paths,
    std::string* error_out)
{
    unsigned long long hash = kFnv1a64Offset;
    nlohmann::json fingerprint_payload = measurement_json;
    if (fingerprint_payload.contains("request") && fingerprint_payload["request"].contains("camera_config_snapshot")) {
        fingerprint_payload["request"]["camera_config_snapshot"]["source_path"] = "";
        fingerprint_payload["request"]["camera_config_snapshot"]["snapshot_path"] =
            std::filesystem::path(paths.camera_config_snapshot_path).filename().generic_string();
    }
    const std::string payload = fingerprint_payload.dump();
    fnv1a64_update_bytes(&hash, payload.data(), payload.size());

    auto hash_directory = [&](const std::filesystem::path& dir) -> bool {
        if (!std::filesystem::exists(dir)) {
            return true;
        }
        std::vector<std::filesystem::path> files;
        for (const auto& entry : std::filesystem::directory_iterator(dir)) {
            if (entry.is_regular_file()) {
                files.push_back(entry.path());
            }
        }
        std::sort(files.begin(), files.end());
        for (const auto& file : files) {
            const std::string rel = artifact_relative_path(paths.artifact_dir, file.string());
            fnv1a64_update_bytes(&hash, rel.data(), rel.size());
            if (!fnv1a64_update_file(&hash, file, error_out)) {
                return false;
            }
        }
        return true;
    };

    if (!hash_directory(paths.target_reference_frames_dir) || !hash_directory(paths.analysis_overlays_dir)) {
        return {};
    }
    if (std::filesystem::exists(paths.camera_config_snapshot_path) &&
        !fnv1a64_update_file(&hash, std::filesystem::path(paths.camera_config_snapshot_path), error_out)) {
        return {};
    }

    std::ostringstream oss;
    oss << kCalibrationFingerprintAlgorithm << ':' << std::hex << std::nouppercase << hash;
    return oss.str();
}

bool write_usaf_resolution_json(
    const std::string& path,
    const nlohmann::json& data,
    std::string* error_out)
{
    orange::ScopedFsuid fsuid_guard;
    (void)fsuid_guard;
    std::ofstream out(path, std::ios::out | std::ios::binary | std::ios::trunc);
    if (!out.is_open()) {
        if (error_out) {
            *error_out = "Failed to open JSON output path: " + path;
        }
        return false;
    }
    out << data.dump(2) << '\n';
    if (!out.good()) {
        if (error_out) {
            *error_out = "Failed to write JSON output path: " + path;
        }
        return false;
    }
    return true;
}

bool write_usaf_resolution_positions_csv(
    const std::string& path,
    const UsafResolutionResult& result,
    std::string* error_out)
{
    orange::ScopedFsuid fsuid_guard;
    (void)fsuid_guard;
    std::ofstream out(path, std::ios::out | std::ios::binary | std::ios::trunc);
    if (!out.is_open()) {
        if (error_out) {
            *error_out = "Failed to open positions CSV output path: " + path;
        }
        return false;
    }

    out << "label,width,height,roi_x,roi_y,roi_width,roi_height,"
           "horizontal_group,horizontal_element,horizontal_lp_per_mm,horizontal_single_bar_width_um,"
           "horizontal_pixels_per_bar,vertical_group,vertical_element,vertical_lp_per_mm,"
           "vertical_single_bar_width_um,vertical_pixels_per_bar,position_best_single_bar_width_um,"
           "position_worst_single_bar_width_um,reference_frame_path,analysis_overlay_path,notes\n";

    for (const UsafPerPositionResult& position : result.positions) {
        out << position.label << ','
            << position.width << ','
            << position.height << ','
            << (position.roi.has_roi ? position.roi.x : 0) << ','
            << (position.roi.has_roi ? position.roi.y : 0) << ','
            << (position.roi.has_roi ? position.roi.width : 0) << ','
            << (position.roi.has_roi ? position.roi.height : 0) << ','
            << (position.horizontal_bars.available ? position.horizontal_bars.group : 0) << ','
            << (position.horizontal_bars.available ? position.horizontal_bars.element : 0) << ','
            << (position.horizontal_bars.available ? position.horizontal_bars.lp_per_mm : 0.0) << ','
            << (position.horizontal_bars.available ? position.horizontal_bars.single_bar_width_um : 0.0) << ','
            << (position.horizontal_bars.has_pixels_per_bar ? position.horizontal_bars.pixels_per_bar : 0.0) << ','
            << (position.vertical_bars.available ? position.vertical_bars.group : 0) << ','
            << (position.vertical_bars.available ? position.vertical_bars.element : 0) << ','
            << (position.vertical_bars.available ? position.vertical_bars.lp_per_mm : 0.0) << ','
            << (position.vertical_bars.available ? position.vertical_bars.single_bar_width_um : 0.0) << ','
            << (position.vertical_bars.has_pixels_per_bar ? position.vertical_bars.pixels_per_bar : 0.0) << ','
            << (position.has_position_summary ? position.position_best_single_bar_width_um : 0.0) << ','
            << (position.has_position_summary ? position.position_worst_single_bar_width_um : 0.0) << ','
            << position.reference_frame_path << ','
            << position.analysis_overlay_path << ','
            << '"' << position.notes << '"' << '\n';
    }

    if (!out.good()) {
        if (error_out) {
            *error_out = "Failed to write positions CSV output path: " + path;
        }
        return false;
    }
    return true;
}
