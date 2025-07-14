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
class FramePreprocessor;
class ImageWriterWorker;
class CropAndEncodeWorker; 

// The signature now matches the implementation in the .cpp file
void acquire_frames(
    CameraEmergent *ecam,
    CameraParams *camera_params,
    CameraEachSelect* camera_select,
    CameraControl* camera_control,
    PTPParams* ptp_params,
    CameraResources* resources,
    FramePreprocessor* preprocessor
);

#endif