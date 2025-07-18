// src/dispatcher_worker.cpp

#include "dispatcher_worker.h"
#include "nvtx_profiling.h"
#include <iostream>

DispatcherWorker::DispatcherWorker(
    const char* name,
    CameraEachSelect* camera_select,
    CameraControl* camera_control,
    COpenGLDisplay* display_worker,
    GPUVideoEncoder* gpu_encoder,
    YOLOv8Worker* yolo_worker,
    SafeQueue<WORKER_ENTRY*>& recycle_queue)
    : CThreadWorker(name),
      camera_select_(camera_select),
      camera_control_(camera_control),
      display_worker_(display_worker),
      gpu_encoder_(gpu_encoder),
      yolo_worker_(yolo_worker),
      m_recycle_queue_(recycle_queue)
{
    std::cout << "[DISPATCH] CONSTRUCTOR for " << name << std::endl;
}

DispatcherWorker::~DispatcherWorker()
{
    std::cout << "[DISPATCH] DESTRUCTOR for " << threadName << std::endl;
}

bool DispatcherWorker::WorkerFunction(WORKER_ENTRY* entry)
{
    if (!entry) {
        return false;
    }

    NVTX_RANGE("DispatchWorker");

    // --- HYBRID LOGIC & FINAL REF_COUNT ---
    // This is the fan-out point. The entry has already been processed by the PreprocessingWorker.
    int dispatch_count = 0;

    // These workers get the PROCESSED RGB data from entry->d_processed_rgb
    if (camera_select_->yolo && yolo_worker_) dispatch_count++;
    if (camera_select_->stream_on && display_worker_) dispatch_count++;
    
    // The encoder is the special case and gets the RAW data from entry->d_image
    if (camera_control_->record_video && gpu_encoder_) dispatch_count++;

    if (dispatch_count > 0) {
        // The preprocessor was the first user. Now we add the new users to the ref count.
        entry->ref_count.fetch_add(dispatch_count, std::memory_order_acq_rel);

        // Dispatch the single WORKER_ENTRY* to all interested consumers.
        if (camera_control_->record_video && gpu_encoder_) {
            gpu_encoder_->PutObjectToQueueIn(entry);
        }
        if (camera_select_->yolo && yolo_worker_) {
            yolo_worker_->PutObjectToQueueIn(entry);
        }
        if (camera_select_->stream_on && display_worker_) {
            display_worker_->PutObjectToQueueIn(entry);
        }
    }

    // The dispatcher is now done with its stage, so we decrement the count once.
    // The last of the consumer threads will be responsible for the final decrement and recycling.
    if (entry->ref_count.fetch_sub(1, std::memory_order_acq_rel) == 1) {
        // Failsafe: if no consumers were active, recycle the entry immediately.
        m_recycle_queue_.push(entry);
    }

    // This worker never passes items to its own output queue.
    return false;
}