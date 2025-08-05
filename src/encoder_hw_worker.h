// src/encoder_hw_worker.h

#ifndef ENCODER_HW_WORKER_H
#define ENCODER_HW_WORKER_H

#include "threadworker.h"
#include "video_capture.h"
#include <chrono>
#include <fstream>
#include "encoder_pipeline.h"

class EncoderPreprocessWorker; // Forward declaration

class EncoderHwWorker : public CThreadWorker<ENCODER_WORKER_ENTRY>
{
public:
    EncoderHwWorker(
        const char* name,
        CameraParams* camera_params,
        const std::string& codec,
        const std::string& preset,
        const std::string& tuning,
        std::string folder_name,
        EncoderPreprocessWorker* prep_worker,
        CameraControl* camera_control
    );
    ~EncoderHwWorker() override;

    void flush_and_close();

    double get_fps() const {
        return current_fps_;
    }

    EncoderContext encoder_;
    EncoderPreprocessWorker* m_prep_worker_;

protected:
    bool WorkerFunction(ENCODER_WORKER_ENTRY* f) override;

private:
    CameraParams* camera_params_;
    std::string folder_name_;
    std::string codec_;
    Writer writer_;
    cudaStream_t m_stream = nullptr;
    CameraControl* camera_control_;

    uint64_t last_recording_frame_id_ = 0;
    std::chrono::steady_clock::time_point last_fps_update_time_;
    int frame_counter_;
    double current_fps_;
};

#endif // ENCODER_HW_WORKER_H