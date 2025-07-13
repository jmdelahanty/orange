// src/acquire_frames.h

#ifndef ORANGE_ACQUIRE_FRAMES
#define ORANGE_ACQUIRE_FRAMES

#include "thread.h"
#include "camera.h"
#include <iostream>
#include <fstream>
#include "network_base.h"
#include "image_processing.h"
#include "video_capture.h"
#include <cuda.h>

// Forward declare worker classes
class FramePreprocessor; // <-- Only preprocessor is needed here

void acquire_frames(
    CameraEmergent *ecam,
    CameraParams *camera_params,
    CameraControl* camera_control,
    CameraResources* resources,
    FramePreprocessor* preprocessor // <-- Simplified signature
);

#endif // ORANGE_ACQUIRE_FRAMES