// src/gpu_video_encoder.cpp

#include "gpu_video_encoder.h"
#include "kernel.cuh"
#include <npp.h>
#include <nppi.h>
#include <nppi_color_conversion.h>
#include <iostream>
#include "global.h"
#include "NvEncoder/NvEncoder.h"
#include "cuda_context_debug.h"
#include "nvtx_profiling.h"

static inline void initialize_writer(Writer *writer, CameraParams *camera_params, std::string folder_name, std::string encoder_str, std::vector<std::uint8_t>& seqParams)
{
    NVTX_RANGE("Initialize_Writer");

    writer->video_file = folder_name + "/Cam" + camera_params->camera_serial + ".mp4";
    writer->metadata_file = folder_name + "/Cam" + camera_params->camera_serial + "_meta.csv";
    writer->keyframe_file = folder_name + "/Cam" + camera_params->camera_serial + "_keyframe.csv";

    AVCodecID codecId = (encoder_str.find("hevc") != std::string::npos) ? AV_CODEC_ID_HEVC : AV_CODEC_ID_H264;

    writer->video = new FFmpegWriter(codecId, camera_params->width, camera_params->height, camera_params->frame_rate, writer->video_file.c_str(), writer->keyframe_file.c_str(), seqParams.data(), seqParams.size());

    writer->metadata = new std::ofstream();
    writer->metadata->open(writer->metadata_file.c_str());
     if (!(*writer->metadata))
    {
        std::cout << "Metadata file did not open!";
        return;
    }
    *writer->metadata << "frame_id,timestamp,timestamp_sys\n";
}


static inline void write_metadata(std::ofstream *metadata, unsigned long long frame_id, unsigned long long timestamp, uint64_t timestamp_sys)
{
    NVTX_RANGE("Write_Metadata");
    *metadata << frame_id << "," << timestamp << "," << timestamp_sys << std::endl;
}

static inline void close_writer(EncoderContext *encoder, Writer *writer)
{
    NVTX_RANGE("Close_Writer");
    
    if(encoder->pEnc) {
        NVTX_RANGE_PUSH("End_Encode");
        encoder->pEnc->EndEncode(encoder->vPacket);
        NVTX_RANGE_POP();
        
        NVTX_RANGE_PUSH("Flush_Remaining_Packets");
        for (std::vector<uint8_t> &packet : encoder->vPacket)
        {
            writer->video->push_packet(packet.data(), (int)packet.size(), encoder->num_frame_encode++);
        }
        NVTX_RANGE_POP();
        
        NVTX_RANGE_PUSH("Destroy_Encoder");
        encoder->pEnc->DestroyEncoder();
        delete encoder->pEnc;
        encoder->pEnc = nullptr;
        NVTX_RANGE_POP();
    }

    if(writer->video) {
        NVTX_RANGE_PUSH("Close_Video_Writer");
        writer->video->quit_thread();
        writer->video->join_thread();
        delete writer->video;
        writer->video = nullptr;
        NVTX_RANGE_POP();
    }

    if(writer->metadata && writer->metadata->is_open()) {
        NVTX_RANGE_PUSH("Close_Metadata");
        writer->metadata->close();
        delete writer->metadata;
        writer->metadata = nullptr;
        NVTX_RANGE_POP();
    }
}

GPUVideoEncoder::GPUVideoEncoder(const char* name, CameraParams *camera_params,
    const std::string& codec, const std::string& preset, const std::string& tuning,
    std::string folder_name, bool* encoder_ready_signal,
    SafeQueue<ProcessedFrame*>* input_queue, // <-- New parameter
    SafeQueue<WORKER_ENTRY*>& raw_recycle_queue,
    SafeQueue<ProcessedFrame*>& processed_recycle_queue)
: CThreadWorker<ProcessedFrame>(name),
  m_input_queue(input_queue), // <-- Store the queue
  camera_params(camera_params),
  folder_name(folder_name),
  encoder_ready_signal(encoder_ready_signal),
  m_recycle_queue(raw_recycle_queue),
  m_processed_recycle_queue(processed_recycle_queue),
  m_stream(nullptr),
  d_rgb_temp_(nullptr),
  d_iyuv_temp_(nullptr),
  d_uv_default_plane_(nullptr),
  last_fps_update_time_(std::chrono::steady_clock::now()),
  frame_counter_(0),
  current_fps_(0.0),
  scaled_width_(camera_params->width),
  scaled_height_(camera_params->height),
  d_scaled_mono_buffer_(nullptr),
  encoder_pitch_(0)
{
    NVTX_ENCODE("GPUVideoEncoder_Constructor");
    
    try {
        ck(cudaSetDevice(camera_params->gpu_id));
        ck(cudaStreamCreate(&m_stream));
        ck(cuCtxGetCurrent(&encoder.cuContext));
        
        ck(cudaMalloc(&d_rgb_temp_, scaled_width_ * scaled_height_ * 3));
        
        encoder.eFormat = NV_ENC_BUFFER_FORMAT_NV12;
        
        encoder.pEnc = new NvEncoderCuda(encoder.cuContext, camera_params->width, camera_params->height, encoder.eFormat);
        
        NV_ENC_INITIALIZE_PARAMS initializeParams = { NV_ENC_INITIALIZE_PARAMS_VER };
        NV_ENC_CONFIG encodeConfig = {NV_ENC_CONFIG_VER};
        initializeParams.encodeConfig = &encodeConfig;

        GUID codecGuid = (codec == "hevc") ? NV_ENC_CODEC_HEVC_GUID : NV_ENC_CODEC_H264_GUID;
        GUID presetGuid = NV_ENC_PRESET_P7_GUID;
        NV_ENC_TUNING_INFO tuningInfo = (tuning == "lossless") ? NV_ENC_TUNING_INFO_LOSSLESS : NV_ENC_TUNING_INFO_HIGH_QUALITY;

        encoder.pEnc->CreateDefaultEncoderParams(&initializeParams, codecGuid, presetGuid, tuningInfo);
        
        initializeParams.encodeWidth = scaled_width_;
        initializeParams.encodeHeight = scaled_height_;
        initializeParams.frameRateNum = camera_params->frame_rate;
        
        if (tuning != "lossless") {
            encodeConfig.rcParams.rateControlMode = NV_ENC_PARAMS_RC_VBR;
            encodeConfig.rcParams.averageBitRate = 20000000;
        } else {
            encodeConfig.rcParams.rateControlMode = NV_ENC_PARAMS_RC_CONSTQP;
            encodeConfig.rcParams.constQP = { 1, 1, 1 };
        }
        
        if (!camera_params->color) {
            encodeConfig.monoChromeEncoding = 1;
        }

        encoder.pEnc->CreateEncoder(&initializeParams);
        encoder.pEnc->SetIOCudaStreams((NV_ENC_CUSTREAM_PTR)&m_stream, (NV_ENC_CUSTREAM_PTR)&m_stream);

        const NvEncInputFrame *tempFrame = encoder.pEnc->GetNextInputFrame();
        encoder_pitch_ = tempFrame->pitch;

        size_t encoder_buffer_size = (size_t)encoder_pitch_ * scaled_height_ * 3 / 2;
        ck(cudaMalloc(&d_iyuv_temp_, encoder_buffer_size));

        std::vector<std::uint8_t> seqParams;
        encoder.pEnc->GetSequenceParams(seqParams);
        initialize_writer(&writer, camera_params, folder_name, codec, seqParams);
        writer.video->create_thread();

        *encoder_ready_signal = true;
        
    } catch (const std::exception& e) {
        std::cerr << "[GPUVideoEncoder] Exception initializing: " << e.what() << std::endl;
        throw;
    }
}

GPUVideoEncoder::~GPUVideoEncoder() {
    close_writer(&encoder, &writer);
    
    if (m_stream) cudaStreamDestroy(m_stream);
    if (d_rgb_temp_) cudaFree(d_rgb_temp_);
    if (d_iyuv_temp_) cudaFree(d_iyuv_temp_);
}

// CHANGE 2: Implement the overridden ThreadRunning function.
void GPUVideoEncoder::ThreadRunning()
{
    printf("GPUVideoEncoder Thread Start %d\n", GetID());
    while (IsMachineOn())
    {
        ProcessedFrame* f = nullptr;
        // Pop from the shared input queue.
        if (m_input_queue && m_input_queue->pop(f))
        {
            if (f)
            {
                WorkerFunction(f);
            }
        }
        else
        {
            usleep(1000); // 1ms sleep
        }
    }

    // Process remaining items.
    while (true)
    {
        ProcessedFrame* f = nullptr;
        if (m_input_queue && m_input_queue->pop(f))
        {
            if (f)
            {
                WorkerFunction(f);
            }
        }
        else
        {
            break;
        }
    }
    printf("GPUVideoEncoder Thread DONE %d\n", GetID());
}

// The WorkerFunction remains the same.
bool GPUVideoEncoder::WorkerFunction(ProcessedFrame* frame)
{
    if (!frame) return false;

    ENCODER_CTX_LOG("=== ENTERING WorkerFunction ===", frame->frame_id);
    dumpCudaState("WorkerFunction Entry", frame->frame_id);
    NVTX_ENCODE("GPUEncoder_WorkerFunction");

    try {
        NVTX_RANGE_PUSH("Setup_Device_And_Stream");
        ck(cudaSetDevice(camera_params->gpu_id));
        nppSetStream(m_stream);
        NVTX_RANGE_POP();
        
        NVTX_RANGE_PUSH("FPS_Tracking");
        frame_counter_++;
        auto now = std::chrono::steady_clock::now();
        std::chrono::duration<double> elapsed = now - last_fps_update_time_;
        if (elapsed.count() >= 1.0) {
            current_fps_ = frame_counter_ / elapsed.count();
            std::cout << "[" << this->threadName << "] Encoding FPS: " << current_fps_
                      << " (Queue depth: " << this->GetCountQueueInSize() << ")" << std::endl;
            frame_counter_ = 0;
            last_fps_update_time_ = now;
        }
        NVTX_RANGE_POP();

        ENCODER_CTX_LOG("Processing frame dimensions", frame->frame_id);
        
        NVTX_RANGE_PUSH("Color_Conversion_For_Encoder");
        rgba2rgb_convert(d_rgb_temp_, frame->d_processed_image, frame->width, frame->height, m_stream);
        
        NppiSize image_size = {frame->width, frame->height};
        unsigned char* d_y_plane = d_iyuv_temp_;
        unsigned char* d_u_plane = d_y_plane + ((size_t)encoder_pitch_ * frame->height);
        unsigned char* d_v_plane = d_u_plane + ((size_t)encoder_pitch_ * frame->height / 4);
        
        unsigned char* yuv_planes[3] = {d_y_plane, d_u_plane, d_v_plane};
        int yuv_steps[3] = {encoder_pitch_, encoder_pitch_ / 2, encoder_pitch_ / 2};

        NppStatus npp_status = nppiRGBToYUV420_8u_C3P3R(d_rgb_temp_, frame->width * 3, yuv_planes, yuv_steps, image_size);
        if (npp_status != NPP_SUCCESS) {
            std::cerr << "[GPUEncoder] NPP RGB to YUV420 conversion failed: " << npp_status << std::endl;
        }
        NVTX_RANGE_POP();

        ENCODER_CTX_LOG("About to call NVIDIA EncodeFrame", frame->frame_id);
        
        const NvEncInputFrame *encoderInputFrame = encoder.pEnc->GetNextInputFrame();
        
        NvEncoderCuda::CopyToDeviceFrame(encoder.cuContext,
                                         d_iyuv_temp_,
                                         encoder_pitch_, 
                                         (CUdeviceptr)encoderInputFrame->inputPtr,
                                         encoderInputFrame->pitch,
                                         encoder.pEnc->GetEncodeWidth(),
                                         encoder.pEnc->GetEncodeHeight(),
                                         CU_MEMORYTYPE_DEVICE,
                                         encoderInputFrame->bufferFormat,
                                         encoderInputFrame->chromaOffsets,
                                         encoderInputFrame->numChromaPlanes);

        encoder.pEnc->EncodeFrame(encoder.vPacket);
        
        for (std::vector<uint8_t> &packet : encoder.vPacket) {
            writer.video->push_packet(packet.data(), (int)packet.size(), encoder.num_frame_encode++);
        }
        
        write_metadata(writer.metadata, frame->frame_id, frame->timestamp, frame->original_entry->timestamp_sys);
        
    } catch (const std::exception& e) {
        std::cerr << "[GPUEncoder] Exception in WorkerFunction: " << e.what() << std::endl;
    }
    
    if (frame->ref_count.fetch_sub(1, std::memory_order_acq_rel) == 1) {
        m_recycle_queue.push(frame->original_entry);
        m_processed_recycle_queue.push(frame);
    }

    ENCODER_CTX_LOG("=== EXITING WorkerFunction ===", frame->frame_id);
    return false;
}