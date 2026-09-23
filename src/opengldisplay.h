// src/opengldisplay.h

#pragma once
#include "threadworker.h"
#include <mutex>
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

public:
    // Exposure watch: about once per second the downsampled preview is read
    // back and its mean and clipped fraction are recorded; a jump is logged.
    // This is the only place a physically open or closed iris shows up, since
    // the EF mount's iris counter is a step count, not a position sensor.
    struct ExposureWatchStats {
        uint64_t samples = 0;
        uint64_t changes = 0;          // mean moved >8% or clipped fraction >0.10 vs the previous sample
        double first_mean = 0.0;
        double last_mean = 0.0;
        double min_mean = 0.0;
        double max_mean = 0.0;
        double last_clip = 0.0;        // fraction of preview pixels >= 250
        double max_clip = 0.0;
        double last_sample_stream_s = 0.0;
        double last_change_stream_s = -1.0;
    };
    ExposureWatchStats exposure_watch() const { return exposure_watch_; }

    // Intensity histogram of the downsampled preview, averaged over N
    // displayed frames, on request from the GUI (Intensity Histogram panel).
    struct IntensityHistogram {
        uint64_t sequence = 0;         // increments per completed grab
        int frames_requested = 0;
        int frames_accumulated = 0;
        bool complete = false;
        double fraction[256] = {};     // fraction of pixels per intensity value, averaged over frames
        double mean = 0.0;
        double p1 = 0.0;
        double p50 = 0.0;
        double p99 = 0.0;
        double clip_fraction = 0.0;    // pixels >= 250
        double dark_fraction = 0.0;    // pixels < 8
    };
    void RequestIntensityHistogram(int frames);
    bool GetIntensityHistogram(IntensityHistogram* out);   // copy of the last completed grab
private:
    void intensity_histogram_sample(CameraParams* camera_params, bool cross_gpu, size_t ds_bytes);
    std::mutex histogram_mutex_;
    IntensityHistogram histogram_;
    std::atomic<int> histogram_frames_pending_{0};
    uint64_t histogram_accum_[256] = {};
    int histogram_accum_frames_ = 0;
    int histogram_accum_requested_ = 0;

    void exposure_watch_sample(CameraParams* camera_params, bool cross_gpu, size_t ds_bytes);
    ExposureWatchStats exposure_watch_;
    unsigned char* h_exposure_sample_ = nullptr;           // host copy of the preview for the watch
    std::chrono::steady_clock::time_point exposure_watch_last_sample_time_{};
    std::chrono::steady_clock::time_point exposure_watch_start_time_{};
    std::atomic<uint64_t> preview_serial_{0};
    std::chrono::steady_clock::time_point last_display_log_time_;

    cudaStream_t m_stream;
    SafeQueue<WORKER_ENTRY*>& m_recycle_queue;
};
