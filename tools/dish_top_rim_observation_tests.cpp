#include "dish_top_rim_observation.h"

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
    request.capture.projector_visible_to_camera = true;
    request.capture.exposure_us = 500000.0;
    request.capture.frame_rate_hz = 1.0;
    request.capture.requires_filter_reinstalled_repeatably = true;
    request.valid_region_erosion_px = 10.0;
    request.operator_confirmed = true;
    request.runtime_verification.status = "unknown";
    request.runtime_verification.reason = "runtime_850nm_rim_not_verified";
    request.software.orange_git_commit = "testcommit";
    request.software.orange_git_dirty_tracked = false;
    request.software.orange_version = "test";
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
    const DishTopRimObservationRequest request = make_request(artifact_id);
    const cv::Mat image = make_synthetic_dish_frame();

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
            image,
            make_hough_params(),
            accepted,
            &result,
            &error),
        "artifact writer should succeed: " + error);

    const std::filesystem::path artifact_dir = root / artifact_id;
    require(std::filesystem::exists(artifact_dir / "manifest.json"), "manifest written");
    require(std::filesystem::exists(artifact_dir / "observation.json"), "observation written");
    require(std::filesystem::exists(artifact_dir / "captures" / "source_frame.png"), "source frame written");
    require(std::filesystem::exists(artifact_dir / "overlays" / "top_rim_fit.png"), "review overlay written");
    require(
        std::filesystem::exists(artifact_dir / "exports" / "palette_dish_mask_v2.json"),
        "Palette adapter JSON written");

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
        observation["runtime_verification"].value("status", "") == "unknown",
        "runtime verification preserved");

    const nlohmann::json manifest = read_json(artifact_dir / "manifest.json");
    require(
        manifest.value("artifact_schema_id", "") == kDishTopRimObservationSchemaId,
        "manifest artifact schema id");
    require(
        manifest["calibration_ref"].value("fingerprint", "") ==
            observation["calibration_ref"].value("fingerprint", ""),
        "manifest and observation fingerprints match");

    const nlohmann::json palette = read_json(artifact_dir / "exports" / "palette_dish_mask_v2.json");
    require(palette.value("shape", "") == "circle", "Palette shape");
    require(palette["detected_circle"]["center"][0].get<int>() == 322, "Palette center x from accepted mask");
    require(palette["detected_circle"]["center"][1].get<int>() == 254, "Palette center y from accepted mask");
    require(palette["detected_circle"].value("radius", 0) == 140, "Palette radius from accepted mask");
    require(palette["metrics"]["image_shape"][0].get<int>() == 512, "Palette image height");
    require(palette["metrics"]["image_shape"][1].get<int>() == 640, "Palette image width");

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

void test_rejects_unknown_source_array_role()
{
    using namespace orange::calibration;
    const std::filesystem::path root = make_temp_root();
    DishTopRimObservationRequest request = make_request("dishrim_bad_source_array");
    request.source_array_role = "full_frame_but_not_declared";

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
    require(!ok, "writer should reject unknown source_array_role");
    require(
        error.find("source_array_role") != std::string::npos,
        "source_array_role error should be explicit");
    std::filesystem::remove_all(root);
}

} // namespace

int main()
{
    try {
        test_hough_detects_circle();
        test_artifact_write_and_snapshot();
        test_rejects_mismatched_image_shape();
        test_rejects_unknown_source_array_role();
    } catch (const std::exception& ex) {
        std::cerr << "dish_top_rim_observation_tests failed: " << ex.what() << std::endl;
        return 1;
    }
    std::cout << "dish_top_rim_observation_tests passed" << std::endl;
    return 0;
}
