#pragma once

#include "json.hpp"

#include <string>

namespace orange::session {

inline constexpr const char* kObservationBindingRequestSchemaId =
    "orange.citrus.recording_observation_binding_request";
inline constexpr const char* kObservationBindingAcceptanceSchemaId =
    "citrus.recording_observation_binding_acceptance";
inline constexpr const char* kObservationBindingFinalizedReceiptSchemaId =
    "citrus.recording_observation_finalized_receipt";
inline constexpr int kObservationBindingSchemaVersion = 1;
// Binding request v2 (2026-09-24, joint with Citrus): the request contract
// carries the frozen parent recording context (contract.recording_context,
// seven fields, inside the request digest) and Citrus answers with acceptance
// v2 whose rejection reasons add recording_context_{missing,invalid,
// mismatch,unavailable} and batch_rejected. Envelope and contract carry the
// same version; a v2 request needs a v2 acceptance; receipts stay v1 (Citrus
// 9aa6d57). Opt-in until the joint test passes:
// ORANGE_CITRUS_BINDING_REQUEST_VERSION=2 (default 1).
inline constexpr int kObservationBindingRequestSchemaVersionV2 = 2;
// Finalized receipt v2 (2026-10-10, sealer 3.1.0, joint with Citrus/Palette):
// the v1 contract plus `citrus_artifacts`, every per-session file Citrus
// writes besides the H5, each declared by role, path, size and SHA-256. The
// stimulus-video finalization entry also carries the container facts Orange
// checks against the file. v1 receipts keep their meaning.
inline constexpr int kObservationBindingFinalizedReceiptSchemaVersionV2 = 2;
// Subject declaration (agent-contracts citrus-orange/subject_declaration_v1,
// PR 74 + amendment PR 75, 2026-10-11): acceptance v3 = acceptance v2 plus
// contract.subject (citrus.subject_declaration v1, the operator's per-arena
// dish at pre-arm), answering a v2 request; receipt v3 = receipt v2 plus
// contract.subject_reference (citrus.subject_reference v1, the MZB-confirmed
// record Citrus wrote to its H5). Orange accepts v2 and v3 side by side.
inline constexpr int kObservationBindingAcceptanceSchemaVersionV3 = 3;
inline constexpr int kObservationBindingFinalizedReceiptSchemaVersionV3 = 3;
// Subject identity v4 (agent-contracts citrus-palette/subject_identity_v4_2026-10-14,
// #105): receipt v4 = receipt v3 with contract.subject_reference as
// citrus.subject_reference v2 (subject_id the MetaZebrobot fish UUID, null for
// a dish_group), always answering an acceptance v3. Receipt v3 keeps reference
// v1; one finalized collection, revisions included, never mixes v3 and v4.
inline constexpr int kObservationBindingFinalizedReceiptSchemaVersionV4 = 4;
inline constexpr const char* kCitrusSubjectDeclarationSchemaId = "citrus.subject_declaration";
inline constexpr const char* kCitrusSubjectReferenceSchemaId = "citrus.subject_reference";
inline constexpr const char* kCitrusArtifactRoleStimulusVideo = "stimulus_video";
inline constexpr const char* kCitrusArtifactRoleStimulusVideoFinalization =
    "stimulus_video_container_finalization";
inline constexpr const char* kCitrusArtifactRoleUpdateTiming = "update_timing";
// The Citrus threading-startup summary: citrus/<stem>_threading_startup.json
// per arena session; recordings made before 3.1.0 carry one
// citrus/<utc>_threading_startup_<pid>.json per recording, declared once per
// collection in the lowest observation_context_id's receipt (PR 71 rule 8).
inline constexpr const char* kCitrusArtifactRoleProcessDiagnostic =
    "process_diagnostic";
inline constexpr const char* kCitrusStimulusVideoFinalizationSchemaId =
    "citrus.stimulus_video_container_finalization";
int resolve_recording_observation_binding_request_version(std::string* error_out = nullptr);
inline bool accepted_observation_binding_schema_version(int version)
{
    return version == kObservationBindingSchemaVersion ||
           version == kObservationBindingRequestSchemaVersionV2;
}
// The schema_version of a sealed envelope or of a contract payload as a
// JSON integer (1 to 4); 0 for anything else (missing, float, out of range).
// Which versions a document admits is checked per document.
inline int binding_record_schema_version(const nlohmann::json& record)
{
    if (!record.is_object() || !record.contains("schema_version") ||
        !record.at("schema_version").is_number_integer()) return 0;
    const auto& v = record.at("schema_version");
    if (v == kObservationBindingSchemaVersion) return kObservationBindingSchemaVersion;
    if (v == kObservationBindingRequestSchemaVersionV2) return kObservationBindingRequestSchemaVersionV2;
    if (v == kObservationBindingAcceptanceSchemaVersionV3) return kObservationBindingAcceptanceSchemaVersionV3;
    if (v == kObservationBindingFinalizedReceiptSchemaVersionV4) return kObservationBindingFinalizedReceiptSchemaVersionV4;
    return 0;
}

// Normalized lifecycle vocabulary for the future recording-context writer.
// A binding becomes authoritative only at `bound`; an acceptance alone is
// `accepted_pending_finalization`.
inline constexpr const char* kObservationBindingStatusNotApplicable =
    "not_applicable";
inline constexpr const char* kObservationBindingStatusRequested = "requested";
inline constexpr const char* kObservationBindingStatusAcceptedPendingFinalization =
    "accepted_pending_finalization";
inline constexpr const char* kObservationBindingStatusBound = "bound";
inline constexpr const char* kObservationBindingStatusUnbound = "unbound";
inline constexpr const char* kObservationBindingStatusHistoricallyUnavailable =
    "historically_unavailable";

// Seal a caller-supplied contract payload in the corresponding canonical
// semantic-digest envelope; the envelope takes the contract's schema_version
// (request 1 or 2, acceptance 1 to 3, receipt 1 to 4).  The payload is validated before it
// is sealed.  These functions perform no filesystem access or producer
// mutation.
bool seal_recording_observation_binding_request(
    const nlohmann::json& contract,
    nlohmann::json* request_out,
    std::string* error_out = nullptr);

bool seal_recording_observation_binding_acceptance(
    const nlohmann::json& contract,
    nlohmann::json* acceptance_out,
    std::string* error_out = nullptr);

bool seal_recording_observation_finalized_receipt(
    const nlohmann::json& contract,
    nlohmann::json* receipt_out,
    std::string* error_out = nullptr);

// Verify individual envelopes and the reciprocal chain.  The acceptance may
// be accepted or rejected.  A finalized receipt is valid only for an accepted
// acceptance and must exactly match request/acceptance identities, target,
// session IDs, and planned H5 path.
bool validate_recording_observation_binding_request(
    const nlohmann::json& request,
    std::string* error_out = nullptr);

bool validate_recording_observation_binding_acceptance(
    const nlohmann::json& acceptance,
    const nlohmann::json& request,
    std::string* error_out = nullptr);

bool validate_recording_observation_finalized_receipt(
    const nlohmann::json& receipt,
    const nlohmann::json& request,
    const nlohmann::json& acceptance,
    std::string* error_out = nullptr);

// One finalized receipt envelope and its contract on their own, without the
// request/acceptance chain (fixtures, offline checks).
bool validate_recording_observation_finalized_receipt_envelope(
    const nlohmann::json& receipt,
    std::string* error_out = nullptr);

// citrus.subject_reference v1 (receipt v3) or v2 (receipt v4) on its own.
bool validate_citrus_subject_reference(const nlohmann::json& reference, int version,
                                       std::string* error_out = nullptr);

// Subject identity v4, declaration against receipt for one context: for an
// individual the receipt's subject_reference.subject_id equals the acceptance
// v3 declaration's fish_id; for a dish_group or no dish the declaration's
// fish_id is null. Citrus guarantees this, so a disagreement is a Citrus bug;
// it is recorded (reported), never a refusal: the receipt is authoritative.
// Returns the disagreement, or an empty string when they agree or the pair is
// not receipt v4 + acceptance v3.
std::string subject_declaration_receipt_disagreement(const nlohmann::json& acceptance,
                                                     const nlohmann::json& receipt);

}  // namespace orange::session
