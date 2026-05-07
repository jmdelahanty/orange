#include "spatial_calibration_snapshot.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>

namespace {

bool write_json_file(const std::filesystem::path& path, const nlohmann::json& value)
{
    std::ofstream out(path, std::ios::out | std::ios::trunc);
    if (!out.is_open()) {
        std::cerr << "failed to open " << path << " for writing" << std::endl;
        return false;
    }
    out << value.dump(2) << std::endl;
    return true;
}

bool expect(bool condition, const char* message)
{
    if (!condition) {
        std::cerr << "FAIL: " << message << std::endl;
        return false;
    }
    return true;
}

std::filesystem::path unique_temp_dir()
{
    const auto now = std::chrono::steady_clock::now().time_since_epoch().count();
    return std::filesystem::temp_directory_path() /
           ("orange_spatial_calibration_snapshot_test_" + std::to_string(now));
}

orange::spatial::ArenaLayoutArtifact make_fixture_arena_artifact()
{
    using namespace orange::spatial;

    ArenaLayoutArtifact artifact;
    artifact.artifact_id = "arenalayout_fixture_Cam2010095";
    artifact.created_utc = "2026-05-04T00:00:00Z";
    artifact.calibration_ref = CalibrationRef{
        artifact.artifact_id,
        kArenaLayoutArtifactSchemaId,
        kArenaLayoutArtifactSchemaVersion,
        "fnv1a64:fixture"
    };
    artifact.layout_id = "fixture_single_circle";
    artifact.layout.coordinate_space = CoordinateSpace::kLayoutMm;
    artifact.layout.outer_geometry.type = LayoutGeometryType::kCircle;
    artifact.layout.outer_geometry.circle = CircleGeometry{0.0, 0.0, 50.0};

    ArenaLayoutZone zone;
    zone.zone_id = "z0";
    zone.has_zone_index = true;
    zone.zone_index = 0;
    zone.display_label = "Experimental Area";
    zone.geometry.type = LayoutGeometryType::kCircle;
    zone.geometry.circle = CircleGeometry{0.0, 0.0, 40.0};
    artifact.layout.zones.push_back(zone);

    artifact.context.dish_design_id = "fixture_dish";
    artifact.context.canvas_id = "fixture_canvas";
    artifact.provenance.source = ArenaLayoutProvenanceSource::kImportedTemplate;
    artifact.provenance.ordering_rule = "single_zone";
    return artifact;
}

orange::spatial::ArenaLayoutRuntime make_fixture_arena_runtime()
{
    using namespace orange::spatial;

    ArenaLayoutRuntime runtime;
    runtime.enabled = true;
    runtime.layout_id = "fixture_single_circle";
    runtime.coordinate_space = CoordinateSpace::kCameraNativePixels;
    runtime.registration.type = RegistrationType::kSimilarity;
    runtime.registration.layout_coordinate_space = CoordinateSpace::kLayoutMm;
    runtime.registration.source = RegistrationSource::kImported;
    runtime.registration.layout_to_camera_matrix = {
        10.0, 0.0, 1000.0,
        0.0, 10.0, 1100.0,
        0.0, 0.0, 1.0
    };
    runtime.registration.has_camera_to_layout_matrix = true;
    runtime.registration.camera_to_layout_matrix = {
        0.1, 0.0, -100.0,
        0.0, 0.1, -110.0,
        0.0, 0.0, 1.0
    };
    runtime.registration.fit_point_count = 3;
    runtime.registration.residual_px = 0.25;
    runtime.registration.has_orientation_status = true;
    runtime.registration.orientation_status = OrientationStatus::kManualConfirmed;

    ResolvedZoneOverlay zone;
    zone.zone_id = "z0";
    zone.has_zone_index = true;
    zone.zone_index = 0;
    zone.visibility_status = VisibilityStatus::kFull;
    zone.geometry.type = RuntimeGeometryType::kCircle;
    zone.geometry.circle = CircleGeometry{1000.0, 1100.0, 400.0};
    runtime.zones.push_back(zone);
    runtime.has_visible_zone_ids = true;
    runtime.visible_zone_ids.push_back("z0");
    return runtime;
}

orange::spatial::DishMaskRuntime make_fixture_dish_mask_runtime()
{
    using namespace orange::spatial;

    DishMaskRuntime runtime;
    runtime.enabled = true;
    runtime.has_geometry = true;
    runtime.source = ObservationSource::kImported;
    runtime.geometry.coordinate_space = CoordinateSpace::kCameraNativePixels;
    runtime.geometry.outer_geometry.type = RuntimeGeometryType::kCircle;
    runtime.geometry.outer_geometry.circle = CircleGeometry{1000.0, 1100.0, 500.0};
    runtime.geometry.valid_geometry.type = RuntimeGeometryType::kCircle;
    runtime.geometry.valid_geometry.circle = CircleGeometry{1000.0, 1100.0, 480.0};
    runtime.geometry.edge_margin_px = 20.0;
    return runtime;
}

bool run_roundtrip_test()
{
    using namespace orange::spatial;

    const std::filesystem::path temp_root = unique_temp_dir();
    const std::filesystem::path artifact_dir = temp_root / "artifact";
    std::filesystem::create_directories(artifact_dir);

    const ArenaLayoutArtifact artifact = make_fixture_arena_artifact();
    const ArenaLayoutRuntime arena_runtime = make_fixture_arena_runtime();
    const DishMaskRuntime dish_runtime = make_fixture_dish_mask_runtime();

    if (!write_json_file(artifact_dir / "measurement.json", arena_layout_artifact_to_json(artifact)) ||
        !write_json_file(artifact_dir / "arena_layout_runtime.json", arena_layout_runtime_to_json(arena_runtime)) ||
        !write_json_file(artifact_dir / "dish_mask_runtime.json", dish_mask_runtime_to_json(dish_runtime))) {
        return false;
    }

    std::string error;
    CameraSpatialCalibration calibration;
    if (!load_camera_spatial_calibration_from_artifact_dir(
            artifact_dir.string(),
            &calibration,
            &error)) {
        std::cerr << "load failed: " << error << std::endl;
        return false;
    }

    bool ok = true;
    ok &= expect(calibration.has_dish_mask, "loaded calibration has dish mask");
    ok &= expect(calibration.has_arena_layout, "loaded calibration has arena layout");
    ok &= expect(calibration.arena_layout.calibration_ref.artifact_id == artifact.artifact_id,
                 "arena calibration ref preserves artifact id");
    ok &= expect(calibration.dish_mask.calibration_ref.artifact_schema_id == kDishMaskArtifactSchemaId,
                 "dish mask calibration ref uses dish mask schema");
    ok &= expect(!calibration.dish_mask.calibration_ref.fingerprint.empty(),
                 "dish mask runtime ref has a fingerprint");

    nlohmann::json snapshot = {{"recording_id", "fixture_recording"}};
    ok &= expect(apply_camera_spatial_calibration_to_snapshot_json(
                     &snapshot,
                     "2010095",
                     calibration,
                     &error),
                 "applied calibration to snapshot");
    ok &= expect(snapshot.contains("calibrations"), "snapshot has calibrations object");
    ok &= expect(snapshot["calibrations"].contains("2010095"), "snapshot has camera calibration");
    ok &= expect(snapshot["calibrations"]["2010095"]["arena_layout"]["runtime"]["layout_id"] ==
                     artifact.layout_id,
                 "snapshot preserves runtime layout id");

    CameraSpatialCalibration parsed;
    ok &= expect(parse_camera_spatial_calibration_json(
                     snapshot["calibrations"]["2010095"],
                     &parsed,
                     &error),
                 "snapshot calibration parses back through schema");

    std::filesystem::remove_all(temp_root);
    return ok;
}

bool run_missing_artifact_test()
{
    orange::spatial::CameraSpatialCalibration calibration;
    std::string error;
    const bool loaded = orange::spatial::load_camera_spatial_calibration_from_artifact_dir(
        "/tmp/orange_missing_spatial_calibration_artifact",
        &calibration,
        &error);
    return expect(!loaded, "missing artifact directory is rejected") &&
           expect(!error.empty(), "missing artifact reports an error");
}

} // namespace

int main()
{
    if (!run_roundtrip_test()) {
        return 1;
    }
    if (!run_missing_artifact_test()) {
        return 1;
    }
    std::cout << "spatial_calibration_snapshot_tests passed" << std::endl;
    return 0;
}
