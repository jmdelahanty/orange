// src/encoder_hw_worker.cpp

#include "encoder_hw_worker.h"
#include "encoder_preprocess_worker.h"
#include <iostream>
#include "global.h"
#include "NvEncoder/NvEncoder.h"
#include "cuda_context_debug.h"
#include "nvtx_profiling.h"
#include "project.h"

// Helper to initialize the FFmpeg-based file writer
static inline void initialize_writer_hw(Writer *writer, CameraParams *camera_params, std::string folder_name, std::string encoder_str)
{
    writer->video_file = folder_name + "/Cam" + camera_params->camera_serial + ".mp4";
    writer->metadata_file = folder_name + "/Cam" + camera_params->camera_serial + "_meta.csv";
    writer->keyframe_file = folder_name + "/Cam" + camera_params->camera_serial + "_keyframe.csv";

    if (encoder_str.find("h264") != std::string::npos) {
        writer->video = new FFmpegWriter(AV_CODEC_ID_H264, camera_params->width, camera_params->height, camera_params->frame_rate, writer->video_file.c_str(), writer->keyframe_file.c_str());
    } else {
        writer->video = new FFmpegWriter(AV_CODEC_ID_HEVC, camera_params->width, camera_params->height, camera_params->frame_rate, writer->video_file.c_str(), writer->keyframe_file.c_str());
    }
    writer->metadata = new std::ofstream();
    writer->metadata->open(writer->metadata_file.c_str());
     if (!(*writer->metadata))
    {
        std::cout << "Metadata file did not open!";
        return;
    }
    *writer->metadata << "frame_id,timestamp,timestamp_sys\n";
    writer->video->create_thread();
}

static inline void write_metadata_hw(std::ofstream *metadata, unsigned long long frame_id, unsigned long long timestamp, uint64_t timestamp_sys)
{
    if (metadata && metadata->is_open())
    {
        *metadata << frame_id << "," << timestamp << "," << timestamp_sys << '\n';
    }
}

// CORRECTED CONSTRUCTOR SIGNATURE
EncoderHwWorker::EncoderHwWorker(
    const char* name,
    CameraParams* camera_params,
    const std::string& codec,
    const std::string& preset,
    const std::string& tuning,
    std::string folder_name,
    EncoderPreprocessWorker* prep_worker,
    CameraControl* camera_control
)
: CThreadWorker(name),
  camera_params_(camera_params),
  folder_name_(folder_name),
  codec_(codec),
  m_prep_worker_(prep_worker),
  camera_control_(camera_control),
  encoder_(),
  m_stream(nullptr)
{
    ck(cudaSetDevice(camera_params_->gpu_id));
    ck(cudaStreamCreate(&m_stream));
    ck(cuCtxGetCurrent(&encoder_.cuContext));
    encoder_.pEnc = new NvEncoderCuda(encoder_.cuContext, camera_params_->width, camera_params_->height, NV_ENC_BUFFER_FORMAT_NV12);

    NV_ENC_INITIALIZE_PARAMS initializeParams = { NV_ENC_INITIALIZE_PARAMS_VER };
    NV_ENC_CONFIG encodeConfig = {NV_ENC_CONFIG_VER};
    initializeParams.encodeConfig = &encodeConfig;

    GUID codecGuid = (codec_ == "hevc") ? NV_ENC_CODEC_HEVC_GUID : NV_ENC_CODEC_H264_GUID;

    GUID presetGuid = NV_ENC_PRESET_P1_GUID;
    NV_ENC_TUNING_INFO tuningInfo = NV_ENC_TUNING_INFO_ULTRA_LOW_LATENCY;
    std::cout << "[" << this->threadName << "] Using FASTEST encoder settings (P1 Preset, Ultra-Low Latency)." << std::endl;

    encoder_.pEnc->CreateDefaultEncoderParams(&initializeParams, codecGuid, presetGuid, tuningInfo);

    initializeParams.frameRateNum = camera_params_->frame_rate;
    initializeParams.frameRateDen = 1;
    initializeParams.enablePTD = 1;
    
    // ============ OPTIMIZED SETTINGS START HERE ============
    
    // Switch to CBR for consistent performance (CRITICAL CHANGE)
    encodeConfig.rcParams.rateControlMode = NV_ENC_PARAMS_RC_CBR;  // Changed from VBR
    encodeConfig.rcParams.averageBitRate = 15000000;  // Reduced from 20MB to 15MB
    encodeConfig.rcParams.maxBitRate = 15000000;      // Same as average for CBR
    encodeConfig.rcParams.vbvBufferSize = encodeConfig.rcParams.averageBitRate / camera_params_->frame_rate;
    
    // GOP settings for seekability - 2 second intervals
    encodeConfig.gopLength = camera_params_->frame_rate * 2;  // 2-second GOP instead of 1-second
    encodeConfig.frameIntervalP = 1;  // Only P-frames between I-frames
    
    // DISABLE ALL QUALITY ENHANCEMENTS FOR SPEED
    encodeConfig.rcParams.enableAQ = 0;          // Disable adaptive quantization
    encodeConfig.rcParams.enableTemporalAQ = 0;   // Disable temporal AQ
    encodeConfig.rcParams.enableLookahead = 0;    // Disable lookahead
    encodeConfig.rcParams.lowDelayKeyFrameScale = 1;
    encodeConfig.rcParams.enableMinQP = 0;       // Disable min QP
    encodeConfig.rcParams.enableMaxQP = 0;       // Disable max QP
    encodeConfig.rcParams.strictGOPTarget = 0;   // Disable strict GOP target
    encodeConfig.rcParams.enableNonRefP = 0;     // Disable non-reference P frames
    
    // HEVC-specific optimizations
    if (codec_ == "hevc") {
        auto& hevcConfig = encodeConfig.encodeCodecConfig.hevcConfig;
        hevcConfig.pixelBitDepthMinus8 = 0;  // 8-bit encoding
        hevcConfig.idrPeriod = encodeConfig.gopLength;
        hevcConfig.sliceMode = 0;  // No slicing for speed
        hevcConfig.sliceModeData = 0;
        hevcConfig.maxNumRefFramesInDPB = 1;  // Minimize reference frames for speed
        hevcConfig.repeatSPSPPS = 1;  // Include SPS/PPS for seeking
        hevcConfig.outputBufferingPeriodSEI = 0;
        hevcConfig.outputPictureTimingSEI = 0;
        hevcConfig.outputAUD = 0;
        hevcConfig.enableLTR = 0;  // Disable long term references
    } else {
        // H.264 specific optimizations (if you test with H.264)
        auto& h264Config = encodeConfig.encodeCodecConfig.h264Config;
        h264Config.idrPeriod = encodeConfig.gopLength;
        h264Config.sliceMode = 0;
        h264Config.sliceModeData = 0;
        h264Config.repeatSPSPPS = 1;
        h264Config.maxNumRefFrames = 1;
        h264Config.adaptiveTransformMode = NV_ENC_H264_ADAPTIVE_TRANSFORM_DISABLE;
        h264Config.bdirectMode = NV_ENC_H264_BDIRECT_MODE_DISABLE;
        h264Config.entropyCodingMode = NV_ENC_H264_ENTROPY_CODING_MODE_CAVLC; // CAVLC is faster than CABAC
    }
    
    // Additional performance flags
    initializeParams.enableWeightedPrediction = 0;
    
    // ============ OPTIMIZED SETTINGS END HERE ============

    if (!camera_params_->color) {
        std::cout << "[" << this->threadName << "] Mono camera detected, enabling monoChromeEncoding." << std::endl;
        encodeConfig.monoChromeEncoding = 1;
    }
    
    // Add debug output to confirm settings
    std::cout << "[" << this->threadName << "] Encoder Configuration:" << std::endl;
    std::cout << "  - Rate Control: CBR (Constant Bitrate)" << std::endl;
    std::cout << "  - Bitrate: " << encodeConfig.rcParams.averageBitRate / 1000000 << " Mbps" << std::endl;
    std::cout << "  - GOP Length: " << encodeConfig.gopLength << " frames" << std::endl;
    std::cout << "  - Quality Enhancements: DISABLED" << std::endl;
    std::cout << "  - Target FPS: " << camera_params_->frame_rate << std::endl;

    encoder_.pEnc->CreateEncoder(&initializeParams);
    encoder_.pEnc->SetIOCudaStreams((NV_ENC_CUSTREAM_PTR)&m_stream, (NV_ENC_CUSTREAM_PTR)&m_stream);

    initialize_writer_hw(&writer_, camera_params_, folder_name_, codec_);
}

EncoderHwWorker::~EncoderHwWorker()
{
    flush_and_close();
    if (encoder_.pEnc) {
        delete encoder_.pEnc;
        encoder_.pEnc = nullptr;
    }
    if (m_stream) {
        cudaStreamDestroy(m_stream);
        m_stream = nullptr;
    }
}

void EncoderHwWorker::flush_and_close()
{
    if (encoder_.pEnc) {
        encoder_.pEnc->EndEncode(encoder_.vPacket);
        for (auto &packet : encoder_.vPacket)
        {
            writer_.video->push_packet(packet.data(), (int)packet.size(), ++last_recording_frame_id_);
        }
    }

    if (writer_.video) {
        writer_.video->quit_thread();
        writer_.video->join_thread();
        delete writer_.video;
        writer_.video = nullptr;
    }
    if (writer_.metadata && writer_.metadata->is_open()) {
        writer_.metadata->close();
        delete writer_.metadata;
        writer_.metadata = nullptr;
    }
}

bool EncoderHwWorker::WorkerFunction(ENCODER_WORKER_ENTRY* entry)
{
    // Start timing this frame's processing
    auto start_time = std::chrono::steady_clock::now();
    
    if (!entry) {
        // No work available in the queue, do nothing.
        return false;
    }

    // --- Performance Tracking (Enhanced) ---
    frame_counter_++;
    auto now = std::chrono::steady_clock::now();
    std::chrono::duration<double> elapsed = now - last_fps_update_time_;
    if (elapsed.count() >= 1.0) {
        current_fps_ = frame_counter_ / elapsed.count();
        
        // Detailed performance metrics
        std::cout << "[" << threadName << "] GPU " << camera_params_->gpu_id 
                  << " Camera " << camera_params_->camera_serial
                  << " | FPS: " << std::fixed << std::setprecision(1) << current_fps_
                  << " | Queue: " << this->GetCountQueueInSize()
                  << " | Packets: " << encoder_.vPacket.size()
                  << " | Slow frames: " << slow_frames_
                  << " | Encode fails: " << encode_failures_
                  << std::endl;
                  
        frame_counter_ = 0;
        slow_frames_ = 0;  // Reset slow frame counter each second
        last_fps_update_time_ = now;
    }

    ck(cudaSetDevice(camera_params_->gpu_id));

    try {
        // --- CRITICAL ASYNC STEP: Wait for the preprocess stage to finish ---
        if (entry->preprocess_complete_event) {
            ck(cudaStreamWaitEvent(m_stream, *entry->preprocess_complete_event, 0));
        }

        // Measure time waiting for encoder input buffer
        auto encode_start = std::chrono::steady_clock::now();
        const NvEncInputFrame* encoderInputFrame = encoder_.pEnc->GetNextInputFrame();
        
        if (!encoderInputFrame) {
            encode_failures_++;
            std::cerr << "[PERF WARNING] " << threadName 
                      << ": Failed to get encoder input frame!" << std::endl;
            throw std::runtime_error("No encoder input frame available");
        }

        // Copy the preprocessed frame into the hardware encoder's input buffer
        NvEncoderCuda::CopyToDeviceFrame(
            encoder_.cuContext,
            entry->d_prepared_frame,
            encoderInputFrame->pitch,
            (CUdeviceptr)encoderInputFrame->inputPtr,
            encoderInputFrame->pitch,
            encoder_.pEnc->GetEncodeWidth(),
            encoder_.pEnc->GetEncodeHeight(),
            CU_MEMORYTYPE_DEVICE,
            encoderInputFrame->bufferFormat,
            encoderInputFrame->chromaOffsets,
            encoderInputFrame->numChromaPlanes
        );

        // Encode the frame
        encoder_.pEnc->EncodeFrame(encoder_.vPacket);

        // Track packet generation
        size_t packets_generated = encoder_.vPacket.size();
        total_packets_ += packets_generated;
        
        // Send packets to FFmpeg writer
        for (auto& packet : encoder_.vPacket) {
            writer_.video->push_packet(packet.data(), (int)packet.size(), entry->recording_frame_id);
        }

        // Write metadata
        write_metadata_hw(writer_.metadata, entry->recording_frame_id, entry->timestamp, entry->timestamp_sys);
        last_recording_frame_id_ = entry->recording_frame_id;

        // Log if we're generating unusual numbers of packets
        if (packets_generated > 2) {
            std::cout << "[PERF INFO] " << threadName 
                      << ": Generated " << packets_generated 
                      << " packets for frame " << entry->recording_frame_id << std::endl;
        }

    } catch (const std::exception& e) {
        encode_failures_++;
        std::cerr << "[" << threadName << "] Exception: " << e.what() 
                  << " (Frame " << entry->recording_frame_id << ")" << std::endl;
    }

    // --- CRITICAL RECYCLING STEP ---
    if (m_prep_worker_) {
        // Return the event to the free pool so it can be used for another frame.
        if (entry->preprocess_complete_event) {
            m_prep_worker_->free_events_.push(entry->preprocess_complete_event);
            m_prep_worker_->available_events_++;  // Increment through the pointer
        }
        // Return the prepared frame buffer to the free pool.
        m_prep_worker_->free_encoder_entries_.push(entry);
        m_prep_worker_->available_buffers_++;  // Increment through the pointer
    }


    // Measure total processing time
    auto end_time = std::chrono::steady_clock::now();
    auto duration_us = std::chrono::duration_cast<std::chrono::microseconds>(end_time - start_time).count();
    
    // Track slow frames (> 12.5ms for 80fps target)
    if (duration_us > 12500) {
        slow_frames_++;
        std::cout << "[PERF WARNING] " << threadName 
                  << " Camera " << camera_params_->camera_serial
                  << " slow encode: " << duration_us << "μs"
                  << " (target: <12500μs for 80fps)"
                  << " Frame: " << entry->recording_frame_id << std::endl;
    }
    
    // Log extremely slow frames immediately
    if (duration_us > 25000) {  // > 25ms is very concerning
        std::cout << "[PERF CRITICAL] " << threadName 
                  << " VERY slow encode: " << duration_us << "μs!" << std::endl;
    }

    return false;
}