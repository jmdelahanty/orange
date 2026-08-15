#pragma once

#include "json.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace orange::session {

struct RecordingObservationBindingRequestArtifact {
    std::string observation_context_id;
    std::string request_id;
    std::string relative_path;
    std::string sha256;
    std::uint64_t byte_size = 0;
    nlohmann::json observation_identity = nlohmann::json::object();
    nlohmann::json request = nlohmann::json::object();
};

struct RecordingObservationBindingRequestMaterialization {
    // materialized: one request was created or verified per current producer
    // observation edge. unavailable: no sufficiently resolved recording-bound
    // Citrus geometry exists, so no association claim was fabricated and the
    // optional Orange recording may continue.
    std::string status;
    std::string reason;
    std::string binding_mode;
    std::string collection_relative_path;
    std::string collection_sha256;
    std::uint64_t collection_byte_size = 0;
    std::vector<RecordingObservationBindingRequestArtifact> artifacts;
};

// Reference stored in mutable recording_snapshot.json. It points to the
// create-once collection manifest rather than copying request payloads into the
// later-mutable snapshot.
nlohmann::json recording_observation_binding_request_collection_reference(
    const RecordingObservationBindingRequestMaterialization& result);

// Build create-once, read-only request artifacts exclusively from the exact
// recording_snapshot_start.json bytes and their digest-bound recording
// geometry contract. The current producer emits requests only for a fully
// resolved Citrus canvas and enforces one arena per source-camera frame stream.
// This function performs no Citrus communication and does not mutate the
// sealed start snapshot.
bool materialize_recording_observation_binding_requests(
    const std::string& recording_folder,
    const std::string& binding_mode,
    const std::string& requested_at_utc,
    RecordingObservationBindingRequestMaterialization* result_out,
    std::string* error_out = nullptr);

}  // namespace orange::session
