#pragma once

#include <string>

namespace orange::gui::spatial_layout {

struct DailyPhysicalRegistrationSavePreflightInput {
    bool writer_busy = false;
    bool has_capture = false;
    std::string selected_camera_serial;
    std::string captured_camera_serial;
    int captured_width_px = 0;
    int captured_height_px = 0;
    int configured_width_px = 0;
    int configured_height_px = 0;
    std::string source_array_role = "images_full";
    bool inner_rim_target_confirmed = false;
    bool has_accepted_physical_circle = false;
    bool has_raw_detected_circle = false;
};

struct DailyPhysicalRegistrationSavePreflightResult {
    bool allowed = false;
    std::string code;
    std::string message;
};

DailyPhysicalRegistrationSavePreflightResult
evaluate_daily_physical_registration_save_preflight(
    const DailyPhysicalRegistrationSavePreflightInput& input);

}  // namespace orange::gui::spatial_layout
