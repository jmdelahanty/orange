// Retired legacy headless acquisition entry point. It is not part of the
// current CMake targets; orange_headless_client.cpp uses acquire_frames.cpp and
// ModernRecordingPipeline instead.

#ifndef ORANGE_ACQUIRE_FRAMES_HEADLESS
#define ORANGE_ACQUIRE_FRAMES_HEADLESS
#include "video_capture.h"

void acquire_frames_headless(CameraEmergent *ecam, CameraParams *camera_params, CameraEachSelect* camera_select, CameraControl* camera_control, std::string encoder_setup, std::string folder_name, PTPParams* ptp_params);

#endif
