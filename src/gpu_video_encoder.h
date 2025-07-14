// src/gpu_video_encoder.h

#ifndef ORANGE_GPU_VIDEO_ENCODER
#define ORANGE_GPU_VIDEO_ENCODER

#include "threadworker.h"
#include "video_capture.h"
#include "FFmpegWriter.h"
#include "NvEncoder/NvEncoderCuda.h"
#include "image_processing.h" // For Debayer, FrameGPU
#include "thread.h" // For SafeQueue
#include <chrono>   // For FPS tracking
#include <cuda.h>   // For CUcontext
#include <fstream>  // For std::ofstream

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
    CUcontext cuContext;
    unsigned long long num_frame_encode = 0;
    std::vector<std::vector<uint8_t>> vPacket;
    NvEncoderCuda *pEnc;
};

class GPUVideoEncoder : public CThreadWorker<ProcessedFrame>
{
public:
    GPUVideoEncoder(
        const char* name,
        CameraParams *camera_params,
        const std::string& codec,
        const std::string& preset,
        const std::string& tuning,
        std::string folder_name,
        bool* encoder_ready_signal,
        SafeQueue<WORKER_ENTRY*>& raw_recycle_queue,
        SafeQueue<ProcessedFrame*>& processed_recycle_queue
    );
    ~GPUVideoEncoder() override;

    double get_fps() const {
        return current_fps_;
    }

    bool* encoder_ready_signal;

protected:
    bool WorkerFunction(ProcessedFrame* f) override;

private:
    CameraParams* camera_params;
    std::string folder_name;
    EncoderContext encoder;
    Writer writer;
    cudaStream_t m_stream = nullptr;
    int encoder_pitch_ = 0;

    // The worker now needs access to both recycle queues to properly manage memory.
    SafeQueue<WORKER_ENTRY*>& m_raw_recycle_queue;
    SafeQueue<ProcessedFrame*>& m_processed_recycle_queue;

    // FPS tracking members
    std::chrono::steady_clock::time_point last_fps_update_time_;
    int frame_counter_ = 0;
    double current_fps_ = 0.0;

    // --- Cleaned up members for the new pipeline ---

    // Intermediate GPU buffer for color space conversion (RGBA from preprocessor -> NV12 for encoder)
    unsigned char* d_iyuv_temp_ = nullptr;

    // If your color conversion goes RGBA -> RGB -> IYUV, you still need this.
    unsigned char* d_rgb_temp_ = nullptr;

    // This is still useful for monochrome cameras to quickly fill the chroma planes.
    unsigned char* d_uv_default_plane_ = nullptr;
};

#endif // ORANGE_GPU_VIDEO_ENCODER