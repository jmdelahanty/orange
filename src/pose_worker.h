#ifndef ORANGE_POSE_WORKER_H
#define ORANGE_POSE_WORKER_H

#include "crop_producer.h"
#include "threadworker.h"

#include <atomic>
#include <mutex>
#include <string>
#include <vector>

class PoseWorker : public CThreadWorker<CropFrame>
{
public:
    PoseWorker(const char* name, CameraParams* camera_params, CropProducer* crop_producer);
    ~PoseWorker() override;

    void SetMaxQueueSize(int size);
    bool TryEnqueueCrop(CropFrame* crop_frame);
    void RotateRecordingFolder(const std::string& recording_folder);
    void CloseRecording();

private:
    bool WorkerFunction(CropFrame* crop_frame) override;
    void reset_run_counters();
    void write_recording_summary_locked();

    CameraParams* camera_params_ = nullptr;
    CropProducer* crop_producer_ = nullptr;
    cudaStream_t stream_ = nullptr;
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
    std::vector<double> detect_to_crop_ready_samples_ms_;
    std::vector<double> crop_ready_to_pose_start_samples_ms_;
    std::vector<double> pose_start_to_pose_done_samples_ms_;
    std::vector<double> capture_to_pose_done_samples_ms_;
};

#endif  // ORANGE_POSE_WORKER_H
