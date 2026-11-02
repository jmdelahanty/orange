// CameraParams -> image-levels settings (src/image_levels.h).
#pragma once

#include "camera.h"
#include "image_levels.h"

namespace orange::image_levels {

inline LevelSettings LevelSettingsFromCamera(const CameraParams& camera)
{
    LevelSettings s;
    s.exposure = camera.exposure;
    s.gain = camera.gain;
    s.iris = camera.iris;
    s.focus = camera.focus;
    s.width = camera.width;
    s.height = camera.height;
    s.offset_x = camera.offsetx;
    s.offset_y = camera.offsety;
    return s;
}

}  // namespace orange::image_levels
