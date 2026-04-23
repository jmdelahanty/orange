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
}

void test_parse_missing_crop_pipeline_uses_default()
{
    CameraParams params = make_camera_params_with_crop_size(640);
    orange::camera_config::parse_crop_pipeline_config(nlohmann::json::object(), &params);

    require(params.crop_pipeline.crop_size_px == CameraCropPipelineConfig::kDefaultCropSizePx,
            "missing crop_pipeline should reset to the default crop size");
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
