#ifndef CROP_AND_ENCODE_WORKER_H
#define CROP_AND_ENCODE_WORKER_H

#include "threadworker.h"
#include "video_capture.h"
#include "gpu_video_encoder.h" // For Writer struct
#include "FFmpegWriter.h"
#include "NvEncoder/NvEncoderCuda.h"
#include "image_processing.h"
#include <atomic>
#include <chrono>
#include <cstdint>
#include <deque>
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
        double crop_pool_wait_ms = 0.0;
        double crop_producer_cpu_ms = 0.0;
        double crop_source_wait_enqueue_cpu_ms = 0.0;
        double source_stage_enqueue_cpu_ms = 0.0;
        double crop_copy_start_event_record_cpu_ms = 0.0;
        double crop_roi_copy_enqueue_cpu_ms = 0.0;
        double crop_ready_event_record_cpu_ms = 0.0;
        double source_release_event_record_cpu_ms = 0.0;
        double crop_copy_gpu_ms = -1.0;
        double crop_preview_cpu_ms = 0.0;
        double encode_submit_cpu_ms = 0.0;
        double metadata_cpu_ms = 0.0;
        double stream_sync_ms = 0.0;
        double display_sync_ms = 0.0;
        double total_ms = 0.0;
    };

    struct CropFrameSnapshot {
        uint64_t recording_frame_id = 0;
        uint64_t local_frame_id = 0;
        uint64_t camera_frame_id = 0;
        uint64_t timestamp = 0;
        uint64_t timestamp_sys = 0;
        int source_width = 0;
        int source_height = 0;
        bool has_detection = false;
        bool blank_frame = false;
        float detection_confidence = 0.0f;
        int crop_x = 0;
        int crop_y = 0;
        int crop_w = 0;
        int crop_h = 0;
        float detection_x = 0.0f;
        float detection_y = 0.0f;
        float detection_w = 0.0f;
        float detection_h = 0.0f;
    };

    struct PendingSourceRelease {
        WORKER_ENTRY* entry = nullptr;
        cudaEvent_t* event = nullptr;
    };

    struct CropFrame {
        unsigned char* d_crop_mono = nullptr;
        cudaEvent_t crop_copy_start_event = nullptr;
        cudaEvent_t crop_copy_stop_event = nullptr;
        cudaEvent_t crop_ready_event = nullptr;
        cudaEvent_t recycle_event = nullptr;
        CropFrameSnapshot frame;
    };

    struct PendingCropFrameRecycle {
        CropFrame* crop_frame = nullptr;
    };

    bool drain_ready();
    bool ensure_recording_started(const std::string& recording_folder);
    void push_encoded_packets(std::vector<std::vector<uint8_t>>& packets,
                              const std::vector<uint64_t>& output_timestamps,
                              uint64_t fallback_zero_based_frame);
    void write_metadata_row(const CropFrameSnapshot& frame);
    void write_perf_row(const CropFrameSnapshot& frame, const CropPerfSample& sample);
    void release_entry(WORKER_ENTRY* entry);
    cudaEvent_t* acquire_source_release_event();
    void defer_source_release(WORKER_ENTRY* entry, cudaEvent_t* event);
    void drain_pending_source_releases(bool synchronize_all);
    CropFrame* acquire_crop_frame();
    void recycle_crop_frame(CropFrame* crop_frame);
    void defer_crop_frame_recycle(CropFrame* crop_frame);
    void drain_pending_crop_frames(bool synchronize_all);
    bool display_cuda_ok(cudaError_t status, const char* operation);
    void copy_crop_to_display_preview();
    void clear_display_preview();
    void synchronize_display_preview();
    size_t crop_preview_bytes() const;
    size_t crop_mono_bytes() const;
    void ensure_source_stage_buffer(int width, int height);

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
    unsigned char* d_source_stage_mono_ = nullptr;
    unsigned char* d_display_buffer_pbo_ = nullptr;
    unsigned char* h_display_crop_ = nullptr;
    int encoder_pitch_ = 0;
    int source_stage_width_ = 0;
    int source_stage_height_ = 0;
    cudaStream_t m_stream = nullptr;
    cudaStream_t m_crop_producer_stream = nullptr;
    cudaStream_t m_display_stream = nullptr;
    std::string crop_perf_file_;
    std::ofstream crop_perf_;
    std::vector<cudaEvent_t> source_release_event_pool_;
    SafeQueue<cudaEvent_t*> free_source_release_events_;
    std::deque<PendingSourceRelease> pending_source_releases_;
    std::vector<CropFrame> crop_frame_pool_;
    SafeQueue<CropFrame*> free_crop_frames_;
    std::deque<PendingCropFrameRecycle> pending_crop_frame_recycles_;
    bool crop_copy_timing_enabled_ = true;
    bool crop_copy_kernel_enabled_ = false;
    bool crop_source_stage_enabled_ = false;
    std::atomic<int> pending_source_release_count_{0};
    std::atomic<int> pending_crop_frame_recycle_count_{0};
    std::atomic<uint64_t> source_release_event_misses_{0};
    std::atomic<uint64_t> crop_frame_pool_misses_{0};
    int frame_counter_ = 0;
    SafeQueue<WORKER_ENTRY*>& m_recycle_queue;
    Debayer debayer_gpu_;
    CameraControl* camera_control_;
    bool is_recording_ = false;
    bool encoder_flushed_ = false;
    bool display_preview_disabled_ = false;
};

#endif // CROP_AND_ENCODE_WORKER_H
