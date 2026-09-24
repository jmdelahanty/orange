// Parent recording context: the explicit, operator-configured description of
// what a recording is (type, subtype, behaviour mode, stimulus intent, data
// origin), emitted per camera parent as `recording_contexts` in
// recording_session.json for Citrus transfer-v2 intake
// (citrus.parent_recording_context, version 1).
//
// Rules (Citrus parent_recording_context_and_synthetic_transfer_2026-09-24):
// - values are configured, never inferred from files, detectors or Citrus;
// - frozen before capture into the sealed recording start snapshot;
// - emitted unchanged in every manifest write, including rolling rollover and
//   external-recorder rebuilds;
// - keyed by exact camera serial with exactly the recording's camera set.
#pragma once

#include <map>
#include <optional>
#include <string>
#include <vector>

#include "json.hpp"

namespace orange::recording {

struct RecordingContext {
    std::string recording_type;
    std::string recording_subtype;
    std::string behavior_mode;      // free | embedded | none
    std::string recording_intent;   // stimulus_experiment | recording_only
    std::string data_origin;        // acquired | synthetic

    static constexpr const char* kSchemaId = "citrus.parent_recording_context";
    static constexpr int kSchemaVersion = 1;

    // Config entry: exactly the five configurable fields.
    static RecordingContext Parse(const nlohmann::json& entry);
    // Emitted form: the seven-field closed object (schema_id, schema_version + five).
    static RecordingContext ParseEmitted(const nlohmann::json& emitted);
    nlohmann::json ToJson() const;          // five fields
    nlohmann::json ToEmittedJson() const;   // seven fields
    bool operator==(const RecordingContext& other) const;
};

// Operator configuration: {"schema_version":1, "default": {...}?, "cameras": {"<serial>": {...}}?}.
// At record start it is resolved against the recording's camera serials:
// each serial takes its own entry, else the default; a serial with neither
// refuses the start. Configured serials that are not recording are ignored.
struct RecordingContextsConfig {
    std::optional<RecordingContext> default_context;
    std::map<std::string, RecordingContext> cameras;
    bool configured() const { return default_context.has_value() || !cameras.empty(); }

    static RecordingContextsConfig Parse(const nlohmann::json& config);
    nlohmann::json ToJson() const;
    std::map<std::string, RecordingContext> Resolve(const std::vector<std::string>& recording_serials) const;
};

// Emitted block keyed by serial (seven-field entries), strict.
std::map<std::string, RecordingContext> ParseEmittedRecordingContexts(const nlohmann::json& block);
nlohmann::json EmittedRecordingContextsJson(const std::map<std::string, RecordingContext>& contexts);

// Manifest gate (called from write_recording_session_manifest): reads
// session.recording_contexts from the sealed start snapshot in `root`
// (recording_snapshot_start.json, else recording_snapshot.json), validates it,
// requires its membership to equal manifest["cameras"], and copies it
// verbatim into manifest["recording_contexts"]. A manifest that already holds
// a different block is refused. No snapshot block: the manifest is left
// without one (validators report it).
void ApplyRecordingContextsGate(const std::string& recording_folder, nlohmann::json* manifest);

// Reads the frozen block from the recording folder's start snapshot; empty
// optional when no block was frozen.
std::optional<std::map<std::string, RecordingContext>> ReadFrozenRecordingContexts(
    const std::string& recording_folder, std::string* error_out);

// Citrus observation binding mode versus the frozen intent:
// recording_only parents must never bind (Citrus refuses the transfer).
// Returns the binding mode to use, or an empty string with error_out set.
//   intent recording_only + mode required     -> error (explicit conflict)
//   intent recording_only + mode optional     -> "not_applicable"
//   intent recording_only + not_applicable    -> unchanged
//   no frozen block / stimulus_experiment     -> unchanged
std::string ApplyRecordingIntentToBindingMode(const std::string& recording_folder,
                                              const std::string& binding_mode,
                                              std::string* error_out);

}  // namespace orange::recording
