#include "gui/guided_capture_autorun.h"

#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>

namespace {

void require(bool condition, const std::string& message)
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

private:
    std::string name_;
    bool had_original_ = false;
    std::string original_;
};

void test_defaults()
{
    ScopedEnv enabled("ORANGE_GUI_GUIDED_CAPTURE_AUTORUN");
    ScopedEnv profile("ORANGE_GUI_GUIDED_CAPTURE_PROFILE");
    ScopedEnv recipe("ORANGE_GUI_GUIDED_CAPTURE_RECIPE");
    ScopedEnv purpose("ORANGE_GUI_GUIDED_CAPTURE_PURPOSE");
    ScopedEnv cameras("ORANGE_GUI_GUIDED_CAPTURE_CAMERAS");
    ScopedEnv result("ORANGE_GUI_GUIDED_CAPTURE_RESULT_JSON");
    ScopedEnv preflight("ORANGE_GUI_GUIDED_CAPTURE_APPLY_CALIBRATION_PREFLIGHT");
    ScopedEnv frame_rate("ORANGE_GUI_GUIDED_CAPTURE_FRAME_RATE_HZ");
    ScopedEnv exposure("ORANGE_GUI_GUIDED_CAPTURE_EXPOSURE_US");
    ScopedEnv gray("ORANGE_GUI_GUIDED_CAPTURE_FOREGROUND_GRAY_U8");
    ScopedEnv recipe_sequence("ORANGE_GUI_GUIDED_CAPTURE_RECIPE_SEQUENCE");
    ScopedEnv fixture_shape("ORANGE_GUI_FIXTURE_APERTURE_SHAPE");
    ScopedEnv sweep_gray("ORANGE_GUI_GUIDED_CAPTURE_SWEEP_FOREGROUND_GRAYS_U8");
    ScopedEnv sweep_repeats("ORANGE_GUI_GUIDED_CAPTURE_SWEEP_REPEATS");
    ScopedEnv outline_reference(
        "ORANGE_GUI_GUIDED_CAPTURE_INCLUDE_ARENA_OUTLINE_REFERENCE");
    ScopedEnv targets_ready("ORANGE_GUI_PROJECTED_SURFACE_SCALE_TARGETS_READY");
    ScopedEnv accept_scales("ORANGE_GUI_ACCEPT_PROJECTED_SURFACE_SCALES_ARMED");
    ScopedEnv fit_homographies("ORANGE_GUI_GUIDED_CAPTURE_FIT_HOMOGRAPHIES");

    const auto config = orange::gui::resolve_guided_capture_autorun_config();
    require(!config.enabled, "guided capture autorun defaults off");
    require(config.workflow_profile_id.empty(), "workflow profile defaults empty");
    require(config.recipe == "black_reference", "default recipe is safe black");
    require(config.purpose == "diagnostic_black_reference",
            "black recipe gets diagnostic-only purpose");
    require(config.frame_count == 1, "default frame count is one");
    require(config.apply_calibration_preflight,
            "guided capture defaults to transactional calibration preflight");
    require(config.calibration_frame_rate_hz == 5,
            "calibration frame rate defaults to 5 Hz");
    require(config.calibration_exposure_us == 100000,
            "calibration exposure defaults to 100 ms");
    require(config.foreground_gray_u8 == 255,
            "projected foreground defaults to opaque white");
    require(config.recipe_sequence.empty(), "recipe sequence defaults empty");
    require(config.fixture_aperture_shape == "circle",
            "fixture aperture defaults to the current circular holder");
    require(config.sweep_foreground_grays_u8.empty(),
            "projected foreground sweep defaults empty");
    require(config.sweep_repeats == 1, "foreground sweep defaults to one repeat");
    require(!config.include_arena_outline_reference,
            "arena outline reference defaults off for generic guided captures");
    require(!config.projected_surface_targets_ready_confirmed,
            "physical-target placement requires explicit confirmation");
    require(!config.accept_projected_surface_scales_armed,
            "physical-scale promotion is not armed by default");
    require(!config.fit_homographies_after_capture,
            "post-capture homography fitting defaults off");
    require(config.preflight_settle_milliseconds == 1000,
            "preflight allows five frames to settle at the default rate");
    require(config.camera_serials.empty(), "default scope is all open cameras");
    require(config.result_json_path == "/tmp/orange_gui_guided_capture_result.json",
            "default result path is stable");
}

void test_overrides_are_normalized()
{
    ScopedEnv enabled("ORANGE_GUI_GUIDED_CAPTURE_AUTORUN");
    ScopedEnv save("ORANGE_GUI_GUIDED_CAPTURE_SAVE");
    ScopedEnv exit_after("ORANGE_GUI_GUIDED_CAPTURE_EXIT_AFTER_COMPLETION");
    ScopedEnv config_path("ORANGE_GUI_GUIDED_CAPTURE_CITRUS_CONFIG_PATH");
    ScopedEnv profile("ORANGE_GUI_GUIDED_CAPTURE_PROFILE");
    ScopedEnv recipe("ORANGE_GUI_GUIDED_CAPTURE_RECIPE");
    ScopedEnv purpose("ORANGE_GUI_GUIDED_CAPTURE_PURPOSE");
    ScopedEnv cameras("ORANGE_GUI_GUIDED_CAPTURE_CAMERAS");
    ScopedEnv frames("ORANGE_GUI_GUIDED_CAPTURE_FRAME_COUNT");
    ScopedEnv startup("ORANGE_GUI_GUIDED_CAPTURE_STARTUP_TIMEOUT_SECONDS");
    ScopedEnv workflow("ORANGE_GUI_GUIDED_CAPTURE_WORKFLOW_TIMEOUT_SECONDS");
    ScopedEnv result("ORANGE_GUI_GUIDED_CAPTURE_RESULT_JSON");
    ScopedEnv preflight("ORANGE_GUI_GUIDED_CAPTURE_APPLY_CALIBRATION_PREFLIGHT");
    ScopedEnv frame_rate("ORANGE_GUI_GUIDED_CAPTURE_FRAME_RATE_HZ");
    ScopedEnv exposure("ORANGE_GUI_GUIDED_CAPTURE_EXPOSURE_US");
    ScopedEnv gray("ORANGE_GUI_GUIDED_CAPTURE_FOREGROUND_GRAY_U8");
    ScopedEnv recipe_sequence("ORANGE_GUI_GUIDED_CAPTURE_RECIPE_SEQUENCE");
    ScopedEnv fixture_shape("ORANGE_GUI_FIXTURE_APERTURE_SHAPE");
    ScopedEnv sweep_gray("ORANGE_GUI_GUIDED_CAPTURE_SWEEP_FOREGROUND_GRAYS_U8");
    ScopedEnv sweep_repeats("ORANGE_GUI_GUIDED_CAPTURE_SWEEP_REPEATS");
    ScopedEnv outline_reference(
        "ORANGE_GUI_GUIDED_CAPTURE_INCLUDE_ARENA_OUTLINE_REFERENCE");
    ScopedEnv targets_ready("ORANGE_GUI_PROJECTED_SURFACE_SCALE_TARGETS_READY");
    ScopedEnv accept_scales("ORANGE_GUI_ACCEPT_PROJECTED_SURFACE_SCALES_ARMED");
    ScopedEnv fit_homographies("ORANGE_GUI_GUIDED_CAPTURE_FIT_HOMOGRAPHIES");

    enabled.Set("1");
    save.Set("true");
    exit_after.Set("0");
    config_path.Set("/tmp/shadow.json");
    profile.Set("installed_tank_registration");
    recipe.Set("experimental_area_center_and_outline");
    purpose.Set("crosshair_alignment");
    cameras.Set(" 2010093,2010094,2010093 ,, 2010096 ");
    frames.Set("999");
    startup.Set("0");
    workflow.Set("900");
    result.Set("/tmp/result.json");
    preflight.Set("0");
    frame_rate.Set("2000");
    exposure.Set("2000000");
    gray.Set("300");
    recipe_sequence.Set(
        "black_reference, uniform_gray,homography_rings,uniform_gray");
    fixture_shape.Set("rounded_rectangle");
    sweep_gray.Set("64, 96,64,300,bad,112");
    sweep_repeats.Set("200");
    outline_reference.Set("1");
    targets_ready.Set("1");
    accept_scales.Set("true");
    fit_homographies.Set("1");

    const auto config = orange::gui::resolve_guided_capture_autorun_config();
    require(config.enabled, "enabled override applies");
    require(config.save_captures, "save override applies");
    require(!config.exit_after_completion, "exit override applies");
    require(config.citrus_config_path == "/tmp/shadow.json", "config path applies");
    require(
        config.workflow_profile_id == "installed_tank_registration",
        "workflow profile applies");
    require(config.recipe == "experimental_area_center_and_outline", "recipe applies");
    require(config.purpose == "crosshair_alignment", "purpose applies");
    require(config.camera_serials.size() == 3, "camera scope is trimmed and deduplicated");
    require(config.camera_serials[0] == "2010093" &&
                config.camera_serials[1] == "2010094" &&
                config.camera_serials[2] == "2010096",
            "camera order is preserved");
    require(config.frame_count == 600, "frame count clamps to 600");
    require(config.startup_timeout_seconds == 1, "startup timeout clamps to one");
    require(config.workflow_timeout_seconds == 600, "workflow timeout clamps to 600");
    require(config.result_json_path == "/tmp/result.json", "result path applies");
    require(!config.apply_calibration_preflight, "preflight override applies");
    require(config.calibration_frame_rate_hz == 1000,
            "calibration frame rate clamps to 1000 Hz");
    require(config.calibration_exposure_us == 1000000,
            "calibration exposure clamps to one second");
    require(config.foreground_gray_u8 == 255,
            "foreground grayscale clamps to 255");
    require(config.recipe_sequence.size() == 4 &&
                config.recipe_sequence[0] == "black_reference" &&
                config.recipe_sequence[1] == "uniform_gray" &&
                config.recipe_sequence[2] == "homography_rings" &&
                config.recipe_sequence[3] == "uniform_gray",
            "recipe sequence is trimmed and preserves order and repetition");
    require(config.fixture_aperture_shape == "rounded_rectangle",
            "fixture aperture shape override applies");
    require(config.sweep_foreground_grays_u8.size() == 3 &&
                config.sweep_foreground_grays_u8[0] == 64 &&
                config.sweep_foreground_grays_u8[1] == 96 &&
                config.sweep_foreground_grays_u8[2] == 112,
            "foreground sweep filters invalid values and preserves unique order");
    require(config.sweep_repeats == 100, "foreground sweep repeat count clamps to 100");
    require(config.include_arena_outline_reference,
            "arena outline reference override applies");
    require(config.projected_surface_targets_ready_confirmed,
            "physical-target placement confirmation applies");
    require(config.accept_projected_surface_scales_armed,
            "explicit physical-scale promotion arm applies");
    require(config.fit_homographies_after_capture,
            "post-capture homography fit override applies");
}

void test_recipe_derives_schema_purpose()
{
    ScopedEnv profile("ORANGE_GUI_GUIDED_CAPTURE_PROFILE");
    ScopedEnv recipe("ORANGE_GUI_GUIDED_CAPTURE_RECIPE");
    ScopedEnv purpose("ORANGE_GUI_GUIDED_CAPTURE_PURPOSE");

    recipe.Set("homography_rings");
    auto config = orange::gui::resolve_guided_capture_autorun_config();
    require(config.purpose == "homography_grid", "rings derive homography purpose");

    recipe.Set("verification_dots");
    config = orange::gui::resolve_guided_capture_autorun_config();
    require(config.purpose == "verification_dots", "dots derive verification purpose");

    recipe.Set("uniform_gray");
    config = orange::gui::resolve_guided_capture_autorun_config();
    require(config.purpose == "projected_surface_scale_calibration",
            "uniform gray derives physical scale purpose");
}

void test_profile_derives_recipe_and_purpose()
{
    ScopedEnv profile("ORANGE_GUI_GUIDED_CAPTURE_PROFILE");
    ScopedEnv recipe("ORANGE_GUI_GUIDED_CAPTURE_RECIPE");
    ScopedEnv purpose("ORANGE_GUI_GUIDED_CAPTURE_PURPOSE");

    profile.Set("unobstructed_canvas_commissioning");
    auto config = orange::gui::resolve_guided_capture_autorun_config();
    require(config.recipe == "homography_grid", "unobstructed profile derives grid");
    require(config.purpose == "homography_grid", "unobstructed profile derives purpose");

    profile.Set("holder_installed_projected_surface");
    config = orange::gui::resolve_guided_capture_autorun_config();
    require(config.recipe == "homography_rings", "holder profile derives rings");
    require(config.purpose == "homography_grid", "holder profile derives homography purpose");

    profile.Set("wet_tank_projected_surface");
    config = orange::gui::resolve_guided_capture_autorun_config();
    require(config.recipe == "homography_rings", "wet tank profile derives rings");

    profile.Set("installed_tank_registration");
    config = orange::gui::resolve_guided_capture_autorun_config();
    require(
        config.recipe == "experimental_area_center_and_outline",
        "daily registration profile derives experimental-area scene");
    require(
        config.purpose == "crosshair_alignment",
        "daily registration profile derives crosshair purpose");
}

void test_start_state()
{
    orange::gui::GuidedCaptureAutorunConfig config;
    orange::gui::GuidedCaptureAutorunState state;
    orange::gui::guided_capture_autorun_start(&state, config);
    require(state.stage == orange::gui::GuidedCaptureAutorunStage::kDisabled,
            "disabled config stays disabled");

    config.enabled = true;
    orange::gui::guided_capture_autorun_start(&state, config);
    require(state.stage == orange::gui::GuidedCaptureAutorunStage::kWaitForStream,
            "enabled config waits for real stream");
    require(state.run_started_at.time_since_epoch().count() != 0,
            "enabled start records monotonic origin");
    require(std::string(orange::gui::guided_capture_autorun_stage_name(state.stage)) ==
                "wait_for_stream",
            "stage has machine-readable name");
}

}  // namespace

int main()
{
    try {
        test_defaults();
        test_overrides_are_normalized();
        test_recipe_derives_schema_purpose();
        test_profile_derives_recipe_and_purpose();
        test_start_state();
    } catch (const std::exception& error) {
        std::cerr << "guided_capture_autorun_config_tests failed: "
                  << error.what() << std::endl;
        return 1;
    }
    std::cout << "guided_capture_autorun_config_tests passed" << std::endl;
    return 0;
}
