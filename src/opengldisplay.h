// src/opengldisplay.h

#pragma once
#include "threadworker.h"
#include "image_processing.h"
#include "thread.h" // For SafeQueue
#include <nppi.h>
#include "common.hpp"
#include <cuda.h>
#include <atomic>
#include <chrono>

class COpenGLDisplay : public CThreadWorker<WORKER_ENTRY>
{
public:
    COpenGLDisplay(
        const char* name,
        CameraParams *camera_params,
        CameraEachSelect *camera_select,
        unsigned char *display_buffer_cuda_pbo,
        INDIGOSignalBuilder* indigo_signal_builder,
        SafeQueue<WORKER_ENTRY*>& recycle_queue);
    ~COpenGLDisplay() override;

    CameraParams* camera_params;
    CameraEachSelect* camera_select;
    unsigned char* display_buffer_pbo_cuda_ptr_;
    FrameGPU frame_original_gpu_;
    Debayer debayer_gpu_;
    INDIGOSignalBuilder* indigo_signal_builder_;
    uint64_t PreviewSerial() const { return preview_serial_.load(std::memory_order_acquire); }

protected:
    bool WorkerFunction(WORKER_ENTRY* f) override;
    void OnFlushTick() override {}  // no flush-time housekeeping

private:
    unsigned char* h_p2p_copy_buffer_;
    pose::Object *d_detections_for_drawing_; 
    unsigned int *d_skeleton_for_drawing_;
    unsigned char *d_display_mono_resize_buffer_;
    unsigned char *d_display_resize_buffer_;
    NppiSize mono_resize_source_size_;
    NppiRect mono_resize_source_roi_;
    NppiSize mono_resize_output_size_;
    NppiRect mono_resize_output_roi_;
    NppiSize output_display_size_;
    NppiRect input_roi_for_display_resize_;
    NppiRect output_roi_for_display_resize_;
    uint64_t display_same_gpu_frames_ = 0;
    uint64_t display_cross_gpu_frames_ = 0;
    // Source-side (landing die) preview downsample. When enabled, the worker
    // reduces the frame on the acquisition GPU and only the reduced image
    // crosses to the display GPU, instead of the full-resolution owned copy.
    bool source_downsample_enabled_ = false;
    int source_downsample_factor_ = 1;
    unsigned char* d_source_downsample_buffer_ = nullptr;  // acquisition GPU
    cudaStream_t source_stream_ = nullptr;                 // acquisition GPU
    cudaEvent_t source_done_event_ = nullptr;              // acquisition GPU
    uint64_t display_source_downsample_frames_ = 0;
    std::atomic<uint64_t> preview_serial_{0};
    std::chrono::steady_clock::time_point last_display_log_time_;

    cudaStream_t m_stream;
    SafeQueue<WORKER_ENTRY*>& m_recycle_queue;
};
