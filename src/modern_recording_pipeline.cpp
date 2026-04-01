// src/modern_recording_pipeline.cpp

#include "modern_recording_pipeline.h"

#include <iostream>
#include <stdexcept>

#include <cuda.h>

#include "NvEncoder/NvCodecUtils.h"
#include "encoder_hw_worker.h"
#include "encoder_preprocess_worker.h"

namespace {
int determine_encoder_pitch_for_output(
    CameraParams* camera_params,
    const RecordingOutputConfig& recording_output_config
) {
    if (!camera_params) {
        throw std::runtime_error("camera params missing for modern recording pipeline");
    }

    int previous_device = -1;
    cudaGetDevice(&previous_device);
    ck(cudaSetDevice(camera_params->gpu_id));

    CUcontext cuContext = nullptr;
    ck(cuCtxGetCurrent(&cuContext));
    if (cuContext == nullptr) {
        throw std::runtime_error("No CUDA context available for modern recording pipeline");
    }

    NvEncoderCuda temp_enc(
        cuContext,
        recording_output_config.resolved_width,
        recording_output_config.resolved_height,
        NV_ENC_BUFFER_FORMAT_NV12);

    NV_ENC_INITIALIZE_PARAMS initializeParams = {NV_ENC_INITIALIZE_PARAMS_VER};
    NV_ENC_CONFIG encodeConfig = {NV_ENC_CONFIG_VER};
    initializeParams.encodeConfig = &encodeConfig;

    temp_enc.CreateDefaultEncoderParams(
        &initializeParams,
        NV_ENC_CODEC_HEVC_GUID,
        NV_ENC_PRESET_P1_GUID,
        NV_ENC_TUNING_INFO_ULTRA_LOW_LATENCY);
    temp_enc.CreateEncoder(&initializeParams);

    const NvEncInputFrame* temp_frame = temp_enc.GetNextInputFrame();
    if (!temp_frame) {
        temp_enc.DestroyEncoder();
        throw std::runtime_error("Failed to get temporary NVENC input frame while determining pitch");
    }

    const int encoder_pitch = temp_frame->pitch;
    temp_enc.DestroyEncoder();

    if (previous_device >= 0 && previous_device != camera_params->gpu_id) {
        ck(cudaSetDevice(previous_device));
    }

    return encoder_pitch;
}
} // namespace

ModernRecordingPipeline::ModernRecordingPipeline(
    CameraParams* camera_params,
    const RecordingOutputConfig& recording_output_config,
    const std::string& codec,
    const std::string& preset,
    const std::string& tuning,
    const std::string& rate_control_mode,
    int quality_value,
    int gop_length,
    const std::string& base_folder_name,
    SafeQueue<WORKER_ENTRY*>& recycle_queue,
    CameraControl* camera_control
)
    : camera_params_(camera_params),
      recording_output_config_(recording_output_config)
{
    const int encoder_pitch = determine_encoder_pitch();

    const std::string preprocess_name = "Preprocess_Cam_" + camera_params_->camera_serial;
    preprocess_worker_ = std::make_unique<EncoderPreprocessWorker>(
        preprocess_name.c_str(),
        camera_params_,
        recording_output_config_,
        encoder_pitch,
        recycle_queue,
        camera_control);

    const std::string hw_encoder_name = "HW_Encoder_Cam_" + camera_params_->camera_serial;
    hw_worker_ = std::make_unique<EncoderHwWorker>(
        hw_encoder_name.c_str(),
        camera_params_,
        recording_output_config_,
        codec,
        preset,
        tuning,
        rate_control_mode,
        quality_value,
        gop_length,
        base_folder_name,
        preprocess_worker_.get(),
        camera_control);

    preprocess_worker_->SetHwWorker(hw_worker_.get());
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

int ModernRecordingPipeline::determine_encoder_pitch() const
{
    return determine_encoder_pitch_for_output(camera_params_, recording_output_config_);
}
