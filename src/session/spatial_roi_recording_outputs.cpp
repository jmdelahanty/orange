#include "session/spatial_roi_recording_outputs.h"

#include <array>
#include <filesystem>
#include <limits>
#include <map>
#include <set>
#include <string>
#include <utility>

namespace orange::session::spatial_roi {
namespace {

namespace fs = std::filesystem;
using orange::session::RecordingOutputDescriptor;
using json = nlohmann::json;

constexpr std::size_t kExpectedStreamCount = 4;
constexpr std::array<const char*, 12> kArtifactKinds = {
    "video",           "metadata",        "keyframes",   "perf",
    "summary",         "status",          "video_sanity", "finalization",
    "recorder_log",    "transport_sidecar", "evidence",   "evidence_manifest"};

bool fail(std::string* error_out, std::string message)
{
    if (error_out) {
        *error_out = std::move(message);
    }
    return false;
}

bool safe_relative_path(const std::string& value)
{
    if (value.empty() || value.find('\0') != std::string::npos) {
        return false;
    }
    const fs::path path(value);
    if (path.empty() || path.is_absolute() || path == fs::path(".")) {
        return false;
    }
    for (const fs::path& component : path) {
        if (component == fs::path(".") || component == fs::path("..")) {
            return false;
        }
    }
    return true;
}

bool safe_absolute_root(const std::string& value)
{
    if (value.empty() || value.find('\0') != std::string::npos) {
        return false;
    }
    const fs::path path(value);
    return path.is_absolute() && path != path.root_path();
}

bool path_below(const fs::path& child,
                const fs::path& parent,
                std::string* relative_out)
{
    const fs::path relative = child.lexically_relative(parent);
    const std::string value = relative.generic_string();
    if (!safe_relative_path(value)) {
        return false;
    }
    if (relative_out) {
        *relative_out = value;
    }
    return true;
}

bool recording_relative_artifact_path(
    const SpatialRoiRecorderCameraContractView& camera_contract,
    const SpatialRoiRecorderArtifactPathView& artifact,
    std::string* relative_out)
{
    if (!relative_out || !safe_absolute_root(camera_contract.recording_root) ||
        !safe_absolute_root(camera_contract.artifact_root) ||
        artifact.absolute_path.empty() ||
        artifact.absolute_path.find('\0') != std::string::npos ||
        !safe_relative_path(artifact.relative_path)) {
        return false;
    }

    const fs::path recording_root(camera_contract.recording_root);
    const fs::path artifact_root(camera_contract.artifact_root);
    const fs::path absolute_path(artifact.absolute_path);
    if (!absolute_path.is_absolute()) {
        return false;
    }

    std::string artifact_root_relative;
    if (!path_below(artifact_root, recording_root, &artifact_root_relative)) {
        return false;
    }
    const fs::path expected_absolute =
        (artifact_root / fs::path(artifact.relative_path)).lexically_normal();
    if (absolute_path.lexically_normal() != expected_absolute) {
        return false;
    }
    return path_below(
        absolute_path.lexically_normal(),
        recording_root,
        relative_out);
}

bool same_raster(const orange::spatial_roi::SpatialRoiFrameRaster& lhs,
                const orange::spatial_roi::SpatialRoiFrameRaster& rhs)
{
    return lhs.width == rhs.width && lhs.height == rhs.height;
}

json authority_json(const SpatialRoiRecorderAuthorityView& authority)
{
    return {{"id", authority.id}, {"sha256", authority.sha256}};
}

bool rect_fits(const orange::spatial_roi::SpatialRoiFrameRect& rect,
               const orange::spatial_roi::SpatialRoiFrameRaster& raster)
{
    return rect.width > 0 && rect.height > 0 && rect.x <= raster.width &&
           rect.y <= raster.height && rect.width <= raster.width - rect.x &&
           rect.height <= raster.height - rect.y;
}

json raster_json(const orange::spatial_roi::SpatialRoiFrameRaster& raster)
{
    return {{"width", raster.width}, {"height", raster.height}};
}

json rect_json(const orange::spatial_roi::SpatialRoiFrameRect& rect)
{
    return {{"x", rect.x},
            {"y", rect.y},
            {"width", rect.width},
            {"height", rect.height}};
}

json padding_json(const orange::spatial_roi::SpatialRoiFramePadding& padding)
{
    return {{"left", padding.left},
            {"top", padding.top},
            {"right", padding.right},
            {"bottom", padding.bottom},
            {"value_mono8", padding.value_mono8}};
}

json encode_profile_json(const SpatialRoiRecorderEncodeProfileView& profile)
{
    return {{"profile_id", profile.profile_id},
            {"codec", profile.codec},
            {"preset", profile.preset},
            {"tuning", profile.tuning},
            {"lossless", profile.lossless},
            {"rate_control_mode", profile.rate_control_mode},
            {"quality_value", profile.quality_value},
            {"gop_length", profile.gop_length},
            {"aq", profile.aq},
            {"temporal_aq", profile.temporal_aq},
            {"lookahead", profile.lookahead},
            {"lookahead_depth", profile.lookahead_depth},
            {"frame_rate", profile.frame_rate},
            {"input_format", profile.input_format},
            {"encoded_format", profile.encoded_format},
            {"no_resize", profile.no_resize},
            {"luma_preserved_exactly", profile.luma_preserved_exactly},
            {"neutral_chroma_value", profile.neutral_chroma_value}};
}

bool supported_encode_profile(
    const SpatialRoiRecorderEncodeProfileView& profile)
{
    // Match the complete immutable profile tuple. The P1 profiles are lossy;
    // accepting luma_preserved_exactly=true would turn a metadata claim into
    // a false exactness guarantee.
    if (profile.codec != "hevc" || profile.input_format != "mono8" ||
        profile.encoded_format != "nv12" || !profile.no_resize ||
        profile.neutral_chroma_value != 128 ||
        profile.luma_preserved_exactly != profile.lossless) {
        return false;
    }
    const bool legacy_lossless =
        profile.profile_id == "hevc_p7_lossless_cqp0_gop1_v1" &&
        profile.codec == "hevc" && profile.preset == "p7" &&
        profile.tuning == "lossless" && profile.lossless &&
        profile.rate_control_mode == "cqp" && profile.quality_value == 0 &&
        profile.gop_length == 1 && !profile.aq && !profile.temporal_aq &&
        !profile.lookahead && profile.lookahead_depth == 0;
    const bool p1_low_latency_gop1 =
        profile.profile_id == "hevc_p1_low_latency_vbr_q20_gop1_v1" &&
        profile.codec == "hevc" && profile.preset == "p1" &&
        profile.tuning == "ll" && !profile.lossless &&
        profile.rate_control_mode == "vbr" && profile.quality_value == 20 &&
        profile.gop_length == 1 && !profile.aq && !profile.temporal_aq &&
        !profile.lookahead && profile.lookahead_depth == 0;
    const bool p1_low_latency_gop25 =
        profile.profile_id == "hevc_p1_low_latency_vbr_q20_gop25_v1" &&
        profile.codec == "hevc" && profile.preset == "p1" &&
        profile.tuning == "ll" && !profile.lossless &&
        profile.rate_control_mode == "vbr" && profile.quality_value == 20 &&
        profile.gop_length == 25 && !profile.aq && !profile.temporal_aq &&
        !profile.lookahead && profile.lookahead_depth == 0;
    return legacy_lossless || p1_low_latency_gop1 || p1_low_latency_gop25;
}

bool validate_artifacts(
    const SpatialRoiRecorderCameraContractView& camera_contract,
    const SpatialRoiRecorderStreamView& stream,
    std::set<std::string>* absolute_paths,
    std::set<std::string>* relative_paths,
    std::map<std::string, std::string>* relative_artifacts,
    std::string* error_out)
{
    if (!absolute_paths || !relative_paths || !relative_artifacts ||
        stream.artifacts.size() != kArtifactKinds.size()) {
        return fail(error_out,
                    "spatial ROI stream must contain exactly twelve artifacts");
    }

    relative_artifacts->clear();
    for (const char* kind : kArtifactKinds) {
        const auto artifact_it = stream.artifacts.find(kind);
        if (artifact_it == stream.artifacts.end()) {
            return fail(error_out,
                        "spatial ROI stream is missing artifact kind " +
                            std::string(kind));
        }
        std::string relative_path;
        if (!recording_relative_artifact_path(
                camera_contract,
                artifact_it->second,
                &relative_path)) {
            return fail(error_out,
                        "spatial ROI artifact path is not a safe recording-relative "
                        "path: " +
                            std::string(kind));
        }
        const std::string normalized_absolute_path =
            fs::path(artifact_it->second.absolute_path)
                .lexically_normal()
                .generic_string();
        if (!absolute_paths->insert(normalized_absolute_path)
                 .second ||
            !relative_paths->insert(relative_path).second) {
            return fail(error_out,
                        "spatial ROI artifact path collides across streams: " +
                            relative_path);
        }
        (*relative_artifacts)[kind] = relative_path;
    }
    return true;
}

bool valid_status(const std::string& status)
{
    return status == "pending" || status == "complete" || status == "failed";
}

}  // namespace

bool build_spatial_roi_recording_outputs(
    const SpatialRoiRecorderCameraContractView& camera_contract,
    const std::string& status,
    std::vector<orange::session::RecordingOutputDescriptor>* outputs_out,
    std::string* error_out)
{
    if (!outputs_out) {
        return fail(error_out, "spatial ROI output destination is null");
    }
    outputs_out->clear();
    if (error_out) {
        error_out->clear();
    }
    if (!valid_status(status)) {
        return fail(error_out,
                    "spatial ROI output status must be pending, complete, or failed");
    }
    if (camera_contract.schema_id !=
            kSpatialRoiRecorderCameraContractSchemaId ||
        camera_contract.schema_version !=
            kSpatialRoiRecorderCameraContractSchemaVersion ||
        camera_contract.product_kind != kSpatialRoiRecorderCameraProductKind) {
        return fail(error_out,
                    "spatial ROI camera contract schema/product identity is invalid");
    }
    if (camera_contract.camera_id < 0 || camera_contract.camera_serial.empty() ||
        camera_contract.recording_id.empty() ||
        camera_contract.session_id != camera_contract.recording_id ||
        camera_contract.recording_identity_token.empty() ||
        camera_contract.producer_generation.empty() ||
        camera_contract.spatial_roi_plan_sha256.empty() ||
        !safe_absolute_root(camera_contract.recording_root) ||
        !safe_absolute_root(camera_contract.artifact_root) ||
        camera_contract.native_raster.width == 0 ||
        camera_contract.native_raster.height == 0 ||
        camera_contract.analytics_gpu_id < 0 ||
        camera_contract.stream_count != kExpectedStreamCount ||
        camera_contract.stream_order.size() != kExpectedStreamCount ||
        camera_contract.streams.size() != kExpectedStreamCount) {
        return fail(error_out,
                    "spatial ROI camera contract has incomplete shared identity");
    }

    std::string artifact_root_relative;
    if (!path_below(fs::path(camera_contract.artifact_root),
                    fs::path(camera_contract.recording_root),
                    &artifact_root_relative)) {
        return fail(error_out,
                    "spatial ROI artifact root is not below recording root");
    }
    const auto analytics_gpu_it =
        camera_contract.analytics_gpu_by_camera_serial.find(
            camera_contract.camera_serial);
    if (camera_contract.analytics_gpu_by_camera_serial.size() != 1 ||
        analytics_gpu_it ==
            camera_contract.analytics_gpu_by_camera_serial.end() ||
        analytics_gpu_it->second != camera_contract.analytics_gpu_id ||
        camera_contract.recorder_gpu_by_logical_stream_id.size() !=
            kExpectedStreamCount) {
        return fail(error_out,
                    "spatial ROI camera contract GPU identity maps are inconsistent");
    }

    std::set<std::string> logical_stream_ids;
    std::set<std::string> roi_ids;
    std::set<std::string> region_ids;
    std::set<std::string> absolute_artifact_paths;
    std::set<std::string> relative_artifact_paths;
    std::vector<RecordingOutputDescriptor> outputs;
    outputs.reserve(kExpectedStreamCount);

    for (std::size_t index = 0; index < kExpectedStreamCount; ++index) {
        const SpatialRoiRecorderStreamView& stream =
            camera_contract.streams[index];
        const std::string& ordered_stream_id =
            camera_contract.stream_order[index];
        const auto recorder_gpu_it =
            camera_contract.recorder_gpu_by_logical_stream_id.find(
                ordered_stream_id);
        if (ordered_stream_id.empty() ||
            !logical_stream_ids.insert(ordered_stream_id).second ||
            stream.stream_id != ordered_stream_id ||
            stream.logical_stream_id != ordered_stream_id ||
            stream.stream_kind != kSpatialRoiRecordingOutputKind ||
            stream.output_kind != kSpatialRoiRecordingOutputKind ||
            stream.camera_id != camera_contract.camera_id ||
            stream.camera_serial != camera_contract.camera_serial ||
            stream.recording_id != camera_contract.recording_id ||
            stream.session_id != camera_contract.session_id ||
            stream.recording_identity_token !=
                camera_contract.recording_identity_token ||
            stream.producer_generation != camera_contract.producer_generation ||
            stream.spatial_roi_plan_sha256 !=
                camera_contract.spatial_roi_plan_sha256 ||
            stream.analytics_gpu_id != camera_contract.analytics_gpu_id ||
            stream.source_gpu_id != stream.analytics_gpu_id ||
            stream.recorder_gpu_id < 0 ||
            stream.assigned_gpu_id != stream.recorder_gpu_id ||
            recorder_gpu_it ==
                camera_contract.recorder_gpu_by_logical_stream_id.end() ||
            stream.recorder_gpu_id != recorder_gpu_it->second ||
            stream.roi_id.empty() || stream.region_id.empty() ||
            stream.arena_group_id.empty() ||
            !roi_ids.insert(stream.roi_id).second ||
            !region_ids.insert(stream.region_id).second ||
            (stream.has_arena_id && stream.arena_id.empty()) ||
            (!stream.has_arena_id && !stream.arena_id.empty()) ||
            !same_raster(stream.geometry.native_raster,
                         camera_contract.native_raster) ||
            stream.geometry.source_coordinate_space !=
                "camera_native_full_frame_pixels" ||
            stream.geometry.video_coordinate_space !=
                "spatial_roi_encoded_pixels" ||
            !rect_fits(stream.geometry.content_rect,
                       stream.geometry.native_raster) ||
            stream.geometry.encoded_raster.width == 0 ||
            stream.geometry.encoded_raster.height == 0 ||
            !rect_fits(stream.geometry.encoded_content_rect,
                       stream.geometry.encoded_raster) ||
            stream.geometry.content_rect.width !=
                stream.geometry.encoded_content_rect.width ||
            stream.geometry.content_rect.height !=
                stream.geometry.encoded_content_rect.height ||
            stream.geometry.encoded_content_rect.x != 0 ||
            stream.geometry.encoded_content_rect.y != 0 ||
            stream.geometry.content_offset_x != 0 ||
            stream.geometry.content_offset_y != 0 ||
            stream.geometry.padding.left != 0 ||
            stream.geometry.padding.top != 0 ||
            stream.geometry.padding.value_mono8 != 0 ||
            stream.geometry.padding.right !=
                stream.geometry.encoded_raster.width -
                    stream.geometry.encoded_content_rect.width ||
            stream.geometry.padding.bottom !=
                stream.geometry.encoded_raster.height -
                    stream.geometry.encoded_content_rect.height ||
            stream.encode_fps == 0 ||
            stream.geometry.encoded_raster.width >
                static_cast<std::uint32_t>(std::numeric_limits<int>::max()) ||
            stream.geometry.encoded_raster.height >
                static_cast<std::uint32_t>(std::numeric_limits<int>::max()) ||
            stream.encode_fps >
                static_cast<std::uint32_t>(std::numeric_limits<int>::max()) ||
            stream.encode_profile.frame_rate != stream.encode_fps ||
            stream.codec != stream.encode_profile.codec ||
            stream.tuning != stream.encode_profile.tuning ||
            stream.rate_control_mode !=
                stream.encode_profile.rate_control_mode ||
            stream.quality_value != stream.encode_profile.quality_value ||
            stream.gop != stream.encode_profile.gop_length ||
            !supported_encode_profile(stream.encode_profile) ||
            stream.encode_profile.frame_rate == 0 ||
            stream.encode_queue_depth == 0 ||
            stream.expected_shard_gpu_ids.size() != 1 ||
            stream.expected_shard_gpu_ids.front() != stream.recorder_gpu_id) {
            return fail(error_out,
                        "spatial ROI stream identity/order/profile is inconsistent at index " +
                            std::to_string(index));
        }

        std::map<std::string, std::string> relative_artifacts;
        if (!validate_artifacts(camera_contract,
                                stream,
                                &absolute_artifact_paths,
                                &relative_artifact_paths,
                                &relative_artifacts,
                                error_out)) {
            return false;
        }

        const json geometry = {
            {"layout", authority_json(stream.geometry.layout)},
            {"materialization", authority_json(stream.geometry.materialization)},
            {"registration", authority_json(stream.geometry.registration)},
            {"native_raster", raster_json(stream.geometry.native_raster)},
            {"content_rect", rect_json(stream.geometry.content_rect)},
            {"encoded_raster", raster_json(stream.geometry.encoded_raster)},
            {"encoded_content_rect",
             rect_json(stream.geometry.encoded_content_rect)},
            {"content_offset",
             {{"x", stream.geometry.content_offset_x},
              {"y", stream.geometry.content_offset_y}}},
            {"padding", padding_json(stream.geometry.padding)},
            {"source_coordinate_space",
             stream.geometry.source_coordinate_space},
            {"video_coordinate_space", stream.geometry.video_coordinate_space},
        };
        const json source_geometry = {
            {"native_raster", raster_json(stream.geometry.native_raster)},
            {"content_rect", rect_json(stream.geometry.content_rect)},
            {"coordinate_space", stream.geometry.source_coordinate_space},
        };
        const json encoded_geometry = {
            {"encoded_raster", raster_json(stream.geometry.encoded_raster)},
            {"encoded_content_rect",
             rect_json(stream.geometry.encoded_content_rect)},
            {"raster", raster_json(stream.geometry.encoded_raster)},
            {"content_rect", rect_json(stream.geometry.encoded_content_rect)},
            {"coordinate_space", stream.geometry.video_coordinate_space},
        };
        json artifact_details = json::object();
        for (const char* kind : kArtifactKinds) {
            artifact_details[kind] = relative_artifacts.at(kind);
        }
        const json details = {
            {"stream_id", stream.stream_id},
            {"logical_stream_id", stream.logical_stream_id},
            {"stream_kind", stream.stream_kind},
            {"identity",
             {{"recording_id", stream.recording_id},
              {"recording_identity_token", stream.recording_identity_token},
              {"producer_generation", stream.producer_generation},
              {"spatial_roi_plan_sha256", stream.spatial_roi_plan_sha256},
              {"camera_id", stream.camera_id},
              {"camera_serial", stream.camera_serial},
              {"arena_group_id", stream.arena_group_id},
              {"arena_id", stream.has_arena_id ? json(stream.arena_id)
                                               : json(nullptr)},
              {"region_id", stream.region_id},
              {"roi_id", stream.roi_id},
              {"logical_stream_id", stream.logical_stream_id}}},
            {"recording_id", stream.recording_id},
            {"session_id", stream.session_id},
            {"recording_identity_token", stream.recording_identity_token},
            {"producer_generation", stream.producer_generation},
            {"spatial_roi_plan_sha256", stream.spatial_roi_plan_sha256},
            {"frame_identity",
             {{"key_fields", stream.frame_identity_key_fields},
              {"roi_stream_frame_index", stream.roi_stream_frame_index_mode},
              {"recording_frame_id_source", stream.recording_frame_id_source}}},
            {"camera_id", stream.camera_id},
            {"camera_serial", stream.camera_serial},
            {"analytics_gpu_id", stream.analytics_gpu_id},
            {"source_gpu_id", stream.source_gpu_id},
            {"recorder_gpu_id", stream.recorder_gpu_id},
            {"assigned_gpu_id", stream.assigned_gpu_id},
            {"roi_id", stream.roi_id},
            {"region_id", stream.region_id},
            {"arena_group_id", stream.arena_group_id},
            {"arena_id", stream.has_arena_id ? json(stream.arena_id) : json(nullptr)},
            {"geometry", geometry},
            {"geometry_identity", geometry},
            {"source_geometry", source_geometry},
            {"encoded_geometry", encoded_geometry},
            {"encode_profile", encode_profile_json(stream.encode_profile)},
            {"encode_fps", stream.encode_fps},
            {"gop", stream.gop},
            {"rate_control_mode", stream.rate_control_mode},
            {"quality_value", stream.quality_value},
            {"encode_queue_depth", stream.encode_queue_depth},
            {"routing_policy", stream.routing_policy},
            {"expected_shard_gpu_ids", stream.expected_shard_gpu_ids},
            {"artifact_path_scope", "recording_root_relative"},
            {"artifact_root_relative", artifact_root_relative},
            {"artifacts", artifact_details},
        };

        RecordingOutputDescriptor output;
        output.camera_serial = stream.camera_serial;
        output.output_kind = kSpatialRoiRecordingOutputKind;
        output.logical_stream_id = stream.logical_stream_id;
        output.role = kRuntimeDerivedAcquisitionInputRole;
        output.backend = "external_ipc";
        output.status = status;
        output.video_path = relative_artifacts.at("video");
        output.metadata_path = relative_artifacts.at("metadata");
        output.keyframe_path = relative_artifacts.at("keyframes");
        output.perf_path = relative_artifacts.at("perf");
        output.summary_path = relative_artifacts.at("summary");
        output.width = static_cast<int>(stream.geometry.encoded_raster.width);
        output.height = static_cast<int>(stream.geometry.encoded_raster.height);
        output.frame_rate = static_cast<int>(stream.encode_fps);
        output.codec = stream.codec;
        output.container = "mp4";
        output.tuning = stream.tuning;
        output.pixel_source_format = stream.encode_profile.input_format;
        output.encoded_format = stream.encode_profile.encoded_format;
        output.coordinate_space = stream.geometry.source_coordinate_space;
        output.video_pixel_coordinate_space =
            stream.geometry.video_coordinate_space;
        output.source_geometry_coordinate_space =
            stream.geometry.source_coordinate_space;
        output.details = details;
        outputs.push_back(std::move(output));
    }

    if (logical_stream_ids.size() != kExpectedStreamCount ||
        roi_ids.size() != kExpectedStreamCount ||
        region_ids.size() != kExpectedStreamCount ||
        absolute_artifact_paths.size() !=
            kExpectedStreamCount * kArtifactKinds.size() ||
        relative_artifact_paths.size() !=
            kExpectedStreamCount * kArtifactKinds.size()) {
        return fail(error_out,
                    "spatial ROI output identities or artifact paths are not unique");
    }
    *outputs_out = std::move(outputs);
    return true;
}

}  // namespace orange::session::spatial_roi
