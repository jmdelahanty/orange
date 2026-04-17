#pragma once

#include "recording_config_state.h"
#include "video_capture.h"

#include <vector>

namespace orange::gui {

struct RecordingPanelActions {
    bool choose_recording_dir_requested = false;
};

RecordingPanelActions render_recording_config_panel(std::string* input_folder,
                                                    EncoderConfig* encoder_config,
                                                    bool camera_open,
                                                    bool streaming_active,
                                                    CameraParams* cameras_params,
                                                    CameraEachSelect* cameras_select,
                                                    int num_cameras,
                                                    const std::string* config_defaults_status,
                                                    bool config_defaults_status_warning,
                                                    const std::vector<std::string>* preflight_errors);

}  // namespace orange::gui
