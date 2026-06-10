#ifndef CROP_AND_ENCODE_WORKER_H
#define CROP_AND_ENCODE_WORKER_H

#include "crop_pipeline_types.h"
#include "threadworker.h"
#include "video_capture.h"
#include "recording_writer_types.h"
#include "FFmpegWriter.h"
#include "NvEncoder/NvEncoderCuda.h"
#include "image_processing.h"
#include <atomic>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <memory>
#include <vector>

class CropProducer;
class CropProducerWorker;
class CropPreviewWorker;

class CropAndEncodeWorker : public CThreadWorker<CropEncodeJob> {
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
    void SetMaxQueueSize(int size);
    bool TryEnqueueJob(CropEncodeJob* job);
    void SetCropProducer(CropProducer* crop_producer) { crop_producer_ = crop_producer; }
    void SetCropProducerWorker(CropProducerWorker* crop_producer_worker) { crop_producer_worker_ = crop_producer_worker; }
    void SetCropPreviewWorker(CropPreviewWorker* crop_preview_worker) { crop_preview_worker_ = crop_preview_worker; }
    void RotateRecordingFolder(const std::string& recording_folder);
    int crop_width() const { return crop_width_; }
    int crop_height() const { return crop_height_; }

protected:
    bool WorkerFunction(CropEncodeJob* f) override;
    void OnFlushTick() override;  // drain notify + finalize crop recording

private:
    class ExternalCropIpcClient;

    bool drain_ready();
    bool ensure_recording_started(const std::string& recording_folder);
    bool external_crop_recording_enabled() const;
    bool submit_external_crop_frame(const CropFrameSnapshot& frame,
                                    unsigned char* d_crop_mono,
                                    CropEncodePerfSample* perf);
    void push_encoded_packets(std::vector<std::vector<uint8_t>>& packets,
                              const std::vector<uint64_t>& output_timestamps,
                              uint64_t fallback_zero_based_frame);
    void write_metadata_row(const CropFrameSnapshot& frame);
    void write_perf_row(const CropFrameSnapshot& frame, const CropEncodePerfSample& sample);
    void write_sidecar_summary();
    void reset_recording_counters();
    void release_job(CropEncodeJob* job);

    uint64_t last_frame_id_used_ = 0;
    CameraParams* camera_params_;
    std::string base_folder_name_; // Renamed from folder_name_
    int crop_width_ = kDefaultCropSize;
    int crop_height_ = kDefaultCropSize;
    Writer writer_;
    NvEncoderCuda* encoder_ = nullptr;
    std::unique_ptr<ExternalCropIpcClient> external_crop_ipc_;
    unsigned char* d_cropped_bgr_ = nullptr;
    unsigned char* d_yuv_buffer_ = nullptr;
    unsigned char* d_blank_frame_ = nullptr;
    int encoder_pitch_ = 0;
    cudaStream_t m_stream = nullptr;
    std::string crop_perf_file_;
    std::ofstream crop_perf_;
    CropProducer* crop_producer_ = nullptr;
    CropProducerWorker* crop_producer_worker_ = nullptr;
    CropPreviewWorker* crop_preview_worker_ = nullptr;
    std::string crop_sidecar_perf_file_;
    std::string current_sidecar_recording_folder_;
    int max_queue_size_ = 40;
    std::atomic<uint64_t> jobs_enqueued_{0};
    std::atomic<uint64_t> queue_full_drops_{0};
    std::atomic<int> queue_high_water_{0};
    uint64_t run_jobs_enqueued_ = 0;
    uint64_t run_queue_full_drops_ = 0;
    int run_queue_high_water_ = 0;
    int frame_counter_ = 0;
    SafeQueue<WORKER_ENTRY*>& m_recycle_queue;
    Debayer debayer_gpu_;
    CameraControl* camera_control_;
    bool is_recording_ = false;
    bool encoder_flushed_ = false;
    std::string crop_recording_sink_mode_ = "in_process";
    uint64_t encoded_frames_ = 0;
    uint64_t blank_frames_encoded_ = 0;
    uint64_t dropped_frames_ = 0;
};

#endif // CROP_AND_ENCODE_WORKER_H
