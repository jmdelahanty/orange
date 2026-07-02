#ifndef ORANGE_RECORDING_INGRESS_H
#define ORANGE_RECORDING_INGRESS_H

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "encoder_pipeline.h"
#include "video_capture.h"

class EncoderPreprocessWorker;

struct RecordingIngressStats {
    double preprocess_fps = 0.0;
    double preprocess_fps_primary = 0.0;
    double preprocess_fps_helpers = 0.0;
    double encode_fps = 0.0;
    double encode_fps_primary = 0.0;
    double encode_fps_helpers = 0.0;
    int preprocess_queue_depth = -1;
    int encode_queue_depth = -1;
    int preprocess_buffers_available = -1;
    int preprocess_events_available = -1;
    uint64_t preprocess_resource_waits = 0;
    uint64_t preprocess_frames_dropped = 0;
    uint64_t detect_priority_gated_frames = 0;
    uint64_t detect_priority_waited_frames = 0;
    uint64_t detect_priority_wait_timeouts = 0;
    uint64_t detect_priority_wait_total_ns = 0;
    uint64_t detect_priority_wait_max_ns = 0;
    uint64_t encode_failures = 0;
    uint64_t encode_slow_frames = 0;
    uint64_t external_ipc_frames_acked = 0;
    uint64_t external_ipc_failures = 0;
    uint64_t external_ipc_ack_timeouts = 0;
    uint64_t submitted_frames = 0;
    uint64_t primary_routed_frames = 0;
    uint64_t helper_requested_frames = 0;
    uint64_t helper_fallback_frames = 0;
    uint64_t helper_dispatched_frames = 0;
    uint64_t enqueue_rejected_frames = 0;
    int last_target_gpu_id = -1;
    std::string last_route_mode = "primary";
};

std::string normalize_recording_sink_mode(const std::string& value);
bool is_real_recording_sink_mode(const std::string& value);

class RecordingIngress {
public:
    RecordingIngress(EncoderPreprocessWorker* primary_preprocess_worker,
                     int source_gpu_id,
                     int primary_encode_gpu_id,
                     uint32_t recording_gop_length,
                     const ResolvedRecordingConfig& resolved_recording_config,
                     SafeQueue<WORKER_ENTRY*>* recycle_queue = nullptr,
                     const std::string& recording_sink_mode = "real",
                     const std::string& camera_serial = "");
    ~RecordingIngress();

    bool SubmitFrame(WORKER_ENTRY* entry);
    RecordingIngressStats GetStats() const;
    bool IsDrained() const;
    bool uses_external_ipc() const;
    void RegisterHelperPreprocessWorker(int encode_gpu_id, EncoderPreprocessWorker* preprocess_worker);
    void start();
    void request_recording_drain();
    void request_stop();
    void shutdown();
    void reset_external_ipc_connection();
    bool requires_owned_cuda_source() const;

    EncoderPreprocessWorker* primary_preprocess_worker() const { return primary_preprocess_worker_; }
    bool fail_on_drop() const { return resolved_recording_config_.fail_on_drop; }

private:
    class ThreadedHandoffWorker;
    class ExternalIpcHandoffWorker;

    void release_entry(WORKER_ENTRY* entry);
    int select_target_gpu_id(uint64_t recording_frame_id, bool* helper_requested) const;
    EncoderPreprocessWorker* resolve_target_worker(int target_gpu_id) const;
    void increment_last_route_mode_primary();
    void increment_last_route_mode_helper();

    EncoderPreprocessWorker* primary_preprocess_worker_ = nullptr;
    int source_gpu_id_ = -1;
    int primary_encode_gpu_id_ = -1;
    uint32_t recording_gop_length_ = 1;
    ResolvedRecordingConfig resolved_recording_config_;
    SafeQueue<WORKER_ENTRY*>* recycle_queue_ = nullptr;
    std::string recording_sink_mode_ = "real";
    std::string camera_serial_;
    std::unique_ptr<ThreadedHandoffWorker> threaded_handoff_worker_;
    std::unique_ptr<ExternalIpcHandoffWorker> external_ipc_handoff_worker_;
    std::vector<int> route_gpu_ids_;
    std::unordered_map<int, EncoderPreprocessWorker*> helper_preprocess_workers_;
    std::atomic<uint64_t> submitted_frames_{0};
    std::atomic<uint64_t> primary_routed_frames_{0};
    std::atomic<uint64_t> helper_requested_frames_{0};
    std::atomic<uint64_t> helper_fallback_frames_{0};
    std::atomic<uint64_t> helper_dispatched_frames_{0};
    std::atomic<uint64_t> enqueue_rejected_frames_{0};
    std::atomic<int> last_target_gpu_id_{-1};
    std::atomic<uint8_t> last_route_mode_{0};
};

#endif // ORANGE_RECORDING_INGRESS_H
