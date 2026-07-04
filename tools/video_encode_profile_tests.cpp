#include "camera.h"
#include "video_encode_profile.h"
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

namespace {
void require(bool condition, const std::string& message)
{
    if (!condition) {
        std::cerr << "FAIL: " << message << std::endl;
        std::exit(1);
    }
}

std::string tag_value(const std::vector<std::pair<std::string, std::string>>& tags,
                      const std::string& key)
{
    for (const auto& tag : tags) {
        if (tag.first == key) {
            return tag.second;
        }
    }
    return {};
}

void require_contains(const std::string& value,
                      const std::string& expected,
                      const std::string& message)
{
    require(value.find(expected) != std::string::npos, message);
}

CameraParams make_camera()
{
    CameraParams camera{};
    camera.width = 4512;
    camera.height = 4512;
    camera.frame_rate = 100;
    camera.gpu_id = 5;
    camera.camera_serial = "2010096";
    camera.color = false;
    return camera;
}

ResolvedRecordingConfig make_full_config()
{
    ResolvedRecordingConfig config;
    config.source_gpu_id = 5;
    config.recording_gpu_id = 5;
    config.encode.codec = "hevc";
    config.encode.preset = "p1";
    config.encode.tuning = "ll";
    config.encode.rate_control_mode = "vbr";
    config.encode.quality_value = 20;
    config.encode.gop_length = 0;
    config.output.mode = "factor";
    config.output.downsample_factor = 1;
    config.output.resolved_width = 4512;
    config.output.resolved_height = 4512;
    config.output.resize_enabled = false;
    return config;
}

NV_ENC_CONFIG apply_to_config(const VideoEncodeProfile& profile,
                              NV_ENC_INITIALIZE_PARAMS* initialize_params_out = nullptr)
{
    NV_ENC_INITIALIZE_PARAMS initialize_params = {NV_ENC_INITIALIZE_PARAMS_VER};
    NV_ENC_CONFIG encode_config = {NV_ENC_CONFIG_VER};
    apply_video_encode_profile_to_nvenc_config(profile, &initialize_params, &encode_config);
    if (initialize_params_out) {
        *initialize_params_out = initialize_params;
    }
    return encode_config;
}

void test_full_profile_defaults()
{
    const CameraParams camera = make_camera();
    const ResolvedRecordingConfig config = make_full_config();
    const VideoEncodeProfile profile =
        build_full_frame_video_encode_profile(camera, 5, config);
    NV_ENC_INITIALIZE_PARAMS initialize_params = {NV_ENC_INITIALIZE_PARAMS_VER};
    const NV_ENC_CONFIG encode_config = apply_to_config(profile, &initialize_params);

    require(profile.output_kind == "full", "full profile output kind");
    require(profile.role == "ingest_authoritative", "full profile role");
    require(profile.codec == "hevc", "full profile codec");
    require(profile.preset == "p1", "full profile preset");
    require(profile.tuning == "ll", "full profile tuning");
    require(profile.resolved_gop_length == 100, "auto GOP resolves to fps");
    require(initialize_params.encodeWidth == 4512, "full init width");
    require(initialize_params.encodeHeight == 4512, "full init height");
    require(initialize_params.frameRateNum == 100, "full init fps");
    require(encode_config.gopLength == 100, "full config GOP");
    require(encode_config.frameIntervalP == 1, "full frame interval P");
    require(encode_config.rcParams.rateControlMode == NV_ENC_PARAMS_RC_VBR, "full VBR rc mode");
    require(encode_config.rcParams.averageBitRate == 150000000U, "full bitrate clamps to max");
    require(encode_config.rcParams.maxBitRate == 150000000U, "full max bitrate clamps to max");
    require(encode_config.rcParams.enableAQ == 1, "full default AQ on");
    require(encode_config.rcParams.enableTemporalAQ == 1, "full default temporal AQ on");
    require(encode_config.rcParams.enableLookahead == 0, "low-latency lookahead off");
    require(encode_config.monoChromeEncoding == 1, "mono full output marks monochrome");
    require(encode_config.encodeCodecConfig.hevcConfig.idrPeriod == 100, "HEVC IDR follows GOP");
    require(encode_config.encodeCodecConfig.hevcConfig.repeatSPSPPS == 1, "HEVC repeats SPS/PPS");
    require(
        encode_config.encodeCodecConfig.hevcConfig.hevcVUIParameters.videoSignalTypePresentFlag == 1,
        "HEVC VUI video signal type present");
    require(
        encode_config.encodeCodecConfig.hevcConfig.hevcVUIParameters.videoFullRangeFlag == 1,
        "HEVC VUI full range flag");

    const auto tags = build_video_encode_metadata_tags(profile);
    const std::string comment = tag_value(tags, "comment");
    require_contains(
        comment,
        "source_pixel_contract=orange.camera.mono8.full_frame.v1",
        "full comment source pixel contract");
    require_contains(
        comment,
        "source_pixel_format=mono8",
        "full comment source pixel format");
    require_contains(
        comment,
        "source_transform_to_encoder=mono8_to_nv12",
        "full comment source transform");
    require_contains(
        comment,
        "encoded_color_range=pc",
        "full comment encoded color range");
    require_contains(
        comment,
        "output_kind=full",
        "full comment output kind");

    const nlohmann::json source_contract =
        build_video_source_pixel_contract_json(profile);
    require(
        source_contract.at("id") == "orange.camera.mono8.full_frame.v1",
        "full source contract json id");
    require(source_contract.at("width") == 4512, "full source contract json width");
}

void test_full_profile_overrides()
{
    CameraParams camera = make_camera();
    camera.width = 640;
    camera.height = 480;
    camera.frame_rate = 100;
    ResolvedRecordingConfig config = make_full_config();
    config.output.resolved_width = 640;
    config.output.resolved_height = 480;
    config.encode.rate_control_mode = "cqp";
    config.encode.quality_value = 999;
    config.encode.gop_length = 25;
    config.encoder_control_overrides.aq = 0;
    config.encoder_control_overrides.temporal_aq = 0;
    const VideoEncodeProfile profile =
        build_full_frame_video_encode_profile(camera, 6, config);
    const NV_ENC_CONFIG encode_config = apply_to_config(profile);

    require(profile.quality_value == 51, "quality clamps to max");
    require(profile.encode_gpu_id == 6, "encode GPU override captured");
    require(profile.resolved_gop_length == 25, "explicit GOP retained");
    require(encode_config.gopLength == 25, "explicit GOP applied");
    require(encode_config.rcParams.rateControlMode == NV_ENC_PARAMS_RC_CONSTQP, "CQP rc mode");
    require(encode_config.rcParams.constQP.qpInterP == 51, "CQP P QP");
    require(encode_config.rcParams.constQP.qpInterB == 51, "CQP B QP");
    require(encode_config.rcParams.constQP.qpIntra == 51, "CQP I QP");
    require(encode_config.rcParams.enableAQ == 0, "AQ override off");
    require(encode_config.rcParams.enableTemporalAQ == 0, "temporal AQ override off");
}

void test_lossless_full_profile()
{
    const CameraParams camera = make_camera();
    ResolvedRecordingConfig config = make_full_config();
    config.encode.tuning = "lossless";
    config.encode.gop_length = 25;
    const VideoEncodeProfile profile =
        build_full_frame_video_encode_profile(camera, 5, config);
    const NV_ENC_CONFIG encode_config = apply_to_config(profile);

    require(profile.resolved_gop_length == 1, "lossless full resolves GOP 1");
    require(encode_config.gopLength == 1, "lossless full applies GOP 1");
    require(encode_config.rcParams.rateControlMode == NV_ENC_PARAMS_RC_CONSTQP, "lossless constqp");
    require(encode_config.rcParams.constQP.qpInterP == 0, "lossless P QP 0");
    require(encode_config.rcParams.constQP.qpInterB == 0, "lossless B QP 0");
    require(encode_config.rcParams.constQP.qpIntra == 0, "lossless I QP 0");
    require(encode_config.rcParams.enableAQ == 0, "lossless AQ off");
    require(encode_config.rcParams.enableTemporalAQ == 0, "lossless temporal AQ off");
}

void test_crop_profile()
{
    const CameraParams camera = make_camera();
    const VideoEncodeProfile profile = build_crop_video_encode_profile(camera, 256, 256);
    NV_ENC_INITIALIZE_PARAMS initialize_params = {NV_ENC_INITIALIZE_PARAMS_VER};
    const NV_ENC_CONFIG encode_config = apply_to_config(profile, &initialize_params);
    const VideoEncodeProfileNvencGuids guids =
        resolve_video_encode_profile_nvenc_guids(profile);
    const auto tags = build_video_encode_metadata_tags(profile);
    const std::string comment = tag_value(tags, "comment");
    const nlohmann::json metadata =
        build_video_metadata_json(profile, "/tmp/Cam2010096_crop.mp4", "2010096_crop");

    require(profile.output_kind == "crop", "crop profile output kind");
    require(profile.role == "sidecar", "crop profile role");
    require(profile.codec == "hevc", "crop codec");
    require(profile.preset == "p7", "crop preset");
    require(profile.tuning == "lossless", "crop tuning");
    require(profile.resolved_gop_length == 1, "crop GOP 1");
    require(guids.tuning_info == NV_ENC_TUNING_INFO_LOSSLESS, "crop tuning GUID");
    require(initialize_params.encodeWidth == 256, "crop init width");
    require(initialize_params.encodeHeight == 256, "crop init height");
    require(initialize_params.frameRateNum == 100, "crop fps");
    require(encode_config.gopLength == 1, "crop config GOP");
    require(encode_config.frameIntervalP == 1, "crop frame interval");
    require(encode_config.rcParams.rateControlMode == NV_ENC_PARAMS_RC_CONSTQP, "crop constqp");
    require(encode_config.rcParams.constQP.qpInterP == 0, "crop P QP 0");
    require(encode_config.rcParams.constQP.qpInterB == 0, "crop B QP 0");
    require(encode_config.rcParams.constQP.qpIntra == 0, "crop I QP 0");
    require(
        encode_config.encodeCodecConfig.hevcConfig.hevcVUIParameters.videoSignalTypePresentFlag == 1,
        "crop HEVC VUI video signal type present");
    require(
        encode_config.encodeCodecConfig.hevcConfig.hevcVUIParameters.videoFullRangeFlag == 1,
        "crop HEVC VUI full range flag");
    require(!tags.empty() && tags[0].second == "Cam2010096 crop", "crop metadata title");
    require_contains(
        comment,
        "source_pixel_contract=orange.crop.mono8.v1",
        "crop comment source pixel contract");
    require_contains(
        comment,
        "source_pixel_format=mono8",
        "crop comment source pixel format");
    require_contains(
        comment,
        "source_transform_to_encoder=crop_mono8_to_nv12",
        "crop comment source transform");
    require_contains(
        comment,
        "encoded_color_range=pc",
        "crop comment encoded color range");
    require(
        metadata.at("source_pixel_contract").at("id") == "orange.crop.mono8.v1",
        "crop metadata json source contract id");
    require(
        metadata.at("mp4_tags_expected").at("title") == "Cam2010096 crop",
        "crop metadata json expected title");
    require(
        metadata.at("mp4_metadata_embedding").at("validated_with_ffprobe") == false,
        "crop metadata json ffprobe validation flag");
}

void test_normalization()
{
    require(normalize_video_encode_codec("vp9") == "h264", "unknown codec fallback");
    require(normalize_video_encode_preset("P5") == "p5", "preset lowercases");
    require(normalize_video_encode_preset("slow") == "p3", "unknown preset fallback");
    require(normalize_video_encode_tuning("ULL") == "ull", "tuning lowercases");
    require(normalize_video_encode_tuning("fast") == "hq", "unknown tuning fallback");
    require(normalize_video_encode_rate_control_mode("") == "vbr", "empty rc fallback");
    require(clamp_video_encode_quality_value(-4) == 20, "low quality fallback");
    require(clamp_video_encode_quality_value(80) == 51, "high quality clamp");
}
} // namespace

int main()
{
    test_full_profile_defaults();
    test_full_profile_overrides();
    test_lossless_full_profile();
    test_crop_profile();
    test_normalization();
    std::cout << "video_encode_profile_tests passed" << std::endl;
    return 0;
}
