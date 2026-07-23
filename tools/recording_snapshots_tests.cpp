#include "gui/recording_snapshots.h"

#include "camera.h"
#include "crop_and_encode_worker.h"
#include "crop_producer.h"
#include "fnv1a64_fingerprint.h"
#include "gui/spatial_layout/sha256.h"
#include "project.h"
#include "video_capture.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <unistd.h>

// recording_snapshots.cpp references CropAndEncodeWorker::SanitizeCropSize, whose
// production definition lives in crop_and_encode_worker.cpp next to the full
// NVENC/FFmpeg pipeline. Linking that here would drag the encoder stack into a
// pure-metadata test, so provide the same definition (the production one is a
// one-line call to the shared inline sanitizer in camera.h).
int CropAndEncodeWorker::SanitizeCropSize(int requested_size_px)
{
    return sanitize_camera_crop_size_px(requested_size_px);
}

namespace {

void require(const bool condition, const std::string& message)
{
    if (!condition) {
        throw std::runtime_error(message);
    }
}

void write_exact_fixture(const std::filesystem::path& path,
                         const std::string& bytes)
{
    std::filesystem::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) {
        throw std::runtime_error("could not write fixture " + path.string());
    }
    output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

std::string read_exact_fixture(const std::filesystem::path& path)
{
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        throw std::runtime_error("could not read fixture " + path.string());
    }
    return std::string(
        std::istreambuf_iterator<char>(input),
        std::istreambuf_iterator<char>());
}

std::string sha256_fixture(const std::filesystem::path& path)
{
    std::string checksum;
    std::string error;
    require(
        orange::gui::spatial_layout::checksum::file_sha256(
            path, &checksum, &error),
        "could not checksum fixture " + path.string() + ": " + error);
    return checksum;
}

std::string fnv1a64_fixture(const std::filesystem::path& path)
{
    const std::string bytes = read_exact_fixture(path);
    std::uint64_t hash = UINT64_C(14695981039346656037);
    for (const unsigned char byte : bytes) {
        hash ^= static_cast<std::uint64_t>(byte);
        hash *= UINT64_C(1099511628211);
    }
    return orange::calibration::format_fnv1a64_fingerprint(hash);
}

class ScopedEnv {
public:
    explicit ScopedEnv(std::string name) : name_(std::move(name))
    {
        if (const char* value = std::getenv(name_.c_str())) {
            had_original_ = true;
            original_ = value;
        }
        unsetenv(name_.c_str());
    }

    ~ScopedEnv()
    {
        if (had_original_) {
            setenv(name_.c_str(), original_.c_str(), 1);
        } else {
            unsetenv(name_.c_str());
        }
    }

    void Set(const std::string& value)
    {
        setenv(name_.c_str(), value.c_str(), 1);
    }

    void Unset()
    {
        unsetenv(name_.c_str());
    }

private:
    std::string name_;
    bool had_original_ = false;
    std::string original_;
};

CameraParams make_camera_params()
{
    CameraParams camera_params{};
    camera_params.camera_id = 3;
    camera_params.camera_serial = "700123";
    camera_params.config_path = "/configs/cam700123.json";
    camera_params.gpu_id = 1;
    camera_params.frame_rate = 250;
    return camera_params;
}

void test_detect_model_snapshot_enabled()
{
    const CameraParams camera_params = make_camera_params();
    CameraEachSelect camera_select;
    camera_select.yolo = true;
    camera_select.yolo_model = nullptr;

    const nlohmann::json snapshot = build_gui_detect_model_snapshot(
        camera_params, camera_select, "/models/global.engine");

    require(snapshot.at("enabled").get<bool>(), "detect snapshot should be enabled");
    require(snapshot.at("source").at("ui_selected").get<bool>(),
            "detect snapshot should record ui selection");
    require(snapshot.at("source").at("camera_config_path").get<std::string>() ==
                "/configs/cam700123.json",
            "detect snapshot should propagate camera config path");
    const nlohmann::json& runtime = snapshot.at("runtime");
    require(runtime.at("worker").get<std::string>() == "YoloWorker",
            "detect snapshot should name YoloWorker");
    require(runtime.at("backend").get<std::string>() == "tensorrt",
            "enabled detect snapshot should use tensorrt backend");
    require(runtime.at("engine_path").get<std::string>() == "/models/global.engine",
            "detect snapshot should propagate the selected engine path");
    require(runtime.at("model_id").get<std::string>() ==
                build_model_id_from_path("/models/global.engine"),
            "detect snapshot model_id should match build_model_id_from_path");
    require(runtime.at("gpu_id").get<int>() == 1,
            "detect snapshot should propagate gpu_id");
}

void test_detect_model_snapshot_per_camera_override_and_disabled()
{
    const CameraParams camera_params = make_camera_params();
    CameraEachSelect camera_select;
    camera_select.yolo = true;
    camera_select.yolo_model = "/models/override.engine";

    const nlohmann::json overridden = build_gui_detect_model_snapshot(
        camera_params, camera_select, "/models/global.engine");
    require(overridden.at("runtime").at("engine_path").get<std::string>() ==
                "/models/override.engine",
            "per-camera yolo model should override the global selection");

    camera_select.yolo = false;
    const nlohmann::json disabled = build_gui_detect_model_snapshot(
        camera_params, camera_select, "/models/global.engine");
    require(!disabled.at("enabled").get<bool>(), "detect snapshot should be disabled");
    require(disabled.at("runtime").at("backend").get<std::string>() == "none",
            "disabled detect snapshot should have backend none");
    require(disabled.at("runtime").at("engine_path").get<std::string>().empty(),
            "disabled detect snapshot should have empty engine path");
    require(disabled.at("runtime").at("model_id").get<std::string>() == "none",
            "disabled detect snapshot should have model_id none");
}

void test_crop_output_snapshot_enabled()
{
    ScopedEnv pool_env("ORANGE_CROP_FRAME_POOL_SIZE");
    ScopedEnv preview_env("ORANGE_CROP_PREVIEW_MAX_FPS");
    pool_env.Set("48");
    preview_env.Set("17");

    const CameraParams camera_params = make_camera_params();
    CameraEachSelect camera_select;
    camera_select.crop_and_encode = true;

    const nlohmann::json snapshot =
        build_gui_crop_output_snapshot(camera_params, camera_select, 512);

    require(snapshot.at("schema_version").get<int>() == 1,
            "crop snapshot schema_version should be 1");
    require(snapshot.at("enabled").get<bool>(), "crop snapshot should be enabled");
    require(snapshot.at("mode").get<std::string>() == "yolo_centered_square",
            "enabled crop snapshot mode should be yolo_centered_square");
    const nlohmann::json& runtime = snapshot.at("runtime");
    require(runtime.at("worker").get<std::string>() == "CropAndEncodeWorker",
            "crop snapshot should name CropAndEncodeWorker");
    require(runtime.at("crop_size_px").get<int>() == sanitize_camera_crop_size_px(512),
            "crop snapshot should sanitize crop size");
    require(runtime.at("crop_frame_pool_size").get<int>() == 48,
            "crop snapshot should resolve pool size from environment");
    require(runtime.at("preview_max_fps").get<int>() ==
                sanitize_camera_crop_preview_max_fps(17),
            "crop snapshot should resolve preview max fps from environment");
    require(runtime.at("frame_rate").get<unsigned int>() == 250u,
            "crop snapshot should propagate frame rate");
    const nlohmann::json& files = runtime.at("files");
    require(files.at("video").get<std::string>() == "Cam700123_crop.mp4",
            "crop snapshot should derive video file name from serial");
    require(files.at("metadata").get<std::string>() == "Cam700123_crop_meta.csv",
            "crop snapshot should derive metadata file name from serial");
    require(files.at("keyframes").get<std::string>() == "Cam700123_crop_keyframe.json",
            "crop snapshot should derive keyframe file name from serial");
}

void test_crop_output_snapshot_pool_size_fallback_and_disabled()
{
    ScopedEnv pool_env("ORANGE_CROP_FRAME_POOL_SIZE");
    ScopedEnv preview_env("ORANGE_CROP_PREVIEW_MAX_FPS");
    pool_env.Set("not-a-number");

    const CameraParams camera_params = make_camera_params();
    CameraEachSelect camera_select;
    camera_select.crop_and_encode = true;

    const nlohmann::json enabled =
        build_gui_crop_output_snapshot(camera_params, camera_select, 512);
    require(enabled.at("runtime").at("crop_frame_pool_size").get<int>() ==
                CropProducer::kDefaultCropFramePoolSize,
            "invalid pool size env should fall back to the default");

    camera_select.crop_and_encode = false;
    const nlohmann::json disabled =
        build_gui_crop_output_snapshot(camera_params, camera_select, 512);
    require(!disabled.at("enabled").get<bool>(), "crop snapshot should be disabled");
    require(disabled.at("mode").get<std::string>() == "disabled",
            "disabled crop snapshot mode should be disabled");
    require(disabled.at("runtime").at("width").get<int>() == 0,
            "disabled crop snapshot width should be 0");
    require(disabled.at("runtime").at("files").is_object() &&
                disabled.at("runtime").at("files").empty(),
            "disabled crop snapshot should have no files");
}

void test_pose_model_snapshot()
{
    ScopedEnv engine_env("ORANGE_POSE_ENGINE_PATH");
    ScopedEnv skeleton_id_env("ORANGE_POSE_SKELETON_ID");
    ScopedEnv skeleton_path_env("ORANGE_POSE_SKELETON_PATH");
    engine_env.Set("/models/pose.engine");
    skeleton_id_env.Set("mouse20");
    skeleton_path_env.Set("/models/mouse20.json");

    const CameraParams camera_params = make_camera_params();
    CameraEachSelect camera_select;
    camera_select.pose = true;

    const nlohmann::json snapshot =
        build_gui_pose_model_snapshot(camera_params, camera_select);

    require(snapshot.at("enabled").get<bool>(), "pose snapshot should be enabled");
    const nlohmann::json& runtime = snapshot.at("runtime");
    require(runtime.at("worker").get<std::string>() == "PoseWorker",
            "pose snapshot should name PoseWorker");
    require(runtime.at("engine_path").get<std::string>() == "/models/pose.engine",
            "pose snapshot should propagate engine path from environment");
    require(runtime.at("model_id").get<std::string>() ==
                build_model_id_from_path("/models/pose.engine"),
            "pose snapshot model_id should match build_model_id_from_path");
    require(runtime.at("skeleton_id").get<std::string>() == "mouse20",
            "pose snapshot should propagate skeleton id from environment");
    require(runtime.at("skeleton_path").get<std::string>() == "/models/mouse20.json",
            "pose snapshot should propagate skeleton path from environment");
    require(runtime.at("files").at("perf").get<std::string>() ==
                "Cam700123_pose_perf.csv",
            "pose snapshot should derive perf file name from serial");

    camera_select.pose = false;
    const nlohmann::json disabled =
        build_gui_pose_model_snapshot(camera_params, camera_select);
    require(!disabled.at("enabled").get<bool>(), "pose snapshot should be disabled");
    require(disabled.at("runtime").at("skeleton_id").get<std::string>() == "none",
            "disabled pose snapshot skeleton_id should be none");
}

void test_spatial_calibration_artifact_resolution()
{
    require(spatial_calibration_artifact_env_name("700123") ==
                "ORANGE_SPATIAL_CALIBRATION_ARTIFACT_700123",
            "artifact env name should append the camera serial");

    ScopedEnv artifact_env("ORANGE_SPATIAL_CALIBRATION_ARTIFACT_700123");
    require(resolve_gui_spatial_calibration_artifact_path("700123").empty(),
            "unset artifact env should resolve to an empty path");

    artifact_env.Set("/artifacts/cam700123_calibration.json");
    require(resolve_gui_spatial_calibration_artifact_path("700123") ==
                "/artifacts/cam700123_calibration.json",
            "set artifact env should resolve to its value");

    require(resolve_gui_spatial_calibration_artifact_path("").empty(),
            "empty serial should resolve to an empty path");
}

void test_gui_camera_has_acquisition_work()
{
    CameraEachSelect camera_select;
    camera_select.stream_on = false;
    require(!gui_camera_has_acquisition_work(camera_select),
            "idle camera should have no acquisition work");

    camera_select.stream_on = true;
    require(gui_camera_has_acquisition_work(camera_select),
            "streaming camera should have acquisition work");

    camera_select.stream_on = false;
    camera_select.record = true;
    require(gui_camera_has_acquisition_work(camera_select),
            "recording camera should have acquisition work");

    camera_select.record = false;
    camera_select.frame_save_state = State_Write_New_Frame;
    require(gui_camera_has_acquisition_work(camera_select),
            "frame-saving camera should have acquisition work");
}

void test_citrus_runtime_geometry_unavailable_is_nonblocking()
{
    ScopedEnv socket_env("ORANGE_CITRUS_PROJECTION_SNAPSHOT_SOCKET");
    const std::string unique_suffix = std::to_string(static_cast<long long>(getpid()));
    socket_env.Set("/tmp/orange_missing_citrus_daily_" + unique_suffix + ".sock");

    const std::filesystem::path recording_folder =
        std::filesystem::temp_directory_path() /
        ("orange_recording_snapshot_daily_" + unique_suffix);
    std::filesystem::remove_all(recording_folder);
    std::filesystem::create_directories(recording_folder);
    {
        std::ofstream output(recording_folder / "recording_snapshot.json");
        output << nlohmann::json{{"schema_version", 2}}.dump(2) << '\n';
    }

    update_gui_citrus_runtime_geometry_snapshot(recording_folder.string());

    nlohmann::json snapshot;
    {
        std::ifstream input(recording_folder / "recording_snapshot.json");
        input >> snapshot;
    }
    const auto& citrus = snapshot.at("citrus_runtime_geometry");
    require(citrus.at("capture_status").get<std::string>() == "unavailable",
            "missing Citrus must be recorded explicitly as unavailable");
    require(!citrus.at("recording_blocked_by_capture_failure").get<bool>(),
            "missing Citrus must not block recording");
    require(citrus.at("daily_registration_optional").get<bool>(),
            "daily registration must remain optional");
    require(citrus.at("mode").get<std::string>() == "unknown",
            "unavailable Citrus must not fabricate a runtime mode");
    require(citrus.at("daily_registration_status").get<std::string>() ==
                "unavailable",
            "unavailable Citrus must use the unavailable daily status");

    std::filesystem::remove_all(recording_folder);
}

void test_recording_geometry_assets_materialize_exact_scoped_sources()
{
    ScopedEnv image_copy_env("ORANGE_RECORDING_GEOMETRY_COPY_IMAGES");
    const std::string suffix = std::to_string(static_cast<long long>(getpid()));
    const std::filesystem::path root =
        std::filesystem::temp_directory_path() /
        ("orange_recording_geometry_assets_" + suffix);
    std::filesystem::remove_all(root);
    const std::filesystem::path sources = root / "sources";
    const std::filesystem::path recording = root / "recording";
    const std::filesystem::path recording_with_images =
        root / "recording_with_images";
    std::filesystem::create_directories(recording);
    std::filesystem::create_directories(recording_with_images);
    write_exact_fixture(
        recording / "recording_snapshot.json",
        nlohmann::json{{"schema_version", 2}}.dump(2) + "\n");
    write_exact_fixture(
        recording_with_images / "recording_snapshot.json",
        nlohmann::json{{"schema_version", 2}}.dump(2) + "\n");

    const std::filesystem::path homography_candidate =
        sources / "homography" / "candidate.json";
    const std::filesystem::path homography_yaml =
        sources / "homography" / "homography.yml";
    const std::filesystem::path homography_overlay =
        sources / "homography" / "detection_overlay.png";
    const std::filesystem::path homography_capture =
        sources / "homography_capture.png";
    write_exact_fixture(homography_overlay, "fake homography overlay\n");
    write_exact_fixture(homography_capture, "fake homography capture\n");
    write_exact_fixture(
        homography_candidate,
        nlohmann::json{
            {"schema_id", "citrus.calibration.homography_candidate"},
            {"debug_outputs", {{"detection_overlay", "detection_overlay.png"}}},
            {"source", {{"image_path", homography_capture.string()}}},
        }.dump(2) + "\n");
    write_exact_fixture(homography_yaml, "homography_matrix: [1, 0, 0]\n");
    const std::filesystem::path homography_active =
        sources / "homography_active.json";
    const nlohmann::json homography_active_json = {
        {"schema_id", "citrus.calibration.active_homography"},
        {"schema_version", 1},
        {"status", "accepted"},
        {"candidate_json_path", homography_candidate.string()},
        {"candidate_json_checksum", sha256_fixture(homography_candidate)},
        {"homography_yaml_path", homography_yaml.string()},
        {"homography_yaml_checksum", sha256_fixture(homography_yaml)},
    };
    write_exact_fixture(
        homography_active, homography_active_json.dump(2) + "  \n");

    const std::filesystem::path scale_candidate =
        sources / "scale" / "candidate.json";
    const std::filesystem::path scale_overlay =
        sources / "scale" / "overlay.png";
    const std::filesystem::path scale_capture =
        sources / "scale_capture.png";
    const std::filesystem::path scale_observation =
        sources / "scale" / "observation.json";
    write_exact_fixture(scale_candidate, nlohmann::json{
        {"schema_id", "citrus.calibration.projected_surface_scale_candidate"},
    }.dump(2) + "\n");
    write_exact_fixture(scale_overlay, "fake scale overlay\n");
    write_exact_fixture(scale_capture, "fake scale capture\n");
    write_exact_fixture(
        scale_observation,
        nlohmann::json{
            {"schema_id", "orange.calibration.projected_surface_scale_observation"},
            {"artifact_paths", {{"overlay_png", scale_overlay.string()}}},
            {"source_capture", {{"image_path", scale_capture.string()}}},
        }.dump(2) + "\n");
    const std::filesystem::path scale_active = sources / "scale_active.json";
    const nlohmann::json scale_active_json = {
        {"schema_id", "citrus.calibration.active_projected_surface_scale"},
        {"schema_version", 1},
        {"status", "accepted"},
        {"candidate_json_path", scale_candidate.string()},
        {"candidate_json_checksum", sha256_fixture(scale_candidate)},
        {"source_observation", {
            {"path", scale_observation.string()},
            {"sha256", sha256_fixture(scale_observation)},
        }},
    };
    write_exact_fixture(scale_active, scale_active_json.dump(2) + "\n");

    const std::filesystem::path spatial = sources / "spatial_cam1";
    write_exact_fixture(
        spatial / "manifest.json",
        nlohmann::json{
            {"schema_id", "orange.calibration.manifest"},
            {"summary", {
                {"camera_serial", "cam1"},
                {"arena_id", "arena_1"},
            }},
        }.dump(2) + "\n");
    write_exact_fixture(spatial / "measurement.json", "{\"measurement\":1}\n");
    write_exact_fixture(
        spatial / "arena_layout_runtime.json", "{\"arena_runtime\":1}\n");
    write_exact_fixture(
        spatial / "dish_mask_runtime.json", "{\"dish_runtime\":1}\n");
    write_exact_fixture(spatial / "accepted_overlay.png", "fake rim overlay\n");

    const std::filesystem::path tank_design = sources / "palm1.json";
    write_exact_fixture(
        tank_design,
        nlohmann::json{
            {"schema_id", "citrus.tank_design_spec"},
            {"schema_version", 1},
            {"tank_design_id", "palm1"},
            {"dimensions", {{"inner_diameter_mm", 80.0}}},
        }.dump(2) + "\n");

    const std::filesystem::path daily_selection =
        sources / "daily_registration" / "runtime_selection.json";
    const std::filesystem::path daily_registration =
        sources / "daily_registration" / "registration.json";
    const std::filesystem::path daily_candidate =
        sources / "daily_registration" / "candidate.json";
    const std::filesystem::path rim_root =
        sources / "daily_registration" / "rim_cam1";
    const std::filesystem::path rim_observation = rim_root / "observation.json";
    const std::filesystem::path rim_manifest = rim_root / "manifest.json";
    const std::filesystem::path rim_image_set = rim_root / "image_set.json";
    const std::filesystem::path rim_spatial_export =
        rim_root / "exports" / "spatial_dish_mask_runtime_v1.json";
    const std::filesystem::path rim_palette_export =
        rim_root / "exports" / "palette_dish_mask_v2.json";
    const std::filesystem::path rim_review =
        rim_root / "overlays" / "top_rim_fit.png";
    const std::filesystem::path rim_valid =
        rim_root / "overlays" / "valid_detection_region.png";
    const std::filesystem::path rim_hough =
        rim_root / "overlays" / "registration_hough_overlay.png";
    const std::filesystem::path rim_source =
        rim_root / "captures" / "source_frame.png";
    write_exact_fixture(daily_selection, "{\"mode\":\"selected_daily_registration\"}\n");
    write_exact_fixture(daily_registration, "{\"registration_id\":\"dailyreg1\"}\n");
    write_exact_fixture(daily_candidate, "{\"candidate_id\":\"dailycandidate1\"}\n");
    const nlohmann::json accepted_inner_rim = {
        {"coordinate_space", "camera_native_pixels"},
        {"target_plane", "dish_top_rim"},
        {"geometry", {
            {"type", "circle"},
            {"center_px", {{"x", 2200.5}, {"y", 2210.25}}},
            {"radius_px", 2100.0}}}};
    const nlohmann::json accepted_mask = {
        {"shape", "circle"},
        {"coordinate_space", "camera_native_pixels"},
        {"center_px", {{"x", 2200.5}, {"y", 2210.25}}},
        {"radius_px", 2117.0}};
    const nlohmann::json valid_detection_region = {
        {"coordinate_space", "camera_native_pixels"},
        {"purpose", "bounding_box_centroid_detection_gating"},
        {"offset_direction", "outward"},
        {"geometry", {
            {"type", "circle"},
            {"center_px", {{"x", 2200.5}, {"y", 2210.25}}},
            {"radius_px", 2117.0}}}};
    write_exact_fixture(rim_observation, nlohmann::json{
        {"schema_id", "orange.calibration.dish_top_rim_observation"},
        {"schema_version", 2},
        {"artifact_id", "dishrim_cam1"},
        {"camera", {{"serial", "cam1"}, {"width", 4512}, {"height", 4512}}},
        {"accepted_inner_rim_boundary", accepted_inner_rim},
        {"accepted_mask", accepted_mask},
        {"valid_detection_region", valid_detection_region},
        {"operator_review", {{"accepted", true}}},
    }.dump(2) + "\n");
    write_exact_fixture(rim_image_set, "{\"purpose\":\"dish_top_rim_observation\"}\n");
    write_exact_fixture(rim_spatial_export, "{\"enabled\":true,\"schema_version\":1}\n");
    write_exact_fixture(rim_palette_export, "{\"version\":\"2.0\",\"shape\":\"circle\"}\n");
    write_exact_fixture(rim_review, "fake top rim review\n");
    write_exact_fixture(rim_valid, "fake valid detection overlay\n");
    write_exact_fixture(rim_hough, "fake hough overlay\n");
    write_exact_fixture(rim_source, "fake source frame\n");
    write_exact_fixture(rim_manifest, nlohmann::json{
        {"schema_id", "orange.calibration.manifest"},
        {"artifact_id", "dishrim_cam1"},
        {"files", {
            {"image_set_json", "image_set.json"},
            {"spatial_dish_mask_runtime_v1",
             "exports/spatial_dish_mask_runtime_v1.json"},
            {"palette_dish_mask_v2", "exports/palette_dish_mask_v2.json"}}},
    }.dump(2) + "\n");
    write_exact_fixture(sources / "unrelated.json", "{\"must_not_copy\":true}\n");

    const nlohmann::json daily_camera_contract = {
        {"camera_serial", "cam1"},
        {"arena_id", "arena_1"},
        {"status", "resolved"},
        {"rim_observation", {
            {"source_path", rim_observation.string()},
            {"sha256", sha256_fixture(rim_observation)}}},
        {"compact_artifacts", {
            {"manifest", {
                {"source_path", rim_manifest.string()},
                {"sha256", sha256_fixture(rim_manifest)}}},
            {"image_set", {
                {"source_path", rim_image_set.string()},
                {"sha256", sha256_fixture(rim_image_set)}}},
            {"spatial_dish_mask_runtime_v1", {
                {"source_path", rim_spatial_export.string()},
                {"sha256", sha256_fixture(rim_spatial_export)}}},
            {"palette_dish_mask_v2", {
                {"source_path", rim_palette_export.string()},
                {"sha256", sha256_fixture(rim_palette_export)}}}}},
        {"optional_evidence", {
            {"review_overlay", {
                {"source_path", rim_review.string()},
                {"declared_checksum", fnv1a64_fixture(rim_review)}}},
            {"valid_detection_overlay", {
                {"source_path", rim_valid.string()},
                {"declared_checksum", fnv1a64_fixture(rim_valid)}}},
            {"registration_hough_overlay", {
                {"source_path", rim_hough.string()},
                {"declared_checksum", fnv1a64_fixture(rim_hough)}}},
            {"source_frame", {
                {"source_path", rim_source.string()},
                {"declared_checksum", fnv1a64_fixture(rim_source)}}}}},
        {"recording_snapshot_entry", {
            {"artifact_id", "dishrim_cam1"},
            {"artifact_schema_id",
             "orange.calibration.dish_top_rim_observation"},
            {"artifact_schema_version", 2},
            {"camera_serial", "cam1"},
            {"arena_id", "arena_1"},
            {"coordinate_space", "camera_native_pixels"},
            {"accepted_inner_rim_boundary", accepted_inner_rim},
            {"accepted_mask", accepted_mask},
            {"valid_detection_region", valid_detection_region},
            {"source", {
                {"path", rim_observation.string()},
                {"sha256", sha256_fixture(rim_observation)}}},
            {"available_for_downstream_detection_gating", true},
            {"active_in_orange_live_detection_pipeline", false},
            {"gating_semantics",
             "bounding_box_centroid_inside_valid_detection_region"}}}};
    const nlohmann::json daily_geometry_contract = {
        {"schema_id", "orange.recording.daily_registration_geometry"},
        {"schema_version", 1},
        {"status", "selected_resolved"},
        {"mode", "selected_daily_registration"},
        {"registration_id", "dailyreg1"},
        {"runtime_selection", {
            {"source_path", daily_selection.string()},
            {"sha256", sha256_fixture(daily_selection)}}},
        {"registration", {
            {"source_path", daily_registration.string()},
            {"sha256", sha256_fixture(daily_registration)}}},
        {"candidate", {
            {"source_path", daily_candidate.string()},
            {"sha256", sha256_fixture(daily_candidate)}}},
        {"cameras", {{"cam1", daily_camera_contract}}}};

    const nlohmann::json contract = {
        {"schema_id", "orange.recording.geometry_contract"},
        {"schema_version", 1},
        {"captured_at_utc", "2026-07-21T12:00:00Z"},
        {"status", "resolved"},
        {"recording_policy", {{"recording_blocked", false}}},
        {"selection", {{"selected_canvas_name", "shadow"}}},
        {"sources", nlohmann::json::object()},
        {"daily_registration_geometry", daily_geometry_contract},
        {"cameras", {{"cam1", {
            {"camera_serial", "cam1"},
            {"arena_id", "arena_1"},
            {"status", "resolved"},
            {"tank_design", {{"tank_design_id", "palm1"}}},
            {"projection_geometry", {
                {"status", "resolved"},
                {"homography", {
                    {"source_path", homography_active.string()},
                    {"source_sha256", sha256_fixture(homography_active)},
                    {"active_pointer_snapshot", homography_active_json},
                }},
                {"scale_models", {{"projected_surface", {
                    {"source_path", scale_active.string()},
                    {"source_sha256", sha256_fixture(scale_active)},
                    {"active_pointer_snapshot", scale_active_json},
                }}}},
            }},
            {"orange_spatial_calibration", {
                {"status", "resolved"},
                {"source_artifact_dir", spatial.string()},
                {"runtime", {{"dish_mask", {{"status", "resolved"}}}}},
            }},
        }}}},
        {"tank_designs", {{"palm1", {
            {"status", "resolved"},
            {"artifact", {
                {"source_path", tank_design.string()},
                {"sha256", sha256_fixture(tank_design)},
                {"snapshot", nlohmann::json::object()},
            }},
        }}}},
        {"warnings", nlohmann::json::array()},
        {"errors", nlohmann::json::array()},
    };

    std::string write_error;
    require(
        write_recording_geometry_contract(
            recording.string(), contract, &write_error),
        "compact geometry asset materialization should succeed: " + write_error);
    const nlohmann::json written_contract = nlohmann::json::parse(
        read_exact_fixture(recording / "recording_geometry_contract.json"));
    const nlohmann::json compact_reference =
        written_contract.at("materialized_assets");
    require(compact_reference.at("status") == "complete",
            "all compact geometry assets should materialize completely");
    const nlohmann::json compact_manifest = nlohmann::json::parse(
        read_exact_fixture(recording / "recording_geometry_assets" / "manifest.json"));
    require(compact_manifest.at("materialized_file_count") == 19,
            "compact bundle should contain static geometry plus eight daily-registration files");
    require(compact_manifest.at("optional_image_evidence_status") == "not_requested",
            "large image evidence should be off by default");
    require(!std::filesystem::exists(
                recording / "recording_geometry_assets" / "cameras" /
                "Camcam1" / "spatial" / "evidence"),
            "default materialization should not copy optional image evidence");
    require(
        read_exact_fixture(
            recording / "recording_geometry_assets" / "cameras" /
            "Camcam1" / "projection" / "homography_active.json") ==
            read_exact_fixture(homography_active),
        "materialized homography pointer must preserve exact source bytes");
    require(read_exact_fixture(
                recording / "recording_geometry_assets" / "tank_designs" /
                "palm1.json") == read_exact_fixture(tank_design),
            "materialized tank definition must preserve exact source bytes");
    require(read_exact_fixture(
                recording / "recording_geometry_assets" / "cameras" /
                "Camcam1" / "daily_registration" / "rim_observation" /
                "observation.json") == read_exact_fixture(rim_observation),
            "recording must retain exact schema-v2 daily rim observation bytes");
    for (const auto& file : compact_manifest.at("files")) {
        require(file.at("source_path") !=
                    (sources / "unrelated.json").string(),
                "unreferenced source files must not enter the recording bundle");
    }
    nlohmann::json snapshot = nlohmann::json::parse(
        read_exact_fixture(recording / "recording_snapshot.json"));
    require(snapshot.at("recording_geometry_contract").at(
                "materialized_assets").at("sha256") ==
                compact_reference.at("sha256"),
            "recording snapshot should carry the exact asset-manifest reference");
    const auto& snapshot_rim = snapshot.at("calibrations").at("cam1").at(
        "dish_top_rim_observation");
    require(snapshot_rim.at("valid_detection_region").at("geometry").at(
                "radius_px") == 2117.0,
            "recording snapshot must directly expose the outward centroid gate");
    require(snapshot_rim.at("recording_local_assets").at(
                "observation_relative_path") ==
                "recording_geometry_assets/cameras/Camcam1/daily_registration/rim_observation/observation.json",
            "snapshot mask must point to its recording-local exact-byte observation");
    const std::string first_contract_bytes = read_exact_fixture(
        recording / "recording_geometry_contract.json");
    require(write_recording_geometry_contract(
                recording.string(), contract, &write_error),
            "an identical retry should safely reuse the verified asset bundle");
    require(read_exact_fixture(recording / "recording_geometry_contract.json") ==
                first_contract_bytes,
            "an identical retry must preserve the immutable contract bytes");

    image_copy_env.Set("1");
    require(write_recording_geometry_contract(
                recording_with_images.string(), contract, &write_error),
            "opt-in image evidence materialization should succeed: " + write_error);
    const nlohmann::json image_manifest = nlohmann::json::parse(
        read_exact_fixture(
            recording_with_images / "recording_geometry_assets" / "manifest.json"));
    require(image_manifest.at("optional_image_evidence_status") == "complete",
            "all requested image evidence should materialize");
    require(image_manifest.at("optional_requested_file_count").get<int>() >= 5,
            "image evidence should include calibration captures and overlays");
    require(std::filesystem::exists(
                recording_with_images / "recording_geometry_assets" /
                "cameras" / "Camcam1" / "spatial" / "evidence" /
                "accepted_overlay.png"),
            "opt-in materialization should copy the spatial evidence image");
    require(std::filesystem::exists(
                recording_with_images / "recording_geometry_assets" /
                "cameras" / "Camcam1" / "daily_registration" /
                "rim_observation" / "evidence" /
                "valid_detection_region.png"),
            "opt-in materialization should copy daily mask review evidence");
    bool found_fnv_verified_daily_overlay = false;
    for (const auto& file : image_manifest.at("files")) {
        if (file.at("role") == "daily_rim_valid_detection_overlay") {
            found_fnv_verified_daily_overlay =
                file.at("declared_checksum_algorithm") == "fnv1a64" &&
                file.at("declared_checksum_verified").get<bool>();
        }
    }
    require(found_fnv_verified_daily_overlay,
            "daily review evidence must verify its declared FNV-1a checksum");

    const nlohmann::json live_targets = nlohmann::json::array({{
        {"camera_id", "cam1"}, {"arena_id", "arena_1"},
        {"registration_path", daily_registration.string()},
        {"registration_sha256", sha256_fixture(daily_registration)},
        {"applied", true}}});
    const nlohmann::json live_daily_runtime = {
        {"mode", "selected_daily_registration"},
        {"daily_registration_status", "selected_valid"},
        {"targets", live_targets}};
    const nlohmann::json live_runtime = {
        {"schema_id", "orange.recording.citrus_runtime_geometry"},
        {"schema_version", 1},
        {"capture_status", "captured"},
        {"daily_registration", {{"runtime", live_daily_runtime}}}};
    require(update_recording_snapshot_citrus_runtime_geometry(
                recording.string(), live_runtime),
            "runtime geometry should update the direct registered-mask view");
    snapshot = nlohmann::json::parse(
        read_exact_fixture(recording / "recording_snapshot.json"));
    require(snapshot.at("calibrations").at("cam1").at(
                "dish_top_rim_observation").at(
                "citrus_runtime_application").at(
                "selected_daily_registration_applied_by_citrus").get<bool>(),
            "exact live registration identity must mark the persisted mask applied");
    require(snapshot.at("citrus_runtime_geometry").at(
                "registered_dish_masks").at("cam1").at(
                "valid_detection_region").at("geometry").at(
                "radius_px") == 2117.0,
            "runtime snapshot must expose the exact per-camera registered mask");

    std::filesystem::remove_all(root);
}

void test_recording_geometry_asset_failure_is_nonblocking()
{
    ScopedEnv image_copy_env("ORANGE_RECORDING_GEOMETRY_COPY_IMAGES");
    const std::filesystem::path root =
        std::filesystem::temp_directory_path() /
        ("orange_recording_geometry_asset_failure_" +
         std::to_string(static_cast<long long>(getpid())));
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root / "recording");
    write_exact_fixture(
        root / "recording" / "recording_snapshot.json",
        nlohmann::json{{"schema_version", 2}}.dump(2) + "\n");
    const std::filesystem::path tank = root / "palm1.json";
    write_exact_fixture(tank, "{\"tank_design_id\":\"palm1\"}\n");
    const nlohmann::json contract = {
        {"schema_id", "orange.recording.geometry_contract"},
        {"schema_version", 1},
        {"status", "resolved"},
        {"recording_policy", {{"recording_blocked", false}}},
        {"cameras", nlohmann::json::object()},
        {"tank_designs", {{"palm1", {
            {"status", "resolved"},
            {"artifact", {
                {"source_path", tank.string()},
                {"sha256", "sha256:" + std::string(64, '0')},
            }},
        }}}},
        {"warnings", nlohmann::json::array()},
    };
    std::string error;
    require(write_recording_geometry_contract(
                (root / "recording").string(), contract, &error),
            "asset checksum failure must not block contract persistence: " + error);
    const nlohmann::json written = nlohmann::json::parse(read_exact_fixture(
        root / "recording" / "recording_geometry_contract.json"));
    require(written.at("status") == "resolved",
            "asset-copy failure must not rewrite numerical geometry status");
    require(written.at("materialized_assets").at("status") == "partial",
            "asset-copy failure should be explicit as a partial bundle");
    const nlohmann::json manifest = nlohmann::json::parse(read_exact_fixture(
        root / "recording" / "recording_geometry_assets" / "manifest.json"));
    require(manifest.at("required_failure_count") == 1,
            "checksum failure should be counted in the asset manifest");
    require(manifest.at("materialized_file_count") == 0,
            "checksum-mismatched source bytes must not be copied");
    require(!written.at("recording_policy").at("recording_blocked").get<bool>(),
            "optional geometry assets must preserve non-blocking recording policy");

    const std::filesystem::path malformed_recording = root / "malformed_recording";
    std::filesystem::create_directories(malformed_recording);
    write_exact_fixture(
        malformed_recording / "recording_snapshot.json",
        nlohmann::json{{"schema_version", 2}}.dump(2) + "\n");
    nlohmann::json malformed_contract = contract;
    malformed_contract["tank_designs"]["palm1"]["artifact"]["source_path"] = 17;
    require(write_recording_geometry_contract(
                malformed_recording.string(), malformed_contract, &error),
            "malformed optional asset metadata must not block contract persistence");
    const nlohmann::json malformed_written = nlohmann::json::parse(
        read_exact_fixture(
            malformed_recording / "recording_geometry_contract.json"));
    require(malformed_written.at("materialized_assets").at("status") ==
                "unavailable",
            "materialization exceptions should become explicit unavailable status");
    require(malformed_written.at("materialized_assets").at("reason") ==
                "materialization_exception",
            "materialization exception status should retain its reason");
    std::filesystem::remove_all(root);
}

void test_recording_geometry_contract_is_always_written()
{
    ScopedEnv explicit_canvas("ORANGE_CITRUS_RECORDING_CANVAS_CONFIG_PATH");
    ScopedEnv guided_canvas("ORANGE_GUI_GUIDED_CAPTURE_CITRUS_CONFIG_PATH");
    ScopedEnv centering_canvas("ORANGE_GUI_ARENA_CENTERING_CITRUS_CONFIG_PATH");
    const std::string unique_suffix = std::to_string(static_cast<long long>(getpid()));
    const std::filesystem::path recording_folder =
        std::filesystem::temp_directory_path() /
        ("orange_recording_geometry_" + unique_suffix);
    std::filesystem::remove_all(recording_folder);
    std::filesystem::create_directories(recording_folder);
    {
        std::ofstream output(recording_folder / "recording_snapshot.json");
        output << nlohmann::json{{"schema_version", 2}}.dump(2) << '\n';
    }
    CameraParams camera = make_camera_params();
    CameraEachSelect selection;
    selection.stream_on = true;
    update_gui_recording_geometry_contract(
        recording_folder.string(), "", &camera, &selection, 1);

    nlohmann::json contract;
    {
        std::ifstream input(recording_folder / "recording_geometry_contract.json");
        input >> contract;
    }
    require(contract.at("schema_id") == "orange.recording.geometry_contract",
            "recording geometry contract should use its stable schema id");
    require(contract.at("status") == "not_configured",
            "Orange-only run should explicitly record missing optional Citrus selection");
    require(!contract.at("recording_policy").at("recording_blocked").get<bool>(),
            "missing optional Citrus selection must never block recording");

    nlohmann::json snapshot;
    {
        std::ifstream input(recording_folder / "recording_snapshot.json");
        input >> snapshot;
    }
    const auto& reference = snapshot.at("recording_geometry_contract");
    require(reference.at("relative_path") == "recording_geometry_contract.json",
            "recording snapshot should reference the local immutable contract");
    require(reference.at("sha256").get<std::string>().rfind("sha256:", 0) == 0,
            "recording snapshot should checksum the exact contract bytes");
    require(contract.at("materialized_assets").at("status") == "empty",
            "a recording without geometry sources should publish an empty asset manifest");
    require(std::filesystem::exists(
                recording_folder / "recording_geometry_assets" / "manifest.json"),
            "the recording-local geometry asset manifest should always be discoverable");
    std::filesystem::remove_all(recording_folder);
}

}  // namespace

int main()
{
    struct NamedTest {
        const char* name;
        void (*fn)();
    };

    const NamedTest tests[] = {
        {"detect_model_snapshot_enabled", &test_detect_model_snapshot_enabled},
        {"detect_model_snapshot_per_camera_override_and_disabled",
         &test_detect_model_snapshot_per_camera_override_and_disabled},
        {"crop_output_snapshot_enabled", &test_crop_output_snapshot_enabled},
        {"crop_output_snapshot_pool_size_fallback_and_disabled",
         &test_crop_output_snapshot_pool_size_fallback_and_disabled},
        {"pose_model_snapshot", &test_pose_model_snapshot},
        {"spatial_calibration_artifact_resolution",
         &test_spatial_calibration_artifact_resolution},
        {"gui_camera_has_acquisition_work", &test_gui_camera_has_acquisition_work},
        {"citrus_runtime_geometry_unavailable_is_nonblocking",
         &test_citrus_runtime_geometry_unavailable_is_nonblocking},
        {"recording_geometry_assets_materialize_exact_scoped_sources",
         &test_recording_geometry_assets_materialize_exact_scoped_sources},
        {"recording_geometry_asset_failure_is_nonblocking",
         &test_recording_geometry_asset_failure_is_nonblocking},
        {"recording_geometry_contract_is_always_written",
         &test_recording_geometry_contract_is_always_written},
    };

    for (const auto& test : tests) {
        try {
            test.fn();
            std::cout << "[PASS] " << test.name << "\n";
        } catch (const std::exception& e) {
            std::cerr << "[FAIL] " << test.name << ": " << e.what() << "\n";
            return EXIT_FAILURE;
        }
    }

    std::cout << "All recording snapshot tests passed.\n";
    return EXIT_SUCCESS;
}
