#include "dish_top_rim_observation.h"
#include "spatial_layout_schema.h"

#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

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
        ("orange_dish_top_rim_observation_tests_" + std::to_string(static_cast<long long>(getpid())));
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root);
    return root;
}

cv::Mat make_synthetic_dish_frame()
{
    cv::Mat image(512, 640, CV_8UC1, cv::Scalar(12));
    cv::circle(image, cv::Point(321, 255), 151, cv::Scalar(235), 4, cv::LINE_AA);
    cv::circle(image, cv::Point(321, 255), 145, cv::Scalar(45), 1, cv::LINE_AA);
    return image;
}

orange::calibration::DishTopRimObservationRequest make_request(const std::string& artifact_id)
{
    orange::calibration::DishTopRimObservationRequest request;
    request.artifact_id = artifact_id;
    request.created_utc = "2026-06-04T12:00:00Z";
    request.camera.serial = "2012632";
    request.camera.name = "Cam2012632";
    request.camera.width = 640;
    request.camera.height = 512;
    request.camera.pixel_format = "Mono8";
    request.capture.operation_id = "dishrim_test_operation";
    request.capture.capture_mode = "session_local_operator_still";
    request.capture.filter_state = "removed";
    request.capture.runtime_filter_state = "850nm_bandpass_installed";
    request.capture.light_handling = "keep_or_restore_mapped_pulse";
    request.capture.light_state = "ttl_nir_strobe_active";
    request.capture.illumination_spectrum = "narrowband_nir";
    request.capture.illumination_source = "custom_ttl_nir_strobe";
    request.capture.illumination_center_wavelength_nm = 855.0;
    request.capture.has_illumination_center_wavelength_nm = true;
    request.capture.illumination_wavelength_confidence = "nominal";
    request.capture.projector_state = "projector_off";
    request.capture.projector_visible_to_camera = true;
    request.capture.exposure_us = 500000.0;
    request.capture.frame_rate_hz = 1.0;
    request.capture.requires_filter_reinstalled_repeatably = true;
    request.valid_region_erosion_px = 10.0;
    request.operator_confirmed = true;
    request.operator_notes = "fixture operator note";
    request.runtime_verification.status = "unknown";
    request.runtime_verification.reason = "runtime_850nm_rim_not_verified";
    request.software.orange_git_commit = "testcommit";
    request.software.orange_git_dirty_tracked = false;
    request.software.orange_version = "test";
    request.image_set_rig_context = {
        {"rig_id", "omnifin0"},
        {"canvas_id", "shadow"},
        {"arena_id", "arena_1"},
        {"camera_id", "2012632"},
        {"associated_image_set_artifact_id", "Cam2012632_arena_1"}
    };
    request.arena_context = {
        {"rig_id", "omnifin0"},
        {"canvas_id", "shadow"},
        {"arena_id", "arena_1"},
        {"camera_serial", "2012632"},
        {"citrus_camera_id", "2012632"},
        {"associated_image_set_artifact_id", "Cam2012632_arena_1"}
    };
    return request;
}

orange::calibration::DishTopRimHoughParams make_hough_params()
{
    orange::calibration::DishTopRimHoughParams params;
    params.dp = 1.2;
    params.min_dist_px = 200.0;
    params.param1 = 100.0;
    params.param2 = 24.0;
    params.min_radius_px = 130;
    params.max_radius_px = 170;
    return params;
}

void test_hough_detects_circle()
{
    using namespace orange::calibration;
    const cv::Mat image = make_synthetic_dish_frame();
    DishTopRimCircle detected;
    std::string error;
    require(
        detect_dish_top_rim_hough_circle(image, make_hough_params(), &detected, &error),
        "Hough detector should find synthetic circle: " + error);
    require(std::abs(detected.center.x - 321.0) < 8.0, "detected x center near fixture");
    require(std::abs(detected.center.y - 255.0) < 8.0, "detected y center near fixture");
    require(std::abs(detected.radius_px - 151.0) < 8.0, "detected radius near fixture");
}

void test_artifact_write_and_snapshot()
{
    using namespace orange::calibration;

    const std::filesystem::path root = make_temp_root();
    const std::string artifact_id =
        build_dish_top_rim_observation_artifact_id("2012632", "20260604T120000Z");
    DishTopRimObservationRequest request = make_request(artifact_id);
    const cv::Mat image = make_synthetic_dish_frame();

    request.has_detected_circle = true;
    request.detected_circle.center.x = 319.0;
    request.detected_circle.center.y = 253.0;
    request.detected_circle.radius_px = 148.0;
    request.detected_circle_source = "fixture_cached_hough_scaled_to_full_resolution";

    DishTopRimCircle accepted;
    accepted.center.x = 322.0;
    accepted.center.y = 254.0;
    accepted.radius_px = 150.0;

    DishTopRimHoughParams unusable_hough_params = make_hough_params();
    unusable_hough_params.min_radius_px = 1;
    unusable_hough_params.max_radius_px = 2;
    unusable_hough_params.param2 = 500.0;

    DishTopRimObservationWriteResult result;
    std::string error;
    require(
        write_dish_top_rim_observation_artifact(
            root.string(),
            request,
            image,
            unusable_hough_params,
            accepted,
            &result,
            &error),
        "artifact writer should succeed: " + error);

    const std::filesystem::path artifact_dir = root / artifact_id;
    require(std::filesystem::exists(artifact_dir / "manifest.json"), "manifest written");
    require(std::filesystem::exists(artifact_dir / "observation.json"), "observation written");
    require(std::filesystem::exists(artifact_dir / "image_set.json"), "image-set companion written");
    require(std::filesystem::exists(artifact_dir / "captures" / "source_frame.png"), "source frame written");
    require(std::filesystem::exists(artifact_dir / "overlays" / "top_rim_fit.png"), "review overlay written");
    require(
        std::filesystem::exists(artifact_dir / "exports" / "palette_dish_mask_v2.json"),
        "Palette adapter JSON written");
    require(
        std::filesystem::exists(artifact_dir / "exports" / "spatial_dish_mask_runtime_v1.json"),
        "spatial dish-mask runtime adapter JSON written");

    const nlohmann::json observation = read_json(artifact_dir / "observation.json");
    require(
        observation.value("schema_id", "") == kDishTopRimObservationSchemaId,
        "observation schema id");
    require(observation.value("artifact_id", "") == artifact_id, "observation artifact id");
    require(
        observation["calibration_ref"].value("artifact_id", "") == artifact_id,
        "calibration_ref artifact id");
    require(
        !observation["calibration_ref"].value("fingerprint", "").empty(),
        "calibration_ref fingerprint present");
    require(
        observation["accepted_mask"].value("coordinate_space", "") == "camera_native_pixels",
        "accepted mask coordinate space");
    require(
        observation["accepted_mask"].value("source_array_role", "") == "images_full",
        "accepted mask source array role");
    require(
        std::abs(observation["accepted_mask"].value("radius_px", 0.0) - 140.0) < 0.001,
        "accepted mask stores eroded valid radius");
    require(
        observation["operator_review"].value("accepted", false),
        "operator review accepted");
    require(
        observation["operator_review"].value("notes", "") == "fixture operator note",
        "operator notes preserved");
    require(
        observation["arena_context"].value("associated_image_set_artifact_id", "") ==
            "Cam2012632_arena_1",
        "observation stores associated image-set artifact id");
    require(
        observation["arena_context"].value("arena_id", "") == "arena_1",
        "observation stores arena id");
    require(
        observation["runtime_verification"].value("status", "") == "unknown",
        "runtime verification preserved");
    require(
        observation["capture"].value("light_handling", "") == "keep_or_restore_mapped_pulse",
        "capture light handling preserved");
    require(
        observation["capture"].value("light_state", "") == "ttl_nir_strobe_active",
        "capture light state preserved");
    require(
        observation["capture"]["illumination"].value("spectrum", "") == "narrowband_nir",
        "capture illumination spectrum preserved");
    require(
        observation["capture"]["illumination"].value("source", "") == "custom_ttl_nir_strobe",
        "capture illumination source preserved");
    require(
        std::abs(observation["capture"]["illumination"].value("center_wavelength_nm", 0.0) - 855.0) < 0.001,
        "capture illumination center wavelength preserved");
    require(
        observation["capture"]["illumination"].value("wavelength_confidence", "") == "nominal",
        "capture illumination confidence preserved");
    require(
        observation["capture"].value("projector_state", "") == "projector_off",
        "capture projector state preserved");
    require(
        observation["circle_detection"].value("detected_circle_source", "") ==
            "fixture_cached_hough_scaled_to_full_resolution",
        "cached detected circle source preserved");
    require(
        std::abs(
            observation["circle_detection"]["detected_circle"]["center_px"].value("x", 0.0) -
            319.0) < 0.001,
        "cached detected circle x preserved");
    require(
        std::abs(
            observation["circle_detection"]["detected_circle"]["center_px"].value("y", 0.0) -
            253.0) < 0.001,
        "cached detected circle y preserved");
    require(
        std::abs(
            observation["circle_detection"]["detected_circle"].value("radius_px", 0.0) -
            148.0) < 0.001,
        "cached detected circle radius preserved");

    const nlohmann::json image_set = read_json(artifact_dir / "image_set.json");
    require(
        image_set.value("schema_id", "") == orange::calibration::kCalibrationImageSetSchemaId,
        "image-set schema id");
    require(image_set.value("artifact_id", "") == artifact_id, "image-set artifact id");
    require(image_set.value("purpose", "") == "dish_top_rim", "image-set purpose");
    require(image_set.value("target_plane", "") == "dish_top_rim", "image-set target plane");
    require(
        image_set.value("coordinate_space", "") == "camera_native_pixels",
        "image-set coordinate space");
    require(image_set["camera"].value("serial", "") == "2012632", "image-set camera serial");
    require(image_set["camera"]["image_shape"].value("height", 0) == 512, "image-set height");
    require(image_set["camera"]["image_shape"].value("width", 0) == 640, "image-set width");
    require(image_set["capture"].value("timestamp_utc", "") == request.created_utc, "image-set timestamp");
    require(
        image_set["capture"].value("capture_mode", "") == request.capture.capture_mode,
        "image-set capture mode");
    require(
        image_set["capture"].value("light_handling", "") == "keep_or_restore_mapped_pulse",
        "image-set light handling");
    require(image_set["capture"].value("light_state", "") == "ttl_nir_strobe_active", "image-set light state");
    require(
        image_set["capture"]["illumination"].value("spectrum", "") == "narrowband_nir",
        "image-set illumination spectrum");
    require(
        std::abs(image_set["capture"]["illumination"].value("center_wavelength_nm", 0.0) - 855.0) < 0.001,
        "image-set illumination center wavelength");
    require(image_set["images"].size() == 1, "image-set source image count");
    require(image_set["images"][0].value("role", "") == "source", "image-set source role");
    require(
        image_set["images"][0].value("path", "") == "captures/source_frame.png",
        "image-set source path");
    require(
        image_set["images"][0].value("checksum", "") ==
            observation["artifacts"].value("source_frame_checksum", ""),
        "image-set source checksum matches observation");
    require(
        image_set["derived_artifacts"].size() == 1,
        "image-set references derived top-rim observation");
    require(
        image_set["derived_artifacts"][0].value("artifact_schema_id", "") ==
            kDishTopRimObservationSchemaId,
        "image-set derived artifact schema");
    require(
        image_set["derived_artifacts"][0].value("fingerprint", "") ==
            observation["calibration_ref"].value("fingerprint", ""),
        "image-set derived artifact fingerprint");
    require(
        image_set["rig_context"].value("canvas_id", "") == "shadow",
        "image-set rig context preserved");
    require(
        image_set["rig_context"]["arena_context"].value("associated_image_set_artifact_id", "") ==
            "Cam2012632_arena_1",
        "image-set companion stores arena context");
    require(
        image_set["observations"]["arena_context"].value("arena_id", "") == "arena_1",
        "image-set observations stores arena context");
    require(
        image_set["citrus_preview"].value("diagnostic_only", false),
        "image-set preview is diagnostic");
    require(
        image_set.value("operator_notes", "") == "fixture operator note",
        "image-set operator notes preserved");

    const nlohmann::json manifest = read_json(artifact_dir / "manifest.json");
    require(
        manifest.value("artifact_schema_id", "") == kDishTopRimObservationSchemaId,
        "manifest artifact schema id");
    require(
        manifest["calibration_ref"].value("fingerprint", "") ==
            observation["calibration_ref"].value("fingerprint", ""),
        "manifest and observation fingerprints match");
    require(
        manifest["files"].value("image_set_json", "") == "image_set.json",
        "manifest records image-set companion");
    require(
        manifest["summary"].value("associated_image_set_artifact_id", "") ==
            "Cam2012632_arena_1",
        "manifest summary stores associated image-set artifact id");
    require(
        manifest["summary"].value("arena_id", "") == "arena_1",
        "manifest summary stores arena id");

    const nlohmann::json palette = read_json(artifact_dir / "exports" / "palette_dish_mask_v2.json");
    require(palette.value("shape", "") == "circle", "Palette shape");
    require(palette["detected_circle"]["center"][0].get<int>() == 322, "Palette center x from accepted mask");
    require(palette["detected_circle"]["center"][1].get<int>() == 254, "Palette center y from accepted mask");
    require(palette["detected_circle"].value("radius", 0) == 140, "Palette radius from accepted mask");
    require(palette["metrics"]["image_shape"][0].get<int>() == 512, "Palette image height");
    require(palette["metrics"]["image_shape"][1].get<int>() == 640, "Palette image width");

    const nlohmann::json spatial_runtime =
        read_json(artifact_dir / "exports" / "spatial_dish_mask_runtime_v1.json");
    require(spatial_runtime.value("schema_version", 0) == 1, "spatial runtime schema version");
    require(spatial_runtime.value("enabled", false), "spatial runtime enabled");
    require(spatial_runtime.value("source", "") == "detected_fit", "spatial runtime source");
    require(
        spatial_runtime["geometry"].value("coordinate_space", "") == "camera_native_pixels",
        "spatial runtime coordinate space");
    require(
        std::abs(spatial_runtime["geometry"]["outer_geometry"].value("r", 0.0) - 150.0) < 0.001,
        "spatial runtime outer radius from accepted circle");
    require(
        std::abs(spatial_runtime["geometry"]["valid_geometry"].value("r", 0.0) - 140.0) < 0.001,
        "spatial runtime valid radius from eroded mask");
    require(
        spatial_runtime["source_observation"].value("artifact_id", "") == artifact_id,
        "spatial runtime references source observation");
    orange::spatial::DishMaskRuntime parsed_runtime;
    require(
        orange::spatial::parse_dish_mask_runtime_json(spatial_runtime, &parsed_runtime, &error),
        "spatial runtime export parses with spatial schema: " + error);
    require(parsed_runtime.has_geometry, "parsed spatial runtime has geometry");

    const nlohmann::json registry = read_json(root / "index.json");
    require(
        registry["latest_by_schema"].value(kDishTopRimObservationSchemaId, "") == artifact_id,
        "registry latest_by_schema points to artifact");
    require(
        registry["artifacts_by_id"][artifact_id].value("fingerprint", "") ==
            observation["calibration_ref"].value("fingerprint", ""),
        "registry stores fingerprint");

    nlohmann::json snapshot = {{"recording_id", "fixture"}};
    require(
        apply_dish_top_rim_observation_to_snapshot_json(
            &snapshot,
            "2012632",
            observation,
            false,
            &error),
        "snapshot helper should apply: " + error);
    const nlohmann::json& entry =
        snapshot["calibrations"]["2012632"]["dish_top_rim_observation"];
    require(entry.value("artifact_id", "") == artifact_id, "snapshot artifact id");
    require(!entry.value("active_for_detection_gating", true), "snapshot does not enable gating");
    require(entry.value("gating_policy", "") == "not_enabled", "snapshot gating policy");

    std::filesystem::remove_all(root);
}

void test_rejects_mismatched_image_shape()
{
    using namespace orange::calibration;
    const std::filesystem::path root = make_temp_root();
    DishTopRimObservationRequest request = make_request("dishrim_bad_shape");
    request.camera.width = 641;

    DishTopRimCircle accepted;
    accepted.center.x = 322.0;
    accepted.center.y = 254.0;
    accepted.radius_px = 150.0;

    DishTopRimObservationWriteResult result;
    std::string error;
    const bool ok = write_dish_top_rim_observation_artifact(
        root.string(),
        request,
        make_synthetic_dish_frame(),
        make_hough_params(),
        accepted,
        &result,
        &error);
    require(!ok, "writer should reject mismatched shape");
    require(
        error.find("image_shape") != std::string::npos ||
            error.find("dimensions") != std::string::npos,
        "shape mismatch should explain error");
    std::filesystem::remove_all(root);
}

void expect_rejects_source_array_role(const std::string& source_array_role)
{
    using namespace orange::calibration;
    const std::filesystem::path root = make_temp_root();
    DishTopRimObservationRequest request = make_request("dishrim_bad_source_array");
    request.source_array_role = source_array_role;

    DishTopRimCircle accepted;
    accepted.center.x = 322.0;
    accepted.center.y = 254.0;
    accepted.radius_px = 150.0;

    DishTopRimObservationWriteResult result;
    std::string error;
    const bool ok = write_dish_top_rim_observation_artifact(
        root.string(),
        request,
        make_synthetic_dish_frame(),
        make_hough_params(),
        accepted,
        &result,
        &error);
    require(!ok, "writer should reject non-full-resolution source_array_role");
    require(
        error.find("source_array_role") != std::string::npos,
        "source_array_role error should be explicit");
    std::filesystem::remove_all(root);
}

void test_rejects_non_full_source_array_role()
{
    expect_rejects_source_array_role("images_ds");
    expect_rejects_source_array_role("full_frame_but_not_declared");
}

} // namespace

int main()
{
    try {
        test_hough_detects_circle();
        test_artifact_write_and_snapshot();
        test_rejects_mismatched_image_shape();
        test_rejects_non_full_source_array_role();
    } catch (const std::exception& ex) {
        std::cerr << "dish_top_rim_observation_tests failed: " << ex.what() << std::endl;
        return 1;
    }
    std::cout << "dish_top_rim_observation_tests passed" << std::endl;
    return 0;
}
