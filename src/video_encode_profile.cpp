// src/video_encode_profile.cpp

#include "video_encode_profile.h"
#include "camera.h"
#include <algorithm>
#include <cctype>
#include <iomanip>
#include <sstream>

namespace {
constexpr uint64_t kMinQualityBitrate = 10000000ULL;
constexpr uint64_t kMaxQualityBitrate = 150000000ULL;
constexpr double kColorTargetBpp = 0.15;
constexpr double kMonoTargetBpp = 0.10;
constexpr int kDefaultQualityValue = 20;
constexpr int kMinQualityValue = 1;
constexpr int kMaxQualityValue = 51;
constexpr int kMaxLookaheadDepth = 32;
constexpr uint32_t kVuiVideoFormatUnspecified = 5;

std::string lower_ascii(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return value;
}

uint32_t clamp_bitrate(uint64_t value)
{
    if (value < kMinQualityBitrate) {
        return static_cast<uint32_t>(kMinQualityBitrate);
    }
    if (value > kMaxQualityBitrate) {
        return static_cast<uint32_t>(kMaxQualityBitrate);
    }
    return static_cast<uint32_t>(value);
}

uint32_t saturate_positive_u32(int value)
{
    if (value <= 0) {
        return 0;
    }
    return static_cast<uint32_t>(value);
}

bool is_vbr_cq_rate_control(const std::string& rate_control_mode)
{
    return rate_control_mode == "vbr_cq";
}

bool is_cbr_rate_control(const std::string& rate_control_mode)
{
    return rate_control_mode == "cbr";
}

bool is_cqp_rate_control(const std::string& rate_control_mode)
{
    return rate_control_mode == "cqp";
}

std::string normalize_importance_map_mode_string(std::string value)
{
    value = lower_ascii(std::move(value));
    if (value.empty() || value == "off" || value == "none" || value == "disabled") {
        return "off";
    }
    if (value == "static_roi" || value == "static-roi" ||
        value == "static_prior" || value == "static-prior" || value == "static") {
        return "static_roi";
    }
    return value;
}

int normalize_importance_map_roi_size_px(int value)
{
    return value > 0 ? value : ImportanceMapConfig::kDefaultRoiSizePx;
}

uint32_t estimate_profile_target_bitrate(const VideoEncodeProfile& profile)
{
    if (profile.encoder_control_overrides.target_bitrate_bps > 0) {
        return saturate_positive_u32(profile.encoder_control_overrides.target_bitrate_bps);
    }

    RecordingOutputConfig output_config;
    output_config.mode = profile.output_mode;
    output_config.downsample_factor = profile.downsample_factor;
    output_config.requested_width = profile.requested_output_width;
    output_config.requested_height = profile.requested_output_height;
    output_config.resolved_width = static_cast<int>(profile.width);
    output_config.resolved_height = static_cast<int>(profile.height);
    output_config.resize_enabled = profile.resize_enabled;

    CameraParams camera_params{};
    camera_params.width = profile.source_width;
    camera_params.height = profile.source_height;
    camera_params.frame_rate = profile.fps;
    camera_params.gpu_id = profile.source_gpu_id;
    camera_params.color = profile.color;
    return estimate_recording_bitrate(camera_params, output_config).average_bitrate;
}

double estimate_profile_target_bpp(const VideoEncodeProfile& profile,
                                   uint32_t target_bps)
{
    const double denominator =
        static_cast<double>(profile.width) *
        static_cast<double>(profile.height) *
        static_cast<double>(profile.fps);
    if (denominator <= 0.0) {
        return 0.0;
    }
    return static_cast<double>(target_bps) / denominator;
}

VideoSourcePixelContract derived_video_source_pixel_contract(
    const VideoEncodeProfile& profile)
{
    VideoSourcePixelContract contract;
    contract.encoder_input_format =
        profile.input_format.empty() ? "nv12" : profile.input_format;
    contract.encoded_pix_fmt = "yuv420p";
    contract.encoded_color_range = "pc";

    if (profile.output_kind == "crop") {
        contract.id = "orange.crop.mono8.v1";
        contract.pixel_format = "mono8";
        contract.color_space = "linear_gray";
        contract.channel_order = "gray";
        contract.memory_layout = "HxW";
        contract.width = profile.width;
        contract.height = profile.height;
        contract.source_origin = "analytics_crop";
        contract.transform_to_encoder = "crop_mono8_to_nv12";
        return contract;
    }

    const std::string source_format =
        profile.source_format.empty()
            ? (profile.color ? "rgb8" : "mono8")
            : lower_ascii(profile.source_format);
    const bool rgb_source = source_format == "rgb8" || source_format == "bgr8";
    contract.id = rgb_source
        ? "orange.camera.rgb8.full_frame.v1"
        : "orange.camera.mono8.full_frame.v1";
    contract.pixel_format = rgb_source ? "rgb8" : "mono8";
    contract.color_space = rgb_source ? "srgb" : "linear_gray";
    contract.channel_order = rgb_source ? "rgb" : "gray";
    contract.memory_layout = rgb_source ? "HxWxC_interleaved" : "HxW";
    contract.width = profile.source_width > 0 ? profile.source_width : profile.width;
    contract.height = profile.source_height > 0 ? profile.source_height : profile.height;
    contract.source_origin = "camera_dma";
    const std::string transform_base = rgb_source ? "rgb8_to_nv12" : "mono8_to_nv12";
    contract.transform_to_encoder =
        profile.resize_enabled ? "resize_then_" + transform_base : transform_base;
    return contract;
}

VideoSourcePixelContract merge_video_source_pixel_contract(
    VideoSourcePixelContract base,
    const VideoSourcePixelContract& override_contract)
{
    if (!override_contract.id.empty()) {
        base.id = override_contract.id;
    }
    if (!override_contract.pixel_format.empty()) {
        base.pixel_format = override_contract.pixel_format;
    }
    if (!override_contract.dtype.empty()) {
        base.dtype = override_contract.dtype;
    }
    if (!override_contract.value_range.empty()) {
        base.value_range = override_contract.value_range;
    }
    if (!override_contract.color_space.empty()) {
        base.color_space = override_contract.color_space;
    }
    if (!override_contract.channel_order.empty()) {
        base.channel_order = override_contract.channel_order;
    }
    if (!override_contract.memory_layout.empty()) {
        base.memory_layout = override_contract.memory_layout;
    }
    if (override_contract.width > 0) {
        base.width = override_contract.width;
    }
    if (override_contract.height > 0) {
        base.height = override_contract.height;
    }
    if (!override_contract.coordinate_origin.empty()) {
        base.coordinate_origin = override_contract.coordinate_origin;
    }
    if (!override_contract.source_origin.empty()) {
        base.source_origin = override_contract.source_origin;
    }
    if (!override_contract.transform_to_encoder.empty()) {
        base.transform_to_encoder = override_contract.transform_to_encoder;
    }
    if (!override_contract.encoder_input_format.empty()) {
        base.encoder_input_format = override_contract.encoder_input_format;
    }
    if (!override_contract.encoded_pix_fmt.empty()) {
        base.encoded_pix_fmt = override_contract.encoded_pix_fmt;
    }
    if (!override_contract.encoded_color_range.empty()) {
        base.encoded_color_range = override_contract.encoded_color_range;
    }
    return base;
}

void append_source_pixel_comment_fields(std::ostringstream& comment,
                                        const VideoEncodeProfile& profile)
{
    const VideoSourcePixelContract contract =
        resolve_video_source_pixel_contract(profile);
    comment << "; source_pixel_contract=" << contract.id
            << "; source_pixel_format=" << contract.pixel_format
            << "; source_pixel_dtype=" << contract.dtype
            << "; source_pixel_range=" << contract.value_range
            << "; source_color_space=" << contract.color_space
            << "; source_channel_order=" << contract.channel_order
            << "; source_memory_layout=" << contract.memory_layout
            << "; source_width=" << contract.width
            << "; source_height=" << contract.height
            << "; source_coordinate_origin=" << contract.coordinate_origin
            << "; source_origin=" << contract.source_origin
            << "; source_transform_to_encoder=" << contract.transform_to_encoder
            << "; encoder_input_format=" << contract.encoder_input_format
            << "; encoded_pix_fmt=" << contract.encoded_pix_fmt
            << "; encoded_color_range=" << contract.encoded_color_range;
}

nlohmann::json metadata_tags_to_json(
    const std::vector<std::pair<std::string, std::string>>& tags)
{
    nlohmann::json out = nlohmann::json::object();
    for (const auto& tag : tags) {
        out[tag.first] = tag.second;
    }
    return out;
}

void apply_quality_recording_profile(NV_ENC_CONFIG& encode_config,
                                     const VideoEncodeProfile& profile,
                                     bool low_latency)
{
    RecordingOutputConfig output_config;
    output_config.mode = profile.output_mode;
    output_config.downsample_factor = profile.downsample_factor;
    output_config.requested_width = profile.requested_output_width;
    output_config.requested_height = profile.requested_output_height;
    output_config.resolved_width = static_cast<int>(profile.width);
    output_config.resolved_height = static_cast<int>(profile.height);
    output_config.resize_enabled = profile.resize_enabled;

    CameraParams camera_params{};
    camera_params.width = profile.source_width;
    camera_params.height = profile.source_height;
    camera_params.frame_rate = profile.fps;
    camera_params.gpu_id = profile.source_gpu_id;
    camera_params.color = profile.color;

    const RecordingBitrateEstimate estimate =
        estimate_recording_bitrate(camera_params, output_config);
    encode_config.rcParams.rateControlMode = NV_ENC_PARAMS_RC_VBR;
    encode_config.rcParams.averageBitRate = estimate.average_bitrate;
    encode_config.rcParams.maxBitRate = estimate.max_bitrate;
    encode_config.rcParams.vbvBufferSize = encode_config.rcParams.maxBitRate;

    encode_config.rcParams.enableAQ = 1;
    encode_config.rcParams.enableTemporalAQ = 1;
    encode_config.rcParams.enableLookahead = low_latency ? 0 : 1;
    encode_config.rcParams.lowDelayKeyFrameScale = low_latency ? 1 : 0;
}

void apply_vbr_cq_recording_profile(NV_ENC_CONFIG& encode_config,
                                    const VideoEncodeProfile& profile,
                                    bool low_latency)
{
    apply_quality_recording_profile(encode_config, profile, low_latency);
    encode_config.rcParams.targetQuality =
        static_cast<uint8_t>(clamp_video_encode_quality_value(profile.quality_value));
    encode_config.rcParams.targetQualityLSB = 0;
}

void apply_cbr_recording_profile(NV_ENC_CONFIG& encode_config,
                                 const VideoEncodeProfile& profile,
                                 bool low_latency)
{
    RecordingOutputConfig output_config;
    output_config.mode = profile.output_mode;
    output_config.downsample_factor = profile.downsample_factor;
    output_config.requested_width = profile.requested_output_width;
    output_config.requested_height = profile.requested_output_height;
    output_config.resolved_width = static_cast<int>(profile.width);
    output_config.resolved_height = static_cast<int>(profile.height);
    output_config.resize_enabled = profile.resize_enabled;

    CameraParams camera_params{};
    camera_params.width = profile.source_width;
    camera_params.height = profile.source_height;
    camera_params.frame_rate = profile.fps;
    camera_params.gpu_id = profile.source_gpu_id;
    camera_params.color = profile.color;

    const RecordingBitrateEstimate estimate =
        estimate_recording_bitrate(camera_params, output_config);
    encode_config.rcParams.rateControlMode = NV_ENC_PARAMS_RC_CBR;
    encode_config.rcParams.averageBitRate = estimate.average_bitrate;
    encode_config.rcParams.maxBitRate = estimate.average_bitrate;
    encode_config.rcParams.vbvBufferSize = estimate.average_bitrate;

    encode_config.rcParams.enableAQ = 1;
    encode_config.rcParams.enableTemporalAQ = 1;
    encode_config.rcParams.enableLookahead = low_latency ? 0 : 1;
    encode_config.rcParams.lowDelayKeyFrameScale = low_latency ? 1 : 0;
}

void apply_cqp_recording_profile(NV_ENC_CONFIG& encode_config,
                                 const VideoEncodeProfile& profile)
{
    const uint8_t qp =
        static_cast<uint8_t>(clamp_video_encode_quality_value(profile.quality_value));
    encode_config.rcParams.rateControlMode = NV_ENC_PARAMS_RC_CONSTQP;
    encode_config.rcParams.constQP = {qp, qp, qp};
    encode_config.rcParams.averageBitRate = 0;
    encode_config.rcParams.maxBitRate = 0;
    encode_config.rcParams.vbvBufferSize = 0;
    encode_config.rcParams.targetQuality = 0;
    encode_config.rcParams.targetQualityLSB = 0;
    encode_config.rcParams.enableAQ = 0;
    encode_config.rcParams.enableTemporalAQ = 0;
    encode_config.rcParams.enableLookahead = 0;
    encode_config.rcParams.lowDelayKeyFrameScale = 0;
}

void apply_encoder_control_overrides(NV_ENC_CONFIG& encode_config,
                                     const EncoderControlOverrides& overrides)
{
    const bool explicit_target_bitrate = overrides.target_bitrate_bps > 0;
    const bool explicit_max_bitrate = overrides.max_bitrate_bps > 0;
    const bool explicit_vbv = overrides.vbv_buffer_size > 0;

    if (overrides.aq >= 0) {
        encode_config.rcParams.enableAQ = overrides.aq ? 1U : 0U;
    }
    if (overrides.temporal_aq >= 0) {
        encode_config.rcParams.enableTemporalAQ = overrides.temporal_aq ? 1U : 0U;
    }
    if (overrides.lookahead >= 0) {
        encode_config.rcParams.enableLookahead = overrides.lookahead ? 1U : 0U;
        if (!overrides.lookahead) {
            encode_config.rcParams.lookaheadDepth = 0;
        }
    }
    if (overrides.lookahead_depth >= 0) {
        encode_config.rcParams.lookaheadDepth = static_cast<uint16_t>(
            std::clamp(overrides.lookahead_depth, 0, kMaxLookaheadDepth));
        if (overrides.lookahead_depth > 0 && overrides.lookahead != 0) {
            encode_config.rcParams.enableLookahead = 1;
        }
    }
    if (explicit_target_bitrate) {
        encode_config.rcParams.averageBitRate =
            saturate_positive_u32(overrides.target_bitrate_bps);
    }
    if (explicit_max_bitrate) {
        encode_config.rcParams.maxBitRate =
            saturate_positive_u32(overrides.max_bitrate_bps);
    }
    if (explicit_vbv) {
        encode_config.rcParams.vbvBufferSize =
            saturate_positive_u32(overrides.vbv_buffer_size);
    }
    if (encode_config.rcParams.rateControlMode == NV_ENC_PARAMS_RC_CBR &&
        explicit_target_bitrate) {
        if (!explicit_max_bitrate) {
            encode_config.rcParams.maxBitRate = encode_config.rcParams.averageBitRate;
        }
        if (!explicit_vbv) {
            encode_config.rcParams.vbvBufferSize = encode_config.rcParams.averageBitRate;
        }
    }
}
} // namespace

int clamp_video_encode_quality_value(int value)
{
    if (value < kMinQualityValue) {
        return kDefaultQualityValue;
    }
    if (value > kMaxQualityValue) {
        return kMaxQualityValue;
    }
    return value;
}

std::string normalize_video_encode_codec(std::string value)
{
    value = lower_ascii(std::move(value));
    return value == "hevc" ? "hevc" : "h264";
}

std::string normalize_video_encode_preset(std::string value)
{
    value = lower_ascii(std::move(value));
    if (value == "p1" || value == "p3" || value == "p5" || value == "p7") {
        return value;
    }
    return "p3";
}

std::string normalize_video_encode_tuning(std::string value)
{
    value = lower_ascii(std::move(value));
    if (value == "ll" || value == "ull" || value == "lossless" || value == "hq") {
        return value;
    }
    return "hq";
}

std::string normalize_video_encode_rate_control_mode(std::string value)
{
    value = lower_ascii(std::move(value));
    if (value == "vbr" || value == "vbr_cq" || value == "cbr" || value == "cqp") {
        return value;
    }
    return "vbr";
}

bool video_encode_tuning_is_low_latency(const std::string& tuning)
{
    return tuning == "ll" || tuning == "ull";
}

bool video_encode_tuning_is_lossless(const std::string& tuning)
{
    return tuning == "lossless";
}

std::string resolve_video_encode_rate_control_strategy(const std::string& tuning,
                                                       const std::string& rate_control_mode)
{
    if (video_encode_tuning_is_lossless(tuning)) {
        return "lossless";
    }
    if (is_cqp_rate_control(rate_control_mode)) {
        return "cqp";
    }
    if (is_cbr_rate_control(rate_control_mode)) {
        return "cbr";
    }
    if (is_vbr_cq_rate_control(rate_control_mode)) {
        return "vbr_cq";
    }
    return "vbr";
}

RecordingBitrateEstimate estimate_recording_bitrate(
    const CameraParams& camera_params,
    const RecordingOutputConfig& recording_output_config)
{
    RecordingBitrateEstimate estimate;
    estimate.target_bpp = camera_params.color ? kColorTargetBpp : kMonoTargetBpp;

    const double unclamped_bits_per_sec =
        static_cast<double>(recording_output_config.resolved_width) *
        static_cast<double>(recording_output_config.resolved_height) *
        static_cast<double>(camera_params.frame_rate) *
        estimate.target_bpp;
    const uint64_t unclamped_average_bitrate =
        static_cast<uint64_t>(unclamped_bits_per_sec);
    estimate.average_clamped_to_min = unclamped_average_bitrate < kMinQualityBitrate;
    estimate.average_clamped_to_max = unclamped_average_bitrate > kMaxQualityBitrate;
    estimate.average_bitrate = clamp_bitrate(unclamped_average_bitrate);

    uint64_t unclamped_max_bitrate =
        static_cast<uint64_t>(estimate.average_bitrate) * 3 / 2;
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
    if (video_encode_tuning_is_lossless(normalize_video_encode_tuning(tuning))) {
        return 1;
    }

    const int sanitized_gop_length = sanitize_recording_gop_length(requested_gop_length);
    if (sanitized_gop_length > 0) {
        return static_cast<uint32_t>(sanitized_gop_length);
    }

    return std::max<uint32_t>(1u, camera_params.frame_rate);
}

VideoEncodeProfile build_full_frame_video_encode_profile(
    const CameraParams& camera_params,
    int encode_gpu_id,
    const ResolvedRecordingConfig& resolved_recording_config)
{
    VideoEncodeProfile profile;
    profile.name = "full_hevc_low_latency";
    profile.output_kind = "full";
    profile.role = "ingest_authoritative";
    profile.camera_serial = camera_params.camera_serial;
    profile.codec = normalize_video_encode_codec(resolved_recording_config.encode.codec);
    profile.preset = normalize_video_encode_preset(resolved_recording_config.encode.preset);
    profile.tuning = normalize_video_encode_tuning(resolved_recording_config.encode.tuning);
    profile.rate_control_mode =
        normalize_video_encode_rate_control_mode(resolved_recording_config.encode.rate_control_mode);
    profile.output_mode = resolved_recording_config.output.mode;
    profile.quality_value =
        clamp_video_encode_quality_value(resolved_recording_config.encode.quality_value);
    profile.requested_gop_length =
        sanitize_recording_gop_length(resolved_recording_config.encode.gop_length);
    profile.width = static_cast<uint32_t>(resolved_recording_config.output.resolved_width);
    profile.height = static_cast<uint32_t>(resolved_recording_config.output.resolved_height);
    profile.source_width = camera_params.width;
    profile.source_height = camera_params.height;
    profile.fps = camera_params.frame_rate;
    profile.downsample_factor = resolved_recording_config.output.downsample_factor;
    profile.requested_output_width = resolved_recording_config.output.requested_width;
    profile.requested_output_height = resolved_recording_config.output.requested_height;
    profile.resize_enabled = resolved_recording_config.output.resize_enabled;
    profile.color = camera_params.color;
    profile.source_gpu_id = camera_params.gpu_id;
    profile.encode_gpu_id = encode_gpu_id >= 0 ? encode_gpu_id : camera_params.gpu_id;
    profile.encoder_control_overrides =
        resolved_recording_config.encoder_control_overrides;
    profile.importance_map = resolved_recording_config.importance_map;
    profile.source_format = camera_params.color ? "rgb8" : "mono8";
    profile.resolved_gop_length =
        resolve_recording_gop_length(
            camera_params,
            profile.tuning,
            profile.requested_gop_length);
    profile.source_pixel_contract = resolve_video_source_pixel_contract(profile);
    return profile;
}

VideoEncodeProfile build_crop_video_encode_profile(
    const CameraParams& camera_params,
    int crop_width,
    int crop_height)
{
    VideoEncodeProfile profile;
    profile.name = "crop_hevc_lossless_gop1";
    profile.output_kind = "crop";
    profile.role = "runtime_derived_acquisition_input";
    profile.camera_serial = camera_params.camera_serial;
    profile.codec = "hevc";
    profile.preset = "p7";
    profile.tuning = "lossless";
    profile.rate_control_mode = "cqp";
    profile.output_mode = "crop";
    profile.quality_value = 0;
    profile.requested_gop_length = 1;
    profile.resolved_gop_length = 1;
    profile.width = static_cast<uint32_t>(std::max(0, crop_width));
    profile.height = static_cast<uint32_t>(std::max(0, crop_height));
    profile.source_width = camera_params.width;
    profile.source_height = camera_params.height;
    profile.fps = camera_params.frame_rate;
    profile.color = false;
    profile.source_gpu_id = camera_params.gpu_id;
    profile.encode_gpu_id = camera_params.gpu_id;
    profile.source_format = "mono8";
    profile.source_pixel_contract = resolve_video_source_pixel_contract(profile);
    return profile;
}

VideoEncodeProfileNvencGuids resolve_video_encode_profile_nvenc_guids(
    const VideoEncodeProfile& profile)
{
    VideoEncodeProfileNvencGuids guids;
    guids.codec_guid =
        profile.codec == "hevc" ? NV_ENC_CODEC_HEVC_GUID : NV_ENC_CODEC_H264_GUID;
    if (profile.preset == "p1") {
        guids.preset_guid = NV_ENC_PRESET_P1_GUID;
    } else if (profile.preset == "p5") {
        guids.preset_guid = NV_ENC_PRESET_P5_GUID;
    } else if (profile.preset == "p7") {
        guids.preset_guid = NV_ENC_PRESET_P7_GUID;
    } else {
        guids.preset_guid = NV_ENC_PRESET_P3_GUID;
    }

    if (profile.tuning == "ll") {
        guids.tuning_info = NV_ENC_TUNING_INFO_LOW_LATENCY;
    } else if (profile.tuning == "ull") {
        guids.tuning_info = NV_ENC_TUNING_INFO_ULTRA_LOW_LATENCY;
    } else if (profile.tuning == "lossless") {
        guids.tuning_info = NV_ENC_TUNING_INFO_LOSSLESS;
    } else {
        guids.tuning_info = NV_ENC_TUNING_INFO_HIGH_QUALITY;
    }
    return guids;
}

void apply_video_encode_profile_to_nvenc_config(
    const VideoEncodeProfile& profile,
    NV_ENC_INITIALIZE_PARAMS* initialize_params,
    NV_ENC_CONFIG* encode_config)
{
    if (!initialize_params || !encode_config) {
        return;
    }

    initialize_params->encodeConfig = encode_config;
    initialize_params->encodeWidth = profile.width;
    initialize_params->encodeHeight = profile.height;
    initialize_params->frameRateNum = profile.fps;
    initialize_params->frameRateDen = 1;
    initialize_params->enablePTD = 1;
    initialize_params->tuningInfo =
        resolve_video_encode_profile_nvenc_guids(profile).tuning_info;
    if (profile.output_kind == "full") {
        initialize_params->darWidth = profile.width;
        initialize_params->darHeight = profile.height;
        initialize_params->enableWeightedPrediction = 0;
    }

    encode_config->gopLength = profile.resolved_gop_length;
    encode_config->frameIntervalP = 1;
    apply_full_range_video_signal_to_nvenc_config(profile.codec, encode_config);

    if (profile.output_kind == "crop") {
        encode_config->gopLength = 1;
        encode_config->frameIntervalP = 1;
        encode_config->rcParams.rateControlMode = NV_ENC_PARAMS_RC_CONSTQP;
        encode_config->rcParams.constQP = {0, 0, 0};
        encode_config->rcParams.averageBitRate = 0;
        encode_config->rcParams.maxBitRate = 0;
        encode_config->rcParams.vbvBufferSize = 0;
        encode_config->rcParams.targetQuality = 0;
        encode_config->rcParams.targetQualityLSB = 0;
        encode_config->rcParams.enableAQ = 0;
        encode_config->rcParams.enableTemporalAQ = 0;
        encode_config->rcParams.enableLookahead = 0;
        encode_config->rcParams.lowDelayKeyFrameScale = 0;
    } else {
        const bool low_latency = video_encode_tuning_is_low_latency(profile.tuning);
        const bool lossless = video_encode_tuning_is_lossless(profile.tuning);
        if (lossless) {
            encode_config->rcParams.rateControlMode = NV_ENC_PARAMS_RC_CONSTQP;
            encode_config->rcParams.constQP = {0, 0, 0};
            encode_config->rcParams.averageBitRate = 0;
            encode_config->rcParams.maxBitRate = 0;
            encode_config->rcParams.vbvBufferSize = 0;
            encode_config->rcParams.targetQuality = 0;
            encode_config->rcParams.targetQualityLSB = 0;
            encode_config->rcParams.enableAQ = 0;
            encode_config->rcParams.enableTemporalAQ = 0;
            encode_config->rcParams.enableLookahead = 0;
            encode_config->rcParams.lowDelayKeyFrameScale = 0;
            encode_config->gopLength = 1;
            encode_config->frameIntervalP = 1;
        } else if (is_cqp_rate_control(profile.rate_control_mode)) {
            apply_cqp_recording_profile(*encode_config, profile);
        } else if (is_cbr_rate_control(profile.rate_control_mode)) {
            apply_cbr_recording_profile(*encode_config, profile, low_latency);
        } else if (is_vbr_cq_rate_control(profile.rate_control_mode)) {
            apply_vbr_cq_recording_profile(*encode_config, profile, low_latency);
        } else {
            apply_quality_recording_profile(*encode_config, profile, low_latency);
        }
    }

    encode_config->rcParams.enableMinQP = 0;
    encode_config->rcParams.enableMaxQP = 0;
    encode_config->rcParams.strictGOPTarget = 0;
    encode_config->rcParams.enableNonRefP = 0;
    apply_encoder_control_overrides(*encode_config, profile.encoder_control_overrides);

    if (profile.codec == "hevc") {
        auto& hevc_config = encode_config->encodeCodecConfig.hevcConfig;
        hevc_config.pixelBitDepthMinus8 = 0;
        hevc_config.idrPeriod = encode_config->gopLength;
        hevc_config.sliceMode = 0;
        hevc_config.sliceModeData = 0;
        hevc_config.maxNumRefFramesInDPB = 1;
        hevc_config.repeatSPSPPS = 1;
        hevc_config.outputBufferingPeriodSEI = 0;
        hevc_config.outputPictureTimingSEI = 0;
        hevc_config.outputAUD = 0;
        hevc_config.enableLTR = 0;
    } else {
        auto& h264_config = encode_config->encodeCodecConfig.h264Config;
        h264_config.idrPeriod = encode_config->gopLength;
        h264_config.sliceMode = 0;
        h264_config.sliceModeData = 0;
        h264_config.repeatSPSPPS = 1;
        h264_config.maxNumRefFrames = 1;
        h264_config.adaptiveTransformMode = NV_ENC_H264_ADAPTIVE_TRANSFORM_DISABLE;
        h264_config.bdirectMode = NV_ENC_H264_BDIRECT_MODE_DISABLE;
        h264_config.entropyCodingMode = NV_ENC_H264_ENTROPY_CODING_MODE_CAVLC;
    }

    if (!profile.color) {
        encode_config->monoChromeEncoding = 1;
    }
}

void apply_full_range_video_signal_to_nvenc_config(
    const std::string& codec,
    NV_ENC_CONFIG* encode_config)
{
    if (!encode_config) {
        return;
    }

    if (normalize_video_encode_codec(codec) == "hevc") {
        auto& vui = encode_config->encodeCodecConfig.hevcConfig.hevcVUIParameters;
        vui.videoSignalTypePresentFlag = 1;
        vui.videoFormat = kVuiVideoFormatUnspecified;
        vui.videoFullRangeFlag = 1;
    } else {
        auto& vui = encode_config->encodeCodecConfig.h264Config.h264VUIParameters;
        vui.videoSignalTypePresentFlag = 1;
        vui.videoFormat = kVuiVideoFormatUnspecified;
        vui.videoFullRangeFlag = 1;
    }
}

VideoSourcePixelContract resolve_video_source_pixel_contract(
    const VideoEncodeProfile& profile)
{
    return merge_video_source_pixel_contract(
        derived_video_source_pixel_contract(profile),
        profile.source_pixel_contract);
}

nlohmann::json build_video_source_pixel_contract_json(
    const VideoEncodeProfile& profile)
{
    const VideoSourcePixelContract contract =
        resolve_video_source_pixel_contract(profile);
    return nlohmann::json{
        {"id", contract.id},
        {"pixel_format", contract.pixel_format},
        {"dtype", contract.dtype},
        {"value_range", contract.value_range},
        {"color_space", contract.color_space},
        {"channel_order", contract.channel_order},
        {"memory_layout", contract.memory_layout},
        {"width", contract.width},
        {"height", contract.height},
        {"coordinate_origin", contract.coordinate_origin},
        {"source_origin", contract.source_origin},
        {"transform_to_encoder", contract.transform_to_encoder},
        {"encoder_input_format", contract.encoder_input_format},
        {"encoded_pix_fmt", contract.encoded_pix_fmt},
        {"encoded_color_range", contract.encoded_color_range}
    };
}

nlohmann::json build_video_encoder_metadata_json(
    const VideoEncodeProfile& profile)
{
    const std::string rc_strategy =
        resolve_video_encode_rate_control_strategy(profile.tuning, profile.rate_control_mode);
    nlohmann::json encoder = {
        {"name", "nvenc"},
        {"profile_name", profile.name},
        {"codec", profile.codec},
        {"preset", profile.preset},
        {"tuning", profile.tuning},
        {"rate_control_mode", profile.rate_control_mode},
        {"rate_control_strategy", rc_strategy},
        {"quality_value", profile.quality_value},
        {"requested_gop_length", profile.requested_gop_length},
        {"resolved_gop_length", profile.resolved_gop_length},
        {"fps", profile.fps},
        {"input_format", profile.input_format},
        {"source_format", profile.source_format},
        {"output_width", profile.width},
        {"output_height", profile.height},
        {"source_width", profile.source_width},
        {"source_height", profile.source_height},
        {"output_kind", profile.output_kind},
        {"output_mode", profile.output_mode},
        {"color", profile.color},
        {"source_gpu_id", profile.source_gpu_id},
        {"encode_gpu_id", profile.encode_gpu_id},
        {"resize_enabled", profile.resize_enabled},
        {"downsample_factor", profile.downsample_factor},
        {"requested_output_width", profile.requested_output_width},
        {"requested_output_height", profile.requested_output_height}
    };

    if (rc_strategy == "lossless") {
        encoder["qp"] = 0;
    } else if (rc_strategy == "cqp") {
        encoder["qp"] = profile.quality_value;
    } else {
        const uint32_t target_bps = estimate_profile_target_bitrate(profile);
        encoder["target_bps"] = target_bps;
        encoder["target_bpp"] = estimate_profile_target_bpp(profile, target_bps);
    }
    if (profile.encoder_control_overrides.max_bitrate_bps > 0) {
        encoder["max_bps"] = profile.encoder_control_overrides.max_bitrate_bps;
    }
    if (profile.encoder_control_overrides.vbv_buffer_size > 0) {
        encoder["vbv_buffer_size"] = profile.encoder_control_overrides.vbv_buffer_size;
    }
    encoder["aq"] = profile.encoder_control_overrides.aq;
    encoder["temporal_aq"] = profile.encoder_control_overrides.temporal_aq;
    encoder["lookahead"] = profile.encoder_control_overrides.lookahead;
    encoder["lookahead_depth"] = profile.encoder_control_overrides.lookahead_depth;

    const std::string importance_map_mode =
        normalize_importance_map_mode_string(profile.importance_map.mode);
    encoder["importance_map"] = {
        {"mode", importance_map_mode},
        {"roi_size_px", normalize_importance_map_roi_size_px(
             profile.importance_map.roi_size_px)}
    };
    return encoder;
}

nlohmann::json build_video_metadata_json(
    const VideoEncodeProfile& profile,
    const std::string& video_path,
    const std::string& stream_id,
    bool mp4_metadata_attempted,
    bool mp4_metadata_succeeded,
    const std::string& mp4_metadata_reason)
{
    nlohmann::json out = {
        {"schema_id", "orange.video_metadata"},
        {"schema_version", profile.output_kind == "crop" ? 2 : 1},
        {"video_path", video_path},
        {"stream_id", stream_id},
        {"camera_serial", profile.camera_serial},
        {"output_kind", profile.output_kind},
        {"role", profile.role},
        {"encoder", build_video_encoder_metadata_json(profile)},
        {"source_pixel_contract", build_video_source_pixel_contract_json(profile)},
        {"geometry", {
            {"output_width", profile.width},
            {"output_height", profile.height},
            {"source_width", profile.source_width},
            {"source_height", profile.source_height},
            {"fps", profile.fps}
        }},
        {"mp4_tags_expected", metadata_tags_to_json(
             build_video_encode_metadata_tags(profile))},
        {"mp4_metadata_embedding", {
            {"attempted", mp4_metadata_attempted},
            {"succeeded", mp4_metadata_succeeded},
            {"reason", mp4_metadata_reason},
            {"validated_with_ffprobe", false}
        }}
    };
    if (profile.output_kind == "crop") {
        out["video_pixel_coordinate_space"] = "crop_frame_pixels";
        out["source_geometry_coordinate_space"] = "full_frame_pixels";
        // Compatibility alias for readers of the pre-split coordinate field.
        out["coordinate_space"] = "full_frame_pixels";
    }
    return out;
}

std::vector<std::pair<std::string, std::string>> build_video_encode_metadata_tags(
    const VideoEncodeProfile& profile)
{
    std::vector<std::pair<std::string, std::string>> tags;

    std::ostringstream title;
    title << "Cam" << profile.camera_serial;
    if (profile.output_kind == "crop") {
        title << " crop";
    }
    tags.emplace_back("title", title.str());

    std::ostringstream comment;
    comment << "nvenc codec=" << profile.codec
            << "; preset=" << profile.preset
            << "; tuning=" << profile.tuning
            << "; res=" << profile.width << "x" << profile.height
            << "; fps=" << profile.fps
            << "; color=" << (profile.color ? 1 : 0)
            << "; gop=" << profile.resolved_gop_length;
    append_source_pixel_comment_fields(comment, profile);

    if (profile.output_kind == "crop") {
        comment << "; output_kind=crop"
                << "; role=runtime_derived_acquisition_input"
                << "; input_format=" << profile.input_format
                << "; source_format=" << profile.source_format
                << "; coordinate_space=full_frame_pixels"
                << "; video_pixel_coordinate_space=crop_frame_pixels"
                << "; source_geometry_coordinate_space=full_frame_pixels"
                << "; selection_policy=largest_detection_by_confidence"
                << "; blank_frame_policy=encode_black_frame_when_no_detection";
        tags.emplace_back("comment", comment.str());
        return tags;
    }

    comment << "; output_kind=" << profile.output_kind
            << "; output_mode=" << profile.output_mode;
    if (profile.resize_enabled) {
        comment << "; source_res=" << profile.source_width << "x" << profile.source_height;
    }

    const std::string rc_strategy =
        resolve_video_encode_rate_control_strategy(profile.tuning, profile.rate_control_mode);
    if (rc_strategy == "lossless") {
        comment << "; rc=constqp; qp=0";
    } else if (rc_strategy == "cqp") {
        comment << "; rc=constqp; qp=" << profile.quality_value;
    } else {
        const uint32_t target_bps = estimate_profile_target_bitrate(profile);
        const double actual_bpp = estimate_profile_target_bpp(profile, target_bps);
        if (rc_strategy == "cbr") {
            comment << "; rc=cbr; bpp=" << std::fixed << std::setprecision(3)
                    << actual_bpp << "; target_bps=" << target_bps;
        } else if (rc_strategy == "vbr_cq") {
            comment << "; rc=vbr; cq=" << profile.quality_value
                    << "; bpp_cap=" << std::fixed << std::setprecision(3)
                    << actual_bpp << "; target_bps=" << target_bps;
        } else {
            comment << "; rc=vbr; bpp=" << std::fixed << std::setprecision(3)
                    << actual_bpp << "; target_bps=" << target_bps;
        }
    }

    if (profile.encoder_control_overrides.max_bitrate_bps > 0) {
        comment << "; max_bps=" << profile.encoder_control_overrides.max_bitrate_bps;
    }
    if (profile.encoder_control_overrides.vbv_buffer_size > 0) {
        comment << "; vbv=" << profile.encoder_control_overrides.vbv_buffer_size;
    }
    const std::string importance_map_mode =
        normalize_importance_map_mode_string(profile.importance_map.mode);
    if (importance_map_mode != "off") {
        comment << "; importance_map=" << importance_map_mode
                << "; importance_map_roi_size_px="
                << normalize_importance_map_roi_size_px(profile.importance_map.roi_size_px);
    }
    if (profile.output_mode == "factor") {
        comment << "; factor=" << profile.downsample_factor;
    } else {
        comment << "; requested_res=" << profile.requested_output_width
                << "x" << profile.requested_output_height;
    }

    tags.emplace_back("comment", comment.str());
    return tags;
}

std::string video_encode_profile_summary(const VideoEncodeProfile& profile)
{
    std::ostringstream out;
    out << "name=" << profile.name
        << " output=" << profile.output_kind
        << " role=" << profile.role
        << " codec=" << profile.codec
        << " preset=" << profile.preset
        << " tuning=" << profile.tuning
        << " rc=" << resolve_video_encode_rate_control_strategy(
               profile.tuning, profile.rate_control_mode)
        << " res=" << profile.width << "x" << profile.height
        << " fps=" << profile.fps
        << " gop=" << profile.resolved_gop_length;
    if (profile.output_kind == "full") {
        out << " color=" << (profile.color ? 1 : 0)
            << " source_gpu=" << profile.source_gpu_id
            << " encode_gpu=" << profile.encode_gpu_id;
    }
    return out.str();
}
