// src/encoder_hw_worker.cpp

#include "encoder_hw_worker.h"
#include "encoder_preprocess_worker.h"
#include "fsuid_guard.h"
#include "shared_recording_output.h"
#include <iostream>
#include <utility>
#include <vector>
#include "global.h"
#include "NvEncoder/NvEncoder.h"
#include "cuda_context_debug.h"
#include "nvtx_profiling.h"
#include "project.h"
#include <sstream>
#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <string>
#include <thread>

namespace {
constexpr size_t kPreEncoderReferenceCaptureRingSize = 3;
constexpr int8_t kStaticRoiInsideDelta = -3;
constexpr int8_t kStaticRoiOutsideDelta = 3;
constexpr int kDefaultNvencExtraOutputDelay = 3;
constexpr int kMinNvencExtraOutputDelay = 1;
constexpr int kMaxNvencExtraOutputDelay = 64;
constexpr int kDefaultNvencHarvestDelayUs = 0;
constexpr int kMinNvencHarvestDelayUs = 0;
constexpr int kMaxNvencHarvestDelayUs = 20000;

bool env_flag_enabled(const char* name)
{
    const char* value = std::getenv(name);
    if (!value || !*value) {
        return false;
    }
    return std::strcmp(value, "0") != 0 &&
           std::strcmp(value, "false") != 0 &&
           std::strcmp(value, "FALSE") != 0 &&
           std::strcmp(value, "off") != 0 &&
           std::strcmp(value, "OFF") != 0;
}

bool guid_equals(const GUID& lhs, const GUID& rhs) {
    return std::memcmp(&lhs, &rhs, sizeof(GUID)) == 0;
}

nlohmann::json latency_stats_to_json(const LatencyAggregateStats& stats) {
    return {
        {"samples", stats.sample_count},
        {"mean_ms", stats.mean_ms()},
        {"max_ms", stats.max_ms()},
        {"last_ms", stats.last_ms()}
    };
}

bool has_recording_output_timing(const RecordingOutputTimingSample& timing)
{
    return timing.encoder_cuda_set_device_ns > 0 ||
           timing.preprocess_complete_stream_wait_enqueue_ns > 0 ||
           timing.source_to_helper_copy_sync_wait_ns > 0 ||
           timing.source_to_helper_copy_elapsed_query_ns > 0 ||
           timing.has_source_to_helper_copy ||
           timing.pre_encoder_reference_capture_enqueue_ns > 0 ||
           timing.nvenc_get_next_input_frame_ns > 0 ||
           timing.has_bitstream_fetch ||
           timing.nvenc_copy_to_input_ns > 0 ||
           timing.nvenc_encode_frame_total_ns > 0 ||
           timing.nvenc_map_input_resource_ns > 0 ||
           timing.nvenc_map_reference_resource_ns > 0 ||
           timing.nvenc_encode_picture_ns > 0 ||
           timing.nvenc_completion_wait_ns > 0 ||
           timing.nvenc_lock_bitstream_ns > 0 ||
           timing.nvenc_bitstream_copy_ns > 0 ||
           timing.nvenc_unlock_bitstream_ns > 0 ||
           timing.nvenc_unmap_input_resource_ns > 0 ||
           timing.nvenc_unmap_reference_resource_ns > 0 ||
           timing.encoder_output_accounting_ns > 0;
}

int parse_nvenc_extra_output_delay_env(const char* name,
                                       int default_value,
                                       bool* used_override)
{
    const char* env = std::getenv(name);
    if (!env || !*env) {
        return default_value;
    }
    char* end = nullptr;
    const long parsed = std::strtol(env, &end, 10);
    if (end == env || *end != '\0') {
        std::cerr << "[EncoderHwWorker] Ignoring invalid " << name
                  << "='" << env << "', using " << default_value << std::endl;
        return default_value;
    }
    if (parsed < kMinNvencExtraOutputDelay || parsed > kMaxNvencExtraOutputDelay) {
        std::cerr << "[EncoderHwWorker] " << name << " must be within ["
                  << kMinNvencExtraOutputDelay << "," << kMaxNvencExtraOutputDelay
                  << "], using " << default_value << std::endl;
        return default_value;
    }
    if (used_override) {
        *used_override = true;
    }
    return static_cast<int>(parsed);
}

int resolve_nvenc_extra_output_delay(const CameraParams* camera_params)
{
    bool used_global = false;
    int delay = parse_nvenc_extra_output_delay_env(
        "ORANGE_NVENC_EXTRA_OUTPUT_DELAY",
        kDefaultNvencExtraOutputDelay,
        &used_global);

    if (camera_params && !camera_params->camera_serial.empty()) {
        const std::string env_key =
            "ORANGE_NVENC_EXTRA_OUTPUT_DELAY_CAM_" + camera_params->camera_serial;
        bool used_camera = false;
        delay = parse_nvenc_extra_output_delay_env(
            env_key.c_str(),
            delay,
            &used_camera);
        if (used_camera) {
            std::cout << "[EncoderHwWorker] " << env_key << "=" << delay
                      << " for full-frame NVENC output-depth diagnostic"
                      << std::endl;
            return delay;
        }
    }

    if (used_global) {
        std::cout << "[EncoderHwWorker] ORANGE_NVENC_EXTRA_OUTPUT_DELAY=" << delay
                  << " for full-frame NVENC output-depth diagnostic"
                  << std::endl;
    }
    return delay;
}

int parse_nvenc_harvest_delay_us_env(const char* name,
                                     int default_value,
                                     bool* used_override)
{
    const char* env = std::getenv(name);
    if (!env || !*env) {
        return default_value;
    }
    char* end = nullptr;
    const long parsed = std::strtol(env, &end, 10);
    if (end == env || *end != '\0') {
        std::cerr << "[EncoderHwWorker] Ignoring invalid " << name
                  << "='" << env << "', using " << default_value << std::endl;
        return default_value;
    }
    if (parsed < kMinNvencHarvestDelayUs || parsed > kMaxNvencHarvestDelayUs) {
        std::cerr << "[EncoderHwWorker] " << name << " must be within ["
                  << kMinNvencHarvestDelayUs << "," << kMaxNvencHarvestDelayUs
                  << "], using " << default_value << std::endl;
        return default_value;
    }
    if (used_override) {
        *used_override = true;
    }
    return static_cast<int>(parsed);
}

int resolve_nvenc_harvest_delay_us(const CameraParams* camera_params)
{
    bool used_global = false;
    int delay_us = parse_nvenc_harvest_delay_us_env(
        "ORANGE_NVENC_HARVEST_DELAY_US",
        kDefaultNvencHarvestDelayUs,
        &used_global);

    if (camera_params && !camera_params->camera_serial.empty()) {
        const std::string env_key =
            "ORANGE_NVENC_HARVEST_DELAY_US_CAM_" + camera_params->camera_serial;
        bool used_camera = false;
        delay_us = parse_nvenc_harvest_delay_us_env(
            env_key.c_str(),
            delay_us,
            &used_camera);
        if (used_camera) {
            std::cout << "[EncoderHwWorker] " << env_key << "=" << delay_us
                      << " for split-harvest launch-window diagnostic"
                      << std::endl;
            return delay_us;
        }
    }

    if (used_global) {
        std::cout << "[EncoderHwWorker] ORANGE_NVENC_HARVEST_DELAY_US="
                  << delay_us
                  << " for split-harvest launch-window diagnostic"
                  << std::endl;
    }
    return delay_us;
}

void merge_nvenc_timing(RecordingOutputTimingSample* sample,
                        const NvEncoderEncodeFrameTiming& timing)
{
    if (!sample) {
        return;
    }
    sample->nvenc_map_input_resource_ns += timing.map_input_resource_ns;
    sample->nvenc_map_reference_resource_ns += timing.map_reference_resource_ns;
    sample->nvenc_encode_picture_ns += timing.encode_picture_ns;
    sample->nvenc_completion_wait_ns += timing.completion_wait_ns;
    sample->nvenc_lock_bitstream_ns += timing.lock_bitstream_ns;
    sample->nvenc_bitstream_copy_ns += timing.bitstream_copy_ns;
    sample->nvenc_unlock_bitstream_ns += timing.unlock_bitstream_ns;
    sample->nvenc_unmap_input_resource_ns += timing.unmap_input_resource_ns;
    sample->nvenc_unmap_reference_resource_ns += timing.unmap_reference_resource_ns;
}

std::string normalize_importance_map_mode(std::string mode) {
    std::transform(mode.begin(), mode.end(), mode.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    if (mode.empty() || mode == "off" || mode == "none" || mode == "disabled") {
        return "off";
    }
    if (mode == "static_roi" || mode == "static-roi" ||
        mode == "static_prior" || mode == "static-prior" || mode == "static") {
        return "static_roi";
    }
    return mode;
}

int normalize_importance_map_roi_size_px(int value) {
    return value > 0 ? value : ImportanceMapConfig::kDefaultRoiSizePx;
}

bool importance_map_mode_enabled(const std::string& mode) {
    return normalize_importance_map_mode(mode) != "off";
}

std::string qp_map_mode_to_string(NV_ENC_QP_MAP_MODE mode) {
    switch (mode) {
        case NV_ENC_QP_MAP_DISABLED:
            return "disabled";
        case NV_ENC_QP_MAP_EMPHASIS:
            return "emphasis";
        case NV_ENC_QP_MAP_DELTA:
            return "delta";
        case NV_ENC_QP_MAP:
            return "absolute_qp";
        default:
            return "unknown";
    }
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

const char* tuning_info_to_string(NV_ENC_TUNING_INFO tuning_info) {
    switch (tuning_info) {
        case NV_ENC_TUNING_INFO_HIGH_QUALITY: return "high_quality";
        case NV_ENC_TUNING_INFO_LOW_LATENCY: return "low_latency";
        case NV_ENC_TUNING_INFO_ULTRA_LOW_LATENCY: return "ultra_low_latency";
        case NV_ENC_TUNING_INFO_LOSSLESS: return "lossless";
        case NV_ENC_TUNING_INFO_UNDEFINED: return "undefined";
        default: return "unknown";
    }
}

const char* multi_pass_to_string(NV_ENC_MULTI_PASS multi_pass) {
    switch (multi_pass) {
        case NV_ENC_MULTI_PASS_DISABLED: return "disabled";
        case NV_ENC_TWO_PASS_QUARTER_RESOLUTION: return "quarter_resolution";
        case NV_ENC_TWO_PASS_FULL_RESOLUTION: return "full_resolution";
        default: return "unknown";
    }
}

nlohmann::json build_resolved_encoder_config_json(const NV_ENC_INITIALIZE_PARAMS& initialize_params,
                                                  const NV_ENC_CONFIG& encode_config,
                                                  int encoder_buffer_count,
                                                  int encoder_input_pitch,
                                                  bool direct_input_enabled,
                                                  int nvenc_extra_output_delay)
{
    nlohmann::json info = nlohmann::json::object();

    info["initialize"] = {
        {"encode_width", initialize_params.encodeWidth},
        {"encode_height", initialize_params.encodeHeight},
        {"dar_width", initialize_params.darWidth},
        {"dar_height", initialize_params.darHeight},
        {"frame_rate_num", initialize_params.frameRateNum},
        {"frame_rate_den", initialize_params.frameRateDen},
        {"enable_ptd", initialize_params.enablePTD},
        {"enable_sub_frame_write", initialize_params.enableSubFrameWrite},
        {"enable_weighted_prediction", initialize_params.enableWeightedPrediction},
        {"enable_output_in_vidmem", initialize_params.enableOutputInVidmem},
        {"max_encode_width", initialize_params.maxEncodeWidth},
        {"max_encode_height", initialize_params.maxEncodeHeight},
        {"tuning_info", {
            {"value", initialize_params.tuningInfo},
            {"name", tuning_info_to_string(static_cast<NV_ENC_TUNING_INFO>(initialize_params.tuningInfo))}
        }}
    };

    info["buffers"] = {
        {"encoder_buffer_count", encoder_buffer_count},
        {"encoder_input_pitch", encoder_input_pitch},
        {"direct_input_enabled", direct_input_enabled},
        {"nvenc_extra_output_delay", nvenc_extra_output_delay}
    };

    info["common"] = {
        {"gop_length", encode_config.gopLength},
        {"frame_interval_p", encode_config.frameIntervalP},
        {"mono_chrome_encoding", encode_config.monoChromeEncoding}
    };

    info["rc"] = {
        {"mode", rc_mode_to_string(static_cast<NV_ENC_PARAMS_RC_MODE>(encode_config.rcParams.rateControlMode))},
        {"mode_value", encode_config.rcParams.rateControlMode},
        {"average_bitrate", encode_config.rcParams.averageBitRate},
        {"max_bitrate", encode_config.rcParams.maxBitRate},
        {"vbv_buffer_size", encode_config.rcParams.vbvBufferSize},
        {"target_quality", encode_config.rcParams.targetQuality},
        {"target_quality_lsb", encode_config.rcParams.targetQualityLSB},
        {"const_qp", {
            {"p", encode_config.rcParams.constQP.qpInterP},
            {"b", encode_config.rcParams.constQP.qpInterB},
            {"i", encode_config.rcParams.constQP.qpIntra}
        }},
        {"enable_min_qp", encode_config.rcParams.enableMinQP},
        {"enable_max_qp", encode_config.rcParams.enableMaxQP},
        {"enable_aq", encode_config.rcParams.enableAQ},
        {"enable_temporal_aq", encode_config.rcParams.enableTemporalAQ},
        {"enable_lookahead", encode_config.rcParams.enableLookahead},
        {"lookahead_depth", encode_config.rcParams.lookaheadDepth},
        {"qp_map_mode", {
            {"value", encode_config.rcParams.qpMapMode},
            {"name", qp_map_mode_to_string(static_cast<NV_ENC_QP_MAP_MODE>(encode_config.rcParams.qpMapMode))}
        }},
        {"multi_pass", {
            {"value", encode_config.rcParams.multiPass},
            {"name", multi_pass_to_string(static_cast<NV_ENC_MULTI_PASS>(encode_config.rcParams.multiPass))}
        }},
        {"low_delay_keyframe_scale", encode_config.rcParams.lowDelayKeyFrameScale},
        {"strict_gop_target", encode_config.rcParams.strictGOPTarget},
        {"enable_non_ref_p", encode_config.rcParams.enableNonRefP}
    };

    info["codec"] = {
        {"name", "unknown"}
    };

    if (guid_equals(initialize_params.encodeGUID, NV_ENC_CODEC_HEVC_GUID)) {
        const auto& hevc = encode_config.encodeCodecConfig.hevcConfig;
        info["codec"] = {
            {"name", "hevc"},
            {"pixel_bit_depth_minus_8", hevc.pixelBitDepthMinus8},
            {"chroma_format_idc", hevc.chromaFormatIDC},
            {"idr_period", hevc.idrPeriod},
            {"slice_mode", hevc.sliceMode},
            {"slice_mode_data", hevc.sliceModeData},
            {"max_num_ref_frames_in_dpb", hevc.maxNumRefFramesInDPB},
            {"repeat_sps_pps", hevc.repeatSPSPPS},
            {"output_buffering_period_sei", hevc.outputBufferingPeriodSEI},
            {"output_picture_timing_sei", hevc.outputPictureTimingSEI},
            {"output_aud", hevc.outputAUD},
            {"enable_ltr", hevc.enableLTR}
        };
    } else if (guid_equals(initialize_params.encodeGUID, NV_ENC_CODEC_H264_GUID)) {
        const auto& h264 = encode_config.encodeCodecConfig.h264Config;
        info["codec"] = {
            {"name", "h264"},
            {"idr_period", h264.idrPeriod},
            {"slice_mode", h264.sliceMode},
            {"slice_mode_data", h264.sliceModeData},
            {"repeat_sps_pps", h264.repeatSPSPPS},
            {"max_num_ref_frames", h264.maxNumRefFrames},
            {"adaptive_transform_mode", h264.adaptiveTransformMode},
            {"bdirect_mode", h264.bdirectMode},
            {"entropy_coding_mode", h264.entropyCodingMode}
        };
    }

    return info;
}

} // namespace

// Helper to initialize the FFmpeg-based file writer
static inline void initialize_writer_hw(
    Writer *writer,
    CameraParams *camera_params,
    const RecordingOutputConfig& recording_output_config,
    const std::string& folder_name,
    const std::string& encoder_str,
    const std::vector<std::pair<std::string, std::string>>& metadata_tags,
    const FFmpegWriterQueueConfig& queue_config
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
            metadata_tags,
            queue_config);
    } else {
        writer->video = new FFmpegWriter(
            AV_CODEC_ID_HEVC,
            recording_output_config.resolved_width,
            recording_output_config.resolved_height,
            camera_params->frame_rate,
            writer->video_file.c_str(),
            writer->keyframe_file.c_str(),
            metadata_tags,
            queue_config);
    }
    writer->metadata = new std::ofstream();
    writer->metadata->open(writer->metadata_file.c_str());
    if (!(*writer->metadata))
    {
        // Fail the recording start loudly: returning here would leave an open
        // writer whose thread never starts, queueing packets to nowhere. The
        // throw is caught at the worker-thread boundary (catch/latch/drain).
        delete writer->metadata;
        writer->metadata = nullptr;
        delete writer->video;
        writer->video = nullptr;
        throw std::runtime_error(
            "Failed to open recording metadata file " + writer->metadata_file);
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
    int encode_gpu_id,
    const ResolvedRecordingConfig& resolved_recording_config,
    std::string base_folder_name,
    std::shared_ptr<SharedRecordingOutput> shared_output,
    bool owns_recording_output,
    EncoderPreprocessWorker* prep_worker,
    CameraControl* camera_control
)
// Member initializers are ordered to match the declaration order in the header
// (encoder_ and m_prep_worker_ are declared first) to satisfy -Wreorder.
: CThreadWorker(name),
  encoder_(),
  m_prep_worker_(prep_worker),
  camera_params_(camera_params),
  encode_gpu_id_(encode_gpu_id >= 0 ? encode_gpu_id : camera_params->gpu_id),
  resolved_recording_config_(resolved_recording_config),
  recording_output_config_(resolved_recording_config.output),
  base_folder_name_(base_folder_name),
  codec_(normalize_video_encode_codec(resolved_recording_config.encode.codec)),
  preset_(normalize_video_encode_preset(resolved_recording_config.encode.preset)),
  tuning_(normalize_video_encode_tuning(resolved_recording_config.encode.tuning)),
  rate_control_mode_(normalize_video_encode_rate_control_mode(
      resolved_recording_config.encode.rate_control_mode)),
  encoder_control_overrides_(resolved_recording_config.encoder_control_overrides),
  importance_map_config_(resolved_recording_config.importance_map),
  pre_encoder_reference_capture_config_(resolved_recording_config.pre_encoder_reference_capture),
  direct_input_enabled_(resolved_recording_config.encode.nvenc_direct_input),
  nvenc_extra_output_delay_(resolve_nvenc_extra_output_delay(camera_params)),
  nvenc_harvest_delay_us_(resolve_nvenc_harvest_delay_us(camera_params)),
  quality_value_(clamp_video_encode_quality_value(resolved_recording_config.encode.quality_value)),
  gop_length_(sanitize_recording_gop_length(resolved_recording_config.encode.gop_length)),
  recording_strategy_config_(resolved_recording_config.strategy),
  shared_output_(std::move(shared_output)),
  owns_recording_output_(owns_recording_output),
  m_stream(nullptr),
  camera_control_(camera_control),
  is_recording_(false), // Initialize recording state
  last_recording_frame_id_(0),
  last_fps_update_time_(std::chrono::steady_clock::now()),
  frame_counter_(0)
{
    resolved_recording_config_.recording_gpu_id = encode_gpu_id_;

    if (!pre_encoder_reference_capture_config_.has_valid_bound()) {
        throw std::invalid_argument(
            "pre_encoder_reference_capture requires exactly one positive bound: max_frames or max_seconds");
    }
    pre_encoder_reference_writer_.Configure(pre_encoder_reference_capture_config_);
    pre_encoder_reference_async_enabled_ =
        pre_encoder_reference_capture_config_.enabled && !direct_input_enabled_;
    if (split_harvest_requested()) {
        if (direct_input_enabled_) {
            std::cout << "[EncoderHwWorker] ORANGE_NVENC_SPLIT_HARVEST=1 ignored for "
                      << threadName
                      << " because direct NVENC input slot retirement is not split-harvest safe yet"
                      << std::endl;
        } else if (!shared_output_) {
            std::cout << "[EncoderHwWorker] ORANGE_NVENC_SPLIT_HARVEST=1 ignored for "
                      << threadName
                      << " because this worker does not use SharedRecordingOutput"
                      << std::endl;
        } else {
            split_harvest_enabled_ = true;
            std::cout << "[EncoderHwWorker] ORANGE_NVENC_SPLIT_HARVEST=1 enabled for "
                      << threadName
                      << " (submit and bitstream harvest run on separate host threads)"
                      << std::endl;
        }
    }

    ck(cudaSetDevice(encode_gpu_id_));
    ck(cudaStreamCreate(&m_stream));
    if (pre_encoder_reference_capture_config_.enabled) {
        ck(cudaStreamCreate(&pre_encoder_reference_stream_));
    }
    ck(cuCtxGetCurrent(&encoder_.cuContext));
    encoder_.pEnc = new NvEncoderCuda(
        encoder_.cuContext,
        recording_output_config_.resolved_width,
        recording_output_config_.resolved_height,
        NV_ENC_BUFFER_FORMAT_NV12,
        static_cast<uint32_t>(nvenc_extra_output_delay_));
    if (direct_input_enabled_) {
        encoder_.pEnc->SetExternalInputBufferMode(true);
    }

    NV_ENC_INITIALIZE_PARAMS initializeParams = { NV_ENC_INITIALIZE_PARAMS_VER };
    NV_ENC_CONFIG encodeConfig = {NV_ENC_CONFIG_VER};
    initializeParams.encodeConfig = &encodeConfig;

    VideoEncodeProfile encode_profile = build_full_frame_video_encode_profile(
        *camera_params_,
        encode_gpu_id_,
        resolved_recording_config_);
    encode_profile.codec = codec_;
    encode_profile.preset = preset_;
    encode_profile.tuning = tuning_;
    encode_profile.rate_control_mode = rate_control_mode_;
    encode_profile.quality_value = quality_value_;
    encode_profile.requested_gop_length = gop_length_;
    encode_profile.resolved_gop_length =
        resolve_recording_gop_length(*camera_params_, tuning_, gop_length_);
    encode_profile.encoder_control_overrides = encoder_control_overrides_;
    encode_profile.importance_map = importance_map_config_;

    const VideoEncodeProfileNvencGuids nvenc_guids =
        resolve_video_encode_profile_nvenc_guids(encode_profile);
    encoder_.pEnc->CreateDefaultEncoderParams(
        &initializeParams,
        nvenc_guids.codec_guid,
        nvenc_guids.preset_guid,
        nvenc_guids.tuning_info);
    apply_video_encode_profile_to_nvenc_config(
        encode_profile,
        &initializeParams,
        &encodeConfig);
    std::cout << "[EncoderHwWorker] Resolved video encode profile for "
              << threadName << ": "
              << video_encode_profile_summary(encode_profile)
              << std::endl;

    importance_map_config_.mode = normalize_importance_map_mode(importance_map_config_.mode);
    importance_map_config_.roi_size_px =
        normalize_importance_map_roi_size_px(importance_map_config_.roi_size_px);
    if (importance_map_config_.mode != "off" && importance_map_config_.mode != "static_roi") {
        throw std::invalid_argument(
            "Unsupported importance_map_mode: " + importance_map_config_.mode +
            " (expected off or static_roi)");
    }
    if (importance_map_mode_enabled(importance_map_config_.mode)) {
        encodeConfig.rcParams.qpMapMode = NV_ENC_QP_MAP_DELTA;
    } else {
        encodeConfig.rcParams.qpMapMode = NV_ENC_QP_MAP_DISABLED;
    }
    initializeParams.enableWeightedPrediction = 0;

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
        encoder_snapshot_.path = direct_input_enabled_
            ? "hw_direct_input"
            : (split_harvest_enabled_ ? "hw_split_harvest" : "hw");
        encoder_snapshot_.codec = codec_;
        encoder_snapshot_.preset = preset_;
        encoder_snapshot_.tuning = tuning_;
        encoder_snapshot_.rc_strategy =
            resolve_video_encode_rate_control_strategy(tuning_, rate_control_mode_);
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
        recording_gop_length_ = std::max<uint32_t>(1u, resolved_config.gopLength);
        encoder_snapshot_.frame_interval_p = resolved_config.frameIntervalP;
        encoder_snapshot_.nvenc_extra_output_delay = nvenc_extra_output_delay_;
        encoder_snapshot_.nvenc_harvest_delay_us = nvenc_harvest_delay_us_;
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
        encoder_snapshot_.lookahead_depth = resolved_config.rcParams.lookaheadDepth;
        encoder_snapshot_.multi_pass = resolved_config.rcParams.multiPass;
        encoder_snapshot_.low_delay_keyframe_scale = resolved_config.rcParams.lowDelayKeyFrameScale;
        encoder_snapshot_.strict_gop_target = resolved_config.rcParams.strictGOPTarget;
        encoder_snapshot_.enable_non_ref_p = resolved_config.rcParams.enableNonRefP;
        encoder_snapshot_.enable_ptd = resolved_params.enablePTD;
        encoder_snapshot_.requested_aq = encoder_control_overrides_.aq;
        encoder_snapshot_.requested_temporal_aq = encoder_control_overrides_.temporal_aq;
        encoder_snapshot_.requested_lookahead = encoder_control_overrides_.lookahead;
        encoder_snapshot_.requested_lookahead_depth = encoder_control_overrides_.lookahead_depth;
        encoder_snapshot_.requested_target_bitrate_bps = encoder_control_overrides_.target_bitrate_bps;
        encoder_snapshot_.requested_max_bitrate_bps = encoder_control_overrides_.max_bitrate_bps;
        encoder_snapshot_.requested_vbv_buffer_size = encoder_control_overrides_.vbv_buffer_size;
        encoder_snapshot_.requested_importance_map_mode = importance_map_config_.mode;
        encoder_snapshot_.requested_importance_map_roi_size_px = importance_map_config_.roi_size_px;
        encoder_snapshot_.qp_map_mode = resolved_config.rcParams.qpMapMode;
        encoder_snapshot_.source_gpu_id = camera_params_->gpu_id;
        encoder_snapshot_.encode_gpu_id = encode_gpu_id_;
        encoder_snapshot_.gpu_id = encode_gpu_id_;
        encoder_snapshot_.gpu = build_gpu_runtime_info(encode_gpu_id_);
        encoder_snapshot_.resolved_config = build_resolved_encoder_config_json(
            resolved_params,
            resolved_config,
            encoder_buffer_count_,
            encoder_input_pitch_,
            direct_input_enabled_,
            nvenc_extra_output_delay_);
        encoder_snapshot_.color = camera_params_->color;
        encoder_snapshot_.recording_strategy = recording_strategy_config_;

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

    initialize_importance_map();
}

EncoderHwWorker::~EncoderHwWorker()
{
    stop_split_harvest_thread();
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

void EncoderHwWorker::SetSplitGopTopologyStaticSnapshot(const nlohmann::json& topology_snapshot)
{
    encoder_snapshot_.split_gop_topology_static = topology_snapshot;
}

void EncoderHwWorker::SetSplitGopTopologyRuntimeSnapshot(const nlohmann::json& topology_snapshot)
{
    encoder_snapshot_.split_gop_topology_runtime = topology_snapshot;
}

void EncoderHwWorker::initialize_importance_map()
{
    importance_map_enabled_ = false;
    importance_map_qp_delta_.clear();
    importance_map_qp_delta_size_ = 0;

    encoder_snapshot_.active_importance_map_mode = "off";
    encoder_snapshot_.importance_map_block_size = 0;
    encoder_snapshot_.importance_map_grid_width = 0;
    encoder_snapshot_.importance_map_grid_height = 0;
    encoder_snapshot_.importance_map_roi_size_px = 0;
    encoder_snapshot_.importance_map_inside_delta = 0;
    encoder_snapshot_.importance_map_outside_delta = 0;

    if (!importance_map_mode_enabled(importance_map_config_.mode)) {
        return;
    }

    const bool is_hevc = codec_ == "hevc";
    const uint32_t block_size = is_hevc ? 32U : 16U;
    const uint32_t width = static_cast<uint32_t>(recording_output_config_.resolved_width);
    const uint32_t height = static_cast<uint32_t>(recording_output_config_.resolved_height);
    const uint32_t grid_width = (width + block_size - 1U) / block_size;
    const uint32_t grid_height = (height + block_size - 1U) / block_size;
    if (grid_width == 0 || grid_height == 0) {
        throw std::runtime_error("Importance map grid dimensions are invalid");
    }

    const uint32_t roi_size_px = static_cast<uint32_t>(std::clamp(
        importance_map_config_.roi_size_px,
        1,
        static_cast<int>(std::min(width, height))));
    const double center_x = static_cast<double>(width) * 0.5;
    const double center_y = static_cast<double>(height) * 0.5;
    const double half_extent = static_cast<double>(roi_size_px) * 0.5;
    const double roi_min_x = center_x - half_extent;
    const double roi_max_x = center_x + half_extent;
    const double roi_min_y = center_y - half_extent;
    const double roi_max_y = center_y + half_extent;

    importance_map_qp_delta_.resize(static_cast<std::size_t>(grid_width) * static_cast<std::size_t>(grid_height));
    for (uint32_t y = 0; y < grid_height; ++y) {
        for (uint32_t x = 0; x < grid_width; ++x) {
            const double sample_x =
                std::min(static_cast<double>(width) - 0.5,
                         (static_cast<double>(x) + 0.5) * static_cast<double>(block_size));
            const double sample_y =
                std::min(static_cast<double>(height) - 0.5,
                         (static_cast<double>(y) + 0.5) * static_cast<double>(block_size));
            const bool inside =
                sample_x >= roi_min_x && sample_x < roi_max_x &&
                sample_y >= roi_min_y && sample_y < roi_max_y;
            importance_map_qp_delta_[static_cast<std::size_t>(y) * grid_width + x] =
                inside ? kStaticRoiInsideDelta : kStaticRoiOutsideDelta;
        }
    }

    importance_map_qp_delta_size_ = importance_map_qp_delta_.size() * sizeof(importance_map_qp_delta_.front());
    importance_map_enabled_ = true;
    encoder_snapshot_.active_importance_map_mode = importance_map_config_.mode;
    encoder_snapshot_.importance_map_block_size = block_size;
    encoder_snapshot_.importance_map_grid_width = grid_width;
    encoder_snapshot_.importance_map_grid_height = grid_height;
    encoder_snapshot_.importance_map_roi_size_px = roi_size_px;
    encoder_snapshot_.importance_map_inside_delta = kStaticRoiInsideDelta;
    encoder_snapshot_.importance_map_outside_delta = kStaticRoiOutsideDelta;
}

bool EncoderHwWorker::importance_map_active() const
{
    return importance_map_enabled_ && !importance_map_qp_delta_.empty() && importance_map_qp_delta_size_ > 0;
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
        recycle_encoder_entry(pending.entry, pending.retired_slots, pending.slot_submitted);
        pending_pre_encoder_reference_captures_.pop_front();
    }
}

void EncoderHwWorker::recycle_encoder_entry(ENCODER_WORKER_ENTRY* entry,
                                           const std::vector<uint32_t>& retired_slots,
                                           bool slot_submitted)
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
    }

    if (!slot_submitted && entry->slot_id >= 0) {
        m_prep_worker_->free_direct_input_slots_.push(entry->slot_id);
        m_prep_worker_->available_buffers_++;
    }
    entry->slot_id = -1;
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
    if (owns_recording_output_ && encoder_snapshot_valid_ && !active_recording_folder_.empty()) {
        const std::string camera_key = camera_params_->camera_serial.empty()
            ? std::to_string(camera_params_->camera_id)
            : camera_params_->camera_serial;
        nlohmann::json encoder_info = build_encoder_snapshot_json();
        if (!active_recording_session_folder_.empty() &&
            active_recording_session_folder_ != active_recording_folder_) {
            update_recording_snapshot_encoder(active_recording_session_folder_, camera_key, encoder_info);
        } else {
            update_recording_snapshot_encoder(active_recording_folder_, camera_key, encoder_info);
        }
    }
    const std::string finalized_recording_folder = active_recording_folder_;
    const std::string finalized_session_folder = active_recording_session_folder_;
    active_recording_folder_.clear();
    active_recording_session_folder_.clear();
    is_recording_ = false;

    if (owns_recording_output_ && camera_control_) {
        int remaining = camera_control_->active_recorders.fetch_sub(1, std::memory_order_relaxed) - 1;
        if (remaining == 0) {
            if (camera_control_->recording_draining) {
                camera_control_->recording_draining = false;
            }
            camera_control_->stop_record = false;
            std::lock_guard<std::mutex> lock(camera_control_->recording_folder_mutex);
            if (camera_control_->recording_output_folder == finalized_recording_folder) {
                camera_control_->recording_output_folder.clear();
            }
            if (!camera_control_->preserve_recording_session_state &&
                camera_control_->recording_folder == finalized_session_folder) {
                camera_control_->recording_folder.clear();
            }
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
    if (shared_output_ && owns_recording_output_ && shared_output_->active_worker_sessions() > 1) {
        return false;
    }
    return true;
}

void EncoderHwWorker::refresh_writer_queue_metrics()
{
    if (shared_output_) {
        const SharedRecordingOutputStats output_stats = shared_output_->stats();
        writer_queue_overflowed_ = output_stats.writer_queue_overflowed;
        writer_queue_overflow_events_ = output_stats.writer_queue_overflow_events;
        writer_queue_peak_packets_ = output_stats.writer_queue_peak_packets;
        writer_queue_peak_bytes_ = output_stats.writer_queue_peak_bytes;
        pending_gop_buffered_bytes_ = output_stats.pending_gop_bytes;
        pending_gop_peak_count_ = output_stats.pending_gop_peak_count;
        pending_gop_peak_backlog_count_ = output_stats.pending_gop_peak_backlog_count;
        pending_gop_peak_bytes_ = output_stats.pending_gop_peak_bytes;
        pending_gop_overflowed_ = output_stats.pending_gop_overflowed;
        pending_gop_overflow_events_ = output_stats.pending_gop_overflow_events;
        next_gop_to_flush_ = output_stats.next_gop_to_flush;
        if (!output_stats.active_folder.empty()) {
            active_recording_folder_ = output_stats.active_folder;
        }
        if (camera_control_ &&
            output_stats.rollover_completed_request_id > 0 &&
            !output_stats.rollover_completed_folder.empty()) {
            std::lock_guard<std::mutex> lock(camera_control_->recording_folder_mutex);
            if (output_stats.rollover_completed_request_id >
                camera_control_->recording_rollover_completed_request_id) {
                camera_control_->recording_rollover_completed_request_id =
                    output_stats.rollover_completed_request_id;
                camera_control_->recording_rollover_completed_frame_id =
                    output_stats.rollover_completed_frame_id;
                camera_control_->recording_rollover_completed_folder =
                    output_stats.rollover_completed_folder;
                camera_control_->recording_output_folder =
                    output_stats.rollover_completed_folder;
            }
        }
        return;
    }
    if (!writer_.video) {
        return;
    }
    writer_queue_overflowed_ =
        writer_queue_overflowed_ || writer_.video->has_queue_overflowed();
    writer_queue_overflow_events_ = std::max<uint64_t>(
        writer_queue_overflow_events_,
        writer_.video->queue_overflow_events());
    writer_queue_peak_packets_ = std::max<size_t>(
        writer_queue_peak_packets_,
        writer_.video->peak_queued_packets());
    writer_queue_peak_bytes_ = std::max<size_t>(
        writer_queue_peak_bytes_,
        writer_.video->peak_queued_bytes());
}

void EncoderHwWorker::reset_pending_gop_state()
{
    pending_output_sample_indices_.clear();
    submitted_frames_by_gop_.clear();
    emitted_frames_by_gop_.clear();
    submitted_complete_gops_.clear();
    writer_sample_index_base_ = 0;
    writer_sample_index_base_initialized_ = false;
    forced_rollover_request_id_ = 0;
    if (shared_output_) {
        next_gop_to_flush_ = 0;
        next_gop_to_flush_initialized_ = false;
        pending_gop_buffered_bytes_ = 0;
        pending_gop_peak_count_ = 0;
        pending_gop_peak_backlog_count_ = 0;
        pending_gop_peak_bytes_ = 0;
        pending_gop_overflowed_ = false;
        pending_gop_overflow_events_ = 0;
        return;
    }
    pending_gops_.clear();
    next_gop_to_flush_ = 0;
    next_gop_to_flush_initialized_ = false;
    pending_gop_buffered_bytes_ = 0;
    pending_gop_peak_count_ = 0;
    pending_gop_peak_backlog_count_ = 0;
    pending_gop_peak_bytes_ = 0;
    pending_gop_overflowed_ = false;
    pending_gop_overflow_events_ = 0;
}

SharedRecordingOutputOpenParams EncoderHwWorker::build_shared_output_open_params(
    const std::string& folder_name) const
{
    VideoEncodeProfile metadata_profile = build_full_frame_video_encode_profile(
        *camera_params_,
        encode_gpu_id_,
        resolved_recording_config_);
    metadata_profile.codec = codec_;
    metadata_profile.preset = preset_;
    metadata_profile.tuning = tuning_;
    metadata_profile.rate_control_mode = rate_control_mode_;
    metadata_profile.quality_value = quality_value_;
    metadata_profile.requested_gop_length = gop_length_;
    metadata_profile.resolved_gop_length = recording_gop_length_;
    metadata_profile.encoder_control_overrides = encoder_control_overrides_;
    metadata_profile.importance_map = importance_map_config_;
    const auto metadata_tags = build_video_encode_metadata_tags(metadata_profile);
    FFmpegWriterQueueConfig queue_config;
    if (recording_strategy_config_.split_gop_enabled()) {
        queue_config.max_queued_packets =
            static_cast<size_t>(recording_strategy_config_.split_gop.writer_queue.max_packets);
        queue_config.max_queued_bytes =
            static_cast<size_t>(recording_strategy_config_.split_gop.writer_queue.max_bytes);
    }

    SharedRecordingOutputOpenParams open_params;
    open_params.camera_params = camera_params_;
    open_params.recording_output_config = recording_output_config_;
    open_params.folder_name = folder_name;
    open_params.codec = codec_;
    open_params.metadata_tags = metadata_tags;
    open_params.queue_config = queue_config;
    open_params.split_gop_config = recording_strategy_config_.split_gop;
    open_params.recording_gop_length = recording_gop_length_;
    return open_params;
}

void EncoderHwWorker::prepare_requested_rollover(uint64_t recording_frame_id)
{
    (void)recording_frame_id;
    if (!shared_output_ || !camera_control_ || !is_recording_) {
        return;
    }

    std::string next_folder;
    uint64_t rollover_at_frame_id = 0;
    uint64_t request_id = 0;
    {
        std::lock_guard<std::mutex> lock(camera_control_->recording_folder_mutex);
        request_id = camera_control_->recording_rollover_request_id;
        rollover_at_frame_id = camera_control_->recording_rollover_at_frame_id;
        next_folder = camera_control_->pending_recording_output_folder;
        if (request_id == 0 ||
            request_id <= camera_control_->recording_rollover_completed_request_id ||
            rollover_at_frame_id == 0 ||
            next_folder.empty()) {
            return;
        }
    }

    shared_output_->prepare_rollover(
        build_shared_output_open_params(next_folder),
        rollover_at_frame_id,
        request_id);
    sync_shared_rollover_completion();
}

void EncoderHwWorker::sync_shared_rollover_completion()
{
    if (!shared_output_ || !camera_control_) {
        return;
    }

    const SharedRecordingOutputStats stats = shared_output_->stats();
    if (!stats.active_folder.empty()) {
        active_recording_folder_ = stats.active_folder;
    }
    if (stats.rollover_completed_request_id == 0 ||
        stats.rollover_completed_folder.empty()) {
        return;
    }

    std::lock_guard<std::mutex> lock(camera_control_->recording_folder_mutex);
    if (stats.rollover_completed_request_id <=
        camera_control_->recording_rollover_completed_request_id) {
        return;
    }
    camera_control_->recording_rollover_completed_request_id =
        stats.rollover_completed_request_id;
    camera_control_->recording_rollover_completed_frame_id =
        stats.rollover_completed_frame_id;
    camera_control_->recording_rollover_completed_folder =
        stats.rollover_completed_folder;
    camera_control_->recording_output_folder = stats.rollover_completed_folder;
}

std::vector<uint64_t> EncoderHwWorker::resolve_output_sample_indices(
    size_t packet_count,
    const std::vector<uint64_t>& output_timestamps,
    std::optional<uint64_t> submitted_sample_index,
    const char* context)
{
    if (submitted_sample_index.has_value()) {
        pending_output_sample_indices_.push_back(*submitted_sample_index);
    }

    std::vector<uint64_t> resolved_sample_indices;
    resolved_sample_indices.reserve(packet_count);

    while (resolved_sample_indices.size() < packet_count && !pending_output_sample_indices_.empty()) {
        resolved_sample_indices.push_back(pending_output_sample_indices_.front());
        pending_output_sample_indices_.pop_front();
    }

    if (resolved_sample_indices.size() == packet_count) {
        return resolved_sample_indices;
    }

    if (output_timestamps.size() >= packet_count) {
        if (packet_count > 0) {
            std::cerr << "[" << threadName << "] Falling back to NVENC output timestamps in "
                      << (context ? context : "unknown")
                      << " because queued sample indices were insufficient. queued="
                      << pending_output_sample_indices_.size()
                      << " packets=" << packet_count
                      << std::endl;
        }
        return output_timestamps;
    }

    if (!resolved_sample_indices.empty()) {
        const uint64_t last_value = resolved_sample_indices.back();
        while (resolved_sample_indices.size() < packet_count) {
            resolved_sample_indices.push_back(last_value);
        }
        return resolved_sample_indices;
    }

    resolved_sample_indices.assign(packet_count, 0);
    return resolved_sample_indices;
}

void EncoderHwWorker::note_submitted_frame(uint64_t gop_index, bool is_last_frame_in_gop)
{
    submitted_frames_by_gop_[gop_index]++;
    if (is_last_frame_in_gop) {
        submitted_complete_gops_.insert(gop_index);
    }
}

std::vector<uint64_t> EncoderHwWorker::note_emitted_packets_and_collect_completed_gops(
    const std::vector<uint64_t>& packet_sample_indices)
{
    std::vector<uint64_t> completed_gops;
    std::set<uint64_t> candidate_gops;

    for (uint64_t sample_index : packet_sample_indices) {
        const uint64_t gop_index = sample_index / std::max<uint32_t>(1u, recording_gop_length_);
        emitted_frames_by_gop_[gop_index]++;
        candidate_gops.insert(gop_index);
    }

    for (uint64_t gop_index : candidate_gops) {
        const auto submitted_it = submitted_frames_by_gop_.find(gop_index);
        const auto emitted_it = emitted_frames_by_gop_.find(gop_index);
        if (submitted_it == submitted_frames_by_gop_.end() ||
            emitted_it == emitted_frames_by_gop_.end()) {
            continue;
        }
        if (!submitted_complete_gops_.count(gop_index)) {
            continue;
        }
        if (emitted_it->second < submitted_it->second) {
            continue;
        }
        completed_gops.push_back(gop_index);
        submitted_frames_by_gop_.erase(submitted_it);
        emitted_frames_by_gop_.erase(emitted_it);
        submitted_complete_gops_.erase(gop_index);
    }

    return completed_gops;
}

void EncoderHwWorker::buffer_encoded_packets(const std::vector<std::vector<uint8_t>>& packets,
                                             const std::vector<uint64_t>& output_timestamps,
                                             int64_t fallback_sample_index,
                                             uint64_t completion_gop_index,
                                             bool mark_complete,
                                             const std::optional<RecordingMetadataRow>& metadata_row,
                                             const std::optional<RecordingOutputTimingSample>& timing_sample)
{
    if (shared_output_) {
        shared_output_->submit_frame_output(
            packets,
            output_timestamps,
            fallback_sample_index,
            completion_gop_index,
            mark_complete,
            metadata_row,
            timing_sample);
        refresh_writer_queue_metrics();
        return;
    }

    if (!recording_strategy_config_.split_gop_enabled()) {
        for (size_t i = 0; i < packets.size(); ++i) {
            const int64_t sample_index = i < output_timestamps.size()
                ? static_cast<int64_t>(output_timestamps[i])
                : fallback_sample_index;
            if (writer_.video) {
                const int64_t writer_sample_index = normalize_writer_sample_index(sample_index);
                writer_.video->push_packet(
                    const_cast<uint8_t*>(packets[i].data()),
                    static_cast<int>(packets[i].size()),
                    writer_sample_index);
            }
            if (sample_index >= 0) {
                last_recording_frame_id_ = std::max<uint64_t>(
                    last_recording_frame_id_,
                    static_cast<uint64_t>(sample_index + 1));
            }
        }
        refresh_writer_queue_metrics();
        return;
    }

    for (size_t i = 0; i < packets.size(); ++i) {
        const int64_t sample_index = i < output_timestamps.size()
            ? static_cast<int64_t>(output_timestamps[i])
            : fallback_sample_index;
        const uint64_t gop_index = sample_index >= 0
            ? static_cast<uint64_t>(sample_index) / recording_gop_length_
            : completion_gop_index;
        if (!next_gop_to_flush_initialized_) {
            next_gop_to_flush_ = gop_index;
            next_gop_to_flush_initialized_ = true;
        }

        auto [it, inserted] = pending_gops_.try_emplace(gop_index);
        PendingGop& pending = it->second;
        if (inserted) {
            pending.gop_index = gop_index;
            pending.created_at = std::chrono::steady_clock::now();
            pending_gop_peak_count_ = std::max(pending_gop_peak_count_, pending_gops_.size());
            pending_gop_peak_backlog_count_ =
                std::max(pending_gop_peak_backlog_count_, pending_gop_backlog_count());
        }

        const size_t packet_size = packets[i].size();
        if (recording_strategy_config_.split_gop.max_buffered_bytes > 0 &&
            pending_gop_buffered_bytes_ + packet_size >
                recording_strategy_config_.split_gop.max_buffered_bytes) {
            pending_gop_overflowed_ = true;
            pending_gop_overflow_events_++;
            throw std::runtime_error("split_gop pending GOP bytes exceeded configured limit");
        }

        BufferedEncodedPacket buffered_packet;
        buffered_packet.bytes = packets[i];
        buffered_packet.sample_index = sample_index;
        pending.total_bytes += packet_size;
        pending_gop_buffered_bytes_ += packet_size;
        pending.packets.push_back(std::move(buffered_packet));

        pending_gop_peak_count_ = std::max(pending_gop_peak_count_, pending_gops_.size());
        pending_gop_peak_backlog_count_ =
            std::max(pending_gop_peak_backlog_count_, pending_gop_backlog_count());
        pending_gop_peak_bytes_ = std::max(pending_gop_peak_bytes_, pending_gop_buffered_bytes_);

        if (sample_index >= 0) {
            last_recording_frame_id_ = std::max<uint64_t>(
                last_recording_frame_id_,
                static_cast<uint64_t>(sample_index + 1));
        }
    }

    if (mark_complete) {
        if (!next_gop_to_flush_initialized_) {
            next_gop_to_flush_ = completion_gop_index;
            next_gop_to_flush_initialized_ = true;
        }
        auto [it, inserted] = pending_gops_.try_emplace(completion_gop_index);
        PendingGop& pending = it->second;
        if (inserted) {
            pending.gop_index = completion_gop_index;
            pending.created_at = std::chrono::steady_clock::now();
            pending_gop_peak_count_ = std::max(pending_gop_peak_count_, pending_gops_.size());
            pending_gop_peak_backlog_count_ =
                std::max(pending_gop_peak_backlog_count_, pending_gop_backlog_count());
        }
        pending.complete = true;
    }

    flush_pending_gops(false);
    if (recording_strategy_config_.split_gop.max_inflight_gops > 0 &&
        pending_gop_backlog_count() > recording_strategy_config_.split_gop.max_inflight_gops) {
        pending_gop_overflowed_ = true;
        pending_gop_overflow_events_++;
        throw std::runtime_error("split_gop pending GOP backlog exceeded configured limit");
    }
}

void EncoderHwWorker::flush_pending_gops(bool flush_all)
{
    if (shared_output_) {
        refresh_writer_queue_metrics();
        return;
    }
    if (!recording_strategy_config_.split_gop_enabled()) {
        return;
    }

    while (true) {
        auto it = pending_gops_.find(next_gop_to_flush_);
        if (it == pending_gops_.end()) {
            break;
        }
        if (!flush_all && !it->second.complete) {
            break;
        }

        PendingGop pending = std::move(it->second);
        pending_gops_.erase(it);
        pending_gop_buffered_bytes_ -= pending.total_bytes;

        for (const auto& packet : pending.packets) {
            if (writer_.video) {
                const int64_t writer_sample_index =
                    normalize_writer_sample_index(packet.sample_index);
                writer_.video->push_packet(
                    const_cast<uint8_t*>(packet.bytes.data()),
                    static_cast<int>(packet.bytes.size()),
                    writer_sample_index);
            }
        }
        next_gop_to_flush_++;
    }
    refresh_writer_queue_metrics();
}

int64_t EncoderHwWorker::normalize_writer_sample_index(int64_t sample_index)
{
    if (sample_index < 0) {
        return sample_index;
    }
    if (!writer_sample_index_base_initialized_) {
        writer_sample_index_base_ = sample_index;
        writer_sample_index_base_initialized_ = true;
    }
    return sample_index - writer_sample_index_base_;
}

size_t EncoderHwWorker::pending_gop_backlog_count() const
{
    size_t backlog = pending_gops_.size();
    if (backlog == 0) {
        return 0;
    }
    if (pending_gops_.find(next_gop_to_flush_) != pending_gops_.end()) {
        backlog--;
    }
    return backlog;
}

int64_t EncoderHwWorker::oldest_pending_gop_age_ms() const
{
    if (shared_output_) {
        return shared_output_->stats().oldest_pending_gop_age_ms;
    }
    if (pending_gops_.empty()) {
        return 0;
    }
    const auto now = std::chrono::steady_clock::now();
    const auto oldest = std::min_element(
        pending_gops_.begin(),
        pending_gops_.end(),
        [](const auto& lhs, const auto& rhs) {
            return lhs.second.created_at < rhs.second.created_at;
        });
    return std::chrono::duration_cast<std::chrono::milliseconds>(now - oldest->second.created_at).count();
}

bool EncoderHwWorker::split_harvest_requested() const
{
    return env_flag_enabled("ORANGE_NVENC_SPLIT_HARVEST");
}

void EncoderHwWorker::start_split_harvest_thread()
{
    if (!split_harvest_enabled_ || split_harvest_thread_.joinable()) {
        return;
    }
    {
        std::lock_guard<std::mutex> lock(split_harvest_mutex_);
        split_harvest_stop_ = false;
        split_harvest_wake_ = false;
        split_harvest_error_ = false;
        split_harvest_error_message_.clear();
    }
    split_harvest_thread_ = std::thread(&EncoderHwWorker::split_harvest_loop, this);
}

void EncoderHwWorker::stop_split_harvest_thread()
{
    if (!split_harvest_thread_.joinable()) {
        return;
    }
    {
        std::lock_guard<std::mutex> lock(split_harvest_mutex_);
        split_harvest_stop_ = true;
        split_harvest_wake_ = true;
    }
    split_harvest_cv_.notify_all();
    split_harvest_thread_.join();
}

void EncoderHwWorker::notify_split_harvest_thread()
{
    if (!split_harvest_enabled_) {
        return;
    }
    {
        std::lock_guard<std::mutex> lock(split_harvest_mutex_);
        split_harvest_wake_ = true;
    }
    split_harvest_cv_.notify_one();
}

void EncoderHwWorker::delay_split_harvest_if_configured()
{
    if (nvenc_harvest_delay_us_ <= 0) {
        return;
    }
    NVTX_SYNC_DYNAMIC(std::string("NVENC split harvest delay"));
    std::this_thread::sleep_for(
        std::chrono::microseconds(nvenc_harvest_delay_us_));
}

void EncoderHwWorker::set_split_harvest_error(const std::string& message)
{
    {
        std::lock_guard<std::mutex> lock(split_harvest_mutex_);
        split_harvest_error_ = true;
        split_harvest_error_message_ = message;
        split_harvest_wake_ = true;
    }
    split_harvest_cv_.notify_all();
}

bool EncoderHwWorker::split_harvest_failed(std::string* message_out)
{
    std::lock_guard<std::mutex> lock(split_harvest_mutex_);
    if (!split_harvest_error_) {
        return false;
    }
    if (message_out) {
        *message_out = split_harvest_error_message_;
    }
    return true;
}

void EncoderHwWorker::publish_harvested_packets(std::vector<std::vector<uint8_t>> packets,
                                                 std::vector<uint64_t> output_timestamps,
                                                 uint64_t bitstream_fetch_duration_ns,
                                                 const NvEncoderEncodeFrameTiming& encode_timing)
{
    if (packets.empty()) {
        return;
    }

    RecordingOutputTimingSample timing_sample;
    if (bitstream_fetch_duration_ns > 0) {
        timing_sample.has_bitstream_fetch = true;
        timing_sample.bitstream_fetch_ns = bitstream_fetch_duration_ns;
    }
    merge_nvenc_timing(&timing_sample, encode_timing);

    std::lock_guard<std::mutex> lock(split_output_mutex_);
    const uint64_t output_accounting_start_ns = steady_clock_now_ns();
    const std::vector<uint64_t> packet_sample_indices = resolve_output_sample_indices(
        packets.size(),
        output_timestamps,
        std::nullopt,
        "SplitHarvest");
    const std::vector<uint64_t> completed_gops =
        note_emitted_packets_and_collect_completed_gops(packet_sample_indices);
    total_packets_ += packets.size();
    const int64_t fallback_sample_index = last_recording_frame_id_ > 0
        ? static_cast<int64_t>(last_recording_frame_id_ - 1)
        : 0;
    const uint64_t completion_gop_index = !packet_sample_indices.empty()
        ? packet_sample_indices.back() / std::max<uint32_t>(1u, recording_gop_length_)
        : static_cast<uint64_t>(std::max<int64_t>(0, fallback_sample_index)) /
            std::max<uint32_t>(1u, recording_gop_length_);
    const uint64_t output_accounting_end_ns = steady_clock_now_ns();
    if (output_accounting_end_ns >= output_accounting_start_ns) {
        timing_sample.encoder_output_accounting_ns +=
            output_accounting_end_ns - output_accounting_start_ns;
    }

    buffer_encoded_packets(
        packets,
        packet_sample_indices,
        fallback_sample_index,
        completion_gop_index,
        false,
        std::nullopt,
        has_recording_output_timing(timing_sample)
            ? std::optional<RecordingOutputTimingSample>(timing_sample)
            : std::nullopt);
    for (uint64_t completed_gop_index : completed_gops) {
        buffer_encoded_packets(
            {},
            {},
            fallback_sample_index,
            completed_gop_index,
            true,
            std::nullopt);
    }
    if (recording_strategy_config_.split_gop_enabled() &&
        recording_strategy_config_.split_gop.writer_queue.fail_on_overflow &&
        writer_queue_overflowed_) {
        throw std::runtime_error("FFmpeg writer queue overflowed while split_gop recording is enabled");
    }
    if (recording_strategy_config_.split_gop_enabled() && pending_gop_overflowed_) {
        throw std::runtime_error("split_gop pending GOP buffer overflowed");
    }
}

void EncoderHwWorker::split_harvest_loop()
{
    try {
        ck(cudaSetDevice(encode_gpu_id_));
        while (true) {
            {
                std::unique_lock<std::mutex> lock(split_harvest_mutex_);
                split_harvest_cv_.wait(lock, [this]() {
                    return split_harvest_stop_ || split_harvest_wake_;
                });
                if (split_harvest_stop_) {
                    break;
                }
                split_harvest_wake_ = false;
            }

            delay_split_harvest_if_configured();

            while (true) {
                std::vector<std::vector<uint8_t>> packets;
                std::vector<uint64_t> output_timestamps;
                uint64_t bitstream_fetch_duration_ns = 0;
                NvEncoderEncodeFrameTiming encode_timing;
                encoder_.pEnc->HarvestEncodedPackets(
                    packets,
                    true,
                    nullptr,
                    &output_timestamps,
                    &bitstream_fetch_duration_ns,
                    &encode_timing);
                if (packets.empty()) {
                    break;
                }
                publish_harvested_packets(
                    std::move(packets),
                    std::move(output_timestamps),
                    bitstream_fetch_duration_ns,
                    encode_timing);
            }
        }
    } catch (const std::exception& e) {
        set_split_harvest_error(e.what());
        std::cerr << "[" << threadName << "] split harvest exception: "
                  << e.what() << std::endl;
    } catch (...) {
        set_split_harvest_error("non-std exception");
        std::cerr << "[" << threadName << "] split harvest non-std exception"
                  << std::endl;
    }
}

nlohmann::json EncoderHwWorker::build_encoder_snapshot_json() const
{
    const SharedRecordingOutputStats shared_output_stats =
        shared_output_ ? shared_output_->stats() : SharedRecordingOutputStats{};
    const FFmpegWriterLatencyStats writer_latency =
        shared_output_ ? shared_output_stats.writer_latency
                       : (writer_.video ? writer_.video->latency_stats() : FFmpegWriterLatencyStats{});
    nlohmann::json info;
    info["backend"] = encoder_snapshot_.backend;
    info["path"] = encoder_snapshot_.path;
    info["split_harvest_enabled"] = split_harvest_enabled_;
    info["codec"] = encoder_snapshot_.codec;
    info["preset"] = encoder_snapshot_.preset;
    info["tuning"] = encoder_snapshot_.tuning;
    info["source_gpu_id"] = encoder_snapshot_.source_gpu_id;
    info["encode_gpu_id"] = encoder_snapshot_.encode_gpu_id;
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
    info["nvenc_extra_output_delay"] = encoder_snapshot_.nvenc_extra_output_delay;
    info["nvenc_harvest_delay_us"] = encoder_snapshot_.nvenc_harvest_delay_us;
    info["encoder_buffer_count"] = encoder_buffer_count_;
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
        {"vbv_buffer_size", encoder_snapshot_.vbv_buffer_size},
        {"qp_map_mode", {
            {"value", encoder_snapshot_.qp_map_mode},
            {"name", qp_map_mode_to_string(static_cast<NV_ENC_QP_MAP_MODE>(encoder_snapshot_.qp_map_mode))}
        }}
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
    info["requested_overrides"] = {
        {"aq", encoder_snapshot_.requested_aq},
        {"temporal_aq", encoder_snapshot_.requested_temporal_aq},
        {"lookahead", encoder_snapshot_.requested_lookahead},
        {"lookahead_depth", encoder_snapshot_.requested_lookahead_depth},
        {"target_bitrate_bps", encoder_snapshot_.requested_target_bitrate_bps},
        {"max_bitrate_bps", encoder_snapshot_.requested_max_bitrate_bps},
        {"vbv_buffer_size", encoder_snapshot_.requested_vbv_buffer_size},
        {"importance_map_mode", encoder_snapshot_.requested_importance_map_mode},
        {"importance_map_roi_size_px", encoder_snapshot_.requested_importance_map_roi_size_px}
    };
    info["importance_map"] = {
        {"requested_mode", encoder_snapshot_.requested_importance_map_mode},
        {"active_mode", encoder_snapshot_.active_importance_map_mode},
        {"enabled", importance_map_active()},
        {"shape", "square"},
        {"roi_size_px", encoder_snapshot_.importance_map_roi_size_px},
        {"block_size", encoder_snapshot_.importance_map_block_size},
        {"grid_width", encoder_snapshot_.importance_map_grid_width},
        {"grid_height", encoder_snapshot_.importance_map_grid_height},
        {"inside_delta_qp", encoder_snapshot_.importance_map_inside_delta},
        {"outside_delta_qp", encoder_snapshot_.importance_map_outside_delta},
        {"qp_map_size_bytes", importance_map_qp_delta_size_}
    };
    info["lookahead"] = {
        {"enable", encoder_snapshot_.enable_lookahead},
        {"depth", encoder_snapshot_.lookahead_depth}
    };
    info["rc"]["multi_pass"] = {
        {"value", encoder_snapshot_.multi_pass},
        {"name", multi_pass_to_string(static_cast<NV_ENC_MULTI_PASS>(encoder_snapshot_.multi_pass))}
    };
    info["low_delay_keyframe_scale"] = encoder_snapshot_.low_delay_keyframe_scale;
    info["strict_gop_target"] = encoder_snapshot_.strict_gop_target;
    info["enable_non_ref_p"] = encoder_snapshot_.enable_non_ref_p;
    info["repeat_sps_pps"] = encoder_snapshot_.repeat_sps_pps;
    info["enable_ptd"] = encoder_snapshot_.enable_ptd;
    info["recording_strategy"] = {
        {"requested_mode", encoder_snapshot_.recording_strategy.requested_mode},
        {"mode", encoder_snapshot_.recording_strategy.mode},
        {"resolution_note", encoder_snapshot_.recording_strategy.resolution_note},
        {"split_gop", {
            {"enabled", encoder_snapshot_.recording_strategy.split_gop.enabled},
            {"placement", encoder_snapshot_.recording_strategy.split_gop.placement},
            {"encoder_gpu_ids", encoder_snapshot_.recording_strategy.split_gop.encoder_gpu_ids},
            {"source_encoder_policy",
             encoder_snapshot_.recording_strategy.split_gop.source_encoder_policy},
            {"transfer_mode", encoder_snapshot_.recording_strategy.split_gop.transfer_mode},
            {"max_inflight_gops", encoder_snapshot_.recording_strategy.split_gop.max_inflight_gops},
            {"max_buffered_bytes", encoder_snapshot_.recording_strategy.split_gop.max_buffered_bytes},
            {"strict", encoder_snapshot_.recording_strategy.split_gop.strict},
            {"pending_gop_buffer", {
                {"next_gop_to_flush",
                 shared_output_ ? shared_output_stats.next_gop_to_flush : next_gop_to_flush_},
                {"current_gops", shared_output_ ? shared_output_stats.pending_gop_count : pending_gops_.size()},
                {"current_backlog_gops",
                 shared_output_ ? shared_output_stats.pending_gop_backlog_count
                                : pending_gop_backlog_count()},
                {"current_bytes",
                 shared_output_ ? shared_output_stats.pending_gop_bytes : pending_gop_buffered_bytes_},
                {"peak_gops",
                 shared_output_ ? shared_output_stats.pending_gop_peak_count : pending_gop_peak_count_},
                {"peak_backlog_gops",
                 shared_output_ ? shared_output_stats.pending_gop_peak_backlog_count
                                : pending_gop_peak_backlog_count_},
                {"peak_bytes",
                 shared_output_ ? shared_output_stats.pending_gop_peak_bytes : pending_gop_peak_bytes_},
                {"overflow_detected",
                 shared_output_ ? shared_output_stats.pending_gop_overflowed : pending_gop_overflowed_},
                {"overflow_events",
                 shared_output_ ? shared_output_stats.pending_gop_overflow_events
                                : pending_gop_overflow_events_},
                {"oldest_pending_age_ms", oldest_pending_gop_age_ms()}
            }},
            {"writer_queue", {
                {"max_packets", encoder_snapshot_.recording_strategy.split_gop.writer_queue.max_packets},
                {"max_bytes", encoder_snapshot_.recording_strategy.split_gop.writer_queue.max_bytes},
                {"overflow_detected", writer_queue_overflowed_},
                {"overflow_events", writer_queue_overflow_events_},
                {"peak_packets", writer_queue_peak_packets_},
                {"peak_bytes", writer_queue_peak_bytes_},
                {"fail_on_overflow",
                 encoder_snapshot_.recording_strategy.split_gop.writer_queue.fail_on_overflow}
            }}
        }}
    };
    const bool has_static_topology = !encoder_snapshot_.split_gop_topology_static.empty();
    const bool has_runtime_topology = !encoder_snapshot_.split_gop_topology_runtime.empty();
    if (has_static_topology || has_runtime_topology) {
        auto& topology = info["recording_strategy"]["split_gop"]["topology"];
        if (has_static_topology) {
            topology["static"] = encoder_snapshot_.split_gop_topology_static;
        }
        if (has_runtime_topology) {
            topology["runtime"] = encoder_snapshot_.split_gop_topology_runtime;
        }
        auto& runtime = topology["runtime"];
        runtime["source_to_helper_copy_samples_total"] =
            shared_output_stats.source_to_helper_copy.sample_count;
        auto topology_it = runtime.find("copy_paths");
        if (topology_it != runtime.end() && topology_it->is_array() &&
            shared_output_stats.source_to_helper_copy.sample_count > 0) {
            for (auto& copy_path : *topology_it) {
                auto& peer_access_observation = copy_path["peer_access_observation"];
                if (!peer_access_observation.is_object()) {
                    peer_access_observation = nlohmann::json::object();
                }
                const bool peer_access_required =
                    copy_path.value("peer_access_required", false);
                const bool can_access_peer =
                    copy_path.value("can_access_peer", false);
                if (!peer_access_required || !can_access_peer) {
                    continue;
                }
                if (!peer_access_observation.contains("enable_attempted")) {
                    peer_access_observation["enable_attempted"] = true;
                    peer_access_observation["enable_attempted_inferred"] = true;
                }
                if (!peer_access_observation.contains("enabled")) {
                    peer_access_observation["enabled"] = true;
                    peer_access_observation["enabled_inferred"] = true;
                }
                if (!peer_access_observation.contains("observation_source")) {
                    peer_access_observation["observation_source"] =
                        "successful_source_to_helper_copy_samples";
                }
            }
        }
    }
    if (shared_output_ && !shared_output_stats.pending_gop_overflow_reason.empty()) {
        info["recording_strategy"]["split_gop"]["pending_gop_buffer"]["last_overflow"] = {
            {"reason", shared_output_stats.pending_gop_overflow_reason},
            {"completion_gop_index",
             shared_output_stats.pending_gop_overflow_completion_gop_index},
            {"next_gop_to_flush",
             shared_output_stats.pending_gop_overflow_next_gop_to_flush},
            {"limit", shared_output_stats.pending_gop_overflow_limit},
            {"pending_count", shared_output_stats.pending_gop_overflow_pending_count},
            {"backlog_count", shared_output_stats.pending_gop_overflow_backlog_count},
            {"frontier_present", shared_output_stats.pending_gop_overflow_frontier_present},
            {"frontier_complete", shared_output_stats.pending_gop_overflow_frontier_complete},
            {"pending_keys", shared_output_stats.pending_gop_overflow_pending_keys}
        };
    }
    info["latency"] = {
        {"encoder_cuda_set_device", latency_stats_to_json(shared_output_stats.encoder_cuda_set_device)},
        {"preprocess_complete_stream_wait_enqueue", latency_stats_to_json(shared_output_stats.preprocess_complete_stream_wait_enqueue)},
        {"source_to_helper_copy_sync_wait", latency_stats_to_json(shared_output_stats.source_to_helper_copy_sync_wait)},
        {"source_to_helper_copy_elapsed_query", latency_stats_to_json(shared_output_stats.source_to_helper_copy_elapsed_query)},
        {"source_to_helper_copy", latency_stats_to_json(shared_output_stats.source_to_helper_copy)},
        {"pre_encoder_reference_capture_enqueue", latency_stats_to_json(shared_output_stats.pre_encoder_reference_capture_enqueue)},
        {"nvenc_get_next_input_frame", latency_stats_to_json(shared_output_stats.nvenc_get_next_input_frame)},
        {"bitstream_fetch", latency_stats_to_json(shared_output_stats.bitstream_fetch)},
        {"nvenc_copy_to_input", latency_stats_to_json(shared_output_stats.nvenc_copy_to_input)},
        {"nvenc_encode_frame_total", latency_stats_to_json(shared_output_stats.nvenc_encode_frame_total)},
        {"nvenc_map_input_resource", latency_stats_to_json(shared_output_stats.nvenc_map_input_resource)},
        {"nvenc_map_reference_resource", latency_stats_to_json(shared_output_stats.nvenc_map_reference_resource)},
        {"nvenc_encode_picture", latency_stats_to_json(shared_output_stats.nvenc_encode_picture)},
        {"nvenc_completion_wait", latency_stats_to_json(shared_output_stats.nvenc_completion_wait)},
        {"nvenc_lock_bitstream", latency_stats_to_json(shared_output_stats.nvenc_lock_bitstream)},
        {"nvenc_bitstream_copy", latency_stats_to_json(shared_output_stats.nvenc_bitstream_copy)},
        {"nvenc_unlock_bitstream", latency_stats_to_json(shared_output_stats.nvenc_unlock_bitstream)},
        {"nvenc_unmap_input_resource", latency_stats_to_json(shared_output_stats.nvenc_unmap_input_resource)},
        {"nvenc_unmap_reference_resource", latency_stats_to_json(shared_output_stats.nvenc_unmap_reference_resource)},
        {"encoder_output_accounting", latency_stats_to_json(shared_output_stats.encoder_output_accounting)},
        {"shared_submit_total", latency_stats_to_json(shared_output_stats.shared_submit_total)},
        {"shared_submit_lock_wait", latency_stats_to_json(shared_output_stats.shared_submit_lock_wait)},
        {"shared_gop_buffering", latency_stats_to_json(shared_output_stats.shared_gop_buffering)},
        {"gop_hold_before_release", latency_stats_to_json(shared_output_stats.gop_hold_before_release)},
        {"writer_push_packet_total", latency_stats_to_json(writer_latency.push_packet_total)},
        {"writer_packet_alloc_copy", latency_stats_to_json(writer_latency.packet_alloc_copy)},
        {"writer_queue_push", latency_stats_to_json(writer_latency.queue_push)},
        {"writer_queue_wait", latency_stats_to_json(writer_latency.queue_wait)},
        {"packet_mux_write", latency_stats_to_json(writer_latency.packet_write)},
        {"gop_release_to_last_write", latency_stats_to_json(writer_latency.gop_release_to_last_write)}
    };
    info["resolved_config"] = encoder_snapshot_.resolved_config;
    info["pre_encoder_reference_capture"] = pre_encoder_reference_writer_.BuildSummaryJson();
    return info;
}

void EncoderHwWorker::flush_and_close()
{
    if (!is_recording_ && active_recording_folder_.empty()) {
        return;
    }
    try {
        if (encoder_.pEnc) {
            stop_split_harvest_thread();
            std::vector<uint32_t> retired_slots;
            std::vector<uint64_t> output_timestamps;
            uint64_t bitstream_fetch_duration_ns = 0;
            NvEncoderEncodeFrameTiming encode_timing;
            encoder_.pEnc->EndEncode(
                encoder_.vPacket,
                direct_input_enabled_ ? &retired_slots : nullptr,
                &output_timestamps,
                &bitstream_fetch_duration_ns,
                &encode_timing);
            const std::vector<uint64_t> packet_sample_indices = resolve_output_sample_indices(
                encoder_.vPacket.size(),
                output_timestamps,
                std::nullopt,
                "EndEncode");
            const std::vector<uint64_t> completed_gops =
                note_emitted_packets_and_collect_completed_gops(packet_sample_indices);
            const int64_t fallback_sample_index = last_recording_frame_id_ > 0
                ? static_cast<int64_t>(last_recording_frame_id_ - 1)
                : 0;
            RecordingOutputTimingSample timing_sample;
            if (bitstream_fetch_duration_ns > 0) {
                timing_sample.has_bitstream_fetch = true;
                timing_sample.bitstream_fetch_ns = bitstream_fetch_duration_ns;
            }
            merge_nvenc_timing(&timing_sample, encode_timing);
            buffer_encoded_packets(
                encoder_.vPacket,
                packet_sample_indices,
                fallback_sample_index,
                static_cast<uint64_t>(fallback_sample_index) /
                    std::max<uint32_t>(1u, recording_gop_length_),
                false,
                std::nullopt,
                has_recording_output_timing(timing_sample)
                    ? std::optional<RecordingOutputTimingSample>(timing_sample)
                    : std::nullopt);
            for (uint64_t completed_gop_index : completed_gops) {
                buffer_encoded_packets(
                    {},
                    {},
                    fallback_sample_index,
                    completed_gop_index,
                    true,
                    std::nullopt);
            }
            encoder_.vPacket.clear();
            if (direct_input_enabled_ && m_prep_worker_) {
                for (uint32_t slot_id : retired_slots) {
                    m_prep_worker_->free_direct_input_slots_.push(static_cast<int>(slot_id));
                    m_prep_worker_->available_buffers_++;
                }
            }
        }
        flush_pending_gops(true);
    } catch (const std::exception& e) {
        encode_failures_++;
        std::cerr << "[" << threadName << "] flush_and_close exception: "
                  << e.what() << std::endl;
    }
    finalize_pre_encoder_reference_capture();
    release_pre_encoder_reference_capture_resources();

    if (shared_output_) {
        refresh_writer_queue_metrics();
        shared_output_->close_worker_session(owns_recording_output_);
        refresh_writer_queue_metrics();
        return;
    }

    if (writer_.video) {
        refresh_writer_queue_metrics();
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

void EncoderHwWorker::OnFlushTick()
{
    const bool recording_enabled = camera_control_->record_video;
    const bool draining = camera_control_->recording_draining;
    poll_pre_encoder_reference_captures(false);

    if (!recording_enabled && is_recording_) {
        if (!draining || drain_ready()) {
            std::cout << "[" << this->threadName << "] HW Recording stopped. Finalizing video file..." << std::endl;
            finalize_recording();
        } else {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            (void)EnqueueFlushTick();
        }
    }
}

bool EncoderHwWorker::WorkerFunction(ENCODER_WORKER_ENTRY* entry)
{
    if (!entry) {
        return false;  // defensive; flush ticks arrive via OnFlushTick()
    }

    const bool recording_enabled = camera_control_->record_video;
    const bool draining = camera_control_->recording_draining;
    poll_pre_encoder_reference_captures(false);

    // If the global flag is true but we aren't recording yet, start a new recording.
    if (recording_enabled && !is_recording_) {
        std::cout << "[" << this->threadName << "] HW Recording started. Opening new video file..." << std::endl;

        // Create a new timestamped folder
        std::string current_recording_folder;
        std::string current_session_folder;
        {
            std::lock_guard<std::mutex> lock(camera_control_->recording_folder_mutex);
            if (camera_control_->recording_folder.empty()) {
                camera_control_->recording_folder = base_folder_name_ + "/" + get_current_date_time();
            }
            current_session_folder = camera_control_->recording_folder;
            current_recording_folder = camera_control_->recording_output_folder.empty()
                ? current_session_folder
                : camera_control_->recording_output_folder;
        }
        active_recording_folder_ = current_recording_folder;
        active_recording_session_folder_ = current_session_folder;
        {
            orange::ScopedFsuid fsuid_guard;
            (void)fsuid_guard;
            make_folder(current_recording_folder);

            writer_queue_overflowed_ = false;
            writer_queue_overflow_events_ = 0;
            writer_queue_peak_packets_ = 0;
            writer_queue_peak_bytes_ = 0;
            reset_pending_gop_state();
            force_next_idr_ = true;

            VideoEncodeProfile metadata_profile = build_full_frame_video_encode_profile(
                *camera_params_,
                encode_gpu_id_,
                resolved_recording_config_);
            metadata_profile.codec = codec_;
            metadata_profile.preset = preset_;
            metadata_profile.tuning = tuning_;
            metadata_profile.rate_control_mode = rate_control_mode_;
            metadata_profile.quality_value = quality_value_;
            metadata_profile.requested_gop_length = gop_length_;
            metadata_profile.resolved_gop_length = recording_gop_length_;
            metadata_profile.encoder_control_overrides = encoder_control_overrides_;
            metadata_profile.importance_map = importance_map_config_;
            const auto metadata_tags = build_video_encode_metadata_tags(metadata_profile);
            FFmpegWriterQueueConfig queue_config;
            if (recording_strategy_config_.split_gop_enabled()) {
                queue_config.max_queued_packets =
                    static_cast<size_t>(recording_strategy_config_.split_gop.writer_queue.max_packets);
                queue_config.max_queued_bytes =
                    static_cast<size_t>(recording_strategy_config_.split_gop.writer_queue.max_bytes);
            }
            if (shared_output_) {
                SharedRecordingOutputOpenParams open_params;
                open_params.camera_params = camera_params_;
                open_params.recording_output_config = recording_output_config_;
                open_params.folder_name = current_recording_folder;
                open_params.codec = codec_;
                open_params.metadata_tags = metadata_tags;
                open_params.queue_config = queue_config;
                open_params.split_gop_config = recording_strategy_config_.split_gop;
                open_params.recording_gop_length = recording_gop_length_;
                shared_output_->open_if_needed(open_params);
            } else {
                initialize_writer_hw(
                    &writer_,
                    camera_params_,
                    recording_output_config_,
                    current_recording_folder,
                    codec_,
                    metadata_tags,
                    queue_config);
            }
        }
        if (owns_recording_output_) {
            initialize_pre_encoder_reference_capture();
        }
        if (owns_recording_output_ && encoder_snapshot_valid_) {
            const std::string camera_key = camera_params_->camera_serial.empty()
                ? std::to_string(camera_params_->camera_id)
                : camera_params_->camera_serial;
            nlohmann::json encoder_info = build_encoder_snapshot_json();
            if (!current_session_folder.empty() && current_session_folder != current_recording_folder) {
                update_recording_snapshot_encoder(current_session_folder, camera_key, encoder_info);
            } else {
                update_recording_snapshot_encoder(current_recording_folder, camera_key, encoder_info);
            }
        }
        if (owns_recording_output_) {
            camera_control_->active_recorders.fetch_add(1, std::memory_order_relaxed);
        }
        is_recording_ = true;
        start_split_harvest_thread();
    }

    // If recording is globally disabled, skip processing but recycle resources.
    if (!recording_enabled && !draining) {
        recycle_encoder_entry(entry, {}, false);
        return false;
    }

    if (!is_recording_) {
        std::cerr << "[" << this->threadName << "] Warning: Dropping frame because encoder is not recording." << std::endl;
        recycle_encoder_entry(entry, {}, false);
        return false;
    }
    if (entry->surface_gpu_id >= 0 && entry->surface_gpu_id != encode_gpu_id_) {
        throw std::runtime_error(
            "Encoder worker on GPU " + std::to_string(encode_gpu_id_) +
            " received a prepared frame resident on GPU " + std::to_string(entry->surface_gpu_id));
    }

    const uint64_t zero_based_recording_frame =
        entry->recording_frame_id > 0 ? entry->recording_frame_id - 1 : 0;
    prepare_requested_rollover(entry->recording_frame_id);
    uint64_t force_rollover_request_id = 0;
    {
        std::lock_guard<std::mutex> lock(camera_control_->recording_folder_mutex);
        if (camera_control_->recording_rollover_request_id > 0 &&
            camera_control_->recording_rollover_request_id != forced_rollover_request_id_ &&
            camera_control_->recording_rollover_at_frame_id > 0 &&
            entry->recording_frame_id >= camera_control_->recording_rollover_at_frame_id) {
            force_rollover_request_id = camera_control_->recording_rollover_request_id;
        }
    }
    entry->gop_index = zero_based_recording_frame / recording_gop_length_;
    entry->frame_index_within_gop =
        static_cast<uint32_t>(zero_based_recording_frame % recording_gop_length_);
    entry->is_last_frame_in_gop =
        entry->frame_index_within_gop + 1 == recording_gop_length_;

    auto start_time = std::chrono::steady_clock::now();
    frame_counter_++;
    auto now = std::chrono::steady_clock::now();
    std::chrono::duration<double> elapsed = now - last_fps_update_time_;
    if (elapsed.count() >= 1.0) {
        current_fps_.store(frame_counter_ / elapsed.count(), std::memory_order_relaxed);
        frame_counter_ = 0;
        last_fps_update_time_ = now;
    }

    RecordingOutputTimingSample timing_sample;
    {
        NVTX_ENCODE_DYNAMIC(std::string("Encoder worker cudaSetDevice"));
        const uint64_t cuda_set_device_start_ns = steady_clock_now_ns();
        ck(cudaSetDevice(encode_gpu_id_));
        const uint64_t cuda_set_device_end_ns = steady_clock_now_ns();
        if (cuda_set_device_end_ns >= cuda_set_device_start_ns) {
            timing_sample.encoder_cuda_set_device_ns +=
                cuda_set_device_end_ns - cuda_set_device_start_ns;
        }
    }
    std::vector<uint32_t> retired_slots;
    bool capture_scheduled = false;
    size_t capture_staging_slot = 0;
    size_t capture_frame_size = 0;
    bool slot_submitted = false;
    NV_ENC_PIC_PARAMS pic_params = { NV_ENC_PIC_PARAMS_VER };
    pic_params.frameIdx = static_cast<uint32_t>(zero_based_recording_frame & 0xffffffffu);
    pic_params.inputTimeStamp = zero_based_recording_frame;
    pic_params.inputDuration = 1;
    const bool force_idr_for_frame = force_next_idr_ || force_rollover_request_id > 0;
    if (force_idr_for_frame) {
        pic_params.encodePicFlags |= NV_ENC_PIC_FLAG_FORCEIDR;
        pic_params.encodePicFlags |= NV_ENC_PIC_FLAG_OUTPUT_SPSPPS;
    }

    if (importance_map_active()) {
        pic_params.qpDeltaMap = importance_map_qp_delta_.data();
        pic_params.qpDeltaMapSize = static_cast<uint32_t>(importance_map_qp_delta_size_);
    }
    NV_ENC_PIC_PARAMS* pic_params_ptr = &pic_params;

    try {
        NVTX_ENCODE_DYNAMIC([&]() {
            std::ostringstream oss;
            oss << "Full-frame encode/output"
                << " cam=" << camera_params_->camera_serial
                << " src_gpu=" << resolved_recording_config_.source_gpu_id
                << " enc_gpu=" << encode_gpu_id_
                << " route="
                << (encode_gpu_id_ == resolved_recording_config_.source_gpu_id ? "source" : "helper")
                << " rec_frame=" << entry->recording_frame_id
                << " gop=" << entry->gop_index;
            return oss.str();
        }());
        std::vector<uint64_t> output_timestamps;
        if (split_harvest_enabled_) {
            std::string harvest_error;
            if (split_harvest_failed(&harvest_error)) {
                throw std::runtime_error("NVENC split harvest failed: " + harvest_error);
            }
        }
        if (entry->preprocess_complete_event) {
            NVTX_SYNC_DYNAMIC(std::string("Encoder preprocess event stream wait enqueue"));
            const uint64_t preprocess_wait_start_ns = steady_clock_now_ns();
            ck(cudaStreamWaitEvent(m_stream, *entry->preprocess_complete_event, 0));
            const uint64_t preprocess_wait_end_ns = steady_clock_now_ns();
            if (preprocess_wait_end_ns >= preprocess_wait_start_ns) {
                timing_sample.preprocess_complete_stream_wait_enqueue_ns +=
                    preprocess_wait_end_ns - preprocess_wait_start_ns;
            }
        }
        if (entry->cross_gpu_copy_performed && entry->copy_start_event && entry->copy_end_event) {
            float copy_ms = 0.0f;
            {
                NVTX_SYNC_DYNAMIC(std::string("Encoder source-to-helper copy sync wait"));
                const uint64_t copy_sync_start_ns = steady_clock_now_ns();
                ck(cudaEventSynchronize(entry->copy_end_event));
                const uint64_t copy_sync_end_ns = steady_clock_now_ns();
                if (copy_sync_end_ns >= copy_sync_start_ns) {
                    timing_sample.source_to_helper_copy_sync_wait_ns +=
                        copy_sync_end_ns - copy_sync_start_ns;
                }
            }
            {
                NVTX_SYNC_DYNAMIC(std::string("Encoder source-to-helper copy elapsed query"));
                const uint64_t copy_elapsed_query_start_ns = steady_clock_now_ns();
                ck(cudaEventElapsedTime(&copy_ms, entry->copy_start_event, entry->copy_end_event));
                const uint64_t copy_elapsed_query_end_ns = steady_clock_now_ns();
                if (copy_elapsed_query_end_ns >= copy_elapsed_query_start_ns) {
                    timing_sample.source_to_helper_copy_elapsed_query_ns +=
                        copy_elapsed_query_end_ns - copy_elapsed_query_start_ns;
                }
            }
            timing_sample.has_source_to_helper_copy = true;
            timing_sample.source_to_helper_copy_ns =
                static_cast<uint64_t>(copy_ms * 1000000.0f);
        }
        {
            NVTX_GPU_COPY_DYNAMIC(std::string("Pre-encoder reference capture enqueue"));
            const uint64_t reference_capture_start_ns = steady_clock_now_ns();
            capture_scheduled = begin_pre_encoder_reference_capture(
                entry, &capture_staging_slot, &capture_frame_size);
            const uint64_t reference_capture_end_ns = steady_clock_now_ns();
            if (reference_capture_end_ns >= reference_capture_start_ns) {
                timing_sample.pre_encoder_reference_capture_enqueue_ns +=
                    reference_capture_end_ns - reference_capture_start_ns;
            }
        }

        uint64_t bitstream_fetch_duration_ns = 0;
        NvEncoderEncodeFrameTiming encode_timing;
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
            {
                const uint64_t encode_frame_start_ns = steady_clock_now_ns();
                encoder_.pEnc->EncodeFrame(
                    encoder_.vPacket,
                    pic_params_ptr,
                    &retired_slots,
                    &output_timestamps,
                    &bitstream_fetch_duration_ns,
                    &encode_timing);
                const uint64_t encode_frame_end_ns = steady_clock_now_ns();
                force_next_idr_ = false;
                if (force_rollover_request_id > 0) {
                    forced_rollover_request_id_ = force_rollover_request_id;
                }
                if (encode_frame_end_ns >= encode_frame_start_ns) {
                    timing_sample.nvenc_encode_frame_total_ns +=
                        encode_frame_end_ns - encode_frame_start_ns;
                }
            }
            slot_submitted = true;
        } else {
            const NvEncInputFrame* encoderInputFrame = nullptr;
            {
                NVTX_ENCODE_DYNAMIC(std::string("NVENC get next input frame"));
                const uint64_t get_next_input_start_ns = steady_clock_now_ns();
                if (split_harvest_enabled_) {
                    while (!encoder_.pEnc->WaitForNextInputFrameAvailable(100)) {
                        std::string harvest_error;
                        if (split_harvest_failed(&harvest_error)) {
                            throw std::runtime_error("NVENC split harvest failed: " + harvest_error);
                        }
                    }
                }
                encoderInputFrame = encoder_.pEnc->GetNextInputFrame();
                const uint64_t get_next_input_end_ns = steady_clock_now_ns();
                if (get_next_input_end_ns >= get_next_input_start_ns) {
                    timing_sample.nvenc_get_next_input_frame_ns +=
                        get_next_input_end_ns - get_next_input_start_ns;
                }
            }
            
            if (!encoderInputFrame) {
                encode_failures_++;
                std::cerr << "[PERF WARNING] " << threadName
                          << ": Failed to get encoder input frame!" << std::endl;
                throw std::runtime_error("No encoder input frame available");
            }

            {
                NVTX_GPU_COPY_DYNAMIC(std::string("NVENC input CopyToDeviceFrame"));
                const uint64_t copy_to_input_start_ns = steady_clock_now_ns();
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
                const uint64_t copy_to_input_end_ns = steady_clock_now_ns();
                if (copy_to_input_end_ns >= copy_to_input_start_ns) {
                    timing_sample.nvenc_copy_to_input_ns +=
                        copy_to_input_end_ns - copy_to_input_start_ns;
                }
            }

            {
                const uint64_t encode_frame_start_ns = steady_clock_now_ns();
                if (split_harvest_enabled_) {
                    encoder_.vPacket.clear();
                    encoder_.pEnc->SubmitFrameOnly(
                        pic_params_ptr,
                        &encode_timing);
                } else {
                    encoder_.pEnc->EncodeFrame(
                        encoder_.vPacket,
                        pic_params_ptr,
                        nullptr,
                        &output_timestamps,
                        &bitstream_fetch_duration_ns,
                        &encode_timing);
                }
                const uint64_t encode_frame_end_ns = steady_clock_now_ns();
                force_next_idr_ = false;
                if (force_rollover_request_id > 0) {
                    forced_rollover_request_id_ = force_rollover_request_id;
                }
                if (encode_frame_end_ns >= encode_frame_start_ns) {
                    timing_sample.nvenc_encode_frame_total_ns +=
                        encode_frame_end_ns - encode_frame_start_ns;
                }
            }
        }
        if (bitstream_fetch_duration_ns > 0) {
            timing_sample.has_bitstream_fetch = true;
            timing_sample.bitstream_fetch_ns = bitstream_fetch_duration_ns;
        }
        merge_nvenc_timing(&timing_sample, encode_timing);

        const std::optional<RecordingMetadataRow> metadata_row = RecordingMetadataRow{
            entry->recording_frame_id,
            entry->timestamp,
            entry->timestamp_sys};

        size_t packets_generated = 0;
        if (split_harvest_enabled_) {
            std::lock_guard<std::mutex> lock(split_output_mutex_);
            const uint64_t output_accounting_start_ns = steady_clock_now_ns();
            note_submitted_frame(entry->gop_index, entry->is_last_frame_in_gop);
            resolve_output_sample_indices(
                0,
                {},
                zero_based_recording_frame,
                "SplitSubmit");
            const uint64_t output_accounting_end_ns = steady_clock_now_ns();
            if (output_accounting_end_ns >= output_accounting_start_ns) {
                timing_sample.encoder_output_accounting_ns +=
                    output_accounting_end_ns - output_accounting_start_ns;
            }
            buffer_encoded_packets(
                {},
                {},
                static_cast<int64_t>(zero_based_recording_frame),
                entry->gop_index,
                false,
                metadata_row,
                has_recording_output_timing(timing_sample)
                    ? std::optional<RecordingOutputTimingSample>(timing_sample)
                    : std::nullopt);
            if (recording_strategy_config_.split_gop_enabled() &&
                recording_strategy_config_.split_gop.writer_queue.fail_on_overflow &&
                writer_queue_overflowed_) {
                throw std::runtime_error("FFmpeg writer queue overflowed while split_gop recording is enabled");
            }
            if (recording_strategy_config_.split_gop_enabled() && pending_gop_overflowed_) {
                throw std::runtime_error("split_gop pending GOP buffer overflowed");
            }
            last_recording_frame_id_ = std::max<uint64_t>(
                last_recording_frame_id_,
                zero_based_recording_frame + 1);
            notify_split_harvest_thread();
        } else {
            const uint64_t output_accounting_start_ns = steady_clock_now_ns();
            note_submitted_frame(entry->gop_index, entry->is_last_frame_in_gop);
            packets_generated = encoder_.vPacket.size();
            const std::vector<uint64_t> packet_sample_indices = resolve_output_sample_indices(
                packets_generated,
                output_timestamps,
                zero_based_recording_frame,
                "EncodeFrame");
            const std::vector<uint64_t> completed_gops =
                note_emitted_packets_and_collect_completed_gops(packet_sample_indices);
            total_packets_ += packets_generated;
            const uint64_t output_accounting_end_ns = steady_clock_now_ns();
            if (output_accounting_end_ns >= output_accounting_start_ns) {
                timing_sample.encoder_output_accounting_ns +=
                    output_accounting_end_ns - output_accounting_start_ns;
            }
            buffer_encoded_packets(
                encoder_.vPacket,
                packet_sample_indices,
                static_cast<int64_t>(zero_based_recording_frame),
                entry->gop_index,
                false,
                metadata_row,
                has_recording_output_timing(timing_sample)
                    ? std::optional<RecordingOutputTimingSample>(timing_sample)
                    : std::nullopt);
            for (uint64_t completed_gop_index : completed_gops) {
                buffer_encoded_packets(
                    {},
                    {},
                    static_cast<int64_t>(zero_based_recording_frame),
                    completed_gop_index,
                    true,
                    std::nullopt);
            }
            if (recording_strategy_config_.split_gop_enabled() &&
                recording_strategy_config_.split_gop.writer_queue.fail_on_overflow &&
                writer_queue_overflowed_) {
                throw std::runtime_error("FFmpeg writer queue overflowed while split_gop recording is enabled");
            }
            if (recording_strategy_config_.split_gop_enabled() && pending_gop_overflowed_) {
                throw std::runtime_error("split_gop pending GOP buffer overflowed");
            }

            if (!shared_output_) {
                write_metadata_hw(writer_.metadata, entry->recording_frame_id, entry->timestamp, entry->timestamp_sys);
            }
            last_recording_frame_id_ = std::max<uint64_t>(
                last_recording_frame_id_,
                zero_based_recording_frame + 1);
        }

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
                retired_slots,
                slot_submitted});
    } else {
        recycle_encoder_entry(entry, retired_slots, slot_submitted);
    }

    auto end_time = std::chrono::steady_clock::now();
    auto duration_us = std::chrono::duration_cast<std::chrono::microseconds>(end_time - start_time).count();
    
    if (duration_us > 12500) {
        slow_frames_++;
    }
    
    return false;
}
