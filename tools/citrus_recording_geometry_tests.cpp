#include "citrus_recording_geometry.h"
#include "gui/spatial_layout/sha256.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <unistd.h>

namespace {

namespace checksum = orange::gui::spatial_layout::checksum;

void require(bool condition, const std::string& message)
{
    if (!condition) throw std::runtime_error(message);
}

void write_json(const std::filesystem::path& path, const nlohmann::json& value)
{
    std::filesystem::create_directories(path.parent_path());
    std::ofstream output(path);
    if (!output) throw std::runtime_error("could not write fixture " + path.string());
    output << value.dump(2) << '\n';
}

std::string file_sha256(const std::filesystem::path& path)
{
    std::string value;
    std::string error;
    require(checksum::file_sha256(path, &value, &error), error);
    return value;
}

struct Fixture {
    std::filesystem::path root;
    std::filesystem::path selected_canvas;
    std::filesystem::path scale_pointer;
    std::filesystem::path daily_observation;
    ~Fixture()
    {
        std::error_code error;
        std::filesystem::remove_all(root, error);
    }
};

Fixture make_inherited_fixture()
{
    Fixture fixture;
    fixture.root = std::filesystem::temp_directory_path() /
        ("orange_citrus_geometry_" + std::to_string(static_cast<long long>(::getpid())));
    std::filesystem::remove_all(fixture.root);

    const auto targets = fixture.root / "targets";
    const auto rig = targets / "rigs" / "rig1";
    const auto selected_dir = rig / "selected";
    const auto authority_dir = rig / "authority";
    const auto artifacts = authority_dir / "calibration_artifacts";
    fixture.selected_canvas = selected_dir / "selected.json";
    fixture.scale_pointer = artifacts / "scale_active_arena_1_cam1_projected_surface.json";
    const auto homography_pointer = artifacts / "homography_active_arena_1_cam1.json";
    const auto homography_candidate = artifacts / "homography_candidates" /
        "fixture" / "arena_1_cam1" / "candidate.json";

    write_json(rig / "rig1_config.json", {
        {"schema_version", 1}, {"rig_id", "rig1"}, {"rig_geometry_revision", 3}});
    write_json(targets / "tank_designs" / "dish_selected.json", {
        {"schema_id", "citrus.tank_design_spec"},
        {"schema_version", 1},
        {"tank_design_id", "dish_selected"},
        {"shape", "circular"},
        {"dimensions", {
            {"inner_diameter_mm", 80.0},
            {"usable_area_diameter_mm", 80.0},
            {"outer_diameter_mm", 90.0},
            {"wall_thickness_mm", 5.0}}}});

    const nlohmann::json selected_camera = {
        {"camera_id", "cam1"}, {"native_width_px", 4512}, {"native_height_px", 4512},
        {"arena_center_x_px", 111}, {"arena_center_y_px", 222},
        {"arena_width_px", 300}, {"arena_height_px", 300}};
    const nlohmann::json authority_camera = {
        {"camera_id", "cam1"}, {"native_width_px", 4512}, {"native_height_px", 4512},
        {"arena_center_x_px", 333}, {"arena_center_y_px", 444},
        {"arena_width_px", 350}, {"arena_height_px", 352}};
    write_json(fixture.selected_canvas, {
        {"canvas_name", "selected"},
        {"canvas_width_px", 1920}, {"canvas_height_px", 1080},
        {"projection_geometry_authority", {
            {"schema_version", 1},
            {"mode", "inherit_active_commissioning"},
            {"source_canvas_name", "authority"},
            {"geometry_scope", "arena_placement_homography_and_projected_surface_scale"},
            {"experimental_area_owner", "selected_canvas"}}},
        {"arenas", {{"arena_1", {
            {"config_name", "arena_1"},
            {"selected_dish_type_name", "dish_selected"},
            {"experimental_area_shape", "RECTANGLE"},
            {"experimental_area_center_x_px", 175.0},
            {"experimental_area_center_y_px", 176.0},
            {"experimental_area_width_mm", 52.0},
            {"experimental_area_height_mm", 51.0},
            {"camera_calibrations", nlohmann::json::array({selected_camera})}}}}}});
    write_json(authority_dir / "authority.json", {
        {"canvas_name", "authority"},
        {"canvas_width_px", 1920}, {"canvas_height_px", 1080},
        {"arenas", {{"arena_1", {
            {"config_name", "arena_1"},
            {"selected_dish_type_name", "different_authority_dish"},
            {"experimental_area_shape", "CIRCLE"},
            {"experimental_area_radius_mm", 40.0},
            {"camera_calibrations", nlohmann::json::array({authority_camera})}}}}}});

    const std::string homography_candidate_id = "homography_candidate_cam1";
    write_json(homography_candidate, {
        {"schema_id", "citrus.calibration.homography_candidate"},
        {"schema_version", 1}, {"candidate_id", homography_candidate_id}});
    write_json(homography_pointer, {
        {"schema_id", "citrus.calibration.active_homography"},
        {"schema_version", 1}, {"status", "accepted"},
        {"rig_id", "rig1"}, {"canvas_name", "authority"},
        {"arena_id", "arena_1"}, {"camera_id", "cam1"},
        {"candidate_id", homography_candidate_id},
        {"candidate_json_path", homography_candidate.string()},
        {"candidate_json_checksum", file_sha256(homography_candidate)},
        {"target_plane", "projected_surface"},
        {"homography_direction", "camera_native_px_to_final_display_canvas_px"},
        {"homography_matrix", {{1.0, 0.0, 10.0}, {0.0, 1.0, 20.0}, {0.0, 0.0, 1.0}}}});
    write_json(fixture.scale_pointer, {
        {"schema_id", "citrus.calibration.active_projected_surface_scale"},
        {"schema_version", 1}, {"status", "accepted"},
        {"rig_id", "rig1"}, {"canvas_name", "authority"},
        {"arena_id", "arena_1"}, {"camera_id", "cam1"},
        {"target_plane", "projected_surface"},
        {"direction", "physical_target_mm_to_final_display_canvas_px"},
        {"active_homography", {{"candidate_id", homography_candidate_id}}},
        {"scale", {
            {"camera_pixels_per_mm", 52.75},
            {"canvas_pixels_per_mm", 4.22},
            {"canvas_pixels_per_mm_x", 4.21},
            {"canvas_pixels_per_mm_y", 4.23}}}});

    const std::string release_id = "commissioning_fixture";
    const auto release_path = artifacts / "commissioning" / release_id / "commissioning.json";
    write_json(release_path, {
        {"schema_id", "citrus.calibration.rig_canvas_commissioning_release"},
        {"schema_version", 1}, {"status", "accepted"},
        {"release_id", release_id}, {"rig_id", "rig1"},
        {"canvas_name", "authority"}, {"rig_geometry_revision", 3},
        {"members", nlohmann::json::array({{
            {"arena_id", "arena_1"}, {"camera_id", "cam1"},
            {"target_plane", "projected_surface"},
            {"homography", {
                {"active_pointer_path", homography_pointer.string()},
                {"active_pointer_sha256", file_sha256(homography_pointer)}}},
            {"projected_surface_scale", {
                {"active_pointer_path", fixture.scale_pointer.string()},
                {"active_pointer_sha256", file_sha256(fixture.scale_pointer)}}},
            {"requirements", {
                {"acceptance_receipts_valid", true},
                {"active_homography_compatible", true},
                {"active_scale_compatible", true},
                {"scale_bound_to_active_homography", true}}}}})}});
    write_json(artifacts / "commissioning_active.json", {
        {"schema_id", "citrus.calibration.active_rig_canvas_commissioning"},
        {"schema_version", 1}, {"status", "accepted"},
        {"release_id", release_id}, {"rig_id", "rig1"},
        {"canvas_name", "authority"},
        {"manifest_path", release_path.string()},
        {"manifest_sha256", file_sha256(release_path)}});

    const auto selected_artifacts = selected_dir / "calibration_artifacts";
    const auto daily_dir = selected_artifacts / "daily_registration" /
        "dailyregtxn_fixture";
    const auto observation_dir = fixture.root / "orange_data" /
        "calibrations" / "sessions" / "fixture" / "artifacts" /
        "Camcam1_arena_1" / "top_rim_observations" / "dishrim_fixture_cam1";
    fixture.daily_observation = observation_dir / "observation.json";
    const nlohmann::json center = {{"x", 2250.5}, {"y", 2261.25}};
    const nlohmann::json inner_geometry = {
        {"type", "circle"}, {"center_px", center}, {"radius_px", 2100.0}};
    const nlohmann::json valid_geometry = {
        {"type", "circle"}, {"center_px", center}, {"radius_px", 2117.0}};
    write_json(fixture.daily_observation, {
        {"schema_id", "orange.calibration.dish_top_rim_observation"},
        {"schema_version", 2},
        {"artifact_id", "dishrim_fixture_cam1"},
        {"physical_target", "dish_top_rim"},
        {"camera", {
            {"serial", "cam1"}, {"name", "Camcam1"},
            {"width", 4512}, {"height", 4512}, {"pixel_format", "Mono8"}}},
        {"arena_context", {
            {"rig_id", "rig1"}, {"canvas_id", "selected"},
            {"arena_id", "arena_1"}, {"camera_serial", "cam1"}}},
        {"accepted_inner_rim_boundary", {
            {"coordinate_space", "camera_native_pixels"},
            {"target_plane", "dish_top_rim"},
            {"geometry", inner_geometry}}},
        {"accepted_experimental_area_boundary", {
            {"coordinate_space", "camera_native_pixels"},
            {"target_plane", "dish_top_rim"},
            {"geometry", inner_geometry}}},
        {"accepted_mask", {
            {"shape", "circle"},
            {"coordinate_space", "camera_native_pixels"},
            {"center_px", center}, {"radius_px", 2117.0}}},
        {"valid_detection_region", {
            {"coordinate_space", "camera_native_pixels"},
            {"purpose", "bounding_box_centroid_detection_gating"},
            {"offset_direction", "outward"},
            {"geometry", valid_geometry}}},
        {"operator_review", {{"accepted", true}}},
        {"boundary_interpretation", {
            {"target_plane", "dish_top_rim"},
            {"valid_detection_region_policy",
             "derived_by_outward_offset_for_bounding_box_centroid_forgiveness"}}}});
    write_json(observation_dir / "image_set.json", {
        {"schema_id", "orange.calibration.image_set"},
        {"schema_version", 1},
        {"artifact_id", "dishrim_fixture_cam1"},
        {"purpose", "dish_top_rim"},
        {"coordinate_space", "camera_native_pixels"},
        {"target_plane", "dish_top_rim"},
        {"camera", {
            {"serial", "cam1"},
            {"image_shape", {{"width", 4512}, {"height", 4512}}}}}});
    write_json(observation_dir / "exports" /
                   "spatial_dish_mask_runtime_v1.json", {
        {"schema_version", 1}, {"enabled", true},
        {"geometry", {
            {"coordinate_space", "camera_native_pixels"},
            {"outer_geometry", {
                {"type", "circle"}, {"cx", 2250.5}, {"cy", 2261.25},
                {"r", 2100.0}}},
            {"valid_geometry", {
                {"type", "circle"}, {"cx", 2250.5}, {"cy", 2261.25},
                {"r", 2117.0}}}}},
        {"source_observation", {
            {"artifact_id", "dishrim_fixture_cam1"},
            {"artifact_schema_id", "orange.calibration.dish_top_rim_observation"},
            {"artifact_schema_version", 2}}}});
    write_json(observation_dir / "exports" / "palette_dish_mask_v2.json", {
        {"version", "2.0"}, {"shape", "circle"},
        {"orange_artifact_id", "dishrim_fixture_cam1"},
        {"orange_artifact_schema_id",
         "orange.calibration.dish_top_rim_observation"},
        {"orange_artifact_schema_version", 2},
        {"detected_circle", {
            {"center", {2251, 2261}}, {"radius", 2117}}}});
    write_json(observation_dir / "manifest.json", {
        {"schema_id", "orange.calibration.manifest"},
        {"schema_version", 1},
        {"artifact_id", "dishrim_fixture_cam1"},
        {"files", {
            {"image_set_json", "image_set.json"},
            {"spatial_dish_mask_runtime_v1",
             "exports/spatial_dish_mask_runtime_v1.json"},
            {"palette_dish_mask_v2", "exports/palette_dish_mask_v2.json"},
            {"review_overlay", "overlays/top_rim_fit.png"},
            {"valid_detection_overlay", "overlays/valid_detection_region.png"},
            {"registration_hough_overlay",
             "overlays/registration_hough_overlay.png"},
            {"source_frame", "captures/source_frame.png"}}},
        {"checksums", {{"algorithm", "fnv1a64"}}}});

    const auto candidate_path = daily_dir / "candidate.json";
    write_json(candidate_path, {
        {"schema_id", "citrus.calibration.daily_registration_candidate"},
        {"schema_version", 1}, {"status", "candidate"},
        {"candidate_id", "dailyreg_fixture"},
        {"transaction_id", "dailyregtxn_fixture"},
        {"rig_id", "rig1"}, {"canvas_name", "selected"}});
    const nlohmann::json commissioning_base = {
        {"release_id", release_id},
        {"manifest_path", release_path.string()},
        {"manifest_sha256", file_sha256(release_path)}};
    const auto registration_path = daily_dir / "registration.json";
    write_json(registration_path, {
        {"schema_id", "citrus.calibration.daily_registration"},
        {"schema_version", 1}, {"status", "accepted"},
        {"rig_id", "rig1"}, {"canvas_name", "selected"},
        {"transaction_id", "dailyregtxn_fixture"},
        {"registration_id", "dailyreg_fixture"},
        {"valid_until_utc", "2099-12-31T23:59:59Z"},
        {"candidate_path", candidate_path.string()},
        {"candidate_sha256", file_sha256(candidate_path)},
        {"commissioning_base", commissioning_base},
        {"targets", nlohmann::json::array({{
            {"arena_id", "arena_1"}, {"camera_id", "cam1"},
            {"target_plane", "projected_surface"},
            {"homography", {
                {"authority_canvas_name", "authority"},
                {"authority_mode", "inherit_active_commissioning"},
                {"selected_canvas_name", "selected"},
                {"candidate_id", homography_candidate_id},
                {"candidate_path", homography_candidate.string()},
                {"candidate_sha256", file_sha256(homography_candidate)}}},
            {"invariants", {
                {"arena_size_unchanged", true},
                {"canvas_geometry_unchanged", true},
                {"experimental_area_local_geometry_unchanged", true},
                {"homography_unchanged", true},
                {"scale_unchanged", true}}},
            {"rim_center_camera_px", center},
            {"observed_rim_radius_camera_px", 2100.0},
            {"rim_observation", {
                {"artifact_id", "dishrim_fixture_cam1"},
                {"path", fixture.daily_observation.string()},
                {"sha256", file_sha256(fixture.daily_observation)}}}}})}});
    write_json(selected_artifacts / "daily_registration_runtime_selection.json", {
        {"schema_id", "citrus.calibration.daily_registration_runtime_selection"},
        {"schema_version", 1}, {"status", "accepted"},
        {"rig_id", "rig1"}, {"canvas_name", "selected"},
        {"mode", "selected_daily_registration"},
        {"commissioning_base", commissioning_base},
        {"registration", {
            {"registration_id", "dailyreg_fixture"},
            {"valid_until_utc", "2099-12-31T23:59:59Z"},
            {"path", registration_path.string()},
            {"sha256", file_sha256(registration_path)}}}});
    return fixture;
}

void test_not_configured_is_nonblocking()
{
    orange::recording_geometry::CitrusGeometryResolveRequest request;
    request.captured_at_utc = "2026-07-21T00:00:00Z";
    request.camera_serials = {"cam1"};
    const auto result = orange::recording_geometry::resolve_citrus_recording_geometry(request);
    require(!result.configured, "empty selection must not be configured");
    require(result.contract.at("status") == "not_configured",
            "empty selection must be explicitly not_configured");
    require(!result.contract.at("recording_policy").at("recording_blocked").get<bool>(),
            "missing optional Citrus selection must not block recording");
}

void test_inherited_authority_preserves_selected_semantics()
{
    Fixture fixture = make_inherited_fixture();
    orange::recording_geometry::CitrusGeometryResolveRequest request;
    request.selected_canvas_config_path = fixture.selected_canvas.string();
    request.selection_source = "test";
    request.captured_at_utc = "2026-07-21T00:00:00Z";
    request.camera_serials = {"cam1"};
    const auto result = orange::recording_geometry::resolve_citrus_recording_geometry(request);
    require(result.configured, "fixture selection must be configured");
    require(result.fully_resolved, "valid inherited fixture must resolve fully");
    require(result.contract.at("status") == "resolved", "valid fixture must resolve");
    const auto& selection = result.contract.at("selection");
    require(selection.at("selected_canvas_name") == "selected",
            "selected canvas identity must be preserved");
    require(selection.at("projection_geometry_authority_canvas_name") == "authority",
            "authority canvas identity must be distinct");
    const auto& camera = result.contract.at("cameras").at("cam1");
    require(camera.at("selected_canvas").at("experimental_area").at(
                "experimental_area_shape") == "RECTANGLE",
            "selected experimental-area shape must not be copied from authority");
    require(camera.at("tank_design").at("tank_design_id") == "dish_selected",
            "selected tank identity must not be copied from authority");
    require(camera.at("projection_geometry").at("arena_placement").at(
                "arena_center_x_px") == 333,
            "arena placement must come from authority canvas");
    require(camera.at("projection_geometry").at("scale_models").at(
                "projected_surface").at("active_pointer_snapshot").at(
                    "scale").at("canvas_pixels_per_mm") == 4.22,
            "accepted authority scale must be embedded");
    require(camera.at("projection_geometry").at("homography").at(
                "source_path") ==
                (fixture.scale_pointer.parent_path() /
                 "homography_active_arena_1_cam1.json").string(),
            "accepted homography source path must be available for exact-byte materialization");
    require(camera.at("projection_geometry").at("scale_models").at(
                "projected_surface").at("source_path") ==
                fixture.scale_pointer.string(),
            "accepted scale source path must be available for exact-byte materialization");
    require(result.contract.at("tank_designs").at("dish_selected").at(
                "artifact").at("snapshot").at("dimensions").at(
                    "wall_thickness_mm") == 5.0,
            "selected tank specification must be self-contained");
    const auto& daily = result.contract.at("daily_registration_geometry");
    require(daily.at("status") == "selected_resolved",
            "selected daily registration should resolve independently");
    const auto& mask = daily.at("cameras").at("cam1").at(
        "recording_snapshot_entry");
    require(mask.at("accepted_inner_rim_boundary").at("geometry").at(
                "radius_px") == 2100.0,
            "physical inner-rim boundary must remain available in camera pixels");
    require(mask.at("valid_detection_region").at("geometry").at(
                "radius_px") == 2117.0,
            "outward centroid gate must remain distinct from the physical rim");
    require(mask.at("source").at("sha256") == file_sha256(
                fixture.daily_observation),
            "daily mask must retain its exact accepted observation checksum");
    require(mask.at("available_for_downstream_detection_gating").get<bool>(),
            "resolved daily mask should be explicitly downstream-usable");
    require(!mask.at("active_in_orange_live_detection_pipeline").get<bool>(),
            "metadata persistence must not claim Orange applied a live gate");
    require(camera.at("daily_registration_geometry").at(
                "recording_snapshot_entry").at("artifact_id") ==
                "dishrim_fixture_cam1",
            "camera-scoped H5 view must carry only its relevant registered mask");
}

void test_daily_observation_tamper_is_explicit_and_nonblocking()
{
    Fixture fixture = make_inherited_fixture();
    { std::ofstream output(fixture.daily_observation, std::ios::app); output << ' '; }
    orange::recording_geometry::CitrusGeometryResolveRequest request;
    request.selected_canvas_config_path = fixture.selected_canvas.string();
    request.camera_serials = {"cam1"};
    const auto result =
        orange::recording_geometry::resolve_citrus_recording_geometry(request);
    require(result.fully_resolved,
            "optional daily-mask failure must not invalidate commissioned geometry");
    require(result.contract.at("status") == "resolved",
            "optional daily-mask failure must preserve the base geometry result");
    require(result.contract.at("daily_registration_geometry").at("status") ==
                "selected_partial",
            "tampered daily observation must be explicit in its own status");
    require(!result.contract.at("recording_policy").at(
                "recording_blocked").get<bool>(),
            "tampered optional mask metadata must remain nonblocking");
}

void test_expired_daily_selection_is_explicit_and_nonblocking()
{
    Fixture fixture = make_inherited_fixture();
    orange::recording_geometry::CitrusGeometryResolveRequest request;
    request.selected_canvas_config_path = fixture.selected_canvas.string();
    request.captured_at_utc = "2100-01-01T00:00:00Z";
    request.camera_serials = {"cam1"};
    const auto result =
        orange::recording_geometry::resolve_citrus_recording_geometry(request);
    require(result.fully_resolved,
            "expired optional registration must preserve commissioned geometry");
    require(result.contract.at("daily_registration_geometry").at("status") ==
                "invalid",
            "expired selected registration must not be presented as usable");
    require(!result.contract.at("recording_policy").at(
                "recording_blocked").get<bool>(),
            "expired optional registration metadata remains nonblocking in Orange");
}

void test_active_member_checksum_failure_is_explicit()
{
    Fixture fixture = make_inherited_fixture();
    { std::ofstream output(fixture.scale_pointer, std::ios::app); output << ' '; }
    orange::recording_geometry::CitrusGeometryResolveRequest request;
    request.selected_canvas_config_path = fixture.selected_canvas.string();
    request.camera_serials = {"cam1"};
    const auto result = orange::recording_geometry::resolve_citrus_recording_geometry(request);
    require(!result.fully_resolved, "tampered active scale must not resolve");
    require(result.contract.at("status") == "invalid",
            "tampered active scale must be marked invalid");
    require(!result.contract.at("recording_policy").at("recording_blocked").get<bool>(),
            "invalid optional metadata must remain non-blocking");
}

}  // namespace

int main()
{
    struct Test { const char* name; void (*fn)(); };
    const Test tests[] = {
        {"not_configured_is_nonblocking", &test_not_configured_is_nonblocking},
        {"inherited_authority_preserves_selected_semantics",
         &test_inherited_authority_preserves_selected_semantics},
        {"daily_observation_tamper_is_explicit_and_nonblocking",
         &test_daily_observation_tamper_is_explicit_and_nonblocking},
        {"expired_daily_selection_is_explicit_and_nonblocking",
         &test_expired_daily_selection_is_explicit_and_nonblocking},
        {"active_member_checksum_failure_is_explicit",
         &test_active_member_checksum_failure_is_explicit}};
    for (const Test& test : tests) {
        try {
            test.fn();
            std::cout << "[PASS] " << test.name << '\n';
        } catch (const std::exception& ex) {
            std::cerr << "[FAIL] " << test.name << ": " << ex.what() << '\n';
            return EXIT_FAILURE;
        }
    }
    std::cout << "All Citrus recording geometry tests passed.\n";
    return EXIT_SUCCESS;
}
