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

// Forward declare worker classes to break include cycles
class COpenGLDisplay;
class YoloWorker;
class ImageWriterWorker;
class CropAndEncodeWorker;
class RecordingIngress;
class FrameIPCManager;
class SpatialSnapshotWorker;
namespace yolo_event_log {
class SyntheticYoloEventEmitter;
}

void acquire_frames(
    CameraEmergent *ecam,
    CameraParams *camera_params,
    CameraEachSelect* camera_select,
    CameraControl* camera_control,
    PTPParams* ptp_params,
    INDIGOSignalBuilder* indigo_signal_builder,
    COpenGLDisplay* openGLDisplay,
    RecordingIngress* recording_ingress,
    YoloWorker* yolo_worker,
    ImageWriterWorker* image_writer,
    CameraResources* resources,
    FrameIPCManager* frame_ipc_manager,
    yolo_event_log::SyntheticYoloEventEmitter* synthetic_yolo_event_emitter = nullptr,
    SpatialSnapshotWorker* spatial_snapshot_worker = nullptr
);
#endif
