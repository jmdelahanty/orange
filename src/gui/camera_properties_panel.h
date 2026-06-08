#pragma once

#include "camera.h"

#include <string>
#include <vector>

namespace orange::gui {

void render_camera_properties_panel(CameraEmergent* ecams,
                                    CameraParams* cameras_params,
                                    int num_cameras,
                                    std::vector<std::string>& color_temps,
                                    const std::string& selected_local_config_folder,
                                    bool recording_mutation_locked);

}  // namespace orange::gui
