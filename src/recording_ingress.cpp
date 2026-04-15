#include "recording_ingress.h"

#include <algorithm>
#include <stdexcept>
#include <utility>

#include "encoder_preprocess_worker.h"

namespace {
constexpr uint8_t kRouteModePrimary = 0;
constexpr uint8_t kRouteModeHelper = 1;

void append_unique_gpu_id(std::vector<int>* gpu_ids, int gpu_id)
{
    if (!gpu_ids || gpu_id < 0) {
        return;
    }
    if (std::find(gpu_ids->begin(), gpu_ids->end(), gpu_id) == gpu_ids->end()) {
        gpu_ids->push_back(gpu_id);
    }
}
} // namespace

RecordingIngress::RecordingIngress(EncoderPreprocessWorker* primary_preprocess_worker,
                                   int source_gpu_id,
                                   int primary_encode_gpu_id,
                                   uint32_t recording_gop_length,
                                   RecordingStrategyConfig recording_strategy_config)
    : primary_preprocess_worker_(primary_preprocess_worker),
      source_gpu_id_(source_gpu_id),
      primary_encode_gpu_id_(primary_encode_gpu_id),
      recording_gop_length_(std::max<uint32_t>(1u, recording_gop_length)),
      recording_strategy_config_(std::move(recording_strategy_config))
{
    if (!recording_strategy_config_.split_gop_enabled()) {
        route_gpu_ids_.push_back(primary_encode_gpu_id_);
        return;
    }

    const std::string& policy = recording_strategy_config_.split_gop.source_encoder_policy;
    if (policy == "pure_offload") {
        for (int gpu_id : recording_strategy_config_.split_gop.encoder_gpu_ids) {
            if (gpu_id != primary_encode_gpu_id_) {
                append_unique_gpu_id(&route_gpu_ids_, gpu_id);
            }
        }
        if (route_gpu_ids_.empty() && !recording_strategy_config_.split_gop.strict) {
            route_gpu_ids_.push_back(primary_encode_gpu_id_);
        }
        return;
    }

    route_gpu_ids_.push_back(primary_encode_gpu_id_);
    if (policy == "hybrid_split") {
        for (int gpu_id : recording_strategy_config_.split_gop.encoder_gpu_ids) {
            if (gpu_id != primary_encode_gpu_id_) {
                append_unique_gpu_id(&route_gpu_ids_, gpu_id);
            }
        }
    }
}

void RecordingIngress::RegisterHelperPreprocessWorker(int encode_gpu_id,
                                                      EncoderPreprocessWorker* preprocess_worker)
{
    if (encode_gpu_id < 0 || !preprocess_worker) {
        return;
    }
    if (encode_gpu_id == primary_encode_gpu_id_) {
        primary_preprocess_worker_ = preprocess_worker;
        return;
    }
    helper_preprocess_workers_[encode_gpu_id] = preprocess_worker;
    append_unique_gpu_id(&route_gpu_ids_, encode_gpu_id);
}

int RecordingIngress::select_target_gpu_id(uint64_t recording_frame_id, bool* helper_requested) const
{
    if (helper_requested) {
        *helper_requested = false;
    }
    if (route_gpu_ids_.empty()) {
        if (recording_strategy_config_.split_gop_enabled() &&
            recording_strategy_config_.split_gop.source_encoder_policy == "pure_offload") {
            if (helper_requested) {
                *helper_requested = true;
            }
            return -1;
        }
        return primary_encode_gpu_id_;
    }
    if (route_gpu_ids_.size() == 1) {
        const int only_gpu = route_gpu_ids_.front();
        if (helper_requested) {
            *helper_requested = only_gpu != primary_encode_gpu_id_;
        }
        return only_gpu;
    }

    const uint64_t zero_based_frame = recording_frame_id > 0 ? recording_frame_id - 1 : 0;
    const uint64_t gop_index = zero_based_frame / static_cast<uint64_t>(recording_gop_length_);
    const int target_gpu_id = route_gpu_ids_[static_cast<size_t>(gop_index % route_gpu_ids_.size())];
    if (helper_requested) {
        *helper_requested = target_gpu_id != primary_encode_gpu_id_;
    }
    return target_gpu_id;
}

EncoderPreprocessWorker* RecordingIngress::resolve_target_worker(int target_gpu_id) const
{
    if (target_gpu_id == primary_encode_gpu_id_) {
        return primary_preprocess_worker_;
    }
    const auto it = helper_preprocess_workers_.find(target_gpu_id);
    if (it == helper_preprocess_workers_.end()) {
        return nullptr;
    }
    return it->second;
}

void RecordingIngress::increment_last_route_mode_primary()
{
    last_route_mode_.store(kRouteModePrimary, std::memory_order_relaxed);
}

void RecordingIngress::increment_last_route_mode_helper()
{
    last_route_mode_.store(kRouteModeHelper, std::memory_order_relaxed);
}

void RecordingIngress::SubmitFrame(WORKER_ENTRY* entry)
{
    if (!primary_preprocess_worker_) {
        throw std::runtime_error("RecordingIngress has no primary preprocess worker");
    }

    submitted_frames_.fetch_add(1, std::memory_order_relaxed);

    bool helper_requested = false;
    int target_gpu_id = select_target_gpu_id(entry ? entry->recording_frame_id : 0, &helper_requested);
    EncoderPreprocessWorker* target_worker = resolve_target_worker(target_gpu_id);
    if (helper_requested) {
        helper_requested_frames_.fetch_add(1, std::memory_order_relaxed);
    }

    if (!target_worker) {
        if (helper_requested && recording_strategy_config_.split_gop.strict) {
            throw std::runtime_error(
                "Split-GOP helper GPU " + std::to_string(target_gpu_id) +
                " was selected by RecordingIngress but no helper preprocess worker is registered");
        }
        target_gpu_id = primary_encode_gpu_id_;
        target_worker = primary_preprocess_worker_;
        if (helper_requested) {
            helper_fallback_frames_.fetch_add(1, std::memory_order_relaxed);
        }
    }

    if (target_gpu_id == primary_encode_gpu_id_) {
        primary_routed_frames_.fetch_add(1, std::memory_order_relaxed);
        increment_last_route_mode_primary();
    } else {
        helper_dispatched_frames_.fetch_add(1, std::memory_order_relaxed);
        increment_last_route_mode_helper();
    }

    last_target_gpu_id_.store(target_gpu_id, std::memory_order_relaxed);
    target_worker->PutObjectToQueueIn(entry);
}

RecordingIngressStats RecordingIngress::GetStats() const
{
    RecordingIngressStats stats;
    if (primary_preprocess_worker_) {
        stats.preprocess_fps = primary_preprocess_worker_->get_fps();
        stats.encode_fps = primary_preprocess_worker_->get_hw_fps();
        stats.preprocess_queue_depth = primary_preprocess_worker_->GetCountQueueInSize();
        stats.encode_queue_depth = primary_preprocess_worker_->get_hw_queue_depth();
        stats.preprocess_buffers_available =
            primary_preprocess_worker_->available_buffers_.load(std::memory_order_relaxed);
        stats.preprocess_events_available =
            primary_preprocess_worker_->available_events_.load(std::memory_order_relaxed);
        stats.preprocess_resource_waits = primary_preprocess_worker_->get_resource_waits();
        stats.preprocess_frames_dropped = primary_preprocess_worker_->get_frames_dropped();
        stats.encode_failures = primary_preprocess_worker_->get_hw_encode_failures();
        stats.encode_slow_frames = primary_preprocess_worker_->get_hw_slow_frames();
    }

    stats.submitted_frames = submitted_frames_.load(std::memory_order_relaxed);
    stats.primary_routed_frames = primary_routed_frames_.load(std::memory_order_relaxed);
    stats.helper_requested_frames = helper_requested_frames_.load(std::memory_order_relaxed);
    stats.helper_fallback_frames = helper_fallback_frames_.load(std::memory_order_relaxed);
    stats.helper_dispatched_frames = helper_dispatched_frames_.load(std::memory_order_relaxed);
    stats.last_target_gpu_id = last_target_gpu_id_.load(std::memory_order_relaxed);
    stats.last_route_mode =
        last_route_mode_.load(std::memory_order_relaxed) == kRouteModeHelper ? "helper" : "primary";
    return stats;
}

bool RecordingIngress::IsDrained() const
{
    if (primary_preprocess_worker_ && !primary_preprocess_worker_->IsDrained()) {
        return false;
    }
    for (const auto& [gpu_id, worker] : helper_preprocess_workers_) {
        (void)gpu_id;
        if (worker && !worker->IsDrained()) {
            return false;
        }
    }
    return true;
}
