#include "gui/spatial_layout/daily_physical_registration_preflight.h"

namespace orange::gui::spatial_layout {
namespace {

DailyPhysicalRegistrationSavePreflightResult reject(
    const std::string& code,
    const std::string& message)
{
    return {false, code, message};
}

}  // namespace

DailyPhysicalRegistrationSavePreflightResult
evaluate_daily_physical_registration_save_preflight(
    const DailyPhysicalRegistrationSavePreflightInput& input)
{
    if (input.writer_busy) {
        return reject(
            "writer_busy",
            "Wait for the current spatial-calibration save to finish.");
    }
    if (!input.has_capture) {
        return reject(
            "capture_missing",
            "Capture a full-resolution camera frame before saving physical dish registration.");
    }
    if (input.selected_camera_serial.empty()) {
        return reject(
            "selected_camera_identity_missing",
            "The selected camera has no stable serial identity.");
    }
    if (input.captured_camera_serial.empty()) {
        return reject(
            "captured_camera_identity_missing",
            "The captured frame has no camera serial; recapture it from the live camera.");
    }
    if (input.captured_camera_serial != input.selected_camera_serial) {
        return reject(
            "captured_camera_mismatch",
            "The captured frame belongs to camera " + input.captured_camera_serial +
                ", not selected camera " + input.selected_camera_serial + ".");
    }
    if (input.captured_width_px <= 0 || input.captured_height_px <= 0) {
        return reject(
            "captured_raster_invalid",
            "The captured frame has invalid dimensions.");
    }
    if ((input.configured_width_px > 0 &&
         input.captured_width_px != input.configured_width_px) ||
        (input.configured_height_px > 0 &&
         input.captured_height_px != input.configured_height_px)) {
        return reject(
            "captured_raster_mismatch",
            "The captured frame dimensions do not match the selected camera's native configured raster.");
    }
    const std::string source_array_role =
        input.source_array_role.empty() ? "images_full" : input.source_array_role;
    if (source_array_role != "images_full") {
        return reject(
            "capture_not_full_resolution",
            "Physical dish registration requires a full-resolution camera-native capture.");
    }
    if (!input.inner_rim_target_confirmed) {
        return reject(
            "inner_rim_not_confirmed",
            "Confirm that the accepted fit follows the water-side inner rim.");
    }
    if (!input.has_accepted_physical_circle) {
        return reject(
            "accepted_physical_circle_missing",
            "An accepted circular physical dish boundary is not available.");
    }
    if (!input.has_raw_detected_circle) {
        return reject(
            "raw_detected_circle_missing",
            "Run Hough circle detection before saving; raw detection evidence is required.");
    }
    return {
        true,
        "ready",
        "Camera-native physical dish registration is ready to save; Citrus is not required."
    };
}

}  // namespace orange::gui::spatial_layout
