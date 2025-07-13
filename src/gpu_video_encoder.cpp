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
    SafeQueue<WORKER_ENTRY*>* input_queue,
    SafeQueue<WORKER_ENTRY*>& recycle_queue)
: CThreadWorker<WORKER_ENTRY>(name),
camera_params(camera_params),
folder_name(folder_name),
encoder_ready_signal(encoder_ready_signal),
m_input_queue(input_queue),
m_recycle_queue(recycle_queue),
m_stream(nullptr),
last_fps_update_time_(std::chrono::steady_clock::now()),
frame_counter_(0),
current_fps_(0.0),
encoder_pitch_(0)
{
    NVTX_ENCODE("GPUVideoEncoder_Constructor");
    CUDA_CTX_LOG("=== GPU Video Encoder Constructor START ===");
    
    std::cout << "[GPUVideoEncoder] Constructor for " << name << " on GPU " << camera_params->gpu_id << std::endl;
    std::cout << "[GPUVideoEncoder] OPTIMIZED DIRECT CONVERSION MODE: " << camera_params->width << "x" << camera_params->height << std::endl;

    try {
        ck(cudaSetDevice(camera_params->gpu_id));
        ck(cudaStreamCreate(&m_stream));
        ck(cuCtxGetCurrent(&encoder.cuContext));

        encoder.eFormat = NV_ENC_BUFFER_FORMAT_NV12;
        
        CUDA_CTX_LOG("Creating NVIDIA encoder");
        encoder.pEnc = new NvEncoderCuda(encoder.cuContext, camera_params->width, camera_params->height, encoder.eFormat);
        CUDA_CTX_LOG("NVIDIA encoder created successfully");
        
        NV_ENC_INITIALIZE_PARAMS initializeParams = { NV_ENC_INITIALIZE_PARAMS_VER };
        NV_ENC_CONFIG encodeConfig = {NV_ENC_CONFIG_VER};
        initializeParams.encodeConfig = &encodeConfig;

        GUID codecGuid = (codec == "hevc") ? NV_ENC_CODEC_HEVC_GUID : NV_ENC_CODEC_H264_GUID;
        GUID presetGuid = (preset == "p1") ? NV_ENC_PRESET_P1_GUID : (preset == "p5") ? NV_ENC_PRESET_P5_GUID : (preset == "p7") ? NV_ENC_PRESET_P7_GUID : NV_ENC_PRESET_P3_GUID;
        NV_ENC_TUNING_INFO tuningInfo = (tuning == "ll") ? NV_ENC_TUNING_INFO_LOW_LATENCY : (tuning == "ull") ? NV_ENC_TUNING_INFO_ULTRA_LOW_LATENCY : (tuning == "lossless") ? NV_ENC_TUNING_INFO_LOSSLESS : NV_ENC_TUNING_INFO_HIGH_QUALITY;

        encoder.pEnc->CreateDefaultEncoderParams(&initializeParams, codecGuid, presetGuid, tuningInfo);

        initializeParams.encodeWidth = camera_params->width;
        initializeParams.encodeHeight = camera_params->height;
        initializeParams.frameRateNum = camera_params->frame_rate;
        initializeParams.frameRateDen = 1;
        initializeParams.enablePTD = 1;

        if (tuningInfo == NV_ENC_TUNING_INFO_LOW_LATENCY || tuningInfo == NV_ENC_TUNING_INFO_ULTRA_LOW_LATENCY)
        {
            encodeConfig.gopLength = NVENC_INFINITE_GOPLENGTH;
            encodeConfig.frameIntervalP = 1;
            encodeConfig.rcParams.lowDelayKeyFrameScale = 1;
        }

        encodeConfig.rcParams.rateControlMode = NV_ENC_PARAMS_RC_VBR;
        encodeConfig.rcParams.averageBitRate = 20000000;
        encodeConfig.rcParams.maxBitRate = 25000000;
        encodeConfig.rcParams.vbvBufferSize = encodeConfig.rcParams.averageBitRate;

        if (!camera_params->color) {
            std::cout << "[GPUVideoEncoder] Mono camera detected, setting monoChromeEncoding to 1" << std::endl;
            encodeConfig.monoChromeEncoding = 1;
        }

        std::cout << "===== NVENC Initialization Parameters =====" << std::endl;
        std::cout << "  Width: " << initializeParams.encodeWidth << std::endl;
        std::cout << "  Height: " << initializeParams.encodeHeight << std::endl;
        std::cout << "  Frame Rate: " << initializeParams.frameRateNum << "/" << initializeParams.frameRateDen << std::endl;
        std::cout << "  Rate Control: " << encodeConfig.rcParams.rateControlMode << std::endl;
        std::cout << "  Avg Bitrate: " << encodeConfig.rcParams.averageBitRate << std::endl;
        std::cout << "  Input Format: " << encoder.eFormat << std::endl;
        std::cout << "========================================" << std::endl;

        encoder.pEnc->CreateEncoder(&initializeParams);
        encoder.pEnc->SetIOCudaStreams((NV_ENC_CUSTREAM_PTR)&m_stream, (NV_ENC_CUSTREAM_PTR)&m_stream);

        const NvEncInputFrame *tempFrame = encoder.pEnc->GetNextInputFrame();
        encoder_pitch_ = tempFrame->pitch;

        std::cout << "[GPUVideoEncoder] Encoder initialized with pitch: " << encoder_pitch_ << std::endl;

        std::vector<std::uint8_t> seqParams;
        encoder.pEnc->GetSequenceParams(seqParams);
        initialize_writer(&writer, camera_params, folder_name, codec, seqParams);
        writer.video->create_thread();
        
        std::cout << "[GPUVideoEncoder] Successfully initialized OPTIMIZED encoder for " << name 
                  << " - Codec: " << codec << ", Preset: " << preset << ", Tuning: " << tuning << std::endl;
        *encoder_ready_signal = true;
        
    } catch (const std::exception& e) {
        std::cerr << "[GPUVideoEncoder] Exception initializing encoder for " << name
                  << ": " << e.what() << std::endl;
        CUDA_CTX_LOG("Exception in constructor");
        throw;
    }
    
    CUDA_CTX_LOG("=== GPU Video Encoder Constructor END ===");
}

GPUVideoEncoder::~GPUVideoEncoder()
{
    NVTX_ENCODE("GPUVideoEncoder_Destructor");
    std::cout << "[GPUVideoEncoder] Destructor for " << this->threadName << std::endl;

    close_writer(&encoder, &writer);
    
    if (m_stream) { 
        cudaStreamDestroy(m_stream); 
    }
}
void GPUVideoEncoder::ThreadRunning()
{
    printf("GPUVideoEncoder Thread Start %d\n", GetID());
    while (IsMachineOn())
    {
        WORKER_ENTRY* f = nullptr;
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

    // Process any remaining items
    while (true)
    {
        WORKER_ENTRY* f = nullptr;
        if (m_input_queue && m_input_queue->pop(f))
        {
             if (f) WorkerFunction(f);
        }
        else
        {
            break;
        }
    }
    printf("GPUVideoEncoder Thread DONE %d\n", id);
}

bool GPUVideoEncoder::WorkerFunction(WORKER_ENTRY* entry)
{
    if (!entry) return false;

    ck(cudaSetDevice(camera_params->gpu_id));
    ENCODER_CTX_LOG("=== ENTERING WorkerFunction ===", entry->frame_id);
    NVTX_ENCODE("GPUEncoder_WorkerFunction");

    try {
        nppSetStream(m_stream);
        
        // Wait for camera data to be ready
        if (entry->event_ptr) {
            ck(cudaStreamWaitEvent(m_stream, *entry->event_ptr, 0));
        }

        // FPS tracking
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

        ENCODER_CTX_LOG("Processing frame dimensions", entry->frame_id);

        // === OPTIMIZED DIRECT CONVERSION ===
        const NvEncInputFrame* encoderInputFrame = encoder.pEnc->GetNextInputFrame();
        
        if (!camera_params->color) {
            // === OPTIMIZED MONO PATH: Direct mono → NV12 ===
            std::cout << "[GPUEncoder] OPTIMIZED MONO Frame " << entry->frame_id 
                      << " - Direct conversion (Mono8 → NV12)" << std::endl;

            // Y plane: Direct copy of mono data
            unsigned char* d_y_plane_dst = static_cast<unsigned char*>(encoderInputFrame->inputPtr);
            
            ck(cudaMemcpy2DAsync(
                d_y_plane_dst,                    // Destination: Encoder's Y-plane
                encoderInputFrame->pitch,         // Destination pitch
                entry->d_image,                   // Source: Raw mono data from camera
                entry->width,                     // Source pitch (mono width)
                entry->width,                     // Width to copy
                entry->height,                    // Height to copy
                cudaMemcpyDeviceToDevice,
                m_stream
            ));

            // UV plane: Set to neutral gray (128)
            unsigned char* d_uv_plane_dst = d_y_plane_dst + (encoderInputFrame->pitch * encoder.pEnc->GetEncodeHeight());
            size_t uv_height = encoder.pEnc->GetEncodeHeight() / 2;

            ck(cudaMemset2DAsync(
                d_uv_plane_dst,              // Destination: Encoder's UV-plane
                encoderInputFrame->pitch,    // Destination pitch
                128,                         // Neutral chroma value
                entry->width,                // Width to set
                uv_height,                   // Height of UV plane
                m_stream
            ));

            std::cout << "[GPUEncoder] OPTIMIZED MONO Frame " << entry->frame_id 
                      << " - Direct conversion completed" << std::endl;

        } else {
            // === COLOR PATH: For future color camera support ===
            std::cout << "[GPUEncoder] COLOR Frame " << entry->frame_id 
                      << " - Color conversion not yet implemented in optimized path" << std::endl;
            // TODO: Implement optimized color conversion if needed
            throw std::runtime_error("Color camera support not implemented in optimized encoder");
        }

        // Hardware encode
        ENCODER_CTX_LOG("About to call NVIDIA EncodeFrame - CRITICAL POINT", entry->frame_id);
        encoder.pEnc->EncodeFrame(encoder.vPacket);
        ENCODER_CTX_LOG("EncodeFrame completed successfully", entry->frame_id);

        // Write packets to file
        for (std::vector<uint8_t> &packet : encoder.vPacket) {
            writer.video->push_packet(packet.data(), (int)packet.size(), encoder.num_frame_encode++);
        }
        
        // Write metadata
        write_metadata(writer.metadata, entry->frame_id, entry->timestamp, entry->timestamp_sys);
        
        std::cout << "[GPUEncoder] Frame " << entry->frame_id << " encoded successfully" << std::endl;

        // Handle reference counting and cleanup
        ENCODER_CTX_LOG("Handling reference count", entry->frame_id);
        int remaining_refs = entry->ref_count.fetch_sub(1, std::memory_order_acq_rel);
        
        if (remaining_refs == 1) {
            // This is the last worker - handle cleanup
            if (entry->gpu_direct_mode && entry->camera_instance && entry->camera_frame_struct) {
                // GPU Direct: Requeue the camera buffer
                EVT_CameraQueueFrame(entry->camera_instance, entry->camera_frame_struct);
                std::cout << "[GPUEncoder] GPU DIRECT Frame " << entry->frame_id 
                          << " - Last worker requeued camera buffer" << std::endl;
                ENCODER_CTX_LOG("GPU Direct camera buffer requeued by last worker", entry->frame_id);
            }
            
            // Reset GPU Direct fields for recycling
            entry->gpu_direct_mode = false;
            entry->owns_memory = true;
            entry->camera_buffer_ptr = nullptr;
            entry->camera_instance = nullptr;
            entry->camera_frame_struct = nullptr;
            
            // Recycle the worker entry
            m_recycle_queue.push(entry);
            
            std::cout << "[GPUEncoder] Frame " << entry->frame_id 
                      << " - Last worker recycled entry" << std::endl;
        } else {
            std::cout << "[GPUEncoder] Frame " << entry->frame_id 
                      << " - Worker finished, " << (remaining_refs - 1) << " workers remaining" << std::endl;
        }

    } catch (const std::exception& e) {
        std::cerr << "[GPUEncoder] Exception in WorkerFunction: " << e.what() << std::endl;
        
        // Handle reference counting even on error
        int remaining_refs = entry->ref_count.fetch_sub(1, std::memory_order_acq_rel);
        if (remaining_refs == 1) {
            if (entry->gpu_direct_mode && entry->camera_instance && entry->camera_frame_struct) {
                EVT_CameraQueueFrame(entry->camera_instance, entry->camera_frame_struct);
            }
            entry->gpu_direct_mode = false;
            entry->owns_memory = true;
            entry->camera_buffer_ptr = nullptr;
            entry->camera_instance = nullptr;
            entry->camera_frame_struct = nullptr;
            m_recycle_queue.push(entry);
        }
        
        return false;
    }
    
    ENCODER_CTX_LOG("=== EXITING WorkerFunction ===", entry->frame_id);
    return false;
}