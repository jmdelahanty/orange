#pragma once

#include "encoder_pipeline.h"
#include "video_capture.h"

#include <string>
#include <vector>

struct EncoderConfig {
    std::string encoder_codec;
    std::string encoder_preset;
    std::string tuning_info;
    std::string rate_control_mode;
    int quality_value;
    int gop_length;
    std::string record_output_mode;
    int record_downsample_factor;
    int record_output_width;
    int record_output_height;
    std::string folder_name;
};

namespace orange::gui {

struct RecordingPanelActions {
    bool choose_recording_dir_requested = false;
};

void sanitize_record_output_config(std::string* mode, int* factor, int* width, int* height);
std::string record_output_summary(const std::string& mode, int factor, int width, int height);
std::string format_bitrate_mbps(uint32_t bitrate_bps);
RecordingOutputConfig resolve_recording_output_config(const CameraParams& camera_params,
                                                      const EncoderConfig& encoder_config,
                                                      const CameraEachSelect& camera_select,
                                                      std::string* warning_out);

RecordingPanelActions render_recording_config_panel(std::string* input_folder,
                                                    EncoderConfig* encoder_config,
                                                    bool camera_open,
                                                    bool streaming_active,
                                                    CameraParams* cameras_params,
                                                    CameraEachSelect* cameras_select,
                                                    int num_cameras,
                                                    const std::vector<std::string>* preflight_errors);

}  // namespace orange::gui
