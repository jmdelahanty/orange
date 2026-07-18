#include "dish_top_rim_observation.h"

#include "fsuid_guard.h"

#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

#include <cmath>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <vector>

namespace orange::calibration {
namespace {

constexpr uint64_t kFnv1a64Offset = 14695981039346656037ULL;
constexpr uint64_t kFnv1a64Prime = 1099511628211ULL;

bool set_error(std::string* error_out, const std::string& message)
{
    if (error_out) {
        *error_out = message;
    }
    return false;
}

void fnv1a64_update(uint64_t* hash, const unsigned char* data, size_t size)
{
    if (!hash || !data) {
        return;
    }
    for (size_t idx = 0; idx < size; ++idx) {
        *hash ^= static_cast<uint64_t>(data[idx]);
        *hash *= kFnv1a64Prime;
    }
}

std::string fnv1a64_to_string(uint64_t hash)
{
    std::ostringstream oss;
    oss << kCalibrationFingerprintAlgorithm << ':' << std::hex << std::nouppercase << hash;
    return oss.str();
}

std::string compute_bytes_fingerprint(const std::vector<unsigned char>& bytes)
{
    uint64_t hash = kFnv1a64Offset;
    if (!bytes.empty()) {
        fnv1a64_update(&hash, bytes.data(), bytes.size());
    }
    return fnv1a64_to_string(hash);
}

bool read_file_bytes(const std::filesystem::path& path,
                     std::vector<unsigned char>* bytes_out,
                     std::string* error_out)
{
    if (!bytes_out) {
        return set_error(error_out, "null byte destination");
    }
    std::ifstream in(path, std::ios::in | std::ios::binary);
    if (!in.is_open()) {
        return set_error(error_out, "failed to open file: " + path.string());
    }
    in.seekg(0, std::ios::end);
    const std::streamoff size = in.tellg();
    if (size < 0) {
        return set_error(error_out, "failed to get file size: " + path.string());
    }
    in.seekg(0, std::ios::beg);
    bytes_out->assign(static_cast<size_t>(size), 0);
    if (size > 0) {
        in.read(reinterpret_cast<char*>(bytes_out->data()), size);
        if (!in) {
            return set_error(error_out, "failed to read file: " + path.string());
        }
    }
    return true;
}

std::string compute_file_fingerprint(const std::filesystem::path& path, std::string* error_out)
{
    std::vector<unsigned char> bytes;
    if (!read_file_bytes(path, &bytes, error_out)) {
        return "";
    }
    return compute_bytes_fingerprint(bytes);
}

std::string relative_to_artifact_dir(const std::string& path, const DishTopRimObservationArtifactPaths& paths)
{
    std::error_code error;
    const std::filesystem::path relative =
        std::filesystem::relative(std::filesystem::path(path), std::filesystem::path(paths.artifact_dir), error);
    if (!error && !relative.empty()) {
        return relative.generic_string();
    }
    return std::filesystem::path(path).filename().generic_string();
}

std::string relative_to_artifact_root(const std::string& path, const DishTopRimObservationArtifactPaths& paths)
{
    if (paths.artifact_root_dir.empty()) {
        return (std::filesystem::path(paths.artifact_id) /
                std::filesystem::path(path).filename()).generic_string();
    }
    std::error_code error;
    const std::filesystem::path relative =
        std::filesystem::relative(
            std::filesystem::path(path),
            std::filesystem::path(paths.artifact_root_dir),
            error);
    if (!error && !relative.empty()) {
        return relative.generic_string();
    }
    return (std::filesystem::path(paths.relative_artifact_dir.empty()
                                      ? paths.artifact_id
                                      : paths.relative_artifact_dir) /
            std::filesystem::path(path).filename()).generic_string();
}

bool write_json_file(const std::filesystem::path& path,
                     const nlohmann::json& value,
                     std::string* error_out)
{
    orange::ScopedFsuid fsuid_guard;
    (void)fsuid_guard;
    std::filesystem::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::out | std::ios::trunc);
    if (!out.is_open()) {
        return set_error(error_out, "failed to open JSON file for writing: " + path.string());
    }
    out << value.dump(2) << '\n';
    return true;
}

bool read_json_file(const std::filesystem::path& path,
                    nlohmann::json* value_out,
                    std::string* error_out)
{
    if (!value_out) {
        return set_error(error_out, "null JSON destination");
    }
    std::ifstream in(path, std::ios::in | std::ios::binary);
    if (!in.is_open()) {
        return set_error(error_out, "failed to open JSON file: " + path.string());
    }
    try {
        in >> *value_out;
    } catch (const std::exception& ex) {
        return set_error(error_out, "failed to parse JSON file " + path.string() + ": " + ex.what());
    }
    return true;
}

bool write_image_file(const std::filesystem::path& path, const cv::Mat& image, std::string* error_out)
{
    orange::ScopedFsuid fsuid_guard;
    (void)fsuid_guard;
    std::filesystem::create_directories(path.parent_path());
    if (image.empty()) {
        return set_error(error_out, "cannot write empty image: " + path.string());
    }
    try {
        if (!cv::imwrite(path.string(), image)) {
            return set_error(error_out, "cv::imwrite failed: " + path.string());
        }
    } catch (const std::exception& ex) {
        return set_error(error_out, "cv::imwrite exception for " + path.string() + ": " + ex.what());
    }
    return true;
}

cv::Mat normalize_source_for_png(const cv::Mat& source)
{
    if (source.empty()) {
        return {};
    }
    if (source.depth() == CV_8U) {
        return source.clone();
    }
    cv::Mat normalized;
    cv::normalize(source, normalized, 0, 255, cv::NORM_MINMAX, CV_8U);
    return normalized;
}

cv::Mat make_gray8(const cv::Mat& source)
{
    if (source.empty()) {
        return {};
    }
    cv::Mat gray;
    if (source.channels() == 1) {
        if (source.depth() == CV_8U) {
            gray = source;
        } else {
            cv::normalize(source, gray, 0, 255, cv::NORM_MINMAX, CV_8U);
        }
    } else {
        cv::Mat source8 = normalize_source_for_png(source);
        cv::cvtColor(source8, gray, cv::COLOR_BGR2GRAY);
    }
    return gray;
}

cv::Mat make_overlay(const cv::Mat& source,
                     const DishTopRimCircle& detected_circle,
                     const DishTopRimCircle& accepted_circle,
                     const DishTopRimCircle& valid_circle)
{
    cv::Mat source8 = normalize_source_for_png(source);
    cv::Mat overlay;
    if (source8.channels() == 1) {
        cv::cvtColor(source8, overlay, cv::COLOR_GRAY2BGR);
    } else if (source8.channels() == 3) {
        overlay = source8.clone();
    } else if (source8.channels() == 4) {
        cv::cvtColor(source8, overlay, cv::COLOR_BGRA2BGR);
    } else {
        cv::Mat gray = make_gray8(source8);
        cv::cvtColor(gray, overlay, cv::COLOR_GRAY2BGR);
    }

    auto draw_circle = [&](const DishTopRimCircle& circle, const cv::Scalar& color, int thickness) {
        cv::circle(
            overlay,
            cv::Point(static_cast<int>(std::lround(circle.center.x)),
                      static_cast<int>(std::lround(circle.center.y))),
            static_cast<int>(std::lround(circle.radius_px)),
            color,
            thickness,
            cv::LINE_AA);
        cv::drawMarker(
            overlay,
            cv::Point(static_cast<int>(std::lround(circle.center.x)),
                      static_cast<int>(std::lround(circle.center.y))),
            color,
            cv::MARKER_CROSS,
            24,
            2,
            cv::LINE_AA);
    };

    draw_circle(detected_circle, cv::Scalar(0, 200, 255), 2);
    draw_circle(accepted_circle, cv::Scalar(0, 255, 0), 2);
    draw_circle(valid_circle, cv::Scalar(255, 0, 0), 1);
    return overlay;
}

bool validate_circle(const DishTopRimCircle& circle,
                     int width,
                     int height,
                     const std::string& path,
                     std::string* error_out)
{
    if (!std::isfinite(circle.center.x) || !std::isfinite(circle.center.y) ||
        !std::isfinite(circle.radius_px)) {
        return set_error(error_out, path + " has non-finite values");
    }
    if (circle.radius_px <= 0.0) {
        return set_error(error_out, path + ".radius_px must be > 0");
    }
    if (width > 0 && height > 0) {
        if (circle.center.x < 0.0 || circle.center.x >= static_cast<double>(width) ||
            circle.center.y < 0.0 || circle.center.y >= static_cast<double>(height)) {
            return set_error(error_out, path + ".center_px must lie inside image bounds");
        }
    }
    return true;
}

nlohmann::json point_to_json(const DishTopRimPoint& point)
{
    return {{"x", point.x}, {"y", point.y}};
}

nlohmann::json circle_to_json(const DishTopRimCircle& circle)
{
    return {
        {"center_px", point_to_json(circle.center)},
        {"radius_px", circle.radius_px}
    };
}

nlohmann::json circle_geometry_to_json(const DishTopRimCircle& circle)
{
    nlohmann::json out = circle_to_json(circle);
    out["type"] = "circle";
    return out;
}

double rounded_or_zero(double value)
{
    if (!std::isfinite(value)) {
        return 0.0;
    }
    return value;
}

nlohmann::json image_shape_json(int height, int width)
{
    return {{"height", height}, {"width", width}};
}

nlohmann::json calibration_ref_json(const std::string& artifact_id, const std::string& fingerprint)
{
    return {
        {"artifact_id", artifact_id},
        {"artifact_schema_id", kDishTopRimObservationSchemaId},
        {"artifact_schema_version", kDishTopRimObservationSchemaVersion},
        {"fingerprint", fingerprint}
    };
}

bool update_local_calibration_registry(const std::filesystem::path& artifact_root_dir,
                                       const nlohmann::json& manifest,
                                       std::string* error_out)
{
    if (!manifest.is_object()) {
        return set_error(error_out, "calibration manifest is not a JSON object");
    }
    const std::string artifact_id = manifest.value("artifact_id", "");
    const std::string artifact_schema_id = manifest.value("artifact_schema_id", "");
    if (artifact_id.empty() || artifact_schema_id.empty()) {
        return set_error(error_out, "manifest must contain artifact_id and artifact_schema_id");
    }

    std::filesystem::create_directories(artifact_root_dir);
    const std::filesystem::path registry_path = artifact_root_dir / "index.json";
    nlohmann::json registry = nlohmann::json::object();
    if (std::filesystem::exists(registry_path)) {
        if (!read_json_file(registry_path, &registry, error_out)) {
            return false;
        }
        if (!registry.is_object()) {
            registry = nlohmann::json::object();
        }
    }

    registry["schema_id"] = kCalibrationRegistrySchemaId;
    registry["schema_version"] = kCalibrationRegistrySchemaVersion;
    registry["artifact_root"] = artifact_root_dir.generic_string();
    registry["updated_utc"] = manifest.value("created_utc", "");
    if (!registry.contains("artifacts_by_id") || !registry["artifacts_by_id"].is_object()) {
        registry["artifacts_by_id"] = nlohmann::json::object();
    }
    if (!registry.contains("latest_by_schema") || !registry["latest_by_schema"].is_object()) {
        registry["latest_by_schema"] = nlohmann::json::object();
    }

    nlohmann::json entry;
    entry["artifact_id"] = artifact_id;
    entry["artifact_schema_id"] = artifact_schema_id;
    entry["artifact_schema_version"] = manifest.value("artifact_schema_version", 0);
    entry["created_utc"] = manifest.value("created_utc", "");
    entry["fingerprint"] = manifest.value("calibration_ref", nlohmann::json::object()).value("fingerprint", "");
    entry["relative_manifest_path"] =
        manifest.value("storage", nlohmann::json::object())
            .value(
                "relative_manifest_path",
                (std::filesystem::path(artifact_id) / "manifest.json").generic_string());
    if (manifest.contains("producer")) {
        entry["producer"] = manifest["producer"];
    }
    if (manifest.contains("compatibility")) {
        entry["compatibility"] = manifest["compatibility"];
    }
    if (manifest.contains("summary")) {
        entry["summary"] = manifest["summary"];
    }

    registry["artifacts_by_id"][artifact_id] = entry;
    registry["latest_by_schema"][artifact_schema_id] = artifact_id;
    registry["artifact_count"] = registry["artifacts_by_id"].size();
    return write_json_file(registry_path, registry, error_out);
}

} // namespace

std::string build_dish_top_rim_observation_artifact_id(
    const std::string& camera_serial,
    const std::string& timestamp_label)
{
    std::string serial = camera_serial.empty() ? "unknown" : camera_serial;
    for (char& ch : serial) {
        if (!(std::isalnum(static_cast<unsigned char>(ch)) || ch == '_' || ch == '-')) {
            ch = '_';
        }
    }
    std::string stamp = timestamp_label.empty() ? "unknown_time" : timestamp_label;
    for (char& ch : stamp) {
        if (!(std::isalnum(static_cast<unsigned char>(ch)) || ch == '_' || ch == '-')) {
            ch = '_';
        }
    }
    return "dishrim_" + stamp + "_" + serial;
}

DishTopRimObservationArtifactPaths make_dish_top_rim_observation_artifact_paths(
    const std::string& artifact_root_dir,
    const std::string& artifact_id,
    const std::string& relative_artifact_dir)
{
    const std::filesystem::path relative_dir =
        relative_artifact_dir.empty()
            ? std::filesystem::path(artifact_id)
            : std::filesystem::path(relative_artifact_dir);
    const std::filesystem::path root =
        std::filesystem::path(artifact_root_dir) / relative_dir;
    DishTopRimObservationArtifactPaths paths;
    paths.artifact_id = artifact_id;
    paths.artifact_root_dir = artifact_root_dir;
    paths.relative_artifact_dir = relative_dir.generic_string();
    paths.artifact_dir = root.generic_string();
    paths.manifest_path = (root / "manifest.json").generic_string();
    paths.observation_json_path = (root / "observation.json").generic_string();
    paths.image_set_json_path = (root / "image_set.json").generic_string();
    paths.source_frame_path = (root / "captures" / "source_frame.png").generic_string();
    paths.review_overlay_path = (root / "overlays" / "top_rim_fit.png").generic_string();
    paths.registration_hough_overlay_path =
        (root / "overlays" / "registration_hough_overlay.png").generic_string();
    paths.valid_detection_overlay_path =
        (root / "overlays" / "valid_detection_region.png").generic_string();
    paths.palette_export_path = (root / "exports" / "palette_dish_mask_v2.json").generic_string();
    paths.spatial_dish_mask_runtime_export_path =
        (root / "exports" / "spatial_dish_mask_runtime_v1.json").generic_string();
    return paths;
}

bool detect_dish_top_rim_hough_circle(const cv::Mat& source_image,
                                      const DishTopRimHoughParams& params,
                                      DishTopRimCircle* detected_circle_out,
                                      std::string* error_out)
{
    if (!detected_circle_out) {
        return set_error(error_out, "null detected circle destination");
    }
    if (source_image.empty()) {
        return set_error(error_out, "source image is empty");
    }
    cv::Mat gray = make_gray8(source_image);
    if (gray.empty()) {
        return set_error(error_out, "failed to convert source image to grayscale");
    }
    cv::medianBlur(gray, gray, 5);

    const double min_dist = params.min_dist_px > 0.0
                                ? params.min_dist_px
                                : static_cast<double>(std::min(gray.cols, gray.rows)) * 0.25;
    std::vector<cv::Vec3f> circles;
    try {
        cv::HoughCircles(
            gray,
            circles,
            cv::HOUGH_GRADIENT,
            params.dp,
            min_dist,
            params.param1,
            params.param2,
            params.min_radius_px,
            params.max_radius_px);
    } catch (const std::exception& ex) {
        return set_error(error_out, std::string("cv::HoughCircles failed: ") + ex.what());
    }

    if (circles.empty()) {
        return set_error(error_out, "Hough circle detection found no circles");
    }

    const cv::Vec3f best = circles.front();
    DishTopRimCircle circle;
    circle.center.x = best[0];
    circle.center.y = best[1];
    circle.radius_px = best[2] + params.radius_adjustment_px;
    if (!validate_circle(circle, source_image.cols, source_image.rows, "detected_circle", error_out)) {
        return false;
    }
    *detected_circle_out = circle;
    return true;
}

nlohmann::json dish_top_rim_observation_to_json(
    const DishTopRimObservationRequest& request,
    const DishTopRimCircle& detected_circle,
    const DishTopRimCircle& accepted_circle,
    const DishTopRimObservationArtifactPaths& paths,
    const std::string& source_frame_checksum,
    const std::string& review_overlay_checksum,
    const std::string& registration_hough_overlay_checksum,
    const std::string& valid_detection_overlay_checksum,
    const std::string& fingerprint)
{
    const double erosion = std::max(0.0, request.valid_region_erosion_px);
    const double outset = std::max(0.0, request.centroid_gate_outset_px);
    DishTopRimCircle valid_circle = accepted_circle;
    valid_circle.radius_px =
        std::max(0.0, accepted_circle.radius_px - erosion + outset);

    const bool has_physical_inner_diameter =
        request.has_physical_inner_diameter_mm &&
        std::isfinite(request.physical_inner_diameter_mm) &&
        request.physical_inner_diameter_mm > 0.0;
    const double physical_inner_radius_mm =
        has_physical_inner_diameter
            ? request.physical_inner_diameter_mm * 0.5
            : 0.0;
    const double top_rim_pixels_per_mm =
        physical_inner_radius_mm > 0.0
            ? accepted_circle.radius_px / physical_inner_radius_mm
            : 0.0;
    const double centroid_gate_outset_mm =
        top_rim_pixels_per_mm > 0.0 ? outset / top_rim_pixels_per_mm : 0.0;

    const double center_dx = accepted_circle.center.x - detected_circle.center.x;
    const double center_dy = accepted_circle.center.y - detected_circle.center.y;
    const double radius_delta = accepted_circle.radius_px - detected_circle.radius_px;
    const std::string accepted_boundary_role =
        request.accepted_boundary_role.empty()
            ? kDishTopRimBoundaryRole
            : request.accepted_boundary_role;
    const std::string accepted_boundary_interpretation =
        request.accepted_boundary_interpretation.empty()
            ? kDishTopRimBoundaryInterpretation
            : request.accepted_boundary_interpretation;
    const std::string boundary_inclusion_policy =
        request.boundary_inclusion_policy.empty()
            ? kDishTopRimBoundaryInclusionPolicy
            : request.boundary_inclusion_policy;

    nlohmann::json accepted_inner_rim_boundary = {
        {"role", accepted_boundary_role},
        {"interpretation", accepted_boundary_interpretation},
        {"physical_target", kDishTopRimTargetPlane},
        {"target_plane", kDishTopRimTargetPlane},
        {"target_feature", kDishTopRimTargetFeature},
        {"region", kDishTopRimRegion},
        {"coordinate_space", "camera_native_pixels"},
        {"geometry", circle_geometry_to_json(accepted_circle)},
        {"operator_confirmed", request.operator_confirmed},
        {"accepted_by_operator", request.operator_confirmed},
        {"accepted_at_utc", request.created_utc},
        {"source_boundary", "observed_boundary"},
        {"operator_boundary_target", kDishTopRimTargetFeature},
        {"boundary_inclusion_policy", boundary_inclusion_policy},
        {"operator_adjustment_px", {
            {"center_dx", rounded_or_zero(center_dx)},
            {"center_dy", rounded_or_zero(center_dy)},
            {"radius_delta", rounded_or_zero(radius_delta)}
        }}
    };
    if (has_physical_inner_diameter) {
        accepted_inner_rim_boundary["physical_geometry"] = {
            {"type", "circle"},
            {"coordinate_space", "dish_top_rim_mm"},
            {"center_mm", {{"x", 0.0}, {"y", 0.0}}},
            {"radius_mm", physical_inner_radius_mm},
            {"diameter_mm", request.physical_inner_diameter_mm},
            {"dish_design_id", request.dish_design_id},
            {"dimension_source", request.physical_inner_diameter_source}
        };
        accepted_inner_rim_boundary["camera_scale"] = {
            {"target_plane", kDishTopRimTargetPlane},
            {"coordinate_space", "camera_native_pixels"},
            {"derivation",
             "accepted_camera_radius_px_divided_by_physical_inner_radius_mm"},
            {"pixels_per_mm", top_rim_pixels_per_mm},
            {"mm_per_pixel", 1.0 / top_rim_pixels_per_mm},
            {"accepted_radius_px", accepted_circle.radius_px},
            {"physical_radius_mm", physical_inner_radius_mm}
        };
        if (request.has_reference_camera_pixels_per_mm &&
            std::isfinite(request.reference_camera_pixels_per_mm) &&
            request.reference_camera_pixels_per_mm > 0.0) {
            accepted_inner_rim_boundary["camera_scale"]["comparison_reference"] = {
                {"pixels_per_mm", request.reference_camera_pixels_per_mm},
                {"target_plane",
                 request.reference_camera_scale_target_plane.empty()
                     ? "unknown"
                     : request.reference_camera_scale_target_plane},
                {"authoritative_for_dish_top_rim", false},
                {"purpose", "cross_plane_diagnostic_only"}
            };
        }
    }
    nlohmann::json accepted_experimental_area_boundary_alias =
        accepted_inner_rim_boundary;
    accepted_experimental_area_boundary_alias["role"] =
        "citrus_experimental_area_boundary";
    accepted_experimental_area_boundary_alias["interpretation"] =
        "compatibility_alias_of_accepted_inner_rim_boundary";
    accepted_experimental_area_boundary_alias["compatibility_alias"] = true;
    accepted_experimental_area_boundary_alias["alias_of"] =
        "accepted_inner_rim_boundary";
    accepted_experimental_area_boundary_alias["asserts_citrus_acceptance"] = false;

    nlohmann::json illumination = nlohmann::json::object();
    if (!request.capture.illumination_spectrum.empty()) {
        illumination["spectrum"] = request.capture.illumination_spectrum;
    }
    if (!request.capture.illumination_source.empty()) {
        illumination["source"] = request.capture.illumination_source;
    }
    if (request.capture.has_illumination_center_wavelength_nm) {
        illumination["center_wavelength_nm"] =
            request.capture.illumination_center_wavelength_nm;
    }
    if (request.capture.has_illumination_min_wavelength_nm) {
        illumination["min_wavelength_nm"] =
            request.capture.illumination_min_wavelength_nm;
    }
    if (request.capture.has_illumination_max_wavelength_nm) {
        illumination["max_wavelength_nm"] =
            request.capture.illumination_max_wavelength_nm;
    }
    if (request.capture.has_illumination_bandwidth_fwhm_nm) {
        illumination["bandwidth_fwhm_nm"] =
            request.capture.illumination_bandwidth_fwhm_nm;
    }
    if (!request.capture.illumination_wavelength_confidence.empty()) {
        illumination["wavelength_confidence"] =
            request.capture.illumination_wavelength_confidence;
    }

    nlohmann::json capture = {
        {"operation_id", request.capture.operation_id},
        {"capture_mode", request.capture.capture_mode},
        {"filter_state", request.capture.filter_state},
        {"runtime_filter_state", request.capture.runtime_filter_state},
        {"light_handling", request.capture.light_handling},
        {"light_state", request.capture.light_state},
        {"projector_state", request.capture.projector_state},
        {"projector_visible_to_camera", request.capture.projector_visible_to_camera},
        {"exposure_us", request.capture.exposure_us},
        {"frame_rate_hz", request.capture.frame_rate_hz},
        {"dish_fill_state", request.capture.dish_fill_state},
        {"requires_camera_mount_unchanged", request.capture.requires_camera_mount_unchanged},
        {"requires_filter_reinstalled_repeatably",
         request.capture.requires_filter_reinstalled_repeatably}
    };
    if (request.capture.has_source_frame_count) {
        capture["source_frame_count"] = request.capture.source_frame_count;
    }
    if (!request.capture.temporal_compositing_method.empty()) {
        capture["temporal_compositing_method"] =
            request.capture.temporal_compositing_method;
    }
    if (request.capture.has_local_frame_range) {
        capture["first_local_frame_id"] = request.capture.first_local_frame_id;
        capture["last_local_frame_id"] = request.capture.last_local_frame_id;
    }
    if (request.capture.has_camera_frame_range) {
        capture["first_camera_frame_id"] = request.capture.first_camera_frame_id;
        capture["last_camera_frame_id"] = request.capture.last_camera_frame_id;
    }
    if (!illumination.empty()) {
        capture["illumination"] = illumination;
    }

    nlohmann::json observation = {
        {"schema_id", kDishTopRimObservationSchemaId},
        {"schema_version", kDishTopRimObservationSchemaVersion},
        {"artifact_id", request.artifact_id},
        {"created_utc", request.created_utc},
        {"calibration_ref", calibration_ref_json(request.artifact_id, fingerprint)},
        {"camera", {
            {"serial", request.camera.serial},
            {"name", request.camera.name},
            {"width", request.camera.width},
            {"height", request.camera.height},
            {"pixel_format", request.camera.pixel_format}
        }},
        {"capture", capture},
        {"source_frames", nlohmann::json::array({
            {
                {"role", "source_frame"},
                {"path", relative_to_artifact_dir(paths.source_frame_path, paths)},
                {"checksum_algorithm", kCalibrationFingerprintAlgorithm},
                {"checksum", source_frame_checksum}
            }
        })},
        {"review_artifacts", {
            {"top_rim_overlay_path", relative_to_artifact_dir(paths.review_overlay_path, paths)},
            {"top_rim_overlay_checksum_algorithm", kCalibrationFingerprintAlgorithm},
            {"top_rim_overlay_checksum", review_overlay_checksum},
            {"registration_hough_overlay_path",
             relative_to_artifact_dir(paths.registration_hough_overlay_path, paths)},
            {"registration_hough_overlay_checksum_algorithm", kCalibrationFingerprintAlgorithm},
            {"registration_hough_overlay_checksum", registration_hough_overlay_checksum},
            {"valid_detection_overlay_path", relative_to_artifact_dir(paths.valid_detection_overlay_path, paths)},
            {"valid_detection_overlay_checksum_algorithm", kCalibrationFingerprintAlgorithm},
            {"valid_detection_overlay_checksum", valid_detection_overlay_checksum}
        }},
        {"circle_detection", {
            {"method", kDishTopRimObservationMethod},
            {"source_array_role", request.source_array_role},
            {"source_frame_index", request.source_frame_index},
            {"detected_circle_source",
             request.has_detected_circle
                 ? (request.detected_circle_source.empty()
                        ? "provided_by_request"
                        : request.detected_circle_source)
                 : "computed_at_write_time"},
            {"image_shape_px", image_shape_json(request.camera.height, request.camera.width)},
            {"detected_circle", circle_to_json(detected_circle)}
        }},
        {"physical_target", kDishTopRimTargetPlane},
        {"observed_boundary", {
            {"surface", kDishTopRimTargetPlane},
            {"target_plane", kDishTopRimTargetPlane},
            {"target_feature", kDishTopRimTargetFeature},
            {"region", kDishTopRimRegion},
            {"coordinate_space", "camera_native_pixels"},
            {"geometry", circle_geometry_to_json(accepted_circle)}
        }},
        {"accepted_inner_rim_boundary", accepted_inner_rim_boundary},
        {"accepted_experimental_area_boundary",
         accepted_experimental_area_boundary_alias},
        {"boundary_interpretation", {
            {"accepted_boundary_field", "accepted_inner_rim_boundary"},
            {"accepted_boundary_role", accepted_boundary_role},
            {"accepted_boundary_semantics", accepted_boundary_interpretation},
            {"observed_boundary_surface", kDishTopRimTargetPlane},
            {"target_plane", kDishTopRimTargetPlane},
            {"target_feature", kDishTopRimTargetFeature},
            {"region", kDishTopRimRegion},
            {"operator_boundary_target", kDishTopRimTargetFeature},
            {"boundary_inclusion_policy", boundary_inclusion_policy},
            {"citrus_runtime_mapping_status", "proposal_pending_citrus_acceptance"},
            {"valid_detection_region_policy",
             outset > 0.0
                 ? "derived_by_outward_offset_for_bounding_box_centroid_forgiveness"
                 : (erosion > 0.0
                        ? "legacy_derived_by_inward_erosion_for_detection_gating"
                        : "matches_primary_inner_rim_boundary")}
        }},
        {"valid_detection_region", {
            {"coordinate_space", "camera_native_pixels"},
            {"derived_from", "accepted_inner_rim_boundary"},
            {"erosion_px", erosion},
            {"centroid_gate_outset_px", outset},
            {"offset_direction",
             outset > 0.0 ? "outward" : (erosion > 0.0 ? "inward" : "none")},
            {"purpose", "bounding_box_centroid_detection_gating"},
            {"geometry", circle_geometry_to_json(valid_circle)}
        }},
        {"accepted_mask", {
            {"shape", "circle"},
            {"derived_from", "accepted_inner_rim_boundary"},
            {"coordinate_space", "camera_native_pixels"},
            {"source_array_role", request.source_array_role},
            {"source_frame_index", request.source_frame_index},
            {"image_shape_px", image_shape_json(request.camera.height, request.camera.width)},
            {"center_px", point_to_json(valid_circle.center)},
            {"radius_px", valid_circle.radius_px},
            {"operator_confirmed", request.operator_confirmed},
            {"accepted_by_operator", request.operator_confirmed},
            {"accepted_at_utc", request.created_utc},
            {"operator_adjustment_px", {
                {"center_dx", rounded_or_zero(center_dx)},
                {"center_dy", rounded_or_zero(center_dy)},
                {"radius_delta", rounded_or_zero(radius_delta - erosion + outset)}
            }}
        }},
        {"quality", {
            {"detector", "hough_circle"},
            {"operator_confirmed", request.operator_confirmed},
            {"quality_flags", nlohmann::json::array()}
        }},
        {"runtime_verification", {
            {"status", request.runtime_verification.status},
            {"reason", request.runtime_verification.reason}
        }},
        {"operator_review", {
            {"status", request.operator_status},
            {"accepted", request.operator_confirmed},
            {"confirmed_target_plane", kDishTopRimTargetPlane},
            {"confirmed_target_feature", kDishTopRimTargetFeature},
            {"confirmed_region", kDishTopRimRegion}
        }},
        {"artifacts", {
            {"observation_path", relative_to_artifact_dir(paths.observation_json_path, paths)},
            {"image_set_path", relative_to_artifact_dir(paths.image_set_json_path, paths)},
            {"source_frame_path", relative_to_artifact_dir(paths.source_frame_path, paths)},
            {"review_overlay_path", relative_to_artifact_dir(paths.review_overlay_path, paths)},
            {"registration_hough_overlay_path",
             relative_to_artifact_dir(paths.registration_hough_overlay_path, paths)},
            {"valid_detection_overlay_path", relative_to_artifact_dir(paths.valid_detection_overlay_path, paths)},
            {"checksum_algorithm", kCalibrationFingerprintAlgorithm},
            {"source_frame_checksum", source_frame_checksum},
            {"review_overlay_checksum", review_overlay_checksum},
            {"registration_hough_overlay_checksum", registration_hough_overlay_checksum},
            {"valid_detection_overlay_checksum", valid_detection_overlay_checksum}
        }},
        {"software", {
            {"orange_git_commit", request.software.orange_git_commit},
            {"orange_git_dirty_tracked", request.software.orange_git_dirty_tracked},
            {"orange_version", request.software.orange_version}
        }},
        {"compatibility_exports", {
            {"palette_dish_mask_v2", {
                {"available", request.write_palette_export},
                {"path", relative_to_artifact_dir(paths.palette_export_path, paths)},
                {"mapping", "orange_circle_mask_to_palette_dish_mask_v2"},
                {"target", "analysis_metadata.attrs.dish_mask"}
            }},
            {"spatial_dish_mask_runtime_v1", {
                {"available", true},
                {"path", relative_to_artifact_dir(paths.spatial_dish_mask_runtime_export_path, paths)},
                {"mapping", "orange_top_rim_observation_to_spatial_dish_mask_runtime_v1"},
                {"target", "dish_mask_runtime.json"}
            }}
        }}
    };
    if (has_physical_inner_diameter) {
        observation["valid_detection_region"]["centroid_gate_outset_mm"] =
            centroid_gate_outset_mm;
        observation["accepted_mask"]["physical_radius_mm"] =
            physical_inner_radius_mm + centroid_gate_outset_mm;
        observation["accepted_mask"]["physical_scale_ref"] =
            "accepted_inner_rim_boundary.camera_scale";
    }
    if (!request.arena_context.empty()) {
        observation["arena_context"] = request.arena_context;
    }
    if (!request.operator_notes.empty()) {
        observation["operator_review"]["notes"] = request.operator_notes;
    }
    return observation;
}

nlohmann::json dish_top_rim_observation_manifest_to_json(
    const DishTopRimObservationRequest& request,
    const DishTopRimObservationArtifactPaths& paths,
    const std::string& source_frame_checksum,
    const std::string& review_overlay_checksum,
    const std::string& registration_hough_overlay_checksum,
    const std::string& valid_detection_overlay_checksum,
    const std::string& fingerprint)
{
    nlohmann::json compatibility = {
        {"camera_serial", request.camera.serial},
        {"pixel_format", request.camera.pixel_format},
        {"width", request.camera.width},
        {"height", request.camera.height},
        {"physical_target", kDishTopRimTargetPlane},
        {"target_feature", kDishTopRimTargetFeature},
        {"region", kDishTopRimRegion},
        {"coordinate_space", "camera_native_pixels"},
        {"source_array_role", request.source_array_role}
    };
    nlohmann::json summary = {
        {"method", kDishTopRimObservationMethod},
        {"operator_confirmed", request.operator_confirmed},
        {"runtime_verification_status", request.runtime_verification.status},
        {"camera_serial", request.camera.serial},
        {"physical_target", kDishTopRimTargetPlane},
        {"target_feature", kDishTopRimTargetFeature},
        {"region", kDishTopRimRegion},
        {"accepted_boundary_role", request.accepted_boundary_role.empty()
                                       ? kDishTopRimBoundaryRole
                                       : request.accepted_boundary_role},
        {"boundary_inclusion_policy", request.boundary_inclusion_policy.empty()
                                          ? kDishTopRimBoundaryInclusionPolicy
                                          : request.boundary_inclusion_policy},
        {"operator_boundary_target", kDishTopRimTargetFeature},
        {"citrus_runtime_mapping_status", "proposal_pending_citrus_acceptance"},
        {"dish_fill_state", request.capture.dish_fill_state},
        {"coordinate_space", "camera_native_pixels"}
    };
    if (!request.arena_context.empty()) {
        compatibility["arena_context"] = request.arena_context;
        summary["arena_context"] = request.arena_context;
        const std::string arena_id = request.arena_context.value("arena_id", "");
        const std::string canvas_id = request.arena_context.value("canvas_id", "");
        const std::string associated_image_set_artifact_id =
            request.arena_context.value("associated_image_set_artifact_id", "");
        if (!arena_id.empty()) {
            summary["arena_id"] = arena_id;
        }
        if (!canvas_id.empty()) {
            summary["canvas_id"] = canvas_id;
        }
        if (!associated_image_set_artifact_id.empty()) {
            summary["associated_image_set_artifact_id"] =
                associated_image_set_artifact_id;
        }
    }
    return {
        {"schema_id", kCalibrationManifestSchemaId},
        {"schema_version", kCalibrationManifestSchemaVersion},
        {"artifact_id", request.artifact_id},
        {"artifact_schema_id", kDishTopRimObservationSchemaId},
        {"artifact_schema_version", kDishTopRimObservationSchemaVersion},
        {"created_utc", request.created_utc},
        {"producer", {
            {"application", "orange"},
            {"artifact_type", "dish_top_rim_observation"}
        }},
        {"storage", {
            {"artifact_root", paths.artifact_root_dir},
            {"artifact_dir", paths.artifact_dir},
            {"relative_artifact_dir", paths.relative_artifact_dir},
            {"relative_manifest_path", relative_to_artifact_root(paths.manifest_path, paths)}
        }},
        {"calibration_ref", calibration_ref_json(request.artifact_id, fingerprint)},
        {"compatibility", compatibility},
        {"summary", summary},
        {"files", {
            {"manifest", relative_to_artifact_dir(paths.manifest_path, paths)},
            {"observation_json", relative_to_artifact_dir(paths.observation_json_path, paths)},
            {"image_set_json", relative_to_artifact_dir(paths.image_set_json_path, paths)},
            {"source_frame", relative_to_artifact_dir(paths.source_frame_path, paths)},
            {"review_overlay", relative_to_artifact_dir(paths.review_overlay_path, paths)},
            {"registration_hough_overlay",
             relative_to_artifact_dir(paths.registration_hough_overlay_path, paths)},
            {"valid_detection_overlay", relative_to_artifact_dir(paths.valid_detection_overlay_path, paths)},
            {"palette_dish_mask_v2", relative_to_artifact_dir(paths.palette_export_path, paths)},
            {"spatial_dish_mask_runtime_v1",
             relative_to_artifact_dir(paths.spatial_dish_mask_runtime_export_path, paths)}
        }},
        {"checksums", {
            {"algorithm", kCalibrationFingerprintAlgorithm},
            {"source_frame", source_frame_checksum},
            {"review_overlay", review_overlay_checksum},
            {"registration_hough_overlay", registration_hough_overlay_checksum},
            {"valid_detection_overlay", valid_detection_overlay_checksum}
        }}
    };
}

CalibrationImageSetRequest build_dish_top_rim_image_set_request(
    const DishTopRimObservationRequest& request,
    const DishTopRimObservationArtifactPaths& paths,
    const std::string& source_frame_checksum,
    const std::string& review_overlay_checksum,
    const std::string& registration_hough_overlay_checksum,
    const std::string& valid_detection_overlay_checksum,
    const std::string& observation_fingerprint,
    const nlohmann::json& observation_json)
{
    CalibrationImageSetRequest image_set;
    image_set.artifact_id = request.artifact_id;
    image_set.created_utc = request.created_utc;
    image_set.purpose = "dish_top_rim";
    image_set.target_plane = "dish_top_rim";
    image_set.coordinate_space = "camera_native_pixels";
    image_set.camera.serial = request.camera.serial;
    image_set.camera.name = request.camera.name;
    image_set.camera.image_shape.height = request.camera.height;
    image_set.camera.image_shape.width = request.camera.width;
    image_set.camera.pixel_format = request.camera.pixel_format;
    image_set.camera.configured_height = request.camera.height;
    image_set.camera.configured_width = request.camera.width;

    image_set.capture.operation_id = request.capture.operation_id;
    image_set.capture.timestamp_utc = request.created_utc;
    image_set.capture.capture_mode = request.capture.capture_mode;
    image_set.capture.source_frame_count = request.capture.source_frame_count;
    image_set.capture.has_source_frame_count = request.capture.has_source_frame_count;
    image_set.capture.temporal_compositing_method =
        request.capture.temporal_compositing_method;
    image_set.capture.first_local_frame_id = request.capture.first_local_frame_id;
    image_set.capture.last_local_frame_id = request.capture.last_local_frame_id;
    image_set.capture.has_local_frame_range = request.capture.has_local_frame_range;
    image_set.capture.first_camera_frame_id = request.capture.first_camera_frame_id;
    image_set.capture.last_camera_frame_id = request.capture.last_camera_frame_id;
    image_set.capture.has_camera_frame_range = request.capture.has_camera_frame_range;
    image_set.capture.exposure_us = request.capture.exposure_us;
    image_set.capture.has_exposure_us = true;
    image_set.capture.frame_rate_hz = request.capture.frame_rate_hz;
    image_set.capture.has_frame_rate_hz = true;
    image_set.capture.filter_state = request.capture.filter_state;
    image_set.capture.runtime_filter_state = request.capture.runtime_filter_state;
    image_set.capture.light_handling = request.capture.light_handling;
    image_set.capture.light_state = request.capture.light_state;
    image_set.capture.illumination_spectrum = request.capture.illumination_spectrum;
    image_set.capture.illumination_source = request.capture.illumination_source;
    image_set.capture.illumination_center_wavelength_nm =
        request.capture.illumination_center_wavelength_nm;
    image_set.capture.has_illumination_center_wavelength_nm =
        request.capture.has_illumination_center_wavelength_nm;
    image_set.capture.illumination_min_wavelength_nm =
        request.capture.illumination_min_wavelength_nm;
    image_set.capture.has_illumination_min_wavelength_nm =
        request.capture.has_illumination_min_wavelength_nm;
    image_set.capture.illumination_max_wavelength_nm =
        request.capture.illumination_max_wavelength_nm;
    image_set.capture.has_illumination_max_wavelength_nm =
        request.capture.has_illumination_max_wavelength_nm;
    image_set.capture.illumination_bandwidth_fwhm_nm =
        request.capture.illumination_bandwidth_fwhm_nm;
    image_set.capture.has_illumination_bandwidth_fwhm_nm =
        request.capture.has_illumination_bandwidth_fwhm_nm;
    image_set.capture.illumination_wavelength_confidence =
        request.capture.illumination_wavelength_confidence;
    image_set.capture.projector_state = request.capture.projector_state;
    image_set.capture.projector_visible_to_camera = request.capture.projector_visible_to_camera;
    image_set.capture.has_projector_visible_to_camera = true;
    image_set.capture.requires_camera_mount_unchanged =
        request.capture.requires_camera_mount_unchanged;
    image_set.capture.has_requires_camera_mount_unchanged = true;
    image_set.capture.requires_filter_reinstalled_repeatably =
        request.capture.requires_filter_reinstalled_repeatably;
    image_set.capture.has_requires_filter_reinstalled_repeatably = true;

    if (!request.image_set_rig_context.empty()) {
        image_set.rig_context = request.image_set_rig_context;
    }
    if (!request.arena_context.empty()) {
        if (!image_set.rig_context.is_object()) {
            image_set.rig_context = nlohmann::json::object();
        }
        image_set.rig_context["arena_context"] = request.arena_context;
    }

    const CalibrationImageSetShape image_shape{request.camera.height, request.camera.width};
    image_set.images.push_back(CalibrationImageSetImageRef{
        "source",
        relative_to_artifact_dir(paths.source_frame_path, paths),
        kCalibrationFingerprintAlgorithm,
        source_frame_checksum,
        "camera_native_pixels",
        image_shape,
        "full-resolution source frame used for top-rim fitting"});

    image_set.review_artifacts = {
        {"top_rim_overlay", {
            {"path", relative_to_artifact_dir(paths.review_overlay_path, paths)},
            {"checksum_algorithm", kCalibrationFingerprintAlgorithm},
            {"checksum", review_overlay_checksum}
        }},
        {"registration_hough_overlay", {
            {"path", relative_to_artifact_dir(paths.registration_hough_overlay_path, paths)},
            {"checksum_algorithm", kCalibrationFingerprintAlgorithm},
            {"checksum", registration_hough_overlay_checksum}
        }},
        {"valid_detection_overlay", {
            {"path", relative_to_artifact_dir(paths.valid_detection_overlay_path, paths)},
            {"checksum_algorithm", kCalibrationFingerprintAlgorithm},
            {"checksum", valid_detection_overlay_checksum}
        }}
    };

    image_set.observations = {
        {"dish_top_rim", {
            {"accepted_boundary",
             observation_json.value(
                 "accepted_inner_rim_boundary",
                 observation_json.value(
                     "accepted_experimental_area_boundary",
                     observation_json.value("observed_boundary", nlohmann::json::object())))},
            {"accepted_inner_rim_boundary",
             observation_json.value(
                 "accepted_inner_rim_boundary",
                 observation_json.value(
                     "accepted_experimental_area_boundary",
                     nlohmann::json::object()))},
            {"accepted_experimental_area_boundary",
             observation_json.value(
                 "accepted_experimental_area_boundary",
                 nlohmann::json::object())},
            {"observed_boundary",
             observation_json.value("observed_boundary", nlohmann::json::object())},
            {"boundary_interpretation",
             observation_json.value("boundary_interpretation", nlohmann::json::object())},
            {"valid_detection_region",
             observation_json.value("valid_detection_region", nlohmann::json::object())},
            {"accepted_mask",
             observation_json.value("accepted_mask", nlohmann::json::object())},
            {"fit_quality",
             observation_json.value("quality", nlohmann::json::object())}
        }}
    };
    if (!request.arena_context.empty()) {
        image_set.observations["arena_context"] = request.arena_context;
    }

    image_set.derived_artifacts.push_back(CalibrationImageSetArtifactRef{
        request.artifact_id,
        kDishTopRimObservationSchemaId,
        kDishTopRimObservationSchemaVersion,
        observation_fingerprint});

    image_set.citrus_preview = {
        {"available", false},
        {"diagnostic_only", true},
        {"authority", "citrus_recomputes_before_acceptance"}
    };
    image_set.operator_notes = request.operator_notes;

    return image_set;
}

nlohmann::json dish_top_rim_palette_export_to_json(
    const nlohmann::json& observation_json)
{
    const nlohmann::json& mask = observation_json.at("accepted_mask");
    const nlohmann::json& image_shape = mask.at("image_shape_px");
    const double width = image_shape.at("width").get<double>();
    const double height = image_shape.at("height").get<double>();
    const double radius = mask.at("radius_px").get<double>();
    const double x = mask.at("center_px").at("x").get<double>();
    const double y = mask.at("center_px").at("y").get<double>();
    const double area = 3.14159265358979323846 * radius * radius;
    const std::string source_array_role = mask.value("source_array_role", "images_full");
    const int frame_index = mask.value("source_frame_index", 0);

    nlohmann::json out = {
        {"shape", "circle"},
        {"version", "2.0"},
        {"method", kDishTopRimObservationMethod},
        {"tuned_timestamp", observation_json.value("created_utc", "")},
        {"source", {
            {"array", source_array_role},
            {"frame", frame_index}
        }},
        {"tuned_on_array", source_array_role},
        {"tuned_on_frame", frame_index},
        {"detected_circle", {
            {"center", nlohmann::json::array({static_cast<int>(std::lround(x)),
                                              static_cast<int>(std::lround(y))})},
            {"radius", static_cast<int>(std::lround(radius))}
        }},
        {"metrics", {
            {"image_shape", nlohmann::json::array({static_cast<int>(std::lround(height)),
                                                   static_cast<int>(std::lround(width))})},
            {"center_px", nlohmann::json::array({static_cast<int>(std::lround(x)),
                                                 static_cast<int>(std::lround(y))})},
            {"center_norm", nlohmann::json::array({width > 0.0 ? x / width : 0.0,
                                                   height > 0.0 ? y / height : 0.0})},
            {"radius_px", static_cast<int>(std::lround(radius))},
            {"radius_norm", std::min(width, height) > 0.0 ? radius / std::min(width, height) : 0.0},
            {"area_px", area},
            {"area_fraction", width > 0.0 && height > 0.0 ? area / (width * height) : 0.0}
        }},
        {"orange_artifact_id", observation_json.value("artifact_id", "")},
        {"orange_artifact_schema_id", observation_json.value("schema_id", "")},
        {"orange_artifact_schema_version", observation_json.value("schema_version", 0)},
        {"orange_artifact_fingerprint",
         observation_json.value("calibration_ref", nlohmann::json::object()).value("fingerprint", "")}
    };

    if (observation_json.contains("circle_detection") &&
        observation_json["circle_detection"].contains("hough_params")) {
        const nlohmann::json& hough = observation_json["circle_detection"]["hough_params"];
        out["hough_params"] = {
            {"param1", hough.value("param1", 0.0)},
            {"param2", hough.value("param2", 0.0)},
            {"radius_adjustment", hough.value("radius_adjustment_px", 0.0)}
        };
    }
    return out;
}

nlohmann::json dish_top_rim_spatial_dish_mask_runtime_export_to_json(
    const nlohmann::json& observation_json)
{
    const nlohmann::json observed_boundary =
        observation_json.value("observed_boundary", nlohmann::json::object());
    const nlohmann::json accepted_boundary =
        observation_json.value(
            "accepted_inner_rim_boundary",
            observation_json.value(
                "accepted_experimental_area_boundary",
                observed_boundary));
    const nlohmann::json accepted_geometry =
        accepted_boundary.value("geometry", nlohmann::json::object());
    const nlohmann::json valid_region =
        observation_json.value("valid_detection_region", nlohmann::json::object());
    const nlohmann::json valid_geometry =
        valid_region.value("geometry", nlohmann::json::object());

    const auto runtime_circle = [](const nlohmann::json& geometry) {
        const nlohmann::json center = geometry.value("center_px", nlohmann::json::object());
        return nlohmann::json{
            {"type", "circle"},
            {"cx", center.value("x", 0.0)},
            {"cy", center.value("y", 0.0)},
            {"r", geometry.value("radius_px", 0.0)}
        };
    };

    const std::string coordinate_space =
        valid_region.value(
            "coordinate_space",
            accepted_boundary.value("coordinate_space", "camera_native_pixels"));

    return {
        {"schema_version", 1},
        {"enabled", true},
        {"source", "detected_fit"},
        {"geometry", {
            {"coordinate_space", coordinate_space},
            {"outer_geometry", runtime_circle(accepted_geometry)},
            {"valid_geometry", runtime_circle(valid_geometry)},
            {"edge_margin_px", valid_region.value("erosion_px", 0.0)},
            {"centroid_gate_outset_px",
             valid_region.value("centroid_gate_outset_px", 0.0)}
        }},
        {"source_observation", {
            {"artifact_id", observation_json.value("artifact_id", "")},
            {"artifact_schema_id", observation_json.value("schema_id", "")},
            {"artifact_schema_version", observation_json.value("schema_version", 0)},
            {"fingerprint",
             observation_json.value("calibration_ref", nlohmann::json::object()).value("fingerprint", "")},
            {"accepted_inner_rim_boundary",
             observation_json.value(
                 "accepted_inner_rim_boundary",
                 observation_json.value(
                     "accepted_experimental_area_boundary",
                     nlohmann::json::object()))},
            {"accepted_experimental_area_boundary",
             observation_json.value("accepted_experimental_area_boundary", nlohmann::json::object())},
            {"boundary_interpretation",
             observation_json.value("boundary_interpretation", nlohmann::json::object())}
        }}
    };
}

std::string compute_dish_top_rim_observation_fingerprint(
    const nlohmann::json& observation_json,
    const DishTopRimObservationArtifactPaths& paths,
    std::string* error_out)
{
    nlohmann::json payload = observation_json;
    if (payload.contains("calibration_ref") && payload["calibration_ref"].is_object()) {
        payload["calibration_ref"]["fingerprint"] = "";
    }
    if (payload.contains("compatibility_exports") &&
        payload["compatibility_exports"].contains("palette_dish_mask_v2")) {
        payload["compatibility_exports"]["palette_dish_mask_v2"]["available"] = false;
    }

    uint64_t hash = kFnv1a64Offset;
    const std::string json_payload = payload.dump();
    fnv1a64_update(
        &hash,
        reinterpret_cast<const unsigned char*>(json_payload.data()),
        json_payload.size());

    for (const std::string& file_path : {
             paths.source_frame_path,
             paths.review_overlay_path,
             paths.registration_hough_overlay_path,
             paths.valid_detection_overlay_path}) {
        std::vector<unsigned char> bytes;
        if (!read_file_bytes(file_path, &bytes, error_out)) {
            return "";
        }
        if (!bytes.empty()) {
            fnv1a64_update(&hash, bytes.data(), bytes.size());
        }
    }
    return fnv1a64_to_string(hash);
}

bool write_dish_top_rim_observation_artifact(
    const std::string& artifact_root_dir,
    const DishTopRimObservationRequest& request,
    const cv::Mat& source_image,
    const DishTopRimHoughParams& hough_params,
    const DishTopRimCircle& accepted_circle,
    DishTopRimObservationWriteResult* result_out,
    std::string* error_out)
{
    if (artifact_root_dir.empty()) {
        return set_error(error_out, "artifact root directory is empty");
    }
    if (request.artifact_id.empty()) {
        return set_error(error_out, "artifact_id is empty");
    }
    if (request.created_utc.empty()) {
        return set_error(error_out, "created_utc is empty");
    }
    if (request.camera.serial.empty()) {
        return set_error(error_out, "camera serial is empty");
    }
    if (request.source_array_role != "images_full") {
        return set_error(
            error_out,
            "source_array_role must be images_full for top-rim observations; Citrus homography images are full resolution");
    }
    if (!request.operator_confirmed) {
        return set_error(
            error_out,
            "schema-v2 top-rim observations require explicit operator confirmation of the water-side inner-rim target");
    }
    if (request.operator_boundary_target != kDishTopRimTargetFeature) {
        return set_error(
            error_out,
            std::string("operator_boundary_target must be ") +
                kDishTopRimTargetFeature + " for schema-v2 observations");
    }
    if (request.boundary_inclusion_policy != kDishTopRimBoundaryInclusionPolicy) {
        return set_error(
            error_out,
            std::string("boundary_inclusion_policy must be ") +
                kDishTopRimBoundaryInclusionPolicy + " for schema-v2 observations");
    }
    if (source_image.empty()) {
        return set_error(error_out, "source image is empty");
    }
    if (request.camera.width != source_image.cols || request.camera.height != source_image.rows) {
        return set_error(error_out, "camera image_shape does not match source image dimensions");
    }
    if (!validate_circle(accepted_circle, source_image.cols, source_image.rows, "accepted_circle", error_out)) {
        return false;
    }
    if (!std::isfinite(request.valid_region_erosion_px) ||
        request.valid_region_erosion_px < 0.0 ||
        !std::isfinite(request.centroid_gate_outset_px) ||
        request.centroid_gate_outset_px < 0.0) {
        return set_error(
            error_out,
            "valid-region erosion and centroid-gate outset must be finite and >= 0");
    }
    if (request.valid_region_erosion_px > 0.0 &&
        request.centroid_gate_outset_px > 0.0) {
        return set_error(
            error_out,
            "valid-region erosion and centroid-gate outset are mutually exclusive");
    }
    if (accepted_circle.radius_px - request.valid_region_erosion_px +
            request.centroid_gate_outset_px <=
        0.0) {
        return set_error(error_out, "valid detection region has non-positive radius");
    }
    if (request.has_physical_inner_diameter_mm &&
        (!std::isfinite(request.physical_inner_diameter_mm) ||
         request.physical_inner_diameter_mm <= 0.0)) {
        return set_error(error_out, "physical inner diameter must be finite and > 0 mm");
    }
    if (request.has_physical_inner_diameter_mm &&
        request.physical_inner_diameter_source.empty()) {
        return set_error(
            error_out,
            "physical inner diameter requires a non-empty dimension source");
    }
    if (request.has_reference_camera_pixels_per_mm &&
        (!std::isfinite(request.reference_camera_pixels_per_mm) ||
         request.reference_camera_pixels_per_mm <= 0.0)) {
        return set_error(error_out, "reference camera scale must be finite and > 0 px/mm");
    }

    const DishTopRimObservationArtifactPaths paths =
        make_dish_top_rim_observation_artifact_paths(
            artifact_root_dir,
            request.artifact_id,
            request.storage_relative_artifact_dir);
    {
        orange::ScopedFsuid fsuid_guard;
        (void)fsuid_guard;
        std::filesystem::create_directories(paths.artifact_dir);
    }

    DishTopRimCircle detected_circle;
    if (request.has_detected_circle) {
        detected_circle = request.detected_circle;
        if (!validate_circle(
                detected_circle,
                source_image.cols,
                source_image.rows,
                "detected_circle",
                error_out)) {
            return false;
        }
    } else {
        if (!detect_dish_top_rim_hough_circle(
                source_image,
                hough_params,
                &detected_circle,
                error_out)) {
            return false;
        }
    }

    const cv::Mat source_png = normalize_source_for_png(source_image);
    if (!write_image_file(paths.source_frame_path, source_png, error_out)) {
        return false;
    }

    DishTopRimCircle valid_circle = accepted_circle;
    valid_circle.radius_px =
        accepted_circle.radius_px - request.valid_region_erosion_px +
        request.centroid_gate_outset_px;
    const cv::Mat overlay = make_overlay(source_image, detected_circle, accepted_circle, valid_circle);
    if (!write_image_file(paths.review_overlay_path, overlay, error_out) ||
        !write_image_file(paths.registration_hough_overlay_path, overlay, error_out) ||
        !write_image_file(paths.valid_detection_overlay_path, overlay, error_out)) {
        return false;
    }

    const std::string source_checksum = compute_file_fingerprint(paths.source_frame_path, error_out);
    if (source_checksum.empty()) {
        return false;
    }
    const std::string review_checksum = compute_file_fingerprint(paths.review_overlay_path, error_out);
    if (review_checksum.empty()) {
        return false;
    }
    const std::string registration_hough_overlay_checksum =
        compute_file_fingerprint(paths.registration_hough_overlay_path, error_out);
    if (registration_hough_overlay_checksum.empty()) {
        return false;
    }
    const std::string valid_overlay_checksum =
        compute_file_fingerprint(paths.valid_detection_overlay_path, error_out);
    if (valid_overlay_checksum.empty()) {
        return false;
    }

    nlohmann::json observation = dish_top_rim_observation_to_json(
        request,
        detected_circle,
        accepted_circle,
        paths,
        source_checksum,
        review_checksum,
        registration_hough_overlay_checksum,
        valid_overlay_checksum,
        "");
    observation["circle_detection"]["hough_params"] = {
        {"dp", hough_params.dp},
        {"min_dist_px", hough_params.min_dist_px},
        {"param1", hough_params.param1},
        {"param2", hough_params.param2},
        {"min_radius_px", hough_params.min_radius_px},
        {"max_radius_px", hough_params.max_radius_px},
        {"max_detection_dimension_px", hough_params.max_detection_dimension_px},
        {"detection_scale", hough_params.detection_scale},
        {"radius_adjustment_px", hough_params.radius_adjustment_px}
    };

    const std::string fingerprint =
        compute_dish_top_rim_observation_fingerprint(observation, paths, error_out);
    if (fingerprint.empty()) {
        return false;
    }
    observation["calibration_ref"]["fingerprint"] = fingerprint;

    nlohmann::json palette_export = nlohmann::json::object();
    if (request.write_palette_export) {
        palette_export = dish_top_rim_palette_export_to_json(observation);
        if (!write_json_file(paths.palette_export_path, palette_export, error_out)) {
            return false;
        }
    }
    const nlohmann::json spatial_dish_mask_runtime_export =
        dish_top_rim_spatial_dish_mask_runtime_export_to_json(observation);
    if (!write_json_file(
            paths.spatial_dish_mask_runtime_export_path,
            spatial_dish_mask_runtime_export,
            error_out)) {
        return false;
    }

    nlohmann::json image_set = nlohmann::json::object();
    if (request.write_image_set_companion) {
        CalibrationImageSetWriteResult image_set_result;
        const CalibrationImageSetRequest image_set_request =
            build_dish_top_rim_image_set_request(
                request,
                paths,
                source_checksum,
                review_checksum,
                registration_hough_overlay_checksum,
                valid_overlay_checksum,
                fingerprint,
                observation);
        if (!write_calibration_image_set_json_file(
                paths.image_set_json_path,
                image_set_request,
                &image_set_result,
                error_out)) {
            return false;
        }
        image_set = image_set_result.image_set;
    }

    if (!write_json_file(paths.observation_json_path, observation, error_out)) {
        return false;
    }
    const nlohmann::json manifest = dish_top_rim_observation_manifest_to_json(
        request,
        paths,
        source_checksum,
        review_checksum,
        registration_hough_overlay_checksum,
        valid_overlay_checksum,
        fingerprint);
    if (!write_json_file(paths.manifest_path, manifest, error_out)) {
        return false;
    }
    if (!update_local_calibration_registry(std::filesystem::path(artifact_root_dir), manifest, error_out)) {
        return false;
    }

    if (result_out) {
        result_out->artifact_id = request.artifact_id;
        result_out->artifact_dir = paths.artifact_dir;
        result_out->fingerprint = fingerprint;
        result_out->manifest = manifest;
        result_out->observation = observation;
        result_out->image_set = image_set;
        result_out->palette_export = palette_export;
    }
    return true;
}

nlohmann::json build_dish_top_rim_recording_snapshot_entry(
    const nlohmann::json& observation_json,
    bool active_for_detection_gating)
{
    nlohmann::json entry;
    entry["artifact_id"] = observation_json.value("artifact_id", "");
    entry["artifact_schema_id"] = observation_json.value("schema_id", "");
    entry["artifact_schema_version"] = observation_json.value("schema_version", 0);
    entry["calibration_ref"] = observation_json.value("calibration_ref", nlohmann::json::object());
    entry["physical_target"] = observation_json.value("physical_target", "dish_top_rim");
    entry["coordinate_space"] =
        observation_json.value("accepted_mask", nlohmann::json::object()).value(
            "coordinate_space",
            "camera_native_pixels");
    entry["accepted_mask"] = observation_json.value("accepted_mask", nlohmann::json::object());
    entry["accepted_inner_rim_boundary"] =
        observation_json.value(
            "accepted_inner_rim_boundary",
            observation_json.value(
                "accepted_experimental_area_boundary",
                nlohmann::json::object()));
    entry["accepted_experimental_area_boundary"] =
        observation_json.value(
            "accepted_experimental_area_boundary",
            nlohmann::json::object());
    entry["boundary_interpretation"] =
        observation_json.value("boundary_interpretation", nlohmann::json::object());
    entry["review_artifacts"] = observation_json.value("review_artifacts", nlohmann::json::object());
    entry["runtime_verification"] =
        observation_json.value("runtime_verification", nlohmann::json::object());
    entry["active_for_detection_gating"] = active_for_detection_gating;
    entry["gating_policy"] =
        active_for_detection_gating ? "reject_outside_region_and_log" : "not_enabled";
    return entry;
}

bool apply_dish_top_rim_observation_to_snapshot_json(
    nlohmann::json* snapshot,
    const std::string& camera_serial,
    const nlohmann::json& observation_json,
    bool active_for_detection_gating,
    std::string* error_out)
{
    if (!snapshot) {
        return set_error(error_out, "null recording snapshot destination");
    }
    if (camera_serial.empty()) {
        return set_error(error_out, "camera serial is empty");
    }
    if (!observation_json.is_object()) {
        return set_error(error_out, "observation_json must be an object");
    }
    if (observation_json.value("schema_id", "") != kDishTopRimObservationSchemaId) {
        return set_error(error_out, "observation_json schema_id is not dish_top_rim_observation");
    }
    if (!snapshot->is_object()) {
        *snapshot = nlohmann::json::object();
    }
    if (!snapshot->contains("calibrations") || !(*snapshot)["calibrations"].is_object()) {
        (*snapshot)["calibrations"] = nlohmann::json::object();
    }
    if (!(*snapshot)["calibrations"].contains(camera_serial) ||
        !(*snapshot)["calibrations"][camera_serial].is_object()) {
        (*snapshot)["calibrations"][camera_serial] = nlohmann::json::object();
    }
    (*snapshot)["calibrations"][camera_serial]["dish_top_rim_observation"] =
        build_dish_top_rim_recording_snapshot_entry(observation_json, active_for_detection_gating);
    return true;
}

} // namespace orange::calibration
