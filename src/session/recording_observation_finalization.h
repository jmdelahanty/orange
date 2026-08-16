#pragma once

#include "json.hpp"

#include <string>

namespace orange::session {

inline constexpr const char* kObservationBindingFinalizationSchemaId =
    "orange.recording.observation_binding_finalization";
inline constexpr const char* kObservationBindingFinalizationRelativePath =
    "recording_observation_bindings/finalized_collection.json";

struct RecordingObservationFinalizationResult {
    bool ok = false;
    std::string error;
    nlohmann::json collection = nlohmann::json::object();
    nlohmann::json collection_reference = nlohmann::json::object();
};

// Validate every Citrus receipt against the immutable Orange request and
// acceptance, independently verify the closed H5 byte size and SHA-256, and
// materialize create-once receipt/collection artifacts. This function is
// intentionally safe to retry with byte-identical evidence.
RecordingObservationFinalizationResult
finalize_recording_observation_bindings(
    const std::string& recording_folder,
    const nlohmann::json& params);

// Add the finalized producer-native collection to recording_session.json.
// Missing or invalid finalization evidence is represented as explicit
// `unbound` state; it is never upgraded to `bound` by inference.
bool apply_recording_observation_finalization_to_manifest(
    const std::string& recording_folder,
    nlohmann::json* manifest,
    std::string* error_out = nullptr);

// If recording_session.json was already finalized before Citrus returned its
// receipts, atomically refresh only its observation-binding projection from
// the immutable finalized collection. A missing manifest is not an error;
// the ordinary finalizer will consume the collection when it writes one.
bool refresh_recording_session_observation_bindings(
    const std::string& recording_folder,
    std::string* error_out = nullptr);

}  // namespace orange::session
