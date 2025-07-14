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

static std::string NvEncFormatToString(NV_ENC_BUFFER_FORMAT format) {
    switch (format) {
        case NV_ENC_BUFFER_FORMAT_NV12: return "NV12";
        case NV_ENC_BUFFER_FORMAT_YV12: return "YV12";
        case NV_ENC_BUFFER_FORMAT_IYUV: return "IYUV";
        case NV_ENC_BUFFER_FORMAT_YUV444: return "YUV444";
        case NV_ENC_BUFFER_FORMAT_ARGB: return "ARGB";
        default: return "Unknown";
    }
}

// Helper to initialize the FFmpeg-based file writer
static inline void initialize_writer(Writer *writer, CameraParams *camera_params, std::string folder_name, std::string encoder_str)
{
    NVTX_RANGE("Initialize_Writer");
    
    writer->video_file = folder_name + "/Cam" + camera_params->camera_serial + ".mp4";
    writer->metadata_file = folder_name + "/Cam" + camera_params->camera_serial + "_meta.csv";
    writer->keyframe_file = folder_name + "/Cam" + camera_params->camera_serial + "_keyframe.csv";

    if (encoder_str.find("h264") != std::string::npos) {
        writer->video = new FFmpegWriter(AV_CODEC_ID_H264, camera_params->width, camera_params->height, camera_params->frame_rate, writer->video_file.c_str(), writer->keyframe_file.c_str());
    } else if (encoder_str.find("hevc") != std::string::npos){
        writer->video = new FFmpegWriter(AV_CODEC_ID_HEVC, camera_params->width, camera_params->height, camera_params->frame_rate, writer->video_file.c_str(), writer->keyframe_file.c_str());
    } else {
        std::cout << "codec not supported" << '\n';
        writer->video = new FFmpegWriter(AV_CODEC_ID_H264, camera_params->width, camera_params->height, camera_params->frame_rate, writer->video_file.c_str(), writer->keyframe_file.c_str());
    }
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

GPUVideoEncoder::GPUVideoEncoder(
    const char* name,
    CameraParams *camera_params,
    const std::string& codec,
    const std::string& preset,
    const std::string& tuning,
    std::string folder_name,
    bool* encoder_ready_signal,
    SafeQueue<WORKER_ENTRY*>& raw_recycle_queue,
    SafeQueue<ProcessedFrame*>& processed_recycle_queue)
: CThreadWorker<ProcessedFrame>(name), // <-- Base class now uses ProcessedFrame
  camera_params(camera_params),
  folder_name(folder_name),
  encoder_ready_signal(encoder_ready_signal),
  m_raw_recycle_queue(raw_recycle_queue),         // Initialize the raw queue
  m_processed_recycle_queue(processed_recycle_queue), // Initialize the new processed queue
  m_stream(nullptr),
  d_rgb_temp_(nullptr),
  d_iyuv_temp_(nullptr),
  d_uv_default_plane_(nullptr),
  last_fps_update_time_(std::chrono::steady_clock::now()),
  frame_counter_(0),
  current_fps_(0.0),
  encoder_pitch_(0)
{
    NVTX_ENCODE("GPUVideoEncoder_Constructor");
    std::cout << "[GPUVideoEncoder] Constructor for " << name << " on GPU " << camera_params->gpu_id << std::endl;

    try {
        ck(cudaSetDevice(camera_params->gpu_id));
        ck(cuCtxGetCurrent(&encoder.cuContext));
        ck(cudaStreamCreate(&m_stream)); // Create the stream here

        // The preprocessor handles debayering, so we don't initialize it here.
        // We only need buffers for the color space conversion required by the encoder.
        NVTX_RANGE_PUSH("Allocate_GPU_Memory");
        // We need an intermediate buffer to convert the RGBA from the preprocessor to RGB.
        ck(cudaMalloc(&d_rgb_temp_, (size_t)camera_params->width * camera_params->height * 3));
        // This buffer holds the final IYUV/NV12 data for the encoder.
        ck(cudaMalloc(&d_iyuv_temp_, (size_t)camera_params->width * camera_params->height * 3 / 2));
        NVTX_RANGE_POP();

        // Encoder setup is largely the same...
        NVTX_RANGE_PUSH("Setup_Encoder");
        encoder.eFormat = NV_ENC_BUFFER_FORMAT_NV12;
        encoder.pEnc = new NvEncoderCuda(encoder.cuContext, camera_params->width, camera_params->height, encoder.eFormat);

        NV_ENC_INITIALIZE_PARAMS initializeParams = { NV_ENC_INITIALIZE_PARAMS_VER };
        NV_ENC_CONFIG encodeConfig = {NV_ENC_CONFIG_VER};
        initializeParams.encodeConfig = &encodeConfig;

        GUID codecGuid = (codec == "hevc") ? NV_ENC_CODEC_HEVC_GUID : NV_ENC_CODEC_H264_GUID;
        GUID presetGuid = (preset == "p1") ? NV_ENC_PRESET_P1_GUID : NV_ENC_PRESET_P3_GUID; // Simplified for clarity
        NV_ENC_TUNING_INFO tuningInfo = (tuning == "ll") ? NV_ENC_TUNING_INFO_LOW_LATENCY : NV_ENC_TUNING_INFO_HIGH_QUALITY;

        encoder.pEnc->CreateDefaultEncoderParams(&initializeParams, codecGuid, presetGuid, tuningInfo);
        initializeParams.frameRateNum = camera_params->frame_rate;
        // ... (other encoder params) ...
        encoder.pEnc->CreateEncoder(&initializeParams);
        encoder.pEnc->SetIOCudaStreams((NV_ENC_CUSTREAM_PTR)&m_stream, (NV_ENC_CUSTREAM_PTR)&m_stream);
        encoder_pitch_ = encoder.pEnc->GetNextInputFrame()->pitch;
        NVTX_RANGE_POP();

        NVTX_RANGE_PUSH("Initialize_File_Writer");
        initialize_writer(&writer, camera_params, folder_name, codec);
        writer.video->create_thread();
        NVTX_RANGE_POP();
        
        *encoder_ready_signal = true;
    }
    catch (const std::exception& e) {
        std::cerr << "[GPUVideoEncoder] EXCEPTION in constructor for " << name << ": " << e.what() << std::endl;
        throw;
    }
}

// Corrected Destructor
GPUVideoEncoder::~GPUVideoEncoder()
{
    NVTX_ENCODE("GPUVideoEncoder_Destructor");
    std::cout << "[GPUVideoEncoder] Destructor for " << this->threadName << std::endl;

    close_writer(&encoder, &writer);

    NVTX_RANGE_PUSH("Cleanup_CUDA_Resources");
    if (m_stream) { // Check if stream was created before destroying
        ck(cudaSetDevice(camera_params->gpu_id));
        if (d_rgb_temp_) cudaFree(d_rgb_temp_);
        if (d_iyuv_temp_) cudaFree(d_iyuv_temp_);
        cudaStreamDestroy(m_stream);
    }
    NVTX_RANGE_POP();
}

// Corrected WorkerFunction
bool GPUVideoEncoder::WorkerFunction(ProcessedFrame* f)
{
    if (!f) return false;

    NVTX_ENCODE("GPUEncoder_WorkerFunction");
    ck(cudaSetDevice(camera_params->gpu_id));
    nppSetStream(m_stream);

    try {
        if (f->processed_event_ptr) {
            ck(cudaStreamWaitEvent(m_stream, *f->processed_event_ptr, 0));
        }

        const int width = f->width;
        const int height = f->height;

        NVTX_RANGE_PUSH("Color_Conversion_for_Encoder");
        // The input 'f->d_processed_image' is always RGBA now.
        // We convert it to RGB, then to IYUV (a planar YUV420 format).
        rgba2rgb_convert(d_rgb_temp_, f->d_processed_image, width, height, m_stream);

        NppiSize image_size = {width, height};
        unsigned char* d_y_plane = d_iyuv_temp_;
        unsigned char* d_u_plane = d_iyuv_temp_ + ((size_t)encoder_pitch_ * height);
        unsigned char* d_v_plane = d_u_plane + ((size_t)encoder_pitch_ * height / 4);
        unsigned char* yuv_planes[3] = {d_y_plane, d_u_plane, d_v_plane};
        int yuv_steps[3] = {encoder_pitch_, encoder_pitch_ / 2, encoder_pitch_ / 2};

        nppiRGBToYUV420_8u_C3P3R(d_rgb_temp_, width * 3, yuv_planes, yuv_steps, image_size);
        NVTX_RANGE_POP();

        NVTX_ENCODE("NVIDIA_Hardware_Encode");
        const NvEncInputFrame *encoderInputFrame = encoder.pEnc->GetNextInputFrame();
        NvEncoderCuda::CopyToDeviceFrame(encoder.cuContext, d_iyuv_temp_, encoder_pitch_,
                                         (CUdeviceptr)encoderInputFrame->inputPtr, encoderInputFrame->pitch,
                                         width, height, CU_MEMORYTYPE_DEVICE,
                                         encoderInputFrame->bufferFormat, encoderInputFrame->chromaOffsets,
                                         encoderInputFrame->numChromaPlanes);

        encoder.pEnc->EncodeFrame(encoder.vPacket);
        
        for (auto& packet : encoder.vPacket) {
            writer.video->push_packet(packet.data(), (int)packet.size(), encoder.num_frame_encode++);
        }
        
        write_metadata(writer.metadata, f->frame_id, f->timestamp, f->timestamp_sys);

    } catch (const std::exception& e) {
        std::cerr << "[GPUEncoder] Exception processing frame " << f->frame_id << ": " << e.what() << std::endl;
    }

    // --- Corrected Recycling Logic ---
    if (f->ref_count.fetch_sub(1, std::memory_order_acq_rel) == 1) {
        // This is the last worker using this ProcessedFrame.
        // First, recycle the original raw entry.
        m_raw_recycle_queue.push(f->original_entry);
        
        // Then, recycle the processed frame struct itself.
        m_processed_recycle_queue.push(f);
    }
    
    return false;
}
