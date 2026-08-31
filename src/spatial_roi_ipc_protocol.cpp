#include "spatial_roi_ipc_protocol.h"

#include "shaman_v2_recording_identity.h"

#include <algorithm>
#include <cctype>
#include <limits>
#include <set>
#include <sstream>
#include <type_traits>
#include <utility>

namespace orange::spatial_roi::ipc {
namespace {

using json = nlohmann::json;

constexpr std::size_t kMaxRecordingIdBytes = 512;
constexpr std::size_t kMaxIdentifierBytes = 64;

bool fail(std::string* error_out, const std::string& message)
{
    if (error_out) {
        *error_out = message;
    }
    return false;
}

bool exact_keys(const json& value,
                const std::set<std::string>& required,
                const std::string& path,
                std::string* error_out)
{
    if (!value.is_object()) {
        return fail(error_out, path + " must be an object");
    }
    for (const std::string& key : required) {
        if (!value.contains(key)) {
            return fail(error_out, path + "." + key + " is required");
        }
    }
    for (auto it = value.begin(); it != value.end(); ++it) {
        if (required.count(it.key()) == 0) {
            return fail(error_out,
                        path + "." + it.key() +
                            " is not allowed by spatial ROI IPC v2");
        }
    }
    return true;
}

bool is_ascii_alnum(const unsigned char value)
{
    return (value >= 'a' && value <= 'z') ||
           (value >= 'A' && value <= 'Z') ||
           (value >= '0' && value <= '9');
}

bool is_safe_identifier(const std::string& value,
                        const std::size_t max_length = kMaxIdentifierBytes)
{
    if (value.empty() || value.size() > max_length ||
        !is_ascii_alnum(static_cast<unsigned char>(value.front()))) {
        return false;
    }
    return std::all_of(
        value.begin() + 1,
        value.end(),
        [](const unsigned char ch) {
            return is_ascii_alnum(ch) || ch == '_' || ch == '-' || ch == '.';
        });
}

bool is_printable_text(const std::string& value,
                       const std::size_t max_length,
                       const bool allow_empty = true)
{
    if ((!allow_empty && value.empty()) || value.size() > max_length) {
        return false;
    }
    return std::none_of(
        value.begin(),
        value.end(),
        [](const unsigned char ch) { return ch < 0x20 || ch == 0x7f; });
}

bool is_recording_id(const std::string& value)
{
    return !value.empty() && value.size() <= kMaxRecordingIdBytes &&
           std::isspace(static_cast<unsigned char>(value.front())) == 0 &&
           std::isspace(static_cast<unsigned char>(value.back())) == 0 &&
           is_printable_text(value, kMaxRecordingIdBytes, false);
}

bool recording_token_matches(const std::string& recording_id,
                             const std::string& token)
{
    try {
        return token == shaman_v2_recording_identity::token_for_recording_id(
                             recording_id);
    } catch (const std::exception&) {
        // Malformed UTF-8 or another serialization failure is untrusted wire
        // input, not a reason for the protocol parser to throw.
        return false;
    }
}

bool is_sha256(const std::string& value)
{
    if (value.size() != 71 || value.compare(0, 7, "sha256:") != 0) {
        return false;
    }
    return std::all_of(
        value.begin() + 7,
        value.end(),
        [](const unsigned char ch) {
            return (ch >= '0' && ch <= '9') ||
                   (ch >= 'a' && ch <= 'f');
        });
}

std::string expected_logical_stream_id(const std::string& camera_serial,
                                       const std::string& roi_id)
{
    return camera_serial + "_spatial_roi_" + roi_id;
}

bool is_nonnegative_u64(const json& value, std::uint64_t* out)
{
    if (!out || value.is_boolean() ||
        (!value.is_number_unsigned() && !value.is_number_integer())) {
        return false;
    }
    if (value.is_number_unsigned()) {
        *out = value.get<std::uint64_t>();
        return true;
    }
    const auto signed_value = value.get<std::int64_t>();
    if (signed_value < 0) {
        return false;
    }
    *out = static_cast<std::uint64_t>(signed_value);
    return true;
}

bool read_string(const json& object,
                 const char* key,
                 const std::string& path,
                 std::string* out,
                 std::string* error_out)
{
    const json& value = object.at(key);
    if (!value.is_string()) {
        return fail(error_out, path + "." + key + " must be a string");
    }
    *out = value.get<std::string>();
    return true;
}

bool read_u64(const json& object,
              const char* key,
              const std::string& path,
              std::uint64_t* out,
              std::string* error_out,
              const bool positive = false)
{
    std::uint64_t parsed = 0;
    if (!is_nonnegative_u64(object.at(key), &parsed) ||
        (positive && parsed == 0)) {
        return fail(error_out,
                    path + "." + key +
                        (positive ? " must be a positive integer"
                                  : " must be a non-negative integer"));
    }
    *out = parsed;
    return true;
}

bool read_u32(const json& object,
              const char* key,
              const std::string& path,
              std::uint32_t* out,
              std::string* error_out,
              const bool positive = false)
{
    std::uint64_t value = 0;
    if (!read_u64(object, key, path, &value, error_out, positive)) {
        return false;
    }
    if (value > std::numeric_limits<std::uint32_t>::max()) {
        return fail(error_out, path + "." + key + " exceeds uint32 capacity");
    }
    *out = static_cast<std::uint32_t>(value);
    return true;
}

bool read_int(const json& object,
              const char* key,
              const std::string& path,
              int* out,
              std::string* error_out)
{
    std::uint64_t value = 0;
    if (!read_u64(object, key, path, &value, error_out) ||
        value > static_cast<std::uint64_t>(std::numeric_limits<int>::max())) {
        if (value > static_cast<std::uint64_t>(std::numeric_limits<int>::max())) {
            return fail(error_out, path + "." + key + " exceeds int capacity");
        }
        return false;
    }
    *out = static_cast<int>(value);
    return true;
}

bool stream_identity_equal(const SpatialRoiIpcStreamIdentity& lhs,
                           const SpatialRoiIpcStreamIdentity& rhs)
{
    return lhs.recording_id == rhs.recording_id &&
           lhs.recording_identity_token == rhs.recording_identity_token &&
           lhs.producer_generation == rhs.producer_generation &&
           lhs.camera_id == rhs.camera_id &&
           lhs.camera_serial == rhs.camera_serial &&
           lhs.roi_id == rhs.roi_id &&
           lhs.region_id == rhs.region_id &&
           lhs.arena_group_id == rhs.arena_group_id &&
           lhs.arena_id == rhs.arena_id &&
           lhs.logical_stream_id == rhs.logical_stream_id &&
           lhs.spatial_roi_plan_sha256 == rhs.spatial_roi_plan_sha256;
}

json stream_identity_to_json(const SpatialRoiIpcStreamIdentity& identity)
{
    return { {"recording_id", identity.recording_id},
             {"recording_identity_token", identity.recording_identity_token},
             {"producer_generation", identity.producer_generation},
             {"camera_id", identity.camera_id},
             {"camera_serial", identity.camera_serial},
             {"roi_id", identity.roi_id},
             {"region_id", identity.region_id},
             {"arena_group_id", identity.arena_group_id},
             {"arena_id", identity.arena_id},
             {"logical_stream_id", identity.logical_stream_id},
             {"spatial_roi_plan_sha256", identity.spatial_roi_plan_sha256} };
}

bool stream_identity_from_json(const json& value,
                               SpatialRoiIpcStreamIdentity* out,
                               const std::string& path,
                               std::string* error_out)
{
    static const std::set<std::string> keys = {
        "recording_id", "recording_identity_token", "producer_generation",
        "camera_id", "camera_serial", "roi_id", "region_id",
        "arena_group_id", "arena_id", "logical_stream_id",
        "spatial_roi_plan_sha256"};
    if (!out || !exact_keys(value, keys, path, error_out)) {
        return out ? false : fail(error_out, path + " destination is null");
    }
    SpatialRoiIpcStreamIdentity parsed;
    if (!read_string(value, "recording_id", path, &parsed.recording_id, error_out) ||
        !read_string(value,
                     "recording_identity_token",
                     path,
                     &parsed.recording_identity_token,
                     error_out) ||
        !read_string(value,
                     "producer_generation",
                     path,
                     &parsed.producer_generation,
                     error_out) ||
        !read_int(value, "camera_id", path, &parsed.camera_id, error_out) ||
        !read_string(value, "camera_serial", path, &parsed.camera_serial, error_out) ||
        !read_string(value, "roi_id", path, &parsed.roi_id, error_out) ||
        !read_string(value, "region_id", path, &parsed.region_id, error_out) ||
        !read_string(value,
                     "arena_group_id",
                     path,
                     &parsed.arena_group_id,
                     error_out) ||
        !read_string(value, "arena_id", path, &parsed.arena_id, error_out) ||
        !read_string(value,
                     "logical_stream_id",
                     path,
                     &parsed.logical_stream_id,
                     error_out) ||
        !read_string(value,
                     "spatial_roi_plan_sha256",
                     path,
                     &parsed.spatial_roi_plan_sha256,
                     error_out) ||
        !validate_spatial_roi_ipc_stream_identity(parsed, error_out)) {
        return false;
    }
    *out = std::move(parsed);
    return true;
}

json correlation_to_json(const SpatialRoiIpcCorrelation& correlation)
{
    return { {"stream", stream_identity_to_json(correlation.stream)},
             {"local_frame_id", correlation.local_frame_id},
             {"camera_frame_id", correlation.camera_frame_id},
             {"recording_frame_id", correlation.recording_frame_id},
             {"roi_stream_frame_index", correlation.roi_stream_frame_index} };
}

bool correlation_from_json(const json& value,
                           SpatialRoiIpcCorrelation* out,
                           const std::string& path,
                           std::string* error_out)
{
    static const std::set<std::string> keys = {
        "stream", "local_frame_id", "camera_frame_id", "recording_frame_id",
        "roi_stream_frame_index"};
    if (!out || !exact_keys(value, keys, path, error_out)) {
        return out ? false : fail(error_out, path + " destination is null");
    }
    SpatialRoiIpcCorrelation parsed;
    if (!stream_identity_from_json(
            value.at("stream"), &parsed.stream, path + ".stream", error_out) ||
        !read_u64(value, "local_frame_id", path, &parsed.local_frame_id, error_out, true) ||
        !read_u64(value, "camera_frame_id", path, &parsed.camera_frame_id, error_out, true) ||
        !read_u64(value,
                  "recording_frame_id",
                  path,
                  &parsed.recording_frame_id,
                  error_out,
                  true) ||
        !read_u64(value,
                  "roi_stream_frame_index",
                  path,
                  &parsed.roi_stream_frame_index,
                  error_out,
                  true) ||
        !validate_spatial_roi_ipc_correlation(parsed, error_out)) {
        return false;
    }
    *out = std::move(parsed);
    return true;
}

json cuda_buffer_to_json(const SpatialRoiCudaIpcBuffer& buffer)
{
    return { {"memory_handle_encoding", buffer.memory_handle_encoding},
             {"memory_handle_hex", buffer.memory_handle_hex},
             {"ready_event_handle_encoding", buffer.ready_event_handle_encoding},
             {"ready_event_handle_hex", buffer.ready_event_handle_hex},
             {"byte_offset", buffer.byte_offset},
             {"byte_length", buffer.byte_length},
             {"row_pitch_bytes", buffer.row_pitch_bytes},
             {"pixel_format", buffer.pixel_format},
             {"layout", buffer.layout} };
}

bool cuda_buffer_from_json(const json& value,
                           SpatialRoiCudaIpcBuffer* out,
                           const std::string& path,
                           std::string* error_out)
{
    static const std::set<std::string> keys = {
        "memory_handle_encoding", "memory_handle_hex",
        "ready_event_handle_encoding", "ready_event_handle_hex",
        "byte_offset", "byte_length", "row_pitch_bytes", "pixel_format",
        "layout"};
    if (!out || !exact_keys(value, keys, path, error_out)) {
        return out ? false : fail(error_out, path + " destination is null");
    }
    SpatialRoiCudaIpcBuffer parsed;
    if (!read_string(value,
                     "memory_handle_encoding",
                     path,
                     &parsed.memory_handle_encoding,
                     error_out) ||
        !read_string(value,
                     "memory_handle_hex",
                     path,
                     &parsed.memory_handle_hex,
                     error_out) ||
        !read_string(value,
                     "ready_event_handle_encoding",
                     path,
                     &parsed.ready_event_handle_encoding,
                     error_out) ||
        !read_string(value,
                     "ready_event_handle_hex",
                     path,
                     &parsed.ready_event_handle_hex,
                     error_out) ||
        !read_u64(value, "byte_offset", path, &parsed.byte_offset, error_out) ||
        !read_u64(value, "byte_length", path, &parsed.byte_length, error_out, true) ||
        !read_u64(value,
                  "row_pitch_bytes",
                  path,
                  &parsed.row_pitch_bytes,
                  error_out,
                  true) ||
        !read_string(value, "pixel_format", path, &parsed.pixel_format, error_out) ||
        !read_string(value, "layout", path, &parsed.layout, error_out)) {
        return false;
    }
    *out = std::move(parsed);
    return true;
}

bool handle_hex_is_canonical(const std::string& value)
{
    if (value.size() != kSpatialRoiCudaIpcHandleHexBytes) {
        return false;
    }
    return std::all_of(value.begin(), value.end(), [](const unsigned char ch) {
        return (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f');
    });
}

bool role_is_valid(const std::string& role)
{
    return role == kSpatialRoiIpcProducerRole ||
           role == kSpatialRoiIpcRecorderRole;
}

bool fixed_lower_hex(const std::string& value, const std::size_t length)
{
    return value.size() == length &&
           std::all_of(value.begin(), value.end(), [](const unsigned char ch) {
               return (ch >= '0' && ch <= '9') ||
                      (ch >= 'a' && ch <= 'f');
           });
}

bool drain_state_is_valid(const std::string& state)
{
    return state == kSpatialRoiIpcDrainStateDraining ||
           state == kSpatialRoiIpcDrainStateDrained ||
           state == kSpatialRoiIpcDrainStateFailed;
}

bool finalize_state_is_valid(const std::string& state)
{
    return state == kSpatialRoiIpcFinalizeStateFinalized ||
           state == kSpatialRoiIpcFinalizeStateFailed;
}

bool features_are_valid(const std::vector<std::string>& features,
                        std::string* error_out)
{
    if (features.size() > kSpatialRoiIpcMaxFeatures) {
        return fail(error_out, "HELLO features exceeds the protocol limit");
    }
    std::set<std::string> unique;
    for (const std::string& feature : features) {
        if (!is_safe_identifier(feature) || !unique.insert(feature).second) {
            return fail(error_out, "HELLO features must be unique safe identifiers");
        }
    }
    return true;
}

bool message_kind_and_payload(const json& value,
                              const char* expected_kind,
                              const json** payload_out,
                              std::string* error_out)
{
    static const std::set<std::string> keys = {
        "protocol", "version", "kind", "payload"};
    if (!exact_keys(value, keys, "message", error_out)) {
        return false;
    }
    if (!value.at("protocol").is_string() ||
        value.at("protocol").get<std::string>() != kSpatialRoiIpcProtocolName) {
        return fail(error_out, "message.protocol is not spatial ROI IPC v2");
    }
    std::uint64_t version = 0;
    if (!is_nonnegative_u64(value.at("version"), &version) ||
        version != static_cast<std::uint64_t>(kSpatialRoiIpcProtocolVersion)) {
        return fail(error_out, "message.version must be 2");
    }
    if (!value.at("kind").is_string() ||
        value.at("kind").get<std::string>() != expected_kind) {
        return fail(error_out, "message.kind does not match the expected message type");
    }
    if (!value.at("payload").is_object()) {
        return fail(error_out, "message.payload must be an object");
    }
    *payload_out = &value.at("payload");
    return true;
}

json message_envelope(const char* kind, const json& payload)
{
    return { {"protocol", kSpatialRoiIpcProtocolName},
             {"version", kSpatialRoiIpcProtocolVersion},
             {"kind", kind},
             {"payload", payload} };
}

bool parse_hello_payload(const json& payload,
                         SpatialRoiIpcHello* out,
                         std::string* error_out)
{
    static const std::set<std::string> keys = {
        "stream", "role", "queue_capacity_frames", "features"};
    if (!out || !exact_keys(payload, keys, "HELLO.payload", error_out)) {
        return out ? false : fail(error_out, "HELLO destination is null");
    }
    SpatialRoiIpcHello parsed;
    if (!stream_identity_from_json(
            payload.at("stream"), &parsed.stream, "HELLO.payload.stream", error_out) ||
        !read_string(payload, "role", "HELLO.payload", &parsed.role, error_out) ||
        !read_u32(payload,
                  "queue_capacity_frames",
                  "HELLO.payload",
                  &parsed.queue_capacity_frames,
                  error_out,
                  true)) {
        return false;
    }
    const json& features = payload.at("features");
    if (!features.is_array() || features.size() > kSpatialRoiIpcMaxFeatures) {
        return fail(error_out, "HELLO.payload.features must be a bounded array");
    }
    for (std::size_t i = 0; i < features.size(); ++i) {
        if (!features.at(i).is_string()) {
            return fail(error_out,
                        "HELLO.payload.features[" + std::to_string(i) +
                            "] must be a string");
        }
        parsed.features.push_back(features.at(i).get<std::string>());
    }
    if (!validate_spatial_roi_ipc_hello(parsed, error_out)) {
        return false;
    }
    *out = std::move(parsed);
    return true;
}

bool parse_frame_payload(const json& payload,
                         SpatialRoiIpcFrame* out,
                         std::string* error_out)
{
    static const std::set<std::string> keys = {"descriptor", "cuda_buffer"};
    if (!out || !exact_keys(payload, keys, "FRAME.payload", error_out)) {
        return out ? false : fail(error_out, "FRAME destination is null");
    }
    SpatialRoiIpcFrame parsed;
    try {
        if (!spatial_roi_frame_descriptor_from_json(
                payload.at("descriptor"), &parsed.descriptor, error_out) ||
            !cuda_buffer_from_json(payload.at("cuda_buffer"),
                                   &parsed.cuda_buffer,
                                   "FRAME.payload.cuda_buffer",
                                   error_out) ||
            !validate_spatial_roi_ipc_frame(parsed, error_out)) {
            return false;
        }
    } catch (const std::exception& exception) {
        return fail(error_out,
                    std::string("FRAME payload validation failed: ") +
                        exception.what());
    }
    *out = std::move(parsed);
    return true;
}

bool parse_ack_payload(const json& payload,
                       SpatialRoiIpcAck* out,
                       std::string* error_out)
{
    static const std::set<std::string> keys = {"correlation", "accepted", "reason"};
    if (!out || !exact_keys(payload, keys, "ACK.payload", error_out)) {
        return out ? false : fail(error_out, "ACK destination is null");
    }
    SpatialRoiIpcAck parsed;
    if (!correlation_from_json(
            payload.at("correlation"), &parsed.correlation, "ACK.payload.correlation", error_out) ||
        !payload.at("accepted").is_boolean()) {
        return payload.at("accepted").is_boolean()
                   ? false
                   : fail(error_out, "ACK.payload.accepted must be a boolean");
    }
    parsed.accepted = payload.at("accepted").get<bool>();
    if (!read_string(payload, "reason", "ACK.payload", &parsed.reason, error_out) ||
        !validate_spatial_roi_ipc_ack(parsed, error_out)) {
        return false;
    }
    *out = std::move(parsed);
    return true;
}

bool parse_release_payload(const json& payload,
                           SpatialRoiIpcRelease* out,
                           std::string* error_out)
{
    static const std::set<std::string> keys = {"correlation", "reason"};
    if (!out || !exact_keys(payload, keys, "RELEASE.payload", error_out)) {
        return out ? false : fail(error_out, "RELEASE destination is null");
    }
    SpatialRoiIpcRelease parsed;
    if (!correlation_from_json(payload.at("correlation"),
                               &parsed.correlation,
                               "RELEASE.payload.correlation",
                               error_out) ||
        !read_string(payload, "reason", "RELEASE.payload", &parsed.reason, error_out) ||
        !validate_spatial_roi_ipc_release(parsed, error_out)) {
        return false;
    }
    *out = std::move(parsed);
    return true;
}

bool parse_terminal_error_payload(const json& payload,
                                  SpatialRoiIpcTerminalError* out,
                                  std::string* error_out)
{
    static const std::set<std::string> keys = {
        "stream", "error_code", "message", "fatal", "correlation"};
    if (!out || !exact_keys(payload, keys, "TERMINAL_ERROR.payload", error_out)) {
        return out ? false : fail(error_out, "TERMINAL_ERROR destination is null");
    }
    SpatialRoiIpcTerminalError parsed;
    if (!stream_identity_from_json(payload.at("stream"),
                                   &parsed.stream,
                                   "TERMINAL_ERROR.payload.stream",
                                   error_out) ||
        !read_string(payload,
                     "error_code",
                     "TERMINAL_ERROR.payload",
                     &parsed.error_code,
                     error_out) ||
        !read_string(payload,
                     "message",
                     "TERMINAL_ERROR.payload",
                     &parsed.message,
                     error_out) ||
        !payload.at("fatal").is_boolean()) {
        return payload.at("fatal").is_boolean()
                   ? false
                   : fail(error_out,
                          "TERMINAL_ERROR.payload.fatal must be a boolean");
    }
    parsed.fatal = payload.at("fatal").get<bool>();
    if (!payload.at("correlation").is_null()) {
        SpatialRoiIpcCorrelation correlation;
        if (!correlation_from_json(payload.at("correlation"),
                                   &correlation,
                                   "TERMINAL_ERROR.payload.correlation",
                                   error_out)) {
            return false;
        }
        parsed.correlation = std::move(correlation);
    }
    if (!validate_spatial_roi_ipc_terminal_error(parsed, error_out)) {
        return false;
    }
    *out = std::move(parsed);
    return true;
}

bool parse_drain_request_payload(const json& payload,
                                 SpatialRoiIpcDrainRequest* out,
                                 std::string* error_out)
{
    static const std::set<std::string> keys = {
        "stream", "sender_role", "drain_sequence", "reason"};
    if (!out || !exact_keys(payload, keys, "DRAIN_REQUEST.payload", error_out)) {
        return out ? false : fail(error_out, "DRAIN_REQUEST destination is null");
    }
    SpatialRoiIpcDrainRequest parsed;
    if (!stream_identity_from_json(payload.at("stream"),
                                   &parsed.stream,
                                   "DRAIN_REQUEST.payload.stream",
                                   error_out) ||
        !read_string(payload,
                     "sender_role",
                     "DRAIN_REQUEST.payload",
                     &parsed.sender_role,
                     error_out) ||
        !read_u64(payload,
                  "drain_sequence",
                  "DRAIN_REQUEST.payload",
                  &parsed.drain_sequence,
                  error_out,
                  true) ||
        !read_string(payload,
                     "reason",
                     "DRAIN_REQUEST.payload",
                     &parsed.reason,
                     error_out) ||
        !validate_spatial_roi_ipc_drain_request(parsed, error_out)) {
        return false;
    }
    *out = std::move(parsed);
    return true;
}

bool parse_drain_status_payload(const json& payload,
                                SpatialRoiIpcDrainStatus* out,
                                std::string* error_out)
{
    static const std::set<std::string> keys = {"stream",
                                               "sender_role",
                                               "drain_sequence",
                                               "status_sequence",
                                               "state",
                                               "reason"};
    if (!out || !exact_keys(payload, keys, "DRAIN_STATUS.payload", error_out)) {
        return out ? false : fail(error_out, "DRAIN_STATUS destination is null");
    }
    SpatialRoiIpcDrainStatus parsed;
    if (!stream_identity_from_json(payload.at("stream"),
                                   &parsed.stream,
                                   "DRAIN_STATUS.payload.stream",
                                   error_out) ||
        !read_string(payload,
                     "sender_role",
                     "DRAIN_STATUS.payload",
                     &parsed.sender_role,
                     error_out) ||
        !read_u64(payload,
                  "drain_sequence",
                  "DRAIN_STATUS.payload",
                  &parsed.drain_sequence,
                  error_out,
                  true) ||
        !read_u64(payload,
                  "status_sequence",
                  "DRAIN_STATUS.payload",
                  &parsed.status_sequence,
                  error_out,
                  true) ||
        !read_string(payload,
                     "state",
                     "DRAIN_STATUS.payload",
                     &parsed.state,
                     error_out) ||
        !read_string(payload,
                     "reason",
                     "DRAIN_STATUS.payload",
                     &parsed.reason,
                     error_out) ||
        !validate_spatial_roi_ipc_drain_status(parsed, error_out)) {
        return false;
    }
    *out = std::move(parsed);
    return true;
}

bool parse_finalize_request_payload(const json& payload,
                                    SpatialRoiIpcFinalizeRequest* out,
                                    std::string* error_out)
{
    static const std::set<std::string> keys = {
        "stream", "sender_role", "drain_sequence", "finalize_nonce", "reason"};
    if (!out ||
        !exact_keys(payload, keys, "FINALIZE_REQUEST.payload", error_out)) {
        return out ? false
                   : fail(error_out, "FINALIZE_REQUEST destination is null");
    }
    SpatialRoiIpcFinalizeRequest parsed;
    if (!stream_identity_from_json(payload.at("stream"),
                                   &parsed.stream,
                                   "FINALIZE_REQUEST.payload.stream",
                                   error_out) ||
        !read_string(payload,
                     "sender_role",
                     "FINALIZE_REQUEST.payload",
                     &parsed.sender_role,
                     error_out) ||
        !read_u64(payload,
                  "drain_sequence",
                  "FINALIZE_REQUEST.payload",
                  &parsed.drain_sequence,
                  error_out,
                  true) ||
        !read_string(payload,
                     "finalize_nonce",
                     "FINALIZE_REQUEST.payload",
                     &parsed.finalize_nonce,
                     error_out) ||
        !read_string(payload,
                     "reason",
                     "FINALIZE_REQUEST.payload",
                     &parsed.reason,
                     error_out) ||
        !validate_spatial_roi_ipc_finalize_request(parsed, error_out)) {
        return false;
    }
    *out = std::move(parsed);
    return true;
}

bool parse_finalize_status_payload(const json& payload,
                                   SpatialRoiIpcFinalizeStatus* out,
                                   std::string* error_out)
{
    static const std::set<std::string> keys = {"stream",
                                               "sender_role",
                                               "drain_sequence",
                                               "finalize_nonce",
                                               "status_sequence",
                                               "state",
                                               "reason"};
    if (!out ||
        !exact_keys(payload, keys, "FINALIZE_STATUS.payload", error_out)) {
        return out ? false
                   : fail(error_out, "FINALIZE_STATUS destination is null");
    }
    SpatialRoiIpcFinalizeStatus parsed;
    if (!stream_identity_from_json(payload.at("stream"),
                                   &parsed.stream,
                                   "FINALIZE_STATUS.payload.stream",
                                   error_out) ||
        !read_string(payload,
                     "sender_role",
                     "FINALIZE_STATUS.payload",
                     &parsed.sender_role,
                     error_out) ||
        !read_u64(payload,
                  "drain_sequence",
                  "FINALIZE_STATUS.payload",
                  &parsed.drain_sequence,
                  error_out,
                  true) ||
        !read_string(payload,
                     "finalize_nonce",
                     "FINALIZE_STATUS.payload",
                     &parsed.finalize_nonce,
                     error_out) ||
        !read_u64(payload,
                  "status_sequence",
                  "FINALIZE_STATUS.payload",
                  &parsed.status_sequence,
                  error_out,
                  true) ||
        !read_string(payload,
                     "state",
                     "FINALIZE_STATUS.payload",
                     &parsed.state,
                     error_out) ||
        !read_string(payload,
                     "reason",
                     "FINALIZE_STATUS.payload",
                     &parsed.reason,
                     error_out) ||
        !validate_spatial_roi_ipc_finalize_status(parsed, error_out)) {
        return false;
    }
    *out = std::move(parsed);
    return true;
}

}  // namespace

std::size_t SpatialRoiIpcCorrelationKeyHash::operator()(
    const SpatialRoiIpcCorrelationKey& key) const noexcept
{
    auto combine = [](std::size_t seed, std::size_t value) {
        return seed ^ (value + static_cast<std::size_t>(0x9e3779b9) +
                       (seed << 6) + (seed >> 2));
    };
    std::size_t hash =
        std::hash<std::string>{}(key.recording_identity_token);
    hash = combine(
        hash, std::hash<std::string>{}(key.producer_generation));
    hash = combine(hash, std::hash<std::string>{}(key.logical_stream_id));
    hash = combine(
        hash, std::hash<std::uint64_t>{}(key.recording_frame_id));
    return combine(
        hash, std::hash<std::uint64_t>{}(key.roi_stream_frame_index));
}

SpatialRoiIpcStreamIdentity spatial_roi_ipc_stream_identity_from_descriptor(
    const SpatialRoiFrameDescriptor& descriptor)
{
    return {descriptor.recording_id,
            descriptor.recording_identity_token,
            descriptor.producer_generation,
            descriptor.camera_id,
            descriptor.camera_serial,
            descriptor.roi_id,
            descriptor.region_id,
            descriptor.arena_group_id,
            descriptor.arena_id,
            descriptor.logical_stream_id,
            descriptor.spatial_roi_plan_sha256};
}

SpatialRoiIpcCorrelation spatial_roi_ipc_correlation_from_descriptor(
    const SpatialRoiFrameDescriptor& descriptor)
{
    return {spatial_roi_ipc_stream_identity_from_descriptor(descriptor),
            descriptor.local_frame_id,
            descriptor.camera_frame_id,
            descriptor.recording_frame_id,
            descriptor.roi_stream_frame_index};
}

bool validate_spatial_roi_ipc_stream_identity(
    const SpatialRoiIpcStreamIdentity& identity,
    std::string* error_out)
{
    if (!is_recording_id(identity.recording_id)) {
        return fail(error_out, "stream identity recording_id is invalid");
    }
    if (!is_sha256(identity.recording_identity_token) ||
        !recording_token_matches(identity.recording_id,
                                 identity.recording_identity_token)) {
        return fail(error_out,
                    "stream identity recording_identity_token does not match recording_id");
    }
    if (!is_safe_identifier(identity.producer_generation)) {
        return fail(error_out, "stream identity producer_generation is invalid");
    }
    if (identity.camera_id < 0 || !is_safe_identifier(identity.camera_serial) ||
        !is_safe_identifier(identity.roi_id) ||
        !is_safe_identifier(identity.region_id) ||
        !is_safe_identifier(identity.arena_group_id) ||
        (!identity.arena_id.empty() && !is_safe_identifier(identity.arena_id)) ||
        !is_safe_identifier(identity.logical_stream_id)) {
        return fail(error_out, "stream identity contains an invalid identifier");
    }
    if (identity.logical_stream_id !=
        expected_logical_stream_id(identity.camera_serial, identity.roi_id)) {
        return fail(error_out,
                    "stream identity logical_stream_id is not canonical");
    }
    if (!is_sha256(identity.spatial_roi_plan_sha256)) {
        return fail(error_out,
                    "stream identity spatial_roi_plan_sha256 is invalid");
    }
    if (error_out) {
        error_out->clear();
    }
    return true;
}

bool validate_spatial_roi_ipc_correlation(
    const SpatialRoiIpcCorrelation& correlation,
    std::string* error_out)
{
    if (!validate_spatial_roi_ipc_stream_identity(correlation.stream, error_out)) {
        return false;
    }
    if (correlation.local_frame_id == 0 || correlation.camera_frame_id == 0 ||
        correlation.recording_frame_id == 0 ||
        correlation.roi_stream_frame_index == 0) {
        return fail(error_out,
                    "correlation frame ids and ROI stream index must be positive");
    }
    if (error_out) {
        error_out->clear();
    }
    return true;
}

bool spatial_roi_ipc_correlation_matches_descriptor(
    const SpatialRoiIpcCorrelation& correlation,
    const SpatialRoiFrameDescriptor& descriptor,
    std::string* error_out)
{
    if (!validate_spatial_roi_frame_descriptor(descriptor, error_out)) {
        return false;
    }
    if (!validate_spatial_roi_ipc_correlation(correlation, error_out)) {
        return false;
    }
    const SpatialRoiIpcCorrelation expected =
        spatial_roi_ipc_correlation_from_descriptor(descriptor);
    if (correlation.stream.recording_id != expected.stream.recording_id ||
        correlation.stream.recording_identity_token !=
            expected.stream.recording_identity_token ||
        correlation.stream.producer_generation != expected.stream.producer_generation ||
        correlation.stream.camera_id != expected.stream.camera_id ||
        correlation.stream.camera_serial != expected.stream.camera_serial ||
        correlation.stream.roi_id != expected.stream.roi_id ||
        correlation.stream.region_id != expected.stream.region_id ||
        correlation.stream.arena_group_id != expected.stream.arena_group_id ||
        correlation.stream.arena_id != expected.stream.arena_id ||
        correlation.stream.logical_stream_id != expected.stream.logical_stream_id ||
        correlation.stream.spatial_roi_plan_sha256 !=
            expected.stream.spatial_roi_plan_sha256 ||
        correlation.local_frame_id != expected.local_frame_id ||
        correlation.camera_frame_id != expected.camera_frame_id ||
        correlation.recording_frame_id != expected.recording_frame_id ||
        correlation.roi_stream_frame_index != expected.roi_stream_frame_index) {
        return fail(error_out,
                    "IPC correlation does not exactly match frame descriptor identity");
    }
    if (error_out) {
        error_out->clear();
    }
    return true;
}

bool validate_spatial_roi_ipc_buffer(
    const SpatialRoiCudaIpcBuffer& buffer,
    const SpatialRoiFrameDescriptor& descriptor,
    std::string* error_out)
{
    if (!validate_spatial_roi_frame_descriptor(descriptor, error_out)) {
        return false;
    }
    if (buffer.memory_handle_encoding != kSpatialRoiCudaIpcHandleEncoding ||
        buffer.ready_event_handle_encoding != kSpatialRoiCudaIpcHandleEncoding ||
        !handle_hex_is_canonical(buffer.memory_handle_hex) ||
        !handle_hex_is_canonical(buffer.ready_event_handle_hex)) {
        return fail(error_out,
                    "CUDA IPC handles must use fixed lower-case 64-byte hex encoding");
    }
    if (buffer.pixel_format != kSpatialRoiMono8PixelFormat ||
        buffer.layout != "packed_row_major") {
        return fail(error_out,
                    "CUDA IPC buffer must be a packed row-major Mono8 raster");
    }
    // Every schema-v1 ROI output owns a dedicated cudaMalloc allocation and
    // begins at that allocation's exported base.  A nonzero offset cannot be
    // bounds-checked from an opaque CUDA IPC handle and would no longer match
    // the verified producer contract.
    if (buffer.byte_offset != 0) {
        return fail(error_out,
                    "CUDA IPC schema v2 requires a zero byte_offset");
    }
    if (descriptor.bytes == 0 ||
        descriptor.bytes > kSpatialRoiIpcMaxPackedMono8Bytes ||
        buffer.byte_length != descriptor.bytes ||
        buffer.row_pitch_bytes != descriptor.encoded_raster.width) {
        return fail(error_out,
                    "CUDA IPC byte span must exactly describe the packed encoded raster");
    }
    if (buffer.byte_offset > std::numeric_limits<std::uint64_t>::max() -
                                  buffer.byte_length) {
        return fail(error_out, "CUDA IPC byte span overflows uint64");
    }
    if (error_out) {
        error_out->clear();
    }
    return true;
}

bool validate_spatial_roi_ipc_hello(const SpatialRoiIpcHello& hello,
                                    std::string* error_out)
{
    if (!validate_spatial_roi_ipc_stream_identity(hello.stream, error_out)) {
        return false;
    }
    if (!role_is_valid(hello.role)) {
        return fail(error_out, "HELLO role must be producer or recorder");
    }
    if (hello.queue_capacity_frames == 0 ||
        hello.queue_capacity_frames > kSpatialRoiIpcMaxQueueFrames) {
        return fail(error_out,
                    "HELLO queue_capacity_frames is outside the bounded range");
    }
    if (!features_are_valid(hello.features, error_out)) {
        return false;
    }
    if (error_out) {
        error_out->clear();
    }
    return true;
}

bool validate_spatial_roi_ipc_frame(const SpatialRoiIpcFrame& frame,
                                    std::string* error_out)
{
    return validate_spatial_roi_ipc_buffer(
        frame.cuda_buffer, frame.descriptor, error_out);
}

bool validate_spatial_roi_ipc_ack(const SpatialRoiIpcAck& ack,
                                  std::string* error_out)
{
    if (!validate_spatial_roi_ipc_correlation(ack.correlation, error_out)) {
        return false;
    }
    if (!is_printable_text(ack.reason, kSpatialRoiIpcMaxTextBytes)) {
        return fail(error_out, "ACK reason contains invalid text");
    }
    if (!ack.accepted && ack.reason.empty()) {
        return fail(error_out, "rejected ACK requires a non-empty reason");
    }
    if (error_out) {
        error_out->clear();
    }
    return true;
}

bool validate_spatial_roi_ipc_release(const SpatialRoiIpcRelease& release,
                                      std::string* error_out)
{
    if (!validate_spatial_roi_ipc_correlation(release.correlation, error_out)) {
        return false;
    }
    if (!is_printable_text(release.reason, kSpatialRoiIpcMaxTextBytes, false)) {
        return fail(error_out, "RELEASE reason must be non-empty printable text");
    }
    if (error_out) {
        error_out->clear();
    }
    return true;
}

bool validate_spatial_roi_ipc_terminal_error(
    const SpatialRoiIpcTerminalError& terminal_error,
    std::string* error_out)
{
    if (!validate_spatial_roi_ipc_stream_identity(terminal_error.stream, error_out)) {
        return false;
    }
    if (!is_safe_identifier(terminal_error.error_code)) {
        return fail(error_out, "TERMINAL_ERROR error_code is invalid");
    }
    if (!is_printable_text(terminal_error.message,
                           kSpatialRoiIpcMaxTextBytes,
                           false)) {
        return fail(error_out, "TERMINAL_ERROR message must be non-empty printable text");
    }
    if (!terminal_error.fatal) {
        return fail(error_out, "TERMINAL_ERROR fatal must be true");
    }
    if (terminal_error.correlation) {
        if (!validate_spatial_roi_ipc_correlation(*terminal_error.correlation,
                                                  error_out)) {
            return false;
        }
        if (!stream_identity_equal(terminal_error.stream,
                                   terminal_error.correlation->stream)) {
            return fail(error_out,
                        "TERMINAL_ERROR correlation stream does not match stream identity");
        }
    }
    if (error_out) {
        error_out->clear();
    }
    return true;
}

bool validate_spatial_roi_ipc_drain_request(
    const SpatialRoiIpcDrainRequest& request,
    std::string* error_out)
{
    if (!validate_spatial_roi_ipc_stream_identity(request.stream, error_out)) {
        return false;
    }
    if (request.sender_role != kSpatialRoiIpcProducerRole) {
        return fail(error_out,
                    "DRAIN_REQUEST sender_role must be producer");
    }
    if (request.drain_sequence == 0) {
        return fail(error_out,
                    "DRAIN_REQUEST drain_sequence must be positive");
    }
    if (!is_printable_text(request.reason, kSpatialRoiIpcMaxTextBytes, false)) {
        return fail(error_out,
                    "DRAIN_REQUEST reason must be non-empty printable text");
    }
    if (error_out) {
        error_out->clear();
    }
    return true;
}

bool validate_spatial_roi_ipc_drain_status(
    const SpatialRoiIpcDrainStatus& status,
    std::string* error_out)
{
    if (!validate_spatial_roi_ipc_stream_identity(status.stream, error_out)) {
        return false;
    }
    if (status.sender_role != kSpatialRoiIpcRecorderRole) {
        return fail(error_out,
                    "DRAIN_STATUS sender_role must be recorder");
    }
    if (status.drain_sequence == 0 || status.status_sequence == 0) {
        return fail(error_out,
                    "DRAIN_STATUS sequences must be positive");
    }
    if (!drain_state_is_valid(status.state)) {
        return fail(error_out,
                    "DRAIN_STATUS state is not a supported drain state");
    }
    if (!is_printable_text(status.reason, kSpatialRoiIpcMaxTextBytes, false)) {
        return fail(error_out,
                    "DRAIN_STATUS reason must be non-empty printable text");
    }
    if (error_out) {
        error_out->clear();
    }
    return true;
}

bool validate_spatial_roi_ipc_finalize_request(
    const SpatialRoiIpcFinalizeRequest& request,
    std::string* error_out)
{
    if (!validate_spatial_roi_ipc_stream_identity(request.stream, error_out)) {
        return false;
    }
    if (request.sender_role != kSpatialRoiIpcProducerRole) {
        return fail(error_out,
                    "FINALIZE_REQUEST sender_role must be producer");
    }
    if (request.drain_sequence == 0) {
        return fail(error_out,
                    "FINALIZE_REQUEST drain_sequence must be positive");
    }
    if (!fixed_lower_hex(request.finalize_nonce,
                         kSpatialRoiIpcFinalizeNonceHexBytes)) {
        return fail(error_out,
                    "FINALIZE_REQUEST finalize_nonce must be 32 lower-case hex characters");
    }
    if (!is_printable_text(request.reason, kSpatialRoiIpcMaxTextBytes, false)) {
        return fail(error_out,
                    "FINALIZE_REQUEST reason must be non-empty printable text");
    }
    if (error_out) {
        error_out->clear();
    }
    return true;
}

bool validate_spatial_roi_ipc_finalize_status(
    const SpatialRoiIpcFinalizeStatus& status,
    std::string* error_out)
{
    if (!validate_spatial_roi_ipc_stream_identity(status.stream, error_out)) {
        return false;
    }
    if (status.sender_role != kSpatialRoiIpcRecorderRole) {
        return fail(error_out,
                    "FINALIZE_STATUS sender_role must be recorder");
    }
    if (status.drain_sequence == 0 || status.status_sequence == 0) {
        return fail(error_out,
                    "FINALIZE_STATUS sequences must be positive");
    }
    if (!fixed_lower_hex(status.finalize_nonce,
                         kSpatialRoiIpcFinalizeNonceHexBytes)) {
        return fail(error_out,
                    "FINALIZE_STATUS finalize_nonce must be 32 lower-case hex characters");
    }
    if (!finalize_state_is_valid(status.state)) {
        return fail(error_out,
                    "FINALIZE_STATUS state is not a supported finalize state");
    }
    if (!is_printable_text(status.reason, kSpatialRoiIpcMaxTextBytes, false)) {
        return fail(error_out,
                    "FINALIZE_STATUS reason must be non-empty printable text");
    }
    if (error_out) {
        error_out->clear();
    }
    return true;
}

nlohmann::json spatial_roi_ipc_message_to_json(
    const SpatialRoiIpcMessage& message)
{
    return std::visit(
        [](const auto& payload) -> json {
            using T = std::decay_t<decltype(payload)>;
            if constexpr (std::is_same_v<T, SpatialRoiIpcHello>) {
                return message_envelope(
                    kSpatialRoiIpcHelloKind,
                    { {"stream", stream_identity_to_json(payload.stream)},
                      {"role", payload.role},
                      {"queue_capacity_frames", payload.queue_capacity_frames},
                      {"features", payload.features} });
            } else if constexpr (std::is_same_v<T, SpatialRoiIpcFrame>) {
                return message_envelope(
                    kSpatialRoiIpcFrameKind,
                    { {"descriptor",
                       spatial_roi_frame_descriptor_to_json(payload.descriptor)},
                      {"cuda_buffer", cuda_buffer_to_json(payload.cuda_buffer)} });
            } else if constexpr (std::is_same_v<T, SpatialRoiIpcAck>) {
                return message_envelope(
                    kSpatialRoiIpcAckKind,
                    { {"correlation", correlation_to_json(payload.correlation)},
                      {"accepted", payload.accepted},
                      {"reason", payload.reason} });
            } else if constexpr (std::is_same_v<T, SpatialRoiIpcRelease>) {
                return message_envelope(
                    kSpatialRoiIpcReleaseKind,
                    { {"correlation", correlation_to_json(payload.correlation)},
                      {"reason", payload.reason} });
            } else if constexpr (std::is_same_v<T, SpatialRoiIpcTerminalError>) {
                return message_envelope(
                    kSpatialRoiIpcTerminalErrorKind,
                    { {"stream", stream_identity_to_json(payload.stream)},
                      {"error_code", payload.error_code},
                      {"message", payload.message},
                      {"fatal", payload.fatal},
                      {"correlation", payload.correlation
                                                ? correlation_to_json(*payload.correlation)
                                                : json(nullptr)} });
            } else if constexpr (std::is_same_v<T, SpatialRoiIpcDrainRequest>) {
                return message_envelope(
                    kSpatialRoiIpcDrainRequestKind,
                    { {"stream", stream_identity_to_json(payload.stream)},
                      {"sender_role", payload.sender_role},
                      {"drain_sequence", payload.drain_sequence},
                      {"reason", payload.reason} });
            } else if constexpr (std::is_same_v<T, SpatialRoiIpcDrainStatus>) {
                return message_envelope(
                    kSpatialRoiIpcDrainStatusKind,
                    { {"stream", stream_identity_to_json(payload.stream)},
                      {"sender_role", payload.sender_role},
                      {"drain_sequence", payload.drain_sequence},
                      {"status_sequence", payload.status_sequence},
                      {"state", payload.state},
                      {"reason", payload.reason} });
            } else if constexpr (std::is_same_v<T, SpatialRoiIpcFinalizeRequest>) {
                return message_envelope(
                    kSpatialRoiIpcFinalizeRequestKind,
                    { {"stream", stream_identity_to_json(payload.stream)},
                      {"sender_role", payload.sender_role},
                      {"drain_sequence", payload.drain_sequence},
                      {"finalize_nonce", payload.finalize_nonce},
                      {"reason", payload.reason} });
            } else {
                return message_envelope(
                    kSpatialRoiIpcFinalizeStatusKind,
                    { {"stream", stream_identity_to_json(payload.stream)},
                      {"sender_role", payload.sender_role},
                      {"drain_sequence", payload.drain_sequence},
                      {"finalize_nonce", payload.finalize_nonce},
                      {"status_sequence", payload.status_sequence},
                      {"state", payload.state},
                      {"reason", payload.reason} });
            }
        },
        message);
}

bool spatial_roi_ipc_message_from_json(const nlohmann::json& value,
                                       SpatialRoiIpcMessage* message_out,
                                       std::string* error_out)
{
    if (!message_out) {
        return fail(error_out, "spatial ROI IPC message destination is null");
    }
    if (!value.is_object() || !value.contains("kind") ||
        !value.at("kind").is_string()) {
        return fail(error_out, "message.kind must be a string");
    }
    const std::string kind = value.at("kind").get<std::string>();
    const json* payload = nullptr;
    if (kind == kSpatialRoiIpcHelloKind) {
        if (!message_kind_and_payload(value, kSpatialRoiIpcHelloKind, &payload, error_out)) {
            return false;
        }
        SpatialRoiIpcHello parsed;
        if (!parse_hello_payload(*payload, &parsed, error_out)) {
            return false;
        }
        *message_out = std::move(parsed);
    } else if (kind == kSpatialRoiIpcFrameKind) {
        if (!message_kind_and_payload(value, kSpatialRoiIpcFrameKind, &payload, error_out)) {
            return false;
        }
        SpatialRoiIpcFrame parsed;
        if (!parse_frame_payload(*payload, &parsed, error_out)) {
            return false;
        }
        *message_out = std::move(parsed);
    } else if (kind == kSpatialRoiIpcAckKind) {
        if (!message_kind_and_payload(value, kSpatialRoiIpcAckKind, &payload, error_out)) {
            return false;
        }
        SpatialRoiIpcAck parsed;
        if (!parse_ack_payload(*payload, &parsed, error_out)) {
            return false;
        }
        *message_out = std::move(parsed);
    } else if (kind == kSpatialRoiIpcReleaseKind) {
        if (!message_kind_and_payload(value, kSpatialRoiIpcReleaseKind, &payload, error_out)) {
            return false;
        }
        SpatialRoiIpcRelease parsed;
        if (!parse_release_payload(*payload, &parsed, error_out)) {
            return false;
        }
        *message_out = std::move(parsed);
    } else if (kind == kSpatialRoiIpcTerminalErrorKind) {
        if (!message_kind_and_payload(value,
                                      kSpatialRoiIpcTerminalErrorKind,
                                      &payload,
                                      error_out)) {
            return false;
        }
        SpatialRoiIpcTerminalError parsed;
        if (!parse_terminal_error_payload(*payload, &parsed, error_out)) {
            return false;
        }
        *message_out = std::move(parsed);
    } else if (kind == kSpatialRoiIpcDrainRequestKind) {
        if (!message_kind_and_payload(value,
                                      kSpatialRoiIpcDrainRequestKind,
                                      &payload,
                                      error_out)) {
            return false;
        }
        SpatialRoiIpcDrainRequest parsed;
        if (!parse_drain_request_payload(*payload, &parsed, error_out)) {
            return false;
        }
        *message_out = std::move(parsed);
    } else if (kind == kSpatialRoiIpcDrainStatusKind) {
        if (!message_kind_and_payload(value,
                                      kSpatialRoiIpcDrainStatusKind,
                                      &payload,
                                      error_out)) {
            return false;
        }
        SpatialRoiIpcDrainStatus parsed;
        if (!parse_drain_status_payload(*payload, &parsed, error_out)) {
            return false;
        }
        *message_out = std::move(parsed);
    } else if (kind == kSpatialRoiIpcFinalizeRequestKind) {
        if (!message_kind_and_payload(value,
                                      kSpatialRoiIpcFinalizeRequestKind,
                                      &payload,
                                      error_out)) {
            return false;
        }
        SpatialRoiIpcFinalizeRequest parsed;
        if (!parse_finalize_request_payload(*payload, &parsed, error_out)) {
            return false;
        }
        *message_out = std::move(parsed);
    } else if (kind == kSpatialRoiIpcFinalizeStatusKind) {
        if (!message_kind_and_payload(value,
                                      kSpatialRoiIpcFinalizeStatusKind,
                                      &payload,
                                      error_out)) {
            return false;
        }
        SpatialRoiIpcFinalizeStatus parsed;
        if (!parse_finalize_status_payload(*payload, &parsed, error_out)) {
            return false;
        }
        *message_out = std::move(parsed);
    } else {
        return fail(error_out, "unsupported spatial ROI IPC message kind");
    }
    if (error_out) {
        error_out->clear();
    }
    return true;
}

std::string serialize_spatial_roi_ipc_message(
    const SpatialRoiIpcMessage& message,
    std::string* error_out)
{
    bool valid = std::visit(
        [&](const auto& payload) {
            using T = std::decay_t<decltype(payload)>;
            if constexpr (std::is_same_v<T, SpatialRoiIpcHello>) {
                return validate_spatial_roi_ipc_hello(payload, error_out);
            } else if constexpr (std::is_same_v<T, SpatialRoiIpcFrame>) {
                return validate_spatial_roi_ipc_frame(payload, error_out);
            } else if constexpr (std::is_same_v<T, SpatialRoiIpcAck>) {
                return validate_spatial_roi_ipc_ack(payload, error_out);
            } else if constexpr (std::is_same_v<T, SpatialRoiIpcRelease>) {
                return validate_spatial_roi_ipc_release(payload, error_out);
            } else if constexpr (std::is_same_v<T, SpatialRoiIpcTerminalError>) {
                return validate_spatial_roi_ipc_terminal_error(payload, error_out);
            } else if constexpr (std::is_same_v<T, SpatialRoiIpcDrainRequest>) {
                return validate_spatial_roi_ipc_drain_request(payload, error_out);
            } else if constexpr (std::is_same_v<T, SpatialRoiIpcDrainStatus>) {
                return validate_spatial_roi_ipc_drain_status(payload, error_out);
            } else if constexpr (std::is_same_v<T, SpatialRoiIpcFinalizeRequest>) {
                return validate_spatial_roi_ipc_finalize_request(payload, error_out);
            } else {
                return validate_spatial_roi_ipc_finalize_status(payload, error_out);
            }
        },
        message);
    if (!valid) {
        return {};
    }
    std::string body;
    try {
        body = spatial_roi_ipc_message_to_json(message).dump(
            -1, ' ', false, nlohmann::json::error_handler_t::strict);
    } catch (const std::exception& exception) {
        fail(error_out,
             std::string("spatial ROI IPC JSON serialization failed: ") +
                 exception.what());
        return {};
    }
    const std::size_t wire_size = body.size() + 1;
    if (wire_size > kSpatialRoiIpcMaxWireMessageBytes) {
        fail(error_out, "spatial ROI IPC message exceeds wire length bound");
        return {};
    }
    if (error_out) {
        error_out->clear();
    }
    return body + '\n';
}

bool parse_spatial_roi_ipc_message(const std::string& wire,
                                   SpatialRoiIpcMessage* message_out,
                                   std::string* error_out)
{
    if (!message_out) {
        return fail(error_out, "spatial ROI IPC message destination is null");
    }
    if (wire.empty() || wire.size() > kSpatialRoiIpcMaxWireMessageBytes) {
        return fail(error_out, "spatial ROI IPC wire message length is invalid");
    }
    std::string body = wire;
    if (!body.empty() && body.back() == '\n') {
        body.pop_back();
    }
    if (body.empty() || body.find('\n') != std::string::npos ||
        body.find('\r') != std::string::npos) {
        return fail(error_out, "spatial ROI IPC message must be one JSON line");
    }
    try {
        const json value = json::parse(body);
        return spatial_roi_ipc_message_from_json(value, message_out, error_out);
    } catch (const std::exception& exception) {
        return fail(error_out,
                    std::string("invalid spatial ROI IPC JSON: ") +
                        exception.what());
    }
}

bool SpatialRoiIpcControlState::bind_or_match_stream(
    const SpatialRoiIpcStreamIdentity& stream,
    std::string* error_out)
{
    if (!stream_identity_) {
        stream_identity_ = stream;
        return true;
    }
    if (!stream_identity_equal(*stream_identity_, stream)) {
        return fail(error_out,
                    "ROI IPC control message stream identity does not match the session");
    }
    return true;
}

bool SpatialRoiIpcControlState::accept_status_sequence(
    const std::uint64_t status_sequence,
    std::string* error_out)
{
    if (last_status_sequence_ == std::numeric_limits<std::uint64_t>::max() ||
        status_sequence != last_status_sequence_ + 1) {
        return fail(error_out,
                    "ROI IPC recorder status_sequence must increase by exactly one");
    }
    last_status_sequence_ = status_sequence;
    return true;
}

bool SpatialRoiIpcControlState::accept(
    const SpatialRoiIpcDrainRequest& request,
    std::string* error_out)
{
    if (!validate_spatial_roi_ipc_drain_request(request, error_out)) {
        return false;
    }
    if (phase_ != SpatialRoiIpcControlPhase::kIdle) {
        return fail(error_out,
                    "duplicate or out-of-order DRAIN_REQUEST for ROI IPC session");
    }
    if (!bind_or_match_stream(request.stream, error_out)) {
        return false;
    }
    drain_sequence_ = request.drain_sequence;
    phase_ = SpatialRoiIpcControlPhase::kDrainRequested;
    if (error_out) {
        error_out->clear();
    }
    return true;
}

bool SpatialRoiIpcControlState::accept(
    const SpatialRoiIpcDrainStatus& status,
    std::string* error_out)
{
    if (!validate_spatial_roi_ipc_drain_status(status, error_out)) {
        return false;
    }
    SpatialRoiIpcControlPhase next_phase;
    if (status.state == kSpatialRoiIpcDrainStateDraining) {
        if (phase_ != SpatialRoiIpcControlPhase::kDrainRequested &&
            phase_ != SpatialRoiIpcControlPhase::kDraining) {
            return fail(error_out,
                        "DRAIN_STATUS draining is not valid in the current ROI IPC phase");
        }
        next_phase = SpatialRoiIpcControlPhase::kDraining;
    } else if (status.state == kSpatialRoiIpcDrainStateDrained) {
        if (phase_ != SpatialRoiIpcControlPhase::kDrainRequested &&
            phase_ != SpatialRoiIpcControlPhase::kDraining) {
            return fail(error_out,
                        "DRAIN_STATUS drained is not valid in the current ROI IPC phase");
        }
        next_phase = SpatialRoiIpcControlPhase::kDrained;
    } else {
        if (phase_ != SpatialRoiIpcControlPhase::kDrainRequested &&
            phase_ != SpatialRoiIpcControlPhase::kDraining) {
            return fail(error_out,
                        "DRAIN_STATUS failed is not valid in the current ROI IPC phase");
        }
        next_phase = SpatialRoiIpcControlPhase::kFailed;
    }
    if (status.drain_sequence != drain_sequence_) {
        return fail(error_out,
                    "DRAIN_STATUS drain_sequence does not match the request");
    }
    if (!bind_or_match_stream(status.stream, error_out)) {
        return false;
    }
    if (!accept_status_sequence(status.status_sequence, error_out)) {
        return false;
    }
    phase_ = next_phase;
    if (error_out) {
        error_out->clear();
    }
    return true;
}

bool SpatialRoiIpcControlState::accept(
    const SpatialRoiIpcFinalizeRequest& request,
    std::string* error_out)
{
    if (!validate_spatial_roi_ipc_finalize_request(request, error_out)) {
        return false;
    }
    if (phase_ != SpatialRoiIpcControlPhase::kDrained) {
        return fail(error_out,
                    "FINALIZE_REQUEST requires a completed DRAIN_STATUS");
    }
    if (request.drain_sequence != drain_sequence_) {
        return fail(error_out,
                    "FINALIZE_REQUEST drain_sequence does not match the request");
    }
    if (!bind_or_match_stream(request.stream, error_out)) {
        return false;
    }
    finalize_nonce_ = request.finalize_nonce;
    phase_ = SpatialRoiIpcControlPhase::kFinalizeRequested;
    if (error_out) {
        error_out->clear();
    }
    return true;
}

bool SpatialRoiIpcControlState::accept(
    const SpatialRoiIpcFinalizeStatus& status,
    std::string* error_out)
{
    if (!validate_spatial_roi_ipc_finalize_status(status, error_out)) {
        return false;
    }
    if (phase_ != SpatialRoiIpcControlPhase::kFinalizeRequested) {
        return fail(error_out,
                    "FINALIZE_STATUS is not valid in the current ROI IPC phase");
    }
    if (status.drain_sequence != drain_sequence_) {
        return fail(error_out,
                    "FINALIZE_STATUS drain_sequence does not match the request");
    }
    if (status.finalize_nonce != finalize_nonce_) {
        return fail(error_out,
                    "FINALIZE_STATUS finalize_nonce does not match the request");
    }
    if (!bind_or_match_stream(status.stream, error_out)) {
        return false;
    }
    if (!accept_status_sequence(status.status_sequence, error_out)) {
        return false;
    }
    phase_ = status.state == kSpatialRoiIpcFinalizeStateFinalized
                 ? SpatialRoiIpcControlPhase::kFinalized
                 : SpatialRoiIpcControlPhase::kFailed;
    if (error_out) {
        error_out->clear();
    }
    return true;
}

bool SpatialRoiIpcControlState::accept(const SpatialRoiIpcMessage& message,
                                       std::string* error_out)
{
    return std::visit(
        [&](const auto& payload) {
            using T = std::decay_t<decltype(payload)>;
            if constexpr (std::is_same_v<T, SpatialRoiIpcDrainRequest> ||
                          std::is_same_v<T, SpatialRoiIpcDrainStatus> ||
                          std::is_same_v<T, SpatialRoiIpcFinalizeRequest> ||
                          std::is_same_v<T, SpatialRoiIpcFinalizeStatus>) {
                return accept(payload, error_out);
            } else {
                return fail(error_out,
                            "message kind is not an ROI IPC session-control message");
            }
        },
        message);
}

bool SpatialRoiIpcCorrelationRegistry::note(
    const SpatialRoiIpcCorrelationKey& key,
    std::string* error_out)
{
    if (!is_sha256(key.recording_identity_token) ||
        !is_safe_identifier(key.producer_generation) ||
        !is_safe_identifier(key.logical_stream_id) ||
        key.recording_frame_id == 0 || key.roi_stream_frame_index == 0) {
        return fail(error_out,
                    "IPC correlation key requires a recording token, producer generation, safe stream, and positive frame/index");
    }
    if (!keys_.insert(key).second) {
        return fail(error_out,
                    "duplicate IPC correlation recording_identity_token=" +
                        key.recording_identity_token +
                        " producer_generation=" + key.producer_generation +
                        " logical_stream_id=" +
                        key.logical_stream_id +
                        " recording_frame_id=" +
                        std::to_string(key.recording_frame_id) +
                        " roi_stream_frame_index=" +
                        std::to_string(key.roi_stream_frame_index));
    }
    if (error_out) {
        error_out->clear();
    }
    return true;
}

bool SpatialRoiIpcCorrelationRegistry::note(
    const SpatialRoiIpcCorrelation& correlation,
    std::string* error_out)
{
    if (!validate_spatial_roi_ipc_correlation(correlation, error_out)) {
        return false;
    }
    return note(correlation.key(), error_out);
}

bool SpatialRoiIpcCorrelationRegistry::contains(
    const SpatialRoiIpcCorrelationKey& key) const
{
    return keys_.find(key) != keys_.end();
}

}  // namespace orange::spatial_roi::ipc
