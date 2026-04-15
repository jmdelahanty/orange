#ifndef ORANGE_RECORDING_INGRESS_H
#define ORANGE_RECORDING_INGRESS_H

#include <atomic>
#include <cstdint>
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
    uint64_t encode_failures = 0;
    uint64_t encode_slow_frames = 0;
    uint64_t submitted_frames = 0;
    uint64_t primary_routed_frames = 0;
    uint64_t helper_requested_frames = 0;
    uint64_t helper_fallback_frames = 0;
    uint64_t helper_dispatched_frames = 0;
    int last_target_gpu_id = -1;
    std::string last_route_mode = "primary";
};

class RecordingIngress {
public:
    RecordingIngress(EncoderPreprocessWorker* primary_preprocess_worker,
                     int source_gpu_id,
                     int primary_encode_gpu_id,
                     uint32_t recording_gop_length,
                     RecordingStrategyConfig recording_strategy_config);

    void SubmitFrame(WORKER_ENTRY* entry);
    RecordingIngressStats GetStats() const;
    bool IsDrained() const;
    void RegisterHelperPreprocessWorker(int encode_gpu_id, EncoderPreprocessWorker* preprocess_worker);

    EncoderPreprocessWorker* primary_preprocess_worker() const { return primary_preprocess_worker_; }

private:
    int select_target_gpu_id(uint64_t recording_frame_id, bool* helper_requested) const;
    EncoderPreprocessWorker* resolve_target_worker(int target_gpu_id) const;
    void increment_last_route_mode_primary();
    void increment_last_route_mode_helper();

    EncoderPreprocessWorker* primary_preprocess_worker_ = nullptr;
    int source_gpu_id_ = -1;
    int primary_encode_gpu_id_ = -1;
    uint32_t recording_gop_length_ = 1;
    RecordingStrategyConfig recording_strategy_config_;
    std::vector<int> route_gpu_ids_;
    std::unordered_map<int, EncoderPreprocessWorker*> helper_preprocess_workers_;
    std::atomic<uint64_t> submitted_frames_{0};
    std::atomic<uint64_t> primary_routed_frames_{0};
    std::atomic<uint64_t> helper_requested_frames_{0};
    std::atomic<uint64_t> helper_fallback_frames_{0};
    std::atomic<uint64_t> helper_dispatched_frames_{0};
    std::atomic<int> last_target_gpu_id_{-1};
    std::atomic<uint8_t> last_route_mode_{0};
};

#endif // ORANGE_RECORDING_INGRESS_H
