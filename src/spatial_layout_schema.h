#ifndef ORANGE_SPATIAL_LAYOUT_SCHEMA_H
#define ORANGE_SPATIAL_LAYOUT_SCHEMA_H

#include "json.hpp"

#include <array>
#include <string>
#include <vector>

namespace orange::spatial {

inline constexpr const char* kDishMaskArtifactSchemaId = "orange.calibration.dish_mask";
inline constexpr int kDishMaskArtifactSchemaVersion = 1;
inline constexpr const char* kArenaLayoutArtifactSchemaId = "orange.calibration.arena_layout";
inline constexpr int kArenaLayoutArtifactSchemaVersion = 1;
inline constexpr int kDishMaskRuntimeSchemaVersion = 1;
inline constexpr int kArenaLayoutRuntimeSchemaVersion = 1;

enum class CoordinateSpace {
    kCameraNativePixels,
    kLayoutMm,
    kLayoutUnits
};

enum class LayoutGeometryType {
    kCircle,
    kRectangle
};

enum class RuntimeGeometryType {
    kCircle,
    kOrientedRectangle
};

enum class RegistrationType {
    kIdentity,
    kTranslation,
    kSimilarity
};

enum class RegistrationSource {
    kIdentity,
    kManual,
    kManualFit,
    kDetectedFit,
    kImported
};

enum class ObservationSource {
    kManual,
    kManualFit,
    kDetectedFit,
    kImported
};

enum class SourceImageKind {
    kEmptyDishFrame,
    kCalibrationCapture,
    kSyntheticTemplate
};

enum class ArenaLayoutProvenanceSource {
    kManualTemplate,
    kImportedTemplate
};

enum class OrientationStatus {
    kTrusted,
    kManualConfirmed,
    kAmbiguous,
    kUnknown
};

enum class VisibilityStatus {
    kFull,
    kPartial,
    kOccluded
};

struct CalibrationRef {
    std::string artifact_id;
    std::string artifact_schema_id;
    int artifact_schema_version = 0;
    std::string fingerprint;
};

struct CircleGeometry {
    double cx = 0.0;
    double cy = 0.0;
    double r = 0.0;
};

struct RectangleGeometry {
    double x = 0.0;
    double y = 0.0;
    double width = 0.0;
    double height = 0.0;
};

struct OrientedRectangleGeometry {
    double cx = 0.0;
    double cy = 0.0;
    double width = 0.0;
    double height = 0.0;
    double rotation_deg_clockwise = 0.0;
};

struct LayoutGeometry {
    LayoutGeometryType type = LayoutGeometryType::kCircle;
    CircleGeometry circle;
    RectangleGeometry rectangle;
};

struct RuntimeGeometry {
    RuntimeGeometryType type = RuntimeGeometryType::kCircle;
    CircleGeometry circle;
    OrientedRectangleGeometry oriented_rectangle;
};

struct DishMaskGeometry {
    CoordinateSpace coordinate_space = CoordinateSpace::kCameraNativePixels;
    RuntimeGeometry outer_geometry;
    RuntimeGeometry valid_geometry;
    double edge_margin_px = 0.0;
    double centroid_gate_outset_px = 0.0;
};

struct DishMaskArtifactCamera {
    std::string serial;
    int width = 0;
    int height = 0;
    std::string pixel_format;
};

struct DishMaskProvenance {
    ObservationSource source = ObservationSource::kManual;
    bool has_source_image_kind = false;
    SourceImageKind source_image_kind = SourceImageKind::kEmptyDishFrame;
    std::string notes;
};

struct DishMaskContext {
    std::string dish_design_id;
    std::string canvas_id;
    std::string shelf_id;
};

struct DishMaskArtifact {
    std::string artifact_id;
    std::string created_utc;
    CalibrationRef calibration_ref;
    DishMaskArtifactCamera camera;
    DishMaskGeometry geometry;
    DishMaskProvenance provenance;
    DishMaskContext context;
};

struct ArenaLayoutZone {
    std::string zone_id;
    bool has_zone_index = false;
    int zone_index = 0;
    std::string display_label;
    LayoutGeometry geometry;
};

struct ArenaLayoutDefinition {
    CoordinateSpace coordinate_space = CoordinateSpace::kLayoutMm;
    LayoutGeometry outer_geometry;
    std::vector<ArenaLayoutZone> zones;
};

struct ArenaLayoutContext {
    std::string dish_design_id;
    std::string canvas_id;
    std::string arena_id;
};

struct ArenaLayoutProvenance {
    ArenaLayoutProvenanceSource source = ArenaLayoutProvenanceSource::kManualTemplate;
    std::string ordering_rule;
    std::string notes;
};

struct ArenaLayoutArtifact {
    std::string artifact_id;
    std::string created_utc;
    CalibrationRef calibration_ref;
    std::string layout_id;
    ArenaLayoutDefinition layout;
    ArenaLayoutContext context;
    ArenaLayoutProvenance provenance;
};

struct ViewRegistration {
    RegistrationType type = RegistrationType::kSimilarity;
    CoordinateSpace layout_coordinate_space = CoordinateSpace::kLayoutMm;
    RegistrationSource source = RegistrationSource::kManualFit;
    std::array<double, 9> layout_to_camera_matrix{
        1.0, 0.0, 0.0,
        0.0, 1.0, 0.0,
        0.0, 0.0, 1.0};
    bool has_camera_to_layout_matrix = false;
    std::array<double, 9> camera_to_layout_matrix{
        1.0, 0.0, 0.0,
        0.0, 1.0, 0.0,
        0.0, 0.0, 1.0};
    int fit_point_count = 0;
    double residual_px = 0.0;
    bool has_orientation_status = false;
    OrientationStatus orientation_status = OrientationStatus::kUnknown;
};

struct DishMaskRuntime {
    int schema_version = kDishMaskRuntimeSchemaVersion;
    bool enabled = false;
    bool has_geometry = false;
    DishMaskGeometry geometry;
    ObservationSource source = ObservationSource::kManual;
};

struct ResolvedZoneOverlay {
    std::string zone_id;
    bool has_zone_index = false;
    int zone_index = 0;
    VisibilityStatus visibility_status = VisibilityStatus::kFull;
    RuntimeGeometry geometry;
};

struct ArenaLayoutRuntime {
    int schema_version = kArenaLayoutRuntimeSchemaVersion;
    bool enabled = false;
    std::string layout_id;
    CoordinateSpace coordinate_space = CoordinateSpace::kCameraNativePixels;
    ViewRegistration registration;
    bool has_visible_zone_ids = false;
    std::vector<std::string> visible_zone_ids;
    std::vector<ResolvedZoneOverlay> zones;
};

struct ReferencedDishMaskRuntime {
    CalibrationRef calibration_ref;
    DishMaskRuntime runtime;
};

struct ReferencedArenaLayoutRuntime {
    CalibrationRef calibration_ref;
    ArenaLayoutRuntime runtime;
};

struct CameraSpatialCalibration {
    bool has_dish_mask = false;
    ReferencedDishMaskRuntime dish_mask;
    bool has_arena_layout = false;
    ReferencedArenaLayoutRuntime arena_layout;
};

const char* coordinate_space_to_string(CoordinateSpace value);
bool coordinate_space_from_string(const std::string& value, CoordinateSpace* out, std::string* error_out = nullptr);

const char* layout_geometry_type_to_string(LayoutGeometryType value);
const char* runtime_geometry_type_to_string(RuntimeGeometryType value);

const char* registration_type_to_string(RegistrationType value);
bool registration_type_from_string(const std::string& value, RegistrationType* out, std::string* error_out = nullptr);

const char* registration_source_to_string(RegistrationSource value);
bool registration_source_from_string(const std::string& value, RegistrationSource* out, std::string* error_out = nullptr);

const char* observation_source_to_string(ObservationSource value);
bool observation_source_from_string(const std::string& value, ObservationSource* out, std::string* error_out = nullptr);

const char* source_image_kind_to_string(SourceImageKind value);
bool source_image_kind_from_string(const std::string& value, SourceImageKind* out, std::string* error_out = nullptr);

const char* arena_layout_provenance_source_to_string(ArenaLayoutProvenanceSource value);
bool arena_layout_provenance_source_from_string(const std::string& value,
                                                ArenaLayoutProvenanceSource* out,
                                                std::string* error_out = nullptr);

const char* orientation_status_to_string(OrientationStatus value);
bool orientation_status_from_string(const std::string& value, OrientationStatus* out, std::string* error_out = nullptr);

const char* visibility_status_to_string(VisibilityStatus value);
bool visibility_status_from_string(const std::string& value, VisibilityStatus* out, std::string* error_out = nullptr);

bool validate_calibration_ref(const CalibrationRef& value,
                              const char* expected_schema_id,
                              int expected_schema_version,
                              std::string* error_out);
bool validate_layout_geometry(const LayoutGeometry& value, std::string* error_out);
bool validate_runtime_geometry(const RuntimeGeometry& value, std::string* error_out);
bool validate_dish_mask_geometry(const DishMaskGeometry& value, std::string* error_out);
bool validate_view_registration(const ViewRegistration& value, std::string* error_out);
bool validate_dish_mask_artifact(const DishMaskArtifact& value, std::string* error_out);
bool validate_arena_layout_artifact(const ArenaLayoutArtifact& value, std::string* error_out);
bool validate_dish_mask_runtime(const DishMaskRuntime& value, std::string* error_out);
bool validate_arena_layout_runtime(const ArenaLayoutRuntime& value, std::string* error_out);
bool validate_camera_spatial_calibration(const CameraSpatialCalibration& value, std::string* error_out);
bool validate_arena_layout_runtime_against_artifact(const ArenaLayoutRuntime& runtime,
                                                    const ArenaLayoutArtifact& artifact,
                                                    std::string* error_out);

nlohmann::json calibration_ref_to_json(const CalibrationRef& value);
nlohmann::json layout_geometry_to_json(const LayoutGeometry& value);
nlohmann::json runtime_geometry_to_json(const RuntimeGeometry& value);
nlohmann::json dish_mask_geometry_to_json(const DishMaskGeometry& value);
nlohmann::json dish_mask_artifact_to_json(const DishMaskArtifact& value);
nlohmann::json arena_layout_artifact_to_json(const ArenaLayoutArtifact& value);
nlohmann::json view_registration_to_json(const ViewRegistration& value);
nlohmann::json dish_mask_runtime_to_json(const DishMaskRuntime& value);
nlohmann::json arena_layout_runtime_to_json(const ArenaLayoutRuntime& value);
nlohmann::json camera_spatial_calibration_to_json(const CameraSpatialCalibration& value);

bool parse_calibration_ref_json(const nlohmann::json& node, CalibrationRef* out, std::string* error_out);
bool parse_layout_geometry_json(const nlohmann::json& node, LayoutGeometry* out, std::string* error_out);
bool parse_runtime_geometry_json(const nlohmann::json& node, RuntimeGeometry* out, std::string* error_out);
bool parse_dish_mask_geometry_json(const nlohmann::json& node, DishMaskGeometry* out, std::string* error_out);
bool parse_dish_mask_artifact_json(const nlohmann::json& node, DishMaskArtifact* out, std::string* error_out);
bool parse_arena_layout_artifact_json(const nlohmann::json& node, ArenaLayoutArtifact* out, std::string* error_out);
bool parse_view_registration_json(const nlohmann::json& node, ViewRegistration* out, std::string* error_out);
bool parse_dish_mask_runtime_json(const nlohmann::json& node, DishMaskRuntime* out, std::string* error_out);
bool parse_arena_layout_runtime_json(const nlohmann::json& node, ArenaLayoutRuntime* out, std::string* error_out);
bool parse_camera_spatial_calibration_json(const nlohmann::json& node,
                                           CameraSpatialCalibration* out,
                                           std::string* error_out);

} // namespace orange::spatial

#endif // ORANGE_SPATIAL_LAYOUT_SCHEMA_H
