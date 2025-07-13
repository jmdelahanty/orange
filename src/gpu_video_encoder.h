// src/gpu_video_encoder.h

#ifndef ORANGE_GPU_VIDEO_ENCODER
#define ORANGE_GPU_VIDEO_ENCODER

#include "threadworker.h"
#include "video_capture.h"
#include "FFmpegWriter.h"
#include "NvEncoder/NvEncoderCuda.h"
#include "NvEncoder/NvEncoderCLIOptions.h"
#include "image_processing.h"
#include "thread.h" // For SafeQueue
#include <chrono>   // For FPS tracking
#include <cuda.h>   // For CUcontext

struct Writer
{
    std::string video_file;
    std::string keyframe_file;
    std::string metadata_file;
    FFmpegWriter *video;
    std::ofstream* metadata;
};

struct EncoderContext
{
    NV_ENC_BUFFER_FORMAT eFormat;
    NvEncoderInitParam encodeCLIOptions;
    CUcontext cuContext;
    unsigned long long num_frame_encode = 0;
    std::vector<std::vector<uint8_t>> vPacket;
    NvEncoderCuda *pEnc;
};

class GPUVideoEncoder : public CThreadWorker<ProcessedFrame>
{
public:
    GPUVideoEncoder(const char* name,
                    CameraParams* camera_params,
                    const std::string& codec,
                    const std::string& preset,
                    const std::string& tuning,
                    std::string folder_name,
                    bool* encoder_ready_signal,
                    // This worker receives frames from the preprocessor via this queue
                    SafeQueue<ProcessedFrame*>* input_queue,
                    SafeQueue<WORKER_ENTRY*>& raw_recycle_queue,
                    SafeQueue<ProcessedFrame*>& processed_recycle_queue);
    ~GPUVideoEncoder() override;

    double get_fps() const { return current_fps_; }
    bool* encoder_ready_signal;

protected:
    bool WorkerFunction(ProcessedFrame* f) override;

private:
    // Overriding the base class's loop to use our own input source.
    void ThreadRunning() override;

    CameraParams* camera_params;
    std::string folder_name;
    FrameGPU frame_original;
    Debayer debayer;
    EncoderContext encoder;
    Writer writer;
    cudaStream_t m_stream;
    int encoder_pitch_;
    int scaled_width_;
    int scaled_height_;
    unsigned char* d_scaled_mono_buffer_;
    unsigned char* d_rgb_temp_;
    unsigned char* d_iyuv_temp_;
    unsigned char* d_uv_default_plane_;

    // Pointer to the shared input queue.
    SafeQueue<ProcessedFrame*>* m_input_queue;
    SafeQueue<WORKER_ENTRY*>& m_recycle_queue;
    SafeQueue<ProcessedFrame*>& m_processed_recycle_queue;

    std::chrono::steady_clock::time_point last_fps_update_time_;
    int frame_counter_;
    double current_fps_;
};


#endif // ORANGE_GPU_VIDEO_ENCODER