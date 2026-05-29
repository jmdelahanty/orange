#ifndef ORANGE_CROP_PRODUCER_WORKER_H
#define ORANGE_CROP_PRODUCER_WORKER_H

#include "crop_pipeline_types.h"
#include "threadworker.h"
#include "video_capture.h"

#include <atomic>
#include <mutex>
#include <memory>
#include <string>

class CropAndEncodeWorker;
class CropPreviewWorker;
class PoseWorker;

class CropProducerWorker : public CThreadWorker<WORKER_ENTRY> {
public:
    struct RecordingCounters {
        uint64_t jobs_offered = 0;
        uint64_t jobs_enqueued = 0;
        uint64_t queue_full_drops = 0;
        uint64_t blank_jobs_offered = 0;
        uint64_t blank_jobs_enqueued = 0;
        uint64_t dropped_jobs_offered = 0;
        uint64_t dropped_jobs_enqueued = 0;
    };

    static constexpr int kDefaultCropSize = CameraCropPipelineConfig::kDefaultCropSizePx;
    static constexpr int kMinCropSize = CameraCropPipelineConfig::kMinCropSizePx;
    static constexpr int kMaxCropSize = CameraCropPipelineConfig::kMaxCropSizePx;

    static int SanitizeCropSize(int requested_size_px);

    CropProducerWorker(
        const char* name,
        CameraParams* camera_params,
        SafeQueue<WORKER_ENTRY*>& recycle_queue,
        CameraControl* camera_control,
        int crop_size_px);
    ~CropProducerWorker() override;

    void SetCropAndEncodeWorker(CropAndEncodeWorker* crop_worker);
    void SetCropPreviewWorker(CropPreviewWorker* crop_preview_worker);
    void SetPoseWorker(PoseWorker* pose_worker);
    CropProducer* GetCropProducer() const { return crop_producer_.get(); }
    void RotateRecordingFolder(const std::string& recording_folder);
    void CloseRecording();
    void ResetRecordingCounters();
    RecordingCounters GetRecordingCounters() const;
    bool ProcessEntryInline(WORKER_ENTRY* entry);

protected:
    bool WorkerFunction(WORKER_ENTRY* entry) override;

private:
    bool ProcessEntryImpl(WORKER_ENTRY*& entry, bool release_source_entry);
    bool ForwardRecordingDrainIfReady();

    CameraParams* camera_params_ = nullptr;
    SafeQueue<WORKER_ENTRY*>& recycle_queue_;
    CameraControl* camera_control_ = nullptr;
    int crop_width_ = kDefaultCropSize;
    int crop_height_ = kDefaultCropSize;
    std::unique_ptr<CropProducer> crop_producer_;
    CropAndEncodeWorker* crop_worker_ = nullptr;
    CropPreviewWorker* crop_preview_worker_ = nullptr;
    PoseWorker* pose_worker_ = nullptr;
    bool pose_enabled_ = false;
    uint64_t jobs_offered_ = 0;
    uint64_t jobs_enqueued_ = 0;
    uint64_t queue_full_drops_ = 0;
    uint64_t blank_jobs_offered_ = 0;
    uint64_t blank_jobs_enqueued_ = 0;
    uint64_t dropped_jobs_offered_ = 0;
    uint64_t dropped_jobs_enqueued_ = 0;
    std::atomic<uint64_t> run_jobs_offered_{0};
    std::atomic<uint64_t> run_jobs_enqueued_{0};
    std::atomic<uint64_t> run_queue_full_drops_{0};
    std::atomic<uint64_t> run_blank_jobs_offered_{0};
    std::atomic<uint64_t> run_blank_jobs_enqueued_{0};
    std::atomic<uint64_t> run_dropped_jobs_offered_{0};
    std::atomic<uint64_t> run_dropped_jobs_enqueued_{0};
    std::string current_recording_folder_;
    bool recording_drain_forwarded_ = false;
    std::mutex process_mutex_;
};

#endif  // ORANGE_CROP_PRODUCER_WORKER_H
