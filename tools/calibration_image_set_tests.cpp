#include "calibration_image_set.h"

#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <unistd.h>

namespace {

void require(bool condition, const std::string& message)
{
    if (!condition) {
        throw std::runtime_error(message);
    }
}

nlohmann::json read_json(const std::filesystem::path& path)
{
    std::ifstream in(path, std::ios::in | std::ios::binary);
    if (!in.is_open()) {
        throw std::runtime_error("failed to open " + path.string());
    }
    nlohmann::json out;
    in >> out;
    return out;
}

std::filesystem::path make_temp_root()
{
    const std::filesystem::path root =
        std::filesystem::temp_directory_path() /
        ("orange_calibration_image_set_tests_" + std::to_string(static_cast<long long>(getpid())));
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root);
    return root;
}

orange::calibration::CalibrationImageSetRequest make_request()
{
    using namespace orange::calibration;

    CalibrationImageSetRequest request;
    request.artifact_id = "calimg_20260608_2010093_homography_grid";
    request.created_utc = "2026-06-08T19:45:00Z";
    request.purpose = "homography_grid";
    request.target_plane = "projected_surface";
    request.coordinate_space = "camera_native_pixels";
    request.camera.serial = "2010093";
    request.camera.name = "Cam2010093";
    request.camera.image_shape.height = 4512;
    request.camera.image_shape.width = 4512;
    request.camera.pixel_format = "Mono8";
    request.capture.operation_id = "daily_homography_20260608";
    request.capture.capture_group_id = "calgrp_20260608T194500Z_shadow_homography_grid";
    request.capture.timestamp_utc = "2026-06-08T19:45:00Z";
    request.capture.capture_mode = "visible_projected_grid";
    request.capture.filter_state = "removed";
    request.capture.runtime_filter_state = "850nm_bandpass_installed";
    request.capture.exposure_us = 500000.0;
    request.capture.has_exposure_us = true;
    request.capture.frame_rate_hz = 1.0;
    request.capture.has_frame_rate_hz = true;
    request.capture.light_handling = "suppress_mapped_strobe";
    request.capture.light_state = "visible_projector_only";
    request.capture.illumination_spectrum = "broadband_visible";
    request.capture.illumination_source = "visible_projector";
    request.capture.illumination_min_wavelength_nm = 400.0;
    request.capture.has_illumination_min_wavelength_nm = true;
    request.capture.illumination_max_wavelength_nm = 700.0;
    request.capture.has_illumination_max_wavelength_nm = true;
    request.capture.illumination_wavelength_confidence = "approximate_range";
    request.capture.projector_state = "grid_on";
    request.capture.projector_visible_to_camera = true;
    request.capture.has_projector_visible_to_camera = true;
    request.capture.requires_camera_mount_unchanged = true;
    request.capture.has_requires_camera_mount_unchanged = true;
    request.capture.requires_filter_reinstalled_repeatably = true;
    request.capture.has_requires_filter_reinstalled_repeatably = true;
    request.rig_context = {
        {"rig_id", "omnifin0"},
        {"canvas_id", "shadow"},
        {"arena_id", "arena_1"},
        {"citrus_config_ref", {
            {"source", "manual_import"},
            {"config_name", "fixture"}
        }}
    };
    request.images.push_back(CalibrationImageSetImageRef{
        "grid_on",
        "images/grid_on.png",
        "fnv1a64",
        "fnv1a64:abc",
        "camera_native_pixels",
        CalibrationImageSetShape{4512, 4512},
        "projected grid source image"});
    request.projected_pattern = {
        {"pattern_id", "citrus_homography_grid_v1"},
        {"type", "dot_grid"},
        {"rows", 11},
        {"cols", 11},
        {"spacing_canvas_px", 32.0},
        {"dot_radius_canvas_px", 3.0},
        {"canvas_coordinate_space", "final_display_canvas_px"}
    };
    request.citrus_preview = {
        {"available", true},
        {"diagnostic_only", true},
        {"authority", "citrus_recomputes_before_acceptance"}
    };
    request.operator_notes = "fixture notes";
    return request;
}

void test_writer_emits_core_shape()
{
    using namespace orange::calibration;

    const std::filesystem::path root = make_temp_root();
    const std::filesystem::path output_path = root / "image_set.json";

    CalibrationImageSetWriteResult result;
    std::string error;
    require(
        write_calibration_image_set_json_file(output_path.string(), make_request(), &result, &error),
        "image-set writer should succeed: " + error);
    require(std::filesystem::exists(output_path), "image_set.json written");

    const nlohmann::json image_set = read_json(output_path);
    require(image_set.value("schema_id", "") == kCalibrationImageSetSchemaId, "schema id");
    require(image_set.value("schema_version", 0) == kCalibrationImageSetSchemaVersion, "schema version");
    require(image_set.value("purpose", "") == "homography_grid", "purpose");
    require(image_set.value("target_plane", "") == "projected_surface", "target plane");
    require(image_set.value("coordinate_space", "") == "camera_native_pixels", "coordinate space");
    require(image_set["camera"].value("serial", "") == "2010093", "camera serial");
    require(image_set["camera"]["image_shape"].value("height", 0) == 4512, "camera height");
    require(image_set["camera"]["image_shape"].value("width", 0) == 4512, "camera width");
    require(
        image_set.value("capture_timestamp_utc", "") == "2026-06-08T19:45:00Z",
        "capture timestamp alias");
    require(image_set["capture"].value("timestamp_utc", "") == "2026-06-08T19:45:00Z", "timestamp");
    require(
        image_set["capture"].value("capture_group_id", "") ==
            "calgrp_20260608T194500Z_shadow_homography_grid",
        "capture group id");
    require(image_set["capture"].value("projector_visible_to_camera", false), "projector visible");
    require(
        image_set["capture"].value("light_handling", "") == "suppress_mapped_strobe",
        "light handling");
    require(
        image_set["capture"]["illumination"].value("spectrum", "") == "broadband_visible",
        "illumination spectrum");
    require(
        image_set["capture"]["illumination"].value("source", "") == "visible_projector",
        "illumination source");
    require(
        std::abs(image_set["capture"]["illumination"].value("min_wavelength_nm", 0.0) - 400.0) < 0.001,
        "illumination min wavelength");
    require(
        std::abs(image_set["capture"]["illumination"].value("max_wavelength_nm", 0.0) - 700.0) < 0.001,
        "illumination max wavelength");
    require(
        image_set["capture"]["illumination"].value("wavelength_confidence", "") == "approximate_range",
        "illumination confidence");
    require(image_set["images"].size() == 1, "one image");
    require(image_set["images"][0].value("role", "") == "grid_on", "image role");
    require(image_set["images"][0].value("checksum_algorithm", "") == "fnv1a64", "checksum algorithm");
    require(image_set["images"][0].value("checksum", "") == "fnv1a64:abc", "checksum");
    require(image_set["projected_pattern"].value("type", "") == "dot_grid", "projected pattern");
    require(image_set["citrus_preview"].value("diagnostic_only", false), "diagnostic preview");
    require(image_set.value("operator_notes", "") == "fixture notes", "operator notes");

    std::filesystem::remove_all(root);
}

void test_rejects_invalid_purpose()
{
    using namespace orange::calibration;

    CalibrationImageSetRequest request = make_request();
    request.purpose = "unsupported_scale";
    std::string error;
    const bool ok = write_calibration_image_set_json_file(
        (make_temp_root() / "image_set.json").string(),
        request,
        nullptr,
        &error);
    require(!ok, "invalid purpose should fail");
    require(error.find("purpose") != std::string::npos, "invalid purpose error should mention purpose");
}

void test_accepts_arena_projection_purpose()
{
    using namespace orange::calibration;

    const std::filesystem::path root = make_temp_root();
    const std::filesystem::path output_path = root / "arena_projection_image_set.json";

    CalibrationImageSetRequest request = make_request();
    request.artifact_id = "calimg_20260608_2010093_arena_projection";
    request.purpose = "arena_projection";
    request.target_plane = "projected_surface";
    request.images[0].role = "projected_arena";
    request.projected_pattern = {
        {"pattern_id", "citrus_arena_projection"},
        {"type", "arena_fill"},
        {"source", "citrus_arena_definition"},
        {"target_plane", "projected_surface"}
    };

    CalibrationImageSetWriteResult result;
    std::string error;
    require(
        write_calibration_image_set_json_file(output_path.string(), request, &result, &error),
        "arena_projection purpose should be accepted: " + error);

    const nlohmann::json image_set = read_json(output_path);
    require(image_set.value("purpose", "") == "arena_projection", "arena_projection purpose emitted");
    require(image_set["images"][0].value("role", "") == "projected_arena", "projected_arena role emitted");
    require(
        image_set["projected_pattern"].value("type", "") == "arena_fill",
        "arena_projection pattern type emitted");

    std::filesystem::remove_all(root);
}

void test_accepts_verification_dots_purpose()
{
    using namespace orange::calibration;

    const std::filesystem::path root = make_temp_root();
    const std::filesystem::path output_path = root / "verification_dots_image_set.json";

    CalibrationImageSetRequest request = make_request();
    request.artifact_id = "calimg_20260608_2010093_verification_dots";
    request.purpose = "verification_dots";
    request.target_plane = "projected_surface";
    request.images[0].role = "verification_dots_on";
    request.projected_pattern = {
        {"pattern_id", "citrus_projected_surface_verification_dots_v1"},
        {"type", "verification_dots"},
        {"mode", "verification_dots"},
        {"source", "citrus_active_projection_snapshot"},
        {"target_plane", "projected_surface"}
    };

    CalibrationImageSetWriteResult result;
    std::string error;
    require(
        write_calibration_image_set_json_file(output_path.string(), request, &result, &error),
        "verification_dots purpose should be accepted: " + error);

    const nlohmann::json image_set = read_json(output_path);
    require(
        image_set.value("purpose", "") == "verification_dots",
        "verification_dots purpose emitted");
    require(
        image_set.value("target_plane", "") == "projected_surface",
        "verification_dots target plane emitted");
    require(
        image_set["images"][0].value("role", "") == "verification_dots_on",
        "verification_dots role emitted");
    require(
        image_set["projected_pattern"].value("type", "") == "verification_dots",
        "verification_dots projected pattern type emitted");

    std::filesystem::remove_all(root);
}

void test_accepts_validation_pattern_purpose()
{
    using namespace orange::calibration;

    const std::filesystem::path root = make_temp_root();
    const std::filesystem::path output_path = root / "validation_pattern_image_set.json";

    CalibrationImageSetRequest request = make_request();
    request.artifact_id = "calimg_20260608_2010093_validation_pattern";
    request.purpose = "validation_pattern";
    request.target_plane = "projected_surface";
    request.images[0].role = "validation_pattern_on";
    request.projected_pattern = {
        {"pattern_id", "citrus_projected_surface_validation_pattern_v1"},
        {"type", "validation_pattern"},
        {"source", "citrus_active_projection_snapshot"},
        {"target_plane", "projected_surface"}
    };

    CalibrationImageSetWriteResult result;
    std::string error;
    require(
        write_calibration_image_set_json_file(output_path.string(), request, &result, &error),
        "validation_pattern purpose should be accepted: " + error);

    const nlohmann::json image_set = read_json(output_path);
    require(
        image_set.value("purpose", "") == "validation_pattern",
        "validation_pattern purpose emitted");
    require(
        image_set["images"][0].value("role", "") == "validation_pattern_on",
        "validation pattern role emitted");
    require(
        image_set["projected_pattern"].value("type", "") == "validation_pattern",
        "validation pattern projected type emitted");

    std::filesystem::remove_all(root);
}

void test_accepts_aggregate_camera_arena_set()
{
    using namespace orange::calibration;

    const std::filesystem::path root = make_temp_root();
    const std::filesystem::path output_path = root / "aggregate_image_set.json";

    CalibrationImageSetRequest request = make_request();
    request.artifact_id = "Cam2010093_arena_1";
    request.purpose = "camera_arena_calibration_set";
    request.target_plane = "multiple";
    request.images[0].role = "grid_on";
    request.images[0].path = "captures/homography_grid_2026_06_08T19_45_00Z.png";

    CalibrationImageSetWriteResult result;
    std::string error;
    require(
        write_calibration_image_set_json_file(output_path.string(), request, &result, &error),
        "camera_arena_calibration_set purpose should be accepted: " + error);

    const nlohmann::json image_set = read_json(output_path);
    require(
        image_set.value("purpose", "") == "camera_arena_calibration_set",
        "aggregate purpose emitted");
    require(image_set.value("target_plane", "") == "multiple", "aggregate target plane emitted");

    std::filesystem::remove_all(root);
}

void test_emits_dry_reference_capture_stage_metadata()
{
    using namespace orange::calibration;

    const std::filesystem::path root = make_temp_root();
    const std::filesystem::path output_path = root / "dry_reference_image_set.json";

    CalibrationImageSetRequest request = make_request();
    request.artifact_id = "calimg_20260608_2010093_dry_reference";
    request.capture_stage = "projected_surface_dry_reference";
    request.target_plane = "projected_surface";
    request.wet_or_dry = "dry";
    request.pattern_type = "rectangular_grid";
    request.pattern_domain = "full_projected_surface";
    request.target_method = "projected_pattern_on_diffuser";
    request.parity_group_role = "dry_reference";
    request.reference_only = true;
    request.has_reference_only = true;

    CalibrationImageSetWriteResult result;
    std::string error;
    require(
        write_calibration_image_set_json_file(output_path.string(), request, &result, &error),
        "dry reference image set should be accepted: " + error);

    const nlohmann::json image_set = read_json(output_path);
    require(
        image_set.value("artifact_schema_id", "") == kCalibrationImageSetSchemaId,
        "artifact schema id emitted");
    require(
        image_set.value("artifact_schema_version", 0) == kCalibrationImageSetSchemaVersion,
        "artifact schema version emitted");
    require(
        image_set.value("capture_stage", "") == "projected_surface_dry_reference",
        "dry reference stage emitted");
    require(image_set.value("wet_or_dry", "") == "dry", "dry state emitted");
    require(
        image_set.value("parity_group_role", "") == "dry_reference",
        "dry reference role emitted");
    require(image_set.value("reference_only", false), "dry reference is reference only");

    std::filesystem::remove_all(root);
}

void test_emits_wet_projected_surface_runtime_stack_metadata()
{
    using namespace orange::calibration;

    const std::filesystem::path root = make_temp_root();
    const std::string parity_group_id = "wet_runtime_stack_20260608T194500Z";

    CalibrationImageSetRequest wet_projection = make_request();
    wet_projection.artifact_id = "calimg_20260608_2010093_wet_projected_surface";
    wet_projection.capture_stage = "projected_surface_wet_runtime_stack";
    wet_projection.target_plane = "projected_surface";
    wet_projection.wet_or_dry = "wet";
    wet_projection.imaging_shelf_installed = true;
    wet_projection.has_imaging_shelf_installed = true;
    wet_projection.dish_installed = true;
    wet_projection.has_dish_installed = true;
    wet_projection.dish_id = "dish_A";
    wet_projection.water_fill_mm = 19.0;
    wet_projection.has_water_fill_mm = true;
    wet_projection.open_water_surface_present = true;
    wet_projection.has_open_water_surface_present = true;
    wet_projection.water_settled_status = "settled";
    wet_projection.target_method = "projected_pattern_on_diffuser";
    wet_projection.pattern_type = "circular_rings";
    wet_projection.pattern_domain = "circular_experimental_domain";
    wet_projection.matched_parity_group_id = parity_group_id;
    wet_projection.parity_group_role = "wet_projected_surface";
    wet_projection.reference_only = false;
    wet_projection.has_reference_only = true;
    wet_projection.projected_pattern = {
        {"pattern_id", "citrus_wet_projected_surface_rings_v1"},
        {"type", "circular_rings"},
        {"target_plane", "projected_surface"}
    };

    CalibrationImageSetWriteResult projection_result;
    std::string error;
    require(
        write_calibration_image_set_json_file(
            (root / "wet_projected_surface_image_set.json").string(),
            wet_projection,
            &projection_result,
            &error),
        "wet projected-surface image set should be accepted: " + error);

    const nlohmann::json projection = projection_result.image_set;
    require(
        projection.value("matched_parity_group_id", "") == parity_group_id,
        "wet projected surface parity group emitted");
    require(
        projection.value("parity_group_role", "") == "wet_projected_surface",
        "wet projected surface parity role emitted");
    require(projection.value("wet_or_dry", "") == "wet", "wet projection state emitted");
    require(
        projection.value("pattern_type", "") == "circular_rings",
        "wet projection rings metadata emitted");

    std::filesystem::remove_all(root);
}

orange::calibration::CalibrationImageSetRequest make_dry_physical_target_request()
{
    using namespace orange::calibration;

    CalibrationImageSetRequest request = make_request();
    request.artifact_id = "calimg_20260608_2010093_dry_physical_fish_height";
    request.purpose = "dry_physical_target_height_parallax_diagnostic";
    request.capture_stage = "camera_physical_fish_height";
    request.target_plane = "estimated_fish_plane";
    request.images[0].role = "physical_target";
    request.projected_pattern = nlohmann::json::object();
    request.wet_or_dry = "dry";
    request.imaging_shelf_installed = true;
    request.has_imaging_shelf_installed = true;
    request.dish_installed = true;
    request.has_dish_installed = true;
    request.fill_state = "dry_or_empty";
    request.open_water_surface_present = false;
    request.has_open_water_surface_present = true;
    request.water_settled_status = "not_applicable";
    request.target_method = "physical_target_known_xy";
    request.pattern_type = "physical_grid";
    request.pattern_domain = "circular_experimental_domain";
    request.matched_parity_group_id = "camera_physical_planes_20260608T194500Z";
    request.parity_group_role = "physical_fish_height";
    request.reference_only = true;
    request.has_reference_only = true;
    request.physical_target_used = true;
    request.has_physical_target_used = true;
    request.projected_pattern_used_as_coordinate_target = false;
    request.has_projected_pattern_used_as_coordinate_target = true;
    request.plane_id = "fish_height_physical_assumed";
    request.plane_z_mm_nominal = 9.5;
    request.has_plane_z_mm_nominal = true;
    request.plane_z_mm_uncertainty = 0.5;
    request.has_plane_z_mm_uncertainty = true;
    request.z_mm_relative_to_projection_surface = 9.5;
    request.has_z_mm_relative_to_projection_surface = true;
    request.target_id = "acrylic_hole_target_78mm_pitch5_margin3_v002";
    request.target_design = "opaque_acrylic_hole_mask_78mm_pitch5_margin3_v002";
    request.physical_target_grid_spacing_mm = 5.0;
    request.has_physical_target_grid_spacing_mm = true;
    request.physical_target_origin_definition = "center of large center marker C";
    request.physical_target_x_orientation_marker_definition =
        "positive X from C toward larger XPLUS orientation marker";
    request.physical_target = {
        {"target_id", request.target_id},
        {"target_design", request.target_design},
        {"grid_spacing_mm", request.physical_target_grid_spacing_mm},
        {"origin_definition", request.physical_target_origin_definition},
        {"x_orientation_marker_definition",
         request.physical_target_x_orientation_marker_definition}
    };
    return request;
}

void test_accepts_dry_physical_target_height_parallax_diagnostic()
{
    using namespace orange::calibration;

    const std::filesystem::path root = make_temp_root();
    const std::filesystem::path output_path =
        root / "dry_physical_target_height_parallax_image_set.json";

    CalibrationImageSetWriteResult result;
    std::string error;
    require(
        write_calibration_image_set_json_file(
            output_path.string(),
            make_dry_physical_target_request(),
            &result,
            &error),
        "dry physical target diagnostic should be accepted: " + error);

    const nlohmann::json image_set = read_json(output_path);
    require(
        image_set.value("purpose", "") ==
            "dry_physical_target_height_parallax_diagnostic",
        "dry physical target diagnostic purpose emitted");
    require(image_set.value("wet_or_dry", "") == "dry", "dry physical target state emitted");
    require(
        !image_set.value("open_water_surface_present", true),
        "dry physical target open-water flag emitted false");
    require(image_set.value("reference_only", false), "dry physical target is reference only");
    require(
        image_set.value("plane_id", "") == "fish_height_physical_assumed",
        "dry physical target plane id emitted");
    require(
        image_set.value("physical_target_used", false),
        "dry physical target flag emitted");
    require(
        !image_set.value("projected_pattern_used_as_coordinate_target", true),
        "dry physical target projected-pattern coordinate guard emitted");

    std::filesystem::remove_all(root);
}

void test_rejects_dry_physical_target_height_parallax_with_water()
{
    using namespace orange::calibration;

    CalibrationImageSetRequest request = make_dry_physical_target_request();
    request.wet_or_dry = "wet";
    request.fill_state = "recording_fill_level";
    request.open_water_surface_present = true;
    request.water_settled_status = "settled";

    std::string error;
    const bool ok = write_calibration_image_set_json_file(
        (make_temp_root() / "dry_physical_target_image_set.json").string(),
        request,
        nullptr,
        &error);
    require(!ok, "dry physical target diagnostic with water should fail");
    require(
        error.find("wet_or_dry=dry") != std::string::npos,
        "dry physical target water-state error should require dry state");
}

void test_rejects_dry_reference_without_reference_only()
{
    using namespace orange::calibration;

    CalibrationImageSetRequest request = make_request();
    request.capture_stage = "projected_surface_dry_reference";
    request.target_plane = "projected_surface";
    request.wet_or_dry = "dry";
    request.parity_group_role = "dry_reference";
    request.reference_only = false;
    request.has_reference_only = true;

    std::string error;
    const bool ok = write_calibration_image_set_json_file(
        (make_temp_root() / "dry_reference_image_set.json").string(),
        request,
        nullptr,
        &error);
    require(!ok, "dry reference without reference_only should fail");
    require(
        error.find("reference_only") != std::string::npos,
        "dry reference error should mention reference_only");
}

void test_rejects_deprecated_tank_bottom_projected_stage()
{
    using namespace orange::calibration;

    CalibrationImageSetRequest request = make_request();
    request.capture_stage = "tank_bottom_inner_surface_wet_runtime_stack";
    request.target_plane = "tank_bottom_inner_surface";
    request.wet_or_dry = "wet";
    request.matched_parity_group_id = "wet_runtime_stack_20260608T194500Z";
    request.parity_group_role = "wet_tank_bottom";

    std::string error;
    const bool ok = write_calibration_image_set_json_file(
        (make_temp_root() / "wet_tank_bottom_image_set.json").string(),
        request,
        nullptr,
        &error);
    require(!ok, "deprecated tank-bottom projected stage should fail");
    require(
        error.find("deprecated") != std::string::npos,
        "tank-bottom stage error should mention deprecation");
}

void test_rejects_projected_tank_bottom_proxy_image_set()
{
    using namespace orange::calibration;

    CalibrationImageSetRequest request = make_request();
    request.artifact_id = "calimg_20260608_2010093_tank_bottom_homography_grid";
    request.target_plane = "tank_bottom_inner_surface";
    request.projected_pattern = {
        {"pattern_id", "citrus_tank_bottom_rings_v1"},
        {"type", "circular_rings"},
        {"target_plane", "tank_bottom_inner_surface"}
    };
    request.runtime_role = {
        {"role", "behavior_plane_proxy"},
        {"behavior_plane_id", "estimated_fish_plane"},
        {"source", "fallback_to_tank_bottom_inner_surface"},
        {"authority", "citrus_decides_runtime_application"}
    };
    request.observations = {
        {"observed_domain", {
            {"shape", "circle"},
            {"source", "orange_spatial_layout_runtime:manual_fit"},
            {"target_plane", "tank_bottom_inner_surface"},
            {"coordinate_space", "camera_native_pixels"},
            {"center_px", nlohmann::json::array({2319.9, 2286.7})},
            {"radius_px", 2169.8},
            {"outer_geometry", {
                {"type", "circle"},
                {"cx", 2319.9},
                {"cy", 2286.7},
                {"r", 2169.8}
            }}
        }}
    };

    CalibrationImageSetWriteResult result;
    std::string error;
    const bool ok = write_calibration_image_set_json_file(
        (make_temp_root() / "tank_bottom_image_set.json").string(),
        request,
        &result,
        &error);
    require(!ok, "projected tank-bottom proxy image set should fail");
    require(
        error.find("projected_pattern captures at tank-bottom") != std::string::npos,
        "projected tank-bottom proxy error should mention projected-pattern deprecation");
}

void test_rejects_missing_image_checksum()
{
    using namespace orange::calibration;

    CalibrationImageSetRequest request = make_request();
    request.images[0].checksum.clear();
    std::string error;
    const bool ok = write_calibration_image_set_json_file(
        (make_temp_root() / "image_set.json").string(),
        request,
        nullptr,
        &error);
    require(!ok, "missing checksum should fail");
    require(error.find("checksum") != std::string::npos, "checksum error should be explicit");
}

} // namespace

int main()
{
    try {
        test_writer_emits_core_shape();
        test_rejects_invalid_purpose();
        test_accepts_arena_projection_purpose();
        test_accepts_verification_dots_purpose();
        test_accepts_validation_pattern_purpose();
        test_accepts_aggregate_camera_arena_set();
        test_emits_dry_reference_capture_stage_metadata();
        test_emits_wet_projected_surface_runtime_stack_metadata();
        test_accepts_dry_physical_target_height_parallax_diagnostic();
        test_rejects_dry_physical_target_height_parallax_with_water();
        test_rejects_dry_reference_without_reference_only();
        test_rejects_deprecated_tank_bottom_projected_stage();
        test_rejects_projected_tank_bottom_proxy_image_set();
        test_rejects_missing_image_checksum();
    } catch (const std::exception& ex) {
        std::cerr << "calibration_image_set_tests failed: " << ex.what() << std::endl;
        return 1;
    }
    std::cout << "calibration_image_set_tests passed" << std::endl;
    return 0;
}
