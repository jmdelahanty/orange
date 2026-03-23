// src/encoder_hw_worker.cpp

#include "encoder_hw_worker.h"
#include "encoder_preprocess_worker.h"
#include "fsuid_guard.h"
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
constexpr int kDefaultQualityValue = 20;
constexpr int kMinQualityValue = 1;
constexpr int kMaxQualityValue = 51;

uint32_t clamp_bitrate(uint64_t value) {
    if (value < kMinQualityBitrate) {
        return static_cast<uint32_t>(kMinQualityBitrate);
    }
    if (value > kMaxQualityBitrate) {
        return static_cast<uint32_t>(kMaxQualityBitrate);
    }
    return static_cast<uint32_t>(value);
}

uint32_t calculate_quality_bitrate(bool color, int width, int height, int frame_rate) {
    const double target_bpp = color ? kColorTargetBpp : kMonoTargetBpp;
    const double bits_per_sec =
        static_cast<double>(width) *
        static_cast<double>(height) *
        static_cast<double>(frame_rate) *
        target_bpp;
    return clamp_bitrate(static_cast<uint64_t>(bits_per_sec));
}

bool is_low_latency_tuning(const std::string& tuning) {
    return tuning == "ll" || tuning == "ull";
}

bool is_lossless_tuning(const std::string& tuning) {
    return tuning == "lossless";
}

bool is_vbr_cq_rate_control(const std::string& rate_control_mode) {
    return rate_control_mode == "vbr_cq";
}

bool is_cqp_rate_control(const std::string& rate_control_mode) {
    return rate_control_mode == "cqp";
}

int clamp_quality_value(int value) {
    if (value < kMinQualityValue) {
        return kDefaultQualityValue;
    }
    if (value > kMaxQualityValue) {
        return kMaxQualityValue;
    }
    return value;
}

std::string resolve_rate_control_strategy(
    const std::string& tuning,
    const std::string& rate_control_mode
) {
    if (is_lossless_tuning(tuning)) {
        return "lossless";
    }
    if (is_cqp_rate_control(rate_control_mode)) {
        return "cqp";
    }
    if (is_vbr_cq_rate_control(rate_control_mode)) {
        return "vbr_cq";
    }
    return "vbr";
}

const char* rc_mode_to_string(NV_ENC_PARAMS_RC_MODE mode) {
    switch (mode) {
        case NV_ENC_PARAMS_RC_CONSTQP: return "constqp";
        case NV_ENC_PARAMS_RC_VBR: return "vbr";
        case NV_ENC_PARAMS_RC_CBR: return "cbr";
        case NV_ENC_PARAMS_RC_CBR_LOWDELAY_HQ: return "cbr_lowdelay_hq";
        case NV_ENC_PARAMS_RC_CBR_HQ: return "cbr_hq";
        case NV_ENC_PARAMS_RC_VBR_HQ: return "vbr_hq";
        default: return "unknown";
    }
}

std::vector<std::pair<std::string, std::string>> build_metadata_tags(
    const CameraParams* camera_params,
    const RecordingOutputConfig& recording_output_config,
    const std::string& codec,
    const std::string& preset,
    const std::string& tuning,
    const std::string& rate_control_mode,
    int quality_value
) {
    std::vector<std::pair<std::string, std::string>> tags;
    tags.emplace_back("title", "Cam" + camera_params->camera_serial);

    std::ostringstream comment;
    const std::string rc_strategy = resolve_rate_control_strategy(tuning, rate_control_mode);
    comment << "nvenc codec=" << codec
            << "; preset=" << preset
            << "; tuning=" << tuning
            << "; res=" << recording_output_config.resolved_width << "x" << recording_output_config.resolved_height
            << "; fps=" << camera_params->frame_rate
            << "; color=" << (camera_params->color ? 1 : 0)
            << "; output_mode=" << recording_output_config.mode;

    if (recording_output_config.resize_enabled) {
        comment << "; source_res=" << camera_params->width << "x" << camera_params->height;
    }

    if (rc_strategy == "lossless") {
        comment << "; rc=constqp; qp=0";
    } else if (rc_strategy == "cqp") {
        comment << "; rc=constqp; qp=" << clamp_quality_value(quality_value);
    } else if (rc_strategy == "vbr_cq") {
        const uint32_t target_bps = calculate_quality_bitrate(
            camera_params->color,
            recording_output_config.resolved_width,
            recording_output_config.resolved_height,
            camera_params->frame_rate);
        const double actual_bpp = static_cast<double>(target_bps) /
                                  (static_cast<double>(recording_output_config.resolved_width) *
                                   static_cast<double>(recording_output_config.resolved_height) *
                                   static_cast<double>(camera_params->frame_rate));
        comment << "; rc=vbr; cq=" << clamp_quality_value(quality_value)
                << "; bpp_cap=" << std::fixed << std::setprecision(3) << actual_bpp
                << "; target_bps=" << target_bps;
    } else {
        const uint32_t target_bps = calculate_quality_bitrate(
            camera_params->color,
            recording_output_config.resolved_width,
            recording_output_config.resolved_height,
            camera_params->frame_rate);
        const double actual_bpp = static_cast<double>(target_bps) /
                                  (static_cast<double>(recording_output_config.resolved_width) *
                                   static_cast<double>(recording_output_config.resolved_height) *
                                   static_cast<double>(camera_params->frame_rate));
        comment << "; rc=vbr; bpp=" << std::fixed << std::setprecision(3) << actual_bpp
                << "; target_bps=" << target_bps;
    }

    if (recording_output_config.mode == "factor") {
        comment << "; factor=" << recording_output_config.downsample_factor;
    } else {
        comment << "; requested_res=" << recording_output_config.requested_width
                << "x" << recording_output_config.requested_height;
    }

    tags.emplace_back("comment", comment.str());
    return tags;
}

void apply_quality_recording_profile(
    NV_ENC_CONFIG& encodeConfig,
    const CameraParams* camera_params,
    const RecordingOutputConfig& recording_output_config,
    bool low_latency
) {
    encodeConfig.rcParams.rateControlMode = NV_ENC_PARAMS_RC_VBR;
    encodeConfig.rcParams.averageBitRate = calculate_quality_bitrate(
        camera_params->color,
        recording_output_config.resolved_width,
        recording_output_config.resolved_height,
        camera_params->frame_rate);

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

void apply_vbr_cq_recording_profile(
    NV_ENC_CONFIG& encodeConfig,
    const CameraParams* camera_params,
    const RecordingOutputConfig& recording_output_config,
    bool low_latency,
    int quality_value
) {
    apply_quality_recording_profile(encodeConfig, camera_params, recording_output_config, low_latency);
    encodeConfig.rcParams.targetQuality = static_cast<uint8_t>(clamp_quality_value(quality_value));
    encodeConfig.rcParams.targetQualityLSB = 0;
}

void apply_cqp_recording_profile(
    NV_ENC_CONFIG& encodeConfig,
    int quality_value
) {
    const uint8_t qp = static_cast<uint8_t>(clamp_quality_value(quality_value));
    encodeConfig.rcParams.rateControlMode = NV_ENC_PARAMS_RC_CONSTQP;
    encodeConfig.rcParams.constQP = {qp, qp, qp};
    encodeConfig.rcParams.averageBitRate = 0;
    encodeConfig.rcParams.maxBitRate = 0;
    encodeConfig.rcParams.vbvBufferSize = 0;
    encodeConfig.rcParams.targetQuality = 0;
    encodeConfig.rcParams.targetQualityLSB = 0;
    encodeConfig.rcParams.enableAQ = 0;
    encodeConfig.rcParams.enableTemporalAQ = 0;
    encodeConfig.rcParams.enableLookahead = 0;
    encodeConfig.rcParams.lowDelayKeyFrameScale = 0;
}
} // namespace

// Helper to initialize the FFmpeg-based file writer
static inline void initialize_writer_hw(
    Writer *writer,
    CameraParams *camera_params,
    const RecordingOutputConfig& recording_output_config,
    const std::string& folder_name,
    const std::string& encoder_str,
    const std::vector<std::pair<std::string, std::string>>& metadata_tags
)
{
    writer->video_file = folder_name + "/Cam" + camera_params->camera_serial + ".mp4";
    writer->metadata_file = folder_name + "/Cam" + camera_params->camera_serial + "_meta.csv";
    writer->keyframe_file = folder_name + "/Cam" + camera_params->camera_serial + "_keyframe.csv";

    if (encoder_str.find("h264") != std::string::npos) {
        writer->video = new FFmpegWriter(
            AV_CODEC_ID_H264,
            recording_output_config.resolved_width,
            recording_output_config.resolved_height,
            camera_params->frame_rate,
            writer->video_file.c_str(),
            writer->keyframe_file.c_str(),
            metadata_tags);
    } else {
        writer->video = new FFmpegWriter(
            AV_CODEC_ID_HEVC,
            recording_output_config.resolved_width,
            recording_output_config.resolved_height,
            camera_params->frame_rate,
            writer->video_file.c_str(),
            writer->keyframe_file.c_str(),
            metadata_tags);
    }
    writer->metadata = new std::ofstream();
    writer->metadata->open(writer->metadata_file.c_str());
     if (!(*writer->metadata))
    {
        std::cout << "Metadata file did not open!";
        delete writer->metadata;
        writer->metadata = nullptr;
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
    const RecordingOutputConfig& recording_output_config,
    const std::string& codec,
    const std::string& preset,
    const std::string& tuning,
    const std::string& rate_control_mode,
    int quality_value,
    std::string base_folder_name,
    EncoderPreprocessWorker* prep_worker,
    CameraControl* camera_control
)
: CThreadWorker(name),
  camera_params_(camera_params),
  recording_output_config_(recording_output_config),
  base_folder_name_(base_folder_name),
  codec_(codec),
  preset_(preset),
  tuning_(tuning),
  rate_control_mode_(rate_control_mode.empty() ? "vbr" : rate_control_mode),
  quality_value_(clamp_quality_value(quality_value)),
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
    encoder_.pEnc = new NvEncoderCuda(
        encoder_.cuContext,
        recording_output_config_.resolved_width,
        recording_output_config_.resolved_height,
        NV_ENC_BUFFER_FORMAT_NV12);

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
    initializeParams.encodeWidth = recording_output_config_.resolved_width;
    initializeParams.encodeHeight = recording_output_config_.resolved_height;
    initializeParams.darWidth = recording_output_config_.resolved_width;
    initializeParams.darHeight = recording_output_config_.resolved_height;

    encodeConfig.gopLength = camera_params_->frame_rate;
    encodeConfig.frameIntervalP = 1;

    if (lossless) {
        encodeConfig.rcParams.rateControlMode = NV_ENC_PARAMS_RC_CONSTQP;
        encodeConfig.rcParams.constQP = {0, 0, 0};
        encodeConfig.rcParams.averageBitRate = 0;
        encodeConfig.rcParams.maxBitRate = 0;
        encodeConfig.rcParams.vbvBufferSize = 0;
        encodeConfig.rcParams.targetQuality = 0;
        encodeConfig.rcParams.targetQualityLSB = 0;
        encodeConfig.rcParams.enableAQ = 0;
        encodeConfig.rcParams.enableTemporalAQ = 0;
        encodeConfig.rcParams.enableLookahead = 0;
        encodeConfig.rcParams.lowDelayKeyFrameScale = 0;
        encodeConfig.gopLength = 1;
        encodeConfig.frameIntervalP = 1;
    } else if (is_cqp_rate_control(rate_control_mode_)) {
        apply_cqp_recording_profile(encodeConfig, quality_value_);
    } else if (is_vbr_cq_rate_control(rate_control_mode_)) {
        apply_vbr_cq_recording_profile(
            encodeConfig,
            camera_params_,
            recording_output_config_,
            low_latency,
            quality_value_);
    } else {
        apply_quality_recording_profile(encodeConfig, camera_params_, recording_output_config_, low_latency);
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

    {
        NV_ENC_INITIALIZE_PARAMS resolved_params = { NV_ENC_INITIALIZE_PARAMS_VER };
        NV_ENC_CONFIG resolved_config = { NV_ENC_CONFIG_VER };
        resolved_params.encodeConfig = &resolved_config;
        encoder_.pEnc->GetInitializeParams(&resolved_params);

        encoder_snapshot_.backend = "nvenc";
        encoder_snapshot_.path = "hw";
        encoder_snapshot_.codec = codec_;
        encoder_snapshot_.preset = preset_;
        encoder_snapshot_.tuning = tuning_;
        encoder_snapshot_.rc_strategy = resolve_rate_control_strategy(tuning_, rate_control_mode_);
        encoder_snapshot_.output_mode = recording_output_config_.mode;
        encoder_snapshot_.width = resolved_params.encodeWidth;
        encoder_snapshot_.height = resolved_params.encodeHeight;
        encoder_snapshot_.source_width = camera_params_->width;
        encoder_snapshot_.source_height = camera_params_->height;
        encoder_snapshot_.fps = resolved_params.frameRateNum;
        encoder_snapshot_.downsample_factor = recording_output_config_.downsample_factor;
        encoder_snapshot_.requested_output_width = recording_output_config_.requested_width;
        encoder_snapshot_.requested_output_height = recording_output_config_.requested_height;
        encoder_snapshot_.gop_length = resolved_config.gopLength;
        encoder_snapshot_.frame_interval_p = resolved_config.frameIntervalP;
        encoder_snapshot_.rc_mode = resolved_config.rcParams.rateControlMode;
        encoder_snapshot_.average_bitrate = resolved_config.rcParams.averageBitRate;
        encoder_snapshot_.max_bitrate = resolved_config.rcParams.maxBitRate;
        encoder_snapshot_.vbv_buffer_size = resolved_config.rcParams.vbvBufferSize;
        encoder_snapshot_.target_quality = resolved_config.rcParams.targetQuality;
        encoder_snapshot_.target_quality_lsb = resolved_config.rcParams.targetQualityLSB;
        encoder_snapshot_.const_qp_inter_p = resolved_config.rcParams.constQP.qpInterP;
        encoder_snapshot_.const_qp_inter_b = resolved_config.rcParams.constQP.qpInterB;
        encoder_snapshot_.const_qp_intra = resolved_config.rcParams.constQP.qpIntra;
        encoder_snapshot_.enable_aq = resolved_config.rcParams.enableAQ;
        encoder_snapshot_.enable_temporal_aq = resolved_config.rcParams.enableTemporalAQ;
        encoder_snapshot_.enable_lookahead = resolved_config.rcParams.enableLookahead;
        encoder_snapshot_.low_delay_keyframe_scale = resolved_config.rcParams.lowDelayKeyFrameScale;
        encoder_snapshot_.strict_gop_target = resolved_config.rcParams.strictGOPTarget;
        encoder_snapshot_.enable_non_ref_p = resolved_config.rcParams.enableNonRefP;
        encoder_snapshot_.enable_ptd = resolved_params.enablePTD;
        encoder_snapshot_.gpu_id = camera_params_->gpu_id;
        encoder_snapshot_.color = camera_params_->color;

        if (codec_ == "hevc") {
            const auto& hevc_config = resolved_config.encodeCodecConfig.hevcConfig;
            encoder_snapshot_.idr_period = hevc_config.idrPeriod;
            encoder_snapshot_.max_num_ref_frames_in_dpb = hevc_config.maxNumRefFramesInDPB;
            encoder_snapshot_.repeat_sps_pps = hevc_config.repeatSPSPPS;
        } else {
            const auto& h264_config = resolved_config.encodeCodecConfig.h264Config;
            encoder_snapshot_.idr_period = h264_config.idrPeriod;
            encoder_snapshot_.max_num_ref_frames = h264_config.maxNumRefFrames;
            encoder_snapshot_.repeat_sps_pps = h264_config.repeatSPSPPS;
        }

        encoder_snapshot_valid_ = true;
    }
}

EncoderHwWorker::~EncoderHwWorker()
{
    // Safeguard: Ensure resources are released if the worker is destroyed.
    if(is_recording_)
    {
        finalize_recording();
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

void EncoderHwWorker::finalize_recording()
{
    if (!is_recording_) {
        return;
    }

    flush_and_close();
    is_recording_ = false;

    if (camera_control_) {
        int remaining = camera_control_->active_recorders.fetch_sub(1, std::memory_order_relaxed) - 1;
        if (remaining == 0) {
            if (camera_control_->recording_draining) {
                camera_control_->recording_draining = false;
            }
            camera_control_->stop_record = false;
            std::lock_guard<std::mutex> lock(camera_control_->recording_folder_mutex);
            camera_control_->recording_folder.clear();
        }
    }
}

bool EncoderHwWorker::drain_ready()
{
    if (GetCountQueueInSize() > 0) {
        return false;
    }
    if (m_prep_worker_ && !m_prep_worker_->IsDrained()) {
        return false;
    }
    return true;
}

nlohmann::json EncoderHwWorker::build_encoder_snapshot_json() const
{
    nlohmann::json info;
    info["backend"] = encoder_snapshot_.backend;
    info["path"] = encoder_snapshot_.path;
    info["codec"] = encoder_snapshot_.codec;
    info["preset"] = encoder_snapshot_.preset;
    info["tuning"] = encoder_snapshot_.tuning;
    info["gpu_id"] = encoder_snapshot_.gpu_id;
    info["color"] = encoder_snapshot_.color;
    info["resolution"] = {
        {"width", encoder_snapshot_.width},
        {"height", encoder_snapshot_.height}
    };
    info["source_resolution"] = {
        {"width", encoder_snapshot_.source_width},
        {"height", encoder_snapshot_.source_height}
    };
    info["output"] = {
        {"mode", encoder_snapshot_.output_mode},
        {"resize_enabled",
         encoder_snapshot_.source_width != encoder_snapshot_.width ||
             encoder_snapshot_.source_height != encoder_snapshot_.height},
        {"resolved_resolution",
         {
             {"width", encoder_snapshot_.width},
             {"height", encoder_snapshot_.height}
         }}
    };
    if (encoder_snapshot_.output_mode == "factor") {
        info["output"]["downsample_factor"] = encoder_snapshot_.downsample_factor;
    } else {
        info["output"]["requested_output_size"] = {
            {"width", encoder_snapshot_.requested_output_width},
            {"height", encoder_snapshot_.requested_output_height}
        };
    }
    info["fps"] = encoder_snapshot_.fps;
    info["gop_length"] = encoder_snapshot_.gop_length;
    info["frame_interval_p"] = encoder_snapshot_.frame_interval_p;
    info["idr_period"] = encoder_snapshot_.idr_period;

    nlohmann::json refs;
    if (encoder_snapshot_.max_num_ref_frames > 0) {
        refs["max_num_ref_frames"] = encoder_snapshot_.max_num_ref_frames;
    }
    if (encoder_snapshot_.max_num_ref_frames_in_dpb > 0) {
        refs["max_num_ref_frames_in_dpb"] = encoder_snapshot_.max_num_ref_frames_in_dpb;
    }
    if (!refs.empty()) {
        info["refs"] = refs;
    }

    info["rc"] = {
        {"strategy", encoder_snapshot_.rc_strategy},
        {"mode", rc_mode_to_string(static_cast<NV_ENC_PARAMS_RC_MODE>(encoder_snapshot_.rc_mode))},
        {"mode_value", encoder_snapshot_.rc_mode},
        {"average_bitrate", encoder_snapshot_.average_bitrate},
        {"max_bitrate", encoder_snapshot_.max_bitrate},
        {"vbv_buffer_size", encoder_snapshot_.vbv_buffer_size}
    };
    if (encoder_snapshot_.target_quality > 0 || encoder_snapshot_.target_quality_lsb > 0) {
        info["rc"]["target_quality"] = encoder_snapshot_.target_quality;
        info["rc"]["target_quality_lsb"] = encoder_snapshot_.target_quality_lsb;
    }
    if (encoder_snapshot_.rc_mode == NV_ENC_PARAMS_RC_CONSTQP ||
        encoder_snapshot_.rc_strategy == "lossless") {
        info["rc"]["const_qp"] = {
            {"p", encoder_snapshot_.const_qp_inter_p},
            {"b", encoder_snapshot_.const_qp_inter_b},
            {"i", encoder_snapshot_.const_qp_intra}
        };
    }
    info["aq"] = {
        {"enable_aq", encoder_snapshot_.enable_aq},
        {"enable_temporal_aq", encoder_snapshot_.enable_temporal_aq}
    };
    info["lookahead"] = {
        {"enable", encoder_snapshot_.enable_lookahead}
    };
    info["low_delay_keyframe_scale"] = encoder_snapshot_.low_delay_keyframe_scale;
    info["strict_gop_target"] = encoder_snapshot_.strict_gop_target;
    info["enable_non_ref_p"] = encoder_snapshot_.enable_non_ref_p;
    info["repeat_sps_pps"] = encoder_snapshot_.repeat_sps_pps;
    info["enable_ptd"] = encoder_snapshot_.enable_ptd;
    return info;
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
    if (writer_.metadata) {
        if (writer_.metadata->is_open()) {
            writer_.metadata->close();
        }
        delete writer_.metadata;
        writer_.metadata = nullptr;
    }
}

bool EncoderHwWorker::WorkerFunction(ENCODER_WORKER_ENTRY* entry)
{
    const bool recording_enabled = camera_control_->record_video;
    const bool draining = camera_control_->recording_draining;

    auto recycle_entry = [&]() {
        if (m_prep_worker_) {
            if (entry->preprocess_complete_event) {
                m_prep_worker_->free_events_.push(entry->preprocess_complete_event);
                m_prep_worker_->available_events_++;
            }
            m_prep_worker_->free_encoder_entries_.push(entry);
            m_prep_worker_->available_buffers_++;
        }
    };

    if (!entry) {
        if (!recording_enabled && is_recording_) {
            if (!draining || drain_ready()) {
                std::cout << "[" << this->threadName << "] HW Recording stopped. Finalizing video file..." << std::endl;
                finalize_recording();
            }
        }
        return false;
    }

    // If the global flag is true but we aren't recording yet, start a new recording.
    if (recording_enabled && !is_recording_) {
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
        {
            orange::ScopedFsuid fsuid_guard;
            (void)fsuid_guard;
            make_folder(current_recording_folder);

            const auto metadata_tags = build_metadata_tags(
                camera_params_,
                recording_output_config_,
                codec_,
                preset_,
                tuning_,
                rate_control_mode_,
                quality_value_
            );
            initialize_writer_hw(
                &writer_,
                camera_params_,
                recording_output_config_,
                current_recording_folder,
                codec_,
                metadata_tags);
        }
        if (encoder_snapshot_valid_) {
            const std::string camera_key = camera_params_->camera_serial.empty()
                ? std::to_string(camera_params_->camera_id)
                : camera_params_->camera_serial;
            nlohmann::json encoder_info = build_encoder_snapshot_json();
            update_recording_snapshot_encoder(current_recording_folder, camera_key, encoder_info);
        }
        camera_control_->active_recorders.fetch_add(1, std::memory_order_relaxed);
        is_recording_ = true;
    }

    // If recording is globally disabled, skip processing but recycle resources.
    if (!recording_enabled && !draining) {
        recycle_entry();
        return false;
    }

    if (!is_recording_) {
        std::cerr << "[" << this->threadName << "] Warning: Dropping frame because encoder is not recording." << std::endl;
        recycle_entry();
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
