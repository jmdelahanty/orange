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
    request.capture.dish_fill_state = "water_filled";
    request.capture.requires_filter_reinstalled_repeatably = true;
    request.centroid_gate_outset_px = 10.0;
    request.has_physical_inner_diameter_mm = true;
    request.physical_inner_diameter_mm = 80.0;
    request.physical_inner_diameter_source =
        "/fixture/shadow.json#dish_config.dimensions.diameter_mm";
    request.dish_design_id = "palm1";
    request.has_reference_camera_pixels_per_mm = true;
    request.reference_camera_pixels_per_mm = 3.7;
    request.reference_camera_scale_target_plane = "projected_surface";
    request.operator_boundary_target =
        orange::calibration::kDishTopRimTargetFeature;
    request.boundary_inclusion_policy =
        orange::calibration::kDishTopRimBoundaryInclusionPolicy;
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
        {"associated_image_set_artifact_id", "Cam2012632_arena_1"},
        {"spatial_layout_registration", {
            {"schema_id", "orange.spatial_layout.registration_snapshot"},
            {"schema_version", 1},
            {"authority", "orange_review_context_only"},
            {"semantics", "fixture_registration_context"},
            {"editor_parameters", {
                {"translate_x_px", 3.0},
                {"translate_y_px", -2.0},
                {"scale_px_per_layout_unit", 12.5},
                {"rotation_deg_clockwise", 1.25},
                {"edge_margin_px", 0.0},
                {"centroid_gate_outset_px", 10.0}
            }}
        }}
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
        std::filesystem::exists(artifact_dir / "overlays" / "registration_hough_overlay.png"),
        "registration/Hough overlay written");
    require(
        std::filesystem::exists(artifact_dir / "overlays" / "valid_detection_region.png"),
        "valid detection overlay written");
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
    require(
        observation.value("schema_version", 0) == kDishTopRimObservationSchemaVersion,
        "observation schema version");
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
        std::abs(observation["accepted_mask"].value("radius_px", 0.0) - 160.0) < 0.001,
        "accepted mask stores outward centroid-gate radius");
    require(
        observation["accepted_inner_rim_boundary"].value("role", "") ==
            kDishTopRimBoundaryRole,
        "accepted inner-rim boundary role");
    require(
        observation["accepted_inner_rim_boundary"].value("target_plane", "") ==
            kDishTopRimTargetPlane,
        "accepted inner-rim target plane");
    require(
        observation["accepted_inner_rim_boundary"].value("target_feature", "") ==
            kDishTopRimTargetFeature,
        "accepted inner-rim target feature");
    require(
        observation["accepted_inner_rim_boundary"].value("region", "") ==
            kDishTopRimRegion,
        "accepted inner-rim region semantics");
    require(
        std::abs(
            observation["accepted_inner_rim_boundary"]["geometry"].value("radius_px", 0.0) -
            150.0) < 0.001,
        "accepted inner-rim boundary stores non-eroded radius");
    require(
        std::abs(
            observation["accepted_inner_rim_boundary"]["physical_geometry"].value(
                "radius_mm",
                0.0) -
            40.0) < 0.001,
        "accepted inner-rim boundary stores physical radius in mm");
    require(
        std::abs(
            observation["accepted_inner_rim_boundary"]["camera_scale"].value(
                "pixels_per_mm",
                0.0) -
            3.75) < 0.001,
        "top-rim camera scale derives from pixel and physical radii");
    require(
        !observation["accepted_inner_rim_boundary"]["camera_scale"]
             ["comparison_reference"]
                 .value("authoritative_for_dish_top_rim", true),
        "projected-surface comparison scale is not authoritative at the top rim");
    require(
        std::abs(
            observation["valid_detection_region"].value(
                "centroid_gate_outset_mm",
                0.0) -
            (10.0 / 3.75)) < 0.001,
        "centroid-gate forgiveness is available in mm");
    require(
        observation["accepted_experimental_area_boundary"].value(
            "compatibility_alias",
            false),
        "legacy experimental-area field is marked as a compatibility alias");
    require(
        observation["accepted_experimental_area_boundary"].value("alias_of", "") ==
            "accepted_inner_rim_boundary",
        "legacy experimental-area field points to v2 boundary");
    require(
        !observation["accepted_experimental_area_boundary"].value(
            "asserts_citrus_acceptance",
            true),
        "legacy alias does not assert Citrus acceptance");
    require(
        observation["boundary_interpretation"].value("boundary_inclusion_policy", "") ==
            kDishTopRimBoundaryInclusionPolicy,
        "boundary inclusion policy preserved");
    require(
        observation["boundary_interpretation"].value("accepted_boundary_field", "") ==
            "accepted_inner_rim_boundary",
        "boundary interpretation selects the v2 field");
    require(
        observation["capture"].value("dish_fill_state", "") == "water_filled",
        "dish fill state preserved");
    require(
        observation["operator_review"].value("accepted", false),
        "operator review accepted");
    require(
        observation["operator_review"].value("confirmed_target_feature", "") ==
            kDishTopRimTargetFeature,
        "operator review records the confirmed target feature");
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
        observation["arena_context"]["spatial_layout_registration"].value("schema_id", "") ==
            "orange.spatial_layout.registration_snapshot",
        "observation stores spatial layout registration snapshot");
    require(
        observation["arena_context"]["spatial_layout_registration"].value("semantics", "") ==
            "fixture_registration_context",
        "observation preserves spatial layout registration semantics");
    require(
        observation["review_artifacts"].value("registration_hough_overlay_path", "") ==
            "overlays/registration_hough_overlay.png",
        "observation stores registration/Hough overlay path");
    require(
        !observation["review_artifacts"].value("registration_hough_overlay_checksum", "").empty(),
        "observation stores registration/Hough overlay checksum");
    require(
        observation["artifacts"].value("registration_hough_overlay_path", "") ==
            "overlays/registration_hough_overlay.png",
        "observation artifact block stores registration/Hough overlay path");
    require(
        !observation["artifacts"].value("registration_hough_overlay_checksum", "").empty(),
        "observation artifact block stores registration/Hough overlay checksum");
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
        image_set["rig_context"]["arena_context"]["spatial_layout_registration"].value("schema_id", "") ==
            "orange.spatial_layout.registration_snapshot",
        "image-set companion stores spatial layout registration snapshot");
    require(
        image_set["observations"]["arena_context"].value("arena_id", "") == "arena_1",
        "image-set observations stores arena context");
    require(
        image_set["observations"]["dish_top_rim"]["accepted_boundary"].value(
            "target_feature",
            "") == kDishTopRimTargetFeature,
        "image-set accepted boundary uses v2 inner-rim semantics");
    require(
        image_set["review_artifacts"]["registration_hough_overlay"].value("path", "") ==
            "overlays/registration_hough_overlay.png",
        "image-set companion stores registration/Hough overlay path");
    require(
        !image_set["review_artifacts"]["registration_hough_overlay"].value("checksum", "").empty(),
        "image-set companion stores registration/Hough overlay checksum");
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
        manifest.value("artifact_schema_version", 0) ==
            kDishTopRimObservationSchemaVersion,
        "manifest artifact schema version");
    require(
        manifest["summary"].value("target_feature", "") ==
            kDishTopRimTargetFeature,
        "manifest summary stores target feature");
    require(
        manifest["calibration_ref"].value("fingerprint", "") ==
            observation["calibration_ref"].value("fingerprint", ""),
        "manifest and observation fingerprints match");
    require(
        manifest["files"].value("image_set_json", "") == "image_set.json",
        "manifest records image-set companion");
    require(
        manifest["files"].value("registration_hough_overlay", "") ==
            "overlays/registration_hough_overlay.png",
        "manifest records registration/Hough overlay");
    require(
        !manifest["checksums"].value("registration_hough_overlay", "").empty(),
        "manifest records registration/Hough overlay checksum");
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
    require(palette["detected_circle"].value("radius", 0) == 160, "Palette radius from accepted mask");
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
        std::abs(spatial_runtime["geometry"]["valid_geometry"].value("r", 0.0) - 160.0) < 0.001,
        "spatial runtime valid radius from outward centroid gate");
    require(
        std::abs(
            spatial_runtime["geometry"].value("centroid_gate_outset_px", 0.0) -
            10.0) < 0.001,
        "spatial runtime carries explicit centroid-gate outset");
    require(
        spatial_runtime["source_observation"].value("artifact_id", "") == artifact_id,
        "spatial runtime references source observation");
    require(
        spatial_runtime["source_observation"]["accepted_inner_rim_boundary"].value(
            "target_feature",
            "") == kDishTopRimTargetFeature,
        "spatial runtime references v2 inner-rim boundary");
    orange::spatial::DishMaskRuntime parsed_runtime;
    require(
        orange::spatial::parse_dish_mask_runtime_json(spatial_runtime, &parsed_runtime, &error),
        "spatial runtime export parses with spatial schema: " + error);
    require(parsed_runtime.has_geometry, "parsed spatial runtime has geometry");
    require(
        std::abs(parsed_runtime.geometry.centroid_gate_outset_px - 10.0) < 0.001,
        "parsed runtime preserves centroid-gate outset");

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
    require(
        entry["accepted_inner_rim_boundary"].value("target_feature", "") ==
            kDishTopRimTargetFeature,
        "snapshot carries v2 inner-rim boundary");
    require(!entry.value("active_for_detection_gating", true), "snapshot does not enable gating");
    require(entry.value("gating_policy", "") == "not_enabled", "snapshot gating policy");

    nlohmann::json v1_observation = observation;
    v1_observation["schema_version"] = kDishTopRimObservationSchemaVersionV1;
    v1_observation.erase("accepted_inner_rim_boundary");
    v1_observation["accepted_experimental_area_boundary"].erase("compatibility_alias");
    v1_observation["accepted_experimental_area_boundary"].erase("alias_of");
    const nlohmann::json v1_runtime =
        dish_top_rim_spatial_dish_mask_runtime_export_to_json(v1_observation);
    require(
        std::abs(v1_runtime["geometry"]["outer_geometry"].value("r", 0.0) - 150.0) <
            0.001,
        "runtime export continues to read the v1 accepted boundary");
    const nlohmann::json v1_snapshot_entry =
        build_dish_top_rim_recording_snapshot_entry(v1_observation, false);
    require(
        std::abs(
            v1_snapshot_entry["accepted_inner_rim_boundary"]["geometry"].value(
                "radius_px",
                0.0) -
            150.0) < 0.001,
        "snapshot adapter maps a v1 boundary into the v2 semantic slot");

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

void test_nested_storage_relative_manifest_path()
{
    using namespace orange::calibration;
    const std::filesystem::path root = make_temp_root();
    DishTopRimObservationRequest request =
        make_request("dishrim_nested_20260604T120000Z_2012632");
    request.storage_relative_artifact_dir =
        "Cam2012632_arena_1/top_rim_observations/" + request.artifact_id;

    DishTopRimCircle accepted;
    accepted.center.x = 322.0;
    accepted.center.y = 254.0;
    accepted.radius_px = 150.0;

    DishTopRimObservationWriteResult result;
    std::string error;
    require(
        write_dish_top_rim_observation_artifact(
            root.string(),
            request,
            make_synthetic_dish_frame(),
            make_hough_params(),
            accepted,
            &result,
            &error),
        "nested artifact writer should succeed: " + error);

    const std::filesystem::path artifact_dir =
        root / "Cam2012632_arena_1" / "top_rim_observations" / request.artifact_id;
    require(std::filesystem::exists(artifact_dir / "manifest.json"), "nested manifest written");
    require(std::filesystem::exists(artifact_dir / "observation.json"), "nested observation written");

    const nlohmann::json manifest = read_json(artifact_dir / "manifest.json");
    require(
        manifest["storage"].value("relative_artifact_dir", "") ==
            request.storage_relative_artifact_dir,
        "manifest records nested relative artifact dir");
    require(
        manifest["storage"].value("relative_manifest_path", "") ==
            request.storage_relative_artifact_dir + "/manifest.json",
        "manifest records nested relative manifest path");

    const nlohmann::json registry = read_json(root / "index.json");
    require(
        registry["artifacts_by_id"][request.artifact_id].value("relative_manifest_path", "") ==
            request.storage_relative_artifact_dir + "/manifest.json",
        "registry preserves nested relative manifest path");

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

void test_rejects_ambiguous_v1_boundary_target()
{
    using namespace orange::calibration;
    const std::filesystem::path root = make_temp_root();
    DishTopRimObservationRequest request = make_request("dishrim_ambiguous_target");
    request.operator_boundary_target = "top_level_visible_boundary";

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
    require(!ok, "schema-v2 writer should reject the ambiguous v1 boundary target");
    require(
        error.find(kDishTopRimTargetFeature) != std::string::npos,
        "ambiguous-target error should name the required feature");
    std::filesystem::remove_all(root);
}

void test_rejects_unconfirmed_inner_rim()
{
    using namespace orange::calibration;
    const std::filesystem::path root = make_temp_root();
    DishTopRimObservationRequest request = make_request("dishrim_unconfirmed");
    request.operator_confirmed = false;

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
    require(!ok, "schema-v2 writer should reject an unconfirmed inner-rim fit");
    require(
        error.find("operator confirmation") != std::string::npos,
        "unconfirmed-fit error should explain the missing confirmation");
    std::filesystem::remove_all(root);
}

void test_rejects_simultaneous_inward_and_outward_offsets()
{
    using namespace orange::calibration;
    const std::filesystem::path root = make_temp_root();
    DishTopRimObservationRequest request = make_request("dishrim_conflicting_offsets");
    request.valid_region_erosion_px = 2.0;

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
    require(!ok, "writer should reject simultaneous inward and outward offsets");
    require(
        error.find("mutually exclusive") != std::string::npos,
        "conflicting-offset error should explain exclusivity");
    std::filesystem::remove_all(root);
}

void test_standalone_physical_registration_without_citrus()
{
    using namespace orange::calibration;
    const std::filesystem::path root = make_temp_root();
    DishTopRimObservationRequest request =
        make_request("dishrim_orange_standalone");
    request.arena_context = {
        {"camera_serial", request.camera.serial},
        {"associated_image_set_artifact_id", "Cam2012632_arena_unknown"},
        {"projection_registration", {
            {"product_id", kDailyProjectionRegistrationProductId},
            {"authority", "citrus"},
            {"status", "not_applicable"},
            {"reason", "no_active_projection_canvas"}
        }}
    };
    request.image_set_rig_context = nlohmann::json::object();
    request.has_physical_inner_diameter_mm = false;
    request.physical_inner_diameter_mm = 0.0;
    request.physical_inner_diameter_source.clear();
    request.dish_design_id.clear();
    request.has_reference_camera_pixels_per_mm = false;
    request.reference_camera_pixels_per_mm = 0.0;
    request.reference_camera_scale_target_plane.clear();
    request.capture.projector_state = "not_in_use";
    request.capture.projector_visible_to_camera = false;

    DishTopRimCircle accepted;
    accepted.center.x = 322.0;
    accepted.center.y = 254.0;
    accepted.radius_px = 150.0;
    request.has_detected_circle = true;
    request.detected_circle = accepted;
    request.detected_circle_source = "standalone_fixture_hough";

    DishTopRimObservationWriteResult result;
    std::string error;
    require(
        write_dish_top_rim_observation_artifact(
            root.string(),
            request,
            make_synthetic_dish_frame(),
            make_hough_params(),
            accepted,
            &result,
            &error),
        "standalone Orange physical registration should save without Citrus: " +
            error);

    const nlohmann::json observation =
        read_json(root / request.artifact_id / "observation.json");
    const auto& products = observation.at("registration_products");
    require(
        products.at("physical_registration").value("product_id", "") ==
            kDailyPhysicalDishRegistrationProductId,
        "physical product identity");
    require(
        products.at("physical_registration").value("status", "") ==
            "accepted",
        "physical product accepted status");
    require(
        !products.at("physical_registration").value("citrus_required", true),
        "physical product must declare Citrus unnecessary");
    require(
        products.at("projection_registration").value("status", "") ==
            "not_applicable",
        "projection status without canvas");
    require(
        products.at("projection_registration").value("reason", "") ==
            "no_active_projection_canvas",
        "projection not-applicable reason");
    require(
        observation.at("boundary_interpretation")
                .value("citrus_runtime_mapping_status", "") ==
            "not_applicable",
        "physical observation must not claim pending Citrus acceptance");

    const nlohmann::json manifest =
        read_json(root / request.artifact_id / "manifest.json");
    require(
        manifest.at("summary").value("physical_registration_status", "") ==
            "accepted",
        "manifest physical status");
    require(
        manifest.at("summary").value("citrus_runtime_mapping_status", "") ==
            "not_applicable",
        "manifest projection applicability");
    std::filesystem::remove_all(root);
}

} // namespace

int main()
{
    try {
        test_hough_detects_circle();
        test_artifact_write_and_snapshot();
        test_nested_storage_relative_manifest_path();
        test_rejects_mismatched_image_shape();
        test_rejects_non_full_source_array_role();
        test_rejects_ambiguous_v1_boundary_target();
        test_rejects_unconfirmed_inner_rim();
        test_rejects_simultaneous_inward_and_outward_offsets();
        test_standalone_physical_registration_without_citrus();
    } catch (const std::exception& ex) {
        std::cerr << "dish_top_rim_observation_tests failed: " << ex.what() << std::endl;
        return 1;
    }
    std::cout << "dish_top_rim_observation_tests passed" << std::endl;
    return 0;
}
