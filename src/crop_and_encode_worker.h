#ifndef CROP_AND_ENCODE_WORKER_H
#define CROP_AND_ENCODE_WORKER_H

#include "threadworker.h"
#include "video_capture.h"
#include "gpu_video_encoder.h" // For Writer struct
#include "FFmpegWriter.h"
#include "NvEncoder/NvEncoderCuda.h"
#include "image_processing.h"
#include <chrono>
#include <cstdint>
#include <fstream>
#include <vector>

class CropAndEncodeWorker : public CThreadWorker<WORKER_ENTRY> {
public:
    static constexpr int kDefaultCropSize = CameraCropPipelineConfig::kDefaultCropSizePx;
    static constexpr int kMinCropSize = CameraCropPipelineConfig::kMinCropSizePx;
    static constexpr int kMaxCropSize = CameraCropPipelineConfig::kMaxCropSizePx;

    static int SanitizeCropSize(int requested_size_px);

    CropAndEncodeWorker(
        const char* name,
        CameraParams *camera_params,
        const std::string& folder_name,
        SafeQueue<WORKER_ENTRY*>& recycle_queue,
        unsigned char* display_buffer_pbo,
        CameraControl* camera_control,
        int crop_size_px
    );
    ~CropAndEncodeWorker() override;

    void flush_and_close();
    void finalize_recording();
    int crop_width() const { return crop_width_; }
    int crop_height() const { return crop_height_; }

protected:
    bool WorkerFunction(WORKER_ENTRY* f) override;

private:
    struct CropPerfSample {
        uint64_t worker_start_steady_ns = 0;
        int queue_depth_start = 0;
        bool encode_active = false;
        bool has_detection = false;
        bool blank_frame = false;
        bool dropped = false;
        const char* drop_reason = "";
        int crop_x = 0;
        int crop_y = 0;
        int crop_w = 0;
        int crop_h = 0;
        size_t packet_count = 0;
        size_t encoded_bytes = 0;
        double event_wait_cpu_ms = 0.0;
        double crop_preview_cpu_ms = 0.0;
        double encode_submit_cpu_ms = 0.0;
        double metadata_cpu_ms = 0.0;
        double stream_sync_ms = 0.0;
        double display_sync_ms = 0.0;
        double total_ms = 0.0;
    };

    bool drain_ready();
    bool ensure_recording_started(const std::string& recording_folder);
    void push_encoded_packets(std::vector<std::vector<uint8_t>>& packets,
                              const std::vector<uint64_t>& output_timestamps,
                              uint64_t fallback_zero_based_frame);
    void write_metadata_row(const WORKER_ENTRY* entry,
                            bool has_detection,
                            bool blank_frame,
                            float detection_confidence,
                            int crop_x,
                            int crop_y,
                            int crop_w,
                            int crop_h,
                            float detection_x,
                            float detection_y,
                            float detection_w,
                            float detection_h);
    void write_perf_row(const WORKER_ENTRY* entry, const CropPerfSample& sample);
    void release_entry(WORKER_ENTRY* entry);
    bool display_cuda_ok(cudaError_t status, const char* operation);
    void copy_crop_to_display_preview();
    void clear_display_preview();
    void synchronize_display_preview();
    size_t crop_preview_bytes() const;

    uint64_t last_frame_id_used_ = 0;
    CameraParams* camera_params_;
    std::string base_folder_name_; // Renamed from folder_name_
    int crop_width_ = kDefaultCropSize;
    int crop_height_ = kDefaultCropSize;
    Writer writer_;
    NvEncoderCuda* encoder_ = nullptr;
    unsigned char* d_cropped_bgr_ = nullptr;
    unsigned char* d_yuv_buffer_ = nullptr;
    unsigned char* d_blank_frame_ = nullptr;
    unsigned char* d_cropped_rgba_ = nullptr;
    unsigned char* d_display_buffer_pbo_ = nullptr;
    unsigned char* h_display_crop_ = nullptr;
    int encoder_pitch_ = 0;
    cudaStream_t m_stream = nullptr;
    cudaStream_t m_display_stream = nullptr;
    std::string crop_perf_file_;
    std::ofstream crop_perf_;
    int frame_counter_ = 0;
    SafeQueue<WORKER_ENTRY*>& m_recycle_queue;
    Debayer debayer_gpu_;
    CameraControl* camera_control_;
    bool is_recording_ = false;
    bool encoder_flushed_ = false;
    bool display_preview_disabled_ = false;
};

#endif // CROP_AND_ENCODE_WORKER_H
