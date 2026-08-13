#pragma once

#include "json.hpp"

#include <string>
#include <vector>

namespace orange::session {

inline constexpr const char* kRecordingObservationIdentitySchemaId =
    "orange.recording.observation_identity";
inline constexpr int kRecordingObservationIdentitySchemaVersion = 1;
inline constexpr const char* kRecordingObservationIdentityScope =
    "recording_camera_arena_observation_edge";
inline constexpr const char* kRecordingObservationIdentityCanonicalization =
    "canonical_json_utf8_sort_keys_compact_v1";
inline constexpr const char* kCameraSerialSourceFrameStreamIdentityPolicy =
    "camera_serial_unique_source_frame_stream_within_recording_v1";

// One recording-bound, camera-stream-to-arena observation edge.
//
// camera_id and qualified arena identity are deliberately not unique across a
// collection:
// one source-camera frame stream may observe several arenas and one arena may
// be observed by several source-camera frame streams.  The edge key is
// (recording_id, source_camera_stream_id, rig_id, canvas_name, arena_id).
struct RecordingObservationEdgeIdentity {
    std::string recording_id;
    std::string camera_id;
    std::string source_camera_stream_id;
    std::string source_camera_stream_identity_policy =
        kCameraSerialSourceFrameStreamIdentityPolicy;
    std::string rig_id;
    std::string canvas_name;
    std::string arena_id;
};

// Build an immutable schema-v1 identity record.  identity_sha256 is computed
// over the record's canonical `identity` payload, which includes schema
// identity and version.  File paths and rolling-clip IDs are intentionally not
// part of the identity.
bool build_recording_observation_identity(
    const RecordingObservationEdgeIdentity& identity,
    nlohmann::json* record_out,
    std::string* error_out = nullptr);

// Parse and verify a record, including its canonical semantic digest and
// derived observation_context_id.
bool parse_recording_observation_identity(
    const nlohmann::json& record,
    RecordingObservationEdgeIdentity* identity_out,
    std::string* error_out = nullptr);

// Validate one recording's context collection.  Exact duplicate observation
// edges fail closed; repeated camera IDs, source-camera stream IDs, and arena
// IDs are individually valid and express the supported many-to-many topology.
bool validate_recording_observation_identity_set(
    const std::vector<nlohmann::json>& records,
    std::string* error_out = nullptr);

// Current producer policy.  The identity schema remains many-to-many capable,
// but Orange materialization v1 deliberately rejects one source-camera frame
// stream assigned to several arenas. Several streams may observe one arena.
bool validate_current_recording_observation_topology(
    const std::vector<nlohmann::json>& records,
    std::string* error_out = nullptr);

}  // namespace orange::session
