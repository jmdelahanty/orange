#include "spatial_roi_lossless_encoder.h"

#include "json.hpp"
#include "shaman_v2_recording_identity.h"
#include "spatial_roi_recorder_artifact_root.h"
#include "video_container_finalization.h"

#include <cuda_runtime.h>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/pixfmt.h>
}

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <unistd.h>
#include <utility>
#include <vector>

namespace {

bool expect(bool condition, const std::string& message);

using orange::spatial_roi::SpatialRoiFrameDescriptor;
using orange::spatial_roi::encoder::SpatialRoiLosslessDeviceView;
using orange::spatial_roi::encoder::SpatialRoiLosslessEncoder;
using orange::spatial_roi::encoder::SpatialRoiLosslessEncoderArtifactBundle;
using orange::spatial_roi::encoder::SpatialRoiLosslessEncoderConfig;
using orange::spatial_roi::encoder::SpatialRoiLosslessFrameMetadata;
using orange::spatial_roi::encoder::SpatialRoiLosslessFrameResult;
using orange::spatial_roi::encoder::SpatialRoiLosslessFrameResultStatus;
#if defined(ORANGE_SPATIAL_ROI_ENCODER_TESTING)
using orange::spatial_roi::encoder::SpatialRoiLosslessEncoderTestFaultPoint;
#endif

// 64x32 is below the HEVC NVENC minimum on supported production GPUs and
// fails nvEncInitializeEncoder with NV_ENC_ERR_INVALID_PARAM. Keep this real
// driver fixture conservative while remaining small enough for fast decode.
constexpr std::uint32_t kWidth = 256;
constexpr std::uint32_t kHeight = 256;
constexpr std::uint64_t kNv12Bytes =
    static_cast<std::uint64_t>(kWidth) * kHeight * 3U / 2U;

struct TestPaths {
    std::filesystem::path recording_root;
    std::filesystem::path artifact_root;
    std::filesystem::path output;
    std::filesystem::path metadata;
    std::filesystem::path keyframes;
    std::filesystem::path finalization;
    std::shared_ptr<orange::spatial_roi::recording::SpatialRoiRecorderArtifactRoot>
        root;
};

constexpr char kArtifactStem[] = "Cam2010096_spatial_roi_roi_1";

std::vector<std::string> artifact_allowlist()
{
    return {std::string(kArtifactStem) + ".mp4",
            std::string(kArtifactStem) + "_meta.csv",
            std::string(kArtifactStem) + "_keyframe.json",
            std::string(kArtifactStem) + ".mp4.finalization.json"};
}

TestPaths test_paths(const std::string& suffix = {})
{
    const std::string stem =
        "/tmp/spatial_roi_lossless_encoder_test_" +
        std::to_string(static_cast<unsigned long long>(::getpid())) + suffix;
    TestPaths paths;
    paths.recording_root = stem;
    paths.artifact_root = paths.recording_root / "external_spatial_roi_recorder";
    paths.output = paths.artifact_root / (std::string(kArtifactStem) + ".mp4");
    paths.metadata =
        paths.artifact_root / (std::string(kArtifactStem) + "_meta.csv");
    paths.keyframes =
        paths.artifact_root / (std::string(kArtifactStem) + "_keyframe.json");
    paths.finalization = paths.artifact_root /
                         (std::string(kArtifactStem) +
                          ".mp4.finalization.json");
    std::error_code directory_error;
    std::filesystem::create_directories(paths.recording_root, directory_error);
    if (!expect(!directory_error,
                "create temporary recording root: " + directory_error.message())) {
        throw std::runtime_error(directory_error.message());
    }
    std::unique_ptr<orange::spatial_roi::recording::SpatialRoiRecorderArtifactRoot>
        root;
    std::string error;
    if (!expect(orange::spatial_roi::recording::SpatialRoiRecorderArtifactRoot::Open(
                    paths.recording_root,
                    artifact_allowlist(),
                    &root,
                    &error),
                "open real temporary recording artifact root: " + error)) {
        throw std::runtime_error(error);
    }
    paths.root = std::shared_ptr<
        orange::spatial_roi::recording::SpatialRoiRecorderArtifactRoot>(
        std::move(root));
    for (const auto& relative : artifact_allowlist()) {
        std::unique_ptr<orange::spatial_roi::recording::SpatialRoiRecorderArtifactFile>
            file;
        if (!expect(paths.root->CreateFile(relative, &file, &error),
                    "create authorized test artifact: " + error)) {
            throw std::runtime_error(error);
        }
    }
    return paths;
}

SpatialRoiLosslessEncoderArtifactBundle open_artifacts(const TestPaths& paths)
{
    SpatialRoiLosslessEncoderArtifactBundle bundle;
    bundle.artifact_root = paths.root;
    std::string error;
    std::unique_ptr<orange::spatial_roi::recording::SpatialRoiRecorderArtifactFile>*
        outputs[] = {&bundle.video, &bundle.metadata_csv,
                     &bundle.keyframes_json, &bundle.finalization_json};
    const auto allowlist = artifact_allowlist();
    for (std::size_t index = 0; index < allowlist.size(); ++index) {
        if (!expect(paths.root->OpenExistingFile(
                        allowlist[index],
                        orange::spatial_roi::recording::
                            SpatialRoiRecorderArtifactFileAccess::kReadWrite,
                        outputs[index],
                        &error),
                    "open retained read-write test artifact: " + error)) {
            throw std::runtime_error(error);
        }
    }
    return bundle;
}

SpatialRoiLosslessEncoderConfig valid_config(const TestPaths& paths)
{
    SpatialRoiLosslessEncoderConfig config;
    config.stream.recording_id = "lossless_encoder_test_recording";
    config.stream.recording_identity_token =
        orange::shaman_v2_recording_identity::token_for_recording_id(
            config.stream.recording_id);
    config.stream.producer_generation = "generation_1";
    config.stream.camera_id = 3;
    config.stream.camera_serial = "2010096";
    config.stream.roi_id = "roi_1";
    config.stream.region_id = "region_1";
    config.stream.arena_group_id = "group_1";
    config.stream.arena_id = "arena_1";
    config.stream.logical_stream_id = "2010096_spatial_roi_roi_1";
    config.stream.spatial_roi_plan_sha256 =
        "sha256:0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";

    config.geometry.native_raster = {kWidth, kHeight};
    config.geometry.content_rect = {0, 0, kWidth, kHeight};
    config.geometry.encoded_raster = {kWidth, kHeight};
    config.geometry.encoded_content_rect = {0, 0, kWidth, kHeight};
    config.geometry.padding = {0, 0, 0, 0, 0};
    config.geometry.routing_policy = "single_shard";

    config.source_gpu_id = 0;
    config.recorder_gpu_id = 0;
    config.assigned_shard_id = 0;
    config.fps = 30;
    config.queue_capacity = 2;
    config.max_queue_bytes = kNv12Bytes * config.queue_capacity;
    config.writer_queue_max_packets = 64;
    config.writer_queue_max_bytes = 8U * 1024U * 1024U;
    config.operation_timeout_ms = 5000;
    config.artifacts = open_artifacts(paths);
    config.max_frames_per_stream = 3;
    config.max_media_bytes_per_stream = 64U * 1024U * 1024U;
    config.frame_result_callback =
        [](const SpatialRoiLosslessFrameResult&, std::string*) { return true; };
    return config;
}

SpatialRoiFrameDescriptor descriptor_for(
    const SpatialRoiLosslessEncoderConfig& config,
    const std::uint64_t source_frame_id,
    const std::uint64_t roi_stream_frame_index)
{
    SpatialRoiFrameDescriptor descriptor;
    descriptor.recording_id = config.stream.recording_id;
    descriptor.recording_identity_token = config.stream.recording_identity_token;
    descriptor.producer_generation = config.stream.producer_generation;
    descriptor.camera_id = config.stream.camera_id;
    descriptor.camera_serial = config.stream.camera_serial;
    descriptor.local_frame_id = source_frame_id;
    descriptor.camera_frame_id = source_frame_id + 1000;
    descriptor.recording_frame_id = source_frame_id;
    descriptor.roi_stream_frame_index = roi_stream_frame_index;
    descriptor.camera_timestamp_ns =
        1000000000ULL + source_frame_id * 33333333ULL;
    descriptor.timestamp_sys_ns =
        2000000000ULL + source_frame_id * 33333333ULL;
    descriptor.roi_id = config.stream.roi_id;
    descriptor.region_id = config.stream.region_id;
    descriptor.arena_group_id = config.stream.arena_group_id;
    descriptor.arena_id = config.stream.arena_id;
    descriptor.logical_stream_id = config.stream.logical_stream_id;
    descriptor.spatial_roi_plan_sha256 = config.stream.spatial_roi_plan_sha256;
    descriptor.native_raster = config.geometry.native_raster;
    descriptor.content_rect = config.geometry.content_rect;
    descriptor.encoded_raster = config.geometry.encoded_raster;
    descriptor.encoded_content_rect = config.geometry.encoded_content_rect;
    descriptor.padding = config.geometry.padding;
    descriptor.source_pixel_format =
        orange::spatial_roi::kSpatialRoiMono8PixelFormat;
    descriptor.bytes = static_cast<std::uint64_t>(kWidth) * kHeight;
    descriptor.source_gpu_id = config.source_gpu_id;
    descriptor.assigned_gpu_id = config.recorder_gpu_id;
    descriptor.assigned_shard_id = config.assigned_shard_id;
    descriptor.routing_policy = config.geometry.routing_policy;
    return descriptor;
}

SpatialRoiLosslessFrameMetadata metadata_for(
    const SpatialRoiLosslessEncoderConfig& config,
    const std::uint64_t source_frame_id,
    const std::uint64_t roi_stream_frame_index)
{
    const SpatialRoiFrameDescriptor descriptor =
        descriptor_for(config, source_frame_id, roi_stream_frame_index);
    SpatialRoiLosslessFrameMetadata metadata;
    metadata.correlation =
        orange::spatial_roi::ipc::spatial_roi_ipc_correlation_from_descriptor(
            descriptor);
    metadata.camera_timestamp_ns = descriptor.camera_timestamp_ns;
    metadata.timestamp_sys_ns = descriptor.timestamp_sys_ns;
    metadata.source_gpu_id = config.source_gpu_id;
    metadata.assigned_gpu_id = config.recorder_gpu_id;
    metadata.assigned_shard_id = config.assigned_shard_id;
    metadata.width = kWidth;
    metadata.height = kHeight;
    metadata.byte_length = kNv12Bytes;
    metadata.row_pitch_bytes = kWidth;
    return metadata;
}

SpatialRoiLosslessDeviceView device_view_for(
    const SpatialRoiLosslessEncoderConfig& config,
    const std::uint64_t source_frame_id,
    const std::uint64_t roi_stream_frame_index,
    const std::uint8_t luma = 91)
{
    std::vector<std::uint8_t> host(kNv12Bytes, 128);
    std::fill(host.begin(), host.begin() + kWidth * kHeight, luma);
    void* device_ptr = nullptr;
    if (cudaMalloc(&device_ptr, host.size()) != cudaSuccess) {
        return {};
    }
    if (cudaMemcpy(device_ptr,
                   host.data(),
                   host.size(),
                   cudaMemcpyHostToDevice) != cudaSuccess) {
        (void)cudaFree(device_ptr);
        return {};
    }
    SpatialRoiLosslessDeviceView view;
    view.device_nv12 = static_cast<const unsigned char*>(device_ptr);
    view.lifetime = std::shared_ptr<const void>(
        device_ptr,
        [](const void* pointer) {
            if (pointer) {
                (void)cudaFree(const_cast<void*>(pointer));
            }
        });
    view.metadata =
        metadata_for(config, source_frame_id, roi_stream_frame_index);
    return view;
}

bool expect(bool condition, const std::string& message)
{
    if (!condition) {
        std::cerr << "[FAIL] " << message << '\n';
        return false;
    }
    return true;
}

SpatialRoiLosslessFrameResult encoded_result_for(
    const SpatialRoiLosslessEncoderConfig& config,
    const std::uint64_t source_frame_id,
    const std::uint64_t roi_stream_frame_index)
{
    const auto metadata =
        metadata_for(config, source_frame_id, roi_stream_frame_index);
    SpatialRoiLosslessFrameResult result;
    result.status = SpatialRoiLosslessFrameResultStatus::Encoded;
    result.correlation = metadata.correlation;
    result.geometry = config.geometry;
    result.camera_timestamp_ns = metadata.camera_timestamp_ns;
    result.timestamp_sys_ns = metadata.timestamp_sys_ns;
    result.source_gpu_id = metadata.source_gpu_id;
    result.assigned_gpu_id = metadata.assigned_gpu_id;
    result.assigned_shard_id = metadata.assigned_shard_id;
    result.output_frame_index = roi_stream_frame_index;
    result.nvenc_pts_assigned = true;
    result.nvenc_pts = roi_stream_frame_index - 1U;
    result.packet_count = 1;
    result.encoded_bytes = 1234;
    result.keyframe = true;
    return result;
}

bool host_frame_result_checks(const SpatialRoiLosslessEncoderConfig& config)
{
    auto valid = encoded_result_for(config, 11, 1);
    std::string error;
    if (!expect(
            orange::spatial_roi::encoder::
                validate_spatial_roi_lossless_frame_result(valid, &error),
            "valid encoded result preserves exact one-based/output and zero-based/PTS identities")) {
        std::cerr << error << '\n';
        return false;
    }
    auto rejects = [&](auto mutate, const std::string& message) {
        auto changed = valid;
        mutate(changed);
        std::string changed_error;
        return expect(
            !orange::spatial_roi::encoder::
                validate_spatial_roi_lossless_frame_result(changed,
                                                           &changed_error),
            message);
    };
    if (!rejects([](auto& value) { value.output_frame_index = 0; },
                 "encoded result rejects zero-based external output index") ||
        !rejects([](auto& value) { value.output_frame_index = 2; },
                 "encoded result rejects output/ROI identity mismatch") ||
        !rejects([](auto& value) { value.nvenc_pts = 1; },
                 "encoded result rejects one-based NVENC PTS") ||
        !rejects([](auto& value) { value.packet_count = 0; },
                 "encoded result requires its exact packet") ||
        !rejects([](auto& value) { value.failure_reason = "invented"; },
                 "encoded result cannot carry a failure reason") ||
        !rejects([](auto& value) { value.geometry.padding.right = 1; },
                 "result rejects geometry identity inconsistent with padding")) {
        return false;
    }
    auto non_keyframe = valid;
    non_keyframe.keyframe = false;
    if (!expect(
            orange::spatial_roi::encoder::
                validate_spatial_roi_lossless_frame_result(non_keyframe, &error),
            "generic encoded-result validation preserves actual non-keyframe evidence")) {
        return false;
    }

    SpatialRoiLosslessFrameResult failed = valid;
    failed.status = SpatialRoiLosslessFrameResultStatus::Failed;
    failed.output_frame_index = 0;
    failed.packet_count = 0;
    failed.encoded_bytes = 0;
    failed.keyframe = false;
    failed.failure_reason = "encoder rejected frame";
    if (!expect(
            orange::spatial_roi::encoder::
                validate_spatial_roi_lossless_frame_result(failed, &error),
            "failed result may retain attempted NVENC PTS but no packet evidence")) {
        return false;
    }
    auto not_submitted = failed;
    not_submitted.nvenc_pts_assigned = false;
    not_submitted.nvenc_pts = 0;
    if (!expect(
            orange::spatial_roi::encoder::
                validate_spatial_roi_lossless_frame_result(not_submitted,
                                                           &error),
            "copy-stage failure remains valid without an assigned NVENC PTS")) {
        return false;
    }
    auto failed_rejects = [&](auto mutate, const std::string& message) {
        auto changed = failed;
        mutate(changed);
        std::string changed_error;
        return expect(
            !orange::spatial_roi::encoder::
                validate_spatial_roi_lossless_frame_result(changed,
                                                           &changed_error),
            message);
    };
    return failed_rejects([](auto& value) { value.failure_reason.clear(); },
                          "failed result requires a reason") &&
           failed_rejects([](auto& value) { value.output_frame_index = 1; },
                          "failed result cannot invent an output frame") &&
           failed_rejects([](auto& value) { value.packet_count = 1; },
                          "failed result cannot invent packet count") &&
           failed_rejects([](auto& value) { value.encoded_bytes = 1; },
                          "failed result cannot invent packet bytes") &&
           failed_rejects([](auto& value) { value.keyframe = true; },
                          "failed result cannot invent a keyframe") &&
           failed_rejects([](auto& value) {
                              value.nvenc_pts_assigned = false;
                              value.nvenc_pts = 7;
                          },
                          "failed result cannot invent an unassigned NVENC PTS") &&
           expect(std::string(orange::spatial_roi::encoder::
                                  spatial_roi_lossless_frame_result_status_name(
                                      SpatialRoiLosslessFrameResultStatus::Encoded)) ==
                      "encoded" &&
                      std::string(orange::spatial_roi::encoder::
                                      spatial_roi_lossless_frame_result_status_name(
                                          SpatialRoiLosslessFrameResultStatus::Failed)) ==
                          "failed",
                  "frame result status names are stable");
}

bool host_contract_checks(const TestPaths& paths)
{
    SpatialRoiLosslessEncoderConfig config = valid_config(paths);
    std::string error;
    if (!expect(
            orange::spatial_roi::encoder::
                validate_spatial_roi_lossless_encoder_config(config, &error),
            "valid strict recorder-owned contract is accepted")) {
        std::cerr << error << '\n';
        return false;
    }
    const auto profile =
        orange::spatial_roi::encoder::
            build_spatial_roi_lossless_encoder_profile(config);
    if (!expect(profile.codec == "hevc" && profile.tuning == "lossless" &&
                    profile.resolved_gop_length == 1 && profile.width == kWidth &&
                    profile.height == kHeight && profile.source_width == kWidth &&
                    profile.source_height == kHeight &&
                    profile.output_kind == "spatial_roi" &&
                    profile.encoder_control_overrides.aq == 0 &&
                    profile.encoder_control_overrides.temporal_aq == 0 &&
                    profile.encoder_control_overrides.lookahead == 0 &&
                    profile.encoder_control_overrides.lookahead_depth == 0 &&
                    profile.source_pixel_contract.id ==
                        "orange.spatial_roi.mono8.v1" &&
                    profile.source_pixel_contract.pixel_format == "mono8" &&
                    profile.source_pixel_contract.encoder_input_format == "nv12",
                "profile binds HEVC lossless GOP-1 Mono8-to-NV12")) {
        return false;
    }

    auto rejects = [&](auto mutate, const std::string& message) {
        auto changed = valid_config(paths);
        mutate(changed);
        std::string changed_error;
        return expect(
            !orange::spatial_roi::encoder::
                validate_spatial_roi_lossless_encoder_config(changed,
                                                              &changed_error),
            message);
    };
    return rejects([](auto& value) { value.codec = "h264"; },
                   "non-HEVC codec is rejected") &&
           rejects([](auto& value) { value.gop_length = 2; },
                   "non-GOP-1 contract is rejected") &&
           rejects([](auto& value) { value.aq = true; },
                   "legacy profile rejects enabled AQ") &&
           rejects([](auto& value) { value.temporal_aq = true; },
                   "legacy profile rejects enabled temporal AQ") &&
           rejects([](auto& value) { value.lookahead = true; },
                   "legacy profile rejects enabled lookahead") &&
           rejects([](auto& value) { value.lookahead_depth = 1; },
                   "legacy profile rejects nonzero lookahead depth") &&
           rejects([](auto& value) { value.neutral_chroma_value = 0; },
                   "non-neutral chroma contract is rejected") &&
           rejects([](auto& value) { value.stream.logical_stream_id = "wrong"; },
                   "non-canonical logical stream identity is rejected") &&
           rejects([](auto& value) { value.geometry.encoded_raster.width = 63; },
                   "odd NV12 width is rejected") &&
           rejects([](auto& value) { value.max_frames_per_stream = 0; },
                   "zero max frame limit is rejected") &&
           rejects([](auto& value) {
                       value.max_frames_per_stream = 4000001ULL;
                   },
                   "max frame limit above the recorder implementation bound is rejected") &&
           rejects([](auto& value) { value.max_media_bytes_per_stream = 0; },
                   "zero max media byte limit is rejected") &&
           rejects([](auto& value) { value.queue_capacity = 0; },
                   "zero input queue depth is rejected") &&
           rejects([](auto& value) { value.queue_capacity = 4097; },
                   "unbounded input queue depth is rejected") &&
           rejects([](auto& value) { value.max_queue_bytes -= 1; },
                   "input queue byte budget must cover every queue slot") &&
           rejects([](auto& value) { value.writer_queue_max_packets = 0; },
                   "zero writer packet queue bound is rejected") &&
           rejects([](auto& value) {
                       value.writer_queue_max_bytes =
                           513ULL * 1024ULL * 1024ULL;
                   },
                   "writer byte queue above the hard bound is rejected") &&
           rejects([](auto& value) { value.operation_timeout_ms = 0; },
                   "zero operation timeout is rejected") &&
           rejects([](auto& value) { value.frame_result_callback = {}; },
                   "missing terminal frame callback is rejected") &&
           host_frame_result_checks(config);
}

bool host_p1_low_latency_contract_checks(const TestPaths& paths)
{
    auto make_p1_config = [&]() {
        auto config = valid_config(paths);
        config.profile_id = orange::spatial_roi::encoder::
            kSpatialRoiP1LowLatencyProfileId;
        config.preset = "p1";
        config.tuning = "ll";
        config.lossless = false;
        config.rate_control_mode = "vbr";
        config.quality_value = 20;
        config.gop_length = 1;
        config.luma_preserved_exactly = false;
        return config;
    };

    auto config = make_p1_config();
    std::string error;
    if (!expect(
            orange::spatial_roi::encoder::validate_spatial_roi_encoder_config(
                config, &error),
            "versioned P1 low-latency ROI profile is accepted")) {
        std::cerr << error << '\n';
        return false;
    }
    if (!expect(
            !orange::spatial_roi::encoder::
                validate_spatial_roi_lossless_encoder_config(config, &error),
            "P1 low-latency profile is not misclassified as lossless")) {
        return false;
    }

    const auto profile = orange::spatial_roi::encoder::
        build_spatial_roi_encoder_profile(config);
    if (!expect(
            profile.name == orange::spatial_roi::encoder::
                                kSpatialRoiP1LowLatencyProfileId &&
                profile.codec == "hevc" && profile.preset == "p1" &&
                profile.tuning == "ll" &&
                profile.rate_control_mode == "vbr" &&
                profile.quality_value == 20 &&
                profile.resolved_gop_length == 1 &&
                profile.encoder_control_overrides.aq == 0 &&
                profile.encoder_control_overrides.temporal_aq == 0 &&
                profile.encoder_control_overrides.lookahead == 0 &&
                profile.encoder_control_overrides.lookahead_depth == 0 &&
                profile.fps == config.fps &&
                profile.output_kind == "spatial_roi" &&
                profile.source_pixel_contract.id ==
                    "orange.spatial_roi.mono8.v1" &&
                profile.source_pixel_contract.source_origin ==
                    "spatial_roi_recorder" &&
                profile.source_pixel_contract.transform_to_encoder ==
                    "spatial_roi_mono8_to_nv12" &&
                profile.source_pixel_contract.pixel_format == "mono8" &&
                profile.source_pixel_contract.encoder_input_format == "nv12",
            "P1 profile preserves the exact versioned encode contract")) {
        return false;
    }

    auto rejects = [&](auto mutate, const std::string& message) {
        auto changed = make_p1_config();
        mutate(changed);
        std::string changed_error;
        return expect(
            !orange::spatial_roi::encoder::validate_spatial_roi_encoder_config(
                changed, &changed_error),
            message);
    };
    auto gop25 = make_p1_config();
    gop25.profile_id = orange::spatial_roi::encoder::
        kSpatialRoiP1LowLatencyGop25ProfileId;
    gop25.gop_length = 25;
    std::string gop25_error;
    if (!expect(
            orange::spatial_roi::encoder::validate_spatial_roi_encoder_config(
                gop25, &gop25_error),
            "versioned P1 low-latency GOP-25 profile is accepted")) {
        std::cerr << gop25_error << '\n';
        return false;
    }
    const auto gop25_profile = orange::spatial_roi::encoder::
        build_spatial_roi_encoder_profile(gop25);
    if (!expect(gop25_profile.name == orange::spatial_roi::encoder::
                                kSpatialRoiP1LowLatencyGop25ProfileId &&
                    gop25_profile.resolved_gop_length == 25 &&
                    gop25_profile.preset == "p1" &&
                    gop25_profile.rate_control_mode == "vbr" &&
                    gop25_profile.encoder_control_overrides.aq == 0 &&
                    gop25_profile.encoder_control_overrides.temporal_aq == 0 &&
                    gop25_profile.encoder_control_overrides.lookahead == 0 &&
                    gop25_profile.encoder_control_overrides.lookahead_depth == 0,
                "GOP-25 profile preserves its exact active encoder identity")) {
        return false;
    }

    return rejects([](auto& value) { value.profile_id = "p1_unversioned"; },
                   "unrecognized P1 profile identity is rejected") &&
           rejects([](auto& value) { value.preset = "p2"; },
                   "P1 profile rejects a different preset") &&
           rejects([](auto& value) { value.lossless = true; },
                   "P1 profile rejects lossless normalization") &&
           rejects([](auto& value) { value.rate_control_mode = "cqp"; },
                   "P1 profile rejects a different rate-control mode") &&
           rejects([](auto& value) { value.quality_value = 19; },
                   "P1 profile rejects a different quality value") &&
           rejects([](auto& value) { value.aq = true; },
                   "P1 profile rejects enabled AQ") &&
           rejects([](auto& value) { value.temporal_aq = true; },
                   "P1 profile rejects enabled temporal AQ") &&
           rejects([](auto& value) { value.lookahead = true; },
                   "P1 profile rejects enabled lookahead") &&
           rejects([](auto& value) { value.lookahead_depth = 1; },
                   "P1 profile rejects nonzero lookahead depth") &&
           rejects([](auto& value) { value.gop_length = 25; },
                   "first-slice P1 profile remains exact GOP-1");
}

bool host_fixed_gop_writer_checks()
{
    const TestPaths paths = test_paths("_fixed_gop_writer");
    auto artifacts = open_artifacts(paths);
    FFmpegWriterDescriptorOutputConfig output(
        artifacts.video->borrowed_fd(),
        artifacts.keyframes_json->borrowed_fd(),
        artifacts.finalization_json->borrowed_fd(),
        64U * 1024U * 1024U,
        artifacts.video->relative_path(),
        artifacts.keyframes_json->relative_path(),
        artifacts.finalization_json->relative_path());
    FFmpegWriterKeyframePolicy policy;
    policy.name = kFFmpegWriterFixedGopIdrPolicyName;
    policy.gop_length = 25;
    FFmpegWriter writer(AV_CODEC_ID_HEVC, 256, 256, 30, std::move(output),
                        {}, {}, std::move(policy));
    const std::vector<std::uint8_t> idr = {
        0x00, 0x00, 0x00, 0x01, 0x26, 0x01, 0x02, 0x03};
    const std::vector<std::uint8_t> inter = {
        0x00, 0x00, 0x00, 0x01, 0x02, 0x01, 0x02, 0x03};
    for (std::uint64_t frame = 0; frame < 26; ++frame) {
        const bool keyframe = frame == 0 || frame == 25;
        const auto& packet = keyframe ? idr : inter;
        if (!expect(
                writer.push_packet_with_keyframe(
                    const_cast<std::uint8_t*>(packet.data()),
                    static_cast<int>(packet.size()),
                    static_cast<std::int64_t>(frame),
                    keyframe,
                    frame / 25U,
                    frame % 25U == 24U),
                "fixed GOP writer accepts IDR boundaries and interior non-IDR packets")) {
            return false;
        }
    }
    if (!expect(writer.finalize(),
                "fixed GOP writer finalizes its 26-frame host fixture")) {
        return false;
    }
    std::ifstream keyframes(paths.keyframes);
    nlohmann::json summary;
    keyframes >> summary;
    const auto& keyframe_policy = summary.at("keyframe_policy");
    const bool valid =
        summary.value("total_frames", 0U) == 26 &&
        summary.at("frame_index_sequence").value("first", -1) == 0 &&
        summary.at("frame_index_sequence").value("last", -1) == 25 &&
        summary.at("frame_index_sequence").value("zero_based_contiguous", false) &&
        keyframe_policy.size() == 4 &&
        keyframe_policy.value("name", std::string()) ==
            "fixed_gop_25_idr" &&
        keyframe_policy.value("keyframe_frames", 0U) == 2 &&
        keyframe_policy.value("non_keyframe_frames", 0U) == 24 &&
        keyframe_policy.value("satisfied", false);
    std::error_code cleanup_error;
    std::filesystem::remove_all(paths.recording_root, cleanup_error);
    return expect(valid && !cleanup_error,
                  "fixed GOP writer emits a closed O(1) schedule summary");
}

bool host_artifact_authority_rejection_checks(const TestPaths& paths)
{
    auto rejects = [&](auto mutate, const std::string& message) {
        auto changed = valid_config(paths);
        mutate(changed);
        std::string error;
        return expect(
            !orange::spatial_roi::encoder::
                validate_spatial_roi_lossless_encoder_config(changed, &error),
            message);
    };

    if (!rejects(
            [](auto& value) {
                value.artifacts.video.swap(value.artifacts.metadata_csv);
            },
            "swapped artifact kind/path handles are rejected") ||
        !rejects(
            [](auto& value) {
                std::unique_ptr<
                    orange::spatial_roi::recording::SpatialRoiRecorderArtifactFile>
                    read_only;
                std::string ignored;
                value.artifacts.artifact_root->OpenExistingFile(
                    std::string(kArtifactStem) + "_meta.csv",
                    orange::spatial_roi::recording::
                        SpatialRoiRecorderArtifactFileAccess::kReadOnly,
                    &read_only,
                    &ignored);
                value.artifacts.metadata_csv = std::move(read_only);
            },
            "read-only/adopted artifact handles are rejected") ||
        !rejects([](auto& value) { value.max_frames_per_stream = 0; },
                 "zero max_frames_per_stream is rejected") ||
        !rejects([](auto& value) { value.max_media_bytes_per_stream = 0; },
                 "zero max_media_bytes_per_stream is rejected")) {
        return false;
    }

    // A hard-link replacement can make two exact contract paths refer to the
    // same inode while both retained handles still pass namespace binding;
    // the bundle's distinct-inode check must reject that constructible case.
    const std::filesystem::path displaced_metadata =
        paths.metadata.string() + ".held";
    std::error_code duplicate_error;
    std::filesystem::rename(paths.metadata, displaced_metadata, duplicate_error);
    if (!expect(!duplicate_error, "prepare duplicate-inode fixture")) {
        return false;
    }
    if (::link(paths.output.c_str(), paths.metadata.c_str()) != 0) {
        std::filesystem::rename(displaced_metadata, paths.metadata, duplicate_error);
        return expect(false, "create duplicate-inode fixture");
    }
    auto duplicate_inode = valid_config(paths);
    std::string duplicate_inode_error;
    const bool duplicate_rejected = !orange::spatial_roi::encoder::
        validate_spatial_roi_lossless_encoder_config(
            duplicate_inode, &duplicate_inode_error);
    std::filesystem::remove(paths.metadata, duplicate_error);
    std::filesystem::rename(displaced_metadata, paths.metadata, duplicate_error);
    if (!expect(duplicate_rejected && !duplicate_error,
                "duplicate inode across exact artifact paths is rejected")) {
        return false;
    }

    // A root retained from another recording cannot authorize the files from
    // this recording even when all four relative labels happen to match.
    auto other_paths = test_paths("_other");
    auto mismatched_root = valid_config(paths);
    mismatched_root.artifacts.artifact_root = other_paths.root;
    std::string mismatch_error;
    if (!expect(
            !orange::spatial_roi::encoder::
                validate_spatial_roi_lossless_encoder_config(
                    mismatched_root, &mismatch_error),
            "mismatched artifact root identity is rejected")) {
        return false;
    }
    std::error_code ignored_other;
    std::filesystem::remove_all(other_paths.recording_root, ignored_other);

    auto replaced = valid_config(paths);
    const std::filesystem::path displaced = paths.output.string() + ".held";
    std::error_code rename_error;
    std::filesystem::rename(paths.output, displaced, rename_error);
    if (!expect(!rename_error, "prepare leaf replacement fixture")) {
        return false;
    }
    {
        std::ofstream replacement(paths.output, std::ios::trunc);
        replacement << "replacement";
    }
    std::string replacement_error;
    const bool rejected = !orange::spatial_roi::encoder::
        validate_spatial_roi_lossless_encoder_config(replaced, &replacement_error);
    std::filesystem::remove(paths.output, rename_error);
    std::filesystem::rename(displaced, paths.output, rename_error);
    return expect(rejected && !rename_error,
                  "leaf replacement after handle retention is rejected");
}

bool check_nv12_fixture(const std::vector<std::uint8_t>& host,
                        const std::uint64_t frame_id)
{
    for (std::uint32_t y = 0; y < kHeight; ++y) {
        for (std::uint32_t x = 0; x < kWidth; ++x) {
            const std::uint8_t expected = static_cast<std::uint8_t>(
                (x * 3U + y * 5U + frame_id * 7U) & 0xffU);
            if (host[static_cast<std::size_t>(y) * kWidth + x] != expected) {
                return false;
            }
        }
    }
    return std::all_of(host.begin() + kWidth * kHeight,
                       host.end(),
                       [](const std::uint8_t value) { return value == 128; });
}

bool decode_and_validate_lossless_mp4(
    const std::filesystem::path& path,
    const std::vector<std::vector<std::uint8_t>>& expected_luma)
{
    AVFormatContext* format = nullptr;
    if (!expect(avformat_open_input(&format, path.c_str(), nullptr, nullptr) >= 0,
                "open finalized lossless MP4 for decode")) {
        return false;
    }
    if (!expect(avformat_find_stream_info(format, nullptr) >= 0,
                "read finalized MP4 stream info")) {
        avformat_close_input(&format);
        return false;
    }
    int video_stream_index = -1;
    for (unsigned int index = 0; index < format->nb_streams; ++index) {
        if (format->streams[index]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            video_stream_index = static_cast<int>(index);
            break;
        }
    }
    if (!expect(video_stream_index >= 0, "finalized MP4 contains a video stream")) {
        avformat_close_input(&format);
        return false;
    }
    AVStream* stream = format->streams[video_stream_index];
    if (!expect(stream->codecpar->codec_id == AV_CODEC_ID_HEVC &&
                    stream->codecpar->width == static_cast<int>(kWidth) &&
                    stream->codecpar->height == static_cast<int>(kHeight),
                "decoded stream is HEVC at the contract dimensions")) {
        avformat_close_input(&format);
        return false;
    }
    const AVCodec* decoder = avcodec_find_decoder(stream->codecpar->codec_id);
    AVCodecContext* codec = decoder ? avcodec_alloc_context3(decoder) : nullptr;
    if (!expect(decoder && codec &&
                    avcodec_parameters_to_context(codec, stream->codecpar) >= 0 &&
                    avcodec_open2(codec, decoder, nullptr) >= 0,
                "open in-process HEVC decoder")) {
        avcodec_free_context(&codec);
        avformat_close_input(&format);
        return false;
    }
    AVPacket* packet = av_packet_alloc();
    AVFrame* frame = av_frame_alloc();
    bool ok = expect(packet && frame, "allocate FFmpeg decode packet/frame");
    std::size_t decoded_frames = 0;
    std::size_t video_packets = 0;
    auto check_decoded_frame = [&](AVFrame* decoded) {
        if (decoded_frames >= expected_luma.size() ||
            decoded->width != static_cast<int>(kWidth) ||
            decoded->height != static_cast<int>(kHeight)) {
            return false;
        }
        const int64_t expected_pts = av_rescale_q(
            static_cast<int64_t>(decoded_frames),
            AVRational{1, 30},
            stream->time_base);
        if (decoded->pts != expected_pts) {
            return false;
        }
        const auto& expected = expected_luma[decoded_frames];
        for (std::uint32_t y = 0; y < kHeight; ++y) {
            const auto* row = decoded->data[0] +
                              static_cast<std::ptrdiff_t>(y) * decoded->linesize[0];
            if (!std::equal(expected.begin() + y * kWidth,
                            expected.begin() + (y + 1) * kWidth,
                            row)) {
                return false;
            }
        }
        if (decoded->format == AV_PIX_FMT_YUV420P ||
            decoded->format == AV_PIX_FMT_YUVJ420P) {
            for (int plane = 1; plane <= 2; ++plane) {
                for (std::uint32_t y = 0; y < kHeight / 2; ++y) {
                    const auto* row = decoded->data[plane] +
                                      static_cast<std::ptrdiff_t>(y) *
                                          decoded->linesize[plane];
                    if (!std::all_of(row, row + kWidth / 2,
                                     [](const std::uint8_t value) {
                                         return value == 128;
                                     })) {
                        return false;
                    }
                }
            }
        } else if (decoded->format == AV_PIX_FMT_NV12) {
            for (std::uint32_t y = 0; y < kHeight / 2; ++y) {
                const auto* row = decoded->data[1] +
                                  static_cast<std::ptrdiff_t>(y) *
                                      decoded->linesize[1];
                if (!std::all_of(row, row + kWidth,
                                 [](const std::uint8_t value) {
                                     return value == 128;
                                 })) {
                    return false;
                }
            }
        } else if (decoded->format != AV_PIX_FMT_GRAY8) {
            // NVENC may signal the strict monochrome HEVC profile as a
            // one-plane gray stream. In that representation neutral chroma
            // is intentionally absent; the source-side fixture above still
            // proves that the recorder supplied UV=128 to the copy path.
            return false;
        }
        ++decoded_frames;
        return true;
    };
    auto receive_frames = [&]() {
        for (;;) {
            const int status = avcodec_receive_frame(codec, frame);
            if (status == AVERROR(EAGAIN) || status == AVERROR_EOF) {
                return true;
            }
            if (status < 0 || !check_decoded_frame(frame)) {
                return false;
            }
        }
    };
    while (ok && av_read_frame(format, packet) >= 0) {
        if (packet->stream_index == video_stream_index) {
            ++video_packets;
            if (!(packet->flags & AV_PKT_FLAG_KEY) ||
                avcodec_send_packet(codec, packet) < 0 || !receive_frames()) {
                ok = false;
            }
        }
        av_packet_unref(packet);
    }
    if (ok && (avcodec_send_packet(codec, nullptr) < 0 || !receive_frames())) {
        ok = false;
    }
    ok = ok && expect(video_packets == expected_luma.size(),
                      "GOP-1 MP4 has one keyframe packet per submitted frame") &&
         expect(decoded_frames == expected_luma.size(),
                "decoded MP4 has exactly three frames");
    av_frame_free(&frame);
    av_packet_free(&packet);
    avcodec_free_context(&codec);
    avformat_close_input(&format);
    return ok;
}

bool decode_and_validate_gop25_mp4(const std::filesystem::path& path,
                                   const std::size_t expected_frames,
                                   const int expected_fps)
{
    AVFormatContext* format = nullptr;
    if (!expect(avformat_open_input(&format, path.c_str(), nullptr, nullptr) >= 0,
                "open finalized GOP-25 MP4 for decode")) {
        return false;
    }
    if (!expect(avformat_find_stream_info(format, nullptr) >= 0,
                "read finalized GOP-25 MP4 stream info")) {
        avformat_close_input(&format);
        return false;
    }
    int video_stream_index = -1;
    for (unsigned int index = 0; index < format->nb_streams; ++index) {
        if (format->streams[index]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            video_stream_index = static_cast<int>(index);
            break;
        }
    }
    if (!expect(video_stream_index >= 0,
                "finalized GOP-25 MP4 contains a video stream")) {
        avformat_close_input(&format);
        return false;
    }
    AVStream* stream = format->streams[video_stream_index];
    if (!expect(stream->codecpar->codec_id == AV_CODEC_ID_HEVC &&
                    stream->codecpar->width == static_cast<int>(kWidth) &&
                    stream->codecpar->height == static_cast<int>(kHeight),
                "decoded GOP-25 stream is HEVC at the contract dimensions")) {
        avformat_close_input(&format);
        return false;
    }
    const AVCodec* decoder = avcodec_find_decoder(stream->codecpar->codec_id);
    AVCodecContext* codec = decoder ? avcodec_alloc_context3(decoder) : nullptr;
    if (!expect(decoder && codec &&
                    avcodec_parameters_to_context(codec, stream->codecpar) >= 0 &&
                    avcodec_open2(codec, decoder, nullptr) >= 0,
                "open in-process GOP-25 HEVC decoder")) {
        avcodec_free_context(&codec);
        avformat_close_input(&format);
        return false;
    }
    AVPacket* packet = av_packet_alloc();
    AVFrame* frame = av_frame_alloc();
    bool ok = expect(packet && frame,
                     "allocate GOP-25 FFmpeg decode packet/frame");
    std::size_t decoded_frames = 0;
    std::size_t video_packets = 0;
    auto check_decoded_frame = [&](AVFrame* decoded) {
        if (decoded_frames >= expected_frames ||
            decoded->width != static_cast<int>(kWidth) ||
            decoded->height != static_cast<int>(kHeight)) {
            return false;
        }
        const int64_t expected_pts = av_rescale_q(
            static_cast<int64_t>(decoded_frames),
            AVRational{1, expected_fps},
            stream->time_base);
        if (decoded->pts != expected_pts) {
            return false;
        }
        ++decoded_frames;
        return true;
    };
    auto receive_frames = [&]() {
        for (;;) {
            const int status = avcodec_receive_frame(codec, frame);
            if (status == AVERROR(EAGAIN) || status == AVERROR_EOF) {
                return true;
            }
            if (status < 0 || !check_decoded_frame(frame)) {
                return false;
            }
        }
    };
    while (ok && av_read_frame(format, packet) >= 0) {
        if (packet->stream_index == video_stream_index) {
            ++video_packets;
            if (avcodec_send_packet(codec, packet) < 0 || !receive_frames()) {
                ok = false;
            }
        }
        av_packet_unref(packet);
    }
    if (ok && (avcodec_send_packet(codec, nullptr) < 0 || !receive_frames())) {
        ok = false;
    }
    ok = ok &&
         expect(video_packets == expected_frames,
                "GOP-25 MP4 has one video packet per submitted frame") &&
         expect(decoded_frames == expected_frames,
                "decoded GOP-25 MP4 has exactly the submitted frame count");
    av_frame_free(&frame);
    av_packet_free(&packet);
    avcodec_free_context(&codec);
    avformat_close_input(&format);
    return ok;
}

#if defined(ORANGE_SPATIAL_ROI_ENCODER_TESTING)
bool gpu_fault_accounting_checks()
{
    const TestPaths paths = test_paths("_fault_accounting");
    auto config = valid_config(paths);
    auto metadata_config = valid_config(paths);
    metadata_config.artifacts = SpatialRoiLosslessEncoderArtifactBundle{};
    std::atomic<int> fault_target{-1};
    config.test_fault_injector =
        [&](const SpatialRoiLosslessEncoderTestFaultPoint point) {
            const int expected = static_cast<int>(point);
            if (fault_target.load(std::memory_order_acquire) == expected) {
                fault_target.store(-1, std::memory_order_release);
                throw std::bad_alloc();
            }
        };
    std::vector<SpatialRoiLosslessFrameResult> results;
    results.reserve(1);
    config.frame_result_callback =
        [&](const SpatialRoiLosslessFrameResult& result, std::string*) {
            results.push_back(result);
            return true;
        };

    auto encoder =
        std::make_unique<SpatialRoiLosslessEncoder>(std::move(config));
    const auto view = device_view_for(metadata_config, 11, 1);
    if (!expect(view.device_nv12 && view.lifetime,
                "create deterministic allocation-fault device view")) {
        return false;
    }
    orange::spatial_roi::ipc::SpatialRoiRecorderDetachedFrame invalid_detached;
    const auto invalid_detached_descriptor =
        descriptor_for(metadata_config, 11, 1);
    std::string invalid_detached_error;
    if (!expect(!encoder->Enqueue(std::move(invalid_detached),
                                  invalid_detached_descriptor,
                                  &invalid_detached_error) &&
                    !invalid_detached_error.empty(),
                "invalid detached-frame public enqueue attempt is accounted with a diagnostic")) {
        return false;
    }
    const SpatialRoiLosslessEncoderTestFaultPoint rejected_points[] = {
        SpatialRoiLosslessEncoderTestFaultPoint::
            BeforeDeviceViewWorkItemAllocation,
        SpatialRoiLosslessEncoderTestFaultPoint::BeforeDeviceViewAckAllocation,
        SpatialRoiLosslessEncoderTestFaultPoint::BeforeQueueInsertion,
    };
    for (const auto point : rejected_points) {
        fault_target.store(static_cast<int>(point), std::memory_order_release);
        if (!expect(!encoder->Enqueue(view),
                    "injected pre-admission exception rejects one attempt")) {
            return false;
        }
    }
    fault_target.store(
        static_cast<int>(SpatialRoiLosslessEncoderTestFaultPoint::
                             BeforeMetadataMappingWrite),
        std::memory_order_release);
    if (!expect(encoder->Enqueue(view),
                "metadata fault frame is admitted and copy-acknowledged")) {
        return false;
    }
    if (!expect(!encoder->Finalize(),
                "metadata mapping failure makes terminal encode fail closed")) {
        return false;
    }
    const auto terminal = encoder->terminal_snapshot();
    const auto stats = encoder->stats();
    const bool ok =
        expect(terminal && terminal->terminal && !terminal->successful &&
                   terminal->all_enqueue_attempts_accounted &&
                   terminal->nonempty_stream &&
                   terminal->all_admitted_results_emitted,
               "failed terminal snapshot preserves attempt and admission cardinality") &&
        expect(stats.enqueue_attempted == 5 && stats.rejected == 4 &&
                   stats.enqueued == 1 && stats.dequeued == 1 &&
                   stats.frame_results_emitted == 1 &&
                   stats.failed_results == 1 && stats.encoded_results == 0 &&
                   stats.encoded_packets == 1 &&
                   stats.result_callback_failures == 0,
               "every public and injected enqueue attempt is counted exactly once without double rejection") &&
        expect(results.size() == 1 &&
                   results.front().status ==
                       SpatialRoiLosslessFrameResultStatus::Failed &&
                   results.front().output_frame_index == 0 &&
                   results.front().packet_count == 0 &&
                   !results.front().failure_reason.empty(),
               "metadata mapping failure emits exactly one Failed callback and never Encoded");
    encoder.reset();
    std::error_code ignored;
    std::filesystem::remove_all(paths.recording_root, ignored);
    return ok;
}

bool gpu_empty_stream_checks()
{
    const TestPaths paths = test_paths("_empty_stream");
    auto config = valid_config(paths);
    auto encoder =
        std::make_unique<SpatialRoiLosslessEncoder>(std::move(config));
    if (!expect(!encoder->Finalize(),
                "zero-admission stream cannot finalize successfully")) {
        return false;
    }
    const auto terminal = encoder->terminal_snapshot();
    const bool ok = expect(
        terminal && terminal->terminal && !terminal->successful &&
            terminal->all_enqueue_attempts_accounted &&
            !terminal->nonempty_stream &&
            terminal->all_admitted_results_emitted &&
            !terminal->media_finalization_validated &&
            !terminal->artifacts_sealed &&
            terminal->counts.enqueue_attempted == 0 &&
            terminal->counts.enqueued == 0 &&
            terminal->counts.frame_results_emitted == 0 &&
            terminal->terminal_reason.find("zero-frame") != std::string::npos,
        "empty terminal snapshot is explicit, unsealed, and cardinality-complete");
    encoder.reset();
    std::error_code ignored;
    std::filesystem::remove_all(paths.recording_root, ignored);
    return ok;
}

bool terminal_sidecar_complete(const std::filesystem::path& path)
{
    try {
        std::ifstream input(path);
        if (!input) {
            return false;
        }
        nlohmann::json document;
        input >> document;
        return document.value("terminal", false) &&
               document.value("status", std::string()) == "complete";
    } catch (...) {
        return false;
    }
}

bool gpu_callback_destruction_checks()
{
    const TestPaths paths = test_paths("_callback_destruction");
    auto config = valid_config(paths);
    auto metadata_config = valid_config(paths);
    metadata_config.artifacts = SpatialRoiLosslessEncoderArtifactBundle{};
    std::unique_ptr<SpatialRoiLosslessEncoder> encoder;
    std::mutex callback_mutex;
    std::condition_variable callback_cv;
    bool callback_destroyed_wrapper = false;
    bool owner_cleanup_completed = false;
    config.test_fault_injector =
        [&](const SpatialRoiLosslessEncoderTestFaultPoint point) {
            if (point ==
                SpatialRoiLosslessEncoderTestFaultPoint::AfterOwnerCleanup) {
                {
                    std::lock_guard<std::mutex> lock(callback_mutex);
                    owner_cleanup_completed = true;
                }
                callback_cv.notify_all();
            }
        };
    config.frame_result_callback =
        [&](const SpatialRoiLosslessFrameResult& result, std::string*) {
            if (result.status != SpatialRoiLosslessFrameResultStatus::Encoded) {
                return false;
            }
            encoder.reset();
            {
                std::lock_guard<std::mutex> lock(callback_mutex);
                callback_destroyed_wrapper = true;
            }
            callback_cv.notify_all();
            return true;
        };
    encoder =
        std::make_unique<SpatialRoiLosslessEncoder>(std::move(config));
    const auto view = device_view_for(metadata_config, 13, 1, 117);
    SpatialRoiLosslessEncoder* const encoder_raw = encoder.get();
    if (!expect(view.device_nv12 && view.lifetime && encoder_raw->Enqueue(view),
                "callback-destruction frame is admitted")) {
        return false;
    }
    {
        std::unique_lock<std::mutex> lock(callback_mutex);
        if (!expect(callback_cv.wait_for(
                        lock,
                        std::chrono::seconds(5),
                        [&]() { return callback_destroyed_wrapper; }),
                    "owner callback safely destroys the public encoder wrapper")) {
            return false;
        }
    }
    if (!expect(!encoder,
                "callback released the final public wrapper owner")) {
        return false;
    }
    {
        std::unique_lock<std::mutex> lock(callback_mutex);
        if (!expect(callback_cv.wait_for(
                        lock,
                        std::chrono::seconds(5),
                        [&]() { return owner_cleanup_completed; }),
                    "self-owned worker reaches terminal cleanup after wrapper destruction")) {
            return false;
        }
    }
    const bool ok = expect(
        terminal_sidecar_complete(paths.finalization),
        "self-owned worker drains and finalizes after callback-triggered wrapper destruction");
    std::error_code ignored;
    std::filesystem::remove_all(paths.recording_root, ignored);
    return ok;
}
#endif

bool gpu_encode_checks(const TestPaths& paths)
{
    const auto test_started = std::chrono::steady_clock::now();
    const auto elapsed_ms = [&]() {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::steady_clock::now() - test_started)
            .count();
    };
    const auto stage = [&](const std::string& name) {
        std::cout << "[SPATIAL_ROI_GPU_TEST] status=RUN stage=" << name
                  << " elapsed_ms=" << elapsed_ms() << std::endl;
    };
    int device_count = 0;
    const cudaError_t count_status = cudaGetDeviceCount(&device_count);
    if ((count_status == cudaSuccess && device_count == 0) ||
        count_status == cudaErrorNoDevice) {
        std::cout << "[SPATIAL_ROI_GPU_TEST] status=SKIP reason=no_cuda_device"
                  << " elapsed_ms=" << elapsed_ms() << std::endl;
        return true;
    }
    if (!expect(count_status == cudaSuccess,
                std::string("CUDA device enumeration failed: ") +
                    cudaGetErrorString(count_status))) {
        std::cout << "[SPATIAL_ROI_GPU_TEST] status=FAIL stage=cuda_enumeration"
                  << " elapsed_ms=" << elapsed_ms() << std::endl;
        return false;
    }
    std::cout << "[SPATIAL_ROI_GPU_TEST] status=RUN"
              << " stage=cuda_device_detected device=0"
              << " raster=" << kWidth << 'x' << kHeight
              << " elapsed_ms=" << elapsed_ms() << std::endl;
    if (!expect(cudaSetDevice(0) == cudaSuccess, "CUDA device 0 is usable")) {
        return false;
    }
    stage("cuda_context_ready");

#if defined(ORANGE_SPATIAL_ROI_ENCODER_TESTING)
    if (!gpu_fault_accounting_checks() ||
        !gpu_empty_stream_checks() ||
        !gpu_callback_destruction_checks()) {
        return false;
    }
    stage("fault_and_lifecycle_checks_complete");
#endif

    auto config = valid_config(paths);
    // The encoder takes ownership of its move-only artifact bundle. Keep a
    // separate contract-value fixture for descriptor metadata construction
    // after that move has transferred the strings and handles.
    auto metadata_config = valid_config(paths);
    // Only the encoder owns retained writable artifact handles during the
    // terminal seal. This second value is used solely to build immutable
    // frame identities after the primary config has moved.
    metadata_config.artifacts = SpatialRoiLosslessEncoderArtifactBundle{};
    std::vector<SpatialRoiLosslessFrameResult> frame_results;
    config.frame_result_callback =
        [&](const SpatialRoiLosslessFrameResult& result,
            std::string* error_out) {
            std::string validation_error;
            if (!orange::spatial_roi::encoder::
                    validate_spatial_roi_lossless_frame_result(
                        result, &validation_error)) {
                if (error_out) {
                    *error_out = validation_error;
                }
                return false;
            }
            frame_results.push_back(result);
            return true;
        };
    std::unique_ptr<SpatialRoiLosslessEncoder> encoder;
    stage("nvenc_initialize_start");
    try {
        encoder = std::make_unique<SpatialRoiLosslessEncoder>(std::move(config));
    } catch (const std::exception& exception) {
        std::cerr << "[SPATIAL_ROI_GPU_TEST] status=FAIL stage=nvenc_init reason="
                  << exception.what() << " elapsed_ms=" << elapsed_ms()
                  << std::endl;
        return false;
    }
    stage("nvenc_initialize_complete");
    if (!expect(encoder->valid(), "lossless encoder worker initialized")) {
        std::cerr << encoder->error() << std::endl;
        return false;
    }

    std::vector<std::vector<std::uint8_t>> expected_luma;

    const std::vector<std::uint64_t> source_frame_ids = {11ULL, 27ULL, 42ULL};
    for (std::size_t frame_offset = 0;
         frame_offset < source_frame_ids.size();
         ++frame_offset) {
        const std::uint64_t frame_id = source_frame_ids[frame_offset];
        const std::uint64_t roi_stream_frame_index = frame_offset + 1U;
        std::vector<std::uint8_t> host(kNv12Bytes);
        for (std::uint32_t y = 0; y < kHeight; ++y) {
            for (std::uint32_t x = 0; x < kWidth; ++x) {
                host[static_cast<std::size_t>(y) * kWidth + x] =
                    static_cast<std::uint8_t>((x * 3U + y * 5U + frame_id * 7U) &
                                              0xffU);
            }
        }
        std::fill(host.begin() + kWidth * kHeight, host.end(), 128);
        if (!expect(check_nv12_fixture(host, frame_id),
                    "fixture preserves exact Mono8 luma and UV=128")) {
            return false;
        }
        expected_luma.emplace_back(host.begin(), host.begin() + kWidth * kHeight);

        void* device_ptr = nullptr;
        if (!expect(cudaMalloc(&device_ptr, host.size()) == cudaSuccess,
                    "allocate immutable NV12 source")) {
            return false;
        }
        if (cudaMemcpy(device_ptr,
                       host.data(),
                       host.size(),
                       cudaMemcpyHostToDevice) != cudaSuccess) {
            cudaFree(device_ptr);
            return false;
        }
        auto lifetime = std::shared_ptr<const void>(
            device_ptr,
            [](const void* pointer) {
                if (pointer) {
                    (void)cudaFree(const_cast<void*>(pointer));
                }
            });
        SpatialRoiLosslessDeviceView view;
        view.device_nv12 = static_cast<const unsigned char*>(device_ptr);
        view.lifetime = lifetime;
        view.metadata =
            metadata_for(metadata_config, frame_id, roi_stream_frame_index);
        if (!expect(encoder->Enqueue(view),
                    "immutable recorder-owned NV12 view is copied and encoded")) {
            return false;
        }
        stage("frame_" + std::to_string(roi_stream_frame_index) +
              "_copy_acknowledged");
    }

    if (!expect(!encoder->terminal_snapshot(),
                "terminal snapshot is unavailable before Finalize")) {
        return false;
    }
    stage("finalize_start");
    if (!expect(encoder->Finalize(), "lossless encoder finalizes successfully")) {
        std::cerr << encoder->error() << std::endl;
        return false;
    }
    stage("finalize_complete");
    const auto terminal = encoder->terminal_snapshot();
    if (!expect(terminal && terminal->terminal && terminal->successful &&
                    terminal->drain_completed && terminal->metadata_flushed &&
                    terminal->media_finalization_validated &&
                    terminal->all_enqueue_attempts_accounted &&
                    terminal->nonempty_stream &&
                    terminal->all_admitted_results_emitted &&
                    terminal->source_release_safe &&
                    !terminal->source_quarantined &&
                    !terminal->destination_quarantined &&
                    terminal->terminal_reason == "complete" &&
                    terminal->writer.observed &&
                    !terminal->writer.failure_latched &&
                    !terminal->writer.packet_write_error_latched &&
                    !terminal->writer.writer_thread_failure_latched &&
                    !terminal->writer.queue_overflow_latched &&
                    terminal->writer.close_finalization_validated &&
                    !terminal->writer.close_finalization_failure_latched &&
                    terminal->writer.close_finalization_failure_reason.empty() &&
                    terminal->writer.total_failures == 0 &&
                    terminal->writer.video_size_limit_failures == 0 &&
                    terminal->artifacts_sealed &&
                    terminal->counts.enqueue_attempted == 3 &&
                    terminal->counts.enqueued == 3 &&
                    terminal->counts.frame_results_emitted == 3 &&
                    terminal->counts.encoded_results == 3 &&
                    terminal->counts.failed_results == 0 &&
                    terminal->counts.encoded_packets == 3 &&
                    terminal->counts.finalize_calls == 1,
                "immutable terminal snapshot proves writer, metadata, media, and quarantine state")) {
        return false;
    }
    if (!expect(encoder->Finalize(),
                "repeated successful Finalize returns the original result") ||
        !expect(encoder->terminal_snapshot().get() == terminal.get(),
                "repeated Finalize preserves the exact immutable snapshot object")) {
        return false;
    }
    const auto stats = encoder->stats();
    if (!expect(stats.enqueue_attempted == 3 && stats.enqueued == 3 &&
                    stats.copy_completed == 3 &&
                    stats.encoded_frames == 3 && stats.encoded_packets == 3 &&
                    stats.frame_results_emitted == 3 &&
                    stats.encoded_results == 3 && stats.failed_results == 0 &&
                    stats.result_callback_failures == 0 && stats.rejected == 0 &&
                    stats.source_release_safe && stats.source_quarantines == 0 &&
                    stats.destination_quarantines == 0 &&
                    stats.writer_failures == 0 && stats.metadata_flushed &&
                    stats.media_finalization_validated &&
                    stats.artifacts_sealed &&
                    stats.finalize_calls == 1 && stats.finalized && !stats.failed,
                "stats prove three copies, three encodes, no rejected admission, and safe release")) {
        return false;
    }
    const auto operational = encoder->operational_snapshot();
    const bool operational_ok =
        operational.initialized && operational.finalized &&
                operational.input_queue_max_frames == config.queue_capacity &&
                operational.input_queue_max_bytes == config.max_queue_bytes &&
                operational.input_queue_current_frames == 0 &&
                operational.input_queue_current_bytes == 0 &&
                operational.input_queue_peak_frames >= 1 &&
                operational.input_queue_peak_bytes >= kNv12Bytes &&
                operational.enqueue_attempted == 3 &&
                operational.enqueue_admitted == 3 &&
                operational.dequeued == 3 && operational.rejected == 0 &&
                operational.queue_overflows == 0 &&
                operational.queue_wait_latency.sample_count == 3 &&
                operational.source_copy_latency.sample_count == 3 &&
                operational.encode_call_latency.sample_count == 3 &&
                operational.admission_to_result_latency.sample_count == 3 &&
                operational.writer.observed &&
                operational.writer.queue_max_packets ==
                    config.writer_queue_max_packets &&
                operational.writer.queue_max_bytes ==
                    config.writer_queue_max_bytes &&
                operational.writer.queue_current_packets == 0 &&
                operational.writer.queue_current_bytes == 0 &&
                operational.writer.queue_peak_packets >= 1 &&
                operational.writer.queue_peak_bytes > 0 &&
                !operational.writer.queue_overflowed &&
                operational.writer.queue_overflow_events == 0 &&
                operational.writer.failure_stats.total_failures == 0 &&
                operational.writer.latency_stats.queue_wait.sample_count == 3;
    if (!operational_ok) {
        std::cerr << "[SPATIAL_ROI_GPU_TEST] operational"
                  << " initialized=" << operational.initialized
                  << " finalized=" << operational.finalized
                  << " input_max_frames=" << operational.input_queue_max_frames
                  << " input_max_bytes=" << operational.input_queue_max_bytes
                  << " input_current_frames=" << operational.input_queue_current_frames
                  << " input_current_bytes=" << operational.input_queue_current_bytes
                  << " input_peak_frames=" << operational.input_queue_peak_frames
                  << " input_peak_bytes=" << operational.input_queue_peak_bytes
                  << " attempted=" << operational.enqueue_attempted
                  << " admitted=" << operational.enqueue_admitted
                  << " dequeued=" << operational.dequeued
                  << " rejected=" << operational.rejected
                  << " overflows=" << operational.queue_overflows
                  << " queue_wait_samples="
                  << operational.queue_wait_latency.sample_count
                  << " copy_samples="
                  << operational.source_copy_latency.sample_count
                  << " encode_samples="
                  << operational.encode_call_latency.sample_count
                  << " result_samples="
                  << operational.admission_to_result_latency.sample_count
                  << " writer_observed=" << operational.writer.observed
                  << " writer_max_packets="
                  << operational.writer.queue_max_packets
                  << " writer_max_bytes=" << operational.writer.queue_max_bytes
                  << " writer_current_packets="
                  << operational.writer.queue_current_packets
                  << " writer_current_bytes="
                  << operational.writer.queue_current_bytes
                  << " writer_peak_packets="
                  << operational.writer.queue_peak_packets
                  << " writer_peak_bytes=" << operational.writer.queue_peak_bytes
                  << " writer_overflowed="
                  << operational.writer.queue_overflowed
                  << " writer_overflow_events="
                  << operational.writer.queue_overflow_events
                  << " writer_failures="
                  << operational.writer.failure_stats.total_failures
                  << " writer_queue_wait_samples="
                  << operational.writer.latency_stats.queue_wait.sample_count
                  << std::endl;
    }
    if (!expect(operational_ok,
                "operational snapshot preserves queue bounds, pressure, latency, and writer telemetry")) {
        return false;
    }
    if (!expect(frame_results.size() == source_frame_ids.size(),
                "callback emits exactly one result for every admitted frame")) {
        return false;
    }
    for (std::size_t index = 0; index < frame_results.size(); ++index) {
        const auto& result = frame_results[index];
        if (!expect(result.status == SpatialRoiLosslessFrameResultStatus::Encoded &&
                        result.correlation.recording_frame_id ==
                            source_frame_ids[index] &&
                        result.correlation.roi_stream_frame_index == index + 1U &&
                        result.output_frame_index == index + 1U &&
                        result.nvenc_pts_assigned && result.nvenc_pts == index &&
                        result.packet_count == 1 && result.encoded_bytes > 0 &&
                        result.keyframe && result.failure_reason.empty() &&
                        result.geometry.encoded_raster.width == kWidth &&
                        result.geometry.encoded_raster.height == kHeight &&
                        result.geometry.content_rect.width == kWidth &&
                        result.geometry.content_rect.height == kHeight,
                    "callbacks are ordered and preserve exact source, output, packet, keyframe, and geometry evidence")) {
            return false;
        }
    }

    std::error_code file_error;
    if (!expect(std::filesystem::file_size(paths.output, file_error) > 0 &&
                    !file_error,
                "encoded MP4 is non-empty")) {
        return false;
    }
    stage("decode_validation_start");
    if (!decode_and_validate_lossless_mp4(paths.output, expected_luma)) {
        return false;
    }
    stage("decode_validation_complete");
    std::ifstream metadata(paths.metadata);
    std::string metadata_text((std::istreambuf_iterator<char>(metadata)), {});
    if (!expect(metadata_text.find(
                    "11,1,1,") != std::string::npos &&
                    metadata_text.find("27,2,2,") != std::string::npos &&
                    metadata_text.find("42,3,3,") != std::string::npos,
                "metadata preserves sparse recording IDs and one-based dense output indices")) {
        return false;
    }

    std::ifstream keyframes(paths.keyframes);
    nlohmann::json keyframe_json;
    keyframes >> keyframe_json;
    if (!expect(
            keyframe_json.is_object() && keyframe_json.size() == 8 &&
                keyframe_json.value("schema_id", std::string()) ==
                    "orange.spatial_roi_keyframe_summary" &&
                keyframe_json.value("schema_version", 0) == 1 &&
                keyframe_json.value("terminal", false) &&
                keyframe_json.value("codec", std::string()) == "hevc" &&
                keyframe_json.value("fps", 0U) == 30 &&
                keyframe_json.value("total_frames", 0U) == 3 &&
                keyframe_json.at("frame_index_sequence").size() == 3 &&
                keyframe_json.at("frame_index_sequence").value("first", -1) == 0 &&
                keyframe_json.at("frame_index_sequence").value("last", -1) == 2 &&
                keyframe_json.at("frame_index_sequence").value(
                    "zero_based_contiguous", false) &&
                keyframe_json.at("keyframe_policy").size() == 4 &&
                keyframe_json.at("keyframe_policy").value(
                    "name", std::string()) == "all_frames_idr" &&
                keyframe_json.at("keyframe_policy").value(
                    "keyframe_frames", 0U) == 3 &&
                keyframe_json.at("keyframe_policy").value(
                    "non_keyframe_frames", 1U) == 0 &&
                keyframe_json.at("keyframe_policy").value("satisfied", false),
            "compact keyframe sidecar is exact, closed, dense, and all-IDR")) {
        return false;
    }

    std::ifstream finalization(paths.finalization);
    nlohmann::json finalization_json;
    finalization >> finalization_json;
    if (!expect(finalization_json.value("status", std::string()) == "complete" &&
                    finalization_json.value("terminal", false) &&
                    finalization_json.at("container").value("finalized", false) &&
                    finalization_json.at(
                        "quicktime_full_frame_rate_playback_intent")
                        .value("patch_applied", false),
                "terminal finalization sidecar is complete")) {
        return false;
    }
    std::cout << "[SPATIAL_ROI_GPU_TEST] status=PASS frames=3 decoded=3"
              << " elapsed_ms=" << elapsed_ms() << std::endl;
    return true;
}

bool gpu_gop25_encode_checks()
{
    constexpr std::size_t kFrameCount = 51;
    constexpr std::uint32_t kFps = 100;
    const auto test_started = std::chrono::steady_clock::now();
    const auto elapsed_ms = [&]() {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::steady_clock::now() - test_started)
            .count();
    };
    const auto stage = [&](const std::string& name) {
        std::cout << "[SPATIAL_ROI_GPU_TEST] status=RUN profile=gop25 stage="
                  << name << " elapsed_ms=" << elapsed_ms() << std::endl;
    };
    int device_count = 0;
    const cudaError_t count_status = cudaGetDeviceCount(&device_count);
    if ((count_status == cudaSuccess && device_count == 0) ||
        count_status == cudaErrorNoDevice) {
        std::cout << "[SPATIAL_ROI_GPU_TEST] status=SKIP profile=gop25"
                  << " reason=no_cuda_device elapsed_ms=" << elapsed_ms()
                  << std::endl;
        return true;
    }
    if (!expect(count_status == cudaSuccess,
                std::string("GOP-25 CUDA device enumeration failed: ") +
                    cudaGetErrorString(count_status))) {
        std::cout << "[SPATIAL_ROI_GPU_TEST] status=FAIL profile=gop25"
                  << " stage=cuda_enumeration elapsed_ms=" << elapsed_ms()
                  << std::endl;
        return false;
    }
    if (!expect(cudaSetDevice(0) == cudaSuccess,
                "GOP-25 CUDA device 0 is usable")) {
        return false;
    }
    stage("cuda_context_ready");

    const TestPaths paths = test_paths("_gop25");
    struct RecordingRootCleanup final {
        std::filesystem::path root;
        ~RecordingRootCleanup()
        {
            std::error_code ignored;
            std::filesystem::remove_all(root, ignored);
        }
    } cleanup{paths.recording_root};

    auto config = valid_config(paths);
    config.profile_id = orange::spatial_roi::encoder::
        kSpatialRoiP1LowLatencyGop25ProfileId;
    config.preset = "p1";
    config.tuning = "ll";
    config.lossless = false;
    config.rate_control_mode = "vbr";
    config.quality_value = 20;
    config.gop_length = 25;
    config.luma_preserved_exactly = false;
    config.fps = kFps;
    config.queue_capacity = 64;
    config.max_queue_bytes = kNv12Bytes * config.queue_capacity;
    config.max_frames_per_stream = kFrameCount;
    std::string config_error;
    if (!expect(
            orange::spatial_roi::encoder::validate_spatial_roi_encoder_config(
                config, &config_error),
            "GOP-25 real-encoder config is accepted")) {
        std::cerr << config_error << '\n';
        return false;
    }

    // The encoder owns the first artifact bundle after construction. Keep a
    // second value only for immutable metadata identity construction.
    auto metadata_config = valid_config(paths);
    metadata_config.profile_id = orange::spatial_roi::encoder::
        kSpatialRoiP1LowLatencyGop25ProfileId;
    metadata_config.preset = "p1";
    metadata_config.tuning = "ll";
    metadata_config.lossless = false;
    metadata_config.rate_control_mode = "vbr";
    metadata_config.quality_value = 20;
    metadata_config.gop_length = 25;
    metadata_config.luma_preserved_exactly = false;
    metadata_config.fps = kFps;
    metadata_config.queue_capacity = config.queue_capacity;
    metadata_config.max_queue_bytes = config.max_queue_bytes;
    metadata_config.max_frames_per_stream = kFrameCount;
    metadata_config.artifacts = SpatialRoiLosslessEncoderArtifactBundle{};

    std::vector<SpatialRoiLosslessFrameResult> frame_results;
    frame_results.reserve(kFrameCount);
    config.frame_result_callback =
        [&](const SpatialRoiLosslessFrameResult& result,
            std::string* error_out) {
            std::string validation_error;
            if (!orange::spatial_roi::encoder::
                    validate_spatial_roi_lossless_frame_result(
                        result, &validation_error)) {
                if (error_out) {
                    *error_out = validation_error;
                }
                return false;
            }
            frame_results.push_back(result);
            return true;
        };

    std::unique_ptr<SpatialRoiLosslessEncoder> encoder;
    stage("nvenc_initialize_start");
    try {
        encoder = std::make_unique<SpatialRoiLosslessEncoder>(std::move(config));
    } catch (const std::exception& exception) {
        std::cerr << "[SPATIAL_ROI_GPU_TEST] status=FAIL profile=gop25"
                  << " stage=nvenc_init reason=" << exception.what()
                  << " elapsed_ms=" << elapsed_ms() << std::endl;
        return false;
    }
    stage("nvenc_initialize_complete");
    if (!expect(encoder->valid(),
                "GOP-25 P1 low-latency encoder worker initialized")) {
        std::cerr << encoder->error() << std::endl;
        return false;
    }

    for (std::size_t frame_offset = 0; frame_offset < kFrameCount;
         ++frame_offset) {
        const std::uint64_t source_frame_id = 1000U + frame_offset;
        const std::uint64_t roi_stream_frame_index = frame_offset + 1U;
        std::vector<std::uint8_t> host(kNv12Bytes, 128);
        const std::uint8_t luma = static_cast<std::uint8_t>(
            64U + (frame_offset % 160U));
        std::fill(host.begin(), host.begin() + kWidth * kHeight, luma);

        void* device_ptr = nullptr;
        if (!expect(cudaMalloc(&device_ptr, host.size()) == cudaSuccess,
                    "allocate GOP-25 immutable NV12 source")) {
            return false;
        }
        const cudaError_t copy_status =
            cudaMemcpy(device_ptr,
                       host.data(),
                       host.size(),
                       cudaMemcpyHostToDevice);
        if (!expect(copy_status == cudaSuccess,
                    "copy GOP-25 immutable NV12 source")) {
            (void)cudaFree(device_ptr);
            return false;
        }
        auto lifetime = std::shared_ptr<const void>(
            device_ptr,
            [](const void* pointer) {
                if (pointer) {
                    (void)cudaFree(const_cast<void*>(pointer));
                }
            });
        SpatialRoiLosslessDeviceView view;
        view.device_nv12 = static_cast<const unsigned char*>(device_ptr);
        view.lifetime = std::move(lifetime);
        view.metadata = metadata_for(
            metadata_config, source_frame_id, roi_stream_frame_index);
        if (!expect(encoder->Enqueue(view),
                    "GOP-25 immutable recorder-owned NV12 view is copied and encoded")) {
            return false;
        }
        if (frame_offset == 0 || frame_offset == 25 || frame_offset == 50) {
            stage("frame_" + std::to_string(roi_stream_frame_index) +
                  "_copy_acknowledged");
        }
    }

    stage("finalize_start");
    if (!expect(encoder->Finalize(),
                "GOP-25 P1 low-latency encoder finalizes successfully")) {
        std::cerr << encoder->error() << std::endl;
        return false;
    }
    stage("finalize_complete");
    const auto terminal = encoder->terminal_snapshot();
    if (!expect(terminal && terminal->terminal && terminal->successful &&
                    terminal->drain_completed && terminal->metadata_flushed &&
                    terminal->media_finalization_validated &&
                    terminal->artifacts_sealed && terminal->nonempty_stream &&
                    terminal->all_admitted_results_emitted &&
                    terminal->source_release_safe &&
                    terminal->terminal_reason == "complete" &&
                    terminal->writer.observed &&
                    !terminal->writer.failure_latched &&
                    terminal->writer.close_finalization_validated &&
                    terminal->writer.total_failures == 0 &&
                    terminal->counts.enqueue_attempted == kFrameCount &&
                    terminal->counts.enqueued == kFrameCount &&
                    terminal->counts.frame_results_emitted == kFrameCount &&
                    terminal->counts.encoded_results == kFrameCount &&
                    terminal->counts.failed_results == 0 &&
                    terminal->counts.encoded_packets == kFrameCount,
                "GOP-25 terminal snapshot proves one packet and result per frame")) {
        return false;
    }
    const auto stats = encoder->stats();
    if (!expect(stats.enqueue_attempted == kFrameCount &&
                    stats.enqueued == kFrameCount &&
                    stats.copy_completed == kFrameCount &&
                    stats.encoded_frames == kFrameCount &&
                    stats.encoded_packets == kFrameCount &&
                    stats.frame_results_emitted == kFrameCount &&
                    stats.encoded_results == kFrameCount &&
                    stats.failed_results == 0 && stats.rejected == 0 &&
                    stats.writer_failures == 0 && stats.media_finalization_validated &&
                    stats.artifacts_sealed && stats.finalized && !stats.failed,
                "GOP-25 stats prove one successful packet per admitted frame")) {
        return false;
    }
    if (!expect(frame_results.size() == kFrameCount,
                "GOP-25 callback emits exactly one result per frame")) {
        return false;
    }
    for (std::size_t index = 0; index < frame_results.size(); ++index) {
        const auto& result = frame_results[index];
        const bool expected_keyframe = index == 0 || index == 25 || index == 50;
        if (!expect(result.status == SpatialRoiLosslessFrameResultStatus::Encoded &&
                        result.correlation.roi_stream_frame_index == index + 1U &&
                        result.output_frame_index == index + 1U &&
                        result.nvenc_pts_assigned && result.nvenc_pts == index &&
                        result.packet_count == 1 && result.encoded_bytes > 0 &&
                        result.keyframe == expected_keyframe &&
                        result.failure_reason.empty(),
                    "GOP-25 callback preserves dense PTS, one packet, and exact IDR cadence")) {
            return false;
        }
    }

    std::error_code file_error;
    if (!expect(std::filesystem::file_size(paths.output, file_error) > 0 &&
                    !file_error,
                "GOP-25 encoded MP4 is non-empty")) {
        return false;
    }
    stage("decode_validation_start");
    if (!decode_and_validate_gop25_mp4(paths.output, kFrameCount, kFps)) {
        return false;
    }
    stage("decode_validation_complete");

    std::ifstream keyframes(paths.keyframes);
    nlohmann::json keyframe_json;
    keyframes >> keyframe_json;
    if (!expect(
            keyframe_json.is_object() && keyframe_json.size() == 8 &&
                keyframe_json.value("schema_id", std::string()) ==
                    "orange.spatial_roi_keyframe_summary" &&
                keyframe_json.value("schema_version", 0) == 1 &&
                keyframe_json.value("terminal", false) &&
                keyframe_json.value("codec", std::string()) == "hevc" &&
                keyframe_json.value("fps", 0U) == kFps &&
                keyframe_json.value("total_frames", std::uint64_t(0)) ==
                    kFrameCount &&
                keyframe_json.at("frame_index_sequence").size() == 3 &&
                keyframe_json.at("frame_index_sequence").value("first", -1) ==
                    0 &&
                keyframe_json.at("frame_index_sequence").value(
                    "last", std::uint64_t(0)) == kFrameCount - 1U &&
                keyframe_json.at("frame_index_sequence").value(
                    "zero_based_contiguous", false) &&
                keyframe_json.at("keyframe_policy").size() == 4 &&
                keyframe_json.at("keyframe_policy").value(
                    "name", std::string()) == "fixed_gop_25_idr" &&
                keyframe_json.at("keyframe_policy").value(
                    "keyframe_frames", std::uint64_t(0)) == 3 &&
                keyframe_json.at("keyframe_policy").value(
                    "non_keyframe_frames", std::uint64_t(0)) == 48 &&
                keyframe_json.at("keyframe_policy").value("satisfied", false),
            "GOP-25 keyframe sidecar is compact, dense, and exact")) {
        return false;
    }

    std::ifstream finalization(paths.finalization);
    nlohmann::json finalization_json;
    finalization >> finalization_json;
    if (!expect(finalization_json.value("status", std::string()) == "complete" &&
                    finalization_json.value("terminal", false) &&
                    finalization_json.at("container").value("finalized", false),
                "GOP-25 finalization sidecar is complete")) {
        return false;
    }
    std::cout << "[SPATIAL_ROI_GPU_TEST] status=PASS profile=gop25 frames="
              << kFrameCount << " keyframes=3 decoded=" << kFrameCount
              << " elapsed_ms=" << elapsed_ms() << std::endl;
    return true;
}

}  // namespace

int main()
{
    const TestPaths paths = test_paths();
    const bool host_ok = host_contract_checks(paths) &&
                         host_p1_low_latency_contract_checks(paths) &&
                         host_fixed_gop_writer_checks() &&
                         host_artifact_authority_rejection_checks(paths);
    const bool gpu_lossless_ok = host_ok && gpu_encode_checks(paths);
    const bool gpu_ok = gpu_lossless_ok && gpu_gop25_encode_checks();
    std::error_code ignored;
    std::filesystem::remove_all(paths.recording_root, ignored);
    if (!host_ok || !gpu_ok) {
        if (host_ok && !gpu_ok) {
            std::cerr << "[SPATIAL_ROI_GPU_TEST] status=FAIL stage=hardware_validation\n";
        }
        return 1;
    }
    std::cout << "[PASS] spatial ROI lossless encoder tests\n";
    return 0;
}
