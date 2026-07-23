#include "gui/arena_centering_autorun.h"

#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {

void require(bool condition, const std::string& message)
{
    if (!condition) throw std::runtime_error(message);
}

class ScopedEnv {
public:
    explicit ScopedEnv(std::string name) : name_(std::move(name))
    {
        if (const char* value = std::getenv(name_.c_str())) {
            original_ = value;
            had_original_ = true;
        }
        unsetenv(name_.c_str());
    }
    ~ScopedEnv()
    {
        if (had_original_) setenv(name_.c_str(), original_.c_str(), 1);
        else unsetenv(name_.c_str());
    }
    void Set(const std::string& value) { setenv(name_.c_str(), value.c_str(), 1); }

private:
    std::string name_;
    std::string original_;
    bool had_original_ = false;
};

void test_defaults_are_safe()
{
    ScopedEnv enabled("ORANGE_GUI_ARENA_CENTERING_AUTORUN");
    ScopedEnv arm("ORANGE_GUI_ARENA_CENTERING_SAVE_VERIFIED_CENTERS_ARMED");
    ScopedEnv resize("ORANGE_GUI_ARENA_CENTERING_RESIZE_ARENAS");
    ScopedEnv layout_arm("ORANGE_GUI_ARENA_CENTERING_SAVE_VERIFIED_LAYOUT_ARMED");
    ScopedEnv save("ORANGE_GUI_ARENA_CENTERING_SAVE_CAPTURES");
    ScopedEnv fit_homography("ORANGE_GUI_ARENA_CENTERING_FIT_HOMOGRAPHIES");
    ScopedEnv accept_homography("ORANGE_GUI_ARENA_CENTERING_ACCEPT_HOMOGRAPHIES_ARMED");
    ScopedEnv gray("ORANGE_GUI_ARENA_CENTERING_FOREGROUND_GRAY_U8");
    ScopedEnv probe("ORANGE_GUI_ARENA_CENTERING_PROBE_CANVAS_PX");
    ScopedEnv tolerance("ORANGE_GUI_ARENA_CENTERING_VERIFICATION_TOLERANCE_CAMERA_PX");
    ScopedEnv projection_settle("ORANGE_GUI_ARENA_CENTERING_PROJECTION_SETTLE_MS");
    ScopedEnv stability_required("ORANGE_GUI_ARENA_CENTERING_REQUIRE_STABILITY_CAPTURE");
    ScopedEnv stability_interval("ORANGE_GUI_ARENA_CENTERING_STABILITY_INTERVAL_MS");
    ScopedEnv stability_attempts("ORANGE_GUI_ARENA_CENTERING_STABILITY_MAX_CAPTURE_ATTEMPTS");
    ScopedEnv saturation_threshold(
        "ORANGE_GUI_HOMOGRAPHY_SATURATION_PIXEL_THRESHOLD_U8");
    ScopedEnv maximum_saturation(
        "ORANGE_GUI_HOMOGRAPHY_MAXIMUM_DOT_CORE_SATURATION_FRACTION");
    ScopedEnv minimum_contrast(
        "ORANGE_GUI_HOMOGRAPHY_MINIMUM_DOT_BACKGROUND_CONTRAST_U8");
    const auto config = orange::gui::resolve_arena_centering_autorun_config();
    require(!config.enabled, "arena centering defaults disabled");
    require(config.save_captures, "evidence capture defaults enabled");
    require(!config.save_verified_centers_armed,
            "persistent center save defaults explicitly disarmed");
    require(!config.resize_arenas,
            "arena resizing defaults opt-in");
    require(!config.save_verified_layout_armed,
            "persistent layout save defaults independently disarmed");
    require(!config.fit_homographies_after_centering,
            "homography fitting defaults opt-in");
    require(!config.accept_homographies_armed,
            "homography promotion defaults explicitly disarmed");
    require(config.rectangle_safety_margin_camera_px == 32.0,
            "rectangle safety margin defaults to 32 native camera px");
    require(config.rectangle_prediction_reserve_camera_px == 8.0,
            "rectangle prediction reserve defaults to 8 native camera px");
    require(config.foreground_gray_u8 == 72, "qualified gray defaults to 72");
    require(config.symmetric_probe_canvas_px == 3, "probe defaults to 3 canvas px");
    require(config.verification_tolerance_camera_px == 2.0,
            "verification defaults to 2 camera px");
    require(config.projection_settle_milliseconds == 1000,
            "each presented projection defaults to a 1000 ms camera drain");
    require(config.require_projection_stability_capture,
            "two independent post-fence captures default required");
    require(config.projection_stability_interval_milliseconds == 300,
            "stability confirmation defaults to a 300 ms interval");
    require(config.projection_stability_max_capture_attempts == 5,
            "stability capture attempts default to five");
    require(config.projection_stability_max_center_delta_camera_px == 2.0,
            "stability center delta defaults to 2 camera px");
    require(config.projection_stability_max_corner_delta_camera_px == 12.0,
            "stability corner delta defaults to 12 camera px");
    require(config.homography_saturation_pixel_threshold_u8 == 250,
            "photometry saturation threshold defaults to Mono8 value 250");
    require(config.homography_maximum_dot_core_saturation_fraction == 0.005,
            "dot-core saturation defaults to the commissioned 0.5% gate");
    require(config.homography_minimum_dot_background_contrast_u8 == 20.0,
            "dot/background contrast defaults to the commissioned Mono8 gate");
}

void test_overrides_are_bounded_and_deduplicated()
{
    ScopedEnv enabled("ORANGE_GUI_ARENA_CENTERING_AUTORUN");
    ScopedEnv arm("ORANGE_GUI_ARENA_CENTERING_SAVE_VERIFIED_CENTERS_ARMED");
    ScopedEnv resize("ORANGE_GUI_ARENA_CENTERING_RESIZE_ARENAS");
    ScopedEnv layout_arm("ORANGE_GUI_ARENA_CENTERING_SAVE_VERIFIED_LAYOUT_ARMED");
    ScopedEnv margin("ORANGE_GUI_ARENA_CENTERING_RECTANGLE_SAFETY_MARGIN_CAMERA_PX");
    ScopedEnv cameras("ORANGE_GUI_ARENA_CENTERING_CAMERAS");
    ScopedEnv gray("ORANGE_GUI_ARENA_CENTERING_FOREGROUND_GRAY_U8");
    ScopedEnv probe("ORANGE_GUI_ARENA_CENTERING_PROBE_CANVAS_PX");
    ScopedEnv tolerance("ORANGE_GUI_ARENA_CENTERING_VERIFICATION_TOLERANCE_CAMERA_PX");
    ScopedEnv span("ORANGE_GUI_ARENA_CENTERING_MAX_PTP_SPAN_NS");
    ScopedEnv fit_homography("ORANGE_GUI_ARENA_CENTERING_FIT_HOMOGRAPHIES");
    ScopedEnv accept_homography("ORANGE_GUI_ARENA_CENTERING_ACCEPT_HOMOGRAPHIES_ARMED");
    ScopedEnv projection_settle("ORANGE_GUI_ARENA_CENTERING_PROJECTION_SETTLE_MS");
    ScopedEnv stability_required("ORANGE_GUI_ARENA_CENTERING_REQUIRE_STABILITY_CAPTURE");
    ScopedEnv stability_interval("ORANGE_GUI_ARENA_CENTERING_STABILITY_INTERVAL_MS");
    ScopedEnv stability_attempts("ORANGE_GUI_ARENA_CENTERING_STABILITY_MAX_CAPTURE_ATTEMPTS");
    ScopedEnv stability_center("ORANGE_GUI_ARENA_CENTERING_STABILITY_MAX_CENTER_DELTA_CAMERA_PX");
    ScopedEnv stability_corner("ORANGE_GUI_ARENA_CENTERING_STABILITY_MAX_CORNER_DELTA_CAMERA_PX");
    ScopedEnv saturation_threshold(
        "ORANGE_GUI_HOMOGRAPHY_SATURATION_PIXEL_THRESHOLD_U8");
    ScopedEnv maximum_saturation(
        "ORANGE_GUI_HOMOGRAPHY_MAXIMUM_DOT_CORE_SATURATION_FRACTION");
    ScopedEnv minimum_contrast(
        "ORANGE_GUI_HOMOGRAPHY_MINIMUM_DOT_BACKGROUND_CONTRAST_U8");
    enabled.Set("1");
    arm.Set("true");
    resize.Set("true");
    layout_arm.Set("true");
    margin.Set("2");
    cameras.Set(" 2010093,2010094,2010093,2010096 ");
    gray.Set("999");
    probe.Set("100");
    tolerance.Set("0");
    span.Set("0");
    fit_homography.Set("true");
    accept_homography.Set("true");
    projection_settle.Set("999999");
    stability_required.Set("false");
    stability_interval.Set("999999");
    stability_attempts.Set("999");
    stability_center.Set("0");
    stability_corner.Set("9999");
    saturation_threshold.Set("999");
    maximum_saturation.Set("9");
    minimum_contrast.Set("999");
    const auto config = orange::gui::resolve_arena_centering_autorun_config();
    require(config.enabled && config.save_verified_centers_armed,
            "enabled and arm overrides apply");
    require(config.resize_arenas && config.save_verified_layout_armed,
            "resize and strong layout arm overrides apply");
    require(config.rectangle_safety_margin_camera_px == 4.0,
            "rectangle margin clamps to a positive minimum");
    require(config.camera_serials.size() == 3,
            "camera scope is trimmed and deduplicated");
    require(config.foreground_gray_u8 == 255, "gray clamps to 255");
    require(config.symmetric_probe_canvas_px == 32, "probe clamps to 32");
    require(config.verification_tolerance_camera_px == 0.1,
            "tolerance clamps to positive minimum");
    require(config.maximum_ptp_capture_span_ns == 1,
            "PTP span gate clamps to positive minimum");
    require(config.fit_homographies_after_centering &&
                config.accept_homographies_armed,
            "homography fit and promotion arms apply independently");
    require(config.projection_settle_milliseconds == 30000,
            "projection settle clamps to 30 seconds");
    require(!config.require_projection_stability_capture,
            "stability capture can be explicitly disabled");
    require(config.projection_stability_interval_milliseconds == 30000,
            "stability interval clamps to 30 seconds");
    require(config.projection_stability_max_capture_attempts == 10,
            "stability capture attempts clamp to ten");
    require(config.projection_stability_max_center_delta_camera_px == 0.1,
            "stability center delta clamps to a positive minimum");
    require(config.projection_stability_max_corner_delta_camera_px == 100.0,
            "stability corner delta clamps to its maximum");
    require(config.homography_saturation_pixel_threshold_u8 == 255,
            "saturation pixel threshold clamps to Mono8");
    require(config.homography_maximum_dot_core_saturation_fraction == 1.0,
            "saturation fraction clamps to one");
    require(config.homography_minimum_dot_background_contrast_u8 == 255.0,
            "contrast threshold clamps to Mono8");
}

}  // namespace

int main()
{
    try {
        test_defaults_are_safe();
        test_overrides_are_bounded_and_deduplicated();
    } catch (const std::exception& error) {
        std::cerr << "arena_centering_autorun_config_tests failed: "
                  << error.what() << std::endl;
        return 1;
    }
    std::cout << "arena_centering_autorun_config_tests passed" << std::endl;
    return 0;
}
