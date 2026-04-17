#pragma once

#include "camera.h"
#include "recording_config_state.h"
#include "video_capture.h"

#include <cstdint>
#include <string>

namespace orange::recording {

void sanitize_record_output_config(std::string* mode, int* factor, int* width, int* height);
std::string record_output_summary(const std::string& mode, int factor, int width, int height);
std::string format_bitrate_mbps(uint32_t bitrate_bps);
RecordingOutputConfig resolve_recording_output_config(const CameraParams& camera_params,
                                                      const EncoderConfig& encoder_config,
                                                      const CameraEachSelect& camera_select,
                                                      std::string* warning_out);

}  // namespace orange::recording
