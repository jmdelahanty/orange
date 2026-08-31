#pragma once

#include "spatial_roi_frame_contract.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <unordered_set>
#include <variant>
#include <vector>

namespace orange::spatial_roi::ipc {

// This is deliberately a new protocol namespace.  It is not wire-compatible
// with external_recorder_ipc_protocol.h (which remains protocol v1 for the
// existing full-frame and scalar-crop clients).
// This slice only defines the contract.  The future supervisor must negotiate
// this protocol on a dedicated ROI-aware endpoint, validate the verified-plan
// identity before accepting FRAME messages, track the three-part correlation
// through ACK/RELEASE, and import the two CUDA IPC handles without routing
// these messages through the legacy probe parser.  The probe needs a separate
// ROI mode and matching metadata/finalization accounting.
inline constexpr const char* kSpatialRoiIpcProtocolName =
    "orange.spatial_roi.external_recorder_ipc";
inline constexpr int kSpatialRoiIpcProtocolVersion = 2;
inline constexpr const char* kSpatialRoiIpcHelloKind = "HELLO";
inline constexpr const char* kSpatialRoiIpcFrameKind = "FRAME";
inline constexpr const char* kSpatialRoiIpcAckKind = "ACK";
inline constexpr const char* kSpatialRoiIpcReleaseKind = "RELEASE";
inline constexpr const char* kSpatialRoiIpcTerminalErrorKind = "TERMINAL_ERROR";

// Messages are compact JSON records sent one per line.  The bound is on the
// encoded JSON line, not on the device allocation named by a CUDA handle.
inline constexpr std::size_t kSpatialRoiIpcMaxWireMessageBytes = 1024 * 1024;
inline constexpr std::size_t kSpatialRoiIpcMaxTextBytes = 1024;
inline constexpr std::size_t kSpatialRoiIpcMaxFeatures = 32;
inline constexpr std::uint32_t kSpatialRoiIpcMaxQueueFrames = 4096;
inline constexpr std::uint64_t kSpatialRoiIpcMaxPackedMono8Bytes =
    2ULL * 1024ULL * 1024ULL * 1024ULL;

// CUDA's public IPC handle types are fixed-size opaque byte arrays.  The
// protocol carries each as exactly 64 bytes represented by exactly 128 lower-
// case hexadecimal characters.  No native struct layout or NUL termination is
// placed on the wire.
inline constexpr std::size_t kSpatialRoiCudaIpcHandleBytes = 64;
inline constexpr std::size_t kSpatialRoiCudaIpcHandleHexBytes =
    kSpatialRoiCudaIpcHandleBytes * 2;
inline constexpr const char* kSpatialRoiCudaIpcHandleEncoding =
    "hex_lower_fixed_64_bytes";

// The identity copied from SpatialRoiFrameDescriptor into control messages.
// Keeping all of these fields in ACK/RELEASE makes a frame acknowledgement
// self-identifying even when several ROI streams share one transport.
struct SpatialRoiIpcStreamIdentity {
    std::string recording_id;
    std::string recording_identity_token;
    std::string producer_generation;
    int camera_id = -1;
    std::string camera_serial;
    std::string roi_id;
    std::string region_id;
    std::string arena_group_id;
    std::string arena_id;
    std::string logical_stream_id;
    std::string spatial_roi_plan_sha256;
};

struct SpatialRoiIpcCorrelationKey {
    // The recording token and producer generation keep a registry safe when
    // a logical stream name and its dense indices restart in a later session
    // or producer epoch.
    std::string recording_identity_token;
    std::string producer_generation;
    std::string logical_stream_id;
    std::uint64_t recording_frame_id = 0;
    std::uint64_t roi_stream_frame_index = 0;

    bool operator==(const SpatialRoiIpcCorrelationKey& other) const noexcept
    {
        return recording_identity_token == other.recording_identity_token &&
               producer_generation == other.producer_generation &&
               logical_stream_id == other.logical_stream_id &&
               recording_frame_id == other.recording_frame_id &&
               roi_stream_frame_index == other.roi_stream_frame_index;
    }
};

struct SpatialRoiIpcCorrelationKeyHash {
    std::size_t operator()(
        const SpatialRoiIpcCorrelationKey& key) const noexcept;
};

// This is the exact source identity needed to correlate a frame and its
// acknowledgement/release.  In particular, recording_frame_id alone is not a
// valid key because four ROI streams can carry the same source frame.
struct SpatialRoiIpcCorrelation {
    SpatialRoiIpcStreamIdentity stream;
    std::uint64_t local_frame_id = 0;
    std::uint64_t camera_frame_id = 0;
    std::uint64_t recording_frame_id = 0;
    std::uint64_t roi_stream_frame_index = 0;

    SpatialRoiIpcCorrelationKey key() const
    {
        return {stream.recording_identity_token,
                stream.producer_generation,
                stream.logical_stream_id,
                recording_frame_id,
                roi_stream_frame_index};
    }
};

SpatialRoiIpcStreamIdentity spatial_roi_ipc_stream_identity_from_descriptor(
    const SpatialRoiFrameDescriptor& descriptor);
SpatialRoiIpcCorrelation spatial_roi_ipc_correlation_from_descriptor(
    const SpatialRoiFrameDescriptor& descriptor);

bool validate_spatial_roi_ipc_stream_identity(
    const SpatialRoiIpcStreamIdentity& identity,
    std::string* error_out = nullptr);
bool validate_spatial_roi_ipc_correlation(
    const SpatialRoiIpcCorrelation& correlation,
    std::string* error_out = nullptr);
bool spatial_roi_ipc_correlation_matches_descriptor(
    const SpatialRoiIpcCorrelation& correlation,
    const SpatialRoiFrameDescriptor& descriptor,
    std::string* error_out = nullptr);

// A frame points at a packed, row-major Mono8 allocation.  The pixels are not
// copied into JSON: byte_length/row_pitch describe the exact raster span and
// the two fixed-size handles identify the CUDA allocation and readiness event.
struct SpatialRoiCudaIpcBuffer {
    std::string memory_handle_encoding = kSpatialRoiCudaIpcHandleEncoding;
    std::string memory_handle_hex;
    std::string ready_event_handle_encoding = kSpatialRoiCudaIpcHandleEncoding;
    std::string ready_event_handle_hex;
    std::uint64_t byte_offset = 0;
    std::uint64_t byte_length = 0;
    std::uint64_t row_pitch_bytes = 0;
    std::string pixel_format = kSpatialRoiMono8PixelFormat;
    std::string layout = "packed_row_major";
};

struct SpatialRoiIpcHello {
    SpatialRoiIpcStreamIdentity stream;
    std::string role;  // "producer" or "recorder"
    std::uint32_t queue_capacity_frames = 0;
    std::vector<std::string> features;
};

struct SpatialRoiIpcFrame {
    SpatialRoiFrameDescriptor descriptor;
    SpatialRoiCudaIpcBuffer cuda_buffer;
};

struct SpatialRoiIpcAck {
    SpatialRoiIpcCorrelation correlation;
    bool accepted = false;
    std::string reason;
};

struct SpatialRoiIpcRelease {
    SpatialRoiIpcCorrelation correlation;
    std::string reason;
};

struct SpatialRoiIpcTerminalError {
    SpatialRoiIpcStreamIdentity stream;
    std::string error_code;
    std::string message;
    bool fatal = true;
    std::optional<SpatialRoiIpcCorrelation> correlation;
};

using SpatialRoiIpcMessage = std::variant<SpatialRoiIpcHello,
                                          SpatialRoiIpcFrame,
                                          SpatialRoiIpcAck,
                                          SpatialRoiIpcRelease,
                                          SpatialRoiIpcTerminalError>;

bool validate_spatial_roi_ipc_buffer(
    const SpatialRoiCudaIpcBuffer& buffer,
    const SpatialRoiFrameDescriptor& descriptor,
    std::string* error_out = nullptr);
bool validate_spatial_roi_ipc_hello(
    const SpatialRoiIpcHello& hello,
    std::string* error_out = nullptr);
bool validate_spatial_roi_ipc_frame(
    const SpatialRoiIpcFrame& frame,
    std::string* error_out = nullptr);
bool validate_spatial_roi_ipc_ack(
    const SpatialRoiIpcAck& ack,
    std::string* error_out = nullptr);
bool validate_spatial_roi_ipc_release(
    const SpatialRoiIpcRelease& release,
    std::string* error_out = nullptr);
bool validate_spatial_roi_ipc_terminal_error(
    const SpatialRoiIpcTerminalError& terminal_error,
    std::string* error_out = nullptr);

nlohmann::json spatial_roi_ipc_message_to_json(
    const SpatialRoiIpcMessage& message);
bool spatial_roi_ipc_message_from_json(
    const nlohmann::json& value,
    SpatialRoiIpcMessage* message_out,
    std::string* error_out = nullptr);

// Serialize/parse one compact JSON line.  The serializer returns an empty
// string on invalid input; parse accepts either a line with one trailing '\n'
// or the same JSON record without a newline.
std::string serialize_spatial_roi_ipc_message(
    const SpatialRoiIpcMessage& message,
    std::string* error_out = nullptr);
bool parse_spatial_roi_ipc_message(
    const std::string& wire,
    SpatialRoiIpcMessage* message_out,
    std::string* error_out = nullptr);

// Host-only collision guard for a supervisor's outstanding frame table.  It
// deliberately keys all three routing dimensions and does not consume entries;
// ACK and RELEASE have different lifecycle points and must be tracked by the
// eventual supervisor separately.
class SpatialRoiIpcCorrelationRegistry {
public:
    bool note(const SpatialRoiIpcCorrelationKey& key,
              std::string* error_out = nullptr);
    bool note(const SpatialRoiIpcCorrelation& correlation,
              std::string* error_out = nullptr);
    bool contains(const SpatialRoiIpcCorrelationKey& key) const;
    std::size_t size() const noexcept { return keys_.size(); }

private:
    std::unordered_set<SpatialRoiIpcCorrelationKey,
                       SpatialRoiIpcCorrelationKeyHash>
        keys_;
};

}  // namespace orange::spatial_roi::ipc
