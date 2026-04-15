#include "recording_ingress.h"

#include "encoder_preprocess_worker.h"

RecordingIngress::RecordingIngress(EncoderPreprocessWorker* primary_preprocess_worker)
    : primary_preprocess_worker_(primary_preprocess_worker)
{
}

void RecordingIngress::SubmitFrame(WORKER_ENTRY* entry)
{
    if (!primary_preprocess_worker_) {
        return;
    }
    primary_preprocess_worker_->PutObjectToQueueIn(entry);
}

RecordingIngressStats RecordingIngress::GetStats() const
{
    RecordingIngressStats stats;
    if (!primary_preprocess_worker_) {
        return stats;
    }

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
    return stats;
}

bool RecordingIngress::IsDrained() const
{
    return !primary_preprocess_worker_ || primary_preprocess_worker_->IsDrained();
}
