// src/encoder_hw_worker.cpp

#include "encoder_hw_worker.h"
#include "encoder_preprocess_worker.h"
#include <iostream>
#include <utility>
#include <vector>
#include "global.h"
#include "NvEncoder/NvEncoder.h"
#include "cuda_context_debug.h"
#include "nvtx_profiling.h"
#include "project.h"
#include <iomanip>
#include <sstream>

namespace {
constexpr uint64_t kMinQualityBitrate = 20000000ULL;
constexpr uint64_t kMaxQualityBitrate = 250000000ULL;
constexpr double kColorTargetBpp = 0.30;
constexpr double kMonoTargetBpp = 0.20;

uint32_t clamp_bitrate(uint64_t value) {
    if (value < kMinQualityBitrate) {
        return static_cast<uint32_t>(kMinQualityBitrate);
    }
    if (value > kMaxQualityBitrate) {
        return static_cast<uint32_t>(kMaxQualityBitrate);
    }
    return static_cast<uint32_t>(value);
}

uint32_t calculate_quality_bitrate(const CameraParams* camera_params) {
    const double target_bpp = camera_params->color ? kColorTargetBpp : kMonoTargetBpp;
    const double bits_per_sec =
        static_cast<double>(camera_params->width) *
        static_cast<double>(camera_params->height) *
        static_cast<double>(camera_params->frame_rate) *
        target_bpp;
    return clamp_bitrate(static_cast<uint64_t>(bits_per_sec));
}

bool is_low_latency_tuning(const std::string& tuning) {
    return tuning == "ll" || tuning == "ull";
}

bool is_lossless_tuning(const std::string& tuning) {
    return tuning == "lossless";
}

std::vector<std::pair<std::string, std::string>> build_metadata_tags(
    const CameraParams* camera_params,
    const std::string& codec,
    const std::string& preset,
    const std::string& tuning
) {
    std::vector<std::pair<std::string, std::string>> tags;
    tags.emplace_back("title", "Cam" + camera_params->camera_serial);

    std::ostringstream comment;
    comment << "nvenc codec=" << codec
            << "; preset=" << preset
            << "; tuning=" << tuning
            << "; res=" << camera_params->width << "x" << camera_params->height
            << "; fps=" << camera_params->frame_rate
            << "; color=" << (camera_params->color ? 1 : 0);

    if (is_lossless_tuning(tuning)) {
        comment << "; rc=constqp; qp=0";
    } else {
        const uint32_t target_bps = calculate_quality_bitrate(camera_params);
        const double actual_bpp = static_cast<double>(target_bps) /
                                  (static_cast<double>(camera_params->width) *
                                   static_cast<double>(camera_params->height) *
                                   static_cast<double>(camera_params->frame_rate));
        comment << "; rc=vbr; bpp=" << std::fixed << std::setprecision(3) << actual_bpp
                << "; target_bps=" << target_bps;
    }

    tags.emplace_back("comment", comment.str());
    return tags;
}

void apply_quality_recording_profile(
    NV_ENC_CONFIG& encodeConfig,
    const CameraParams* camera_params,
    bool low_latency
) {
    encodeConfig.rcParams.rateControlMode = NV_ENC_PARAMS_RC_VBR;
    encodeConfig.rcParams.averageBitRate = calculate_quality_bitrate(camera_params);

    uint64_t max_bitrate = static_cast<uint64_t>(encodeConfig.rcParams.averageBitRate) * 3 / 2;
    if (max_bitrate < encodeConfig.rcParams.averageBitRate) {
        max_bitrate = encodeConfig.rcParams.averageBitRate;
    }
    encodeConfig.rcParams.maxBitRate = clamp_bitrate(max_bitrate);
    encodeConfig.rcParams.vbvBufferSize = encodeConfig.rcParams.maxBitRate;

    encodeConfig.rcParams.enableAQ = 1;
    encodeConfig.rcParams.enableTemporalAQ = 1;
    encodeConfig.rcParams.enableLookahead = low_latency ? 0 : 1;
    encodeConfig.rcParams.lowDelayKeyFrameScale = low_latency ? 1 : 0;
}
} // namespace

// Helper to initialize the FFmpeg-based file writer
static inline void initialize_writer_hw(
    Writer *writer,
    CameraParams *camera_params,
    const std::string& folder_name,
    const std::string& encoder_str,
    const std::vector<std::pair<std::string, std::string>>& metadata_tags
)
{
    writer->video_file = folder_name + "/Cam" + camera_params->camera_serial + ".mp4";
    writer->metadata_file = folder_name + "/Cam" + camera_params->camera_serial + "_meta.csv";
    writer->keyframe_file = folder_name + "/Cam" + camera_params->camera_serial + "_keyframe.csv";

    if (encoder_str.find("h264") != std::string::npos) {
        writer->video = new FFmpegWriter(AV_CODEC_ID_H264, camera_params->width, camera_params->height, camera_params->frame_rate, writer->video_file.c_str(), writer->keyframe_file.c_str(), metadata_tags);
    } else {
        writer->video = new FFmpegWriter(AV_CODEC_ID_HEVC, camera_params->width, camera_params->height, camera_params->frame_rate, writer->video_file.c_str(), writer->keyframe_file.c_str(), metadata_tags);
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

EncoderHwWorker::EncoderHwWorker(
    const char* name,
    CameraParams* camera_params,
    const std::string& codec,
    const std::string& preset,
    const std::string& tuning,
    std::string base_folder_name,
    EncoderPreprocessWorker* prep_worker,
    CameraControl* camera_control
)
: CThreadWorker(name),
  camera_params_(camera_params),
  base_folder_name_(base_folder_name),
  codec_(codec),
  preset_(preset),
  tuning_(tuning),
  m_prep_worker_(prep_worker),
  camera_control_(camera_control),
  encoder_(),
  m_stream(nullptr),
  last_recording_frame_id_(0),
  last_fps_update_time_(std::chrono::steady_clock::now()),
  frame_counter_(0),
  current_fps_(0.0),
  is_recording_(false) // Initialize recording state
{
    ck(cudaSetDevice(camera_params_->gpu_id));
    ck(cudaStreamCreate(&m_stream));
    ck(cuCtxGetCurrent(&encoder_.cuContext));
    encoder_.pEnc = new NvEncoderCuda(encoder_.cuContext, camera_params_->width, camera_params_->height, NV_ENC_BUFFER_FORMAT_NV12);

    NV_ENC_INITIALIZE_PARAMS initializeParams = { NV_ENC_INITIALIZE_PARAMS_VER };
    NV_ENC_CONFIG encodeConfig = {NV_ENC_CONFIG_VER};
    initializeParams.encodeConfig = &encodeConfig;

    GUID codecGuid = (codec_ == "hevc") ? NV_ENC_CODEC_HEVC_GUID : NV_ENC_CODEC_H264_GUID;
    GUID presetGuid = (preset == "p1") ? NV_ENC_PRESET_P1_GUID : (preset == "p5") ? NV_ENC_PRESET_P5_GUID : (preset == "p7") ? NV_ENC_PRESET_P7_GUID : NV_ENC_PRESET_P3_GUID;
    NV_ENC_TUNING_INFO tuningInfo = (tuning == "ll") ? NV_ENC_TUNING_INFO_LOW_LATENCY : (tuning == "ull") ? NV_ENC_TUNING_INFO_ULTRA_LOW_LATENCY : (tuning == "lossless") ? NV_ENC_TUNING_INFO_LOSSLESS : NV_ENC_TUNING_INFO_HIGH_QUALITY;


    encoder_.pEnc->CreateDefaultEncoderParams(&initializeParams, codecGuid, presetGuid, tuningInfo);

    const bool low_latency = is_low_latency_tuning(tuning);
    const bool lossless = is_lossless_tuning(tuning);

    initializeParams.frameRateNum = camera_params_->frame_rate;
    initializeParams.frameRateDen = 1;
    initializeParams.enablePTD = 1;

    encodeConfig.gopLength = camera_params_->frame_rate * 2;
    encodeConfig.frameIntervalP = 1;

    if (lossless) {
        encodeConfig.rcParams.rateControlMode = NV_ENC_PARAMS_RC_CONSTQP;
        encodeConfig.rcParams.constQP = {0, 0, 0};
        encodeConfig.rcParams.enableAQ = 0;
        encodeConfig.rcParams.enableTemporalAQ = 0;
        encodeConfig.rcParams.enableLookahead = 0;
        encodeConfig.rcParams.lowDelayKeyFrameScale = 0;
        encodeConfig.gopLength = 1;
        encodeConfig.frameIntervalP = 1;
    } else {
        apply_quality_recording_profile(encodeConfig, camera_params_, low_latency);
    }

    encodeConfig.rcParams.enableMinQP = 0;
    encodeConfig.rcParams.enableMaxQP = 0;
    encodeConfig.rcParams.strictGOPTarget = 0;
    encodeConfig.rcParams.enableNonRefP = 0;
    initializeParams.enableWeightedPrediction = 0;

    if (codec_ == "hevc") {
        auto& hevcConfig = encodeConfig.encodeCodecConfig.hevcConfig;
        hevcConfig.pixelBitDepthMinus8 = 0;
        hevcConfig.idrPeriod = encodeConfig.gopLength;
        hevcConfig.sliceMode = 0;
        hevcConfig.sliceModeData = 0;
        hevcConfig.maxNumRefFramesInDPB = 1;
        hevcConfig.repeatSPSPPS = 1;
        hevcConfig.outputBufferingPeriodSEI = 0;
        hevcConfig.outputPictureTimingSEI = 0;
        hevcConfig.outputAUD = 0;
        hevcConfig.enableLTR = 0;
    } else {
        auto& h264Config = encodeConfig.encodeCodecConfig.h264Config;
        h264Config.idrPeriod = encodeConfig.gopLength;
        h264Config.sliceMode = 0;
        h264Config.sliceModeData = 0;
        h264Config.repeatSPSPPS = 1;
        h264Config.maxNumRefFrames = 1;
        h264Config.adaptiveTransformMode = NV_ENC_H264_ADAPTIVE_TRANSFORM_DISABLE;
        h264Config.bdirectMode = NV_ENC_H264_BDIRECT_MODE_DISABLE;
        h264Config.entropyCodingMode = NV_ENC_H264_ENTROPY_CODING_MODE_CAVLC;
    }

    if (!camera_params_->color) {
        encodeConfig.monoChromeEncoding = 1;
    }
    
    encoder_.pEnc->CreateEncoder(&initializeParams);
    encoder_.pEnc->SetIOCudaStreams((NV_ENC_CUSTREAM_PTR)&m_stream, (NV_ENC_CUSTREAM_PTR)&m_stream);
}

EncoderHwWorker::~EncoderHwWorker()
{
    // Safeguard: Ensure resources are released if the worker is destroyed.
    if(is_recording_)
    {
        flush_and_close();
    }
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
            if (writer_.video) {
                writer_.video->push_packet(packet.data(), (int)packet.size(), ++last_recording_frame_id_);
            }
        }
        encoder_.vPacket.clear();
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
    if (!entry) {
        if (is_recording_ && !camera_control_->record_video) {
            std::cout << "[" << this->threadName << "] HW Recording paused. Finalizing video file..." << std::endl;
            flush_and_close();
            is_recording_ = false;
            {
                std::lock_guard<std::mutex> lock(camera_control_->recording_folder_mutex);
                camera_control_->recording_folder.clear();
            }
        }
        return false;
    }

    // If the global flag is true but we aren't recording yet, start a new recording.
    if (camera_control_->record_video && !is_recording_) {
        std::cout << "[" << this->threadName << "] HW Recording started. Opening new video file..." << std::endl;

        // Create a new timestamped folder
        std::string current_recording_folder;
        {
            std::lock_guard<std::mutex> lock(camera_control_->recording_folder_mutex);
            if (camera_control_->recording_folder.empty()) {
                camera_control_->recording_folder = base_folder_name_ + "/" + get_current_date_time();
            }
            current_recording_folder = camera_control_->recording_folder;
        }
        make_folder(current_recording_folder);

        const auto metadata_tags = build_metadata_tags(camera_params_, codec_, preset_, tuning_);
        initialize_writer_hw(&writer_, camera_params_, current_recording_folder, codec_, metadata_tags);
        is_recording_ = true;
    }

    // If recording is globally disabled, skip processing but recycle resources.
    if (!camera_control_->record_video) {
        if (m_prep_worker_) {
            if (entry->preprocess_complete_event) {
                m_prep_worker_->free_events_.push(entry->preprocess_complete_event);
                m_prep_worker_->available_events_++;
            }
            m_prep_worker_->free_encoder_entries_.push(entry);
            m_prep_worker_->available_buffers_++;
        }
        return false;
    }

    auto start_time = std::chrono::steady_clock::now();
    frame_counter_++;
    auto now = std::chrono::steady_clock::now();
    std::chrono::duration<double> elapsed = now - last_fps_update_time_;
    // if (elapsed.count() >= 1.0) {
    //     current_fps_ = frame_counter_ / elapsed.count();
    //     std::cout << "[" << threadName << "] GPU " << camera_params_->gpu_id
    //               << " Camera " << camera_params_->camera_serial
    //               << " | FPS: " << std::fixed << std::setprecision(1) << current_fps_
    //               << " | Queue: " << this->GetCountQueueInSize()
    //               << " | Packets: " << encoder_.vPacket.size()
    //               << " | Slow frames: " << slow_frames_
    //               << " | Encode fails: " << encode_failures_
    //               << std::endl;
    //     frame_counter_ = 0;
    //     slow_frames_ = 0;
    //     last_fps_update_time_ = now;
    // }

    ck(cudaSetDevice(camera_params_->gpu_id));

    try {
        if (entry->preprocess_complete_event) {
            ck(cudaStreamWaitEvent(m_stream, *entry->preprocess_complete_event, 0));
        }

        const NvEncInputFrame* encoderInputFrame = encoder_.pEnc->GetNextInputFrame();
        
        if (!encoderInputFrame) {
            encode_failures_++;
            std::cerr << "[PERF WARNING] " << threadName
                      << ": Failed to get encoder input frame!" << std::endl;
            throw std::runtime_error("No encoder input frame available");
        }

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

        encoder_.pEnc->EncodeFrame(encoder_.vPacket);

        size_t packets_generated = encoder_.vPacket.size();
        total_packets_ += packets_generated;
        
        for (auto& packet : encoder_.vPacket) {
            writer_.video->push_packet(packet.data(), (int)packet.size(), entry->recording_frame_id);
        }

        write_metadata_hw(writer_.metadata, entry->recording_frame_id, entry->timestamp, entry->timestamp_sys);
        last_recording_frame_id_ = entry->recording_frame_id;

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

    if (m_prep_worker_) {
        if (entry->preprocess_complete_event) {
            m_prep_worker_->free_events_.push(entry->preprocess_complete_event);
            m_prep_worker_->available_events_++;
        }
        m_prep_worker_->free_encoder_entries_.push(entry);
        m_prep_worker_->available_buffers_++;
    }

    auto end_time = std::chrono::steady_clock::now();
    auto duration_us = std::chrono::duration_cast<std::chrono::microseconds>(end_time - start_time).count();
    
    if (duration_us > 12500) {
        slow_frames_++;
    }
    
    return false;
}
