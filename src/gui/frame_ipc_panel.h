#pragma once

#include "camera.h"
#include "frame_ipc_manager.h"
#include "video_capture.h"

#include <string>
#include <vector>

namespace orange::gui {

void render_frame_ipc_status_panel(bool streaming_active,
                                   CameraEachSelect* cameras_select,
                                   CameraParams* cameras_params,
                                   int num_cameras,
                                   const std::vector<FrameIPCManager*>& frame_ipc_managers,
                                   const std::vector<std::string>& frame_ipc_init_errors);

}  // namespace orange::gui
