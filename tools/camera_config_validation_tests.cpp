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

void test_rig_io_mapping_round_trip()
{
    const nlohmann::json camera_config = {
        {"rig_io", {
            {"schema_id", "orange.camera.rig_io"},
            {"schema_version", 1},
            {"connections", {{
                {"purpose", "nir_strobe_trigger"},
                {"direction", "output"},
                {"camera_line", "GPO_0"},
                {"physical_pin", 7},
                {"reference_line", "GND"},
                {"reference_pin", 8},
                {"electrical", "ttl_0_5v"},
                {"active_level", "high"},
                {"inactive_level", "low"},
                {"normal_output_mode", "Exposure"},
                {"normal_polarity", false},
                {"controlled_device", "near_infrared_strobe"},
                {"nominal_wavelength_nm", 855.0},
                {"verified", true},
                {"notes", "Campus custom strobe"}
            }}}
        }}
    };

    CameraParams params{};
    orange::camera_config::parse_rig_io_config(camera_config, &params);

    require(params.rig_io_connections.size() == 1,
            "rig_io should parse one connection");
    const CameraRigIoConnection& connection = params.rig_io_connections.front();
    require(connection.purpose == "nir_strobe_trigger",
            "rig_io purpose should load");
    require(connection.direction == "output",
            "rig_io direction should load");
    require(connection.camera_line == "GPO_0",
            "rig_io camera line should load");
    require(connection.physical_pin == 7,
            "rig_io physical pin should load");
    require(connection.reference_line == "GND",
            "rig_io reference line should load");
    require(connection.reference_pin == 8,
            "rig_io reference pin should load");
    require(connection.electrical == "ttl_0_5v",
            "rig_io electrical mode should load");
    require(connection.active_level == "high",
            "rig_io active level should load");
    require(connection.inactive_level == "low",
            "rig_io inactive level should load");
    require(connection.normal_output_mode == "Exposure",
            "rig_io normal output mode should load");
    require(!connection.normal_polarity,
            "rig_io normal polarity should load");
    require(connection.controlled_device == "near_infrared_strobe",
            "rig_io controlled device should load");
    require(connection.nominal_wavelength_nm == 855.0,
            "rig_io wavelength should load");
    require(connection.verified,
            "rig_io verified should load");
    require(connection.notes == "Campus custom strobe",
            "rig_io notes should load");

    const nlohmann::json saved = orange::camera_config::build_rig_io_config(params);
    require(saved["schema_id"].get<std::string>() == "orange.camera.rig_io",
            "rig_io builder should emit schema id");
    require(saved["schema_version"].get<int>() == 1,
            "rig_io builder should emit schema version");
    require(saved["connections"].is_array() && saved["connections"].size() == 1,
            "rig_io builder should emit one connection");
    const nlohmann::json& saved_connection = saved["connections"][0];
    require(saved_connection["purpose"].get<std::string>() == "nir_strobe_trigger",
            "rig_io purpose should round-trip");
    require(saved_connection["camera_line"].get<std::string>() == "GPO_0",
            "rig_io camera line should round-trip");
    require(saved_connection["physical_pin"].get<int>() == 7,
            "rig_io physical pin should round-trip");
    require(saved_connection["reference_line"].get<std::string>() == "GND",
            "rig_io reference line should round-trip");
    require(saved_connection["reference_pin"].get<int>() == 8,
            "rig_io reference pin should round-trip");
    require(saved_connection["normal_output_mode"].get<std::string>() == "Exposure",
            "rig_io normal output mode should round-trip");
    require(!saved_connection["normal_polarity"].get<bool>(),
            "rig_io normal polarity should round-trip");
    require(saved_connection["nominal_wavelength_nm"].get<double>() == 855.0,
            "rig_io wavelength should round-trip");
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
        {"rig_io_mapping_round_trip", &test_rig_io_mapping_round_trip},
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
