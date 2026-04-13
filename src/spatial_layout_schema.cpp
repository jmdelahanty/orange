#include "spatial_layout_schema.h"

#include <algorithm>
#include <cmath>
#include <set>
#include <sstream>
#include <utility>

namespace orange::spatial {
namespace {

constexpr double kValidationEpsilon = 1e-6;
constexpr double kPi = 3.14159265358979323846;

struct Point2d {
    double x = 0.0;
    double y = 0.0;
};

bool set_error(std::string* error_out, const std::string& message)
{
    if (error_out) {
        *error_out = message;
    }
    return false;
}

bool is_finite(double value)
{
    return std::isfinite(value) != 0;
}

std::string join_path(const std::string& parent, const std::string& child)
{
    if (parent.empty()) {
        return child;
    }
    return parent + "." + child;
}

bool require_object(const nlohmann::json& node, const std::string& path, std::string* error_out)
{
    if (!node.is_object()) {
        return set_error(error_out, path + " must be a JSON object");
    }
    return true;
}

bool parse_required_string(const nlohmann::json& node,
                           const char* key,
                           const std::string& path,
                           std::string* out,
                           std::string* error_out)
{
    if (!node.contains(key) || !node.at(key).is_string()) {
        return set_error(error_out, join_path(path, key) + " must be a string");
    }
    *out = node.at(key).get<std::string>();
    return true;
}

bool parse_optional_string(const nlohmann::json& node,
                           const char* key,
                           std::string* out,
                           std::string* error_out,
                           const std::string& path)
{
    if (!node.contains(key)) {
        out->clear();
        return true;
    }
    if (!node.at(key).is_string()) {
        return set_error(error_out, join_path(path, key) + " must be a string");
    }
    *out = node.at(key).get<std::string>();
    return true;
}

bool parse_required_bool(const nlohmann::json& node,
                         const char* key,
                         const std::string& path,
                         bool* out,
                         std::string* error_out)
{
    if (!node.contains(key) || !node.at(key).is_boolean()) {
        return set_error(error_out, join_path(path, key) + " must be a boolean");
    }
    *out = node.at(key).get<bool>();
    return true;
}

bool parse_required_int(const nlohmann::json& node,
                        const char* key,
                        const std::string& path,
                        int* out,
                        std::string* error_out)
{
    if (!node.contains(key) || !node.at(key).is_number_integer()) {
        return set_error(error_out, join_path(path, key) + " must be an integer");
    }
    *out = node.at(key).get<int>();
    return true;
}

bool parse_required_number(const nlohmann::json& node,
                           const char* key,
                           const std::string& path,
                           double* out,
                           std::string* error_out)
{
    if (!node.contains(key) || !node.at(key).is_number()) {
        return set_error(error_out, join_path(path, key) + " must be a number");
    }
    *out = node.at(key).get<double>();
    return true;
}

bool parse_required_matrix9(const nlohmann::json& node,
                            const char* key,
                            const std::string& path,
                            std::array<double, 9>* out,
                            std::string* error_out)
{
    if (!node.contains(key) || !node.at(key).is_array()) {
        return set_error(error_out, join_path(path, key) + " must be an array with 9 numbers");
    }
    const nlohmann::json& array = node.at(key);
    if (array.size() != 9) {
        return set_error(error_out, join_path(path, key) + " must contain exactly 9 numbers");
    }
    for (size_t idx = 0; idx < 9; ++idx) {
        if (!array[idx].is_number()) {
            return set_error(error_out, join_path(path, key) + "[" + std::to_string(idx) + "] must be a number");
        }
        (*out)[idx] = array[idx].get<double>();
    }
    return true;
}

bool parse_optional_string_array(const nlohmann::json& node,
                                 const char* key,
                                 bool* found,
                                 std::vector<std::string>* out,
                                 std::string* error_out,
                                 const std::string& path)
{
    if (found) {
        *found = false;
    }
    out->clear();
    if (!node.contains(key)) {
        return true;
    }
    if (found) {
        *found = true;
    }
    if (!node.at(key).is_array()) {
        return set_error(error_out, join_path(path, key) + " must be an array of strings");
    }
    for (size_t idx = 0; idx < node.at(key).size(); ++idx) {
        const nlohmann::json& item = node.at(key)[idx];
        if (!item.is_string()) {
            return set_error(error_out, join_path(path, key) + "[" + std::to_string(idx) + "] must be a string");
        }
        out->push_back(item.get<std::string>());
    }
    return true;
}

bool validate_circle(const CircleGeometry& value, const std::string& path, std::string* error_out)
{
    if (!is_finite(value.cx) || !is_finite(value.cy) || !is_finite(value.r)) {
        return set_error(error_out, path + " has non-finite circle values");
    }
    if (value.r <= 0.0) {
        return set_error(error_out, path + ".r must be > 0");
    }
    return true;
}

bool validate_rectangle(const RectangleGeometry& value, const std::string& path, std::string* error_out)
{
    if (!is_finite(value.x) || !is_finite(value.y) || !is_finite(value.width) || !is_finite(value.height)) {
        return set_error(error_out, path + " has non-finite rectangle values");
    }
    if (value.width <= 0.0) {
        return set_error(error_out, path + ".width must be > 0");
    }
    if (value.height <= 0.0) {
        return set_error(error_out, path + ".height must be > 0");
    }
    return true;
}

bool validate_oriented_rectangle(const OrientedRectangleGeometry& value,
                                 const std::string& path,
                                 std::string* error_out)
{
    if (!is_finite(value.cx) || !is_finite(value.cy) || !is_finite(value.width) ||
        !is_finite(value.height) || !is_finite(value.rotation_deg_clockwise)) {
        return set_error(error_out, path + " has non-finite oriented rectangle values");
    }
    if (value.width <= 0.0) {
        return set_error(error_out, path + ".width must be > 0");
    }
    if (value.height <= 0.0) {
        return set_error(error_out, path + ".height must be > 0");
    }
    return true;
}

std::array<Point2d, 4> rectangle_corners(const RectangleGeometry& rect)
{
    return {
        Point2d{rect.x, rect.y},
        Point2d{rect.x + rect.width, rect.y},
        Point2d{rect.x + rect.width, rect.y + rect.height},
        Point2d{rect.x, rect.y + rect.height}
    };
}

std::array<Point2d, 4> oriented_rectangle_corners(const OrientedRectangleGeometry& rect)
{
    const double half_w = rect.width * 0.5;
    const double half_h = rect.height * 0.5;
    const double theta = rect.rotation_deg_clockwise * kPi / 180.0;
    const double c = std::cos(theta);
    const double s = std::sin(theta);
    const std::array<Point2d, 4> local = {
        Point2d{-half_w, -half_h},
        Point2d{ half_w, -half_h},
        Point2d{ half_w,  half_h},
        Point2d{-half_w,  half_h}
    };

    std::array<Point2d, 4> out{};
    for (size_t idx = 0; idx < local.size(); ++idx) {
        out[idx].x = rect.cx + (c * local[idx].x) - (s * local[idx].y);
        out[idx].y = rect.cy + (s * local[idx].x) + (c * local[idx].y);
    }
    return out;
}

bool point_in_circle(const Point2d& point, const CircleGeometry& circle)
{
    const double dx = point.x - circle.cx;
    const double dy = point.y - circle.cy;
    return (dx * dx) + (dy * dy) <= ((circle.r + kValidationEpsilon) * (circle.r + kValidationEpsilon));
}

bool point_in_rectangle(const Point2d& point, const RectangleGeometry& rect)
{
    return point.x >= rect.x - kValidationEpsilon &&
           point.x <= rect.x + rect.width + kValidationEpsilon &&
           point.y >= rect.y - kValidationEpsilon &&
           point.y <= rect.y + rect.height + kValidationEpsilon;
}

bool point_in_oriented_rectangle(const Point2d& point, const OrientedRectangleGeometry& rect)
{
    const double theta = rect.rotation_deg_clockwise * kPi / 180.0;
    const double c = std::cos(theta);
    const double s = std::sin(theta);
    const double vx = point.x - rect.cx;
    const double vy = point.y - rect.cy;
    const double local_x = (c * vx) + (s * vy);
    const double local_y = (-s * vx) + (c * vy);
    return std::abs(local_x) <= rect.width * 0.5 + kValidationEpsilon &&
           std::abs(local_y) <= rect.height * 0.5 + kValidationEpsilon;
}

bool circle_inside_circle(const CircleGeometry& inner, const CircleGeometry& outer)
{
    const double dx = inner.cx - outer.cx;
    const double dy = inner.cy - outer.cy;
    const double center_distance = std::sqrt((dx * dx) + (dy * dy));
    return center_distance + inner.r <= outer.r + kValidationEpsilon;
}

bool rectangle_inside_circle(const RectangleGeometry& inner, const CircleGeometry& outer)
{
    const auto corners = rectangle_corners(inner);
    return std::all_of(corners.begin(), corners.end(), [&](const Point2d& point) {
        return point_in_circle(point, outer);
    });
}

bool circle_inside_rectangle(const CircleGeometry& inner, const RectangleGeometry& outer)
{
    return inner.cx - inner.r >= outer.x - kValidationEpsilon &&
           inner.cx + inner.r <= outer.x + outer.width + kValidationEpsilon &&
           inner.cy - inner.r >= outer.y - kValidationEpsilon &&
           inner.cy + inner.r <= outer.y + outer.height + kValidationEpsilon;
}

bool rectangle_inside_rectangle(const RectangleGeometry& inner, const RectangleGeometry& outer)
{
    const auto corners = rectangle_corners(inner);
    return std::all_of(corners.begin(), corners.end(), [&](const Point2d& point) {
        return point_in_rectangle(point, outer);
    });
}

bool oriented_rectangle_inside_oriented_rectangle(const OrientedRectangleGeometry& inner,
                                                  const OrientedRectangleGeometry& outer)
{
    const auto corners = oriented_rectangle_corners(inner);
    return std::all_of(corners.begin(), corners.end(), [&](const Point2d& point) {
        return point_in_oriented_rectangle(point, outer);
    });
}

bool runtime_geometry_inside_runtime_geometry(const RuntimeGeometry& inner,
                                              const RuntimeGeometry& outer)
{
    if (inner.type != outer.type) {
        return false;
    }
    if (inner.type == RuntimeGeometryType::kCircle) {
        return circle_inside_circle(inner.circle, outer.circle);
    }
    return oriented_rectangle_inside_oriented_rectangle(inner.oriented_rectangle, outer.oriented_rectangle);
}

bool layout_geometry_inside_layout_geometry(const LayoutGeometry& inner,
                                            const LayoutGeometry& outer)
{
    if (outer.type == LayoutGeometryType::kCircle) {
        if (inner.type == LayoutGeometryType::kCircle) {
            return circle_inside_circle(inner.circle, outer.circle);
        }
        return rectangle_inside_circle(inner.rectangle, outer.circle);
    }

    if (inner.type == LayoutGeometryType::kCircle) {
        return circle_inside_rectangle(inner.circle, outer.rectangle);
    }
    return rectangle_inside_rectangle(inner.rectangle, outer.rectangle);
}

nlohmann::json matrix_to_json(const std::array<double, 9>& matrix)
{
    nlohmann::json out = nlohmann::json::array();
    for (double value : matrix) {
        out.push_back(value);
    }
    return out;
}

template <typename EnumT>
bool parse_enum_string(const std::string& value,
                       EnumT* out,
                       const std::vector<std::pair<const char*, EnumT>>& mapping,
                       const std::string& type_name,
                       std::string* error_out)
{
    for (const auto& entry : mapping) {
        if (value == entry.first) {
            *out = entry.second;
            return true;
        }
    }
    return set_error(error_out, "Unsupported " + type_name + ": " + value);
}

template <typename EnumT>
const char* enum_to_string(EnumT value, const std::vector<std::pair<const char*, EnumT>>& mapping)
{
    for (const auto& entry : mapping) {
        if (entry.second == value) {
            return entry.first;
        }
    }
    return "unknown";
}

const std::vector<std::pair<const char*, CoordinateSpace>>& coordinate_space_mapping()
{
    static const std::vector<std::pair<const char*, CoordinateSpace>> mapping = {
        {"camera_native_pixels", CoordinateSpace::kCameraNativePixels},
        {"layout_mm", CoordinateSpace::kLayoutMm},
        {"layout_units", CoordinateSpace::kLayoutUnits},
    };
    return mapping;
}

const std::vector<std::pair<const char*, RegistrationType>>& registration_type_mapping()
{
    static const std::vector<std::pair<const char*, RegistrationType>> mapping = {
        {"identity", RegistrationType::kIdentity},
        {"translation", RegistrationType::kTranslation},
        {"similarity", RegistrationType::kSimilarity},
    };
    return mapping;
}

const std::vector<std::pair<const char*, RegistrationSource>>& registration_source_mapping()
{
    static const std::vector<std::pair<const char*, RegistrationSource>> mapping = {
        {"identity", RegistrationSource::kIdentity},
        {"manual", RegistrationSource::kManual},
        {"manual_fit", RegistrationSource::kManualFit},
        {"detected_fit", RegistrationSource::kDetectedFit},
        {"imported", RegistrationSource::kImported},
    };
    return mapping;
}

const std::vector<std::pair<const char*, ObservationSource>>& observation_source_mapping()
{
    static const std::vector<std::pair<const char*, ObservationSource>> mapping = {
        {"manual", ObservationSource::kManual},
        {"manual_fit", ObservationSource::kManualFit},
        {"detected_fit", ObservationSource::kDetectedFit},
        {"imported", ObservationSource::kImported},
    };
    return mapping;
}

const std::vector<std::pair<const char*, SourceImageKind>>& source_image_kind_mapping()
{
    static const std::vector<std::pair<const char*, SourceImageKind>> mapping = {
        {"empty_dish_frame", SourceImageKind::kEmptyDishFrame},
        {"calibration_capture", SourceImageKind::kCalibrationCapture},
        {"synthetic_template", SourceImageKind::kSyntheticTemplate},
    };
    return mapping;
}

const std::vector<std::pair<const char*, ArenaLayoutProvenanceSource>>& arena_layout_source_mapping()
{
    static const std::vector<std::pair<const char*, ArenaLayoutProvenanceSource>> mapping = {
        {"manual_template", ArenaLayoutProvenanceSource::kManualTemplate},
        {"imported_template", ArenaLayoutProvenanceSource::kImportedTemplate},
    };
    return mapping;
}

const std::vector<std::pair<const char*, OrientationStatus>>& orientation_status_mapping()
{
    static const std::vector<std::pair<const char*, OrientationStatus>> mapping = {
        {"trusted", OrientationStatus::kTrusted},
        {"manual_confirmed", OrientationStatus::kManualConfirmed},
        {"ambiguous", OrientationStatus::kAmbiguous},
        {"unknown", OrientationStatus::kUnknown},
    };
    return mapping;
}

const std::vector<std::pair<const char*, VisibilityStatus>>& visibility_status_mapping()
{
    static const std::vector<std::pair<const char*, VisibilityStatus>> mapping = {
        {"full", VisibilityStatus::kFull},
        {"partial", VisibilityStatus::kPartial},
        {"occluded", VisibilityStatus::kOccluded},
    };
    return mapping;
}

bool parse_circle_geometry_json(const nlohmann::json& node,
                                CircleGeometry* out,
                                const std::string& path,
                                std::string* error_out)
{
    if (!require_object(node, path, error_out)) {
        return false;
    }
    if (!parse_required_number(node, "cx", path, &out->cx, error_out) ||
        !parse_required_number(node, "cy", path, &out->cy, error_out) ||
        !parse_required_number(node, "r", path, &out->r, error_out)) {
        return false;
    }
    return validate_circle(*out, path, error_out);
}

bool parse_rectangle_geometry_json(const nlohmann::json& node,
                                   RectangleGeometry* out,
                                   const std::string& path,
                                   std::string* error_out)
{
    if (!require_object(node, path, error_out)) {
        return false;
    }
    if (!parse_required_number(node, "x", path, &out->x, error_out) ||
        !parse_required_number(node, "y", path, &out->y, error_out) ||
        !parse_required_number(node, "width", path, &out->width, error_out) ||
        !parse_required_number(node, "height", path, &out->height, error_out)) {
        return false;
    }
    return validate_rectangle(*out, path, error_out);
}

bool parse_oriented_rectangle_geometry_json(const nlohmann::json& node,
                                            OrientedRectangleGeometry* out,
                                            const std::string& path,
                                            std::string* error_out)
{
    if (!require_object(node, path, error_out)) {
        return false;
    }
    if (!parse_required_number(node, "cx", path, &out->cx, error_out) ||
        !parse_required_number(node, "cy", path, &out->cy, error_out) ||
        !parse_required_number(node, "width", path, &out->width, error_out) ||
        !parse_required_number(node, "height", path, &out->height, error_out) ||
        !parse_required_number(node, "rotation_deg_clockwise", path, &out->rotation_deg_clockwise, error_out)) {
        return false;
    }
    return validate_oriented_rectangle(*out, path, error_out);
}

} // namespace

const char* coordinate_space_to_string(CoordinateSpace value)
{
    return enum_to_string(value, coordinate_space_mapping());
}

bool coordinate_space_from_string(const std::string& value, CoordinateSpace* out, std::string* error_out)
{
    return parse_enum_string(value, out, coordinate_space_mapping(), "coordinate_space", error_out);
}

const char* layout_geometry_type_to_string(LayoutGeometryType value)
{
    switch (value) {
        case LayoutGeometryType::kCircle:
            return "circle";
        case LayoutGeometryType::kRectangle:
            return "rectangle";
    }
    return "unknown";
}

const char* runtime_geometry_type_to_string(RuntimeGeometryType value)
{
    switch (value) {
        case RuntimeGeometryType::kCircle:
            return "circle";
        case RuntimeGeometryType::kOrientedRectangle:
            return "oriented_rectangle";
    }
    return "unknown";
}

const char* registration_type_to_string(RegistrationType value)
{
    return enum_to_string(value, registration_type_mapping());
}

bool registration_type_from_string(const std::string& value, RegistrationType* out, std::string* error_out)
{
    return parse_enum_string(value, out, registration_type_mapping(), "registration type", error_out);
}

const char* registration_source_to_string(RegistrationSource value)
{
    return enum_to_string(value, registration_source_mapping());
}

bool registration_source_from_string(const std::string& value, RegistrationSource* out, std::string* error_out)
{
    return parse_enum_string(value, out, registration_source_mapping(), "registration source", error_out);
}

const char* observation_source_to_string(ObservationSource value)
{
    return enum_to_string(value, observation_source_mapping());
}

bool observation_source_from_string(const std::string& value, ObservationSource* out, std::string* error_out)
{
    return parse_enum_string(value, out, observation_source_mapping(), "observation source", error_out);
}

const char* source_image_kind_to_string(SourceImageKind value)
{
    return enum_to_string(value, source_image_kind_mapping());
}

bool source_image_kind_from_string(const std::string& value, SourceImageKind* out, std::string* error_out)
{
    return parse_enum_string(value, out, source_image_kind_mapping(), "source_image_kind", error_out);
}

const char* arena_layout_provenance_source_to_string(ArenaLayoutProvenanceSource value)
{
    return enum_to_string(value, arena_layout_source_mapping());
}

bool arena_layout_provenance_source_from_string(const std::string& value,
                                                ArenaLayoutProvenanceSource* out,
                                                std::string* error_out)
{
    return parse_enum_string(value, out, arena_layout_source_mapping(), "arena layout provenance source", error_out);
}

const char* orientation_status_to_string(OrientationStatus value)
{
    return enum_to_string(value, orientation_status_mapping());
}

bool orientation_status_from_string(const std::string& value, OrientationStatus* out, std::string* error_out)
{
    return parse_enum_string(value, out, orientation_status_mapping(), "orientation status", error_out);
}

const char* visibility_status_to_string(VisibilityStatus value)
{
    return enum_to_string(value, visibility_status_mapping());
}

bool visibility_status_from_string(const std::string& value, VisibilityStatus* out, std::string* error_out)
{
    return parse_enum_string(value, out, visibility_status_mapping(), "visibility status", error_out);
}

bool validate_calibration_ref(const CalibrationRef& value,
                              const char* expected_schema_id,
                              int expected_schema_version,
                              std::string* error_out)
{
    if (value.artifact_id.empty()) {
        return set_error(error_out, "calibration_ref.artifact_id must be non-empty");
    }
    if (value.artifact_schema_id.empty()) {
        return set_error(error_out, "calibration_ref.artifact_schema_id must be non-empty");
    }
    if (value.artifact_schema_version <= 0) {
        return set_error(error_out, "calibration_ref.artifact_schema_version must be > 0");
    }
    if (value.fingerprint.empty()) {
        return set_error(error_out, "calibration_ref.fingerprint must be non-empty");
    }
    if (expected_schema_id && value.artifact_schema_id != expected_schema_id) {
        return set_error(error_out, "calibration_ref.artifact_schema_id mismatch: expected " +
                                        std::string(expected_schema_id) + ", got " + value.artifact_schema_id);
    }
    if (expected_schema_version > 0 && value.artifact_schema_version != expected_schema_version) {
        return set_error(error_out, "calibration_ref.artifact_schema_version mismatch: expected " +
                                        std::to_string(expected_schema_version) + ", got " +
                                        std::to_string(value.artifact_schema_version));
    }
    return true;
}

bool validate_layout_geometry(const LayoutGeometry& value, std::string* error_out)
{
    if (value.type == LayoutGeometryType::kCircle) {
        return validate_circle(value.circle, "layout_geometry", error_out);
    }
    return validate_rectangle(value.rectangle, "layout_geometry", error_out);
}

bool validate_runtime_geometry(const RuntimeGeometry& value, std::string* error_out)
{
    if (value.type == RuntimeGeometryType::kCircle) {
        return validate_circle(value.circle, "runtime_geometry", error_out);
    }
    return validate_oriented_rectangle(value.oriented_rectangle, "runtime_geometry", error_out);
}

bool validate_dish_mask_geometry(const DishMaskGeometry& value, std::string* error_out)
{
    if (value.coordinate_space != CoordinateSpace::kCameraNativePixels) {
        return set_error(error_out, "dish_mask.geometry.coordinate_space must be camera_native_pixels");
    }
    if (!validate_runtime_geometry(value.outer_geometry, error_out) ||
        !validate_runtime_geometry(value.valid_geometry, error_out)) {
        return false;
    }
    if (value.outer_geometry.type != value.valid_geometry.type) {
        return set_error(error_out, "dish_mask outer_geometry and valid_geometry must have the same type");
    }
    if (!is_finite(value.edge_margin_px) || value.edge_margin_px < 0.0) {
        return set_error(error_out, "dish_mask.edge_margin_px must be >= 0");
    }
    if (!runtime_geometry_inside_runtime_geometry(value.valid_geometry, value.outer_geometry)) {
        return set_error(error_out, "dish_mask.valid_geometry must lie inside dish_mask.outer_geometry");
    }
    return true;
}

bool validate_view_registration(const ViewRegistration& value, std::string* error_out)
{
    if (value.layout_coordinate_space != CoordinateSpace::kLayoutMm &&
        value.layout_coordinate_space != CoordinateSpace::kLayoutUnits) {
        return set_error(error_out, "registration.layout_coordinate_space must be layout_mm or layout_units");
    }
    if (value.fit_point_count < 0) {
        return set_error(error_out, "registration.fit_point_count must be >= 0");
    }
    if (!is_finite(value.residual_px) || value.residual_px < 0.0) {
        return set_error(error_out, "registration.residual_px must be finite and >= 0");
    }
    auto validate_matrix = [&](const std::array<double, 9>& matrix, const char* field) -> bool {
        for (double entry : matrix) {
            if (!is_finite(entry)) {
                return set_error(error_out, std::string("registration.") + field + " contains a non-finite value");
            }
        }
        if (std::abs(matrix[6]) > kValidationEpsilon ||
            std::abs(matrix[7]) > kValidationEpsilon ||
            std::abs(matrix[8] - 1.0) > kValidationEpsilon) {
            return set_error(error_out, std::string("registration.") + field +
                                            " must be an affine 3x3 matrix with bottom row [0,0,1]");
        }
        return true;
    };

    if (!validate_matrix(value.layout_to_camera_matrix, "layout_to_camera_matrix")) {
        return false;
    }
    if (value.has_camera_to_layout_matrix &&
        !validate_matrix(value.camera_to_layout_matrix, "camera_to_layout_matrix")) {
        return false;
    }
    return true;
}

bool validate_dish_mask_artifact(const DishMaskArtifact& value, std::string* error_out)
{
    if (value.artifact_id.empty()) {
        return set_error(error_out, "dish_mask artifact_id must be non-empty");
    }
    if (value.created_utc.empty()) {
        return set_error(error_out, "dish_mask created_utc must be non-empty");
    }
    if (!validate_calibration_ref(value.calibration_ref,
                                  kDishMaskArtifactSchemaId,
                                  kDishMaskArtifactSchemaVersion,
                                  error_out)) {
        return false;
    }
    if (value.calibration_ref.artifact_id != value.artifact_id) {
        return set_error(error_out, "dish_mask calibration_ref.artifact_id must match artifact_id");
    }
    if (value.camera.serial.empty()) {
        return set_error(error_out, "dish_mask.camera.serial must be non-empty");
    }
    if (value.camera.width <= 0 || value.camera.height <= 0) {
        return set_error(error_out, "dish_mask.camera width and height must be > 0");
    }
    if (!validate_dish_mask_geometry(value.geometry, error_out)) {
        return false;
    }
    return true;
}

bool validate_arena_layout_artifact(const ArenaLayoutArtifact& value, std::string* error_out)
{
    if (value.artifact_id.empty()) {
        return set_error(error_out, "arena_layout artifact_id must be non-empty");
    }
    if (value.created_utc.empty()) {
        return set_error(error_out, "arena_layout created_utc must be non-empty");
    }
    if (!validate_calibration_ref(value.calibration_ref,
                                  kArenaLayoutArtifactSchemaId,
                                  kArenaLayoutArtifactSchemaVersion,
                                  error_out)) {
        return false;
    }
    if (value.calibration_ref.artifact_id != value.artifact_id) {
        return set_error(error_out, "arena_layout calibration_ref.artifact_id must match artifact_id");
    }
    if (value.layout_id.empty()) {
        return set_error(error_out, "arena_layout.layout_id must be non-empty");
    }
    if (value.layout.coordinate_space != CoordinateSpace::kLayoutMm &&
        value.layout.coordinate_space != CoordinateSpace::kLayoutUnits) {
        return set_error(error_out, "arena_layout.layout.coordinate_space must be layout_mm or layout_units");
    }
    if (!validate_layout_geometry(value.layout.outer_geometry, error_out)) {
        return false;
    }
    if (value.layout.zones.empty()) {
        return set_error(error_out, "arena_layout.layout.zones must contain at least one zone");
    }

    std::set<std::string> zone_ids;
    std::set<int> zone_indices;
    for (size_t idx = 0; idx < value.layout.zones.size(); ++idx) {
        const ArenaLayoutZone& zone = value.layout.zones[idx];
        if (zone.zone_id.empty()) {
            return set_error(error_out, "arena_layout.layout.zones[" + std::to_string(idx) +
                                            "].zone_id must be non-empty");
        }
        if (!zone_ids.insert(zone.zone_id).second) {
            return set_error(error_out, "arena_layout.layout.zones contains duplicate zone_id: " + zone.zone_id);
        }
        if (zone.has_zone_index && !zone_indices.insert(zone.zone_index).second) {
            return set_error(error_out, "arena_layout.layout.zones contains duplicate zone_index: " +
                                            std::to_string(zone.zone_index));
        }
        if (!validate_layout_geometry(zone.geometry, error_out)) {
            return false;
        }
        if (!layout_geometry_inside_layout_geometry(zone.geometry, value.layout.outer_geometry)) {
            return set_error(error_out, "arena_layout.layout.zones[" + std::to_string(idx) +
                                            "] must lie inside arena_layout.layout.outer_geometry");
        }
    }
    if (value.provenance.ordering_rule.empty()) {
        return set_error(error_out, "arena_layout.provenance.ordering_rule must be non-empty");
    }
    return true;
}

bool validate_dish_mask_runtime(const DishMaskRuntime& value, std::string* error_out)
{
    if (value.schema_version != kDishMaskRuntimeSchemaVersion) {
        return set_error(error_out, "dish_mask.runtime.schema_version mismatch");
    }
    if (value.enabled) {
        if (!value.has_geometry) {
            return set_error(error_out, "dish_mask.runtime.geometry is required when enabled=true");
        }
        if (!validate_dish_mask_geometry(value.geometry, error_out)) {
            return false;
        }
    } else if (value.has_geometry && !validate_dish_mask_geometry(value.geometry, error_out)) {
        return false;
    }
    return true;
}

bool validate_arena_layout_runtime(const ArenaLayoutRuntime& value, std::string* error_out)
{
    if (value.schema_version != kArenaLayoutRuntimeSchemaVersion) {
        return set_error(error_out, "arena_layout.runtime.schema_version mismatch");
    }
    if (value.layout_id.empty()) {
        return set_error(error_out, "arena_layout.runtime.layout_id must be non-empty");
    }
    if (value.coordinate_space != CoordinateSpace::kCameraNativePixels) {
        return set_error(error_out, "arena_layout.runtime.coordinate_space must be camera_native_pixels");
    }
    if (!validate_view_registration(value.registration, error_out)) {
        return false;
    }

    std::set<std::string> zone_ids;
    for (size_t idx = 0; idx < value.zones.size(); ++idx) {
        const ResolvedZoneOverlay& zone = value.zones[idx];
        if (zone.zone_id.empty()) {
            return set_error(error_out, "arena_layout.runtime.zones[" + std::to_string(idx) +
                                            "].zone_id must be non-empty");
        }
        if (!zone_ids.insert(zone.zone_id).second) {
            return set_error(error_out, "arena_layout.runtime.zones contains duplicate zone_id: " + zone.zone_id);
        }
        if (!validate_runtime_geometry(zone.geometry, error_out)) {
            return false;
        }
    }

    if (value.has_visible_zone_ids) {
        std::set<std::string> visible_zone_ids(value.visible_zone_ids.begin(), value.visible_zone_ids.end());
        if (visible_zone_ids.size() != value.visible_zone_ids.size()) {
            return set_error(error_out, "arena_layout.runtime.visible_zone_ids contains duplicates");
        }
        if (visible_zone_ids != zone_ids) {
            return set_error(error_out, "arena_layout.runtime.visible_zone_ids must exactly match runtime.zones[*].zone_id");
        }
    }
    return true;
}

bool validate_camera_spatial_calibration(const CameraSpatialCalibration& value, std::string* error_out)
{
    if (value.has_dish_mask) {
        if (!validate_calibration_ref(value.dish_mask.calibration_ref,
                                      kDishMaskArtifactSchemaId,
                                      kDishMaskArtifactSchemaVersion,
                                      error_out) ||
            !validate_dish_mask_runtime(value.dish_mask.runtime, error_out)) {
            return false;
        }
    }
    if (value.has_arena_layout) {
        if (!validate_calibration_ref(value.arena_layout.calibration_ref,
                                      kArenaLayoutArtifactSchemaId,
                                      kArenaLayoutArtifactSchemaVersion,
                                      error_out) ||
            !validate_arena_layout_runtime(value.arena_layout.runtime, error_out)) {
            return false;
        }
    }
    return true;
}

bool validate_arena_layout_runtime_against_artifact(const ArenaLayoutRuntime& runtime,
                                                    const ArenaLayoutArtifact& artifact,
                                                    std::string* error_out)
{
    if (!validate_arena_layout_runtime(runtime, error_out) ||
        !validate_arena_layout_artifact(artifact, error_out)) {
        return false;
    }
    if (runtime.layout_id != artifact.layout_id) {
        return set_error(error_out, "arena_layout.runtime.layout_id must match arena_layout artifact layout_id");
    }
    std::set<std::string> artifact_zone_ids;
    for (const ArenaLayoutZone& zone : artifact.layout.zones) {
        artifact_zone_ids.insert(zone.zone_id);
    }
    for (const ResolvedZoneOverlay& zone : runtime.zones) {
        if (artifact_zone_ids.count(zone.zone_id) == 0) {
            return set_error(error_out, "arena_layout.runtime zone_id not present in canonical artifact: " + zone.zone_id);
        }
    }
    return true;
}

nlohmann::json calibration_ref_to_json(const CalibrationRef& value)
{
    return {
        {"artifact_id", value.artifact_id},
        {"artifact_schema_id", value.artifact_schema_id},
        {"artifact_schema_version", value.artifact_schema_version},
        {"fingerprint", value.fingerprint}
    };
}

nlohmann::json layout_geometry_to_json(const LayoutGeometry& value)
{
    if (value.type == LayoutGeometryType::kCircle) {
        return {
            {"type", "circle"},
            {"cx", value.circle.cx},
            {"cy", value.circle.cy},
            {"r", value.circle.r}
        };
    }
    return {
        {"type", "rectangle"},
        {"x", value.rectangle.x},
        {"y", value.rectangle.y},
        {"width", value.rectangle.width},
        {"height", value.rectangle.height}
    };
}

nlohmann::json runtime_geometry_to_json(const RuntimeGeometry& value)
{
    if (value.type == RuntimeGeometryType::kCircle) {
        return {
            {"type", "circle"},
            {"cx", value.circle.cx},
            {"cy", value.circle.cy},
            {"r", value.circle.r}
        };
    }
    return {
        {"type", "oriented_rectangle"},
        {"cx", value.oriented_rectangle.cx},
        {"cy", value.oriented_rectangle.cy},
        {"width", value.oriented_rectangle.width},
        {"height", value.oriented_rectangle.height},
        {"rotation_deg_clockwise", value.oriented_rectangle.rotation_deg_clockwise}
    };
}

nlohmann::json dish_mask_geometry_to_json(const DishMaskGeometry& value)
{
    return {
        {"coordinate_space", coordinate_space_to_string(value.coordinate_space)},
        {"outer_geometry", runtime_geometry_to_json(value.outer_geometry)},
        {"valid_geometry", runtime_geometry_to_json(value.valid_geometry)},
        {"edge_margin_px", value.edge_margin_px}
    };
}

nlohmann::json dish_mask_artifact_to_json(const DishMaskArtifact& value)
{
    nlohmann::json out = {
        {"schema_id", kDishMaskArtifactSchemaId},
        {"schema_version", kDishMaskArtifactSchemaVersion},
        {"artifact_id", value.artifact_id},
        {"created_utc", value.created_utc},
        {"calibration_ref", calibration_ref_to_json(value.calibration_ref)},
        {"camera", {
            {"serial", value.camera.serial},
            {"width", value.camera.width},
            {"height", value.camera.height}
        }},
        {"geometry", dish_mask_geometry_to_json(value.geometry)},
        {"provenance", {
            {"source", observation_source_to_string(value.provenance.source)}
        }}
    };
    if (!value.camera.pixel_format.empty()) {
        out["camera"]["pixel_format"] = value.camera.pixel_format;
    }
    if (value.provenance.has_source_image_kind) {
        out["provenance"]["source_image_kind"] = source_image_kind_to_string(value.provenance.source_image_kind);
    }
    if (!value.provenance.notes.empty()) {
        out["provenance"]["notes"] = value.provenance.notes;
    }
    if (!value.context.dish_design_id.empty() || !value.context.canvas_id.empty() || !value.context.shelf_id.empty()) {
        out["context"] = nlohmann::json::object();
        if (!value.context.dish_design_id.empty()) {
            out["context"]["dish_design_id"] = value.context.dish_design_id;
        }
        if (!value.context.canvas_id.empty()) {
            out["context"]["canvas_id"] = value.context.canvas_id;
        }
        if (!value.context.shelf_id.empty()) {
            out["context"]["shelf_id"] = value.context.shelf_id;
        }
    }
    return out;
}

nlohmann::json arena_layout_artifact_to_json(const ArenaLayoutArtifact& value)
{
    nlohmann::json zones = nlohmann::json::array();
    for (const ArenaLayoutZone& zone : value.layout.zones) {
        nlohmann::json zone_json = {
            {"zone_id", zone.zone_id},
            {"geometry", layout_geometry_to_json(zone.geometry)}
        };
        if (zone.has_zone_index) {
            zone_json["zone_index"] = zone.zone_index;
        }
        if (!zone.display_label.empty()) {
            zone_json["display_label"] = zone.display_label;
        }
        zones.push_back(std::move(zone_json));
    }

    nlohmann::json out = {
        {"schema_id", kArenaLayoutArtifactSchemaId},
        {"schema_version", kArenaLayoutArtifactSchemaVersion},
        {"artifact_id", value.artifact_id},
        {"created_utc", value.created_utc},
        {"calibration_ref", calibration_ref_to_json(value.calibration_ref)},
        {"layout_id", value.layout_id},
        {"layout", {
            {"coordinate_space", coordinate_space_to_string(value.layout.coordinate_space)},
            {"outer_geometry", layout_geometry_to_json(value.layout.outer_geometry)},
            {"zones", std::move(zones)}
        }},
        {"provenance", {
            {"source", arena_layout_provenance_source_to_string(value.provenance.source)},
            {"ordering_rule", value.provenance.ordering_rule}
        }}
    };

    if (!value.provenance.notes.empty()) {
        out["provenance"]["notes"] = value.provenance.notes;
    }
    if (!value.context.dish_design_id.empty() || !value.context.canvas_id.empty()) {
        out["context"] = nlohmann::json::object();
        if (!value.context.dish_design_id.empty()) {
            out["context"]["dish_design_id"] = value.context.dish_design_id;
        }
        if (!value.context.canvas_id.empty()) {
            out["context"]["canvas_id"] = value.context.canvas_id;
        }
    }
    return out;
}

nlohmann::json view_registration_to_json(const ViewRegistration& value)
{
    nlohmann::json out = {
        {"type", registration_type_to_string(value.type)},
        {"layout_coordinate_space", coordinate_space_to_string(value.layout_coordinate_space)},
        {"source", registration_source_to_string(value.source)},
        {"layout_to_camera_matrix", matrix_to_json(value.layout_to_camera_matrix)},
        {"fit_point_count", value.fit_point_count},
        {"residual_px", value.residual_px}
    };
    if (value.has_camera_to_layout_matrix) {
        out["camera_to_layout_matrix"] = matrix_to_json(value.camera_to_layout_matrix);
    }
    if (value.has_orientation_status) {
        out["orientation_status"] = orientation_status_to_string(value.orientation_status);
    }
    return out;
}

nlohmann::json dish_mask_runtime_to_json(const DishMaskRuntime& value)
{
    nlohmann::json out = {
        {"schema_version", value.schema_version},
        {"enabled", value.enabled},
        {"source", observation_source_to_string(value.source)}
    };
    if (value.has_geometry) {
        out["geometry"] = dish_mask_geometry_to_json(value.geometry);
    }
    return out;
}

nlohmann::json arena_layout_runtime_to_json(const ArenaLayoutRuntime& value)
{
    nlohmann::json zones = nlohmann::json::array();
    for (const ResolvedZoneOverlay& zone : value.zones) {
        nlohmann::json zone_json = {
            {"zone_id", zone.zone_id},
            {"visibility_status", visibility_status_to_string(zone.visibility_status)},
            {"geometry", runtime_geometry_to_json(zone.geometry)}
        };
        if (zone.has_zone_index) {
            zone_json["zone_index"] = zone.zone_index;
        }
        zones.push_back(std::move(zone_json));
    }

    nlohmann::json out = {
        {"schema_version", value.schema_version},
        {"enabled", value.enabled},
        {"layout_id", value.layout_id},
        {"coordinate_space", coordinate_space_to_string(value.coordinate_space)},
        {"registration", view_registration_to_json(value.registration)},
        {"zones", std::move(zones)}
    };
    if (value.has_visible_zone_ids) {
        out["visible_zone_ids"] = value.visible_zone_ids;
    }
    return out;
}

nlohmann::json camera_spatial_calibration_to_json(const CameraSpatialCalibration& value)
{
    nlohmann::json out = nlohmann::json::object();
    if (value.has_dish_mask) {
        out["dish_mask"] = {
            {"calibration_ref", calibration_ref_to_json(value.dish_mask.calibration_ref)},
            {"runtime", dish_mask_runtime_to_json(value.dish_mask.runtime)}
        };
    }
    if (value.has_arena_layout) {
        out["arena_layout"] = {
            {"calibration_ref", calibration_ref_to_json(value.arena_layout.calibration_ref)},
            {"runtime", arena_layout_runtime_to_json(value.arena_layout.runtime)}
        };
    }
    return out;
}

bool parse_calibration_ref_json(const nlohmann::json& node, CalibrationRef* out, std::string* error_out)
{
    if (!out) {
        return set_error(error_out, "null CalibrationRef destination");
    }
    if (!require_object(node, "calibration_ref", error_out)) {
        return false;
    }
    if (!parse_required_string(node, "artifact_id", "calibration_ref", &out->artifact_id, error_out) ||
        !parse_required_string(node, "artifact_schema_id", "calibration_ref", &out->artifact_schema_id, error_out) ||
        !parse_required_int(node, "artifact_schema_version", "calibration_ref", &out->artifact_schema_version, error_out) ||
        !parse_required_string(node, "fingerprint", "calibration_ref", &out->fingerprint, error_out)) {
        return false;
    }
    return true;
}

bool parse_layout_geometry_json(const nlohmann::json& node, LayoutGeometry* out, std::string* error_out)
{
    if (!out) {
        return set_error(error_out, "null LayoutGeometry destination");
    }
    if (!require_object(node, "layout_geometry", error_out)) {
        return false;
    }
    std::string type;
    if (!parse_required_string(node, "type", "layout_geometry", &type, error_out)) {
        return false;
    }
    if (type == "circle") {
        out->type = LayoutGeometryType::kCircle;
        return parse_circle_geometry_json(node, &out->circle, "layout_geometry", error_out);
    }
    if (type == "rectangle") {
        out->type = LayoutGeometryType::kRectangle;
        return parse_rectangle_geometry_json(node, &out->rectangle, "layout_geometry", error_out);
    }
    return set_error(error_out, "layout_geometry.type must be circle or rectangle");
}

bool parse_runtime_geometry_json(const nlohmann::json& node, RuntimeGeometry* out, std::string* error_out)
{
    if (!out) {
        return set_error(error_out, "null RuntimeGeometry destination");
    }
    if (!require_object(node, "runtime_geometry", error_out)) {
        return false;
    }
    std::string type;
    if (!parse_required_string(node, "type", "runtime_geometry", &type, error_out)) {
        return false;
    }
    if (type == "circle") {
        out->type = RuntimeGeometryType::kCircle;
        return parse_circle_geometry_json(node, &out->circle, "runtime_geometry", error_out);
    }
    if (type == "oriented_rectangle") {
        out->type = RuntimeGeometryType::kOrientedRectangle;
        return parse_oriented_rectangle_geometry_json(node, &out->oriented_rectangle, "runtime_geometry", error_out);
    }
    return set_error(error_out, "runtime_geometry.type must be circle or oriented_rectangle");
}

bool parse_dish_mask_geometry_json(const nlohmann::json& node, DishMaskGeometry* out, std::string* error_out)
{
    if (!out) {
        return set_error(error_out, "null DishMaskGeometry destination");
    }
    if (!require_object(node, "dish_mask.geometry", error_out)) {
        return false;
    }
    std::string coordinate_space;
    if (!parse_required_string(node, "coordinate_space", "dish_mask.geometry", &coordinate_space, error_out) ||
        !coordinate_space_from_string(coordinate_space, &out->coordinate_space, error_out)) {
        return false;
    }
    if (!node.contains("outer_geometry") || !parse_runtime_geometry_json(node.at("outer_geometry"), &out->outer_geometry, error_out) ||
        !node.contains("valid_geometry") || !parse_runtime_geometry_json(node.at("valid_geometry"), &out->valid_geometry, error_out) ||
        !parse_required_number(node, "edge_margin_px", "dish_mask.geometry", &out->edge_margin_px, error_out)) {
        return false;
    }
    return validate_dish_mask_geometry(*out, error_out);
}

bool parse_dish_mask_artifact_json(const nlohmann::json& node, DishMaskArtifact* out, std::string* error_out)
{
    if (!out) {
        return set_error(error_out, "null DishMaskArtifact destination");
    }
    if (!require_object(node, "dish_mask_artifact", error_out)) {
        return false;
    }
    std::string schema_id;
    int schema_version = 0;
    if (!parse_required_string(node, "schema_id", "dish_mask_artifact", &schema_id, error_out) ||
        !parse_required_int(node, "schema_version", "dish_mask_artifact", &schema_version, error_out)) {
        return false;
    }
    if (schema_id != kDishMaskArtifactSchemaId || schema_version != kDishMaskArtifactSchemaVersion) {
        return set_error(error_out, "dish_mask artifact schema mismatch");
    }
    if (!parse_required_string(node, "artifact_id", "dish_mask_artifact", &out->artifact_id, error_out) ||
        !parse_required_string(node, "created_utc", "dish_mask_artifact", &out->created_utc, error_out)) {
        return false;
    }
    if (!node.contains("calibration_ref") || !parse_calibration_ref_json(node.at("calibration_ref"), &out->calibration_ref, error_out)) {
        return false;
    }
    if (!node.contains("camera") || !require_object(node.at("camera"), "dish_mask_artifact.camera", error_out)) {
        return false;
    }
    if (!parse_required_string(node.at("camera"), "serial", "dish_mask_artifact.camera", &out->camera.serial, error_out) ||
        !parse_required_int(node.at("camera"), "width", "dish_mask_artifact.camera", &out->camera.width, error_out) ||
        !parse_required_int(node.at("camera"), "height", "dish_mask_artifact.camera", &out->camera.height, error_out) ||
        !parse_optional_string(node.at("camera"), "pixel_format", &out->camera.pixel_format, error_out, "dish_mask_artifact.camera")) {
        return false;
    }
    if (!node.contains("geometry") || !parse_dish_mask_geometry_json(node.at("geometry"), &out->geometry, error_out)) {
        return false;
    }
    if (!node.contains("provenance") || !require_object(node.at("provenance"), "dish_mask_artifact.provenance", error_out)) {
        return false;
    }
    std::string source;
    if (!parse_required_string(node.at("provenance"), "source", "dish_mask_artifact.provenance", &source, error_out) ||
        !observation_source_from_string(source, &out->provenance.source, error_out)) {
        return false;
    }
    if (node.at("provenance").contains("source_image_kind")) {
        out->provenance.has_source_image_kind = true;
        std::string kind;
        if (!parse_required_string(node.at("provenance"), "source_image_kind", "dish_mask_artifact.provenance", &kind, error_out) ||
            !source_image_kind_from_string(kind, &out->provenance.source_image_kind, error_out)) {
            return false;
        }
    }
    if (!parse_optional_string(node.at("provenance"), "notes", &out->provenance.notes, error_out, "dish_mask_artifact.provenance")) {
        return false;
    }
    if (node.contains("context")) {
        if (!require_object(node.at("context"), "dish_mask_artifact.context", error_out) ||
            !parse_optional_string(node.at("context"), "dish_design_id", &out->context.dish_design_id, error_out, "dish_mask_artifact.context") ||
            !parse_optional_string(node.at("context"), "canvas_id", &out->context.canvas_id, error_out, "dish_mask_artifact.context") ||
            !parse_optional_string(node.at("context"), "shelf_id", &out->context.shelf_id, error_out, "dish_mask_artifact.context")) {
            return false;
        }
    }
    return validate_dish_mask_artifact(*out, error_out);
}

bool parse_arena_layout_artifact_json(const nlohmann::json& node, ArenaLayoutArtifact* out, std::string* error_out)
{
    if (!out) {
        return set_error(error_out, "null ArenaLayoutArtifact destination");
    }
    if (!require_object(node, "arena_layout_artifact", error_out)) {
        return false;
    }
    std::string schema_id;
    int schema_version = 0;
    if (!parse_required_string(node, "schema_id", "arena_layout_artifact", &schema_id, error_out) ||
        !parse_required_int(node, "schema_version", "arena_layout_artifact", &schema_version, error_out)) {
        return false;
    }
    if (schema_id != kArenaLayoutArtifactSchemaId || schema_version != kArenaLayoutArtifactSchemaVersion) {
        return set_error(error_out, "arena_layout artifact schema mismatch");
    }
    if (!parse_required_string(node, "artifact_id", "arena_layout_artifact", &out->artifact_id, error_out) ||
        !parse_required_string(node, "created_utc", "arena_layout_artifact", &out->created_utc, error_out) ||
        !parse_required_string(node, "layout_id", "arena_layout_artifact", &out->layout_id, error_out)) {
        return false;
    }
    if (!node.contains("calibration_ref") || !parse_calibration_ref_json(node.at("calibration_ref"), &out->calibration_ref, error_out)) {
        return false;
    }
    if (!node.contains("layout") || !require_object(node.at("layout"), "arena_layout_artifact.layout", error_out)) {
        return false;
    }
    std::string coordinate_space;
    if (!parse_required_string(node.at("layout"), "coordinate_space", "arena_layout_artifact.layout", &coordinate_space, error_out) ||
        !coordinate_space_from_string(coordinate_space, &out->layout.coordinate_space, error_out)) {
        return false;
    }
    if (!node.at("layout").contains("outer_geometry") ||
        !parse_layout_geometry_json(node.at("layout").at("outer_geometry"), &out->layout.outer_geometry, error_out)) {
        return false;
    }
    if (!node.at("layout").contains("zones") || !node.at("layout").at("zones").is_array()) {
        return set_error(error_out, "arena_layout_artifact.layout.zones must be an array");
    }
    out->layout.zones.clear();
    for (size_t idx = 0; idx < node.at("layout").at("zones").size(); ++idx) {
        const nlohmann::json& zone_json = node.at("layout").at("zones")[idx];
        if (!require_object(zone_json, "arena_layout_artifact.layout.zones[" + std::to_string(idx) + "]", error_out)) {
            return false;
        }
        ArenaLayoutZone zone;
        if (!parse_required_string(zone_json, "zone_id", "arena_layout_artifact.layout.zones[" + std::to_string(idx) + "]", &zone.zone_id, error_out) ||
            !zone_json.contains("geometry") ||
            !parse_layout_geometry_json(zone_json.at("geometry"), &zone.geometry, error_out)) {
            return false;
        }
        if (zone_json.contains("zone_index")) {
            zone.has_zone_index = true;
            if (!parse_required_int(zone_json, "zone_index", "arena_layout_artifact.layout.zones[" + std::to_string(idx) + "]", &zone.zone_index, error_out)) {
                return false;
            }
        }
        if (!parse_optional_string(zone_json, "display_label", &zone.display_label, error_out,
                                   "arena_layout_artifact.layout.zones[" + std::to_string(idx) + "]")) {
            return false;
        }
        out->layout.zones.push_back(std::move(zone));
    }
    if (!node.contains("provenance") || !require_object(node.at("provenance"), "arena_layout_artifact.provenance", error_out)) {
        return false;
    }
    std::string provenance_source;
    if (!parse_required_string(node.at("provenance"), "source", "arena_layout_artifact.provenance", &provenance_source, error_out) ||
        !arena_layout_provenance_source_from_string(provenance_source, &out->provenance.source, error_out) ||
        !parse_required_string(node.at("provenance"), "ordering_rule", "arena_layout_artifact.provenance", &out->provenance.ordering_rule, error_out) ||
        !parse_optional_string(node.at("provenance"), "notes", &out->provenance.notes, error_out, "arena_layout_artifact.provenance")) {
        return false;
    }
    if (node.contains("context")) {
        if (!require_object(node.at("context"), "arena_layout_artifact.context", error_out) ||
            !parse_optional_string(node.at("context"), "dish_design_id", &out->context.dish_design_id, error_out, "arena_layout_artifact.context") ||
            !parse_optional_string(node.at("context"), "canvas_id", &out->context.canvas_id, error_out, "arena_layout_artifact.context")) {
            return false;
        }
    }
    return validate_arena_layout_artifact(*out, error_out);
}

bool parse_view_registration_json(const nlohmann::json& node, ViewRegistration* out, std::string* error_out)
{
    if (!out) {
        return set_error(error_out, "null ViewRegistration destination");
    }
    if (!require_object(node, "registration", error_out)) {
        return false;
    }
    std::string type;
    std::string layout_coordinate_space;
    std::string source;
    if (!parse_required_string(node, "type", "registration", &type, error_out) ||
        !registration_type_from_string(type, &out->type, error_out) ||
        !parse_required_string(node, "layout_coordinate_space", "registration", &layout_coordinate_space, error_out) ||
        !coordinate_space_from_string(layout_coordinate_space, &out->layout_coordinate_space, error_out) ||
        !parse_required_string(node, "source", "registration", &source, error_out) ||
        !registration_source_from_string(source, &out->source, error_out) ||
        !parse_required_matrix9(node, "layout_to_camera_matrix", "registration", &out->layout_to_camera_matrix, error_out) ||
        !parse_required_int(node, "fit_point_count", "registration", &out->fit_point_count, error_out) ||
        !parse_required_number(node, "residual_px", "registration", &out->residual_px, error_out)) {
        return false;
    }
    out->has_camera_to_layout_matrix = node.contains("camera_to_layout_matrix");
    if (out->has_camera_to_layout_matrix &&
        !parse_required_matrix9(node, "camera_to_layout_matrix", "registration", &out->camera_to_layout_matrix, error_out)) {
        return false;
    }
    out->has_orientation_status = node.contains("orientation_status");
    if (out->has_orientation_status) {
        std::string orientation_status;
        if (!parse_required_string(node, "orientation_status", "registration", &orientation_status, error_out) ||
            !orientation_status_from_string(orientation_status, &out->orientation_status, error_out)) {
            return false;
        }
    }
    return validate_view_registration(*out, error_out);
}

bool parse_dish_mask_runtime_json(const nlohmann::json& node, DishMaskRuntime* out, std::string* error_out)
{
    if (!out) {
        return set_error(error_out, "null DishMaskRuntime destination");
    }
    if (!require_object(node, "dish_mask.runtime", error_out)) {
        return false;
    }
    std::string source;
    if (!parse_required_int(node, "schema_version", "dish_mask.runtime", &out->schema_version, error_out) ||
        !parse_required_bool(node, "enabled", "dish_mask.runtime", &out->enabled, error_out) ||
        !parse_required_string(node, "source", "dish_mask.runtime", &source, error_out) ||
        !observation_source_from_string(source, &out->source, error_out)) {
        return false;
    }
    out->has_geometry = node.contains("geometry");
    if (out->has_geometry && !parse_dish_mask_geometry_json(node.at("geometry"), &out->geometry, error_out)) {
        return false;
    }
    return validate_dish_mask_runtime(*out, error_out);
}

bool parse_arena_layout_runtime_json(const nlohmann::json& node, ArenaLayoutRuntime* out, std::string* error_out)
{
    if (!out) {
        return set_error(error_out, "null ArenaLayoutRuntime destination");
    }
    if (!require_object(node, "arena_layout.runtime", error_out)) {
        return false;
    }
    std::string coordinate_space;
    if (!parse_required_int(node, "schema_version", "arena_layout.runtime", &out->schema_version, error_out) ||
        !parse_required_bool(node, "enabled", "arena_layout.runtime", &out->enabled, error_out) ||
        !parse_required_string(node, "layout_id", "arena_layout.runtime", &out->layout_id, error_out) ||
        !parse_required_string(node, "coordinate_space", "arena_layout.runtime", &coordinate_space, error_out) ||
        !coordinate_space_from_string(coordinate_space, &out->coordinate_space, error_out)) {
        return false;
    }
    if (!node.contains("registration") || !parse_view_registration_json(node.at("registration"), &out->registration, error_out)) {
        return false;
    }
    if (!parse_optional_string_array(node, "visible_zone_ids", &out->has_visible_zone_ids, &out->visible_zone_ids, error_out,
                                     "arena_layout.runtime")) {
        return false;
    }
    if (!node.contains("zones") || !node.at("zones").is_array()) {
        return set_error(error_out, "arena_layout.runtime.zones must be an array");
    }
    out->zones.clear();
    for (size_t idx = 0; idx < node.at("zones").size(); ++idx) {
        const nlohmann::json& zone_json = node.at("zones")[idx];
        if (!require_object(zone_json, "arena_layout.runtime.zones[" + std::to_string(idx) + "]", error_out)) {
            return false;
        }
        ResolvedZoneOverlay zone;
        std::string visibility_status;
        if (!parse_required_string(zone_json, "zone_id", "arena_layout.runtime.zones[" + std::to_string(idx) + "]", &zone.zone_id, error_out) ||
            !parse_required_string(zone_json, "visibility_status", "arena_layout.runtime.zones[" + std::to_string(idx) + "]", &visibility_status, error_out) ||
            !visibility_status_from_string(visibility_status, &zone.visibility_status, error_out) ||
            !zone_json.contains("geometry") ||
            !parse_runtime_geometry_json(zone_json.at("geometry"), &zone.geometry, error_out)) {
            return false;
        }
        if (zone_json.contains("zone_index")) {
            zone.has_zone_index = true;
            if (!parse_required_int(zone_json, "zone_index", "arena_layout.runtime.zones[" + std::to_string(idx) + "]", &zone.zone_index, error_out)) {
                return false;
            }
        }
        out->zones.push_back(std::move(zone));
    }
    return validate_arena_layout_runtime(*out, error_out);
}

bool parse_camera_spatial_calibration_json(const nlohmann::json& node,
                                           CameraSpatialCalibration* out,
                                           std::string* error_out)
{
    if (!out) {
        return set_error(error_out, "null CameraSpatialCalibration destination");
    }
    if (!require_object(node, "camera_spatial_calibration", error_out)) {
        return false;
    }
    out->has_dish_mask = node.contains("dish_mask");
    if (out->has_dish_mask) {
        if (!require_object(node.at("dish_mask"), "camera_spatial_calibration.dish_mask", error_out) ||
            !node.at("dish_mask").contains("calibration_ref") ||
            !parse_calibration_ref_json(node.at("dish_mask").at("calibration_ref"), &out->dish_mask.calibration_ref, error_out) ||
            !node.at("dish_mask").contains("runtime") ||
            !parse_dish_mask_runtime_json(node.at("dish_mask").at("runtime"), &out->dish_mask.runtime, error_out)) {
            return false;
        }
    }
    out->has_arena_layout = node.contains("arena_layout");
    if (out->has_arena_layout) {
        if (!require_object(node.at("arena_layout"), "camera_spatial_calibration.arena_layout", error_out) ||
            !node.at("arena_layout").contains("calibration_ref") ||
            !parse_calibration_ref_json(node.at("arena_layout").at("calibration_ref"), &out->arena_layout.calibration_ref, error_out) ||
            !node.at("arena_layout").contains("runtime") ||
            !parse_arena_layout_runtime_json(node.at("arena_layout").at("runtime"), &out->arena_layout.runtime, error_out)) {
            return false;
        }
    }
    return validate_camera_spatial_calibration(*out, error_out);
}

} // namespace orange::spatial
