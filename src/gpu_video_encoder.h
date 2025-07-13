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

class GPUVideoEncoder : public CThreadWorker<WORKER_ENTRY>
{
public:
    GPUVideoEncoder(const char* name, CameraParams *camera_params,
        const std::string& codec, const std::string& preset, const std::string& tuning,
        std::string folder_name, bool* encoder_ready_signal,
        SafeQueue<WORKER_ENTRY*>* input_queue, // This worker gets its input from here
        SafeQueue<WORKER_ENTRY*>& recycle_queue); // This is where it recycles entries

    ~GPUVideoEncoder() override;

    double get_fps() const {
        return current_fps_;
    }
    bool* encoder_ready_signal;

protected:
    bool WorkerFunction(WORKER_ENTRY* f) override;

private:
    void ThreadRunning() override;

    CameraParams* camera_params;
    std::string folder_name;
    EncoderContext encoder;
    Writer writer;
    cudaStream_t m_stream;
    int encoder_pitch_;

    SafeQueue<WORKER_ENTRY*>* m_input_queue;
    SafeQueue<WORKER_ENTRY*>& m_recycle_queue;

    // FPS tracking
    std::chrono::steady_clock::time_point last_fps_update_time_;
    int frame_counter_;
    double current_fps_;
};

#endif // ORANGE_GPU_VIDEO_ENCODER