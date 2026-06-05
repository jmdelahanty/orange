#ifndef ORANGE_CROP_PREVIEW_WORKER_H
#define ORANGE_CROP_PREVIEW_WORKER_H

#include "crop_preview_cadence.h"
#include "crop_pipeline_types.h"
#include "threadworker.h"
#include "video_capture.h"

#include <atomic>
#include <cstdint>
#include <mutex>

class CropProducerWorker;

class CropPreviewWorker : public CThreadWorker<CropPreviewJob> {
public:
    struct Summary {
        int max_fps = CameraCropPipelineConfig::kDefaultPreviewMaxFps;
        bool available = false;
        bool display_enabled = false;
        uint64_t frames_offered = 0;
        uint64_t frames_updated = 0;
        uint64_t frames_skipped_by_cadence = 0;
        uint64_t clears_updated = 0;
        uint64_t queue_full_drops = 0;
        int queue_high_water = 0;
        uint64_t serial = 0;
    };

    static constexpr int kDefaultQueueSize = 32;

    CropPreviewWorker(
        const char* name,
        CameraParams* camera_params,
        unsigned char* display_buffer_pbo,
        CropProducer* crop_producer,
        int crop_size_px);
    ~CropPreviewWorker() override;

    void SetMaxQueueSize(int size);
    void SetCropProducerWorker(CropProducerWorker* crop_producer_worker) { crop_producer_worker_ = crop_producer_worker; }
    void SetPreviewDisplayEnabled(bool enabled);
    CropPreviewCadence::Decision EvaluateOffer(bool blank_preview);
    bool TryEnqueuePreview(CropPreviewJob* job);
    uint64_t PreviewSerial() const { return preview_serial_.load(std::memory_order_acquire); }
    void ResetRunCounters();
    void WaitUntilIdle(int timeout_ms);
    Summary GetSummary() const;

protected:
    void OnQueueInDequeued(CropPreviewJob* job, int queue_depth) override;
    bool WorkerFunction(CropPreviewJob* job) override;

private:
    bool display_cuda_ok(cudaError_t status, const char* operation);
    void copy_crop_to_display_preview();
    void clear_display_preview();
    void synchronize_display_preview();
    bool crop_preview_available() const;
    bool crop_preview_active() const;
    void mark_display_preview_updated(bool blank_preview);
    void release_job(CropPreviewJob* job);
    size_t crop_preview_bytes() const;

    CameraParams* camera_params_ = nullptr;
    CropProducer* crop_producer_ = nullptr;
    CropProducerWorker* crop_producer_worker_ = nullptr;
    int crop_width_ = CameraCropPipelineConfig::kDefaultCropSizePx;
    int crop_height_ = CameraCropPipelineConfig::kDefaultCropSizePx;
    unsigned char* d_cropped_rgba_ = nullptr;
    unsigned char* d_display_buffer_pbo_ = nullptr;
    unsigned char* h_display_crop_ = nullptr;
    cudaStream_t stream_ = nullptr;
    cudaStream_t display_stream_ = nullptr;
    std::atomic<bool> display_preview_disabled_{false};
    CropPreviewCadence preview_cadence_;
    mutable std::mutex preview_mutex_;
    int max_queue_size_ = kDefaultQueueSize;
    std::atomic<uint64_t> preview_serial_{0};
    std::atomic<uint64_t> frames_offered_{0};
    std::atomic<uint64_t> frames_updated_{0};
    std::atomic<uint64_t> frames_skipped_by_cadence_{0};
    std::atomic<uint64_t> clears_updated_{0};
    std::atomic<uint64_t> queue_full_drops_{0};
    std::atomic<int> queue_high_water_{0};
    std::atomic<int> active_jobs_{0};
};

#endif  // ORANGE_CROP_PREVIEW_WORKER_H
