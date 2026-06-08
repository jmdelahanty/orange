#include "spatial_layout_ui.h"

#include "camera_preview_utils.h"
#include "dish_top_rim_observation.h"
#include "fsuid_guard.h"
#include "imgui.h"
#include "implot.h"
#include "misc/cpp/imgui_stdlib.h"
#include <ImGuiFileDialog.h>
#include <opencv2/opencv.hpp>
#include "project.h"
#include "spatial_snapshot_worker.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <sstream>
#include <vector>

namespace {

using orange::spatial::ArenaLayoutArtifact;
using orange::spatial::ArenaLayoutProvenanceSource;
using orange::spatial::ArenaLayoutRuntime;
using orange::spatial::ArenaLayoutZone;
using orange::spatial::CalibrationRef;
using orange::spatial::CircleGeometry;
using orange::spatial::CoordinateSpace;
using orange::spatial::DishMaskGeometry;
using orange::spatial::DishMaskRuntime;
using orange::spatial::LayoutGeometry;
using orange::spatial::LayoutGeometryType;
using orange::spatial::ObservationSource;
using orange::spatial::OrientationStatus;
using orange::spatial::RegistrationSource;
using orange::spatial::RegistrationType;
using orange::spatial::ResolvedZoneOverlay;
using orange::spatial::RuntimeGeometry;
using orange::spatial::RuntimeGeometryType;
using orange::spatial::ViewRegistration;
using orange::spatial::VisibilityStatus;

constexpr int kSpatialCaptureBufferCount = 2;
constexpr double kPi = 3.14159265358979323846;
constexpr const char* kCalibrationManifestSchemaId = "orange.calibration.manifest";
constexpr int kCalibrationManifestSchemaVersion = 1;
constexpr const char* kCalibrationFingerprintAlgorithm = "fnv1a64";
constexpr uint64_t kFnv1a64Offset = 14695981039346656037ULL;
constexpr uint64_t kFnv1a64Prime = 1099511628211ULL;
constexpr const char* kSpatialLayoutMeasurementFilename = "measurement.json";
constexpr const char* kSpatialLayoutManifestFilename = "manifest.json";
constexpr const char* kSpatialLayoutArenaLayoutRuntimeFilename = "arena_layout_runtime.json";
constexpr const char* kSpatialLayoutDishMaskRuntimeFilename = "dish_mask_runtime.json";
constexpr const char* kLoadSpatialLayoutDialogId = "LoadSpatialLayoutArtifact";
constexpr const char* kLoadCitrusArenaConfigDialogId = "LoadCitrusArenaConfig";
constexpr int kProjectedCircleSampleCount = 96;

struct Point2d {
    double x = 0.0;
    double y = 0.0;
};

struct SpatialLayoutPersistedFiles {
    std::filesystem::path artifact_dir;
    std::filesystem::path measurement_path;
    std::filesystem::path manifest_path;
    std::filesystem::path arena_layout_runtime_path;
    std::filesystem::path dish_mask_runtime_path;
};

Point2d transform_point(const std::array<double, 9>& matrix, double x, double y);
void set_registration_transform(SpatialLayoutUiState* ui_state,
                                RegistrationType type,
                                const Point2d& desired_outer_center,
                                double scale,
                                double rotation_deg_clockwise,
                                RegistrationSource source);

std::string default_citrus_rigs_root()
{
    const std::filesystem::path citrus_rigs_root("/home/jeremy/citrus/targets/rigs");
    if (std::filesystem::exists(citrus_rigs_root)) {
        return citrus_rigs_root.string();
    }
    return ".";
}

std::string sanitize_artifact_component(const std::string& value)
{
    std::string sanitized;
    sanitized.reserve(value.size());
    for (char ch : value) {
        if ((ch >= 'a' && ch <= 'z') ||
            (ch >= 'A' && ch <= 'Z') ||
            (ch >= '0' && ch <= '9')) {
            sanitized.push_back(ch);
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
        sanitized = "spatial_layout";
    }
    return sanitized;
}

void fnv1a64_update_bytes(uint64_t* hash, const void* data, size_t size)
{
    if (hash == nullptr || data == nullptr) {
        return;
    }
    const auto* bytes = static_cast<const unsigned char*>(data);
    for (size_t i = 0; i < size; ++i) {
        *hash ^= static_cast<uint64_t>(bytes[i]);
        *hash *= kFnv1a64Prime;
    }
}

std::string compute_json_fingerprint(const nlohmann::json& value)
{
    nlohmann::json fingerprint_payload = value;
    if (fingerprint_payload.contains("calibration_ref") &&
        fingerprint_payload["calibration_ref"].is_object()) {
        fingerprint_payload["calibration_ref"]["fingerprint"] = "";
    }
    const std::string payload = fingerprint_payload.dump();
    uint64_t hash = kFnv1a64Offset;
    fnv1a64_update_bytes(&hash, payload.data(), payload.size());

    std::ostringstream oss;
    oss << kCalibrationFingerprintAlgorithm << ':'
        << std::hex << std::nouppercase << hash;
    return oss.str();
}

bool write_json_file(const std::filesystem::path& path,
                     const nlohmann::json& value,
                     std::string* error_out)
{
    orange::ScopedFsuid fsuid_guard;
    (void)fsuid_guard;
    std::ofstream out(path, std::ios::out | std::ios::binary | std::ios::trunc);
    if (!out.is_open()) {
        if (error_out) {
            *error_out = "Failed to open JSON output path: " + path.string();
        }
        return false;
    }
    out << value.dump(2) << '\n';
    if (!out.good()) {
        if (error_out) {
            *error_out = "Failed to write JSON output path: " + path.string();
        }
        return false;
    }
    return true;
}

bool read_json_file(const std::filesystem::path& path,
                    nlohmann::json* value,
                    std::string* error_out)
{
    if (value == nullptr) {
        if (error_out) {
            *error_out = "Null JSON destination.";
        }
        return false;
    }
    std::ifstream in(path, std::ios::in | std::ios::binary);
    if (!in.is_open()) {
        if (error_out) {
            *error_out = "Failed to open JSON input path: " + path.string();
        }
        return false;
    }

    try {
        in >> *value;
    } catch (const std::exception& ex) {
        if (error_out) {
            *error_out = std::string("Failed to parse JSON from ") + path.string() + ": " + ex.what();
        }
        return false;
    }
    return true;
}

std::string build_arena_layout_artifact_id(
    const std::string& prefix_base,
    const CameraParams& camera_params,
    const std::string& timestamp_label)
{
    std::ostringstream oss;
    oss << "arenalayout_" << sanitize_artifact_component(prefix_base)
        << "_" << sanitize_artifact_component(timestamp_label)
        << "_Cam" << camera_params.camera_serial;
    return oss.str();
}

SpatialLayoutPersistedFiles make_spatial_layout_persisted_files(
    const std::string& artifact_root_dir,
    const std::string& artifact_id)
{
    SpatialLayoutPersistedFiles files;
    files.artifact_dir = std::filesystem::path(artifact_root_dir) / artifact_id;
    files.measurement_path = files.artifact_dir / kSpatialLayoutMeasurementFilename;
    files.manifest_path = files.artifact_dir / kSpatialLayoutManifestFilename;
    files.arena_layout_runtime_path = files.artifact_dir / kSpatialLayoutArenaLayoutRuntimeFilename;
    files.dish_mask_runtime_path = files.artifact_dir / kSpatialLayoutDishMaskRuntimeFilename;
    return files;
}

bool parse_required_json_number(const nlohmann::json& node,
                                const char* field,
                                double* out,
                                std::string* error_out)
{
    if (out == nullptr) {
        if (error_out) {
            *error_out = "Null number destination.";
        }
        return false;
    }
    if (!node.contains(field) || !node.at(field).is_number()) {
        if (error_out) {
            *error_out = std::string("Missing numeric field: ") + field;
        }
        return false;
    }
    *out = node.at(field).get<double>();
    return true;
}

bool parse_optional_json_number(const nlohmann::json& node,
                                const char* field,
                                double* out)
{
    if (out == nullptr || !node.contains(field) || !node.at(field).is_number()) {
        return false;
    }
    *out = node.at(field).get<double>();
    return true;
}

bool json_string_equals_ignore_case(const nlohmann::json& node,
                                    const char* field,
                                    const std::string& expected_uppercase)
{
    if (!node.contains(field) || !node.at(field).is_string()) {
        return false;
    }
    std::string value = node.at(field).get<std::string>();
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::toupper(c));
    });
    return value == expected_uppercase;
}

template <typename T>
T clamp_index(T value, T count)
{
    if (count <= 0) {
        return 0;
    }
    return std::clamp(value, static_cast<T>(0), static_cast<T>(count - 1));
}

Point2d make_point(double x, double y)
{
    return Point2d{x, y};
}

Point2d citrus_arena_origin_canvas_px(const CitrusSpatialTemplateState& template_state)
{
    if (!template_state.has_arena_canvas_region) {
        return make_point(0.0, 0.0);
    }
    return make_point(
        template_state.arena_center_x_px - template_state.arena_width_px * 0.5,
        template_state.arena_center_y_px - template_state.arena_height_px * 0.5);
}

Point2d citrus_arena_relative_to_canvas_px(
    const CitrusSpatialTemplateState& template_state,
    const Point2d& arena_relative_point)
{
    const Point2d origin = citrus_arena_origin_canvas_px(template_state);
    return make_point(origin.x + arena_relative_point.x,
                      origin.y + arena_relative_point.y);
}

Point2d citrus_canvas_to_arena_relative_px(
    const CitrusSpatialTemplateState& template_state,
    const Point2d& canvas_point)
{
    const Point2d origin = citrus_arena_origin_canvas_px(template_state);
    return make_point(canvas_point.x - origin.x,
                      canvas_point.y - origin.y);
}

Point2d layout_geometry_center(const LayoutGeometry& geometry)
{
    if (geometry.type == LayoutGeometryType::kCircle) {
        return make_point(geometry.circle.cx, geometry.circle.cy);
    }
    return make_point(geometry.rectangle.x + geometry.rectangle.width * 0.5,
                      geometry.rectangle.y + geometry.rectangle.height * 0.5);
}

double layout_geometry_max_dimension(const LayoutGeometry& geometry)
{
    if (geometry.type == LayoutGeometryType::kCircle) {
        return geometry.circle.r * 2.0;
    }
    return std::max(geometry.rectangle.width, geometry.rectangle.height);
}

RuntimeGeometry runtime_circle(double cx, double cy, double r)
{
    RuntimeGeometry geometry;
    geometry.type = RuntimeGeometryType::kCircle;
    geometry.circle.cx = cx;
    geometry.circle.cy = cy;
    geometry.circle.r = r;
    return geometry;
}

RuntimeGeometry runtime_oriented_rectangle(
    double cx,
    double cy,
    double width,
    double height,
    double rotation_deg_clockwise)
{
    RuntimeGeometry geometry;
    geometry.type = RuntimeGeometryType::kOrientedRectangle;
    geometry.oriented_rectangle.cx = cx;
    geometry.oriented_rectangle.cy = cy;
    geometry.oriented_rectangle.width = width;
    geometry.oriented_rectangle.height = height;
    geometry.oriented_rectangle.rotation_deg_clockwise = rotation_deg_clockwise;
    return geometry;
}

LayoutGeometry default_outer_geometry()
{
    LayoutGeometry geometry;
    geometry.type = LayoutGeometryType::kCircle;
    geometry.circle.cx = 0.0;
    geometry.circle.cy = 0.0;
    geometry.circle.r = 50.0;
    return geometry;
}

ArenaLayoutZone make_default_zone(const LayoutGeometry& outer_geometry, int zone_index)
{
    ArenaLayoutZone zone;
    zone.zone_id = "z" + std::to_string(zone_index);
    zone.has_zone_index = true;
    zone.zone_index = zone_index;
    zone.display_label = "Zone " + std::to_string(zone_index);

    const int col = zone_index % 3;
    const int row = (zone_index / 3) % 3;
    const double norm_x = (static_cast<double>(col) - 1.0) * 0.35;
    const double norm_y = (static_cast<double>(row) - 1.0) * 0.35;

    zone.geometry.type = LayoutGeometryType::kCircle;
    if (outer_geometry.type == LayoutGeometryType::kCircle) {
        zone.geometry.circle.cx = outer_geometry.circle.cx + norm_x * outer_geometry.circle.r;
        zone.geometry.circle.cy = outer_geometry.circle.cy + norm_y * outer_geometry.circle.r;
        zone.geometry.circle.r = outer_geometry.circle.r * 0.18;
    } else {
        zone.geometry.circle.cx = outer_geometry.rectangle.x + outer_geometry.rectangle.width * (0.5 + norm_x * 0.8);
        zone.geometry.circle.cy = outer_geometry.rectangle.y + outer_geometry.rectangle.height * (0.5 + norm_y * 0.8);
        zone.geometry.circle.r = std::min(outer_geometry.rectangle.width, outer_geometry.rectangle.height) * 0.12;
    }
    return zone;
}

void initialize_spatial_layout_defaults(SpatialLayoutUiState* ui_state)
{
    if (ui_state == nullptr || !ui_state->layout_artifact.layout_id.empty()) {
        return;
    }

    ArenaLayoutArtifact artifact;
    artifact.artifact_id = "preview.arena_layout";
    artifact.created_utc = get_current_utc_timestamp();
    artifact.calibration_ref.artifact_id = artifact.artifact_id;
    artifact.calibration_ref.artifact_schema_id = orange::spatial::kArenaLayoutArtifactSchemaId;
    artifact.calibration_ref.artifact_schema_version = orange::spatial::kArenaLayoutArtifactSchemaVersion;
    artifact.calibration_ref.fingerprint = "preview-only";
    artifact.layout_id = "layout_preview";
    artifact.layout.coordinate_space = CoordinateSpace::kLayoutUnits;
    artifact.layout.outer_geometry = default_outer_geometry();
    artifact.layout.zones.push_back(make_default_zone(artifact.layout.outer_geometry, 0));
    artifact.provenance.source = ArenaLayoutProvenanceSource::kManualTemplate;
    artifact.provenance.ordering_rule = "row_major_top_left";
    artifact.provenance.notes = "Preview-only layout authoring state.";
    artifact.context.canvas_id = "preview_canvas";
    ui_state->layout_artifact = std::move(artifact);

    ui_state->registration.type = RegistrationType::kSimilarity;
    ui_state->registration.layout_coordinate_space = ui_state->layout_artifact.layout.coordinate_space;
    ui_state->registration.source = RegistrationSource::kManualFit;
    ui_state->registration.fit_point_count = 0;
    ui_state->registration.residual_px = 0.0;
    ui_state->registration.has_orientation_status = true;
    ui_state->registration.orientation_status = OrientationStatus::kUnknown;

    ui_state->registration_tx_px = 512.0;
    ui_state->registration_ty_px = 512.0;
    ui_state->registration_scale = 8.0;
    ui_state->registration_rotation_deg_clockwise = 0.0;
    ui_state->edge_margin_px = 12.0;
}

void reset_registration_from_frame(SpatialLayoutUiState* ui_state)
{
    if (ui_state == nullptr || ui_state->captured_texture_width <= 0 || ui_state->captured_texture_height <= 0) {
        return;
    }

    const LayoutGeometry& outer = ui_state->layout_artifact.layout.outer_geometry;
    const Point2d center = layout_geometry_center(outer);
    const double layout_dim = std::max(layout_geometry_max_dimension(outer), 1e-6);
    const double frame_dim = static_cast<double>(std::min(ui_state->captured_texture_width, ui_state->captured_texture_height));
    ui_state->registration_scale = std::max(0.1, 0.85 * frame_dim / layout_dim);
    ui_state->registration_rotation_deg_clockwise = 0.0;
    ui_state->registration_tx_px = static_cast<double>(ui_state->captured_texture_width) * 0.5 - ui_state->registration_scale * center.x;
    ui_state->registration_ty_px = static_cast<double>(ui_state->captured_texture_height) * 0.5 - ui_state->registration_scale * center.y;
    ui_state->captured_canvas_view.fit_requested = true;
}

void clear_detected_experimental_area_circle(SpatialLayoutUiState* ui_state)
{
    if (ui_state == nullptr) {
        return;
    }
    ui_state->has_detected_experimental_area_circle = false;
    ui_state->detected_experimental_area_geometry = RuntimeGeometry{};
    ui_state->detection_status.clear();
    ui_state->detection_error.clear();
}

void clear_citrus_template_import(SpatialLayoutUiState* ui_state)
{
    if (ui_state == nullptr) {
        return;
    }
    ui_state->citrus_template = {};
    ui_state->has_citrus_projected_circle = false;
    ui_state->citrus_projected_circle_geometry = RuntimeGeometry{};
    ui_state->citrus_import_status.clear();
    ui_state->citrus_import_error.clear();
}

bool transform_point_projective(const std::array<double, 9>& matrix,
                                const Point2d& input,
                                Point2d* output)
{
    if (output == nullptr) {
        return false;
    }
    const double w = matrix[6] * input.x + matrix[7] * input.y + matrix[8];
    if (!std::isfinite(w) || std::abs(w) < 1e-12) {
        return false;
    }
    output->x = (matrix[0] * input.x + matrix[1] * input.y + matrix[2]) / w;
    output->y = (matrix[3] * input.x + matrix[4] * input.y + matrix[5]) / w;
    return std::isfinite(output->x) && std::isfinite(output->y);
}

std::vector<Point2d> sample_circle_boundary_points(double cx, double cy, double radius, int point_count)
{
    std::vector<Point2d> points;
    point_count = std::max(3, point_count);
    points.reserve(static_cast<size_t>(point_count));
    for (int idx = 0; idx < point_count; ++idx) {
        const double theta = (2.0 * kPi * static_cast<double>(idx)) / static_cast<double>(point_count);
        points.push_back(make_point(cx + radius * std::cos(theta),
                                    cy + radius * std::sin(theta)));
    }
    return points;
}

bool fit_circle_to_points(const std::vector<Point2d>& points,
                          CircleGeometry* circle_out,
                          double* rms_error_out,
                          std::string* error_out)
{
    if (circle_out == nullptr) {
        if (error_out) {
            *error_out = "Null circle destination.";
        }
        return false;
    }
    if (points.size() < 3) {
        if (error_out) {
            *error_out = "Need at least three points to fit a circle.";
        }
        return false;
    }

    cv::Mat design(static_cast<int>(points.size()), 3, CV_64F);
    cv::Mat rhs(static_cast<int>(points.size()), 1, CV_64F);
    for (int row = 0; row < static_cast<int>(points.size()); ++row) {
        const double x = points[static_cast<size_t>(row)].x;
        const double y = points[static_cast<size_t>(row)].y;
        design.at<double>(row, 0) = x;
        design.at<double>(row, 1) = y;
        design.at<double>(row, 2) = 1.0;
        rhs.at<double>(row, 0) = -(x * x + y * y);
    }

    cv::Mat solution;
    if (!cv::solve(design, rhs, solution, cv::DECOMP_SVD) || solution.rows != 3) {
        if (error_out) {
            *error_out = "Failed to solve circle fit.";
        }
        return false;
    }

    const double d = solution.at<double>(0, 0);
    const double e = solution.at<double>(1, 0);
    const double f = solution.at<double>(2, 0);
    const double cx = -0.5 * d;
    const double cy = -0.5 * e;
    const double radius_sq = cx * cx + cy * cy - f;
    if (!std::isfinite(radius_sq) || radius_sq <= 0.0) {
        if (error_out) {
            *error_out = "Circle fit produced an invalid radius.";
        }
        return false;
    }

    circle_out->cx = cx;
    circle_out->cy = cy;
    circle_out->r = std::sqrt(radius_sq);

    if (rms_error_out != nullptr) {
        double sum_sq = 0.0;
        for (const Point2d& point : points) {
            const double dx = point.x - circle_out->cx;
            const double dy = point.y - circle_out->cy;
            const double radial_error = std::sqrt(dx * dx + dy * dy) - circle_out->r;
            sum_sq += radial_error * radial_error;
        }
        *rms_error_out = std::sqrt(sum_sq / static_cast<double>(points.size()));
    }

    return true;
}

std::string join_strings(const std::vector<std::string>& values, const std::string& separator)
{
    std::ostringstream oss;
    for (size_t idx = 0; idx < values.size(); ++idx) {
        if (idx > 0) {
            oss << separator;
        }
        oss << values[idx];
    }
    return oss.str();
}

bool sample_citrus_experimental_area_outline_in_camera_px(
    const CitrusSpatialTemplateState& template_state,
    const Point2d& center_arena_relative_px,
    std::vector<Point2d>* camera_points_out,
    std::string* error_out)
{
    if (camera_points_out == nullptr) {
        if (error_out) {
            *error_out = "Null Citrus outline destination.";
        }
        return false;
    }
    camera_points_out->clear();
    if (!template_state.available ||
        !template_state.has_arena_canvas_region ||
        !template_state.has_canvas_to_camera_homography ||
        template_state.experimental_area_radius_px <= 0.0) {
        if (error_out) {
            *error_out = "Citrus outline projection requires arena canvas region, canvas-to-camera homography, and positive radius.";
        }
        return false;
    }

    const std::vector<Point2d> arena_relative_points = sample_circle_boundary_points(
        center_arena_relative_px.x,
        center_arena_relative_px.y,
        template_state.experimental_area_radius_px,
        kProjectedCircleSampleCount);
    camera_points_out->reserve(arena_relative_points.size());
    for (const Point2d& arena_relative_point : arena_relative_points) {
        const Point2d canvas_point =
            citrus_arena_relative_to_canvas_px(template_state, arena_relative_point);
        Point2d camera_point{};
        if (!transform_point_projective(
                template_state.canvas_to_camera_homography,
                canvas_point,
                &camera_point)) {
            camera_points_out->clear();
            if (error_out) {
                *error_out = "Failed to project Citrus outline point into camera space.";
            }
            return false;
        }
        camera_points_out->push_back(camera_point);
    }
    return !camera_points_out->empty();
}

bool update_citrus_projected_circle_preview(SpatialLayoutUiState* ui_state,
                                            double* rms_error_out,
                                            std::string* error_out)
{
    if (ui_state == nullptr) {
        if (error_out) {
            *error_out = "Spatial layout UI state is null.";
        }
        return false;
    }
    if (!ui_state->citrus_template.available ||
        !ui_state->citrus_template.has_canvas_to_camera_homography) {
        if (error_out) {
            *error_out = "Imported Citrus template does not have a canvas-to-camera homography.";
        }
        return false;
    }

    std::vector<Point2d> camera_points;
    if (!sample_citrus_experimental_area_outline_in_camera_px(
            ui_state->citrus_template,
            make_point(ui_state->citrus_template.experimental_area_center_x_px,
                       ui_state->citrus_template.experimental_area_center_y_px),
            &camera_points,
            error_out)) {
        return false;
    }

    CircleGeometry fitted_circle;
    double rms_error = 0.0;
    if (!fit_circle_to_points(camera_points, &fitted_circle, &rms_error, error_out)) {
        return false;
    }

    ui_state->has_citrus_projected_circle = true;
    ui_state->citrus_projected_circle_geometry =
        runtime_circle(fitted_circle.cx, fitted_circle.cy, fitted_circle.r);
    if (rms_error_out != nullptr) {
        *rms_error_out = rms_error;
    }
    return true;
}

bool seed_registration_from_citrus_homography(SpatialLayoutUiState* ui_state,
                                              std::string* error_out)
{
    if (ui_state == nullptr) {
        if (error_out) {
            *error_out = "Spatial layout UI state is null.";
        }
        return false;
    }
    if (ui_state->layout_artifact.layout.outer_geometry.type != LayoutGeometryType::kCircle) {
        if (error_out) {
            *error_out = "Citrus homography seeding currently supports only circular outer geometry.";
        }
        return false;
    }

    double rms_error = 0.0;
    if (!update_citrus_projected_circle_preview(ui_state, &rms_error, error_out)) {
        return false;
    }

    const double canonical_radius = ui_state->layout_artifact.layout.outer_geometry.circle.r;
    if (canonical_radius <= 0.0) {
        if (error_out) {
            *error_out = "Canonical outer radius must be positive.";
        }
        return false;
    }

    const Point2d center(
        make_point(ui_state->citrus_projected_circle_geometry.circle.cx,
                   ui_state->citrus_projected_circle_geometry.circle.cy));
    const double scale =
        ui_state->citrus_projected_circle_geometry.circle.r / canonical_radius;
    set_registration_transform(
        ui_state,
        RegistrationType::kSimilarity,
        center,
        scale,
        0.0,
        RegistrationSource::kImported);
    ui_state->registration.fit_point_count = kProjectedCircleSampleCount;
    ui_state->registration.residual_px = std::max(0.0, rms_error);
    return true;
}

bool load_homography_matrix_from_citrus_sidecar(const std::filesystem::path& config_path,
                                                const std::string& config_name,
                                                const std::string& camera_id,
                                                CitrusSpatialTemplateState* template_state,
                                                std::string* error_out)
{
    if (template_state == nullptr) {
        if (error_out) {
            *error_out = "Null Citrus template state.";
        }
        return false;
    }
    if (config_name.empty() || camera_id.empty()) {
        return false;
    }

    std::string safe_camera_id = camera_id;
    std::replace(safe_camera_id.begin(), safe_camera_id.end(), ' ', '_');
    const std::filesystem::path homography_path =
        config_path.parent_path() / "calibration_artifacts" /
        ("homography_" + config_name + "_" + safe_camera_id + ".yml");
    if (!std::filesystem::exists(homography_path)) {
        return false;
    }

    try {
        cv::FileStorage fs(homography_path.string(), cv::FileStorage::READ);
        if (!fs.isOpened()) {
            if (error_out) {
                *error_out = "Found Citrus homography sidecar but could not open it: " + homography_path.string();
            }
            return false;
        }
        cv::Mat homography;
        fs["homography_matrix"] >> homography;
        fs.release();
        if (homography.empty() || homography.rows != 3 || homography.cols != 3) {
            if (error_out) {
                *error_out = "Citrus homography sidecar did not contain a valid 3x3 matrix: " + homography_path.string();
            }
            return false;
        }

        homography.convertTo(homography, CV_64F);
        for (int row = 0; row < 3; ++row) {
            for (int col = 0; col < 3; ++col) {
                template_state->camera_to_canvas_homography[static_cast<size_t>(row * 3 + col)] =
                    homography.at<double>(row, col);
            }
        }
        template_state->has_camera_to_canvas_homography = true;

        const double det = cv::determinant(homography);
        if (std::isfinite(det) && std::abs(det) > 1e-12) {
            const cv::Mat inverse = homography.inv();
            for (int row = 0; row < 3; ++row) {
                for (int col = 0; col < 3; ++col) {
                    template_state->canvas_to_camera_homography[static_cast<size_t>(row * 3 + col)] =
                        inverse.at<double>(row, col);
                }
            }
            template_state->has_canvas_to_camera_homography = true;
        }
    } catch (const cv::Exception& ex) {
        if (error_out) {
            *error_out = std::string("Failed to load Citrus homography sidecar: ") + ex.what();
        }
        return false;
    }

    return true;
}

bool import_citrus_single_circle_template(SpatialLayoutUiState* ui_state,
                                          const CameraParams& selected_camera,
                                          const std::filesystem::path& config_path,
                                          std::string* status_out,
                                          std::string* error_out)
{
    if (ui_state == nullptr) {
        if (error_out) {
            *error_out = "Spatial layout UI state is null.";
        }
        return false;
    }

    nlohmann::json root;
    if (!read_json_file(config_path, &root, error_out)) {
        return false;
    }
    if (!root.is_object()) {
        if (error_out) {
            *error_out = "Citrus arena config root must be a JSON object.";
        }
        return false;
    }

    const nlohmann::json* matched_arena = nullptr;
    const nlohmann::json* matched_camera_calibration = nullptr;
    std::string matched_arena_name;
    std::vector<std::string> available_camera_ids;

    auto try_match_arena = [&](const nlohmann::json& arena_json, const std::string& arena_name) -> bool {
        if (!arena_json.is_object()) {
            return false;
        }
        if (!arena_json.contains("camera_calibrations") || !arena_json.at("camera_calibrations").is_array()) {
            return false;
        }
        for (const auto& camera_json : arena_json.at("camera_calibrations")) {
            if (!camera_json.is_object()) {
                continue;
            }
            const std::string camera_id = camera_json.value("camera_id", "");
            if (!camera_id.empty()) {
                available_camera_ids.push_back(camera_id);
            }
            if (camera_id == selected_camera.camera_serial) {
                matched_arena = &arena_json;
                matched_camera_calibration = &camera_json;
                matched_arena_name = arena_name;
                return true;
            }
        }
        return false;
    };

    if (root.contains("arenas") && root.at("arenas").is_object()) {
        for (auto it = root.at("arenas").begin(); it != root.at("arenas").end(); ++it) {
            if (try_match_arena(it.value(), it.key())) {
                break;
            }
        }
    } else {
        const std::string fallback_arena_name = root.value("config_name", config_path.stem().string());
        if (!try_match_arena(root, fallback_arena_name)) {
            if (root.contains("camera_calibrations") &&
                root.at("camera_calibrations").is_array() &&
                root.at("camera_calibrations").size() == 1 &&
                root.contains("experimental_area_shape")) {
                matched_arena = &root;
                matched_camera_calibration = &root.at("camera_calibrations")[0];
                matched_arena_name = fallback_arena_name;
                const std::string fallback_camera_id =
                    matched_camera_calibration->value("camera_id", "");
                if (!fallback_camera_id.empty()) {
                    available_camera_ids.push_back(fallback_camera_id);
                }
            }
        }
    }

    if (matched_arena == nullptr || matched_camera_calibration == nullptr) {
        if (error_out) {
            std::ostringstream oss;
            oss << "No Citrus arena entry matched selected camera "
                << selected_camera.camera_serial;
            if (!available_camera_ids.empty()) {
                std::sort(available_camera_ids.begin(), available_camera_ids.end());
                available_camera_ids.erase(
                    std::unique(available_camera_ids.begin(), available_camera_ids.end()),
                    available_camera_ids.end());
                oss << ". Available camera_ids: " << join_strings(available_camera_ids, ", ");
            }
            *error_out = oss.str();
        }
        return false;
    }

    if (!json_string_equals_ignore_case(*matched_arena, "experimental_area_shape", "CIRCLE")) {
        if (error_out) {
            *error_out = "Citrus import v1 currently supports only experimental_area_shape = CIRCLE.";
        }
        return false;
    }

    CitrusSpatialTemplateState template_state;
    template_state.available = true;
    template_state.source_config_path = config_path.string();
    template_state.source_canvas_name = config_path.parent_path().filename().string();
    template_state.source_rig_name = config_path.parent_path().parent_path().filename().string();
    template_state.source_arena_name = matched_arena_name;
    template_state.source_config_name = matched_arena->value("config_name", matched_arena_name);
    template_state.source_camera_id = matched_camera_calibration->value("camera_id", "");
    template_state.source_dish_type_name = matched_arena->value("selected_dish_type_name", "");
    const bool has_arena_center =
        parse_optional_json_number(*matched_camera_calibration, "arena_center_x_px", &template_state.arena_center_x_px) &&
        parse_optional_json_number(*matched_camera_calibration, "arena_center_y_px", &template_state.arena_center_y_px);
    const bool has_arena_size =
        parse_optional_json_number(*matched_camera_calibration, "arena_width_px", &template_state.arena_width_px) &&
        parse_optional_json_number(*matched_camera_calibration, "arena_height_px", &template_state.arena_height_px);
    template_state.has_arena_canvas_region =
        has_arena_center &&
        has_arena_size &&
        template_state.arena_width_px > 0.0 &&
        template_state.arena_height_px > 0.0;

    if (!parse_required_json_number(*matched_arena, "experimental_area_center_x_px",
                                    &template_state.experimental_area_center_x_px, error_out) ||
        !parse_required_json_number(*matched_arena, "experimental_area_center_y_px",
                                    &template_state.experimental_area_center_y_px, error_out) ||
        !parse_required_json_number(*matched_arena, "experimental_area_radius_px",
                                    &template_state.experimental_area_radius_px, error_out)) {
        return false;
    }
    if (matched_arena->contains("experimental_area_radius_mm") &&
        matched_arena->at("experimental_area_radius_mm").is_number()) {
        template_state.has_radius_mm = true;
        template_state.experimental_area_radius_mm =
            matched_arena->at("experimental_area_radius_mm").get<double>();
    }
    if (matched_camera_calibration->contains("pixels_per_mm_projector") &&
        matched_camera_calibration->at("pixels_per_mm_projector").is_number()) {
        template_state.has_pixels_per_mm_projector = true;
        template_state.pixels_per_mm_projector =
            matched_camera_calibration->at("pixels_per_mm_projector").get<double>();
    }

    std::string homography_error;
    const bool loaded_homography = load_homography_matrix_from_citrus_sidecar(
        config_path,
        template_state.source_config_name,
        template_state.source_camera_id,
        &template_state,
        &homography_error);

    ui_state->citrus_template = template_state;
    ui_state->has_citrus_projected_circle = false;
    ui_state->citrus_projected_circle_geometry = RuntimeGeometry{};

    LayoutGeometry imported_outer;
    imported_outer.type = LayoutGeometryType::kCircle;
    imported_outer.circle.cx = template_state.experimental_area_center_x_px;
    imported_outer.circle.cy = template_state.experimental_area_center_y_px;
    imported_outer.circle.r = template_state.experimental_area_radius_px;

    ui_state->layout_artifact.artifact_id = "preview.citrus_import";
    ui_state->layout_artifact.created_utc = get_current_utc_timestamp();
    ui_state->layout_artifact.calibration_ref.artifact_id = ui_state->layout_artifact.artifact_id;
    ui_state->layout_artifact.calibration_ref.artifact_schema_id = orange::spatial::kArenaLayoutArtifactSchemaId;
    ui_state->layout_artifact.calibration_ref.artifact_schema_version = orange::spatial::kArenaLayoutArtifactSchemaVersion;
    ui_state->layout_artifact.calibration_ref.fingerprint = "preview-only";
    ui_state->layout_artifact.layout_id =
        "citrus_" + sanitize_artifact_component(template_state.source_canvas_name) + "_" +
        sanitize_artifact_component(template_state.source_config_name);
    ui_state->layout_artifact.layout.coordinate_space = CoordinateSpace::kLayoutUnits;
    ui_state->layout_artifact.layout.outer_geometry = imported_outer;
    ui_state->layout_artifact.layout.zones.clear();
    ArenaLayoutZone zone;
    zone.zone_id = "z0";
    zone.has_zone_index = true;
    zone.zone_index = 0;
    zone.display_label = "Experimental Area";
    zone.geometry = imported_outer;
    ui_state->layout_artifact.layout.zones.push_back(std::move(zone));
    ui_state->layout_artifact.context.canvas_id = template_state.source_canvas_name;
    ui_state->layout_artifact.context.dish_design_id = template_state.source_dish_type_name;
    ui_state->layout_artifact.provenance.source = ArenaLayoutProvenanceSource::kImportedTemplate;
    ui_state->layout_artifact.provenance.ordering_rule = "single_circle_imported_from_citrus";
    ui_state->layout_artifact.provenance.notes =
        "Imported from Citrus config " + config_path.string() +
        " for camera " + selected_camera.camera_serial + ".";
    ui_state->selected_zone_index = 0;

    ui_state->registration.layout_coordinate_space = ui_state->layout_artifact.layout.coordinate_space;
    if (ui_state->has_capture) {
        reset_registration_from_frame(ui_state);
    }

    std::ostringstream status;
    status << "Imported Citrus circle from " << template_state.source_canvas_name
           << " / " << template_state.source_config_name
           << " for camera " << template_state.source_camera_id;
    if (loaded_homography && template_state.has_canvas_to_camera_homography) {
        double preview_rms = 0.0;
        std::string preview_error;
        if (update_citrus_projected_circle_preview(ui_state, &preview_rms, &preview_error)) {
            status << ". Homography loaded; projected-circle seed RMS " << std::fixed << std::setprecision(2)
                   << preview_rms << " px.";
        } else {
            status << ". Homography loaded but preview seed failed (" << preview_error << ").";
        }
    } else if (!homography_error.empty()) {
        status << ". Homography sidecar issue: " << homography_error;
    } else {
        status << ". No homography sidecar found.";
    }

    if (status_out) {
        *status_out = status.str();
    }
    return true;
}

bool detect_experimental_area_circle_from_capture(
    SpatialLayoutUiState* ui_state,
    std::string* error_out)
{
    if (ui_state == nullptr) {
        if (error_out) {
            *error_out = "Experimental-area detection requires non-null UI state.";
        }
        return false;
    }
    if (!ui_state->has_capture || ui_state->captured_texture_width <= 0 || ui_state->captured_texture_height <= 0 ||
        ui_state->captured_rgba.empty()) {
        if (error_out) {
            *error_out = "Capture a frame before running experimental-area detection.";
        }
        return false;
    }

    cv::Mat rgba(ui_state->captured_texture_height,
                 ui_state->captured_texture_width,
                 CV_8UC4,
                 ui_state->captured_rgba.data());
    cv::Mat gray;
    cv::cvtColor(rgba, gray, cv::COLOR_RGBA2GRAY);

    ui_state->hough_dp = std::clamp(ui_state->hough_dp, 1.0, 3.0);
    ui_state->hough_min_dist_fraction =
        std::clamp(ui_state->hough_min_dist_fraction, 0.01, 2.0);
    ui_state->hough_param1 = std::clamp(ui_state->hough_param1, 1.0, 500.0);
    ui_state->hough_param2 = std::clamp(ui_state->hough_param2, 1.0, 500.0);
    ui_state->hough_min_radius_fraction =
        std::clamp(ui_state->hough_min_radius_fraction, 0.001, 1.0);
    ui_state->hough_max_radius_fraction =
        std::clamp(ui_state->hough_max_radius_fraction, 0.001, 1.5);
    if (ui_state->hough_max_radius_fraction < ui_state->hough_min_radius_fraction) {
        std::swap(ui_state->hough_max_radius_fraction, ui_state->hough_min_radius_fraction);
    }
    ui_state->hough_max_detection_dimension_px =
        std::clamp(ui_state->hough_max_detection_dimension_px, 256, 8192);
    ui_state->hough_median_blur_ksize =
        std::clamp(ui_state->hough_median_blur_ksize, 1, 31);
    if ((ui_state->hough_median_blur_ksize % 2) == 0) {
        ++ui_state->hough_median_blur_ksize;
    }
    double detection_scale = 1.0;
    cv::Mat detection_gray = gray;
    const int max_dim = std::max(gray.cols, gray.rows);
    if (max_dim > ui_state->hough_max_detection_dimension_px) {
        detection_scale =
            static_cast<double>(ui_state->hough_max_detection_dimension_px) /
            static_cast<double>(max_dim);
        cv::resize(gray, detection_gray, cv::Size(), detection_scale, detection_scale, cv::INTER_AREA);
    }

    cv::Mat blurred;
    if (ui_state->hough_median_blur_ksize > 1) {
        cv::medianBlur(detection_gray, blurred, ui_state->hough_median_blur_ksize);
    } else {
        blurred = detection_gray;
    }

    const double min_dim = static_cast<double>(std::min(blurred.cols, blurred.rows));
    if (min_dim < 32.0) {
        if (error_out) {
            *error_out = "Experimental-area detection requires a larger captured frame.";
        }
        return false;
    }

    const int min_radius = std::max(
        1,
        static_cast<int>(std::round(min_dim * ui_state->hough_min_radius_fraction)));
    const int max_radius = std::max(
        min_radius + 1,
        static_cast<int>(std::round(min_dim * ui_state->hough_max_radius_fraction)));
    const double min_dist =
        std::max(1.0, min_dim * ui_state->hough_min_dist_fraction);

    std::vector<cv::Vec3f> circles;
    try {
        cv::HoughCircles(
            blurred,
            circles,
            cv::HOUGH_GRADIENT,
            ui_state->hough_dp,
            min_dist,
            ui_state->hough_param1,
            ui_state->hough_param2,
            min_radius,
            max_radius);

        if (circles.empty() && ui_state->hough_fallback_enabled) {
            cv::HoughCircles(
                blurred,
                circles,
                cv::HOUGH_GRADIENT,
                std::max(1.0, ui_state->hough_dp * 0.96),
                min_dist,
                std::max(1.0, ui_state->hough_param1 * 0.75),
                std::max(1.0, ui_state->hough_param2 * 0.73),
                min_radius,
                max_radius);
        }
    } catch (const cv::Exception& ex) {
        if (error_out) {
            *error_out = std::string("Hough circle detection failed: ") + ex.what();
        }
        return false;
    }

    if (circles.empty()) {
        if (error_out) {
            *error_out = "No experimental-area circle was detected in the captured frame.";
        }
        return false;
    }

    const Point2d image_center = make_point(blurred.cols * 0.5, blurred.rows * 0.5);
    double best_score = std::numeric_limits<double>::lowest();
    cv::Vec3f best_circle = circles.front();
    for (const cv::Vec3f& circle : circles) {
        const double dx = static_cast<double>(circle[0]) - image_center.x;
        const double dy = static_cast<double>(circle[1]) - image_center.y;
        const double dist = std::sqrt(dx * dx + dy * dy);
        const double score = static_cast<double>(circle[2]) - 0.35 * dist;
        if (score > best_score) {
            best_score = score;
            best_circle = circle;
        }
    }

    const double inv_scale = 1.0 / detection_scale;
    ui_state->detected_experimental_area_geometry =
        runtime_circle(
            static_cast<double>(best_circle[0]) * inv_scale,
            static_cast<double>(best_circle[1]) * inv_scale,
            std::max(
                1.0,
                static_cast<double>(best_circle[2]) * inv_scale +
                    ui_state->hough_radius_adjustment_px));
    ui_state->has_detected_experimental_area_circle = true;
    ui_state->detection_error.clear();

    std::ostringstream status;
    status << "Detected experimental-area circle at ("
           << std::lround(ui_state->detected_experimental_area_geometry.circle.cx) << ", "
           << std::lround(ui_state->detected_experimental_area_geometry.circle.cy) << ")"
           << " r=" << std::lround(ui_state->detected_experimental_area_geometry.circle.r);
    if (circles.size() > 1) {
        status << " from " << circles.size() << " Hough candidates";
    }
    status << " using dp=" << ui_state->hough_dp
           << " param1=" << ui_state->hough_param1
           << " param2=" << ui_state->hough_param2
           << " radius=[" << std::lround(static_cast<double>(min_radius) * inv_scale)
           << "," << std::lround(static_cast<double>(max_radius) * inv_scale) << "]";
    ui_state->detection_status = status.str();
    return true;
}

bool seed_registration_from_detected_experimental_area_circle(
    SpatialLayoutUiState* ui_state,
    std::string* error_out)
{
    if (ui_state == nullptr) {
        if (error_out) {
            *error_out = "Registration seeding requires non-null UI state.";
        }
        return false;
    }
    if (!ui_state->has_detected_experimental_area_circle) {
        if (error_out) {
            *error_out = "Run experimental-area detection before seeding registration.";
        }
        return false;
    }

    const LayoutGeometry& canonical_outer = ui_state->layout_artifact.layout.outer_geometry;
    if (canonical_outer.type != LayoutGeometryType::kCircle || canonical_outer.circle.r <= 0.0) {
        if (error_out) {
            *error_out =
                "Experimental-area circle seeding currently requires a circular canonical experimental area.";
        }
        return false;
    }

    ui_state->registration.type = RegistrationType::kSimilarity;
    ui_state->registration.source = RegistrationSource::kDetectedFit;
    ui_state->registration.fit_point_count = 3;
    ui_state->registration.residual_px = 0.0;
    ui_state->registration.has_orientation_status = true;
    ui_state->registration.orientation_status = OrientationStatus::kAmbiguous;

    const double detected_radius = ui_state->detected_experimental_area_geometry.circle.r;
    const double scale = std::max(1e-6, detected_radius / canonical_outer.circle.r);
    const Point2d desired_center = make_point(
        ui_state->detected_experimental_area_geometry.circle.cx,
        ui_state->detected_experimental_area_geometry.circle.cy);
    const double theta = ui_state->registration_rotation_deg_clockwise * kPi / 180.0;
    const double rotated_center_x =
        scale * (std::cos(theta) * canonical_outer.circle.cx - std::sin(theta) * canonical_outer.circle.cy);
    const double rotated_center_y =
        scale * (std::sin(theta) * canonical_outer.circle.cx + std::cos(theta) * canonical_outer.circle.cy);

    ui_state->registration.type = RegistrationType::kSimilarity;
    ui_state->registration_scale = scale;
    ui_state->registration_tx_px = desired_center.x - rotated_center_x;
    ui_state->registration_ty_px = desired_center.y - rotated_center_y;
    return true;
}

double normalize_angle_deg(double angle_deg)
{
    while (angle_deg <= -180.0) {
        angle_deg += 360.0;
    }
    while (angle_deg > 180.0) {
        angle_deg -= 360.0;
    }
    return angle_deg;
}

double effective_registration_scale(const SpatialLayoutUiState& ui_state)
{
    if (ui_state.registration.type == RegistrationType::kIdentity ||
        ui_state.registration.type == RegistrationType::kTranslation) {
        return 1.0;
    }
    return std::max(1e-6, ui_state.registration_scale);
}

double effective_registration_rotation_deg(const SpatialLayoutUiState& ui_state)
{
    return ui_state.registration.type == RegistrationType::kSimilarity
        ? ui_state.registration_rotation_deg_clockwise
        : 0.0;
}

Point2d runtime_geometry_center(const RuntimeGeometry& geometry)
{
    if (geometry.type == RuntimeGeometryType::kCircle) {
        return make_point(geometry.circle.cx, geometry.circle.cy);
    }
    return make_point(geometry.oriented_rectangle.cx, geometry.oriented_rectangle.cy);
}

Point2d point_from_local_offset(const Point2d& center, double local_x, double local_y, double rotation_deg_clockwise)
{
    const double theta = rotation_deg_clockwise * kPi / 180.0;
    const double cos_theta = std::cos(theta);
    const double sin_theta = std::sin(theta);
    return make_point(
        center.x + cos_theta * local_x - sin_theta * local_y,
        center.y + sin_theta * local_x + cos_theta * local_y);
}

Point2d runtime_geometry_right_handle(const RuntimeGeometry& geometry)
{
    const Point2d center = runtime_geometry_center(geometry);
    if (geometry.type == RuntimeGeometryType::kCircle) {
        return make_point(geometry.circle.cx + geometry.circle.r, geometry.circle.cy);
    }
    return point_from_local_offset(
        center,
        geometry.oriented_rectangle.width * 0.5,
        0.0,
        geometry.oriented_rectangle.rotation_deg_clockwise);
}

Point2d runtime_geometry_bottom_handle(const RuntimeGeometry& geometry)
{
    const Point2d center = runtime_geometry_center(geometry);
    if (geometry.type == RuntimeGeometryType::kCircle) {
        return make_point(geometry.circle.cx, geometry.circle.cy + geometry.circle.r);
    }
    return point_from_local_offset(
        center,
        0.0,
        geometry.oriented_rectangle.height * 0.5,
        geometry.oriented_rectangle.rotation_deg_clockwise);
}

Point2d runtime_geometry_rotation_handle(const RuntimeGeometry& geometry)
{
    const Point2d center = runtime_geometry_center(geometry);
    Point2d top_boundary = center;
    if (geometry.type == RuntimeGeometryType::kCircle) {
        top_boundary = make_point(geometry.circle.cx, geometry.circle.cy - geometry.circle.r);
    } else {
        top_boundary = point_from_local_offset(
            center,
            0.0,
            -geometry.oriented_rectangle.height * 0.5,
            geometry.oriented_rectangle.rotation_deg_clockwise);
    }

    const ImVec2 center_px = ImPlot::PlotToPixels(ImPlotPoint(center.x, center.y));
    const ImVec2 top_px = ImPlot::PlotToPixels(ImPlotPoint(top_boundary.x, top_boundary.y));
    ImVec2 direction(top_px.x - center_px.x, top_px.y - center_px.y);
    const float length = std::sqrt(direction.x * direction.x + direction.y * direction.y);
    if (length < 1e-3f) {
        direction = ImVec2(0.0f, -1.0f);
    } else {
        direction.x /= length;
        direction.y /= length;
    }
    const ImVec2 handle_px(
        center_px.x + direction.x * (length + 28.0f),
        center_px.y + direction.y * (length + 28.0f));
    const ImPlotPoint handle_plot = ImPlot::PixelsToPlot(handle_px);
    return make_point(handle_plot.x, handle_plot.y);
}

Point2d camera_point_to_layout_point(const SpatialLayoutUiState& ui_state, const Point2d& camera_point)
{
    return transform_point(
        ui_state.registration.camera_to_layout_matrix,
        camera_point.x,
        camera_point.y);
}

void set_registration_transform(SpatialLayoutUiState* ui_state,
                                RegistrationType type,
                                const Point2d& desired_outer_center,
                                double scale,
                                double rotation_deg_clockwise,
                                RegistrationSource source)
{
    if (ui_state == nullptr) {
        return;
    }

    if (type == RegistrationType::kIdentity) {
        scale = 1.0;
        rotation_deg_clockwise = 0.0;
    } else if (type == RegistrationType::kTranslation) {
        scale = 1.0;
        rotation_deg_clockwise = 0.0;
    } else {
        scale = std::max(1e-6, scale);
    }

    const Point2d canonical_center = layout_geometry_center(ui_state->layout_artifact.layout.outer_geometry);
    const double theta = rotation_deg_clockwise * kPi / 180.0;
    const double rotated_center_x =
        scale * (std::cos(theta) * canonical_center.x - std::sin(theta) * canonical_center.y);
    const double rotated_center_y =
        scale * (std::sin(theta) * canonical_center.x + std::cos(theta) * canonical_center.y);

    ui_state->registration.type = type;
    ui_state->registration.source = source;
    ui_state->registration_tx_px = desired_outer_center.x - rotated_center_x;
    ui_state->registration_ty_px = desired_outer_center.y - rotated_center_y;
    ui_state->registration_scale = scale;
    ui_state->registration_rotation_deg_clockwise = normalize_angle_deg(rotation_deg_clockwise);
}

bool update_drag_point(int id,
                       const Point2d& initial_point,
                       const ImVec4& color,
                       float radius_px,
                       Point2d* updated_point)
{
    if (updated_point == nullptr) {
        return false;
    }

    double x = initial_point.x;
    double y = initial_point.y;
    const bool changed = ImPlot::DragPoint(
        id,
        &x,
        &y,
        color,
        radius_px,
        ImPlotDragToolFlags_NoFit);
    *updated_point = make_point(x, y);
    return changed;
}

bool handle_registration_canvas_edit(SpatialLayoutUiState* ui_state)
{
    if (ui_state == nullptr || !ui_state->dish_mask_runtime.has_geometry) {
        return false;
    }

    const RuntimeGeometry& outer = ui_state->dish_mask_runtime.geometry.outer_geometry;
    const LayoutGeometry& canonical_outer = ui_state->layout_artifact.layout.outer_geometry;
    const Point2d current_center = runtime_geometry_center(outer);
    const double current_scale = effective_registration_scale(*ui_state);
    const double current_rotation_deg = effective_registration_rotation_deg(*ui_state);

    bool changed = false;
    Point2d center_handle{};
    if (update_drag_point(4100, current_center, ImVec4(0.25f, 0.80f, 1.0f, 1.0f), 6.0f, &center_handle)) {
        const RegistrationType new_type =
            ui_state->registration.type == RegistrationType::kIdentity
                ? RegistrationType::kTranslation
                : ui_state->registration.type;
        set_registration_transform(
            ui_state,
            new_type,
            center_handle,
            current_scale,
            current_rotation_deg,
            RegistrationSource::kManualFit);
        ui_state->registration.fit_point_count = 0;
        ui_state->registration.residual_px = 0.0;
        changed = true;
    }

    if (ui_state->registration.type != RegistrationType::kSimilarity) {
        return changed;
    }

    Point2d scale_handle{};
    if (update_drag_point(
            4101,
            runtime_geometry_right_handle(outer),
            ImVec4(1.0f, 0.82f, 0.18f, 1.0f),
            5.0f,
            &scale_handle)) {
        double new_scale = current_scale;
        if (canonical_outer.type == LayoutGeometryType::kCircle && canonical_outer.circle.r > 0.0) {
            const double dx = scale_handle.x - current_center.x;
            const double dy = scale_handle.y - current_center.y;
            new_scale = std::max(1e-6, std::sqrt(dx * dx + dy * dy) / canonical_outer.circle.r);
        } else if (canonical_outer.type == LayoutGeometryType::kRectangle && canonical_outer.rectangle.width > 0.0) {
            const Point2d layout_center = layout_geometry_center(canonical_outer);
            const Point2d mouse_layout = camera_point_to_layout_point(*ui_state, scale_handle);
            new_scale = std::max(
                1e-6,
                (2.0 * std::abs(mouse_layout.x - layout_center.x)) / canonical_outer.rectangle.width);
        }
        set_registration_transform(
            ui_state,
            RegistrationType::kSimilarity,
            current_center,
            new_scale,
            current_rotation_deg,
            RegistrationSource::kManualFit);
        ui_state->registration.fit_point_count = 0;
        ui_state->registration.residual_px = 0.0;
        changed = true;
    }

    Point2d rotation_handle{};
    if (update_drag_point(
            4102,
            runtime_geometry_rotation_handle(outer),
            ImVec4(1.0f, 0.35f, 0.80f, 1.0f),
            5.0f,
            &rotation_handle)) {
        const double dx = rotation_handle.x - current_center.x;
        const double dy = rotation_handle.y - current_center.y;
        if ((dx * dx + dy * dy) > 1e-8) {
            const double angle_deg = std::atan2(dy, dx) * 180.0 / kPi;
            const double new_rotation_deg = normalize_angle_deg(angle_deg + 90.0);
            set_registration_transform(
                ui_state,
                RegistrationType::kSimilarity,
                current_center,
                current_scale,
                new_rotation_deg,
                RegistrationSource::kManualFit);
            ui_state->registration.fit_point_count = 0;
            ui_state->registration.residual_px = 0.0;
            ui_state->registration.has_orientation_status = true;
            ui_state->registration.orientation_status = OrientationStatus::kManualConfirmed;
            changed = true;
        }
    }

    return changed;
}

bool handle_selected_zone_canvas_edit(SpatialLayoutUiState* ui_state)
{
    if (ui_state == nullptr ||
        !ui_state->registration.has_camera_to_layout_matrix ||
        ui_state->selected_zone_index < 0 ||
        ui_state->selected_zone_index >= static_cast<int>(ui_state->layout_artifact.layout.zones.size()) ||
        ui_state->selected_zone_index >= static_cast<int>(ui_state->arena_layout_runtime.zones.size())) {
        return false;
    }

    ArenaLayoutZone& zone =
        ui_state->layout_artifact.layout.zones[static_cast<size_t>(ui_state->selected_zone_index)];
    const ResolvedZoneOverlay& overlay =
        ui_state->arena_layout_runtime.zones[static_cast<size_t>(ui_state->selected_zone_index)];
    const Point2d current_center = runtime_geometry_center(overlay.geometry);
    bool changed = false;

    Point2d center_handle{};
    if (update_drag_point(4200, current_center, ImVec4(0.15f, 0.95f, 0.55f, 1.0f), 6.0f, &center_handle)) {
        const Point2d layout_center = camera_point_to_layout_point(*ui_state, center_handle);
        if (zone.geometry.type == LayoutGeometryType::kCircle) {
            zone.geometry.circle.cx = layout_center.x;
            zone.geometry.circle.cy = layout_center.y;
        } else {
            zone.geometry.rectangle.x = layout_center.x - zone.geometry.rectangle.width * 0.5;
            zone.geometry.rectangle.y = layout_center.y - zone.geometry.rectangle.height * 0.5;
        }
        changed = true;
    }

    const double current_scale = std::max(1e-6, effective_registration_scale(*ui_state));
    if (zone.geometry.type == LayoutGeometryType::kCircle) {
        Point2d radius_handle{};
        if (update_drag_point(
                4201,
                runtime_geometry_right_handle(overlay.geometry),
                ImVec4(0.95f, 0.95f, 0.25f, 1.0f),
                5.0f,
                &radius_handle)) {
            const double dx = radius_handle.x - current_center.x;
            const double dy = radius_handle.y - current_center.y;
            zone.geometry.circle.r = std::max(0.0, std::sqrt(dx * dx + dy * dy) / current_scale);
            changed = true;
        }
        return changed;
    }

    const Point2d layout_center = layout_geometry_center(zone.geometry);
    Point2d width_handle{};
    if (update_drag_point(
            4202,
            runtime_geometry_right_handle(overlay.geometry),
            ImVec4(0.95f, 0.95f, 0.25f, 1.0f),
            5.0f,
            &width_handle)) {
        const Point2d layout_handle = camera_point_to_layout_point(*ui_state, width_handle);
        zone.geometry.rectangle.width = std::max(0.0, 2.0 * std::abs(layout_handle.x - layout_center.x));
        zone.geometry.rectangle.x = layout_center.x - zone.geometry.rectangle.width * 0.5;
        changed = true;
    }

    Point2d height_handle{};
    if (update_drag_point(
            4203,
            runtime_geometry_bottom_handle(overlay.geometry),
            ImVec4(0.95f, 0.60f, 0.25f, 1.0f),
            5.0f,
            &height_handle)) {
        const Point2d layout_handle = camera_point_to_layout_point(*ui_state, height_handle);
        zone.geometry.rectangle.height = std::max(0.0, 2.0 * std::abs(layout_handle.y - layout_center.y));
        zone.geometry.rectangle.y = layout_center.y - zone.geometry.rectangle.height * 0.5;
        changed = true;
    }

    return changed;
}

bool capture_single_camera_frame(
    SpatialLayoutUiState* ui_state,
    CameraEmergent* ecams,
    CameraParams* cameras_params,
    std::string* error_out)
{
    if (ui_state == nullptr || ecams == nullptr || cameras_params == nullptr) {
        if (error_out) {
            *error_out = "Capture frame received invalid state or camera pointers.";
        }
        return false;
    }

    const int selected_camera = ui_state->selected_camera;
    CameraEmergent* ecam = &ecams[selected_camera];
    CameraParams* camera_params = &cameras_params[selected_camera];

    int dropped_frames = 0;
    int width = 0;
    int height = 0;
    std::string capture_error;
    if (!orange::preview::capture_single_frame_rgba(
            &ecam->camera,
            camera_params,
            kSpatialCaptureBufferCount,
            1000,
            &ui_state->captured_rgba,
            &width,
            &height,
            &dropped_frames,
            &capture_error)) {
        if (error_out) {
            *error_out = capture_error;
        }
        return false;
    }

    orange::preview::update_rgba_texture(
        &ui_state->captured_texture,
        &ui_state->captured_texture_width,
        &ui_state->captured_texture_height,
        ui_state->captured_rgba,
        width,
        height);
    clear_detected_experimental_area_circle(ui_state);
    ui_state->has_capture = true;
    ui_state->captured_camera_serial = camera_params->camera_serial;
    ui_state->captured_source_array_role = "images_full";
    ui_state->captured_capture_mode = "single_camera_direct_still";
    ui_state->preview_error.clear();

    std::ostringstream status;
    status << "Captured " << width << "x" << height << " from " << camera_params->camera_serial;
    if (dropped_frames > 0) {
        status << " (dropped " << dropped_frames << " buffered frames)";
    }
    ui_state->preview_status = status.str();
    return true;
}

bool capture_live_stream_preview_texture(
    SpatialLayoutUiState* ui_state,
    const CameraParams& camera_params,
    const CameraEachSelect& camera_select,
    GLuint preview_texture,
    std::string* error_out)
{
    if (ui_state == nullptr) {
        if (error_out) {
            *error_out = "Live stream snapshot received a null UI state.";
        }
        return false;
    }
    if (!camera_select.stream_on || preview_texture == 0) {
        if (error_out) {
            *error_out = "Start streaming and wait for a live preview frame before taking a live stream snapshot.";
        }
        return false;
    }

    const int width = std::max(1, static_cast<int>(camera_params.width) / std::max(1, camera_select.downsample));
    const int height = std::max(1, static_cast<int>(camera_params.height) / std::max(1, camera_select.downsample));
    std::vector<unsigned char> rgba(static_cast<size_t>(width) * static_cast<size_t>(height) * 4u);

    GLint previous_texture = 0;
    GLint previous_pack_alignment = 4;
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &previous_texture);
    glGetIntegerv(GL_PACK_ALIGNMENT, &previous_pack_alignment);
    glBindTexture(GL_TEXTURE_2D, preview_texture);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
    const GLenum gl_status = glGetError();
    glPixelStorei(GL_PACK_ALIGNMENT, previous_pack_alignment);
    glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(previous_texture));
    if (gl_status != GL_NO_ERROR) {
        if (error_out) {
            std::ostringstream oss;
            oss << "Failed to read live preview texture for " << camera_params.camera_serial
                << " (GL error 0x" << std::hex << gl_status << ").";
            *error_out = oss.str();
        }
        return false;
    }

    ui_state->captured_rgba = std::move(rgba);
    ui_state->captured_texture_width = width;
    ui_state->captured_texture_height = height;
    ui_state->captured_camera_serial = camera_params.camera_serial;
    ui_state->captured_source_array_role =
        camera_select.downsample > 1 ? "images_ds" : "images_full";
    ui_state->captured_capture_mode = "live_stream_preview_snapshot";
    ui_state->has_capture = true;
    ui_state->preview_error.clear();
    clear_detected_experimental_area_circle(ui_state);

    std::string texture_error;
    if (!orange::preview::update_rgba_texture(
            &ui_state->captured_texture,
            &ui_state->captured_texture_width,
            &ui_state->captured_texture_height,
            ui_state->captured_rgba,
            width,
            height,
            &texture_error)) {
        if (error_out) {
            *error_out = texture_error;
        }
        return false;
    }

    reset_registration_from_frame(ui_state);

    std::ostringstream status;
    status << "Captured live stream preview " << width << "x" << height
           << " from " << camera_params.camera_serial
           << " as " << ui_state->captured_source_array_role;
    if (camera_select.downsample > 1) {
        status << " (display downsample " << camera_select.downsample << "x)";
    }
    ui_state->preview_status = status.str();
    return true;
}

bool apply_full_resolution_stream_snapshot(
    SpatialLayoutUiState* ui_state,
    const SpatialSnapshotResult& result,
    std::string* error_out)
{
    if (ui_state == nullptr) {
        if (error_out) {
            *error_out = "Full-resolution stream snapshot received a null UI state.";
        }
        return false;
    }
    if (!result.ok) {
        if (error_out) {
            *error_out = result.error.empty()
                             ? "Full-resolution stream snapshot failed."
                             : result.error;
        }
        return false;
    }
    if (result.width <= 0 || result.height <= 0 || result.rgba.empty()) {
        if (error_out) {
            *error_out = "Full-resolution stream snapshot returned an empty image.";
        }
        return false;
    }

    ui_state->captured_rgba = result.rgba;
    ui_state->captured_camera_serial = result.camera_serial;
    ui_state->captured_source_array_role = "images_full";
    ui_state->captured_capture_mode = "full_resolution_stream_snapshot";
    ui_state->has_capture = true;
    ui_state->pending_full_res_snapshot_request_id = 0;
    ui_state->pending_full_res_snapshot_camera_serial.clear();
    ui_state->preview_error.clear();
    clear_detected_experimental_area_circle(ui_state);

    std::string texture_error;
    if (!orange::preview::update_rgba_texture(
            &ui_state->captured_texture,
            &ui_state->captured_texture_width,
            &ui_state->captured_texture_height,
            ui_state->captured_rgba,
            result.width,
            result.height,
            &texture_error)) {
        if (error_out) {
            *error_out = texture_error;
        }
        return false;
    }

    reset_registration_from_frame(ui_state);

    std::ostringstream status;
    status << "Captured full-resolution stream snapshot "
           << result.width << "x" << result.height
           << " from " << result.camera_serial
           << " frame=" << result.local_frame_id
           << " camera_frame=" << result.camera_frame_id;
    ui_state->preview_status = status.str();
    return true;
}

bool invert_affine_3x3(const std::array<double, 9>& matrix, std::array<double, 9>* out)
{
    if (out == nullptr) {
        return false;
    }

    const double a = matrix[0];
    const double b = matrix[1];
    const double c = matrix[2];
    const double d = matrix[3];
    const double e = matrix[4];
    const double f = matrix[5];
    const double det = a * e - b * d;
    if (!std::isfinite(det) || std::abs(det) < 1e-12) {
        return false;
    }

    *out = {
        e / det, -b / det, (b * f - e * c) / det,
        -d / det, a / det, (d * c - a * f) / det,
        0.0, 0.0, 1.0
    };
    return true;
}

std::array<double, 9> build_layout_to_camera_matrix(const SpatialLayoutUiState& ui_state)
{
    double tx = ui_state.registration_tx_px;
    double ty = ui_state.registration_ty_px;
    double scale = ui_state.registration_scale;
    double rotation_deg_clockwise = ui_state.registration_rotation_deg_clockwise;

    if (ui_state.registration.type == RegistrationType::kIdentity) {
        tx = 0.0;
        ty = 0.0;
        scale = 1.0;
        rotation_deg_clockwise = 0.0;
    } else if (ui_state.registration.type == RegistrationType::kTranslation) {
        scale = 1.0;
        rotation_deg_clockwise = 0.0;
    }

    scale = std::max(scale, 1e-6);
    const double theta = rotation_deg_clockwise * kPi / 180.0;
    const double cos_theta = std::cos(theta);
    const double sin_theta = std::sin(theta);

    return {
        scale * cos_theta, -scale * sin_theta, tx,
        scale * sin_theta, scale * cos_theta, ty,
        0.0, 0.0, 1.0
    };
}

void apply_view_registration_to_editor_state(
    SpatialLayoutUiState* ui_state,
    const ViewRegistration& registration)
{
    if (ui_state == nullptr) {
        return;
    }

    ui_state->registration = registration;
    ui_state->registration.layout_coordinate_space = ui_state->layout_artifact.layout.coordinate_space;
    ui_state->registration_tx_px = registration.layout_to_camera_matrix[2];
    ui_state->registration_ty_px = registration.layout_to_camera_matrix[5];

    if (registration.type == RegistrationType::kSimilarity) {
        ui_state->registration_scale = std::max(
            1e-6,
            std::sqrt(registration.layout_to_camera_matrix[0] * registration.layout_to_camera_matrix[0] +
                      registration.layout_to_camera_matrix[3] * registration.layout_to_camera_matrix[3]));
        ui_state->registration_rotation_deg_clockwise = normalize_angle_deg(
            std::atan2(registration.layout_to_camera_matrix[3], registration.layout_to_camera_matrix[0]) * 180.0 / kPi);
    } else {
        ui_state->registration_scale = 1.0;
        ui_state->registration_rotation_deg_clockwise = 0.0;
    }
}

nlohmann::json make_arena_layout_manifest_json(
    const ArenaLayoutArtifact& artifact,
    const CameraParams& camera_params,
    const SpatialLayoutPersistedFiles& files)
{
    nlohmann::json manifest = {
        {"schema_id", kCalibrationManifestSchemaId},
        {"schema_version", kCalibrationManifestSchemaVersion},
        {"artifact_id", artifact.artifact_id},
        {"artifact_schema_id", orange::spatial::kArenaLayoutArtifactSchemaId},
        {"artifact_schema_version", orange::spatial::kArenaLayoutArtifactSchemaVersion},
        {"created_utc", artifact.created_utc},
        {"producer", {
            {"name", "orange"},
            {"artifact_schema_id", orange::spatial::kArenaLayoutArtifactSchemaId},
            {"artifact_schema_version", orange::spatial::kArenaLayoutArtifactSchemaVersion}
        }},
        {"calibration_ref", orange::spatial::calibration_ref_to_json(artifact.calibration_ref)},
        {"compatibility", {
            {"camera_serial", camera_params.camera_serial},
            {"focus", camera_params.focus},
            {"iris", camera_params.iris},
            {"exposure", camera_params.exposure},
            {"gain", camera_params.gain},
            {"pixel_format", camera_params.pixel_format},
            {"width", camera_params.width},
            {"height", camera_params.height}
        }},
        {"summary", {
            {"layout_id", artifact.layout_id},
            {"zone_count", static_cast<int>(artifact.layout.zones.size())},
            {"coordinate_space", orange::spatial::coordinate_space_to_string(artifact.layout.coordinate_space)},
            {"outer_geometry_type", orange::spatial::layout_geometry_type_to_string(artifact.layout.outer_geometry.type)}
        }},
        {"files", {
            {"manifest", files.manifest_path.filename().string()},
            {"measurement_json", files.measurement_path.filename().string()},
            {"arena_layout_runtime_json", files.arena_layout_runtime_path.filename().string()},
            {"dish_mask_runtime_json", files.dish_mask_runtime_path.filename().string()}
        }}
    };

    if (!artifact.context.canvas_id.empty()) {
        manifest["summary"]["canvas_id"] = artifact.context.canvas_id;
    }
    if (!artifact.context.dish_design_id.empty()) {
        manifest["summary"]["dish_design_id"] = artifact.context.dish_design_id;
    }
    return manifest;
}

bool save_spatial_layout_artifact(
    SpatialLayoutUiState* ui_state,
    const CameraParams& selected_camera,
    const std::string& artifact_root_dir,
    std::string* status_out,
    std::string* error_out)
{
    if (ui_state == nullptr) {
        if (error_out) {
            *error_out = "Spatial layout UI state is null.";
        }
        return false;
    }
    if (artifact_root_dir.empty()) {
        if (error_out) {
            *error_out = "Calibration artifact root directory is empty.";
        }
        return false;
    }

    if (!ensure_directory_exists(artifact_root_dir, error_out)) {
        return false;
    }

    ArenaLayoutArtifact artifact = ui_state->layout_artifact;
    if (artifact.artifact_id.empty() || artifact.artifact_id == "preview.arena_layout") {
        artifact.artifact_id = build_arena_layout_artifact_id(
            artifact.layout_id.empty() ? "layout" : artifact.layout_id,
            selected_camera,
            get_current_utc_timestamp());
    }
    artifact.created_utc = get_current_utc_timestamp();
    artifact.calibration_ref.artifact_id = artifact.artifact_id;
    artifact.calibration_ref.artifact_schema_id = orange::spatial::kArenaLayoutArtifactSchemaId;
    artifact.calibration_ref.artifact_schema_version = orange::spatial::kArenaLayoutArtifactSchemaVersion;
    artifact.calibration_ref.fingerprint = "pending";

    std::string validation_error;
    if (!orange::spatial::validate_arena_layout_artifact(artifact, &validation_error)) {
        if (error_out) {
            *error_out = "Arena layout artifact is invalid: " + validation_error;
        }
        return false;
    }
    if (!orange::spatial::validate_dish_mask_runtime(ui_state->dish_mask_runtime, &validation_error)) {
        if (error_out) {
            *error_out = "Dish-mask runtime is invalid: " + validation_error;
        }
        return false;
    }
    if (!orange::spatial::validate_arena_layout_runtime_against_artifact(
            ui_state->arena_layout_runtime,
            artifact,
            &validation_error)) {
        if (error_out) {
            *error_out = "Arena-layout runtime is invalid: " + validation_error;
        }
        return false;
    }

    nlohmann::json measurement_json = orange::spatial::arena_layout_artifact_to_json(artifact);
    artifact.calibration_ref.fingerprint = compute_json_fingerprint(measurement_json);
    measurement_json = orange::spatial::arena_layout_artifact_to_json(artifact);

    const SpatialLayoutPersistedFiles files =
        make_spatial_layout_persisted_files(artifact_root_dir, artifact.artifact_id);
    if (!ensure_directory_exists(files.artifact_dir.string(), error_out)) {
        return false;
    }

    const nlohmann::json manifest_json = make_arena_layout_manifest_json(artifact, selected_camera, files);
    const nlohmann::json arena_layout_runtime_json =
        orange::spatial::arena_layout_runtime_to_json(ui_state->arena_layout_runtime);
    const nlohmann::json dish_mask_runtime_json =
        orange::spatial::dish_mask_runtime_to_json(ui_state->dish_mask_runtime);

    if (!write_json_file(files.measurement_path, measurement_json, error_out) ||
        !write_json_file(files.arena_layout_runtime_path, arena_layout_runtime_json, error_out) ||
        !write_json_file(files.dish_mask_runtime_path, dish_mask_runtime_json, error_out) ||
        !write_json_file(files.manifest_path, manifest_json, error_out)) {
        return false;
    }
    if (!update_calibration_artifact_registry(artifact_root_dir, manifest_json, error_out)) {
        return false;
    }

    ui_state->layout_artifact = std::move(artifact);
    if (status_out) {
        *status_out = "Saved arena layout artifact to " + files.artifact_dir.string();
    }
    return true;
}

bool runtime_geometry_to_top_rim_circle(
    const RuntimeGeometry& geometry,
    orange::calibration::DishTopRimCircle* circle_out,
    std::string* error_out)
{
    if (circle_out == nullptr) {
        if (error_out) {
            *error_out = "Top-rim circle destination is null.";
        }
        return false;
    }
    if (geometry.type != RuntimeGeometryType::kCircle || geometry.circle.r <= 0.0) {
        if (error_out) {
            *error_out = "Top-rim observation requires a circular resolved experimental boundary.";
        }
        return false;
    }
    circle_out->center.x = geometry.circle.cx;
    circle_out->center.y = geometry.circle.cy;
    circle_out->radius_px = geometry.circle.r;
    return true;
}

bool captured_frame_to_gray8(const SpatialLayoutUiState& ui_state, cv::Mat* image_out, std::string* error_out)
{
    if (image_out == nullptr) {
        if (error_out) {
            *error_out = "Top-rim source image destination is null.";
        }
        return false;
    }
    if (!ui_state.has_capture ||
        ui_state.captured_texture_width <= 0 ||
        ui_state.captured_texture_height <= 0 ||
        ui_state.captured_rgba.empty()) {
        if (error_out) {
            *error_out = "Capture a frame before saving a top-rim observation.";
        }
        return false;
    }

    const size_t expected_size =
        static_cast<size_t>(ui_state.captured_texture_width) *
        static_cast<size_t>(ui_state.captured_texture_height) *
        4u;
    if (ui_state.captured_rgba.size() < expected_size) {
        if (error_out) {
            *error_out = "Captured RGBA buffer is smaller than the recorded image dimensions.";
        }
        return false;
    }

    const cv::Mat rgba(
        ui_state.captured_texture_height,
        ui_state.captured_texture_width,
        CV_8UC4,
        const_cast<unsigned char*>(ui_state.captured_rgba.data()));
    cv::Mat gray;
    cv::cvtColor(rgba, gray, cv::COLOR_RGBA2GRAY);
    *image_out = gray.clone();
    return true;
}

orange::calibration::DishTopRimHoughParams make_top_rim_hough_params(
    const SpatialLayoutUiState& ui_state,
    const orange::calibration::DishTopRimCircle& accepted_circle,
    int width,
    int height)
{
    orange::calibration::DishTopRimHoughParams params;
    const double min_dim = static_cast<double>(std::max(1, std::min(width, height)));
    const double max_dim = static_cast<double>(std::max(width, height));
    const double radius = std::max(1.0, accepted_circle.radius_px);
    params.dp = std::clamp(ui_state.hough_dp, 1.0, 3.0);
    params.min_dist_px =
        std::max(1.0, min_dim * std::clamp(ui_state.hough_min_dist_fraction, 0.01, 2.0));
    params.param1 = std::clamp(ui_state.hough_param1, 1.0, 500.0);
    params.param2 = std::clamp(ui_state.hough_param2, 1.0, 500.0);
    params.min_radius_px =
        std::max(
            4,
            static_cast<int>(
                std::floor(min_dim * std::clamp(ui_state.hough_min_radius_fraction, 0.001, 1.0))));
    params.max_radius_px =
        std::max(params.min_radius_px + 1,
                 static_cast<int>(
                     std::ceil(
                         std::min(
                             max_dim,
                             min_dim *
                                 std::clamp(ui_state.hough_max_radius_fraction, 0.001, 1.5)))));
    if (params.min_radius_px > static_cast<int>(std::floor(radius * 1.10)) ||
        params.max_radius_px < static_cast<int>(std::ceil(radius * 0.90))) {
        params.min_radius_px = std::max(4, static_cast<int>(std::floor(radius * 0.75)));
        params.max_radius_px =
            std::max(
                params.min_radius_px + 1,
                static_cast<int>(std::ceil(std::min(max_dim, radius * 1.25))));
    }
    params.radius_adjustment_px = ui_state.hough_radius_adjustment_px;
    return params;
}

bool save_dish_top_rim_observation_from_spatial_layout(
    SpatialLayoutUiState* ui_state,
    const CameraParams& selected_camera,
    const std::string& artifact_root_dir,
    std::string* status_out,
    std::string* error_out)
{
    if (ui_state == nullptr) {
        if (error_out) {
            *error_out = "Spatial layout UI state is null.";
        }
        return false;
    }
    if (artifact_root_dir.empty()) {
        if (error_out) {
            *error_out = "Calibration artifact root directory is empty.";
        }
        return false;
    }
    if (!ui_state->dish_mask_runtime.has_geometry) {
        if (error_out) {
            *error_out = "Resolved dish-mask geometry is not available yet.";
        }
        return false;
    }
    const std::string source_array_role =
        ui_state->captured_source_array_role.empty()
            ? "images_full"
            : ui_state->captured_source_array_role;
    if (source_array_role != "images_full") {
        if (error_out) {
            *error_out =
                "Top-rim observations must be saved in full-resolution camera coordinates. "
                "The current capture is a downsampled live preview; recapture with stream downsample=1 "
                "or use a full-resolution capture path.";
        }
        return false;
    }

    orange::calibration::DishTopRimCircle accepted_circle;
    if (!runtime_geometry_to_top_rim_circle(
            ui_state->dish_mask_runtime.geometry.outer_geometry,
            &accepted_circle,
            error_out)) {
        return false;
    }

    cv::Mat source_gray;
    if (!captured_frame_to_gray8(*ui_state, &source_gray, error_out)) {
        return false;
    }

    const std::string timestamp = get_current_utc_timestamp();
    orange::calibration::DishTopRimObservationRequest request;
    request.artifact_id =
        orange::calibration::build_dish_top_rim_observation_artifact_id(
            selected_camera.camera_serial,
            timestamp);
    request.created_utc = timestamp;
    request.camera.serial = selected_camera.camera_serial;
    request.camera.name = selected_camera.camera_name;
    request.camera.width = ui_state->captured_texture_width;
    request.camera.height = ui_state->captured_texture_height;
    request.camera.pixel_format = selected_camera.pixel_format.empty()
                                      ? "captured_rgba_converted_to_gray8"
                                      : selected_camera.pixel_format;
    request.capture.operation_id = "spatial_layout_top_rim_" + request.artifact_id;
    request.capture.capture_mode = ui_state->captured_capture_mode.empty()
                                       ? "session_local_operator_still"
                                       : ui_state->captured_capture_mode;
    request.capture.filter_state = "unknown";
    request.capture.runtime_filter_state = "unknown";
    request.capture.light_state = "unknown";
    request.capture.projector_state = "unknown";
    request.capture.projector_visible_to_camera = false;
    request.capture.exposure_us = static_cast<double>(selected_camera.exposure);
    request.capture.frame_rate_hz = static_cast<double>(selected_camera.frame_rate);
    request.capture.requires_filter_reinstalled_repeatably = false;
    request.source_array_role = source_array_role;
    request.source_frame_index = 0;
    request.valid_region_erosion_px = std::max(0.0, ui_state->edge_margin_px);
    request.operator_confirmed = true;
    request.operator_status = "orange_spatial_layout_ui_confirmed";
    request.runtime_verification.status = "unknown";
    request.runtime_verification.reason = "runtime_850nm_rim_not_verified";
    request.write_palette_export = true;
    if (ui_state->citrus_template.available) {
        nlohmann::json rig_context = nlohmann::json::object();
        if (!ui_state->citrus_template.source_rig_name.empty()) {
            rig_context["rig_id"] = ui_state->citrus_template.source_rig_name;
        }
        if (!ui_state->citrus_template.source_canvas_name.empty()) {
            rig_context["canvas_id"] = ui_state->citrus_template.source_canvas_name;
        }
        if (!ui_state->citrus_template.source_arena_name.empty()) {
            rig_context["arena_id"] = ui_state->citrus_template.source_arena_name;
        }
        if (!ui_state->citrus_template.source_camera_id.empty()) {
            rig_context["camera_id"] = ui_state->citrus_template.source_camera_id;
        }
        if (!ui_state->citrus_template.source_config_path.empty() ||
            !ui_state->citrus_template.source_config_name.empty()) {
            rig_context["citrus_config_ref"] = {
                {"source", "spatial_layout_import"},
                {"path", ui_state->citrus_template.source_config_path},
                {"config_name", ui_state->citrus_template.source_config_name}
            };
        }
        if (ui_state->citrus_template.has_camera_to_canvas_homography) {
            rig_context["citrus_homography_ref"] = {
                {"available", true},
                {"source", "citrus_homography_sidecar"},
                {"direction", "camera_view_px_to_final_display_canvas_px"}
            };
        }
        request.image_set_rig_context = rig_context;
    }

    const orange::calibration::DishTopRimHoughParams hough_params =
        make_top_rim_hough_params(
            *ui_state,
            accepted_circle,
            ui_state->captured_texture_width,
            ui_state->captured_texture_height);

    orange::calibration::DishTopRimObservationWriteResult result;
    if (!orange::calibration::write_dish_top_rim_observation_artifact(
            artifact_root_dir,
            request,
            source_gray,
            hough_params,
            accepted_circle,
            &result,
            error_out)) {
        return false;
    }

    const orange::calibration::DishTopRimObservationArtifactPaths paths =
        orange::calibration::make_dish_top_rim_observation_artifact_paths(
            artifact_root_dir,
            result.artifact_id);
    if (status_out) {
        *status_out =
            "Saved top-rim observation to " + result.artifact_dir +
            ", image-set companion to " + paths.image_set_json_path +
            " and spatial dish-mask runtime export to " +
            paths.spatial_dish_mask_runtime_export_path;
    }
    return true;
}

bool resolve_measurement_json_path_from_selection(
    const std::filesystem::path& selected_path,
    std::filesystem::path* measurement_path_out,
    std::string* error_out)
{
    if (measurement_path_out == nullptr) {
        if (error_out) {
            *error_out = "Null measurement-path destination.";
        }
        return false;
    }

    nlohmann::json selected_json;
    if (!read_json_file(selected_path, &selected_json, error_out)) {
        return false;
    }

    const std::string schema_id = selected_json.value("schema_id", "");
    if (schema_id == orange::spatial::kArenaLayoutArtifactSchemaId) {
        *measurement_path_out = selected_path;
        return true;
    }
    if (schema_id != kCalibrationManifestSchemaId) {
        if (error_out) {
            *error_out = "Selected JSON is neither an arena layout artifact nor a calibration manifest.";
        }
        return false;
    }

    std::string measurement_filename = kSpatialLayoutMeasurementFilename;
    if (selected_json.contains("files") &&
        selected_json["files"].is_object() &&
        selected_json["files"].contains("measurement_json") &&
        selected_json["files"]["measurement_json"].is_string()) {
        measurement_filename = selected_json["files"]["measurement_json"].get<std::string>();
    }
    *measurement_path_out = selected_path.parent_path() / measurement_filename;
    return true;
}

bool load_spatial_layout_artifact(
    SpatialLayoutUiState* ui_state,
    const std::filesystem::path& selected_json_path,
    std::string* status_out,
    std::string* error_out)
{
    if (ui_state == nullptr) {
        if (error_out) {
            *error_out = "Spatial layout UI state is null.";
        }
        return false;
    }

    std::filesystem::path measurement_path;
    if (!resolve_measurement_json_path_from_selection(selected_json_path, &measurement_path, error_out)) {
        return false;
    }

    nlohmann::json measurement_json;
    if (!read_json_file(measurement_path, &measurement_json, error_out)) {
        return false;
    }

    ArenaLayoutArtifact artifact;
    if (!orange::spatial::parse_arena_layout_artifact_json(measurement_json, &artifact, error_out)) {
        return false;
    }

    ui_state->layout_artifact = artifact;
    ui_state->selected_zone_index = clamp_index(
        ui_state->selected_zone_index,
        static_cast<int>(ui_state->layout_artifact.layout.zones.size()));
    clear_citrus_template_import(ui_state);
    clear_detected_experimental_area_circle(ui_state);

    bool loaded_runtime_registration = false;
    std::vector<std::string> loaded_parts;
    const std::filesystem::path artifact_dir = measurement_path.parent_path();
    const std::filesystem::path arena_layout_runtime_path =
        artifact_dir / kSpatialLayoutArenaLayoutRuntimeFilename;
    const std::filesystem::path dish_mask_runtime_path =
        artifact_dir / kSpatialLayoutDishMaskRuntimeFilename;

    if (std::filesystem::exists(arena_layout_runtime_path)) {
        nlohmann::json runtime_json;
        if (!read_json_file(arena_layout_runtime_path, &runtime_json, error_out)) {
            return false;
        }
        ArenaLayoutRuntime runtime;
        if (!orange::spatial::parse_arena_layout_runtime_json(runtime_json, &runtime, error_out) ||
            !orange::spatial::validate_arena_layout_runtime_against_artifact(runtime, artifact, error_out)) {
            return false;
        }
        apply_view_registration_to_editor_state(ui_state, runtime.registration);
        loaded_runtime_registration = true;
        loaded_parts.push_back("registration");
    }

    if (std::filesystem::exists(dish_mask_runtime_path)) {
        nlohmann::json dish_mask_json;
        if (!read_json_file(dish_mask_runtime_path, &dish_mask_json, error_out)) {
            return false;
        }
        DishMaskRuntime dish_mask_runtime;
        if (!orange::spatial::parse_dish_mask_runtime_json(dish_mask_json, &dish_mask_runtime, error_out)) {
            return false;
        }
        if (dish_mask_runtime.has_geometry) {
            ui_state->edge_margin_px = std::max(0.0, dish_mask_runtime.geometry.edge_margin_px);
            loaded_parts.push_back("edge_margin");
        }
    }

    if (!loaded_runtime_registration) {
        ui_state->registration.layout_coordinate_space = ui_state->layout_artifact.layout.coordinate_space;
        if (ui_state->has_capture) {
            reset_registration_from_frame(ui_state);
        }
    }

    if (status_out) {
        std::ostringstream status;
        status << "Loaded arena layout artifact from " << measurement_path.string();
        if (!loaded_parts.empty()) {
            status << " with ";
            for (size_t idx = 0; idx < loaded_parts.size(); ++idx) {
                if (idx > 0) {
                    status << (idx + 1 == loaded_parts.size() ? " and " : ", ");
                }
                status << loaded_parts[idx];
            }
            status << " sidecar";
            if (loaded_parts.size() > 1) {
                status << "s";
            }
        }
        *status_out = status.str();
    }
    return true;
}

Point2d transform_point(const std::array<double, 9>& matrix, double x, double y)
{
    return make_point(
        matrix[0] * x + matrix[1] * y + matrix[2],
        matrix[3] * x + matrix[4] * y + matrix[5]);
}

RuntimeGeometry transform_layout_geometry(
    const LayoutGeometry& layout_geometry,
    const std::array<double, 9>& layout_to_camera_matrix,
    double rotation_deg_clockwise)
{
    if (layout_geometry.type == LayoutGeometryType::kCircle) {
        const Point2d center = transform_point(layout_to_camera_matrix, layout_geometry.circle.cx, layout_geometry.circle.cy);
        const double scale =
            std::sqrt(layout_to_camera_matrix[0] * layout_to_camera_matrix[0] +
                      layout_to_camera_matrix[3] * layout_to_camera_matrix[3]);
        return runtime_circle(center.x, center.y, std::abs(scale) * layout_geometry.circle.r);
    }

    const double center_x = layout_geometry.rectangle.x + layout_geometry.rectangle.width * 0.5;
    const double center_y = layout_geometry.rectangle.y + layout_geometry.rectangle.height * 0.5;
    const Point2d center = transform_point(layout_to_camera_matrix, center_x, center_y);
    const double scale =
        std::sqrt(layout_to_camera_matrix[0] * layout_to_camera_matrix[0] +
                  layout_to_camera_matrix[3] * layout_to_camera_matrix[3]);
    return runtime_oriented_rectangle(
        center.x,
        center.y,
        std::abs(scale) * layout_geometry.rectangle.width,
        std::abs(scale) * layout_geometry.rectangle.height,
        rotation_deg_clockwise);
}

RuntimeGeometry inset_runtime_geometry(const RuntimeGeometry& geometry, double edge_margin_px)
{
    if (geometry.type == RuntimeGeometryType::kCircle) {
        return runtime_circle(
            geometry.circle.cx,
            geometry.circle.cy,
            std::max(0.0, geometry.circle.r - edge_margin_px));
    }

    return runtime_oriented_rectangle(
        geometry.oriented_rectangle.cx,
        geometry.oriented_rectangle.cy,
        std::max(0.0, geometry.oriented_rectangle.width - 2.0 * edge_margin_px),
        std::max(0.0, geometry.oriented_rectangle.height - 2.0 * edge_margin_px),
        geometry.oriented_rectangle.rotation_deg_clockwise);
}

std::array<Point2d, 4> oriented_rectangle_corners(const RuntimeGeometry& geometry)
{
    const auto& rect = geometry.oriented_rectangle;
    const double half_width = rect.width * 0.5;
    const double half_height = rect.height * 0.5;
    const double theta = rect.rotation_deg_clockwise * kPi / 180.0;
    const double cos_theta = std::cos(theta);
    const double sin_theta = std::sin(theta);

    auto rotate_and_translate = [&](double local_x, double local_y) -> Point2d {
        return make_point(
            rect.cx + cos_theta * local_x - sin_theta * local_y,
            rect.cy + sin_theta * local_x + cos_theta * local_y);
    };

    return {
        rotate_and_translate(-half_width, -half_height),
        rotate_and_translate(half_width, -half_height),
        rotate_and_translate(half_width, half_height),
        rotate_and_translate(-half_width, half_height)
    };
}

bool point_inside_image(const Point2d& point, int image_width, int image_height)
{
    return point.x >= 0.0 && point.x <= static_cast<double>(image_width) &&
           point.y >= 0.0 && point.y <= static_cast<double>(image_height);
}

VisibilityStatus compute_visibility_status(const RuntimeGeometry& geometry, int image_width, int image_height)
{
    if (image_width <= 0 || image_height <= 0) {
        return VisibilityStatus::kFull;
    }

    if (geometry.type == RuntimeGeometryType::kCircle) {
        const double left = geometry.circle.cx - geometry.circle.r;
        const double right = geometry.circle.cx + geometry.circle.r;
        const double top = geometry.circle.cy - geometry.circle.r;
        const double bottom = geometry.circle.cy + geometry.circle.r;
        if (right < 0.0 || bottom < 0.0 ||
            left > static_cast<double>(image_width) ||
            top > static_cast<double>(image_height)) {
            return VisibilityStatus::kOccluded;
        }
        if (left >= 0.0 && top >= 0.0 &&
            right <= static_cast<double>(image_width) &&
            bottom <= static_cast<double>(image_height)) {
            return VisibilityStatus::kFull;
        }
        return VisibilityStatus::kPartial;
    }

    const std::array<Point2d, 4> corners = oriented_rectangle_corners(geometry);
    int inside_count = 0;
    double min_x = std::numeric_limits<double>::max();
    double max_x = std::numeric_limits<double>::lowest();
    double min_y = std::numeric_limits<double>::max();
    double max_y = std::numeric_limits<double>::lowest();
    for (const Point2d& corner : corners) {
        if (point_inside_image(corner, image_width, image_height)) {
            ++inside_count;
        }
        min_x = std::min(min_x, corner.x);
        max_x = std::max(max_x, corner.x);
        min_y = std::min(min_y, corner.y);
        max_y = std::max(max_y, corner.y);
    }

    if (inside_count == 4) {
        return VisibilityStatus::kFull;
    }
    if (max_x < 0.0 || max_y < 0.0 ||
        min_x > static_cast<double>(image_width) ||
        min_y > static_cast<double>(image_height)) {
        return VisibilityStatus::kOccluded;
    }
    return VisibilityStatus::kPartial;
}

void rebuild_schema_preview(SpatialLayoutUiState* ui_state, const CameraParams* selected_camera)
{
    if (ui_state == nullptr) {
        return;
    }

    ui_state->layout_artifact.calibration_ref.artifact_id = ui_state->layout_artifact.artifact_id;
    ui_state->layout_artifact.calibration_ref.artifact_schema_id = orange::spatial::kArenaLayoutArtifactSchemaId;
    ui_state->layout_artifact.calibration_ref.artifact_schema_version = orange::spatial::kArenaLayoutArtifactSchemaVersion;
    if (ui_state->layout_artifact.calibration_ref.fingerprint.empty()) {
        ui_state->layout_artifact.calibration_ref.fingerprint = "preview-only";
    }

    ui_state->registration.layout_coordinate_space = ui_state->layout_artifact.layout.coordinate_space;
    ui_state->registration.layout_to_camera_matrix = build_layout_to_camera_matrix(*ui_state);
    ui_state->registration.has_camera_to_layout_matrix =
        invert_affine_3x3(ui_state->registration.layout_to_camera_matrix, &ui_state->registration.camera_to_layout_matrix);

    const double effective_rotation_deg =
        (ui_state->registration.type == RegistrationType::kSimilarity)
            ? ui_state->registration_rotation_deg_clockwise
            : 0.0;

    ui_state->arena_layout_runtime = {};
    ui_state->arena_layout_runtime.enabled = true;
    ui_state->arena_layout_runtime.layout_id = ui_state->layout_artifact.layout_id;
    ui_state->arena_layout_runtime.coordinate_space = CoordinateSpace::kCameraNativePixels;
    ui_state->arena_layout_runtime.registration = ui_state->registration;
    ui_state->arena_layout_runtime.zones.clear();

    for (const ArenaLayoutZone& zone : ui_state->layout_artifact.layout.zones) {
        ResolvedZoneOverlay overlay;
        overlay.zone_id = zone.zone_id;
        overlay.has_zone_index = zone.has_zone_index;
        overlay.zone_index = zone.zone_index;
        overlay.geometry =
            transform_layout_geometry(zone.geometry, ui_state->registration.layout_to_camera_matrix, effective_rotation_deg);
        overlay.visibility_status = compute_visibility_status(
            overlay.geometry,
            ui_state->captured_texture_width,
            ui_state->captured_texture_height);
        ui_state->arena_layout_runtime.zones.push_back(std::move(overlay));
    }

    ui_state->dish_mask_runtime = {};
    ui_state->dish_mask_runtime.enabled = true;
    ui_state->dish_mask_runtime.has_geometry = true;
    switch (ui_state->registration.source) {
        case RegistrationSource::kDetectedFit:
            ui_state->dish_mask_runtime.source = ObservationSource::kDetectedFit;
            break;
        case RegistrationSource::kImported:
            ui_state->dish_mask_runtime.source = ObservationSource::kImported;
            break;
        case RegistrationSource::kManual:
            ui_state->dish_mask_runtime.source = ObservationSource::kManual;
            break;
        case RegistrationSource::kIdentity:
        case RegistrationSource::kManualFit:
        default:
            ui_state->dish_mask_runtime.source = ObservationSource::kManualFit;
            break;
    }
    ui_state->dish_mask_runtime.geometry.coordinate_space = CoordinateSpace::kCameraNativePixels;
    ui_state->dish_mask_runtime.geometry.outer_geometry =
        transform_layout_geometry(ui_state->layout_artifact.layout.outer_geometry,
                                  ui_state->registration.layout_to_camera_matrix,
                                  effective_rotation_deg);
    ui_state->dish_mask_runtime.geometry.valid_geometry =
        inset_runtime_geometry(ui_state->dish_mask_runtime.geometry.outer_geometry, ui_state->edge_margin_px);
    ui_state->dish_mask_runtime.geometry.edge_margin_px = ui_state->edge_margin_px;

    ui_state->preview_calibration = {};
    ui_state->preview_calibration.has_dish_mask = true;
    ui_state->preview_calibration.dish_mask.calibration_ref = CalibrationRef{
        "preview.dish_mask",
        orange::spatial::kDishMaskArtifactSchemaId,
        orange::spatial::kDishMaskArtifactSchemaVersion,
        "preview-only"
    };
    ui_state->preview_calibration.dish_mask.runtime = ui_state->dish_mask_runtime;
    ui_state->preview_calibration.has_arena_layout = true;
    ui_state->preview_calibration.arena_layout.calibration_ref = ui_state->layout_artifact.calibration_ref;
    ui_state->preview_calibration.arena_layout.runtime = ui_state->arena_layout_runtime;

    ui_state->canonical_layout_json = orange::spatial::arena_layout_artifact_to_json(ui_state->layout_artifact).dump(2);
    ui_state->runtime_preview_json = orange::spatial::camera_spatial_calibration_to_json(ui_state->preview_calibration).dump(2);

    std::string error;
    ui_state->preview_valid =
        orange::spatial::validate_arena_layout_artifact(ui_state->layout_artifact, &error) &&
        orange::spatial::validate_dish_mask_runtime(ui_state->dish_mask_runtime, &error) &&
        orange::spatial::validate_arena_layout_runtime_against_artifact(
            ui_state->arena_layout_runtime,
            ui_state->layout_artifact,
            &error) &&
        orange::spatial::validate_camera_spatial_calibration(ui_state->preview_calibration, &error);

    if (selected_camera != nullptr) {
        std::ostringstream status;
        status << (ui_state->preview_valid ? "Preview valid" : "Preview invalid")
               << " for " << selected_camera->camera_serial;
        if (ui_state->has_capture) {
            status << " (" << ui_state->captured_texture_width << "x" << ui_state->captured_texture_height << ")";
        }
        ui_state->preview_status = status.str();
    }
    ui_state->preview_error = ui_state->preview_valid ? std::string() : error;
}

void draw_circle_geometry(const RuntimeGeometry& geometry, ImU32 color, float thickness)
{
    const ImPlotPoint center_point(geometry.circle.cx, geometry.circle.cy);
    const ImPlotPoint edge_point(geometry.circle.cx + geometry.circle.r, geometry.circle.cy);
    const ImVec2 center = ImPlot::PlotToPixels(center_point);
    const ImVec2 edge = ImPlot::PlotToPixels(edge_point);
    const float radius_px = std::abs(edge.x - center.x);
    ImPlot::GetPlotDrawList()->AddCircle(center, radius_px, color, 0, thickness);
}

void draw_oriented_rectangle_geometry(const RuntimeGeometry& geometry, ImU32 color, float thickness)
{
    const std::array<Point2d, 4> corners = oriented_rectangle_corners(geometry);
    ImDrawList* draw_list = ImPlot::GetPlotDrawList();
    for (size_t i = 0; i < corners.size(); ++i) {
        const Point2d& a = corners[i];
        const Point2d& b = corners[(i + 1) % corners.size()];
        draw_list->AddLine(
            ImPlot::PlotToPixels(ImPlotPoint(a.x, a.y)),
            ImPlot::PlotToPixels(ImPlotPoint(b.x, b.y)),
            color,
            thickness);
    }
}

void draw_runtime_geometry(const RuntimeGeometry& geometry, ImU32 color, float thickness)
{
    if (geometry.type == RuntimeGeometryType::kCircle) {
        draw_circle_geometry(geometry, color, thickness);
        return;
    }
    draw_oriented_rectangle_geometry(geometry, color, thickness);
}

void draw_hough_proposal_overlay(const RuntimeGeometry& geometry)
{
    if (geometry.type != RuntimeGeometryType::kCircle || geometry.circle.r <= 0.0) {
        return;
    }

    const ImU32 color = IM_COL32(255, 30, 190, 255);
    draw_circle_geometry(geometry, color, 3.2f);

    const ImVec2 center = ImPlot::PlotToPixels(
        ImPlotPoint(geometry.circle.cx, geometry.circle.cy));
    ImDrawList* draw_list = ImPlot::GetPlotDrawList();
    constexpr float cross = 9.0f;
    draw_list->AddLine(
        ImVec2(center.x - cross, center.y),
        ImVec2(center.x + cross, center.y),
        color,
        2.2f);
    draw_list->AddLine(
        ImVec2(center.x, center.y - cross),
        ImVec2(center.x, center.y + cross),
        color,
        2.2f);
    draw_list->AddText(
        ImVec2(center.x + 10.0f, center.y - 20.0f),
        color,
        "Hough");
}

void draw_citrus_projected_circle_overlay(const RuntimeGeometry& geometry)
{
    if (geometry.type != RuntimeGeometryType::kCircle || geometry.circle.r <= 0.0) {
        return;
    }

    const ImU32 color = IM_COL32(100, 190, 255, 230);
    draw_circle_geometry(geometry, color, 2.0f);

    const ImVec2 center = ImPlot::PlotToPixels(
        ImPlotPoint(geometry.circle.cx, geometry.circle.cy));
    ImDrawList* draw_list = ImPlot::GetPlotDrawList();
    constexpr float marker = 9.0f;
    const ImVec2 p0(center.x, center.y - marker);
    const ImVec2 p1(center.x - marker * 0.866f, center.y + marker * 0.5f);
    const ImVec2 p2(center.x + marker * 0.866f, center.y + marker * 0.5f);
    draw_list->AddTriangleFilled(p0, p1, p2, color);
    draw_list->AddTriangle(p0, p1, p2, IM_COL32(10, 20, 35, 240), 1.5f);
    draw_list->AddText(
        ImVec2(center.x + 10.0f, center.y + 6.0f),
        color,
        "Citrus");
}

bool compute_citrus_corrected_outline_overlay(
    const SpatialLayoutUiState& ui_state,
    Point2d* corrected_center_camera_out,
    std::vector<Point2d>* corrected_outline_camera_out)
{
    if (!ui_state.has_detected_experimental_area_circle ||
        ui_state.detected_experimental_area_geometry.type != RuntimeGeometryType::kCircle ||
        !ui_state.citrus_template.available ||
        !ui_state.citrus_template.has_camera_to_canvas_homography ||
        !ui_state.citrus_template.has_canvas_to_camera_homography ||
        !ui_state.citrus_template.has_arena_canvas_region) {
        return false;
    }

    const Point2d observed_center_camera = make_point(
        ui_state.detected_experimental_area_geometry.circle.cx,
        ui_state.detected_experimental_area_geometry.circle.cy);
    Point2d observed_center_canvas{};
    if (!transform_point_projective(
            ui_state.citrus_template.camera_to_canvas_homography,
            observed_center_camera,
            &observed_center_canvas)) {
        return false;
    }

    const Point2d corrected_center_arena_relative =
        citrus_canvas_to_arena_relative_px(
            ui_state.citrus_template,
            observed_center_canvas);
    if (!sample_citrus_experimental_area_outline_in_camera_px(
            ui_state.citrus_template,
            corrected_center_arena_relative,
            corrected_outline_camera_out,
            nullptr)) {
        return false;
    }

    const Point2d corrected_center_canvas =
        citrus_arena_relative_to_canvas_px(
            ui_state.citrus_template,
            corrected_center_arena_relative);
    Point2d corrected_center_camera{};
    if (!transform_point_projective(
            ui_state.citrus_template.canvas_to_camera_homography,
            corrected_center_canvas,
            &corrected_center_camera)) {
        return false;
    }
    if (corrected_center_camera_out != nullptr) {
        *corrected_center_camera_out = corrected_center_camera;
    }
    return true;
}

void draw_citrus_corrected_center_overlay(
    const RuntimeGeometry& current_citrus_geometry,
    const Point2d& corrected_center_camera,
    const std::vector<Point2d>& corrected_outline_camera)
{
    if (corrected_outline_camera.size() < 3) {
        return;
    }

    const ImU32 color = IM_COL32(70, 255, 150, 255);
    const ImVec2 corrected = ImPlot::PlotToPixels(
        ImPlotPoint(corrected_center_camera.x, corrected_center_camera.y));
    ImDrawList* draw_list = ImPlot::GetPlotDrawList();

    for (size_t idx = 0; idx < corrected_outline_camera.size(); ++idx) {
        const Point2d& a = corrected_outline_camera[idx];
        const Point2d& b = corrected_outline_camera[(idx + 1) % corrected_outline_camera.size()];
        draw_list->AddLine(
            ImPlot::PlotToPixels(ImPlotPoint(a.x, a.y)),
            ImPlot::PlotToPixels(ImPlotPoint(b.x, b.y)),
            color,
            2.6f);
    }

    if (current_citrus_geometry.type == RuntimeGeometryType::kCircle &&
        current_citrus_geometry.circle.r > 0.0) {
        const ImVec2 current = ImPlot::PlotToPixels(
            ImPlotPoint(current_citrus_geometry.circle.cx, current_citrus_geometry.circle.cy));
        draw_list->AddLine(current, corrected, color, 2.0f);
        draw_list->AddCircleFilled(current, 3.0f, color, 12);
    }

    constexpr float marker = 8.0f;
    const ImVec2 p0(corrected.x, corrected.y - marker);
    const ImVec2 p1(corrected.x + marker, corrected.y);
    const ImVec2 p2(corrected.x, corrected.y + marker);
    const ImVec2 p3(corrected.x - marker, corrected.y);
    draw_list->AddQuadFilled(p0, p1, p2, p3, color);
    draw_list->AddQuad(p0, p1, p2, p3, IM_COL32(10, 35, 20, 240), 1.5f);
    draw_list->AddText(
        ImVec2(corrected.x + 10.0f, corrected.y - 4.0f),
        color,
        "Corrected");
}

void draw_zone_label(const ResolvedZoneOverlay& zone, const ArenaLayoutArtifact& artifact, ImU32 color)
{
    std::string label = zone.zone_id;
    for (const ArenaLayoutZone& canonical_zone : artifact.layout.zones) {
        if (canonical_zone.zone_id == zone.zone_id && !canonical_zone.display_label.empty()) {
            label = canonical_zone.display_label;
            break;
        }
    }

    Point2d center{};
    if (zone.geometry.type == RuntimeGeometryType::kCircle) {
        center = make_point(zone.geometry.circle.cx, zone.geometry.circle.cy);
    } else {
        center = make_point(zone.geometry.oriented_rectangle.cx, zone.geometry.oriented_rectangle.cy);
    }
    ImPlot::GetPlotDrawList()->AddText(
        ImPlot::PlotToPixels(ImPlotPoint(center.x, center.y)),
        color,
        label.c_str());
}

bool draw_runtime_preview(SpatialLayoutUiState* ui_state)
{
    if (ui_state == nullptr || ui_state->captured_texture == 0 ||
        ui_state->captured_texture_width <= 0 || ui_state->captured_texture_height <= 0) {
        return false;
    }

    if (!orange::ui::begin_image_canvas(
            "Spatial Layout View",
            ui_state->captured_texture,
            ui_state->captured_texture_width,
            ui_state->captured_texture_height,
            &ui_state->captured_canvas_view,
            0.62f)) {
        return false;
    }

    bool changed = false;
    ImPlot::PushPlotClipRect();
    if (ui_state->has_citrus_projected_circle &&
        (ui_state->citrus_template.source_camera_id.empty() ||
         ui_state->captured_camera_serial.empty() ||
         ui_state->citrus_template.source_camera_id == ui_state->captured_camera_serial)) {
        draw_citrus_projected_circle_overlay(ui_state->citrus_projected_circle_geometry);
    }
    draw_runtime_geometry(
        ui_state->dish_mask_runtime.geometry.outer_geometry,
        IM_COL32(255, 165, 0, 230),
        2.0f);
    draw_runtime_geometry(
        ui_state->dish_mask_runtime.geometry.valid_geometry,
        IM_COL32(255, 220, 70, 210),
        2.0f);

    for (size_t idx = 0; idx < ui_state->arena_layout_runtime.zones.size(); ++idx) {
        const ResolvedZoneOverlay& zone = ui_state->arena_layout_runtime.zones[idx];
        const bool selected = static_cast<int>(idx) == ui_state->selected_zone_index;
        const ImU32 color = selected
            ? IM_COL32(90, 235, 255, 255)
            : IM_COL32(40, 220, 120, 235);
        const float thickness = selected ? 2.6f : 1.8f;
        draw_runtime_geometry(zone.geometry, color, thickness);
        draw_zone_label(zone, ui_state->layout_artifact, color);
    }
    if (ui_state->show_hough_proposal_overlay &&
        ui_state->has_detected_experimental_area_circle) {
        draw_hough_proposal_overlay(ui_state->detected_experimental_area_geometry);
    }
    if (ui_state->show_citrus_corrected_center_overlay &&
        ui_state->has_detected_experimental_area_circle &&
        ui_state->has_citrus_projected_circle) {
        Point2d corrected_center_camera{};
        std::vector<Point2d> corrected_outline_camera;
        if (compute_citrus_corrected_outline_overlay(
                *ui_state,
                &corrected_center_camera,
                &corrected_outline_camera)) {
            draw_citrus_corrected_center_overlay(
                ui_state->citrus_projected_circle_geometry,
                corrected_center_camera,
                corrected_outline_camera);
        }
    }
    ImPlot::PopPlotClipRect();

    if (ui_state->canvas_edit_mode == 0) {
        changed = handle_registration_canvas_edit(ui_state) || changed;
    } else {
        changed = handle_selected_zone_canvas_edit(ui_state) || changed;
    }
    ImPlot::EndPlot();
    return changed;
}

const char* layout_coordinate_space_label(CoordinateSpace value)
{
    return value == CoordinateSpace::kLayoutMm ? "layout_mm" : "layout_units";
}

void render_layout_geometry_editor(const char* label_prefix, LayoutGeometry* geometry)
{
    if (geometry == nullptr) {
        return;
    }

    int geometry_type = geometry->type == LayoutGeometryType::kCircle ? 0 : 1;
    const char* geometry_items[] = {"circle", "rectangle"};
    if (ImGui::Combo((std::string(label_prefix) + " shape").c_str(), &geometry_type, geometry_items, IM_ARRAYSIZE(geometry_items))) {
        geometry->type = geometry_type == 0 ? LayoutGeometryType::kCircle : LayoutGeometryType::kRectangle;
    }

    if (geometry->type == LayoutGeometryType::kCircle) {
        ImGui::InputDouble((std::string(label_prefix) + " cx").c_str(), &geometry->circle.cx, 0.5);
        ImGui::InputDouble((std::string(label_prefix) + " cy").c_str(), &geometry->circle.cy, 0.5);
        ImGui::InputDouble((std::string(label_prefix) + " r").c_str(), &geometry->circle.r, 0.5);
        geometry->circle.r = std::max(0.0, geometry->circle.r);
    } else {
        ImGui::InputDouble((std::string(label_prefix) + " x").c_str(), &geometry->rectangle.x, 0.5);
        ImGui::InputDouble((std::string(label_prefix) + " y").c_str(), &geometry->rectangle.y, 0.5);
        ImGui::InputDouble((std::string(label_prefix) + " width").c_str(), &geometry->rectangle.width, 0.5);
        ImGui::InputDouble((std::string(label_prefix) + " height").c_str(), &geometry->rectangle.height, 0.5);
        geometry->rectangle.width = std::max(0.0, geometry->rectangle.width);
        geometry->rectangle.height = std::max(0.0, geometry->rectangle.height);
    }
}

void render_registration_editor(SpatialLayoutUiState* ui_state)
{
    const char* registration_type_items[] = {"identity", "translation", "similarity"};
    int registration_type = 2;
    if (ui_state->registration.type == RegistrationType::kIdentity) {
        registration_type = 0;
    } else if (ui_state->registration.type == RegistrationType::kTranslation) {
        registration_type = 1;
    }
    if (ImGui::Combo("Registration type", &registration_type, registration_type_items, IM_ARRAYSIZE(registration_type_items))) {
        ui_state->registration.type =
            registration_type == 0 ? RegistrationType::kIdentity :
            (registration_type == 1 ? RegistrationType::kTranslation : RegistrationType::kSimilarity);
    }

    const char* source_items[] = {"manual", "manual_fit", "detected_fit", "imported", "identity"};
    const RegistrationSource source_values[] = {
        RegistrationSource::kManual,
        RegistrationSource::kManualFit,
        RegistrationSource::kDetectedFit,
        RegistrationSource::kImported,
        RegistrationSource::kIdentity
    };
    int current_source = 1;
    for (int idx = 0; idx < IM_ARRAYSIZE(source_values); ++idx) {
        if (ui_state->registration.source == source_values[idx]) {
            current_source = idx;
            break;
        }
    }
    if (ImGui::Combo("Registration source", &current_source, source_items, IM_ARRAYSIZE(source_items))) {
        ui_state->registration.source = source_values[current_source];
    }

    const char* orientation_items[] = {"unknown", "trusted", "manual_confirmed", "ambiguous"};
    const OrientationStatus orientation_values[] = {
        OrientationStatus::kUnknown,
        OrientationStatus::kTrusted,
        OrientationStatus::kManualConfirmed,
        OrientationStatus::kAmbiguous
    };
    int current_orientation = 0;
    if (!ui_state->registration.has_orientation_status) {
        current_orientation = 0;
    } else {
        for (int idx = 0; idx < IM_ARRAYSIZE(orientation_values); ++idx) {
            if (ui_state->registration.orientation_status == orientation_values[idx]) {
                current_orientation = idx;
                break;
            }
        }
    }
    if (ImGui::Combo("Orientation status", &current_orientation, orientation_items, IM_ARRAYSIZE(orientation_items))) {
        ui_state->registration.has_orientation_status = true;
        ui_state->registration.orientation_status = orientation_values[current_orientation];
    }

    const bool translation_enabled =
        ui_state->registration.type == RegistrationType::kTranslation ||
        ui_state->registration.type == RegistrationType::kSimilarity;
    const bool similarity_enabled = ui_state->registration.type == RegistrationType::kSimilarity;

    ImGui::BeginDisabled(!translation_enabled);
    ImGui::InputDouble("Translate X (px)", &ui_state->registration_tx_px, 1.0, 20.0, "%.2f");
    ImGui::InputDouble("Translate Y (px)", &ui_state->registration_ty_px, 1.0, 20.0, "%.2f");
    ImGui::EndDisabled();

    ImGui::BeginDisabled(!similarity_enabled);
    ImGui::InputDouble("Scale (px / layout unit)", &ui_state->registration_scale, 0.05, 1.0, "%.4f");
    ImGui::InputDouble("Rotation CW (deg)", &ui_state->registration_rotation_deg_clockwise, 0.25, 2.0, "%.2f");
    ImGui::EndDisabled();
    ui_state->registration_scale = std::max(0.0001, ui_state->registration_scale);

    ImGui::InputInt("Fit point count", &ui_state->registration.fit_point_count);
    ui_state->registration.fit_point_count = std::max(0, ui_state->registration.fit_point_count);
    ImGui::InputDouble("Residual (px)", &ui_state->registration.residual_px, 0.1, 1.0, "%.3f");
    ui_state->registration.residual_px = std::max(0.0, ui_state->registration.residual_px);
}

void render_zone_editor(SpatialLayoutUiState* ui_state)
{
    if (ui_state == nullptr) {
        return;
    }

    if (ui_state->layout_artifact.layout.zones.empty()) {
        ImGui::TextDisabled("No zones yet.");
        if (ImGui::Button("Add zone")) {
            ui_state->layout_artifact.layout.zones.push_back(
                make_default_zone(ui_state->layout_artifact.layout.outer_geometry, 0));
            ui_state->selected_zone_index = 0;
        }
        return;
    }

    ui_state->selected_zone_index = clamp_index(
        ui_state->selected_zone_index,
        static_cast<int>(ui_state->layout_artifact.layout.zones.size()));

    std::vector<std::string> zone_labels_storage;
    std::vector<const char*> zone_labels;
    zone_labels_storage.reserve(ui_state->layout_artifact.layout.zones.size());
    zone_labels.reserve(ui_state->layout_artifact.layout.zones.size());
    for (const ArenaLayoutZone& zone : ui_state->layout_artifact.layout.zones) {
        std::ostringstream label;
        label << zone.zone_id;
        if (!zone.display_label.empty()) {
            label << " (" << zone.display_label << ")";
        }
        zone_labels_storage.push_back(label.str());
    }
    for (const std::string& label : zone_labels_storage) {
        zone_labels.push_back(label.c_str());
    }
    ImGui::Combo(
        "Selected zone",
        &ui_state->selected_zone_index,
        zone_labels.data(),
        static_cast<int>(zone_labels.size()));

    ArenaLayoutZone& zone =
        ui_state->layout_artifact.layout.zones[static_cast<size_t>(ui_state->selected_zone_index)];
    ImGui::InputText("Zone ID", &zone.zone_id);
    ImGui::Checkbox("Has zone index", &zone.has_zone_index);
    if (zone.has_zone_index) {
        ImGui::InputInt("Zone index", &zone.zone_index);
        zone.zone_index = std::max(0, zone.zone_index);
    }
    ImGui::InputText("Display label", &zone.display_label);
    render_layout_geometry_editor("Zone", &zone.geometry);

    if (ImGui::Button("Add zone")) {
        const int next_index = static_cast<int>(ui_state->layout_artifact.layout.zones.size());
        ui_state->layout_artifact.layout.zones.push_back(
            make_default_zone(ui_state->layout_artifact.layout.outer_geometry, next_index));
        ui_state->selected_zone_index = next_index;
    }
    ImGui::SameLine();
    if (ImGui::Button("Remove zone") && !ui_state->layout_artifact.layout.zones.empty()) {
        ui_state->layout_artifact.layout.zones.erase(
            ui_state->layout_artifact.layout.zones.begin() + ui_state->selected_zone_index);
        ui_state->selected_zone_index = std::max(0, ui_state->selected_zone_index - 1);
    }
}

} // namespace

void clear_spatial_layout_texture(SpatialLayoutUiState* ui_state)
{
    if (ui_state == nullptr) {
        return;
    }
    orange::preview::clear_texture(
        &ui_state->captured_texture,
        &ui_state->captured_texture_width,
        &ui_state->captured_texture_height);
    clear_detected_experimental_area_circle(ui_state);
    ui_state->captured_rgba.clear();
    ui_state->has_capture = false;
    ui_state->captured_camera_serial.clear();
    ui_state->captured_source_array_role = "images_full";
    ui_state->captured_capture_mode = "single_camera_direct_still";
    ui_state->pending_full_res_snapshot_request_id = 0;
    ui_state->pending_full_res_snapshot_camera_serial.clear();
    ui_state->captured_canvas_view.fit_requested = true;
    ui_state->captured_canvas_view.last_image_width = 0;
    ui_state->captured_canvas_view.last_image_height = 0;
}

void render_spatial_layout_window(
    SpatialLayoutUiState* ui_state,
    CameraControl* camera_control,
    CameraEmergent* ecams,
    CameraParams* cameras_params,
    CameraEachSelect* cameras_select,
    int num_cameras,
    bool other_calibration_tool_busy,
    const std::string& artifact_root_dir,
    const GLuint* live_preview_texture_ids,
    const uint64_t* live_preview_uploaded_serials,
    SpatialSnapshotWorker* const* spatial_snapshot_workers)
{
    if (ui_state == nullptr) {
        return;
    }

    initialize_spatial_layout_defaults(ui_state);

    if (!ui_state->show_window) {
        return;
    }

    if (!ImGui::Begin("Spatial Layout / Experimental Area Registration", &ui_state->show_window)) {
        ImGui::End();
        return;
    }

    if (num_cameras <= 0 ||
        cameras_params == nullptr ||
        cameras_select == nullptr ||
        ecams == nullptr ||
        camera_control == nullptr ||
        !camera_control->open) {
        ImGui::TextDisabled("Open cameras before using spatial layout view registration.");
        ImGui::End();
        return;
    }

    ui_state->selected_camera = std::clamp(ui_state->selected_camera, 0, std::max(0, num_cameras - 1));

    std::vector<std::string> camera_labels_storage;
    std::vector<const char*> camera_labels;
    camera_labels_storage.reserve(num_cameras);
    camera_labels.reserve(num_cameras);
    for (int i = 0; i < num_cameras; ++i) {
        std::ostringstream label;
        label << i << ": " << cameras_params[i].camera_serial;
        camera_labels_storage.push_back(label.str());
    }
    for (const std::string& label : camera_labels_storage) {
        camera_labels.push_back(label.c_str());
    }

    ImGui::Combo("Camera", &ui_state->selected_camera, camera_labels.data(), num_cameras);
    if (ui_state->configured_camera_index != ui_state->selected_camera) {
        ui_state->configured_camera_index = ui_state->selected_camera;
        clear_spatial_layout_texture(ui_state);
        ui_state->preview_status = "Capture a frame to preview the selected camera.";
        ui_state->preview_error.clear();
        clear_detected_experimental_area_circle(ui_state);
    }

    CameraParams& selected_camera = cameras_params[ui_state->selected_camera];
    SpatialSnapshotWorker* selected_snapshot_worker =
        spatial_snapshot_workers ? spatial_snapshot_workers[ui_state->selected_camera] : nullptr;
    if (selected_snapshot_worker) {
        SpatialSnapshotResult snapshot_result;
        if (selected_snapshot_worker->PopCompletedSnapshot(&snapshot_result)) {
            std::string snapshot_error;
            if (!apply_full_resolution_stream_snapshot(
                    ui_state,
                    snapshot_result,
                    &snapshot_error)) {
                ui_state->pending_full_res_snapshot_request_id = 0;
                ui_state->pending_full_res_snapshot_camera_serial.clear();
                ui_state->preview_error = snapshot_error;
                ui_state->preview_status = "Full-resolution stream snapshot failed.";
            }
        }
    }
    ImGui::Text("Current settings: focus=%u iris=%u exposure=%u gain=%u size=%ux%u",
                selected_camera.focus,
                selected_camera.iris,
                selected_camera.exposure,
                selected_camera.gain,
                selected_camera.width,
                selected_camera.height);

    const bool can_capture =
        !camera_control->subscribe &&
        !camera_control->record_video &&
        !other_calibration_tool_busy;
    const bool can_capture_live_preview =
        camera_control->subscribe &&
        !camera_control->record_video &&
        !other_calibration_tool_busy &&
        cameras_select[ui_state->selected_camera].stream_on &&
        live_preview_texture_ids != nullptr &&
        live_preview_texture_ids[ui_state->selected_camera] != 0 &&
        live_preview_uploaded_serials != nullptr &&
        live_preview_uploaded_serials[ui_state->selected_camera] !=
            std::numeric_limits<uint64_t>::max();
    const bool full_res_request_pending_for_selected =
        ui_state->pending_full_res_snapshot_request_id != 0 &&
        ui_state->pending_full_res_snapshot_camera_serial == selected_camera.camera_serial;
    const bool can_capture_full_resolution_stream_snapshot =
        camera_control->subscribe &&
        !camera_control->record_video &&
        !other_calibration_tool_busy &&
        cameras_select[ui_state->selected_camera].stream_on &&
        selected_snapshot_worker != nullptr &&
        !full_res_request_pending_for_selected;

    if (!can_capture) {
        ImGui::TextDisabled("Direct still capture requires streaming, recording, and other calibration tools to be stopped.");
    }

    if (ImGui::Button("Capture Frame") && can_capture) {
        std::string capture_error;
        if (!capture_single_camera_frame(ui_state, ecams, cameras_params, &capture_error)) {
            ui_state->preview_error = capture_error;
            ui_state->preview_status = "Capture failed.";
        } else {
            reset_registration_from_frame(ui_state);
        }
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(!can_capture_live_preview);
    if (ImGui::Button("Capture Live Stream Snapshot")) {
        std::string capture_error;
        if (!capture_live_stream_preview_texture(
                ui_state,
                selected_camera,
                cameras_select[ui_state->selected_camera],
                live_preview_texture_ids ? live_preview_texture_ids[ui_state->selected_camera] : 0,
                &capture_error)) {
            ui_state->preview_error = capture_error;
            ui_state->preview_status = "Live stream snapshot failed.";
        }
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(!can_capture_full_resolution_stream_snapshot);
    if (ImGui::Button("Capture Full-Resolution Stream Snapshot")) {
        std::string request_error;
        uint64_t request_id = 0;
        std::ostringstream operation_id;
        operation_id << "spatial_layout_full_res_" << selected_camera.camera_serial;
        if (!selected_snapshot_worker ||
            !selected_snapshot_worker->RequestSnapshot(
                operation_id.str(),
                &request_id,
                &request_error)) {
            ui_state->preview_error = request_error.empty()
                                          ? "Failed to request full-resolution stream snapshot."
                                          : request_error;
            ui_state->preview_status = "Full-resolution stream snapshot request failed.";
        } else {
            ui_state->pending_full_res_snapshot_request_id = request_id;
            ui_state->pending_full_res_snapshot_camera_serial = selected_camera.camera_serial;
            ui_state->preview_error.clear();
            ui_state->preview_status =
                "Waiting for full-resolution stream snapshot from " +
                selected_camera.camera_serial + ".";
        }
    }
    ImGui::EndDisabled();
    if (!camera_control->subscribe) {
        ImGui::TextDisabled("Stream snapshots use the active GUI stream, useful for TTL-lit rigs.");
    } else if (!can_capture_live_preview) {
        ImGui::TextDisabled("Wait for the selected camera's live preview texture before taking a live preview snapshot.");
    }
    if (full_res_request_pending_for_selected) {
        ImGui::TextDisabled(
            "Full-resolution snapshot request %llu is waiting for the next acquisition frame.",
            static_cast<unsigned long long>(ui_state->pending_full_res_snapshot_request_id));
    } else if (camera_control->subscribe && !can_capture_full_resolution_stream_snapshot) {
        ImGui::TextDisabled("Full-resolution stream snapshot worker is not available for the selected camera.");
    }

    const bool citrus_template_matches_selected_camera =
        !ui_state->citrus_template.available ||
        ui_state->citrus_template.source_camera_id.empty() ||
        ui_state->citrus_template.source_camera_id == selected_camera.camera_serial;

    ImGui::SeparatorText("Citrus Single-Circle Import");
    if (ImGui::Button("Import Citrus Arena Config...")) {
        IGFD::FileDialogConfig config;
        config.path = default_citrus_rigs_root();
        config.countSelectionMax = 1;
        ImGuiFileDialog::Instance()->OpenDialog(
            kLoadCitrusArenaConfigDialogId,
            "Choose Citrus Arena Config JSON",
            ".json",
            config);
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(
        !ui_state->citrus_template.available ||
        !ui_state->citrus_template.has_canvas_to_camera_homography ||
        !citrus_template_matches_selected_camera);
    if (ImGui::Button("Seed From Citrus Global Homography")) {
        std::string seed_error;
        if (!seed_registration_from_citrus_homography(ui_state, &seed_error)) {
            ui_state->citrus_import_error = seed_error;
        } else {
            ui_state->citrus_import_error.clear();
            if (ui_state->citrus_import_status.empty()) {
                ui_state->citrus_import_status =
                    "Seeded registration from Citrus global-canvas homography projection.";
            } else {
                ui_state->citrus_import_status += " Applied to registration.";
            }
        }
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Clear Citrus Import State")) {
        clear_citrus_template_import(ui_state);
    }

    if (!ui_state->citrus_template.available) {
        ImGui::TextDisabled("Import a Citrus arena config to seed the single circular experimental area.");
    } else {
        ImGui::TextWrapped(
            "Imported: rig=%s canvas=%s arena=%s config=%s camera=%s",
            ui_state->citrus_template.source_rig_name.c_str(),
            ui_state->citrus_template.source_canvas_name.c_str(),
            ui_state->citrus_template.source_arena_name.c_str(),
            ui_state->citrus_template.source_config_name.c_str(),
            ui_state->citrus_template.source_camera_id.c_str());
        ImGui::TextWrapped(
            "Citrus circle: arena-relative center=(%.2f, %.2f) r=%.2f canvas px",
            ui_state->citrus_template.experimental_area_center_x_px,
            ui_state->citrus_template.experimental_area_center_y_px,
            ui_state->citrus_template.experimental_area_radius_px);
        if (ui_state->citrus_template.has_arena_canvas_region) {
            const Point2d origin = citrus_arena_origin_canvas_px(ui_state->citrus_template);
            const Point2d global_center = citrus_arena_relative_to_canvas_px(
                ui_state->citrus_template,
                make_point(
                    ui_state->citrus_template.experimental_area_center_x_px,
                    ui_state->citrus_template.experimental_area_center_y_px));
            ImGui::TextWrapped(
                "Citrus arena canvas region: center=(%.2f, %.2f) size=(%.2f, %.2f) origin=(%.2f, %.2f)",
                ui_state->citrus_template.arena_center_x_px,
                ui_state->citrus_template.arena_center_y_px,
                ui_state->citrus_template.arena_width_px,
                ui_state->citrus_template.arena_height_px,
                origin.x,
                origin.y);
            ImGui::TextWrapped(
                "Citrus current experimental center in global canvas: (%.2f, %.2f)",
                global_center.x,
                global_center.y);
            if (ui_state->has_citrus_projected_circle &&
                ui_state->citrus_projected_circle_geometry.type == RuntimeGeometryType::kCircle) {
                ImGui::TextWrapped(
                    "Citrus current experimental center in camera px: (%.2f, %.2f), r=%.2f",
                    ui_state->citrus_projected_circle_geometry.circle.cx,
                    ui_state->citrus_projected_circle_geometry.circle.cy,
                    ui_state->citrus_projected_circle_geometry.circle.r);
            }
        } else {
            ImGui::TextColored(
                ImVec4(1.0f, 0.75f, 0.35f, 1.0f),
                "Citrus arena canvas region fields are missing; arena-relative points cannot be mapped to global canvas correctly.");
        }
        if (ui_state->citrus_template.has_radius_mm) {
            ImGui::Text("Radius: %.3f mm", ui_state->citrus_template.experimental_area_radius_mm);
        }
        if (ui_state->citrus_template.has_pixels_per_mm_projector) {
            ImGui::Text("Projector scale: %.4f px/mm", ui_state->citrus_template.pixels_per_mm_projector);
        }
        ImGui::TextDisabled(
            "%s",
            ui_state->citrus_template.has_canvas_to_camera_homography
                ? "Canvas-to-camera homography loaded."
                : "No canvas-to-camera homography sidecar found.");
        if (!citrus_template_matches_selected_camera) {
            ImGui::TextColored(
                ImVec4(1.0f, 0.75f, 0.35f, 1.0f),
                "Imported config targets camera %s, but the selected Orange camera is %s.",
                ui_state->citrus_template.source_camera_id.c_str(),
                selected_camera.camera_serial.c_str());
        }
        if (ui_state->has_detected_experimental_area_circle &&
            ui_state->detected_experimental_area_geometry.type == RuntimeGeometryType::kCircle &&
            ui_state->citrus_template.has_camera_to_canvas_homography &&
            ui_state->citrus_template.has_arena_canvas_region) {
            const Point2d detected_camera_center = make_point(
                ui_state->detected_experimental_area_geometry.circle.cx,
                ui_state->detected_experimental_area_geometry.circle.cy);
            Point2d detected_canvas_center{};
            if (transform_point_projective(
                    ui_state->citrus_template.camera_to_canvas_homography,
                    detected_camera_center,
                    &detected_canvas_center)) {
                const Point2d detected_arena_relative =
                    citrus_canvas_to_arena_relative_px(
                        ui_state->citrus_template,
                        detected_canvas_center);
                const double delta_x =
                    detected_arena_relative.x -
                    ui_state->citrus_template.experimental_area_center_x_px;
                const double delta_y =
                    detected_arena_relative.y -
                    ui_state->citrus_template.experimental_area_center_y_px;
                ImGui::TextWrapped(
                    "Detected top-rim center maps to Citrus global canvas=(%.2f, %.2f), arena-relative=(%.2f, %.2f), delta=(%+.2f, %+.2f) px.",
                    detected_canvas_center.x,
                    detected_canvas_center.y,
                    detected_arena_relative.x,
                    detected_arena_relative.y,
                    delta_x,
                    delta_y);
            }
        }
    }

    if (ImGui::Button("Reset registration from frame") && ui_state->has_capture) {
        reset_registration_from_frame(ui_state);
    }
    ImGui::SameLine();
    if (ImGui::Button("Detect Experimental Area Circle") && ui_state->has_capture) {
        std::string detect_error;
        if (!detect_experimental_area_circle_from_capture(ui_state, &detect_error)) {
            ui_state->detection_error = detect_error;
            ui_state->detection_status = "Experimental-area detection failed.";
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Use Detection For Registration") && ui_state->has_detected_experimental_area_circle) {
        std::string seed_error;
        if (!seed_registration_from_detected_experimental_area_circle(ui_state, &seed_error)) {
            ui_state->detection_error = seed_error;
        } else {
            ui_state->detection_error.clear();
            if (ui_state->detection_status.empty()) {
                ui_state->detection_status = "Seeded registration from detected experimental area.";
            } else {
                ui_state->detection_status += " Applied to registration.";
            }
        }
    }
    if (ImGui::CollapsingHeader("Hough Circle Tuning", ImGuiTreeNodeFlags_DefaultOpen)) {
        if (ImGui::Button("Reset Hough Defaults")) {
            ui_state->hough_dp = 1.25;
            ui_state->hough_min_dist_fraction = 0.20;
            ui_state->hough_param1 = 120.0;
            ui_state->hough_param2 = 30.0;
            ui_state->hough_min_radius_fraction = 0.18;
            ui_state->hough_max_radius_fraction = 0.49;
            ui_state->hough_radius_adjustment_px = 0.0;
            ui_state->hough_median_blur_ksize = 5;
            ui_state->hough_max_detection_dimension_px = 2048;
            ui_state->hough_fallback_enabled = true;
            ui_state->show_hough_proposal_overlay = true;
            ui_state->show_citrus_corrected_center_overlay = true;
        }
        ImGui::Checkbox("Show Hough proposal overlay", &ui_state->show_hough_proposal_overlay);
        ImGui::Checkbox(
            "Show corrected Citrus outline overlay",
            &ui_state->show_citrus_corrected_center_overlay);
        ImGui::InputDouble("Hough dp", &ui_state->hough_dp, 0.05, 0.25, "%.3f");
        ImGui::InputDouble("Hough min distance fraction", &ui_state->hough_min_dist_fraction, 0.01, 0.05, "%.3f");
        ImGui::InputDouble("Hough param1", &ui_state->hough_param1, 5.0, 25.0, "%.1f");
        ImGui::InputDouble("Hough param2", &ui_state->hough_param2, 1.0, 5.0, "%.1f");
        ImGui::InputDouble("Hough min radius fraction", &ui_state->hough_min_radius_fraction, 0.01, 0.05, "%.3f");
        ImGui::InputDouble("Hough max radius fraction", &ui_state->hough_max_radius_fraction, 0.01, 0.05, "%.3f");
        ImGui::InputDouble("Hough radius adjustment px", &ui_state->hough_radius_adjustment_px, 1.0, 10.0, "%.2f");
        ImGui::InputInt("Hough median blur kernel", &ui_state->hough_median_blur_ksize, 2, 4);
        ImGui::InputInt("Hough max detection dimension px", &ui_state->hough_max_detection_dimension_px, 128, 512);
        ImGui::Checkbox("Hough fallback pass", &ui_state->hough_fallback_enabled);

        if (ui_state->has_detected_experimental_area_circle &&
            ui_state->detected_experimental_area_geometry.type == RuntimeGeometryType::kCircle) {
            bool edited_detection = false;
            edited_detection |= ImGui::InputDouble(
                "Detected circle cx",
                &ui_state->detected_experimental_area_geometry.circle.cx,
                0.5,
                5.0,
                "%.2f");
            edited_detection |= ImGui::InputDouble(
                "Detected circle cy",
                &ui_state->detected_experimental_area_geometry.circle.cy,
                0.5,
                5.0,
                "%.2f");
            edited_detection |= ImGui::InputDouble(
                "Detected circle r",
                &ui_state->detected_experimental_area_geometry.circle.r,
                0.5,
                5.0,
                "%.2f");
            ui_state->detected_experimental_area_geometry.circle.r =
                std::max(1.0, ui_state->detected_experimental_area_geometry.circle.r);
            if (edited_detection) {
                ui_state->detection_error.clear();
                ui_state->detection_status = "Edited detected experimental-area circle.";
            }
        }
    }

    ImGui::SeparatorText("Detection And Canonical Layout");
    ImGui::InputText("Layout ID", &ui_state->layout_artifact.layout_id);
    ImGui::InputText("Artifact ID", &ui_state->layout_artifact.artifact_id);
    ImGui::InputText("Canvas ID", &ui_state->layout_artifact.context.canvas_id);
    ImGui::InputText("Dish design ID", &ui_state->layout_artifact.context.dish_design_id);

    int coordinate_space = ui_state->layout_artifact.layout.coordinate_space == CoordinateSpace::kLayoutMm ? 0 : 1;
    const char* coordinate_items[] = {"layout_mm", "layout_units"};
    if (ImGui::Combo("Layout coordinate space", &coordinate_space, coordinate_items, IM_ARRAYSIZE(coordinate_items))) {
        ui_state->layout_artifact.layout.coordinate_space =
            coordinate_space == 0 ? CoordinateSpace::kLayoutMm : CoordinateSpace::kLayoutUnits;
    }
    ImGui::InputText("Ordering rule", &ui_state->layout_artifact.provenance.ordering_rule);
    render_layout_geometry_editor("Experimental area", &ui_state->layout_artifact.layout.outer_geometry);

    ImGui::SeparatorText("View Registration");
    render_registration_editor(ui_state);
    ImGui::InputDouble("Experimental area edge margin (px)", &ui_state->edge_margin_px, 0.5, 5.0, "%.2f");
    ui_state->edge_margin_px = std::max(0.0, ui_state->edge_margin_px);

    ImGui::SeparatorText("Zones");
    render_zone_editor(ui_state);

    rebuild_schema_preview(ui_state, &selected_camera);

    ImGui::SeparatorText("Persistence");
    const bool captured_in_full_resolution =
        !ui_state->has_capture ||
        ui_state->captured_source_array_role.empty() ||
        ui_state->captured_source_array_role == "images_full";
    const bool can_save_top_rim_observation =
        ui_state->has_capture &&
        ui_state->dish_mask_runtime.has_geometry &&
        captured_in_full_resolution;
    ImGui::BeginDisabled(!can_save_top_rim_observation);
    if (ImGui::Button("Save Top-Rim Observation")) {
        std::string status;
        std::string error;
        if (!save_dish_top_rim_observation_from_spatial_layout(
                ui_state,
                selected_camera,
                artifact_root_dir,
                &status,
                &error)) {
            ui_state->persistence_error = error;
            ui_state->persistence_status.clear();
        } else {
            ui_state->persistence_status = status;
            ui_state->persistence_error.clear();
        }
    }
    ImGui::EndDisabled();
    if (ui_state->has_capture && !captured_in_full_resolution) {
        ImGui::TextDisabled(
            "Top-rim observations require full-resolution camera coordinates. "
            "This live snapshot is preview/downsample space only.");
    }
    ImGui::SameLine();
    if (ImGui::Button("Save Arena Layout Artifact")) {
        std::string status;
        std::string error;
        if (!save_spatial_layout_artifact(ui_state, selected_camera, artifact_root_dir, &status, &error)) {
            ui_state->persistence_error = error;
            ui_state->persistence_status.clear();
        } else {
            ui_state->persistence_status = status;
            ui_state->persistence_error.clear();
            rebuild_schema_preview(ui_state, &selected_camera);
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Load Arena Layout Artifact...")) {
        IGFD::FileDialogConfig config;
        config.path = artifact_root_dir.empty() ? "." : artifact_root_dir;
        config.countSelectionMax = 1;
        ImGuiFileDialog::Instance()->OpenDialog(
            kLoadSpatialLayoutDialogId,
            "Choose Arena Layout JSON",
            ".json",
            config);
    }
    ImGui::TextDisabled(
        "Top-rim save writes an Orange observation plus Palette and spatial dish-mask exports. Arena save writes %s, %s, %s, and %s under calibrations/artifacts/<artifact_id>.",
        kSpatialLayoutMeasurementFilename,
        kSpatialLayoutManifestFilename,
        kSpatialLayoutArenaLayoutRuntimeFilename,
        kSpatialLayoutDishMaskRuntimeFilename);

    ImGui::SeparatorText("Camera Overlay Preview");
    const char* canvas_edit_items[] = {"registration", "selected_zone"};
    ImGui::Combo("Canvas edit mode", &ui_state->canvas_edit_mode, canvas_edit_items, IM_ARRAYSIZE(canvas_edit_items));
    if (!ui_state->has_capture) {
        ImGui::TextDisabled("Capture a frame to render the resolved camera-pixel overlays.");
    } else {
        const bool canvas_changed = draw_runtime_preview(ui_state);
        if (canvas_changed) {
            rebuild_schema_preview(ui_state, &selected_camera);
        }
        if (ui_state->canvas_edit_mode == 0) {
            ImGui::TextDisabled("Drag cyan to move the experimental area. Drag gold to scale it. Drag pink to rotate the layout.");
        } else {
            ImGui::TextDisabled("Drag green to move the selected zone. Drag gold/orange handles to resize it.");
        }
        ImGui::TextDisabled("Blue outline/triangle: current Citrus global-canvas homography projection. Pink outline/cross: detected experimental-area proposal. Green outline/diamond/line: corrected Citrus outline preserving current Citrus radius with the proposed center. Orange outline: resolved experimental boundary. Yellow outline: valid region after edge margin. Green/cyan outlines: resolved zone overlays.");
    }

    ImGui::Separator();
    ImGui::TextWrapped("%s", ui_state->preview_status.c_str());
    if (!ui_state->preview_error.empty()) {
        ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.35f, 1.0f), "%s", ui_state->preview_error.c_str());
    }
    if (!ui_state->detection_status.empty()) {
        ImGui::TextWrapped("%s", ui_state->detection_status.c_str());
    }
    if (!ui_state->detection_error.empty()) {
        ImGui::TextColored(ImVec4(1.0f, 0.55f, 0.35f, 1.0f), "%s", ui_state->detection_error.c_str());
    }
    if (!ui_state->citrus_import_status.empty()) {
        ImGui::TextWrapped("%s", ui_state->citrus_import_status.c_str());
    }
    if (!ui_state->citrus_import_error.empty()) {
        ImGui::TextColored(ImVec4(0.95f, 0.65f, 0.35f, 1.0f), "%s", ui_state->citrus_import_error.c_str());
    }
    if (!ui_state->persistence_status.empty()) {
        ImGui::TextWrapped("%s", ui_state->persistence_status.c_str());
    }
    if (!ui_state->persistence_error.empty()) {
        ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.35f, 1.0f), "%s", ui_state->persistence_error.c_str());
    }
    ImGui::Text("Preview valid: %s", ui_state->preview_valid ? "yes" : "no");

    if (ImGui::TreeNode("Canonical Layout JSON")) {
        if (ImGui::SmallButton("Copy canonical JSON")) {
            ImGui::SetClipboardText(ui_state->canonical_layout_json.c_str());
        }
        ImGui::BeginChild("SpatialCanonicalJson", ImVec2(0.0f, 180.0f), true, ImGuiWindowFlags_HorizontalScrollbar);
        ImGui::TextUnformatted(ui_state->canonical_layout_json.c_str());
        ImGui::EndChild();
        ImGui::TreePop();
    }

    if (ImGui::TreeNode("Runtime Calibration JSON")) {
        if (ImGui::SmallButton("Copy runtime JSON")) {
            ImGui::SetClipboardText(ui_state->runtime_preview_json.c_str());
        }
        ImGui::BeginChild("SpatialRuntimeJson", ImVec2(0.0f, 220.0f), true, ImGuiWindowFlags_HorizontalScrollbar);
        ImGui::TextUnformatted(ui_state->runtime_preview_json.c_str());
        ImGui::EndChild();
        ImGui::TreePop();
    }

    ImGui::End();

    if (ImGuiFileDialog::Instance()->Display(kLoadSpatialLayoutDialogId)) {
        if (ImGuiFileDialog::Instance()->IsOk()) {
            std::string status;
            std::string error;
            if (!load_spatial_layout_artifact(
                    ui_state,
                    ImGuiFileDialog::Instance()->GetFilePathName(),
                    &status,
                    &error)) {
                ui_state->persistence_error = error;
                ui_state->persistence_status.clear();
            } else {
                ui_state->persistence_status = status;
                ui_state->persistence_error.clear();
                rebuild_schema_preview(ui_state, &selected_camera);
            }
        }
        ImGuiFileDialog::Instance()->Close();
    }

    if (ImGuiFileDialog::Instance()->Display(kLoadCitrusArenaConfigDialogId)) {
        if (ImGuiFileDialog::Instance()->IsOk()) {
            std::string status;
            std::string error;
            if (!import_citrus_single_circle_template(
                    ui_state,
                    selected_camera,
                    ImGuiFileDialog::Instance()->GetFilePathName(),
                    &status,
                    &error)) {
                ui_state->citrus_import_error = error;
                ui_state->citrus_import_status.clear();
            } else {
                ui_state->citrus_import_status = status;
                ui_state->citrus_import_error.clear();
                rebuild_schema_preview(ui_state, &selected_camera);
            }
        }
        ImGuiFileDialog::Instance()->Close();
    }
}
