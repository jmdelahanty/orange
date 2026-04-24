#ifndef ORANGE_CROP_PRODUCER_H
#define ORANGE_CROP_PRODUCER_H

#include "video_capture.h"
#include <atomic>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <vector>

class PoseWorker;

struct CropFrameSnapshot {
    uint64_t recording_frame_id = 0;
    uint64_t local_frame_id = 0;
    uint64_t camera_frame_id = 0;
    uint64_t timestamp = 0;
    uint64_t timestamp_sys = 0;
    std::string recording_folder;
    int source_width = 0;
    int source_height = 0;
    uint64_t acquisition_receive_host_ns = 0;
    uint64_t yolo_detect_done_host_ns = 0;
    uint64_t crop_producer_worker_start_host_ns = 0;
    uint64_t crop_ready_host_ns = 0;
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

struct CropFrame {
    unsigned char* d_crop_mono = nullptr;
    cudaEvent_t crop_copy_start_event = nullptr;
    cudaEvent_t crop_copy_stop_event = nullptr;
    cudaEvent_t crop_ready_event = nullptr;
    cudaEvent_t recycle_event = nullptr;
    std::atomic<int> active_leases{0};
    CropFrameSnapshot frame;

    CropFrame() = default;

    CropFrame(const CropFrame& other)
        : d_crop_mono(other.d_crop_mono),
          crop_copy_start_event(other.crop_copy_start_event),
          crop_copy_stop_event(other.crop_copy_stop_event),
          crop_ready_event(other.crop_ready_event),
          recycle_event(other.recycle_event),
          active_leases(other.active_leases.load(std::memory_order_relaxed)),
          frame(other.frame) {}

    CropFrame& operator=(const CropFrame& other)
    {
        if (this != &other) {
            d_crop_mono = other.d_crop_mono;
            crop_copy_start_event = other.crop_copy_start_event;
            crop_copy_stop_event = other.crop_copy_stop_event;
            crop_ready_event = other.crop_ready_event;
            recycle_event = other.recycle_event;
            active_leases.store(other.active_leases.load(std::memory_order_relaxed), std::memory_order_relaxed);
            frame = other.frame;
        }
        return *this;
    }

    CropFrame(CropFrame&& other) noexcept
        : d_crop_mono(other.d_crop_mono),
          crop_copy_start_event(other.crop_copy_start_event),
          crop_copy_stop_event(other.crop_copy_stop_event),
          crop_ready_event(other.crop_ready_event),
          recycle_event(other.recycle_event),
          active_leases(other.active_leases.load(std::memory_order_relaxed)),
          frame(std::move(other.frame)) {}

    CropFrame& operator=(CropFrame&& other) noexcept
    {
        if (this != &other) {
            d_crop_mono = other.d_crop_mono;
            crop_copy_start_event = other.crop_copy_start_event;
            crop_copy_stop_event = other.crop_copy_stop_event;
            crop_ready_event = other.crop_ready_event;
            recycle_event = other.recycle_event;
            active_leases.store(other.active_leases.load(std::memory_order_relaxed), std::memory_order_relaxed);
            frame = std::move(other.frame);
        }
        return *this;
    }
};

struct CropProducerPerfSample {
    double event_wait_cpu_ms = 0.0;
    double crop_pool_wait_ms = 0.0;
    double crop_producer_cpu_ms = 0.0;
    double crop_source_wait_enqueue_cpu_ms = 0.0;
    double analytics_owned_wait_cpu_ms = 0.0;
    double source_stage_enqueue_cpu_ms = 0.0;
    double crop_copy_start_event_record_cpu_ms = 0.0;
    double crop_roi_copy_enqueue_cpu_ms = 0.0;
    double crop_ready_event_record_cpu_ms = 0.0;
    double source_release_event_record_cpu_ms = 0.0;
    double crop_copy_gpu_ms = -1.0;
};

class CropProducer {
public:
    struct ProduceResult {
        CropFrame* crop_frame = nullptr;
        bool dropped = false;
        const char* drop_reason = "";
    };

    CropProducer(
        CameraParams* camera_params,
        SafeQueue<WORKER_ENTRY*>& recycle_queue,
        int crop_width,
        int crop_height);
    ~CropProducer();

    CropProducer(const CropProducer&) = delete;
    CropProducer& operator=(const CropProducer&) = delete;

    ProduceResult Produce(
        WORKER_ENTRY*& entry,
        const CropFrameSnapshot& frame,
        int crop_x,
        int crop_y,
        bool needs_crop_frame,
        CropProducerPerfSample* perf,
        bool release_source_entry = true);

    void ReleaseSourceEntry(WORKER_ENTRY*& entry);
    void SetPoseWorker(PoseWorker* pose_worker);
    void RecycleAfterConsumerStream(CropFrame* crop_frame, cudaStream_t consumer_stream);
    void RecycleNow(CropFrame* crop_frame);
    void QueryCopyTiming(CropFrame* crop_frame, CropProducerPerfSample* perf);
    void DrainPending(bool synchronize_all);
    bool DrainReady();

private:
    struct PendingSourceRelease {
        WORKER_ENTRY* entry = nullptr;
        cudaEvent_t* event = nullptr;
    };

    struct PendingCropFrameRecycle {
        CropFrame* crop_frame = nullptr;
    };

    void release_entry(WORKER_ENTRY* entry);
    cudaEvent_t* acquire_source_release_event();
    void defer_source_release(WORKER_ENTRY* entry, cudaEvent_t* event);
    void drain_pending_source_releases(bool synchronize_all);
    CropFrame* acquire_crop_frame();
    void recycle_crop_frame(CropFrame* crop_frame);
    void release_crop_frame_lease(CropFrame* crop_frame);
    void defer_crop_frame_recycle(CropFrame* crop_frame);
    void drain_pending_crop_frames(bool synchronize_all);
    size_t crop_mono_bytes() const;
    void ensure_source_stage_buffer(int width, int height);

    CameraParams* camera_params_ = nullptr;
    SafeQueue<WORKER_ENTRY*>& recycle_queue_;
    int crop_width_ = 0;
    int crop_height_ = 0;
    cudaStream_t producer_stream_ = nullptr;
    unsigned char* d_source_stage_mono_ = nullptr;
    int source_stage_width_ = 0;
    int source_stage_height_ = 0;
    std::vector<cudaEvent_t> source_release_event_pool_;
    SafeQueue<cudaEvent_t*> free_source_release_events_;
    std::deque<PendingSourceRelease> pending_source_releases_;
    std::mutex pending_source_releases_mutex_;
    std::vector<CropFrame> crop_frame_pool_;
    SafeQueue<CropFrame*> free_crop_frames_;
    std::deque<PendingCropFrameRecycle> pending_crop_frame_recycles_;
    std::mutex pending_crop_frame_recycles_mutex_;
    bool crop_copy_timing_enabled_ = true;
    bool crop_copy_kernel_enabled_ = false;
    bool crop_source_stage_enabled_ = false;
    bool crop_early_owned_frame_enabled_ = false;
    PoseWorker* pose_worker_ = nullptr;
    std::atomic<int> pending_source_release_count_{0};
    std::atomic<int> pending_crop_frame_recycle_count_{0};
    std::atomic<uint64_t> source_release_event_misses_{0};
    std::atomic<uint64_t> crop_frame_pool_misses_{0};
    std::atomic<uint64_t> frames_produced_{0};
    std::atomic<uint64_t> frames_recycled_{0};
    std::atomic<uint64_t> crop_frame_release_count_{0};
    std::atomic<uint64_t> pose_frames_offered_{0};
    std::atomic<uint64_t> pose_frames_accepted_{0};
    std::atomic<uint64_t> pose_frames_dropped_{0};
};

#endif // ORANGE_CROP_PRODUCER_H
