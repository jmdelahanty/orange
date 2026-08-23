#include "gui/spatial_layout/daily_physical_registration_preflight.h"

#include <iostream>
#include <stdexcept>
#include <string>

namespace {

using orange::gui::spatial_layout::DailyPhysicalRegistrationSavePreflightInput;
using orange::gui::spatial_layout::evaluate_daily_physical_registration_save_preflight;

void require(bool condition, const std::string& message)
{
    if (!condition) {
        throw std::runtime_error(message);
    }
}

DailyPhysicalRegistrationSavePreflightInput ready_input()
{
    DailyPhysicalRegistrationSavePreflightInput input;
    input.has_capture = true;
    input.selected_camera_serial = "2010093";
    input.captured_camera_serial = "2010093";
    input.captured_width_px = 4512;
    input.captured_height_px = 4512;
    input.configured_width_px = 4512;
    input.configured_height_px = 4512;
    input.source_array_role = "images_full";
    input.inner_rim_target_confirmed = true;
    input.has_accepted_physical_circle = true;
    input.has_raw_detected_circle = true;
    return input;
}

void test_ready_without_citrus()
{
    const auto result =
        evaluate_daily_physical_registration_save_preflight(ready_input());
    require(result.allowed, "camera-native save should not require Citrus");
    require(result.code == "ready", "ready preflight code");
}

void test_rejects_camera_mismatch()
{
    auto input = ready_input();
    input.captured_camera_serial = "2010094";
    const auto result =
        evaluate_daily_physical_registration_save_preflight(input);
    require(!result.allowed, "mismatched capture camera must be rejected");
    require(result.code == "captured_camera_mismatch", "camera mismatch code");
}

void test_rejects_raster_mismatch()
{
    auto input = ready_input();
    input.captured_width_px = 2256;
    const auto result =
        evaluate_daily_physical_registration_save_preflight(input);
    require(!result.allowed, "non-native raster must be rejected");
    require(result.code == "captured_raster_mismatch", "raster mismatch code");
}

void test_rejects_downsampled_capture()
{
    auto input = ready_input();
    input.source_array_role = "images_ds";
    const auto result =
        evaluate_daily_physical_registration_save_preflight(input);
    require(!result.allowed, "downsampled capture must be rejected");
    require(
        result.code == "capture_not_full_resolution",
        "downsampled-capture code");
}

void test_rejects_unconfirmed_or_incomplete_fit()
{
    auto input = ready_input();
    input.inner_rim_target_confirmed = false;
    auto result = evaluate_daily_physical_registration_save_preflight(input);
    require(!result.allowed, "unconfirmed physical target must be rejected");
    require(result.code == "inner_rim_not_confirmed", "confirmation code");

    input = ready_input();
    input.has_accepted_physical_circle = false;
    result = evaluate_daily_physical_registration_save_preflight(input);
    require(!result.allowed, "missing accepted circle must be rejected");
    require(
        result.code == "accepted_physical_circle_missing",
        "accepted-circle code");

    input = ready_input();
    input.has_raw_detected_circle = false;
    result = evaluate_daily_physical_registration_save_preflight(input);
    require(!result.allowed, "missing raw detection must be rejected");
    require(
        result.code == "raw_detected_circle_missing",
        "raw-detection code");
}

}  // namespace

int main()
{
    try {
        test_ready_without_citrus();
        test_rejects_camera_mismatch();
        test_rejects_raster_mismatch();
        test_rejects_downsampled_capture();
        test_rejects_unconfirmed_or_incomplete_fit();
    } catch (const std::exception& ex) {
        std::cerr << "daily_physical_registration_preflight_tests failed: "
                  << ex.what() << std::endl;
        return 1;
    }
    std::cout << "daily_physical_registration_preflight_tests passed"
              << std::endl;
    return 0;
}
