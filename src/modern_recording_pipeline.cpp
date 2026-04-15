// src/modern_recording_pipeline.cpp

#include "modern_recording_pipeline.h"

#include <iostream>

#include "encoder_hw_worker.h"
#include "encoder_preprocess_worker.h"
#include "recording_ingress.h"

ModernRecordingPipeline::ModernRecordingPipeline(
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
    const PreEncoderReferenceCaptureConfig& pre_encoder_reference_capture_config
)
    : camera_params_(camera_params),
      recording_gpu_id_(recording_gpu_id >= 0 ? recording_gpu_id : camera_params->gpu_id),
      recording_output_config_(recording_output_config)
{
    const std::string hw_encoder_name = "HW_Encoder_Cam_" + camera_params_->camera_serial;
    hw_worker_ = std::make_unique<EncoderHwWorker>(
        hw_encoder_name.c_str(),
        camera_params_,
        recording_gpu_id_,
        recording_output_config_,
        codec,
        preset,
        tuning,
        rate_control_mode,
        quality_value,
        gop_length,
        encoder_control_overrides,
        importance_map_config,
        base_folder_name,
        nullptr,
        camera_control,
        pre_encoder_reference_capture_config);

    const std::string preprocess_name = "Preprocess_Cam_" + camera_params_->camera_serial;
    preprocess_worker_ = std::make_unique<EncoderPreprocessWorker>(
        preprocess_name.c_str(),
        camera_params_,
        recording_gpu_id_,
        recording_output_config_,
        hw_worker_->direct_input_enabled(),
        hw_worker_->encoder_input_pitch(),
        hw_worker_->encoder_buffer_count(),
        recycle_queue,
        camera_control);

    preprocess_worker_->SetHwWorker(hw_worker_.get());
    hw_worker_->SetPreprocessWorker(preprocess_worker_.get());
    recording_ingress_ = std::make_unique<RecordingIngress>(
        preprocess_worker_.get(),
        camera_params_->gpu_id,
        hw_worker_->encode_gpu_id(),
        hw_worker_->recording_gop_length(),
        hw_worker_->recording_strategy_config());
}

ModernRecordingPipeline::~ModernRecordingPipeline()
{
    shutdown();
}

void ModernRecordingPipeline::start()
{
    if (preprocess_worker_) {
        preprocess_worker_->SetMaxQueueSize(240);
        preprocess_worker_->StartThread();
    }
    if (hw_worker_) {
        hw_worker_->SetMaxQueueSize(240);
        hw_worker_->StartThread();
    }
}

void ModernRecordingPipeline::request_stop()
{
    if (hw_worker_) {
        hw_worker_->StopThread();
    }
    if (preprocess_worker_) {
        preprocess_worker_->StopThread();
    }
}

void ModernRecordingPipeline::shutdown()
{
    request_stop();

    if (hw_worker_) {
        std::cout << "Flushing final packets for main HW encoder "
                  << camera_params_->camera_serial << "..." << std::endl;
        hw_worker_->flush_and_close();
        hw_worker_.reset();
    }

    if (preprocess_worker_) {
        preprocess_worker_.reset();
    }
}
