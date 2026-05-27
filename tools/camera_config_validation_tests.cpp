#include "camera_config_schema.h"

#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {

void require(const bool condition, const std::string& message)
{
    if (!condition) {
        throw std::runtime_error(message);
    }
}

CameraParams make_camera_params_with_crop_size(const int crop_size_px)
{
    CameraParams params{};
    params.crop_pipeline.crop_size_px = crop_size_px;
    return params;
}

void test_build_emits_canonical_crop_size()
{
    const CameraParams params = make_camera_params_with_crop_size(328);
    const nlohmann::json crop_pipeline =
        orange::camera_config::build_crop_pipeline_config(params);

    require(crop_pipeline.is_object(), "crop pipeline config should be an object");
    require(crop_pipeline.contains("crop_size_px"), "crop pipeline should emit crop_size_px");
    require(crop_pipeline["crop_size_px"].get<int>() == 328,
            "crop pipeline should preserve an even configured crop size");
    require(crop_pipeline.contains("preview_max_fps"), "crop pipeline should emit preview_max_fps");
    require(crop_pipeline["preview_max_fps"].get<int>() ==
                CameraCropPipelineConfig::kDefaultPreviewMaxFps,
            "crop pipeline should emit the default preview max FPS");
}

void test_build_sanitizes_odd_crop_size()
{
    const CameraParams params = make_camera_params_with_crop_size(329);
    const nlohmann::json crop_pipeline =
        orange::camera_config::build_crop_pipeline_config(params);

    require(crop_pipeline["crop_size_px"].get<int>() == 328,
            "crop pipeline save should sanitize odd crop sizes to even values");
}

void test_parse_round_trips_canonical_crop_size()
{
    const CameraParams saved_params = make_camera_params_with_crop_size(328);
    const nlohmann::json camera_config = {
        {"crop_pipeline", orange::camera_config::build_crop_pipeline_config(saved_params)}
    };

    CameraParams loaded_params{};
    orange::camera_config::parse_crop_pipeline_config(camera_config, &loaded_params);

    require(loaded_params.crop_pipeline.crop_size_px == 328,
            "crop pipeline should round-trip crop_size_px");
    require(loaded_params.crop_pipeline.preview_max_fps ==
                CameraCropPipelineConfig::kDefaultPreviewMaxFps,
            "crop pipeline should preserve default preview_max_fps when absent");
}

void test_parse_missing_crop_pipeline_uses_default()
{
    CameraParams params = make_camera_params_with_crop_size(640);
    orange::camera_config::parse_crop_pipeline_config(nlohmann::json::object(), &params);

    require(params.crop_pipeline.crop_size_px == CameraCropPipelineConfig::kDefaultCropSizePx,
            "missing crop_pipeline should reset to the default crop size");
    require(params.crop_pipeline.preview_max_fps == CameraCropPipelineConfig::kDefaultPreviewMaxFps,
            "missing crop_pipeline should reset to the default preview max FPS");
}

void test_parse_clamps_too_small_crop_size()
{
    const nlohmann::json camera_config = {
        {"crop_pipeline", {{"crop_size_px", 1}}}
    };

    CameraParams params{};
    orange::camera_config::parse_crop_pipeline_config(camera_config, &params);

    require(params.crop_pipeline.crop_size_px == CameraCropPipelineConfig::kMinCropSizePx,
            "too-small crop sizes should clamp to the minimum");
}

void test_parse_clamps_too_large_crop_size()
{
    const nlohmann::json camera_config = {
        {"crop_pipeline", {{"crop_size_px", 999999}}}
    };

    CameraParams params{};
    orange::camera_config::parse_crop_pipeline_config(camera_config, &params);

    require(params.crop_pipeline.crop_size_px == CameraCropPipelineConfig::kMaxCropSizePx,
            "too-large crop sizes should clamp to the maximum");
}

void test_parse_legacy_size_px_alias()
{
    const nlohmann::json camera_config = {
        {"crop_pipeline", {{"size_px", 512}}}
    };

    CameraParams params{};
    orange::camera_config::parse_crop_pipeline_config(camera_config, &params);

    require(params.crop_pipeline.crop_size_px == 512,
            "legacy size_px alias should load as crop_size_px");
}

void test_parse_legacy_square_dimensions()
{
    const nlohmann::json camera_config = {
        {"crop_pipeline", {{"width", 384}, {"height", 384}}}
    };

    CameraParams params{};
    orange::camera_config::parse_crop_pipeline_config(camera_config, &params);

    require(params.crop_pipeline.crop_size_px == 384,
            "legacy square width/height should load as crop_size_px");
}

void test_parse_preview_max_fps()
{
    const nlohmann::json camera_config = {
        {"crop_pipeline", {{"crop_size_px", 256}, {"preview_max_fps", 30}}}
    };

    CameraParams params{};
    orange::camera_config::parse_crop_pipeline_config(camera_config, &params);

    require(params.crop_pipeline.preview_max_fps == 30,
            "preview_max_fps should load from crop_pipeline");
}

void test_parse_preview_max_fps_zero_unlimited()
{
    const nlohmann::json camera_config = {
        {"crop_pipeline", {{"crop_size_px", 256}, {"preview_max_fps", 0}}}
    };

    CameraParams params{};
    orange::camera_config::parse_crop_pipeline_config(camera_config, &params);

    require(params.crop_pipeline.preview_max_fps == 0,
            "preview_max_fps=0 should be preserved as unlimited mode");
}

void test_parse_preview_max_fps_negative_unlimited()
{
    const nlohmann::json camera_config = {
        {"crop_pipeline", {{"crop_size_px", 256}, {"preview_max_fps", -1}}}
    };

    CameraParams params{};
    orange::camera_config::parse_crop_pipeline_config(camera_config, &params);

    require(params.crop_pipeline.preview_max_fps == 0,
            "negative preview_max_fps should sanitize to unlimited mode");
}

void test_parse_preview_max_fps_clamps_too_large()
{
    const nlohmann::json camera_config = {
        {"crop_pipeline", {{"crop_size_px", 256}, {"preview_max_fps", 999999}}}
    };

    CameraParams params{};
    orange::camera_config::parse_crop_pipeline_config(camera_config, &params);

    require(params.crop_pipeline.preview_max_fps == CameraCropPipelineConfig::kMaxPreviewMaxFps,
            "too-large preview_max_fps should clamp to the maximum");
}

}  // namespace

int main()
{
    struct TestCase {
        const char* name;
        void (*fn)();
    };

    const TestCase tests[] = {
        {"build_emits_canonical_crop_size", &test_build_emits_canonical_crop_size},
        {"build_sanitizes_odd_crop_size", &test_build_sanitizes_odd_crop_size},
        {"parse_round_trips_canonical_crop_size", &test_parse_round_trips_canonical_crop_size},
        {"parse_missing_crop_pipeline_uses_default", &test_parse_missing_crop_pipeline_uses_default},
        {"parse_clamps_too_small_crop_size", &test_parse_clamps_too_small_crop_size},
        {"parse_clamps_too_large_crop_size", &test_parse_clamps_too_large_crop_size},
        {"parse_legacy_size_px_alias", &test_parse_legacy_size_px_alias},
        {"parse_legacy_square_dimensions", &test_parse_legacy_square_dimensions},
        {"parse_preview_max_fps", &test_parse_preview_max_fps},
        {"parse_preview_max_fps_zero_unlimited", &test_parse_preview_max_fps_zero_unlimited},
        {"parse_preview_max_fps_negative_unlimited", &test_parse_preview_max_fps_negative_unlimited},
        {"parse_preview_max_fps_clamps_too_large", &test_parse_preview_max_fps_clamps_too_large},
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

    std::cout << "All camera config validation tests passed.\n";
    return EXIT_SUCCESS;
}
