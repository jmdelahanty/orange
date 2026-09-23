#pragma once
#include "camera.h"
#include "json.hpp"

namespace orange::recording {
// Shared GUI/headless snapshot of configured values, not hardware readback.
inline nlohmann::json RegisteredContextCameraConfiguration(const CameraParams& camera) {
    return {{"camera_id", camera.camera_id}, {"width", camera.width}, {"height", camera.height},
        {"pixel_format", camera.pixel_format}, {"frame_rate_hz", camera.frame_rate}, {"exposure_us", camera.exposure},
        {"gain", camera.gain}, {"focus", camera.focus}, {"iris", camera.iris}};
}
}
