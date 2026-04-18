// src/modern_recording_pipeline.h

#ifndef ORANGE_MODERN_RECORDING_PIPELINE_H
#define ORANGE_MODERN_RECORDING_PIPELINE_H

#include <memory>
#include <string>
#include <vector>

#include "encoder_pipeline.h"
#include "video_capture.h"

class EncoderPreprocessWorker;
class EncoderHwWorker;
class RecordingIngress;
class SharedRecordingOutput;

class ModernRecordingPipeline {
public:
    ModernRecordingPipeline(
        CameraParams* camera_params,
        const ResolvedRecordingConfig& resolved_recording_config,
        SafeQueue<WORKER_ENTRY*>& recycle_queue,
        CameraControl* camera_control,
        const std::string& recording_sink_mode = "real"
    );
    ~ModernRecordingPipeline();

    void start();
    void request_stop();
    void shutdown();

    RecordingIngress* recording_ingress() const { return recording_ingress_.get(); }
    EncoderPreprocessWorker* preprocess_worker() const { return preprocess_worker_.get(); }
    EncoderHwWorker* hw_worker() const { return hw_worker_.get(); }
    const RecordingOutputConfig& recording_output_config() const { return resolved_recording_config_.output; }
    const ResolvedRecordingConfig& resolved_recording_config() const { return resolved_recording_config_; }

    CameraParams* camera_params_ = nullptr;
    int recording_gpu_id_ = -1;
    ResolvedRecordingConfig resolved_recording_config_;
    struct HelperEncodeTarget {
        int gpu_id = -1;
        std::unique_ptr<EncoderPreprocessWorker> preprocess_worker;
        std::unique_ptr<EncoderHwWorker> hw_worker;
    };
    std::shared_ptr<SharedRecordingOutput> shared_recording_output_;
    std::unique_ptr<RecordingIngress> recording_ingress_;
    std::unique_ptr<EncoderPreprocessWorker> preprocess_worker_;
    std::unique_ptr<EncoderHwWorker> hw_worker_;
    std::vector<HelperEncodeTarget> helper_encode_targets_;

private:
    void initialize_split_gop_topology_static_snapshot();
    void refresh_split_gop_runtime_topology_snapshot();
};

#endif
