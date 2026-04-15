#ifndef ORANGE_RECORDING_INGRESS_H
#define ORANGE_RECORDING_INGRESS_H

#include <cstdint>

#include "video_capture.h"

class EncoderPreprocessWorker;

struct RecordingIngressStats {
    double preprocess_fps = 0.0;
    double encode_fps = 0.0;
    int preprocess_queue_depth = -1;
    int encode_queue_depth = -1;
    int preprocess_buffers_available = -1;
    int preprocess_events_available = -1;
    uint64_t preprocess_resource_waits = 0;
    uint64_t preprocess_frames_dropped = 0;
    uint64_t encode_failures = 0;
    uint64_t encode_slow_frames = 0;
};

class RecordingIngress {
public:
    explicit RecordingIngress(EncoderPreprocessWorker* primary_preprocess_worker);

    void SubmitFrame(WORKER_ENTRY* entry);
    RecordingIngressStats GetStats() const;
    bool IsDrained() const;

    EncoderPreprocessWorker* primary_preprocess_worker() const { return primary_preprocess_worker_; }

private:
    EncoderPreprocessWorker* primary_preprocess_worker_ = nullptr;
};

#endif // ORANGE_RECORDING_INGRESS_H
