// CameraParams -> empty-background camera configuration (src/empty_background.h).
#pragma once

#include "camera.h"
#include "empty_background.h"

namespace orange::empty_background {

inline CameraConfiguration CameraConfigurationFromCamera(const CameraParams& camera)
{
    CameraConfiguration c;
    c.width = camera.width;
    c.height = camera.height;
    c.offset_x = camera.offsetx;
    c.offset_y = camera.offsety;
    c.pixel_format = camera.pixel_format;
    c.frame_rate_hz = camera.frame_rate;
    c.exposure_us = camera.exposure;
    c.gain = camera.gain;
    c.focus = camera.focus;
    c.iris = camera.iris;
    return c;
}

}  // namespace orange::empty_background
