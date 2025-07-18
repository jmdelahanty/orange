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

// Forward declare all consumers
class COpenGLDisplay;
class GPUVideoEncoder;
class YOLOv8Worker;
class ImageWriterWorker;
class CropAndEncodeWorker;

void acquire_frames(
    CameraEmergent *ecam,
    CameraParams *camera_params,
    CameraEachSelect* camera_select,
    CameraControl* camera_control,
    PTPParams* ptp_params,
    INDIGOSignalBuilder* indigo_signal_builder,
    COpenGLDisplay* display_worker,
    GPUVideoEncoder* gpu_encoder,
    YOLOv8Worker* yolo_worker,
    CropAndEncodeWorker* crop_encode_worker,
    ImageWriterWorker* image_writer,
    CameraResources* resources
);
#endif