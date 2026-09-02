#include "spatial_roi_recorder_evidence.h"

#include "gui/spatial_layout/sha256.h"
#include "shaman_v2_recording_identity.h"
#include "session/spatial_roi_recording_config.h"
#include "session/spatial_roi_recorder_contract.h"
#include "session/spatial_roi_recorder_contract_parser.h"
#include "spatial_roi_recorder_terminal_sidecars.h"
#include "spatial_roi_recorder_video_sanity.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <cctype>
#include <cmath>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <numeric>
#include <set>
#include <sstream>
#include <string_view>
#include <system_error>
#include <unordered_set>
#include <utility>

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#if defined(__linux__)
#include <linux/fs.h>
#include <sys/syscall.h>
#endif

namespace orange::spatial_roi::recording {

struct SpatialRoiRecorderEvidenceMetadataDigest {
    orange::gui::spatial_layout::checksum::StreamingSha256 hasher;
};

namespace {

using json = nlohmann::json;
namespace checksum = orange::gui::spatial_layout::checksum;

constexpr std::size_t kMaxIdentifierBytes = 512;
constexpr std::size_t kMaxReasonBytes = 1024;
constexpr std::size_t kMaxArtifactKinds = 64;
constexpr std::size_t kMaxPathComponents = 64;
constexpr std::size_t kMaxPathComponentBytes = 255;
constexpr std::uint64_t kKeyframeSummaryMaxBytes = 16ULL * 1024ULL;
constexpr std::string_view kMetadataCsvHeader =
    "recording_frame_id,roi_stream_frame_index,output_frame_index,"
    "camera_timestamp_ns,timestamp_sys_ns,source_gpu_id,assigned_gpu_id,"
    "assigned_shard_id\n";

constexpr char kHeaderRecordType[] = "stream_header";
constexpr char kFrameRecordType[] = "frame";
constexpr char kTerminalRecordType[] = "stream_terminal";

constexpr char kTerminalComplete[] = "complete";
constexpr char kTerminalFailed[] = "failed";

constexpr char kEncodeEncoded[] = "encoded";
constexpr char kEncodeFailed[] = "failed";
constexpr char kEncodeNotAttempted[] = "not_attempted";

constexpr std::array<std::string_view, 8> kDetachStatuses = {
    "detached", "invalid_argument", "wrong_device", "busy",
    "pool_exhausted", "cuda_error", "source_quarantined", "stopped"};

// These names mirror the strict spatial-ROI contract's expected_artifacts
// object.  The evidence manifest is not a generic caller-defined checksum
// bag: admitting an unknown kind would make completion semantics ambiguous.
constexpr std::array<std::string_view, 12> kArtifactKinds = {
    "video", "metadata", "keyframes", "perf", "summary", "status",
    "video_sanity", "finalization", "recorder_log", "transport_sidecar",
    "evidence", "evidence_manifest"};

constexpr std::array<std::string_view, 10> kFinalizeArtifactKinds = {
    "video", "metadata", "keyframes", "perf", "summary", "status",
    "video_sanity", "finalization", "recorder_log", "transport_sidecar"};

constexpr std::array<std::string_view, 31> kContractKeys = {
    "schema_id", "schema_version", "contract_scope", "strict", "backend",
    "mode", "supervise_processes", "require_summary", "require_status",
    "require_video_sanity", "require_protocol_hello",
    "require_frame_identity_proof", "require_gop_routing",
    "require_storage_preflight", "preserve_shard_mp4s", "recording_id",
    "session_id", "recording_identity_token", "producer_generation",
    "spatial_roi_plan_sha256", "recording_root", "artifact_root",
    "source_cadence", "source_pixel_format", "stream_count", "stream_order",
    "ipc_v2", "recording_control", "rollover", "gpu_mapping", "streams"};

constexpr std::array<std::string_view, 2> kPlanKeys = {
    "schema_id", "schema_version"};

constexpr std::array<std::string_view, 49> kStreamKeys = {
    "stream_id", "logical_stream_id", "stream_kind", "output_kind",
    "camera_id", "camera_serial", "env_key", "socket_path",
    "analytics_gpu_id", "recorder_gpu_id", "source_gpu_id", "assigned_gpu_id",
    "roi_id", "region_id", "arena_group_id", "arena_id", "recording_id",
    "session_id", "recording_identity_token", "producer_generation",
    "spatial_roi_plan_sha256", "frame_identity", "identity", "geometry_identity",
    "encode_profile", "encode_fps", "codec", "tuning", "rate_control_mode",
    "quality_value", "gop", "encode_queue_depth", "routing_policy",
    "detach_pool_frames", "max_detach_pool_bytes",
    "expected_shard_gpu_ids", "recording_control", "rollover", "mp4",
    "metadata_csv", "keyframe_json", "perf_csv", "summary_json", "status_json",
    "video_sanity_json", "finalization_json", "recorder_log",
    "transport_sidecar", "expected_artifacts"};

constexpr std::array<std::string_view, 11> kGeometryKeys = {
    "layout", "materialization", "registration", "native_raster", "content_rect",
    "encoded_raster", "encoded_content_rect", "content_offset", "padding",
    "source_coordinate_space", "video_coordinate_space"};

constexpr std::array<std::string_view, 2> kAuthorityKeys = {"id", "sha256"};

constexpr std::array<std::string_view, 2> kRasterKeys = {"width", "height"};

constexpr std::array<std::string_view, 4> kRectKeys = {
    "x", "y", "width", "height"};

constexpr std::array<std::string_view, 5> kPaddingKeys = {
    "left", "top", "right", "bottom", "value_mono8"};

constexpr std::array<std::string_view, 11> kBindingTopKeys = {
    "contract", "plan", "recording", "camera", "stream", "geometry", "gpu",
    "encode_profile", "roots", "expected_artifacts", "limits"};

constexpr std::array<std::string_view, 2> kBindingRootKeys = {
    "recording_root", "artifact_root"};

constexpr std::array<std::string_view, 3> kBindingLimitKeys = {
    "max_frames_per_stream", "max_media_bytes_per_stream",
    "max_evidence_bytes_per_stream"};

constexpr std::array<std::string_view, 4> kBindingContractKeys = {
    "schema_id", "schema_version", "sha256", "mode"};

constexpr std::array<std::string_view, 3> kBindingPlanKeys = {
    "schema_id", "schema_version", "sha256"};

constexpr std::array<std::string_view, 4> kBindingRecordingKeys = {
    "recording_id", "session_id", "recording_identity_token",
    "producer_generation"};

constexpr std::array<std::string_view, 4> kBindingCameraKeys = {
    "camera_id", "camera_serial", "analytics_gpu_id", "source_gpu_id"};

constexpr std::array<std::string_view, 7> kBindingStreamKeys = {
    "roi_id", "region_id", "arena_group_id", "arena_id", "has_arena_id",
    "logical_stream_id", "routing_policy"};

constexpr std::array<std::string_view, 4> kBindingGpuKeys = {
    "recorder_gpu_id", "assigned_gpu_id", "assigned_shard_id", "routing_policy"};

constexpr std::array<std::string_view, 5> kEvidenceHeaderKeys = {
    "record_type", "schema_id", "schema_version", "canonicalization", "binding"};

constexpr std::array<std::string_view, 12> kEvidenceFrameKeysWithRest = {
    "record_type", "schema_id", "schema_version", "canonicalization",
    "frame_record_index", "correlation", "frame", "detach", "dispatch",
    "ack", "release", "encode"};

constexpr std::array<std::string_view, 14> kEvidenceTerminalKeys = {
    "record_type", "schema_id", "schema_version", "canonicalization",
    "terminal_state", "reason", "frame_count", "ranges", "counts",
    "binding_sha256", "receipt_scope", "finalize_request_sha256", "artifacts",
    "encoder_terminal"};

constexpr std::array<std::string_view, 4> kCorrelationKeys = {
    "recording_identity_token", "producer_generation", "logical_stream_id",
    "recording_frame_id"};

constexpr std::array<std::string_view, 7> kCorrelationKeysWithIds = {
    "recording_identity_token", "producer_generation", "logical_stream_id",
    "local_frame_id", "camera_frame_id", "recording_frame_id",
    "roi_stream_frame_index"};

constexpr std::array<std::string_view, 2> kDetachKeys = {
    "status", "source_release_safe"};

constexpr std::array<std::string_view, 2> kDispatchKeys = {
    "admitted", "reason"};

constexpr std::array<std::string_view, 5> kAckKeys = {
    "attempted", "sent", "accepted", "reason", "error"};

constexpr std::array<std::string_view, 4> kReleaseKeys = {
    "attempted", "sent", "reason", "error"};

constexpr std::array<std::string_view, 5> kEncodeKeys = {
    "status", "output_frame_index", "packet_count", "encoded_bytes", "keyframe"};

constexpr std::array<std::string_view, 4> kRangeKeys = {
    "recording_frame_id", "roi_stream_frame_index", "has_frames", "frame_count"};

constexpr std::array<std::string_view, 16> kCountKeys = {
    "detach_successes", "dispatch_admitted", "dispatch_rejected",
    "ack_attempted", "ack_sent", "ack_accepted", "release_attempted",
    "release_sent", "encoded_frames", "failed_frames",
    "packet_count", "encoded_bytes", "keyframes", "ack_write_failures",
    "release_write_failures", "lifecycle_failures"};

constexpr std::array<std::string_view, 5> kManifestKeys = {
    "schema_id", "schema_version", "canonicalization", "stream_kind",
    "binding"};

constexpr std::array<std::string_view, 7> kManifestKeysRest = {
    "schema_id", "schema_version", "canonicalization", "stream_kind", "binding",
    "evidence", "artifacts"};

constexpr std::array<std::string_view, 4> kManifestKeysWithCounts = {
    "schema_id", "schema_version", "canonicalization", "stream_kind"};

bool fail(std::string* error_out, std::string message)
{
    if (error_out) {
        *error_out = std::move(message);
    }
    return false;
}

template <std::size_t N>
bool exact_keys(const json& value,
                const std::array<std::string_view, N>& expected,
                const std::string& path,
                std::string* error_out)
{
    if (!value.is_object()) {
        return fail(error_out, path + " must be an object");
    }
    for (const std::string_view key : expected) {
        if (!value.contains(std::string(key))) {
            return fail(error_out, path + "." + std::string(key) + " is required");
        }
    }
    if (value.size() != expected.size()) {
        for (auto it = value.begin(); it != value.end(); ++it) {
            bool allowed = false;
            for (const std::string_view key : expected) {
                if (it.key() == key) {
                    allowed = true;
                    break;
                }
            }
            if (!allowed) {
                return fail(error_out,
                            path + "." + it.key() + " is not allowed by the " +
                                std::string(kSpatialRoiRecorderEvidenceSchemaId) +
                                " schema");
            }
        }
        return fail(error_out, path + " has an unexpected key count");
    }
    return true;
}

bool is_sha256(const std::string& value)
{
    if (value.size() != 71 || value.compare(0, 7, "sha256:") != 0) {
        return false;
    }
    return std::all_of(value.begin() + 7,
                       value.end(),
                       [](const unsigned char ch) {
                           return (ch >= '0' && ch <= '9') ||
                                  (ch >= 'a' && ch <= 'f');
                       });
}

bool safe_text(const std::string& value, const std::size_t max_bytes)
{
    if (value.empty() || value.size() > max_bytes) {
        return false;
    }
    return std::none_of(value.begin(), value.end(), [](const unsigned char ch) {
        return ch < 0x20 || ch == 0x7f;
    });
}

bool safe_recording_id(const std::string& value)
{
    return safe_text(value, kMaxIdentifierBytes) &&
        !std::isspace(static_cast<unsigned char>(value.front())) &&
        !std::isspace(static_cast<unsigned char>(value.back()));
}

bool safe_identifier(const std::string& value,
                     const std::size_t max_bytes = kMaxIdentifierBytes)
{
    if (!safe_text(value, max_bytes)) {
        return false;
    }
    if (!std::isalnum(static_cast<unsigned char>(value.front()))) {
        return false;
    }
    return std::all_of(value.begin() + 1,
                       value.end(),
                       [](const unsigned char ch) {
                           return std::isalnum(ch) || ch == '_' || ch == '-' ||
                                  ch == '.';
                       });
}

bool safe_relative_path(const std::string& value)
{
    if (value.empty() || value.size() > kSpatialRoiRecorderEvidenceMaxPathBytes ||
        value.front() == '/' || value.find('\\') != std::string::npos ||
        value.find('\0') != std::string::npos ||
        std::any_of(value.begin(), value.end(), [](const unsigned char ch) {
            return ch < 0x20 || ch == 0x7f;
        })) {
        return false;
    }
    std::size_t component_count = 0;
    std::size_t begin = 0;
    while (begin < value.size()) {
        const std::size_t end = value.find('/', begin);
        const std::size_t length =
            end == std::string::npos ? value.size() - begin : end - begin;
        if (length == 0 || length > kMaxPathComponentBytes) {
            return false;
        }
        const std::string_view component(value.data() + begin, length);
        if (component == "." || component == "..") {
            return false;
        }
        ++component_count;
        if (component_count > kMaxPathComponents) {
            return false;
        }
        if (end == std::string::npos) {
            break;
        }
        begin = end + 1;
    }
    return component_count != 0 && value.back() != '/';
}

std::vector<std::string> path_components(const std::string& value)
{
    std::vector<std::string> components;
    std::size_t begin = 0;
    while (begin < value.size()) {
        const std::size_t end = value.find('/', begin);
        components.emplace_back(value.substr(
            begin, end == std::string::npos ? value.size() - begin : end - begin));
        if (end == std::string::npos) {
            break;
        }
        begin = end + 1;
    }
    return components;
}

bool read_string(const json& object,
                 const char* key,
                 std::string* out,
                 const std::string& path,
                 std::string* error_out,
                 const std::size_t max_bytes = kMaxIdentifierBytes)
{
    if (!object.is_object() || !object.contains(key) ||
        !object.at(key).is_string()) {
        return fail(error_out, path + "." + key + " must be a string");
    }
    const std::string value = object.at(key).get<std::string>();
    if (!safe_text(value, max_bytes)) {
        return fail(error_out, path + "." + key + " contains unsafe text");
    }
    if (out) {
        *out = value;
    }
    return true;
}

bool read_optional_text(const json& object,
                        const char* key,
                        std::string* out,
                        const std::string& path,
                        std::string* error_out,
                        const std::size_t max_bytes)
{
    if (!object.is_object() || !object.contains(key) ||
        !object.at(key).is_string()) {
        return fail(error_out, path + "." + key + " must be a string");
    }
    const std::string value = object.at(key).get<std::string>();
    if (value.size() > max_bytes ||
        std::any_of(value.begin(), value.end(), [](const unsigned char ch) {
            return ch < 0x20 || ch == 0x7f;
        })) {
        return fail(error_out, path + "." + key + " contains unsafe text");
    }
    if (out) {
        *out = value;
    }
    return true;
}

bool read_bool(const json& object,
               const char* key,
               bool* out,
               const std::string& path,
               std::string* error_out)
{
    if (!object.is_object() || !object.contains(key) ||
        !object.at(key).is_boolean()) {
        return fail(error_out, path + "." + key + " must be boolean");
    }
    if (out) {
        *out = object.at(key).get<bool>();
    }
    return true;
}

bool read_int(const json& object,
              const char* key,
              int* out,
              const std::string& path,
              std::string* error_out,
              const bool nonnegative = false)
{
    if (!object.is_object() || !object.contains(key) ||
        object.at(key).is_boolean() || !object.at(key).is_number_integer()) {
        return fail(error_out, path + "." + key + " must be an integer");
    }
    const auto value = object.at(key).get<std::int64_t>();
    if (value < std::numeric_limits<int>::min() ||
        value > std::numeric_limits<int>::max() || (nonnegative && value < 0)) {
        return fail(error_out, path + "." + key + " is outside the integer range");
    }
    if (out) {
        *out = static_cast<int>(value);
    }
    return true;
}

bool read_u64(const json& object,
              const char* key,
              std::uint64_t* out,
              const std::string& path,
              std::string* error_out)
{
    if (!object.is_object() || !object.contains(key) ||
        object.at(key).is_boolean() ||
        (!object.at(key).is_number_unsigned() &&
         !object.at(key).is_number_integer())) {
        return fail(error_out, path + "." + key + " must be an unsigned integer");
    }
    if (object.at(key).is_number_unsigned()) {
        if (out) {
            *out = object.at(key).get<std::uint64_t>();
        }
        return true;
    }
    const auto value = object.at(key).get<std::int64_t>();
    if (value < 0) {
        return fail(error_out, path + "." + key + " must not be negative");
    }
    if (out) {
        *out = static_cast<std::uint64_t>(value);
    }
    return true;
}

std::string canonical_json_sha256(const json& value)
{
    const std::string bytes = value.dump(
        -1, ' ', false, json::error_handler_t::strict);
    return "sha256:" + checksum::sha256_hex(bytes);
}

bool read_const_string(const json& object,
                       const char* key,
                       const char* expected,
                       const std::string& path,
                       std::string* error_out)
{
    std::string value;
    return read_string(object, key, &value, path, error_out) &&
        (value == expected ||
         fail(error_out, path + "." + key + " must be " + expected));
}

bool read_const_int(const json& object,
                    const char* key,
                    const int expected,
                    const std::string& path,
                    std::string* error_out)
{
    int value = 0;
    return read_int(object, key, &value, path, error_out) &&
        (value == expected ||
         fail(error_out,
              path + "." + key + " must be " + std::to_string(expected)));
}

bool read_const_bool(const json& object,
                     const char* key,
                     const bool expected,
                     const std::string& path,
                     std::string* error_out)
{
    bool value = false;
    return read_bool(object, key, &value, path, error_out) &&
        (value == expected ||
         fail(error_out,
              path + "." + key + " must be " + (expected ? "true" : "false")));
}

bool validate_sha_field(const json& object,
                        const char* key,
                        const std::string& path,
                        std::string* error_out)
{
    std::string value;
    return read_string(object, key, &value, path, error_out, 71) &&
        (is_sha256(value) ||
         fail(error_out, path + "." + key + " must be sha256:<64 lowercase hex>"));
}

bool validate_authority(const json& value,
                        const std::string& path,
                        std::string* error_out)
{
    if (!value.is_object()) {
        return fail(error_out, path + " must be an object");
    }
    // Authority references in the recorder contract are generated from the
    // plan and intentionally have exactly id/sha256.  The wider array above
    // is not used here because accepting source/scope/version would make the
    // evidence projection ambiguous.
    static constexpr std::array<std::string_view, 2> keys = {"id", "sha256"};
    if (!exact_keys(value, keys, path, error_out)) {
        return false;
    }
    std::string id;
    if (!read_string(value, "id", &id, path, error_out, 128) ||
        !safe_identifier(id, 128)) {
        return fail(error_out, path + ".id is not a safe identifier");
    }
    return validate_sha_field(value, "sha256", path, error_out);
}

bool validate_geometry(const json& geometry, std::string* error_out)
{
    if (!exact_keys(geometry, kGeometryKeys, "binding.geometry", error_out)) {
        return false;
    }
    if (!validate_authority(geometry.at("layout"),
                            "binding.geometry.layout",
                            error_out) ||
        !validate_authority(geometry.at("materialization"),
                            "binding.geometry.materialization",
                            error_out) ||
        !validate_authority(geometry.at("registration"),
                            "binding.geometry.registration",
                            error_out)) {
        return false;
    }
    const auto validate_raster = [&](const char* key) {
        const json& raster = geometry.at(key);
        static constexpr std::array<std::string_view, 2> keys = {"width", "height"};
        if (!exact_keys(raster, keys, std::string("binding.geometry.") + key,
                        error_out)) {
            return false;
        }
        std::uint64_t width = 0;
        std::uint64_t height = 0;
        return read_u64(raster, "width", &width,
                        std::string("binding.geometry.") + key, error_out) &&
            read_u64(raster, "height", &height,
                     std::string("binding.geometry.") + key, error_out) &&
            width > 0 && height > 0 && width <= std::numeric_limits<std::uint32_t>::max() &&
            height <= std::numeric_limits<std::uint32_t>::max();
    };
    const auto validate_rect = [&](const char* key) {
        const json& rect = geometry.at(key);
        if (!exact_keys(rect, kRectKeys, std::string("binding.geometry.") + key,
                        error_out)) {
            return false;
        }
        std::uint64_t x = 0;
        std::uint64_t y = 0;
        std::uint64_t width = 0;
        std::uint64_t height = 0;
        const std::string path = std::string("binding.geometry.") + key;
        return read_u64(rect, "x", &x, path, error_out) &&
            read_u64(rect, "y", &y, path, error_out) &&
            read_u64(rect, "width", &width, path, error_out) &&
            read_u64(rect, "height", &height, path, error_out) && width > 0 &&
            height > 0 && x <= std::numeric_limits<std::uint32_t>::max() &&
            y <= std::numeric_limits<std::uint32_t>::max() &&
            width <= std::numeric_limits<std::uint32_t>::max() &&
            height <= std::numeric_limits<std::uint32_t>::max();
    };
    if (!validate_raster("native_raster") || !validate_raster("encoded_raster") ||
        !validate_rect("content_rect") || !validate_rect("encoded_content_rect")) {
        return fail(error_out, "binding geometry contains an invalid raster/rectangle");
    }
    if (!exact_keys(geometry.at("content_offset"),
                    std::array<std::string_view, 2>{"x", "y"},
                    "binding.geometry.content_offset",
                    error_out)) {
        return false;
    }
    for (const char* key : {"x", "y"}) {
        std::uint64_t value = 0;
        if (!read_u64(geometry.at("content_offset"), key, &value,
                      "binding.geometry.content_offset", error_out) ||
            value != 0) {
            return fail(error_out,
                        std::string("binding.geometry.content_offset.") + key +
                            " must be zero");
        }
    }
    if (!exact_keys(geometry.at("padding"), kPaddingKeys,
                    "binding.geometry.padding", error_out)) {
        return false;
    }
    std::uint64_t padding_value = 0;
    if (!read_u64(geometry.at("padding"), "value_mono8", &padding_value,
                  "binding.geometry.padding", error_out) ||
        padding_value != 0) {
        return fail(error_out, "binding.geometry.padding.value_mono8 must be zero");
    }
    for (const char* key : {"left", "top", "right", "bottom"}) {
        std::uint64_t value = 0;
        if (!read_u64(geometry.at("padding"), key, &value,
                      "binding.geometry.padding", error_out) ||
            value > std::numeric_limits<std::uint32_t>::max()) {
            return false;
        }
    }
    std::string source_space;
    std::string video_space;
    return read_string(geometry, "source_coordinate_space", &source_space,
                       "binding.geometry", error_out, 128) &&
        read_string(geometry, "video_coordinate_space", &video_space,
                    "binding.geometry", error_out, 128);
}

bool validate_profile(const json& profile, std::string* error_out)
{
    // Evidence v2 binds the complete immutable encoder profile projected by
    // recorder-contract v4. Keep this closed shape in lockstep with the
    // authoritative contract parser rather than silently discarding the
    // preset/rate-control fields that distinguish the two supported profiles.
    static constexpr std::array<std::string_view, 18> keys = {
        "profile_id", "codec", "preset", "tuning", "lossless",
        "rate_control_mode", "quality_value", "gop_length", "frame_rate",
        "input_format", "encoded_format", "no_resize", "luma_preserved_exactly",
        "neutral_chroma_value", "aq", "temporal_aq", "lookahead",
        "lookahead_depth"};
    if (!exact_keys(profile, keys, "binding.encode_profile", error_out)) {
        return false;
    }
    std::string profile_id;
    std::string codec;
    std::string preset;
    std::string tuning;
    std::string rate_control_mode;
    std::string input_format;
    std::string encoded_format;
    bool lossless = false;
    bool no_resize = false;
    bool luma_preserved_exactly = false;
    bool aq = false;
    bool temporal_aq = false;
    bool lookahead = false;
    std::uint64_t quality_value = 0;
    std::uint64_t gop = 0;
    std::uint64_t fps = 0;
    std::uint64_t chroma = 0;
    std::uint64_t lookahead_depth = 0;
    if (!read_string(profile, "profile_id", &profile_id,
                     "binding.encode_profile", error_out) ||
        !read_string(profile, "codec", &codec,
                     "binding.encode_profile", error_out) ||
        !read_string(profile, "preset", &preset,
                     "binding.encode_profile", error_out) ||
        !read_string(profile, "tuning", &tuning,
                     "binding.encode_profile", error_out) ||
        !read_bool(profile, "lossless", &lossless,
                   "binding.encode_profile", error_out) ||
        !read_string(profile, "rate_control_mode", &rate_control_mode,
                     "binding.encode_profile", error_out) ||
        !read_u64(profile, "quality_value", &quality_value,
                  "binding.encode_profile", error_out) ||
        !read_u64(profile, "gop_length", &gop,
                  "binding.encode_profile", error_out) ||
        !read_u64(profile, "frame_rate", &fps,
                  "binding.encode_profile", error_out) ||
        !read_string(profile, "input_format", &input_format,
                     "binding.encode_profile", error_out) ||
        !read_string(profile, "encoded_format", &encoded_format,
                     "binding.encode_profile", error_out) ||
        !read_bool(profile, "no_resize", &no_resize,
                   "binding.encode_profile", error_out) ||
        !read_bool(profile, "luma_preserved_exactly", &luma_preserved_exactly,
                   "binding.encode_profile", error_out) ||
        !read_bool(profile, "aq", &aq,
                   "binding.encode_profile", error_out) ||
        !read_bool(profile, "temporal_aq", &temporal_aq,
                   "binding.encode_profile", error_out) ||
        !read_bool(profile, "lookahead", &lookahead,
                   "binding.encode_profile", error_out) ||
        !read_u64(profile, "neutral_chroma_value", &chroma,
                  "binding.encode_profile", error_out) ||
        !read_u64(profile, "lookahead_depth", &lookahead_depth,
                  "binding.encode_profile", error_out)) {
        return false;
    }

    const auto matches = [&](const orange::session::spatial_roi::EncodeProfile& expected) {
        return profile_id == expected.name && codec == expected.codec &&
            preset == expected.preset && tuning == expected.tuning &&
            lossless == expected.lossless &&
            rate_control_mode == expected.rate_control_mode &&
            quality_value == expected.quality_value &&
            gop == expected.gop_length && aq == expected.aq &&
            temporal_aq == expected.temporal_aq &&
            lookahead == expected.lookahead &&
            lookahead_depth == expected.lookahead_depth;
    };
    const bool supported =
        matches(orange::session::spatial_roi::legacy_lossless_encode_profile()) ||
        matches(orange::session::spatial_roi::
                    legacy_low_latency_vbr_gop1_encode_profile()) ||
        matches(orange::session::spatial_roi::low_latency_vbr_encode_profile());
    if (!supported || fps == 0 || input_format != "mono8" ||
        encoded_format != "nv12" || !no_resize ||
        luma_preserved_exactly != lossless || chroma != 128) {
        return fail(error_out,
                    "binding.encode_profile is not an allowed immutable HEVC profile");
    }
    return true;
}

std::uint64_t encode_profile_gop_length(const json& profile)
{
    if (!profile.is_object() || !profile.contains("gop_length")) {
        return 0;
    }
    std::uint64_t gop_length = 0;
    return read_u64(profile, "gop_length", &gop_length,
                    "binding.encode_profile", nullptr) && gop_length != 0
        ? gop_length
        : 0;
}

std::uint64_t expected_keyframe_count(const std::uint64_t frame_count,
                                      const std::uint64_t gop_length)
{
    return frame_count == 0 || gop_length == 0
        ? 0
        : 1U + ((frame_count - 1U) / gop_length);
}

bool expected_keyframe_for_output(const std::uint64_t one_based_output_index,
                                  const std::uint64_t gop_length)
{
    return one_based_output_index != 0 && gop_length != 0 &&
        ((one_based_output_index - 1U) % gop_length) == 0;
}

std::string keyframe_policy_name(const std::uint64_t gop_length)
{
    return gop_length == 1
        ? "all_frames_idr"
        : "fixed_gop_" + std::to_string(gop_length) + "_idr";
}

bool validate_binding_json(const json& value, std::string* error_out)
{
    if (!exact_keys(value, kBindingTopKeys, "binding", error_out)) {
        return false;
    }
    const json& contract = value.at("contract");
    const json& plan = value.at("plan");
    const json& recording = value.at("recording");
    const json& camera = value.at("camera");
    const json& stream = value.at("stream");
    const json& gpu = value.at("gpu");
    const json& roots = value.at("roots");
    const json& limits = value.at("limits");
    if (!exact_keys(contract, kBindingContractKeys, "binding.contract", error_out) ||
        !exact_keys(plan, kBindingPlanKeys, "binding.plan", error_out) ||
        !exact_keys(recording, kBindingRecordingKeys, "binding.recording", error_out) ||
        !exact_keys(camera, kBindingCameraKeys, "binding.camera", error_out) ||
        !exact_keys(stream, kBindingStreamKeys, "binding.stream", error_out) ||
        !exact_keys(gpu, kBindingGpuKeys, "binding.gpu", error_out) ||
        !exact_keys(roots, kBindingRootKeys, "binding.roots", error_out) ||
        !exact_keys(limits, kBindingLimitKeys, "binding.limits", error_out)) {
        return false;
    }
    if (!read_const_string(contract, "schema_id",
                           orange::session::spatial_roi::kSpatialRoiRecorderContractSchemaId,
                           "binding.contract", error_out) ||
        !read_const_int(
            contract, "schema_version",
            orange::session::spatial_roi::kSpatialRoiRecorderContractSchemaVersion,
            "binding.contract", error_out) ||
        !validate_sha_field(contract, "sha256", "binding.contract", error_out) ||
        !read_const_string(
            contract, "mode",
            orange::session::spatial_roi::kSpatialRoiRecorderContractMode,
                           "binding.contract", error_out) ||
        !read_const_string(plan, "schema_id",
                           orange::session::spatial_roi::kPlanSchemaId,
                           "binding.plan", error_out) ||
        !read_const_int(plan, "schema_version",
                        orange::session::spatial_roi::kPlanSchemaVersion,
                        "binding.plan", error_out) ||
        !validate_sha_field(plan, "sha256", "binding.plan", error_out)) {
        return false;
    }
    std::string recording_id;
    std::string session_id;
    std::string producer_generation;
    for (const auto& item : {std::pair<const char*, std::string*>{"recording_id", &recording_id},
                             {"session_id", &session_id},
                             {"producer_generation", &producer_generation}}) {
        if (!read_string(recording, item.first, item.second, "binding.recording",
                         error_out)) {
            return false;
        }
    }
    if (!safe_recording_id(recording_id) || !safe_recording_id(session_id) ||
        session_id != recording_id || !safe_identifier(producer_generation)) {
        return fail(error_out, "binding recording/session identity is inconsistent");
    }
    std::string token;
    if (!read_string(recording, "recording_identity_token", &token,
                     "binding.recording", error_out, 71) || !is_sha256(token) ||
        token != orange::shaman_v2_recording_identity::token_for_recording_id(
                     recording_id)) {
        return fail(error_out, "binding.recording.recording_identity_token is invalid");
    }
    int camera_id = -1;
    int analytics_gpu = -1;
    int source_gpu = -1;
    if (!read_int(camera, "camera_id", &camera_id, "binding.camera", error_out, true) ||
        !read_string(camera, "camera_serial", nullptr, "binding.camera", error_out, 128) ||
        !read_int(camera, "analytics_gpu_id", &analytics_gpu, "binding.camera", error_out,
                  true) ||
        !read_int(camera, "source_gpu_id", &source_gpu, "binding.camera", error_out,
                  true)) {
        return false;
    }
    for (const char* key : {"roi_id", "region_id", "arena_group_id",
                            "logical_stream_id", "routing_policy"}) {
        std::string value_string;
        if (!read_string(stream, key, &value_string, "binding.stream", error_out) ||
            !safe_identifier(value_string)) {
            return fail(error_out, std::string("binding.stream.") + key +
                                   " is not a safe identifier");
        }
    }
    bool has_arena = false;
    if (!read_bool(stream, "has_arena_id", &has_arena, "binding.stream", error_out)) {
        return false;
    }
    if (has_arena) {
        std::string arena;
        if (!read_string(stream, "arena_id", &arena, "binding.stream", error_out) ||
            !safe_identifier(arena)) {
            return fail(error_out, "binding.stream.arena_id is invalid");
        }
    } else if (!stream.at("arena_id").is_null()) {
        return fail(error_out, "binding.stream.arena_id must be null when absent");
    }
    for (const char* key : {"recorder_gpu_id", "assigned_gpu_id",
                            "assigned_shard_id"}) {
        int ignored = -1;
        if (!read_int(gpu, key, &ignored, "binding.gpu", error_out, true)) {
            return false;
        }
    }
    std::string route;
    if (!read_string(gpu, "routing_policy", &route, "binding.gpu", error_out) ||
        route != stream.at("routing_policy").get<std::string>()) {
        return fail(error_out, "binding GPU routing policy disagrees with stream");
    }
    std::string recording_root;
    std::string artifact_root;
    if (!read_string(roots, "recording_root", &recording_root, "binding.roots",
                     error_out, kSpatialRoiRecorderEvidenceMaxPathBytes) ||
        !read_string(roots, "artifact_root", &artifact_root, "binding.roots",
                     error_out, kSpatialRoiRecorderEvidenceMaxPathBytes)) {
        return false;
    }
    const std::filesystem::path recording_path(recording_root);
    const std::filesystem::path artifact_path(artifact_root);
    if (!recording_path.is_absolute() || recording_path == recording_path.root_path() ||
        recording_path.lexically_normal() != recording_path ||
        !artifact_path.is_absolute() || artifact_path == artifact_path.root_path() ||
        artifact_path.lexically_normal() != artifact_path ||
        artifact_path != recording_path / "external_spatial_roi_recorder") {
        return fail(error_out, "binding roots are not the authoritative artifact layout");
    }
    std::uint64_t max_frames = 0;
    std::uint64_t max_media_bytes = 0;
    std::uint64_t max_evidence_bytes = 0;
    if (!read_u64(limits, "max_frames_per_stream", &max_frames,
                  "binding.limits", error_out) ||
        !read_u64(limits, "max_media_bytes_per_stream", &max_media_bytes,
                  "binding.limits", error_out) ||
        !read_u64(limits, "max_evidence_bytes_per_stream", &max_evidence_bytes,
                  "binding.limits", error_out) ||
        max_frames == 0 ||
        max_frames > kSpatialRoiRecorderEvidenceMaxFrames ||
        max_media_bytes == 0 || max_evidence_bytes == 0 ||
        max_evidence_bytes > kSpatialRoiRecorderEvidenceMaxFileBytes ||
        max_frames > std::numeric_limits<std::size_t>::max()) {
        return fail(error_out,
                    "binding limits exceed the evidence implementation admission");
    }
    const json& expected_artifacts = value.at("expected_artifacts");
    if (!expected_artifacts.is_object() ||
        expected_artifacts.size() != kArtifactKinds.size()) {
        return fail(error_out, "binding.expected_artifacts must be the exact closed set");
    }
    std::set<std::string> artifact_paths;
    for (const std::string_view kind_view : kArtifactKinds) {
        const std::string kind(kind_view);
        if (!expected_artifacts.contains(kind) ||
            !expected_artifacts.at(kind).is_string()) {
            return fail(error_out,
                        "binding.expected_artifacts." + kind + " is required");
        }
        const std::string path = expected_artifacts.at(kind).get<std::string>();
        if (!safe_relative_path(path) || !artifact_paths.insert(path).second) {
            return fail(error_out,
                        "binding.expected_artifacts contains an unsafe or duplicate path");
        }
    }
    return validate_geometry(value.at("geometry"), error_out) &&
        validate_profile(value.at("encode_profile"), error_out);
}

bool binding_json_equal(const SpatialRoiRecorderEvidenceBinding& lhs,
                        const SpatialRoiRecorderEvidenceBinding& rhs)
{
    return spatial_roi_recorder_evidence_binding_to_json(lhs) ==
        spatial_roi_recorder_evidence_binding_to_json(rhs);
}

bool binding_matches_descriptor(const SpatialRoiRecorderEvidenceBinding& binding,
                                const SpatialRoiFrameDescriptor& descriptor,
                                std::string* error_out)
{
    if (descriptor.recording_id != binding.recording_id ||
        descriptor.recording_identity_token != binding.recording_identity_token ||
        descriptor.producer_generation != binding.producer_generation ||
        descriptor.camera_id != binding.camera_id ||
        descriptor.camera_serial != binding.camera_serial ||
        descriptor.roi_id != binding.roi_id ||
        descriptor.region_id != binding.region_id ||
        descriptor.arena_group_id != binding.arena_group_id ||
        descriptor.arena_id != (binding.has_arena_id ? binding.arena_id : "") ||
        descriptor.logical_stream_id != binding.logical_stream_id ||
        descriptor.spatial_roi_plan_sha256 != binding.plan_sha256 ||
        descriptor.source_gpu_id != binding.source_gpu_id ||
        descriptor.assigned_gpu_id != binding.assigned_gpu_id ||
        descriptor.assigned_shard_id != binding.assigned_shard_id ||
        descriptor.routing_policy != binding.routing_policy) {
        return fail(error_out, "frame identity/GPU fields do not match evidence binding");
    }
    const json descriptor_json = spatial_roi_frame_descriptor_to_json(descriptor);
    for (const char* key : {"native_raster", "content_rect", "encoded_raster",
                            "encoded_content_rect", "padding"}) {
        if (descriptor_json.at(key) != binding.geometry_identity.at(key)) {
            return fail(error_out,
                        std::string("frame geometry does not exactly match binding.") +
                            key);
        }
    }
    if (descriptor.source_pixel_format !=
        binding.encode_profile.at("input_format").get<std::string>()) {
        return fail(error_out,
                    "frame source pixel format does not match encode profile");
    }
    const std::uint64_t width = descriptor.encoded_raster.width;
    const std::uint64_t height = descriptor.encoded_raster.height;
    if (height != 0 && width > std::numeric_limits<std::uint64_t>::max() / height) {
        return fail(error_out, "frame encoded byte geometry overflows");
    }
    if (descriptor.bytes != width * height) {
        return fail(error_out,
                    "frame bytes must exactly equal packed encoded Mono8 geometry");
    }
    return true;
}

json correlation_json(const SpatialRoiFrameDescriptor& descriptor)
{
    return {
        {"recording_identity_token", descriptor.recording_identity_token},
        {"producer_generation", descriptor.producer_generation},
        {"logical_stream_id", descriptor.logical_stream_id},
        {"local_frame_id", descriptor.local_frame_id},
        {"camera_frame_id", descriptor.camera_frame_id},
        {"recording_frame_id", descriptor.recording_frame_id},
        {"roi_stream_frame_index", descriptor.roi_stream_frame_index},
    };
}

bool validate_correlation(const json& value,
                          const SpatialRoiFrameDescriptor& descriptor,
                          std::string* error_out)
{
    if (!exact_keys(value, kCorrelationKeysWithIds, "frame.correlation", error_out)) {
        return false;
    }
    return value == correlation_json(descriptor) ||
        fail(error_out, "frame correlation does not match frame descriptor");
}

std::string metadata_csv_row(const SpatialRoiFrameDescriptor& descriptor,
                             const std::uint64_t output_frame_index)
{
    std::ostringstream row;
    row << descriptor.recording_frame_id << ','
        << descriptor.roi_stream_frame_index << ',' << output_frame_index << ','
        << descriptor.camera_timestamp_ns << ',' << descriptor.timestamp_sys_ns
        << ',' << descriptor.source_gpu_id << ',' << descriptor.assigned_gpu_id
        << ',' << descriptor.assigned_shard_id << '\n';
    return row.str();
}

struct EvidenceCounts {
    std::uint64_t detach_successes = 0;
    std::uint64_t dispatch_admitted = 0;
    std::uint64_t dispatch_rejected = 0;
    std::uint64_t ack_attempted = 0;
    std::uint64_t ack_sent = 0;
    std::uint64_t ack_accepted = 0;
    std::uint64_t release_attempted = 0;
    std::uint64_t release_sent = 0;
    std::uint64_t encoded_frames = 0;
    std::uint64_t failed_frames = 0;
    std::uint64_t packet_count = 0;
    std::uint64_t encoded_bytes = 0;
    std::uint64_t keyframes = 0;
    std::uint64_t ack_write_failures = 0;
    std::uint64_t release_write_failures = 0;
    std::uint64_t lifecycle_failures = 0;
};

struct CanonicalFrameLifecycle {
    std::string detach_status;
    bool source_release_safe = false;
    bool dispatch_admitted = false;
    std::string dispatch_reason;
    bool ack_attempted = false;
    bool ack_sent = false;
    bool ack_accepted = false;
    std::string ack_reason;
    std::string ack_error;
    bool release_attempted = false;
    bool release_sent = false;
    std::string release_reason;
    std::string release_error;
};

bool canonicalize_lifecycle(const SpatialRoiRecorderFrameEvidence& value,
                            CanonicalFrameLifecycle* out,
                            std::string* error_out)
{
    if (!out) {
        return fail(error_out, "frame lifecycle destination is null");
    }
    out->detach_status = value.detach_status;
    out->source_release_safe = value.source_release_safe;
    out->dispatch_admitted = value.dispatch_admitted;
    out->dispatch_reason = value.dispatch_reason;
    out->ack_attempted = value.ack_attempted;
    out->ack_sent = value.ack_sent;
    out->ack_accepted = value.ack_accepted;
    out->ack_reason = value.ack_reason;
    out->ack_error = value.ack_error;
    out->release_attempted = value.release_attempted;
    out->release_sent = value.release_sent;
    out->release_reason = value.release_reason;
    out->release_error = value.release_error;
    return true;
}

struct NormalizedFinalizeRequest {
    std::string terminal_state;
    std::string terminal_reason;
    std::map<std::string, std::string> artifacts;
    json encoder_terminal = nullptr;
};

json normalized_finalize_json(const NormalizedFinalizeRequest& request)
{
    return {
        {"terminal_state", request.terminal_state},
        {"terminal_reason", request.terminal_reason},
        {"artifacts", request.artifacts},
        {"encoder_terminal", request.encoder_terminal},
    };
}

bool encoder_stream_matches_binding(
    const orange::spatial_roi::ipc::SpatialRoiIpcStreamIdentity& stream,
    const SpatialRoiRecorderEvidenceBinding& binding)
{
    return stream.recording_id == binding.recording_id &&
        stream.recording_identity_token == binding.recording_identity_token &&
        stream.producer_generation == binding.producer_generation &&
        stream.camera_id == binding.camera_id &&
        stream.camera_serial == binding.camera_serial &&
        stream.roi_id == binding.roi_id && stream.region_id == binding.region_id &&
        stream.arena_group_id == binding.arena_group_id &&
        stream.arena_id == (binding.has_arena_id ? binding.arena_id : "") &&
        stream.logical_stream_id == binding.logical_stream_id &&
        stream.spatial_roi_plan_sha256 == binding.plan_sha256;
}

json encoder_terminal_snapshot_json(
    const orange::spatial_roi::encoder::SpatialRoiLosslessEncoderTerminalSnapshot&
        snapshot)
{
    const auto& counts = snapshot.counts;
    const auto& writer = snapshot.writer;
    return {
        {"snapshot_schema", "spatial_roi_lossless_terminal_v2"},
        {"terminal", snapshot.terminal},
        {"successful", snapshot.successful},
        {"drain_completed", snapshot.drain_completed},
        {"metadata_flushed", snapshot.metadata_flushed},
        {"media_finalization_validated", snapshot.media_finalization_validated},
        {"artifacts_sealed", snapshot.artifacts_sealed},
        {"all_admitted_results_emitted", snapshot.all_admitted_results_emitted},
        {"all_enqueue_attempts_accounted",
         snapshot.all_enqueue_attempts_accounted},
        {"nonempty_stream", snapshot.nonempty_stream},
        {"source_release_safe", snapshot.source_release_safe},
        {"source_quarantined", snapshot.source_quarantined},
        {"destination_quarantined", snapshot.destination_quarantined},
        {"terminal_reason", snapshot.terminal_reason},
        {"counts", {
            {"enqueue_attempted", counts.enqueue_attempted},
            {"enqueued", counts.enqueued},
            {"dequeued", counts.dequeued},
            {"rejected", counts.rejected},
            {"queue_overflows", counts.queue_overflows},
            {"copy_completed", counts.copy_completed},
            {"source_releases", counts.source_releases},
            {"encoded_frames", counts.encoded_frames},
            {"encoded_packets", counts.encoded_packets},
            {"encoded_bytes", counts.encoded_bytes},
            {"copy_failures", counts.copy_failures},
            {"encode_failures", counts.encode_failures},
            {"writer_failures", counts.writer_failures},
            {"writer_queue_overflows", counts.writer_queue_overflows},
            {"frame_results_emitted", counts.frame_results_emitted},
            {"encoded_results", counts.encoded_results},
            {"failed_results", counts.failed_results},
            {"result_callback_failures", counts.result_callback_failures},
            {"source_quarantines", counts.source_quarantines},
            {"destination_quarantines", counts.destination_quarantines},
            {"peak_queue_depth", counts.peak_queue_depth},
            {"finalize_calls", counts.finalize_calls},
            {"finalized", counts.finalized},
            {"failed", counts.failed},
            {"source_release_safe", counts.source_release_safe},
            {"metadata_flushed", counts.metadata_flushed},
            {"media_finalization_validated", counts.media_finalization_validated},
            {"artifacts_sealed", counts.artifacts_sealed},
        }},
        {"writer", {
            {"snapshot_complete", true},
            {"observed", writer.observed},
            {"failure_latched", writer.failure_latched},
            {"packet_write_error_latched", writer.packet_write_error_latched},
            {"writer_thread_failure_latched", writer.writer_thread_failure_latched},
            {"queue_overflow_latched", writer.queue_overflow_latched},
            {"close_finalization_validated", writer.close_finalization_validated},
            {"close_finalization_failure_latched",
             writer.close_finalization_failure_latched},
            {"packet_allocation_failures", writer.packet_allocation_failures},
            {"packet_enqueue_failures", writer.packet_enqueue_failures},
            {"packet_write_failures", writer.packet_write_failures},
            {"muxer_flush_failures", writer.muxer_flush_failures},
            {"sidecar_write_failures", writer.sidecar_write_failures},
            {"video_size_limit_failures", writer.video_size_limit_failures},
            {"thread_failures", writer.thread_failures},
            {"total_failures", writer.total_failures},
            {"queue_overflow_events", writer.queue_overflow_events},
            {"last_error_code", writer.last_error_code},
            {"first_failure_reason", writer.first_failure_reason},
            {"close_finalization_failure_reason",
             writer.close_finalization_failure_reason},
        }},
    };
}

bool validate_encoder_terminal_snapshot(
    const orange::spatial_roi::encoder::SpatialRoiLosslessEncoderTerminalSnapshot&
        snapshot,
    const SpatialRoiRecorderEvidenceBinding& binding,
    const bool require_success,
    std::string* error_out)
{
    if (!encoder_stream_matches_binding(snapshot.stream, binding) ||
        !snapshot.terminal ||
        (!snapshot.terminal_reason.empty() &&
         !safe_text(snapshot.terminal_reason, kMaxReasonBytes))) {
        return fail(error_out,
                    "encoder terminal snapshot identity/state is invalid");
    }
    const auto& counts = snapshot.counts;
    const bool enqueue_attempts_accounted =
        counts.enqueue_attempted >= counts.enqueued &&
        counts.enqueue_attempted - counts.enqueued == counts.rejected;
    const bool admitted_results_accounted =
        counts.frame_results_emitted >= counts.encoded_results &&
        counts.frame_results_emitted - counts.encoded_results ==
            counts.failed_results;
    const bool all_admitted_results_emitted =
        counts.frame_results_emitted == counts.enqueued;
    const bool encoder_requires_exact = require_success || snapshot.successful;
    if (!enqueue_attempts_accounted ||
        counts.queue_overflows > counts.rejected ||
        counts.dequeued > counts.enqueued ||
        counts.copy_completed > counts.dequeued ||
        counts.source_releases > counts.copy_completed ||
        !admitted_results_accounted ||
        counts.frame_results_emitted > counts.enqueued ||
        counts.encoded_frames != counts.encoded_results ||
        counts.encoded_packets < counts.encoded_results ||
        counts.encoded_packets > counts.enqueued ||
        ((counts.encoded_packets == 0) != (counts.encoded_bytes == 0)) ||
        counts.peak_queue_depth > counts.enqueued ||
        snapshot.all_enqueue_attempts_accounted !=
            enqueue_attempts_accounted ||
        snapshot.all_admitted_results_emitted !=
            all_admitted_results_emitted ||
        snapshot.nonempty_stream != (counts.enqueued != 0) ||
        snapshot.source_quarantined != (counts.source_quarantines != 0) ||
        snapshot.destination_quarantined !=
            (counts.destination_quarantines != 0) ||
        counts.finalized != snapshot.successful ||
        counts.metadata_flushed != snapshot.metadata_flushed ||
        counts.media_finalization_validated !=
            snapshot.media_finalization_validated ||
        counts.artifacts_sealed != snapshot.artifacts_sealed ||
        counts.source_release_safe != snapshot.source_release_safe) {
        return fail(error_out,
                    "encoder terminal snapshot aggregate counts disagree");
    }
    const auto& writer = snapshot.writer;
    if (!writer.first_failure_reason.empty() &&
        !safe_text(writer.first_failure_reason, kMaxReasonBytes)) {
        return fail(error_out,
                    "encoder writer terminal reason contains unsafe text");
    }
    if (encoder_requires_exact &&
        (!snapshot.nonempty_stream || counts.enqueued == 0)) {
        return fail(error_out,
                    "complete finalization requires a nonempty encoder stream");
    }
    if (encoder_requires_exact &&
        (!snapshot.successful || !snapshot.drain_completed ||
         !snapshot.metadata_flushed || !snapshot.media_finalization_validated ||
         !snapshot.artifacts_sealed ||
         !snapshot.all_admitted_results_emitted ||
         !snapshot.all_enqueue_attempts_accounted ||
         !snapshot.nonempty_stream ||
         !snapshot.source_release_safe || snapshot.source_quarantined ||
         snapshot.destination_quarantined || snapshot.terminal_reason != "complete" ||
         counts.enqueue_attempted != counts.enqueued || counts.rejected != 0 ||
         counts.queue_overflows != 0 || counts.dequeued != counts.enqueued ||
         counts.copy_completed != counts.enqueued ||
         counts.source_releases != counts.enqueued ||
         counts.copy_failures != 0 ||
         (counts.enqueued != 0 && counts.peak_queue_depth == 0) ||
         counts.failed || counts.encode_failures != 0 ||
         counts.writer_failures != 0 || counts.result_callback_failures != 0 ||
         counts.writer_queue_overflows != 0 ||
         counts.failed_results != 0 || counts.source_quarantines != 0 ||
         counts.destination_quarantines != 0 || !writer.observed ||
         writer.failure_latched || writer.packet_write_error_latched ||
         writer.writer_thread_failure_latched || writer.queue_overflow_latched ||
         !writer.close_finalization_validated ||
         writer.close_finalization_failure_latched ||
         writer.packet_allocation_failures != 0 ||
         writer.packet_enqueue_failures != 0 ||
         writer.packet_write_failures != 0 || writer.muxer_flush_failures != 0 ||
         writer.sidecar_write_failures != 0 ||
         writer.video_size_limit_failures != 0 ||
         writer.thread_failures != 0 ||
         writer.total_failures != 0 || writer.queue_overflow_events != 0 ||
         writer.last_error_code != 0 || !writer.first_failure_reason.empty() ||
         !writer.close_finalization_failure_reason.empty())) {
        return fail(error_out,
                    "complete finalization lacks a clear successful encoder terminal snapshot");
    }
    return true;
}

bool validate_encoder_terminal_projection(const json& value,
                                          const bool require_success,
                                          const EvidenceCounts* evidence_counts,
                                          std::string* error_out)
{
    static constexpr std::array<std::string_view, 16> top_keys = {
        "terminal", "successful", "drain_completed", "metadata_flushed",
        "media_finalization_validated", "artifacts_sealed",
        "all_admitted_results_emitted", "all_enqueue_attempts_accounted",
        "nonempty_stream",
        "source_release_safe", "source_quarantined",
        "destination_quarantined", "terminal_reason", "counts", "writer",
        "snapshot_schema"};
    // snapshot_schema was not part of the encoder object; the evidence
    // projection supplies it so future encoder fields cannot silently change
    // this closed receipt.
    if (!exact_keys(value, top_keys, "encoder_terminal", error_out) ||
        value.at("snapshot_schema") != "spatial_roi_lossless_terminal_v2") {
        return false;
    }
    static constexpr std::array<std::string_view, 28> count_keys = {
        "enqueue_attempted", "enqueued", "dequeued", "rejected",
        "queue_overflows", "copy_completed", "source_releases",
        "encoded_frames", "encoded_packets", "encoded_bytes",
        "copy_failures", "encode_failures", "writer_failures",
        "writer_queue_overflows",
        "frame_results_emitted", "encoded_results", "failed_results",
        "result_callback_failures", "source_quarantines",
        "destination_quarantines", "peak_queue_depth", "finalize_calls",
        "finalized", "failed",
        "source_release_safe", "metadata_flushed",
        "media_finalization_validated", "artifacts_sealed"};
    static constexpr std::array<std::string_view, 20> writer_keys = {
        "observed", "failure_latched", "packet_write_error_latched",
        "writer_thread_failure_latched", "queue_overflow_latched",
        "close_finalization_validated",
        "close_finalization_failure_latched",
        "packet_allocation_failures", "packet_enqueue_failures",
        "packet_write_failures", "muxer_flush_failures",
        "sidecar_write_failures", "video_size_limit_failures",
        "thread_failures", "total_failures",
        "queue_overflow_events", "last_error_code", "first_failure_reason",
        "close_finalization_failure_reason", "snapshot_complete"};
    const json& counts = value.at("counts");
    const json& writer = value.at("writer");
    if (!exact_keys(counts, count_keys, "encoder_terminal.counts", error_out) ||
        !exact_keys(writer, writer_keys, "encoder_terminal.writer", error_out) ||
        writer.at("snapshot_complete") != true) {
        return false;
    }
    for (const char* key : {"terminal", "successful", "drain_completed",
                            "metadata_flushed", "media_finalization_validated",
                            "artifacts_sealed",
                            "all_admitted_results_emitted",
                            "all_enqueue_attempts_accounted", "nonempty_stream",
                            "source_release_safe",
                            "source_quarantined", "destination_quarantined"}) {
        if (!value.at(key).is_boolean()) {
            return fail(error_out,
                        std::string("encoder_terminal.") + key + " must be boolean");
        }
    }
    for (const char* key : {"finalized", "failed", "source_release_safe",
                            "metadata_flushed", "media_finalization_validated",
                            "artifacts_sealed"}) {
        if (!counts.at(key).is_boolean()) {
            return fail(error_out,
                        std::string("encoder_terminal.counts.") + key +
                            " must be boolean");
        }
    }
    for (const char* key : {"observed", "failure_latched",
                            "packet_write_error_latched",
                            "writer_thread_failure_latched",
                            "queue_overflow_latched",
                            "close_finalization_validated",
                            "close_finalization_failure_latched"}) {
        if (!writer.at(key).is_boolean()) {
            return fail(error_out,
                        std::string("encoder_terminal.writer.") + key +
                            " must be boolean");
        }
    }
    std::string reason;
    std::string writer_reason;
    std::string close_reason;
    if (!read_string(value, "terminal_reason", &reason, "encoder_terminal",
                     error_out, kMaxReasonBytes) ||
        !read_optional_text(writer, "first_failure_reason", &writer_reason,
                            "encoder_terminal.writer", error_out,
                            kMaxReasonBytes) ||
        !read_optional_text(writer, "close_finalization_failure_reason",
                            &close_reason, "encoder_terminal.writer", error_out,
                            kMaxReasonBytes)) {
        return false;
    }
    std::map<std::string, std::uint64_t> numeric_counts;
    for (const char* key : {"enqueue_attempted", "enqueued", "dequeued",
                            "rejected", "queue_overflows", "copy_completed",
                            "source_releases", "encoded_frames",
                            "encoded_packets", "encoded_bytes", "copy_failures",
                            "encode_failures", "writer_failures",
                            "writer_queue_overflows", "frame_results_emitted",
                            "encoded_results", "failed_results",
                            "result_callback_failures", "source_quarantines",
                            "destination_quarantines", "peak_queue_depth",
                            "finalize_calls"}) {
        if (!read_u64(counts, key, &numeric_counts[key],
                      "encoder_terminal.counts", error_out)) {
            return false;
        }
    }
    std::map<std::string, std::uint64_t> numeric_writer;
    for (const char* key : {"packet_allocation_failures",
                            "packet_enqueue_failures", "packet_write_failures",
                            "muxer_flush_failures", "sidecar_write_failures",
                            "video_size_limit_failures", "thread_failures",
                            "total_failures",
                            "queue_overflow_events"}) {
        if (!read_u64(writer, key, &numeric_writer[key],
                      "encoder_terminal.writer", error_out)) {
            return false;
        }
    }
    int last_error_code = 0;
    if (!read_int(writer, "last_error_code", &last_error_code,
                  "encoder_terminal.writer", error_out, false)) {
        return false;
    }
    const bool enqueue_attempts_accounted =
        numeric_counts["enqueue_attempted"] >= numeric_counts["enqueued"] &&
        numeric_counts["enqueue_attempted"] - numeric_counts["enqueued"] ==
            numeric_counts["rejected"];
    const bool admitted_results_accounted =
        numeric_counts["frame_results_emitted"] >=
            numeric_counts["encoded_results"] &&
        numeric_counts["frame_results_emitted"] -
                numeric_counts["encoded_results"] ==
            numeric_counts["failed_results"];
    const bool all_admitted_results_emitted =
        numeric_counts["frame_results_emitted"] == numeric_counts["enqueued"];
    const bool encoder_requires_exact =
        require_success || value.at("successful").get<bool>();
    if (!enqueue_attempts_accounted ||
        numeric_counts["queue_overflows"] > numeric_counts["rejected"] ||
        numeric_counts["dequeued"] > numeric_counts["enqueued"] ||
        numeric_counts["copy_completed"] > numeric_counts["dequeued"] ||
        numeric_counts["source_releases"] >
            numeric_counts["copy_completed"] ||
        !admitted_results_accounted ||
        numeric_counts["frame_results_emitted"] > numeric_counts["enqueued"] ||
        numeric_counts["encoded_frames"] != numeric_counts["encoded_results"] ||
        numeric_counts["encoded_packets"] < numeric_counts["encoded_results"] ||
        numeric_counts["encoded_packets"] > numeric_counts["enqueued"] ||
        ((numeric_counts["encoded_packets"] == 0) !=
         (numeric_counts["encoded_bytes"] == 0)) ||
        numeric_counts["peak_queue_depth"] > numeric_counts["enqueued"] ||
        value.at("all_enqueue_attempts_accounted") !=
            enqueue_attempts_accounted ||
        value.at("all_admitted_results_emitted") !=
            all_admitted_results_emitted ||
        value.at("nonempty_stream") !=
            (numeric_counts["enqueued"] != 0) ||
        value.at("source_quarantined") !=
            (numeric_counts["source_quarantines"] != 0) ||
        value.at("destination_quarantined") !=
            (numeric_counts["destination_quarantines"] != 0) ||
        counts.at("finalized") != value.at("successful") ||
        counts.at("metadata_flushed") != value.at("metadata_flushed") ||
        counts.at("media_finalization_validated") !=
            value.at("media_finalization_validated") ||
        counts.at("artifacts_sealed") != value.at("artifacts_sealed") ||
        counts.at("source_release_safe") != value.at("source_release_safe")) {
        return fail(error_out,
                    "encoder terminal projection aggregate counts disagree");
    }
    if (evidence_counts &&
        (numeric_counts["encoded_frames"] < evidence_counts->encoded_frames ||
         numeric_counts["encoded_packets"] < evidence_counts->packet_count ||
         numeric_counts["encoded_bytes"] < evidence_counts->encoded_bytes ||
         (encoder_requires_exact &&
          (numeric_counts["encoded_frames"] !=
               evidence_counts->encoded_frames ||
           numeric_counts["encoded_packets"] !=
               evidence_counts->packet_count ||
           numeric_counts["encoded_bytes"] !=
               evidence_counts->encoded_bytes)))) {
        return fail(error_out,
                    "encoder terminal projection disagrees with frame evidence");
    }
    if (encoder_requires_exact &&
        (value.at("terminal") != true || value.at("successful") != true ||
         value.at("drain_completed") != true ||
         value.at("metadata_flushed") != true ||
         value.at("media_finalization_validated") != true ||
         value.at("artifacts_sealed") != true ||
         value.at("all_admitted_results_emitted") != true ||
         value.at("all_enqueue_attempts_accounted") != true ||
         value.at("nonempty_stream") != true ||
         value.at("source_release_safe") != true ||
         value.at("source_quarantined") != false ||
         value.at("destination_quarantined") != false || reason != "complete" ||
         counts.at("finalized") != true || counts.at("failed") != false ||
         counts.at("artifacts_sealed") != true ||
         numeric_counts["enqueue_attempted"] != numeric_counts["enqueued"] ||
         numeric_counts["rejected"] != 0 ||
         numeric_counts["queue_overflows"] != 0 ||
         numeric_counts["dequeued"] != numeric_counts["enqueued"] ||
         numeric_counts["copy_completed"] != numeric_counts["enqueued"] ||
         numeric_counts["source_releases"] != numeric_counts["enqueued"] ||
         numeric_counts["copy_failures"] != 0 ||
         (numeric_counts["enqueued"] != 0 &&
          numeric_counts["peak_queue_depth"] == 0) ||
         numeric_counts["encode_failures"] != 0 ||
         numeric_counts["writer_failures"] != 0 ||
         numeric_counts["writer_queue_overflows"] != 0 ||
         numeric_counts["result_callback_failures"] != 0 ||
         numeric_counts["failed_results"] != 0 ||
         numeric_counts["source_quarantines"] != 0 ||
         numeric_counts["destination_quarantines"] != 0 ||
         writer.at("observed") != true || writer.at("failure_latched") != false ||
         writer.at("packet_write_error_latched") != false ||
         writer.at("writer_thread_failure_latched") != false ||
         writer.at("queue_overflow_latched") != false ||
         writer.at("close_finalization_validated") != true ||
         writer.at("close_finalization_failure_latched") != false ||
         !writer_reason.empty() || !close_reason.empty() ||
         last_error_code != 0 ||
         std::any_of(numeric_writer.begin(), numeric_writer.end(),
                     [](const auto& item) { return item.second != 0; }))) {
        return fail(error_out,
                    "complete encoder terminal projection contains failure state");
    }
    return true;
}

bool normalize_finalize_request(
    const SpatialRoiRecorderFinalizeRequest& request,
    const SpatialRoiRecorderEvidenceBinding& binding,
    NormalizedFinalizeRequest* output,
    std::string* error_out)
{
    if (!output) {
        return fail(error_out, "normalized finalization destination is null");
    }
    if (request.terminal_state != kTerminalComplete &&
        request.terminal_state != kTerminalFailed) {
        return fail(error_out,
                    "finalization terminal_state must be complete or failed");
    }
    NormalizedFinalizeRequest normalized;
    normalized.terminal_state = request.terminal_state;
    if (request.terminal_state == kTerminalComplete) {
        if (!request.terminal_reason.empty() &&
            request.terminal_reason != kTerminalComplete) {
            return fail(error_out,
                        "complete finalization reason must be empty or complete");
        }
        normalized.terminal_reason = kTerminalComplete;
    } else {
        if (!safe_text(request.terminal_reason, kMaxReasonBytes)) {
            return fail(error_out,
                        "failed finalization requires a bounded terminal reason");
        }
        normalized.terminal_reason = request.terminal_reason;
    }
    if (request.encoder_terminal_snapshot) {
        if (!validate_encoder_terminal_snapshot(
                *request.encoder_terminal_snapshot, binding,
                request.terminal_state == kTerminalComplete, error_out)) {
            return false;
        }
        normalized.encoder_terminal =
            encoder_terminal_snapshot_json(*request.encoder_terminal_snapshot);
    } else if (request.terminal_state == kTerminalComplete) {
        return fail(error_out,
                    "complete finalization requires the immutable encoder terminal snapshot");
    }
    if (request.artifacts.size() > kFinalizeArtifactKinds.size()) {
        return fail(error_out, "finalization artifact set exceeds its closed bound");
    }
    std::set<std::string> paths;
    for (const auto& artifact : request.artifacts) {
        if (artifact.kind == "evidence" || artifact.kind == "evidence_manifest" ||
            std::find(kFinalizeArtifactKinds.begin(), kFinalizeArtifactKinds.end(),
                      artifact.kind) == kFinalizeArtifactKinds.end()) {
            return fail(error_out,
                        "finalization contains a non-output artifact kind");
        }
        const auto expected = binding.expected_artifacts.find(artifact.kind);
        if (expected == binding.expected_artifacts.end() ||
            artifact.relative_path != expected->second ||
            !normalized.artifacts.emplace(artifact.kind,
                                          artifact.relative_path).second ||
            !paths.insert(artifact.relative_path).second) {
            return fail(error_out,
                        "finalization artifact kind/path does not exactly match contract");
        }
    }
    if (request.terminal_state == kTerminalComplete) {
        if (normalized.artifacts.size() != kFinalizeArtifactKinds.size()) {
            return fail(error_out,
                        "complete finalization requires every contract artifact");
        }
        for (const std::string_view kind : kFinalizeArtifactKinds) {
            if (!normalized.artifacts.count(std::string(kind))) {
                return fail(error_out,
                            "complete finalization is missing contract artifact " +
                                std::string(kind));
            }
        }
    }
    *output = std::move(normalized);
    return true;
}

bool validate_terminal_artifact_receipts(
    const json& receipts,
    const SpatialRoiRecorderEvidenceBinding& binding,
    const std::string& terminal_state,
    NormalizedFinalizeRequest* normalized_out,
    std::string* error_out)
{
    if (!receipts.is_object() ||
        receipts.size() > kFinalizeArtifactKinds.size()) {
        return fail(error_out,
                    "terminal artifact receipts exceed their closed bound");
    }
    NormalizedFinalizeRequest normalized;
    normalized.terminal_state = terminal_state;
    normalized.terminal_reason = terminal_state;
    static constexpr std::array<std::string_view, 3> reference_keys = {
        "relative_path", "size_bytes", "sha256"};
    for (const auto& [kind, receipt] : receipts.items()) {
        if (std::find(kFinalizeArtifactKinds.begin(), kFinalizeArtifactKinds.end(),
                      kind) == kFinalizeArtifactKinds.end() ||
            !exact_keys(receipt, reference_keys,
                        "evidence.terminal.artifacts." + kind, error_out)) {
            return false;
        }
        std::string path;
        std::uint64_t size = 0;
        if (!read_string(receipt, "relative_path", &path,
                         "evidence.terminal.artifacts." + kind, error_out,
                         kSpatialRoiRecorderEvidenceMaxPathBytes) ||
            !read_u64(receipt, "size_bytes", &size,
                      "evidence.terminal.artifacts." + kind, error_out) ||
            !validate_sha_field(receipt, "sha256",
                                "evidence.terminal.artifacts." + kind,
                                error_out) ||
            size > static_cast<std::uint64_t>(
                       std::numeric_limits<off_t>::max())) {
            return fail(error_out, "terminal artifact receipt is invalid");
        }
        const auto expected = binding.expected_artifacts.find(kind);
        if (expected == binding.expected_artifacts.end() ||
            expected->second != path ||
            !normalized.artifacts.emplace(kind, path).second) {
            return fail(error_out,
                        "terminal artifact receipt path does not match contract");
        }
    }
    if (terminal_state == kTerminalComplete &&
        normalized.artifacts.size() != kFinalizeArtifactKinds.size()) {
        return fail(error_out,
                    "complete terminal receipt is missing contract artifacts");
    }
    *normalized_out = std::move(normalized);
    return true;
}

bool increment(std::uint64_t* target, const std::uint64_t amount = 1)
{
    if (!target || std::numeric_limits<std::uint64_t>::max() - *target < amount) {
        return false;
    }
    *target += amount;
    return true;
}

bool validate_frame_input(const SpatialRoiRecorderEvidenceBinding& binding,
                          const SpatialRoiRecorderFrameEvidence& value,
                          std::string* error_out,
                          EvidenceCounts* counts_out)
{
    if (!validate_spatial_roi_frame_descriptor(value.frame, error_out)) {
        return false;
    }
    if (!binding_matches_descriptor(binding, value.frame, error_out)) {
        return false;
    }
    CanonicalFrameLifecycle lifecycle;
    if (!canonicalize_lifecycle(value, &lifecycle, error_out)) {
        return false;
    }
    if (!safe_identifier(lifecycle.detach_status, 64) ||
        std::find(kDetachStatuses.begin(), kDetachStatuses.end(),
                  lifecycle.detach_status) == kDetachStatuses.end()) {
        return fail(error_out, "frame.detach_status is not a known detach result");
    }
    if (!safe_text(lifecycle.dispatch_reason, kMaxReasonBytes) &&
        !lifecycle.dispatch_reason.empty()) {
        return fail(error_out, "frame.dispatch_reason contains unsafe text");
    }
    if (!safe_text(lifecycle.ack_reason, kMaxReasonBytes) &&
        !lifecycle.ack_reason.empty()) {
        return fail(error_out, "frame.ack_reason contains unsafe text");
    }
    if (!safe_text(lifecycle.ack_error, kMaxReasonBytes) &&
        !lifecycle.ack_error.empty()) {
        return fail(error_out, "frame.ack_error contains unsafe text");
    }
    if (!safe_text(lifecycle.release_reason, kMaxReasonBytes) &&
        !lifecycle.release_reason.empty()) {
        return fail(error_out, "frame.release_reason contains unsafe text");
    }
    if (!safe_text(lifecycle.release_error, kMaxReasonBytes) &&
        !lifecycle.release_error.empty()) {
        return fail(error_out, "frame.release_error contains unsafe text");
    }
    if (lifecycle.dispatch_admitted && !lifecycle.dispatch_reason.empty()) {
        return fail(error_out, "admitted dispatch reason must be empty");
    }
    if (!lifecycle.dispatch_admitted && lifecycle.dispatch_reason.empty() &&
        (lifecycle.ack_attempted || lifecycle.release_attempted ||
         lifecycle.detach_status == "detached")) {
        return fail(error_out, "rejected dispatch requires a reason");
    }
    if (!lifecycle.ack_attempted &&
        (lifecycle.ack_sent || lifecycle.ack_accepted ||
         !lifecycle.ack_reason.empty() || !lifecycle.ack_error.empty())) {
        return fail(error_out, "ACK evidence is attempted inconsistently");
    }
    if (lifecycle.ack_sent && !lifecycle.ack_attempted) {
        return fail(error_out, "sent ACK was not attempted");
    }
    if (lifecycle.ack_accepted != lifecycle.dispatch_admitted) {
        return fail(error_out,
                    "ACK payload accepted bit must equal dispatch admission");
    }
    if (lifecycle.ack_accepted && !lifecycle.ack_reason.empty()) {
        return fail(error_out, "accepted ACK reason must be empty");
    }
    if (lifecycle.ack_attempted && !lifecycle.ack_accepted &&
        lifecycle.ack_reason.empty()) {
        return fail(error_out, "unaccepted ACK requires a reason");
    }
    if (lifecycle.ack_sent && !lifecycle.ack_error.empty()) {
        return fail(error_out, "sent ACK must not carry a write error");
    }
    if (!lifecycle.ack_sent && lifecycle.ack_attempted &&
        lifecycle.ack_error.empty()) {
        return fail(error_out, "failed ACK write requires an error");
    }
    if (!lifecycle.release_attempted &&
        (lifecycle.release_sent ||
         !lifecycle.release_reason.empty() || !lifecycle.release_error.empty())) {
        return fail(error_out, "RELEASE evidence is attempted inconsistently");
    }
    if (lifecycle.release_sent && !lifecycle.release_attempted) {
        return fail(error_out, "sent RELEASE was not attempted");
    }
    if (lifecycle.release_attempted && !lifecycle.ack_sent) {
        return fail(error_out,
                    "RELEASE cannot be attempted after ACK write failure");
    }
    const std::string expected_release_reason =
        lifecycle.dispatch_admitted ? "source_detached" : "source_rejected";
    if (lifecycle.release_attempted &&
        lifecycle.release_reason != expected_release_reason) {
        return fail(error_out,
                    "RELEASE reason does not match dispatch outcome");
    }
    if (lifecycle.release_sent && !lifecycle.release_error.empty()) {
        return fail(error_out, "sent RELEASE must not carry a write error");
    }
    if (lifecycle.release_attempted && !lifecycle.release_sent &&
        lifecycle.release_error.empty()) {
        return fail(error_out, "failed RELEASE write requires an error");
    }
    if (lifecycle.dispatch_admitted && !lifecycle.ack_attempted) {
        return fail(error_out, "admitted dispatch requires an ACK attempt");
    }
    if (!safe_identifier(value.encode_status, 32) ||
        (value.encode_status != kEncodeEncoded &&
         value.encode_status != kEncodeFailed &&
         value.encode_status != kEncodeNotAttempted)) {
        return fail(error_out, "frame encode status is not recognized");
    }
    if (!lifecycle.dispatch_admitted && value.encode_status != kEncodeNotAttempted) {
        return fail(error_out, "rejected dispatch cannot carry encode evidence");
    }
    if (lifecycle.dispatch_admitted && value.encode_status == kEncodeNotAttempted) {
        return fail(error_out, "admitted dispatch requires an encoder result");
    }
    const std::uint64_t gop_length =
        encode_profile_gop_length(binding.encode_profile);
    if (gop_length == 0) {
        return fail(error_out, "frame binding has no valid GOP length");
    }
    if (value.encode_status == kEncodeEncoded &&
        (value.output_frame_index == 0 || value.packet_count != 1 ||
         value.encoded_bytes == 0 ||
         value.keyframe != expected_keyframe_for_output(
                               value.output_frame_index, gop_length) ||
         value.output_frame_index != value.frame.roi_stream_frame_index)) {
        return fail(error_out,
                    "encoded frame requires positive output/packet/byte counts and exact configured-GOP keyframe evidence");
    }
    if (value.encode_status != kEncodeEncoded &&
        (value.output_frame_index != 0 || value.packet_count != 0 ||
         value.encoded_bytes != 0 || value.keyframe)) {
        return fail(error_out,
                    "nonencoded frame must not claim packets, bytes, or keyframes");
    }
    if (counts_out) {
        if (lifecycle.detach_status == "detached" &&
            !increment(&counts_out->detach_successes)) {
            return fail(error_out, "detach success count overflow");
        }
        if (lifecycle.dispatch_admitted &&
            !increment(&counts_out->dispatch_admitted)) {
            return fail(error_out, "dispatch admission count overflow");
        }
        if (!lifecycle.dispatch_admitted &&
            !increment(&counts_out->dispatch_rejected)) {
            return fail(error_out, "dispatch rejection count overflow");
        }
        if (lifecycle.ack_attempted && !increment(&counts_out->ack_attempted)) {
            return fail(error_out, "ACK attempt count overflow");
        }
        if (lifecycle.ack_sent && !increment(&counts_out->ack_sent)) {
            return fail(error_out, "ACK count overflow");
        }
        if (lifecycle.ack_accepted && !increment(&counts_out->ack_accepted)) {
            return fail(error_out, "accepted ACK count overflow");
        }
        if (lifecycle.release_attempted &&
            !increment(&counts_out->release_attempted)) {
            return fail(error_out, "RELEASE attempt count overflow");
        }
        if (lifecycle.release_sent && !increment(&counts_out->release_sent)) {
            return fail(error_out, "RELEASE count overflow");
        }
        if (lifecycle.ack_attempted && !lifecycle.ack_sent &&
            !lifecycle.ack_error.empty() &&
            !increment(&counts_out->ack_write_failures)) {
            return fail(error_out, "ACK write failure count overflow");
        }
        if (lifecycle.release_attempted && !lifecycle.release_sent &&
            !lifecycle.release_error.empty() &&
            !increment(&counts_out->release_write_failures)) {
            return fail(error_out, "RELEASE write failure count overflow");
        }
        const bool frame_failed = lifecycle.detach_status != "detached" ||
            !lifecycle.dispatch_admitted || !lifecycle.ack_sent ||
            !lifecycle.release_sent || value.encode_status != kEncodeEncoded;
        if (frame_failed && !increment(&counts_out->failed_frames)) {
            return fail(error_out, "failed frame count overflow");
        }
        if (frame_failed && !increment(&counts_out->lifecycle_failures)) {
            return fail(error_out, "lifecycle failure count overflow");
        }
        if (value.encode_status == kEncodeEncoded &&
            !increment(&counts_out->encoded_frames)) {
            return fail(error_out, "encoded frame count overflow");
        }
        if (!increment(&counts_out->packet_count, value.packet_count) ||
            !increment(&counts_out->encoded_bytes, value.encoded_bytes)) {
            return fail(error_out, "packet/byte count overflow");
        }
        if (value.keyframe && !increment(&counts_out->keyframes)) {
            return fail(error_out, "keyframe count overflow");
        }
    }
    return true;
}

json frame_evidence_json(const SpatialRoiRecorderFrameEvidence& value,
                         const std::size_t record_index)
{
    CanonicalFrameLifecycle lifecycle;
    std::string ignored;
    (void)canonicalize_lifecycle(value, &lifecycle, &ignored);
    return {
        {"record_type", kFrameRecordType},
        {"schema_id", kSpatialRoiRecorderEvidenceSchemaId},
        {"schema_version", kSpatialRoiRecorderEvidenceSchemaVersion},
        {"canonicalization", kSpatialRoiRecorderCanonicalization},
        {"frame_record_index", record_index},
        {"correlation", correlation_json(value.frame)},
        {"frame", spatial_roi_frame_descriptor_to_json(value.frame)},
        {"detach", {
            {"status", lifecycle.detach_status},
            {"source_release_safe", lifecycle.source_release_safe},
        }},
        {"dispatch", {
            {"admitted", lifecycle.dispatch_admitted},
            {"reason", lifecycle.dispatch_reason},
        }},
        {"ack", {
            {"attempted", lifecycle.ack_attempted},
            {"sent", lifecycle.ack_sent},
            {"accepted", lifecycle.ack_accepted},
            {"reason", lifecycle.ack_reason},
            {"error", lifecycle.ack_error},
        }},
        {"release", {
            {"attempted", lifecycle.release_attempted},
            {"sent", lifecycle.release_sent},
            {"reason", lifecycle.release_reason},
            {"error", lifecycle.release_error},
        }},
        {"encode", {
            {"status", value.encode_status},
            {"output_frame_index", value.output_frame_index},
            {"packet_count", value.packet_count},
            {"encoded_bytes", value.encoded_bytes},
            {"keyframe", value.keyframe},
        }},
    };
}

json counts_json(const EvidenceCounts& counts)
{
    return {
        {"detach_successes", counts.detach_successes},
        {"dispatch_admitted", counts.dispatch_admitted},
        {"dispatch_rejected", counts.dispatch_rejected},
        {"ack_attempted", counts.ack_attempted},
        {"ack_sent", counts.ack_sent},
        {"ack_accepted", counts.ack_accepted},
        {"release_attempted", counts.release_attempted},
        {"release_sent", counts.release_sent},
        {"encoded_frames", counts.encoded_frames},
        {"failed_frames", counts.failed_frames},
        {"packet_count", counts.packet_count},
        {"encoded_bytes", counts.encoded_bytes},
        {"keyframes", counts.keyframes},
        {"ack_write_failures", counts.ack_write_failures},
        {"release_write_failures", counts.release_write_failures},
        {"lifecycle_failures", counts.lifecycle_failures},
    };
}

json ranges_json(const bool has_frames,
                 const std::size_t frame_count,
                 const std::uint64_t first_recording_frame_id,
                 const std::uint64_t last_recording_frame_id,
                 const std::uint64_t first_roi_stream_frame_index,
                 const std::uint64_t last_roi_stream_frame_index)
{
    return {
        {"recording_frame_id", {
            {"first", first_recording_frame_id},
            {"last", last_recording_frame_id},
        }},
        {"roi_stream_frame_index", {
            {"first", first_roi_stream_frame_index},
            {"last", last_roi_stream_frame_index},
        }},
        {"has_frames", has_frames},
        {"frame_count", frame_count},
    };
}

json terminal_json(const std::string& state,
                   const std::string& reason,
                   const std::string& finalize_request_sha256,
                   const json& artifacts,
                   const json& encoder_terminal,
                   const SpatialRoiRecorderEvidenceBinding& binding,
                   const std::size_t frame_count,
                   const EvidenceCounts& counts,
                   const bool has_frames,
                   const std::uint64_t first_recording_frame_id,
                   const std::uint64_t last_recording_frame_id,
                   const std::uint64_t first_roi_stream_frame_index,
                   const std::uint64_t last_roi_stream_frame_index)
{
    return {
        {"record_type", kTerminalRecordType},
        {"schema_id", kSpatialRoiRecorderEvidenceSchemaId},
        {"schema_version", kSpatialRoiRecorderEvidenceSchemaVersion},
        {"canonicalization", kSpatialRoiRecorderCanonicalization},
        {"terminal_state", state},
        {"reason", reason},
        {"frame_count", frame_count},
        {"ranges", ranges_json(has_frames, frame_count, first_recording_frame_id,
                                last_recording_frame_id,
                                first_roi_stream_frame_index,
                                last_roi_stream_frame_index)},
        {"counts", counts_json(counts)},
        {"binding_sha256", binding.contract_sha256},
        {"receipt_scope", "canonical_manifest_without_finalized_receipt_digest_v1"},
        {"finalize_request_sha256", finalize_request_sha256},
        {"artifacts", artifacts},
        {"encoder_terminal", encoder_terminal},
    };
}

json header_json(const SpatialRoiRecorderEvidenceBinding& binding)
{
    return {
        {"record_type", kHeaderRecordType},
        {"schema_id", kSpatialRoiRecorderEvidenceSchemaId},
        {"schema_version", kSpatialRoiRecorderEvidenceSchemaVersion},
        {"canonicalization", kSpatialRoiRecorderCanonicalization},
        {"binding", spatial_roi_recorder_evidence_binding_to_json(binding)},
    };
}

struct Fd final {
    int value = -1;
    Fd() = default;
    explicit Fd(const int fd) : value(fd) {}
    ~Fd()
    {
        if (value >= 0) {
            (void)::close(value);
        }
    }
    Fd(const Fd&) = delete;
    Fd& operator=(const Fd&) = delete;
    Fd(Fd&& other) noexcept : value(other.value) { other.value = -1; }
    Fd& operator=(Fd&& other) noexcept
    {
        if (this != &other) {
            if (value >= 0) {
                (void)::close(value);
            }
            value = other.value;
            other.value = -1;
        }
        return *this;
    }
    int release() noexcept
    {
        const int result = value;
        value = -1;
        return result;
    }
    explicit operator bool() const noexcept { return value >= 0; }
};

constexpr std::size_t kMaxJsonDepth = 64;
constexpr std::size_t kMaxJsonStringBytes = 1024 * 1024;
constexpr std::size_t kMaxJsonContainerItems =
    kSpatialRoiRecorderEvidenceMaxFrames + 16;

// nlohmann's DOM parser intentionally keeps the last value for a duplicate
// object key.  Evidence is security-sensitive, so every byte-backed JSON
// document first passes this bounded SAX walk before a DOM is constructed.
class BoundedUniqueKeyJsonSax final : public nlohmann::json_sax<json> {
public:
    explicit BoundedUniqueKeyJsonSax(const std::size_t max_events)
        : max_events_(max_events)
    {
    }

    bool null() override { return event(); }
    bool boolean(bool) override { return event(); }
    bool number_integer(number_integer_t) override { return event(); }
    bool number_unsigned(number_unsigned_t) override { return event(); }
    bool number_float(number_float_t, const string_t& text) override
    {
        return text.size() <= kMaxJsonStringBytes && event();
    }
    bool string(string_t& value) override
    {
        return value.size() <= kMaxJsonStringBytes && event();
    }
    bool binary(binary_t&) override { return false; }

    bool start_object(const std::size_t elements) override
    {
        if (!start_container(elements)) {
            return false;
        }
        stack_.push_back({true, {}});
        return true;
    }

    bool key(string_t& value) override
    {
        if (stack_.empty() || !stack_.back().object ||
            value.size() > kMaxJsonStringBytes || !event()) {
            return false;
        }
        if (stack_.back().keys.size() >= kMaxJsonContainerItems) {
            return false;
        }
        return stack_.back().keys.insert(value).second;
    }

    bool end_object() override
    {
        if (stack_.empty() || !stack_.back().object || !event()) {
            return false;
        }
        stack_.pop_back();
        return true;
    }

    bool start_array(const std::size_t elements) override
    {
        if (!start_container(elements)) {
            return false;
        }
        stack_.push_back({false, {}});
        return true;
    }

    bool end_array() override
    {
        if (stack_.empty() || stack_.back().object || !event()) {
            return false;
        }
        stack_.pop_back();
        return true;
    }

    bool parse_error(std::size_t,
                     const std::string&,
                     const nlohmann::detail::exception&) override
    {
        return false;
    }

private:
    struct Container {
        bool object = false;
        std::unordered_set<std::string> keys;
    };

    bool event() noexcept
    {
        if (events_ >= max_events_) {
            return false;
        }
        ++events_;
        return true;
    }

    bool start_container(const std::size_t elements) noexcept
    {
        return stack_.size() < kMaxJsonDepth &&
            (elements == static_cast<std::size_t>(-1) ||
             elements <= kMaxJsonContainerItems) &&
            event();
    }

    std::size_t max_events_ = 0;
    std::size_t events_ = 0;
    std::vector<Container> stack_;
};

bool strict_json_from_bytes(const std::string& bytes,
                            const std::size_t max_events,
                            json* value_out,
                            const std::string& label,
                            std::string* error_out)
{
    if (!value_out) {
        return fail(error_out, label + " JSON destination is null");
    }
    BoundedUniqueKeyJsonSax validator(max_events);
    if (!json::sax_parse(bytes, &validator)) {
        return fail(error_out,
                    label +
                        " JSON is invalid, structurally unbounded, or contains duplicate keys");
    }
    json value = json::parse(bytes, nullptr, false);
    if (value.is_discarded()) {
        return fail(error_out, label + " is not valid JSON");
    }
    *value_out = std::move(value);
    return true;
}

bool write_all_fd(const int fd, const std::string& bytes, std::string* error_out)
{
    std::size_t offset = 0;
    while (offset < bytes.size()) {
        const ssize_t written = ::write(fd, bytes.data() + offset,
                                        bytes.size() - offset);
        if (written < 0) {
            if (errno == EINTR) {
                continue;
            }
            return fail(error_out,
                        "evidence write failed: " +
                            std::string(std::strerror(errno)));
        }
        if (written == 0) {
            return fail(error_out, "evidence write returned zero bytes");
        }
        offset += static_cast<std::size_t>(written);
    }
    return true;
}

bool validate_owned_root(const std::filesystem::path& root,
                         std::string* error_out)
{
    if (root.empty() || !root.is_absolute() || root.lexically_normal() != root ||
        root == root.root_path()) {
        return fail(error_out, "owned recording root must be absolute, normalized, and non-root");
    }
    return true;
}

bool validate_artifact_root_authority(
    const std::shared_ptr<SpatialRoiRecorderArtifactRoot>& authority,
    const SpatialRoiRecorderEvidenceBinding& binding,
    std::string* error_out)
{
    if (!authority || !authority->valid()) {
        return fail(error_out,
                    "recorder artifact-root authority is absent or invalid");
    }
    const std::filesystem::path recording_root(binding.recording_root);
    const std::filesystem::path artifact_root(binding.artifact_root);
    const std::filesystem::path expected_artifact_root =
        recording_root / kSpatialRoiRecorderArtifactDirectory;
    if (!validate_owned_root(recording_root, error_out) ||
        !validate_owned_root(artifact_root, error_out) ||
        authority->opened_recording_root() != recording_root ||
        artifact_root != expected_artifact_root.lexically_normal()) {
        return fail(error_out,
                    "recorder artifact-root authority does not match the authenticated binding roots");
    }
    struct stat recording_status {};
    struct stat artifact_status {};
    if (::fstat(authority->borrowed_recording_root_fd(), &recording_status) != 0 ||
        ::fstat(authority->borrowed_artifact_root_fd(), &artifact_status) != 0 ||
        !S_ISDIR(recording_status.st_mode) || !S_ISDIR(artifact_status.st_mode) ||
        authority->recording_root_identity().device !=
            static_cast<std::uint64_t>(recording_status.st_dev) ||
        authority->recording_root_identity().inode !=
            static_cast<std::uint64_t>(recording_status.st_ino) ||
        authority->artifact_root_identity().device !=
            static_cast<std::uint64_t>(artifact_status.st_dev) ||
        authority->artifact_root_identity().inode !=
            static_cast<std::uint64_t>(artifact_status.st_ino) ||
        authority->recording_root_identity() ==
            authority->artifact_root_identity()) {
        return fail(error_out,
                    "recorder artifact-root retained filesystem identity is invalid");
    }
    for (const auto& [kind, relative_path] : binding.expected_artifacts) {
        if (!authority->IsAllowed(relative_path)) {
            return fail(error_out,
                        "recorder artifact-root allow-list omits contract artifact " +
                            kind);
        }
    }
    return true;
}

bool open_existing_artifact(
    const SpatialRoiRecorderArtifactRoot& authority,
    const std::string& relative_path,
    std::unique_ptr<SpatialRoiRecorderArtifactFile>* file_out,
    std::string* error_out)
{
    if (!safe_relative_path(relative_path) ||
        !authority.IsAllowed(relative_path)) {
        return fail(error_out,
                    "artifact path is not authorized by the retained contract root: " +
                        relative_path);
    }
    if (!authority.OpenExistingFile(
        relative_path, SpatialRoiRecorderArtifactFileAccess::kReadOnly,
        file_out, error_out)) {
        return false;
    }
    if (!*file_out || !(*file_out)->valid() ||
        (*file_out)->relative_path() != relative_path ||
        (*file_out)->artifact_root_identity() !=
            authority.artifact_root_identity()) {
        file_out->reset();
        return fail(error_out,
                    "opened artifact handle does not retain the authenticated root/path identity");
    }
    return true;
}

bool duplicate_fd(const int source, Fd* destination, std::string* error_out)
{
    const int fd = ::dup(source);
    if (fd < 0) {
        return fail(error_out, "could not duplicate recording root fd: " +
                                   std::string(std::strerror(errno)));
    }
    (void)::fcntl(fd, F_SETFD, FD_CLOEXEC);
    destination->value = fd;
    return true;
}

bool open_parent_dir(const int root_fd,
                     const std::string& relative_path,
                     const bool create_directories,
                     Fd* parent_out,
                     std::string* leaf_out,
                     std::string* error_out)
{
    if (!safe_relative_path(relative_path) || !parent_out || !leaf_out) {
        return fail(error_out, "unsafe relative artifact path");
    }
    const std::vector<std::string> components = path_components(relative_path);
    if (components.empty()) {
        return fail(error_out, "artifact path has no components");
    }
    Fd current;
    if (!duplicate_fd(root_fd, &current, error_out)) {
        return false;
    }
    for (std::size_t index = 0; index + 1 < components.size(); ++index) {
        const std::string& component = components[index];
        int next = ::openat(current.value,
                            component.c_str(),
                            O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
        if (next < 0 && errno == ENOENT && create_directories) {
            if (::mkdirat(current.value, component.c_str(), 0700) != 0 &&
                errno != EEXIST) {
                return fail(error_out,
                            "could not create evidence directory " + component +
                                ": " + std::strerror(errno));
            }
            next = ::openat(current.value,
                            component.c_str(),
                            O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
        }
        if (next < 0) {
            return fail(error_out,
                        "could not open evidence directory " + component +
                            ": " + std::strerror(errno));
        }
        current = Fd(next);
    }
    *leaf_out = components.back();
    *parent_out = std::move(current);
    return true;
}

bool regular_file_at(const int parent_fd,
                     const std::string& leaf,
                     Fd* fd_out,
                     struct stat* stat_out,
                     std::string* error_out)
{
    const int fd = ::openat(parent_fd, leaf.c_str(), O_RDONLY | O_CLOEXEC |
                                                       O_NOFOLLOW);
    if (fd < 0) {
        return fail(error_out, "could not open artifact " + leaf + ": " +
                                   std::strerror(errno));
    }
    struct stat status {};
    if (::fstat(fd, &status) != 0 || !S_ISREG(status.st_mode)) {
        (void)::close(fd);
        return fail(error_out, "artifact is not a regular file: " + leaf);
    }
    if (fd_out) {
        fd_out->value = fd;
    } else {
        (void)::close(fd);
    }
    if (stat_out) {
        *stat_out = status;
    }
    return true;
}

bool optional_regular_file_at(const int parent_fd,
                              const std::string& leaf,
                              bool* present_out,
                              Fd* fd_out,
                              struct stat* stat_out,
                              std::string* error_out)
{
    if (!present_out || !fd_out) {
        return fail(error_out, "optional artifact destination is null");
    }
    *present_out = false;
    const int fd = ::openat(parent_fd, leaf.c_str(),
                            O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) {
        if (errno == ENOENT) {
            return true;
        }
        return fail(error_out, "could not inspect immutable artifact " + leaf +
                                   ": " + std::strerror(errno));
    }
    struct stat status {};
    if (::fstat(fd, &status) != 0 || !S_ISREG(status.st_mode)) {
        (void)::close(fd);
        return fail(error_out,
                    "immutable artifact is not a regular file: " + leaf);
    }
    fd_out->value = fd;
    if (stat_out) {
        *stat_out = status;
    }
    *present_out = true;
    return true;
}

bool same_file_snapshot(const struct stat& lhs, const struct stat& rhs)
{
    return lhs.st_dev == rhs.st_dev && lhs.st_ino == rhs.st_ino &&
        lhs.st_mode == rhs.st_mode && lhs.st_nlink == rhs.st_nlink &&
        lhs.st_size == rhs.st_size &&
        lhs.st_mtim.tv_sec == rhs.st_mtim.tv_sec &&
        lhs.st_mtim.tv_nsec == rhs.st_mtim.tv_nsec &&
        lhs.st_ctim.tv_sec == rhs.st_ctim.tv_sec &&
        lhs.st_ctim.tv_nsec == rhs.st_ctim.tv_nsec;
}

bool seek_file_start(const int fd,
                     const std::string& label,
                     std::string* error_out)
{
    return ::lseek(fd, 0, SEEK_SET) >= 0 ||
        fail(error_out, "could not seek " + label + ": " +
                            std::string(std::strerror(errno)));
}

// A max_bytes value of zero means that no unauthenticated universal media cap
// is imposed.  The read is still finite because the exact regular-file size is
// frozen with fstat and checked again after the streaming hash.  JSON callers
// always pass their separate fixed schema bounds.
bool hash_open_file_fd(const int fd,
                       const std::uint64_t max_bytes,
                       const std::string& label,
                       std::uint64_t* size_out,
                       std::string* sha_out,
                       struct stat* snapshot_out,
                       std::string* error_out)
{
    struct stat before {};
    if (::fstat(fd, &before) != 0 || !S_ISREG(before.st_mode) ||
        before.st_size < 0) {
        return fail(error_out, label + " is not a readable regular file");
    }
    const auto declared_size = static_cast<std::uint64_t>(before.st_size);
    if (max_bytes != 0 && declared_size > max_bytes) {
        return fail(error_out, label + " exceeds its authenticated/schema bound");
    }
    if (!seek_file_start(fd, label, error_out)) {
        return false;
    }
    checksum::StreamingSha256 hasher;
    std::array<std::uint8_t, 1024 * 1024> buffer{};
    std::uint64_t total = 0;
    for (;;) {
        const ssize_t count = ::read(fd, buffer.data(), buffer.size());
        if (count < 0) {
            if (errno == EINTR) {
                continue;
            }
            return fail(error_out, label + " read failed: " +
                                       std::string(std::strerror(errno)));
        }
        if (count == 0) {
            break;
        }
        const auto amount = static_cast<std::uint64_t>(count);
        if (total > std::numeric_limits<std::uint64_t>::max() - amount ||
            (max_bytes != 0 && total > max_bytes - amount)) {
            return fail(error_out, label + " grew beyond its bounded size");
        }
        hasher.update(buffer.data(), static_cast<std::size_t>(count));
        total += amount;
    }
    struct stat after {};
    if (::fstat(fd, &after) != 0 || !same_file_snapshot(before, after) ||
        total != declared_size) {
        return fail(error_out, label + " changed while it was being hashed");
    }
    if (size_out) {
        *size_out = total;
    }
    if (sha_out) {
        *sha_out = "sha256:" + hasher.final_hex();
    }
    if (snapshot_out) {
        *snapshot_out = after;
    }
    return true;
}

bool read_open_file_fd(const int fd,
                       const std::uint64_t max_bytes,
                       const std::string& label,
                       std::string* bytes_out,
                       std::string* error_out)
{
    if (!bytes_out) {
        return fail(error_out, label + " read destination is null");
    }
    struct stat before {};
    if (::fstat(fd, &before) != 0 || !S_ISREG(before.st_mode) ||
        before.st_size < 0 ||
        static_cast<std::uint64_t>(before.st_size) > max_bytes) {
        return fail(error_out, label + " size is outside its schema bound");
    }
    if (!seek_file_start(fd, label, error_out)) {
        return false;
    }
    bytes_out->clear();
    bytes_out->reserve(static_cast<std::size_t>(before.st_size));
    std::array<char, 64 * 1024> buffer{};
    for (;;) {
        const ssize_t count = ::read(fd, buffer.data(), buffer.size());
        if (count < 0) {
            if (errno == EINTR) {
                continue;
            }
            return fail(error_out, label + " read failed: " +
                                       std::string(std::strerror(errno)));
        }
        if (count == 0) {
            break;
        }
        if (bytes_out->size() > max_bytes - static_cast<std::size_t>(count)) {
            return fail(error_out, label + " grew beyond its schema bound");
        }
        bytes_out->append(buffer.data(), static_cast<std::size_t>(count));
    }
    struct stat after {};
    if (::fstat(fd, &after) != 0 || !same_file_snapshot(before, after) ||
        bytes_out->size() != static_cast<std::size_t>(before.st_size)) {
        return fail(error_out, label + " changed while it was being read");
    }
    return true;
}

template <typename Callback>
bool visit_bounded_lines_fd(const int fd,
                            const std::uint64_t max_bytes,
                            const std::size_t max_line_bytes,
                            const std::uint64_t max_lines,
                            const std::string& label,
                            Callback&& callback,
                            std::uint64_t* line_count_out,
                            std::string* error_out)
{
    struct stat before {};
    if (::fstat(fd, &before) != 0 || !S_ISREG(before.st_mode) ||
        before.st_size <= 0 ||
        static_cast<std::uint64_t>(before.st_size) > max_bytes ||
        !seek_file_start(fd, label, error_out)) {
        return fail(error_out, label + " size is outside its bounded range");
    }
    std::array<char, 64 * 1024> buffer{};
    std::string line;
    line.reserve(std::min<std::size_t>(max_line_bytes, 16 * 1024));
    std::uint64_t total = 0;
    std::uint64_t line_count = 0;
    for (;;) {
        const ssize_t count = ::read(fd, buffer.data(), buffer.size());
        if (count < 0) {
            if (errno == EINTR) {
                continue;
            }
            return fail(error_out, label + " read failed: " +
                                       std::string(std::strerror(errno)));
        }
        if (count == 0) {
            break;
        }
        const auto amount = static_cast<std::uint64_t>(count);
        if (amount > max_bytes || total > max_bytes - amount) {
            return fail(error_out, label + " grew beyond its bounded size");
        }
        total += amount;
        for (ssize_t index = 0; index < count; ++index) {
            const char current = buffer[static_cast<std::size_t>(index)];
            if (current == '\n') {
                if (line.empty() || line_count == max_lines) {
                    return fail(error_out,
                                label + " has an empty or excessive record");
                }
                ++line_count;
                if (!callback(line, line_count)) {
                    return false;
                }
                line.clear();
            } else {
                if (line.size() == max_line_bytes) {
                    return fail(error_out,
                                label + " record exceeds its bounded size");
                }
                line.push_back(current);
            }
        }
    }
    if (!line.empty()) {
        return fail(error_out, label + " must be newline terminated");
    }
    struct stat after {};
    if (line_count == 0 || ::fstat(fd, &after) != 0 ||
        !same_file_snapshot(before, after) ||
        total != static_cast<std::uint64_t>(before.st_size)) {
        return fail(error_out, label + " changed or has no records");
    }
    if (line_count_out) {
        *line_count_out = line_count;
    }
    return true;
}

bool parse_decimal_u64(const std::string_view value,
                       std::uint64_t* output)
{
    if (!output || value.empty()) {
        return false;
    }
    std::uint64_t result = 0;
    for (const unsigned char ch : value) {
        if (ch < '0' || ch > '9') {
            return false;
        }
        const std::uint64_t digit = ch - '0';
        if (result > (std::numeric_limits<std::uint64_t>::max() - digit) / 10) {
            return false;
        }
        result = result * 10 + digit;
    }
    *output = result;
    return true;
}

template <std::size_t N>
bool split_exact_csv(const std::string& line,
                     std::array<std::string_view, N>* fields)
{
    if (!fields) {
        return false;
    }
    std::size_t begin = 0;
    for (std::size_t index = 0; index < N; ++index) {
        const std::size_t comma = line.find(',', begin);
        if ((index + 1 < N && comma == std::string::npos) ||
            (index + 1 == N && comma != std::string::npos)) {
            return false;
        }
        const std::size_t end = comma == std::string::npos ? line.size() : comma;
        (*fields)[index] = std::string_view(line).substr(begin, end - begin);
        if ((*fields)[index].empty()) {
            return false;
        }
        begin = end + 1;
    }
    return true;
}

bool validate_metadata_csv(const int fd,
                           const SpatialRoiRecorderEvidenceBinding& binding,
                           const std::uint64_t expected_frames,
                           std::string* error_out)
{
    constexpr std::string_view kHeader =
        "recording_frame_id,roi_stream_frame_index,output_frame_index,"
        "camera_timestamp_ns,timestamp_sys_ns,source_gpu_id,assigned_gpu_id,"
        "assigned_shard_id";
    constexpr std::uint64_t kHeaderBytes = 256;
    constexpr std::uint64_t kRowBytes = 256;
    std::uint64_t previous_recording_frame_id = 0;
    const auto callback = [&](const std::string& line,
                              const std::uint64_t line_number) -> bool {
        if (line_number == 1) {
            return line == kHeader ||
                fail(error_out, "metadata CSV header is not the encoder schema");
        }
        std::array<std::string_view, 8> fields{};
        std::array<std::uint64_t, 8> values{};
        if (!split_exact_csv(line, &fields)) {
            return fail(error_out, "metadata CSV row has the wrong column count");
        }
        for (std::size_t index = 0; index < fields.size(); ++index) {
            if (!parse_decimal_u64(fields[index], &values[index])) {
                return fail(error_out,
                            "metadata CSV row contains a non-unsigned value");
            }
        }
        const std::uint64_t frame_index = line_number - 1;
        if (values[0] == 0 || values[0] <= previous_recording_frame_id ||
            values[1] != frame_index || values[2] != frame_index ||
            values[3] == 0 || values[4] == 0 ||
            values[5] != static_cast<std::uint64_t>(binding.source_gpu_id) ||
            values[6] != static_cast<std::uint64_t>(binding.assigned_gpu_id) ||
            values[7] != static_cast<std::uint64_t>(binding.assigned_shard_id)) {
            return fail(error_out,
                        "metadata CSV identity/cardinality does not match the stream");
        }
        previous_recording_frame_id = values[0];
        return true;
    };
    std::uint64_t line_count = 0;
    if (!visit_bounded_lines_fd(
            fd, kHeaderBytes + binding.max_frames_per_stream * kRowBytes,
            kRowBytes, binding.max_frames_per_stream + 1, "metadata CSV",
            callback, &line_count, error_out)) {
        return false;
    }
    return line_count == expected_frames + 1 ||
        fail(error_out, "metadata CSV row count does not match encoded frames");
}

bool validate_perf_csv(const int fd,
                       const SpatialRoiRecorderEvidenceBinding& binding,
                       const std::uint64_t expected_frames,
                       std::string* error_out)
{
    std::string actual;
    if (!read_open_file_fd(fd, kSpatialRoiRecorderManifestMaxFileBytes,
                           "perf CSV", &actual, error_out)) {
        return false;
    }
    const std::string expected =
        std::string("metric,value\n") +
        "schema_id," + kSpatialRoiRecorderPerfSchemaId +
        "\nschema_version," +
        std::to_string(kSpatialRoiRecorderTerminalCandidateSchemaVersion) +
        "\nstate," + kSpatialRoiRecorderPendingManifestState +
        "\ncertifying,false\nrequires_finalized_evidence_manifest,true"
        "\ncommit_marker,evidence_manifest"
        "\ncommit_marker_state,required_finalized\nlogical_stream_id," +
        binding.logical_stream_id + "\nframe_count," +
        std::to_string(expected_frames) + "\n";
    return actual == expected ||
        fail(error_out, "perf CSV is not the exact pending-manifest candidate");
}

json terminal_candidate_base(const char* schema_id,
                             const SpatialRoiRecorderEvidenceBinding& binding,
                             const std::uint64_t expected_frames)
{
    return {
        {"schema_id", schema_id},
        {"schema_version", kSpatialRoiRecorderTerminalCandidateSchemaVersion},
        {"state", kSpatialRoiRecorderPendingManifestState},
        {"certifying", false},
        {"requires_finalized_evidence_manifest", true},
        {"commit_marker", "evidence_manifest"},
        {"commit_marker_state", "required_finalized"},
        {"logical_stream_id", binding.logical_stream_id},
        {"frame_count", expected_frames},
    };
}

bool validate_terminal_json_sidecar(
    const int fd,
    const char* kind,
    const SpatialRoiRecorderEvidenceBinding& binding,
    const std::uint64_t expected_frames,
    std::string* error_out)
{
    std::string bytes;
    if (!read_open_file_fd(fd, kSpatialRoiRecorderManifestMaxFileBytes,
                           std::string(kind) + " sidecar", &bytes, error_out)) {
        return false;
    }
    json value;
    if (!strict_json_from_bytes(bytes, 200000, &value,
                                std::string(kind) + " sidecar", error_out) ||
        !value.is_object()) {
        return fail(error_out, std::string(kind) + " sidecar is not a bounded object");
    }
    json expected;
    if (std::string_view(kind) == "summary") {
        expected = terminal_candidate_base(kSpatialRoiRecorderSummarySchemaId,
                                           binding, expected_frames);
        expected["status"] = kSpatialRoiRecorderPendingManifestState;
    } else if (std::string_view(kind) == "status") {
        expected = terminal_candidate_base(kSpatialRoiRecorderStatusSchemaId,
                                           binding, expected_frames);
        expected["terminal"] = false;
    } else {
        return fail(error_out, "unknown terminal JSON sidecar kind");
    }
    return value == expected ||
        fail(error_out, std::string(kind) +
                            " sidecar is not the exact pending-manifest candidate");
}

bool validate_recorder_log(const int fd,
                           const SpatialRoiRecorderEvidenceBinding& binding,
                           const std::uint64_t expected_frames,
                           std::string* error_out)
{
    std::string actual;
    if (!read_open_file_fd(fd, kSpatialRoiRecorderManifestMaxFileBytes,
                           "recorder log", &actual, error_out)) {
        return false;
    }
    const std::string expected =
        std::string("schema_id=") + kSpatialRoiRecorderLogSchemaId +
        " schema_version=" +
        std::to_string(kSpatialRoiRecorderTerminalCandidateSchemaVersion) +
        " state=" + kSpatialRoiRecorderPendingManifestState +
        " certifying=false requires_finalized_evidence_manifest=true"
        " commit_marker=evidence_manifest"
        " commit_marker_state=required_finalized logical_stream_id=" +
        binding.logical_stream_id + " frame_count=" +
        std::to_string(expected_frames) + "\n";
    return actual == expected ||
        fail(error_out,
             "recorder log is not the exact pending-manifest candidate");
}

bool validate_transport_sidecar(
    const int fd,
    const SpatialRoiRecorderEvidenceBinding& binding,
    const std::uint64_t expected_frames,
    std::string* error_out)
{
    std::string bytes;
    if (!read_open_file_fd(fd, kSpatialRoiRecorderManifestMaxFileBytes,
                           "transport sidecar", &bytes, error_out)) {
        return false;
    }
    json actual;
    if (!strict_json_from_bytes(bytes, 200000, &actual,
                                "transport sidecar", error_out)) {
        return false;
    }
    const json expected = terminal_candidate_base(
        kSpatialRoiRecorderTransportSchemaId, binding, expected_frames);
    return actual == expected ||
        fail(error_out,
             "transport sidecar is not the exact pending-manifest candidate");
}

bool bytes_equal_at(const int parent_fd,
                    const std::string& leaf,
                    const std::string& bytes,
                    std::string* error_out)
{
    Fd existing;
    struct stat status {};
    if (!regular_file_at(parent_fd, leaf, &existing, &status, error_out)) {
        return false;
    }
    if (status.st_size < 0 || static_cast<std::size_t>(status.st_size) != bytes.size()) {
        return false;
    }
    std::size_t offset = 0;
    std::array<char, 64 * 1024> buffer{};
    while (offset < bytes.size()) {
        const std::size_t remaining = bytes.size() - offset;
        const ssize_t count = ::read(existing.value, buffer.data(),
                                     std::min(buffer.size(), remaining));
        if (count < 0) {
            if (errno == EINTR) {
                continue;
            }
            return fail(error_out, "existing artifact comparison failed: " +
                                       std::string(std::strerror(errno)));
        }
        if (count == 0 ||
            std::memcmp(buffer.data(), bytes.data() + offset,
                        static_cast<std::size_t>(count)) != 0) {
            return false;
        }
        offset += static_cast<std::size_t>(count);
    }
    struct stat after {};
    struct stat current_binding {};
    if (::fstat(existing.value, &after) != 0 ||
        !same_file_snapshot(status, after) ||
        ::fstatat(parent_fd, leaf.c_str(), &current_binding,
                  AT_SYMLINK_NOFOLLOW) != 0 ||
        !S_ISREG(current_binding.st_mode) ||
        current_binding.st_dev != after.st_dev ||
        current_binding.st_ino != after.st_ino) {
        return fail(error_out,
                    "existing immutable artifact changed binding during comparison: " +
                        leaf);
    }
    return true;
}

bool stage_bytes(const int parent_fd,
                 const std::string& bytes,
                 const std::size_t max_bytes,
                 Fd* staged_fd_out,
                 std::string* error_out)
{
    if (!staged_fd_out || bytes.size() > max_bytes) {
        return fail(error_out, "staged evidence bytes exceed the bounded size");
    }
    // O_TMPFILE gives publication an inode-bound fd with no mutable staging
    // pathname.  linkat(AT_EMPTY_PATH) below publishes this exact inode.
    const int fd = ::openat(parent_fd, ".", O_TMPFILE | O_RDWR | O_CLOEXEC, 0644);
    if (fd < 0) {
        return fail(error_out,
                    std::string("could not create unnamed evidence staging file: ") +
                        std::strerror(errno));
    }
    bool ok = write_all_fd(fd, bytes, error_out);
    if (ok && ::fsync(fd) != 0) {
        ok = fail(error_out, "evidence staging fsync failed: " +
                                  std::string(std::strerror(errno)));
    }
    if (!ok) {
        (void)::close(fd);
        return false;
    }
    staged_fd_out->value = fd;
    return true;
}

bool fsync_directory(const int fd, std::string* error_out)
{
    return ::fsync(fd) == 0 ||
        fail(error_out, "evidence directory fsync failed: " +
                           std::string(std::strerror(errno)));
}

bool publish_staged_no_replace(const int parent_fd,
                               const int staging_fd,
                               const std::string& leaf,
                               const std::string* existing_bytes,
                               std::string* error_out)
{
    bool published =
        ::linkat(staging_fd, "", parent_fd, leaf.c_str(), AT_EMPTY_PATH) == 0;
    int publication_error = published ? 0 : errno;
    if (!published &&
        (publication_error == ENOENT || publication_error == EINVAL ||
         publication_error == EPERM || publication_error == EOPNOTSUPP)) {
        // Some filesystems/kernels reject AT_EMPTY_PATH for an otherwise
        // linkable O_TMPFILE inode. /proc/self/fd names the retained open-file
        // description, not a caller-controlled staging pathname, so this
        // fallback still publishes the exact inode with no replacement race.
        std::array<char, 64> fd_path{};
        const int length = std::snprintf(fd_path.data(), fd_path.size(),
                                         "/proc/self/fd/%d", staging_fd);
        if (length > 0 && static_cast<std::size_t>(length) < fd_path.size()) {
            published = ::linkat(AT_FDCWD, fd_path.data(), parent_fd,
                                 leaf.c_str(), AT_SYMLINK_FOLLOW) == 0;
            publication_error = published ? 0 : errno;
        }
    }
    if (!published && publication_error != EEXIST) {
        return fail(error_out,
                    "fd-bound no-replace evidence publication failed: " +
                        std::string(std::strerror(publication_error)));
    }
    if (!published) {
        if (!existing_bytes) {
            return fail(error_out,
                        "immutable evidence artifact already exists: " + leaf);
        }
        std::string compare_error;
        if (!bytes_equal_at(parent_fd, leaf, *existing_bytes, &compare_error)) {
            if (!compare_error.empty()) {
                return fail(error_out, compare_error);
            }
            return fail(error_out,
                        "immutable evidence artifact already exists with different bytes: " +
                            leaf);
        }
    }
    return fsync_directory(parent_fd, error_out);
}

bool publish_bytes(const int root_fd,
                   const std::string& relative_path,
                   const std::string& bytes,
                   const std::size_t max_bytes,
                   std::string* error_out)
{
    Fd parent;
    std::string leaf;
    if (!open_parent_dir(root_fd, relative_path, true, &parent, &leaf, error_out)) {
        return false;
    }
    Fd staging;
    if (!stage_bytes(parent.value, bytes, max_bytes, &staging, error_out)) {
        return false;
    }
    return publish_staged_no_replace(parent.value, staging.value, leaf, &bytes,
                                     error_out);
}

struct OpenArtifact {
    std::string kind;
    std::string relative_path;
    std::unique_ptr<SpatialRoiRecorderArtifactFile> file;
    struct stat snapshot {};
    json reference = json::object();
};

bool checked_add_bytes(std::uint64_t value,
                       std::uint64_t* total,
                       std::string* error_out)
{
    if (!total || *total > std::numeric_limits<std::uint64_t>::max() - value) {
        return fail(error_out, "aggregate evidence byte count overflowed");
    }
    *total += value;
    return true;
}

bool validate_aggregate_evidence_budget(
    const json& artifact_receipts,
    const json& evidence_reference,
    const std::uint64_t manifest_bytes,
    const SpatialRoiRecorderEvidenceBinding& binding,
    std::string* error_out)
{
    if (!artifact_receipts.is_object() || !evidence_reference.is_object()) {
        return fail(error_out,
                    "aggregate evidence budget lacks artifact receipts");
    }
    std::uint64_t total = 0;
    for (const auto& [kind, reference] : artifact_receipts.items()) {
        if (kind == "video") {
            continue;
        }
        std::uint64_t size = 0;
        if (!reference.is_object() ||
            !read_u64(reference, "size_bytes", &size,
                      "artifact_receipts." + kind, error_out) ||
            !checked_add_bytes(size, &total, error_out)) {
            return false;
        }
    }
    std::uint64_t evidence_bytes = 0;
    if (!read_u64(evidence_reference, "size_bytes", &evidence_bytes,
                  "evidence_reference", error_out) ||
        !checked_add_bytes(evidence_bytes, &total, error_out) ||
        !checked_add_bytes(manifest_bytes, &total, error_out)) {
        return false;
    }
    if (total > binding.max_evidence_bytes_per_stream) {
        return fail(error_out,
                    "aggregate sidecar/evidence bytes exceed the authenticated per-stream budget");
    }
    return true;
}

std::uint64_t artifact_schema_size_bound(
    const std::string& kind,
    const SpatialRoiRecorderEvidenceBinding& binding)
{
    constexpr std::uint64_t kCsvHeaderAllowance = 256;
    constexpr std::uint64_t kMetadataRowMaxBytes = 256;
    constexpr std::uint64_t kPerfRowMaxBytes = 512;
    if (kind == "video") {
        return binding.max_media_bytes_per_stream;
    }
    if (kind == "metadata") {
        return kCsvHeaderAllowance +
            binding.max_frames_per_stream * kMetadataRowMaxBytes;
    }
    if (kind == "perf") {
        return kCsvHeaderAllowance +
            binding.max_frames_per_stream * kPerfRowMaxBytes;
    }
    if (kind == "keyframes") {
        return kKeyframeSummaryMaxBytes;
    }
    if (kind == "summary" || kind == "status" ||
        kind == "video_sanity" || kind == "finalization") {
        return kSpatialRoiRecorderManifestMaxFileBytes;
    }
    if (kind == "recorder_log" || kind == "transport_sidecar") {
        return binding.max_evidence_bytes_per_stream;
    }
    // The finalize-request kind set is closed. Returning zero here makes an
    // internal extension fail before reading rather than silently restoring
    // the former unbounded-to-EOF behavior.
    return 0;
}

bool open_and_hash_finalize_artifacts(
    const SpatialRoiRecorderArtifactRoot& artifact_root,
    const NormalizedFinalizeRequest& request,
    const SpatialRoiRecorderEvidenceBinding& binding,
    std::vector<OpenArtifact>* artifacts_out,
    std::string* error_out)
{
    if (!artifacts_out) {
        return fail(error_out, "opened artifact destination is null");
    }
    artifacts_out->clear();
    std::set<std::pair<dev_t, ino_t>> inodes;
    for (const auto& [kind, path] : request.artifacts) {
        OpenArtifact artifact;
        artifact.kind = kind;
        artifact.relative_path = path;
        if (!open_existing_artifact(
                artifact_root, path, &artifact.file, error_out)) {
            return false;
        }
        std::uint64_t size = 0;
        std::string sha;
        const std::uint64_t size_bound =
            artifact_schema_size_bound(kind, binding);
        if (size_bound == 0 ||
            !hash_open_file_fd(artifact.file->borrowed_fd(),
                               size_bound, path, &size,
                               &sha, &artifact.snapshot, error_out)) {
            return false;
        }
        if ((kind == "video" || kind == "metadata" || kind == "keyframes" ||
             kind == "perf" || kind == "summary" || kind == "status" ||
             kind == "video_sanity" || kind == "finalization" ||
             kind == "transport_sidecar") &&
            size == 0) {
            return fail(error_out,
                        "required finalization artifact is empty: " + kind);
        }
        if (!inodes.insert({artifact.snapshot.st_dev,
                            artifact.snapshot.st_ino}).second) {
            return fail(error_out,
                        "contract artifacts resolve to the same regular-file inode");
        }
        artifact.reference = {
            {"relative_path", path}, {"size_bytes", size}, {"sha256", sha}};
        artifacts_out->push_back(std::move(artifact));
    }
    return true;
}

OpenArtifact* find_open_artifact(std::vector<OpenArtifact>* artifacts,
                                 const std::string& kind)
{
    if (!artifacts) {
        return nullptr;
    }
    const auto found = std::find_if(
        artifacts->begin(), artifacts->end(),
        [&](const OpenArtifact& value) { return value.kind == kind; });
    return found == artifacts->end() ? nullptr : &*found;
}

bool artifacts_unchanged(const std::vector<OpenArtifact>& artifacts,
                         std::string* error_out)
{
    for (const auto& artifact : artifacts) {
        struct stat after {};
        if (!artifact.file ||
            ::fstat(artifact.file->borrowed_fd(), &after) != 0 ||
            !same_file_snapshot(artifact.snapshot, after) ||
            !artifact.file->VerifyCurrentBinding(error_out)) {
            return fail(error_out,
                        "artifact changed during finalization: " + artifact.kind);
        }
    }
    return true;
}

bool validate_container_finalization_sidecar(
    const int sidecar_fd,
    const std::string& artifact_root,
    const std::string& relative_path,
    const std::string& expected_video_relative_path,
    const std::uint64_t actual_video_size,
    const std::uint64_t expected_recording_fps,
    std::string* error_out)
{
    std::string bytes;
    if (!read_open_file_fd(sidecar_fd,
                           kSpatialRoiRecorderManifestMaxFileBytes,
                           "container finalization sidecar", &bytes,
                           error_out)) {
        return false;
    }
    json document;
    if (!strict_json_from_bytes(bytes, 200000, &document,
                                "container finalization sidecar", error_out)) {
        return false;
    }
    static constexpr std::array<std::string_view, 10> top_keys = {
        "schema_id", "schema_version", "generated_at_utc", "status", "terminal",
        "video_path", "sidecar_path", "recording_fps", "container",
        "quicktime_full_frame_rate_playback_intent"};
    if (!exact_keys(document, top_keys, "container_finalization", error_out) ||
        !read_const_string(document, "schema_id", "orange.video_container_finalization",
                           "container_finalization", error_out) ||
        !read_const_int(document, "schema_version", 1, "container_finalization",
                        error_out) ||
        !read_string(document, "generated_at_utc", nullptr, "container_finalization",
                     error_out, 128) ||
        !read_const_string(document, "status", "complete", "container_finalization",
                           error_out) ||
        !read_const_bool(document, "terminal", true, "container_finalization",
                         error_out) ||
        !read_string(document, "video_path", nullptr, "container_finalization",
                     error_out, kSpatialRoiRecorderEvidenceMaxPathBytes) ||
        !read_string(document, "sidecar_path", nullptr, "container_finalization",
                     error_out, kSpatialRoiRecorderEvidenceMaxPathBytes)) {
        return false;
    }
    const std::string expected_video_absolute =
        (std::filesystem::path(artifact_root) / expected_video_relative_path)
            .lexically_normal().generic_string();
    const std::string expected_sidecar_absolute =
        (std::filesystem::path(artifact_root) / relative_path)
            .lexically_normal().generic_string();
    const std::string sidecar_video_path = document.at("video_path").get<std::string>();
    const std::string sidecar_path = document.at("sidecar_path").get<std::string>();
    if (sidecar_video_path != expected_video_relative_path &&
        sidecar_video_path != expected_video_absolute) {
        return fail(error_out,
                    "container finalization sidecar does not bind the manifest video");
    }
    if (sidecar_path != relative_path && sidecar_path != expected_sidecar_absolute) {
        return fail(error_out,
                    "container finalization sidecar path does not match its manifest path");
    }
    std::uint64_t recording_fps = 0;
    if (!read_u64(document, "recording_fps", &recording_fps,
                  "container_finalization", error_out) ||
        recording_fps != expected_recording_fps) {
        return fail(error_out,
                    "container_finalization.recording_fps does not match contract");
    }

    const json& container = document.at("container");
    static constexpr std::array<std::string_view, 12> container_keys = {
        "header_written", "trailer_attempted", "trailer_written",
        "output_close_attempted", "output_closed", "finalized",
        "trailer_error_code", "trailer_error", "output_close_error_code",
        "output_close_error", "file_size_bytes", "file_size_error"};
    if (!exact_keys(container, container_keys, "container_finalization.container",
                    error_out)) {
        return false;
    }
    for (const char* key : {"header_written", "trailer_attempted", "trailer_written",
                            "output_close_attempted", "output_closed", "finalized"}) {
        if (!read_const_bool(container, key, true, "container_finalization.container",
                             error_out)) {
            return false;
        }
    }
    for (const char* key : {"trailer_error_code", "trailer_error",
                            "output_close_error_code", "output_close_error",
                            "file_size_error"}) {
        if (!container.at(key).is_null()) {
            return fail(error_out, std::string("container_finalization.container.") +
                                       key + " must be null for complete output");
        }
    }
    std::uint64_t file_size = 0;
    if (!read_u64(container, "file_size_bytes", &file_size,
                  "container_finalization.container", error_out) ||
        file_size == 0 || file_size != actual_video_size) {
        return fail(error_out,
                    "container finalization video size does not match the actual file");
    }

    const json& playback = document.at("quicktime_full_frame_rate_playback_intent");
    static constexpr std::array<std::string_view, 7> playback_keys = {
        "key", "requested_value", "required_data_type", "quicktime_data_atom_type",
        "patch_attempted", "patch_applied", "error"};
    if (!exact_keys(playback, playback_keys,
                    "container_finalization.quicktime_full_frame_rate_playback_intent",
                    error_out) ||
        !read_const_string(playback, "key",
                           "com.apple.quicktime.full-frame-rate-playback-intent",
                           "container_finalization.quicktime_full_frame_rate_playback_intent",
                           error_out) ||
        !read_const_string(playback, "required_data_type", "UInt8",
                           "container_finalization.quicktime_full_frame_rate_playback_intent",
                           error_out) ||
        !read_const_bool(playback, "patch_attempted", true,
                         "container_finalization.quicktime_full_frame_rate_playback_intent",
                         error_out) ||
        !read_const_bool(playback, "patch_applied", true,
                         "container_finalization.quicktime_full_frame_rate_playback_intent",
                         error_out) ||
        !playback.at("error").is_null()) {
        return false;
    }
    std::uint64_t requested_value = 0;
    std::uint64_t atom_type = 0;
    if (!read_u64(playback, "requested_value", &requested_value,
                  "container_finalization.quicktime_full_frame_rate_playback_intent",
                  error_out) || requested_value != 1 ||
        !read_u64(playback, "quicktime_data_atom_type", &atom_type,
                  "container_finalization.quicktime_full_frame_rate_playback_intent",
                  error_out) || atom_type != 22) {
        return fail(error_out,
                    "container finalization sidecar playback intent is invalid");
    }
    return true;
}


bool validate_closed_keyframe_sidecar(const int fd,
                                      const std::uint64_t expected_frames,
                                      const std::uint64_t expected_fps,
                                      const std::uint64_t expected_gop_length,
                                      std::string* error_out)
{
    std::string bytes;
    if (!read_open_file_fd(fd, kKeyframeSummaryMaxBytes,
                           "keyframe summary sidecar", &bytes, error_out)) {
        return false;
    }
    json document;
    if (!strict_json_from_bytes(bytes, 256, &document,
                                "keyframe summary sidecar", error_out)) {
        return false;
    }
    static constexpr std::array<std::string_view, 8> top_keys = {
        "schema_id", "schema_version", "terminal", "codec", "fps",
        "total_frames", "frame_index_sequence", "keyframe_policy"};
    if (!exact_keys(document, top_keys, "keyframe_summary", error_out) ||
        !read_const_string(document, "schema_id",
                           "orange.spatial_roi_keyframe_summary",
                           "keyframe_summary", error_out) ||
        !read_const_int(document, "schema_version", 1, "keyframe_summary",
                        error_out) ||
        !read_const_bool(document, "terminal", true, "keyframe_summary",
                         error_out) ||
        !read_const_string(document, "codec", "hevc", "keyframe_summary",
                           error_out)) {
        return false;
    }
    std::uint64_t fps = 0;
    std::uint64_t total_frames = 0;
    if (!read_u64(document, "fps", &fps, "keyframe_summary", error_out) ||
        !read_u64(document, "total_frames", &total_frames,
                  "keyframe_summary", error_out) ||
        fps != expected_fps || total_frames != expected_frames ||
        expected_frames == 0) {
        return fail(error_out,
                    "keyframe summary cadence/frame count is invalid");
    }

    const json& sequence = document.at("frame_index_sequence");
    static constexpr std::array<std::string_view, 3> sequence_keys = {
        "first", "last", "zero_based_contiguous"};
    std::uint64_t first = 0;
    std::uint64_t last = 0;
    if (!exact_keys(sequence, sequence_keys,
                    "keyframe_summary.frame_index_sequence", error_out) ||
        !read_u64(sequence, "first", &first,
                  "keyframe_summary.frame_index_sequence", error_out) ||
        !read_u64(sequence, "last", &last,
                  "keyframe_summary.frame_index_sequence", error_out) ||
        !read_const_bool(sequence, "zero_based_contiguous", true,
                         "keyframe_summary.frame_index_sequence", error_out) ||
        first != 0 || last != expected_frames - 1) {
        return fail(error_out,
                    "keyframe summary frame-index sequence is invalid");
    }

    const json& policy = document.at("keyframe_policy");
    static constexpr std::array<std::string_view, 4> policy_keys = {
        "name", "keyframe_frames", "non_keyframe_frames", "satisfied"};
    std::string policy_name;
    std::uint64_t keyframe_frames = 0;
    std::uint64_t non_keyframe_frames = 0;
    if (!exact_keys(policy, policy_keys, "keyframe_summary.keyframe_policy",
                    error_out) ||
        !read_string(policy, "name", &policy_name,
                     "keyframe_summary.keyframe_policy", error_out) ||
        !read_u64(policy, "keyframe_frames", &keyframe_frames,
                  "keyframe_summary.keyframe_policy", error_out) ||
        !read_u64(policy, "non_keyframe_frames", &non_keyframe_frames,
                  "keyframe_summary.keyframe_policy", error_out) ||
        !read_const_bool(policy, "satisfied", true,
                         "keyframe_summary.keyframe_policy", error_out) ||
        expected_gop_length == 0 ||
        policy_name != keyframe_policy_name(expected_gop_length) ||
        keyframe_frames !=
            expected_keyframe_count(expected_frames, expected_gop_length) ||
        non_keyframe_frames != expected_frames - keyframe_frames) {
        return fail(error_out,
                    "keyframe summary configured-GOP policy is not satisfied");
    }
    return true;
}

bool finite_number(const json& value, double* output)
{
    if (!value.is_number()) {
        return false;
    }
    const double parsed = value.get<double>();
    if (!std::isfinite(parsed)) {
        return false;
    }
    if (output) *output = parsed;
    return true;
}

bool validate_closed_video_sanity_sidecar(
    const int fd,
    const SpatialRoiRecorderEvidenceBinding& binding,
    const std::string& video_relative_path,
    const std::uint64_t video_size,
    const std::string& video_sha256,
    const std::uint64_t video_device,
    const std::uint64_t video_inode,
    const std::uint64_t expected_frames,
    const SpatialRoiRecorderArtifactIdentity& video_root_identity,
    const SpatialRoiRecorderVideoSanityResult* verified_probe,
    std::string* error_out)
{
    std::string bytes;
    if (!read_open_file_fd(fd, kSpatialRoiRecorderManifestMaxFileBytes,
                           "video sanity sidecar", &bytes, error_out)) {
        return false;
    }
    json document;
    if (!strict_json_from_bytes(bytes, 1000000, &document,
                                "video sanity sidecar", error_out)) {
        return false;
    }
    static constexpr std::array<std::string_view, 32> keys = {
        "schema_id", "schema_version", "state", "certifying",
        "requires_finalized_evidence_manifest", "commit_marker",
        "commit_marker_state", "logical_stream_id", "frame_count",
        "video_path", "video_size_bytes", "video_sha256", "video_device",
        "video_inode", "content_checked", "content_valid", "status", "width",
        "height", "nb_frames", "container", "container_name", "codec",
        "decoder", "timeline", "pixel_semantics", "sampled_frame_count",
        "mean_luma", "max_stddev", "max_black_fraction_lt8", "thresholds",
        "sampled_frames"};
    if (!exact_keys(document, keys, "video_sanity", error_out) ||
        !read_const_string(document, "schema_id",
                           kSpatialRoiRecorderVideoSanitySchemaId,
                           "video_sanity", error_out) ||
        !read_const_int(document, "schema_version",
                        kSpatialRoiRecorderTerminalCandidateSchemaVersion,
                        "video_sanity", error_out) ||
        !read_const_string(document, "state",
                           kSpatialRoiRecorderPendingManifestState,
                           "video_sanity", error_out) ||
        !read_const_bool(document, "certifying", false, "video_sanity",
                         error_out) ||
        !read_const_bool(document, "requires_finalized_evidence_manifest", true,
                         "video_sanity", error_out) ||
        !read_const_string(document, "commit_marker", "evidence_manifest",
                           "video_sanity", error_out) ||
        !read_const_string(document, "commit_marker_state",
                           "required_finalized", "video_sanity", error_out) ||
        !read_const_bool(document, "content_checked", true, "video_sanity",
                         error_out) ||
        !read_const_bool(document, "content_valid", true, "video_sanity",
                         error_out) ||
        !read_const_string(document, "status", "pass", "video_sanity",
                           error_out)) {
        return false;
    }
    std::string logical_stream_id;
    std::string video_path;
    std::string sidecar_sha256;
    std::uint64_t sidecar_frames = 0;
    std::uint64_t sidecar_video_size = 0;
    std::uint64_t sidecar_video_device = 0;
    std::uint64_t sidecar_video_inode = 0;
    if (!read_string(document, "logical_stream_id", &logical_stream_id,
                     "video_sanity", error_out) ||
        !read_u64(document, "frame_count", &sidecar_frames, "video_sanity",
                  error_out) ||
        !read_u64(document, "video_size_bytes", &sidecar_video_size,
                  "video_sanity", error_out) ||
        !read_string(document, "video_sha256", &sidecar_sha256,
                     "video_sanity", error_out, 71) ||
        !read_u64(document, "video_device", &sidecar_video_device,
                  "video_sanity", error_out) ||
        !read_u64(document, "video_inode", &sidecar_video_inode,
                  "video_sanity", error_out) ||
        logical_stream_id != binding.logical_stream_id ||
        sidecar_frames != expected_frames || sidecar_video_size != video_size ||
        !is_sha256(sidecar_sha256) || sidecar_sha256 != video_sha256 ||
        sidecar_video_inode == 0 ||
        (verified_probe != nullptr &&
         (sidecar_video_device != video_device ||
          sidecar_video_inode != video_inode))) {
        return fail(error_out,
                    "video sanity identity does not bind the retained video receipt");
    }
    if (verified_probe != nullptr &&
        (verified_probe->artifact_root_identity() != video_root_identity ||
         verified_probe->video_identity().device != video_device ||
         verified_probe->video_identity().inode != video_inode ||
         verified_probe->relative_path() != video_relative_path ||
         verified_probe->size_bytes() != video_size ||
         verified_probe->sha256() != video_sha256 ||
         verified_probe->frame_count() != expected_frames)) {
        return fail(error_out,
                    "video sanity decoder capability does not bind the retained video");
    }
    if (!read_string(document, "video_path", &video_path, "video_sanity",
                     error_out, kSpatialRoiRecorderEvidenceMaxPathBytes)) {
        return false;
    }
    if (video_path != video_relative_path) {
        return fail(error_out,
                    "video sanity sidecar does not bind the contract video");
    }
    std::uint64_t width = 0;
    std::uint64_t height = 0;
    std::uint64_t frames = 0;
    std::uint64_t sampled_count = 0;
    if (!read_u64(document, "width", &width, "video_sanity", error_out) ||
        !read_u64(document, "height", &height, "video_sanity", error_out) ||
        !read_u64(document, "nb_frames", &frames, "video_sanity", error_out) ||
        !read_u64(document, "sampled_frame_count", &sampled_count,
                  "video_sanity", error_out) ||
        width != binding.geometry_identity.at("encoded_raster").at("width") ||
        height != binding.geometry_identity.at("encoded_raster").at("height") ||
        frames != expected_frames || sampled_count == 0 || sampled_count > 5 ||
        !document.at("sampled_frames").is_array() ||
        document.at("sampled_frames").size() != sampled_count ||
        (verified_probe != nullptr &&
         (verified_probe->width() != width ||
          verified_probe->height() != height))) {
        return fail(error_out,
                    "video sanity dimensions/frame/sample counts do not match evidence");
    }
    std::vector<std::uint64_t> expected_sample_indices{0};
    if (expected_frames > 2) {
        expected_sample_indices.push_back(expected_frames / 2);
    }
    if (expected_frames > 1) {
        expected_sample_indices.push_back(expected_frames - 1);
    }
    if (sampled_count != expected_sample_indices.size() ||
        (verified_probe != nullptr &&
         verified_probe->samples().size() != sampled_count)) {
        return fail(error_out,
                    "video sanity does not use the deterministic sample schedule");
    }
    double mean_luma = 0;
    double max_stddev = 0;
    double max_black = 0;
    if (!finite_number(document.at("mean_luma"), &mean_luma) ||
        !finite_number(document.at("max_stddev"), &max_stddev) ||
        !finite_number(document.at("max_black_fraction_lt8"), &max_black) ||
        mean_luma < 0 || mean_luma > 255 || max_stddev < 5.0 ||
        max_black < 0 || max_black >= 0.98) {
        return fail(error_out, "video sanity aggregate measurements are invalid");
    }
    static constexpr std::array<std::string_view, 2> threshold_keys = {
        "max_black_fraction_lt8", "min_max_stddev"};
    const json& thresholds = document.at("thresholds");
    double black_threshold = 0;
    double stddev_threshold = 0;
    if (!exact_keys(thresholds, threshold_keys, "video_sanity.thresholds",
                    error_out) ||
        !finite_number(thresholds.at("max_black_fraction_lt8"),
                       &black_threshold) ||
        !finite_number(thresholds.at("min_max_stddev"), &stddev_threshold) ||
        black_threshold != 0.98 || stddev_threshold != 5.0) {
        return fail(error_out, "video sanity thresholds are not the closed v1 values");
    }
    if (height != 0 && width > std::numeric_limits<std::uint64_t>::max() / height) {
        return fail(error_out, "video sanity raster byte count overflowed");
    }
    const std::uint64_t pixels = width * height;
    static constexpr std::array<std::string_view, 8> sample_keys = {
        "requested_frame_index", "mean", "stddev", "min", "max",
        "black_fraction_lt8", "white_fraction_gt247", "decoded_bytes"};
    std::uint64_t previous_index = 0;
    double sample_mean_sum = 0.0;
    double observed_max_stddev = 0.0;
    double observed_max_black = 0.0;
    bool first = true;
    std::size_t sample_offset = 0;
    for (const auto& sample : document.at("sampled_frames")) {
        if (!exact_keys(sample, sample_keys, "video_sanity.sample", error_out)) {
            return false;
        }
        std::uint64_t index = 0;
        std::uint64_t min_value = 0;
        std::uint64_t max_value = 0;
        std::uint64_t decoded_bytes = 0;
        double sample_mean = 0;
        double sample_stddev = 0;
        double black = 0;
        double white = 0;
        if (!read_u64(sample, "requested_frame_index", &index,
                      "video_sanity.sample", error_out) ||
            !read_u64(sample, "min", &min_value, "video_sanity.sample",
                      error_out) ||
            !read_u64(sample, "max", &max_value, "video_sanity.sample",
                      error_out) ||
            !read_u64(sample, "decoded_bytes", &decoded_bytes,
                      "video_sanity.sample", error_out) ||
            !finite_number(sample.at("mean"), &sample_mean) ||
            !finite_number(sample.at("stddev"), &sample_stddev) ||
            !finite_number(sample.at("black_fraction_lt8"), &black) ||
            !finite_number(sample.at("white_fraction_gt247"), &white) ||
            index >= expected_frames ||
            index != expected_sample_indices.at(sample_offset) ||
            (!first && index <= previous_index) ||
            min_value > max_value || max_value > 255 || decoded_bytes != pixels ||
            sample_mean < 0 || sample_mean > 255 || sample_stddev < 0 ||
            sample_mean < static_cast<double>(min_value) ||
            sample_mean > static_cast<double>(max_value) ||
            sample_stddev >
                (static_cast<double>(max_value) -
                 static_cast<double>(min_value)) /
                        2.0 +
                    1e-12 ||
            black < 0 || black > 1 || white < 0 || white > 1 ||
            black + white > 1.0 + 1e-12 ||
            (min_value >= 8 && black != 0.0) ||
            (max_value < 8 && black != 1.0) ||
            (max_value <= 247 && white != 0.0) ||
            (min_value > 247 && white != 1.0)) {
            return fail(error_out,
                        "video sanity sampled-frame evidence is invalid");
        }
        if (verified_probe != nullptr) {
            const auto& probed = verified_probe->samples().at(sample_offset);
            if (probed.requested_frame_index != index ||
                probed.mean != sample_mean ||
                probed.stddev != sample_stddev || probed.min != min_value ||
                probed.max != max_value ||
                probed.black_fraction_lt8 != black ||
                probed.white_fraction_gt247 != white ||
                probed.decoded_bytes != decoded_bytes) {
                return fail(error_out,
                            "video sanity sidecar samples do not match the decoder capability");
            }
        }
        previous_index = index;
        first = false;
        sample_mean_sum += sample_mean;
        observed_max_stddev = std::max(observed_max_stddev, sample_stddev);
        observed_max_black = std::max(observed_max_black, black);
        ++sample_offset;
    }
    const double observed_mean = sample_mean_sum / static_cast<double>(sampled_count);
    const auto approximately_equal = [](const double lhs, const double rhs) {
        const double scale = std::max({1.0, std::fabs(lhs), std::fabs(rhs)});
        return std::fabs(lhs - rhs) <= scale * 1e-12;
    };
    if (!approximately_equal(mean_luma, observed_mean) ||
        !approximately_equal(max_stddev, observed_max_stddev) ||
        !approximately_equal(max_black, observed_max_black)) {
        return fail(error_out,
                    "video sanity aggregate measurements do not match samples");
    }

    const json& container = document.at("container");
    static constexpr std::array<std::string_view, 2> container_keys = {
        "size", "duration"};
    std::string container_size;
    std::string duration_text;
    if (!exact_keys(container, container_keys, "video_sanity.container",
                    error_out) ||
        !read_string(container, "size", &container_size,
                     "video_sanity.container", error_out, 32) ||
        !read_string(container, "duration", &duration_text,
                     "video_sanity.container", error_out, 64) ||
        container_size != std::to_string(video_size) ||
        (verified_probe != nullptr &&
         verified_probe->duration_seconds() != duration_text)) {
        return fail(error_out,
                    "video sanity container size does not match actual video");
    }

    errno = 0;
    char* duration_end = nullptr;
    const double duration = std::strtod(duration_text.c_str(), &duration_end);
    const std::uint64_t expected_fps =
        binding.encode_profile.at("frame_rate").get<std::uint64_t>();
    if (errno != 0 || duration_end == duration_text.c_str() ||
        *duration_end != '\0' || !std::isfinite(duration) || duration <= 0.0) {
        return fail(error_out,
                    "video sanity duration does not match frame count and cadence");
    }

    std::string container_name;
    std::string codec;
    std::string decoder;
    if (!read_string(document, "container_name", &container_name,
                     "video_sanity", error_out, 256) ||
        !read_string(document, "codec", &codec, "video_sanity", error_out,
                     32) ||
        !read_string(document, "decoder", &decoder, "video_sanity", error_out,
                     256) ||
        container_name != "mov,mp4,m4a,3gp,3g2,mj2" || codec != "hevc" ||
        decoder.rfind("hevc@", 0) != 0 ||
        (verified_probe != nullptr &&
         (verified_probe->container() != container_name ||
          verified_probe->codec() != codec ||
          verified_probe->decoder() != decoder))) {
        return fail(error_out,
                    "video sanity codec/container/decoder evidence is invalid");
    }

    const json& pixel_semantics = document.at("pixel_semantics");
    static constexpr std::array<std::string_view, 4> pixel_keys = {
        "pixel_format", "color_range", "bit_depth", "chroma_subsampling"};
    std::string pixel_format;
    std::string color_range;
    std::string chroma_subsampling;
    std::uint64_t bit_depth = 0;
    if (!exact_keys(pixel_semantics, pixel_keys,
                    "video_sanity.pixel_semantics", error_out) ||
        !read_string(pixel_semantics, "pixel_format", &pixel_format,
                     "video_sanity.pixel_semantics", error_out, 32) ||
        !read_string(pixel_semantics, "color_range", &color_range,
                     "video_sanity.pixel_semantics", error_out, 32) ||
        !read_u64(pixel_semantics, "bit_depth", &bit_depth,
                  "video_sanity.pixel_semantics", error_out) ||
        !read_string(pixel_semantics, "chroma_subsampling", &chroma_subsampling,
                     "video_sanity.pixel_semantics", error_out, 32) ||
        (pixel_format != "yuv420p" && pixel_format != "yuvj420p" &&
         pixel_format != "nv12") ||
        color_range != "pc" || bit_depth != 8 ||
        chroma_subsampling != "4:2:0" ||
        (verified_probe != nullptr &&
         (verified_probe->pixel_format() != pixel_format ||
          verified_probe->color_range() != color_range ||
          verified_probe->bit_depth() != bit_depth ||
          verified_probe->chroma_subsampling() != chroma_subsampling))) {
        return fail(error_out,
                    "video sanity pixel semantics are not full-range 8-bit 4:2:0");
    }

    const auto parse_positive_canonical_rational = [](const std::string& value,
                                                       std::uint64_t* numerator,
                                                       std::uint64_t* denominator) {
        const std::size_t slash = value.find('/');
        if (slash == std::string::npos || slash == 0 || slash + 1 >= value.size() ||
            value.find('/', slash + 1) != std::string::npos) {
            return false;
        }
        const std::string_view num(value.data(), slash);
        const std::string_view den(value.data() + slash + 1,
                                   value.size() - slash - 1);
        if ((num.size() > 1 && num.front() == '0') ||
            (den.size() > 1 && den.front() == '0') ||
            !parse_decimal_u64(num, numerator) ||
            !parse_decimal_u64(den, denominator) || *numerator == 0 ||
            *denominator == 0 || std::gcd(*numerator, *denominator) != 1) {
            return false;
        }
        return true;
    };
    const auto read_i64_or_null = [](const json& value,
                                     std::int64_t* output) {
        if (!output || value.is_null() || value.is_boolean() ||
            (!value.is_number_integer() && !value.is_number_unsigned())) {
            return false;
        }
        if (value.is_number_unsigned()) {
            const std::uint64_t parsed = value.get<std::uint64_t>();
            if (parsed > static_cast<std::uint64_t>(
                             std::numeric_limits<std::int64_t>::max())) {
                return false;
            }
            *output = static_cast<std::int64_t>(parsed);
            return true;
        }
        *output = value.get<std::int64_t>();
        return true;
    };
    const json& timeline = document.at("timeline");
    static constexpr std::array<std::string_view, 5> timeline_keys = {
        "frame_rate", "time_base", "has_decoded_pts", "first_decoded_pts",
        "last_decoded_pts"};
    std::string frame_rate;
    std::string time_base;
    bool has_decoded_pts = false;
    std::uint64_t time_base_num = 0;
    std::uint64_t time_base_den = 0;
    if (!exact_keys(timeline, timeline_keys, "video_sanity.timeline", error_out) ||
        !read_string(timeline, "frame_rate", &frame_rate,
                     "video_sanity.timeline", error_out, 64) ||
        !read_string(timeline, "time_base", &time_base,
                     "video_sanity.timeline", error_out, 64) ||
        !read_bool(timeline, "has_decoded_pts", &has_decoded_pts,
                   "video_sanity.timeline", error_out) ||
        frame_rate != std::to_string(expected_fps) + "/1" ||
        !parse_positive_canonical_rational(time_base, &time_base_num,
                                           &time_base_den) ||
        (verified_probe != nullptr &&
         (verified_probe->frame_rate() != frame_rate ||
          verified_probe->time_base() != time_base))) {
        return fail(error_out, "video sanity timeline cadence is invalid");
    }
    const long double time_base_seconds =
        static_cast<long double>(time_base_num) /
        static_cast<long double>(time_base_den);
    const long double expected_period =
        1.0L / static_cast<long double>(expected_fps);
    const long double ticks_per_frame = expected_period / time_base_seconds;
    const long double integral_ticks = std::round(ticks_per_frame);
    if (!std::isfinite(time_base_seconds) || time_base_seconds <= 0.0L ||
        time_base_seconds > expected_period / 4.0L ||
        integral_ticks < 4.0L ||
        std::fabs(ticks_per_frame - integral_ticks) > 1e-9L) {
        return fail(error_out,
                    "video sanity time base cannot represent the contract cadence exactly");
    }
    const long double expected_duration =
        static_cast<long double>(expected_frames) * expected_period;
    if (std::fabs(static_cast<long double>(duration) - expected_duration) >
        time_base_seconds / 2.0L + 1e-9L) {
        return fail(error_out,
                    "video sanity duration does not match frame count and time base");
    }
    if (!has_decoded_pts) {
        return fail(error_out,
                    "video sanity fixed MP4 profile requires decoded PTS evidence");
    } else {
        std::int64_t first_pts = 0;
        std::int64_t last_pts = 0;
        if (!read_i64_or_null(timeline.at("first_decoded_pts"), &first_pts) ||
            !read_i64_or_null(timeline.at("last_decoded_pts"), &last_pts) ||
            first_pts > last_pts ||
            (expected_frames > 1 && first_pts == last_pts)) {
            return fail(error_out, "video sanity decoded PTS range is invalid");
        }
        const long double ticks =
            static_cast<long double>(last_pts) -
            static_cast<long double>(first_pts);
        const long double expected_ticks =
            integral_ticks * static_cast<long double>(expected_frames - 1);
        if (!std::isfinite(ticks) ||
            std::fabs(ticks - expected_ticks) > 0.5L ||
            (verified_probe != nullptr &&
             (!verified_probe->has_decoded_pts() ||
              verified_probe->first_decoded_pts() != first_pts ||
              verified_probe->last_decoded_pts() != last_pts))) {
            return fail(error_out,
                        "video sanity decoded PTS span does not match cadence");
        }
    }
    return true;
}

bool validate_opened_finalize_artifacts(
    std::vector<OpenArtifact>* artifacts,
    const NormalizedFinalizeRequest& request,
    const SpatialRoiRecorderEvidenceBinding& binding,
    const EvidenceCounts& counts,
    const SpatialRoiRecorderVideoSanityResult* verified_probe,
    json* receipts_out,
    std::string* error_out)
{
    if (!artifacts || !receipts_out) {
        return fail(error_out, "finalization artifact validation destination is null");
    }
    OpenArtifact* video = find_open_artifact(artifacts, "video");
    OpenArtifact* metadata = find_open_artifact(artifacts, "metadata");
    OpenArtifact* keyframes = find_open_artifact(artifacts, "keyframes");
    OpenArtifact* perf = find_open_artifact(artifacts, "perf");
    OpenArtifact* summary = find_open_artifact(artifacts, "summary");
    OpenArtifact* status = find_open_artifact(artifacts, "status");
    OpenArtifact* finalization = find_open_artifact(artifacts, "finalization");
    OpenArtifact* sanity = find_open_artifact(artifacts, "video_sanity");
    OpenArtifact* recorder_log = find_open_artifact(artifacts, "recorder_log");
    OpenArtifact* transport = find_open_artifact(artifacts, "transport_sidecar");
    if (request.terminal_state == kTerminalComplete &&
        (!video || !metadata || !keyframes || !perf || !summary || !status ||
         !finalization || !sanity || !recorder_log || !transport)) {
        return fail(error_out,
                    "complete finalization is missing a required artifact proof");
    }
    if (request.terminal_state == kTerminalComplete &&
        (!validate_metadata_csv(metadata->file->borrowed_fd(), binding,
                                counts.encoded_frames, error_out) ||
         !validate_perf_csv(perf->file->borrowed_fd(), binding,
                            counts.encoded_frames, error_out) ||
         !validate_terminal_json_sidecar(
             summary->file->borrowed_fd(), "summary", binding,
             counts.encoded_frames, error_out) ||
         !validate_terminal_json_sidecar(
             status->file->borrowed_fd(), "status", binding,
             counts.encoded_frames, error_out) ||
         !validate_recorder_log(recorder_log->file->borrowed_fd(), binding,
                                counts.encoded_frames, error_out) ||
         !validate_transport_sidecar(
             transport->file->borrowed_fd(), binding, counts.encoded_frames,
             error_out))) {
        return false;
    }
    if (keyframes &&
        !validate_closed_keyframe_sidecar(keyframes->file->borrowed_fd(),
                                          counts.encoded_frames,
                                          binding.encode_profile.at("frame_rate")
                                              .get<std::uint64_t>(),
                                          encode_profile_gop_length(
                                              binding.encode_profile),
                                          error_out)) {
        return false;
    }
    if (finalization) {
        if (!video) {
            return fail(error_out,
                        "container finalization sidecar has no contract video");
        }
        const std::uint64_t video_size =
            video->reference.at("size_bytes").get<std::uint64_t>();
        const std::uint64_t fps =
            binding.encode_profile.at("frame_rate").get<std::uint64_t>();
        if (!validate_container_finalization_sidecar(
                finalization->file->borrowed_fd(), binding.artifact_root,
                finalization->relative_path, video->relative_path, video_size,
                fps, error_out)) {
            return false;
        }
    }
    if (sanity) {
        if (!video || !validate_closed_video_sanity_sidecar(
                          sanity->file->borrowed_fd(), binding,
                          video->relative_path,
                          video->reference.at("size_bytes").get<std::uint64_t>(),
                          video->reference.at("sha256").get<std::string>(),
                          static_cast<std::uint64_t>(video->snapshot.st_dev),
                          static_cast<std::uint64_t>(video->snapshot.st_ino),
                          counts.encoded_frames,
                          video->file->artifact_root_identity(),
                          verified_probe, error_out)) {
            return false;
        }
    }
    if (!artifacts_unchanged(*artifacts, error_out)) {
        return false;
    }
    json receipts = json::object();
    for (const auto& artifact : *artifacts) {
        receipts[artifact.kind] = artifact.reference;
    }
    *receipts_out = std::move(receipts);
    return true;
}

bool parse_binding_json(const json& value,
                        SpatialRoiRecorderEvidenceBinding* output,
                        std::string* error_out)
{
    if (!validate_binding_json(value, error_out) || !output) {
        return output ? false : fail(error_out, "binding output is null");
    }
    const json& contract = value.at("contract");
    const json& plan = value.at("plan");
    const json& recording = value.at("recording");
    const json& camera = value.at("camera");
    const json& stream = value.at("stream");
    const json& gpu = value.at("gpu");
    const json& roots = value.at("roots");
    const json& limits = value.at("limits");
    output->contract_schema_id = contract.at("schema_id").get<std::string>();
    output->contract_schema_version = contract.at("schema_version").get<int>();
    output->contract_sha256 = contract.at("sha256").get<std::string>();
    output->contract_mode = contract.at("mode").get<std::string>();
    output->plan_schema_id = plan.at("schema_id").get<std::string>();
    output->plan_schema_version = plan.at("schema_version").get<int>();
    output->plan_sha256 = plan.at("sha256").get<std::string>();
    output->recording_id = recording.at("recording_id").get<std::string>();
    output->session_id = recording.at("session_id").get<std::string>();
    output->recording_identity_token =
        recording.at("recording_identity_token").get<std::string>();
    output->producer_generation = recording.at("producer_generation").get<std::string>();
    output->camera_id = camera.at("camera_id").get<int>();
    output->camera_serial = camera.at("camera_serial").get<std::string>();
    output->analytics_gpu_id = camera.at("analytics_gpu_id").get<int>();
    output->source_gpu_id = camera.at("source_gpu_id").get<int>();
    output->roi_id = stream.at("roi_id").get<std::string>();
    output->region_id = stream.at("region_id").get<std::string>();
    output->arena_group_id = stream.at("arena_group_id").get<std::string>();
    output->has_arena_id = stream.at("has_arena_id").get<bool>();
    output->arena_id = output->has_arena_id ? stream.at("arena_id").get<std::string>() : "";
    output->logical_stream_id = stream.at("logical_stream_id").get<std::string>();
    output->routing_policy = stream.at("routing_policy").get<std::string>();
    output->geometry_identity = value.at("geometry");
    output->encode_profile = value.at("encode_profile");
    output->recorder_gpu_id = gpu.at("recorder_gpu_id").get<int>();
    output->assigned_gpu_id = gpu.at("assigned_gpu_id").get<int>();
    output->assigned_shard_id = gpu.at("assigned_shard_id").get<int>();
    output->recording_root = roots.at("recording_root").get<std::string>();
    output->artifact_root = roots.at("artifact_root").get<std::string>();
    output->max_frames_per_stream =
        limits.at("max_frames_per_stream").get<std::uint64_t>();
    output->max_media_bytes_per_stream =
        limits.at("max_media_bytes_per_stream").get<std::uint64_t>();
    output->max_evidence_bytes_per_stream =
        limits.at("max_evidence_bytes_per_stream").get<std::uint64_t>();
    output->expected_artifacts.clear();
    for (const auto& [kind, path] : value.at("expected_artifacts").items()) {
        output->expected_artifacts.emplace(kind, path.get<std::string>());
    }
    return true;
}

bool parse_u64_value(const json& value,
                     std::uint64_t* output,
                     const std::string& path,
                     std::string* error_out)
{
    if (value.is_boolean() ||
        (!value.is_number_unsigned() && !value.is_number_integer())) {
        return fail(error_out, path + " must be an unsigned integer");
    }
    if (value.is_number_unsigned()) {
        *output = value.get<std::uint64_t>();
        return true;
    }
    const auto signed_value = value.get<std::int64_t>();
    if (signed_value < 0) {
        return fail(error_out, path + " must not be negative");
    }
    *output = static_cast<std::uint64_t>(signed_value);
    return true;
}

bool parse_frame_json(const json& value,
                      const SpatialRoiRecorderEvidenceBinding& binding,
                      std::uint64_t* expected_recording_frame_id,
                      std::uint64_t* expected_roi_index,
                      std::uint64_t* expected_output_index,
                      std::size_t expected_record_index,
                      EvidenceCounts* counts,
                      std::uint64_t* first_recording_frame_id,
                      std::uint64_t* last_recording_frame_id,
                      std::uint64_t* first_roi_index,
                      std::uint64_t* last_roi_index,
                      checksum::StreamingSha256* metadata_hasher,
                      std::string* error_out)
{
    if (!exact_keys(value, kEvidenceFrameKeysWithRest, "evidence.frame", error_out)) {
        return false;
    }
    if (!read_const_string(value, "record_type", kFrameRecordType,
                           "evidence.frame", error_out) ||
        !read_const_string(value, "schema_id", kSpatialRoiRecorderEvidenceSchemaId,
                           "evidence.frame", error_out) ||
        !read_const_int(value, "schema_version",
                        kSpatialRoiRecorderEvidenceSchemaVersion,
                        "evidence.frame", error_out) ||
        !read_const_string(value, "canonicalization", kSpatialRoiRecorderCanonicalization,
                           "evidence.frame", error_out)) {
        return false;
    }
    std::uint64_t record_index = 0;
    if (!parse_u64_value(value.at("frame_record_index"), &record_index,
                         "evidence.frame.frame_record_index", error_out) ||
        record_index != expected_record_index) {
        return fail(error_out, "evidence frame record index is not contiguous");
    }
    SpatialRoiFrameDescriptor descriptor;
    if (!spatial_roi_frame_descriptor_from_json(value.at("frame"), &descriptor,
                                                error_out) ||
        !validate_correlation(value.at("correlation"), descriptor, error_out)) {
        return false;
    }
    SpatialRoiRecorderFrameEvidence frame;
    frame.frame = descriptor;
    const json& detach = value.at("detach");
    const json& dispatch = value.at("dispatch");
    const json& ack = value.at("ack");
    const json& release = value.at("release");
    const json& encode = value.at("encode");
    if (!exact_keys(detach, kDetachKeys, "evidence.frame.detach", error_out) ||
        !exact_keys(dispatch, kDispatchKeys, "evidence.frame.dispatch", error_out) ||
        !exact_keys(ack, kAckKeys, "evidence.frame.ack", error_out) ||
        !exact_keys(release, kReleaseKeys, "evidence.frame.release", error_out) ||
        !exact_keys(encode, kEncodeKeys, "evidence.frame.encode", error_out) ||
        !read_string(detach, "status", &frame.detach_status,
                     "evidence.frame.detach", error_out, 64) ||
        !read_bool(detach, "source_release_safe", &frame.source_release_safe,
                   "evidence.frame.detach", error_out) ||
        !read_bool(dispatch, "admitted", &frame.dispatch_admitted,
                   "evidence.frame.dispatch", error_out) ||
        !read_optional_text(dispatch, "reason", &frame.dispatch_reason,
                            "evidence.frame.dispatch", error_out,
                            kMaxReasonBytes) ||
        !read_bool(ack, "attempted", &frame.ack_attempted,
                   "evidence.frame.ack", error_out) ||
        !read_bool(ack, "sent", &frame.ack_sent, "evidence.frame.ack", error_out) ||
        !read_bool(ack, "accepted", &frame.ack_accepted, "evidence.frame.ack", error_out) ||
        !read_optional_text(ack, "reason", &frame.ack_reason,
                            "evidence.frame.ack", error_out, kMaxReasonBytes) ||
        !read_optional_text(ack, "error", &frame.ack_error,
                            "evidence.frame.ack", error_out, kMaxReasonBytes) ||
        !read_bool(release, "attempted", &frame.release_attempted,
                   "evidence.frame.release", error_out) ||
        !read_bool(release, "sent", &frame.release_sent,
                   "evidence.frame.release", error_out) ||
        !read_optional_text(release, "reason", &frame.release_reason,
                            "evidence.frame.release", error_out, kMaxReasonBytes) ||
        !read_optional_text(release, "error", &frame.release_error,
                            "evidence.frame.release", error_out, kMaxReasonBytes) ||
        !read_string(encode, "status", &frame.encode_status,
                     "evidence.frame.encode", error_out, 32) ||
        !read_u64(encode, "output_frame_index", &frame.output_frame_index,
                  "evidence.frame.encode", error_out) ||
        !read_u64(encode, "packet_count", &frame.packet_count,
                  "evidence.frame.encode", error_out) ||
        !read_u64(encode, "encoded_bytes", &frame.encoded_bytes,
                  "evidence.frame.encode", error_out) ||
        !read_bool(encode, "keyframe", &frame.keyframe,
                   "evidence.frame.encode", error_out) ||
        !validate_frame_input(binding, frame, error_out, counts)) {
        return false;
    }
    if (frame.encode_status == kEncodeEncoded) {
        if (*expected_output_index == std::numeric_limits<std::uint64_t>::max() ||
            frame.output_frame_index != *expected_output_index + 1) {
            return fail(error_out,
                        "evidence output frame indices are not dense and one-based");
        }
        *expected_output_index = frame.output_frame_index;
        if (!metadata_hasher) {
            return fail(error_out,
                        "evidence metadata projection hasher is unavailable");
        }
        metadata_hasher->update(
            metadata_csv_row(descriptor, frame.output_frame_index));
    } else if (frame.output_frame_index != 0) {
        return fail(error_out, "nonencoded evidence frame has an output index");
    }
    if (*expected_recording_frame_id != 0 &&
        descriptor.recording_frame_id <= *expected_recording_frame_id) {
        return fail(error_out, "evidence recording frame IDs are duplicate/out of order");
    }
    if (*expected_roi_index == 0 && descriptor.roi_stream_frame_index != 1) {
        return fail(error_out, "evidence ROI frame indices must start at one");
    }
    if (*expected_roi_index != 0 && descriptor.roi_stream_frame_index != *expected_roi_index + 1) {
        return fail(error_out, "evidence ROI frame indices are not dense and ordered");
    }
    if (*expected_recording_frame_id == 0) {
        *first_recording_frame_id = descriptor.recording_frame_id;
        *first_roi_index = descriptor.roi_stream_frame_index;
    }
    *expected_recording_frame_id = descriptor.recording_frame_id;
    *expected_roi_index = descriptor.roi_stream_frame_index;
    *last_recording_frame_id = descriptor.recording_frame_id;
    *last_roi_index = descriptor.roi_stream_frame_index;
    return true;
}

// Validate JSONL directly from the descriptor-relative evidence fd.  A large
// recording must not require a second multi-gigabyte copy of its history in
// memory merely to verify the final receipt.
bool parse_evidence_stream_fd(
    const int evidence_fd,
    const SpatialRoiRecorderEvidenceBinding& expected_binding,
    std::size_t* frame_count_out,
    EvidenceCounts* counts_out,
    std::uint64_t* first_recording_frame_id,
    std::uint64_t* last_recording_frame_id,
    std::uint64_t* first_roi_index,
    std::uint64_t* last_roi_index,
    std::string* terminal_state_out,
    std::string* terminal_reason_out,
    std::string* finalize_request_sha256_out,
    json* artifact_receipts_out,
    json* encoder_terminal_out,
    std::string* expected_metadata_sha256_out,
    std::uint64_t* evidence_size_out,
    std::string* evidence_sha256_out,
    std::string* error_out)
{
    if (evidence_fd < 0) {
        return fail(error_out, "evidence file descriptor is invalid");
    }
    struct stat before {};
    if (::fstat(evidence_fd, &before) != 0 || !S_ISREG(before.st_mode) ||
        before.st_size <= 0 ||
        static_cast<std::uint64_t>(before.st_size) >
            expected_binding.max_evidence_bytes_per_stream) {
        return fail(error_out, "evidence JSONL file size is outside its bound");
    }
    if (::lseek(evidence_fd, 0, SEEK_SET) < 0) {
        return fail(error_out, "could not seek evidence JSONL to its beginning");
    }

    EvidenceCounts counts;
    std::size_t frame_count = 0;
    std::uint64_t expected_recording_frame_id = 0;
    std::uint64_t expected_roi_index = 0;
    std::uint64_t expected_output_index = 0;
    std::uint64_t first_recording = 0;
    std::uint64_t last_recording = 0;
    std::uint64_t first_roi = 0;
    std::uint64_t last_roi = 0;
    std::string terminal_state;
    std::string terminal_reason;
    std::string finalize_request_sha256;
    json artifact_receipts = json::object();
    json terminal_encoder_snapshot = nullptr;
    bool header_seen = false;
    bool terminal_seen = false;
    std::uint64_t total_bytes = 0;
    checksum::StreamingSha256 evidence_hasher;
    checksum::StreamingSha256 metadata_hasher;
    metadata_hasher.update(
        reinterpret_cast<const std::uint8_t*>(kMetadataCsvHeader.data()),
        kMetadataCsvHeader.size());
    std::string line;
    line.reserve(16 * 1024);

    const auto process_line = [&](const std::string& line_bytes) -> bool {
        if (line_bytes.empty()) {
            return fail(error_out, "evidence JSONL contains an empty record");
        }
        if (line_bytes.size() > kSpatialRoiRecorderEvidenceMaxLineBytes) {
            return fail(error_out, "evidence JSONL record exceeds bounded line size");
        }
        json record;
        if (!strict_json_from_bytes(line_bytes, 200000, &record,
                                    "evidence JSONL record", error_out) ||
            !record.is_object()) {
            return fail(error_out, "evidence JSONL contains invalid JSON record");
        }
        if (terminal_seen) {
            return fail(error_out, "evidence JSONL contains records after its terminal record");
        }
        if (!header_seen) {
            if (!exact_keys(record, kEvidenceHeaderKeys, "evidence.header", error_out) ||
                !read_const_string(record, "record_type", kHeaderRecordType,
                                   "evidence.header", error_out) ||
                !read_const_string(record, "schema_id", kSpatialRoiRecorderEvidenceSchemaId,
                                   "evidence.header", error_out) ||
                !read_const_int(record, "schema_version",
                                kSpatialRoiRecorderEvidenceSchemaVersion,
                                "evidence.header", error_out) ||
                !read_const_string(record, "canonicalization",
                                   kSpatialRoiRecorderCanonicalization,
                                   "evidence.header", error_out)) {
                return false;
            }
            SpatialRoiRecorderEvidenceBinding header_binding;
            if (!parse_binding_json(record.at("binding"), &header_binding, error_out) ||
                !binding_json_equal(header_binding, expected_binding)) {
                return fail(error_out,
                            "evidence header binding disagrees with manifest binding");
            }
            header_seen = true;
            return true;
        }
        const auto record_type_it = record.find("record_type");
        if (record_type_it == record.end() || !record_type_it->is_string()) {
            return fail(error_out, "evidence record_type must be a string");
        }
        if (record_type_it->get<std::string>() == kTerminalRecordType) {
            if (!exact_keys(record, kEvidenceTerminalKeys, "evidence.terminal", error_out) ||
                !read_const_string(record, "record_type", kTerminalRecordType,
                                   "evidence.terminal", error_out) ||
                !read_const_string(record, "schema_id", kSpatialRoiRecorderEvidenceSchemaId,
                                   "evidence.terminal", error_out) ||
                !read_const_int(record, "schema_version",
                                kSpatialRoiRecorderEvidenceSchemaVersion,
                                "evidence.terminal", error_out) ||
                !read_const_string(record, "canonicalization",
                                   kSpatialRoiRecorderCanonicalization,
                                   "evidence.terminal", error_out)) {
                return false;
            }
            std::uint64_t terminal_frame_count = 0;
            if (!read_string(record, "terminal_state", &terminal_state,
                             "evidence.terminal", error_out, 32) ||
                !read_string(record, "reason", &terminal_reason,
                             "evidence.terminal", error_out, kMaxReasonBytes) ||
                !read_u64(record, "frame_count", &terminal_frame_count,
                          "evidence.terminal", error_out) ||
                terminal_frame_count != frame_count ||
                record.at("binding_sha256") != expected_binding.contract_sha256 ||
                record.at("receipt_scope") !=
                    "canonical_manifest_without_finalized_receipt_digest_v1") {
                return fail(error_out, "evidence terminal record is inconsistent");
            }
            if (terminal_state != kTerminalComplete &&
                terminal_state != kTerminalFailed) {
                return fail(error_out, "evidence terminal state is invalid");
            }
            NormalizedFinalizeRequest terminal_request;
            if (!validate_terminal_artifact_receipts(
                    record.at("artifacts"), expected_binding, terminal_state,
                    &terminal_request, error_out)) {
                return false;
            }
            terminal_request.terminal_reason = terminal_reason;
            if (record.at("encoder_terminal").is_null()) {
                if (terminal_state == kTerminalComplete) {
                    return fail(error_out,
                                "complete evidence lacks encoder terminal snapshot");
                }
            } else if (!validate_encoder_terminal_projection(
                           record.at("encoder_terminal"),
                           terminal_state == kTerminalComplete, &counts,
                           error_out)) {
                return false;
            }
            terminal_request.encoder_terminal = record.at("encoder_terminal");
            terminal_encoder_snapshot = record.at("encoder_terminal");
            if (!validate_sha_field(record, "finalize_request_sha256",
                                    "evidence.terminal", error_out)) {
                return false;
            }
            finalize_request_sha256 =
                record.at("finalize_request_sha256").get<std::string>();
            if (finalize_request_sha256 != canonical_json_sha256(
                    normalized_finalize_json(terminal_request))) {
                return fail(error_out,
                            "evidence terminal finalization request digest is invalid");
            }
            artifact_receipts = record.at("artifacts");
            const json& terminal_counts = record.at("counts");
            if (!exact_keys(terminal_counts, kCountKeys,
                            "evidence.terminal.counts", error_out) ||
                terminal_counts != counts_json(counts)) {
                return fail(error_out,
                            "evidence terminal counts do not match frame records");
            }
            if (terminal_state == kTerminalComplete &&
                (frame_count == 0 || counts.failed_frames != 0)) {
                return fail(error_out,
                            "complete evidence terminal contains failed or empty frames");
            }
            if (!exact_keys(record.at("ranges"), kRangeKeys,
                            "evidence.terminal.ranges", error_out) ||
                (frame_count == 0 &&
                 record.at("ranges") != ranges_json(false, 0, 0, 0, 0, 0)) ||
                (frame_count != 0 &&
                 record.at("ranges") !=
                     ranges_json(true, frame_count, first_recording, last_recording,
                                 first_roi, last_roi))) {
                return fail(error_out,
                            "evidence terminal ranges do not match frame records");
            }
            terminal_seen = true;
            return true;
        }
        if (record_type_it->get<std::string>() != kFrameRecordType) {
            return fail(error_out, "evidence JSONL contains an unknown record type");
        }
        if (frame_count >= expected_binding.max_frames_per_stream) {
            return fail(error_out, "evidence JSONL exceeds bounded record count");
        }
        ++frame_count;
        return parse_frame_json(record, expected_binding,
                                &expected_recording_frame_id, &expected_roi_index,
                                &expected_output_index,
                                frame_count, &counts, &first_recording, &last_recording,
                                &first_roi, &last_roi, &metadata_hasher, error_out);
    };

    std::array<char, 64 * 1024> buffer{};
    for (;;) {
        const ssize_t count = ::read(evidence_fd, buffer.data(), buffer.size());
        if (count < 0) {
            if (errno == EINTR) {
                continue;
            }
            return fail(error_out, "evidence JSONL read failed: " +
                                       std::string(std::strerror(errno)));
        }
        if (count == 0) {
            break;
        }
        const auto amount = static_cast<std::uint64_t>(count);
        if (amount > expected_binding.max_evidence_bytes_per_stream ||
            total_bytes >
                expected_binding.max_evidence_bytes_per_stream - amount) {
            return fail(error_out, "evidence JSONL exceeds its bounded file size");
        }
        total_bytes += amount;
        evidence_hasher.update(
            reinterpret_cast<const std::uint8_t*>(buffer.data()),
            static_cast<std::size_t>(count));
        for (ssize_t index = 0; index < count; ++index) {
            const char current = buffer[static_cast<std::size_t>(index)];
            if (current == '\n') {
                if (!process_line(line)) {
                    return false;
                }
                line.clear();
            } else {
                if (line.size() >= kSpatialRoiRecorderEvidenceMaxLineBytes) {
                    return fail(error_out,
                                "evidence JSONL record exceeds bounded line size");
                }
                line.push_back(current);
            }
        }
    }
    if (!line.empty()) {
        return fail(error_out, "evidence JSONL must be newline terminated");
    }
    if (!header_seen || !terminal_seen) {
        return fail(error_out,
                    "evidence JSONL requires header and terminal records");
    }
    struct stat after {};
    if (::fstat(evidence_fd, &after) != 0 || !S_ISREG(after.st_mode) ||
        !same_file_snapshot(before, after) ||
        static_cast<std::uint64_t>(after.st_size) != total_bytes) {
        return fail(error_out, "evidence JSONL changed while being validated");
    }
    if (frame_count_out) *frame_count_out = frame_count;
    if (counts_out) *counts_out = counts;
    if (first_recording_frame_id) *first_recording_frame_id = first_recording;
    if (last_recording_frame_id) *last_recording_frame_id = last_recording;
    if (first_roi_index) *first_roi_index = first_roi;
    if (last_roi_index) *last_roi_index = last_roi;
    if (terminal_state_out) *terminal_state_out = terminal_state;
    if (terminal_reason_out) *terminal_reason_out = terminal_reason;
    if (finalize_request_sha256_out) {
        *finalize_request_sha256_out = finalize_request_sha256;
    }
    if (artifact_receipts_out) *artifact_receipts_out = artifact_receipts;
    if (encoder_terminal_out) {
        // terminal_encoder_snapshot is assigned when the terminal row is
        // parsed; retain it independently of per-line DOM lifetime.
        *encoder_terminal_out = terminal_encoder_snapshot;
    }
    if (expected_metadata_sha256_out) {
        *expected_metadata_sha256_out =
            "sha256:" + metadata_hasher.final_hex();
    }
    if (evidence_size_out) *evidence_size_out = total_bytes;
    if (evidence_sha256_out) {
        *evidence_sha256_out = "sha256:" + evidence_hasher.final_hex();
    }
    return true;
}

bool manifest_counts_from_json(const json& value,
                               EvidenceCounts* counts,
                               std::string* error_out)
{
    if (!exact_keys(value, kCountKeys, "manifest.counts", error_out) || !counts) {
        return counts ? false : fail(error_out, "manifest count output is null");
    }
    auto read = [&](const char* key, std::uint64_t* out) {
        return read_u64(value, key, out, "manifest.counts", error_out);
    };
    return read("detach_successes", &counts->detach_successes) &&
        read("dispatch_admitted", &counts->dispatch_admitted) &&
        read("dispatch_rejected", &counts->dispatch_rejected) &&
        read("ack_attempted", &counts->ack_attempted) &&
        read("ack_sent", &counts->ack_sent) &&
        read("ack_accepted", &counts->ack_accepted) &&
        read("release_attempted", &counts->release_attempted) &&
        read("release_sent", &counts->release_sent) &&
        read("encoded_frames", &counts->encoded_frames) &&
        read("failed_frames", &counts->failed_frames) &&
        read("packet_count", &counts->packet_count) &&
        read("encoded_bytes", &counts->encoded_bytes) &&
        read("keyframes", &counts->keyframes) &&
        read("ack_write_failures", &counts->ack_write_failures) &&
        read("release_write_failures", &counts->release_write_failures) &&
        read("lifecycle_failures", &counts->lifecycle_failures);
}

bool manifest_ranges_from_json(const json& value,
                               bool* has_frames,
                               std::size_t* frame_count,
                               std::uint64_t* first_recording,
                               std::uint64_t* last_recording,
                               std::uint64_t* first_roi,
                               std::uint64_t* last_roi,
                               std::string* error_out)
{
    if (!exact_keys(value, kRangeKeys, "manifest.ranges", error_out) ||
        !value.at("recording_frame_id").is_object() ||
        !value.at("roi_stream_frame_index").is_object()) {
        return false;
    }
    static constexpr std::array<std::string_view, 2> endpoints = {"first", "last"};
    if (!exact_keys(value.at("recording_frame_id"), endpoints,
                    "manifest.ranges.recording_frame_id", error_out) ||
        !exact_keys(value.at("roi_stream_frame_index"), endpoints,
                    "manifest.ranges.roi_stream_frame_index", error_out) ||
        !read_bool(value, "has_frames", has_frames, "manifest.ranges", error_out)) {
        return false;
    }
    std::uint64_t frame_count_value = 0;
    if (!read_u64(value, "frame_count", &frame_count_value, "manifest.ranges",
                  error_out) ||
        frame_count_value > std::numeric_limits<std::size_t>::max()) {
        return fail(error_out, "manifest.ranges.frame_count is out of range");
    }
    *frame_count = static_cast<std::size_t>(frame_count_value);
    std::uint64_t first = 0;
    std::uint64_t last = 0;
    std::uint64_t first_roi_value = 0;
    std::uint64_t last_roi_value = 0;
    if (!read_u64(value.at("recording_frame_id"), "first", &first,
                  "manifest.ranges.recording_frame_id", error_out) ||
        !read_u64(value.at("recording_frame_id"), "last", &last,
                  "manifest.ranges.recording_frame_id", error_out) ||
        !read_u64(value.at("roi_stream_frame_index"), "first", &first_roi_value,
                  "manifest.ranges.roi_stream_frame_index", error_out) ||
        !read_u64(value.at("roi_stream_frame_index"), "last", &last_roi_value,
                  "manifest.ranges.roi_stream_frame_index", error_out)) {
        return false;
    }
    if (*has_frames && (*frame_count == 0 || first == 0 || last < first ||
                        first_roi_value == 0 || last_roi_value < first_roi_value)) {
        return fail(error_out, "manifest frame ranges are invalid");
    }
    if (!*has_frames && (*frame_count != 0 || first != 0 || last != 0 ||
                         first_roi_value != 0 || last_roi_value != 0)) {
        return fail(error_out, "empty manifest ranges contain nonzero values");
    }
    *first_recording = first;
    *last_recording = last;
    *first_roi = first_roi_value;
    *last_roi = last_roi_value;
    return true;
}

}  // namespace

// Internal retained-authority validator used by finalization and recovery.
bool validate_spatial_roi_recorder_finalized_manifest_authority(
    const SpatialRoiRecorderArtifactRoot& artifact_root,
    const SpatialRoiRecorderEvidenceBinding& expected_binding,
    const nlohmann::json& manifest,
    std::string* error_out);

json spatial_roi_recorder_evidence_binding_to_json(
    const SpatialRoiRecorderEvidenceBinding& binding)
{
    return {
        {"contract", {
            {"schema_id", binding.contract_schema_id},
            {"schema_version", binding.contract_schema_version},
            {"sha256", binding.contract_sha256},
            {"mode", binding.contract_mode},
        }},
        {"plan", {
            {"schema_id", binding.plan_schema_id},
            {"schema_version", binding.plan_schema_version},
            {"sha256", binding.plan_sha256},
        }},
        {"recording", {
            {"recording_id", binding.recording_id},
            {"session_id", binding.session_id},
            {"recording_identity_token", binding.recording_identity_token},
            {"producer_generation", binding.producer_generation},
        }},
        {"camera", {
            {"camera_id", binding.camera_id},
            {"camera_serial", binding.camera_serial},
            {"analytics_gpu_id", binding.analytics_gpu_id},
            {"source_gpu_id", binding.source_gpu_id},
        }},
        {"stream", {
            {"roi_id", binding.roi_id},
            {"region_id", binding.region_id},
            {"arena_group_id", binding.arena_group_id},
            {"arena_id", binding.has_arena_id ? json(binding.arena_id) : json(nullptr)},
            {"has_arena_id", binding.has_arena_id},
            {"logical_stream_id", binding.logical_stream_id},
            {"routing_policy", binding.routing_policy},
        }},
        {"geometry", binding.geometry_identity},
        {"gpu", {
            {"recorder_gpu_id", binding.recorder_gpu_id},
            {"assigned_gpu_id", binding.assigned_gpu_id},
            {"assigned_shard_id", binding.assigned_shard_id},
            {"routing_policy", binding.routing_policy},
        }},
        {"encode_profile", binding.encode_profile},
        {"roots", {
            {"recording_root", binding.recording_root},
            {"artifact_root", binding.artifact_root},
        }},
        {"limits", {
            {"max_frames_per_stream", binding.max_frames_per_stream},
            {"max_media_bytes_per_stream", binding.max_media_bytes_per_stream},
            {"max_evidence_bytes_per_stream",
             binding.max_evidence_bytes_per_stream},
        }},
        {"expected_artifacts", binding.expected_artifacts},
    };
}

bool validate_spatial_roi_recorder_evidence_binding(
    const SpatialRoiRecorderEvidenceBinding& binding,
    std::string* error_out)
{
    if (!safe_text(binding.contract_schema_id, 128) ||
        binding.contract_schema_id !=
            orange::session::spatial_roi::kSpatialRoiRecorderContractSchemaId ||
        binding.contract_schema_version !=
            orange::session::spatial_roi::kSpatialRoiRecorderContractSchemaVersion ||
        !is_sha256(binding.contract_sha256) ||
        binding.contract_mode !=
            orange::session::spatial_roi::kSpatialRoiRecorderContractMode ||
        !safe_text(binding.plan_schema_id, 128) ||
        binding.plan_schema_id != orange::session::spatial_roi::kPlanSchemaId ||
        binding.plan_schema_version !=
            orange::session::spatial_roi::kPlanSchemaVersion ||
        !is_sha256(binding.plan_sha256) ||
        !safe_recording_id(binding.recording_id) ||
        !safe_recording_id(binding.session_id) ||
        binding.session_id != binding.recording_id ||
        !is_sha256(binding.recording_identity_token) ||
        binding.recording_identity_token !=
            orange::shaman_v2_recording_identity::token_for_recording_id(
                binding.recording_id) ||
        !safe_identifier(binding.producer_generation, kMaxIdentifierBytes) ||
        binding.camera_id < 0 || !safe_identifier(binding.camera_serial, 128) ||
        binding.analytics_gpu_id < 0 || binding.source_gpu_id < 0 ||
        !safe_identifier(binding.roi_id) || !safe_identifier(binding.region_id) ||
        !safe_identifier(binding.arena_group_id) ||
        (binding.has_arena_id && !safe_identifier(binding.arena_id)) ||
        !safe_identifier(binding.logical_stream_id) ||
        binding.routing_policy != "single_shard" || binding.recorder_gpu_id < 0 ||
        binding.assigned_gpu_id != binding.recorder_gpu_id ||
        binding.source_gpu_id != binding.analytics_gpu_id ||
        binding.assigned_shard_id != 0 || binding.max_frames_per_stream == 0 ||
        binding.max_frames_per_stream > kSpatialRoiRecorderEvidenceMaxFrames ||
        binding.max_media_bytes_per_stream == 0 ||
        binding.max_media_bytes_per_stream >
            orange::session::spatial_roi::kMaxMediaBytesPerStream ||
        binding.max_evidence_bytes_per_stream == 0 ||
        binding.max_evidence_bytes_per_stream >
            kSpatialRoiRecorderEvidenceMaxFileBytes ||
        !validate_geometry(binding.geometry_identity, error_out) ||
        !validate_profile(binding.encode_profile, error_out)) {
        return fail(error_out, "spatial ROI recorder evidence binding is invalid");
    }
    return validate_binding_json(
        spatial_roi_recorder_evidence_binding_to_json(binding), error_out);
}

bool make_spatial_roi_recorder_evidence_binding(
    const json& contract,
    const json& verified_plan,
    const std::string& expected_recording_root,
    const orange::session::spatial_roi::SpatialRoiRecorderRuntimeGpuMapping&
        expected_gpu_mapping,
    const std::string& logical_stream_id,
    SpatialRoiRecorderEvidenceBinding* binding_out,
    std::string* error_out)
{
    if (!binding_out) {
        return fail(error_out, "evidence binding destination is null");
    }
    try {
        orange::session::spatial_roi::SpatialRoiRecorderContractView parsed;
        if (!orange::session::spatial_roi::parse_spatial_roi_recorder_contract(
                contract, verified_plan, expected_recording_root,
                expected_gpu_mapping, logical_stream_id, &parsed, error_out)) {
            return false;
        }
        const auto& stream = parsed.selected_stream;
        SpatialRoiRecorderEvidenceBinding binding;
        binding.contract_schema_id = parsed.schema_id;
        binding.contract_schema_version = parsed.schema_version;
        binding.contract_sha256 = canonical_json_sha256(contract);
        binding.contract_mode = parsed.mode;
        binding.plan_schema_id = verified_plan.at("schema_id").get<std::string>();
        binding.plan_schema_version = verified_plan.at("schema_version").get<int>();
        binding.plan_sha256 = parsed.spatial_roi_plan_sha256;
        binding.recording_id = parsed.recording_id;
        binding.session_id = parsed.session_id;
        binding.recording_identity_token = parsed.recording_identity_token;
        binding.producer_generation = parsed.producer_generation;
        binding.camera_id = stream.camera_id;
        binding.camera_serial = stream.camera_serial;
        binding.analytics_gpu_id = stream.analytics_gpu_id;
        binding.source_gpu_id = stream.source_gpu_id;
        binding.roi_id = stream.roi_id;
        binding.region_id = stream.region_id;
        binding.arena_group_id = stream.arena_group_id;
        binding.has_arena_id = stream.has_arena_id;
        binding.arena_id = stream.arena_id;
        binding.logical_stream_id = stream.logical_stream_id;
        // The authority parser has already required exact deterministic
        // contract equality.  Copy the exact selected JSON objects only after
        // that verification rather than maintaining a second parser here.
        const json& selected_json = contract.at("streams").at(logical_stream_id);
        binding.geometry_identity = selected_json.at("geometry_identity");
        binding.encode_profile = selected_json.at("encode_profile");
        binding.recorder_gpu_id = stream.recorder_gpu_id;
        binding.assigned_gpu_id = stream.assigned_gpu_id;
        binding.assigned_shard_id = 0;
        binding.routing_policy = stream.routing_policy;
        binding.recording_root = parsed.recording_root;
        binding.artifact_root = parsed.artifact_root;
        binding.max_frames_per_stream = stream.max_frames_per_stream;
        binding.max_media_bytes_per_stream =
            stream.max_media_bytes_per_stream;
        binding.max_evidence_bytes_per_stream =
            stream.max_evidence_bytes_per_stream;
        for (const auto& [kind, path] : stream.artifacts) {
            binding.expected_artifacts.emplace(kind, path.relative_path);
        }
        if (!validate_spatial_roi_recorder_evidence_binding(binding, error_out)) {
            return false;
        }
        *binding_out = std::move(binding);
        return true;
    } catch (const std::exception& exception) {
        return fail(error_out, std::string("evidence binding parse failed: ") +
                                   exception.what());
    } catch (...) {
        return fail(error_out, "evidence binding parse failed: unknown exception");
    }
}

SpatialRoiRecorderEvidenceWriter::~SpatialRoiRecorderEvidenceWriter()
{
    if (staging_fd_ >= 0) {
        (void)::close(staging_fd_);
        staging_fd_ = -1;
    }
    if (staging_dir_fd_ >= 0 && !staging_name_.empty()) {
        (void)::unlinkat(staging_dir_fd_, staging_name_.c_str(), 0);
    }
    if (staging_dir_fd_ >= 0) {
        (void)::close(staging_dir_fd_);
        staging_dir_fd_ = -1;
    }
    if (root_fd_ >= 0) {
        (void)::close(root_fd_);
        root_fd_ = -1;
    }
}

bool SpatialRoiRecorderEvidenceWriter::Open(
    SpatialRoiRecorderEvidenceWriterConfig config,
    SpatialRoiRecorderEvidenceBinding binding,
    std::unique_ptr<SpatialRoiRecorderEvidenceWriter>* writer_out,
    std::string* error_out)
{
    if (!writer_out) {
        return fail(error_out, "evidence writer destination is null");
    }
    writer_out->reset();
    if (!validate_spatial_roi_recorder_evidence_binding(binding, error_out) ||
        !validate_artifact_root_authority(config.artifact_root, binding,
                                          error_out) ||
        !safe_relative_path(config.evidence_relative_path) ||
        !safe_relative_path(config.manifest_relative_path) ||
        config.evidence_relative_path !=
            binding.expected_artifacts.at("evidence") ||
        config.manifest_relative_path !=
            binding.expected_artifacts.at("evidence_manifest") ||
        config.evidence_relative_path == config.manifest_relative_path ||
        config.max_frames == 0 ||
        config.max_frames != binding.max_frames_per_stream) {
        return fail(error_out, "evidence writer configuration is invalid");
    }
    try {
        auto writer = std::unique_ptr<SpatialRoiRecorderEvidenceWriter>(
            new SpatialRoiRecorderEvidenceWriter());
        writer->config_ = std::move(config);
        writer->binding_ = std::move(binding);
        writer->metadata_digest_ =
            std::make_unique<SpatialRoiRecorderEvidenceMetadataDigest>();
        writer->metadata_digest_->hasher.update(
            reinterpret_cast<const std::uint8_t*>(kMetadataCsvHeader.data()),
            kMetadataCsvHeader.size());
        writer->artifact_root_ = writer->config_.artifact_root;
        writer->evidence_leaf_ = path_components(writer->config_.evidence_relative_path).back();
        writer->manifest_leaf_ = path_components(writer->config_.manifest_relative_path).back();
        int root_fd = -1;
        if (!writer->artifact_root_->DuplicateArtifactRootFd(&root_fd,
                                                             error_out)) {
            return false;
        }
        writer->root_fd_ = root_fd;
        Fd evidence_parent;
        std::string evidence_leaf;
        if (!open_parent_dir(writer->root_fd_, writer->config_.evidence_relative_path,
                             true, &evidence_parent, &evidence_leaf, error_out)) {
            return false;
        }
        Fd manifest_parent;
        std::string manifest_leaf;
        if (!open_parent_dir(writer->root_fd_, writer->config_.manifest_relative_path,
                             true, &manifest_parent, &manifest_leaf, error_out)) {
            return false;
        }

        // Probe exact final names before creating any staging inode. Existing
        // bytes are never replaced: a validated terminal JSONL can be adopted
        // after a crash between evidence and manifest publication, and a
        // validated complete pair is an idempotent finalized writer.
        bool evidence_present = false;
        bool manifest_present = false;
        Fd evidence_probe;
        Fd manifest_probe;
        struct stat evidence_snapshot {};
        struct stat manifest_snapshot {};
        if (!optional_regular_file_at(
                evidence_parent.value, evidence_leaf, &evidence_present,
                &evidence_probe, &evidence_snapshot, error_out) ||
            !optional_regular_file_at(
                manifest_parent.value, manifest_leaf, &manifest_present,
                &manifest_probe, &manifest_snapshot, error_out)) {
            return false;
        }
        std::unique_ptr<SpatialRoiRecorderArtifactFile> existing_evidence;
        std::unique_ptr<SpatialRoiRecorderArtifactFile> existing_manifest;
        if ((evidence_present && !open_existing_artifact(
                 *writer->artifact_root_, writer->config_.evidence_relative_path,
                 &existing_evidence, error_out)) ||
            (manifest_present && !open_existing_artifact(
                 *writer->artifact_root_, writer->config_.manifest_relative_path,
                 &existing_manifest, error_out))) {
            return false;
        }
        if ((existing_evidence &&
             (::fstat(existing_evidence->borrowed_fd(), &evidence_snapshot) != 0 ||
              !existing_evidence->VerifyCurrentBinding(error_out))) ||
            (existing_manifest &&
             (::fstat(existing_manifest->borrowed_fd(), &manifest_snapshot) != 0 ||
              !existing_manifest->VerifyCurrentBinding(error_out)))) {
            return fail(error_out,
                        "existing evidence namespace binding is not stable");
        }
        if (manifest_present && !evidence_present) {
            return fail(error_out,
                        "finalized manifest exists without its evidence JSONL");
        }
        if (manifest_present) {
            if (manifest_snapshot.st_dev == evidence_snapshot.st_dev &&
                manifest_snapshot.st_ino == evidence_snapshot.st_ino) {
                return fail(error_out,
                            "manifest and evidence resolve to the same inode");
            }
            std::string manifest_bytes;
            json manifest;
            if (!read_open_file_fd(
                    existing_manifest->borrowed_fd(),
                    kSpatialRoiRecorderManifestMaxFileBytes,
                    "existing finalized evidence manifest", &manifest_bytes,
                    error_out) ||
                !strict_json_from_bytes(
                    manifest_bytes, 1000000, &manifest,
                    "existing finalized evidence manifest", error_out) ||
                !validate_spatial_roi_recorder_finalized_manifest_authority(
                    *writer->artifact_root_, writer->binding_, manifest,
                    error_out) ||
                !validate_aggregate_evidence_budget(
                    manifest.at("artifacts"), manifest.at("evidence"),
                    manifest_bytes.size(), writer->binding_, error_out) ||
                !existing_manifest->VerifyCurrentBinding(error_out)) {
                return false;
            }
            EvidenceCounts counts;
            bool has_frames = false;
            std::size_t frame_count = 0;
            std::uint64_t first_recording = 0;
            std::uint64_t last_recording = 0;
            std::uint64_t first_roi = 0;
            std::uint64_t last_roi = 0;
            if (!manifest_counts_from_json(manifest.at("counts"), &counts,
                                           error_out) ||
                !manifest_ranges_from_json(
                    manifest.at("ranges"), &has_frames, &frame_count,
                    &first_recording, &last_recording, &first_roi, &last_roi,
                    error_out) ||
                frame_count > writer->config_.max_frames) {
                return fail(error_out,
                            "existing finalized evidence exceeds writer frame admission");
            }
            writer->staging_dir_fd_ = evidence_parent.release();
            writer->frame_count_ = frame_count;
            writer->has_frames_ = has_frames;
            writer->first_recording_frame_id_ = first_recording;
            writer->last_recording_frame_id_ = last_recording;
            writer->first_roi_stream_frame_index_ = first_roi;
            writer->last_roi_stream_frame_index_ = last_roi;
            writer->last_output_frame_index_ = counts.encoded_frames;
            writer->detach_successes_ = counts.detach_successes;
            writer->dispatch_admitted_ = counts.dispatch_admitted;
            writer->dispatch_rejected_ = counts.dispatch_rejected;
            writer->ack_attempted_ = counts.ack_attempted;
            writer->ack_sent_ = counts.ack_sent;
            writer->ack_accepted_ = counts.ack_accepted;
            writer->release_attempted_ = counts.release_attempted;
            writer->release_sent_ = counts.release_sent;
            writer->encoded_frames_ = counts.encoded_frames;
            writer->failed_frames_ = counts.failed_frames;
            writer->packet_count_ = counts.packet_count;
            writer->encoded_bytes_ = counts.encoded_bytes;
            writer->keyframes_ = counts.keyframes;
            writer->ack_write_failures_ = counts.ack_write_failures;
            writer->release_write_failures_ = counts.release_write_failures;
            writer->lifecycle_failures_ = counts.lifecycle_failures;
            writer->evidence_bytes_written_ =
                manifest.at("evidence").at("size_bytes").get<std::uint64_t>();
            writer->evidence_published_ = true;
            writer->terminal_written_ = true;
            writer->terminal_state_ =
                manifest.at("terminal").at("state").get<std::string>();
            writer->terminal_reason_ =
                manifest.at("terminal").at("reason").get<std::string>();
            writer->terminal_artifact_receipts_ = manifest.at("artifacts");
            writer->terminal_encoder_snapshot_ = manifest.at("encoder_terminal");
            writer->evidence_reference_ = manifest.at("evidence");
            writer->finalized_request_digest_ =
                manifest.at("finalize_request_sha256").get<std::string>();
            writer->finalized_manifest_ = std::move(manifest);
            writer->finalized_ = true;
            *writer_out = std::move(writer);
            if (error_out) error_out->clear();
            return true;
        }
        if (evidence_present) {
            EvidenceCounts counts;
            std::size_t frame_count = 0;
            std::uint64_t first_recording = 0;
            std::uint64_t last_recording = 0;
            std::uint64_t first_roi = 0;
            std::uint64_t last_roi = 0;
            std::string terminal_state;
            std::string terminal_reason;
            std::string request_digest;
            json artifact_receipts;
            json encoder_terminal;
            std::string expected_metadata_sha;
            std::uint64_t evidence_size = 0;
            std::string evidence_sha;
            if (!parse_evidence_stream_fd(
                    existing_evidence->borrowed_fd(), writer->binding_,
                    &frame_count,
                    &counts, &first_recording, &last_recording, &first_roi,
                    &last_roi, &terminal_state, &terminal_reason,
                    &request_digest, &artifact_receipts, &encoder_terminal,
                    &expected_metadata_sha, &evidence_size, &evidence_sha,
                    error_out) ||
                frame_count > writer->config_.max_frames) {
                return fail(error_out,
                            "existing evidence cannot be safely adopted");
            }
            NormalizedFinalizeRequest recovered;
            if (!validate_terminal_artifact_receipts(
                    artifact_receipts, writer->binding_, terminal_state,
                    &recovered, error_out)) {
                return false;
            }
            recovered.terminal_reason = terminal_reason;
            recovered.encoder_terminal = encoder_terminal;
            std::vector<OpenArtifact> opened_artifacts;
            json actual_receipts;
            if (!open_and_hash_finalize_artifacts(
                    *writer->artifact_root_, recovered, writer->binding_,
                    &opened_artifacts, error_out) ||
                !validate_opened_finalize_artifacts(
                    &opened_artifacts, recovered, writer->binding_, counts,
                    nullptr,
                    &actual_receipts, error_out) ||
                actual_receipts != artifact_receipts ||
                (terminal_state == kTerminalComplete &&
                 actual_receipts.at("metadata").at("sha256") !=
                     expected_metadata_sha) ||
                !existing_evidence->VerifyCurrentBinding(error_out)) {
                return fail(error_out,
                            "published evidence artifacts cannot be identically adopted");
            }
            for (const auto& artifact : opened_artifacts) {
                if (artifact.snapshot.st_dev == evidence_snapshot.st_dev &&
                    artifact.snapshot.st_ino == evidence_snapshot.st_ino) {
                    return fail(error_out,
                                "published evidence aliases a finalized artifact inode");
                }
            }
            writer->staging_dir_fd_ = evidence_parent.release();
            int adopted_evidence_fd = -1;
            if (!existing_evidence->DuplicateFd(&adopted_evidence_fd,
                                                error_out)) {
                return false;
            }
            writer->staging_fd_ = adopted_evidence_fd;
            writer->frame_count_ = frame_count;
            writer->has_frames_ = frame_count != 0;
            writer->first_recording_frame_id_ = first_recording;
            writer->last_recording_frame_id_ = last_recording;
            writer->first_roi_stream_frame_index_ = first_roi;
            writer->last_roi_stream_frame_index_ = last_roi;
            writer->last_output_frame_index_ = counts.encoded_frames;
            writer->detach_successes_ = counts.detach_successes;
            writer->dispatch_admitted_ = counts.dispatch_admitted;
            writer->dispatch_rejected_ = counts.dispatch_rejected;
            writer->ack_attempted_ = counts.ack_attempted;
            writer->ack_sent_ = counts.ack_sent;
            writer->ack_accepted_ = counts.ack_accepted;
            writer->release_attempted_ = counts.release_attempted;
            writer->release_sent_ = counts.release_sent;
            writer->encoded_frames_ = counts.encoded_frames;
            writer->failed_frames_ = counts.failed_frames;
            writer->packet_count_ = counts.packet_count;
            writer->encoded_bytes_ = counts.encoded_bytes;
            writer->keyframes_ = counts.keyframes;
            writer->ack_write_failures_ = counts.ack_write_failures;
            writer->release_write_failures_ = counts.release_write_failures;
            writer->lifecycle_failures_ = counts.lifecycle_failures;
            writer->evidence_bytes_written_ = evidence_size;
            writer->evidence_published_ = true;
            writer->terminal_written_ = true;
            writer->finalized_request_digest_ = request_digest;
            writer->terminal_state_ = terminal_state;
            writer->terminal_reason_ = terminal_reason;
            writer->terminal_artifact_receipts_ = artifact_receipts;
            writer->terminal_encoder_snapshot_ = encoder_terminal;
            writer->evidence_reference_ = {
                {"relative_path", writer->config_.evidence_relative_path},
                {"size_bytes", evidence_size},
                {"sha256", evidence_sha},
            };
            *writer_out = std::move(writer);
            if (error_out) error_out->clear();
            return true;
        }

        writer->staging_dir_fd_ = evidence_parent.release();
        const std::string header_bytes =
            header_json(writer->binding_).dump(-1, ' ', false,
                                               json::error_handler_t::strict) +
            "\n";
        Fd staging;
        if (!stage_bytes(writer->staging_dir_fd_, header_bytes,
                         writer->binding_.max_evidence_bytes_per_stream, &staging,
                         error_out)) {
            return false;
        }
        writer->evidence_bytes_written_ = header_bytes.size();
        writer->staging_fd_ = staging.release();
        *writer_out = std::move(writer);
        if (error_out) error_out->clear();
        return true;
    } catch (const std::exception& exception) {
        return fail(error_out, std::string("evidence writer open failed: ") +
                                   exception.what());
    } catch (...) {
        return fail(error_out, "evidence writer open failed: unknown exception");
    }
}

bool SpatialRoiRecorderEvidenceWriter::latch_failure(std::string message,
                                                     std::string* error_out)
{
    fatal_ = true;
    error_ = std::move(message);
    if (error_out) {
        *error_out = error_;
    }
    return false;
}

bool SpatialRoiRecorderEvidenceWriter::append_json_line(const json& value,
                                                        std::string* error_out)
{
    if (staging_fd_ < 0 || fatal_ || finalized_) {
        return latch_failure("evidence writer is not accepting records", error_out);
    }
    std::string line;
    try {
        line = value.dump(-1, ' ', false, json::error_handler_t::strict) + "\n";
    } catch (const std::exception& exception) {
        return latch_failure(std::string("evidence JSON serialization failed: ") +
                        exception.what(),
                    error_out);
    }
    if (line.size() > kSpatialRoiRecorderEvidenceMaxLineBytes) {
        return latch_failure("evidence JSONL line exceeds bounded size", error_out);
    }
    if (line.size() > binding_.max_evidence_bytes_per_stream ||
        evidence_bytes_written_ >
            binding_.max_evidence_bytes_per_stream - line.size()) {
        return latch_failure("evidence JSONL exceeds its bounded file size", error_out);
    }
    if (!write_all_fd(staging_fd_, line, error_out)) {
        fatal_ = true;
        error_ = error_out ? *error_out : "evidence JSONL write failed";
        return false;
    }
    evidence_bytes_written_ += line.size();
    return true;
}

bool SpatialRoiRecorderEvidenceWriter::AppendFrame(
    const SpatialRoiRecorderFrameEvidence& frame,
    std::string* error_out)
{
    if (fatal_ || finalized_ || terminal_written_ || staging_fd_ < 0) {
        return latch_failure("evidence writer is not accepting frames", error_out);
    }
    if (frame_count_ >= config_.max_frames) {
        return latch_failure("evidence frame bound exceeded", error_out);
    }
    if (!validate_frame_input(binding_, frame, error_out, nullptr)) {
        fatal_ = true;
        error_ = error_out ? *error_out : "invalid frame evidence";
        return false;
    }
    if (has_frames_ &&
        (frame.frame.recording_frame_id <= last_recording_frame_id_ ||
         last_roi_stream_frame_index_ == std::numeric_limits<std::uint64_t>::max() ||
         frame.frame.roi_stream_frame_index != last_roi_stream_frame_index_ + 1)) {
        return latch_failure("duplicate or out-of-order frame evidence", error_out);
    }
    if (!has_frames_) {
        if (frame.frame.roi_stream_frame_index != 1) {
            return latch_failure(
                "first evidence frame must use one-based ROI frame index 1",
                error_out);
        }
        first_recording_frame_id_ = frame.frame.recording_frame_id;
        first_roi_stream_frame_index_ = frame.frame.roi_stream_frame_index;
        has_frames_ = true;
    }
    EvidenceCounts counts_after;
    if (!validate_frame_input(binding_, frame, error_out, &counts_after)) {
        fatal_ = true;
        error_ = error_out ? *error_out : "invalid frame evidence";
        return false;
    }
    if (frame.encode_status == kEncodeEncoded) {
        if (last_output_frame_index_ == std::numeric_limits<std::uint64_t>::max() ||
            frame.output_frame_index != last_output_frame_index_ + 1) {
            return latch_failure(
                "encoded output frame indices must be dense and one-based",
                error_out);
        }
    } else if (frame.output_frame_index != 0) {
        return latch_failure("nonencoded frame cannot carry an output index",
                             error_out);
    }
    EvidenceCounts totals;
    totals.detach_successes = detach_successes_;
    totals.dispatch_admitted = dispatch_admitted_;
    totals.dispatch_rejected = dispatch_rejected_;
    totals.ack_attempted = ack_attempted_;
    totals.ack_sent = ack_sent_;
    totals.ack_accepted = ack_accepted_;
    totals.release_attempted = release_attempted_;
    totals.release_sent = release_sent_;
    totals.encoded_frames = encoded_frames_;
    totals.failed_frames = failed_frames_;
    totals.packet_count = packet_count_;
    totals.encoded_bytes = encoded_bytes_;
    totals.keyframes = keyframes_;
    totals.ack_write_failures = ack_write_failures_;
    totals.release_write_failures = release_write_failures_;
    totals.lifecycle_failures = lifecycle_failures_;
    if (!increment(&totals.detach_successes, counts_after.detach_successes) ||
        !increment(&totals.dispatch_admitted, counts_after.dispatch_admitted) ||
        !increment(&totals.dispatch_rejected, counts_after.dispatch_rejected) ||
        !increment(&totals.ack_attempted, counts_after.ack_attempted) ||
        !increment(&totals.ack_sent, counts_after.ack_sent) ||
        !increment(&totals.ack_accepted, counts_after.ack_accepted) ||
        !increment(&totals.release_attempted, counts_after.release_attempted) ||
        !increment(&totals.release_sent, counts_after.release_sent) ||
        !increment(&totals.ack_write_failures, counts_after.ack_write_failures) ||
        !increment(&totals.release_write_failures,
                   counts_after.release_write_failures) ||
        !increment(&totals.lifecycle_failures, counts_after.lifecycle_failures) ||
        !increment(&totals.encoded_frames, counts_after.encoded_frames) ||
        !increment(&totals.failed_frames, counts_after.failed_frames) ||
        !increment(&totals.packet_count, counts_after.packet_count) ||
        !increment(&totals.encoded_bytes, counts_after.encoded_bytes) ||
        !increment(&totals.keyframes, counts_after.keyframes)) {
        return latch_failure("evidence aggregate counter overflow", error_out);
    }
    if (!append_json_line(frame_evidence_json(frame, frame_count_ + 1), error_out)) {
        return false;
    }
    if (frame.encode_status == kEncodeEncoded) {
        if (!metadata_digest_) {
            return latch_failure(
                "metadata projection digest is unavailable", error_out);
        }
        metadata_digest_->hasher.update(
            metadata_csv_row(frame.frame, frame.output_frame_index));
    }
    last_recording_frame_id_ = frame.frame.recording_frame_id;
    last_roi_stream_frame_index_ = frame.frame.roi_stream_frame_index;
    if (frame.encode_status == kEncodeEncoded) {
        last_output_frame_index_ = frame.output_frame_index;
    }
    ++frame_count_;
    detach_successes_ = totals.detach_successes;
    dispatch_admitted_ = totals.dispatch_admitted;
    dispatch_rejected_ = totals.dispatch_rejected;
    ack_attempted_ = totals.ack_attempted;
    ack_sent_ = totals.ack_sent;
    ack_accepted_ = totals.ack_accepted;
    release_attempted_ = totals.release_attempted;
    release_sent_ = totals.release_sent;
    encoded_frames_ = totals.encoded_frames;
    failed_frames_ = totals.failed_frames;
    packet_count_ = totals.packet_count;
    encoded_bytes_ = totals.encoded_bytes;
    keyframes_ = totals.keyframes;
    ack_write_failures_ = totals.ack_write_failures;
    release_write_failures_ = totals.release_write_failures;
    lifecycle_failures_ = totals.lifecycle_failures;
    if (error_out) error_out->clear();
    return true;
}

bool SpatialRoiRecorderEvidenceWriter::Finalize(
    const SpatialRoiRecorderFinalizeRequest& request,
    json* manifest_out,
    std::string* error_out)
{
    NormalizedFinalizeRequest normalized;
    std::string normalize_error;
    if (!normalize_finalize_request(request, binding_, &normalized,
                                    &normalize_error)) {
        return latch_failure(normalize_error.empty()
                                 ? "invalid finalization request"
                                 : normalize_error,
                             error_out);
    }
    const std::string request_digest =
        canonical_json_sha256(normalized_finalize_json(normalized));
    if (finalized_) {
        if (request_digest != finalized_request_digest_) {
            return latch_failure("finalization is already sealed with different inputs", error_out);
        }
        if (manifest_out) *manifest_out = finalized_manifest_;
        if (error_out) error_out->clear();
        return true;
    }
    if (normalized.terminal_state == kTerminalComplete &&
        !request.video_sanity_result) {
        return latch_failure(
            "complete finalization requires the descriptor-bound decoder-probe capability",
            error_out);
    }
    if (fatal_ || (staging_fd_ < 0 && !evidence_published_)) {
        return latch_failure(error_.empty() ? "evidence writer is not usable" : error_, error_out);
    }
    const auto latch_existing_error = [&](const char* fallback) {
        return latch_failure(
            error_out && !error_out->empty() ? *error_out : std::string(fallback),
            error_out);
    };
    if (normalized.terminal_state == kTerminalComplete &&
        (!has_frames_ || failed_frames_ != 0)) {
        return latch_failure("complete finalization requires successful evidence for every frame",
                    error_out);
    }
    EvidenceCounts counts;
    counts.detach_successes = detach_successes_;
    counts.dispatch_admitted = dispatch_admitted_;
    counts.dispatch_rejected = dispatch_rejected_;
    counts.ack_attempted = ack_attempted_;
    counts.ack_sent = ack_sent_;
    counts.ack_accepted = ack_accepted_;
    counts.release_attempted = release_attempted_;
    counts.release_sent = release_sent_;
    counts.encoded_frames = encoded_frames_;
    counts.failed_frames = failed_frames_;
    counts.packet_count = packet_count_;
    counts.encoded_bytes = encoded_bytes_;
    counts.keyframes = keyframes_;
    counts.ack_write_failures = ack_write_failures_;
    counts.release_write_failures = release_write_failures_;
    counts.lifecycle_failures = lifecycle_failures_;
    if (normalized.terminal_state == kTerminalComplete) {
        const auto& snapshot_counts = request.encoder_terminal_snapshot->counts;
        const std::uint64_t gop_length =
            encode_profile_gop_length(binding_.encode_profile);
        const std::uint64_t required_keyframes =
            expected_keyframe_count(frame_count_, gop_length);
        if (snapshot_counts.enqueued != frame_count_ ||
            snapshot_counts.encoded_frames != counts.encoded_frames ||
            snapshot_counts.encoded_packets != counts.packet_count ||
            snapshot_counts.encoded_bytes != counts.encoded_bytes ||
            counts.encoded_frames != frame_count_ ||
            counts.packet_count != frame_count_ || gop_length == 0 ||
            counts.keyframes != required_keyframes) {
            return latch_failure(
                "encoder terminal snapshot does not exactly match frame evidence",
                error_out);
        }
    }
    std::vector<OpenArtifact> opened_artifacts;
    json artifact_receipts;
    if (!artifact_root_ ||
        !open_and_hash_finalize_artifacts(*artifact_root_, normalized, binding_,
                                          &opened_artifacts, error_out)) {
        return latch_existing_error("could not hash finalization artifacts");
    }
    if (normalized.terminal_state == kTerminalComplete &&
        !evidence_published_) {
        OpenArtifact* metadata =
            find_open_artifact(&opened_artifacts, "metadata");
        if (!metadata_digest_ || !metadata) {
            return latch_failure(
                "metadata projection digest/proof is unavailable", error_out);
        }
        auto expected_hasher = metadata_digest_->hasher;
        const std::string expected_sha =
            "sha256:" + expected_hasher.final_hex();
        if (metadata->reference.at("sha256") != expected_sha) {
            return latch_failure(
                "metadata CSV bytes do not exactly match frame evidence",
                error_out);
        }
    }
    if (!validate_opened_finalize_artifacts(
            &opened_artifacts, normalized, binding_, counts,
            request.video_sanity_result.get(),
            &artifact_receipts, error_out)) {
        return latch_existing_error("could not validate finalization artifacts");
    }
    if (evidence_published_) {
        if (request_digest != finalized_request_digest_ ||
            artifact_receipts != terminal_artifact_receipts_ ||
            normalized.encoder_terminal != terminal_encoder_snapshot_) {
            return latch_failure(
                "published evidence can only adopt byte-identical finalization inputs",
                error_out);
        }
    }
    const bool publish_evidence_now = !evidence_published_;
    if (publish_evidence_now) {
        if (!append_json_line(
                terminal_json(normalized.terminal_state,
                              normalized.terminal_reason, request_digest,
                              artifact_receipts, normalized.encoder_terminal,
                              binding_, frame_count_, counts, has_frames_,
                              first_recording_frame_id_, last_recording_frame_id_,
                              first_roi_stream_frame_index_,
                              last_roi_stream_frame_index_),
                error_out)) {
            return false;
        }
        terminal_written_ = true;
        if (::fsync(staging_fd_) != 0) {
            return latch_failure("evidence staging final fsync failed: " +
                                     std::string(std::strerror(errno)),
                                 error_out);
        }
        std::uint64_t evidence_size = 0;
        std::string evidence_sha;
        if (!hash_open_file_fd(staging_fd_,
                               binding_.max_evidence_bytes_per_stream,
                               config_.evidence_relative_path, &evidence_size,
                               &evidence_sha, nullptr, error_out)) {
            return latch_existing_error("could not hash terminal evidence JSONL");
        }
        evidence_reference_ = {
            {"relative_path", config_.evidence_relative_path},
            {"size_bytes", evidence_size},
            {"sha256", evidence_sha},
        };
    } else if (evidence_reference_.empty()) {
        return latch_failure("adopted evidence reference is unavailable", error_out);
    }
    json manifest = {
        {"schema_id", kSpatialRoiRecorderManifestSchemaId},
        {"schema_version", kSpatialRoiRecorderManifestSchemaVersion},
        {"canonicalization", kSpatialRoiRecorderCanonicalization},
        {"stream_kind", kSpatialRoiRecorderFixedRegionKind},
        {"binding", spatial_roi_recorder_evidence_binding_to_json(binding_)},
        {"evidence", evidence_reference_},
        {"artifacts", artifact_receipts},
        {"counts", counts_json(counts)},
        {"ranges", ranges_json(has_frames_, frame_count_, first_recording_frame_id_,
                                last_recording_frame_id_, first_roi_stream_frame_index_,
                                last_roi_stream_frame_index_)},
        {"terminal", {
            {"state", normalized.terminal_state},
            {"reason", normalized.terminal_reason},
        }},
        {"encoder_terminal", normalized.encoder_terminal},
        {"finalize_request_sha256", request_digest},
    };
    manifest["finalized_receipt_digest"] = "";
    json receipt_payload = manifest;
    receipt_payload.erase("finalized_receipt_digest");
    manifest["finalized_receipt_digest"] = canonical_json_sha256(receipt_payload);
    std::string manifest_bytes;
    try {
        manifest_bytes = manifest.dump(2, ' ', false, json::error_handler_t::strict) + "\n";
    } catch (const std::exception& exception) {
        return latch_failure(std::string("manifest serialization failed: ") + exception.what(),
                    error_out);
    }
    if (manifest_bytes.size() > kSpatialRoiRecorderManifestMaxFileBytes) {
        return latch_failure("finalized manifest exceeds bounded size", error_out);
    }
    if (!validate_aggregate_evidence_budget(
            artifact_receipts, evidence_reference_, manifest_bytes.size(),
            binding_, error_out)) {
        return latch_existing_error(
            "finalized artifacts exceed aggregate evidence budget");
    }
    if (publish_evidence_now) {
        if (!publish_staged_no_replace(staging_dir_fd_, staging_fd_,
                                       evidence_leaf_, nullptr, error_out)) {
            return latch_existing_error("could not publish evidence JSONL");
        }
        evidence_published_ = true;
        finalized_request_digest_ = request_digest;
        terminal_state_ = normalized.terminal_state;
        terminal_reason_ = normalized.terminal_reason;
        terminal_artifact_receipts_ = artifact_receipts;
        terminal_encoder_snapshot_ = normalized.encoder_terminal;
    }
    if (!validate_spatial_roi_recorder_finalized_manifest_authority(
            *artifact_root_, binding_, manifest, error_out) ||
        !publish_bytes(root_fd_, config_.manifest_relative_path, manifest_bytes,
                       kSpatialRoiRecorderManifestMaxFileBytes, error_out)) {
        return latch_existing_error("could not validate or publish finalized manifest");
    }
    finalized_manifest_ = manifest;
    finalized_request_digest_ = request_digest;
    finalized_ = true;
    if (manifest_out) *manifest_out = manifest;
    if (error_out) error_out->clear();
    return true;
}

bool validate_spatial_roi_recorder_finalized_manifest_authority(
    const SpatialRoiRecorderArtifactRoot& artifact_root,
    const SpatialRoiRecorderEvidenceBinding& expected_binding,
    const json& manifest,
    std::string* error_out)
{
    // Keep the parser exact to the emitted version rather than silently
    // accepting future manifest members.
    // emitted version rather than silently accepting future manifest members.
    static constexpr std::array<std::string_view, 13> emitted_keys = {
        "schema_id", "schema_version", "canonicalization", "stream_kind", "binding",
        "evidence", "artifacts", "counts", "ranges", "terminal",
        "encoder_terminal", "finalize_request_sha256",
        "finalized_receipt_digest"};
    if (!exact_keys(manifest, emitted_keys, "manifest", error_out) ||
        !read_const_string(manifest, "schema_id", kSpatialRoiRecorderManifestSchemaId,
                           "manifest", error_out) ||
        !read_const_int(manifest, "schema_version",
                        kSpatialRoiRecorderManifestSchemaVersion,
                        "manifest", error_out) ||
        !read_const_string(manifest, "canonicalization", kSpatialRoiRecorderCanonicalization,
                           "manifest", error_out) ||
        !read_const_string(manifest, "stream_kind", kSpatialRoiRecorderFixedRegionKind,
                           "manifest", error_out) ||
        !validate_sha_field(manifest, "finalized_receipt_digest", "manifest", error_out)) {
        return false;
    }
    (void)kManifestKeys;
    (void)kManifestKeysRest;
    (void)kManifestKeysWithCounts;
    (void)kContractKeys;
    (void)kPlanKeys;
    (void)kRasterKeys;
    (void)kAuthorityKeys;
    SpatialRoiRecorderEvidenceBinding binding;
    if (!parse_binding_json(manifest.at("binding"), &binding, error_out)) {
        return false;
    }
    if (!binding_json_equal(binding, expected_binding)) {
        return fail(error_out,
                    "manifest binding does not match authenticated recorder binding");
    }
    json receipt_payload = manifest;
    const std::string supplied_digest =
        manifest.at("finalized_receipt_digest").get<std::string>();
    receipt_payload.erase("finalized_receipt_digest");
    if (canonical_json_sha256(receipt_payload) != supplied_digest) {
        return fail(error_out, "manifest finalized receipt digest does not verify");
    }
    static constexpr std::array<std::string_view, 3> reference_keys = {
        "relative_path", "size_bytes", "sha256"};
    if (!exact_keys(manifest.at("evidence"), reference_keys,
                    "manifest.evidence", error_out)) {
        return false;
    }
    const std::string evidence_path =
        manifest.at("evidence").at("relative_path").get<std::string>();
    std::uint64_t expected_evidence_size = 0;
    if (evidence_path != binding.expected_artifacts.at("evidence") ||
        !read_u64(manifest.at("evidence"), "size_bytes",
                  &expected_evidence_size, "manifest.evidence", error_out) ||
        expected_evidence_size > binding.max_evidence_bytes_per_stream ||
        !validate_sha_field(manifest.at("evidence"), "sha256",
                            "manifest.evidence", error_out)) {
        return fail(error_out,
                    "manifest evidence reference does not match contract");
    }
    const json& terminal = manifest.at("terminal");
    static constexpr std::array<std::string_view, 2> terminal_keys = {
        "state", "reason"};
    std::string manifest_state;
    std::string manifest_reason;
    if (!exact_keys(terminal, terminal_keys, "manifest.terminal", error_out) ||
        !read_string(terminal, "state", &manifest_state, "manifest.terminal",
                     error_out, 32) ||
        !read_string(terminal, "reason", &manifest_reason, "manifest.terminal",
                     error_out, kMaxReasonBytes) ||
        (manifest_state != kTerminalComplete && manifest_state != kTerminalFailed)) {
        return false;
    }
    NormalizedFinalizeRequest normalized;
    if (!validate_terminal_artifact_receipts(manifest.at("artifacts"), binding,
                                             manifest_state, &normalized,
                                             error_out)) {
        return false;
    }
    normalized.terminal_reason = manifest_reason;
    if (manifest.at("encoder_terminal").is_null()) {
        if (manifest_state == kTerminalComplete) {
            return fail(error_out,
                        "complete manifest lacks encoder terminal snapshot");
        }
    } else if (!validate_encoder_terminal_projection(
                   manifest.at("encoder_terminal"),
                   manifest_state == kTerminalComplete, nullptr, error_out)) {
        return false;
    }
    normalized.encoder_terminal = manifest.at("encoder_terminal");
    if (!validate_sha_field(manifest, "finalize_request_sha256", "manifest",
                            error_out) ||
        manifest.at("finalize_request_sha256").get<std::string>() !=
            canonical_json_sha256(normalized_finalize_json(normalized))) {
        return fail(error_out,
                    "manifest finalization request digest does not verify");
    }
    std::string canonical_manifest_bytes;
    try {
        canonical_manifest_bytes =
            manifest.dump(2, ' ', false, json::error_handler_t::strict) + "\n";
    } catch (const std::exception& exception) {
        return fail(error_out,
                    std::string("manifest budget serialization failed: ") +
                        exception.what());
    }
    if (canonical_manifest_bytes.size() >
            kSpatialRoiRecorderManifestMaxFileBytes ||
        !validate_aggregate_evidence_budget(
            manifest.at("artifacts"), manifest.at("evidence"),
            canonical_manifest_bytes.size(), binding, error_out)) {
        return false;
    }
    std::vector<OpenArtifact> opened_artifacts;
    json actual_artifact_receipts;
    if (!open_and_hash_finalize_artifacts(artifact_root, normalized, binding,
                                          &opened_artifacts, error_out)) {
        return false;
    }
    for (const auto& artifact : opened_artifacts) {
        if (manifest.at("artifacts").at(artifact.kind) != artifact.reference) {
            return fail(error_out,
                        "manifest artifact receipt does not match stable file bytes");
        }
    }
    EvidenceCounts evidence_counts;
    std::size_t evidence_frame_count = 0;
    std::uint64_t evidence_first_recording = 0;
    std::uint64_t evidence_last_recording = 0;
    std::uint64_t evidence_first_roi = 0;
    std::uint64_t evidence_last_roi = 0;
    std::string evidence_terminal_state;
    std::string evidence_terminal_reason;
    std::string evidence_request_digest;
    json evidence_artifact_receipts;
    json evidence_encoder_terminal;
    std::string expected_metadata_sha;
    std::uint64_t actual_evidence_size = 0;
    std::string actual_evidence_sha;
    std::unique_ptr<SpatialRoiRecorderArtifactFile> evidence_file;
    if (!open_existing_artifact(artifact_root, evidence_path, &evidence_file,
                                error_out) ||
        !parse_evidence_stream_fd(
            evidence_file->borrowed_fd(), binding, &evidence_frame_count,
            &evidence_counts,
            &evidence_first_recording, &evidence_last_recording, &evidence_first_roi,
            &evidence_last_roi, &evidence_terminal_state, &evidence_terminal_reason,
            &evidence_request_digest, &evidence_artifact_receipts,
            &evidence_encoder_terminal, &expected_metadata_sha,
            &actual_evidence_size,
            &actual_evidence_sha, error_out)) {
        return false;
    }
    if (actual_evidence_size != expected_evidence_size ||
        actual_evidence_sha != manifest.at("evidence").at("sha256") ||
        evidence_request_digest != manifest.at("finalize_request_sha256") ||
        evidence_artifact_receipts != manifest.at("artifacts") ||
        evidence_encoder_terminal != manifest.at("encoder_terminal") ||
        (manifest_state == kTerminalComplete &&
         manifest.at("artifacts").at("metadata").at("sha256") !=
             expected_metadata_sha)) {
        return fail(error_out,
                    "manifest does not match the stable parsed evidence bytes");
    }
    EvidenceCounts manifest_counts;
    if (!manifest_counts_from_json(manifest.at("counts"), &manifest_counts, error_out) ||
        counts_json(manifest_counts) != counts_json(evidence_counts)) {
        return fail(error_out, "manifest counts do not match evidence JSONL");
    }
    bool manifest_has_frames = false;
    std::size_t manifest_frame_count = 0;
    std::uint64_t manifest_first_recording = 0;
    std::uint64_t manifest_last_recording = 0;
    std::uint64_t manifest_first_roi = 0;
    std::uint64_t manifest_last_roi = 0;
    if (!manifest_ranges_from_json(manifest.at("ranges"), &manifest_has_frames,
                                   &manifest_frame_count, &manifest_first_recording,
                                   &manifest_last_recording, &manifest_first_roi,
                                   &manifest_last_roi, error_out) ||
        manifest_frame_count != evidence_frame_count ||
        manifest_has_frames != (evidence_frame_count != 0) ||
        manifest_first_recording != evidence_first_recording ||
        manifest_last_recording != evidence_last_recording ||
        manifest_first_roi != evidence_first_roi || manifest_last_roi != evidence_last_roi) {
        return fail(error_out, "manifest ranges do not match evidence JSONL");
    }
    if (manifest_state != evidence_terminal_state ||
        manifest_reason != evidence_terminal_reason) {
        return fail(error_out,
                    "manifest terminal state/reason disagrees with evidence JSONL");
    }
    if (!manifest.at("encoder_terminal").is_null() &&
        !validate_encoder_terminal_projection(
            manifest.at("encoder_terminal"),
            manifest_state == kTerminalComplete, &evidence_counts,
            error_out)) {
        return false;
    }
    json validated_artifact_receipts;
    if (!validate_opened_finalize_artifacts(
            &opened_artifacts, normalized, binding, evidence_counts,
            nullptr,
            &validated_artifact_receipts, error_out) ||
        validated_artifact_receipts != manifest.at("artifacts")) {
        return fail(error_out,
                    "manifest artifact receipts changed during validation");
    }
    struct stat evidence_snapshot {};
    if (::fstat(evidence_file->borrowed_fd(), &evidence_snapshot) != 0 ||
        !evidence_file->VerifyCurrentBinding(error_out)) {
        return fail(error_out, "could not stat validated evidence JSONL");
    }
    for (const auto& artifact : opened_artifacts) {
        if (artifact.snapshot.st_dev == evidence_snapshot.st_dev &&
            artifact.snapshot.st_ino == evidence_snapshot.st_ino) {
            return fail(error_out,
                        "evidence JSONL aliases a finalized artifact inode");
        }
    }
    return true;
}

bool validate_spatial_roi_recorder_finalized_manifest(
    const std::shared_ptr<SpatialRoiRecorderArtifactRoot>& artifact_root,
    const SpatialRoiRecorderEvidenceBinding& expected_binding,
    const json& manifest,
    std::string* error_out)
{
    if (!validate_spatial_roi_recorder_evidence_binding(expected_binding,
                                                        error_out) ||
        !validate_artifact_root_authority(artifact_root, expected_binding,
                                          error_out)) {
        return fail(error_out,
                    "manifest validation authority does not match authenticated binding");
    }
    return validate_spatial_roi_recorder_finalized_manifest_authority(
        *artifact_root, expected_binding, manifest, error_out);
}

bool read_and_validate_spatial_roi_recorder_finalized_manifest(
    const std::shared_ptr<SpatialRoiRecorderArtifactRoot>& artifact_root,
    const std::string& manifest_relative_path,
    const SpatialRoiRecorderEvidenceBinding& expected_binding,
    json* manifest_out,
    std::string* error_out)
{
    if (!manifest_out ||
        !validate_spatial_roi_recorder_evidence_binding(expected_binding,
                                                        error_out) ||
        !validate_artifact_root_authority(artifact_root, expected_binding,
                                          error_out) ||
        !safe_relative_path(manifest_relative_path) ||
        manifest_relative_path !=
            expected_binding.expected_artifacts.at("evidence_manifest")) {
        return fail(error_out, "manifest read path is invalid");
    }
    std::unique_ptr<SpatialRoiRecorderArtifactFile> file;
    if (!open_existing_artifact(*artifact_root, manifest_relative_path, &file,
                                error_out)) {
        return false;
    }
    std::string bytes;
    json value;
    if (!read_open_file_fd(file->borrowed_fd(),
                           kSpatialRoiRecorderManifestMaxFileBytes,
                           "finalized evidence manifest", &bytes, error_out) ||
        !strict_json_from_bytes(bytes, 1000000, &value,
                                "finalized evidence manifest", error_out)) {
        return false;
    }
    if (value.is_object() && value.contains("evidence") &&
        value.at("evidence").is_object() &&
        value.at("evidence").value("relative_path", "") == manifest_relative_path) {
        return fail(error_out, "manifest path collides with its evidence path");
    }
    if (value.is_object() && value.contains("artifacts") &&
        value.at("artifacts").is_object()) {
        for (const auto& [kind, reference] : value.at("artifacts").items()) {
            (void)kind;
            if (reference.is_object() &&
                reference.value("relative_path", "") == manifest_relative_path) {
                return fail(error_out, "manifest path collides with an artifact path");
            }
        }
    }
    if (!validate_spatial_roi_recorder_finalized_manifest_authority(
            *artifact_root, expected_binding, value, error_out) ||
        !validate_aggregate_evidence_budget(
            value.at("artifacts"), value.at("evidence"), bytes.size(),
            expected_binding, error_out) ||
        !file->VerifyCurrentBinding(error_out)) {
        return false;
    }
    *manifest_out = std::move(value);
    return true;
}

}  // namespace orange::spatial_roi::recording
