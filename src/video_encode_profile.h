// src/video_encode_profile.h

#ifndef ORANGE_VIDEO_ENCODE_PROFILE_H
#define ORANGE_VIDEO_ENCODE_PROFILE_H

#include "encoder_pipeline.h"
#include "NvEncoder/NvEncoder.h"
#include "json.hpp"
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

struct CameraParams;

struct RecordingBitrateEstimate {
    uint32_t average_bitrate = 0;
    uint32_t max_bitrate = 0;
    double target_bpp = 0.0;
    bool average_clamped_to_min = false;
    bool average_clamped_to_max = false;
    bool max_clamped_to_max = false;
};

struct VideoSourcePixelContract {
    std::string id;
    std::string pixel_format;
    std::string dtype = "uint8";
    std::string value_range = "0_255";
    std::string color_space;
    std::string channel_order;
    std::string memory_layout;
    uint32_t width = 0;
    uint32_t height = 0;
    std::string coordinate_origin = "top_left";
    std::string source_origin;
    std::string transform_to_encoder;
    std::string encoder_input_format = "nv12";
    std::string encoded_pix_fmt = "yuv420p";
    std::string encoded_color_range = "pc";
};

struct VideoEncodeProfile {
    std::string name;
    std::string output_kind;
    std::string role;
    std::string camera_serial;
    std::string codec;
    std::string preset;
    std::string tuning;
    std::string rate_control_mode;
    std::string output_mode;
    std::string input_format = "nv12";
    std::string source_format = "mono8";
    int quality_value = 20;
    int requested_gop_length = 0;
    uint32_t resolved_gop_length = 1;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t source_width = 0;
    uint32_t source_height = 0;
    uint32_t fps = 0;
    int downsample_factor = 1;
    int requested_output_width = 0;
    int requested_output_height = 0;
    bool resize_enabled = false;
    bool color = false;
    int source_gpu_id = -1;
    int encode_gpu_id = -1;
    VideoSourcePixelContract source_pixel_contract;
    EncoderControlOverrides encoder_control_overrides;
    ImportanceMapConfig importance_map;
};

struct VideoEncodeProfileNvencGuids {
    GUID codec_guid = NV_ENC_CODEC_H264_GUID;
    GUID preset_guid = NV_ENC_PRESET_P3_GUID;
    NV_ENC_TUNING_INFO tuning_info = NV_ENC_TUNING_INFO_HIGH_QUALITY;
};

int clamp_video_encode_quality_value(int value);
std::string normalize_video_encode_codec(std::string value);
std::string normalize_video_encode_preset(std::string value);
std::string normalize_video_encode_tuning(std::string value);
std::string normalize_video_encode_rate_control_mode(std::string value);
bool video_encode_tuning_is_low_latency(const std::string& tuning);
bool video_encode_tuning_is_lossless(const std::string& tuning);
std::string resolve_video_encode_rate_control_strategy(const std::string& tuning,
                                                       const std::string& rate_control_mode);
RecordingBitrateEstimate estimate_recording_bitrate(
    const CameraParams& camera_params,
    const RecordingOutputConfig& recording_output_config);
int sanitize_recording_gop_length(int requested_gop_length);
uint32_t resolve_recording_gop_length(const CameraParams& camera_params,
                                      const std::string& tuning,
                                      int requested_gop_length);

VideoEncodeProfile build_full_frame_video_encode_profile(
    const CameraParams& camera_params,
    int encode_gpu_id,
    const ResolvedRecordingConfig& resolved_recording_config);
VideoEncodeProfile build_crop_video_encode_profile(
    const CameraParams& camera_params,
    int crop_width,
    int crop_height);
VideoEncodeProfileNvencGuids resolve_video_encode_profile_nvenc_guids(
    const VideoEncodeProfile& profile);
void apply_video_encode_profile_to_nvenc_config(
    const VideoEncodeProfile& profile,
    NV_ENC_INITIALIZE_PARAMS* initialize_params,
    NV_ENC_CONFIG* encode_config);
void apply_full_range_video_signal_to_nvenc_config(
    const std::string& codec,
    NV_ENC_CONFIG* encode_config);
VideoSourcePixelContract resolve_video_source_pixel_contract(
    const VideoEncodeProfile& profile);
nlohmann::json build_video_source_pixel_contract_json(
    const VideoEncodeProfile& profile);
nlohmann::json build_video_encoder_metadata_json(
    const VideoEncodeProfile& profile);
nlohmann::json build_video_metadata_json(
    const VideoEncodeProfile& profile,
    const std::string& video_path,
    const std::string& stream_id,
    bool mp4_metadata_attempted = true,
    bool mp4_metadata_succeeded = true,
    const std::string& mp4_metadata_reason = std::string());
std::vector<std::pair<std::string, std::string>> build_video_encode_metadata_tags(
    const VideoEncodeProfile& profile);
std::string video_encode_profile_summary(const VideoEncodeProfile& profile);

#endif // ORANGE_VIDEO_ENCODE_PROFILE_H
