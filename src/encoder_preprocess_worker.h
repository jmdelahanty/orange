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

struct HelperPreprocessSample {
    uint64_t recording_frame_id = 0;
    uint64_t local_frame_id = 0;
    int source_gpu_id = -1;
    int target_gpu_id = -1;
    bool gpu_direct_mode = false;
    bool direct_input_enabled = false;
    int queue_depth = -1;
    int available_buffers = -1;
    int available_events = -1;
    uint64_t resource_waits = 0;
    uint64_t frames_dropped = 0;
    float copy_ms = 0.0f;
    float total_preprocess_ms = 0.0f;
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
    double get_hw_fps() const;
    uint64_t get_hw_encode_failures() const;
    uint64_t get_hw_slow_frames() const;
    int get_hw_queue_depth() const;
    bool IsDrained();
    int preprocess_gpu_id() const { return preprocess_gpu_id_; }
    bool direct_input_enabled() const { return direct_input_enabled_; }
    int direct_input_pitch() const { return direct_input_pitch_; }
    int direct_input_slot_count() const { return direct_input_slot_count_; }
    const std::vector<void*>& direct_input_surfaces() const { return direct_input_surfaces_; }
    std::vector<PeerAccessRouteState> peer_access_states() const;

protected:
    bool WorkerFunction(WORKER_ENTRY* entry) override;

private:
    void append_helper_preprocess_sample(const HelperPreprocessSample& sample);
    void dump_helper_preprocess_history(const HelperPreprocessSample& trigger_sample) const;
    bool ensure_peer_access_enabled(int source_gpu_id);
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
    std::vector<void*> direct_input_surfaces_;
    std::set<int> peer_access_enabled_gpus_;
    mutable std::mutex peer_access_states_mutex_;
    std::map<int, PeerAccessRouteState> peer_access_states_;
    mutable std::mutex helper_preprocess_history_mutex_;
    std::deque<HelperPreprocessSample> helper_preprocess_history_;
    std::atomic<bool> helper_preprocess_history_dumped_{false};
    
    // Performance monitoring members
    std::chrono::steady_clock::time_point last_fps_update_time_;
    std::atomic<int> frame_counter_{0};
    std::atomic<double> current_fps_{0.0};
    std::atomic<uint64_t> frames_dropped_{0};
    std::atomic<uint64_t> resource_waits_{0};
    std::atomic<int> in_flight_{0};
};

#endif // ENCODER_PREPROCESS_WORKER_H
