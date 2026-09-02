#include "session/spatial_roi_recorder_contract.h"

#include "session/spatial_roi_recording_config.h"
#include "spatial_roi_ipc_protocol.h"
#include "spatial_roi_recorder_cuda_detach.h"
#include "spatial_roi_recorder_storage_preflight.h"

#include <cstdint>
#include <filesystem>
#include <exception>
#include <limits>
#include <set>
#include <string>
#include <utility>

namespace orange::session::spatial_roi {
namespace {

using json = nlohmann::json;

// Keep this in sync with the closed ROI IPC-v2 grammar.  The recorder
// contract intentionally does not link the wire-protocol implementation, but
// it must reject a verified plan whose per-stream queue cannot be represented
// by an IPC-v2 HELLO message.
constexpr std::uint32_t kMaxSpatialRoiIpcQueueFrames = 4096;

bool fail(std::string* error_out, std::string message)
{
    if (error_out) {
        *error_out = std::move(message);
    }
    return false;
}

bool is_absolute_recording_root(const std::string& value,
                                std::filesystem::path* root_out,
                                std::string* error_out)
{
    if (value.empty()) {
        return fail(error_out, "spatial ROI recorder recording_root is empty");
    }

    const std::filesystem::path root(value);
    if (!root.is_absolute()) {
        return fail(error_out,
                    "spatial ROI recorder recording_root must be absolute");
    }
    if (root == root.root_path()) {
        return fail(error_out,
                    "spatial ROI recorder recording_root must not be filesystem root");
    }
    if (root.lexically_normal() != root) {
        return fail(error_out,
                    "spatial ROI recorder recording_root must not contain . or .. components");
    }
    if (root.string().find('\0') != std::string::npos) {
        return fail(error_out,
                    "spatial ROI recorder recording_root contains a NUL byte");
    }
    if (root_out) {
        *root_out = root;
    }
    return true;
}

bool read_positive_u32(const json& object,
                       const char* key,
                       std::uint32_t* value_out,
                       const std::string& path,
                       std::string* error_out)
{
    if (!object.is_object() || !object.contains(key) ||
        !object.at(key).is_number_unsigned() || object.at(key).get<std::uint64_t>() == 0 ||
        object.at(key).get<std::uint64_t>() >
            std::numeric_limits<std::uint32_t>::max()) {
        return fail(error_out, path + "." + key + " must be a positive uint32");
    }
    if (value_out) {
        *value_out = object.at(key).get<std::uint32_t>();
    }
    return true;
}

bool read_exact_string(const json& object,
                       const char* key,
                       std::string* value_out,
                       const std::string& path,
                       std::string* error_out)
{
    if (!object.is_object() || !object.contains(key) ||
        !object.at(key).is_string() || object.at(key).get<std::string>().empty()) {
        return fail(error_out, path + "." + key + " must be a nonempty string");
    }
    if (value_out) {
        *value_out = object.at(key).get<std::string>();
    }
    return true;
}

json rect_json(const Rect& rect)
{
    return {
        {"x", rect.x},
        {"y", rect.y},
        {"width", rect.width},
        {"height", rect.height},
    };
}

json raster_json(const Raster& raster)
{
    return {{"width", raster.width}, {"height", raster.height}};
}

// The plan's closed profile is projected into the recorder contract with the
// fixed Mono8 -> NV12 geometry fields that are part of this recorder's wire
// contract.  Keep the profile identifier and every encoder policy field
// authenticated; the derived pixel fields are intentionally repeated on each
// stream so a consumer never has to reconstruct them from a profile name.
json encode_profile_json(const EncodeProfile& profile,
                         const std::uint32_t frame_rate,
                         const bool include_encoder_controls)
{
    json value = {
        {"profile_id", profile.name},
        {"codec", profile.codec},
        {"preset", profile.preset},
        {"tuning", profile.tuning},
        {"lossless", profile.lossless},
        {"rate_control_mode", profile.rate_control_mode},
        {"quality_value", profile.quality_value},
        {"gop_length", profile.gop_length},
        {"frame_rate", frame_rate},
        {"input_format", kSourcePixelFormat},
        {"encoded_format", "nv12"},
        {"no_resize", true},
        {"luma_preserved_exactly", profile.lossless},
        {"neutral_chroma_value", 128},
    };
    if (include_encoder_controls) {
        value["aq"] = profile.aq;
        value["temporal_aq"] = profile.temporal_aq;
        value["lookahead"] = profile.lookahead;
        value["lookahead_depth"] = profile.lookahead_depth;
    }
    return value;
}

bool check_exact_camera_gpu_mapping(
    const SpatialRoiRecordingPlan& plan,
    const SpatialRoiRecorderRuntimeGpuMapping& gpu_mapping,
    std::set<std::string>* logical_stream_ids_out,
    std::string* error_out)
{
    std::set<std::string> expected_cameras;
    std::set<std::string> expected_streams;
    for (const auto& [serial, camera] : plan.cameras) {
        (void)camera;
        expected_cameras.insert(serial);
    }
    for (const auto& [serial, camera] : plan.cameras) {
        (void)serial;
        for (const auto& roi : camera.rois) {
            if (!expected_streams.insert(roi.logical_stream_id).second) {
                return fail(error_out,
                            "verified spatial ROI plan has duplicate logical_stream_id " +
                                roi.logical_stream_id);
            }
        }
    }

    if (gpu_mapping.analytics_gpu_by_camera_serial.size() !=
        expected_cameras.size()) {
        return fail(error_out,
                    "analytics GPU mapping must contain exactly one entry per plan camera");
    }
    for (const auto& [serial, gpu_id] :
         gpu_mapping.analytics_gpu_by_camera_serial) {
        if (!expected_cameras.count(serial)) {
            return fail(error_out,
                        "analytics GPU mapping contains an unknown camera " + serial);
        }
        if (gpu_id < 0) {
            return fail(error_out,
                        "analytics GPU mapping contains a negative GPU for " + serial);
        }
    }

    if (gpu_mapping.recorder_gpu_by_logical_stream_id.size() !=
        expected_streams.size()) {
        return fail(error_out,
                    "recorder GPU mapping must contain exactly one entry per plan ROI stream");
    }
    for (const auto& [stream_id, gpu_id] :
         gpu_mapping.recorder_gpu_by_logical_stream_id) {
        if (!expected_streams.count(stream_id)) {
            return fail(error_out,
                        "recorder GPU mapping contains an unknown logical_stream_id " +
                            stream_id);
        }
        if (gpu_id < 0) {
            return fail(error_out,
                        "recorder GPU mapping contains a negative GPU for " + stream_id);
        }
    }
    if (logical_stream_ids_out) {
        *logical_stream_ids_out = std::move(expected_streams);
    }
    return true;
}

bool find_resolved_roi(const json& resolved_camera,
                       const std::string& roi_id,
                       const json** roi_out,
                       const std::string& path,
                       std::string* error_out)
{
    if (!resolved_camera.is_object() || !resolved_camera.contains("rois") ||
        !resolved_camera.at("rois").is_array()) {
        return fail(error_out, path + ".rois must be an array");
    }
    const json* match = nullptr;
    for (const auto& candidate : resolved_camera.at("rois")) {
        if (!candidate.is_object() || !candidate.contains("roi_id") ||
            !candidate.at("roi_id").is_string()) {
            return fail(error_out, path + ".rois contains an invalid ROI");
        }
        if (candidate.at("roi_id").get<std::string>() == roi_id) {
            if (match != nullptr) {
                return fail(error_out,
                            path + ".rois duplicates roi_id " + roi_id);
            }
            match = &candidate;
        }
    }
    if (match == nullptr) {
        return fail(error_out, path + ".rois is missing roi_id " + roi_id);
    }
    if (roi_out) {
        *roi_out = match;
    }
    return true;
}

bool add_unique_path(std::set<std::string>* paths,
                     const std::string& path,
                     const std::string& kind,
                     std::string* error_out)
{
    if (!paths->insert(path).second) {
        return fail(error_out,
                    "spatial ROI recorder has duplicate " + kind + " path " + path);
    }
    return true;
}

bool checked_add(const std::uint64_t left,
                 const std::uint64_t right,
                 std::uint64_t* out)
{
    if (!out || right > std::numeric_limits<std::uint64_t>::max() - left) {
        return false;
    }
    *out = left + right;
    return true;
}

bool checked_multiply(const std::uint64_t left,
                      const std::uint64_t right,
                      std::uint64_t* out)
{
    if (!out || (left != 0 &&
                 right > std::numeric_limits<std::uint64_t>::max() / left)) {
        return false;
    }
    *out = left * right;
    return true;
}

bool nv12_queue_byte_budget(const Raster& encoded_raster,
                            const std::uint32_t queue_frames,
                            std::uint64_t* bytes_out,
                            std::string* error_out,
                            const std::string& path)
{
    std::uint64_t pixels = 0;
    std::uint64_t bytes_per_frame = 0;
    std::uint64_t queue_bytes = 0;
    if (!bytes_out || (encoded_raster.width % 2U) != 0U ||
        (encoded_raster.height % 2U) != 0U ||
        !checked_multiply(encoded_raster.width,
                          encoded_raster.height,
                          &pixels) ||
        !checked_add(pixels, pixels / 2U, &bytes_per_frame) ||
        !checked_multiply(bytes_per_frame, queue_frames, &queue_bytes) ||
        queue_bytes == 0) {
        return fail(error_out,
                    path + " detached NV12 encode queue byte budget overflowed");
    }
    *bytes_out = queue_bytes;
    return true;
}

bool detach_pool_byte_budget(const Raster& encoded_raster,
                             const std::uint32_t pool_frames,
                             std::uint64_t* bytes_out,
                             std::string* error_out,
                             const std::string& path)
{
    std::uint64_t pixels = 0;
    std::uint64_t nv12_bytes = 0;
    std::uint64_t bytes_per_slot = 0;
    std::uint64_t pool_bytes = 0;
    if (!bytes_out || pool_frames == 0 ||
        !checked_multiply(encoded_raster.width,
                          encoded_raster.height,
                          &pixels) ||
        !checked_add(pixels, pixels / 2U, &nv12_bytes) ||
        !checked_add(pixels, nv12_bytes, &bytes_per_slot) ||
        !checked_multiply(bytes_per_slot, pool_frames, &pool_bytes) ||
        pool_bytes == 0 ||
        pool_bytes > orange::spatial_roi::ipc::
            kSpatialRoiRecorderCudaDetachMaxPoolBytes) {
        return fail(
            error_out,
            path +
                " recorder Mono8+NV12 detach-pool byte budget overflowed or exceeds the implementation ceiling");
    }
    *bytes_out = pool_bytes;
    return true;
}

}  // namespace

bool build_spatial_roi_recorder_contract(
    const nlohmann::json& verified_plan,
    const std::string& recording_root,
    const SpatialRoiRecorderRuntimeGpuMapping& gpu_mapping,
    nlohmann::json* contract_out,
    std::string* error_out)
{
    if (!contract_out) {
        return fail(error_out,
                    "spatial ROI recorder contract destination is null");
    }
    if (error_out) {
        error_out->clear();
    }

    try {
        SpatialRoiRecordingPlan plan;
        if (!parse_verified_plan(verified_plan, &plan, error_out)) {
            return false;
        }
        const bool legacy_plan =
            plan.schema_version == kLegacyPlanSchemaVersion;
        if (!legacy_plan && plan.schema_version != kPlanSchemaVersion) {
            return fail(error_out,
                        "spatial ROI recorder contract requires plan schema v2 or v3");
        }
        const int contract_schema_version =
            legacy_plan ? kLegacySpatialRoiRecorderContractSchemaVersion
                        : kSpatialRoiRecorderContractSchemaVersion;
        const char* contract_scope =
            legacy_plan ? kLegacySpatialRoiRecorderContractScope
                        : kSpatialRoiRecorderContractScope;
        const char* contract_mode =
            legacy_plan ? kLegacySpatialRoiRecorderContractMode
                        : kSpatialRoiRecorderContractMode;
        const char* contract_backend =
            legacy_plan ? kLegacyBackend : kBackend;

        std::filesystem::path root;
        if (!is_absolute_recording_root(recording_root, &root, error_out)) {
            return false;
        }

        std::set<std::string> expected_stream_ids;
        if (!check_exact_camera_gpu_mapping(
                plan, gpu_mapping, &expected_stream_ids, error_out)) {
            return false;
        }

        const json& payload = verified_plan.at("plan");
        const json& configuration = payload.at("configuration");
        const json& configured_cameras = configuration.at("cameras");
        const json& resolved_cameras = payload.at("resolved_cameras");
        if (!configured_cameras.is_object() || !resolved_cameras.is_object()) {
            return fail(error_out,
                        "verified spatial ROI plan camera sections are not objects");
        }
        std::uint32_t encode_queue_depth = 0;
        if (!configuration.contains("buffering") ||
            !read_positive_u32(configuration.at("buffering"),
                               "queue_frames_per_stream",
                               &encode_queue_depth,
                               "plan.configuration.buffering",
                               error_out)) {
            return false;
        }
        if (encode_queue_depth > kMaxSpatialRoiIpcQueueFrames) {
            return fail(error_out,
                        "spatial ROI recorder queue_frames_per_stream exceeds the IPC-v2 bound");
        }

        const std::filesystem::path artifact_root =
            root / "external_spatial_roi_recorder";
        std::set<std::string> artifact_paths;
        std::set<std::string> environment_keys;
        std::set<std::string> socket_paths;
        json streams = json::object();
        json stream_order = json::array();
        std::size_t stream_count = 0;
        std::uint64_t max_detach_pool_bytes_total = 0;
        std::uint64_t max_queue_bytes_total = 0;
        std::uint64_t writer_queue_max_packets_total = 0;
        std::uint64_t writer_queue_max_bytes_total = 0;
        std::uint64_t max_media_bytes_total = 0;
        std::uint64_t max_evidence_bytes_total = 0;

        for (const auto& [camera_serial, camera] : plan.cameras) {
            const std::string camera_path =
                "plan.configuration.cameras[" + camera_serial + "]";
            if (!configured_cameras.contains(camera_serial) ||
                !resolved_cameras.contains(camera_serial)) {
                return fail(error_out,
                            "verified spatial ROI plan is missing camera " +
                                camera_serial);
            }
            const json& configured_camera = configured_cameras.at(camera_serial);
            const json& resolved_camera = resolved_cameras.at(camera_serial);

            std::uint32_t source_frame_rate = 0;
            if (!read_positive_u32(configured_camera,
                                   "source_frame_rate",
                                   &source_frame_rate,
                                   camera_path,
                                   error_out)) {
                return false;
            }
            std::string configured_serial;
            if (!read_exact_string(configured_camera,
                                   "camera_serial",
                                   &configured_serial,
                                   camera_path,
                                   error_out) ||
                configured_serial != camera_serial ||
                configured_serial != camera.camera_serial) {
                return fail(error_out,
                            camera_path + ".camera_serial does not match the verified camera");
            }
            if (configured_camera.value("camera_id", -1) != camera.camera_id ||
                resolved_camera.value("camera_id", -1) != camera.camera_id ||
                resolved_camera.value("camera_serial", std::string()) != camera_serial ||
                resolved_camera.value("arena_group_id", std::string()) !=
                    camera.arena_group_id) {
                return fail(error_out,
                            camera_path + " camera identity disagrees with resolved plan");
            }

            const int analytics_gpu =
                gpu_mapping.analytics_gpu_by_camera_serial.at(camera_serial);
            for (const auto& roi : camera.rois) {
                const std::string roi_path = camera_path + ".rois[" + roi.roi_id + "]";
                const json* resolved_roi = nullptr;
                if (!find_resolved_roi(
                        resolved_camera, roi.roi_id, &resolved_roi, roi_path, error_out)) {
                    return false;
                }

                const std::string expected_logical_id =
                    ::orange::session::spatial_roi::expected_logical_stream_id(
                        camera_serial, roi.roi_id);
                if (roi.logical_stream_id != expected_logical_id ||
                    resolved_roi->value("logical_stream_id", std::string()) !=
                        expected_logical_id) {
                    return fail(error_out,
                                roi_path + ".logical_stream_id does not match verified identity");
                }
                const std::string expected_stem =
                    ::orange::session::spatial_roi::expected_artifact_stem(
                        camera_serial, roi.roi_id);
                if (resolved_roi->value("artifact_stem", std::string()) !=
                    expected_stem) {
                    return fail(error_out,
                                roi_path + ".artifact_stem does not match verified identity");
                }
                if (!expected_stream_ids.count(roi.logical_stream_id)) {
                    return fail(error_out,
                                roi_path + " logical stream was not admitted in GPU mapping");
                }
                const int recorder_gpu =
                    gpu_mapping.recorder_gpu_by_logical_stream_id.at(
                        roi.logical_stream_id);
                if ((roi.encoded_raster.width % 2U) != 0U ||
                    (roi.encoded_raster.height % 2U) != 0U) {
                    return fail(
                        error_out,
                        roi_path +
                            " encoded raster must have even dimensions for NV12/HEVC");
                }
                if (static_cast<std::uint64_t>(roi.encoded_raster.width) *
                        roi.encoded_raster.height >
                    orange::spatial_roi::ipc::kSpatialRoiIpcMaxPackedMono8Bytes) {
                    return fail(
                        error_out,
                        roi_path +
                            " encoded raster exceeds the IPC-v2 packed Mono8 byte bound");
                }

                std::uint64_t max_detach_pool_bytes = 0;
                std::uint64_t max_queue_bytes = 0;
                if (!nv12_queue_byte_budget(roi.encoded_raster,
                                            encode_queue_depth,
                                            &max_queue_bytes,
                                            error_out,
                                            roi_path) ||
                    !detach_pool_byte_budget(roi.encoded_raster,
                                             encode_queue_depth,
                                             &max_detach_pool_bytes,
                                             error_out,
                                             roi_path)) {
                    return false;
                }
                if (!checked_add(max_detach_pool_bytes_total,
                                 max_detach_pool_bytes,
                                 &max_detach_pool_bytes_total) ||
                    !checked_add(max_queue_bytes_total,
                                 max_queue_bytes,
                                 &max_queue_bytes_total) ||
                    !checked_add(writer_queue_max_packets_total,
                                 kSpatialRoiRecorderWriterQueueMaxPackets,
                                 &writer_queue_max_packets_total) ||
                    !checked_add(writer_queue_max_bytes_total,
                                 kSpatialRoiRecorderWriterQueueMaxBytes,
                                 &writer_queue_max_bytes_total) ||
                    !checked_add(max_media_bytes_total,
                                 plan.recording_limits.max_media_bytes_per_stream,
                                 &max_media_bytes_total) ||
                    !checked_add(
                        max_evidence_bytes_total,
                        plan.recording_limits.max_evidence_bytes_per_stream,
                        &max_evidence_bytes_total)) {
                    return fail(error_out,
                                roi_path +
                                    " recorder aggregate byte/packet budget overflowed");
                }

                const std::string environment_key =
                    "spatial_roi_" + roi.logical_stream_id;
                const std::string socket_path = expected_socket_path(
                    plan.recording_identity_token, roi.logical_stream_id);
                if (!environment_keys.insert(environment_key).second) {
                    return fail(error_out,
                                "spatial ROI recorder has duplicate env_key " +
                                    environment_key);
                }
                if (!socket_paths.insert(socket_path).second) {
                    return fail(error_out,
                                "spatial ROI recorder has duplicate socket_path " +
                                    socket_path);
                }
                if (socket_path.size() >= 108) {
                    return fail(error_out,
                                roi_path + " socket_path exceeds Unix socket length limit");
                }

                const std::filesystem::path stream_artifact_root = artifact_root;
                const std::string stem = expected_stem;
                const std::map<std::string, std::string> artifact_names = {
                    {"video", stem + ".mp4"},
                    {"metadata", stem + "_meta.csv"},
                    {"keyframes", stem + "_keyframe.json"},
                    {"perf", stem + "_perf.csv"},
                    {"summary", stem + "_summary.json"},
                    {"status", stem + "_status.json"},
                    {"video_sanity", stem + "_video_sanity.json"},
                    {"finalization", stem + ".mp4.finalization.json"},
                    {"recorder_log", stem + "_recorder.log"},
                    {"transport_sidecar", stem + "_transport.jsonl"},
                    {"evidence", stem + "_evidence.jsonl"},
                    {"evidence_manifest", stem + "_evidence_manifest.json"},
                };
                json expected_artifacts = json::object();
                for (const auto& [kind, name] : artifact_names) {
                    const std::string path =
                        (stream_artifact_root / name).generic_string();
                    if (!add_unique_path(
                            &artifact_paths, path, "artifact", error_out)) {
                        return false;
                    }
                    expected_artifacts[kind] = path;
                }

                const json& native_raster = configured_camera.at("native_raster");
                const json& layout = configured_camera.at("layout");
                const json& materialization = configured_camera.at("materialization");
                const json& registration = configured_camera.at("registration");
                const Rect encoded_content_rect = roi.encoded_content_rect;
                const std::uint32_t right_padding =
                    resolved_roi->at("padding").at("right").get<std::uint32_t>();
                const std::uint32_t bottom_padding =
                    resolved_roi->at("padding").at("bottom").get<std::uint32_t>();

                const json stream = {
                    {"stream_id", roi.logical_stream_id},
                    {"logical_stream_id", roi.logical_stream_id},
                    {"stream_kind", "spatial_roi"},
                    {"output_kind", "spatial_roi"},
                    {"camera_id", camera.camera_id},
                    {"camera_serial", camera.camera_serial},
                    {"env_key", environment_key},
                    {"socket_path", socket_path},
                    {"analytics_gpu_id", analytics_gpu},
                    {"recorder_gpu_id", recorder_gpu},
                    {"source_gpu_id", analytics_gpu},
                    {"assigned_gpu_id", recorder_gpu},
                    {"roi_id", roi.roi_id},
                    {"region_id", roi.region_id},
                    {"arena_group_id", roi.arena_group_id},
                    {"arena_id", roi.has_arena_id ? json(roi.arena_id) : json(nullptr)},
                    {"recording_id", plan.recording_id},
                    {"session_id", plan.recording_id},
                    {"recording_identity_token", plan.recording_identity_token},
                    {"producer_generation", plan.producer_generation},
                    {"spatial_roi_plan_sha256", plan.plan_sha256},
                    {"frame_identity", {
                        {"key_fields", {"recording_identity_token",
                                        "producer_generation",
                                        "logical_stream_id",
                                        "recording_frame_id",
                                        "roi_stream_frame_index"}},
                        {"roi_stream_frame_index", "dense_one_based"},
                        {"recording_frame_id_source", "parent_camera_recording"},
                    }},
                    {"identity", {
                        {"recording_id", plan.recording_id},
                        {"recording_identity_token", plan.recording_identity_token},
                        {"producer_generation", plan.producer_generation},
                        {"spatial_roi_plan_sha256", plan.plan_sha256},
                        {"camera_id", camera.camera_id},
                        {"camera_serial", camera.camera_serial},
                        {"arena_group_id", roi.arena_group_id},
                        {"arena_id", roi.has_arena_id ? json(roi.arena_id) : json(nullptr)},
                        {"region_id", roi.region_id},
                        {"roi_id", roi.roi_id},
                        {"logical_stream_id", roi.logical_stream_id},
                    }},
                    {"geometry_identity", {
                        {"layout", layout},
                        {"materialization", materialization},
                        {"registration", registration},
                        {"native_raster", native_raster},
                        {"content_rect", rect_json(roi.source_rect)},
                        {"encoded_raster", raster_json({
                            roi.encoded_raster.width, roi.encoded_raster.height})},
                        {"encoded_content_rect", rect_json(encoded_content_rect)},
                        {"content_offset", {{"x", 0}, {"y", 0}}},
                        {"padding", {
                            {"left", 0},
                            {"top", 0},
                            {"right", right_padding},
                            {"bottom", bottom_padding},
                            {"value_mono8", 0},
                        }},
                        {"source_coordinate_space", "camera_native_full_frame_pixels"},
                        {"video_coordinate_space", "spatial_roi_encoded_pixels"},
                    }},
                    {"encode_profile", encode_profile_json(
                        plan.encode_profile,
                        source_frame_rate,
                        plan.schema_version == kPlanSchemaVersion)},
                    // Keep the direct names understood by the existing
                    // external-recorder materialization shape in addition to
                    // the nested profile above. These values are fixed here;
                    // a caller cannot override them per stream.
                    {"encode_fps", source_frame_rate},
                    {"codec", plan.encode_profile.codec},
                    {"tuning", plan.encode_profile.tuning},
                    {"rate_control_mode", plan.encode_profile.rate_control_mode},
                    {"quality_value", plan.encode_profile.quality_value},
                    {"gop", plan.encode_profile.gop_length},
                    {"encode_queue_depth", encode_queue_depth},
                    {"detach_pool_frames", encode_queue_depth},
                    {"max_detach_pool_bytes", max_detach_pool_bytes},
                    {"max_queue_bytes", max_queue_bytes},
                    {"writer_queue_max_packets",
                     kSpatialRoiRecorderWriterQueueMaxPackets},
                    {"writer_queue_max_bytes",
                     kSpatialRoiRecorderWriterQueueMaxBytes},
                    {"operation_timeout_ms",
                     kSpatialRoiRecorderOperationTimeoutMs},
                    {"max_frames_per_stream",
                     plan.recording_limits.max_frames_per_stream},
                    {"max_media_bytes_per_stream",
                     plan.recording_limits.max_media_bytes_per_stream},
                    {"max_evidence_bytes_per_stream",
                     plan.recording_limits.max_evidence_bytes_per_stream},
                    {"routing_policy", "single_shard"},
                    {"expected_shard_gpu_ids", {recorder_gpu}},
                    {"recording_control", {
                        {"record_for_seconds", 0},
                        {"clip_seconds", 0},
                    }},
                    {"rollover", {
                        {"requested", false},
                        {"status", "not_requested"},
                        {"implementation", "none"},
                    }},
                    {"mp4", expected_artifacts.at("video")},
                    {"metadata_csv", expected_artifacts.at("metadata")},
                    {"keyframe_json", expected_artifacts.at("keyframes")},
                    {"perf_csv", expected_artifacts.at("perf")},
                    {"summary_json", expected_artifacts.at("summary")},
                    {"status_json", expected_artifacts.at("status")},
                    {"video_sanity_json", expected_artifacts.at("video_sanity")},
                    {"finalization_json", expected_artifacts.at("finalization")},
                    {"recorder_log", expected_artifacts.at("recorder_log")},
                    {"transport_sidecar", expected_artifacts.at("transport_sidecar")},
                    {"evidence_jsonl", expected_artifacts.at("evidence")},
                    {"evidence_manifest_json",
                     expected_artifacts.at("evidence_manifest")},
                    {"expected_artifacts", std::move(expected_artifacts)},
                };
                streams[roi.logical_stream_id] = stream;
                stream_order.push_back(roi.logical_stream_id);
                ++stream_count;
            }
        }

        if (stream_count != plan.admission_usage.roi_count ||
            streams.size() != plan.admission_usage.roi_count ||
            stream_order.size() != plan.admission_usage.roi_count) {
            return fail(error_out,
                        "spatial ROI recorder stream count does not match verified plan");
        }

        // The verified plan already accounts for one bounded queue per ROI.
        // Preserve that authenticated aggregate rather than allowing a
        // recorder supervisor to invent a larger outstanding table.
        const std::uint64_t expected_total_queue_frames =
            static_cast<std::uint64_t>(stream_count) * encode_queue_depth;
        if (expected_total_queue_frames != plan.admission_usage.queue_frames) {
            return fail(error_out,
                        "spatial ROI recorder IPC-v2 queue bound disagrees with the verified plan");
        }
        if (max_media_bytes_total != plan.admission_usage.media_bytes ||
            max_evidence_bytes_total != plan.admission_usage.evidence_bytes) {
            return fail(error_out,
                        "spatial ROI recorder long-run bounds disagree with the verified plan");
        }

        json analytics_gpu_json = json::object();
        for (const auto& [serial, gpu_id] :
             gpu_mapping.analytics_gpu_by_camera_serial) {
            analytics_gpu_json[serial] = gpu_id;
        }
        json recorder_gpu_json = json::object();
        for (const auto& [stream_id, gpu_id] :
             gpu_mapping.recorder_gpu_by_logical_stream_id) {
            recorder_gpu_json[stream_id] = gpu_id;
        }

        *contract_out = {
            {"schema_id", kSpatialRoiRecorderContractSchemaId},
            {"schema_version", contract_schema_version},
            {"contract_scope", contract_scope},
            {"strict", true},
            {"backend", contract_backend},
            {"mode", contract_mode},
            {"supervise_processes", true},
            {"require_summary", true},
            {"require_status", true},
            {"require_video_sanity", true},
            {"require_protocol_hello", true},
            {"require_frame_identity_proof", true},
            {"require_gop_routing", false},
            {"require_storage_preflight", true},
            {"storage_preflight_policy", {
                {"schema_id",
                 kSpatialRoiRecorderStoragePreflightPolicySchemaId},
                {"schema_version",
                 kSpatialRoiRecorderStoragePreflightPolicySchemaVersion},
                {"required", true},
                {"reserved_free_bytes", kSpatialRoiRecorderReservedFreeBytes},
            }},
            {"preserve_shard_mp4s", false},
            {"recording_id", plan.recording_id},
            {"session_id", plan.recording_id},
            {"recording_identity_token", plan.recording_identity_token},
            {"producer_generation", plan.producer_generation},
            {"spatial_roi_plan_sha256", plan.plan_sha256},
            {"recording_root", root.generic_string()},
            {"artifact_root", artifact_root.generic_string()},
            {"source_cadence", kSourceCadence},
            {"source_pixel_format", kSourcePixelFormat},
            {"stream_count", stream_count},
            {"stream_order", std::move(stream_order)},
            {"ipc_v2", {
                {"protocol", "orange.spatial_roi.external_recorder_ipc"},
                {"version", 2},
                {"features",
                 orange::spatial_roi::ipc::spatial_roi_ipc_required_features()},
                {"source_lifetime_mode", "deferred_release"},
                {"ack", {
                    {"message_kind", "ACK"},
                    {"accepted_true", {
                        {"means", "recorder_accepted_frame_and_retains_source_access"},
                        {"source_safe_after_ack", false},
                        {"release_required", true},
                    }},
                    {"accepted_false", {
                        {"means", "recorder_rejected_frame_but_source_access_is_not_yet_released"},
                        {"source_safe_after_ack", false},
                        {"release_required", true},
                    }},
                }},
                {"release", {
                    {"message_kind", "RELEASE"},
                    {"means", "recorder_finished_with_source_allocation"},
                    {"source_safe_after_release", true},
                    {"required_after_accepted_ack", true},
                    {"required_after_rejected_ack", true},
                    {"does_not_mean_encode_or_disk_complete", true},
                }},
                {"drain_finalize", {
                    {"status", "defined_not_negotiated"},
                    {"operational", false},
                    {"message_order", {"DRAIN_REQUEST", "DRAIN_STATUS",
                                        "FINALIZE_REQUEST", "FINALIZE_STATUS"}},
                    {"drain_request", {
                        {"message_kind", "DRAIN_REQUEST"},
                        {"sender", "producer"},
                        {"receiver", "recorder"},
                        {"correlation", "stream_identity_and_drain_sequence"},
                        {"reason_required", true},
                    }},
                    {"drain_status", {
                        {"message_kind", "DRAIN_STATUS"},
                        {"sender", "recorder"},
                        {"receiver", "producer"},
                        {"states", {"draining", "drained", "failed"}},
                        {"correlation", "stream_identity_and_drain_sequence"},
                        {"reason_required", true},
                        {"finalize_request_allowed_only_when", "state=drained"},
                    }},
                    {"finalize_request", {
                        {"message_kind", "FINALIZE_REQUEST"},
                        {"sender", "producer"},
                        {"receiver", "recorder"},
                        {"requires", "matching_drained_status"},
                        {"correlation", "stream_identity_and_drain_sequence"},
                        {"nonce", "fresh_16_byte_lower_hex"},
                        {"reason_required", true},
                    }},
                    {"finalize_status", {
                        {"message_kind", "FINALIZE_STATUS"},
                        {"sender", "recorder"},
                        {"receiver", "producer"},
                        {"states", {"finalized", "failed"}},
                        {"correlation", "stream_identity_drain_sequence_and_finalize_nonce"},
                        {"nonce", "must_equal_request"},
                        {"reason_required", true},
                        {"session_finalized_only_when", "state=finalized"},
                    }},
                }},
                {"bounds", {
                    {"queue_capacity_frames_per_stream", encode_queue_depth},
                    {"max_outstanding_frames_per_stream", encode_queue_depth},
                    {"max_queue_capacity_frames_per_stream",
                     kMaxSpatialRoiIpcQueueFrames},
                    {"queue_capacity_frames_total", expected_total_queue_frames},
                    {"max_outstanding_frames_total", expected_total_queue_frames},
                    {"overflow_action", "reject_frame_without_releasing_prior_frames"},
                    {"producer_backpressure", "nonblocking_fail_closed"},
                }},
            }},
            {"aggregate_bounds", {
                {"max_detach_pool_bytes_total", max_detach_pool_bytes_total},
                {"max_queue_bytes_total", max_queue_bytes_total},
                {"writer_queue_max_packets_total",
                 writer_queue_max_packets_total},
                {"writer_queue_max_bytes_total", writer_queue_max_bytes_total},
                {"operation_timeout_ms_per_stream",
                 kSpatialRoiRecorderOperationTimeoutMs},
                {"max_media_bytes_total", max_media_bytes_total},
                {"max_evidence_bytes_total", max_evidence_bytes_total},
            }},
            {"recording_control", {
                {"record_for_seconds", 0},
                {"clip_seconds", 0},
            }},
            {"rollover", {
                {"requested", false},
                {"status", "not_requested"},
                {"implementation", "none"},
            }},
            {"gpu_mapping", {
                {"analytics_gpu_by_camera_serial", std::move(analytics_gpu_json)},
                {"recorder_gpu_by_logical_stream_id", std::move(recorder_gpu_json)},
            }},
            {"streams", std::move(streams)},
        };
        return true;
    } catch (const std::exception& ex) {
        return fail(error_out,
                    std::string("spatial ROI recorder contract build failed: ") +
                        ex.what());
    } catch (...) {
        return fail(error_out,
                    "spatial ROI recorder contract build failed: unknown exception");
    }
}

}  // namespace orange::session::spatial_roi
