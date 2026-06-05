#ifndef ORANGE_POSE_WORKER_H
#define ORANGE_POSE_WORKER_H

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
    void RotateRecordingFolder(const std::string& recording_folder);
    void CloseRecording();

private:
    bool WorkerFunction(CropFrame* crop_frame) override;
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
    std::vector<double> capture_to_detect_done_samples_ms_;
    std::vector<double> detect_to_crop_worker_start_samples_ms_;
    std::vector<double> crop_worker_start_to_crop_ready_samples_ms_;
    std::vector<double> detect_to_crop_ready_samples_ms_;
    std::vector<double> crop_ready_to_pose_start_samples_ms_;
    std::vector<double> pose_start_to_pose_done_samples_ms_;
    std::vector<double> capture_to_pose_done_samples_ms_;
};

#endif  // ORANGE_POSE_WORKER_H
