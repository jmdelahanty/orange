// src/opengldisplay.h

#pragma once

#include "threadworker.h"
#include "video_capture.h" // Includes ProcessedFrame definition
#include "common.hpp"      // For pose::Object and NppiSize
#include <cuda.h>
#include <nppi.h>          // For NppiSize

// Forward declaration
class INDIGOSignalBuilder;

// This worker now processes the output of the FramePreprocessor
class COpenGLDisplay : public CThreadWorker<ProcessedFrame>
{
public:
    COpenGLDisplay(
        const char* name,
        CameraParams* camera_params,
        CameraEachSelect* camera_select,
        unsigned char* display_buffer_cuda_pbo,
        INDIGOSignalBuilder* indigo_signal_builder,
        SafeQueue<WORKER_ENTRY*>& raw_recycle_queue,
        SafeQueue<ProcessedFrame*>& processed_recycle_queue);

    ~COpenGLDisplay() override;

private:
    bool WorkerFunction(ProcessedFrame* f) override;
    
    CameraParams* camera_params;
    CameraEachSelect* camera_select;
    unsigned char* display_buffer_pbo_cuda_ptr_;
    INDIGOSignalBuilder* indigo_signal_builder_;
    
    // --- Members for this worker's specific tasks ---

    // A dedicated CUDA stream for this worker's operations (resizing, drawing)
    cudaStream_t m_stream;

    // Buffer for drawing detection boxes onto the image.
    // It will hold a copy of the detection data from the ProcessedFrame.
    pose::Object* d_detections_for_drawing_;

    // Buffer for resizing the final image if downsampling is needed for display.
    unsigned char* d_display_resize_buffer_;

    // Staging buffer for P2P transfers if the display GPU is different from the worker GPU
    unsigned char* h_p2p_copy_buffer_; 

    // --- Memory Management ---
    SafeQueue<WORKER_ENTRY*>& m_raw_recycle_queue;
    SafeQueue<ProcessedFrame*>& m_processed_recycle_queue;
};