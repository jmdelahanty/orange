// src/encoder_hw_worker.h

#ifndef ENCODER_HW_WORKER_H
#define ENCODER_HW_WORKER_H

#include "threadworker.h"
#include "video_capture.h"
#include "gpu_video_encoder.h"
#include <chrono>
#include <fstream>
#include "encoder_pipeline.h"
#include "json.hpp"

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

    double get_fps() const { return current_fps_; }
    uint64_t get_total_packets() const { return total_packets_.load(); }
    uint64_t get_encode_failures() const { return encode_failures_.load(); }

    EncoderContext encoder_;
    EncoderPreprocessWorker* m_prep_worker_;

protected:
    bool WorkerFunction(ENCODER_WORKER_ENTRY* f) override;

private:
    void finalize_recording();
    bool drain_ready();
    nlohmann::json build_encoder_snapshot_json() const;

    struct EncoderSnapshotInfo {
        std::string backend;
        std::string path;
        std::string codec;
        std::string preset;
        std::string tuning;
        uint32_t width = 0;
        uint32_t height = 0;
        uint32_t fps = 0;
        uint32_t gop_length = 0;
        uint32_t frame_interval_p = 0;
        uint32_t idr_period = 0;
        uint32_t max_num_ref_frames = 0;
        uint32_t max_num_ref_frames_in_dpb = 0;
        uint32_t rc_mode = 0;
        uint32_t average_bitrate = 0;
        uint32_t max_bitrate = 0;
        uint32_t vbv_buffer_size = 0;
        uint32_t enable_aq = 0;
        uint32_t enable_temporal_aq = 0;
        uint32_t enable_lookahead = 0;
        uint32_t low_delay_keyframe_scale = 0;
        uint32_t strict_gop_target = 0;
        uint32_t enable_non_ref_p = 0;
        uint32_t repeat_sps_pps = 0;
        uint32_t enable_ptd = 0;
        int gpu_id = -1;
        bool color = false;
    };

    CameraParams* camera_params_;
    std::string base_folder_name_;
    std::string codec_;
    std::string preset_;
    std::string tuning_;
    Writer writer_;
    cudaStream_t m_stream = nullptr;
    CameraControl* camera_control_;

    bool is_recording_ = false; // Tracks the local recording state of this worker
    bool encoder_snapshot_valid_ = false;
    EncoderSnapshotInfo encoder_snapshot_;

    uint64_t last_recording_frame_id_ = 0;
    std::chrono::steady_clock::time_point last_fps_update_time_;
    int frame_counter_;
    double current_fps_;
    std::atomic<uint64_t> slow_frames_{0};
    std::atomic<uint64_t> total_packets_{0};
    std::atomic<uint64_t> encode_failures_{0};
};

#endif // ENCODER_HW_WORKER_H
