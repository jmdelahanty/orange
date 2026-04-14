// src/modern_recording_pipeline.h

#ifndef ORANGE_MODERN_RECORDING_PIPELINE_H
#define ORANGE_MODERN_RECORDING_PIPELINE_H

#include <memory>
#include <string>

#include "encoder_pipeline.h"
#include "video_capture.h"

class EncoderPreprocessWorker;
class EncoderHwWorker;

class ModernRecordingPipeline {
public:
    ModernRecordingPipeline(
        CameraParams* camera_params,
        int recording_gpu_id,
        const RecordingOutputConfig& recording_output_config,
        const std::string& codec,
        const std::string& preset,
        const std::string& tuning,
        const std::string& rate_control_mode,
        int quality_value,
        int gop_length,
        const EncoderControlOverrides& encoder_control_overrides,
        const ImportanceMapConfig& importance_map_config,
        const std::string& base_folder_name,
        SafeQueue<WORKER_ENTRY*>& recycle_queue,
        CameraControl* camera_control,
        const PreEncoderReferenceCaptureConfig& pre_encoder_reference_capture_config = {}
    );
    ~ModernRecordingPipeline();

    void start();
    void request_stop();
    void shutdown();

    EncoderPreprocessWorker* preprocess_worker() const { return preprocess_worker_.get(); }
    EncoderHwWorker* hw_worker() const { return hw_worker_.get(); }
    const RecordingOutputConfig& recording_output_config() const { return recording_output_config_; }

    CameraParams* camera_params_ = nullptr;
    int recording_gpu_id_ = -1;
    RecordingOutputConfig recording_output_config_;
    std::unique_ptr<EncoderPreprocessWorker> preprocess_worker_;
    std::unique_ptr<EncoderHwWorker> hw_worker_;
};

#endif
