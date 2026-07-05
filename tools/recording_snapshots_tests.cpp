#include "gui/recording_snapshots.h"

#include "camera.h"
#include "crop_and_encode_worker.h"
#include "crop_producer.h"
#include "project.h"
#include "video_capture.h"

#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>

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
