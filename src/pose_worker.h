#ifndef ORANGE_POSE_WORKER_H
#define ORANGE_POSE_WORKER_H

#include "bounded_sample_statistics.h"
#include "crop_producer.h"
#include "pose_event_log.h"
#include "threadworker.h"

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

class TensorRtPoseBackend;
class FrameIPCManager;
struct DetectRoi;

class PoseWorker : public CThreadWorker<CropFrame>
{
public:
    PoseWorker(const char* name,
               CameraParams* camera_params,
               CropProducer* crop_producer,
               FrameIPCManager* frame_ipc_manager = nullptr);
    ~PoseWorker() override;

    void SetMaxQueueSize(int size);
    bool TryEnqueueCrop(CropFrameLease crop_frame_lease);

    // Device crop path (ORANGE_ANALYTICS_DEVICE_CROP, step 1 of
    // docs/handoff_pose_analytics_2026_09_16.md): the YOLO worker thread cuts
    // the pose crop from the device ROI and queues preprocess and the pose
    // enqueue right behind the detect graph, so no CPU hop sits between
    // detect and pose. This thread then only waits for the result and
    // decodes it. Slots are a small ring owned here; pose runs on every
    // frame (run-and-mask) and an invalid ROI yields "no_result".
    bool EnableDeviceStage(int pose_crop_px, std::string* error_out);
    bool device_stage_enabled() const { return device_stage_enabled_; }
    int device_stage_crop_px() const { return device_stage_crop_px_; }
    // The pose stream: work queued on it after EnqueueDeviceStage follows
    // this frame's pose graph in stream order.
    cudaStream_t device_stage_stream() const { return stream_; }
    // Called on the YOLO worker thread, on the YOLO stream, after the ROI
    // kernel and before the frame's completion event (so the source read is
    // covered by that event). d_source is the mono frame the ROI refers to.
    // Returns 1 when queued, 0 when every slot is still busy (the frame gets
    // no pose), -2 when the enqueue failed.
    // done_event_out (optional) receives the slot's pose-done event, recorded
    // on the pose stream after the output copy, so the caller can order
    // later GPU work (the late owned copy, step 4) behind this frame's pose.
    int EnqueueDeviceStage(
        WORKER_ENTRY* entry,
        const unsigned char* d_source,
        int source_pitch,
        cudaStream_t yolo_stream,
        double* cpu_ms_out,
        cudaEvent_t* done_event_out = nullptr);
    void RotateRecordingFolder(const std::string& recording_folder);
    void CloseRecording();

private:
    struct DeviceStageSlot;
    bool WorkerFunction(CropFrame* crop_frame) override;
    DeviceStageSlot* find_device_slot(CropFrame* crop_frame);
    void process_device_slot(DeviceStageSlot& slot);
    void free_device_slots();
    // Flush tick (drain cascade marker from CropProducerWorker, or shutdown):
    // all queued crops have been processed; close the event log + summary.
    void OnFlushTick() override { CloseRecording(); }
    void reset_run_counters();
    void write_recording_summary_locked();
    pose_event_log::PoseResultRecord build_pose_event_record(
        const CropFrameSnapshot& frame,
        uint64_t pose_start_host_ns,
        uint64_t pose_done_host_ns,
        const std::string& status,
        const std::string& error,
        const std::vector<pose_event_log::PoseInstanceRecord>& poses) const;
    void publish_pose_result_v2(
        const CropFrameSnapshot& frame,
        const std::string& status,
        const std::vector<pose_event_log::PoseInstanceRecord>& poses);

    CameraParams* camera_params_ = nullptr;
    CropProducer* crop_producer_ = nullptr;
    FrameIPCManager* frame_ipc_manager_ = nullptr;
    cudaStream_t stream_ = nullptr;
    std::unique_ptr<TensorRtPoseBackend> tensorrt_backend_;
    pose_event_log::PoseEventLogger pose_event_logger_;
    std::string pose_backend_ = "noop";
    std::string pose_mode_ = "noop";
    std::string pose_model_id_ = "none";
    std::string pose_engine_path_;
    std::string pose_skeleton_id_ = "unknown";
    std::string pose_skeleton_path_;
    int pose_prewarm_iterations_ = 0;
    int max_queue_size_ = 32;
    std::mutex recording_mutex_;
    std::string current_recording_folder_;
    std::string pose_perf_file_;
    std::atomic<uint64_t> frames_enqueued_{0};
    std::atomic<uint64_t> frames_processed_{0};
    std::atomic<uint64_t> queue_full_drops_{0};
    std::atomic<int> queue_high_water_{0};
    std::atomic<uint64_t> run_frames_enqueued_{0};
    std::atomic<uint64_t> run_frames_processed_{0};
    std::atomic<uint64_t> run_queue_full_drops_{0};
    std::atomic<int> run_queue_high_water_{0};
    orange::BoundedSampleStatistics capture_to_detect_done_samples_ms_;
    orange::BoundedSampleStatistics detect_to_crop_worker_start_samples_ms_;
    orange::BoundedSampleStatistics crop_worker_start_to_crop_ready_samples_ms_;
    orange::BoundedSampleStatistics detect_to_crop_ready_samples_ms_;
    orange::BoundedSampleStatistics crop_ready_to_pose_start_samples_ms_;
    orange::BoundedSampleStatistics pose_start_to_pose_done_samples_ms_;
    orange::BoundedSampleStatistics capture_to_pose_done_samples_ms_;
    // Device crop path.
    bool device_stage_enabled_ = false;
    int device_stage_crop_px_ = 0;
    int device_slot_count_ = 0;
    int device_stage_graphs_ = 0;   // slots whose pose stage is a captured CUDA graph
    uint64_t device_slot_next_ = 0;
    std::vector<std::unique_ptr<DeviceStageSlot>> device_slots_;
    std::atomic<uint64_t> device_stage_enqueued_{0};
    std::atomic<uint64_t> device_stage_slot_busy_{0};
    std::atomic<uint64_t> device_stage_failed_{0};
    // GPU time from the pose input being ready (after crop + preprocess on
    // the YOLO stream) to the pose output copied back, per frame.
    orange::BoundedSampleStatistics device_stage_gpu_samples_ms_;
};

#endif  // ORANGE_POSE_WORKER_H
