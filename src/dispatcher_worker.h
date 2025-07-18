// src/dispatcher_worker.h

#ifndef DISPATCHER_WORKER_H
#define DISPATCHER_WORKER_H

#include "threadworker.h"
#include "video_capture.h"
#include "opengldisplay.h"
#include "gpu_video_encoder.h"
#include "yolo_worker.h"

class DispatcherWorker : public CThreadWorker<WORKER_ENTRY>
{
public:
    DispatcherWorker(
        const char* name,
        CameraEachSelect* camera_select,
        CameraControl* camera_control,
        COpenGLDisplay* display_worker,
        GPUVideoEncoder* gpu_encoder,
        YOLOv8Worker* yolo_worker,
        SafeQueue<WORKER_ENTRY*>& recycle_queue);

    ~DispatcherWorker() override;

protected:
    bool WorkerFunction(WORKER_ENTRY* entry) override;

private:
    CameraEachSelect* camera_select_;
    CameraControl* camera_control_;
    COpenGLDisplay* display_worker_;
    GPUVideoEncoder* gpu_encoder_;
    YOLOv8Worker* yolo_worker_;
    SafeQueue<WORKER_ENTRY*>& m_recycle_queue_;
};

#endif // DISPATCHER_WORKER_H