#include "spatial_roi_finalized_session_receipt.h"

#include "gui/spatial_layout/sha256.h"
#include "session/spatial_roi_recording_config.h"
#include "spatial_roi_recorder_evidence.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <limits>
#include <set>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>

namespace orange::spatial_roi::recording {
namespace {

using json = nlohmann::json;
using CameraContractView =
    orange::session::spatial_roi::SpatialRoiRecorderCameraContractView;
using StreamView =
    orange::session::spatial_roi::SpatialRoiRecorderStreamView;
using Binding = SpatialRoiRecorderEvidenceBinding;

constexpr std::array<const char*, 12> kArtifactKinds = {
    "video", "metadata", "keyframes", "perf", "summary", "status",
    "video_sanity", "finalization", "recorder_log", "transport_sidecar",
    "evidence", "evidence_manifest"};

bool fail(std::string* error_out, std::string message)
{
    if (error_out) {
        *error_out = std::move(message);
    }
    return false;
}

json raster_json(const orange::spatial_roi::SpatialRoiFrameRaster& value)
{
    return {{"width", value.width}, {"height", value.height}};
}

json rect_json(const orange::spatial_roi::SpatialRoiFrameRect& value)
{
    return {{"x", value.x}, {"y", value.y}, {"width", value.width},
            {"height", value.height}};
}

json padding_json(const orange::spatial_roi::SpatialRoiFramePadding& value)
{
    return {{"left", value.left},
            {"top", value.top},
            {"right", value.right},
            {"bottom", value.bottom},
            {"value_mono8", value.value_mono8}};
}

json authority_json(
    const orange::session::spatial_roi::SpatialRoiRecorderAuthorityView& value)
{
    return {{"id", value.id}, {"sha256", value.sha256}};
}

json geometry_json(
    const orange::session::spatial_roi::SpatialRoiRecorderGeometryView& value)
{
    return {{"layout", authority_json(value.layout)},
            {"materialization", authority_json(value.materialization)},
            {"registration", authority_json(value.registration)},
            {"native_raster", raster_json(value.native_raster)},
            {"content_rect", rect_json(value.content_rect)},
            {"encoded_raster", raster_json(value.encoded_raster)},
            {"encoded_content_rect", rect_json(value.encoded_content_rect)},
            {"content_offset", {{"x", value.content_offset_x},
                                 {"y", value.content_offset_y}}},
            {"padding", padding_json(value.padding)},
            {"source_coordinate_space", value.source_coordinate_space},
            {"video_coordinate_space", value.video_coordinate_space}};
}

json profile_json(
    const orange::session::spatial_roi::SpatialRoiRecorderEncodeProfileView& value)
{
    return {{"codec", value.codec},
            {"tuning", value.tuning},
            {"lossless", value.lossless},
            {"gop_length", value.gop_length},
            {"aq", value.aq},
            {"temporal_aq", value.temporal_aq},
            {"lookahead", value.lookahead},
            {"lookahead_depth", value.lookahead_depth},
            {"frame_rate", value.frame_rate},
            {"input_format", value.input_format},
            {"encoded_format", value.encoded_format},
            {"no_resize", value.no_resize},
            {"luma_preserved_exactly", value.luma_preserved_exactly},
            {"neutral_chroma_value", value.neutral_chroma_value}};
}

json artifact_path_json(
    const orange::session::spatial_roi::SpatialRoiRecorderArtifactPathView& value)
{
    return {{"absolute_path", value.absolute_path},
            {"relative_path", value.relative_path}};
}

json stream_json(const StreamView& value)
{
    json artifacts = json::object();
    for (const auto& [kind, artifact] : value.artifacts) {
        artifacts[kind] = artifact_path_json(artifact);
    }
    return {{"stream_id", value.stream_id},
            {"logical_stream_id", value.logical_stream_id},
            {"stream_kind", value.stream_kind},
            {"output_kind", value.output_kind},
            {"camera_id", value.camera_id},
            {"camera_serial", value.camera_serial},
            {"env_key", value.env_key},
            {"socket_path", value.socket_path},
            {"analytics_gpu_id", value.analytics_gpu_id},
            {"recorder_gpu_id", value.recorder_gpu_id},
            {"source_gpu_id", value.source_gpu_id},
            {"assigned_gpu_id", value.assigned_gpu_id},
            {"roi_id", value.roi_id},
            {"region_id", value.region_id},
            {"arena_group_id", value.arena_group_id},
            {"has_arena_id", value.has_arena_id},
            {"arena_id", value.has_arena_id ? json(value.arena_id)
                                               : json(nullptr)},
            {"recording_id", value.recording_id},
            {"session_id", value.session_id},
            {"recording_identity_token", value.recording_identity_token},
            {"producer_generation", value.producer_generation},
            {"spatial_roi_plan_sha256", value.spatial_roi_plan_sha256},
            {"frame_identity_key_fields", value.frame_identity_key_fields},
            {"roi_stream_frame_index_mode", value.roi_stream_frame_index_mode},
            {"recording_frame_id_source", value.recording_frame_id_source},
            {"geometry", geometry_json(value.geometry)},
            {"encode_profile", profile_json(value.encode_profile)},
            {"encode_fps", value.encode_fps},
            {"codec", value.codec},
            {"tuning", value.tuning},
            {"rate_control_mode", value.rate_control_mode},
            {"quality_value", value.quality_value},
            {"gop", value.gop},
            {"encode_queue_depth", value.encode_queue_depth},
            {"detach_pool_frames", value.detach_pool_frames},
            {"max_detach_pool_bytes", value.max_detach_pool_bytes},
            {"max_queue_bytes", value.max_queue_bytes},
            {"writer_queue_max_packets", value.writer_queue_max_packets},
            {"writer_queue_max_bytes", value.writer_queue_max_bytes},
            {"operation_timeout_ms", value.operation_timeout_ms},
            {"max_frames_per_stream", value.max_frames_per_stream},
            {"max_media_bytes_per_stream", value.max_media_bytes_per_stream},
            {"max_evidence_bytes_per_stream", value.max_evidence_bytes_per_stream},
            {"routing_policy", value.routing_policy},
            {"expected_shard_gpu_ids", value.expected_shard_gpu_ids},
            {"artifacts", std::move(artifacts)}};
}

json ipc_json(
    const orange::session::spatial_roi::SpatialRoiRecorderIpcView& value)
{
    return {{"protocol", value.protocol},
            {"version", value.version},
            {"features", value.features},
            {"source_lifetime_mode", value.source_lifetime_mode},
            {"queue_capacity_frames_per_stream",
             value.queue_capacity_frames_per_stream},
            {"max_outstanding_frames_per_stream",
             value.max_outstanding_frames_per_stream},
            {"max_queue_capacity_frames_per_stream",
             value.max_queue_capacity_frames_per_stream},
            {"queue_capacity_frames_total", value.queue_capacity_frames_total},
            {"max_outstanding_frames_total",
             value.max_outstanding_frames_total}};
}

json aggregate_bounds_json(
    const orange::session::spatial_roi::SpatialRoiRecorderAggregateBoundsView& value)
{
    return {{"max_detach_pool_bytes_total", value.max_detach_pool_bytes_total},
            {"max_queue_bytes_total", value.max_queue_bytes_total},
            {"writer_queue_max_packets_total",
             value.writer_queue_max_packets_total},
            {"writer_queue_max_bytes_total", value.writer_queue_max_bytes_total},
            {"operation_timeout_ms_per_stream",
             value.operation_timeout_ms_per_stream},
            {"max_media_bytes_total", value.max_media_bytes_total},
            {"max_evidence_bytes_total", value.max_evidence_bytes_total}};
}

json camera_contract_json(const CameraContractView& value)
{
    json streams = json::array();
    for (const auto& stream : value.streams) {
        streams.push_back(stream_json(stream));
    }
    json analytics = json::object();
    for (const auto& [serial, gpu] : value.analytics_gpu_by_camera_serial) {
        analytics[serial] = gpu;
    }
    json recorders = json::object();
    for (const auto& [stream, gpu] : value.recorder_gpu_by_logical_stream_id) {
        recorders[stream] = gpu;
    }
    return {{"schema_id", value.schema_id},
            {"schema_version", value.schema_version},
            {"product_kind", value.product_kind},
            {"recording_id", value.recording_id},
            {"session_id", value.session_id},
            {"recording_identity_token", value.recording_identity_token},
            {"producer_generation", value.producer_generation},
            {"spatial_roi_plan_sha256", value.spatial_roi_plan_sha256},
            {"recording_root", value.recording_root},
            {"artifact_root", value.artifact_root},
            {"camera_id", value.camera_id},
            {"camera_serial", value.camera_serial},
            {"native_raster", raster_json(value.native_raster)},
            {"analytics_gpu_id", value.analytics_gpu_id},
            {"stream_count", value.stream_count},
            {"stream_order", value.stream_order},
            {"ipc_v2", ipc_json(value.ipc_v2)},
            {"aggregate_bounds", aggregate_bounds_json(value.aggregate_bounds)},
            {"analytics_gpu_by_camera_serial", std::move(analytics)},
            {"recorder_gpu_by_logical_stream_id", std::move(recorders)},
            {"streams", std::move(streams)}};
}

bool same_camera_contract(const CameraContractView& expected,
                          const CameraContractView& actual,
                          std::string* error_out)
{
    if (camera_contract_json(expected) != camera_contract_json(actual)) {
        return fail(error_out,
                    "camera contract changed before finalized receipt validation");
    }
    return true;
}

bool hash_open_file(
    const SpatialRoiRecorderArtifactRoot& authority,
    const std::string& relative_path,
    const std::uint64_t max_bytes,
    json* reference_out,
    std::set<std::pair<dev_t, ino_t>>* seen_inodes,
    std::string* error_out)
{
    if (!reference_out || max_bytes == 0) {
        return fail(error_out, "receipt artifact hash request is invalid");
    }
    std::unique_ptr<SpatialRoiRecorderArtifactFile> file;
    if (!authority.OpenExistingFile(
            relative_path, SpatialRoiRecorderArtifactFileAccess::kReadOnly,
            &file, error_out) ||
        !file || !file->valid() || file->relative_path() != relative_path ||
        file->artifact_root_identity() != authority.artifact_root_identity()) {
        return fail(error_out,
                    "receipt artifact could not be opened from the exact allow-list: " +
                        relative_path);
    }
    struct stat before {};
    if (::fstat(file->borrowed_fd(), &before) != 0 || !S_ISREG(before.st_mode) ||
        before.st_size < 0 ||
        static_cast<std::uint64_t>(before.st_size) > max_bytes) {
        return fail(error_out,
                    "receipt artifact is missing, non-regular, or exceeds its bound: " +
                        relative_path);
    }
    if (seen_inodes &&
        !seen_inodes->insert({before.st_dev, before.st_ino}).second) {
        return fail(error_out,
                    "receipt artifacts resolve to a duplicate regular-file inode: " +
                        relative_path);
    }

    int fd = file->borrowed_fd();
    if (::lseek(fd, 0, SEEK_SET) < 0) {
        return fail(error_out,
                    "receipt artifact could not seek for hashing: " + relative_path);
    }
    orange::gui::spatial_layout::checksum::StreamingSha256 hasher;
    std::array<std::uint8_t, 1024 * 1024> buffer{};
    std::uint64_t total = 0;
    for (;;) {
        const ssize_t count = ::read(fd, buffer.data(), buffer.size());
        if (count < 0 && errno == EINTR) {
            continue;
        }
        if (count < 0) {
            return fail(error_out,
                        "receipt artifact read failed for " + relative_path +
                            ": " + std::strerror(errno));
        }
        if (count == 0) {
            break;
        }
        const auto count_u64 = static_cast<std::uint64_t>(count);
        if (count_u64 > max_bytes - total) {
            return fail(error_out,
                        "receipt artifact grew beyond its bound: " + relative_path);
        }
        hasher.update(buffer.data(), static_cast<std::size_t>(count));
        total += count_u64;
    }
    struct stat after {};
    if (::fstat(fd, &after) != 0 || after.st_dev != before.st_dev ||
        after.st_ino != before.st_ino || after.st_size != before.st_size ||
        !file->VerifyCurrentBinding(error_out)) {
        return fail(error_out,
                    "receipt artifact changed during validation: " + relative_path);
    }
    *reference_out = {{"relative_path", relative_path}};
    (*reference_out)["size_bytes"] = total;
    (*reference_out)["sha256"] = "sha256:" + hasher.final_hex();
    return true;
}

bool append_manifest_artifact_receipt(
    const SpatialRoiRecorderArtifactRoot& authority,
    const Binding& binding,
    const json& manifest,
    const char* kind,
    std::set<std::pair<dev_t, ino_t>>* seen_inodes,
    json* output,
    std::string* error_out)
{
    if (!output || !manifest.is_object() || !manifest.contains("artifacts") ||
        !manifest.at("artifacts").is_object()) {
        return fail(error_out, "finalized manifest artifact receipts are missing");
    }
    const auto expected = binding.expected_artifacts.find(kind);
    if (expected == binding.expected_artifacts.end()) {
        return fail(error_out,
                    "finalized receipt contract is missing artifact kind " +
                        std::string(kind));
    }
    json expected_reference;
    if (std::string(kind) == "evidence") {
        if (!manifest.contains("evidence") ||
            !manifest.at("evidence").is_object()) {
            return fail(error_out, "finalized manifest evidence reference is invalid");
        }
        expected_reference = manifest.at("evidence");
    } else if (std::string(kind) == "evidence_manifest") {
        expected_reference = json::object();
    } else {
        if (!manifest.at("artifacts").contains(kind)) {
            return fail(error_out,
                        "finalized manifest is missing artifact kind " +
                            std::string(kind));
        }
        expected_reference = manifest.at("artifacts").at(kind);
    }
    if (std::string(kind) != "evidence_manifest" &&
        (!expected_reference.is_object() ||
         expected_reference.value("relative_path", std::string()) !=
             expected->second)) {
        return fail(error_out,
                    "finalized manifest artifact path does not match the contract: " +
                        std::string(kind));
    }
    json actual;
    const std::uint64_t max_bytes =
        std::string(kind) == "video"
            ? binding.max_media_bytes_per_stream
            : std::string(kind) == "evidence_manifest"
                  ? kSpatialRoiRecorderManifestMaxFileBytes
                  : binding.max_evidence_bytes_per_stream;
    if (!hash_open_file(authority, expected->second, max_bytes, &actual,
                        seen_inodes, error_out)) {
        return false;
    }
    if (std::string(kind) != "evidence_manifest" && actual != expected_reference) {
        return fail(error_out,
                    "finalized manifest artifact bytes do not match the validated receipt: " +
                        std::string(kind));
    }
    output->push_back({{"kind", kind}, {"relative_path", actual.at("relative_path")},
                       {"size_bytes", actual.at("size_bytes")},
                       {"sha256", actual.at("sha256")}});
    return true;
}

bool validate_complete_manifest(const json& manifest,
                                const Binding& binding,
                                std::string* error_out)
{
    if (!manifest.is_object() || !manifest.contains("terminal") ||
        !manifest.at("terminal").is_object() ||
        manifest.at("terminal").value("state", std::string()) != "complete" ||
        manifest.at("terminal").value("reason", std::string()) != "complete") {
        return fail(error_out,
                    "spatial ROI evidence manifest is pending or failed");
    }
    if (manifest.value("finalized_receipt_digest", std::string()).empty()) {
        return fail(error_out,
                    "spatial ROI evidence manifest lacks finalized receipt digest");
    }
    if (!manifest.contains("artifacts") || !manifest.at("artifacts").is_object() ||
        manifest.at("artifacts").size() != 10U ||
        !manifest.contains("evidence") || !manifest.at("evidence").is_object()) {
        return fail(error_out,
                    "spatial ROI evidence manifest does not contain the closed artifact set");
    }
    const json binding_json = manifest.value("binding", json::object());
    if (!binding_json.is_object() ||
        binding_json.value("stream", json::object()).value(
            "logical_stream_id", std::string()) != binding.logical_stream_id) {
        return fail(error_out,
                    "spatial ROI evidence manifest stream identity is substituted");
    }
    return true;
}

json stream_identity(const Binding& binding)
{
    return {{"recording_id", binding.recording_id},
            {"session_id", binding.session_id},
            {"recording_identity_token", binding.recording_identity_token},
            {"producer_generation", binding.producer_generation},
            {"spatial_roi_plan_sha256", binding.plan_sha256},
            {"camera_id", binding.camera_id},
            {"camera_serial", binding.camera_serial},
            {"roi_id", binding.roi_id},
            {"region_id", binding.region_id},
            {"arena_group_id", binding.arena_group_id},
            {"logical_stream_id", binding.logical_stream_id},
            {"assigned_gpu_id", binding.assigned_gpu_id},
            {"assigned_shard_id", binding.assigned_shard_id}};
}

}  // namespace

bool build_spatial_roi_finalized_session_receipt(
    const SpatialRoiFinalizedSessionReceiptRequest& request,
    nlohmann::json* receipt_out,
    std::string* error_out)
{
    if (!receipt_out) {
        return fail(error_out, "finalized session receipt destination is null");
    }
    *receipt_out = json::object();
    if (error_out) {
        error_out->clear();
    }
    try {
        orange::session::spatial_roi::SpatialRoiRecordingPlan parsed_plan;
        std::string authority_error;
        if (!orange::session::spatial_roi::parse_verified_plan(
                request.verified_plan, &parsed_plan, &authority_error)) {
            return fail(error_out,
                        "finalized receipt verified plan is not authenticated: " +
                            authority_error);
        }
        CameraContractView authenticated_camera_contract;
        if (!orange::session::spatial_roi::parse_spatial_roi_recorder_camera_contract(
                request.recorder_contract, request.verified_plan,
                request.expected_recording_root, request.expected_gpu_mapping,
                &authenticated_camera_contract, &authority_error)) {
            return fail(error_out,
                        "finalized receipt camera contract is not authenticated: " +
                            authority_error);
        }
        if (!same_camera_contract(request.camera_contract,
                                  authenticated_camera_contract,
                                  error_out)) {
            return false;
        }
        if (authenticated_camera_contract.stream_count != 4U ||
            authenticated_camera_contract.stream_order.size() != 4U ||
            authenticated_camera_contract.streams.size() != 4U ||
            parsed_plan.cameras.size() != 1U) {
            return fail(error_out,
                        "finalized receipt requires one camera and four streams");
        }

        std::vector<Binding> bindings;
        bindings.reserve(4U);
        std::vector<std::string> allowed_paths;
        allowed_paths.reserve(48U);
        std::set<std::string> unique_paths;
        for (std::size_t index = 0; index < 4U; ++index) {
            const std::string& logical_stream_id =
                authenticated_camera_contract.stream_order[index];
            if (logical_stream_id !=
                authenticated_camera_contract.streams[index].logical_stream_id) {
                return fail(error_out,
                            "finalized receipt stream order is not plan ordered");
            }
            Binding binding;
            if (!make_spatial_roi_recorder_evidence_binding(
                    request.recorder_contract, request.verified_plan,
                    request.expected_recording_root, request.expected_gpu_mapping,
                    logical_stream_id, &binding, &authority_error)) {
                return fail(error_out,
                            "finalized receipt evidence binding failed: " +
                                authority_error);
            }
            if (binding.expected_artifacts.size() != kArtifactKinds.size()) {
                return fail(error_out,
                            "finalized receipt stream does not contain exactly twelve artifacts");
            }
            for (const char* kind : kArtifactKinds) {
                const auto found = binding.expected_artifacts.find(kind);
                if (found == binding.expected_artifacts.end() ||
                    !unique_paths.insert(found->second).second) {
                    return fail(error_out,
                                "finalized receipt contract has missing or duplicate artifact paths");
                }
                allowed_paths.push_back(found->second);
            }
            bindings.push_back(std::move(binding));
        }

        std::unique_ptr<SpatialRoiRecorderArtifactRoot> opened_root;
        if (!SpatialRoiRecorderArtifactRoot::OpenExisting(
                request.expected_recording_root, allowed_paths, &opened_root,
                &authority_error) ||
            !opened_root || !opened_root->valid()) {
            return fail(error_out,
                        "finalized receipt could not reopen the exact contract artifact root: " +
                            authority_error);
        }
        const auto artifact_authority =
            std::shared_ptr<SpatialRoiRecorderArtifactRoot>(
                std::move(opened_root));

        json streams = json::array();
        std::set<std::string> stream_ids;
        std::set<std::pair<dev_t, ino_t>> seen_inodes;
        for (std::size_t index = 0; index < bindings.size(); ++index) {
            const Binding& binding = bindings[index];
            if (!stream_ids.insert(binding.logical_stream_id).second) {
                return fail(error_out,
                            "finalized receipt contains duplicate stream identity");
            }
            json manifest;
            const std::string manifest_path =
                binding.expected_artifacts.at("evidence_manifest");
            if (!read_and_validate_spatial_roi_recorder_finalized_manifest(
                    artifact_authority, manifest_path, binding, &manifest,
                    &authority_error) ||
                !validate_complete_manifest(manifest, binding, &authority_error)) {
                return fail(error_out,
                            "finalized receipt manifest validation failed for " +
                                binding.logical_stream_id + ": " + authority_error);
            }
            json artifacts = json::array();
            // The evidence validator already performs a bounded hash pass over
            // the ten manifest artifacts and the evidence JSONL.  It exposes
            // no receipt vector, however, and does not hash the manifest file
            // itself.  Repeat the descriptor-bound pass intentionally so this
            // returned receipt contains exact bytes for all twelve artifacts
            // (and so a replacement between validation phases is rejected).
            for (const char* kind : kArtifactKinds) {
                if (!append_manifest_artifact_receipt(
                        *artifact_authority, binding, manifest, kind,
                        &seen_inodes, &artifacts, &authority_error)) {
                    return fail(error_out,
                                "finalized receipt artifact validation failed for " +
                                    binding.logical_stream_id + ": " + authority_error);
                }
            }
            if (artifacts.size() != kArtifactKinds.size()) {
                return fail(error_out,
                            "finalized receipt did not close exactly twelve artifacts");
            }
            streams.push_back({{"logical_stream_id", binding.logical_stream_id},
                               {"identity", stream_identity(binding)},
                               {"counts", manifest.at("counts")},
                               {"ranges", manifest.at("ranges")},
                               {"finalized_receipt_digest",
                                manifest.at("finalized_receipt_digest")},
                               {"artifacts", std::move(artifacts)}});
        }

        const auto& recording_identity =
            artifact_authority->recording_root_identity();
        const auto& artifact_identity =
            artifact_authority->artifact_root_identity();
        json identity = {
            {"recording_id", authenticated_camera_contract.recording_id},
            {"session_id", authenticated_camera_contract.session_id},
            {"recording_identity_token",
             authenticated_camera_contract.recording_identity_token},
            {"producer_generation", authenticated_camera_contract.producer_generation},
            {"spatial_roi_plan_sha256",
             authenticated_camera_contract.spatial_roi_plan_sha256},
            {"camera_id", authenticated_camera_contract.camera_id},
            {"camera_serial", authenticated_camera_contract.camera_serial},
            {"stream_count", authenticated_camera_contract.stream_count},
            {"stream_order", authenticated_camera_contract.stream_order}};
        json root_continuity = {
            {"proven", json::array({
                "the current recording-root path is an opened directory",
                "the current external_spatial_roi_recorder child is an opened non-symlink directory",
                "all listed artifacts are opened beneath the retained descriptor-bound child",
                "all listed artifact path bindings are verified during hashing"})},
            {"not_proven", json::array({
                "that the current path names the same root inode used before the recorder child exited",
                "historical continuity across an unprovided producer-to-child root-identity handoff",
                "atomic simultaneity of the four stream finalizations"})}};
        json root_authority = {
            {"artifact_root_relative", kSpatialRoiRecorderArtifactDirectory},
            {"recording_root_identity", {{"device", recording_identity.device},
                                           {"inode", recording_identity.inode}}},
            {"artifact_root_identity", {{"device", artifact_identity.device},
                                          {"inode", artifact_identity.inode}}},
            {"root_continuity", std::move(root_continuity)}};
        *receipt_out = {
            {"schema_id", kSpatialRoiFinalizedSessionReceiptSchemaId},
            {"schema_version", kSpatialRoiFinalizedSessionReceiptSchemaVersion},
            {"canonicalization", "canonical_json_utf8_sort_keys_compact_v1"},
            {"stream_kind", "fixed_region"},
            {"status", "complete"},
            {"stream_count", authenticated_camera_contract.stream_count},
            {"stream_order", authenticated_camera_contract.stream_order},
            {"identity", std::move(identity)},
            {"root_authority", std::move(root_authority)},
            {"streams", std::move(streams)}};
        return true;
    } catch (const std::exception& exception) {
        return fail(error_out,
                    std::string("finalized session receipt failed: ") +
                        exception.what());
    } catch (...) {
        return fail(error_out,
                    "finalized session receipt failed: unknown exception");
    }
}

}  // namespace orange::spatial_roi::recording
