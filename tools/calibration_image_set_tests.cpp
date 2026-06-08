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
    require(image_set["capture"].value("timestamp_utc", "") == "2026-06-08T19:45:00Z", "timestamp");
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
        test_accepts_aggregate_camera_arena_set();
        test_rejects_missing_image_checksum();
    } catch (const std::exception& ex) {
        std::cerr << "calibration_image_set_tests failed: " << ex.what() << std::endl;
        return 1;
    }
    std::cout << "calibration_image_set_tests passed" << std::endl;
    return 0;
}
