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
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <string>

namespace {
constexpr uint64_t kMinQualityBitrate = 10000000ULL;
constexpr uint64_t kMaxQualityBitrate = 150000000ULL;
constexpr double kColorTargetBpp = 0.15;
constexpr double kMonoTargetBpp = 0.10;
constexpr int kDefaultQualityValue = 20;
constexpr int kMinQualityValue = 1;
constexpr int kMaxQualityValue = 51;
constexpr size_t kPreEncoderReferenceCaptureRingSize = 3;

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

bool parse_env_flag(const char* name, bool default_value) {
    const char* env = std::getenv(name);
    if (!env || *env == '\0') {
        return default_value;
    }
    if (strcmp(env, "0") == 0 || strcmp(env, "false") == 0 || strcmp(env, "FALSE") == 0 ||
        strcmp(env, "off") == 0 || strcmp(env, "OFF") == 0) {
        return false;
    }
    return true;
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
    int quality_value,
    int gop_length
) {
    std::vector<std::pair<std::string, std::string>> tags;
    tags.emplace_back("title", "Cam" + camera_params->camera_serial);

    std::ostringstream comment;
    const std::string rc_strategy = resolve_rate_control_strategy(tuning, rate_control_mode);
    const uint32_t resolved_gop_length = resolve_recording_gop_length(*camera_params, tuning, gop_length);
    comment << "nvenc codec=" << codec
            << "; preset=" << preset
            << "; tuning=" << tuning
            << "; res=" << recording_output_config.resolved_width << "x" << recording_output_config.resolved_height
            << "; fps=" << camera_params->frame_rate
            << "; color=" << (camera_params->color ? 1 : 0)
            << "; gop=" << resolved_gop_length
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
    const RecordingBitrateEstimate estimate =
        estimate_recording_bitrate(*camera_params, recording_output_config);
    encodeConfig.rcParams.rateControlMode = NV_ENC_PARAMS_RC_VBR;
    encodeConfig.rcParams.averageBitRate = estimate.average_bitrate;
    encodeConfig.rcParams.maxBitRate = estimate.max_bitrate;
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

RecordingBitrateEstimate estimate_recording_bitrate(const CameraParams& camera_params,
                                                    const RecordingOutputConfig& recording_output_config)
{
    RecordingBitrateEstimate estimate;
    estimate.target_bpp = camera_params.color ? kColorTargetBpp : kMonoTargetBpp;

    const double unclamped_bits_per_sec =
        static_cast<double>(recording_output_config.resolved_width) *
        static_cast<double>(recording_output_config.resolved_height) *
        static_cast<double>(camera_params.frame_rate) *
        estimate.target_bpp;
    const uint64_t unclamped_average_bitrate = static_cast<uint64_t>(unclamped_bits_per_sec);
    estimate.average_clamped_to_min = unclamped_average_bitrate < kMinQualityBitrate;
    estimate.average_clamped_to_max = unclamped_average_bitrate > kMaxQualityBitrate;
    estimate.average_bitrate = clamp_bitrate(unclamped_average_bitrate);

    uint64_t unclamped_max_bitrate = static_cast<uint64_t>(estimate.average_bitrate) * 3 / 2;
    if (unclamped_max_bitrate < estimate.average_bitrate) {
        unclamped_max_bitrate = estimate.average_bitrate;
    }
    estimate.max_clamped_to_max = unclamped_max_bitrate > kMaxQualityBitrate;
    estimate.max_bitrate = clamp_bitrate(unclamped_max_bitrate);
    return estimate;
}

int sanitize_recording_gop_length(int requested_gop_length)
{
    return std::max(0, requested_gop_length);
}

uint32_t resolve_recording_gop_length(const CameraParams& camera_params,
                                      const std::string& tuning,
                                      int requested_gop_length)
{
    if (is_lossless_tuning(tuning)) {
        return 1;
    }

    const int sanitized_gop_length = sanitize_recording_gop_length(requested_gop_length);
    if (sanitized_gop_length > 0) {
        return static_cast<uint32_t>(sanitized_gop_length);
    }

    return std::max<uint32_t>(1u, camera_params.frame_rate);
}

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
    int gop_length,
    std::string base_folder_name,
    EncoderPreprocessWorker* prep_worker,
    CameraControl* camera_control,
    const PreEncoderReferenceCaptureConfig& pre_encoder_reference_capture_config
)
: CThreadWorker(name),
  camera_params_(camera_params),
  recording_output_config_(recording_output_config),
  base_folder_name_(base_folder_name),
  codec_(codec),
  preset_(preset),
  tuning_(tuning),
  rate_control_mode_(rate_control_mode.empty() ? "vbr" : rate_control_mode),
  pre_encoder_reference_capture_config_(pre_encoder_reference_capture_config),
  direct_input_enabled_(parse_env_flag("ORANGE_NVENC_DIRECT_INPUT", false)),
  quality_value_(clamp_quality_value(quality_value)),
  gop_length_(sanitize_recording_gop_length(gop_length)),
  m_prep_worker_(prep_worker),
  camera_control_(camera_control),
  encoder_(),
  m_stream(nullptr),
  last_recording_frame_id_(0),
  last_fps_update_time_(std::chrono::steady_clock::now()),
  frame_counter_(0),
  is_recording_(false) // Initialize recording state
{
    if (!pre_encoder_reference_capture_config_.has_valid_bound()) {
        throw std::invalid_argument(
            "pre_encoder_reference_capture requires exactly one positive bound: max_frames or max_seconds");
    }
    pre_encoder_reference_writer_.Configure(pre_encoder_reference_capture_config_);
    pre_encoder_reference_async_enabled_ =
        pre_encoder_reference_capture_config_.enabled && !direct_input_enabled_;

    ck(cudaSetDevice(camera_params_->gpu_id));
    ck(cudaStreamCreate(&m_stream));
    if (pre_encoder_reference_capture_config_.enabled) {
        ck(cudaStreamCreate(&pre_encoder_reference_stream_));
    }
    ck(cuCtxGetCurrent(&encoder_.cuContext));
    encoder_.pEnc = new NvEncoderCuda(
        encoder_.cuContext,
        recording_output_config_.resolved_width,
        recording_output_config_.resolved_height,
        NV_ENC_BUFFER_FORMAT_NV12);
    if (direct_input_enabled_) {
        encoder_.pEnc->SetExternalInputBufferMode(true);
    }

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

    encodeConfig.gopLength = resolve_recording_gop_length(*camera_params_, tuning_, gop_length_);
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
    encoder_buffer_count_ = static_cast<int>(encoder_.pEnc->GetEncoderBufferCount());
    if (!direct_input_enabled_) {
        const NvEncInputFrame* encoder_input_frame = encoder_.pEnc->GetNextInputFrame();
        if (!encoder_input_frame) {
            throw std::runtime_error("Failed to get NVENC input frame while initializing hardware worker");
        }
        encoder_input_pitch_ = static_cast<int>(encoder_input_frame->pitch);
    } else {
        std::cout << "[EncoderHwWorker] Direct NVENC input enabled via ORANGE_NVENC_DIRECT_INPUT=1"
                  << " (ring slots: " << encoder_buffer_count_ << ")" << std::endl;
    }

    {
        NV_ENC_INITIALIZE_PARAMS resolved_params = { NV_ENC_INITIALIZE_PARAMS_VER };
        NV_ENC_CONFIG resolved_config = { NV_ENC_CONFIG_VER };
        resolved_params.encodeConfig = &resolved_config;
        encoder_.pEnc->GetInitializeParams(&resolved_params);

        encoder_snapshot_.backend = "nvenc";
        encoder_snapshot_.path = direct_input_enabled_ ? "hw_direct_input" : "hw";
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
        encoder_snapshot_.gpu = build_gpu_runtime_info(camera_params_->gpu_id);
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
    poll_pre_encoder_reference_captures(true);
    finalize_pre_encoder_reference_capture();
    release_pre_encoder_reference_capture_resources();
    if (encoder_.pEnc) {
        delete encoder_.pEnc;
        encoder_.pEnc = nullptr;
    }
    if (m_stream) {
        cudaStreamDestroy(m_stream);
        m_stream = nullptr;
    }
    if (pre_encoder_reference_stream_) {
        cudaStreamDestroy(pre_encoder_reference_stream_);
        pre_encoder_reference_stream_ = nullptr;
    }
}

void EncoderHwWorker::SetPreprocessWorker(EncoderPreprocessWorker* prep_worker)
{
    m_prep_worker_ = prep_worker;
    if (!direct_input_enabled_ || direct_input_registered_ || !m_prep_worker_ || !encoder_.pEnc) {
        return;
    }

    encoder_.pEnc->RegisterExternalCudaInputBuffers(
        m_prep_worker_->direct_input_surfaces(),
        static_cast<uint32_t>(m_prep_worker_->direct_input_pitch()));
    direct_input_registered_ = true;
    encoder_input_pitch_ = m_prep_worker_->direct_input_pitch();
}

void EncoderHwWorker::initialize_pre_encoder_reference_capture()
{
    if (!pre_encoder_reference_capture_config_.enabled) {
        return;
    }

    const size_t pitch = static_cast<size_t>(encoder_input_pitch_);
    const size_t frame_size =
        pitch * static_cast<size_t>(recording_output_config_.resolved_height) * 3 / 2;

    if (pitch == 0 || frame_size == 0) {
        pre_encoder_reference_writer_.SetError(
            "Pre-encoder reference capture could not resolve a valid NV12 surface pitch");
        return;
    }

    std::string staging_error;
    if (!ensure_pre_encoder_reference_staging_slots(frame_size, &staging_error)) {
        pre_encoder_reference_writer_.SetError(staging_error);
        return;
    }

    PreEncoderReferenceWriter::OpenParams params;
    params.camera_serial = camera_params_->camera_serial;
    params.output_dir = pre_encoder_reference_capture_config_.output_dir.empty()
        ? active_recording_folder_
        : pre_encoder_reference_capture_config_.output_dir;
    params.path_type = direct_input_enabled_ ? "direct_input" : "copy";
    params.source_path_flavor = camera_params_->color ? "color" : "mono";
    params.resize_enabled = recording_output_config_.resize_enabled;
    params.width = recording_output_config_.resolved_width;
    params.height = recording_output_config_.resolved_height;
    params.pitch = pitch;
    params.frame_size = frame_size;
    params.encoder_snapshot = build_encoder_snapshot_json();
    params.encoder_snapshot.erase("pre_encoder_reference_capture");

    std::string open_error;
    if (!pre_encoder_reference_writer_.Open(params, &open_error)) {
        std::cerr << "[" << threadName << "] Warning: failed to open pre-encoder reference capture for camera "
                  << camera_params_->camera_serial << ": " << open_error << std::endl;
    }
}

void EncoderHwWorker::finalize_pre_encoder_reference_capture()
{
    poll_pre_encoder_reference_captures(true);
    pre_encoder_reference_writer_.Close();
}

bool EncoderHwWorker::begin_pre_encoder_reference_capture(ENCODER_WORKER_ENTRY* entry,
                                                         size_t* staging_slot_out,
                                                         size_t* frame_size_out)
{
    if (!entry || !pre_encoder_reference_writer_.ShouldCaptureNextFrame()) {
        return false;
    }

    const size_t frame_pitch = entry->surface_pitch;
    const size_t frame_height =
        static_cast<size_t>(recording_output_config_.resolved_height) * 3 / 2;
    const size_t frame_size = frame_pitch * frame_height;
    if (frame_size == 0) {
        pre_encoder_reference_writer_.SetError("Pre-encoder reference frame size resolved to zero");
        return false;
    }

    std::string staging_error;
    if (!ensure_pre_encoder_reference_staging_slots(frame_size, &staging_error)) {
        pre_encoder_reference_writer_.SetError(staging_error);
        return false;
    }

    auto available_slot = pre_encoder_reference_staging_slots_.end();
    for (auto it = pre_encoder_reference_staging_slots_.begin();
         it != pre_encoder_reference_staging_slots_.end();
         ++it) {
        if (!it->in_use) {
            available_slot = it;
            break;
        }
    }

    if (available_slot == pre_encoder_reference_staging_slots_.end()) {
        if (pre_encoder_reference_async_enabled_) {
            poll_pre_encoder_reference_captures(true);
            for (auto it = pre_encoder_reference_staging_slots_.begin();
                 it != pre_encoder_reference_staging_slots_.end();
                 ++it) {
                if (!it->in_use) {
                    available_slot = it;
                    break;
                }
            }
        }
        if (available_slot == pre_encoder_reference_staging_slots_.end()) {
            pre_encoder_reference_writer_.SetError(
                "Pre-encoder reference capture staging ring exhausted before copy completion");
            return false;
        }
    }

    const size_t staging_slot = static_cast<size_t>(
        std::distance(pre_encoder_reference_staging_slots_.begin(), available_slot));
    available_slot->in_use = true;
    cudaStream_t capture_stream = pre_encoder_reference_async_enabled_
        ? pre_encoder_reference_stream_
        : m_stream;
    if (pre_encoder_reference_async_enabled_ && entry->preprocess_complete_event) {
        ck(cudaStreamWaitEvent(capture_stream, *entry->preprocess_complete_event, 0));
    }
    cudaError_t copy_status = cudaMemcpy2DAsync(
        available_slot->host_buffer,
        frame_pitch,
        entry->d_prepared_frame,
        frame_pitch,
        frame_pitch,
        frame_height,
        cudaMemcpyDeviceToHost,
        capture_stream);
    if (copy_status != cudaSuccess) {
        available_slot->in_use = false;
        pre_encoder_reference_writer_.SetError(
            std::string("cudaMemcpy2DAsync failed during pre-encoder reference capture: ") +
            cudaGetErrorString(copy_status));
        return false;
    }

    if (pre_encoder_reference_async_enabled_) {
        ck(cudaEventRecord(available_slot->copy_complete_event, pre_encoder_reference_stream_));
        if (staging_slot_out) {
            *staging_slot_out = staging_slot;
        }
        if (frame_size_out) {
            *frame_size_out = frame_size;
        }
        return true;
    }

    cudaError_t sync_status = cudaStreamSynchronize(m_stream);
    if (sync_status != cudaSuccess) {
        available_slot->in_use = false;
        pre_encoder_reference_writer_.SetError(
            std::string("cudaStreamSynchronize failed during pre-encoder reference capture: ") +
            cudaGetErrorString(sync_status));
        return false;
    }

    std::string append_error;
    if (!pre_encoder_reference_writer_.AppendFrame(
            available_slot->host_buffer,
            frame_size,
            entry->recording_frame_id,
            entry->timestamp,
            entry->timestamp_sys,
            &append_error)) {
        available_slot->in_use = false;
        if (!append_error.empty()) {
            std::cerr << "[" << threadName << "] Warning: failed to append pre-encoder reference frame for camera "
                      << camera_params_->camera_serial << ": " << append_error << std::endl;
        }
        return false;
    }
    available_slot->in_use = false;
    return true;
}

void EncoderHwWorker::poll_pre_encoder_reference_captures(bool wait_for_all)
{
    while (!pending_pre_encoder_reference_captures_.empty()) {
        PendingReferenceCapture& pending = pending_pre_encoder_reference_captures_.front();
        ReferenceCaptureStagingSlot& slot = pre_encoder_reference_staging_slots_[pending.staging_slot];
        cudaError_t event_status = wait_for_all
            ? cudaEventSynchronize(slot.copy_complete_event)
            : cudaEventQuery(slot.copy_complete_event);
        if (!wait_for_all && event_status == cudaErrorNotReady) {
            break;
        }
        if (event_status != cudaSuccess) {
            pre_encoder_reference_writer_.SetError(
                std::string("Pre-encoder reference capture completion failed: ") +
                cudaGetErrorString(event_status));
        } else {
            std::string append_error;
            if (!pre_encoder_reference_writer_.AppendFrame(
                    slot.host_buffer,
                    pending.frame_size,
                    pending.entry->recording_frame_id,
                    pending.entry->timestamp,
                    pending.entry->timestamp_sys,
                    &append_error) &&
                !append_error.empty()) {
                std::cerr << "[" << threadName
                          << "] Warning: failed to append async pre-encoder reference frame for camera "
                          << camera_params_->camera_serial << ": " << append_error << std::endl;
            }
        }

        slot.in_use = false;
        recycle_encoder_entry(pending.entry, pending.retired_slots);
        pending_pre_encoder_reference_captures_.pop_front();
    }
}

void EncoderHwWorker::recycle_encoder_entry(ENCODER_WORKER_ENTRY* entry,
                                           const std::vector<uint32_t>& retired_slots)
{
    if (!m_prep_worker_ || !entry) {
        return;
    }

    if (entry->preprocess_complete_event) {
        m_prep_worker_->free_events_.push(entry->preprocess_complete_event);
        m_prep_worker_->available_events_++;
    }
    m_prep_worker_->free_encoder_entries_.push(entry);
    if (!direct_input_enabled_) {
        m_prep_worker_->available_buffers_++;
        return;
    }

    if (!retired_slots.empty()) {
        for (uint32_t slot_id : retired_slots) {
            m_prep_worker_->free_direct_input_slots_.push(static_cast<int>(slot_id));
            m_prep_worker_->available_buffers_++;
        }
        return;
    }

    if (entry->slot_id >= 0) {
        m_prep_worker_->free_direct_input_slots_.push(entry->slot_id);
        m_prep_worker_->available_buffers_++;
        entry->slot_id = -1;
    }
}

void EncoderHwWorker::release_pre_encoder_reference_capture_resources()
{
    for (auto& slot : pre_encoder_reference_staging_slots_) {
        if (slot.copy_complete_event) {
            cudaEventDestroy(slot.copy_complete_event);
            slot.copy_complete_event = nullptr;
        }
        if (slot.host_buffer) {
            cudaFreeHost(slot.host_buffer);
            slot.host_buffer = nullptr;
        }
        slot.buffer_size = 0;
        slot.in_use = false;
    }
    pre_encoder_reference_staging_slots_.clear();
    pending_pre_encoder_reference_captures_.clear();
}

bool EncoderHwWorker::ensure_pre_encoder_reference_staging_slots(size_t frame_size, std::string* error_out)
{
    if (frame_size == 0) {
        if (error_out) {
            *error_out = "Pre-encoder reference staging buffer size resolved to zero";
        }
        return false;
    }

    if (!pre_encoder_reference_staging_slots_.empty()) {
        bool sizes_match = true;
        for (const auto& slot : pre_encoder_reference_staging_slots_) {
            if (slot.buffer_size != frame_size) {
                sizes_match = false;
                break;
            }
        }
        if (sizes_match) {
            return true;
        }
    }

    if (!pending_pre_encoder_reference_captures_.empty()) {
        if (error_out) {
            *error_out =
                "Cannot resize pre-encoder reference capture staging slots while captures are pending";
        }
        return false;
    }

    release_pre_encoder_reference_capture_resources();

    pre_encoder_reference_staging_slots_.resize(kPreEncoderReferenceCaptureRingSize);
    for (auto& slot : pre_encoder_reference_staging_slots_) {
        cudaError_t alloc_status =
            cudaMallocHost(reinterpret_cast<void**>(&slot.host_buffer), frame_size);
        if (alloc_status != cudaSuccess) {
            if (error_out) {
                *error_out =
                    std::string("Failed to allocate pre-encoder reference staging buffer: ") +
                    cudaGetErrorString(alloc_status);
            }
            release_pre_encoder_reference_capture_resources();
            return false;
        }
        cudaError_t event_status = cudaEventCreateWithFlags(&slot.copy_complete_event, cudaEventDisableTiming);
        if (event_status != cudaSuccess) {
            if (error_out) {
                *error_out =
                    std::string("Failed to allocate pre-encoder reference staging event: ") +
                    cudaGetErrorString(event_status);
            }
            release_pre_encoder_reference_capture_resources();
            return false;
        }
        slot.buffer_size = frame_size;
        slot.in_use = false;
    }

    return true;
}

void EncoderHwWorker::finalize_recording()
{
    if (!is_recording_) {
        return;
    }

    flush_and_close();
    if (encoder_snapshot_valid_ && !active_recording_folder_.empty()) {
        const std::string camera_key = camera_params_->camera_serial.empty()
            ? std::to_string(camera_params_->camera_id)
            : camera_params_->camera_serial;
        nlohmann::json encoder_info = build_encoder_snapshot_json();
        update_recording_snapshot_encoder(active_recording_folder_, camera_key, encoder_info);
    }
    active_recording_folder_.clear();
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
    poll_pre_encoder_reference_captures(false);
    if (GetCountQueueInSize() > 0) {
        return false;
    }
    if (m_prep_worker_ && !m_prep_worker_->IsDrained()) {
        return false;
    }
    if (!pending_pre_encoder_reference_captures_.empty()) {
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
    info["gpu"] = encoder_snapshot_.gpu;
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
    info["pre_encoder_reference_capture"] = pre_encoder_reference_writer_.BuildSummaryJson();
    return info;
}

void EncoderHwWorker::flush_and_close()
{
    if (encoder_.pEnc) {
        std::vector<uint32_t> retired_slots;
        encoder_.pEnc->EndEncode(encoder_.vPacket, direct_input_enabled_ ? &retired_slots : nullptr);
        for (auto &packet : encoder_.vPacket)
        {
            if (writer_.video) {
                writer_.video->push_packet(packet.data(), (int)packet.size(), ++last_recording_frame_id_);
            }
        }
        encoder_.vPacket.clear();
        if (direct_input_enabled_ && m_prep_worker_) {
            for (uint32_t slot_id : retired_slots) {
                m_prep_worker_->free_direct_input_slots_.push(static_cast<int>(slot_id));
                m_prep_worker_->available_buffers_++;
            }
        }
    }
    finalize_pre_encoder_reference_capture();
    release_pre_encoder_reference_capture_resources();

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
    poll_pre_encoder_reference_captures(false);

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
        active_recording_folder_ = current_recording_folder;
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
                quality_value_,
                gop_length_
            );
            initialize_writer_hw(
                &writer_,
                camera_params_,
                recording_output_config_,
                current_recording_folder,
                codec_,
                metadata_tags);
        }
        initialize_pre_encoder_reference_capture();
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
        recycle_encoder_entry(entry, {});
        return false;
    }

    if (!is_recording_) {
        std::cerr << "[" << this->threadName << "] Warning: Dropping frame because encoder is not recording." << std::endl;
        recycle_encoder_entry(entry, {});
        return false;
    }

    auto start_time = std::chrono::steady_clock::now();
    frame_counter_++;
    auto now = std::chrono::steady_clock::now();
    std::chrono::duration<double> elapsed = now - last_fps_update_time_;
    if (elapsed.count() >= 1.0) {
        current_fps_.store(frame_counter_ / elapsed.count(), std::memory_order_relaxed);
        frame_counter_ = 0;
        last_fps_update_time_ = now;
    }

    ck(cudaSetDevice(camera_params_->gpu_id));
    std::vector<uint32_t> retired_slots;
    bool capture_scheduled = false;
    size_t capture_staging_slot = 0;
    size_t capture_frame_size = 0;

    try {
        if (entry->preprocess_complete_event) {
            ck(cudaStreamWaitEvent(m_stream, *entry->preprocess_complete_event, 0));
        }
        capture_scheduled = begin_pre_encoder_reference_capture(
            entry, &capture_staging_slot, &capture_frame_size);

        if (direct_input_enabled_) {
            if (!direct_input_registered_) {
                throw std::runtime_error("Direct NVENC input pool is not registered");
            }
            const uint32_t expected_slot = encoder_.pEnc->GetNextInputFrameIndex();
            if (entry->slot_id < 0 || static_cast<uint32_t>(entry->slot_id) != expected_slot) {
                std::ostringstream error;
                error << "Direct-input slot mismatch: expected " << expected_slot
                      << " but got " << entry->slot_id;
                throw std::runtime_error(error.str());
            }
            encoder_.pEnc->EncodeFrame(encoder_.vPacket, nullptr, &retired_slots);
        } else {
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
                static_cast<uint32_t>(entry->surface_pitch),
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
        }

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

    if (capture_scheduled && pre_encoder_reference_async_enabled_) {
        pending_pre_encoder_reference_captures_.push_back(
            PendingReferenceCapture{
                entry,
                capture_staging_slot,
                capture_frame_size,
                retired_slots});
    } else {
        recycle_encoder_entry(entry, retired_slots);
    }

    auto end_time = std::chrono::steady_clock::now();
    auto duration_us = std::chrono::duration_cast<std::chrono::microseconds>(end_time - start_time).count();
    
    if (duration_us > 12500) {
        slow_frames_++;
    }
    
    return false;
}
