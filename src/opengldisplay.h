// src/opengldisplay.h

#pragma once

#include "video_capture.h" // Includes ProcessedFrame definition
#include "common.hpp"      // For pose::Object and NppiSize
#include <cuda.h>
#include <nppi.h>          // For NppiSize
#include "gui.h" // For GL_Texture

// Forward declaration
class INDIGOSignalBuilder;

// This class now processes the output of the FramePreprocessor on the main thread
class COpenGLDisplay
{
public:
    COpenGLDisplay(
        const char* name,
        CUcontext pCudaContext, // The CUDA context for this worker
        CameraParams* camera_params,
        CameraEachSelect* camera_select,
        GL_Texture * gl_texture,
        INDIGOSignalBuilder* indigo_signal_builder,
        SafeQueue<WORKER_ENTRY*>& raw_recycle_queue,
        SafeQueue<ProcessedFrame*>& processed_recycle_queue);

    ~COpenGLDisplay();

    void update_texture(ProcessedFrame* f);

private:
    CameraParams* camera_params_;
    CameraEachSelect* camera_select;
    GL_Texture * m_gl_texture;
    INDIGOSignalBuilder* indigo_signal_builder_;

    // A dedicated CUDA stream for this worker's operations (resizing, drawing)
    cudaStream_t m_stream;
    CUcontext m_cuContext;

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