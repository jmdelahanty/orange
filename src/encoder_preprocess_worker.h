// src/encoder_preprocess_worker.h

#ifndef ENCODER_PREPROCESS_WORKER_H
#define ENCODER_PREPROCESS_WORKER_H

#include "threadworker.h"
#include "video_capture.h"
#include "image_processing.h"
#include "encoder_pipeline.h" 
#include <cuda_runtime.h>
#include <atomic>
#include <chrono>
#include <deque>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <vector>

class EncoderHwWorker; // Forward declaration is sufficient here

struct PeerAccessRouteState {
    int source_gpu_id = -1;
    int target_gpu_id = -1;
    bool can_access_peer = false;
    bool peer_access_enable_attempted = false;
    bool peer_access_enabled = false;
    std::string enable_error;
};

struct HelperPreprocessHostSample {
    uint64_t recording_frame_id = 0;
    uint64_t local_frame_id = 0;
    int source_gpu_id = -1;
    int target_gpu_id = -1;
    bool gpu_direct_mode = false;
    bool direct_input_enabled = false;
    uint64_t enqueue_host_ns = 0;
    uint64_t start_host_ns = 0;
    uint64_t done_host_ns = 0;
    int queue_depth_on_enqueue = -1;
    int queue_depth_on_start = -1;
    int available_buffers_on_enqueue = -1;
    int available_events_on_enqueue = -1;
    int available_buffers_on_start = -1;
    int available_events_on_start = -1;
    size_t helper_copy_bytes = 0;
    int64_t helper_copy_delay_ns = 0;
    uint64_t resource_waits_on_start = 0;
    uint64_t frames_dropped_on_start = 0;
};

struct SourceReleaseSample {
    uint64_t recording_frame_id = 0;
    uint64_t local_frame_id = 0;
    int source_gpu_id = -1;
    int target_gpu_id = -1;
    bool gpu_direct_mode = false;
    bool cross_gpu_copy = false;
    bool direct_input_enabled = false;
    uint64_t receive_host_ns = 0;
    uint64_t submit_host_ns = 0;
    uint64_t worker_start_host_ns = 0;
    uint64_t source_safe_event_record_host_ns = 0;
    uint64_t source_release_host_ns = 0;
    int pending_releases_on_record = 0;
};

class EncoderPreprocessWorker : public CThreadWorker<WORKER_ENTRY>
{
public:
    EncoderPreprocessWorker(
        const char* name,
        CameraParams* cam_params,
        int preprocess_gpu_id,
        const RecordingOutputConfig& recording_output_config,
        bool direct_input_enabled,
        int encoder_pitch,
        int direct_input_slot_count,
        int configured_entry_pool_size,
        SafeQueue<WORKER_ENTRY*>& recycle_queue,
        CameraControl* camera_control
    );
    ~EncoderPreprocessWorker() override;

    void SetHwWorker(EncoderHwWorker* hw_worker);

    // This queue is public so the HW worker can return buffers
    SafeQueue<ENCODER_WORKER_ENTRY*> free_encoder_entries_;
    SafeQueue<cudaEvent_t*> free_events_;  // Assuming this is also public
    SafeQueue<int> free_direct_input_slots_;
    
    // Public atomic counters for resource tracking
    std::atomic<int> available_buffers_{0};
    std::atomic<int> available_events_{0};
    
    // Performance monitoring getters
    double get_fps() const { return current_fps_.load(); }
    uint64_t get_frames_dropped() const { return frames_dropped_.load(); }
    uint64_t get_resource_waits() const { return resource_waits_.load(); }
    uint64_t get_detect_priority_gated_frames() const { return detect_priority_gated_frames_.load(); }
    uint64_t get_detect_priority_waited_frames() const { return detect_priority_waited_frames_.load(); }
    uint64_t get_detect_priority_wait_timeouts() const { return detect_priority_wait_timeouts_.load(); }
    uint64_t get_detect_priority_wait_total_ns() const { return detect_priority_wait_total_ns_.load(); }
    uint64_t get_detect_priority_wait_max_ns() const { return detect_priority_wait_max_ns_.load(); }
    double get_hw_fps() const;
    uint64_t get_hw_encode_failures() const;
    uint64_t get_hw_slow_frames() const;
    int get_hw_queue_depth() const;
    bool IsDrained();
    void PrepareCrossGpuInput(int source_gpu_id);
    int preprocess_gpu_id() const { return preprocess_gpu_id_; }
    bool direct_input_enabled() const { return direct_input_enabled_; }
    int direct_input_pitch() const { return direct_input_pitch_; }
    int direct_input_slot_count() const { return direct_input_slot_count_; }
    const std::vector<void*>& direct_input_surfaces() const { return direct_input_surfaces_; }
    std::vector<PeerAccessRouteState> peer_access_states() const;

protected:
    bool WorkerFunction(WORKER_ENTRY* entry) override;

private:
    struct PendingSourceRelease {
        WORKER_ENTRY* entry = nullptr;
        cudaEvent_t* event = nullptr;
        SourceReleaseSample sample;
    };

    void append_helper_preprocess_host_sample(const HelperPreprocessHostSample& sample);
    void dump_helper_preprocess_host_history() const;
    void append_source_release_sample(const SourceReleaseSample& sample);
    void dump_source_release_history() const;
    cudaEvent_t* acquire_source_release_event();
    void defer_source_release(WORKER_ENTRY* entry,
                              cudaEvent_t* event,
                              const SourceReleaseSample& sample);
    void drain_pending_source_releases(bool synchronize_all);
    void release_source_entry(WORKER_ENTRY* entry);
    bool ensure_peer_access_enabled(int source_gpu_id);
    void record_detect_priority_wait(uint64_t wait_ns, bool timeout);
    CameraParams* camera_params_;
    int preprocess_gpu_id_;
    SafeQueue<WORKER_ENTRY*>& m_recycle_queue_;
    CameraControl* camera_control_;
    cudaStream_t m_stream;
    EncoderHwWorker* m_hw_worker_;

    FrameGPU frame_original_gpu_;
    Debayer debayer_gpu_;
    unsigned char* d_input_staging_;
    unsigned char* d_rgba_resize_;
    unsigned char* d_uv_default_plane_;
    bool direct_input_enabled_;
    RecordingOutputConfig recording_output_config_;
    int encoder_pitch_;
    int direct_input_pitch_;
    int direct_input_slot_count_;
    int output_width_;
    int output_height_;
    NppiSize resize_source_size_;
    NppiRect resize_source_roi_;
    NppiSize resize_output_size_;
    NppiRect resize_output_roi_;

    static const int DEFAULT_ENCODER_ENTRY_POOL_SIZE = 120;
    std::vector<ENCODER_WORKER_ENTRY> encoder_entry_pool_;
    std::vector<cudaEvent_t> event_pool_;
    std::vector<cudaEvent_t> copy_start_event_pool_;
    std::vector<cudaEvent_t> copy_end_event_pool_;
    std::vector<cudaEvent_t> source_release_event_pool_;
    SafeQueue<cudaEvent_t*> free_source_release_events_;
    std::vector<void*> direct_input_surfaces_;
    std::set<int> peer_access_enabled_gpus_;
    mutable std::mutex peer_access_states_mutex_;
    std::map<int, PeerAccessRouteState> peer_access_states_;
    mutable std::mutex helper_preprocess_host_history_mutex_;
    std::deque<HelperPreprocessHostSample> helper_preprocess_host_history_;
    std::atomic<uint64_t> helper_preprocess_host_seen_{0};
    mutable std::mutex source_release_history_mutex_;
    std::deque<SourceReleaseSample> source_release_history_;
    std::deque<PendingSourceRelease> pending_source_releases_;
    bool defer_source_release_enabled_ = true;
    bool helper_noop_source_read_enabled_ = false;
    bool detect_priority_wait_enabled_ = false;
    bool yolo_detach_input_enabled_ = false;
    int64_t helper_copy_limit_bytes_ = -1;
    int64_t helper_copy_delay_ns_ = 0;
    std::atomic<int> pending_source_release_count_{0};
    std::atomic<uint64_t> source_release_event_misses_{0};
    std::atomic<uint64_t> detect_priority_gated_frames_{0};
    std::atomic<uint64_t> detect_priority_waited_frames_{0};
    std::atomic<uint64_t> detect_priority_wait_timeouts_{0};
    std::atomic<uint64_t> detect_priority_wait_total_ns_{0};
    std::atomic<uint64_t> detect_priority_wait_max_ns_{0};
    std::atomic<bool> detect_priority_timeout_logged_{false};
    
    // Performance monitoring members
    std::chrono::steady_clock::time_point last_fps_update_time_;
    std::atomic<int> frame_counter_{0};
    std::atomic<double> current_fps_{0.0};
    std::atomic<uint64_t> frames_dropped_{0};
    std::atomic<uint64_t> resource_waits_{0};
    std::atomic<int> in_flight_{0};
};

#endif // ENCODER_PREPROCESS_WORKER_H
