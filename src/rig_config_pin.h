// Rig documents in effect at record start, sealed into the recording start
// snapshot (recording_snapshot_start.json session.rig_config) so a recording
// says which rig description and which open part conditions it was made under.
//
// The rig documents belong to pancakebatter: rig.yml (current values) and the
// append-only events ledger (rigs/<rig_id>/events.yml, rules in its
// docs/equipment_events.md "Rig events"), installed on the host as
// /etc/pancakebatter/rigs/<rig_id>.yml, <rig_id>.events.yml and a .source
// sidecar naming the commit. Orange pins both files by sha256 and copies, from
// the ledger, the conditions still open (condition_observed with corrections
// applied and no condition_cleared; a port of rig_events.active_conditions) and
// the attestations no correction voided. Whether an attestation is still
// current is pancakebatter's decision (rig_events.current_attestations: its
// orientation_sha256 equals the installed rig.yml's orientation digest), which
// install_host_config.sh writes into the sidecar as one
// "current_attestations.<field>: <event ids>" line per attestable field. Orange
// copies it per attestation
// (current_per_pancakebatter) only when the sidecar's sha256 and events_sha256
// both match the pinned files, and never recomputes it.
//
// Resolution: $PANCAKEBATTER_RIG_CONFIG (a rig file path), else
// /etc/pancakebatter/rigs/<rig_id>.yml for the recording's Citrus rig id. No
// default: a missing file is recorded as not_installed. Never blocks a start.
//
// Schema: docs/schemas/orange_recording_rig_config_v2.schema.json (closed; v1 had
// attestation_revision_matches instead of current_per_pancakebatter).
//   {
//     "schema_id": "orange.recording.rig_config", "schema_version": 2,
//     "captured_at_utc": "...Z",
//     "status": one of kRigConfigStatuses (below),
//     "resolution": {"source": "environment"|"installed"|null, "environment_variable": "PANCAKEBATTER_RIG_CONFIG",
//                    "recording_rig_id": str|null},
//     "rig_id": str|null, "rig_geometry_revision": int|null,
//     "rig_file": {"path", "sha256", "size_bytes"}|null,
//     "events_file": {"path", "status": "pinned"|"missing"|"unreadable"|"invalid", "sha256", "size_bytes"}|null,
//     "source_sidecar": {"path", "status": "read"|"missing", "commit", "installed", "source", "events_source",
//                        "rig_sha256_matches": bool|null, "events_sha256_matches": bool|null,
//                        "orientation_sha256": str|null, "current_attestations": {<field>: [str]}|null}|null,
//     "active_conditions": [<condition_observed event, corrections applied, as in the ledger>],
//     "attestations": [{"event_id", "fields", "rig_geometry_revision", "orientation_sha256", "scene_id",
//                       "current_per_pancakebatter": bool|null}],
//     "warnings": [str]
//   }
#pragma once

#include <string>

#include "json.hpp"

namespace orange::recording {

inline constexpr const char* kRigConfigPinSchemaId = "orange.recording.rig_config";
inline constexpr int kRigConfigPinSchemaVersion = 2;
inline constexpr const char* kRigConfigEnvironmentVariable = "PANCAKEBATTER_RIG_CONFIG";
inline constexpr const char* kInstalledRigConfigDir = "/etc/pancakebatter/rigs";
// Sidecar lines with pancakebatter's verdict, one per attestable field:
// "current_attestations.<field>: e3,e7" (empty value = none current for that field).
inline constexpr const char* kSidecarCurrentAttestationsPrefix = "current_attestations.";

// status values, closed:
//   pinned           rig file and events ledger read and parsed; the .source sidecar, when present, agrees
//   rig_id_unknown   no $PANCAKEBATTER_RIG_CONFIG and no Citrus rig id for this recording
//   not_installed    the resolved rig file does not exist (this host does not drive that rig)
//   unreadable       the rig file or the events ledger exists but cannot be read
//   invalid          the rig file or the ledger is not valid YAML, or lacks rig_id / rig_geometry_revision / events
//   rig_id_mismatch  the rig file's rig_id differs from the recording's Citrus rig id
//   events_missing   the rig file is pinned but its events ledger does not exist
//   sidecar_mismatch a pinned file's sha256 differs from the one its .source sidecar records
inline constexpr const char* kRigConfigStatuses[] = {
    "pinned", "rig_id_unknown", "not_installed", "unreadable", "invalid",
    "rig_id_mismatch", "events_missing", "sidecar_mismatch"};

// recording_rig_id: the Citrus rig of this recording (geometry contract
// selection.rig_id), empty when unknown. env_rig_config: the value of
// $PANCAKEBATTER_RIG_CONFIG, empty when unset. installed_dir: normally
// kInstalledRigConfigDir (tests pass a fixture directory).
nlohmann::json BuildRigConfigPin(const std::string& recording_rig_id,
                                 const std::string& env_rig_config,
                                 const std::string& installed_dir,
                                 const std::string& captured_at_utc);

// The process environment and the installed directory.
nlohmann::json BuildRigConfigPinForRecording(const std::string& recording_rig_id,
                                             const std::string& captured_at_utc);

// The recording's Citrus rig id from a recording geometry contract, or "".
std::string RecordingRigIdFromGeometryContract(const nlohmann::json& geometry_contract);

// Ledger logic (ports of pancakebatter rig_events.py), on the ledger's events
// list converted to JSON. Exposed for tests.
nlohmann::json RigEventsActiveConditions(const nlohmann::json& events);
nlohmann::json RigEventsAttestations(const nlohmann::json& events);

// YAML text to JSON with YAML 1.1 plain-scalar typing for ints, floats, bools
// and null (as PyYAML safe_load does for the values the rig documents use).
// Throws std::runtime_error on a parse error.
nlohmann::json RigYamlToJson(const std::string& text);

}  // namespace orange::recording
